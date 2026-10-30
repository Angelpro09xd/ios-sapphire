/* rawsynth: test images and simulated sensor dumps for rawtool.
 *
 *   rawsynth pattern --kind K --size WxH -o out.ppm|out.png [--bits 8|16]
 *   rawsynth sensor (--in IMG | --kind K) --size WxH [--crop X,Y,W,H] --cfa grbg --cell 3
 *            [--phase X,Y] [--black 64 --white 1023] [--fmt raw10|raw16] [--stride N | --align A]
 *            [--sens R,G,B] [--expo E] [--noise READ_DN] [--shot K] [--seed S]
 *            -o out.raw [--ref ref.ppm] [--ref-bayer ref.pgm --ref-cfa rggb]
 *
 * Kinds: bars gradient zone star text natural mix. The sensor model: reference image (sRGB
 * values) -> linear -> times channel sensitivity (--sens, simulates the colour cast that WB
 * must remove) -> colour of the cell pattern at each pixel -> black + (white-black) * value
 * + Gaussian read/shot noise -> 10-bit -> MIPI RAW10 rows with the given stride. --ref
 * writes the reference (16-bit sRGB) that an ideal pipeline would reproduce with
 * --wb 1/sens and identity CCM; --ref-bayer the ideal Bayer mosaic (linear * sens).
 */
#include "common.h"
#include "imgread.h"
#include "rawio.h"

/* ---- 5x7 font ---- */
static const uint8_t font_digits[10][7] = {
	{ 0x0E, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0E }, { 0x04, 0x0C, 0x04, 0x04, 0x04, 0x04, 0x0E },
	{ 0x0E, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1F }, { 0x1F, 0x02, 0x04, 0x02, 0x01, 0x11, 0x0E },
	{ 0x02, 0x06, 0x0A, 0x12, 0x1F, 0x02, 0x02 }, { 0x1F, 0x10, 0x1E, 0x01, 0x01, 0x11, 0x0E },
	{ 0x06, 0x08, 0x10, 0x1E, 0x11, 0x11, 0x0E }, { 0x1F, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08 },
	{ 0x0E, 0x11, 0x11, 0x0E, 0x11, 0x11, 0x0E }, { 0x0E, 0x11, 0x11, 0x0F, 0x01, 0x02, 0x0C },
};
static const uint8_t font_alpha[26][7] = {
	{ 0x0E, 0x11, 0x11, 0x11, 0x1F, 0x11, 0x11 }, { 0x1E, 0x11, 0x11, 0x1E, 0x11, 0x11, 0x1E },
	{ 0x0E, 0x11, 0x10, 0x10, 0x10, 0x11, 0x0E }, { 0x1C, 0x12, 0x11, 0x11, 0x11, 0x12, 0x1C },
	{ 0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x1F }, { 0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x10 },
	{ 0x0E, 0x11, 0x10, 0x17, 0x11, 0x11, 0x0F }, { 0x11, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11 },
	{ 0x0E, 0x04, 0x04, 0x04, 0x04, 0x04, 0x0E }, { 0x07, 0x02, 0x02, 0x02, 0x02, 0x12, 0x0C },
	{ 0x11, 0x12, 0x14, 0x18, 0x14, 0x12, 0x11 }, { 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x1F },
	{ 0x11, 0x1B, 0x15, 0x15, 0x11, 0x11, 0x11 }, { 0x11, 0x11, 0x19, 0x15, 0x13, 0x11, 0x11 },
	{ 0x0E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E }, { 0x1E, 0x11, 0x11, 0x1E, 0x10, 0x10, 0x10 },
	{ 0x0E, 0x11, 0x11, 0x11, 0x15, 0x12, 0x0D }, { 0x1E, 0x11, 0x11, 0x1E, 0x14, 0x12, 0x11 },
	{ 0x0F, 0x10, 0x10, 0x0E, 0x01, 0x01, 0x1E }, { 0x1F, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04 },
	{ 0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E }, { 0x11, 0x11, 0x11, 0x11, 0x11, 0x0A, 0x04 },
	{ 0x11, 0x11, 0x11, 0x15, 0x15, 0x15, 0x0A }, { 0x11, 0x11, 0x0A, 0x04, 0x0A, 0x11, 0x11 },
	{ 0x11, 0x11, 0x11, 0x0A, 0x04, 0x04, 0x04 }, { 0x1F, 0x01, 0x02, 0x04, 0x08, 0x10, 0x1F },
};

