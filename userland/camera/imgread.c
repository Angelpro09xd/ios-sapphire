/* image input for the test generator: binary PPM/PGM (8/16-bit) and PNG (non-interlaced,
 * any colour type, 8/16-bit, palette) with a small inflate written for this project
 * (follows RFC 1950/1951 directly, canonical Huffman decode as in the RFC's description). */
#include "imgread.h"

typedef struct {
	const uint8_t *s;
	size_t n, pos;
	uint32_t bb;
	int bc;
	uint8_t *o;
	size_t on, ocap;
} Inf;

typedef struct {
	short count[16];
	short sym[320];
} Hf;

static int ibits(Inf *s, int need)
{
	uint32_t v = s->bb;
	while (s->bc < need) {
		if (s->pos >= s->n) die("inflate: truncated stream");
		v |= (uint32_t)s->s[s->pos++] << s->bc;
		s->bc += 8;
	}
	s->bb = v >> need;
	s->bc -= need;
	return (int)(v & ((1u << need) - 1));
}

static int hbuild(Hf *h, const uint8_t *len, int n)
{
	short off[16];
	memset(h->count, 0, sizeof h->count);
	for (int i = 0; i < n; i++) h->count[len[i]]++;
	if (h->count[0] == n) return 0;
	int left = 1;
	for (int l = 1; l < 16; l++) {
		left <<= 1;
		left -= h->count[l];
		if (left < 0) return -1;
	}
	off[1] = 0;
	for (int l = 1; l < 15; l++) off[l + 1] = off[l] + h->count[l];
	for (int i = 0; i < n; i++)
		if (len[i]) h->sym[off[len[i]]++] = i;
	return left;
}

static int hdecode(Inf *s, const Hf *h)
{
	int code = 0, first = 0, index = 0;
	for (int l = 1; l < 16; l++) {
		code |= ibits(s, 1);
		int count = h->count[l];
		if (code - count < first) return h->sym[index + (code - first)];
		index += count;
		first += count;
		first <<= 1;
		code <<= 1;
	}
	die("inflate: bad code");
}

static void oput(Inf *s, uint8_t b)
{
	if (s->on == s->ocap) {
		s->ocap = s->ocap * 2 + 65536;
		s->o = realloc(s->o, s->ocap);
		if (!s->o) die("out of memory");
	}
	s->o[s->on++] = b;
}

static const short lbase[29] = { 3,  4,  5,  6,  7,  8,  9,  10, 11,  13,  15,  17,  19,  23, 27,
				 31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258 };
static const short lext[29] = { 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0 };
static const short dbase[30] = { 1,   2,   3,   4,   5,   7,    9,    13,   17,   25,   33,   49,   65,    97,    129,
				 193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577 };
static const short dext[30] = { 0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13 };

static void codes(Inf *s, const Hf *lc, const Hf *dc)
{
	for (;;) {
		int sym = hdecode(s, lc);
		if (sym < 256) oput(s, sym);
		else if (sym == 256) return;
		else {
			sym -= 257;
			if (sym >= 29) die("inflate: bad length");
			int len = lbase[sym] + ibits(s, lext[sym]);
			int ds = hdecode(s, dc);
			if (ds >= 30) die("inflate: bad distance");
			size_t dist = dbase[ds] + ibits(s, dext[ds]);
			if (dist > s->on) die("inflate: distance too far");
			while (len--) oput(s, s->o[s->on - dist]);
		}
	}
}

