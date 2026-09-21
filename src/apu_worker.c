/*
 * apu_worker.c - run the SNES APU on the Vita's second core.
 *
 * The SPC700 and S-DSP are a physically separate chip: their own crystal,
 * their own 64 KB of SRAM, and four 8-bit latches ($2140-$2143) as the only
 * link to the 5A22. Emulating them on the game thread is what makes them cost
 * 5.7 ms of a 16.6 ms frame here; emulating them on the other core costs the
 * frame nothing but a lock acquisition.
 *
 * What this file owns is only the thread and the toggle. The contract -- what
 * the worker may advance to, and why that keeps the emulation deterministic
 * -- lives in common_rtl.c beside the state it guards.
 *
 * Runtime toggle, so this can be A/B'd on device the way the GXM renderer,
 * the sprite early-out and the compose hoist were: FZERO_APU_WORKER=0 in the
 * environment disables it, and apu_worker_set() flips it at runtime.
 */

#include "apu_worker.h"

#include <SDL.h>
#include <stdlib.h>
#include <psp2/kernel/threadmgr.h>

#include "platform.h"
#include "common_rtl.h"

/* SPC cycles per slice. One frame of SPC time is ~17,000 cycles / ~5.7 ms, so
 * an unsliced advance would hold RtlApuLock for a third of a frame and any
 * game-thread port write or audio callback would wait behind it -- the win
 * would go straight back out again as blocking. 512 cycles is ~170 us of
 * hold, ~33 acquisitions per frame, and costs nothing in the uncontended
 * case. Slicing cannot change what the SPC computes: the ceiling is unchanged
 * across slices, so a resumed advance lands on exactly the same cycles. */
#define APU_WORKER_SLICE 512u

/* Idle back-off. The worker reaches its ceiling and then has nothing to do
 * until the game thread publishes the next frame; a spin would burn the core
 * the DSP mixing wants. One ms is well under a frame and the next publish is
 * never urgent -- the cushion is four frames deep. */
#define APU_WORKER_IDLE_MS 1

/* Sampled on the game thread so the log line can say whether the two ended up
 * on the same core. */
static int g_main_affinity;
static int g_worker_affinity;

static SDL_Thread *g_thread;
static volatile int g_run;      /* clear to ask the thread to exit */
static volatile int g_enabled;  /* the A/B toggle */

static int APUWorkerMain(void *unused) {
    (void)unused;
    /* Which core this lands on decides whether the change is worth anything,
     * and SDL's Vita backend picks the mask, not us. Pin to the two user
     * cores the game thread is least likely to be on, and LOG what actually
     * happened -- a worker sharing core 0 with the game thread would show up
     * as "no speedup" and look exactly like a wrong projection. */
    int before = sceKernelGetThreadCpuAffinityMask(0);
    int rc = sceKernelChangeThreadCpuAffinityMask(
        0, SCE_KERNEL_CPU_MASK_USER_1 | SCE_KERNEL_CPU_MASK_USER_2);
    int mask = sceKernelGetThreadCpuAffinityMask(0);
    g_worker_affinity = mask;
    FZERO_LOG("INFO",
              "APU worker thread up (slice=%u SPC cycles) affinity %#x -> %#x"
              " (rc=%d, game thread %#x) -- cores %s",
              (unsigned)APU_WORKER_SLICE, (unsigned)before,
              (unsigned)mask, rc, (unsigned)g_main_affinity,
              (rc == 0 && mask > 0 && (mask & g_main_affinity) == 0)
                  ? "DISJOINT (guaranteed never co-scheduled)"
                  : "OVERLAPPING (no guarantee)");
    while (g_run) {
        if (!g_enabled || !rtl_apu_worker_active()) {
            SDL_Delay(APU_WORKER_IDLE_MS);
            continue;
        }
        /* Returns 0 when the published ceiling is reached: sleep rather than
         * spin, since only the game thread can raise it. */
        if (!rtl_apu_worker_slice(APU_WORKER_SLICE))
            SDL_Delay(APU_WORKER_IDLE_MS);
    }
    FZERO_LOG("INFO", "APU worker thread exiting");
    return 0;
}

