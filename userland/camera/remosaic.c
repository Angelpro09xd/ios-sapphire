/* Remosaic of same-colour cell sensors (nonacell 3x3 = S5KHM6 full resolution, tetracell 2x2).
 *
 * The cell pattern is a Bayer pattern of n x n blocks: every R or B block has green blocks
 * on its four sides, and every green block has R blocks on one axis and B blocks on the
 * other. We interpolate a full RGB image from it and resample that to a standard Bayer
 * mosaic (for the DNG) or use it directly (for JPEG/PNG):
 *
 *  1. Green at R/B blocks ("gest" stage). Along each axis the colour difference D = G - C
 *     (C = the block's own colour) is measured just outside both block edges, with
 *     half-pixel slope corrections so that a linear ramp gives D exactly, and linearly
 *     interpolated across the block: G_h = C + D_h(x). Because C itself is sampled at every
 *     pixel of the block, luminance detail inside the block survives (for grey edges D is 0
 *     and G = C exactly). The horizontal and vertical estimates are blended with weights
 *     1/(eps + activity)^2, activity = change of D across the block + green gradients next
 *     to the block + own-colour change across the block, summed over the block's lines
 *     (edge-directed, the usual Hamilton-Adams idea at block scale).
 *  2. R and B everywhere ("rb" stage). Colour differences K = C - G are known at every
 *     pixel of the C blocks. For a missing colour, K is gathered from the nearest pixel of
 *     each C block within two blocks, weighted by 1/distance^2 and by the similarity of
 *     the green (guide) values, 1/(1 + (dG/sigma)^2) - a joint-bilateral (guided)
 *     interpolation that keeps chroma from bleeding across edges. C = G + K.
 *  3. Bayer sampling ("bayer" stage): pick the channel the output CFA wants per pixel.
 */
#include "remosaic.h"

typedef struct {
	int8_t dx, dy; /* source pixel relative to the target */
	float wd;      /* 1 / distance^2 */
} Tap;

static void tap_set(Tap *t, int dx, int dy)
{
	t->dx = dx;
	t->dy = dy;
	t->wd = 1.0f / (dx * dx + dy * dy);
}

typedef struct {
	Geo g;
	int n, w, h;
	uint8_t *cpar, *ci; /* per column (index x + PAD): block parity, index inside the block */
	uint8_t *rpar, *ri; /* per row (index y + RPAD) */
	RemosaicParams rp;
	Stage *raw, *G;
	/* chroma gather taps per block parity and position inside the block, for R and B */
	Tap rbt[2][2][3][3][2][8];
	uint8_t nrbt[2][2][3][3][2];
} Remo;

#define RPAD 64

static void remo_tables(Remo *m)
{
	int n = m->n, w = m->w, h = m->h;
	m->cpar = xmalloc(w + 2 * PAD);
	m->ci = xmalloc(w + 2 * PAD);
	m->rpar = xmalloc(h + 2 * RPAD);
	m->ri = xmalloc(h + 2 * RPAD);
	for (int x = -PAD; x < w + PAD; x++) {
		int X = x + m->g.px + 2 * n * 64;
		m->cpar[x + PAD] = (X / n) & 1;
		m->ci[x + PAD] = X % n;
	}
	for (int y = -RPAD; y < h + RPAD; y++) {
		int Y = y + m->g.py + 2 * n * 64;
		m->rpar[y + RPAD] = (Y / n) & 1;
		m->ri[y + RPAD] = Y % n;
	}
	/* nearest pixel of every block of colour c within two blocks */
	for (int py = 0; py < 2; py++)
		for (int px = 0; px < 2; px++)
			for (int j = 0; j < n; j++)
				for (int i = 0; i < n; i++)
					for (int ci = 0; ci < 2; ci++) {
						int c = ci ? CB : CR, k = 0;
						for (int dy = -2; dy <= 2; dy++)
							for (int dx = -2; dx <= 2; dx++)
								if ((dx || dy) && m->g.cfa.c[(py + dy) & 1][(px + dx) & 1] == c && k < 8) {
									int qx = dx < 0 ? dx * n + n - 1 - i : dx > 0 ? dx * n - i : 0;
									int qy = dy < 0 ? dy * n + n - 1 - j : dy > 0 ? dy * n - j : 0;
									tap_set(&m->rbt[py][px][j][i][ci][k++], qx, qy);
								}
						m->nrbt[py][px][j][i][ci] = k;
					}
}