static uint8_t *inflate_zlib(const uint8_t *src, size_t n, size_t *outn)
{
	if (n < 2 || (src[0] & 15) != 8) die("PNG: not a zlib/deflate stream");
	Inf s = { src, n, 2, 0, 0, NULL, 0, 0 };
	int last;
	do {
		last = ibits(&s, 1);
		int type = ibits(&s, 2);
		if (type == 0) {
			s.bb = 0;
			s.bc = 0;
			if (s.pos + 4 > s.n) die("inflate: truncated");
			unsigned len = s.s[s.pos] | (s.s[s.pos + 1] << 8);
			s.pos += 4;
			if (s.pos + len > s.n) die("inflate: truncated");
			for (unsigned i = 0; i < len; i++) oput(&s, s.s[s.pos + i]);
			s.pos += len;
		} else if (type == 1) {
			static Hf fl, fd;
			static int init;
			if (!init) {
				uint8_t l[288];
				for (int i = 0; i < 288; i++) l[i] = i < 144 ? 8 : i < 256 ? 9 : i < 280 ? 7 : 8;
				hbuild(&fl, l, 288);
				for (int i = 0; i < 30; i++) l[i] = 5;
				hbuild(&fd, l, 30);
				init = 1;
			}
			codes(&s, &fl, &fd);
		} else if (type == 2) {
			static const uint8_t ord[19] = { 16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15 };
			int nlen = ibits(&s, 5) + 257, ndist = ibits(&s, 5) + 1, ncode = ibits(&s, 4) + 4;
			uint8_t l[320] = { 0 };
			for (int i = 0; i < ncode; i++) l[ord[i]] = ibits(&s, 3);
			Hf lc, dc;
			if (hbuild(&lc, l, 19) != 0) die("inflate: bad code lengths");
			memset(l, 0, sizeof l);
			for (int i = 0; i < nlen + ndist;) {
				int sym = hdecode(&s, &lc);
				if (sym < 16) l[i++] = sym;
				else {
					int rep, v = 0;
					if (sym == 16) {
						if (!i) die("inflate: repeat without length");
						v = l[i - 1];
						rep = 3 + ibits(&s, 2);
					} else if (sym == 17) rep = 3 + ibits(&s, 3);
					else rep = 11 + ibits(&s, 7);
					if (i + rep > nlen + ndist) die("inflate: too many lengths");
					while (rep--) l[i++] = v;
				}
			}
			if (hbuild(&lc, l, nlen) < 0 || hbuild(&dc, l + nlen, ndist) < 0) die("inflate: bad tables");
			codes(&s, &lc, &dc);
		} else die("inflate: bad block type");
	} while (!last);
	*outn = s.on;
	return s.o;
}

static uint32_t rd32(const uint8_t *p) { return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }

static int paeth(int a, int b, int c)
{
	int p = a + b - c, pa = abs(p - a), pb = abs(p - b), pc = abs(p - c);
	return pa <= pb && pa <= pc ? a : pb <= pc ? b : c;
}

