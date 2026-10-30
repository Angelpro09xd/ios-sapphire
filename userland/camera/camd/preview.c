/* camd preview renderer, see preview.h */
#include "preview.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

const float pv_ccm[9] = { 1.62f, -0.48f, -0.14f, -0.24f, 1.52f, -0.28f, -0.04f, -0.56f, 1.60f };

/* linear 0..1 -> sRGB byte, 4096 steps */
static uint8_t srgb_lut[4097];
static int lut_ready;

static void lut_init(void)
{
	if (lut_ready) return;
	for (int i = 0; i <= 4096; i++) {
		double v = i / 4096.0, e = v <= 0.0031308 ? 12.92 * v : 1.055 * pow(v, 1 / 2.4) - 0.055;
		int b = (int)(e * 255 + 0.5);
		srgb_lut[i] = (uint8_t)(b < 0 ? 0 : b > 255 ? 255 : b);
	}
	lut_ready = 1;
}

static inline uint8_t enc(float v)
{
	if (!(v > 0)) return 0;
	if (v >= 1) return 255;
	return srgb_lut[(int)(v * 4096.0f + 0.5f)];
}

void pv_geometry(const PvRaw *r, const PvParams *p, int *block, int *step, int *ow, int *oh)
{
	int cell = r->cell > 0 ? r->cell : 1, tile = 2 * cell;
	int longest = r->w > r->h ? r->w : r->h;
	int maxo = p->max_out > 16 ? p->max_out : 1000;
	int f = (longest + tile * maxo - 1) / (tile * maxo);
	if (f < 1) f = 1;
	int b = tile * f, s = b;
	if (cell == 1 && f == 1 && longest / 2 < maxo) s = 1; /* small Bayer frame: sliding 2x2 quads */
	int w = (r->w - b) / s + 1, h = (r->h - b) / s + 1;
	if (block) *block = b;
	if (step) *step = s;
	if (p->rotate == 90 || p->rotate == 270) {
		if (ow) *ow = h;
		if (oh) *oh = w;
	} else {
		if (ow) *ow = w;
		if (oh) *oh = h;
	}
}

/* one packed RAW10 line, pixels [0, n) */
static void unpack10(const uint8_t *s, int n, uint16_t *d)
{
	int x = 0;
	for (; x + 4 <= n; x += 4, s += 5) {
		uint8_t lo = s[4];
		d[x] = (uint16_t)((s[0] << 2) | (lo & 3));
		d[x + 1] = (uint16_t)((s[1] << 2) | ((lo >> 2) & 3));
		d[x + 2] = (uint16_t)((s[2] << 2) | ((lo >> 4) & 3));
		d[x + 3] = (uint16_t)((s[3] << 2) | ((lo >> 6) & 3));
	}
	for (int k = 0; x < n; x++, k++) d[x] = (uint16_t)((s[k] << 2) | ((s[4] >> (2 * k)) & 3));
}

/* GRBG blocks: row 0 G R, row 1 B G (colour index 0 R, 1 G, 2 B) */
static inline int grbg(int x, int y, int cell)
{
	int bx = (x / cell) & 1, by = (y / cell) & 1;
	return by == 0 ? (bx == 0 ? 1 : 0) : (bx == 0 ? 2 : 1);
}

