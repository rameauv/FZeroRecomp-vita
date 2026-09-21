/*
 * F-Zero (US) game-layer frame driver.
 *
 * Frame model (see snesrecomp/docs/FRAME_MODEL_HOSTS.md):
 *
 *   Reset at $00:8000 initializes the machine and enters the cooperative
 *   main loop. Each iteration establishes 8-bit registers, clears the
 *   software NMI flag at WRAM $7E:0040, and waits at $00:803A for vblank.
 *
 *   NMI ($00:80D9) runs first each vblank, does all per-frame PPU writes, then
 *   sets the software NMI flag to 0xFF. The main loop then
 *   runs one mode dispatch and loops back to the spin.
 *
 *   This is the exact "cooperative scheduler" shape interp_bridge's
 *   scheduler helper models: we enter it at the spin PC ($00:803A), yield
 *   when it reaches the spin with the flag cleared (one frame's dispatch
 *   complete). The host drives NMI delivery; the main loop is LLE'd through
 *   the bridge so every AOT'd task body bounces to compiled code.
 *
 *   The reset entry has no AOT body in the current manifest (it is one of the
 *   LLE-only `unproven_callee_exit` nodes), so the reset path itself runs
 *   through the same scheduler bridge: entering at $00:8000 runs boot init,
 *   falls into the loop, and yields at the spin once the flag is cleared.
 */
#include "fzero_rtl.h"
#include "fzero_layers.h"
#include "fzero_scene.h"
#include "common_cpu_infra.h"
#include "common_rtl.h"
#include "snes/snes.h"
#include "snes/ppu.h"
#include "snes/interp_bridge.h"
#include "cpu_state.h"
#include "funcs.h"
#include "platform.h"

/* Reset vector: $00:8000. */
static const uint32 kFZeroResetPc = 0x008000u;
/* Main loop waits here for the software NMI flag. */
static const uint32 kFZeroLoopSpinPc = 0x00803Au;
/* Software NMI flag in WRAM, with direct page zero. */
static const uint16 kFZeroNmiFlagAddr = 0x0040u;
/* Draw-loop profiling (see main_vita.c's heartbeat). Splits FZeroDrawPpuFrame
 * into the native rasteriser, the per-line HDMA walk, and the raster-split IRQ
 * dispatch, so the emu cost can be attributed instead of inferred. Two timer
 * reads per scanline: the heartbeat reports the calibrated cost of those so the
 * probe's own overhead is visible rather than silently folded into the result. */
uint64_t g_fzprof_ppu, g_fzprof_hdma, g_fzprof_split;
/* cpu= split: the recompiled NMI handler vs the main-loop scheduler
 * bridge. Four timer reads per frame, so effectively free. */
uint64_t g_fzprof_nmi, g_fzprof_sched;
uint32_t g_fzprof_lines, g_fzprof_frames;

static FZeroLayers *g_layers;

/* Deliberately OUTSIDE any FZERO_DIAG guard. This lived inside that block, so
 * a normal build had the whole layers pipeline compiled in and calling through
 * `if (g_layers)` -- with nothing able to set g_layers, and the setter not even
 * defined. That is a large part of why widescreen had never run on Vita. */
void FZeroSetLayers(FZeroLayers *layers) { g_layers = layers; }

/* Bring-up logging window. Network logging is synchronous UDP, so a handful of
 * lines per frame is a real cost at the frame rates this port currently runs
 * at; keep the per-frame chatter inside the probe window and let errors
 * (bails, off-rails, stack imbalance) always through. */
#if FZERO_DIAG
#define FZERO_PROBE (g_fzero_probe_frame <= 40)
#else
/* Diagnostics off: every `if (FZERO_PROBE)` block folds away at compile time. */
#define FZERO_PROBE 0
#endif
/* The register-write probes above stop at 40 to bound log volume, but the
 * wedge is at frame ~46 -- every draw-half probe was therefore silent on the
 * failing frame and the three before it. Frame-level probes (draw begin/end,
 * per-split band+line, pacing) use this wider window instead; they are a
 * handful of lines per frame, not one per register write. */
#if FZERO_DIAG
#define FZERO_PROBE_DRAW (g_fzero_probe_frame <= 60)
#else
#define FZERO_PROBE_DRAW 0
#endif

/* ── Vita bring-up instrumentation ───────────────────────────────────────
 * Called from WriteReg() for every $2100-$213F write. Logs only the display
 * registers that can produce an all-black frame, and only for the first few
 * frames, so the netlog stays readable. */
int g_fzero_probe_frame = 0;   /* also read by main_vita.c's NMI trampoline */

#if FZERO_DIAG
/* Everything from here to the matching #endif is bring-up diagnostics, gated
 * by the FZERO_DIAG cmake option (OFF by default). The hook call sites in the
 * snesrecomp submodule are gated by the same macro, so with it off these
 * definitions are not merely unused -- nothing references them. */