static void load_png(Image *im, const uint8_t *d, size_t n)
{
	size_t pos = 8;
	int w = 0, h = 0, depth = 0, ct = 0;
	uint8_t pal[256][3];
	uint8_t *idat = NULL;
	size_t idn = 0;
	while (pos + 12 <= n) {
		uint32_t len = rd32(d + pos);
		const uint8_t *t = d + pos + 4, *c = d + pos + 8;
		if (pos + 12 + len > n) die("PNG: truncated chunk");
		if (!memcmp(t, "IHDR", 4)) {
			w = rd32(c);
			h = rd32(c + 4);
			depth = c[8];
			ct = c[9];
			if (c[12]) die("PNG: interlaced images not supported");
		} else if (!memcmp(t, "PLTE", 4)) {
			for (uint32_t i = 0; i < len / 3 && i < 256; i++) memcpy(pal[i], c + 3 * i, 3);
		} else if (!memcmp(t, "IDAT", 4)) {
			idat = realloc(idat, idn + len);
			memcpy(idat + idn, c, len);
			idn += len;
		} else if (!memcmp(t, "IEND", 4)) break;
		pos += 12 + len;
	}
	int chn = ct == 0 ? 1 : ct == 2 ? 3 : ct == 3 ? 1 : ct == 4 ? 2 : ct == 6 ? 4 : 0;
	if (!chn || !w || !h) die("PNG: unsupported colour type %d", ct);
	if (depth != 8 && depth != 16 && !(ct == 3 && depth == 8)) die("PNG: bit depth %d not supported", depth);
	size_t zn;
	uint8_t *z = inflate_zlib(idat, idn, &zn);
	free(idat);
	int bpp = chn * depth / 8;
	size_t rb = (size_t)w * bpp;
	if (zn < (rb + 1) * h) die("PNG: image data too short");
	uint8_t *prev = xcalloc(rb, 1);
	im->w = w;
	im->h = h;
	im->px = xmalloc(sizeof(float) * 3 * w * h);
	float maxv = depth == 16 ? 65535.0f : 255.0f;
	for (int y = 0; y < h; y++) {
		uint8_t *row = z + (rb + 1) * y + 1;
		int f = row[-1];
		for (size_t i = 0; i < rb; i++) {
			int a = i >= (size_t)bpp ? row[i - bpp] : 0, b = prev[i], c = i >= (size_t)bpp ? prev[i - bpp] : 0;
			int p = f == 0 ? 0 : f == 1 ? a : f == 2 ? b : f == 3 ? (a + b) / 2 : f == 4 ? paeth(a, b, c) : -1;
			if (p < 0) die("PNG: bad filter");
			row[i] = (uint8_t)(row[i] + p);
		}
		memcpy(prev, row, rb);
		for (int x = 0; x < w; x++) {
			float v[4];
			for (int k = 0; k < chn; k++)
				v[k] = depth == 16 ? (row[(x * chn + k) * 2] << 8 | row[(x * chn + k) * 2 + 1]) / maxv : row[x * chn + k] / maxv;
			float *o = im->px + 3 * ((size_t)y * w + x);
			if (ct == 3) {
				int ix = row[x];
				for (int k = 0; k < 3; k++) o[k] = pal[ix][k] / 255.0f;
			} else if (chn >= 3) o[0] = v[0], o[1] = v[1], o[2] = v[2];
			else o[0] = o[1] = o[2] = v[0];
		}
	}
	free(prev);
	free(z);
}

static int tok(const uint8_t *d, size_t n, size_t *p)
{
	int v = 0;
	while (*p < n && (d[*p] == ' ' || d[*p] == '\n' || d[*p] == '\r' || d[*p] == '\t' || d[*p] == '#')) {
		if (d[*p] == '#')
			while (*p < n && d[*p] != '\n') (*p)++;
		else (*p)++;
	}
	if (*p >= n || d[*p] < '0' || d[*p] > '9') return -1;
	while (*p < n && d[*p] >= '0' && d[*p] <= '9') v = v * 10 + d[(*p)++] - '0';
	return v;
}

void image_load(Image *im, const char *path)
{
	FILE *f = fopen(path, "rb");
	if (!f) die("cannot open %s", path);
	fseek(f, 0, SEEK_END);
	size_t n = ftell(f);
	fseek(f, 0, SEEK_SET);
	uint8_t *d = xmalloc(n);
	if (fread(d, 1, n, f) != n) die("read error %s", path);
	fclose(f);
	if (n > 8 && !memcmp(d, "\x89PNG\r\n\x1a\n", 8)) load_png(im, d, n);
	else if (n > 2 && d[0] == 'P' && (d[1] == '5' || d[1] == '6')) {
		size_t p = 2;
		int w = tok(d, n, &p), h = tok(d, n, &p), mv = tok(d, n, &p);
		p++;
		int ch = d[1] == '6' ? 3 : 1, b = mv > 255 ? 2 : 1;
		if (w <= 0 || h <= 0 || mv <= 0 || p + (size_t)w * h * ch * b > n) die("%s: bad PNM", path);
		im->w = w;
		im->h = h;
		im->px = xmalloc(sizeof(float) * 3 * w * h);
		for (size_t i = 0; i < (size_t)w * h; i++)
			for (int k = 0; k < 3; k++) {
				size_t j = i * ch + (ch == 3 ? k : 0);
				float v = b == 2 ? (d[p + 2 * j] << 8 | d[p + 2 * j + 1]) : d[p + j];
				im->px[3 * i + k] = v / mv;
			}
	} else die("%s: unsupported image (PNG or binary PPM/PGM)", path);
	free(d);
}
