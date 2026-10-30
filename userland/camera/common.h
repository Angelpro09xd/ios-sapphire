/* Sapphire camera pipeline: shared declarations.
 *
 * Everything works on float rows held in per-stage ring buffers (see stage.c), so a
 * 108 MP frame is processed as a stream of row chunks and never held in memory whole.
 * Values inside the pipeline are linear and normalised: 0 = black level, 1 = sensor white.
 */
#ifndef CAM_COMMON_H
#define CAM_COMMON_H

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#define PAD 16 /* horizontal padding (pixels) on every ring row, filled phase-preserving */

/* ---- util.c ---- */
void die(const char *fmt, ...) __attribute__((noreturn, format(printf, 1, 2)));
void *xmalloc(size_t n);
void *xcalloc(size_t n, size_t m);
double now_sec(void);
long peak_rss_kb(void);
int ncpus(void);
extern int g_verbose;
void logv(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

static inline float clampf(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }
static inline int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

/* index clamp that keeps the colour phase: out-of-range coordinates are moved by whole
 * periods (period 1 = plain edge clamp). n must be >= period. */
static inline int phase_clamp(int v, int n, int period)
{
	if (v < 0) {
		if (period <= 1) return 0;
		v += ((-v + period - 1) / period) * period;
		return v < n ? v : n - 1;
	}
	if (v >= n) {
		if (period <= 1) return n - 1;
		v -= ((v - n) / period + 1) * period;
		return v >= 0 ? v : 0;
	}
	return v;
}

/* ---- thread pool (util.c) ---- */
typedef void (*range_fn)(void *ctx, int a, int b);
void pool_init(int nthreads);
int pool_threads(void);
/* run fn over [0,n) in pieces of `grain` items on all threads; returns when done */
void pool_for(int n, int grain, range_fn fn, void *ctx);

/* ---- colour filter arrays ---- */
enum { CR = 0, CG = 1, CB = 2 };
typedef struct {
	int c[2][2]; /* colour of block (bx&1, by&1): c[by&1][bx&1] */
} Cfa;
int cfa_parse(const char *s, Cfa *out); /* "rggb", "grbg", "gbrg", "bggr" */
void cfa_name(const Cfa *c, char out[5]);

/* sensor geometry: base Bayer order of the colour blocks, block size n (1 = Bayer,
 * 2 = tetracell/quad Bayer, 3 = nonacell) and the image origin inside the 2n x 2n tile */
typedef struct {
	Cfa cfa;
	int n;
	int px, py;
} Geo;
static inline int geo_color(const Geo *g, int x, int y)
{
	int X = x + g->px + 2 * g->n * 64, Y = y + g->py + 2 * g->n * 64; /* keep positive */
	return g->cfa.c[(Y / g->n) & 1][(X / g->n) & 1];
}

/* ---- sRGB ---- */
float srgb_encode(float v);
float srgb_decode(float v);

#endif
