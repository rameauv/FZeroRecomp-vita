#pragma once
/* One-frame capture for replaying a margin glitch on PC. See
 * fzero_capture_vita.c. Every entry point is a no-op unless FZERO_CAPTURE. */
#include <stdint.h>

#ifndef FZERO_CAPTURE
#define FZERO_CAPTURE 0
#endif

#if FZERO_CAPTURE
#include "fzero_layers.h"
void FZeroCaptureInit(void);
void FZeroCapturePoll(void);
void FZeroCaptureAfterPrepare(const uint8_t *ram);
void FZeroCaptureEndFrame(const FZeroLayers *layers, const Ppu *live,
                          unsigned frame);
#else
static inline void FZeroCaptureAfterPrepare(const uint8_t *ram) { (void)ram; }
#endif
