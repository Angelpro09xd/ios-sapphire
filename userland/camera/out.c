/* output writers (streamed row chunks): PPM, PNG (stored deflate), PGM, JPEG, DNG */
#include "out.h"

/* ---- tone curve ---- */
void tone_init(Tone *t, const char *curve)
{
	int mode = 0;
	float gam = 1;
	if (!curve || !strcmp(curve, "srgb")) mode = 0;
	else if (!strcmp(curve, "linear")) mode = 1;
	else {
		gam = (float)atof(curve);
		if (gam <= 0.1f || gam > 10) die("bad --gamma %s", curve);
		mode = 2;
	}
	for (int i = 0; i <= 65536; i++) {
		float v = (i > 65535 ? 65535 : i) / 65535.0f, o;
		o = mode == 0 ? srgb_encode(v) : mode == 1 ? v : powf(v, 1 / gam);
		t->lut[i] = (uint16_t)(o * 65535.0f + 0.5f);
	}
}

static FILE *xfopen(const char *path)
{
	FILE *f = fopen(path, "wb");
	if (!f) die("cannot create %s", path);
	static char *bufs[16];
	static int nb;
	if (nb < 16) {
		bufs[nb] = xmalloc(1 << 20);
		setvbuf(f, bufs[nb++], _IOFBF, 1 << 20);
	}
	return f;
}

static void xfclose(FILE *f, const char *path)
{
	if (ferror(f) | fclose(f)) die("write error on %s", path);
}

typedef struct {
	FILE *f;
	const char *path;
	int bits;
	const Tone *tone;
	uint8_t *buf;
	/* png */
	uint32_t adler_a, adler_b;
	int first;
	long long remaining; /* uncompressed bytes still to come (png) */
	/* pgm */
	Cfa cfa;
	float igain[3];
	/* jpeg */
	JpegEnc *je;
	uint8_t **rows8;
	uint8_t **outb;
	size_t *outl;
	int quality;
} Wr;

static Sink *sink_new(const char *name, Stage *src, int gran, Wr *w)
{
	Sink *s = xcalloc(1, sizeof *s);
	s->name = name;
	s->src = src;
	s->h = src->h;
	s->gran = gran;
	s->p = w;
	stage_sink_need(src, g_chunk + gran);
	return s;
}

/* rows [y0,y1) of an RGB stage -> interleaved big-endian 16-bit or 8-bit, in parallel */
typedef struct {
	Sink *s;
	int y0;
	uint8_t *dst;
	size_t rowbytes;
} ConvCtx;

static void conv_rows(void *vc, int a, int b)
{
	ConvCtx *c = vc;
	Wr *w = c->s->p;
	Stage *st = c->s->src;
	int pngf = c->rowbytes != (size_t)st->w * 3 * (w->bits / 8); /* png: filter byte */
	for (int k = a; k < b; k++) {
		const float *r = srow(st, c->y0 + k);
		uint8_t *d = c->dst + c->rowbytes * k;
		if (pngf) *d++ = 0;
		if (w->bits == 16)
			for (int x = 0; x < st->w * 3; x++) {
				unsigned v = tone16(w->tone, r[x]);
				d[2 * x] = v >> 8;
				d[2 * x + 1] = v & 255;
			}
		else
			for (int x = 0; x < st->w * 3; x++) d[x] = (tone16(w->tone, r[x]) * 255 + 32767) / 65535;
	}
}

static void conv(Sink *s, int y0, int y1, size_t rowbytes, uint8_t *dst)
{
	ConvCtx c = { s, y0, dst, rowbytes };
	pool_for(y1 - y0, 4, conv_rows, &c);
}

/* ---- PPM ---- */
static void ppm_consume(Sink *s, int y0, int y1)
{
	Wr *w = s->p;
	size_t rb = (size_t)s->src->w * 3 * (w->bits / 8);
	conv(s, y0, y1, rb, w->buf);
	fwrite(w->buf, rb, y1 - y0, w->f);
	s->bytes += rb * (y1 - y0);
}

static void ppm_finish(Sink *s)
{
	Wr *w = s->p;
	xfclose(w->f, w->path);
}

static Wr *wr_new(const char *path, int bits, const Tone *tone, size_t bufbytes)
{
	Wr *w = xcalloc(1, sizeof *w);
	w->f = xfopen(path);
	w->path = path;
	w->bits = bits;
	w->tone = tone;
	w->buf = xmalloc(bufbytes);
	return w;
}

