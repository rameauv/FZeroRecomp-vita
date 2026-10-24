#include "fzero_ground.h"
#include "fzero_wide_vram.h"
#include "display_layout.h"
#include "fzero_scene.h"
#include <string.h>
#include "fzero_gpu_ground.h"

/* Probe: the ground pass split into prep, render and patch. The side array
 * removes the patch, and the margins-only render shrinks the render, so the
 * parts must be timed separately. Vita only; the PC tree times this pass
 * through g_wideprof. */
#if FZERO_WIDEPROF
#include <psp2/kernel/processmgr.h>
uint64_t g_fzero_ground_prep, g_fzero_ground_render, g_fzero_ground_patch;
uint32_t g_fzero_ground_reads;
#define GP_T0(v) uint64_t v = sceKernelGetProcessTimeWide()
#define GP_ADD(acc, v) do { acc += sceKernelGetProcessTimeWide() - (v); \
                            g_fzero_ground_reads += 2; } while (0)
/* Close one segment of a phase and open the next on the same timer read, so
 * the patch loop's time can be accumulated around the renders inside it
 * without any phase being derived by subtraction. */
#define GP_SPLIT(acc, v) do { uint64_t gp_n = sceKernelGetProcessTimeWide(); \
                              acc += gp_n - (v); (v) = gp_n; \
                              ++g_fzero_ground_reads; } while (0)
#else
#define GP_T0(v)         ((void)0)
#define GP_ADD(acc, v)   ((void)0)
#define GP_SPLIT(acc, v) ((void)0)
#endif

static unsigned Word(const uint8_t *p) { return p[0] | (unsigned)p[1] << 8; }

static uint64_t PageSpan(unsigned first, unsigned last) {
    uint64_t m = 0;
    for (unsigned pg = first >> 10; pg <= (last > 0xffff ? 0xffffu : last) >> 10; ++pg)
        m |= 1ull << pg;
    return m;
}

/* True when no lookup the world grid can reach is refused: FZeroGroundTile's
 * two failure cases, for every room in the grid and every row and column.
 * *deps gets the pages the answer read. */
static bool CourseValid(const FZeroGround *ground, uint64_t *deps) {
    const uint8_t *m = ground->map;
    uint8_t seen[256] = {0};
    uint64_t d = PageSpan(ground->world, ground->world + 511);
    bool valid = true;
    for (unsigned cell = 0; cell < 512; ++cell) {
        unsigned room = m[ground->world + cell];
        if (seen[room]) continue;
        seen[room] = 1;
        d |= PageSpan(0x5000 + room * 32, 0x5000 + room * 32 + 31);
        for (unsigned r = 0; r < 16; ++r) {
            unsigned row = Word(m + 0x5000 + room * 32 + r * 2);
            d |= PageSpan(row, row + 31);
            for (unsigned c = 0; c < 16; ++c) {
                unsigned entry = row + c * 2;
                if (entry > 0xfffe || Word(m + entry) > 0xfffc) valid = false;
            }
        }
    }
    *deps = d;
    return valid;
}

void FZeroGroundPrepare(FZeroGround *ground, const uint8_t *ram) {
    ground->ready = false;
    if (!FZeroSceneWide(ram)) return;
    unsigned world = Word(ram + 0xb0);
    if (world < 0x4c00 || world > 0x4f00 || (world & 255)) return;
    ground->world = world;
    ground->scroll_x = Word(ram + 0xa2);
    ground->scroll_y = Word(ram + 0xa6);
    /* By 1 KB pages, so the GPU copy takes only what changed. In a race only
     * 0x4A00..0x4BFF changes, which no lookup reads. */
    const uint8_t *src = ram + 0x10000;
    uint64_t changed = 0;
    if (!ground->course_init) {
        memcpy(ground->map, src, sizeof(ground->map));
        changed = ~0ull;
        ground->course_init = true;
    } else {
        for (unsigned pg = 0; pg < 64; ++pg) {
            const size_t o = (size_t)pg << 10;
            if (memcmp(ground->map + o, src + o, 1024)) {
                memcpy(ground->map + o, src + o, 1024);
                changed |= 1ull << pg;
            }
        }
    }
    ground->gpu_dirty[0] |= changed;
    ground->gpu_dirty[1] |= changed;
    if (!ground->valid_deps || (changed & ground->valid_deps) ||
        ground->valid_world != world)
        ground->course_valid = CourseValid(ground, &ground->valid_deps);
    ground->valid_world = (uint16_t)world;
    ground->ready = true;
}

