/* Baseline JPEG encoder (written for this project).
 *
 * YCbCr 4:2:0, standard (ITU-T T.81 Annex K) quantisation and Huffman tables, IJG quality
 * scaling, AAN float DCT. A restart interval of one MCU row makes every 16-pixel band
 * independent, so bands are encoded in parallel and the file is written as a stream.
 */
#include "out.h"

static const uint8_t zz[64] = { 0,  1,  8,  16, 9,  2,  3,  10, 17, 24, 32, 25, 18, 11, 4,  5,
				 12, 19, 26, 33, 40, 48, 41, 34, 27, 20, 13, 6,  7,  14, 21, 28,
				 35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23, 30, 37, 44, 51,
				 58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63 };

static const uint8_t qlum[64] = { 16, 11, 10, 16, 24,  40,  51,  61,  12, 12, 14, 19, 26,  58,  60,  55,
				  14, 13, 16, 24, 40,  57,  69,  56,  14, 17, 22, 29, 51,  87,  80,  62,
				  18, 22, 37, 56, 68,  109, 103, 77,  24, 35, 55, 64, 81,  104, 113, 92,
				  49, 64, 78, 87, 103, 121, 120, 101, 72, 92, 95, 98, 112, 100, 103, 99 };
static const uint8_t qchr[64] = { 17, 18, 24, 47, 99, 99, 99, 99, 18, 21, 26, 66, 99, 99, 99, 99,
				  24, 26, 56, 99, 99, 99, 99, 99, 47, 66, 99, 99, 99, 99, 99, 99,
				  99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99,
				  99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99 };

static const uint8_t dc_lum_bits[16] = { 0, 1, 5, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0 };
static const uint8_t dc_chr_bits[16] = { 0, 3, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0 };
static const uint8_t dc_vals[12] = { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11 };
static const uint8_t ac_lum_bits[16] = { 0, 2, 1, 3, 3, 2, 4, 3, 5, 5, 4, 4, 0, 0, 1, 0x7d };
static const uint8_t ac_lum_vals[162] = {
	0x01, 0x02, 0x03, 0x00, 0x04, 0x11, 0x05, 0x12, 0x21, 0x31, 0x41, 0x06, 0x13, 0x51, 0x61, 0x07, 0x22, 0x71,
	0x14, 0x32, 0x81, 0x91, 0xa1, 0x08, 0x23, 0x42, 0xb1, 0xc1, 0x15, 0x52, 0xd1, 0xf0, 0x24, 0x33, 0x62, 0x72,
	0x82, 0x09, 0x0a, 0x16, 0x17, 0x18, 0x19, 0x1a, 0x25, 0x26, 0x27, 0x28, 0x29, 0x2a, 0x34, 0x35, 0x36, 0x37,
	0x38, 0x39, 0x3a, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48, 0x49, 0x4a, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58, 0x59,
	0x5a, 0x63, 0x64, 0x65, 0x66, 0x67, 0x68, 0x69, 0x6a, 0x73, 0x74, 0x75, 0x76, 0x77, 0x78, 0x79, 0x7a, 0x83,
	0x84, 0x85, 0x86, 0x87, 0x88, 0x89, 0x8a, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97, 0x98, 0x99, 0x9a, 0xa2, 0xa3,
	0xa4, 0xa5, 0xa6, 0xa7, 0xa8, 0xa9, 0xaa, 0xb2, 0xb3, 0xb4, 0xb5, 0xb6, 0xb7, 0xb8, 0xb9, 0xba, 0xc2, 0xc3,
	0xc4, 0xc5, 0xc6, 0xc7, 0xc8, 0xc9, 0xca, 0xd2, 0xd3, 0xd4, 0xd5, 0xd6, 0xd7, 0xd8, 0xd9, 0xda, 0xe1, 0xe2,
	0xe3, 0xe4, 0xe5, 0xe6, 0xe7, 0xe8, 0xe9, 0xea, 0xf1, 0xf2, 0xf3, 0xf4, 0xf5, 0xf6, 0xf7, 0xf8, 0xf9, 0xfa
};
static const uint8_t ac_chr_bits[16] = { 0, 2, 1, 2, 4, 4, 3, 4, 7, 5, 4, 4, 0, 1, 2, 0x77 };
static const uint8_t ac_chr_vals[162] = {
	0x00, 0x01, 0x02, 0x03, 0x11, 0x04, 0x05, 0x21, 0x31, 0x06, 0x12, 0x41, 0x51, 0x07, 0x61, 0x71, 0x13, 0x22,
	0x32, 0x81, 0x08, 0x14, 0x42, 0x91, 0xa1, 0xb1, 0xc1, 0x09, 0x23, 0x33, 0x52, 0xf0, 0x15, 0x62, 0x72, 0xd1,
	0x0a, 0x16, 0x24, 0x34, 0xe1, 0x25, 0xf1, 0x17, 0x18, 0x19, 0x1a, 0x26, 0x27, 0x28, 0x29, 0x2a, 0x35, 0x36,
	0x37, 0x38, 0x39, 0x3a, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48, 0x49, 0x4a, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58,
	0x59, 0x5a, 0x63, 0x64, 0x65, 0x66, 0x67, 0x68, 0x69, 0x6a, 0x73, 0x74, 0x75, 0x76, 0x77, 0x78, 0x79, 0x7a,
	0x82, 0x83, 0x84, 0x85, 0x86, 0x87, 0x88, 0x89, 0x8a, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97, 0x98, 0x99, 0x9a,
	0xa2, 0xa3, 0xa4, 0xa5, 0xa6, 0xa7, 0xa8, 0xa9, 0xaa, 0xb2, 0xb3, 0xb4, 0xb5, 0xb6, 0xb7, 0xb8, 0xb9, 0xba,
	0xc2, 0xc3, 0xc4, 0xc5, 0xc6, 0xc7, 0xc8, 0xc9, 0xca, 0xd2, 0xd3, 0xd4, 0xd5, 0xd6, 0xd7, 0xd8, 0xd9, 0xda,
	0xe2, 0xe3, 0xe4, 0xe5, 0xe6, 0xe7, 0xe8, 0xe9, 0xea, 0xf2, 0xf3, 0xf4, 0xf5, 0xf6, 0xf7, 0xf8, 0xf9, 0xfa
};

