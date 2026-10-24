/* See fzero_gpu_ground.h. */
#include "fzero_gpu_ground.h"

#include <string.h>

#include <psp2/kernel/processmgr.h>

#include "fzero_ground_gpu.h"
#include "gxm_present.h"
#include "platform.h"

/* Check build: ground_f.cg writes one course lookup stage to alpha (t8.w
 * selects it) and the check compares it with the C model. Needs the matching
 * shader code, which ground_f.cg does not have now. */
#define FZERO_GROUND_STAGE_DUMP 0

enum { kSets = 2, kW = kGxmGroundW, kStride = kGxmGroundStride, kRows = kGxmGroundH,
       kMargin = kFZGroundGpuMargin, kControlEvery = 512 };

static struct {
    bool begun, control;
    int set;
    GxmGroundSet cur;
    uint32_t frame;
    /* The VRAM words last uploaded into each set, so an unchanged frame
     * skips the 32 KB copy. */
    uint16_t vram_shadow[kSets][128 * 128];
    bool vram_valid[kSets];
    uint8_t cgram[4 * 256];
#if FZERO_GPU_GROUND_CHECK
    /* Per set: what the CPU drew, 0x00RRGGBB, and which lines the GPU drew. */
    uint32_t ref[kSets][kRows][kW];
    uint8_t gpu_row[kSets][kRows];
    bool submitted[kSets], was_control[kSets];
    uint32_t frames, ctl_frames, ctl_bad, ctl_blind;
    uint64_t gpu_px, cpu_px, bad, bad_cpu, cmp_us;
    bool have_first;
    int first_row, first_col;
    uint32_t first_gpu, first_want;
    /* Course lookup diagnosis: per bad margin pixel on a
     * course line, the C model's tile on the same GPU inputs (the set's line
     * words and course texture) and on the CPU's map, against the CPU's tile. */
    uint8_t cpu_tiles[kSets][kRows][2 * kMargin];
    const FZeroGround *ground;
    uint32_t cm_bad, cm_tex_ok, cm_map_ok, cm_tex_map_differ;
    /* Per stage (t8.w, 0..5): margin course pixels checked, and alpha != model. */
    uint32_t st_n[12], st_bad[12];
    bool st_have[12];
    int st_row[12], st_col[12];
    unsigned st_gpu[12], st_model[12];
    uint16_t st_w[12][36];
    unsigned st_ra;           /* first row-stage mismatch: the model's address */
    uint8_t st_near[8];       /* course[ra-2..ra+3], course[ra-256], course[ra+256] */
    bool cm_have;
    uint16_t cm_w[36];
    int cm_row, cm_col;
    unsigned cm_cpu, cm_tex, cm_map;
#endif
    uint32_t sub_frames, gpu_rows, vram_uploads, course_rows, course_pages;
    uint32_t sync_n, sync_us, sync_max;
    uint64_t fill_us;
#if FZERO_WIDEPROF
    /* The time around the ground render, and what a dirty-page upload would
     * copy. Timer reads are counted so the probe's own cost shows. */
    uint64_t t_course, t_acquire, t_clear, t_vcmp, t_vcopy, t_cgram, t_line, t_end, t_pages;
    uint32_t reads, vram_writes, pages1, pages2, last_version;
    bool have_version;
#endif
} s;

#if FZERO_WIDEPROF
#define GW_T0(v) uint64_t v = sceKernelGetProcessTimeWide()
#define GW_SPLIT(acc, v) do { uint64_t gw_n = sceKernelGetProcessTimeWide(); \
                              s.acc += gw_n - (v); (v) = gw_n; ++s.reads; } while (0)
#else
#define GW_T0(v)         ((void)0)
#define GW_SPLIT(acc, v) ((void)0)
#endif

enum { kPageWords = 512, kPages = 128 * 128 / kPageWords }; /* 1 KB pages */