Sink *sink_ppm(Stage *rgb, const char *path, int bits, const Tone *tone)
{
	Wr *w = wr_new(path, bits, tone, (size_t)rgb->w * 3 * 2 * (g_chunk + 16));
	fprintf(w->f, "P6\n%d %d\n%d\n", rgb->w, rgb->h, bits == 16 ? 65535 : 255);
	Sink *s = sink_new("ppm", rgb, 1, w);
	s->consume = ppm_consume;
	s->finish = ppm_finish;
	return s;
}

/* ---- PNG (zlib stream of stored blocks; big but simple and fast) ---- */
static uint32_t crc_table[256];
static void crc_init(void)
{
	for (uint32_t n = 0; n < 256; n++) {
		uint32_t c = n;
		for (int k = 0; k < 8; k++) c = c & 1 ? 0xedb88320u ^ (c >> 1) : c >> 1;
		crc_table[n] = c;
	}
}
static uint32_t crc_update(uint32_t c, const uint8_t *b, size_t n)
{
	for (size_t i = 0; i < n; i++) c = crc_table[(c ^ b[i]) & 255] ^ (c >> 8);
	return c;
}
static void be32(uint8_t *p, uint32_t v)
{
	p[0] = v >> 24;
	p[1] = v >> 16;
	p[2] = v >> 8;
	p[3] = v;
}

static void png_chunk(FILE *f, const char *type, const uint8_t *data, size_t n)
{
	uint8_t hdr[8];
	be32(hdr, (uint32_t)n);
	memcpy(hdr + 4, type, 4);
	fwrite(hdr, 1, 8, f);
	if (n) fwrite(data, 1, n, f);
	uint32_t c = crc_update(0xffffffffu, (const uint8_t *)type, 4);
	c = crc_update(c, data, n) ^ 0xffffffffu;
	be32(hdr, c);
	fwrite(hdr, 1, 4, f);
}

static void png_consume(Sink *s, int y0, int y1)
{
	Wr *w = s->p;
	size_t rb = 1 + (size_t)s->src->w * 3 * (w->bits / 8);
	size_t n = rb * (y1 - y0);
	uint8_t *raw = w->buf;
	conv(s, y0, y1, rb, raw);
	/* adler32 */
	uint32_t a = w->adler_a, b = w->adler_b;
	for (size_t i = 0; i < n;) {
		size_t m = n - i < 5552 ? n - i : 5552;
		for (size_t k = 0; k < m; k++) {
			a += raw[i + k];
			b += a;
		}
		a %= 65521;
		b %= 65521;
		i += m;
	}
	w->adler_a = a;
	w->adler_b = b;
	w->remaining -= n;
	/* deflate stored blocks */
	size_t nblk = (n + 65534) / 65535;
	uint8_t *z = xmalloc(n + nblk * 5 + 6 + 4);
	size_t zl = 0;
	if (w->first) {
		z[zl++] = 0x78;
		z[zl++] = 0x01;
		w->first = 0;
	}
	for (size_t i = 0; i < n;) {
		size_t m = n - i < 65535 ? n - i : 65535;
		int final = (i + m == n) && w->remaining == 0;
		z[zl++] = final;
		z[zl++] = m & 255;
		z[zl++] = m >> 8;
		z[zl++] = ~m & 255;
		z[zl++] = (~m >> 8) & 255;
		memcpy(z + zl, raw + i, m);
		zl += m;
		i += m;
	}
	if (w->remaining == 0) {
		be32(z + zl, (b << 16) | a);
		zl += 4;
	}
	png_chunk(w->f, "IDAT", z, zl);
	s->bytes += zl;
	free(z);
}

static void png_finish(Sink *s)
{
	Wr *w = s->p;
	png_chunk(w->f, "IEND", NULL, 0);
	xfclose(w->f, w->path);
}

Sink *sink_png(Stage *rgb, const char *path, int bits, const Tone *tone)
{
	if (!crc_table[1]) crc_init();
	Wr *w = wr_new(path, bits, tone, (1 + (size_t)rgb->w * 3 * 2) * (g_chunk + 16));
	static const uint8_t sig[8] = { 137, 80, 78, 71, 13, 10, 26, 10 };
	fwrite(sig, 1, 8, w->f);
	uint8_t ih[13];
	be32(ih, rgb->w);
	be32(ih + 4, rgb->h);
	ih[8] = bits;
	ih[9] = 2; /* RGB */
	ih[10] = ih[11] = ih[12] = 0;
	png_chunk(w->f, "IHDR", ih, 13);
	w->adler_a = 1;
	w->adler_b = 0;
	w->first = 1;
	w->remaining = (1 + (long long)rgb->w * 3 * (bits / 8)) * rgb->h;
	Sink *s = sink_new("png", rgb, 1, w);
	s->consume = png_consume;
	s->finish = png_finish;
	return s;
}

