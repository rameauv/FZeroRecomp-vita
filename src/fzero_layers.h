#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "snes/ppu.h"
#include "display_layout.h"
#include "fzero_vehicles.h"
#include "fzero_ground.h"

enum { FZERO_LAYER_WIDTH = FZERO_NATIVE_WIDTH, FZERO_LAYER_HEIGHT = FZERO_DISPLAY_HEIGHT };

/* See the FZERO_WIDEPROF block in fzero_layers.c. Declared here rather than
 * re-declared at the print site: two copies of this layout that drift apart
 * would read garbage silently. */
#if FZERO_WIDEPROF
struct FZeroWideProf {
    uint64_t t_mask, t_build, t_move;          /* the three ProcessLine parts */
    uint64_t t_cap_copy, t_cap_run, t_cap_loop;/* inside CaptureSprites       */
    uint64_t t_move_copy, t_move_run, t_move_loop, t_move_pre; /* MoveWideHud */
    uint64_t t_build_copy;
    uint64_t t_build_ground, t_build_run; /* BuildWideLine's TWO renderers */
    uint64_t t_capsub_mask, t_capsub_move;     /* CaptureSprites, by caller   */
    uint32_t n_cap_called, n_cap_ran, n_cap_bytes_kb;
    uint32_t n_move_ran, n_move_bytes_kb, n_build_ran, n_build_bytes_kb;
    uint32_t n_build_ground, n_build_run;
    uint32_t n_reads;      /* this probe's own timer reads, so its cost shows */
};
extern struct FZeroWideProf g_wideprof;
#endif
/* 1 = CaptureSprites and MoveWideHud share CopyPpuForWideLine's ~18 KB copy,
 * 0 = the original whole-struct 198,432 B memcpy at both. */
extern int g_fzero_wide_sharedcopy;
/* 1 = read sprite coverage from ppu->objSlotOf, 0 = re-render the scanline
 * once per OAM range (704 extra ppu_runLine calls a frame). */
extern int g_fzero_wide_fastcov;
/* 1 = spread the HUD to the screen edges, 0 = leave it centred and skip
 * MoveWideHud entirely. */
extern int g_fzero_wide_movehud;
typedef struct FZeroLayers {
    uint32_t world[FZERO_LAYER_HEIGHT][FZERO_LAYER_WIDTH];
    uint32_t hud[FZERO_LAYER_HEIGHT][FZERO_LAYER_WIDTH];
    uint32_t capture[FZERO_LAYER_HEIGHT][FZERO_LAYER_WIDTH];
    uint32_t wide_world[FZERO_LAYER_HEIGHT][FZERO_WIDE_WIDTH];
    uint32_t wide_hud[FZERO_LAYER_HEIGHT][FZERO_WIDE_WIDTH];
    uint32_t wide_capture[FZERO_LAYER_HEIGHT][FZERO_WIDE_WIDTH];
    Ppu scratch;
    /* scratch.vram is copied only when the live VRAM changes; see
     * fzero_wide_vram.h. valid=false forces a full re-copy next line. */
    uint32_t scratch_vram_version;
    bool scratch_vram_valid;
    FZeroVehicles vehicles;
    FZeroGround ground;
    /* Policy for the pending upload, captured with vehicles and ground. */
    bool wide_scene, hud_layout, native_oam, intro_panorama, results_layout;
    /* Explosion phases reuse rank slots before the full native upload begins. */
    bool crash_layout;
    /* Instrument placement is independent of selective scene effects. */
    bool move_hud;
    unsigned long wide_lines;
    unsigned long extracted_lines, protected_lines, hud_pixels;
} FZeroLayers;

/* Called after the authentic scanline and before HDMA/IRQ changes its state.
 * Only the copied PPU is redrawn. Live PPU and guest memory remain untouched.
 * racing enables background and vehicle-edge expansion. hud_layout permits racing HUD
 * extraction; intro_panorama and results_layout protect lettering while filtering scenery.
 * Other layouts retain Original colours across the full width. */
void FZeroLayersProcessLine(FZeroLayers *layers, const Ppu *ppu, int line,
                            bool racing, bool hud_layout);
