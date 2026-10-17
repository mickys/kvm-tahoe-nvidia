// swizzle-bench.c: speed of the pre-Blackwell NVENC input rearrangement (plugin patch 0002) and faster variants.
// Every variant must produce exactly the reference output. Plain C + pthreads, so it runs on Linux and macOS.
//   cc -O2 -o swizzle-bench swizzle-bench.c -lpthread && ./swizzle-bench      (AVX2 code is per-function, like the plugin)
#include <immintrin.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#ifdef __APPLE__
#include <dispatch/dispatch.h>
#endif

static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec * 1e-9; }

// ---- v0: the patch as committed (one chunk at a time, per-chunk formula, clamp on every chunk) ----
static inline void src_of(int plane, int r, int j, int *sr, int *sc)
{
    if (plane == 0) { *sr = (r >> 1) + 4 * ((j >> 1) & 1); *sc = 8 * (r & 1) + 16 * (j & 1) + 32 * (j >> 2); }
    else            { *sr = (r & 3) + 4 * ((j >> 1) & 1);  *sc = 8 * (j & 1) + 16 * ((r >> 2) & 1) + 32 * (j >> 2); }
}
static void v0(int plane, const uint8_t *src, uint8_t *d, size_t bpr, size_t rows)
{
    size_t prow = (rows + 15) & ~(size_t)15, cols = bpr & ~(size_t)63;
    for (size_t r = 0; r < prow; r++) {
        uint8_t *drow = d + r * bpr;
        for (size_t c = 0; c < cols; c += 8) {
            int sr, sc; src_of(plane, (int)(r & 7), (int)((c & 63) >> 3), &sr, &sc);
            size_t R = (r & ~(size_t)7) + (size_t)sr; if (R >= rows) R = rows - 1;
            memcpy(drow + c, src + R * bpr + (c & ~(size_t)63) + (size_t)sc, 8);
        }
        if (cols < bpr) memcpy(drow + cols, src + (r < rows ? r : rows - 1) * bpr + cols, bpr - cols);
    }
}

// ---- shared: the slow path for the bottom band (rows past the plane are clamped) ----
static void band_slow(int plane, const uint8_t *src, uint8_t *d, size_t bpr, size_t rows, size_t r0, size_t r1)
{
    size_t cols = bpr & ~(size_t)63;
    for (size_t r = r0; r < r1; r++) {
        uint8_t *drow = d + r * bpr;
        for (size_t c = 0; c < cols; c += 8) {
            int sr, sc; src_of(plane, (int)(r & 7), (int)((c & 63) >> 3), &sr, &sc);
            size_t R = (r & ~(size_t)7) + (size_t)sr; if (R >= rows) R = rows - 1;
            memcpy(drow + c, src + R * bpr + (c & ~(size_t)63) + (size_t)sc, 8);
        }
        if (cols < bpr) memcpy(drow + cols, src + (r < rows ? r : rows - 1) * bpr + cols, bpr - cols);
    }
}

// ---- v1: whole 8-row bands, one 64-byte tile at a time, offsets from a 64-entry table ----
// Within a full band no row can be past the plane, so the clamp is gone; the table turns the formula into 64 fixed
// (source row, source column) pairs that the compiler keeps in a loop over tiles.
static void band_v1(int plane, const uint8_t *src, uint8_t *d, size_t bpr, size_t b /* band start row */)
{
    static int tab[2][64][2]; static int init;
    if (!init) { for (int p = 0; p < 2; p++) for (int r = 0; r < 8; r++) for (int j = 0; j < 8; j++) src_of(p, r, j, &tab[p][r * 8 + j][0], &tab[p][r * 8 + j][1]); init = 1; }
    size_t cols = bpr & ~(size_t)63;
    const uint8_t *s = src + b * bpr; uint8_t *o = d + b * bpr;
    size_t off[64];
    for (int k = 0; k < 64; k++) off[k] = (size_t)tab[plane][k][0] * bpr + (size_t)tab[plane][k][1];
    for (size_t t = 0; t < cols; t += 64)
        for (int r = 0; r < 8; r++)
            for (int j = 0; j < 8; j++) { uint64_t v; memcpy(&v, s + t + off[r * 8 + j], 8); memcpy(o + r * bpr + t + j * 8, &v, 8); }
    if (cols < bpr) for (int r = 0; r < 8; r++) memcpy(o + r * bpr + cols, s + r * bpr + cols, bpr - cols);
}

