#include "fzero_layers.h"
#include "fzero_wide_vram.h"
#include <string.h>
#include <stdlib.h>

/* FZERO_WIDEPROF: probes inside the layers pass. The three parts of
 * FZeroLayersProcessLine, and inside each the scratch copy, the extra
 * ppu_runLine and the per-pixel loops, plus the bytes each copy moves (on this
 * machine, bytes moved predict the cost better than time on another). */
#if FZERO_WIDEPROF
#include <psp2/kernel/processmgr.h>
struct FZeroWideProf g_wideprof;
#define WP_T0(v) uint64_t v = sceKernelGetProcessTimeWide()
#define WP_ADD(acc, v) do { g_wideprof.acc += sceKernelGetProcessTimeWide() - (v); \
                            g_wideprof.n_reads += 2; } while (0)
#define WP_CAP_TOTAL() (g_wideprof.t_cap_copy + g_wideprof.t_cap_run + g_wideprof.t_cap_loop)
#else
#define WP_T0(v)            ((void)0)
#define WP_ADD(acc, v)      ((void)0)
#define WP_CAP_TOTAL()      0
#endif

/* The shared scratch copy applied to CaptureSprites and MoveWideHud as well.
 * 1 = use CopyPpuForWideLine everywhere (~18 KB per copy), 0 = the original
 * whole-struct memcpy (198,432 B). */
int g_fzero_wide_sharedcopy = 1;

/* Collapse the 704 coverage renders. 1 = read the winning OAM
 * slot the native sprite pass already recorded, 0 = re-render the line once per
 * OAM range. */
int g_fzero_wide_fastcov = 1;

/* Spread the HUD to the screen edges, or leave it centred. 1 = MoveWideHud
 * runs: right-half HUD shifts +142px and the scene beneath it is re-rendered
 * to fill the hole. 0 = the HUD stays in the original centred 256 band and the
 * world extends past it, and the pass is skipped.
 *
 * Default 0: MoveWideHud is expensive and changes only where the HUD is.
 * Unlike the other levers here, this one changes the picture, so WIDECRC
 * differs between the two values. */
int g_fzero_wide_movehud = 0;

/* The ground's corrected tiles as a side array rather than journalled patches
 * into the live tilemap. See fzero_ground.h. Both values give the same
 * picture (WIDECRC and FBCRC identical). */
int g_fzero_wide_sidetiles = 1;

/* Render the margins only. Every scanline is rasterised twice: 256 px kept as
 * the authentic output, then 398 px whose centre 256 are discarded and
 * overwritten. 1 = mark the centre span as already drawn so the drawers, the
 * backdrop clear and the compose loop all skip it; 0 = full-width wide render.
 *
 * Both values must give the same picture: a WIDECRC difference means the skip
 * reached something that is read. */
int g_fzero_wide_marginsonly = 1;

/* Write the composited frame directly, instead of building a world/hud pair
 * and merging it back together.
 *
 * The desktop host keeps the two layers apart so FZeroVisualStyle can shade
 * the scene without touching the HUD. The Vita presents one flat image, so the
 * split carries no information: the merged centre is the native framebuffer,
 * and every merged margin pixel equals wide_capture. The mask phase splits
 * each row into two complementary images, and the merge recombines them into
 * the row it started from, which is an expensive memcpy.
 *
 * 1 = write layers->flat directly and skip all of it; 0 = the pair route,
 * which still fills layers->flat at the end. The two must be byte-identical.
 *
 * It steps aside when MoveWideHud is live. That pass writes the pair directly
 * and relies on the split (its last loop writes hud without clearing world, and
 * that is where the layers stop being complementary), so the direct route
 * cannot reproduce it. */
int g_fzero_wide_flatdirect = 1;

/* Do not run the native 256-wide render on lines the wide pass covers, and
 * take the centre from the wide render.
 *
 * This is not an equivalence in general, because the wide pass feeds the
 * rasteriser different inputs:
 * ExtendPanorama rewrites BG tilemap entries, FZeroVehiclesApply and
 * ExtendVehicleSprites add reconstructed vehicles which can evict real sprites
 * through the 32-per-line and 34-tile limits, kPpuRenderFlags_NoSpriteLimits
 * removes the authentic flicker those limits cause, and the layer clamp
 * differs. With the additive sprite list and the hardware limits kept, the
 * centre is byte-identical to the native render.
 *
 * Both arrangements rasterise 398 columns a line (native 256 plus margins
 * 142, or wide 398), so only the per-line fixed cost changes: one
 * ppu_evaluateSprites instead of two (it walks all 128 OAM entries regardless
 * of width), one set of window calculations, one backdrop setup.
 *
 * To check the centre, compare SNESRECOMP_CENTRECRC with the FBCRC of a run
 * with SNESRECOMP_PRESENTATION=legacy, which never loads the wide pass.
 *
 * Two hard requirements, enforced in FZeroLayersWantsNativeLine:
 *   - the direct composite must be on. The pair route's mask phase reads
 *     ppu->objBuffer and ppu->bgBuffers[0], which only the native render
 *     fills; without it they are the previous line's, silently.
 *   - the line must be one the wide pass actually renders. On an unsupported
 *     line BuildWideLine draws nothing, and with no native render either the
 *     centre would simply be blank. */
#ifndef FZERO_WIDE_NONATIVE_DEFAULT
#define FZERO_WIDE_NONATIVE_DEFAULT 0
#endif
int g_fzero_wide_nonative = FZERO_WIDE_NONATIVE_DEFAULT;

/* The picture half of the option above, without its saving. 1 = take the
 * centre from the wide render; 0 = from the native framebuffer. Implied by
 * g_fzero_wide_nonative. On its own, the native render still runs, so
 * g_pixels stays a reference for how much the centre changed. */
int g_fzero_wide_widecentre = 0;