static int glyph_bit(char ch, int gx, int gy)
{
	if (gx < 0 || gx > 4 || gy < 0 || gy > 6) return 0;
	const uint8_t *g = NULL;
	if (ch >= '0' && ch <= '9') g = font_digits[ch - '0'];
	else if (ch >= 'A' && ch <= 'Z') g = font_alpha[ch - 'A'];
	else if (ch == '-') return gy == 3;
	else if (ch == '.') return gy >= 5 && gx >= 1 && gx <= 2;
	if (!g) return 0;
	return (g[gy] >> (4 - gx)) & 1;
}

/* ---- noise helpers ---- */
static inline uint32_t hash3(int x, int y, int s)
{
	uint32_t h = (uint32_t)x * 374761393u + (uint32_t)y * 668265263u + (uint32_t)s * 2246822519u;
	h = (h ^ (h >> 13)) * 1274126177u;
	return h ^ (h >> 16);
}
static inline float hf(int x, int y, int s) { return (hash3(x, y, s) & 0xffffff) / 16777215.0f; }

static float vnoise(float x, float y, int s)
{
	int ix = (int)floorf(x), iy = (int)floorf(y);
	float fx = x - ix, fy = y - iy;
	fx = fx * fx * (3 - 2 * fx);
	fy = fy * fy * (3 - 2 * fy);
	float a = hf(ix, iy, s), b = hf(ix + 1, iy, s), c = hf(ix, iy + 1, s), d = hf(ix + 1, iy + 1, s);
	return a + (b - a) * fx + (c - a) * fy + (a - b - c + d) * fx * fy;
}

static float fbm(float x, float y, float period, float minp, int s)
{
	float v = 0, amp = 1, tot = 0;
	for (float p = period; p >= minp; p *= 0.5f, amp *= 0.6f, s += 17) {
		v += amp * vnoise(x / p, y / p, s);
		tot += amp;
	}
	return v / tot;
}

static void hsv(float h, float s, float v, float *o)
{
	h = (h - floorf(h)) * 6;
	int i = (int)h;
	float f = h - i, p = v * (1 - s), q = v * (1 - s * f), t = v * (1 - s * (1 - f));
	float r[6][3] = { { v, t, p }, { q, v, p }, { p, v, t }, { p, q, v }, { t, p, v }, { v, p, q } };
	o[0] = r[i % 6][0], o[1] = r[i % 6][1], o[2] = r[i % 6][2];
}

/* ---- patterns: (x,y) inside a W x H tile -> sRGB 0..1 ---- */
enum { K_BARS, K_GRAD, K_ZONE, K_STAR, K_TEXT, K_NAT, K_MIX };

