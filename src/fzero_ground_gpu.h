#pragma once
/* GPU ground lines: the per-line inputs of
 * src/shaders/ground_f.cg, and an integer C model of that shader.
 *
 * Shared by the Vita host (which packs the inputs) and the PC oracle (which
 * packs them the same way and runs the model against the real PPU), so the
 * PC run tests the packing as well as the composite. Keep the model and the
 * shader in step: every line of the shader has its counterpart below. */
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include "snes/ppu.h"

enum {
    kFZGroundGpuWidth = 398,     /* screen x -71..326 */
    kFZGroundGpuMargin = 71,
    kFZGroundGpuTexels = 16,     /* per line, 4 x U16 each */
    kFZGroundGpuWords = 4 * kFZGroundGpuTexels,
    kFZGroundGpuEdgeBias = 128,  /* edges are sent as x + 128 */
    kFZGroundGpuNoEdge = 65535,
};

/* Per-line layout, in U16 words (texel t = words 4t..4t+3):
 *   t0  bu_lo, bu_hi, ku_lo, ku_hi   u(x) = bu + ku * x modulo 2^21, with
 *   t1  bv_lo, bv_hi, kv_lo, kv_hi   lo = bits 0..10 and hi = bits 11..20
 *   t2  fixed r, g, b, INIDISP brightness
 *   t3  drawn here, half, subtract, course (1: the margins' tiles come from
 *       the course texture, not the side array)
 *   t4  colour window edges 1..4, as x + 128 (unused: 65535)
 *   t5  edges 5..7, 65535
 *   t6  span flags 0..3, t7 span flags 4..7:
 *       bit 0 keep colour, bit 1 math on BG1, bit 2 on OBJ layer 4, bit 3 on
 *       the backdrop (layer 5)
 *   t8  course world, origin x mod 8192, origin y mod 4096, 0 (set by the
 *       host when t3.w is 1)
 * Returns false when the line is not one the composite models. */
static inline bool FZeroGroundGpuPack(Ppu *copy, int line, uint32_t sx,
                                      uint32_t sy, uint16_t *w) {
    if (PPU_forcedBlank(copy) || PPU_addSubscreen(copy)) return false;
    /* BG1 must be on the main screen and not windowed. On ground lines it
     * always is, so the composite does not model the other cases. */
    if (!(copy->screenEnabled[0] & 1) || (copy->screenWindowed[0] & 1)) return false;
    int16_t edges[8];
    uint8_t keep, math;
    int nr = PpuColorWindowSpans(copy, line, edges, &keep, &math);
    if (nr < 1 || nr > 7) return false;
    if (edges[0] != -kFZGroundGpuMargin || edges[nr] != 256 + kFZGroundGpuMargin) return false;
    memset(w, 0, kFZGroundGpuWords * sizeof(uint16_t));
    const bool flip = PPU_m7xFlip(copy);
    const int a = copy->m7matrix[0], c = copy->m7matrix[2];
    /* rx = 255 - x under the flip, so a * rx = 255a - a * x. */
    /* 21 bits: the course lookup needs u modulo 8192 (tools/mode7_exact.c
     * part 6); the centre's tile, modulo 128, is unchanged by the wider hi. */
    uint32_t bu = (sx + (flip ? 255u * (uint32_t)a : 0)) & 0x1fffff;
    uint32_t bv = (sy + (flip ? 255u * (uint32_t)c : 0)) & 0x1fffff;
    uint32_t ku = (uint32_t)(flip ? -a : a) & 0x1fffff;
    uint32_t kv = (uint32_t)(flip ? -c : c) & 0x1fffff;
    w[0] = bu & 2047, w[1] = (uint16_t)(bu >> 11), w[2] = ku & 2047, w[3] = (uint16_t)(ku >> 11);
    w[4] = bv & 2047, w[5] = (uint16_t)(bv >> 11), w[6] = kv & 2047, w[7] = (uint16_t)(kv >> 11);
    const unsigned f = copy->fixedColor, me = PPU_mathEnabled(copy);
    w[8] = f & 31, w[9] = (f >> 5) & 31, w[10] = (f >> 10) & 31;
    w[11] = PPU_brightness(copy);
    w[12] = 1, w[13] = PPU_halfColor(copy) ? 1 : 0, w[14] = PPU_subtractColor(copy) ? 1 : 0;
    for (int i = 1; i < 8; ++i)
        w[16 + (i - 1)] = i < nr ? (uint16_t)(edges[i] + kFZGroundGpuEdgeBias) : kFZGroundGpuNoEdge;
    w[23] = kFZGroundGpuNoEdge;
    for (int i = 0; i < nr; ++i) {
        unsigned m = (math >> i) & 1;
        w[24 + i] = (uint16_t)(((keep >> i) & 1) | (m & me & 1) << 1 |
                               (m & (me >> 4) & 1) << 2 | (m & (me >> 5) & 1) << 3);
    }
    return true;
}

static inline int FZGFloorDiv(int v, int d) { return v >= 0 ? v / d : -((-v + d - 1) / d); }

/* The shader's course lookup for a margin column, in integers: FZeroGroundTile
 * on U = u modulo 8192 and V = v modulo 4096 plus the origin in t8. `course`
 * is the 64 KB map. The host only sets t3.w on a course whose every reachable
 * lookup is valid, so there is no failure case here. */