/* Keep the game's sprite list in the wide render and add the reconstructed
 * cars to it, instead of wiping it. Every extension this pass makes must be
 * side-local; wiping OAM is not, because it removes the HUD from the centre
 * and cuts the game's own sprites at the native edge. See FZeroVehiclesApply.
 *
 * Test this with a race (SNESRECOMP_INPUT_SCRIPT), not the attract demo: the
 * demo shows only the player car and never reconstructs one. */
#ifndef FZERO_WIDE_KEEPOAM_DEFAULT
#define FZERO_WIDE_KEEPOAM_DEFAULT 1
#endif
int g_fzero_wide_keepoam = FZERO_WIDE_KEEPOAM_DEFAULT;

/* kPpuRenderFlags_NoSpriteLimits lets the reconstructed side pieces bypass
 * the sprite budget, but it lifts the limits across the whole scanline,
 * centre included. Default 0: obey the hardware limits. At a race start the
 * field is bunched, lines exceed 32 sprites, and without the limits the wide
 * render draws sprites the hardware drops, so the centre differs from the
 * native one. */
int g_fzero_wide_nolimits = 0;

/* Keep the wide render's scratch Ppu for a whole frame instead of re-copying
 * it every line. See FRAME SCRATCH in BuildWideLine. Byte-identical to the
 * per-line copy. */
int g_fzero_wide_framescratch = 1;

/* Defined below, beside the reasoning about what may be skipped. */
static size_t CopyPpuForWideLine(FZeroLayers *layers, Ppu *copy, const Ppu *ppu);

/* Whether the direct route may be taken for this frame's configuration. */
static bool FlatDirect(const FZeroLayers *layers) {
    return g_fzero_wide_flatdirect &&
           !(layers->move_hud && g_fzero_wide_movehud);
}

/* The one place the `supported` test lives, so the host's decision to skip the
 * native render (before ppu_runLine) and BuildWideLine's decision to render
 * (inside ProcessLine) cannot disagree. A disagreement would be a blank
 * centre. */
static bool LineSupported(const Ppu *ppu, bool racing) {
    return racing && (PPU_mode(ppu) == 1 || PPU_mode(ppu) == 7) &&
        !PPU_forcedBlank(ppu) && !ppu->extraLeftRight && !PPU_objInterlace(ppu) &&
        (ppu->renderFlags & kPpuRenderFlags_NewRenderer);
}

bool FZeroLayersWantsNativeLine(const FZeroLayers *layers, const Ppu *ppu,
                                int line, bool racing) {
    if (!g_fzero_wide_nonative) return true;
    if (line < 1 || line > FZERO_LAYER_HEIGHT) return true;
    /* See g_fzero_wide_nonative. Without these, the mask is stale and the
     * centre is blank. */
    if (!FlatDirect(layers)) return true;
    /* Without the additive sprite list the wide render has no HUD for the
     * centre. */
    if (!g_fzero_wide_keepoam) return true;
    if (LineSupported(ppu, racing)) {
#if FZERO_WIDEPROF
        ++g_wideprof.n_native_skip;
#endif
        return false;
    }
    return true;
}

/* The pair route's merge into layers->flat, so both routes produce the same
 * output. Per line, so it reads data still in cache. */
static void FlattenLine(FZeroLayers *layers, int y) {
    const uint32_t *w = layers->wide_world[y], *h = layers->wide_hud[y];
    uint32_t *flat = layers->flat[y];
    for (int x = 0; x < FZERO_WIDE_WIDTH; ++x)
        flat[x] = (h[x] & 0xff000000u) ? (h[x] & 0x00ffffffu) : w[x];
}

static size_t CopyScratch(FZeroLayers *layers, Ppu *copy, const Ppu *ppu) {
    layers->frame_scratch_valid = false;   /* see FRAME SCRATCH */
    if (g_fzero_wide_sharedcopy) return CopyPpuForWideLine(layers, copy, ppu);
    memcpy(copy, ppu, sizeof(*copy));
    copy->renderVram = NULL;   /* this arm owns its own VRAM copy */
    /* The whole-struct copy re-establishes the invariant by brute force, but
     * it also leaves the version bookkeeping stale for whichever site runs
     * next. Mark it invalid rather than reason about the interleaving. */
    layers->scratch_vram_valid = false;
    return sizeof(*copy);
}

/* Skip capture passes when none of the selected slots can reach this line.
 * 64 is the largest supported sprite dimension; this is only an optimisation. */
static bool RangeOnLine(const Ppu *ppu, int y, int first, int count) {
    for (int slot = first; slot < first + count; ++slot)
        if ((uint8_t)(y - (ppu->oam[slot * 2] >> 8)) < 64) return true;
    return false;
}

/* Coverage without a render. The native sprite pass resolves a unique winning
 * slot per pixel -- lower OAM indices overwrite higher ones -- and now records
 * it in ppu->objSlotOf. The re-render only ever set the mask where the range
 * winner WAS the global winner (selected == bgBuffers[0]), so the global slot
 * carries everything the range query can extract and "did a slot in [first,
 * first+count) cover this pixel" is a comparison, not a render.
 *
 * Safe on the one flag that would break it: every call site passes flags = 0,
 * so kPpuOverlayFlag_RemoveFromGame never fires and the global winner is never
 * the pixel the render skipped. */
static void CaptureSpritesFromSlots(const Ppu *ppu, int first, int count,
                                    bool *mask) {
    WP_T0(wp_c);
#if FZERO_WIDEPROF
    ++g_wideprof.n_cap_ran;
#endif
    const PpuZbufType *obj = ppu->objBuffer.data;
    const PpuZbufType *bg = ppu->bgBuffers[0].data;
    for (int x = 0; x < FZERO_LAYER_WIDTH; ++x) {
        int index = x + kPpuExtraLeftRight;
        PpuZbufType selected = obj[index];
        /* A non-zero palette index is what says this line wrote the pixel; the
         * backdrop clear leaves 0x0500 and objSlotOf stale from an older line. */
        if (!(selected & 0xff) || selected != bg[index]) continue;
        int slot = ppu->objSlotOf[index];
        if (slot >= first && slot < first + count) mask[x] = true;
    }
    WP_ADD(t_cap_loop, wp_c);
}