static void pattern(int kind, int x, int y, int W, int H, int seed, float *o)
{
	switch (kind) {
	case K_BARS: {
		static const float bars[8][3] = { { .9f, .9f, .9f }, { .9f, .9f, .1f }, { .1f, .9f, .9f }, { .1f, .9f, .1f },
						  { .9f, .1f, .9f }, { .9f, .1f, .1f }, { .1f, .1f, .9f }, { .1f, .1f, .1f } };
		if (y < H * 6 / 10) {
			int b = x * 8 / W;
			memcpy(o, bars[b], 3 * sizeof(float));
		} else if (y < H * 8 / 10) {
			o[0] = o[1] = o[2] = (float)x / (W - 1);
		} else {
			/* fine vertical stripes of decreasing period, then colour patches */
			int seg = x * 6 / W;
			if (seg < 3) {
				int per = 8 >> seg; /* 8, 4, 2 px */
				float v = ((x / (per / 2 ? per / 2 : 1)) & 1) ? 0.85f : 0.15f;
				o[0] = o[1] = o[2] = v;
			} else {
				static const float pc[3][3] = { { .7f, .35f, .25f }, { .3f, .45f, .7f }, { .35f, .55f, .25f } };
				memcpy(o, pc[seg - 3], 3 * sizeof(float));
			}
		}
		break;
	}
	case K_GRAD: {
		float s = y < H / 2 ? 1.0f : 1.0f - (float)(y - H / 2) / (H / 2) * 0.9f;
		float v = y < H / 2 ? 0.25f + 0.7f * y / (H / 2) : 0.95f;
		hsv((float)x / W, s, v, o);
		break;
	}
	case K_ZONE: {
		float cx = W / 2.0f, cy = H / 2.0f, R = fminf(cx, cy);
		float dx = x + 0.5f - cx, dy = y + 0.5f - cy, r2 = dx * dx + dy * dy;
		float a = (float)M_PI * 0.35f / R; /* local frequency 0.35 cycles/px at radius R */
		float v = 0.5f + 0.4f * cosf(a * r2);
		o[0] = o[1] = o[2] = v;
		break;
	}
	case K_STAR: {
		float dx = x + 0.5f - W / 2.0f, dy = y + 0.5f - H / 2.0f;
		float t = atan2f(dy, dx);
		float v = 0.5f + 0.42f * sinf(36 * t);
		float r = sqrtf(dx * dx + dy * dy) / fminf(W, H) * 2;
		/* coloured outer ring: red/cyan spokes */
		if (r > 0.75f) {
			o[0] = 0.15f + 0.7f * v;
			o[1] = o[2] = 0.85f - 0.7f * v;
		} else o[0] = o[1] = o[2] = v;
		break;
	}
	case K_TEXT: {
		static const char *txt = "THE QUICK BROWN FOX JUMPS OVER THE LAZY DOG 0123456789 - SAPPHIRE 108MP. ";
		static const float sch[4][2][3] = { { { .05f, .05f, .05f }, { .95f, .95f, .95f } },
						   { { .95f, .95f, .95f }, { .08f, .08f, .08f } },
						   { { .85f, .1f, .1f }, { .1f, .7f, .2f } },
						   { { .1f, .15f, .8f }, { .9f, .85f, .2f } } };
		/* bands of growing scale: 1..6, then repeat */
		int yy = y, band = 0, sc = 1;
		for (;;) {
			sc = band % 6 + 1;
			int bh = 9 * sc;
			if (yy < bh) break;
			yy -= bh;
			band++;
		}
		int s = band % 4, cw = 6 * sc;
		int ci = (x / cw) % (int)strlen(txt);
		int on = glyph_bit(txt[ci], (x % cw) / sc, (yy - sc) / sc) && yy >= sc;
		memcpy(o, sch[s][on ? 0 : 1], 3 * sizeof(float));
		break;
	}
	case K_NAT: {
		float base[3];
		float h = fbm(x, y, 400, 100, seed), sat = 0.25f + 0.5f * fbm(x, y, 300, 150, seed + 5);
		float lum = 0.15f + 0.7f * fbm(x, y, 256, 2, seed + 9);
		hsv(h * 1.5f, sat, 1, base);
		for (int c = 0; c < 3; c++) o[c] = base[c] * lum;
		/* sharp-edged discs */
		int cell = 160, gx = x / cell, gy = y / cell;
		for (int j = -1; j <= 1; j++)
			for (int i = -1; i <= 1; i++) {
				int cx0 = (gx + i) * cell, cy0 = (gy + j) * cell;
				float ccx = cx0 + hf(gx + i, gy + j, seed + 21) * cell, ccy = cy0 + hf(gx + i, gy + j, seed + 22) * cell;
				float rr = 15 + 50 * hf(gx + i, gy + j, seed + 23);
				float dx = x + 0.5f - ccx, dy = y + 0.5f - ccy;
				if (hf(gx + i, gy + j, seed + 24) < 0.5f && dx * dx + dy * dy < rr * rr) {
					float c[3];
					hsv(hf(gx + i, gy + j, seed + 25), 0.8f, 0.2f + 0.7f * hf(gx + i, gy + j, seed + 26), c);
					float tex = 0.85f + 0.15f * vnoise(x / 3.0f, y / 3.0f, seed + 27);
					for (int k = 0; k < 3; k++) o[k] = c[k] * tex;
				}
			}
		break;
	}
	case K_MIX: {
		int tw = W / 3, th = H / 2;
		int tx = x / tw, ty = y / th;
		if (tx > 2) tx = 2;
		if (ty > 1) ty = 1;
		static const int kinds[2][3] = { { K_BARS, K_GRAD, K_ZONE }, { K_TEXT, K_NAT, K_STAR } };
		int lw = tx == 2 ? W - 2 * tw : tw, lh = ty == 1 ? H - th : th;
		pattern(kinds[ty][tx], x - tx * tw, y - ty * th, lw, lh, seed, o);
		break;
	}
	}
}

