/* main_vita.c - PS Vita entry point and game loop for FZeroRecomp. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>

#define SDL_ENABLE_OLD_NAMES
#include <SDL3/SDL.h>

#include "platform.h"
#include "snes/snes.h"
#include "fzero_capture_vita.h"
#include "snes/ppu.h"
#include "snes/cpu.h"
#include "snes/apu.h"
#include "snes/dsp.h"
#include "common_cpu_infra.h"
#include "common_rtl.h"
#include "fzero_rtl.h"
#include "fzero_layers.h"
#include "rom_image_verify.h"
#include "input_vita.h"
#include "audio_sdl.h"
#include "apu_worker.h"
#include "fzero_spc_player.h"
#include "fzero_gpu_ground.h"
#include "gxm_present.h"
#include "funcs.h"
#include <signal.h>
#include <psp2/power.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/kernel/threadmgr/callback.h>

/* 64 MB heap. */
unsigned int _newlib_heap_size_user = 64 * 1024 * 1024;

extern Ppu *g_ppu;
extern Snes *g_snes;
extern const RtlGameInfo *g_rtl_game_info;

#define VITA_SCREEN_WIDTH  960
#define VITA_SCREEN_HEIGHT 544

#define SNES_WIDTH  256
#define SNES_HEIGHT 224

#define ROM_PATH FZERO_ROM_PATH

/* Searched in order. */
static const char *rom_paths[] = {
    ROM_PATH,                                    /* ux0:/data/fzero_recomp/fzero.sfc */
    "ux0:/data/fzero_recomp/F-Zero (USA).sfc",  /* Alternative filename */
    "ux0:/data/fzero_recomp/F-Zero (USA).smc",  /* SMC header version */
    "fzero_usa_reference.sfc",                  /* Desktop default */
    "F-Zero (USA).sfc",                         /* Repo root */
    NULL
};

static uint8_t g_pixels[SNES_WIDTH * SNES_HEIGHT * 4];

/* The wide layers. The per-line pipeline in fzero_rtl.c runs under
 * `if (g_layers)`, so it is on only after FZeroSetLayers. The Vita presents
 * one flat ARGB image: fzero_layers.c composites straight into layers->flat. */
static FZeroLayers *g_layers_vita;
/* Chosen at boot with a system dialog (InitAll), so one VPK runs both ways. */
static bool g_widescreen;

/* SHA-256 of F-Zero (USA). */
static const uint8_t expected_sha256[32] = {
    0xbf, 0x16, 0xc3, 0xc8, 0x67, 0xc5, 0x8e, 0x2a,
    0xb0, 0x61, 0xc7, 0x0d, 0xe9, 0x29, 0x5b, 0x69,
    0x30, 0xd6, 0x3f, 0x29, 0xf8, 0x1c, 0xc9, 0x86,
    0xf5, 0xec, 0xae, 0x03, 0xe0, 0xad, 0x18, 0xd2,
};

static uint64_t g_frame_start_time = 0;
/* The SNES NTSC frame period: 16,639,264 ns = 60.0988 Hz, as on the PC host
 * (FZERO_GAME_PERIOD_NS). */
static const uint64_t g_frame_time_us = 16639;

static bool g_running = true;
static SDL_Window *g_window = NULL;

/* Per-phase frame timing. Always on: about 7 timer reads per frame, one log
 * line per 60 frames. The loop is capped at ~60 Hz, so the FPS figure does not
 * show headroom; the per-phase busy time does. */
static uint64_t g_acc_cpu, g_acc_draw, g_acc_aud, g_acc_upl, g_acc_clr, g_acc_cpy, g_acc_pre;
static uint64_t g_acc_flat;
/* This frame's own phase times, so the breakdown of an over-budget frame can
 * be logged without extra timer reads. */
static uint64_t g_frame_cpu_us, g_frame_draw_us, g_frame_aud_us, g_frame_upl_us, g_frame_cpy_us;

/* Draw-loop split, accumulated in fzero_rtl.c. */
extern uint64_t g_fzprof_ppu, g_fzprof_hdma, g_fzprof_split;
extern uint32_t g_fzprof_lines;

/* Rasteriser phase split, accumulated in snes/ppu.c. */
extern uint64_t g_ppuprof_eval, g_ppuprof_bg, g_ppuprof_compose;
/* APU catch-up cost, accumulated in common_rtl.c. */
extern uint64_t g_apuprof_us; extern uint32_t g_apuprof_calls;
extern uint64_t g_apuprof_port_us, g_apuprof_frame_us;
extern uint32_t g_apuprof_port_calls, g_apuprof_frame_calls;
extern uint64_t g_fzprof_nmi, g_fzprof_sched;
/* Sprite range-evaluation early-out. On by default. */
extern int g_ppu_fast_sprite_eval;
/* Compose-loop widescreen-HUD hoist. On by default. */
extern int g_ppu_fast_compose;
/* Compose palette table (ppu->cgramLut): the per-pixel cgram load, three
 * brightnessMult lookups and two shifts become one load and one store.
 * Pixel-exact. */
extern int g_ppu_fast_cgram_lut;

#if FZERO_COREBENCH
/* Does a second core help memory-bound work? Vita3K cannot answer this: it
 * JITs to x86 with a different memory system. The cold arm of MemcpyBench (a
 * source rotating through 8 buffers, so nothing stays in L2) runs on core 0
 * alone, then again with an identical loop on core 2. Reports both cores'
 * throughput and the aggregate:
 *
 *   scaling 2.0x   the bus is not the limit
 *   scaling 1.0x   the bus is the limit; moving work to another core only
 *                  moves the cost
 *
 * Both arms consume their destination so the optimiser cannot delete the
 * loop. Startup only; does not touch the game. */
enum { kCB_NSrc = 8 };
static volatile int g_cb_run;       /* neighbour: keep going */
static volatile int g_cb_ready;     /* neighbour: warmed up and streaming */
static volatile uint32_t g_cb_sink; /* consumed, so nothing is optimised out */
static volatile uint64_t g_cb_bytes;/* what the neighbour actually moved */

static size_t CoreBenchStream(char **src, char *dst, size_t n, int iters) {
    uint32_t sum = 0;
    for (int i = 0; i < iters; ++i) {
        memcpy(dst, src[i % kCB_NSrc], n);
        sum += (uint32_t)(unsigned char)dst[i & 1023];
    }
    g_cb_sink += sum;
    return (size_t)iters * n;
}

static int CoreBenchNeighbour(void *unused) {
    (void)unused;
    int rc = sceKernelChangeThreadCpuAffinityMask(0, SCE_KERNEL_CPU_MASK_USER_2);
    int mask = sceKernelGetThreadCpuAffinityMask(0);
    FZERO_LOG("INFO", "corebench: neighbour on %#x (rc=%d)", (unsigned)mask, rc);
    const size_t n = sizeof(Ppu);
    char *src[kCB_NSrc], *dst = malloc(n);
    int ok = dst != NULL;
    for (int i = 0; i < kCB_NSrc; ++i) {
        src[i] = malloc(n);
        if (!src[i]) ok = 0; else memset(src[i], (char)(0x11 + i), n);
    }
    if (!ok) { FZERO_LOG("WARN", "corebench: neighbour alloc failed"); return 1; }
    CoreBenchStream(src, dst, n, 32);           /* warm */
    uint64_t moved = 0, t0 = sceKernelGetProcessTimeWide();
    g_cb_ready = 1;
    while (g_cb_run) moved += CoreBenchStream(src, dst, n, 64);
    uint64_t dt = sceKernelGetProcessTimeWide() - t0;
    g_cb_bytes = dt ? (moved * 1000000ull) / dt : 0;   /* B/s */
    free(dst);
    for (int i = 0; i < kCB_NSrc; ++i) free(src[i]);
    return 0;
}