// ---- v2: AVX2. Each output row of a tile takes 4 chunks from each of two source rows; build it with two
// 64-bit permutes and a blend, 32 bytes at a time ----
// luma, output row r of a tile: chunks j = (j2 j1 j0), source row (r>>1) + 4*j1, source chunk index (j2 j0 r0).
//   j1 = 0 -> chunks j = 0,1,4,5 from row (r>>1):   source chunks r0, 2+r0, 4+r0, 6+r0  (every other chunk)
//   j1 = 1 -> chunks j = 2,3,6,7 from row (r>>1)+4: the same source chunks
// so output = [A0 A1 B0 B1 | A2 A3 B2 B3] with A = row (r>>1) chunks {r0,2+r0,4+r0,6+r0}, B = same from row +4.
// chroma, output row r: source row (r&3) + 4*j1, source chunk (j2 r2 j0):
//   output = [A0 A1 B0 B1 | A2 A3 B2 B3] with A = row (r&3) chunks {2*r2, 2*r2+1, 4+2*r2, 5+2*r2}, B = row +4.
__attribute__((target("avx2"))) static void band_v2(int plane, const uint8_t *src, uint8_t *d, size_t bpr, size_t b)
{
    size_t cols = bpr & ~(size_t)63;
    const uint8_t *s = src + b * bpr; uint8_t *o = d + b * bpr;
    for (size_t t = 0; t < cols; t += 64) {
        for (int r = 0; r < 8; r++) {
            const uint8_t *ra, *rb; __m256i A, B;
            if (plane == 0) {
                int r0 = r & 1; ra = s + (size_t)(r >> 1) * bpr + t; rb = ra + 4 * bpr;
                // every other chunk starting at r0: even or odd qwords of the 64-byte row
                __m256i a_lo = _mm256_loadu_si256((const __m256i *)ra), a_hi = _mm256_loadu_si256((const __m256i *)(ra + 32));
                __m256i b_lo = _mm256_loadu_si256((const __m256i *)rb), b_hi = _mm256_loadu_si256((const __m256i *)(rb + 32));
                // unpack: lo/hi qwords of each 128-bit lane; [x0 x1 | x2 x3] , [x4 x5 | x6 x7]
                __m256i ae = r0 ? _mm256_unpackhi_epi64(a_lo, a_hi) : _mm256_unpacklo_epi64(a_lo, a_hi);   // [a0|a4 a2|a6] lanes
                __m256i be = r0 ? _mm256_unpackhi_epi64(b_lo, b_hi) : _mm256_unpacklo_epi64(b_lo, b_hi);
                // ae qwords = [a(r0), a(4+r0), a(2+r0), a(6+r0)] -> want A = [a(r0), a(2+r0), a(4+r0), a(6+r0)]
                A = _mm256_permute4x64_epi64(ae, 0xD8); B = _mm256_permute4x64_epi64(be, 0xD8);
            } else {
                int r2 = (r >> 2) & 1; ra = s + (size_t)(r & 3) * bpr + t; rb = ra + 4 * bpr;
                // chunks {2r2, 2r2+1, 4+2r2, 5+2r2}: 16-byte pieces at offsets 16*r2 and 32+16*r2
                __m128i a0 = _mm_loadu_si128((const __m128i *)(ra + 16 * r2)), a1 = _mm_loadu_si128((const __m128i *)(ra + 32 + 16 * r2));
                __m128i b0 = _mm_loadu_si128((const __m128i *)(rb + 16 * r2)), b1 = _mm_loadu_si128((const __m128i *)(rb + 32 + 16 * r2));
                A = _mm256_set_m128i(a1, a0); B = _mm256_set_m128i(b1, b0);
            }
            // output row = [A0 A1 B0 B1 | A2 A3 B2 B3]: low lanes of A and B, then high lanes
            __m256i out0 = _mm256_permute2x128_si256(A, B, 0x20), out1 = _mm256_permute2x128_si256(A, B, 0x31);
            _mm256_storeu_si256((__m256i *)(o + r * bpr + t), out0);
            _mm256_storeu_si256((__m256i *)(o + r * bpr + t + 32), out1);
        }
    }
    if (cols < bpr) for (int r = 0; r < 8; r++) memcpy(o + r * bpr + cols, s + r * bpr + cols, bpr - cols);
}