static int kind_parse(const char *s)
{
	static const char *names[] = { "bars", "gradient", "zone", "star", "text", "natural", "mix" };
	for (int i = 0; i < 7; i++)
		if (!strcmp(s, names[i])) return i;
	die("unknown kind %s", s);
}

/* ---- source image (pattern or resampled file) ---- */
typedef struct {
	int kind, seed, W, H;
	Image im;
	int cx, cy, cw, ch; /* crop in the image */
} Src;

static void src_row(const Src *s, int y, float *o)
{
	if (s->kind >= 0) {
		for (int x = 0; x < s->W; x++) pattern(s->kind, x, y, s->W, s->H, s->seed, o + 3 * x);
		return;
	}
	float sx = (float)s->cw / s->W, sy = (float)s->ch / s->H;
	int kx = sx > 1 ? (int)ceilf(sx) : 1, ky = sy > 1 ? (int)ceilf(sy) : 1;
	for (int x = 0; x < s->W; x++) {
		float acc[3] = { 0 };
		for (int j = 0; j < ky; j++)
			for (int i = 0; i < kx; i++) {
				float u = s->cx + (x + (i + 0.5f) / kx) * sx - 0.5f, v = s->cy + (y + (j + 0.5f) / ky) * sy - 0.5f;
				int x0 = (int)floorf(u), y0 = (int)floorf(v);
				float fx = u - x0, fy = v - y0;
				int xa = clampi(x0, 0, s->im.w - 1), xb = clampi(x0 + 1, 0, s->im.w - 1);
				int ya = clampi(y0, 0, s->im.h - 1), yb = clampi(y0 + 1, 0, s->im.h - 1);
				const float *p00 = s->im.px + 3 * ((size_t)ya * s->im.w + xa), *p01 = s->im.px + 3 * ((size_t)ya * s->im.w + xb);
				const float *p10 = s->im.px + 3 * ((size_t)yb * s->im.w + xa), *p11 = s->im.px + 3 * ((size_t)yb * s->im.w + xb);
				for (int c = 0; c < 3; c++)
					acc[c] += (p00[c] * (1 - fx) + p01[c] * fx) * (1 - fy) + (p10[c] * (1 - fx) + p11[c] * fx) * fy;
			}
		for (int c = 0; c < 3; c++) o[3 * x + c] = acc[c] / (kx * ky);
	}
}

/* ---- output helpers ---- */
static void put16(uint8_t *d, float v)
{
	unsigned q = (unsigned)(clampf(v, 0, 1) * 65535.0f + 0.5f);
	d[0] = q >> 8;
	d[1] = q & 255;
}

typedef struct {
	Src *src;
	int y0;
	float *rows; /* 3*W per row */
} GenCtx;

static void gen_rows(void *vc, int a, int b)
{
	GenCtx *g = vc;
	for (int k = a; k < b; k++) src_row(g->src, g->y0 + k, g->rows + (size_t)3 * g->src->W * k);
}

static void usage(void)
{
	puts("usage: rawsynth pattern --kind K --size WxH -o out.ppm|.png [--bits 8|16]\n"
	     "       rawsynth sensor (--in IMG | --kind K) --size WxH [--crop X,Y,W,H] --cfa grbg --cell 3 [--phase X,Y]\n"
	     "                [--black 64] [--white 1023] [--fmt raw10|raw16] [--stride N | --align A] [--sens R,G,B]\n"
	     "                [--expo E] [--noise READ_DN] [--shot K] [--seed S] -o out.raw [--ref ref.ppm]\n"
	     "                [--ref-bayer ref.pgm --ref-cfa rggb]\n"
	     "kinds: bars gradient zone star text natural mix");
	exit(2);
}