static void CoreBench(void) {
    const size_t n = sizeof(Ppu);
    char *src[kCB_NSrc], *dst = malloc(n);
    int ok = dst != NULL;
    for (int i = 0; i < kCB_NSrc; ++i) {
        src[i] = malloc(n);
        if (!src[i]) ok = 0; else memset(src[i], (char)(0x5a + i), n);
    }
    if (!ok) { FZERO_LOG("WARN", "corebench: alloc failed"); return; }
    /* apu_worker_start() pins the game thread to core 0; log what it is
     * instead of assuming the pin succeeded. */
    FZERO_LOG("INFO", "corebench: this thread on %#x, %u KB per copy",
              (unsigned)sceKernelGetThreadCpuAffinityMask(0),
              (unsigned)(n / 1024));
    enum { kIters = 512 };
    CoreBenchStream(src, dst, n, 64);                      /* warm */

    uint64_t t0 = sceKernelGetProcessTimeWide();
    size_t moved = CoreBenchStream(src, dst, n, kIters);
    uint64_t alone_us = sceKernelGetProcessTimeWide() - t0;
    uint64_t alone_bs = alone_us ? ((uint64_t)moved * 1000000ull) / alone_us : 0;

    g_cb_run = 1; g_cb_ready = 0; g_cb_bytes = 0;
    SDL_Thread *th = SDL_CreateThread(CoreBenchNeighbour, "cb", NULL);
    if (!th) { FZERO_LOG("WARN", "corebench: no neighbour thread"); goto done; }
    while (!g_cb_ready) SDL_Delay(1);                      /* let it get going */
    t0 = sceKernelGetProcessTimeWide();
    moved = CoreBenchStream(src, dst, n, kIters);
    uint64_t shared_us = sceKernelGetProcessTimeWide() - t0;
    uint64_t shared_bs = shared_us ? ((uint64_t)moved * 1000000ull) / shared_us : 0;
    g_cb_run = 0;
    SDL_WaitThread(th, NULL);

    uint64_t agg = shared_bs + g_cb_bytes;
    FZERO_LOG("INFO",
        "corebench: core0 ALONE %u MB/s | core0 SHARED %u MB/s (%u%%)"
        " | core2 %u MB/s | AGGREGATE %u MB/s = %u.%02ux one core",
        (unsigned)(alone_bs >> 20), (unsigned)(shared_bs >> 20),
        (unsigned)(alone_bs ? shared_bs * 100 / alone_bs : 0),
        (unsigned)(g_cb_bytes >> 20), (unsigned)(agg >> 20),
        (unsigned)(alone_bs ? agg / alone_bs : 0),
        (unsigned)(alone_bs ? (agg * 100 / alone_bs) % 100 : 0));
    FZERO_LOG("INFO",
        "corebench: sink=%u (consumed so the copies cannot be deleted)",
        (unsigned)g_cb_sink);
done:
    free(dst);
    for (int i = 0; i < kCB_NSrc; ++i) free(src[i]);
}
#endif

#if FZERO_GPUBENCH
#include <psp2/kernel/sysmem.h>
/* The CPU read of one margin buffer (2 x 71 x 224 ARGB, 127 KB) out of each
 * memory type a GXM render target can use, for reading GPU output back.
 * Cached main memory is the baseline only: GPU writes to it need a cache
 * invalidate that user mode cannot request. Startup only; does not touch the
 * game. */
enum { kGB_W = 142, kGB_H = 224, kGB_Iters = 60 };

static void GpuBenchRead(const char *name, SceUInt32 type, void *dst) {
    const SceSize n = kGB_W * kGB_H * 4, sz = 256 * 1024;
    SceUID uid = sceKernelAllocMemBlock("gpubench", type, sz, NULL);
    if (uid < 0) {
        FZERO_LOG("WARN", "gpubench: read[%s] alloc failed %#x", name, (unsigned)uid);
        return;
    }
    void *src = NULL;
    sceKernelGetMemBlockBase(uid, &src);
    memset(src, 0x5a, n);
    uint32_t sum = 0;
    uint64_t best = ~0ull, total = 0;
    for (int i = 0; i < kGB_Iters; ++i) {
        uint64_t t0 = sceKernelGetProcessTimeWide();
        memcpy(dst, src, n);
        uint64_t dt = sceKernelGetProcessTimeWide() - t0;
        sum += ((const uint32_t *)dst)[i * 97];
        total += dt;
        if (dt < best) best = dt;
    }
    FZERO_LOG("INFO",
              "gpubench: read[%s] %u B mean=%llu us min=%llu us = %u MB/s | chk=%08x",
              name, (unsigned)n, (unsigned long long)(total / kGB_Iters),
              (unsigned long long)best,
              (unsigned)(total ? (uint64_t)n * kGB_Iters * 1000000ull / total >> 20 : 0),
              (unsigned)sum);
    sceKernelFreeMemBlock(uid);
}

static void GpuBench(void) {
    uint32_t *dst = malloc(kGB_W * kGB_H * 4);
    if (!dst) { FZERO_LOG("WARN", "gpubench: alloc failed"); return; }
    GpuBenchRead("main-cached ", SCE_KERNEL_MEMBLOCK_TYPE_USER_RW, dst);
    GpuBenchRead("main-nc     ", SCE_KERNEL_MEMBLOCK_TYPE_USER_RW_UNCACHE, dst);
    GpuBenchRead("phycont-nc  ", SCE_KERNEL_MEMBLOCK_TYPE_USER_MAIN_PHYCONT_NC_RW, dst);
    GpuBenchRead("cdram       ", SCE_KERNEL_MEMBLOCK_TYPE_USER_CDRAM_RW, dst);

    FZERO_LOG("INFO", "gpubench: the margin render it would replace costs 7420 us/frame");
    free(dst);
}
#endif

#if FZERO_MEMBENCH
/* The cost of BuildWideLine's per-scanline `memcpy(copy, ppu, sizeof(*copy))`
 * on this machine. The working set (~400 KB) is close to the ~512 KB L2, so a
 * bandwidth calculation cannot predict it. Two arms, 224 copies each (one
 * frame):
 *
 *   hot   one src, one dst, reused: the real access pattern. A lower bound.
 *   cold  src rotates through 8 buffers (1.6 MB, past L2), dst stays put.
 *         An upper bound.
 *
 * The checksum consumes dst so the optimiser cannot delete the loop, and it
 * is logged so it is not dead code either. */