static void CaptureSprites(FZeroLayers *layers, const Ppu *ppu, int line,
                           int first, int count, bool *mask) {
    int y = line - 1;
#if FZERO_WIDEPROF
    ++g_wideprof.n_cap_called;
#endif
    if (!(ppu->screenEnabled[0] & 0x10) || !RangeOnLine(ppu, y, first, count)) return;
    if (g_fzero_wide_fastcov) { CaptureSpritesFromSlots(ppu, first, count, mask); return; }
    Ppu *copy = &layers->scratch;
    WP_T0(wp_a);
    size_t wp_moved = CopyScratch(layers, copy, ppu);
    WP_ADD(t_cap_copy, wp_a);
#if FZERO_WIDEPROF
    ++g_wideprof.n_cap_ran; g_wideprof.n_cap_bytes_kb += (uint32_t)(wp_moved / 1024);
#else
    (void)wp_moved;
#endif
    copy->renderBuffer = (uint8_t *)layers->capture;
    copy->renderPitch = sizeof(layers->capture[0]);
    PpuClearOverlayBindings(copy);
    PpuBindOverlaySurface(copy, kPpuOverlaySource_Obj,
                          (uint8_t *)layers->capture, sizeof(layers->capture[0]));
    PpuSetOverlayCapture(copy, kPpuOverlaySource_Obj, 0, y, 256, 1, 0);
    PpuSetOverlayOamRange(copy, first, count);
    WP_T0(wp_b);
    ppu_runLine(copy, line);
    WP_ADD(t_cap_run, wp_b);
    WP_T0(wp_c);
    for (int x = 0; x < FZERO_LAYER_WIDTH; ++x) {
        int index = x + kPpuExtraLeftRight;
        PpuZbufType selected = copy->overlayBuffers[kPpuOverlaySource_Obj].data[index];
        /* Use capture for coverage only. Preserve final original RGB, including
         * colour math, windows and master brightness. A hidden sprite cannot
         * promote a different foreground layer into the HUD. */
        if ((selected & 0xff) && selected == ppu->bgBuffers[0].data[index])
            mask[x] = true;
    }
    WP_ADD(t_cap_loop, wp_c);
}

/* Slot 47 is the car's slide spark, between boost and rank instruments.
 * Earlier HUD slots win an equal-priority OBJ overlap; the spark in turn
 * wins over later HUD slots. Keep that ordering when coverage colours match. */
static void CaptureHudSprites(FZeroLayers *layers, const Ppu *ppu, int line,
                               int first, bool *mask) {
    bool spark[256]={0}, later[256]={0};
    CaptureSprites(layers,ppu,line,first,47-first,mask);
    /* Crash animation puts explosion and smoke pieces in slots 48 onward,
     * including the tail. Keep those pieces in the filtered scene. */
    if(layers->crash_layout) return;
    CaptureSprites(layers,ppu,line,47,1,spark);
    CaptureSprites(layers,ppu,line,48,4,later);
    /* Racing uploads use the entire tail for shadows. Only full native
     * uploads can put counters in the final two slots. */
    if(layers->native_oam) CaptureSprites(layers,ppu,line,126,2,later);
    for(int x=0;x<256;++x) mask[x] |= later[x] && !spark[x];
}

/* The racing panoramas are packed into seven-tile-high strips, with the
 * next strip duplicated in the right tilemap page for native scrolling.
 * Unwrap horizontal positions before selecting a strip. A hardware 512-pixel
 * wrap alone selects the wrong strip at the extended screen edges. */
static void ExtendPanorama(Ppu *copy, const Ppu *ppu, int line, int layer, bool intro) {
    const int first_row = layer == 0 ? 4 : 11;
    const int first_scroll = layer == 0 ? 36 : 92;
    const int period = layer == 0 ? 896 : 768;
    const int base = layer == 0 ? 0x7800 : 0x7000;
    /* The intro adds a vertical displacement as its horizon enters the view.
     * Integer division retains the packed strip phase; the row below includes
     * the displacement. Ordinary racing still requires an aligned strip. */
    int vertical = (int)ppu->vScroll[layer] - first_scroll;
    if (line > 47 || ppu->bgXsc[layer] != (base >> 8 | 1) ||
        PPU_bigTiles(ppu, layer) || ppu->hScroll[layer] > 255 ||
        vertical < 0 || (!intro && vertical % 56) || vertical / 56 > 3)
        return;
    int phase = vertical / 56;
    if (phase * 256 >= period) return;
    int row = (ppu->vScroll[layer] + line) / 8;
    int strip_row = row - first_row - phase * 7;
    if (strip_row < 0 || strip_row >= 7) return;
    /* +512 makes division well-defined for the negative left margin. */
    int left = ((int)ppu->hScroll[layer] - FZERO_WIDE_MARGIN + 512) / 8 - 64;
    int right = (ppu->hScroll[layer] + 255 + FZERO_WIDE_MARGIN) / 8;
    for (int tile = left; tile <= right; ++tile) {
        int position = (phase * 256 + tile * 8 + period) % period;
        int source = base + (first_row + (position / 256) * 7 + strip_row) * 32 +
                     (position % 256) / 8;
        unsigned column = (unsigned)tile & 63;
        int destination = base + (column / 32) * 1024 + row * 32 + column % 32;
        FZeroWideVramWrite(copy, (unsigned)destination, ppu->vram[source]);
    }
}

/* The racing OAM layout assigns six eight-piece vehicle groups to 68..115,
 * followed by shadows through slot 127. Full native uploads have a separate
 * side-rendering path. Hints distinguish right-side pieces from the
 * hardware's signed, nine-bit offscreen coordinates. */