typedef struct {
	uint16_t code[256];
	uint8_t len[256];
} Huff;

struct JpegEnc {
	int w, h, mx, my; /* size, MCUs across/down */
	uint8_t q[2][64];  /* natural order */
	float rq[2][64];   /* reciprocal divisors including the AAN scale */
	Huff dc[2], ac[2];
};

static void huff_build(Huff *t, const uint8_t bits[16], const uint8_t *vals)
{
	int code = 0, k = 0;
	memset(t, 0, sizeof *t);
	for (int l = 1; l <= 16; l++) {
		for (int i = 0; i < bits[l - 1]; i++) {
			t->code[vals[k]] = code++;
			t->len[vals[k]] = l;
			k++;
		}
		code <<= 1;
	}
}

JpegEnc *jpeg_new(int w, int h, int quality)
{
	if (w > 65535 || h > 65535) die("JPEG: image too large");
	JpegEnc *e = xcalloc(1, sizeof *e);
	e->w = w;
	e->h = h;
	e->mx = (w + 15) / 16;
	e->my = (h + 15) / 16;
	quality = clampi(quality, 1, 100);
	int sc = quality < 50 ? 5000 / quality : 200 - 2 * quality;
	static const double aan[8] = { 1.0, 1.387039845, 1.306562965, 1.175875602, 1.0, 0.785694958, 0.541196100, 0.275899379 };
	for (int t = 0; t < 2; t++)
		for (int i = 0; i < 64; i++) {
			int v = ((t ? qchr : qlum)[i] * sc + 50) / 100;
			e->q[t][i] = clampi(v, 1, 255);
			e->rq[t][i] = (float)(1.0 / (e->q[t][i] * aan[i >> 3] * aan[i & 7] * 8.0));
		}
	huff_build(&e->dc[0], dc_lum_bits, dc_vals);
	huff_build(&e->dc[1], dc_chr_bits, dc_vals);
	huff_build(&e->ac[0], ac_lum_bits, ac_lum_vals);
	huff_build(&e->ac[1], ac_chr_bits, ac_chr_vals);
	return e;
}

int jpeg_mcu_rows(const JpegEnc *e) { return e->my; }

static void w16(FILE *f, int v)
{
	fputc(v >> 8, f);
	fputc(v & 255, f);
}

