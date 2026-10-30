/* RGB stages: light denoise, colour (highlight clip + CCM + exposure), sharpen, box downscale */
#include "color.h"

/* ---- denoise: chroma smoothing (5x5) and light luma smoothing (3x3), both edge-aware ---- */
typedef struct {
	float strength;
} Dn;

static void dn_rows(Stage *s, int y0, int y1)
{
	Dn *d = s->p;
	Stage *in = s->in[0];
	int w = s->w;
	const float k = d->strength;
	for (int y = y0; y < y1; y++) {
		const float *r[5];
		for (int l = 0; l < 5; l++) r[l] = srow(in, y + l - 2);
		float *o = srow_w(s, y);
		for (int x = 0; x < w; x++) {
			const float *p = r[2] + 3 * x;
			float Y0 = 0.25f * (p[0] + 2 * p[1] + p[2]);
			float sy = k * (0.004f + 0.03f * sqrtf(Y0 > 0 ? Y0 : 0)); /* ~ shot + read noise */
			float isy = 1.0f / (sy * sy);
			float cr = 0, cb = 0, cw = 0, ly = 0, lw = 0;
			for (int dy = -2; dy <= 2; dy++)
				for (int dx = -2; dx <= 2; dx++) {
					const float *q = r[dy + 2] + 3 * (x + dx);
					float Y = 0.25f * (q[0] + 2 * q[1] + q[2]);
					float dY = Y - Y0;
					float wgt = 1.0f / (1.0f + dY * dY * isy);
					cr += wgt * (q[0] - q[1]);
					cb += wgt * (q[2] - q[1]);
					cw += wgt;
					if (dx >= -1 && dx <= 1 && dy >= -1 && dy <= 1) {
						float ws = (dx == 0 && dy == 0) ? 1.0f : (dx == 0 || dy == 0) ? 0.5f : 0.25f;
						float wl = ws / (1.0f + 4 * dY * dY * isy);
						ly += wl * Y;
						lw += wl;
					}
				}
			cr /= cw;
			cb /= cw;
			float Y = ly / lw;
			float G = Y - 0.25f * (cr + cb);
			o[3 * x] = G + cr;
			o[3 * x + 1] = G;
			o[3 * x + 2] = G + cb;
		}
	}
}

Stage *stage_denoise(Stage *in, float strength)
{
	Dn *d = xmalloc(sizeof *d);
	d->strength = strength;
	Stage *s = stage_new("denoise", in->w, in->h, 3, 1, dn_rows, d);
	stage_add_input(s, in);
	stage_set_map(s, 1, 2);
	return s;
}

/* ---- colour ---- */
typedef struct {
	float m[9];
	float expo;
	int clip;
} Col;

static void col_rows(Stage *s, int y0, int y1)
{
	Col *c = s->p;
	Stage *in = s->in[0];
	const float *m = c->m;
	for (int y = y0; y < y1; y++) {
		const float *p = srow(in, y);
		float *o = srow_w(s, y);
		for (int x = 0; x < s->w; x++) {
			float r = p[3 * x], g = p[3 * x + 1], b = p[3 * x + 2];
			if (c->clip) { /* sensor saturation -> neutral white (gains are >= 1) */
				r = clampf(r, 0, 1);
				g = clampf(g, 0, 1);
				b = clampf(b, 0, 1);
			}
			o[3 * x] = c->expo * (m[0] * r + m[1] * g + m[2] * b);
			o[3 * x + 1] = c->expo * (m[3] * r + m[4] * g + m[5] * b);
			o[3 * x + 2] = c->expo * (m[6] * r + m[7] * g + m[8] * b);
		}
	}
}

Stage *stage_color(Stage *in, const float ccm[9], float exposure, int clip)
{
	Col *c = xmalloc(sizeof *c);
	memcpy(c->m, ccm, sizeof c->m);
	c->expo = exposure;
	c->clip = clip;
	Stage *s = stage_new("color", in->w, in->h, 3, 1, col_rows, c);
	stage_add_input(s, in);
	stage_set_map(s, 1, 0);
	return s;
}

/* ---- sharpen: unsharp mask on luminance, 3x3 ---- */
typedef struct {
	float amount;
} Sh;

static void sh_rows(Stage *s, int y0, int y1)
{
	Sh *d = s->p;
	Stage *in = s->in[0];
	for (int y = y0; y < y1; y++) {
		const float *a = srow(in, y - 1), *b = srow(in, y), *c = srow(in, y + 1);
		float *o = srow_w(s, y);
		for (int x = 0; x < s->w; x++) {
#define LUM(p, i) (0.25f * ((p)[3 * (i)] + 2 * (p)[3 * (i) + 1] + (p)[3 * (i) + 2]))
			float bl = (LUM(a, x - 1) + 2 * LUM(a, x) + LUM(a, x + 1) + 2 * (LUM(b, x - 1) + LUM(b, x + 1)) +
				    4 * LUM(b, x) + LUM(c, x - 1) + 2 * LUM(c, x) + LUM(c, x + 1)) *
				   (1.0f / 16);
			float det = d->amount * (LUM(b, x) - bl);
#undef LUM
			o[3 * x] = b[3 * x] + det;
			o[3 * x + 1] = b[3 * x + 1] + det;
			o[3 * x + 2] = b[3 * x + 2] + det;
		}
	}
}

Stage *stage_sharpen(Stage *in, float amount)
{
	Sh *d = xmalloc(sizeof *d);
	d->amount = amount;
	Stage *s = stage_new("sharpen", in->w, in->h, 3, 1, sh_rows, d);
	stage_add_input(s, in);
	stage_set_map(s, 1, 1);
	return s;
}

/* ---- box downscale by an integer factor ---- */
static void down_rows(Stage *s, int y0, int y1)
{
	int k = *(int *)s->p;
	Stage *in = s->in[0];
	int ch = s->ch;
	float inv = 1.0f / (k * k);
	for (int y = y0; y < y1; y++) {
		float *o = srow_w(s, y);
		for (int x = 0; x < s->w * ch; x++) o[x] = 0;
		for (int l = 0; l < k; l++) {
			const float *r = srow(in, y * k + l);
			for (int x = 0; x < s->w; x++)
				for (int i = 0; i < k; i++)
					for (int c = 0; c < ch; c++) o[x * ch + c] += r[(x * k + i) * ch + c];
		}
		for (int x = 0; x < s->w * ch; x++) o[x] *= inv;
	}
}

Stage *stage_downscale(Stage *in, int k)
{
	int *p = xmalloc(sizeof *p);
	*p = k;
	Stage *s = stage_new("downscale", in->w / k, in->h / k, in->ch, 1, down_rows, p);
	stage_add_input(s, in);
	stage_set_map(s, k, 0);
	return s;
}
