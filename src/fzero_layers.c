#include "fzero_layers.h"
#include "fzero_wide_vram.h"
#include <string.h>
#include <stdlib.h>


/* ---------------------------------------------------------------------------
 * FZERO_WIDEPROF -- probes INSIDE the layers pass.
 *
 * Timed only from the outside, most of widescreen's Vita cost shows up as
 * "everything else". These probes open it: the three parts of FZeroLayersProcessLine, and inside each the scratch
 * copy, the extra ppu_runLine and the per-pixel loops -- plus the BYTES each
 * copy moves, because the startup membench shows bytes are what port to this
 * machine and microseconds are what do not.
 *
 * Measured on PC first: ProcessLine 9,070 us/frame = mask 3,185 + build 1,425
 * + move 4,470, of which CaptureSprites alone is 6,390 (70%) and its
 * whole-Ppu memcpy is 4,280. The pass copies 153 MB/frame and 149 MB of that
 * is CaptureSprites and MoveWideHud -- two sites CopyPpuForWideLine
 * never reached, while the one it did reach now moves 3.8 MB.
 * --------------------------------------------------------------------------- */
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
 * whole-struct memcpy (198,432 B). Runtime so FZERO_WIDE_SHAREDCOPY_AB can
 * alternate it inside one run; a Vita launch has no environment, so the arm
 * must come from the build. Defaults ON. */
int g_fzero_wide_sharedcopy = 1;

/* Collapse the 704 coverage renders. 1 = read the winning OAM
 * slot the native sprite pass already recorded, 0 = re-render the line once per
 * OAM range. Runtime so FZERO_WIDE_FASTCOV_AB can alternate it inside one run;
 * a Vita launch has no environment, so the arm must come from the build. */
int g_fzero_wide_fastcov = 1;

/* Spread the HUD to the screen edges, or leave it centred. 1 = MoveWideHud
 * runs: right-half HUD shifts +142px and the scene beneath it is re-rendered
 * to fill the hole. 0 = the HUD stays in the original centred 256 band and the
 * world simply extends past it, which costs nothing because the pass is
 * skipped entirely.
 *
 * DEFAULT 0 -- a deliberate concession, taken because MoveWideHud is 38% of
 * the pass on PC and buys placement rather than content. FZERO_WIDE_MOVEHUD_AB
 * alternates the arms to re-price it. Unlike every other lever here this one
 * CHANGES THE PICTURE, so WIDECRC differing between arms is expected. */
int g_fzero_wide_movehud = 0;

/* Defined below, beside the reasoning about what may be skipped. */
static size_t CopyPpuForWideLine(FZeroLayers *layers, Ppu *copy, const Ppu *ppu);

static size_t CopyScratch(FZeroLayers *layers, Ppu *copy, const Ppu *ppu) {
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
 * first+count) cover this pixel" is a comparison rather than a scanline.
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

/* The per-line scratch copy. See FZERO_WIDE_FASTCOPY. */
static int g_wide_fastcopy = -1;
static int g_wide_vramshare = -1;
/* wsMode2Capture and wsMode2Bg1Palette are two FRAME-sized (224 x 256) buffers
 * inside Ppu -- together 114,688 bytes, 58% of the struct. The rasteriser only
 * ever WRITES them, and memsets each row before filling it, so nothing this
 * copy brings in is ever read back. They also belong to a Mode 2 capture
 * feature that F-Zero never enables (wsMode2CaptureLayer stays 0). Copying
 * them per scanline moves 25.7 MB/frame of untouched zeroes.
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
    /* THREE ranges, not two. bgBuffers, objBuffer, objSlotOf and overlayBuffers
     * are contiguous -- 13,600 B, 72% of what this copy used to move -- and are
     * per-line WORKING buffers: ppu_runLine clears each one before it reads it,
     * so nothing the copy brought across was ever observed. Proven rather than
     * argued: poisoning the range with 0xAA instead of copying it left WIDECRC
     * byte-identical over 1,707 distinct wide images.
     *
     * The asserts are load-bearing in the same way the wsMode2 pair is. A field
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
     * it. vramVersion is deliberately NOT bumped -- the array is byte-identical
     * again by the time anyone can look, which is the same invariant the copy
     * used to provide by brute force. */
    copy->renderVram = ppu->vram;
    return moved;
}
static void BuildWideLine(FZeroLayers *layers, const Ppu *ppu, int line,
                          bool supported, bool hud_layout) {
    int y = line - 1;
    uint32_t *world = layers->wide_world[y], *hud = layers->wide_hud[y];
    memset(world, 0, sizeof(layers->wide_world[y]));
    for (int x = 0; x < FZERO_WIDE_WIDTH; ++x) hud[x] = 0xff000000;
    if (supported) {
        Ppu *copy = &layers->scratch;
        WP_T0(wp_a);
        size_t wp_moved = CopyPpuForWideLine(layers, copy, ppu);
        WP_ADD(t_build_copy, wp_a);
#if FZERO_WIDEPROF
        ++g_wideprof.n_build_ran;
        g_wideprof.n_build_bytes_kb += (uint32_t)(wp_moved / 1024);
#else
        (void)wp_moved;
#endif
        FZeroWideVramBegin();
        copy->renderBuffer = (uint8_t *)layers->wide_capture;
        copy->renderPitch = sizeof(layers->wide_capture[0]);
        PpuClearOverlayBindings(copy);
        PpuSetExtraSpace(copy, FZERO_WIDE_MARGIN);
        PpuSetWidescreenLayerClamp(copy, y < 48 ? 4 : 0);
        if (PPU_mode(ppu) == 1) {
            ExtendPanorama(copy, ppu, line, 0, layers->intro_panorama);
            ExtendPanorama(copy, ppu, line, 1, layers->intro_panorama);
        }
        if (layers->native_oam) {
            /* Full-screen effects use the native list, not racing vehicle
             * groups. Preserve its signed coordinates and current artwork. */
            uint8_t hints[16] = {0};
            PpuWsSetOamLeftHints(copy, hints);
            PpuWsSetOamRightHints(copy, hints);
            copy->renderFlags |= kPpuRenderFlags_NoSpriteLimits;
        } else if (layers->vehicles.ready) FZeroVehiclesApply(&layers->vehicles, copy);
        else ExtendVehicleSprites(copy);
        /* Two renderers, not one. FZeroGroundRenderLine is the Mode 7 margin
         * path and takes most race lines; ppu_runLine is the general fallback.
         * Timing them together would attribute all of this to
         * "BuildWideLine's own wide ppu_runLine". */
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
         * live VRAM -- the invariant the whole-struct copy used to provide.
         * An overflowed journal cannot be unwound, so force a full re-copy. */
        if (!FZeroWideVramRestore(copy))
            layers->scratch_vram_valid = false;
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
    bool supported = racing && (PPU_mode(ppu) == 1 || PPU_mode(ppu) == 7) &&
        !PPU_forcedBlank(ppu) && !ppu->extraLeftRight && !PPU_objInterlace(ppu) &&
        (ppu->renderFlags & kPpuRenderFlags_NewRenderer);
    bool mask[FZERO_LAYER_WIDTH] = {0};
    bool text_layout = layers->intro_panorama || layers->results_layout;
    if (supported && text_layout) {
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
    unsigned pixels = 0;
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
    if (supported && (hud_layout || text_layout) && pixels) ++layers->extracted_lines;
    layers->hud_pixels += pixels;
}