static void MemcpyBench(void) {
    enum { kLines = 224, kNSrc = 8, kSkip = 114688, kRegs = 5400 };
    const size_t kFull = sizeof(Ppu);
    char *src[kNSrc], *dst = malloc(kFull);
    int ok = dst != NULL;
    for (int i = 0; i < kNSrc; ++i) {
        src[i] = malloc(kFull);
        if (!src[i]) ok = 0; else memset(src[i], (char)(0x5a + i), kFull);
    }
    if (!ok) { FZERO_LOG("WARN", "membench: alloc failed"); goto done; }
    memset(dst, 0, kFull);
    const size_t sizes[3] = { kFull, kFull - kSkip, kRegs };
    const char *names[3] = { "full ", "fixed", "regs " };
    for (int s_i = 0; s_i < 3; ++s_i) {
        size_t n = sizes[s_i];
        uint32_t sum = 0;
        uint64_t t0, hot, cold;
        for (int i = 0; i < kLines; ++i) memcpy(dst, src[0], n);   /* warm */
        t0 = sceKernelGetProcessTimeWide();
        for (int i = 0; i < kLines; ++i) { memcpy(dst, src[0], n); sum += dst[i]; }
        hot = sceKernelGetProcessTimeWide() - t0;
        t0 = sceKernelGetProcessTimeWide();
        for (int i = 0; i < kLines; ++i) { memcpy(dst, src[i & (kNSrc-1)], n); sum += dst[i]; }
        cold = sceKernelGetProcessTimeWide() - t0;
        FZERO_LOG("INFO",
                  "membench[%s]: %6u B x224 = %2u MB/frame | hot=%llu us"
                  " cold=%llu us | chk=%08x",
                  names[s_i], (unsigned int)n,
                  (unsigned int)((n * kLines) / 1000000u),
                  (unsigned long long)hot, (unsigned long long)cold,
                  (unsigned int)sum);
    }
    FZERO_LOG("INFO", "membench: budget 16639 us/frame; sizeof(Ppu)=%u",
              (unsigned int)sizeof(Ppu));
done:
    free(dst);
    for (int i = 0; i < kNSrc; ++i) free(src[i]);
}
#endif

/* Cost of one sceKernelGetProcessTimeWide(), measured at startup, so the
 * probes can report their own overhead. */
static uint32_t g_timer_ns;
static void CalibrateTimer(void) {
    enum { N = 20000 };
    uint64_t a = sceKernelGetProcessTimeWide();
    for (int i = 0; i < N; ++i) (void)sceKernelGetProcessTimeWide();
    uint64_t b = sceKernelGetProcessTimeWide();
    g_timer_ns = (uint32_t)(((b - a) * 1000ull) / N);
    FZERO_LOG("INFO", "timer: sceKernelGetProcessTimeWide ~%u ns/call"
              " (draw probe adds ~675/frame = ~%u us/frame)",
              (unsigned int)g_timer_ns, (unsigned int)((g_timer_ns * 675u) / 1000u));
}
/* Previous block's worker counters, so the heartbeat prints per-block deltas
 * rather than run totals. */
static uint64_t s_w_slices, s_w_idle, s_w_late;
static uint64_t g_acc_sleep;
static uint32_t g_acc_over, g_acc_frames;

/* Per-frame phase breakdown, captured only for frames that miss budget: it
 * shows whether the overrun is in cpu, draw (which includes the ground GPU
 * acquire wait) or upl (the present-texture wait). Capped per window. */
#define FZ_OVER_CAP 4
static struct {
    uint32_t idx;  /* 1..60: which frame in the window */
    uint64_t cpu, draw, aud, upl, cpy;
} g_over_detail[FZ_OVER_CAP];
static uint32_t g_over_detail_n;

static void ResetPhaseAccumulators(void) {
    g_acc_cpu = g_acc_draw = g_acc_aud = g_acc_upl = g_acc_clr = g_acc_cpy = g_acc_pre = 0;
    g_acc_flat = 0;
    g_over_detail_n = 0;
    g_fzprof_ppu = g_fzprof_hdma = g_fzprof_split = 0; g_fzprof_lines = 0;
    g_ppuprof_eval = g_ppuprof_bg = g_ppuprof_compose = 0;
    g_apuprof_us = 0; g_apuprof_calls = 0;
    g_apuprof_port_us = g_apuprof_frame_us = 0;
    g_apuprof_port_calls = g_apuprof_frame_calls = 0;
    g_fzprof_nmi = g_fzprof_sched = 0;
    g_acc_sleep = 0;
    g_acc_over = g_acc_frames = 0;
}

static Snes *g_snes_instance = NULL;
static uint8_t *g_rom_buffer = NULL;
static long g_rom_size = 0;

/* FNV-1a over the battery SRAM, used to detect changes for the periodic
 * auto-save below. */
static uint32_t SramHash(void) {
    uint32_t h = 2166136261u;
    for (int i = 0; g_sram && i < g_sram_size; i++) {
        h ^= g_sram[i];
        h *= 16777619u;
    }
    return h;
}

static uint32_t s_last_saved_sram_hash;
static int s_have_saved_sram_hash;

/* Write battery SRAM to disk and record its hash, so the auto-save below
 * only writes again when the content actually changed. */
static void SaveSram(void) {
    if (!g_snes_instance) return;
    s_last_saved_sram_hash = SramHash();
    s_have_saved_sram_hash = 1;
    RtlWriteSram();
}

/* Power callback: saves battery SRAM when the app is suspended (PS button
 * pressed or closed from LiveArea). SDL3's Vita backend does not deliver
 * SDL_QUIT for these events, so we use a ScePower callback instead.
 * Saving on resume as well covers the case where the suspend callback was
 * missed (e.g. the PS button suspend fired while the callback thread was
 * busy): the write is idempotent, so doing it twice is harmless. */
static SceUID g_power_cb_id = -1;
static SceUID g_callback_thread_id = -1;

static int PowerCallback(int notifyId, int notifyCount, int powerInfo, void *userData) {
    (void)notifyId; (void)notifyCount; (void)userData;
    if (powerInfo & SCE_POWER_CB_APP_SUSPEND) {
        FZERO_LOG("INFO", "App suspend: writing battery SRAM");
        SaveSram();
    } else if (powerInfo & SCE_POWER_CB_APP_RESUME) {
        FZERO_LOG("INFO", "App resume: writing battery SRAM");
        SaveSram();
    }
    return 0;
}

static int CallbackThread(SceSize args, void *argp) {
    (void)args; (void)argp;
    g_power_cb_id = sceKernelCreateCallback("FZero Power Callback", 0, PowerCallback, NULL);
    if (g_power_cb_id >= 0) {
        scePowerRegisterCallback(g_power_cb_id);
        FZERO_LOG("INFO", "Power callback registered (id=%d)", g_power_cb_id);
    } else {
        FZERO_LOG("WARN", "Failed to create power callback (id=%d)", g_power_cb_id);
    }
    /* This thread only handles callbacks: delay forever with CB so callbacks fire.
     * 1 s poll so the thread exits promptly when g_running goes false. */
    while (g_running) {
        sceKernelDelayThreadCB(1 * 1000 * 1000); /* 1 s */
    }
    return 0;
}

static void StartPowerCallback(void) {
    /* 16 KB stack: the callback calls FZERO_LOG (1 KB stack buffer in
     * platform_vita_log) and RtlWriteSram (fopen/fwrite/fclose), which
     * together overflow a 4 KB stack. */
    g_callback_thread_id = sceKernelCreateThread("fzero_cb", CallbackThread,
                                                  0x10000100, 0x4000, 0, 0, NULL);
    if (g_callback_thread_id >= 0) {
        sceKernelStartThread(g_callback_thread_id, 0, NULL);
    } else {
        FZERO_LOG("WARN", "Failed to create callback thread (id=%d)", g_callback_thread_id);
    }
}

static void StopPowerCallback(void) {
    if (g_power_cb_id >= 0) {
        scePowerUnregisterCallback(g_power_cb_id);
        sceKernelDeleteCallback(g_power_cb_id);
        g_power_cb_id = -1;
    }
    if (g_callback_thread_id >= 0) {
        /* The thread checks g_running, so setting it false (done by caller)
         * lets it exit. Wait for it. */
        sceKernelWaitThreadEnd(g_callback_thread_id, NULL, NULL);
        g_callback_thread_id = -1;
    }
}

