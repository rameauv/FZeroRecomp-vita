#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "snes/ppu.h"

/* Undo journal for the widescreen pass's writes into the scratch PPU's VRAM.
 *
 * BuildWideLine used to memcpy the whole Ppu per scanline, VRAM included, so
 * the scratch started every line byte-identical to the live PPU and any write
 * the wide pass made was wiped by the next line's copy. That copy is 64 KB a
 * line -- 14.7 MB a frame, and on Vita the dominant remaining cost.
 *
 * VRAM is now copied only when the live one actually changes (ppu->vramVersion),
 * which preserves the same invariant by a different route: the wide pass
 * journals what it overwrites and restores it at end of line, so the scratch is
 * again equal to the live VRAM before the next line starts.
 *
 * Both writers must go through FZeroWideVramWrite: ExtendPanorama in
 * fzero_layers.c and the ground tile cache in fzero_ground.c. A direct
 * copy->vram[i] = ... would not be undone and would corrupt every later line.
 *
 * On overflow the journal reports failure rather than restoring partially, and
 * the offending write is DROPPED rather than applied. That matters now that the
 * target can be the live VRAM: an unjournalled write there would not be a wrong
 * margin pixel, it would be permanent corruption of emulated state. Dropping it
 * makes the worst case cosmetic. Measured high-water is 142 of 512 entries -- the
 * ground's one write per margin pixel -- so this is a net, not a normal path. */

enum { kFZeroWideVramJournal = 512 };

/* The array the wide render actually samples. With renderVram bound this is the
 * LIVE PPU's VRAM, shared rather than copied; with it NULL it is the scratch's
 * own copy. Both writers and the ground's read-back go through here so the two
 * arrangements need no separate code paths.
 *
 * Casting away const is deliberate and is the whole point: renderVram is the
 * PPU's read-only drawing hook, and the wide pass borrows the live array,
 * patches it, renders, and puts it back before anything else can look. */
static inline uint16_t *FZeroWideVram(Ppu *copy) {
    return (uint16_t *)(copy->renderVram ? copy->renderVram : copy->vram);
}

void FZeroWideVramBegin(void);
void FZeroWideVramWrite(Ppu *copy, unsigned index, uint16_t value);
/* Restores in reverse, so repeated writes to one word unwind correctly.
 * Returns false if the journal overflowed and the scratch is now untrustworthy. */
bool FZeroWideVramRestore(Ppu *copy);
