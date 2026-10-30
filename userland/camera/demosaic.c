/* Bayer demosaic: bilinear and Malvar-He-Cutler (gradient-corrected linear, 5x5) */
#include "remosaic.h"

static void bil_rows(Stage *s, int y0, int y1)
{
	Cfa *cf = s->p;
	Stage *in = s->in[0];
	for (int y = y0; y < y1; y++) {
		const float *pm = srow(in, y - 1), *p0 = srow(in, y), *pp = srow(in, y + 1);
		float *o = srow_w(s, y);
		for (int x = 0; x < s->w; x++) {
			int c = cf->c[y & 1][x & 1];
			float C = p0[x], rgb[3];
			if (c == CG) {
				int hc = cf->c[y & 1][(x + 1) & 1];
				rgb[CG] = C;
				rgb[hc] = 0.5f * (p0[x - 1] + p0[x + 1]);
				rgb[2 - hc] = 0.5f * (pm[x] + pp[x]);
			} else {
				rgb[c] = C;
				rgb[CG] = 0.25f * (p0[x - 1] + p0[x + 1] + pm[x] + pp[x]);
				rgb[2 - c] = 0.25f * (pm[x - 1] + pm[x + 1] + pp[x - 1] + pp[x + 1]);
			}
			o[3 * x] = rgb[0];
			o[3 * x + 1] = rgb[1];
			o[3 * x + 2] = rgb[2];
		}
	}
}

static void mhc_rows(Stage *s, int y0, int y1)
{
	Cfa *cf = s->p;
	Stage *in = s->in[0];
	for (int y = y0; y < y1; y++) {
		const float *m2 = srow(in, y - 2), *m1 = srow(in, y - 1), *p0 = srow(in, y), *p1 = srow(in, y + 1),
			    *p2 = srow(in, y + 2);
		float *o = srow_w(s, y);
		for (int x = 0; x < s->w; x++) {
			int c = cf->c[y & 1][x & 1];
			float C = p0[x], rgb[3];
			float H1 = p0[x - 1] + p0[x + 1], V1 = m1[x] + p1[x];
			float H2 = p0[x - 2] + p0[x + 2], V2 = m2[x] + p2[x];
			float D = m1[x - 1] + m1[x + 1] + p1[x - 1] + p1[x + 1];
			if (c == CG) {
				int hc = cf->c[y & 1][(x + 1) & 1];
				rgb[CG] = C;
				rgb[hc] = (5 * C + 4 * H1 - H2 - D + 0.5f * V2) * 0.125f;
				rgb[2 - hc] = (5 * C + 4 * V1 - V2 - D + 0.5f * H2) * 0.125f;
			} else {
				rgb[c] = C;
				rgb[CG] = (4 * C + 2 * (H1 + V1) - (H2 + V2)) * 0.125f;
				rgb[2 - c] = (6 * C + 2 * D - 1.5f * (H2 + V2)) * 0.125f;
			}
			o[3 * x] = rgb[0];
			o[3 * x + 1] = rgb[1];
			o[3 * x + 2] = rgb[2];
		}
	}
}

static Stage *mk(const char *name, Stage *bayer, const Cfa *cfa, stage_fn fn, int ctx)
{
	Cfa *c = xmalloc(sizeof *c);
	*c = *cfa;
	Stage *s = stage_new(name, bayer->w, bayer->h, 3, 1, fn, c);
	stage_add_input(s, bayer);
	stage_set_map(s, 1, ctx);
	return s;
}

Stage *demosaic_bilinear(Stage *bayer, const Cfa *cfa) { return mk("bilinear", bayer, cfa, bil_rows, 1); }
Stage *demosaic_mhc(Stage *bayer, const Cfa *cfa) { return mk("mhc", bayer, cfa, mhc_rows, 2); }