static bool VerifyRom(const char *path) {
    const char *driver = getenv("SDL_VIDEODRIVER");
    if (getenv("SNESRECOMP_MAX_FRAMES") || (driver && !strcmp(driver, "dummy"))) {
        return snesrecomp_rom_match_sha256(path, (const uint8 (*)[32])&expected_sha256, 1) == 0;
    }
    return snesrecomp_rom_verify_sha256(path, expected_sha256) != 0;
}

static uint8_t *ReadWholeFile(const char *path, long *size_out) {
    void *buffer = NULL;
    size_t file_size = 0;
    if (platform_read_file(path, &buffer, &file_size)) {
        long size = (long)file_size;
        if (size % 1024 == 512) {
            size -= 512;
            memmove(buffer, (char *)buffer + 512, (size_t)size);
        }
        *size_out = size;
        return (uint8_t *)buffer;
    }
    return NULL;
}

/* Allocate the wide layers and hand them to the RTL. Without this call the
 * whole widescreen pass stays off (everything in it runs under
 * `if (g_layers)`). */
static bool EnableWidescreen(void) {
    /* ~2 MB, so heap-allocated. */
    g_layers_vita = calloc(1, sizeof(*g_layers_vita));
    if (!g_layers_vita) {
        FZERO_LOG("ERROR", "widescreen: could not allocate %u bytes of layers",
                  (unsigned int)sizeof(*g_layers_vita));
        return false;
    }
    FZeroSetLayers(g_layers_vita);
    g_widescreen = true;
    FZERO_LOG("INFO", "widescreen: ON, %ux%u, layers=%u KB",
              (unsigned int)FZERO_WIDE_WIDTH, (unsigned int)FZERO_LAYER_HEIGHT,
              (unsigned int)(sizeof(*g_layers_vita) / 1024));
    return true;
}

static bool InitAll(void) {
    platform_init();
    platform_ensure_directories();

    FZERO_LOG("INFO", "Initializing SDL3");
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        FZERO_LOG("ERROR", "SDL_Init(VIDEO) failed: %s", SDL_GetError());
        return false;
    }

    /* keybinds.c calls SDL_GetScancodeFromName, which needs the keyboard
     * function pointers these subsystems set up. */
    SDL_InitSubSystem(SDL_INIT_JOYSTICK);
    SDL_InitSubSystem(SDL_INIT_GAMEPAD);

    FZERO_LOG("INFO", "Creating window at %dx%d", VITA_SCREEN_WIDTH, VITA_SCREEN_HEIGHT);
    g_window = SDL_CreateWindow("F-Zero Recomp (Vita)",
                                VITA_SCREEN_WIDTH, VITA_SCREEN_HEIGHT,
                                SDL_WINDOW_FULLSCREEN);
    if (!g_window) {
        FZERO_LOG("ERROR", "SDL_CreateWindow failed: %s", SDL_GetError());
        return false;
    }

    CalibrateTimer();
#if FZERO_CAPTURE
    FZeroCaptureInit();
#endif
#if FZERO_MEMBENCH
    MemcpyBench();
#endif

    FZERO_LOG("INFO", "Creating renderer and SNES frame texture");
    /* The game owns GXM, so no SDL renderer is created: SDL's would call
     * sceGxmInitialize first. */
    /* The texture is allocated for the wide frame, and shrunk if the player
     * says No. The dialog needs GXM up, so it comes after the init. */
    if (!GxmPresentInit(FZERO_WIDE_WIDTH, FZERO_LAYER_HEIGHT)) return false;
    int answer = GxmPresentAskYesNo("Enable widescreen?");
    FZERO_LOG("INFO", "widescreen dialog: %s",
              answer > 0 ? "Yes" : answer == 0 ? "No" : "cancelled or failed");
    if (answer > 0) {
        if (!EnableWidescreen()) return false;
    } else if (!GxmPresentSetSourceSize(SNES_WIDTH, SNES_HEIGHT)) {
        return false;
    }
    ResetPhaseAccumulators();
#if FZERO_GPUBENCH
    GpuBench();
#endif

    FZERO_LOG("INFO", "Initializing controller input");
    input_init();

    FZERO_LOG("INFO", "Initializing audio");
    if (!audio_init()) {
        FZERO_LOG("WARN", "Audio initialization failed, continuing without audio");
    }

    return true;
}

static bool LoadAndInitSNES(void) {
    const char *rom_path = NULL;
    FZERO_LOG("INFO", "Searching for ROM file...");
    for (int i = 0; rom_paths[i] != NULL; i++) {
        if (platform_file_exists(rom_paths[i])) {
            if (VerifyRom(rom_paths[i])) {
                rom_path = rom_paths[i];
                FZERO_LOG("INFO", "Found valid ROM at: %s", rom_path);
                break;
            } else {
                FZERO_LOG("WARN", "ROM at %s exists but failed verification", rom_paths[i]);
            }
        }
    }

    if (!rom_path) {
        FZERO_LOG("ERROR", "ROM not found or verification failed!");
        FZERO_LOG("ERROR", "Tried the following paths:");
        for (int i = 0; rom_paths[i] != NULL; i++) {
            FZERO_LOG("ERROR", "  - %s", rom_paths[i]);
        }
        FZERO_LOG("ERROR", "");
        FZERO_LOG("ERROR", "For Vita: Place ROM at ux0:/data/fzero_recomp/fzero.sfc");
        FZERO_LOG("ERROR", "Expected SHA-256: bf16c3c867c58e2ab061c70de9295b6930d63f29f81cc986f5ecae03e0ad18d2");
        return false;
    }

    FZERO_LOG("INFO", "Loading ROM from: %s", rom_path);
    g_rom_size = 0;
    g_rom_buffer = ReadWholeFile(rom_path, &g_rom_size);
    if (!g_rom_buffer) {
        FZERO_LOG("ERROR", "Failed to load ROM");
        return false;
    }
    FZERO_LOG("INFO", "ROM loaded: %ld bytes", g_rom_size);

    FZERO_LOG("INFO", "Registering game");
    RtlRegisterGame(&kFZeroGameInfo);

    FZERO_LOG("INFO", "Initializing SPC player");
    g_spc_player = FZeroSpcPlayer_Create();
    if (g_spc_player && g_spc_player->initialize) {
        g_spc_player->initialize(g_spc_player);
    }

    FZERO_LOG("INFO", "Initializing SNES");
    RtlSetSaveRoot(FZERO_SAVES_PATH);
    g_snes_instance = SnesInit(g_rom_buffer, (int)g_rom_size);
    if (!g_snes_instance) {
        FZERO_LOG("ERROR", "SnesInit failed");
        free(g_rom_buffer);
        g_rom_buffer = NULL;
        return false;
    }
    FZERO_LOG("INFO", "SNES initialized successfully");

    /* Load battery SRAM from ux0:/data/fzero_recomp/saves/save.srm.
     * Must run after SnesInit so g_sram/g_sram_size are set. */
    RtlReadSram();
    FZERO_LOG("INFO", "Battery SRAM loaded");

    /* Start the power callback thread so SRAM is saved on app suspend
     * (PS button or close from LiveArea). */
    StartPowerCallback();
#if FZERO_DIAG
    FZERO_LOG("INFO", "  Sinit after SnesInit S=%04X e=%d",
              (unsigned int)g_cpu.S, (int)g_cpu.emulation);
#endif

    /* No bounce exclusions are needed. $00:F806 and $03:8062 yield correctly
     * because RecompReturn has a fixed width (snesrecomp/runner/src/cpu_state.h);
     * with ARM EABI short enums the yield sentinel was truncated. */

    /* RtlEnableExtendedFrameTiming() is not called: the PC reference build
     * does not call it either, and the Vita host must match it. */

    FZERO_LOG("INFO", "Starting PPU drawing");
    PpuBeginDrawing(g_ppu, g_pixels, (size_t)SNES_WIDTH * 4, kPpuRenderFlags_NewRenderer);

    return true;
}