/* ---- PGM (mosaic) ---- */
static void pgm_consume(Sink *s, int y0, int y1)
{
	Wr *w = s->p;
	Stage *st = s->src;
	for (int y = y0; y < y1; y++) {
		const float *r = srow(st, y);
		uint8_t *d = w->buf;
		for (int x = 0; x < st->w; x++) {
			float v = r[x] * w->igain[w->cfa.c[y & 1][x & 1]];
			unsigned q = (unsigned)(clampf(v, 0, 1) * 65535.0f + 0.5f);
			d[2 * x] = q >> 8;
			d[2 * x + 1] = q & 255;
		}
		fwrite(d, 2, st->w, w->f);
	}
	s->bytes += 2LL * st->w * (y1 - y0);
}

Sink *sink_pgm(Stage *bayer, const char *path, const Cfa *cfa, const float gain[3])
{
	Wr *w = wr_new(path, 16, NULL, 2 * (size_t)bayer->w);
	w->cfa = *cfa;
	for (int c = 0; c < 3; c++) w->igain[c] = 1 / gain[c];
	fprintf(w->f, "P5\n%d %d\n65535\n", bayer->w, bayer->h);
	Sink *s = sink_new("pgm", bayer, 1, w);
	s->consume = pgm_consume;
	s->finish = ppm_finish;
	return s;
}

/* ---- JPEG ---- */
typedef struct {
	Sink *s;
	int m0;
} JCtx;

static void jpeg_rows(void *vc, int a, int b)
{
	JCtx *c = vc;
	Sink *s = c->s;
	Wr *w = s->p;
	Stage *st = s->src;
	int W = st->w;
	for (int m = a; m < b; m++) {
		uint8_t *buf = w->rows8[m];
		const uint8_t *rows[16];
		for (int k = 0; k < 16; k++) {
			int y = (c->m0 + m) * 16 + k;
			if (y >= st->h) y = st->h - 1;
			const float *r = srow(st, y);
			uint8_t *d = buf + (size_t)W * 3 * k;
			for (int x = 0; x < W * 3; x++) d[x] = (tone16(w->tone, r[x]) * 255 + 32767) / 65535;
			rows[k] = d;
		}
		w->outb[m] = jpeg_encode_mcu_row(w->je, rows, &w->outl[m]);
	}
}

static void jpeg_consume(Sink *s, int y0, int y1)
{
	Wr *w = s->p;
	int m0 = y0 / 16, m1 = (y1 + 15) / 16;
	JCtx c = { s, m0 };
	pool_for(m1 - m0, 1, jpeg_rows, &c);
	for (int m = 0; m < m1 - m0; m++) {
		jpeg_write_mcu_row(w->je, w->f, m0 + m, w->outb[m], w->outl[m]);
		s->bytes += w->outl[m];
		free(w->outb[m]);
	}
}

static void jpeg_fin(Sink *s)
{
	Wr *w = s->p;
	jpeg_finish(w->je, w->f);
	xfclose(w->f, w->path);
}

Sink *sink_jpeg(Stage *rgb, const char *path, int quality, const Tone *tone)
{
	Wr *w = wr_new(path, 8, tone, 1);
	w->je = jpeg_new(rgb->w, rgb->h, quality);
	jpeg_write_header(w->je, w->f);
	int maxm = (g_chunk + 32) / 16 + 2;
	w->rows8 = xmalloc(sizeof(uint8_t *) * maxm);
	for (int i = 0; i < maxm; i++) w->rows8[i] = xmalloc((size_t)rgb->w * 3 * 16);
	w->outb = xcalloc(maxm, sizeof(uint8_t *));
	w->outl = xcalloc(maxm, sizeof(size_t));
	Sink *s = sink_new("jpeg", rgb, 16, w);
	s->consume = jpeg_consume;
	s->finish = jpeg_fin;
	return s;
}

/* ---- DNG (TIFF/EP, little endian, raw CFA image in IFD0, uncompressed 16-bit) ---- */
typedef struct {
	uint16_t tag, type;
	uint32_t count;
	uint32_t value;  /* inline value or offset */
	const void *data; /* external data (if > 4 bytes) */
	uint32_t size;
} Ent;

