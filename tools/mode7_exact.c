/* Exactness check for the Mode 7 shader arithmetic.
 *
 * Build and run on the host:  cc -O2 -o /tmp/mode7_exact tools/mode7_exact.c && /tmp/mode7_exact
 *
 * Part 1, the reduction. For every a (all 65,536), every margin column, both
 * X flips, and 257 values of the 32-bit line start, the host's (bu, ku)
 * modulo 2048 must give the same pixel-in-tile bits as the PPU's own sum
 * xpos = m7startX + a * rx (ppu.c, PpuDrawBackground_mode7).
 *
 * Part 2, the arithmetic. For EVERY (bu, ku) below 2048 and every margin
 * column, the shader's fp32 sequence, written as psp2cgc compiled it
 * (floor as x - frc(x), frc as x - floorf(x)), must give the integer result.
 * 595,591,168 cases. fp32 is forced with volatile stores so the host FPU
 * cannot keep wider intermediates.
 *
 * Part 3, the negative control. The same fp32 sequence fed the UNREDUCED
 * 32-bit start and the signed a, as a naive shader would be. It must fail,
 * or parts 1 and 2 prove nothing.
 *
 * Part 4, brightness (src/shaders/margin_f.cg, Bright). For every INIDISP
 * brightness and every value the compose loop can index, with and without
 * the half table, the fp32 formula must equal the PPU's tables (ppu.c,
 * brightnessMult and brightnessMultHalf).
 *
 * Part 5, the whole line (src/shaders/ground_f.cg). The
 * centre needs the tilemap cell too, bits 11..17 of the sum. The host sends
 * each 18-bit value as lo (bits 0..10) and hi (bits 11..17); the shader forms
 * A = lo_b + lo_k * x and T = hi_b + floor(A / 2048) + hi_k * x. For every a,
 * both flips, 257 starts and every x in -71..326, the fp32 sequence must give
 * the PPU's (xpos >> 8) & 7 and (xpos >> 11) & 127.
 *
 * Part 6, the course lookup on the GPU. The margins' course
 * tile needs u = xpos >> 8 modulo 8192, so bits 11..20 of the sum: the host
 * sends 21-bit values as lo (bits 0..10) and hi (bits 11..20), and the shader
 * forms U = (T mod 1024) * 8 + fine. Same inputs as part 5; the fp32 sequence
 * must give (xpos >> 8) & 8191 AND still part 5's tile, since the centre now
 * reads the wider hi too. Negative control: the 18-bit hi of part 5 must
 * fail the 13-bit U. */
#include <math.h>
#include <stdint.h>
#include <stdio.h>

static int Column(int col) { return col < 71 ? col - 71 : col + 185; }

static float Frc(float v) { volatile float f = v - floorf(v); return f; }

static unsigned ShaderBits(unsigned b, unsigned k, int col) {
    volatile float fcol = (float)col;
    volatile float x = fcol < 71.0f ? fcol - 71.0f : fcol + 185.0f;
    volatile float acc = (float)b + (float)k * x;
    volatile float q = acc * (1.0f / 2048.0f);
    volatile float fl = q - Frc(q);
    volatile float m = acc - 2048.0f * fl;
    volatile float r = m * (1.0f / 256.0f);
    volatile float pix = r - Frc(r);
    return (unsigned)pix;
}

