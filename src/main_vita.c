/*
 * main_vita.c - PS Vita main entry point for FZeroRecomp
 *
 * Runs the full game loop:
 *   - Continuous frame loop
 *   - Controller input handling
 *   - Audio output
 *   - Proper frame timing
 *   - Clean quit handling
 *
 * Based on patterns from sm64-vita's pc_main.c
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>

#include <SDL.h>
#include <SDL_video.h>
#include <SDL_render.h>

#include "platform.h"
#include "snes/snes.h"
#include "snes/ppu.h"
#include "snes/cpu.h"
#include "snes/apu.h"
#include "snes/dsp.h"
#include "common_cpu_infra.h"
#include "common_rtl.h"
#include "fzero_rtl.h"
#if FZERO_WIDESCREEN
#include "fzero_layers.h"
#endif
#include "rom_image_verify.h"
#include "input_vita.h"
#include "audio_sdl.h"
#include "apu_worker.h"
#include "fzero_spc_player.h"
#include "funcs.h"
#include <signal.h>

/* PS Vita heap size configuration (64 MB) */
unsigned int _newlib_heap_size_user = 64 * 1024 * 1024;

/* External declarations from common_rtl.c */
extern Ppu *g_ppu;
extern Snes *g_snes;
extern const RtlGameInfo *g_rtl_game_info;

/* Vita screen dimensions */
#define VITA_SCREEN_WIDTH  960
#define VITA_SCREEN_HEIGHT 544

/* SNES native resolution */
#define SNES_WIDTH  256
#define SNES_HEIGHT 224

/* ROM path for Vita */
#define ROM_PATH FZERO_ROM_PATH

/* Alternative ROM paths to try for development/testing */
static const char *rom_paths[] = {
    ROM_PATH,                                    /* ux0:/data/fzero_recomp/fzero.sfc */
    "ux0:/data/fzero_recomp/F-Zero (USA).sfc",  /* Alternative filename */
    "ux0:/data/fzero_recomp/F-Zero (USA).smc",  /* SMC header version */
    "fzero_usa_reference.sfc",                  /* Desktop default */
    "F-Zero (USA).sfc",                         /* Repo root */
    NULL
};

/* Pixel buffer for PPU output */
static uint8_t g_pixels[SNES_WIDTH * SNES_HEIGHT * 4];

#if FZERO_WIDESCREEN
/* Widescreen bring-up. The whole per-line pipeline already exists in this tree
 * and in fzero_rtl.c -- everything runs under `if (g_layers)` -- and the only
 * reason it has never executed on Vita is that nothing called FZeroSetLayers.
 * This wires it up so the cost can be MEASURED on device instead of projected.
 *
 * Expect it to be slow. PC puts BuildWideLine at ~1,949 us/frame even after
 * the scratch-copy reduction, and Vita's rasteriser runs roughly 10x slower, so the
 * honest expectation is well past the 16,639 us budget. The point of this
 * build is the number, not the frame rate.
 *
 * Presentation is deliberately the simple thing: the layered world/hud pair
 * that the desktop host keeps apart (so it can filter them differently) is
 * flattened into one ARGB image here. They are complementary by construction
 * in BuildWideLine -- a pixel is in exactly one of them, and hud carries an
 * opaque alpha -- so flattening loses nothing except the ability to treat them
 * differently, which this bring-up does not do. */
static FZeroLayers *g_layers_vita;
static uint32_t g_wide_pixels[FZERO_LAYER_HEIGHT * FZERO_WIDE_WIDTH];

static void FlattenWideFrame(void) {
    const uint32_t *w = (const uint32_t *)g_layers_vita->wide_world;
    const uint32_t *h = (const uint32_t *)g_layers_vita->wide_hud;
    for (size_t i = 0; i < FZERO_LAYER_HEIGHT * (size_t)FZERO_WIDE_WIDTH; ++i)
        g_wide_pixels[i] = (h[i] & 0xff000000u) ? (h[i] & 0x00ffffffu) : w[i];
}
#endif

/* Expected SHA-256 of F-Zero (USA) ROM */
static const uint8_t expected_sha256[32] = {
    0xbf, 0x16, 0xc3, 0xc8, 0x67, 0xc5, 0x8e, 0x2a,
    0xb0, 0x61, 0xc7, 0x0d, 0xe9, 0x29, 0x5b, 0x69,
    0x30, 0xd6, 0x3f, 0x29, 0xf8, 0x1c, 0xc9, 0x86,
    0xf5, 0xec, 0xae, 0x03, 0xe0, 0xad, 0x18, 0xd2,
};

/* Frame timing */
static uint64_t g_frame_start_time = 0;
/* True SNES NTSC frame period, matching the PC host's FZERO_GAME_PERIOD_NS
 * (16639264 ns = 60.0988 Hz). The previous 1000000/60 (16666 us) paced the
 * guest 0.16% slow, which alone is not the vPos drift but is a real error and
 * must be out of the way before pacing can be judged. */
static const uint64_t g_frame_time_us = 16639;

/* ========================================================================
 * GLOBAL STATE
 * ======================================================================== */

static bool g_running = true;
static SDL_Window *g_window = NULL;
static SDL_Renderer *g_renderer = NULL;
static SDL_Texture *g_texture = NULL;