/* ---- stage 1: green estimate ---- */
static void gest_rows(Stage *s, int y0, int y1)
{
	Remo *m = s->p;
	int n = m->n, w = m->w;
	const float inv_n = 1.0f / n, eps = m->rp.eps;
	for (int y = y0; y < y1; y++) {
		float *out = srow_w(s, y);
		int j = m->ri[y + RPAD], bpy = m->rpar[y + RPAD];
		int yb0 = y - j; /* first row of this block row */
		const float *L[3];
		for (int l = 0; l < n; l++) L[l] = srow(m->raw, yb0 + l);
		const float *me = L[j];
		const float *Gt = srow(m->raw, yb0 - 1), *Gt2 = srow(m->raw, yb0 - 2);
		const float *Gb = srow(m->raw, yb0 + n), *Gb2 = srow(m->raw, yb0 + n + 1);
		const float *Ct = L[0], *Cb = L[n - 1];
		const float *Ct1 = L[n > 1 ? 1 : 0], *Cb1 = L[n > 1 ? n - 2 : 0];
		const float sv = (j + 0.5f) * inv_n;
		for (int x = 0; x < w; x++) {
			int bpx = m->cpar[x + PAD];
			if (m->g.cfa.c[bpy][bpx] == CG) {
				out[x] = me[x];
				continue;
			}
			int i = m->ci[x + PAD], xb0 = x - i, xl = xb0 - 1, xr = xb0 + n;
			float C = me[x];
			if (m->rp.simple) { /* baseline: inverse-distance mean of the 4 nearest greens */
				float a = 1.0f / (i + 1), b = 1.0f / (n - i), c = 1.0f / (j + 1), d = 1.0f / (n - j);
				a *= a, b *= b, c *= c, d *= d;
				out[x] = (a * me[xl] + b * me[xr] + c * Gt[x] + d * Gb[x]) / (a + b + c + d);
				continue;
			}
			/* horizontal: D at the left/right block edges (half-pixel slope corrected) */
			float Dl = me[xl] - me[xb0] + 0.5f * ((me[xl] - me[xl - 1]) + (me[xb0 + (n > 1)] - me[xb0]));
			float Dr = me[xr] - me[xr - 1] - 0.5f * ((me[xr + 1] - me[xr]) + (me[xr - 1] - me[xr - 1 - (n > 1)]));
			float th = (i + 0.5f) * inv_n;
			float Gh = C + Dl + th * (Dr - Dl);
			/* vertical */
			float Dt = Gt[x] - Ct[x] + 0.5f * ((Gt[x] - Gt2[x]) + (Ct1[x] - Ct[x]));
			float Db = Gb[x] - Cb[x] - 0.5f * ((Gb2[x] - Gb[x]) + (Cb[x] - Cb1[x]));
			float Gv = C + Dt + sv * (Db - Dt);
			/* activities over the block */
			float ah = 0, av = 0;
			for (int l = 0; l < n; l++) {
				const float *r = L[l];
				float dl = r[xl] - r[xb0], dr = r[xr] - r[xr - 1];
				ah += fabsf(dl - dr) + fabsf(r[xl] - r[xl - 1]) + fabsf(r[xr + 1] - r[xr]) + fabsf(r[xr - 1] - r[xb0]);
				int xc = xb0 + l;
				float dt = Gt[xc] - Ct[xc], db = Gb[xc] - Cb[xc];
				av += fabsf(dt - db) + fabsf(Gt[xc] - Gt2[xc]) + fabsf(Gb2[xc] - Gb[xc]) + fabsf(Cb[xc] - Ct[xc]);
			}
			float wh = 1.0f / ((eps + ah) * (eps + ah)), wv = 1.0f / ((eps + av) * (eps + av));
			out[x] = (wh * Gh + wv * Gv) / (wh + wv);
		}
	}
}

/* ---- refinement: green again, guided by the block's own (true) colour ----
 * At pixels of the neighbouring green blocks the green is true and the block colour C has
 * a first estimate (from the rb stage); D = G - C_est is gathered with weights
 * 1/d^2 * 1/(1 + ((C(p) - C_est(q))/sigma)^2), so the difference comes from the side of an
 * edge that has the same colour as p. Facing lines of the four side blocks contribute all
 * their n pixels, the other green blocks within two blocks their nearest pixel. */
typedef struct {
	Remo *m;
	Stage *rgb1, *g1;
	Tap t[2][2][3][3][24];
	uint8_t nt[2][2][3][3];
} Refine;