static void ExtendVehicleSprites(Ppu *copy) {
    uint8_t hints[16] = {0};
    for (int slot = 0; slot < 128; ++slot) {
        int index = slot * 2;
        int shift = index & 7;
        unsigned high = (copy->highOam[index >> 3] >> shift) & 3;
        bool vehicle = slot >= 68;
        /* Empty vehicle and shadow entries use this parked coordinate.
         * A large sprite there could otherwise reach the far left margin. */
        bool parked = copy->oam[index] == 0x8080 && (high & 1);
        /* Unused pieces in an active vehicle group have Y=0. Their remaining
         * attributes can be stale, including an X high bit that would turn a
         * hidden entry into a fragment at the upper right edge. */
        bool unused = slot < 116 && (copy->oam[index] >> 8) == 0;
        if (vehicle && !parked && !unused) {
            hints[slot >> 3] |= 1u << (slot & 7);
        } else if (g_fzero_wide_keepoam) {
            /* Leave it where the game put it. Without a hint it still cannot
             * decode into the right margin; it simply stays in the centre. */
        } else {
            /* With no right hint, x=256 decodes to -256. Unlike hiding by Y,
             * this cannot wrap back onto the top scanlines. */
            copy->oam[index] &= 0xff00;
            copy->highOam[index >> 3] |= 1u << shift;
        }
    }
    PpuWsSetOamLeftHints(copy, hints);
    PpuWsSetOamRightHints(copy, hints);
    /* Extra side pieces must not consume the authentic sprite budget.
     * Only this disposable side pass bypasses the hardware limits. */
    if (g_fzero_wide_nolimits)
        copy->renderFlags |= kPpuRenderFlags_NoSpriteLimits;
}

/* Intro and result layouts use sprites and BG3 for lettering and counters.
 * Use the visible source so hidden text does not mask the scenery above it. */
static bool TextOverlayPixel(const Ppu *ppu, int x, bool native_oam) {
    unsigned layer = (ppu->bgBuffers[0].data[x + kPpuExtraLeftRight] >> 8) & 15;
    /* Source 6 is OBJ palettes 0..3, which bypass SNES colour math. */
    return layer == kPpuOverlaySource_Bg3 ||
        (native_oam && (layer == kPpuOverlaySource_Obj || layer == 6));
}

/* Side backgrounds and vehicles are rendered on a copy. The authentic centre
 * and live sprite evaluation retain their original width and hardware limits.
 * Capture also runs with widescreen off so a paused menu can switch immediately. */
#include <stddef.h>
/* --- scratch VRAM undo journal (fzero_wide_vram.h) --- */
static struct { uint16_t index; uint16_t value; } g_wv[kFZeroWideVramJournal];
static unsigned g_wv_n;
static bool g_wv_overflow;

void FZeroWideVramBegin(void) { g_wv_n = 0; g_wv_overflow = false; }

void FZeroWideVramWrite(Ppu *copy, unsigned index, uint16_t value) {
    uint16_t *vram = FZeroWideVram(copy);
    index &= 0x7fff;
    if (g_wv_n >= kFZeroWideVramJournal) {
        /* Drop it. See fzero_wide_vram.h: with the live VRAM shared, applying
         * an unjournalled write corrupts emulated state permanently. */
        g_wv_overflow = true;
        return;
    }
    g_wv[g_wv_n].index = (uint16_t)index;
    g_wv[g_wv_n].value = vram[index];
    ++g_wv_n;
    vram[index] = value;
}

bool FZeroWideVramRestore(Ppu *copy) {
    /* Reverse order: a word written twice unwinds to its original value. */
    uint16_t *vram = FZeroWideVram(copy);
    while (g_wv_n) { --g_wv_n; vram[g_wv[g_wv_n].index] = g_wv[g_wv_n].value; }
    return !g_wv_overflow;
}

/* The per-line scratch copy. */
static int g_wide_fastcopy = -1;
static int g_wide_vramshare = -1;
/* wsMode2Capture and wsMode2Bg1Palette are two FRAME-sized (224 x 256) buffers
 * inside Ppu, together 114,688 bytes, 58% of the struct. The rasteriser only
 * writes them, and memsets each row before filling it, so nothing this copy
 * brings in is read. They belong to a Mode 2 capture feature that F-Zero never
 * enables (wsMode2CaptureLayer stays 0).
 *
 * Skipped only when the feature is off; if a title ever enables it, fall back
 * to the whole-struct copy rather than reason about what it reads. */