static void HandleFrame(void) {
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
        if (event.type == SDL_QUIT) {
            g_running = false;
            return;
        }
    }

    /* Sample the pad once; everything below reads that one sample. */
    input_poll();
#if FZERO_CAPTURE
    if (g_widescreen) FZeroCapturePoll();
#endif

    /* L + R + START held soft-resets the SNES (preserves battery SRAM).
     * F-Zero sometimes locks up with no way back to the title screen. */
    if (input_should_reset()) {
        FZERO_LOG("INFO", "Soft reset (L+R+START)");
        RtlReset(1);
        /* Re-arm the frame driver's boot gate so the game's reset-vector boot
         * re-runs (Mode 7 matrix, HDMA tables, VRAM uploads included). */
        FZeroSoftReset();
        return;
    }

    /* START + SELECT held writes battery SRAM to disk on demand: a
     * deterministic save path that does not depend on the suspend callback. */
    if (input_should_save()) {
        FZERO_LOG("INFO", "Manual save (START+SELECT): writing battery SRAM");
        if (g_snes_instance) {
            SaveSram();
            int64_t sz = platform_file_size(FZERO_SAVES_PATH "save.srm");
            FZERO_LOG("INFO", "Manual save: %s is %lld bytes",
                      FZERO_SAVES_PATH "save.srm", (long long)sz);
        }
    }

    uint32_t inputs = input_read();

    static uint32_t s_frame_count = 0;
    static uint64_t s_last_fps_time = 0;
    s_frame_count++;

    if (s_frame_count <= 10) {
        FZERO_LOG("INFO", ">>> Frame %u starting...", (unsigned int)s_frame_count);
    }

    /* Auto-save battery SRAM about once a second whenever its content
     * changes. Closing the app from LiveArea kills the process without
     * SDL_QUIT, and the suspend callback's save can be missed or cut short
     * by the kill, so without a periodic save a hard close loses every
     * record since boot. The hash check keeps the 2 KB write off storage
     * unless the game actually wrote SRAM (boot init, record updates), so
     * any exit path -- LiveArea close, PS suspend, power off, crash -- now
     * loses at most about a second of records. Runs on the game thread
     * between frames, where SRAM is stable. */
    if (s_frame_count % 60 == 0 && g_snes_instance) {
        uint32_t h = SramHash();
        if (!s_have_saved_sram_hash || h != s_last_saved_sram_hash)
            SaveSram();
    }

    uint64_t t_phase = sceKernelGetProcessTimeWide();
    const uint64_t t_frame_begin = t_phase;

#if FZERO_DIAG
    {
      extern int snes_frame_counter;
      if (snes_frame_counter <= 6)
        FZERO_LOG("INFO", "  Sbefore f%d S=%04X e=%d", snes_frame_counter,
                  (unsigned int)g_cpu.S, (int)g_cpu.emulation);
    }
#endif
    RtlRunFrame(inputs);
    { uint64_t n = sceKernelGetProcessTimeWide(); uint64_t d = n - t_phase;
      g_acc_cpu += d; g_frame_cpu_us = d; t_phase = n; }

#if FZERO_DIAG
    /* Per-frame WRAM CRC, at the same point in the frame as the PC probe
     * (after the CPU half, before draw_ppu_frame), over the same 128 KB, so
     * the two logs compare directly. */
    {
      extern uint32_t crc32_compute(const uint8_t *data, size_t len);
      extern int snes_frame_counter;
      if (snes_frame_counter <= 120)
        FZERO_LOG("INFO", "  ramcrc f%d=%08X", snes_frame_counter,
                  (unsigned int)crc32_compute(g_ram, 0x20000));
      /* Per-4 KB-page CRCs for the first frames, to find which region
       * differs. 32 lines per frame, three frames. */
      if (snes_frame_counter >= 1 && snes_frame_counter <= 3)
        for (int pg = 0; pg < 32; ++pg)
          FZERO_LOG("INFO", "  rampg f%d p%02d=%08X", snes_frame_counter, pg,
                    (unsigned int)crc32_compute(g_ram + pg * 0x1000, 0x1000));
      /* Stack pointer before each frame's NMI push. PC idles at S=01FF. */
      if (snes_frame_counter <= 6)
        FZERO_LOG("INFO", "  Safter f%d S=%04X e=%d", snes_frame_counter,
                  (unsigned int)g_cpu.S, (int)g_cpu.emulation);
    }
#endif

    if (s_frame_count <= 10) {
        FZERO_LOG("INFO", ">>> Frame %u: RtlRunFrame done", (unsigned int)s_frame_count);
    }

    if (g_rtl_game_info && g_rtl_game_info->draw_ppu_frame) {
        if (s_frame_count <= 10) {
            FZERO_LOG("INFO", ">>> Frame %u: draw_ppu_frame...", (unsigned int)s_frame_count);
        }
        g_rtl_game_info->draw_ppu_frame();
        if (s_frame_count <= 10) {
            FZERO_LOG("INFO", ">>> Frame %u: draw_ppu_frame done", (unsigned int)s_frame_count);
            FZERO_LOG("INFO", ">>> Frame %u: PPU inidisp=0x%02X (forcedBlank=%d, brightness=%d)",
                      (unsigned int)s_frame_count,
                      (unsigned int)g_ppu->inidisp,
                      (int)((g_ppu->inidisp >> 7) & 1),
                      (int)(g_ppu->inidisp & 0xF));
            FZERO_LOG("INFO", ">>> Frame %u: PPU screenEnabled[0]=0x%02X screenEnabled[1]=0x%02X",
                      (unsigned int)s_frame_count,
                      (unsigned int)g_ppu->screenEnabled[0],
                      (unsigned int)g_ppu->screenEnabled[1]);
            /* Is anything non-black drawn? */
            uint32_t *px = (uint32_t *)g_pixels;
            uint32_t nonzero = 0;
            for (int _pi = 0; _pi < SNES_WIDTH * SNES_HEIGHT; ++_pi)
                nonzero |= px[_pi];
            FZERO_LOG("INFO", ">>> Frame %u: pixel sample nonzero=0x%08X",
                      (unsigned int)s_frame_count, (unsigned int)nonzero);
        }
    }

    { uint64_t n = sceKernelGetProcessTimeWide(); uint64_t d = n - t_phase;
      g_acc_draw += d; g_frame_draw_us = d; t_phase = n; }
#if FZERO_CAPTURE
    if (g_widescreen) FZeroCaptureEndFrame(g_layers_vita, g_ppu, s_frame_count);