void FZeroLogPpuRegWrite(uint16 reg, uint8 value, int line) {
  if (g_fzero_probe_frame > 40) return;
  if (reg != 0x2100 && reg != 0x212C && reg != 0x212D) return;
  FZERO_LOG("INFO", "  ppuw f%d $%04X=0x%02X vPos=%d", g_fzero_probe_frame,
            (unsigned int)reg, (unsigned int)value, line);
}

/* Companion probe for the $4200-$421F block. $4200 arms NMI (bit 7) and the
 * H/V timer IRQ (bits 4/5); $4207-$420A carry the H and V targets each raster
 * split re-arms for the next band. If the chain is never armed, vIrqEnabled
 * stays false, FZeroDrawPpuFrame's trigger stays -1, and no split ever runs. */
void FZeroLogCpuRegWrite(uint16 reg, uint8 value, int line) {
  if (g_fzero_probe_frame > 40) return;
  if (reg != 0x4200 && reg != 0x4207 && reg != 0x4208 &&
      reg != 0x4209 && reg != 0x420A) return;
  FZERO_LOG("INFO", "  cpuw f%d $%04X=0x%02X vPos=%d", g_fzero_probe_frame,
            (unsigned int)reg, (unsigned int)value, line);
}

/* Off-rails capture, latched live by the interpreter (see interp_bridge.c).
 * The wedge loop is far longer than the 8192-entry step ring, so by bail time
 * the ring holds only the garbage loop; the crossing has to be recorded at the
 * moment it happens. Only the FIRST crossing per frame is kept. */
#define FZERO_APPROACH 12
static struct {
  uint32_t prev_pc, pc;
  uint16_t prev_sp;
  uint8_t prev_op, op;
  int valid;
  /* The steps leading INTO the crossing. The wedge ring read at bail time
   * only shows the garbage loop afterwards; the question is how control
   * reached the handler tail, so snapshot the approach at crossing time. */
  struct { uint32_t pc; uint16_t sp; uint8_t op; uint8_t ok; }
      approach[FZERO_APPROACH];
} g_fzero_offrails;
static int g_fzero_offrails_ever;   /* never cleared: the first crossing of the
                                     * whole run is the one that matters */

static void FZeroDumpWatch(void);

void interp_bridge_note_offrails_vita(uint32_t prev_pc, uint8_t prev_op,
                                      uint16_t prev_sp, uint32_t pc,
                                      uint8_t op) {
  if (g_fzero_offrails.valid) return;
  g_fzero_offrails.prev_pc = prev_pc;
  g_fzero_offrails.prev_op = prev_op;
  g_fzero_offrails.prev_sp = prev_sp;
  g_fzero_offrails.pc = pc;
  g_fzero_offrails.op = op;
  /* back=1 is the step just recorded (the crossing itself); walk backwards
   * from there while the ring still holds the approach. */
  for (int i = 0; i < FZERO_APPROACH; ++i) {
    uint32_t a_pc; uint8_t a_op; uint16_t a_sp; int32_t a_fr;
    g_fzero_offrails.approach[i].ok =
        interp_bridge_recent_step(i + 1, &a_pc, &a_op, &a_sp, &a_fr) ? 1 : 0;
    if (!g_fzero_offrails.approach[i].ok) continue;
    g_fzero_offrails.approach[i].pc = a_pc;
    g_fzero_offrails.approach[i].op = a_op;
    g_fzero_offrails.approach[i].sp = a_sp;
  }
  g_fzero_offrails.valid = 1;

  /* Report the FIRST crossing of the run immediately, with its approach.
   * Waiting for a bail loses it: the frame that first jumps into the stack
   * page need not bail in that same frame -- frame 46 did not -- and the
   * per-frame clear then wipes it before anything prints. */
  if (!g_fzero_offrails_ever) {
    g_fzero_offrails_ever = 1;
    FZERO_LOG("WARN", "FIRST OFF-RAILS f%d: $%06X op=%02X sp=%04X  --->  $%06X op=%02X",
              g_fzero_probe_frame, (unsigned int)prev_pc, (unsigned int)prev_op,
              (unsigned int)prev_sp, (unsigned int)pc, (unsigned int)op);
    FZeroDumpWatch();
    for (int i = FZERO_APPROACH - 1; i >= 0; --i) {
      if (!g_fzero_offrails.approach[i].ok) continue;
      FZERO_LOG("WARN", "  first-approach[-%d]: $%06X op=%02X sp=%04X", i,
                (unsigned int)g_fzero_offrails.approach[i].pc,
                (unsigned int)g_fzero_offrails.approach[i].op,
                (unsigned int)g_fzero_offrails.approach[i].sp);
    }
  }
}