#if FZERO_GPU_GROUND_CHECK
static void Check(int set) {
    if (!s.submitted[set]) return;
    const uint32_t *out = s.cur.out;
    uint64_t bad = 0;
    for (int row = 0; row < kRows; ++row) {
        const uint32_t *o = out + row * kStride;
        const uint32_t *r = s.ref[set][row];
        const bool gpu = s.gpu_row[set][row];
        for (int col = 0; col < kW; ++col) {
            uint32_t w = r[col];
            uint32_t want = (w >> 16 & 255) | (w & 0xff00) | (w & 255) << 16;
            uint32_t got = o[col] & 0xffffff;
#if FZERO_GROUND_STAGE_DUMP
            {
                const uint16_t *lw = s.cur.lines + row * kFZGroundGpuWords;
                if (gpu && lw[15] && (col < kMargin || col >= 256 + kMargin) &&
                    !s.was_control[set]) {
                    const int st = lw[35] % 12;
                    const unsigned m = FZeroGroundGpuCourseStage(lw, col - kMargin,
                                                                 s.cur.course, st);
                    const unsigned a = o[col] >> 24;
                    ++s.st_n[st];
                    if (a != m) {
                        ++s.st_bad[st];
                        if (!s.st_have[st]) {
                            s.st_have[st] = true;
                            s.st_row[st] = row, s.st_col[st] = col;
                            s.st_gpu[st] = a, s.st_model[st] = m;
                            memcpy(s.st_w[st], lw, sizeof(s.st_w[st]));
                            if (st == 2) {
                                const int x = col - kMargin;
                                const unsigned ra =
                                    FZeroGroundGpuCourseStage(lw, x, s.cur.course, 7) << 8 |
                                    FZeroGroundGpuCourseStage(lw, x, s.cur.course, 6);
                                s.st_ra = ra;
                                for (int k = 0; k < 6; ++k)
                                    s.st_near[k] = s.cur.course[(ra + k - 2) & 0xffff];
                                s.st_near[6] = s.cur.course[(ra - 256) & 0xffff];
                                s.st_near[7] = s.cur.course[(ra + 256) & 0xffff];
                            }
                        }
                    }
                }
            }
#endif
            if (got == want) continue;
            ++bad;
            const uint16_t *lw = s.cur.lines + row * kFZGroundGpuWords;
            if (gpu && lw[15] && (col < kMargin || col >= 256 + kMargin) && s.ground &&
                !s.was_control[set]) {
                const int x = col - kMargin;
                const unsigned ct = s.cpu_tiles[set][row][col < kMargin ? col : col - 256];
                const unsigned tt = FZeroGroundGpuCourseTile(lw, x, s.cur.course);
                const unsigned mt = FZeroGroundGpuCourseTile(lw, x, s.ground->map);
                ++s.cm_bad;
                s.cm_tex_ok += tt == ct;
                s.cm_map_ok += mt == ct;
                s.cm_tex_map_differ += tt != mt;
                if (!s.cm_have) {
                    s.cm_have = true;
                    memcpy(s.cm_w, lw, sizeof(s.cm_w));
                    s.cm_row = row, s.cm_col = col;
                    s.cm_cpu = ct, s.cm_tex = tt, s.cm_map = mt;
                }
            }
            if (!s.was_control[set]) {
                if (gpu) ++s.bad; else ++s.bad_cpu;
                if (!s.have_first) {
                    s.have_first = true;
                    s.first_row = row, s.first_col = col;
                    s.first_gpu = o[col], s.first_want = want;
                }
            }
        }
        if (!s.was_control[set]) {
            if (gpu) s.gpu_px += kW; else s.cpu_px += kW;
        }
    }
    if (s.was_control[set]) {
        ++s.ctl_frames;
        s.ctl_bad += (uint32_t)bad;
        s.ctl_blind += bad == 0;
    } else {
        ++s.frames;
    }
    s.submitted[set] = false;
}
#endif