#endif

    audio_render_frame(g_snes_instance);
    { uint64_t n = sceKernelGetProcessTimeWide(); uint64_t d = n - t_phase;
      g_acc_aud += d; g_frame_aud_us = d; t_phase = n; }

    if (g_widescreen) {
        /* flat= is kept in the log format. It should read ~0. */
        { uint64_t n = sceKernelGetProcessTimeWide(); g_acc_flat += n - t_phase; t_phase = n; }
        GxmPresentUpload(&g_layers_vita->flat[0][0], FZERO_WIDE_WIDTH);
    } else {
        GxmPresentUpload((const uint32_t *)g_pixels, SNES_WIDTH);
    }
    { uint64_t n = sceKernelGetProcessTimeWide(); uint64_t d = n - t_phase;
      g_acc_upl += d; g_frame_upl_us = d; t_phase = n; }

    /* GXM clears inside its scene, in cpy= below. clr= stays in the log
     * format. */
    { uint64_t n = sceKernelGetProcessTimeWide(); g_acc_clr += n - t_phase; t_phase = n; }

    SDL_Rect dest_rect;
    
    /* Aspect-correct destination rectangle. */
    float snes_aspect = (float)SNES_WIDTH / (float)SNES_HEIGHT;
    if (g_widescreen)
        snes_aspect = (float)FZERO_WIDE_WIDTH / (float)FZERO_LAYER_HEIGHT;
    float screen_aspect = (float)VITA_SCREEN_WIDTH / (float)VITA_SCREEN_HEIGHT;
    
    if (screen_aspect > snes_aspect) {
        int height = VITA_SCREEN_HEIGHT;
        int width = (int)(height * snes_aspect);
        dest_rect.x = (VITA_SCREEN_WIDTH - width) / 2;
        dest_rect.y = 0;
        dest_rect.w = width;
        dest_rect.h = height;
    } else {
        int width = VITA_SCREEN_WIDTH;
        int height = (int)(width / snes_aspect);
        dest_rect.x = 0;
        dest_rect.y = (VITA_SCREEN_HEIGHT - height) / 2;
        dest_rect.w = width;
        dest_rect.h = height;
    }

    /* Its own scene, queued ahead of the present scene. Counted in cpy=. */
    if (g_widescreen) FZeroGpuGroundEndFrame(&g_layers_vita->flat[0][0], FZERO_WIDE_WIDTH);
    /* Scene and flip together: cpy= is the whole GPU submission, pre= is 0. */
    GxmPresentDraw(dest_rect.x, dest_rect.y, dest_rect.w, dest_rect.h);
    { uint64_t n = sceKernelGetProcessTimeWide(); uint64_t d = n - t_phase;
      g_acc_cpy += d; g_frame_cpy_us = d; t_phase = n; }
    { uint64_t n = sceKernelGetProcessTimeWide(); g_acc_pre += n - t_phase; t_phase = n; }
    g_acc_frames++;
    (void)t_frame_begin;

    if (s_frame_count == 1) {
        FZERO_LOG("INFO", "First frame rendered successfully!");
        s_last_fps_time = sceKernelGetProcessTimeWide();
    } else if (s_frame_count <= 10) {
        FZERO_LOG("INFO", "Frame %u rendered successfully!", (unsigned int)s_frame_count);
    } else if (s_frame_count % 60 == 0) {
        uint64_t now = sceKernelGetProcessTimeWide();
        float elapsed_sec = (float)(now - s_last_fps_time) / 1000000.0f;
        float fps = (elapsed_sec > 0.0f) ? (60.0f / elapsed_sec) : 0.0f;
        uint32_t n = g_acc_frames ? g_acc_frames : 1;
        uint64_t busy = g_acc_flat + g_acc_cpu + g_acc_draw + g_acc_aud + g_acc_upl
                      + g_acc_clr + g_acc_cpy + g_acc_pre;
        FZERO_LOG("INFO",
                  "Heartbeat: f%u %s ~%.1f FPS | emu=%u [cpu=%u draw=%u] aud=%u"
                  " | present=%u [upl=%u clr=%u cpy=%u pre=%u] flat=%u"
                  " | busy=%u sleep=%u over=%u n=%u",
                  (unsigned int)s_frame_count, "GXM", fps,
                  (unsigned int)((g_acc_cpu + g_acc_draw) / n),
                  (unsigned int)(g_acc_cpu / n), (unsigned int)(g_acc_draw / n),
                  (unsigned int)(g_acc_aud / n),
                  (unsigned int)((g_acc_upl + g_acc_clr + g_acc_cpy + g_acc_pre) / n),
                  (unsigned int)(g_acc_upl / n), (unsigned int)(g_acc_clr / n),
                  (unsigned int)(g_acc_cpy / n), (unsigned int)(g_acc_pre / n),
                  (unsigned int)(g_acc_flat / n),
                  (unsigned int)(busy / n), (unsigned int)(g_acc_sleep / n),
                  (unsigned int)g_acc_over, (unsigned int)n);
        /* Phase breakdown of the first frames that missed budget in this
         * window. wf = which frame in the window, 1..60. */
        for (uint32_t oi = 0; oi < g_over_detail_n; ++oi)
            FZERO_LOG("INFO",
                      "  over[%u]: wf=%u cpu=%u draw=%u aud=%u upl=%u cpy=%u",
                      (unsigned int)oi, (unsigned int)g_over_detail[oi].idx,
                      (unsigned int)g_over_detail[oi].cpu,
                      (unsigned int)g_over_detail[oi].draw,
                      (unsigned int)g_over_detail[oi].aud,
                      (unsigned int)g_over_detail[oi].upl,
                      (unsigned int)g_over_detail[oi].cpy);
        FZeroGpuGroundReport();
        /* APU catch-up: the measured part of cpu=. apuprobe= is this probe's
         * own cost. */
        /* cpu= attributed. The APU sync is nested INSIDE nmi/sched, so it is
         * not additive with them; nmi+sched vs cpu= exposes RtlRunFrame's own
         * overhead instead. */
        FZERO_LOG("INFO",
                  "  cpu-split: nmi=%u sched=%u | nmi+sched=%u vs cpu=%u",
                  (unsigned int)(g_fzprof_nmi / n), (unsigned int)(g_fzprof_sched / n),
                  (unsigned int)((g_fzprof_nmi + g_fzprof_sched) / n),
                  (unsigned int)(g_acc_cpu / n));
        FZERO_LOG("INFO",
                  "  apu-sites: port=%u us (%u calls) frameEnd=%u us (%u calls)",
                  (unsigned int)(g_apuprof_port_us / n),
                  (unsigned int)(g_apuprof_port_calls / n),
                  (unsigned int)(g_apuprof_frame_us / n),
                  (unsigned int)(g_apuprof_frame_calls / n));
        /* APU worker health:
         *   late=  frames whose previous ceiling was still unreached at
         *          publish time: the worker is behind real time. 0 is the
         *          goal.
         *   occ=   DSP ring occupancy in natives, sampled once per block. The
         *          servo steers toward 2136 (4 frames of 534). The apu-audio
         *          line below counts the actual faults.
         * Both are cumulative counters printed per 60-frame block. */
        {
            uint64_t wslices = 0, widle = 0, wlate = 0;
            rtl_apu_worker_stats(&wslices, &widle, &wlate);
            extern Snes *g_snes;
            unsigned occ = (g_snes && g_snes->apu && g_snes->apu->dsp)
                ? (unsigned)(g_snes->apu->dsp->sampleWrite -
                             g_snes->apu->dsp->sampleRead) : 0u;
            FZERO_LOG("INFO",
                      "  apu-worker: %s slices=%u/frame idle=%u late=%u occ=%u"
                      " (target 2136, 1 frame = 534)",
                      apu_worker_enabled() ? "ON" : "off",
                      (unsigned int)((wslices - s_w_slices) / n),
                      (unsigned int)(widle - s_w_idle),
                      (unsigned int)(wlate - s_w_late), occ);
            {   /* What the SDL audio thread costs. It is a pure consumer --
                 * it never advances the SPC -- so this is resampling plus the
                 * MSU/mod mix passes, and its RtlApuLock hold is what the APU
                 * worker contends with. */
                extern uint64_t g_audthread_us, g_audthread_calls,
                                g_audthread_max_us;
                static uint64_t p_us, p_calls;
                uint64_t d_us = g_audthread_us - p_us;
                uint64_t d_c  = g_audthread_calls - p_calls;
                p_us = g_audthread_us; p_calls = g_audthread_calls;
                FZERO_LOG("INFO",
                          "  aud-thread: %u us/frame (%u calls/frame,"
                          " %u us/call, max %u us)",
                          (unsigned int)(d_us / n), (unsigned int)(d_c / n),
                          (unsigned int)(d_c ? d_us / d_c : 0),
                          (unsigned int)g_audthread_max_us);
            }
            apu_worker_log_affinity();
            FZERO_LOG("INFO",
                      "  apu-audio: starve episodes=%u frames=%u (cumulative;"
                      " 0 means the cushion held)",
                      (unsigned int)g_rtl_audio_starve_episodes,
                      (unsigned int)g_rtl_audio_starve_frames);
            s_w_slices = wslices; s_w_idle = widle; s_w_late = wlate;
        }
        FZERO_LOG("INFO",
                  "  apu: sync=%u us/frame calls=%u/frame (%.0f%% of cpu=)"
                  " apuprobe=%u",
                  (unsigned int)(g_apuprof_us / n), (unsigned int)(g_apuprof_calls / n),
                  g_acc_cpu ? 100.0 * (double)g_apuprof_us / (double)g_acc_cpu : 0.0,
                  (unsigned int)((g_timer_ns * 2u * (g_apuprof_calls / n)) / 1000u));
#if FZERO_PPUPROF
        /* The draw half, attributed. probe= is this probe's own cost. */
        FZERO_LOG("INFO",
                  "  draw-split: ppu_runLine=%u hdma=%u irq_split=%u | lines=%u/frame"
                  " probe=%u (us/frame)",
                  (unsigned int)(g_fzprof_ppu / n), (unsigned int)(g_fzprof_hdma / n),
                  (unsigned int)(g_fzprof_split / n), (unsigned int)(g_fzprof_lines / n),
                  (unsigned int)((g_timer_ns * 8u * (g_fzprof_lines / n)) / 1000u));
        /* All three are measured directly, not derived by subtraction, so a
         * phase with no probe cannot hide inside another. sum= is the
         * cross-check against ppu_runLine above. */
        FZERO_LOG("INFO",
                  "  ppu-phases[compose=%s/%s]: spriteEval=%u bg+mode7=%u compose=%u"
                  " | sum=%u vs ppu_runLine=%u",
                  g_ppu_fast_compose ? "HOIST" : "ORIG",
                  g_ppu_fast_cgram_lut ? "LUT" : "orig",
                  (unsigned int)(g_ppuprof_eval / n), (unsigned int)(g_ppuprof_bg / n),
                  (unsigned int)(g_ppuprof_compose / n),
                  (unsigned int)((g_ppuprof_eval + g_ppuprof_bg + g_ppuprof_compose) / n),
                  (unsigned int)(g_fzprof_ppu / n));
#endif
#if FZERO_WIDEPROF
        /* The widescreen layers pass, attributed. `hdma=` above contains this
         * whole pass when widescreen is on. */
        {
            uint64_t cap = g_wideprof.t_cap_copy + g_wideprof.t_cap_run
                         + g_wideprof.t_cap_loop;
            FZERO_LOG("INFO",
                "  wide[copy=%s cov=%s hud=%s tiles=%s render=%s comp=%s pass=%s scr=%s oam=%s]:"
                " ProcessLine=%u = mask %u"
                " + build %u + move %u + merge %u | probe=%u (us/frame)",
                g_fzero_wide_sharedcopy ? "SHARED" : "full",
                g_fzero_wide_fastcov ? "SLOTS" : "render",
                g_fzero_wide_movehud ? "SPREAD" : "CENTRED",
                g_fzero_wide_sidetiles ? "SIDE" : "patch",
                g_fzero_wide_marginsonly ? "MARGINS" : "full",
                g_fzero_wide_flatdirect ? "DIRECT" : "pair",
                g_fzero_wide_nonative ? "ONE" : "two",
                g_fzero_wide_framescratch ? "FRAME" : "line",
                g_fzero_wide_keepoam ? "KEEP" : "wipe",
                (unsigned int)((g_wideprof.t_mask + g_wideprof.t_build
                                + g_wideprof.t_move + g_wideprof.t_flatten) / n),
                (unsigned int)(g_wideprof.t_mask / n),
                (unsigned int)(g_wideprof.t_build / n),
                (unsigned int)(g_wideprof.t_move / n),
                (unsigned int)(g_wideprof.t_flatten / n),
                (unsigned int)((g_timer_ns * (g_wideprof.n_reads / n)) / 1000u));
            FZERO_LOG("INFO",
                "  wide-capture: total=%u (copy %u + runLine %u + loop %u)"
                " called=%u ran=%u bytes=%uKB/frame | in mask %u, in move %u",
                (unsigned int)(cap / n),
                (unsigned int)(g_wideprof.t_cap_copy / n),
                (unsigned int)(g_wideprof.t_cap_run / n),
                (unsigned int)(g_wideprof.t_cap_loop / n),
                (unsigned int)(g_wideprof.n_cap_called / n),
                (unsigned int)(g_wideprof.n_cap_ran / n),
                (unsigned int)(g_wideprof.n_cap_bytes_kb / n),
                (unsigned int)(g_wideprof.t_capsub_mask / n),
                (unsigned int)(g_wideprof.t_capsub_move / n));
            FZERO_LOG("INFO",
                "  wide-sites: build own=%u (copy %u, %uKB over %u lines)"
                " | move own=%u = pre %u + copy %u + runLine %u + loops %u"
                " (%u lines, %uKB)",
                (unsigned int)(g_wideprof.t_build / n),
                (unsigned int)(g_wideprof.t_build_copy / n),
                (unsigned int)(g_wideprof.n_build_bytes_kb / n),
                (unsigned int)(g_wideprof.n_build_ran / n),
                (unsigned int)((g_wideprof.t_move - g_wideprof.t_capsub_move) / n),
                (unsigned int)((g_wideprof.t_move_pre
                                - g_wideprof.t_capsub_move) / n),
                (unsigned int)(g_wideprof.t_move_copy / n),
                (unsigned int)(g_wideprof.t_move_run / n),
                (unsigned int)(g_wideprof.t_move_loop / n),
                (unsigned int)(g_wideprof.n_move_ran / n),
                (unsigned int)(g_wideprof.n_move_bytes_kb / n));
            FZERO_LOG("INFO",
                "  wide-build: ground %u lines %u us | ppu_runLine %u lines %u us"
                " | centre-skipped %u, native-skipped %u, full-copy %u lines"
                " | limits=%s spared %lu lines/window (us/frame)",
                (unsigned int)(g_wideprof.n_build_ground / n),
                (unsigned int)(g_wideprof.t_build_ground / n),
                (unsigned int)(g_wideprof.n_build_run / n),
                (unsigned int)(g_wideprof.t_build_run / n),
                (unsigned int)(g_wideprof.n_build_skip / n),
                (unsigned int)(g_wideprof.n_native_skip / n),
                (unsigned int)(g_wideprof.n_build_fresh / n),
                g_ppu_margin_limits_hw ? "HW" : "old",
                g_ppu_margin_spared);
            g_ppu_margin_spared = 0;
            /* The parts of draw= that no line above itemises. loop= is the host
             * scanline loop measured per line; draw= minus it is the work
             * around the loop. build pre and post are BuildWideLine around its
             * render, centre is the 1 KB centre row copy. */
            {
                uint64_t loop = g_wideprof.t_loop_native + g_wideprof.t_loop_proc
                              + g_wideprof.t_loop_hdma + g_wideprof.t_loop_irq;
                FZERO_LOG("INFO",
                    "  draw-loop: native %u + ProcessLine %u + hdma %u + irq %u"
                    " = loop %u vs draw %u | build: pre %u post %u centre %u"
                    " | wide reads %u (us/frame)",
                    (unsigned int)(g_wideprof.t_loop_native / n),
                    (unsigned int)(g_wideprof.t_loop_proc / n),
                    (unsigned int)(g_wideprof.t_loop_hdma / n),
                    (unsigned int)(g_wideprof.t_loop_irq / n),
                    (unsigned int)(loop / n), (unsigned int)(g_acc_draw / n),
                    (unsigned int)(g_wideprof.t_build_pre / n),
                    (unsigned int)(g_wideprof.t_build_post / n),
                    (unsigned int)(g_wideprof.t_build_centre / n),
                    (unsigned int)(g_wideprof.n_reads / n));
            }
            /* The side array removes patch, and the margins-only render shrinks
             * render, so the total alone cannot tell them apart. */
            FZERO_LOG("INFO",
                "  wide-ground: prep %u + render %u + patch %u"
                " | probe=%u (us/frame)",
                (unsigned int)(g_fzero_ground_prep / n),
                (unsigned int)(g_fzero_ground_render / n),
                (unsigned int)(g_fzero_ground_patch / n),
                (unsigned int)((g_timer_ns * (g_fzero_ground_reads / n)) / 1000u));
            g_fzero_ground_prep = g_fzero_ground_render = g_fzero_ground_patch = 0;
            g_fzero_ground_reads = 0;
            FZERO_LOG("INFO",
                "  wide-ground memo: lookups %u room %u row %u panel %u (per frame)",
                (unsigned int)(g_fzero_ground_lookups / n),
                (unsigned int)(g_fzero_ground_room_reads / n),
                (unsigned int)(g_fzero_ground_row_reads / n),
                (unsigned int)(g_fzero_ground_panel_reads / n));
            g_fzero_ground_lookups = g_fzero_ground_room_reads = 0;
            g_fzero_ground_row_reads = g_fzero_ground_panel_reads = 0;
            memset(&g_wideprof, 0, sizeof(g_wideprof));
        }
#endif
        ResetPhaseAccumulators();
        s_last_fps_time = now;
    }
}