int main(int argc, char **argv)
{
	if (argc < 2) usage();
	int mode_sensor = !strcmp(argv[1], "sensor");
	if (!mode_sensor && strcmp(argv[1], "pattern")) usage();
	Src s;
	memset(&s, 0, sizeof s);
	s.kind = K_MIX;
	s.seed = 1;
	const char *in = NULL, *out = NULL, *ref = NULL, *refb = NULL;
	int bits = 8, crop = 0, fmt = FMT_RAW10, align = 1;
	long stride = 0;
	Geo g = { .n = 3 };
	cfa_parse("grbg", &g.cfa);
	Cfa rcfa;
	cfa_parse("rggb", &rcfa);
	float black = 64, white = 1023, sens[3] = { 0.55f, 1.0f, 0.7f }, expo = 1, noise = 0, shot = 0;
	for (int i = 2; i < argc; i++) {
		const char *a = argv[i];
#define VAL() (i + 1 < argc ? argv[++i] : (die("%s needs a value", a), ""))
		if (!strcmp(a, "--kind")) s.kind = kind_parse(VAL());
		else if (!strcmp(a, "--in")) in = VAL();
		else if (!strcmp(a, "--size")) {
			if (sscanf(VAL(), "%dx%d", &s.W, &s.H) != 2) die("bad --size");
		} else if (!strcmp(a, "--crop")) {
			if (sscanf(VAL(), "%d,%d,%d,%d", &s.cx, &s.cy, &s.cw, &s.ch) != 4) die("bad --crop");
			crop = 1;
		} else if (!strcmp(a, "-o")) out = VAL();
		else if (!strcmp(a, "--bits")) bits = atoi(VAL());
		else if (!strcmp(a, "--seed")) s.seed = atoi(VAL());
		else if (!strcmp(a, "--cfa")) {
			if (cfa_parse(VAL(), &g.cfa)) die("bad --cfa");
		} else if (!strcmp(a, "--cell")) g.n = atoi(VAL());
		else if (!strcmp(a, "--phase")) {
			if (sscanf(VAL(), "%d,%d", &g.px, &g.py) != 2) die("bad --phase");
		} else if (!strcmp(a, "--black")) black = atof(VAL());
		else if (!strcmp(a, "--white")) white = atof(VAL());
		else if (!strcmp(a, "--fmt")) {
			const char *f = VAL();
			fmt = !strcmp(f, "raw10") ? FMT_RAW10 : !strcmp(f, "raw16") ? FMT_RAW16 : (die("--fmt raw10|raw16"), 0);
		} else if (!strcmp(a, "--stride")) stride = atol(VAL());
		else if (!strcmp(a, "--align")) align = atoi(VAL());
		else if (!strcmp(a, "--sens")) {
			if (sscanf(VAL(), "%f,%f,%f", &sens[0], &sens[1], &sens[2]) != 3) die("bad --sens");
		} else if (!strcmp(a, "--expo")) expo = atof(VAL());
		else if (!strcmp(a, "--noise")) noise = atof(VAL());
		else if (!strcmp(a, "--shot")) shot = atof(VAL());
		else if (!strcmp(a, "--ref")) ref = VAL();
		else if (!strcmp(a, "--ref-bayer")) refb = VAL();
		else if (!strcmp(a, "--ref-cfa")) {
			if (cfa_parse(VAL(), &rcfa)) die("bad --ref-cfa");
		} else usage();
#undef VAL
	}
	if (!out) usage();
	if (in) {
		s.kind = -1;
		image_load(&s.im, in);
		if (!crop) s.cx = 0, s.cy = 0, s.cw = s.im.w, s.ch = s.im.h;
		if (!s.W) s.W = s.cw, s.H = s.ch;
	}
	if (s.W <= 0 || s.H <= 0) die("--size needed");
	if (g.n < 1 || g.n > 3) die("--cell 1..3");
	pool_init(ncpus());

	int W = s.W, H = s.H, chunk = 64;
	float *rows = xmalloc(sizeof(float) * 3 * W * chunk);
	double t0 = now_sec();

	if (!mode_sensor) {
		/* .png -> PNG with stored deflate blocks (one IDAT per row), else binary PPM */
		int png = strlen(out) > 4 && !strcmp(out + strlen(out) - 4, ".png");
		FILE *f = fopen(out, "wb");
		if (!f) die("cannot create %s", out);
		int B = bits == 16 ? 2 : 1;
		uint8_t *line = xmalloc((size_t)W * 3 * B + 1);
		uint32_t crc_tab[256];
		for (uint32_t n = 0; n < 256; n++) {
			uint32_t c = n;
			for (int k = 0; k < 8; k++) c = c & 1 ? 0xedb88320u ^ (c >> 1) : c >> 1;
			crc_tab[n] = c;
		}
		uint32_t ad_a = 1, ad_b = 0;
		long long remaining = (long long)H * (1 + (long long)W * 3 * B);
		int first = 1;
		if (png) {
			uint8_t hd[8 + 25] = { 137, 80, 78, 71, 13, 10, 26, 10, 0, 0, 0, 13, 'I', 'H', 'D', 'R' };
			uint8_t *ih = hd + 16;
			ih[0] = W >> 24, ih[1] = W >> 16, ih[2] = W >> 8, ih[3] = W;
			ih[4] = H >> 24, ih[5] = H >> 16, ih[6] = H >> 8, ih[7] = H;
			ih[8] = bits == 16 ? 16 : 8, ih[9] = 2, ih[10] = ih[11] = ih[12] = 0;
			uint32_t c = 0xffffffffu;
			for (int k = 12; k < 29; k++) c = crc_tab[(c ^ hd[k]) & 255] ^ (c >> 8);
			c ^= 0xffffffffu;
			hd[29] = c >> 24, hd[30] = c >> 16, hd[31] = c >> 8, hd[32] = c;
			fwrite(hd, 1, 33, f);
		} else fprintf(f, "P6\n%d %d\n%d\n", W, H, bits == 16 ? 65535 : 255);
		for (int y0 = 0; y0 < H; y0 += chunk) {
			int n = H - y0 < chunk ? H - y0 : chunk;
			GenCtx gc = { &s, y0, rows };
			pool_for(n, 1, gen_rows, &gc);
			for (int k = 0; k < n; k++) {
				const float *r = rows + (size_t)3 * W * k;
				uint8_t *d = line;
				if (png) *d++ = 0;
				for (int x = 0; x < 3 * W; x++) {
					if (B == 2) put16(d + 2 * x, r[x]);
					else d[x] = (uint8_t)(clampf(r[x], 0, 1) * 255 + 0.5f);
				}
				size_t len = (size_t)W * 3 * B + (png ? 1 : 0);
				if (!png) {
					fwrite(line, 1, len, f);
					continue;
				}
				/* one IDAT per row: zlib header (first), stored block(s), adler (last) */
				for (size_t i = 0; i < len; i++) {
					ad_a = (ad_a + line[i]) % 65521;
					ad_b = (ad_b + ad_a) % 65521;
				}
				remaining -= len;
				size_t zcap = len + (len / 65535 + 1) * 5 + 8;
				uint8_t *z = xmalloc(zcap + 8), *zp = z + 8;
				size_t zl = 0;
				if (first) zp[zl++] = 0x78, zp[zl++] = 0x01, first = 0;
				for (size_t i = 0; i < len;) {
					size_t m = len - i < 65535 ? len - i : 65535;
					zp[zl++] = (i + m == len) && remaining == 0;
					zp[zl++] = m & 255, zp[zl++] = m >> 8, zp[zl++] = ~m & 255, zp[zl++] = (~m >> 8) & 255;
					memcpy(zp + zl, line + i, m);
					zl += m;
					i += m;
				}
				if (remaining == 0) {
					uint32_t ad = (ad_b << 16) | ad_a;
					zp[zl++] = ad >> 24, zp[zl++] = ad >> 16, zp[zl++] = ad >> 8, zp[zl++] = ad;
				}
				z[0] = zl >> 24, z[1] = zl >> 16, z[2] = zl >> 8, z[3] = zl;
				memcpy(z + 4, "IDAT", 4);
				uint32_t c = 0xffffffffu;
				for (size_t k2 = 4; k2 < zl + 8; k2++) c = crc_tab[(c ^ z[k2]) & 255] ^ (c >> 8);
				c ^= 0xffffffffu;
				fwrite(z, 1, zl + 8, f);
				uint8_t cb[4] = { c >> 24, c >> 16, c >> 8, c };
				fwrite(cb, 1, 4, f);
				free(z);
			}
		}
		if (png) {
			static const uint8_t iend[12] = { 0, 0, 0, 0, 'I', 'E', 'N', 'D', 0xae, 0x42, 0x60, 0x82 };
			fwrite(iend, 1, 12, f);
		}
		if (fclose(f)) die("write error");
		fprintf(stderr, "%s: %dx%d in %.2f s\n", out, W, H, now_sec() - t0);
		return 0;
	}

	/* ---- sensor simulation ---- */
	long minst = raw_min_stride(fmt, W);
	if (!stride) stride = (minst + align - 1) / align * align;
	if (stride < minst) die("stride too small (min %ld)", minst);
	FILE *fo = fopen(out, "wb"), *fr = NULL, *fb = NULL;
	if (!fo) die("cannot create %s", out);
	if (ref) {
		fr = fopen(ref, "wb");
		if (!fr) die("cannot create %s", ref);
		fprintf(fr, "P6\n%d %d\n65535\n", W, H);
	}
	if (refb) {
		fb = fopen(refb, "wb");
		if (!fb) die("cannot create %s", refb);
		fprintf(fb, "P5\n%d %d\n65535\n", W, H);
	}
	uint8_t *line = xcalloc(stride > 6L * W ? stride : 6L * W, 1);
	uint16_t *v16 = xmalloc(sizeof(uint16_t) * W);
	float lut[4096];
	for (int i = 0; i < 4096; i++) lut[i] = srgb_decode(i / 4095.0f);
	int maxcode = fmt == FMT_RAW10 ? 1023 : 65535;
	for (int y0 = 0; y0 < H; y0 += chunk) {
		int n = H - y0 < chunk ? H - y0 : chunk;
		GenCtx gc = { &s, y0, rows };
		pool_for(n, 1, gen_rows, &gc);
		for (int k = 0; k < n; k++) {
			int y = y0 + k;
			const float *r = rows + (size_t)3 * W * k;
			uint64_t st = hash3(y, s.seed, 99) | 1;
			for (int x = 0; x < W; x++) {
				int c = geo_color(&g, x, y);
				float sv = clampf(r[3 * x + c], 0, 1);
				float lin = lut[(int)(sv * 4095 + 0.5f)] * sens[c] * expo;
				float dn = lin * (white - black);
				if (noise > 0 || shot > 0) {
					/* xorshift64 + Box-Muller */
					st ^= st << 13, st ^= st >> 7, st ^= st << 17;
					float u1 = ((st >> 11) + 1) * (1.0f / 9007199254740993.0f);
					st ^= st << 13, st ^= st >> 7, st ^= st << 17;
					float u2 = (st >> 11) * (1.0f / 9007199254740992.0f);
					float gsn = sqrtf(-2 * logf(u1)) * cosf(6.2831853f * u2);
					dn += gsn * sqrtf(noise * noise + shot * (dn > 0 ? dn : 0));
				}
				float code = black + dn;
				v16[x] = (uint16_t)clampf(code + 0.5f, 0, maxcode);
			}
			memset(line, 0, stride);
			if (fmt == FMT_RAW10) {
				int x = 0;
				uint8_t *d = line;
				for (; x + 4 <= W; x += 4, d += 5) {
					d[0] = v16[x] >> 2, d[1] = v16[x + 1] >> 2, d[2] = v16[x + 2] >> 2, d[3] = v16[x + 3] >> 2;
					d[4] = (v16[x] & 3) | (v16[x + 1] & 3) << 2 | (v16[x + 2] & 3) << 4 | (v16[x + 3] & 3) << 6;
				}
				for (int kk = 0; x < W; x++, kk++) {
					d[kk] = v16[x] >> 2;
					d[4] |= (v16[x] & 3) << (2 * kk);
				}
			} else
				for (int x = 0; x < W; x++) line[2 * x] = v16[x] & 255, line[2 * x + 1] = v16[x] >> 8;
			fwrite(line, 1, stride, fo);
			if (fr) {
				for (int x = 0; x < 3 * W; x++) put16(line + 2 * x, r[x]);
				fwrite(line, 2, 3 * (size_t)W, fr);
			}
			if (fb) {
				for (int x = 0; x < W; x++) {
					int c = rcfa.c[y & 1][x & 1];
					put16(line + 2 * x, lut[(int)(clampf(r[3 * x + c], 0, 1) * 4095 + 0.5f)] * sens[c] * expo);
				}
				fwrite(line, 2, W, fb);
			}
		}
	}
	if (fclose(fo) || (fr && fclose(fr)) || (fb && fclose(fb))) die("write error");
	char cn[5];
	cfa_name(&g.cfa, cn);
	fprintf(stderr, "%s: %dx%d %s cell %d phase %d,%d stride %ld black %.0f, sens %.2f,%.2f,%.2f (WB gains %.3f,%.3f,%.3f) in %.2f s\n",
		out, W, H, cn, g.n, g.px, g.py, stride, black, sens[0], sens[1], sens[2], 1 / sens[0], 1 / sens[1], 1 / sens[2],
		now_sec() - t0);
	return 0;
}
