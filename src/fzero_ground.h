#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "snes/ppu.h"

/* 1 = hand the 142 corrected tile IDs to the rasteriser as a side array, one
 * slot per margin column; 0 = patch those tiles into the live tilemap,
 * render, and unwind the writes through the journal in fzero_wide_vram.h.
 *
 * In the patching scheme, 16,384 tilemap cells are shared between 142
 * columns, so two columns on one line can need different tiles from one cell,
 * and the line must be rendered again (BeginPass/stamps/serial and the
 * split-pass loop). One slot per column cannot collide. Defined in
 * fzero_layers.c. */
extern int g_fzero_wide_sidetiles;

typedef struct FZeroGround {
    /* A frame-owned copy keeps course edits paired with the pending upload. */
    uint8_t map[0x10000];
    uint16_t world, scroll_x, scroll_y;
    bool ready;
    /* GPU course lookup. The map is kept by 1 KB pages:
     * gpu_dirty[set] collects the pages changed since that GPU set last took
     * them. course_valid says every lookup the world grid can reach is valid
     * (entry <= 0xfffe, panel <= 0xfffc), so the GPU never meets the case in
     * which the CPU drops a line to the fallback. It is checked again only
     * when a page it depends on (valid_deps) changes, or the world moves. */
    bool course_init, course_valid;
    uint16_t valid_world;
    uint64_t valid_deps, gpu_dirty[2];
    uint32_t stamps[0x4000], serial;
    unsigned long lines, corrected_pixels, split_passes;
} FZeroGround;

#if FZERO_WIDEPROF
/* The ground pass split into its three parts. See the probe in
 * fzero_ground.c. `patch` covers the journalled tilemap writes AND the margin
 * writeback that follows them, and is zero in the side-array arm because that
 * arm has neither. Microseconds, accumulated; the host divides and clears. */
extern uint64_t g_fzero_ground_prep, g_fzero_ground_render, g_fzero_ground_patch;
extern uint32_t g_fzero_ground_reads;
/* Margin tile lookups, and the dependent reads the memo did not skip. */
extern uint32_t g_fzero_ground_lookups, g_fzero_ground_room_reads,
                g_fzero_ground_row_reads, g_fzero_ground_panel_reads;
#endif

void FZeroGroundPrepare(FZeroGround *ground, const uint8_t *ram);
bool FZeroGroundTile(const FZeroGround *ground, unsigned x, unsigned y,
                     uint8_t *tile);
/* Draw only on the disposable wide PPU. False requests the normal fallback. */
bool FZeroGroundRenderLine(FZeroGround *ground, Ppu *copy, int line);