/* Drift-free frame pacing.
 *
 * The deadline advances by exactly one frame period; it is not resampled from
 * the clock after the sleep. sceKernelDelayThreadCB returns late by the
 * scheduler's granularity, and taking the wake-up time as the next frame's
 * start adds that overshoot to every period (16,639 us became 16,835 us).
 *
 * That is mainly an audio fault: the guest makes a fixed 534 natives per
 * frame, so a slow loop under-produces audio, and rtl_render_native's servo
 * can bend consumption by only +/-0.5%% (RTL_AUDIO_SERVO_MAX; more is audible
 * as pitch drift). The DSP ring would drain, starve and fade to silence.
 *
 * Resync instead of catching up when more than two periods behind: after a
 * real hitch, sleeping out the debt would run the game fast. */
static void FrameTiming(void) {
    g_frame_start_time += g_frame_time_us;
    uint64_t now = sceKernelGetProcessTimeWide();
    if (now < g_frame_start_time) {
        uint64_t sleep_us = g_frame_start_time - now;
        g_acc_sleep += sleep_us;
        sceKernelDelayThreadCB((SceUInt)sleep_us);
    } else {
        g_acc_over++;   /* frame missed its budget: no headroom left here */
        if (g_over_detail_n < FZ_OVER_CAP) {
            g_over_detail[g_over_detail_n].idx = g_acc_frames;
            g_over_detail[g_over_detail_n].cpu = g_frame_cpu_us;
            g_over_detail[g_over_detail_n].draw = g_frame_draw_us;
            g_over_detail[g_over_detail_n].aud = g_frame_aud_us;
            g_over_detail[g_over_detail_n].upl = g_frame_upl_us;
            g_over_detail[g_over_detail_n].cpy = g_frame_cpy_us;
            ++g_over_detail_n;
        }
        if (now - g_frame_start_time > 2 * g_frame_time_us)
            g_frame_start_time = now;
    }
}