/* --- presentation measurement -------------------------------------------
 * Always on: ~7 timer reads per frame, one log line per 60 frames.
 *
 * FrameTiming() caps the loop at ~60 Hz, so the capped FPS number says
 * nothing about headroom -- what costs what only shows up as per-phase busy
 * time. That breakdown is what chose the renderer below, and it is what any
 * future frame-time work will be judged against, so it stays in.
 */
static int    g_renderer_mode = -1;      /* 0 = software, 1 = accelerated */
static uint64_t g_acc_cpu, g_acc_draw, g_acc_aud, g_acc_upl, g_acc_clr, g_acc_cpy, g_acc_pre;
static uint64_t g_acc_flat;  /* widescreen world/hud flatten, its own phase */

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
/* Sprite range-evaluation early-out: measured at 853 us/frame, kept on. The
 * switch stays so it can be A/B'd again if the OAM walk is reworked. */
extern int g_ppu_fast_sprite_eval;
/* Compose-loop widescreen-HUD hoist: measured at 1568 us/frame, kept on. */
extern int g_ppu_fast_compose;
#ifndef FZERO_PPU_LUT_AB
/* 1 = alternate the compose-LUT arm every 300 frames, for a measurement
 * build. Default 0: the shipping build just runs the fast arm. */
#define FZERO_PPU_LUT_AB 0
#endif
/* Compose palette table (ppu->cgramLut): the per-pixel cgram load, three
 * brightnessMult lookups and two shifts become one load and one store.
 * Proven pixel-exact on PC over 3,999 frames / 2,520 distinct images, and
 * PC coverage counters put 81%% of compose pixels on the table path with the
 * original per-pixel loop never executing at all. Unmeasured on device --
 * FZERO_PPU_LUT=ab. */
extern int g_ppu_fast_cgram_lut;

#if FZERO_MEMBENCH
/* What does BuildWideLine's per-scanline `memcpy(copy, ppu, sizeof(*copy))`
 * actually cost on THIS machine?
 *
 * It cannot be measured in situ: widescreen is unwired on Vita (FZeroSetLayers
 * has no caller, g_layers is NULL), so BuildWideLine never runs. And it cannot
 * be predicted: the working set is ~400 KB of source+destination against a
 * ~512 KB L2, exactly the regime where a bandwidth calculation is worthless.
 * An estimate made that way was 3x too pessimistic on PC, because the same src
 * and dst are reused every line and largely stay cached.
 *
 * Two arms, each 224 copies -- one frame's worth -- and NEITHER needs a
 * subtraction, which is what broke the first version of this probe:
 *
 *   hot   one src, one dst, reused -- exactly the real access pattern, with
 *         the cache as friendly as it can possibly be. A LOWER bound.
 *   cold  src rotates through 8 buffers (1.6 MB, comfortably past L2) so every
 *         copy reads memory that cannot still be cached, while dst stays put.
 *         An UPPER bound, and it costs nothing extra to measure -- no eviction
 *         walk to time and subtract.
 *
 * The first version also reported hot=1 us for 44 MB, because nothing consumed
 * dst and the whole loop was optimised away. The checksum below exists to stop
 * that; it is logged so it cannot be dead-code eliminated either. */
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

/* Cost of one sceKernelGetProcessTimeWide(), measured at startup. The draw-loop
 * probe adds three reads per scanline (~675/frame), so its own overhead has to
 * be a reported number rather than an assumption. */
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

static void ResetPhaseAccumulators(void) {
    g_acc_cpu = g_acc_draw = g_acc_aud = g_acc_upl = g_acc_clr = g_acc_cpy = g_acc_pre = 0;
    g_acc_flat = 0;
    g_fzprof_ppu = g_fzprof_hdma = g_fzprof_split = 0; g_fzprof_lines = 0;
    g_ppuprof_eval = g_ppuprof_bg = g_ppuprof_compose = 0;
    g_apuprof_us = 0; g_apuprof_calls = 0;
    g_apuprof_port_us = g_apuprof_frame_us = 0;
    g_apuprof_port_calls = g_apuprof_frame_calls = 0;
    g_fzprof_nmi = g_fzprof_sched = 0;
    g_acc_sleep = 0;
    g_acc_over = g_acc_frames = 0;
}

static void LogRendererInfo(const char *tag) {
    SDL_RendererInfo info;
    if (SDL_GetRendererInfo(g_renderer, &info) != 0) {
        FZERO_LOG("WARN", "renderer[%s]: SDL_GetRendererInfo failed: %s", tag, SDL_GetError());
        return;
    }
    FZERO_LOG("INFO", "renderer[%s]: name=%s flags=0x%08X%s%s%s%s max=%dx%d nformats=%d",
              tag, info.name ? info.name : "?", (unsigned int)info.flags,
              (info.flags & SDL_RENDERER_SOFTWARE)      ? " SOFTWARE"      : "",
              (info.flags & SDL_RENDERER_ACCELERATED)   ? " ACCELERATED"   : "",
              (info.flags & SDL_RENDERER_PRESENTVSYNC)  ? " PRESENTVSYNC"  : "",
              (info.flags & SDL_RENDERER_TARGETTEXTURE) ? " TARGETTEXTURE" : "",
              info.max_texture_width, info.max_texture_height,
              (int)info.num_texture_formats);
    for (Uint32 i = 0; i < info.num_texture_formats && i < 8u; ++i)
        FZERO_LOG("INFO", "renderer[%s]: texture_format[%u]=%s", tag, (unsigned int)i,
                  SDL_GetPixelFormatName(info.texture_formats[i]));
}