/* Every LLE arrival into $00:8601-$00:8680 this frame, with the step that
 * jumped there and the width it arrived carrying. The bands need M=8/X=16,
 * which $8601's REP #$10 establishes; an entry that skips $8601 arrives with
 * whatever the caller had, and an 8-bit X mis-decodes LDX #imm by one byte. */
/* Emulation-mode flips seen this frame. Native->emulation is the fault that
 * matters: it pins M and X to 8-bit against the REP #$30 at $86AA. */
#define FZERO_EMU_FLIPS 8
static struct { uint32_t pc; uint16_t sp; uint8_t op, to_e; int frame; }
    g_fzero_emu_flip[FZERO_EMU_FLIPS];
static int g_fzero_emu_flip_n;   /* cumulative: a flip in an earlier frame is
                                  * exactly what a per-frame reset would hide */

/* ── Return-address watch ────────────────────────────────────────────────
 * The RTS at $03:8D0B pops its return address from $01F4/$01F5. Its only
 * caller is $03:823C JSR $8CA3, which writes exactly those two bytes, so any
 * other write to them between the call and the return is the corruption.
 *
 * Recorded into a ring rather than logged live: this is the stack page, every
 * push touches it, and the netlog is synchronous UDP. The last few writes
 * before the fault are what matter, so dump them when it trips. */
#define FZERO_WATCH_LO   0x01F0u
#define FZERO_WATCH_HI   0x01FFu
#define FZERO_WATCH_N    24
static struct {
  uint32_t ipc;
  const char *fn;
  uint16_t addr, val, s;
  uint8_t bank, width;
  int frame;
} g_fzero_watch[FZERO_WATCH_N];
static int g_fzero_watch_n;      /* total seen; index is n % FZERO_WATCH_N */

void FZeroWatchWrite(uint8 bank, uint16 addr, uint16 v, int width) {
  if (addr < FZERO_WATCH_LO || addr > FZERO_WATCH_HI) return;
  extern const char *g_last_recomp_func;
  extern uint32_t g_interp816_cur_pc;
  int i = g_fzero_watch_n++ % FZERO_WATCH_N;
  g_fzero_watch[i].bank = bank;
  g_fzero_watch[i].addr = addr;
  g_fzero_watch[i].val = v;
  g_fzero_watch[i].width = (uint8_t)width;
  g_fzero_watch[i].s = g_cpu.S;
  g_fzero_watch[i].ipc = g_interp816_cur_pc;
  g_fzero_watch[i].fn = g_last_recomp_func;
  g_fzero_watch[i].frame = g_fzero_probe_frame;
}

static void FZeroDumpWatch(void) {
  int have = g_fzero_watch_n < FZERO_WATCH_N ? g_fzero_watch_n : FZERO_WATCH_N;
  FZERO_LOG("WARN", "  last %d writes to $%04X-$%04X (of %d):", have,
            FZERO_WATCH_LO, FZERO_WATCH_HI, g_fzero_watch_n);
  for (int k = have; k >= 1; --k) {
    int i = (g_fzero_watch_n - k) % FZERO_WATCH_N;
    FZERO_LOG("WARN", "    w[-%d] f%d %02X:%04X=%0*X w%d S=%04X ipc=$%06X %s",
              k - 1, g_fzero_watch[i].frame, (unsigned int)g_fzero_watch[i].bank,
              (unsigned int)g_fzero_watch[i].addr,
              g_fzero_watch[i].width * 2,
              (unsigned int)(g_fzero_watch[i].val &
                             (g_fzero_watch[i].width == 1 ? 0xFFu : 0xFFFFu)),
              (int)g_fzero_watch[i].width, (unsigned int)g_fzero_watch[i].s,
              (unsigned int)g_fzero_watch[i].ipc,
              g_fzero_watch[i].fn ? g_fzero_watch[i].fn : "?");
  }
}

/* First step that takes S outside the $0100-$01FF stack page. */
void interp_bridge_note_stack_escape_vita(uint32_t pc, uint8_t op, uint16_t sp) {
  FZERO_LOG("WARN", "STACK ESCAPE f%d: after $%06X op=%02X, S=%04X",
            g_fzero_probe_frame, (unsigned int)pc, (unsigned int)op,
            (unsigned int)sp);
}

void interp_bridge_note_emulation_vita(uint32_t pc, uint8_t op, uint16_t sp,
                                       uint8_t to_e) {
  if (g_fzero_emu_flip_n >= FZERO_EMU_FLIPS) return;
  int i = g_fzero_emu_flip_n++;
  g_fzero_emu_flip[i].pc = pc;
  g_fzero_emu_flip[i].op = op;
  g_fzero_emu_flip[i].sp = sp;
  g_fzero_emu_flip[i].to_e = to_e;
  g_fzero_emu_flip[i].frame = g_fzero_probe_frame;
}