static int tsize(int type) { return type == 3 ? 2 : (type == 4 || type == 9) ? 4 : (type == 5 || type == 10) ? 8 : 1; }

typedef struct {
	Ent e[48];
	int n;
} Ifd;

static void add(Ifd *d, int tag, int type, uint32_t count, const void *data)
{
	Ent *e = &d->e[d->n++];
	e->tag = tag;
	e->type = type;
	e->count = count;
	e->size = tsize(type) * count;
	e->data = data;
	e->value = 0;
}

static void add1(Ifd *d, int tag, int type, uint32_t v)
{
	uint32_t *p = xmalloc(4);
	if (type == 3) {
		uint16_t s = v;
		memcpy(p, &s, 2);
	} else *p = v;
	add(d, tag, type, 1, p);
}

static void put_rat(int32_t *dst, double v, int sgn)
{
	int32_t den = 10000;
	double n = v * den;
	if (!sgn && n < 0) n = 0;
	dst[0] = (int32_t)lrint(n);
	dst[1] = den;
}

static int ent_cmp(const void *a, const void *b) { return ((const Ent *)a)->tag - ((const Ent *)b)->tag; }

typedef struct {
	FILE *f;
	const char *path;
	DngInfo info;
	uint16_t *row;
} Dng;

static void dng_consume(Sink *s, int y0, int y1)
{
	Dng *d = s->p;
	Stage *st = s->src;
	float ig[3], k = d->info.white - d->info.black;
	for (int c = 0; c < 3; c++) ig[c] = k / d->info.gain[c];
	for (int y = y0; y < y1; y++) {
		const float *r = srow(st, y);
		for (int x = 0; x < st->w; x++) {
			float v = d->info.black + r[x] * ig[d->info.cfa.c[y & 1][x & 1]];
			d->row[x] = (uint16_t)clampf(v + 0.5f, 0, 65535);
		}
		fwrite(d->row, 2, st->w, d->f); /* host is little endian (x86-64, arm64) */
	}
	s->bytes += 2LL * st->w * (y1 - y0);
}

static void dng_finish(Sink *s)
{
	Dng *d = s->p;
	xfclose(d->f, d->path);
}