typedef void (*BandFn)(int, const uint8_t *, uint8_t *, size_t, size_t);
static void run_bands(BandFn f, int plane, const uint8_t *src, uint8_t *d, size_t bpr, size_t rows, size_t b0, size_t b1)
{
    for (size_t b = b0; b < b1; b += 8) f(plane, src, d, bpr, b);
}
static void whole(BandFn f, int plane, const uint8_t *src, uint8_t *d, size_t bpr, size_t rows)
{
    size_t full = rows & ~(size_t)7, prow = (rows + 15) & ~(size_t)15;
    run_bands(f, plane, src, d, bpr, rows, 0, full);
    band_slow(plane, src, d, bpr, rows, full, prow);
}
// ---- v3: v2 on N threads, split by bands ----
typedef struct { BandFn f; int plane; const uint8_t *src; uint8_t *d; size_t bpr, rows, b0, b1; } Job;
static void *job(void *a) { Job *j = a; run_bands(j->f, j->plane, j->src, j->d, j->bpr, j->rows, j->b0, j->b1); return NULL; }
static void threaded(BandFn f, int nt, int plane, const uint8_t *src, uint8_t *d, size_t bpr, size_t rows)
{
    size_t full = rows & ~(size_t)7, prow = (rows + 15) & ~(size_t)15, bands = full / 8;
    pthread_t th[16]; Job jb[16];
    for (int i = 0; i < nt; i++) {
        jb[i] = (Job){ f, plane, src, d, bpr, rows, (bands * i / nt) * 8, (bands * (i + 1) / nt) * 8 };
        pthread_create(&th[i], NULL, job, &jb[i]);
    }
    for (int i = 0; i < nt; i++) pthread_join(th[i], NULL);
    band_slow(plane, src, d, bpr, rows, full, prow);
}