/*
 * Create (or recreate) the renderer and the SNES frame texture.
 * mode 0 = SDL_RENDERER_SOFTWARE, 1 = SDL_RENDERER_ACCELERATED (the
 * default: measured at 1.07 ms/frame of presentation against the software
 * path's 105 ms).
 *
 * The original code fell back to flags 0 on failure, which degrades
 * SILENTLY -- that is exactly how you measure software and call it
 * hardware. Here every fallback is logged and the mode actually obtained is
 * confirmed with SDL_GetRendererInfo().
 */
static bool SelectRenderer(int mode) {
    const char *tag = mode ? "ACCEL" : "SOFT";
    Uint32 flags = mode ? SDL_RENDERER_ACCELERATED : SDL_RENDERER_SOFTWARE;

    if (g_texture)  { SDL_DestroyTexture(g_texture);   g_texture = NULL; }
    if (g_renderer) { SDL_DestroyRenderer(g_renderer); g_renderer = NULL; }

    g_renderer = SDL_CreateRenderer(g_window, -1, flags);
    if (!g_renderer) {
        FZERO_LOG("WARN", "renderer[%s]: SDL_CreateRenderer failed: %s", tag, SDL_GetError());
        if (mode != 0) {
            mode = 0; tag = "SOFT";
            g_renderer = SDL_CreateRenderer(g_window, -1, SDL_RENDERER_SOFTWARE);
        }
        if (!g_renderer) {
            FZERO_LOG("ERROR", "renderer: no renderer available at all: %s", SDL_GetError());
            return false;
        }
    }

#if FZERO_WIDESCREEN
    g_texture = SDL_CreateTexture(g_renderer, SDL_PIXELFORMAT_ARGB8888,
                                  SDL_TEXTUREACCESS_STREAMING,
                                  FZERO_WIDE_WIDTH, FZERO_LAYER_HEIGHT);
#else
    g_texture = SDL_CreateTexture(g_renderer, SDL_PIXELFORMAT_ARGB8888,
                                  SDL_TEXTUREACCESS_STREAMING, SNES_WIDTH, SNES_HEIGHT);
#endif
    if (!g_texture) {
        FZERO_LOG("ERROR", "renderer[%s]: SDL_CreateTexture failed: %s", tag, SDL_GetError());
        return false;
    }

    g_renderer_mode = mode;
    LogRendererInfo(tag);
    ResetPhaseAccumulators();
    return true;
}

static Snes *g_snes_instance = NULL;
static uint8_t *g_rom_buffer = NULL;
static long g_rom_size = 0;

/* ========================================================================
 * ROM HANDLING
 * ======================================================================== */

/*
 * Verify ROM against expected SHA-256
 */
static bool VerifyRom(const char *path) {
    const char *driver = getenv("SDL_VIDEODRIVER");
    if (getenv("SNESRECOMP_MAX_FRAMES") || (driver && !strcmp(driver, "dummy"))) {
        return snesrecomp_rom_match_sha256(path, (const uint8 (*)[32])&expected_sha256, 1) == 0;
    }
    return snesrecomp_rom_verify_sha256(path, expected_sha256) != 0;
}

/*
 * Read entire file into buffer
 * Uses Vita platform file reading
 */
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

/* ========================================================================
 * INITIALIZATION
 * ======================================================================== */

/*
 * Initialize all subsystems
 */
static bool InitAll(void) {
    /* Initialize platform */
    platform_init();
    platform_ensure_directories();

    /* Initialize SDL with VIDEO first */
    FZERO_LOG("INFO", "Initializing SDL2");
    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        FZERO_LOG("ERROR", "SDL_Init(VIDEO) failed: %s", SDL_GetError());
        return false;
    }
    
    /* Initialize other subsystems that might be needed for input
     * This ensures keyboard/mouse function pointers are initialized
     * (required for keybinds.c which calls SDL_GetScancodeFromName) */
    SDL_InitSubSystem(SDL_INIT_JOYSTICK);
    SDL_InitSubSystem(SDL_INIT_GAMECONTROLLER);

    /* Create window at Vita native resolution */
    FZERO_LOG("INFO", "Creating window at %dx%d", VITA_SCREEN_WIDTH, VITA_SCREEN_HEIGHT);
    g_window = SDL_CreateWindow("F-Zero Recomp (Vita)",
                                          SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                                          VITA_SCREEN_WIDTH, VITA_SCREEN_HEIGHT,
                                          SDL_WINDOW_FULLSCREEN);
    if (!g_window) {
        FZERO_LOG("ERROR", "SDL_CreateWindow failed: %s", SDL_GetError());
        return false;
    }

    CalibrateTimer();
