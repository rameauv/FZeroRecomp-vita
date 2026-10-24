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
    /* The pair route's merge: a fourth part of ProcessLine, not a
     * subdivision of the other three. */
    uint64_t t_flatten;
    uint64_t t_capsub_mask, t_capsub_move;     /* CaptureSprites, by caller   */
    uint32_t n_cap_called, n_cap_ran, n_cap_bytes_kb;
    uint32_t n_move_ran, n_move_bytes_kb, n_build_ran, n_build_bytes_kb;
    uint32_t n_build_ground, n_build_run;
    /* Lines on which the centre skip was asked for. Not n_build_ran:
     * PpuWidescreenCentreSkipSafe refuses a line whenever something couples
     * the centre to the margins. */
    uint32_t n_build_skip;
    /* Lines on which the NATIVE render was skipped. Not n_build_ran: the lever
     * refuses any line the wide pass cannot carry the centre for. */
    uint32_t n_native_skip;
    /* Lines that took the full scratch copy. With the frame scratch this is
     * about one a frame; without it, every built line. */
    uint32_t n_build_fresh;
    uint32_t n_reads;      /* this probe's own timer reads, so its cost shows */
    /* The host scanline loop, per line: native ppu_runLine, ProcessLine, HDMA,
     * IRQ split. BuildWideLine around its render: before and after. */
    uint64_t t_loop_native, t_loop_proc, t_loop_hdma, t_loop_irq;
    uint64_t t_build_pre, t_build_post, t_build_centre;
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
/* 1 = the wide render skips the authentic centre it was discarding anyway,
 * 0 = it renders all 398 columns. See fzero_layers.c. */
extern int g_fzero_wide_marginsonly;
/* 1 = write layers->flat directly and skip the world/hud split that a flat
 * presentation merges back together anyway, 0 = build the pair and merge it.
 * See the argument in fzero_layers.c. */
extern int g_fzero_wide_flatdirect;
/* 1 = do not run the native 256-wide render at all on
 * lines the wide pass can cover, and take the centre from the wide render
 * instead. This is NOT an equivalence -- see fzero_layers.c. */
extern int g_fzero_wide_nonative;
/* Take the centre from the wide render while the native render still runs,
 * so g_pixels stays a reference for how much the centre changed. */
extern int g_fzero_wide_widecentre;
/* 1 = keep the game's own sprite list in the wide render and place the
 * reconstructed cars in slots it is not using; 0 = the side pass wipes OAM and
 * owns it. Only the additive form is SIDE-LOCAL. See fzero_vehicles.c. */
extern int g_fzero_wide_keepoam;
/* 1 = the wide render bypasses the hardware 32-sprite / 34-tile per-line
 * limits, 0 = it obeys them. Bypassing is per-LINE, so it changes the centre
 * as well as the margins; that is only acceptable while the centre is thrown
 * away. */
extern int g_fzero_wide_nolimits;
extern int g_fzero_wide_framescratch;
typedef struct FZeroLayers {
    uint32_t world[FZERO_LAYER_HEIGHT][FZERO_LAYER_WIDTH];
    uint32_t hud[FZERO_LAYER_HEIGHT][FZERO_LAYER_WIDTH];
    uint32_t capture[FZERO_LAYER_HEIGHT][FZERO_LAYER_WIDTH];
    uint32_t wide_world[FZERO_LAYER_HEIGHT][FZERO_WIDE_WIDTH];
    uint32_t wide_hud[FZERO_LAYER_HEIGHT][FZERO_WIDE_WIDTH];
    uint32_t wide_capture[FZERO_LAYER_HEIGHT][FZERO_WIDE_WIDTH];
    /* The composited frame the Vita uploads: world and hud merged into one
     * opaque ARGB image. Produced either by merging the pair above or written
     * directly, per g_fzero_wide_flatdirect; the two must be identical. */
    uint32_t flat[FZERO_LAYER_HEIGHT][FZERO_WIDE_WIDTH];
    Ppu scratch;
    /* scratch.vram is copied only when the live VRAM changes; see
     * fzero_wide_vram.h. valid=false forces a full re-copy next line. */
    uint32_t scratch_vram_version;
    bool scratch_vram_valid;
    /* FRAME SCRATCH (BuildWideLine): the scratch is set up for this frame
     * as of frame_scratch_line, and matches OAM/CGRAM at frame_scratch_memver. */
    bool frame_scratch_valid;
    uint8_t frame_scratch_key, frame_scratch_oamaddl, frame_scratch_oamaddh;
    int frame_scratch_line;
    /* The scratch renders from its own VRAM this frame (a car was added), as
     * of live vramVersion frame_scratch_vramver. */
    bool frame_scratch_ownvram;
    uint32_t frame_scratch_vramver;
    uint16_t frame_scratch_memver;
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

/* Whether the host must still run ppu_runLine on the LIVE PPU for this line.
 * Always true unless the no-native experiment is on AND this line is one the
 * wide pass will render in full. Call it BEFORE the native render; it is the
 * only way the decision can be made in time. */
bool FZeroLayersWantsNativeLine(const FZeroLayers *layers, const Ppu *ppu,
                                int line, bool racing);

/* Called after the authentic scanline and before HDMA/IRQ changes its state.
 * Only the copied PPU is redrawn. Live PPU and guest memory remain untouched.
 * racing enables background and vehicle-edge expansion. hud_layout permits racing HUD
 * extraction; intro_panorama and results_layout protect lettering while filtering scenery.
 * Other layouts retain Original colours across the full width. */
void FZeroLayersProcessLine(FZeroLayers *layers, const Ppu *ppu, int line,
                            bool racing, bool hud_layout);