#ifdef __APPLE__
// ---- v4: v2 split over GCD's thread pool (threads are reused, unlike v3's pthread_create per frame) ----
static void gcd(BandFn f, int parts, int plane, const uint8_t *src, uint8_t *d, size_t bpr, size_t rows)
{
    size_t full = rows & ~(size_t)7, prow = (rows + 15) & ~(size_t)15, bands = full / 8;
    dispatch_apply((size_t)parts, dispatch_get_global_queue(QOS_CLASS_USER_INTERACTIVE, 0), ^(size_t i) {
        run_bands(f, plane, src, d, bpr, rows, (bands * i / parts) * 8, (bands * (i + 1) / parts) * 8); });
    band_slow(plane, src, d, bpr, rows, full, prow);
}
#endif
static void cold(void)
{
    // 4K NV12, 8 different source frames (8 x 12 MB, more than the L3), as a capture pipeline delivers them
    const int W = 3840, H = 2160, NF = 8; size_t bpr = W, rows[2] = { H, H / 2 };
    uint8_t *src[NF][2], *out[2][2];
    for (int i = 0; i < NF; i++) for (int p = 0; p < 2; p++) { src[i][p] = malloc(bpr * ((rows[p] + 15) & ~15)); for (size_t k = 0; k < bpr * rows[p]; k++) src[i][p][k] = (uint8_t)rand(); }
    for (int i = 0; i < 2; i++) for (int p = 0; p < 2; p++) out[i][p] = malloc(bpr * ((rows[p] + 15) & ~15));
    const char *names[] = { "v0 per-chunk", "v2 avx2", "v3 avx2 x4 pthreads", "v4 avx2 gcd x2", "v4 avx2 gcd x4", "v4 avx2 gcd x8" };
    for (int v = 0; v < 6; v++) {
        double tot = 0; int n = 0;
        for (int it = 0; it < 48; it++) {
            int f = it % NF; uint8_t **o = out[it & 1];
            double t = now();
            for (int p = 0; p < 2; p++) {
                if (v == 0) v0(p, src[f][p], o[p], bpr, rows[p]);
                else if (v == 1) whole(band_v2, p, src[f][p], o[p], bpr, rows[p]);
                else if (v == 2) threaded(band_v2, 4, p, src[f][p], o[p], bpr, rows[p]);
#ifdef __APPLE__
                else gcd(band_v2, v == 3 ? 2 : v == 4 ? 4 : 8, p, src[f][p], o[p], bpr, rows[p]);
#else
                else whole(band_v2, p, src[f][p], o[p], bpr, rows[p]);
#endif
            }
            t = now() - t; if (it >= 8) { tot += t; n++; }
        }
        printf("cold 4K frame (luma+chroma), %-20s %.3f ms avg\n", names[v], tot / n * 1e3);
    }
}
int main(void)
{
    if (getenv("COLD")) { cold(); return 0; }
    struct { int w, h; } sizes[] = { { 1920, 1080 }, { 3840, 2160 } };
    for (int si = 0; si < 2; si++) {
        int W = sizes[si].w, H = sizes[si].h; size_t bpr = W;   // CVPixelBuffer strides are 64-byte multiples here
        for (int plane = 0; plane < 2; plane++) {
            size_t rows = plane ? H / 2 : H, prow = (rows + 15) & ~(size_t)15;
            uint8_t *src = malloc(bpr * prow), *ref = malloc(bpr * prow), *out = malloc(bpr * prow);
            for (size_t i = 0; i < bpr * prow; i++) src[i] = (uint8_t)(rand());
            double best[5] = { 1e9, 1e9, 1e9, 1e9, 1e9 };
            for (int it = 0; it < 30; it++) {
                double t;
                t = now(); v0(plane, src, ref, bpr, rows); t = now() - t; if (t < best[0]) best[0] = t;
                memset(out, 0, bpr * prow); t = now(); whole(band_v1, plane, src, out, bpr, rows); t = now() - t; if (t < best[1]) best[1] = t;
                if (memcmp(out, ref, bpr * prow)) { printf("v1 MISMATCH plane %d\n", plane); return 1; }
                memset(out, 0, bpr * prow); t = now(); whole(band_v2, plane, src, out, bpr, rows); t = now() - t; if (t < best[2]) best[2] = t;
                if (memcmp(out, ref, bpr * prow)) { printf("v2 MISMATCH plane %d\n", plane); return 1; }
                memset(out, 0, bpr * prow); t = now(); threaded(band_v2, 4, plane, src, out, bpr, rows); t = now() - t; if (t < best[3]) best[3] = t;
                if (memcmp(out, ref, bpr * prow)) { printf("v3 MISMATCH plane %d\n", plane); return 1; }
                t = now(); memcpy(out, src, bpr * rows); t = now() - t; if (t < best[4]) best[4] = t;
            }
            printf("%dx%d %-6s %5.2f MB | v0 %.3f ms | v1 table %.3f ms | v2 avx2 %.3f ms | v3 avx2x4 %.3f ms | memcpy %.3f ms\n",
                   W, H, plane ? "chroma" : "luma", bpr * rows / 1e6, best[0] * 1e3, best[1] * 1e3, best[2] * 1e3, best[3] * 1e3, best[4] * 1e3);
            free(src); free(ref); free(out);
        }
    }
    return 0;
}