#if FZERO_WIDESCREEN
    /* ~2 MB, so it is heap-allocated rather than static: the text segment has
     * ~1 KB of slack and .bss is not the constraint, but a 2 MB static would
     * still be the wrong shape for something this build-optional. */
    g_layers_vita = calloc(1, sizeof(*g_layers_vita));
    if (!g_layers_vita) {
        FZERO_LOG("ERROR", "widescreen: could not allocate %u bytes of layers",
                  (unsigned int)sizeof(*g_layers_vita));
        return false;
    }
    FZeroSetLayers(g_layers_vita);
    FZERO_LOG("INFO", "widescreen: ON, %ux%u, layers=%u KB (MEASUREMENT build)",
              (unsigned int)FZERO_WIDE_WIDTH, (unsigned int)FZERO_LAYER_HEIGHT,
              (unsigned int)(sizeof(*g_layers_vita) / 1024));
#endif
#if FZERO_MEMBENCH
    MemcpyBench();
#endif

    /* Enumerate what render drivers this SDL build actually has, before
     * asking for one -- so a missing GXM driver is visible as such rather
     * than as a mysteriously slow "hardware" path. */
    int ndrivers = SDL_GetNumRenderDrivers();
    FZERO_LOG("INFO", "render drivers: %d", ndrivers);
    for (int i = 0; i < ndrivers; ++i) {
        SDL_RendererInfo di;
        if (SDL_GetRenderDriverInfo(i, &di) == 0)
            FZERO_LOG("INFO", "render driver[%d]: name=%s flags=0x%08X%s%s%s",
                      i, di.name ? di.name : "?", (unsigned int)di.flags,
                      (di.flags & SDL_RENDERER_SOFTWARE)     ? " SOFTWARE"     : "",
                      (di.flags & SDL_RENDERER_ACCELERATED)  ? " ACCELERATED"  : "",
                      (di.flags & SDL_RENDERER_PRESENTVSYNC) ? " PRESENTVSYNC" : "");
        else
            FZERO_LOG("WARN", "render driver[%d]: query failed: %s", i, SDL_GetError());
    }

    FZERO_LOG("INFO", "Creating renderer and SNES frame texture");
    if (!SelectRenderer(1)) return false;

    /* Initialize input */
    FZERO_LOG("INFO", "Initializing controller input");
    input_init();

    /* Initialize audio */
    FZERO_LOG("INFO", "Initializing audio");
    if (!audio_init()) {
        FZERO_LOG("WARN", "Audio initialization failed, continuing without audio");
    }

    return true;
}

/*
 * Load ROM and initialize SNES
 */
static bool LoadAndInitSNES(void) {
    /* Try to find ROM at multiple locations */
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

    /* Load ROM */
    FZERO_LOG("INFO", "Loading ROM from: %s", rom_path);
    g_rom_size = 0;
    g_rom_buffer = ReadWholeFile(rom_path, &g_rom_size);
    if (!g_rom_buffer) {
        FZERO_LOG("ERROR", "Failed to load ROM");
        return false;
    }
    FZERO_LOG("INFO", "ROM loaded: %ld bytes", g_rom_size);

    /* Initialize SNES emulation */
    FZERO_LOG("INFO", "Registering game");
    RtlRegisterGame(&kFZeroGameInfo);

    /* Initialize SPC player */
    FZERO_LOG("INFO", "Initializing SPC player");
    g_spc_player = FZeroSpcPlayer_Create();
    if (g_spc_player && g_spc_player->initialize) {
        g_spc_player->initialize(g_spc_player);
    }

    FZERO_LOG("INFO", "Initializing SNES");
    g_snes_instance = SnesInit(g_rom_buffer, (int)g_rom_size);
    if (!g_snes_instance) {
        FZERO_LOG("ERROR", "SnesInit failed");
        free(g_rom_buffer);
        g_rom_buffer = NULL;
        return false;
    }
    FZERO_LOG("INFO", "SNES initialized successfully");
#if FZERO_DIAG
    FZERO_LOG("INFO", "  Sinit after SnesInit S=%04X e=%d",
              (unsigned int)g_cpu.S, (int)g_cpu.emulation);
#endif

    /* No bounce exclusions needed. The two targets that appeared to "leak" a
     * return frame ($00:F806 and $03:8062) were in fact yielding correctly --
     * their yield sentinel was being truncated to RECOMP_RETURN_NORMAL by the
     * ARM EABI's short-enum packing of RecompReturn. Fixed at the type in
     * snesrecomp/runner/src/cpu_state.h. */

    /* Deliberately NOT calling RtlEnableExtendedFrameTiming() here. It is only
     * ever called from snesrecomp's desktop/host_main.c, which NEITHER build
     * links -- the PC target has its own main.c too. Enabling it on Vita was
     * tested and had no effect on the wedge or the vPos drift, and it made the
     * Vita host diverge from the working PC reference, so it stays off. */

    /* Begin PPU drawing */
    FZERO_LOG("INFO", "Starting PPU drawing");
    PpuBeginDrawing(g_ppu, g_pixels, (size_t)SNES_WIDTH * 4, kPpuRenderFlags_NewRenderer);

    return true;
}

/* ========================================================================
 * GAME LOOP
 * ======================================================================== */

/*
 * Handle one frame of game logic
 */
static void HandleFrame(void) {
    /* Poll SDL events */
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
        if (event.type == SDL_QUIT) {
            g_running = false;
            return;
        }
    }

    /* Sample the pad once; everything below reads that one sample. */
    input_poll();

    /* Check for quit (START + SELECT held) before building the frame's input */
    if (input_should_quit()) {
        FZERO_LOG("INFO", "Quit combo held, exiting");
        g_running = false;
        return;
    }

    /* Read input */
    uint32_t inputs = input_read();

    static uint32_t s_frame_count = 0;
    static uint64_t s_last_fps_time = 0;
    s_frame_count++;

    if (s_frame_count <= 10) {
        FZERO_LOG("INFO", ">>> Frame %u starting...", (unsigned int)s_frame_count);
    }

    uint64_t t_phase = sceKernelGetProcessTimeWide();
    const uint64_t t_frame_begin = t_phase;

    /* Run game frame */