void pv_render_rows(const uint8_t *raw, const PvRaw *r, const PvParams *p, int by0, int by1, uint8_t *dst,
		    long dst_stride, PvStats *st)
{
	lut_init();
	int B, S, ow, oh;
	pv_geometry(r, p, &B, &S, &ow, &oh);
	int W = (r->w - B) / S + 1, H = (r->h - B) / S + 1, cell = r->cell > 0 ? r->cell : 1, tile = 2 * cell;
	int npx = (W - 1) * S + B;
	uint16_t *line = malloc(sizeof(uint16_t) * (size_t)npx);
	uint32_t *acc = malloc(sizeof(uint32_t) * 3 * (size_t)W);
	if (!line || !acc) {
		free(line);
		free(acc);
		return;
	}
	/* pixels of each colour in a block (the same for every block: B is a multiple of the tile, or a
	 * 2x2 quad of a Bayer frame) */
	int cnt[3] = { 0, 0, 0 };
	for (int y = 0; y < B; y++)
		for (int x = 0; x < B; x++) cnt[grbg(x, y, cell)]++;
	float range = (float)(r->white - r->black);
	if (range < 1) range = 1;
	float inv[3], blk = (float)r->black;
	for (int c = 0; c < 3; c++) inv[c] = 1.0f / (cnt[c] * range);
	const float *m = pv_ccm;
	float g = p->gain > 0 ? p->gain : 1;
	float clipv = 0.97f;
	int spot = p->meter_x >= 0 && p->meter_y >= 0;

	for (int by = by0; by < by1 && by < H; by++) {
		memset(acc, 0, sizeof(uint32_t) * 3 * (size_t)W);
		for (int yy = 0; yy < B; yy++) {
			int y = by * S + yy;
			unpack10(raw + (size_t)r->stride * y, npx, line);
			int col[6];
			for (int k = 0; k < tile; k++) col[k] = grbg(k, y, cell);
			uint32_t *a = acc;
			if (S == B) {
				int k = 0;
				for (int bx = 0; bx < W; bx++, a += 3) {
					const uint16_t *s = line + (size_t)bx * B;
					for (int x = 0; x < B; x++) {
						a[col[k]] += s[x];
						if (++k == tile) k = 0;
					}
				}
			} else {
				for (int bx = 0; bx < W; bx++, a += 3) {
					int x0 = bx * S;
					for (int x = 0; x < B; x++) a[col[(x0 + x) % tile]] += line[x0 + x];
				}
			}
		}
		float ny = (by + 0.5f) / H;
		for (int bx = 0; bx < W; bx++) {
			const uint32_t *a = acc + 3 * bx;
			float v[3];
			int clipped = 0;
			for (int c = 0; c < 3; c++) {
				v[c] = (a[c] - blk * cnt[c]) * inv[c];
				if (v[c] < 0) v[c] = 0;
				if (v[c] >= clipv) clipped = 1;
			}
			/* statistics (linear, before white balance) */
			st->blocks += 1;
			st->gsum += v[1];
			st->gn += 1;
			int hb = (int)(v[1] * PV_HIST);
			st->hist[hb < 0 ? 0 : hb >= PV_HIST ? PV_HIST - 1 : hb]++;
			if (clipped) st->clipped += 1;
			else if (v[1] > 0.004f) {
				st->sum[0] += v[0];
				st->sum[1] += v[1];
				st->sum[2] += v[2];
				st->n += 1;
			}
			float nx = (bx + 0.5f) / W, w;
			if (spot) {
				float dx = nx - p->meter_x, dy = ny - p->meter_y;
				w = 0.05f + expf(-(dx * dx + dy * dy) / (2 * 0.08f * 0.08f));
			} else {
				float dx = nx - 0.5f, dy = ny - 0.5f;
				w = 1.0f + 2.0f * expf(-(dx * dx + dy * dy) / (2 * 0.22f * 0.22f));
			}
			st->gw += w * v[1];
			st->gwn += w;
			/* colour: WB, clip, matrix, digital gain, sRGB */
			float rr = v[0] * p->wb[0], gg = v[1] * p->wb[1], bb = v[2] * p->wb[2];
			rr = rr > 1 ? 1 : rr;
			gg = gg > 1 ? 1 : gg;
			bb = bb > 1 ? 1 : bb;
			float R = g * (m[0] * rr + m[1] * gg + m[2] * bb);
			float G = g * (m[3] * rr + m[4] * gg + m[5] * bb);
			float Bc = g * (m[6] * rr + m[7] * gg + m[8] * bb);
			int ox, oy;
			switch (p->rotate) {
			case 90: ox = H - 1 - by; oy = bx; break;
			case 180: ox = W - 1 - bx; oy = H - 1 - by; break;
			case 270: ox = by; oy = W - 1 - bx; break;
			default: ox = bx; oy = by; break;
			}
			uint8_t *o = dst + (size_t)dst_stride * oy + 4 * (size_t)ox;
			o[0] = enc(Bc);
			o[1] = enc(G);
			o[2] = enc(R);
			o[3] = 255;
		}
	}
	free(line);
	free(acc);
}

void pv_stats_merge(PvStats *into, const PvStats *from)
{
	for (int c = 0; c < 3; c++) into->sum[c] += from->sum[c];
	into->n += from->n;
	into->gw += from->gw;
	into->gwn += from->gwn;
	into->gsum += from->gsum;
	into->gn += from->gn;
	into->clipped += from->clipped;
	into->blocks += from->blocks;
	for (int i = 0; i < PV_HIST; i++) into->hist[i] += from->hist[i];
}

void pv_stats_finish(PvStats *s)
{
	s->gmean = s->gwn > 0 ? s->gw / s->gwn : 0;
	s->gplain = s->gn > 0 ? s->gsum / s->gn : 0;
	for (int c = 0; c < 3; c++) s->mean[c] = s->n > 0 ? s->sum[c] / s->n : 0;
	double total = 0, run = 0;
	for (int i = 0; i < PV_HIST; i++) total += s->hist[i];
	s->gp98 = 1;
	for (int i = 0; i < PV_HIST; i++) {
		run += s->hist[i];
		if (run >= 0.98 * total) {
			s->gp98 = (i + 1.0) / PV_HIST;
			break;
		}
	}
}
