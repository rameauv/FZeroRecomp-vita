#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "snes/ppu.h"
#include "fzero_ground.h"

/* GPU ground lines: a Mode 7 ground line is composed on the
 * GPU (src/shaders/ground_f.cg) instead of by the CPU rasteriser: BG1 across
 * all 398 columns, priority against the sprites, colour window, colour math
 * and brightness. The PPU still draws the sprites of such a line, and nothing
 * else (Ppu::wsMarginGpu = 2). The GPU frame is shown in place of the CPU's
 * in the same frame; nothing is read back.
 *
 * FZERO_GPU_GROUND_CHECK adds the device oracle: every GPU line is rendered a
 * second time the normal way, and one frame later the GPU output is read back
 * and compared with it pixel for pixel, lines the CPU kept included. Every
 * 512th frame is a negative control with the brightness off by one. It costs
 * a second render per line and the readback, so it is not a timing build. */

/* Before FZeroGroundRenderLine renders `line` (1..224). True if the GPU takes
 * the line; the caller then renders with PpuSetWidescreenMarginGpu(copy, 2)
 * and calls FZeroGpuGroundLineEnd. `tiles` is the margins' side array, or
 * NULL for the GPU to look the course up itself from `ground` at the origin
 * (ox, oy); the caller has checked ground->course_valid. */
bool FZeroGpuGroundLineBegin(int line, Ppu *copy, const uint8_t *tiles,
                             uint32_t sx, uint32_t sy, FZeroGround *ground,
                             unsigned ox, unsigned oy);

#if FZERO_GPU_GROUND_CHECK
/* Check build only: compare the CPU's side array for a course line. */
void FZeroGpuGroundCheckTiles(int line, const FZeroGround *ground, const uint8_t *tiles);
#endif

/* After that render, with the flag cleared again. */
void FZeroGpuGroundLineEnd(int line, Ppu *copy);

/* Submit this frame's ground scene, if any line was fed. Call after
 * GxmPresentUpload and before GxmPresentDraw. `flat` is the uploaded frame,
 * which the check compares the CPU's lines with. */
void FZeroGpuGroundEndFrame(const uint32_t *flat, int stride);

/* Log and clear the counters. Called once per heartbeat window. */
void FZeroGpuGroundReport(void);