static void Begin(Ppu *copy, FZeroGround *ground) {
    uint64_t t0 = sceKernelGetProcessTimeWide();
    GW_T0(tw);
    if (!GxmGroundAcquire(&s.cur)) return;
    GW_SPLIT(t_acquire, tw);
    s.set ^= 1;
#if FZERO_GPU_GROUND_CHECK
    Check(s.set);
    uint64_t t1 = sceKernelGetProcessTimeWide();
    s.cmp_us += t1 - t0;
    t0 = t1;
    memset(s.gpu_row[s.set], 0, sizeof(s.gpu_row[s.set]));
#endif
    ++s.frame;
#if FZERO_GPU_GROUND_CHECK
    /* The negative control corrupts the picture on purpose (brightness 15 + 1
     * wraps to 0, a black frame), so it exists only in the check build. */
    s.control = s.frame % kControlEvery == 0;
#endif
    /* No line is the GPU's until it says so: clear texel 3 of every row. */
    for (int row = 0; row < kRows; ++row)
        memset(s.cur.lines + row * kFZGroundGpuWords + 12, 0, 8);
    /* VRAM (tilemap and character data) and CGRAM are frame-start state. */
    const uint16_t *vram = PpuRenderVram(copy);
#if FZERO_WIDEPROF
    /* What a dirty-page upload would copy: the 1 KB pages that differ from
     * the frame before (the other set's shadow) and from two frames before
     * (this set's shadow, which is what a double-buffered upload must bring
     * up to date). Plus the guest's VRAM writes since the last frame, which
     * vramVersion counts one per byte or word written. Measured before the
     * shadows are updated, and timed on its own so it stays out of fill. */
    uint64_t pages_us = s.t_pages;
    {
        GW_SPLIT(t_clear, tw);
        if (s.have_version) s.vram_writes += copy->vramVersion - s.last_version;
        s.last_version = copy->vramVersion;
        s.have_version = true;
        for (int pg = 0; pg < kPages; ++pg) {
            const size_t o = (size_t)pg * kPageWords, b = kPageWords * sizeof(uint16_t);
            if (s.vram_valid[s.set ^ 1])
                s.pages1 += memcmp(s.vram_shadow[s.set ^ 1] + o, vram + o, b) != 0;
            if (s.vram_valid[s.set])
                s.pages2 += memcmp(s.vram_shadow[s.set] + o, vram + o, b) != 0;
        }
        GW_SPLIT(t_pages, tw);
    }
    pages_us = s.t_pages - pages_us;
#endif
    bool vram_same = s.vram_valid[s.set] &&
        !memcmp(s.vram_shadow[s.set], vram, sizeof(s.vram_shadow[s.set]));
    GW_SPLIT(t_vcmp, tw);
    if (!vram_same) {
        memcpy(s.vram_shadow[s.set], vram, sizeof(s.vram_shadow[s.set]));
        memcpy(s.cur.vram, vram, sizeof(s.vram_shadow[s.set]));
        s.vram_valid[s.set] = true;
        ++s.vram_uploads;
    }
    GW_SPLIT(t_vcopy, tw);
    for (int i = 0; i < 256; ++i) {
        uint16_t c = copy->cgram[i];
        s.cgram[i * 4] = c & 31;
        s.cgram[i * 4 + 1] = (c >> 5) & 31;
        s.cgram[i * 4 + 2] = (c >> 10) & 31;
        s.cgram[i * 4 + 3] = 0;
    }
    memcpy(s.cur.cgram, s.cgram, sizeof(s.cgram));
    GW_SPLIT(t_cgram, tw);
    /* The course, by the pages changed since this set last took them. */
    uint64_t dirty = ground->gpu_dirty[s.set];
    ground->gpu_dirty[s.set] = 0;
    for (unsigned pg = 0; dirty; ++pg, dirty >>= 1)
        if (dirty & 1) {
            memcpy(s.cur.course + ((size_t)pg << 10), ground->map + ((size_t)pg << 10), 1024);
            ++s.course_pages;
        }
    GW_SPLIT(t_course, tw);
    s.begun = true;
    s.fill_us += sceKernelGetProcessTimeWide() - t0;
#if FZERO_WIDEPROF
    s.fill_us -= pages_us; /* fill excludes the page probe */
#endif
}

bool FZeroGpuGroundLineBegin(int line, Ppu *copy, const uint8_t *tiles,
                             uint32_t sx, uint32_t sy, FZeroGround *ground,
                             unsigned ox, unsigned oy) {
    const int row = line - 1;
    if (row < 0 || row >= kRows) return false;
    GW_T0(tw);
    uint16_t w[kFZGroundGpuWords];
    if (!FZeroGroundGpuPack(copy, line, sx, sy, w)) return false;
    if (!tiles) {
        /* t3.w: look the margins up in the course. t8: its origin. */
        w[15] = 1;
        w[32] = ground->world;
        w[33] = (uint16_t)(ox & 8191);
        w[34] = (uint16_t)(oy & 4095);
#if FZERO_GPU_GROUND_CHECK && FZERO_GROUND_STAGE_DUMP
        w[35] = (uint16_t)(s.frame % 12);
#endif
        ++s.course_rows;
    }
    GW_SPLIT(t_line, tw);
    if (!s.begun) {
        Begin(copy, ground);
        if (!s.begun) return false;
#if FZERO_WIDEPROF
        tw = sceKernelGetProcessTimeWide(); /* Begin is timed on its own */
        ++s.reads;
#endif
    }
    uint64_t t0 = sceKernelGetProcessTimeWide();
    if (s.control) w[11] = (uint16_t)((w[11] + 1) & 15);
    memcpy(s.cur.lines + row * kFZGroundGpuWords, w, sizeof(w));
    if (tiles) {
        uint8_t padded[kGxmGroundTilesW] = {0};
        memcpy(padded, tiles, 2 * kMargin);
        memcpy(s.cur.mtiles + row * kGxmGroundTilesW, padded, sizeof(padded));
    }
    ++s.gpu_rows;
    s.fill_us += sceKernelGetProcessTimeWide() - t0;
    GW_SPLIT(t_line, tw);
    return true;
}