static void Cleanup(void) {
    FZERO_LOG("INFO", "Cleaning up...");

    /* Stop the power callback thread first so it cannot race with the
     * final RtlWriteSram below. g_running is already false at this point. */
    StopPowerCallback();

    /* Join the APU worker before the audio thread, and both before anything
     * below frees emulator state: the worker cycles the SPC and the callback
     * reads the DSP ring, and g_spc_player owns the RAM under both. */
    apu_worker_stop();

    /* Stop audio first. SDL_DestroyAudioStream joins the audio thread, so
     * after this nothing is inside the callback. Everything below frees state
     * the callback renders from (g_spc_player owns the DSP's RAM). */
    audio_shutdown();

    if (g_snes_instance) {
        SaveSram();
    }

    if (g_rom_buffer) {
        free(g_rom_buffer);
        g_rom_buffer = NULL;
    }

    if (g_spc_player) {
        free(g_spc_player);
        g_spc_player = NULL;
    }

    input_shutdown();

    GxmPresentShutdown();
    if (g_window) {
        SDL_DestroyWindow(g_window);
        g_window = NULL;
    }
    SDL_Quit();

    platform_shutdown();

    FZERO_LOG("INFO", "Cleanup complete");
}

static void handle_sigabrt(int sig) {
    (void)sig;
    FZERO_LOG("ERROR", "FATAL: SIGABRT / abort() triggered!");
    sceKernelDelayThread(1000000);
}

int main(int argc, char **argv) {
    (void)argc;
    (void)argv;

    signal(SIGABRT, handle_sigabrt);
    freopen("ux0:/data/fzero_recomp/stderr.txt", "w", stderr);
    /* The runner reports SRAM write failures with fprintf(stderr, ...). A
     * freopen'd file stream is block-buffered, so on a hard kill (power off,
     * crash) those messages never reach the file. Unbuffer stderr so a failed
     * RtlWriteSram is visible in stderr.txt. */
    setvbuf(stderr, NULL, _IONBF, 0);
    freopen("ux0:/data/fzero_recomp/stdout.txt", "w", stdout);

    FZERO_LOG("INFO", "F-Zero Recomp - PS Vita starting (Heap: %u MB)", _newlib_heap_size_user / (1024 * 1024));

    if (!InitAll()) {
        FZERO_LOG("ERROR", "Initialization failed!");
        Cleanup();
        return 1;
    }

    if (!LoadAndInitSNES()) {
        FZERO_LOG("ERROR", "ROM loading or SNES initialization failed!");
        Cleanup();
        return 1;
    }

    /* The audio thread and the APU worker render from g_snes, so they start
     * only now. */
    apu_worker_start();
#if FZERO_COREBENCH
    /* After apu_worker_start, which pins the game thread to core 0. The APU
     * worker is idle here (the game loop has not started, so
     * rtl_apu_worker_active is false), so this runs before audio_start. */
    CoreBench();
#endif
    audio_start();

    FZERO_LOG("INFO", "Entering main game loop");
    
    g_frame_start_time = sceKernelGetProcessTimeWide();
    
    while (g_running) {
        HandleFrame();
        FrameTiming();
    }

    Cleanup();
    FZERO_LOG("INFO", "F-Zero Recomp exited cleanly");

    return 0;
}