bool FZeroGroundTile(const FZeroGround *ground, unsigned x, unsigned y,
                     uint8_t *tile) {
    if (!ground->ready) return false;
    /* The course is a wrapping grid of rooms, each containing 16x16 panels.
     * Each panel supplies four 8x8 tiles in column order. All mutable map
     * levels come from this frame's RAM, including altered track panels. */
    x &= 8191;
    y &= 4095;
    unsigned room = ground->map[ground->world + (y >> 8) * 32 + (x >> 8)];
    unsigned row = Word(ground->map + 0x5000 + room * 32 + ((y >> 4) & 15) * 2);
    unsigned entry = row + ((x >> 4) & 15) * 2;
    if (entry > 0xfffe) return false;
    unsigned panel = Word(ground->map + entry);
    if (panel > 0xfffc) return false;
    *tile = ground->map[panel + ((x >> 3) & 1) * 2 + ((y >> 3) & 1)];
    return true;
}

/* FZeroGroundTile with the last room, row and panel kept. Neighbouring margin
 * pixels usually share them, and each is a dependent read. The result is the
 * same tile; only reads whose index is unchanged are skipped. */
typedef struct TileMemo {
    unsigned room_key, room, row_key, row, entry, panel;
} TileMemo;

#if FZERO_WIDEPROF
uint32_t g_fzero_ground_lookups, g_fzero_ground_room_reads,
         g_fzero_ground_row_reads, g_fzero_ground_panel_reads;
#define GP_COUNT(v) (++(v))
#else
#define GP_COUNT(v) ((void)0)
#endif

static void TileMemoReset(TileMemo *m) {
    m->room_key = m->row_key = m->entry = ~0u;
}

static inline bool MemoTile(const FZeroGround *ground, TileMemo *m,
                            unsigned x, unsigned y, uint8_t *tile) {
    x &= 8191;
    y &= 4095;
    GP_COUNT(g_fzero_ground_lookups);
    unsigned room_key = (y >> 8) * 32 + (x >> 8);
    if (room_key != m->room_key) {
        GP_COUNT(g_fzero_ground_room_reads);
        m->room = ground->map[ground->world + room_key];
        m->room_key = room_key;
        m->row_key = ~0u;
    }
    unsigned row_key = (y >> 4) & 15;
    if (row_key != m->row_key) {
        GP_COUNT(g_fzero_ground_row_reads);
        m->row = Word(ground->map + 0x5000 + m->room * 32 + row_key * 2);
        m->row_key = row_key;
        m->entry = ~0u;
    }
    unsigned entry = m->row + ((x >> 4) & 15) * 2;
    if (entry > 0xfffe) return false;
    if (entry != m->entry) {
        GP_COUNT(g_fzero_ground_panel_reads);
        m->panel = Word(ground->map + entry);
        m->entry = entry;
    }
    if (m->panel > 0xfffc) return false;
    *tile = ground->map[m->panel + ((x >> 3) & 1) * 2 + ((y >> 3) & 1)];
    return true;
}