bool apu_worker_start(void) {
    if (g_thread) return true;

    /* Disjoint masks are what makes "the worker never shares a core with the
     * game thread" a guarantee rather than a hope. Confining only the worker
     * (USER_1|USER_2) leaves the game thread on USER_ALL, and the scheduler
     * may put it on core 1 or 2 on top of the worker at any moment; the win
     * measured on device happened with that still possible. Pin the game
     * thread to core 0 and the intersection is empty, so the kernel cannot
     * co-schedule them however loaded the machine gets.
     *
     * The guarantee is exactly as strong as the two calls below succeeding,
     * so both are checked and logged rather than assumed. If either fails we
     * say so: the code still works, but the property does not hold.
     *
     * SDL's audio thread was created back in audio_init(), before this runs,
     * so it keeps whatever mask it inherited then and is unaffected -- it
     * stays free to float, which is what we want for a thread that must meet
     * a device deadline. */
    int main_before = sceKernelGetThreadCpuAffinityMask(0);
    int main_rc = sceKernelChangeThreadCpuAffinityMask(
        0, SCE_KERNEL_CPU_MASK_USER_0);
    g_main_affinity = sceKernelGetThreadCpuAffinityMask(0);
    FZERO_LOG(main_rc < 0 ? "WARN" : "INFO",
              "game thread affinity %#x -> %#x (rc=%d)",
              (unsigned)main_before, (unsigned)g_main_affinity, main_rc);
    if (main_rc < 0)
        FZERO_LOG("WARN", "game thread not pinned: the worker may still share"
                          " a core with it");

    const char *e = getenv("FZERO_APU_WORKER");
    g_enabled = (e && e[0] == '0') ? 0 : 1;

    /* Order matters at both ends of the lifetime: this must run AFTER
     * LoadAndInitSNES, because the worker dereferences g_snes->apu on its
     * first slice, and BEFORE audio_start only in the sense that both need
     * the SNES to exist. Opening the audio device before the emulator is
     * what crashed the first build of the callback rewrite. */
    RtlApuWorkerEnable(g_enabled);

    g_run = 1;
    g_thread = SDL_CreateThread(APUWorkerMain, "apu", NULL);
    if (!g_thread) {
        g_run = 0;
        RtlApuWorkerEnable(0);
        FZERO_LOG("ERROR", "SDL_CreateThread(apu) failed: %s", SDL_GetError());
        return false;
    }
    FZERO_LOG("INFO", "APU worker %s (FZERO_APU_WORKER=%s)",
              g_enabled ? "ENABLED" : "disabled", e ? e : "unset");
    return true;
}

void apu_worker_stop(void) {
    if (!g_thread) return;
    /* Stop the worker BEFORE anything frees emulator state. Cleanup() frees
     * the SPC player, which owns the DSP's RAM; the same use-after-free that
     * the audio thread had is available to this one. Joining here means no
     * slice can be in flight once this returns. */
    RtlApuWorkerEnable(0);
    g_run = 0;
    SDL_WaitThread(g_thread, NULL);
    g_thread = NULL;
}

void apu_worker_set(bool on) {
    g_enabled = on ? 1 : 0;
    RtlApuWorkerEnable(g_enabled);
}

bool apu_worker_enabled(void) { return g_enabled != 0; }

void apu_worker_log_affinity(void) {
    /* Re-stated periodically because the netlog reaching a session is usually
     * a TAIL: the startup line that says whether the two masks are disjoint
     * had been scrolling off before anyone could read it. */
    int w = g_worker_affinity;
    FZERO_LOG("INFO", "  cores: game=%#x worker=%#x -- %s",
              (unsigned)g_main_affinity, (unsigned)w,
              (w > 0 && (w & g_main_affinity) == 0)
                  ? "DISJOINT (never co-scheduled)"
                  : "OVERLAPPING (no guarantee)");
}