#if FZERO_DIAG
    {
      extern int snes_frame_counter;
      if (snes_frame_counter <= 6)
        FZERO_LOG("INFO", "  Sbefore f%d S=%04X e=%d", snes_frame_counter,
                  (unsigned int)g_cpu.S, (int)g_cpu.emulation);
    }
#endif
    RtlRunFrame(inputs);
    { uint64_t n = sceKernelGetProcessTimeWide(); g_acc_cpu += n - t_phase; t_phase = n; }

#if FZERO_DIAG
    /* Per-frame WRAM fingerprint, for bisecting guest-state divergence against
     * the PC reference. Same point in the frame as PC's probe (immediately
     * after the CPU half, before draw_ppu_frame) and the same crc32 over the
     * same 128K, so the two columns are directly comparable. */
    {
      extern uint32_t crc32_compute(const uint8_t *data, size_t len);
      extern int snes_frame_counter;
      if (snes_frame_counter <= 120)
        FZERO_LOG("INFO", "  ramcrc f%d=%08X", snes_frame_counter,
                  (unsigned int)crc32_compute(g_ram, 0x20000));
      /* WRAM diverges from PC at frame 2. Localise it: per-4K-page CRCs on the
       * first few frames say WHICH region differs, so only that region needs a
       * byte dump. 32 lines per frame, three frames. */
      if (snes_frame_counter >= 1 && snes_frame_counter <= 3)
        for (int pg = 0; pg < 32; ++pg)
          FZERO_LOG("INFO", "  rampg f%d p%02d=%08X", snes_frame_counter, pg,
                    (unsigned int)crc32_compute(g_ram + pg * 0x1000, 0x1000));
      /* Page 0 localisation done: Vita's stack sits 2 bytes deeper than PC's.
       * Now pin WHEN those 2 bytes appear. PC idles at S=01FF before every
       * frame's NMI push; Vita idles at 01FD. */
      if (snes_frame_counter <= 6)
        FZERO_LOG("INFO", "  Safter f%d S=%04X e=%d", snes_frame_counter,
                  (unsigned int)g_cpu.S, (int)g_cpu.emulation);
    }
#endif

    if (s_frame_count <= 10) {
        FZERO_LOG("INFO", ">>> Frame %u: RtlRunFrame done", (unsigned int)s_frame_count);
    }

    /* Draw PPU frame */
    if (g_rtl_game_info && g_rtl_game_info->draw_ppu_frame) {
        if (s_frame_count <= 10) {
            FZERO_LOG("INFO", ">>> Frame %u: draw_ppu_frame...", (unsigned int)s_frame_count);
        }
        g_rtl_game_info->draw_ppu_frame();
        if (s_frame_count <= 10) {
            FZERO_LOG("INFO", ">>> Frame %u: draw_ppu_frame done", (unsigned int)s_frame_count);
            /* Diagnose PPU state: forced blank, brightness, layer enables */
            FZERO_LOG("INFO", ">>> Frame %u: PPU inidisp=0x%02X (forcedBlank=%d, brightness=%d)",
                      (unsigned int)s_frame_count,
                      (unsigned int)g_ppu->inidisp,
                      (int)((g_ppu->inidisp >> 7) & 1),
                      (int)(g_ppu->inidisp & 0xF));
            FZERO_LOG("INFO", ">>> Frame %u: PPU screenEnabled[0]=0x%02X screenEnabled[1]=0x%02X",
                      (unsigned int)s_frame_count,
                      (unsigned int)g_ppu->screenEnabled[0],
                      (unsigned int)g_ppu->screenEnabled[1]);
            /* Sample a few pixels to see if anything non-black was drawn */
            uint32_t *px = (uint32_t *)g_pixels;
            uint32_t nonzero = 0;
            for (int _pi = 0; _pi < SNES_WIDTH * SNES_HEIGHT; ++_pi)
                nonzero |= px[_pi];
            FZERO_LOG("INFO", ">>> Frame %u: pixel sample nonzero=0x%08X",
                      (unsigned int)s_frame_count, (unsigned int)nonzero);
        }
    }

    { uint64_t n = sceKernelGetProcessTimeWide(); g_acc_draw += n - t_phase; t_phase = n; }

    /* Submit audio */
    audio_render_frame(g_snes_instance);
    { uint64_t n = sceKernelGetProcessTimeWide(); g_acc_aud += n - t_phase; t_phase = n; }

    /* Upload pixels to texture */
#if FZERO_WIDESCREEN
    /* Timed on its own. Folding it into upl= made the upload look 5.9x more
     * expensive than the 1.55x pixel increase can explain, which is a probe
     * artifact, not a finding. */
    FlattenWideFrame();
    { uint64_t n = sceKernelGetProcessTimeWide(); g_acc_flat += n - t_phase; t_phase = n; }
    SDL_Rect update_rect = {0, 0, FZERO_WIDE_WIDTH, FZERO_LAYER_HEIGHT};
    SDL_UpdateTexture(g_texture, &update_rect, g_wide_pixels, FZERO_WIDE_WIDTH * 4);