static void g2_rows(Stage *s, int y0, int y1)
{
	Refine *f = s->p;
	Remo *m = f->m;
	int w = m->w, R = 3 * m->n;
	const float sa = m->rp.sigma_rel, sb = m->rp.sigma_abs, bl = m->rp.blend;
	const float *RW[2 * 9 + 1], *EW[2 * 9 + 1];
	for (int y = y0; y < y1; y++) {
		float *out = srow_w(s, y);
		for (int k = -R; k <= R; k++) {
			RW[k + R] = srow(m->raw, y + k);
			EW[k + R] = srow(f->rgb1, y + k);
		}
		const float *raw0 = RW[R], *g1 = srow(f->g1, y);
		int j = m->ri[y + RPAD], bpy = m->rpar[y + RPAD];
		for (int x = 0; x < w; x++) {
			int bpx = m->cpar[x + PAD], i = m->ci[x + PAD];
			int col = m->g.cfa.c[bpy][bpx];
			if (col == CG) {
				out[x] = raw0[x];
				continue;
			}
			float C = raw0[x], num = 0, den = 0;
			const Tap *t = f->t[bpy][bpx][j][i];
			int nt = f->nt[bpy][bpx][j][i];
			for (int k = 0; k < nt; k++) {
				int qx = x + t[k].dx, r = t[k].dy + R;
				float ce = EW[r][3 * qx + col], D = RW[r][qx] - ce;
				float sg = sa * (C + ce) + sb, dc = C - ce, s2 = sg * sg;
				float wgt = t[k].wd * s2 / (s2 + dc * dc);
				num += wgt * D;
				den += wgt;
			}
			out[x] = bl * g1[x] + (1 - bl) * (C + num / den);
		}
	}
}

/* ---- stage 2: R and B ---- */
static void rb_rows(Stage *s, int y0, int y1)
{
	Remo *m = s->p;
	int n = m->n, w = m->w, R = 3 * n;
	const float sa = m->rp.sigma_rel, sb = m->rp.sigma_abs;
	const int simple = m->rp.simple;
	const float *RW[2 * 9 + 1], *GW[2 * 9 + 1];
	for (int y = y0; y < y1; y++) {
		float *out = srow_w(s, y);
		for (int k = -R; k <= R; k++) {
			RW[k + R] = srow(m->raw, y + k);
			GW[k + R] = srow(m->G, y + k);
		}
		const float *raw0 = RW[R], *g0 = GW[R];
		int j = m->ri[y + RPAD], bpy = m->rpar[y + RPAD];
		for (int x = 0; x < w; x++) {
			int bpx = m->cpar[x + PAD], i = m->ci[x + PAD];
			int col = m->g.cfa.c[bpy][bpx];
			float g = g0[x];
			(void)n;
			float rgb[3];
			rgb[CG] = g;
			rgb[col] = raw0[x];
			for (int ci = 0; ci < 2; ci++) {
				int c = ci ? CB : CR;
				if (c == col) continue;
				const Tap *t = m->rbt[bpy][bpx][j][i][ci];
				int nt = m->nrbt[bpy][bpx][j][i][ci];
				float num = 0, den = 0;
				if (simple) {
					for (int k = 0; k < nt; k++) {
						num += t[k].wd * RW[t[k].dy + R][x + t[k].dx];
						den += t[k].wd;
					}
					rgb[c] = num / den;
					continue;
				}
				for (int k = 0; k < nt; k++) {
					int qx = x + t[k].dx, r = t[k].dy + R;
					float gq = GW[r][qx], K = RW[r][qx] - gq;
					float sg = sa * (g + gq) + sb, dg = g - gq, s2 = sg * sg;
					float wgt = t[k].wd * s2 / (s2 + dg * dg);
					num += wgt * K;
					den += wgt;
				}
				rgb[c] = g + num / den;
			}
			out[3 * x] = rgb[0];
			out[3 * x + 1] = rgb[1];
			out[3 * x + 2] = rgb[2];
		}
	}
}

