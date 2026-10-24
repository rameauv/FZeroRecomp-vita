#pragma once

#include "common_cpu_infra.h"

/* F-Zero (US) frame driver. Mirrors the SMW game layer: the host main loop
 * calls RtlRunFrame() which invokes FZeroRunOneFrameOfGame(), then the host
 * calls FZeroDrawPpuFrame() to render the PPU field (HDMA + raster IRQ). */
void FZeroRunOneFrameOfGame(void);
void FZeroDrawPpuFrame(void);

/* Re-arm the frame driver's boot gate after the host soft-resets the machine
 * (RtlReset). The next FZeroRunOneFrameOfGame then re-runs the reset-vector
 * boot (cpu_state_init + scheduler from $8000) instead of resuming the
 * main-loop spin with a stale CPU over a zeroed machine. Call after
 * RtlReset, before the next frame. */
void FZeroSoftReset(void);

struct FZeroLayers;
void FZeroSetLayers(struct FZeroLayers *layers);

extern const RtlGameInfo kFZeroGameInfo;