#define FZERO_IRQ_ENTRIES 8
static struct {
  uint32_t from_pc, pc;
  uint16_t sp;
  uint8_t from_op, mf, xf, e;
} g_fzero_irq_entry[FZERO_IRQ_ENTRIES];
static int g_fzero_irq_entry_n;

void interp_bridge_note_irq_entry_vita(uint32_t from_pc, uint8_t from_op,
                                       uint32_t pc, uint16_t sp,
                                       uint8_t mf, uint8_t xf, uint8_t e) {
  if (g_fzero_irq_entry_n >= FZERO_IRQ_ENTRIES) return;
  int i = g_fzero_irq_entry_n++;
  g_fzero_irq_entry[i].from_pc = from_pc;
  g_fzero_irq_entry[i].from_op = from_op;
  g_fzero_irq_entry[i].pc = pc;
  g_fzero_irq_entry[i].sp = sp;
  g_fzero_irq_entry[i].mf = mf;
  g_fzero_irq_entry[i].xf = xf;
  g_fzero_irq_entry[i].e = e;
}

static void FZeroLogBailTrace(void) {
  uint32_t pc; uint8_t op; uint16_t sp; int32_t fr;
  if (g_fzero_offrails.valid)
    FZERO_LOG("WARN", "  OFF-RAILS: $%06X op=%02X sp=%04X  --->  $%06X op=%02X",
              (unsigned int)g_fzero_offrails.prev_pc,
              (unsigned int)g_fzero_offrails.prev_op,
              (unsigned int)g_fzero_offrails.prev_sp,
              (unsigned int)g_fzero_offrails.pc,
              (unsigned int)g_fzero_offrails.op);
  else
    FZERO_LOG("WARN", "  OFF-RAILS: no crossing recorded this frame");
  for (int i = 0; i < g_fzero_emu_flip_n; ++i)
    FZERO_LOG("WARN", "  EMU-FLIP[%d]: f%d at $%06X op=%02X sp=%04X -> e=%d"
              " (2 = arrived in emulation)", i, g_fzero_emu_flip[i].frame,
              (unsigned int)g_fzero_emu_flip[i].pc,
              (unsigned int)g_fzero_emu_flip[i].op,
              (unsigned int)g_fzero_emu_flip[i].sp,
              (int)g_fzero_emu_flip[i].to_e);
  for (int i = 0; i < g_fzero_irq_entry_n; ++i)
    FZERO_LOG("WARN", "  IRQ-ENTRY[%d]: from $%06X op=%02X -> $%06X "
              "sp=%04X m=%d x=%d e=%d", i,
              (unsigned int)g_fzero_irq_entry[i].from_pc,
              (unsigned int)g_fzero_irq_entry[i].from_op,
              (unsigned int)g_fzero_irq_entry[i].pc,
              (unsigned int)g_fzero_irq_entry[i].sp,
              (int)g_fzero_irq_entry[i].mf, (int)g_fzero_irq_entry[i].xf,
              (int)g_fzero_irq_entry[i].e);
  if (!g_fzero_irq_entry_n)
    FZERO_LOG("WARN", "  IRQ-ENTRY: none this frame (handler reached only via AOT)");
  for (int i = FZERO_APPROACH - 1; i >= 0; --i) {
    if (!g_fzero_offrails.approach[i].ok) continue;
    FZERO_LOG("WARN", "  approach[-%d]: $%06X op=%02X sp=%04X", i,
              (unsigned int)g_fzero_offrails.approach[i].pc,
              (unsigned int)g_fzero_offrails.approach[i].op,
              (unsigned int)g_fzero_offrails.approach[i].sp);
  }
  for (int back = 4; back >= 1; back--) {
    if (!interp_bridge_recent_step(back, &pc, &op, &sp, &fr)) continue;
    FZERO_LOG("WARN", "  wedge[-%d]: $%06X op=%02X sp=%04X", back,
              (unsigned int)pc, (unsigned int)op, (unsigned int)sp);
  }
}

#endif  /* FZERO_DIAG */