Stage *remosaic_rgb(Stage *raw, const Geo *g, const RemosaicParams *rp)
{
	if (g->n < 2 || g->n > 3) die("remosaic: cell size %d not supported", g->n);
	Remo *m = xcalloc(1, sizeof *m);
	m->g = *g;
	m->n = g->n;
	m->w = raw->w;
	m->h = raw->h;
	m->rp = *rp;
	m->raw = raw;
	remo_tables(m);
	Stage *G = stage_new("gest", raw->w, raw->h, 1, 2 * g->n, gest_rows, m);
	stage_add_input(G, raw);
	stage_set_map(G, 1, g->n + 2);
	m->G = G;
	Stage *rgb = stage_new("remosaic", raw->w, raw->h, 3, 1, rb_rows, m);
	stage_add_input(rgb, raw);
	stage_add_input(rgb, G);
	stage_set_map(rgb, 1, 3 * g->n);
	for (int it = 0; it < (rp->simple ? 0 : rp->iter); it++) {
		Refine *f = xcalloc(1, sizeof *f);
		f->m = m;
		f->rgb1 = rgb;
		f->g1 = m->G;
		/* green taps: the facing line (n pixels) of the four side blocks, the nearest pixel
		 * of the other green blocks within two blocks */
		int n = g->n;
		for (int py = 0; py < 2; py++)
			for (int px = 0; px < 2; px++)
				for (int j = 0; j < n; j++)
					for (int i = 0; i < n; i++) {
						int k = 0;
						for (int dy = -2; dy <= 2; dy++)
							for (int dx = -2; dx <= 2; dx++) {
								if (!(dx || dy) || g->cfa.c[(py + dy) & 1][(px + dx) & 1] != CG) continue;
								if (abs(dx) + abs(dy) == 1) {
									for (int l = 0; l < n && k < 24; l++) {
										if (dy == 0) tap_set(&f->t[py][px][j][i][k++], dx < 0 ? -i - 1 : n - i, l - j);
										else tap_set(&f->t[py][px][j][i][k++], l - i, dy < 0 ? -j - 1 : n - j);
									}
								} else if (k < 24) {
									int qx = dx < 0 ? dx * n + n - 1 - i : dx > 0 ? dx * n - i : 0;
									int qy = dy < 0 ? dy * n + n - 1 - j : dy > 0 ? dy * n - j : 0;
									tap_set(&f->t[py][px][j][i][k++], qx, qy);
								}
							}
						f->nt[py][px][j][i] = k;
					}
		Stage *G2 = stage_new("refine-g", raw->w, raw->h, 1, 2 * g->n, g2_rows, f);
		stage_add_input(G2, raw);
		stage_add_input(G2, rgb);
		stage_add_input(G2, f->g1);
		stage_set_map(G2, 1, 3 * g->n);
		Remo *m2 = xmalloc(sizeof *m2);
		*m2 = *m;
		m2->G = G2;
		m = m2; /* a further pass blends with this one */
		rgb = stage_new("refine-rb", raw->w, raw->h, 3, 1, rb_rows, m2);
		stage_add_input(rgb, raw);
		stage_add_input(rgb, G2);
		stage_set_map(rgb, 1, 3 * g->n);
	}
	return rgb;
}

/* ---- RGB -> Bayer ---- */
static void sample_rows(Stage *s, int y0, int y1)
{
	Cfa *c = s->p;
	Stage *in = s->in[0];
	for (int y = y0; y < y1; y++) {
		const float *r = srow(in, y);
		float *o = srow_w(s, y);
		int c0 = c->c[y & 1][0], c1 = c->c[y & 1][1];
		for (int x = 0; x + 1 < s->w; x += 2) {
			o[x] = r[3 * x + c0];
			o[x + 1] = r[3 * x + 3 + c1];
		}
		if (s->w & 1) o[s->w - 1] = r[3 * (s->w - 1) + c0];
	}
}

Stage *mosaic_sample(Stage *rgb, const Cfa *out)
{
	Cfa *c = xmalloc(sizeof *c);
	*c = *out;
	Stage *s = stage_new("bayer", rgb->w, rgb->h, 1, 2, sample_rows, c);
	stage_add_input(s, rgb);
	stage_set_map(s, 1, 0);
	return s;
}

/* ---- binning (preview) ---- */
typedef struct {
	int n, sx, sy;
} Bin;

static void bin_rows(Stage *s, int y0, int y1)
{
	Bin *b = s->p;
	Stage *in = s->in[0];
	int n = b->n;
	float inv = 1.0f / (n * n);
	for (int y = y0; y < y1; y++) {
		float *o = srow_w(s, y);
		const float *r[3];
		for (int l = 0; l < n; l++) r[l] = srow(in, b->sy + y * n + l);
		for (int X = 0; X < s->w; X++) {
			int x = b->sx + X * n;
			float a = 0;
			for (int l = 0; l < n; l++)
				for (int k = 0; k < n; k++) a += r[l][x + k];
			o[X] = a * inv;
		}
	}
}

Stage *cell_bin(Stage *raw, const Geo *g, Cfa *out_cfa)
{
	Bin *b = xmalloc(sizeof *b);
	int n = g->n;
	b->n = n;
	b->sx = (n - g->px % n) % n;
	b->sy = (n - g->py % n) % n;
	for (int j = 0; j < 2; j++)
		for (int i = 0; i < 2; i++) out_cfa->c[j][i] = geo_color(g, b->sx + i * n, b->sy + j * n);
	Stage *s = stage_new("bin", (raw->w - b->sx) / n, (raw->h - b->sy) / n, 1, 2, bin_rows, b);
	stage_add_input(s, raw);
	stage_set_map(s, n, n);
	return s;
}

Cfa geo_bayer_cfa(const Geo *g)
{
	Cfa c;
	for (int j = 0; j < 2; j++)
		for (int i = 0; i < 2; i++) c.c[j][i] = geo_color(g, i, j);
	return c;
}