#else
    SDL_Rect update_rect = {0, 0, SNES_WIDTH, SNES_HEIGHT};
    SDL_UpdateTexture(g_texture, &update_rect, g_pixels, SNES_WIDTH * 4);
#endif
    { uint64_t n = sceKernelGetProcessTimeWide(); g_acc_upl += n - t_phase; t_phase = n; }

    /* Clear screen */
    SDL_SetRenderDrawColor(g_renderer, 0, 0, 0, 255);
    SDL_RenderClear(g_renderer);
    { uint64_t n = sceKernelGetProcessTimeWide(); g_acc_clr += n - t_phase; t_phase = n; }

    /* Render texture */
    SDL_Rect dest_rect;
    
    /* Calculate aspect-ratio-correct scaling */
#if FZERO_WIDESCREEN
    float snes_aspect = (float)FZERO_WIDE_WIDTH / (float)FZERO_LAYER_HEIGHT;
#else
    float snes_aspect = (float)SNES_WIDTH / (float)SNES_HEIGHT;
#endif
    float screen_aspect = (float)VITA_SCREEN_WIDTH / (float)VITA_SCREEN_HEIGHT;
    
    if (screen_aspect > snes_aspect) {
        /* Screen is wider than SNES - pillbox horizontally */
        int height = VITA_SCREEN_HEIGHT;
        int width = (int)(height * snes_aspect);
        dest_rect.x = (VITA_SCREEN_WIDTH - width) / 2;
        dest_rect.y = 0;
        dest_rect.w = width;
        dest_rect.h = height;
    } else {
        /* Screen is taller than SNES - pillbox vertically */
        int width = VITA_SCREEN_WIDTH;
        int height = (int)(width / snes_aspect);
        dest_rect.x = 0;
        dest_rect.y = (VITA_SCREEN_HEIGHT - height) / 2;
        dest_rect.w = width;
        dest_rect.h = height;
    }

    SDL_RenderCopy(g_renderer, g_texture, NULL, &dest_rect);
    { uint64_t n = sceKernelGetProcessTimeWide(); g_acc_cpy += n - t_phase; t_phase = n; }

    /* Present to screen */
    SDL_RenderPresent(g_renderer);
    { uint64_t n = sceKernelGetProcessTimeWide(); g_acc_pre += n - t_phase; t_phase = n; }
    g_acc_frames++;
    (void)t_frame_begin;

    /* A/B the worker inside ONE run, alternating every 300 frames, so both
     * arms see nearly the same scenes and consecutive blocks can be compared
     * directly. Pooling arms across a run charges scene drift to the change:
     * bg+mode7 swings 3,830-5,672 us WITHIN an arm.
     *
     * OFF by default: the arm is already decided. Measured over 139 clean
     * blocks, worker ON is 5,835 us/frame cheaper and takes 47.9 -> 59.5 FPS
     * with late=0 for the whole run. The lever stays so the next change to
     * the APU path can be measured the same way -- FZERO_APU_WORKER=ab. */
    {
        static int s_ab = -1;
        if (s_ab < 0) {
            const char *e = getenv("FZERO_APU_WORKER");
            s_ab = (e && e[0] == 'a') ? 1 : 0;
        }
        if (s_ab && s_frame_count % 300 == 0) {
            bool on = !apu_worker_enabled();
            apu_worker_set(on);
            FZERO_LOG("INFO", "A/B: APU worker -> %s at f%u",
                      on ? "ON" : "off", (unsigned int)s_frame_count);
        }
    }

    /* Same lever for the compose palette table, and for the same reason: the
     * arms must alternate INSIDE one run so consecutive 300-frame blocks can
     * be compared. bg+mode7 does not move with this change, so it doubles as
     * the drift check -- a saving that shows up anywhere but compose and the
     * total is a measurement artifact, not a win. FZERO_PPU_LUT=ab. */
    {
        static int s_ab = -1;
        if (s_ab < 0) {
            /* A Vita launch has no environment -- getenv is always NULL
             * here -- so the arm has to come from the build. The env check
             * stays for the desktop build of this file. */
            const char *e = getenv("FZERO_PPU_LUT");
            s_ab = (e && e[0] == 'a') ? 1 : FZERO_PPU_LUT_AB;
        }
        if (s_ab && s_frame_count % 300 == 0) {
            g_ppu_fast_cgram_lut = !g_ppu_fast_cgram_lut;
            FZERO_LOG("INFO", "A/B: compose LUT -> %s at f%u",
                      g_ppu_fast_cgram_lut ? "LUT" : "orig",
                      (unsigned int)s_frame_count);
        }
    }

#if FZERO_WIDE_SHAREDCOPY_AB
    /* Same lever again, for the scratch copy CaptureSprites and MoveWideHud do.
     * PC says removing that copy is a net LOSS there -- the memcpy prefetches
     * for the ppu_runLine right after it, and PC has the cache to profit. The
     * startup membench says this machine has neither the bandwidth
     * nor the cache, so the two should disagree. This decides it on device. */
    {

        if (s_frame_count % 300 == 0) {
            g_fzero_wide_sharedcopy = !g_fzero_wide_sharedcopy;
            FZERO_LOG("INFO", "A/B: wide scratch copy -> %s at f%u",
                      g_fzero_wide_sharedcopy ? "SHARED" : "full",
                      (unsigned int)s_frame_count);
        }
    }