#if FZERO_GPU_GROUND_CHECK
void FZeroGpuGroundCheckTiles(int line, const FZeroGround *ground, const uint8_t *tiles) {
    const int row = line - 1;
    if (row < 0 || row >= kRows) return;
    s.ground = ground;
    memcpy(s.cpu_tiles[s.set][row], tiles, 2 * kMargin);
}
#endif

void FZeroGpuGroundLineEnd(int line, Ppu *copy) {
    const int row = line - 1;
    GW_T0(tw);
    uint64_t t0 = sceKernelGetProcessTimeWide();
    memcpy(s.cur.objz + row * kStride,
           &copy->bgBuffers[0].data[kPpuExtraLeftRight - kMargin], kW * sizeof(uint16_t));
    s.fill_us += sceKernelGetProcessTimeWide() - t0;
    GW_SPLIT(t_end, tw);
#if FZERO_GPU_GROUND_CHECK
    /* The reference: the same line, rendered the normal way. ppu_runLine only
     * draws from this scratch copy, so rendering it twice is safe. */
    ppu_runLine(copy, line);
    const uint32_t *output = (const uint32_t *)(copy->renderBuffer + row * copy->renderPitch);
    for (int col = 0; col < kW; ++col)
        s.ref[s.set][row][col] = output[col] & 0xffffff;
    s.gpu_row[s.set][row] = 1;
#else
    (void)line;
#endif
}

void FZeroGpuGroundEndFrame(const uint32_t *flat, int stride) {
    if (!s.begun) return;
    s.begun = false;
    ++s.sub_frames;
#if FZERO_GPU_GROUND_CHECK
    /* The lines the CPU kept: the GPU must pass the uploaded pixels through. */
    for (int row = 0; row < kRows; ++row) {
        if (s.gpu_row[s.set][row]) continue;
        const uint32_t *f = flat + row * stride;
        for (int col = 0; col < kW; ++col)
            s.ref[s.set][row][col] = f[col] & 0xffffff;
    }
#else
    (void)flat, (void)stride;
#endif
    /* Every 64th frame waits for the scene, to time it. Any earlier scene
     * still queued is inside the figure, so it is an upper bound. */
    bool sync = s.frame % 64 == 1;
    uint32_t us = GxmGroundSubmit(sync);
    if (sync) {
        ++s.sync_n;
        s.sync_us += us;
        if (us > s.sync_max) s.sync_max = us;
    }
#if FZERO_GPU_GROUND_CHECK
    s.submitted[s.set] = true;
    s.was_control[s.set] = s.control;
#endif
}