static void dht(FILE *f, int cls_id, const uint8_t bits[16], const uint8_t *vals)
{
	int n = 0;
	for (int i = 0; i < 16; i++) n += bits[i];
	fputc(cls_id, f);
	fwrite(bits, 1, 16, f);
	fwrite(vals, 1, n, f);
}

void jpeg_write_header(JpegEnc *e, FILE *f)
{
	static const uint8_t app0[] = { 0xff, 0xe0, 0, 16, 'J', 'F', 'I', 'F', 0, 1, 1, 0, 0, 1, 0, 1, 0, 0 };
	fputc(0xff, f);
	fputc(0xd8, f);
	fwrite(app0, 1, sizeof app0, f);
	/* DQT */
	fputc(0xff, f);
	fputc(0xdb, f);
	w16(f, 2 + 2 * 65);
	for (int t = 0; t < 2; t++) {
		fputc(t, f);
		for (int i = 0; i < 64; i++) fputc(e->q[t][zz[i]], f);
	}
	/* SOF0 */
	static const uint8_t sof_tail[] = { 3, 1, 0x22, 0, 2, 0x11, 1, 3, 0x11, 1 };
	fputc(0xff, f);
	fputc(0xc0, f);
	w16(f, 17);
	fputc(8, f);
	w16(f, e->h);
	w16(f, e->w);
	fwrite(sof_tail, 1, sizeof sof_tail, f);
	/* DHT */
	fputc(0xff, f);
	fputc(0xc4, f);
	w16(f, 2 + 4 * 17 + 12 + 12 + 162 + 162);
	dht(f, 0x00, dc_lum_bits, dc_vals);
	dht(f, 0x10, ac_lum_bits, ac_lum_vals);
	dht(f, 0x01, dc_chr_bits, dc_vals);
	dht(f, 0x11, ac_chr_bits, ac_chr_vals);
	/* DRI: one MCU row */
	fputc(0xff, f);
	fputc(0xdd, f);
	w16(f, 4);
	w16(f, e->mx);
	/* SOS */
	static const uint8_t sos[] = { 0xff, 0xda, 0, 12, 3, 1, 0x00, 2, 0x11, 3, 0x11, 0, 63, 0 };
	fwrite(sos, 1, sizeof sos, f);
}

/* ---- bit writer ---- */
typedef struct {
	uint8_t *b;
	size_t n, cap;
	uint32_t acc;
	int nb;
} Bits;

static inline void putbyte(Bits *w, uint8_t v)
{
	if (w->n + 2 > w->cap) {
		w->cap = w->cap * 2 + 4096;
		w->b = realloc(w->b, w->cap);
		if (!w->b) die("out of memory");
	}
	w->b[w->n++] = v;
	if (v == 0xff) w->b[w->n++] = 0;
}

static inline void putbits(Bits *w, uint32_t code, int len)
{
	w->acc = (w->acc << len) | (code & ((1u << len) - 1));
	w->nb += len;
	while (w->nb >= 8) {
		w->nb -= 8;
		putbyte(w, (uint8_t)(w->acc >> w->nb));
	}
}

static void flushbits(Bits *w)
{
	if (w->nb) putbits(w, 0x7f, 8 - w->nb); /* pad with ones */
}

/* ---- DCT (AAN, float) ---- */
static void fdct(float *d)
{
	for (int pass = 0; pass < 2; pass++) {
		int step = pass ? 8 : 1, stride = pass ? 1 : 8;
		for (int r = 0; r < 8; r++) {
			float *p = d + r * stride;
			float t0 = p[0] + p[7 * step], t7 = p[0] - p[7 * step];
			float t1 = p[step] + p[6 * step], t6 = p[step] - p[6 * step];
			float t2 = p[2 * step] + p[5 * step], t5 = p[2 * step] - p[5 * step];
			float t3 = p[3 * step] + p[4 * step], t4 = p[3 * step] - p[4 * step];
			float t10 = t0 + t3, t13 = t0 - t3, t11 = t1 + t2, t12 = t1 - t2;
			p[0] = t10 + t11;
			p[4 * step] = t10 - t11;
			float z1 = (t12 + t13) * 0.707106781f;
			p[2 * step] = t13 + z1;
			p[6 * step] = t13 - z1;
			t10 = t4 + t5;
			t11 = t5 + t6;
			t12 = t6 + t7;
			float z5 = (t10 - t12) * 0.382683433f;
			float z2 = 0.541196100f * t10 + z5;
			float z4 = 1.306562965f * t12 + z5;
			float z3 = t11 * 0.707106781f;
			float z11 = t7 + z3, z13 = t7 - z3;
			p[5 * step] = z13 + z2;
			p[3 * step] = z13 - z2;
			p[step] = z11 + z4;
			p[7 * step] = z11 - z4;
		}
	}
}