#endif

#if FZERO_WIDE_MOVEHUD_AB
    /* The centred-HUD concession. Unlike every other lever here this one is
     * NOT an equivalence: it deliberately changes the picture, leaving the HUD
     * in the original 256 band instead of spreading it to the screen edges.
     * WIDECRC differing between these arms is the expected result, not a bug. */
    {
        if (s_frame_count % 300 == 0) {
            g_fzero_wide_movehud = !g_fzero_wide_movehud;
            FZERO_LOG("INFO", "A/B: wide HUD -> %s at f%u",
                      g_fzero_wide_movehud ? "SPREAD" : "CENTRED",
                      (unsigned int)s_frame_count);
        }
    }
#endif

#if FZERO_WIDE_FASTCOV_AB
    /* Same lever, for the 704 coverage renders. Unlike the scratch copy this
     * one removes work outright rather than trading memory traffic for
     * locality, so PC and Vita should agree in sign -- PC measured
     * ProcessLine 9,142 -> 2,780 us/frame. If they disagree here, something
     * other than the render count dominates. */
    {
        if (s_frame_count % 300 == 0) {
            g_fzero_wide_fastcov = !g_fzero_wide_fastcov;
            FZERO_LOG("INFO", "A/B: wide coverage -> %s at f%u",
                      g_fzero_wide_fastcov ? "SLOTS" : "render",
                      (unsigned int)s_frame_count);
        }
    }
#endif

    /* Frame telemetry / heartbeat logging */
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
                  (unsigned int)s_frame_count, g_renderer_mode ? "ACCEL" : "SOFT", fps,
                  (unsigned int)((g_acc_cpu + g_acc_draw) / n),
                  (unsigned int)(g_acc_cpu / n), (unsigned int)(g_acc_draw / n),
                  (unsigned int)(g_acc_aud / n),
                  (unsigned int)((g_acc_upl + g_acc_clr + g_acc_cpy + g_acc_pre) / n),
                  (unsigned int)(g_acc_upl / n), (unsigned int)(g_acc_clr / n),
                  (unsigned int)(g_acc_cpy / n), (unsigned int)(g_acc_pre / n),
                  (unsigned int)(g_acc_flat / n),
                  (unsigned int)(busy / n), (unsigned int)(g_acc_sleep / n),
                  (unsigned int)g_acc_over, (unsigned int)n);
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
        /* APU worker health, in one line so one device round-trip settles it:
         *   late=  frames whose previous ceiling was still unreached at
         *          publish time -- the worker is behind real time. 0 is the
         *          goal; a steady nonzero count means the second core cannot
         *          keep up and the projection is wrong.
         *   occ=   DSP ring occupancy in natives, sampled once per block --
         *          evidence, not a bound. The servo steers toward 2136 (4
         *          frames of 534) and holds it once the frame pacer is
         *          honest; what actually went wrong is counted by the
         *          apu-audio line below, which is the one to read.
         * Both are cumulative counters printed per 60-frame block, so compare
         * CONSECUTIVE blocks -- pooling arms charges scene drift to the
         * change. */
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
        /* The draw half, attributed. probe= is this probe's own cost, so the
         * reader can tell measurement from measured. */
        FZERO_LOG("INFO",
                  "  draw-split: ppu_runLine=%u hdma=%u irq_split=%u | lines=%u/frame"
                  " probe=%u (us/frame)",
                  (unsigned int)(g_fzprof_ppu / n), (unsigned int)(g_fzprof_hdma / n),
                  (unsigned int)(g_fzprof_split / n), (unsigned int)(g_fzprof_lines / n),
                  (unsigned int)((g_timer_ns * 8u * (g_fzprof_lines / n)) / 1000u));
        /* All three measured directly, never derived by subtraction -- the
         * upstream profiler's compose-by-subtraction is exactly how a phase
         * with no probes gets silently absorbed into another. sum= is the
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
        /* The widescreen layers pass, attributed. `hdma=` above CONTAINS this
         * whole pass when widescreen is on; these lines are
         * what is inside it. bytes= is the number that ports -- the membench
         * measured this machine at ~1 GB/s with caching worth only 25-35%. */
        {
            uint64_t cap = g_wideprof.t_cap_copy + g_wideprof.t_cap_run
                         + g_wideprof.t_cap_loop;
            FZERO_LOG("INFO",
                "  wide[copy=%s cov=%s hud=%s]: ProcessLine=%u = mask %u"
                " + build %u + move %u | probe=%u (us/frame)",
                g_fzero_wide_sharedcopy ? "SHARED" : "full",
                g_fzero_wide_fastcov ? "SLOTS" : "render",
                g_fzero_wide_movehud ? "SPREAD" : "CENTRED",
                (unsigned int)((g_wideprof.t_mask + g_wideprof.t_build
                                + g_wideprof.t_move) / n),
                (unsigned int)(g_wideprof.t_mask / n),
                (unsigned int)(g_wideprof.t_build / n),
                (unsigned int)(g_wideprof.t_move / n),
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
                " (us/frame)",
                (unsigned int)(g_wideprof.n_build_ground / n),
                (unsigned int)(g_wideprof.t_build_ground / n),
                (unsigned int)(g_wideprof.n_build_run / n),
                (unsigned int)(g_wideprof.t_build_run / n));
            memset(&g_wideprof, 0, sizeof(g_wideprof));
        }