static inline unsigned FZeroGroundGpuCourseStage(const uint16_t *w, int x,
                                                 const uint8_t *course, int stage) {
    int au = w[0] + w[2] * x, av = w[4] + w[6] * x;
    int fu = ((au % 2048) + 2048) % 2048 >> 8, fv = ((av % 2048) + 2048) % 2048 >> 8;
    int tu = ((w[1] + FZGFloorDiv(au, 2048) + w[3] * x) % 1024 + 1024) % 1024;
    int tv = ((w[5] + FZGFloorDiv(av, 2048) + w[7] * x) % 1024 + 1024) % 1024;
    unsigned X = (unsigned)(tu * 8 + fu + w[33]) & 8191;
    unsigned Y = (unsigned)(tv * 8 + fv + w[34]) & 4095;
    unsigned room = course[w[32] + (Y >> 8) * 32 + (X >> 8)];
    unsigned ra = 0x5000 + room * 32 + ((Y >> 4) & 15) * 2;
    unsigned row = course[ra] | (unsigned)course[ra + 1] << 8;
    unsigned entry = (row + ((X >> 4) & 15) * 2) & 0xffff;
    unsigned panel = course[entry] | (unsigned)course[(entry + 1) & 0xffff] << 8;
    unsigned tile = course[(panel + ((X >> 3) & 1) * 2 + ((Y >> 3) & 1)) & 0xffff];
    switch (stage) {
    case 6: return ra & 255;
    case 7: return ra >> 8;
    case 8: case 9: case 10: return 0;   /* fractions: exact values have none */
    case 11: return course[ra];
    case 1: return room;
    case 2: return row & 255;
    case 3: return panel & 255;
    case 4: return X & 255;
    case 5: return Y & 255;
    default: return tile;
    }
}

static inline unsigned FZeroGroundGpuCourseTile(const uint16_t *w, int x,
                                                const uint8_t *course) {
    return FZeroGroundGpuCourseStage(w, x, course, 0);
}

/* The shader, in integers. col is 0..397. margin_tiles holds the side array
 * (142 entries), or is NULL when t3.w selects the course (then `course` is
 * the 64 KB map), vram the 32,768 VRAM words, objz the line's z-buffer from
 * screen x -71 (398 entries). Returns 0x00RRGGBB as the PPU writes it. */
static inline uint32_t FZeroGroundGpuModel(const uint16_t *w, int col,
                                           const uint8_t *margin_tiles,
                                           const uint16_t *vram,
                                           const uint16_t *cgram,
                                           const uint16_t *objz,
                                           const uint8_t *course) {
    const int x = col - kFZGroundGpuMargin;
    /* BG1. */
    int au = w[0] + w[2] * x, av = w[4] + w[6] * x;
    int pu = ((au % 2048) + 2048) % 2048 >> 8, pv = ((av % 2048) + 2048) % 2048 >> 8;
    int tu = ((w[1] + FZGFloorDiv(au, 2048) + w[3] * x) % 128 + 128) % 128;
    int tv = ((w[5] + FZGFloorDiv(av, 2048) + w[7] * x) % 128 + 128) % 128;
    int tile;
    if (x >= 0 && x < 256) tile = vram[tv * 128 + tu] & 255;
    else if (w[15]) tile = (int)FZeroGroundGpuCourseTile(w, x, course);
    else if (x < 0) tile = margin_tiles[col];
    else tile = margin_tiles[col - 256];
    int bg = vram[tile * 64 + pv * 8 + pu] >> 8;
    /* Priority: BG1 is level 5. */
    unsigned z = objz[col], level = z >> 12;
    unsigned index, layer;
    if (level <= 5 && bg) index = (unsigned)bg, layer = 0;
    else index = z & 255, layer = (z >> 8) & 15;
    /* The colour window span. */
    int span = 0;
    for (int i = 0; i < 7; ++i) span += x + kFZGroundGpuEdgeBias >= w[16 + i];
    unsigned flags = w[24 + span];
    unsigned cc = cgram[index];
    int r = cc & 31, g = (cc >> 5) & 31, b = (cc >> 10) & 31;
    if (!(flags & 1)) r = g = b = 0;
    unsigned math = layer == 0 ? flags >> 1 & 1 : layer == 4 ? flags >> 2 & 1
                  : layer == 5 ? flags >> 3 & 1 : 0;
    if (math) {
        if (w[14]) {
            r = r > w[8] ? r - w[8] : 0; g = g > w[9] ? g - w[9] : 0; b = b > w[10] ? b - w[10] : 0;
        } else {
            r += w[8], g += w[9], b += w[10];
        }
        if (w[13]) r >>= 1, g >>= 1, b >>= 1;
    }
    int rgb[3] = { r, g, b };
    uint32_t out = 0;
    for (int k = 0; k < 3; ++k) {
        int v = rgb[k] > 31 ? 31 : rgb[k];
        int e = v * 8 + v / 4;
        out = out << 8 | (uint32_t)((e * w[11] * 2 + 1) / 30);  /* floor((e*b + 0.5) / 15) */
    }
    return out;
}
