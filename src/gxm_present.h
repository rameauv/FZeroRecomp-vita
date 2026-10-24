#pragma once

#include <stdbool.h>
#include <stdint.h>

/* Direct GXM presentation: one texture, one quad, point filtering. The game
 * owns GXM because neither SDL2's nor SDL3's Vita renderer can run a custom
 * shader, which the GPU ground lines need. */

/* Set up GXM for a src_w x src_h ARGB8888 frame. False on any failure, with
 * the reason logged. src_w x src_h is also the largest size
 * GxmPresentSetSourceSize accepts later. */
bool GxmPresentInit(int src_w, int src_h);

/* Present a src_w x src_h frame from now on. It must fit the size given to
 * GxmPresentInit. */
bool GxmPresentSetSourceSize(int src_w, int src_h);

/* Show a system Yes/No message dialog and wait for the answer. 1 is Yes, 0 is
 * No, -1 is an error or a cancel (the Circle or Cross back button). */
int GxmPresentAskYesNo(const char *message);

/* Copy the frame into the GPU texture. src_stride is in pixels. */
void GxmPresentUpload(const uint32_t *pixels, int src_stride);

/* Draw the texture into (x, y, w, h) on the 960x544 screen, black elsewhere,
 * and queue the flip. */
void GxmPresentDraw(int x, int y, int w, int h);

void GxmPresentShutdown(void);

/* Whole Mode 7 ground lines, composed on the GPU (src/shaders/ground_f.cg).
 * Two sets of inputs and outputs alternate, so the CPU
 * fills one while the GPU still reads the other. All buffers are uncached
 * main memory; their layout is documented in ground_f.cg. */
enum { kGxmGroundW = 398, kGxmGroundStride = 400, kGxmGroundH = 224,
       kGxmGroundTilesW = 144, kGxmGroundLineTexels = 16 };

typedef struct {
    uint16_t *lines;      /* kGxmGroundH rows of kGxmGroundLineTexels x 4 */
    uint8_t *mtiles;      /* kGxmGroundH rows of kGxmGroundTilesW */
    uint16_t *vram;       /* the first 16,384 VRAM words */
    uint16_t *objz;       /* kGxmGroundH rows of kGxmGroundStride */
    uint8_t *cgram;       /* 256 x (r, g, b, 0) */
    uint8_t *course;      /* the 64 KB course map, 256 x 256 */
    const uint32_t *out;  /* kGxmGroundH rows of kGxmGroundStride, R G B A */
} GxmGroundSet;

/* Switch to the other set, wait until the GPU no longer reads it (its ground
 * scene and the present scene that showed it), and return its buffers. `out`
 * then holds that set's last result. */
bool GxmGroundAcquire(GxmGroundSet *set);

/* Draw the acquired set in a scene of its own. Call after GxmPresentUpload
 * (lines the CPU kept read that frame) and before GxmPresentDraw, which then
 * shows the result instead of the uploaded frame. With `sync`, wait for the
 * scene and return the microseconds waited, else 0. */
uint32_t GxmGroundSubmit(bool sync);