void FZeroGpuGroundReport(void) {
    if (!s.sub_frames) return;
    uint32_t n = s.sub_frames;
    FZERO_LOG("INFO",
              "  gpuground: frames=%u gpu_rows=%u course_rows=%u course_pages=%u"
              " vram_uploads=%u fill=%u us/frame | wait=%u max=%u us (n=%u)",
              (unsigned)n, (unsigned)s.gpu_rows, (unsigned)s.course_rows,
              (unsigned)s.course_pages, (unsigned)s.vram_uploads,
              (unsigned)(s.fill_us / n),
              (unsigned)(s.sync_n ? s.sync_us / s.sync_n : 0), (unsigned)s.sync_max,
              (unsigned)s.sync_n);
#if FZERO_GPU_GROUND_CHECK
    FZERO_LOG("INFO",
              "  gpuground check: frames=%u gpu_px=%llu bad=%llu cpu_px=%llu bad_cpu=%llu"
              " | ctl frames=%u bad=%u blind=%u | cmp=%u us/frame",
              (unsigned)s.frames, (unsigned long long)s.gpu_px, (unsigned long long)s.bad,
              (unsigned long long)s.cpu_px, (unsigned long long)s.bad_cpu,
              (unsigned)s.ctl_frames, (unsigned)s.ctl_bad, (unsigned)s.ctl_blind,
              (unsigned)(s.cmp_us / n));
    if (s.have_first)
        FZERO_LOG("WARN", "  gpuground first bad: row=%d col=%d gpu=%08x want=%06x",
                  s.first_row, s.first_col, (unsigned)s.first_gpu, (unsigned)s.first_want);
    FZERO_LOG("INFO",
              "  gpuground course: bad margin px %u | model tile == cpu: on GPU texture %u,"
              " on CPU map %u | texture != map %u",
              (unsigned)s.cm_bad, (unsigned)s.cm_tex_ok, (unsigned)s.cm_map_ok,
              (unsigned)s.cm_tex_map_differ);
    if (s.cm_have) {
        const uint16_t *w = s.cm_w;
        FZERO_LOG("WARN",
                  "  gpuground course first: row=%d col=%d cpu=%u tex=%u map=%u"
                  " w=%u %u %u %u %u %u %u %u t3=%u %u %u %u t8=%u %u %u",
                  s.cm_row, s.cm_col, s.cm_cpu, s.cm_tex, s.cm_map,
                  w[0], w[1], w[2], w[3], w[4], w[5], w[6], w[7],
                  w[12], w[13], w[14], w[15], w[32], w[33], w[34]);
    }
#if FZERO_GROUND_STAGE_DUMP
    static const char *const kStage[12] = { "tile", "room", "row", "panel", "X", "Y",
        "ra_lo", "ra_hi", "fracX", "fracY", "fracRaw", "raw" };
    for (int st = 0; st < 12; ++st) {
        FZERO_LOG("INFO", "  gpuground stage %s: px %u, alpha != model %u", kStage[st],
                  (unsigned)s.st_n[st], (unsigned)s.st_bad[st]);
        if (s.st_have[st]) {
            const uint16_t *w = s.st_w[st];
            FZERO_LOG("WARN", "  gpuground stage %s first: row=%d col=%d gpu=%u model=%u"
                      " w=%u %u %u %u %u %u %u %u t8=%u %u %u",
                      kStage[st], s.st_row[st], s.st_col[st], s.st_gpu[st], s.st_model[st],
                      w[0], w[1], w[2], w[3], w[4], w[5], w[6], w[7], w[32], w[33], w[34]);
        }
        if (st == 2 && s.st_have[st])
            FZERO_LOG("WARN", "  gpuground row near: ra=%04X bytes[-2..+3]=%u %u %u %u %u %u"
                      " [-256]=%u [+256]=%u", s.st_ra, s.st_near[0], s.st_near[1],
                      s.st_near[2], s.st_near[3], s.st_near[4], s.st_near[5],
                      s.st_near[6], s.st_near[7]);
        s.st_n[st] = s.st_bad[st] = 0;
        s.st_have[st] = false;
    }
#endif
    s.cm_bad = s.cm_tex_ok = s.cm_map_ok = s.cm_tex_map_differ = 0;
    s.cm_have = false;
    s.frames = s.ctl_frames = s.ctl_bad = s.ctl_blind = 0;
    s.gpu_px = s.cpu_px = s.bad = s.bad_cpu = s.cmp_us = 0;
    s.have_first = false;
#endif
#if FZERO_WIDEPROF
    FZERO_LOG("INFO",
              "  gpuground fill: acquire %u + clear %u + vram cmp %u + copy %u + cgram %u"
              " + course %u"
              " | per line: pack+rows %u, objz %u (us/frame)",
              (unsigned)(s.t_acquire / n), (unsigned)(s.t_clear / n),
              (unsigned)(s.t_vcmp / n), (unsigned)(s.t_vcopy / n),
              (unsigned)(s.t_cgram / n), (unsigned)(s.t_course / n),
              (unsigned)(s.t_line / n),
              (unsigned)(s.t_end / n));
    FZERO_LOG("INFO",
              "  gpuground vram: writes %u | 1 KB pages changed of %u: vs last frame %u,"
              " vs 2 frames %u | page probe %u us, reads %u (per frame)",
              (unsigned)(s.vram_writes / n), (unsigned)kPages,
              (unsigned)(s.pages1 / n), (unsigned)(s.pages2 / n),
              (unsigned)(s.t_pages / n), (unsigned)(s.reads / n));
    s.t_acquire = s.t_clear = s.t_vcmp = s.t_vcopy = s.t_cgram = 0;
    s.t_line = s.t_end = s.t_pages = s.t_course = 0;
    s.reads = s.vram_writes = s.pages1 = s.pages2 = 0;
#endif
    s.sub_frames = s.gpu_rows = s.vram_uploads = s.course_rows = s.course_pages = 0;
    s.sync_n = s.sync_us = s.sync_max = 0;
    s.fill_us = 0;
}