static inline int nbits(int v)
{
	if (v < 0) v = -v;
	int n = 0;
	while (v) {
		n++;
		v >>= 1;
	}
	return n;
}

static void block(JpegEnc *e, Bits *w, float *d, int t, int *pred)
{
	int q[64];
	fdct(d);
	const float *rq = e->rq[t];
	for (int i = 0; i < 64; i++) {
		float v = d[zz[i]] * rq[zz[i]];
		q[i] = (int)(v < 0 ? v - 0.5f : v + 0.5f);
	}
	int diff = q[0] - *pred;
	*pred = q[0];
	int nb = nbits(diff);
	putbits(w, e->dc[t].code[nb], e->dc[t].len[nb]);
	if (nb) putbits(w, diff < 0 ? diff - 1 : diff, nb);
	int run = 0;
	const Huff *ac = &e->ac[t];
	for (int i = 1; i < 64; i++) {
		if (!q[i]) {
			run++;
			continue;
		}
		while (run > 15) {
			putbits(w, ac->code[0xf0], ac->len[0xf0]);
			run -= 16;
		}
		int v = q[i];
		nb = nbits(v);
		int sym = (run << 4) | nb;
		putbits(w, ac->code[sym], ac->len[sym]);
		putbits(w, v < 0 ? v - 1 : v, nb);
		run = 0;
	}
	if (run) putbits(w, ac->code[0], ac->len[0]);
}

uint8_t *jpeg_encode_mcu_row(JpegEnc *e, const uint8_t *const rows[16], size_t *len)
{
	Bits w = { 0 };
	w.cap = (size_t)e->mx * 400 + 4096;
	w.b = xmalloc(w.cap);
	int pred[3] = { 0, 0, 0 };
	float Y[4][64], Cb[64], Cr[64];
	for (int m = 0; m < e->mx; m++) {
		float cbs[16][16], crs[16][16];
		for (int j = 0; j < 16; j++) {
			const uint8_t *r = rows[j];
			for (int i = 0; i < 16; i++) {
				int x = m * 16 + i;
				if (x >= e->w) x = e->w - 1;
				float R = r[3 * x], G = r[3 * x + 1], B = r[3 * x + 2];
				Y[(j >> 3) * 2 + (i >> 3)][(j & 7) * 8 + (i & 7)] = 0.299f * R + 0.587f * G + 0.114f * B - 128;
				cbs[j][i] = -0.168736f * R - 0.331264f * G + 0.5f * B;
				crs[j][i] = 0.5f * R - 0.418688f * G - 0.081312f * B;
			}
		}
		for (int j = 0; j < 8; j++)
			for (int i = 0; i < 8; i++) {
				Cb[j * 8 + i] = 0.25f * (cbs[2 * j][2 * i] + cbs[2 * j][2 * i + 1] + cbs[2 * j + 1][2 * i] + cbs[2 * j + 1][2 * i + 1]);
				Cr[j * 8 + i] = 0.25f * (crs[2 * j][2 * i] + crs[2 * j][2 * i + 1] + crs[2 * j + 1][2 * i] + crs[2 * j + 1][2 * i + 1]);
			}
		for (int k = 0; k < 4; k++) block(e, &w, Y[k], 0, &pred[0]);
		block(e, &w, Cb, 1, &pred[1]);
		block(e, &w, Cr, 1, &pred[2]);
	}
	flushbits(&w);
	*len = w.n;
	return w.b;
}

void jpeg_write_mcu_row(JpegEnc *e, FILE *f, int index, const uint8_t *buf, size_t len)
{
	fwrite(buf, 1, len, f);
	if (index + 1 < e->my) {
		fputc(0xff, f);
		fputc(0xd0 + (index & 7), f);
	}
}

void jpeg_finish(JpegEnc *e, FILE *f)
{
	(void)e;
	fputc(0xff, f);
	fputc(0xd9, f);
}
