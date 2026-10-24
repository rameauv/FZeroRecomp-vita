/*
 * fzero_capture_vita.c - one-frame capture for replaying a margin glitch on PC.
 *
 * FZERO_CAPTURE only. A tap on the front touchscreen, which the game does not
 * read, arms a capture. The next frame then writes, to
 * ux0:data/fzero_recomp/capture/:
 *
 *   cNN_ram.bin     128 KB WRAM as FZeroVehiclesPrepare read it (start of frame)
 *   cNN_live.bin    live PPU: register block, oam, highOam, cgram, vram
 *   cNN_scratch.bin wide scratch PPU after FZeroVehiclesApply: the same parts,
 *                   plus the left/right OAM hints
 *   cNN_flat.bin    the composited 398x224 frame, 32-bit words
 *   cNN_meta.txt    frame number, sizes, levers and the prepared car list
 *
 * Every array is written separately, so the files do not depend on the ARM
 * layout of Ppu or FZeroVehicles.
 */
#include "fzero_capture_vita.h"

#if FZERO_CAPTURE
#include <psp2/touch.h>
#include <psp2/io/stat.h>
#include <stdio.h>
#include <stddef.h>
#include <string.h>
#include "fzero_layers.h"
#include "platform.h"

#define CAPTURE_DIR "ux0:data/fzero_recomp/capture"

static uint8_t s_ram[0x20000];
static bool s_touch_was_down;
static bool s_armed;      /* next Prepare snapshots WRAM */
static bool s_have_ram;   /* WRAM snapshot taken for this frame */
static unsigned s_index;

void FZeroCaptureInit(void) {
    sceTouchSetSamplingState(SCE_TOUCH_PORT_FRONT, SCE_TOUCH_SAMPLING_STATE_START);
    sceIoMkdir(CAPTURE_DIR, 0777);
}

void FZeroCapturePoll(void) {
    SceTouchData touch;
    bool down = sceTouchPeek(SCE_TOUCH_PORT_FRONT, &touch, 1) > 0 &&
                touch.reportNum > 0;
    if (down && !s_touch_was_down && !s_armed && !s_have_ram) {
        s_armed = true;
        FZERO_LOG("INFO", "capture: armed");
    }
    s_touch_was_down = down;
}

void FZeroCaptureAfterPrepare(const uint8_t *ram) {
    if (!s_armed) return;
    memcpy(s_ram, ram, sizeof(s_ram));
    s_armed = false;
    s_have_ram = true;
}

static bool WriteFile(const char *name, const void *data, size_t size) {
    char path[160];
    snprintf(path, sizeof(path), CAPTURE_DIR "/c%02u_%s", s_index, name);
    FILE *f = fopen(path, "wb");
    if (!f) return false;
    bool ok = fwrite(data, 1, size, f) == size;
    fclose(f);
    return ok;
}

static void WritePpu(FILE *f, const Ppu *p) {
    fwrite(p, 1, offsetof(Ppu, extraLeftCur), f);
    fwrite(p->oam, 1, sizeof(p->oam), f);
    fwrite(p->highOam, 1, sizeof(p->highOam), f);
    fwrite(p->cgram, 1, sizeof(p->cgram), f);
    fwrite(p->vram, 1, sizeof(p->vram), f);
}

static bool WritePpuFile(const char *name, const Ppu *p, bool hints) {
    char path[160];
    snprintf(path, sizeof(path), CAPTURE_DIR "/c%02u_%s", s_index, name);
    FILE *f = fopen(path, "wb");
    if (!f) return false;
    WritePpu(f, p);
    if (hints) {
        fwrite(&p->wsOamLeftHintStrict, 1, 1, f);
        fwrite(p->wsOamLeftHint, 1, sizeof(p->wsOamLeftHint), f);
        fwrite(&p->wsOamRightHintStrict, 1, 1, f);
        fwrite(p->wsOamRightHint, 1, sizeof(p->wsOamRightHint), f);
        fwrite(&p->extraLeftCur, sizeof(p->extraLeftCur), 1, f);
        fwrite(&p->extraRightCur, sizeof(p->extraRightCur), 1, f);
        fwrite(&p->renderFlags, sizeof(p->renderFlags), 1, f);
    }
    bool ok = !ferror(f);
    fclose(f);
    return ok;
}

void FZeroCaptureEndFrame(const FZeroLayers *layers, const Ppu *live,
                          unsigned frame) {
    if (!s_have_ram) return;
    s_have_ram = false;
    bool ok = WriteFile("ram.bin", s_ram, sizeof(s_ram));
    ok &= WritePpuFile("live.bin", live, false);
    ok &= WritePpuFile("scratch.bin", &layers->scratch, true);
    ok &= WriteFile("flat.bin", layers->flat, sizeof(layers->flat));

    char path[160];
    snprintf(path, sizeof(path), CAPTURE_DIR "/c%02u_meta.txt", s_index);
    FILE *f = fopen(path, "w");
    if (f) {
        const FZeroVehicles *v = &layers->vehicles;
        fprintf(f, "frame %u\nregs %u\nkeepoam %d nonative %d framescratch %d "
                   "limits_hw %d\nready %d shadows %u\n",
                frame, (unsigned)offsetof(Ppu, extraLeftCur),
                g_fzero_wide_keepoam, g_fzero_wide_nonative,
                g_fzero_wide_framescratch, g_ppu_margin_limits_hw,
                v->ready, v->shadow_count);
        for (unsigned id = 0; id < 6; ++id) {
            const FZeroVehicle *c = &v->car[id];
            fprintf(f, "car %u visible %d added %d x %d ground_y %d size %u "
                       "count %u high %04x gfx %04x anchor %d/%d oam",
                    id, c->visible, c->added, c->x, c->ground_y, c->size,
                    c->count, c->high, c->graphics_address,
                    v->jump_anchor_valid[id], v->jump_anchor_y[id]);
            for (unsigned w = 0; w < 16; ++w) fprintf(f, " %04x", c->oam[w]);
            fputc('\n', f);
        }
        ok &= !ferror(f);
        fclose(f);
    } else {
        ok = false;
    }
    FZERO_LOG(ok ? "INFO" : "WARN", "capture: c%02u at f%u %s", s_index, frame,
              ok ? "saved" : "FAILED");
    ++s_index;
}
#endif