/* The side array: the course tile of each of the 142 margin columns. */
static bool SideTiles(const FZeroGround *ground, const Ppu *copy, uint32_t sx,
                      uint32_t sy, unsigned ox, unsigned oy, uint8_t *tiles) {
    /* The two margins are independent chains of dependent reads. Walking
     * them side by side lets their cache misses overlap. */
    TileMemo left, right;
    TileMemoReset(&left);
    TileMemoReset(&right);
    for (int i = 0; i < FZERO_WIDE_MARGIN; ++i) {
        int xl = i - FZERO_WIDE_MARGIN, xr = i + 256;
        int rl = PPU_m7xFlip(copy) ? 255 - xl : xl;
        int rr = PPU_m7xFlip(copy) ? 255 - xr : xr;
        unsigned ul = (sx + (uint32_t)(copy->m7matrix[0] * rl)) >> 8;
        unsigned vl = (sy + (uint32_t)(copy->m7matrix[2] * rl)) >> 8;
        unsigned ur = (sx + (uint32_t)(copy->m7matrix[0] * rr)) >> 8;
        unsigned vr = (sy + (uint32_t)(copy->m7matrix[2] * rr)) >> 8;
        if (!MemoTile(ground, &left, ul + ox, vl + oy, &tiles[i]) ||
            !MemoTile(ground, &right, ur + ox, vr + oy,
                      &tiles[i + FZERO_WIDE_MARGIN])) return false;
    }
    return true;
}

static int Signed13(unsigned value) {
    value &= 8191;
    return value & 4096 ? (int)value - 8192 : (int)value;
}

static int ScrollDelta(int value) {
    return value & 8192 ? (int)((unsigned)value | ~1023u) : value & 1023;
}

static void BeginPass(FZeroGround *ground) {
    if (++ground->serial == 0) {
        memset(ground->stamps, 0, sizeof(ground->stamps));
        ground->serial = 1;
    }
}