Sink *sink_dng(Stage *bayer, const char *path, const DngInfo *info)
{
	Dng *d = xcalloc(1, sizeof *d);
	d->info = *info;
	d->path = path;
	d->f = xfopen(path);
	d->row = xmalloc(2 * (size_t)bayer->w);
	int W = bayer->w, H = bayer->h;
	const int rps = 64;
	int nstrips = (H + rps - 1) / rps;
	uint32_t *offs = xmalloc(4 * nstrips), *cnts = xmalloc(4 * nstrips);
	Ifd ifd = { .n = 0 };
	static const uint8_t dngver[4] = { 1, 4, 0, 0 }, dngback[4] = { 1, 1, 0, 0 };
	static const uint16_t rdim[2] = { 2, 2 }, bdim[2] = { 1, 1 };
	uint8_t *cfap = xmalloc(4), *plane = xmalloc(3);
	for (int i = 0; i < 4; i++) cfap[i] = info->cfa.c[i >> 1][i & 1];
	plane[0] = 0, plane[1] = 1, plane[2] = 2;
	int32_t *cm = xmalloc(9 * 8), *neu = xmalloc(3 * 8), *blk = xmalloc(8);
	for (int i = 0; i < 9; i++) put_rat(cm + 2 * i, info->colmat[i], 1);
	for (int i = 0; i < 3; i++) put_rat(neu + 2 * i, info->neutral[i], 0);
	put_rat(blk, info->black, 0);
	add1(&ifd, 254, 4, 0);       /* NewSubfileType: main image */
	add1(&ifd, 256, 4, W);
	add1(&ifd, 257, 4, H);
	add1(&ifd, 258, 3, 16);
	add1(&ifd, 259, 3, 1);       /* uncompressed */
	add1(&ifd, 262, 3, 32803);   /* CFA */
	if (info->desc) add(&ifd, 270, 2, strlen(info->desc) + 1, info->desc);
	add(&ifd, 271, 2, strlen(info->make) + 1, info->make);
	add(&ifd, 272, 2, strlen(info->model) + 1, info->model);
	add(&ifd, 273, 4, nstrips, offs);
	add1(&ifd, 274, 3, 1);
	add1(&ifd, 277, 3, 1);
	add1(&ifd, 278, 4, rps);
	add(&ifd, 279, 4, nstrips, cnts);
	add1(&ifd, 284, 3, 1);
	add(&ifd, 305, 2, strlen(info->software) + 1, info->software);
	add(&ifd, 33421, 3, 2, rdim);   /* CFARepeatPatternDim */
	add(&ifd, 33422, 1, 4, cfap);   /* CFAPattern */
	add(&ifd, 50706, 1, 4, dngver);
	add(&ifd, 50707, 1, 4, dngback);
	add(&ifd, 50708, 2, strlen(info->unique) + 1, info->unique);
	add(&ifd, 50710, 1, 3, plane);  /* CFAPlaneColor */
	add1(&ifd, 50711, 3, 1);        /* CFALayout: rectangular */
	add(&ifd, 50713, 3, 2, bdim);   /* BlackLevelRepeatDim */
	add(&ifd, 50714, 5, 1, blk);    /* BlackLevel */
	add1(&ifd, 50717, 4, (uint32_t)lrintf(info->white));
	add(&ifd, 50721, 10, 9, cm);    /* ColorMatrix1 */
	add(&ifd, 50728, 5, 3, neu);    /* AsShotNeutral */
	add1(&ifd, 50778, 3, 21);       /* CalibrationIlluminant1: D65 */
	qsort(ifd.e, ifd.n, sizeof(Ent), ent_cmp);
	/* layout: header | IFD | external data | image */
	uint32_t pos = 8 + 2 + 12 * ifd.n + 4;
	for (int i = 0; i < ifd.n; i++)
		if (ifd.e[i].size > 4) {
			ifd.e[i].value = pos;
			pos += (ifd.e[i].size + 1) & ~1u;
		}
	uint32_t img = (pos + 4095) & ~4095u;
	for (int i = 0; i < nstrips; i++) {
		int rows = H - i * rps < rps ? H - i * rps : rps;
		offs[i] = img + (uint32_t)((uint64_t)i * rps * W * 2);
		cnts[i] = (uint32_t)rows * W * 2;
	}
	if ((uint64_t)img + (uint64_t)W * H * 2 > 0xffffffffull) die("DNG larger than 4 GB");
	FILE *f = d->f;
	uint8_t hdr[8] = { 'I', 'I', 42, 0, 8, 0, 0, 0 };
	fwrite(hdr, 1, 8, f);
	uint16_t n16 = ifd.n;
	fwrite(&n16, 2, 1, f);
	for (int i = 0; i < ifd.n; i++) {
		Ent *e = &ifd.e[i];
		fwrite(&e->tag, 2, 1, f);
		fwrite(&e->type, 2, 1, f);
		fwrite(&e->count, 4, 1, f);
		if (e->size > 4) fwrite(&e->value, 4, 1, f);
		else {
			uint8_t v[4] = { 0 };
			memcpy(v, e->data, e->size);
			fwrite(v, 1, 4, f);
		}
	}
	uint32_t zero = 0;
	fwrite(&zero, 4, 1, f);
	for (int i = 0; i < ifd.n; i++)
		if (ifd.e[i].size > 4) {
			fwrite(ifd.e[i].data, 1, ifd.e[i].size, f);
			if (ifd.e[i].size & 1) fputc(0, f);
		}
	while ((uint32_t)ftell(f) < img) fputc(0, f);
	Sink *s = sink_new("dng", bayer, 1, (Wr *)d);
	s->p = d;
	s->consume = dng_consume;
	s->finish = dng_finish;
	return s;
}

/* ---- driver: advance all sinks in lockstep (same fraction of their height) ---- */
void sinks_run(Sink **s, int n)
{
	int H = 0;
	for (int i = 0; i < n; i++)
		if (s[i]->h > H) H = s[i]->h;
	for (int r = 0; r < H;) {
		r += g_chunk;
		if (r > H) r = H;
		for (int i = 0; i < n; i++) {
			Sink *k = s[i];
			int t = (int)(((long long)r * k->h + H - 1) / H);
			t = (t + k->gran - 1) / k->gran * k->gran;
			if (t > k->h) t = k->h;
			if (r == H) t = k->h;
			while (k->done < t) {
				int b = k->done + g_chunk;
				b = (b + k->gran - 1) / k->gran * k->gran;
				if (b > t) b = t;
				stage_ensure(k->src, b + 1 > k->h ? k->h : b + 1);
				double t0 = now_sec();
				k->consume(k, k->done, b);
				k->t += now_sec() - t0;
				k->done = b;
			}
		}
	}
	for (int i = 0; i < n; i++) {
		double t0 = now_sec();
		s[i]->finish(s[i]);
		s[i]->t += now_sec() - t0;
	}
}