static size_t CopyPpuForWideLine(FZeroLayers *layers, Ppu *copy, const Ppu *ppu) {
    if (g_wide_fastcopy < 0) { const char *e = getenv("FZERO_WIDE_FASTCOPY");
                               g_wide_fastcopy = (e && *e) ? atoi(e) : 1; }
    if (g_wide_vramshare < 0) { const char *e = getenv("FZERO_WIDE_VRAMSHARE");
                                g_wide_vramshare = (e && *e) ? atoi(e) : 1; }
    if (!g_wide_fastcopy || ppu->wsMode2CaptureLayer) {
        memcpy(copy, ppu, sizeof(*copy));
        layers->scratch_vram_valid = false;
        return sizeof(*copy);
    }
    /* The two buffers must stay adjacent for a single skipped range. A future
     * field inserted between them would silently reintroduce the copy. */
    _Static_assert(offsetof(Ppu, wsMode2Bg1Palette) ==
                   offsetof(Ppu, wsMode2Capture) + sizeof(((Ppu *)0)->wsMode2Capture),
                   "wsMode2 buffers must remain adjacent");
    /* vram is the last field, so skipping it is a shortened second range. */
    _Static_assert(offsetof(Ppu, vram) + sizeof(((Ppu *)0)->vram) == sizeof(Ppu),
                   "vram must remain the last field of Ppu");
    enum { kFrom = offsetof(Ppu, wsMode2Capture),
           kTo   = offsetof(Ppu, wsMode2Bg1Palette) +
                   sizeof(((Ppu *)0)->wsMode2Bg1Palette),
           kVram = offsetof(Ppu, vram) };
    /* Three ranges. bgBuffers, objBuffer, objSlotOf and overlayBuffers are
     * contiguous (13,600 B) and are per-line working buffers: ppu_runLine
     * clears each one before it reads it, so a copy of them is never read.
     * (Filling the range with 0xAA instead of copying it leaves WIDECRC
     * unchanged.)
     *
     * The asserts protect this the same way as for the wsMode2 pair. A field
     * inserted anywhere inside this block would be silently skipped, and a
     * field that is real state rather than a working buffer would then read as
     * whatever the previous line left behind. */
    _Static_assert(offsetof(Ppu, objBuffer) ==
                   offsetof(Ppu, bgBuffers) + sizeof(((Ppu *)0)->bgBuffers),
                   "objBuffer must follow bgBuffers");
    _Static_assert(offsetof(Ppu, objSlotOf) ==
                   offsetof(Ppu, objBuffer) + sizeof(((Ppu *)0)->objBuffer),
                   "objSlotOf must follow objBuffer");
    _Static_assert(offsetof(Ppu, overlayBuffers) ==
                   offsetof(Ppu, objSlotOf) + sizeof(((Ppu *)0)->objSlotOf),
                   "overlayBuffers must follow objSlotOf");
    enum { kSkipFrom = offsetof(Ppu, bgBuffers),
           kSkipTo   = offsetof(Ppu, overlayBuffers) +
                       sizeof(((Ppu *)0)->overlayBuffers) };
    if (!g_wide_vramshare) {
        memcpy(copy, ppu, kFrom);
        memcpy((char *)copy + kTo, (const char *)ppu + kTo, sizeof(*copy) - kTo);
        copy->renderVram = NULL;   /* this arm owns its own VRAM copy */
        layers->scratch_vram_valid = false;
        return sizeof(*copy) - (kTo - kFrom);
    }
    memcpy(copy, ppu, kFrom);
    memcpy((char *)copy + kTo, (const char *)ppu + kTo, kSkipFrom - kTo);
    memcpy((char *)copy + kSkipTo, (const char *)ppu + kSkipTo, kVram - kSkipTo);
    size_t moved = kFrom + (kSkipFrom - kTo) + (kVram - kSkipTo);
    /* SHARE the live VRAM instead of copying it. renderVram is the PPU's
     * read-only drawing hook -- every drawing path goes through
     * PpuRenderVram(), while the CPU ports and save states always use ->vram --
     * so binding it here lets the wide render sample the live 64 KB with no
     * copy at all. The patches the ground and ExtendPanorama make now land in
     * the live array, journalled, and FZeroWideVramRestore puts it back before
     * the line ends. Nothing observes VRAM in between: the guest is not running
     * inside the per-line hook, and the APU worker and audio thread never touch
     * it. vramVersion is not incremented: the array is byte-identical again
     * before anything can read it. */
    copy->renderVram = ppu->vram;
    return moved;
}
static void BuildWideLine(FZeroLayers *layers, const Ppu *ppu, int line,
                          bool supported, bool hud_layout) {
    int y = line - 1;
    const bool direct = FlatDirect(layers);
    /* True exactly when the host skipped ppu_runLine for this line, which is
     * the same condition inverted -- shared so the two cannot disagree. */
    const bool nonative = supported && direct && g_fzero_wide_nonative &&
                          g_fzero_wide_keepoam;
    /* The centre comes from the wide render when asked for it, and always when
     * there is no native render to take it from. */
    const bool widecentre = nonative ||
                            (supported && direct && g_fzero_wide_widecentre);
    uint32_t *world = layers->wide_world[y], *hud = layers->wide_hud[y];
    if (!direct) {
        memset(world, 0, sizeof(layers->wide_world[y]));
        for (int x = 0; x < FZERO_WIDE_WIDTH; ++x) hud[x] = 0xff000000;
    } else if (!supported) {
        /* The pair route produced world=0 / hud=opaque black here, which
         * merges to black. Only the margins need it: the centre is overwritten
         * from the native framebuffer below either way. */
        uint32_t *flat = layers->flat[y];
        for (int x = 0; x < FZERO_WIDE_MARGIN; ++x) flat[x] = 0;
        for (int x = FZERO_WIDE_MARGIN + FZERO_NATIVE_WIDTH;
             x < FZERO_WIDE_WIDTH; ++x) flat[x] = 0;
    }
    if (supported) {
        Ppu *copy = &layers->scratch;
        /* FRAME SCRATCH. Between two lines of one frame the guest changes
         * only the register block, and the setup below gives the same result
         * on every line except the layer clamp. So keep the
         * scratch for the whole frame: copy all of it on the frame's first
         * line, and only the registers after that. The render's own tables
         * (cgramLut, brightnessMult) are then built once a frame instead of
         * once a line. A guest write into OAM or CGRAM, a policy change, or
         * another user of the scratch forces the full copy again. */
        const uint8_t fs_key = (uint8_t)(1u | layers->native_oam << 1 |
                                         layers->vehicles.ready << 2 |
                                         (g_fzero_wide_nolimits != 0) << 3);
        const bool fs_keep = g_fzero_wide_framescratch &&
                             layers->frame_scratch_valid &&
                             line > layers->frame_scratch_line &&
                             ppu->oamCgramVersion == layers->frame_scratch_memver &&
                             fs_key == layers->frame_scratch_key &&
                             (!layers->frame_scratch_ownvram ||
                              ppu->vramVersion == layers->frame_scratch_vramver);
        _Static_assert(offsetof(Ppu, extraLeftCur) >=
                       offsetof(Ppu, countersLatched) + sizeof(((Ppu *)0)->countersLatched),
                       "the register block must end before extraLeftCur");
        _Static_assert(offsetof(Ppu, inidisp) == 0, "the register block starts the struct");
        WP_T0(wp_a);
        size_t wp_moved;
        if (fs_keep) {
            memcpy(copy, ppu, offsetof(Ppu, extraLeftCur));
            /* FZeroVehiclesApply's wiped path zeroes these two registers. */
            copy->oamaddl = layers->frame_scratch_oamaddl;
            copy->oamaddh = layers->frame_scratch_oamaddh;
            wp_moved = offsetof(Ppu, extraLeftCur);
        } else {
            wp_moved = CopyPpuForWideLine(layers, copy, ppu);
        }
        WP_ADD(t_build_copy, wp_a);
        WP_T0(wp_pre);
#if FZERO_WIDEPROF
        ++g_wideprof.n_build_ran;
        g_wideprof.n_build_bytes_kb += (uint32_t)(wp_moved / 1024);
#else
        (void)wp_moved;
#endif
        FZeroWideVramBegin();
        if (!fs_keep) {
            copy->renderBuffer = (uint8_t *)layers->wide_capture;
            copy->renderPitch = sizeof(layers->wide_capture[0]);
            PpuClearOverlayBindings(copy);
            PpuSetExtraSpace(copy, FZERO_WIDE_MARGIN);
            /* Artwork. FZeroVehiclesApply writes a reconstructed car's pose
             * into copy->vram, but a scratch that shares the live VRAM
             * renders from renderVram, so the car would be drawn with
             * whatever the live VRAM holds at that address. On a frame that
             * adds a car, give the scratch its own
             * copy of VRAM (64 KB once a frame) so the artwork is what is
             * drawn. The ground's and panorama's journalled patches follow,
             * because the journal writes to FZeroWideVram(copy). */
            bool own_vram = false;
            if (!layers->native_oam && layers->vehicles.ready)
                for (unsigned id = 0; id < 6; ++id)
                    own_vram |= layers->vehicles.car[id].added;
            if (own_vram) {
                memcpy(copy->vram, ppu->vram, sizeof(copy->vram));
                copy->renderVram = NULL;
            }
            layers->frame_scratch_ownvram = own_vram;
            layers->frame_scratch_vramver = ppu->vramVersion;
            if (layers->native_oam) {
                /* Full-screen effects use the native list, not racing vehicle
                 * groups. Preserve its signed coordinates and current artwork. */
                uint8_t hints[16] = {0};
                PpuWsSetOamLeftHints(copy, hints);
                PpuWsSetOamRightHints(copy, hints);
                if (g_fzero_wide_nolimits)
                    copy->renderFlags |= kPpuRenderFlags_NoSpriteLimits;
            } else if (layers->vehicles.ready) FZeroVehiclesApply(&layers->vehicles, copy);
            else ExtendVehicleSprites(copy);
            layers->frame_scratch_valid = true;
            layers->frame_scratch_memver = ppu->oamCgramVersion;
            layers->frame_scratch_key = fs_key;
            layers->frame_scratch_oamaddl = copy->oamaddl;
            layers->frame_scratch_oamaddh = copy->oamaddh;
        }
        layers->frame_scratch_line = line;
#if FZERO_WIDEPROF
        g_wideprof.n_build_fresh += !fs_keep;
#endif
        /* Per line: the clamp band, and the panorama's journalled VRAM
         * patches, which FZeroWideVramRestore undoes at the end of the line. */
        PpuSetWidescreenLayerClamp(copy, y < 48 ? 4 : 0);
        if (PPU_mode(ppu) == 1) {
            ExtendPanorama(copy, ppu, line, 0, layers->intro_panorama);
            ExtendPanorama(copy, ppu, line, 1, layers->intro_panorama);
        }
        /* Two renderers: FZeroGroundRenderLine is the Mode 7 margin path and
         * takes most race lines; ppu_runLine is the general fallback. They
         * are timed separately. */
        /* Margins only. The consume loop below reads only x outside the
         * authentic centre, and the centre of this framebuffer is memcpy'd
         * over from the native render immediately afterwards, so the centre
         * of this render is discarded in full. Ask the rasteriser not to
         * produce it -- but only on lines where the PPU says the margins come
         * out unchanged, which is what PpuWidescreenCentreSkipSafe decides. */
        {
            /* The margins-only skip and the no-native experiment want opposite
             * things from the same render: one discards the centre, the other
             * keeps it as the only copy there is. No-native wins. */
            bool wp_skip = g_fzero_wide_marginsonly && !widecentre &&
                           PpuWidescreenCentreSkipSafe(copy, y);
            PpuSetWidescreenCentreSkip(copy, wp_skip);
#if FZERO_WIDEPROF
            g_wideprof.n_build_skip += wp_skip;
#endif
        }
        WP_ADD(t_build_pre, wp_pre);
        { WP_T0(wp_r);
          if (FZeroGroundRenderLine(&layers->ground, copy, line)) {
              WP_ADD(t_build_ground, wp_r);
#if FZERO_WIDEPROF
              ++g_wideprof.n_build_ground;
#endif
          } else {
              ppu_runLine(copy, line);
              WP_ADD(t_build_run, wp_r);
#if FZERO_WIDEPROF
              ++g_wideprof.n_build_run;
#endif
          } }
        WP_T0(wp_post);
        PpuSetWidescreenCentreSkip(copy, false);
        if (direct) {
            /* Both branches of the pair route merge to the same value, so a
             * flat host's margin is just the rendered pixel. Two tight runs
             * over 71 columns each, instead of 398 iterations skipping 256. */
            uint32_t *flat = layers->flat[y];
            const uint32_t *cap = layers->wide_capture[y];
            for (int x = 0; x < FZERO_WIDE_MARGIN; ++x)
                flat[x] = cap[x] & 0x00ffffffu;
            for (int x = FZERO_WIDE_MARGIN + FZERO_NATIVE_WIDTH;
                 x < FZERO_WIDE_WIDTH; ++x)
                flat[x] = cap[x] & 0x00ffffffu;
        } else
        for (int x = 0; x < FZERO_WIDE_WIDTH; ++x) {
            if (x >= FZERO_WIDE_MARGIN && x < FZERO_WIDE_MARGIN + FZERO_NATIVE_WIDTH) continue;
            /* A full-centre fallback must also protect the sides. Otherwise
             * Enhanced grades only the margins and exposes the old viewport. */
            if (hud_layout || ((layers->intro_panorama || layers->results_layout) &&
                !TextOverlayPixel(copy, x - FZERO_WIDE_MARGIN, layers->native_oam))) {
                world[x] = layers->wide_capture[y][x];
                hud[x] = 0;
            } else {
                hud[x] = layers->wide_capture[y][x] | 0xff000000u;
            }
        }
        ++layers->wide_lines;
        /* Undo this line's VRAM patches so the scratch is again equal to the
         * live VRAM. An overflowed journal cannot be unwound, so force a full re-copy. */
        if (!FZeroWideVramRestore(copy)) {
            layers->scratch_vram_valid = false;
            layers->frame_scratch_valid = false;
        }
        WP_ADD(t_build_post, wp_post);
    }
    if (direct) {
        if (widecentre) {
            /* There is no native framebuffer row for this line -- the host did
             * not render one. The wide render's own centre is the only copy. */
            WP_T0(wp_c);
            memcpy(layers->flat[y] + FZERO_WIDE_MARGIN,
                   &layers->wide_capture[y][FZERO_WIDE_MARGIN],
                   FZERO_NATIVE_WIDTH * sizeof(uint32_t));
            WP_ADD(t_build_centre, wp_c);
            return;
        }
        /* The merged centre is the native framebuffer row. The pair route
         * reaches it by splitting that row on mask[] and joining it again. */
        WP_T0(wp_c);
        memcpy(layers->flat[y] + FZERO_WIDE_MARGIN,
               ppu->renderBuffer + y * ppu->renderPitch,
               FZERO_NATIVE_WIDTH * sizeof(uint32_t));
        WP_ADD(t_build_centre, wp_c);
        return;
    }
    memcpy(world + FZERO_WIDE_MARGIN, layers->world[y], sizeof(layers->world[y]));
    memcpy(hud + FZERO_WIDE_MARGIN, layers->hud[y], sizeof(layers->hud[y]));
}