bool FZeroGroundRenderLine(FZeroGround *ground, Ppu *copy, int line) {
    /* This layout separates the Mode 7 map from OBJ graphics. Do not remap
     * shared VRAM in other layouts or change hardware overflow/mosaic rules. */
    if (!ground->ready || line < 1 || line > FZERO_DISPLAY_HEIGHT ||
        PPU_mode(copy) != 7 || PPU_m7extBg(copy) || (copy->m7sel & 0xc0) ||
        PPU_mosaicEnabled(copy, 0) || (copy->obsel & 31) != 2 ||
        copy->extraLeftRight != FZERO_WIDE_MARGIN) return false;

    int cx = Signed13(copy->m7matrix[4]), cy = Signed13(copy->m7matrix[5]);
    int dh = ScrollDelta(Signed13(copy->m7matrix[6]) - cx);
    int dv = ScrollDelta(Signed13(copy->m7matrix[7]) - cy);
    int y = PPU_m7yFlip(copy) ? 255 - line : line;
    /* Keep the hardware's fixed-point truncation before adding the screen X
     * term. The full course offset is an integral number of cache periods. */
    uint32_t sx = (copy->m7matrix[0] * dh & ~63) +
                  (copy->m7matrix[1] * y & ~63) +
                  (copy->m7matrix[1] * dv & ~63) + cx * 256;
    uint32_t sy = (copy->m7matrix[2] * dh & ~63) +
                  (copy->m7matrix[3] * y & ~63) +
                  (copy->m7matrix[3] * dv & ~63) + cy * 256;
    unsigned ox = ground->scroll_x + 512u - cx;
    unsigned oy = ground->scroll_y + 512u - cy;
    /* The cache origin can cross a boundary between frames. Only accept the
     * metadata paired with these raster registers, never a newer camera. */
    if ((ox | oy) & 1023) return false;

    enum { count = FZERO_WIDE_MARGIN * 2 };
    const bool side = g_fzero_wide_sidetiles != 0;
    GP_T0(gp_prep);
    uint16_t addresses[count];
    uint8_t tiles[count];
    uint32_t pixels[count];
    unsigned corrected = 0;
    /* The GPU looks the margins' course tiles up itself, so
     * the side array below is not built. Only a course on which no lookup is
     * refused qualifies: the CPU cannot drop a GPU line to the fallback. */
    bool course = side && ground->course_valid &&
                  FZeroGpuGroundLineBegin(line, copy, NULL, sx, sy, ground, ox, oy);
    /* The check build still builds it: its reference render needs it. */
    if (side && (!course || FZERO_GPU_GROUND_CHECK) &&
        !SideTiles(ground, copy, sx, sy, ox, oy, tiles) && !course) return false;
#if FZERO_GPU_GROUND_CHECK
    if (course) FZeroGpuGroundCheckTiles(line, ground, tiles);
#endif
    for (int i = 0; !side && i < count; ++i) {
        int x = i < FZERO_WIDE_MARGIN ? i - FZERO_WIDE_MARGIN :
                                                       i - FZERO_WIDE_MARGIN + 256;
        int rx = PPU_m7xFlip(copy) ? 255 - x : x;
        unsigned u = (sx + (uint32_t)(copy->m7matrix[0] * rx)) >> 8;
        unsigned v = (sy + (uint32_t)(copy->m7matrix[2] * rx)) >> 8;
        if (!FZeroGroundTile(ground, u + ox, v + oy, &tiles[i])) return false;
        /* The cell address is only needed by the patching arm, and so is the
         * read-back that counts corrections. The side-array arm addresses the
         * tile by screen column: cells alias between columns, and columns
         * cannot alias each other. */
        if (!side) {
            addresses[i] = (uint16_t)(((v >> 3) & 127) * 128 + ((u >> 3) & 127));
            corrected += (FZeroWideVram(copy)[addresses[i]] & 255) != tiles[i];
        }
    }

    GP_ADD(g_fzero_ground_prep, gp_prep);

    uint32_t *output = (uint32_t *)(copy->renderBuffer + (line - 1) * copy->renderPitch);
    if (side) {
        /* One render, no journal, no second pass. The margins the render
         * leaves in `output` are the final ones. (The arm below copies them
         * out and back only because a split pass would overwrite the spans an
         * earlier pass got right.) */
        (void)output;
        PpuSetMode7MarginTiles(copy, tiles);
        /* The GPU composes this whole line, and the PPU draws only its
         * sprites. */
        const bool gpu = course ||
            FZeroGpuGroundLineBegin(line, copy, tiles, sx, sy, ground, ox, oy);
        PpuSetWidescreenMarginGpu(copy, gpu ? 2 : 0);
        GP_T0(gp_render);
        ppu_runLine(copy, line);
        GP_ADD(g_fzero_ground_render, gp_render);
        if (gpu) {
            PpuSetWidescreenMarginGpu(copy, 0);
            FZeroGpuGroundLineEnd(line, copy);
        }
        PpuSetMode7MarginTiles(copy, NULL);
        ++ground->lines;
        return true;
    }
    GP_T0(gp_patch);
    int first = 0;
    BeginPass(ground);
    for (int i = 0; i <= count; ++i) {
        /* Two course positions can alias the same cache cell on one line.
         * Finish the current span before assigning that cell a different tile.
         * Each pass still uses the PPU's own sprites, windows and colour math. */
        bool conflict = i < count && ground->stamps[addresses[i]] == ground->serial &&
                        (FZeroWideVram(copy)[addresses[i]] & 255) != tiles[i];
        if (i == count || conflict) {
            GP_SPLIT(g_fzero_ground_patch, gp_patch);
            ppu_runLine(copy, line);
            GP_SPLIT(g_fzero_ground_render, gp_patch);
            for (int j = first; j < i; ++j)
                pixels[j] = output[j < FZERO_WIDE_MARGIN ? j : j + 256];
            if (i == count) break;
            ++ground->split_passes;
            first = i;
            BeginPass(ground);
        }
        unsigned address = addresses[i];
        ground->stamps[address] = ground->serial;
        /* High bytes hold the current animated tile artwork. */
        /* Journalled: the scratch VRAM is not rebuilt each line, so this
         * write must be undoable (fzero_wide_vram.h). */
        FZeroWideVramWrite(copy, address,
                           (uint16_t)((FZeroWideVram(copy)[address] & 0xff00) | tiles[i]));
    }
    for (int i = 0; i < count; ++i)
        output[i < FZERO_WIDE_MARGIN ? i : i + 256] = pixels[i];
    GP_ADD(g_fzero_ground_patch, gp_patch);
    ++ground->lines;
    ground->corrected_pixels += corrected;
    return true;
}