void FZeroRunOneFrameOfGame(void) {
  // First-call reset gate (host-side bool, independent of WRAM contents).
  static bool g_did_reset = false;
  static bool g_first_frame_done = false;

  if (!g_did_reset) {
    cpu_state_init(&g_cpu, g_ram);
    /* Run boot through the scheduler bridge: reset falls into the
     * main loop and reaches the spin with the NMI flag cleared. */
    int boot_ret = interp_bridge_run_scheduler(&g_cpu, kFZeroResetPc,
                                              kFZeroLoopSpinPc,
                                              kFZeroNmiFlagAddr);
#if FZERO_DIAG
    /* PC performs ~100k stack writes during boot; Vita performs almost none and
     * leaves S=01FD (2 bytes of an unreturned JSR). The boot run is exiting
     * early -- report how it ended and where the interpreter actually was. */
    { extern unsigned long g_interp_paired_frame_repairs;
      extern struct InterpRepairLog { uint32_t target, site; int air;
                                      uint16_t sp_pre, sp_post; uint8_t fs;
                                      int sched_depth, in_sched, owner_depth; }
          g_interp_repair_log[8];
      extern unsigned long g_interp_bounce_imbalances;
      FZERO_LOG("WARN", "BOOT paired-frame repairs=%lu",
                g_interp_paired_frame_repairs);
      FZERO_LOG("WARN", "BOOT bounce imbalances=%lu", g_interp_bounce_imbalances);
      unsigned long _n = g_interp_bounce_imbalances;
      if (_n > 8) _n = 8;
      for (unsigned long i = 0; i < _n; ++i)
        FZERO_LOG("WARN", "  imbal[%lu] site=$%06X -> $%06X air=%d "
                  "sp_pre=%04X sp_post=%04X fs=%d sched=%d in_sched=%d "
                  "owner=%d", i,
                  (unsigned int)g_interp_repair_log[i].site,
                  (unsigned int)g_interp_repair_log[i].target,
                  g_interp_repair_log[i].air,
                  (unsigned int)g_interp_repair_log[i].sp_pre,
                  (unsigned int)g_interp_repair_log[i].sp_post,
                  (int)g_interp_repair_log[i].fs,
                  g_interp_repair_log[i].sched_depth,
                  g_interp_repair_log[i].in_sched,
                  g_interp_repair_log[i].owner_depth); }
    { extern struct ApuPortProbe { unsigned adr, val; unsigned long long master,
                    frame_start, guest_cycle, port_clock; int frame; }
          g_apu_port_probe[8];
      extern unsigned g_apu_port_probe_n;
      extern unsigned long g_apu_port_reads_total;
      extern struct ApuPortProbe g_apu_port_last;
      FZERO_LOG("WARN", "BOOT apu-port reads total=%lu",
                g_apu_port_reads_total);
      FZERO_LOG("WARN", "  apu[LAST] f%d $%04X=%02X master=%llu "
                "guest=%llu portclk=%llu", g_apu_port_last.frame,
                g_apu_port_last.adr, g_apu_port_last.val,
                g_apu_port_last.master, g_apu_port_last.guest_cycle,
                g_apu_port_last.port_clock);
      unsigned _n = g_apu_port_probe_n > 8 ? 8 : g_apu_port_probe_n;
      for (unsigned i = 0; i < _n; ++i)
        FZERO_LOG("WARN", "  apu[%u] f%d $%04X=%02X master=%llu start=%llu "
                  "guest=%llu portclk=%llu", i, g_apu_port_probe[i].frame,
                  g_apu_port_probe[i].adr, g_apu_port_probe[i].val,
                  g_apu_port_probe[i].master, g_apu_port_probe[i].frame_start,
                  g_apu_port_probe[i].guest_cycle,
                  g_apu_port_probe[i].port_clock); }
    FZERO_LOG("WARN", "BOOT ret=%d S=%04X PB=%02X curpc=%06X e=%d m=%d x=%d "
              "P=%02X flag$40=%02X", boot_ret, (unsigned int)g_cpu.S,
              (unsigned int)g_cpu.PB, (unsigned int)g_interp816_cur_pc,
              (int)g_cpu.emulation, (int)g_cpu.m_flag, (int)g_cpu.x_flag,
              (unsigned int)g_cpu.P, (unsigned int)g_ram[kFZeroNmiFlagAddr]);
    for (int back = 24; back >= 1; --back) {
      uint32_t bpc; uint8_t bop; uint16_t bsp; int32_t bfr;
      if (!interp_bridge_recent_step(back, &bpc, &bop, &bsp, &bfr)) continue;
      FZERO_LOG("WARN", "  boot[-%02d]: $%06X op=%02X sp=%04X", back,
                (unsigned int)bpc, (unsigned int)bop, (unsigned int)bsp);
    }
#else
    (void)boot_ret;
#endif
    g_did_reset = true;
  }

  /* Frame 0 is special: on real hardware the first NMI fires AFTER reset
   * completes and the main loop has spun up (flag cleared, 8-bit registers).
   * Skipping NMI on frame 0 lets the handler save the main loop's P state.
   * The reset run above already reached the spin,
   * so frame 0 yields there with the flag still cleared. */
  if (g_first_frame_done) {
    /* Pair presentation metadata with the OAM/graphics upload about to run.
     * The following game update produces the next frame's guest buffers. */
    if (g_layers) {
      g_layers->wide_scene = FZeroSceneWide(g_ram);
      g_layers->native_oam = g_ram[0x50] == 0;
      g_layers->intro_panorama = FZeroSceneIntro(g_ram);
      g_layers->results_layout = FZeroSceneResults(g_ram) ||
          (g_layers->wide_scene && g_ram[0x54] == 2 && g_ram[0xc3] == 0x11);
      /* Rank slots become explosion pieces at counter 7. The full native
       * upload starts later, so upload mode alone misses the first phases. */
      g_layers->crash_layout = g_layers->wide_scene && g_ram[0x54] == 2 &&
          g_ram[0xc3] == 0x40 && (g_layers->native_oam || g_ram[0xcf] >= 7);
      g_layers->hud_layout = g_layers->wide_scene &&
          (!g_layers->native_oam || g_layers->crash_layout) &&
          !g_layers->intro_panorama && !FZeroSceneTitle(g_ram) &&
          !FZeroSceneResults(g_ram) && g_ram[0xc3] != 0x11;
      /* Finish/loss, crash and GP ending cameras retain the instruments.
       * Crash uploads also retain selective filtering; their effect slots
       * are excluded from instrument capture and movement. Intro and standalone
       * result uploads use their reduced score/lives layout. */
      g_layers->move_hud = g_layers->hud_layout ||
          (g_layers->wide_scene && g_layers->native_oam &&
           (g_layers->intro_panorama || g_layers->results_layout)) ||
          (g_layers->wide_scene && g_ram[0x54] == 2 &&
           (g_ram[0xc3] == 0x40 ||
            (g_ram[0xc3] == 0x11 && !g_layers->native_oam)));
      FZeroVehiclesPrepare(&g_layers->vehicles, g_ram, g_rom, 0x80000);
    }
    if (g_layers) FZeroGroundPrepare(&g_layers->ground, g_ram);
    /* NMI handler runs BEFORE the main-loop game code each frame. Assert the
     * hardware NMI latch so the recompiled handler's read of $4210 (RDNMI)
     * returns bit 7 = 1, matching real hardware; the read clears the latch. */
    g_snes->inNmi = true;
    /* Model the hardware NMI-entry push so the handler returns through a real
     * interrupt frame instead of over-popping the guest stack. */
    cpu_push_interrupt_frame(&g_cpu);
    g_fzero_probe_frame++;
#if FZERO_DIAG
    g_fzero_offrails.valid = 0;
#endif
#if FZERO_DIAG
    g_fzero_irq_entry_n = 0;
#endif
    if (FZERO_PROBE)
  FZERO_LOG("INFO", "FZero: calling bank_00_80D9 (NMI)...");
    if (FZERO_PROBE_DRAW)
      FZERO_LOG("INFO", "  NMI entry S=%04X m=%d x=%d e=%d P=%02X",
                (unsigned int)g_cpu.S, (int)g_cpu.m_flag, (int)g_cpu.x_flag,
                (int)g_cpu.emulation, (unsigned int)g_cpu.P);
    { uint16 _s_before = (uint16)(g_cpu.S + (g_cpu.emulation ? 3 : 4));
      uint64_t _tn = sceKernelGetProcessTimeWide();
      bank_00_80D9(&g_cpu);
      g_fzprof_nmi += sceKernelGetProcessTimeWide() - _tn;
      if (g_cpu.S != _s_before && FZERO_PROBE_DRAW)
        FZERO_LOG("WARN", "  NMI UNBALANCED S: expected=%04X after=%04X (net %+d) e=%d",
                  (unsigned int)_s_before, (unsigned int)g_cpu.S,
                  (int)(int16_t)(g_cpu.S - _s_before), (int)g_cpu.emulation);
    }
    if (FZERO_PROBE)
  FZERO_LOG("INFO", "FZero: NMI done. Flag $40=0x%02X. Calling scheduler...", g_ram[kFZeroNmiFlagAddr]);
    if (FZERO_PROBE)
      FZERO_LOG("INFO", "FZero: post-NMI baseline inidisp=0x%02X tm=0x%02X",
                (unsigned int)g_ppu->inidisp, (unsigned int)g_ppu->screenEnabled[0]);

    /* Main loop: yield when it reaches the vblank-wait spin with the flag
     * cleared (one mode dispatch complete). */
    if (FZERO_PROBE)
      FZERO_LOG("INFO", "FZero: S at scheduler entry=%04X", (unsigned int)g_cpu.S);
    uint64_t _ts = sceKernelGetProcessTimeWide();
    int sched_ret = interp_bridge_run_scheduler(&g_cpu, kFZeroLoopSpinPc, kFZeroLoopSpinPc,
                                                kFZeroNmiFlagAddr);
    g_fzprof_sched += sceKernelGetProcessTimeWide() - _ts;
    if (FZERO_PROBE_DRAW)
      FZERO_LOG("INFO", "  sched exit S=%04X m=%d x=%d e=%d P=%02X",
                (unsigned int)g_cpu.S, (int)g_cpu.m_flag, (int)g_cpu.x_flag,
                (int)g_cpu.emulation, (unsigned int)g_cpu.P);
    if (FZERO_PROBE)
      FZERO_LOG("INFO", "FZero: scheduler returned %d. Flag $40=0x%02X", sched_ret, g_ram[kFZeroNmiFlagAddr]);
    if (!sched_ret) {
      /* 0 = the interpreter hit its 2M-step cap and bailed: this frame's mode
       * dispatch never completed, so whatever it was going to write to the PPU
       * (including the fade-in's INIDISP brightness ramp) never happened. */
      extern const char *g_last_recomp_func;
      FZERO_LOG("WARN", "FZero: scheduler BAILED (step cap) PB=0x%02X DB=0x%02X S=0x%04X last=%s",
                (unsigned int)g_cpu.PB, (unsigned int)g_cpu.DB, (unsigned int)g_cpu.S,
                g_last_recomp_func ? g_last_recomp_func : "<none>");
#if FZERO_DIAG
      FZeroLogBailTrace();
#endif
    }
  }
  g_first_frame_done = true;
}