#endif
        ResetPhaseAccumulators();
        s_last_fps_time = now;
    }
}

/*
 * Frame timing to maintain consistent frame rate
 */
/* Drift-free frame pacing.
 *
 * The deadline advances by exactly one frame period; it is NOT resampled from
 * the clock after the sleep. Resampling was the bug: sceKernelDelayThreadCB
 * returns late by the scheduler's granularity, and taking the wake-up time as
 * the next frame's start folded that overshoot into the period permanently.
 * Measured: a 16,639 us target produced 16,835 us frames -- 59.4 Hz instead of
 * 60.0988, a 1.2%% shortfall that never recovered.
 *
 * That shortfall is an AUDIO bug more than a video one. The guest produces a
 * fixed 534 natives per emulated frame, so running the frame loop 1.2%% slow
 * under-produces audio by 1.2%%, and rtl_render_native's servo can only bend
 * consumption by +/-0.5%% (RTL_AUDIO_SERVO_MAX -- wider would be audible as
 * pitch drift). The deficit therefore cannot be absorbed: the DSP ring drains
 * to empty, starves, fades to silence, refills, and repeats. Measured at 0.5%%
 * of all output frames faded, in ~2 episodes per second, stable over 35,000
 * frames. The fix belongs here, where the missing time is, not in the servo.
 *
 * Resync rather than chase if we fall more than two periods behind: after a
 * real hitch, sleeping out an accumulated debt would run the game fast to
 * "catch up", which is worse than dropping the debt. */
static void FrameTiming(void) {
    g_frame_start_time += g_frame_time_us;
    uint64_t now = sceKernelGetProcessTimeWide();
    if (now < g_frame_start_time) {
        uint64_t sleep_us = g_frame_start_time - now;
        g_acc_sleep += sleep_us;
        sceKernelDelayThreadCB((SceUInt)sleep_us);
    } else {
        g_acc_over++;   /* frame missed its budget: no headroom left here */
        if (now - g_frame_start_time > 2 * g_frame_time_us)
            g_frame_start_time = now;
    }
}

/* ========================================================================
 * CLEANUP
 * ======================================================================== */

/*
 * Clean up all resources
 */
static void Cleanup(void) {
    FZERO_LOG("INFO", "Cleaning up...");

    /* Join the APU worker before the audio thread, and both before anything
     * below frees emulator state: the worker cycles the SPC and the callback
     * reads the DSP ring, and g_spc_player owns the RAM under both. */
    apu_worker_stop();

    /* Stop audio FIRST. SDL_CloseAudioDevice joins the audio thread, so after
     * this nothing can be inside the callback. Everything below frees state
     * the callback renders from -- g_spc_player owns the DSP's RAM -- so
     * tearing those down first is a use-after-free on the audio thread. */
    audio_shutdown();

    /* Write SRAM */
    if (g_snes_instance) {
        RtlWriteSram();
    }

    /* Free ROM buffer */
    if (g_rom_buffer) {
        free(g_rom_buffer);
        g_rom_buffer = NULL;
    }

    /* Free SPC player */
    if (g_spc_player) {
        free(g_spc_player);
        g_spc_player = NULL;
    }

    /* Clean up input */
    input_shutdown();

    /* Clean up SDL */
    if (g_texture) {
        SDL_DestroyTexture(g_texture);
        g_texture = NULL;
    }
    if (g_renderer) {
        SDL_DestroyRenderer(g_renderer);
        g_renderer = NULL;
    }
    if (g_window) {
        SDL_DestroyWindow(g_window);
        g_window = NULL;
    }
    SDL_Quit();

    /* Shutdown platform */
    platform_shutdown();

    FZERO_LOG("INFO", "Cleanup complete");
}

/* ========================================================================
 * MAIN ENTRY POINT
 * ======================================================================== */

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
    freopen("ux0:/data/fzero_recomp/stdout.txt", "w", stdout);

    FZERO_LOG("INFO", "F-Zero Recomp - PS Vita starting (Heap: %u MB)", _newlib_heap_size_user / (1024 * 1024));

    /* Initialize all subsystems */
    if (!InitAll()) {
        FZERO_LOG("ERROR", "Initialization failed!");
        Cleanup();
        return 1;
    }

    /* Load ROM and initialize SNES */
    if (!LoadAndInitSNES()) {
        FZERO_LOG("ERROR", "ROM loading or SNES initialization failed!");
        Cleanup();
        return 1;
    }

    /* Safe to start the audio thread now: it renders from g_snes. The APU
     * worker dereferences the same state, so it starts here too and not a
     * line earlier. */
    apu_worker_start();
    audio_start();

    /* Main game loop */
    FZERO_LOG("INFO", "Entering main game loop");
    
    g_frame_start_time = sceKernelGetProcessTimeWide();
    
    while (g_running) {
        HandleFrame();
        FrameTiming();
    }

    /* Clean up and exit */
    Cleanup();
    FZERO_LOG("INFO", "F-Zero Recomp exited cleanly");

    return 0;
}