static void HideHudSlot(Ppu *copy, unsigned slot) {
    unsigned shift=(slot&3)*2;
    copy->oam[slot*2] &= 0xff00;
    copy->highOam[slot/4] |= 1u<<shift; /* Signed X=-256, never a Y wrap. */
}

/* Move only the racing instrumentation. Messages and repair sprites occupy
 * separate slots and retain their original positions and colour protection. */
static void MoveWideHud(FZeroLayers *layers, const Ppu *ppu, int line, bool protect_scene) {
    WP_T0(wp_pre);
    int y=line-1;
    bool native_text=layers->native_oam &&
        (layers->intro_panorama || layers->results_layout);
    bool gp_bottom=!native_text && layers->results_layout && y>=48;
    bool slots[52]={0};
    for(unsigned slot=20;slot<52;++slot) {
        int x=ppu->oam[slot*2]&255;
        if(ppu->highOam[slot/4] & (1u<<((slot&3)*2))) x-=256;
        /* The ending first retains the map, markers and counters, then
         * reuses their slots for the central results table. Racing corner
         * groups start outside x=48..183; results lettering stays inside. */
        slots[slot]=slot!=47 && (!layers->crash_layout || slot<48) &&
            (!gp_bottom || x<48 || x>=184);
    }
    bool tail[2]={layers->native_oam && !layers->crash_layout,
                  layers->native_oam && !layers->crash_layout};
    bool move[256]={0}, centred[256]={0};
    if(native_text) {
        /* Intro, race results and the crashed-out page share a reduced HUD.
         * Practice course selection instead uses the tail for map pieces.
         * Only tail sprites in the lower-right counter area are lives. */
        CaptureSprites(layers,ppu,line,0,126,centred);
        for(unsigned slot=126;slot<128;++slot) {
            unsigned position=ppu->oam[slot*2];
            unsigned x=position&255, y=position>>8;
            bool offscreen=ppu->highOam[slot/4] & (1u<<((slot&3)*2));
            tail[slot-126]=!offscreen && x>=192 && y>=184 && y<216;
            CaptureSprites(layers,ppu,line,slot,1,tail[slot-126]?move:centred);
        }
    } else CaptureSprites(layers,ppu,line,0,20,centred);
    if(gp_bottom) {
        /* Capture and remove the same selected groups. Keep result lettering
         * in the clean scene beneath an instrument, including equal colours. */
        for(int first=20;first<52;) {
            int end=first+1;
            while(end<52 && slots[end]==slots[first]) ++end;
            CaptureSprites(layers,ppu,line,first,end-first,slots[first]?move:centred);
            first=end;
        }
        CaptureSprites(layers,ppu,line,52,16,centred);
    } else if(!native_text) CaptureHudSprites(layers,ppu,line,20,move);
    bool top=PPU_mode(ppu)==1 && (native_text ?
        layers->results_layout && y<32 : y<48);
    bool power_band=!native_text && top && y>=18 && y<30;
    bool power_math=power_band && y<28;
    bool any=power_band;
    for(int x=0;x<256;++x) {
        unsigned layer=(ppu->bgBuffers[0].data[x+kPpuExtraLeftRight]>>8)&15;
        if(top && layer==2 && (!native_text || x<64)) move[x]=true;
        if(centred[x]) move[x]=false;
        any |= move[x];
    }
    WP_ADD(t_move_pre, wp_pre);
    if(!any) return;

    /* Re-render the live scene beneath the old HUD. No shifted screenshot or
     * neighbouring pixel can recover a car or track detail hidden by it. */
    Ppu *copy=&layers->scratch;
    WP_T0(wp_a);
    size_t wp_moved = CopyScratch(layers, copy, ppu);
    WP_ADD(t_move_copy, wp_a);
#if FZERO_WIDEPROF
    ++g_wideprof.n_move_ran; g_wideprof.n_move_bytes_kb += (uint32_t)(wp_moved / 1024);
#else
    (void)wp_moved;
#endif
    copy->renderBuffer=(uint8_t*)layers->capture;
    copy->renderPitch=sizeof(layers->capture[0]);
    PpuClearOverlayBindings(copy);
    if(!native_text)
        for(unsigned slot=20;slot<52;++slot) if(slots[slot]) HideHudSlot(copy,slot);
    for(unsigned slot=126;slot<128;++slot)
        if(tail[slot-126]) HideHudSlot(copy,slot);
    if(top) {
        copy->screenEnabled[0] &= ~4;
        copy->screenEnabled[1] &= ~4;
    }
    if(power_math) {
        /* The meter fill is produced by a colour window over the sky.
         * Remove that operation only in this clean-scene pass. */
        copy->cgwsel &= 15;
        copy->cgadsub=0;
        copy->fixedColor=0;
    }
    WP_T0(wp_b);
    ppu_runLine(copy,line);
    WP_ADD(t_move_run, wp_b);
    WP_T0(wp_c);
    const uint32_t *original=(const uint32_t*)(ppu->renderBuffer+y*ppu->renderPitch);
    uint32_t *world=layers->wide_world[y], *hud=layers->wide_hud[y];
    for(int x=0;x<256;++x) {
        bool old_meter=power_band && x>=174 && x<242 && !centred[x];
        if(old_meter && power_math && x>=ppu->window1left && x<=ppu->window1right)
            move[x]=true;
        if(!move[x] && !old_meter) continue;
        bool protected_pixel=protect_scene ||
            (native_text && TextOverlayPixel(copy,x,true));
        world[x+FZERO_WIDE_MARGIN]=protected_pixel?0:layers->capture[y][x];
        hud[x+FZERO_WIDE_MARGIN]=protected_pixel?(layers->capture[y][x]|0xff000000u):0;
    }
    /* Clear all sources before drawing destinations: the two regions can
     * overlap when a wide HUD group extends back into the original centre. */
    for(int x=0;x<256;++x) if(move[x]) {
        int destination=x+(x<128?0:2*FZERO_WIDE_MARGIN);
        hud[destination]=original[x]|0xff000000u;
    }
    WP_ADD(t_move_loop, wp_c);
}