int main(void) {
    unsigned long bad = 0, n = 0;
    for (int flip = 0; flip < 2; ++flip)
        for (int a = -32768; a < 32768; ++a)
            for (int s = 0; s < 257; ++s) {
                uint32_t start = (uint32_t)s * 0x9e3779b9u;
                unsigned bu = (start + (flip ? 255u * (uint32_t)a : 0)) & 2047;
                unsigned ku = (uint32_t)(flip ? -a : a) & 2047;
                for (int col = 0; col < 142; ++col) {
                    int x = Column(col);
                    uint32_t rx = flip ? (uint32_t)(255 - x) : (uint32_t)x;
                    uint32_t xpos = start + (uint32_t)a * rx;
                    unsigned want = xpos >> 8 & 7;
                    unsigned got = (bu + ku * (uint32_t)x) >> 8 & 7;
                    bad += want != got, ++n;
                }
            }
    printf("reduction: %lu cases, %lu bad\n", n, bad);
    unsigned long bad2 = 0, n2 = 0;
    for (unsigned b = 0; b < 2048; ++b)
        for (unsigned k = 0; k < 2048; ++k)
            for (int col = 0; col < 142; ++col) {
                unsigned want = (b + k * (uint32_t)Column(col)) >> 8 & 7;
                bad2 += ShaderBits(b, k, col) != want, ++n2;
            }
    printf("fp32 model: %lu cases, %lu bad\n", n2, bad2);
    unsigned long bad3 = 0, n3 = 0;
    for (int a = -32768; a < 32768; a += 7)
        for (int s = 0; s < 64; ++s) {
            uint32_t start = (uint32_t)s * 0x9e3779b9u;
            for (int col = 0; col < 142; ++col) {
                int x = Column(col);
                unsigned want = (start + (uint32_t)a * (uint32_t)x) >> 8 & 7;
                volatile float acc = (float)start + (float)a * (float)x;
                volatile float r = acc * (1.0f / 256.0f);
                volatile float q = r * (1.0f / 8.0f);
                volatile float m = r - 8.0f * (q - Frc(q));
                bad3 += (unsigned)(m - Frc(m)) != want, ++n3;
            }
        }
    printf("negative control (unreduced): %lu cases, %lu bad (must be > 0)\n", n3, bad3);
    unsigned long bad4 = 0, n4 = 0;
    for (int br = 0; br < 16; ++br) {
        uint8_t bm[32 + 31], bmh[64];
        for (int i = 0; i < 32; ++i)
            bmh[i * 2] = bmh[i * 2 + 1] = bm[i] = (uint8_t)(((i << 3) | (i >> 2)) * br / 15);
        for (int i = 32; i < 63; ++i) bm[i] = bm[31];
        for (int half = 0; half < 2; ++half)
            for (int v = 0; v < (half ? 64 : 63); ++v) {
                unsigned want = half ? bmh[v] : bm[v];
                volatile float fv = (float)v;
                if (half) { volatile float h = fv * 0.5f; fv = h - Frc(h); }
                volatile float c = fv < 31.0f ? fv : 31.0f;
                volatile float q = c * 0.25f;
                volatile float e = c * 8.0f + (q - Frc(q));
                volatile float t = (e * (float)br + 0.5f) * (1.0f / 15.0f);
                bad4 += (unsigned)(t - Frc(t)) != want, ++n4;
            }
    }
    printf("brightness: %lu cases, %lu bad\n", n4, bad4);
    unsigned long bad5 = 0, n5 = 0;
    for (int flip = 0; flip < 2; ++flip)
        for (int a = -32768; a < 32768; ++a)
            for (int s5 = 0; s5 < 257; ++s5) {
                uint32_t start = (uint32_t)s5 * 0x9e3779b9u;
                uint32_t b = (start + (flip ? 255u * (uint32_t)a : 0)) & 0x3ffff;
                uint32_t k = (uint32_t)(flip ? -a : a) & 0x3ffff;
                float blo = (float)(b & 2047), bhi = (float)(b >> 11);
                float klo = (float)(k & 2047), khi = (float)(k >> 11);
                for (int x = -71; x <= 326; ++x) {
                    uint32_t rx = flip ? (uint32_t)(255 - x) : (uint32_t)x;
                    uint32_t xpos = start + (uint32_t)a * rx;
                    volatile float fx = (float)x;
                    volatile float A = blo + klo * fx;
                    volatile float qa = A * (1.0f / 2048.0f);
                    volatile float q = qa - Frc(qa);
                    volatile float m = A - 2048.0f * q;
                    volatile float r = m * (1.0f / 256.0f);
                    volatile float fine = r - Frc(r);
                    volatile float T = bhi + q + khi * fx;
                    volatile float tq = T * (1.0f / 128.0f);
                    volatile float tile = T - 128.0f * (tq - Frc(tq));
                    bad5 += (unsigned)fine != (xpos >> 8 & 7) || (unsigned)tile != (xpos >> 11 & 127);
                    ++n5;
                }
            }
    printf("whole line: %lu cases, %lu bad\n", n5, bad5);
    unsigned long bad6 = 0, n6 = 0, ctl6 = 0;
    for (int flip = 0; flip < 2; ++flip)
        for (int a = -32768; a < 32768; ++a)
            for (int s6 = 0; s6 < 257; ++s6) {
                uint32_t start = (uint32_t)s6 * 0x9e3779b9u;
                uint32_t b = (start + (flip ? 255u * (uint32_t)a : 0)) & 0x1fffff;
                uint32_t k = (uint32_t)(flip ? -a : a) & 0x1fffff;
                float blo = (float)(b & 2047), bhi = (float)(b >> 11);
                float klo = (float)(k & 2047), khi = (float)(k >> 11);
                float bhi18 = (float)((b & 0x3ffff) >> 11), khi18 = (float)((k & 0x3ffff) >> 11);
                for (int x = -71; x <= 326; ++x) {
                    uint32_t rx = flip ? (uint32_t)(255 - x) : (uint32_t)x;
                    uint32_t xpos = start + (uint32_t)a * rx;
                    volatile float fx = (float)x;
                    volatile float A = blo + klo * fx;
                    volatile float qa = A * (1.0f / 2048.0f);
                    volatile float q = qa - Frc(qa);
                    volatile float m = A - 2048.0f * q;
                    volatile float r = m * (1.0f / 256.0f);
                    volatile float fine = r - Frc(r);
                    volatile float T = bhi + q + khi * fx;
                    volatile float tq = T * (1.0f / 128.0f);
                    volatile float tile = T - 128.0f * (tq - Frc(tq));
                    volatile float uq = T * (1.0f / 1024.0f);
                    volatile float U = (T - 1024.0f * (uq - Frc(uq))) * 8.0f + fine;
                    bad6 += (unsigned)U != (xpos >> 8 & 8191) ||
                            (unsigned)tile != (xpos >> 11 & 127);
                    volatile float T18 = bhi18 + q + khi18 * fx;
                    volatile float uq18 = T18 * (1.0f / 1024.0f);
                    volatile float U18 = (T18 - 1024.0f * (uq18 - Frc(uq18))) * 8.0f + fine;
                    ctl6 += (unsigned)U18 != (xpos >> 8 & 8191);
                    ++n6;
                }
            }
    printf("course U: %lu cases, %lu bad | 18-bit control %lu bad (must be > 0)\n",
           n6, bad6, ctl6);
    return bad || bad2 || !bad3 || bad4 || bad5 || bad6 || !ctl6;
}