void FZeroDrawPpuFrame(void) {
  SimpleHdma hdma_chans[8];

  Dma *dma = g_dma;

  /* This host walks per-line HDMA itself (SimpleHdma below). Suppress the
   * beam simulator's own HDMA engine while rendering: its walk is positioned
   * by the beam (which is not at line 0 here; it is wherever the game logic
   * left it), so a dma_doHdma firing inside a raster IRQ would write the
   * window/scroll registers with the wrong band and persist through the next
   * host band hold, causing horizon-band flicker. */
  const bool beam_hdma_was_enabled = !g_snes->hdmaBeamOff;
  snes_set_hdma_beam_enabled(g_snes, false);

  /* Re-arm HDMA channels from the $420C latch (the runner tracks it in
   * g_snesrecomp_last_hdmaen on every $420C write, including the NMI upload). */
  dma_startDma(dma, g_snesrecomp_last_hdmaen, true);

  /* F-Zero uses HDMA channels 1-7 (Mode 7 matrix/scroll splits); inactive
   * channels are no-ops (table == NULL). */
  for (int i = 0; i < 8; i++)
    SimpleHdma_Init(&hdma_chans[i], &dma->channel[i]);

  /* F-Zero arms the H/V timer IRQ ($4200 = $B1, V count $4209 = $12 = 18)
   * and chains four raster splits per frame at lines 18/28/47/86, each
   * handler re-arming the V count for the next band. The PPU line argument
   * is already the hardware scanline (line 0 is the pre-render line). Apply
   * the HBlank handler after drawing vTimer, so its writes and that line's
   * HDMA agree on the following scanline. Adding one leaves a row in the old
   * mode after the new band's scroll/matrix data has arrived.
   * The runner's vTimer is the target line, and
   * the IRQ handler reads $4211 (HW_TIMEUP), which returns inIrq<<7 and
   * clears the latch. Assert inIrq to take the timer path. */
  int trigger = g_snes->vIrqEnabled ? g_snes->vTimer : -1;
  int splits = 0;
  if (FZERO_PROBE_DRAW)
    FZERO_LOG("INFO", "FZero: draw begin trigger=%d vIrqEn=%d hIrqEn=%d "
              "vTimer=%d ($0041)=$%02X%02X", trigger, (int)g_snes->vIrqEnabled,
              (int)g_snes->hIrqEnabled, (int)g_snes->vTimer,
              (unsigned int)g_ram[0x42], (unsigned int)g_ram[0x41]);

  for (int i = 0; i <= 224; i++) {
#if FZERO_PPUPROF
    uint64_t tp_a = sceKernelGetProcessTimeWide();
#endif
    ppu_runLine(g_ppu, i);
#if FZERO_PPUPROF
    uint64_t tp_b = sceKernelGetProcessTimeWide();
    g_fzprof_ppu += tp_b - tp_a;
    ++g_fzprof_lines;
#endif
    /* Capture before HDMA/IRQ changes the raster state. Recharge reuses HUD
     * slots, whose visible pixels are already protected by sprite capture.
     * Keep scene effects active while the repair animation enters and leaves;
     * its lifetime must not switch the whole screen to Original colours. */
    if (g_layers) {
      /* Use the policy paired with this upload, not the next guest update's
       * menu or exception state. That update can already describe a new frame. */
      FZeroLayersProcessLine(g_layers, g_ppu, i,
                            g_layers->wide_scene, g_layers->hud_layout);
    }
    for (int c = 0; c < 8; c++)
      SimpleHdma_DoLine(&hdma_chans[c]);
#if FZERO_PPUPROF
    { uint64_t n = sceKernelGetProcessTimeWide(); g_fzprof_hdma += n - tp_b; tp_b = n; }
#endif
    if (i == trigger) {
      ++splits;
      /* PC host form: assert the timer latch, push the hardware interrupt
       * frame, and call the generated handler. The alias brackets its body
       * with cpu_interrupt_context_enter()/leave(), so an LLE tail that
       * crosses out of the AOT body stops on its own RTI instead of
       * interpreting the placeholder return PC this push installs. */
      uint16 s_before = g_cpu.S;
      g_snes->inIrq = true;
      cpu_push_interrupt_frame(&g_cpu);
      /* The handler opens SEP #$20 / REP #$10, so every band below $860F's
       * JMP ($0041) must run M=8 / X=16 and the compiled bands are all M1X0.
       * Landing on the M1X1 case there means REP #$10 did not clear x_flag,
       * which on a 65816 is what emulation mode does -- E=1 forces X to 8-bit
       * and pins the mirror regardless of what the body writes to P. Record
       * the entry state and what the flags actually became. */
      uint8 mf0 = g_cpu.m_flag, xf0 = g_cpu.x_flag;
      uint8 e0 = g_cpu.emulation, p0 = g_cpu.P;
      /* Which band this split dispatches to, so a width that differs per band
       * is visible. The wedge is in the band armed for line 28 ($863C), whose
       * entry the splits==1 probe never showed. */
      uint8 b41 = g_ram[0x41], b42 = g_ram[0x42];
      bank_00_8601(&g_cpu);
      if (FZERO_PROBE_DRAW)
        FZERO_LOG("INFO", "  IRQ split=%d band=$%02X%02X line=%d "
                  "m=%d x=%d e=%d P=%02X -> m=%d x=%d e=%d P=%02X",
                  splits, (unsigned int)b42, (unsigned int)b41, i,
                  (int)mf0, (int)xf0, (int)e0, (unsigned int)p0,
                  (int)g_cpu.m_flag, (int)g_cpu.x_flag, (int)g_cpu.emulation,
                  (unsigned int)g_cpu.P);
      /* Diagnostic only — the PC host does not restore S, so leave it alone
       * and let any imbalance show rather than masking it. */
      if (g_cpu.S != s_before && FZERO_PROBE_DRAW)
        FZERO_LOG("WARN", "  IRQ line=%d S %04X->%04X (net %+d)", i,
                  (unsigned int)s_before, (unsigned int)g_cpu.S,
                  (int)(int16_t)(g_cpu.S - s_before));
      trigger = g_snes->vIrqEnabled ? g_snes->vTimer : -1;
    }
#if FZERO_PPUPROF
    { uint64_t n = sceKernelGetProcessTimeWide(); g_fzprof_split += n - tp_b; }
#endif
  }

#if FZERO_DIAG
  if (FZERO_PROBE_DRAW) {
    extern unsigned long g_interp_paired_frame_repairs;
    extern unsigned long g_interp_frame_left_for_ancestor;
    FZERO_LOG("INFO", "  repairs f%d total=%lu skipN_left=%lu",
              g_fzero_probe_frame, g_interp_paired_frame_repairs,
              g_interp_frame_left_for_ancestor);
  }
#endif
  if (FZERO_PROBE_DRAW)
    FZERO_LOG("INFO", "  pacing f%d timeline=%d periods=%.4f vPos=%d",
              g_fzero_probe_frame, (int)rtl_apu_frame_timeline_active(),
              RtlLastFramePeriods(), (int)g_snes->vPos);
  if (FZERO_PROBE_DRAW)
    FZERO_LOG("INFO", "FZero: draw end splits=%d vIrqEn=%d vTimer=%d "
              "($0041)=$%02X%02X", splits, (int)g_snes->vIrqEnabled,
              (int)g_snes->vTimer, (unsigned int)g_ram[0x42],
              (unsigned int)g_ram[0x41]);
  snes_set_hdma_beam_enabled(g_snes, beam_hdma_was_enabled);
}

const RtlGameInfo kFZeroGameInfo = {
  .title = "fzero",
  .initialize = NULL,
  .run_frame = &FZeroRunOneFrameOfGame,
  .draw_ppu_frame = &FZeroDrawPpuFrame,
  /* 2 KB battery SRAM (cart header: ROM+RAM+battery, SRAM size 2 KB). The
   * runner maps it from the header automatically; main_vita.c writes it back
   * with RtlWriteSram on exit. */
  .save_name_prefix = "save",
  .tier2_capture = 0,
};