void FZeroLayersProcessLine(FZeroLayers *layers, const Ppu *ppu, int line,
                            bool racing, bool hud_layout) {
    if (line < 1 || line > FZERO_LAYER_HEIGHT) return;
    WP_T0(wp_mask);
#if FZERO_WIDEPROF
    uint64_t wp_cap0 = WP_CAP_TOTAL();
#endif
    int y = line - 1;
    const uint32_t *original = (const uint32_t *)(ppu->renderBuffer + y * ppu->renderPitch);
    bool supported = LineSupported(ppu, racing);
    bool mask[FZERO_LAYER_WIDTH] = {0};
    bool text_layout = layers->intro_panorama || layers->results_layout;
    /* The whole of this phase exists to split `original` into two complementary
     * images on mask[], and the only reader of that pair merges it back into
     * `original`. On the direct route nothing splits, so none of it runs --
     * sprite coverage included. See FlatDirect and g_fzero_wide_flatdirect. */
    const bool direct = FlatDirect(layers);
    unsigned pixels = 0;
    if (direct) {
        (void)mask;
    } else if (supported && text_layout) {
        /* Keep the selected filter through intro and results. The GP ending
         * retains racing vehicles; protect its text slots but not those cars.
         * Only the GP ending retains the racing power-window mask. */
        if (!layers->native_oam) {
            CaptureSprites(layers, ppu, line, 0, 68, mask);
        }
        for (int x = 0; x < FZERO_LAYER_WIDTH; ++x)
            mask[x] |= TextOverlayPixel(ppu, x, layers->native_oam);
        if (layers->move_hud && !layers->native_oam &&
            PPU_mode(ppu) == 1 && y >= 18 && y < 30)
            for (int x = 174; x < 242; ++x) mask[x] = true;
    } else if (supported && hud_layout) {
        /* Racing messages, map, times and counters. Exhaust, sparks, vehicles
         * and all twelve shadow slots remain in the scene. */
        CaptureHudSprites(layers, ppu, line, 0, mask);
        for (int x = 0; x < FZERO_LAYER_WIDTH; ++x) {
            unsigned layer = (ppu->bgBuffers[0].data[x + kPpuExtraLeftRight] >> 8) & 15;
            if (y < 48 && PPU_mode(ppu) == 1 && layer == 2) mask[x] = true;
            /* The power fill is a colour-window effect, not a tile layer. */
            if (y >= 18 && y < 30 && x >= 174 && x < 242) mask[x] = true;
        }
    } else {
        /* Menus and unclassified HUD layouts retain Original
         * in the centre. Background expansion has a separate eligibility. */
        memset(mask, 1, sizeof(mask));
        ++layers->protected_lines;
    }
    if (!direct)
        for (int x = 0; x < FZERO_LAYER_WIDTH; ++x) {
            layers->hud[y][x] = mask[x] ? original[x] | 0xff000000u : 0;
            layers->world[y][x] = mask[x] ? 0 : original[x];
            pixels += mask[x];
        }
#if FZERO_WIDEPROF
    g_wideprof.t_capsub_mask += WP_CAP_TOTAL() - wp_cap0;
#endif
    WP_ADD(t_mask, wp_mask);

    WP_T0(wp_build);
    BuildWideLine(layers, ppu, line, supported, hud_layout);
    WP_ADD(t_build, wp_build);

    WP_T0(wp_move);
#if FZERO_WIDEPROF
    uint64_t wp_cap1 = WP_CAP_TOTAL();
#endif
    if(supported && layers->move_hud && g_fzero_wide_movehud)
        MoveWideHud(layers,ppu,line,!hud_layout && !text_layout);
#if FZERO_WIDEPROF
    g_wideprof.t_capsub_move += WP_CAP_TOTAL() - wp_cap1;
#endif
    WP_ADD(t_move, wp_move);
    /* The pair route's merge, per line while the 398 pixels are still in
     * cache. The direct route has already written layers->flat. */
    if (!direct) { WP_T0(wp_fl); FlattenLine(layers, y); WP_ADD(t_flatten, wp_fl); }
    if (supported && (hud_layout || text_layout) && pixels) ++layers->extracted_lines;
    layers->hud_pixels += pixels;
}
