/* raw sensor input: MIPI RAW10/RAW12 packed, RAW8, 16-bit unpacked, 16-bit PGM */
#include "rawio.h"
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <pthread.h>

long raw_min_stride(int fmt, int w)
{
	switch (fmt) {
	case FMT_RAW10: return (w * 10L + 7) / 8;
	case FMT_RAW12: return (w * 12L + 7) / 8;
	case FMT_RAW8: return w;
	default: return w * 2L;
	}
}

static int read_tok(FILE *f)
{
	int c, v = 0;
	do {
		c = fgetc(f);
		if (c == '#')
			while (c != '\n' && c != EOF) c = fgetc(f);
	} while (c == ' ' || c == '\n' || c == '\r' || c == '\t');
	if (c < '0' || c > '9') return -1;
	while (c >= '0' && c <= '9') {
		v = v * 10 + (c - '0');
		c = fgetc(f);
	}
	return v;
}

void rawin_open(RawIn *r, const char *path)
{
	r->fd = open(path, O_RDONLY);
	if (r->fd < 0) die("cannot open %s", path);
	struct stat st;
	fstat(r->fd, &st);
	if (r->fmt == FMT_PGM) {
		FILE *f = fopen(path, "rb");
		if (!f || fgetc(f) != 'P' || fgetc(f) != '5') die("%s: not a binary PGM", path);
		r->w = read_tok(f);
		r->h = read_tok(f);
		int maxv = read_tok(f);
		if (r->w <= 0 || r->h <= 0 || maxv <= 0) die("%s: bad PGM header", path);
		r->offset = ftell(f);
		fclose(f);
		r->be16 = 1;
		r->stride = r->w * (maxv > 255 ? 2L : 1L);
		if (maxv <= 255) r->fmt = FMT_RAW8;
		else r->fmt = FMT_RAW16;
	}
	if (r->w <= 0 || r->h <= 0) die("image size not given (--size WxH or --preset)");
	long minst = raw_min_stride(r->fmt, r->w);
	if (r->stride <= 0) {
		/* guess the stride from the file size (padded DMA lines) */
		long long body = st.st_size - r->offset;
		if (body > 0 && body % r->h == 0 && body / r->h >= minst && body / r->h < minst + 4096)
			r->stride = (long)(body / r->h);
		else
			r->stride = minst;
	}
	if (r->stride < minst) die("stride %ld < minimum %ld for width %d", r->stride, minst, r->w);
	if ((long long)r->stride * (r->h - 1) + minst + r->offset > st.st_size)
		die("%s: file too small (%lld bytes) for %dx%d stride %ld offset %lld", path, (long long)st.st_size, r->w,
		    r->h, r->stride, r->offset);
	logv("input %s: %dx%d fmt %d stride %ld offset %lld\n", path, r->w, r->h, r->fmt, r->stride, r->offset);
}

void rawin_read_rows(RawIn *r, int y0, int n, uint16_t *dst)
{
	size_t bytes = (size_t)r->stride * n;
	uint8_t *buf = xmalloc(bytes);
	size_t got = 0;
	off_t off = r->offset + (off_t)r->stride * y0;
	while (got < bytes) {
		ssize_t k = pread(r->fd, buf + got, bytes - got, off + got);
		if (k <= 0) {
			/* last row may be short (no padding after it) */
			if (got >= bytes - r->stride + (size_t)raw_min_stride(r->fmt, r->w)) break;
			die("read error at row %d", y0);
		}
		got += k;
	}
	int w = r->w;
	for (int j = 0; j < n; j++) {
		const uint8_t *s = buf + (size_t)r->stride * j;
		uint16_t *d = dst + (size_t)w * j;
		switch (r->fmt) {
		case FMT_RAW10: {
			int x = 0;
			for (; x + 4 <= w; x += 4, s += 5) {
				uint8_t lo = s[4];
				d[x] = (s[0] << 2) | (lo & 3);
				d[x + 1] = (s[1] << 2) | ((lo >> 2) & 3);
				d[x + 2] = (s[2] << 2) | ((lo >> 4) & 3);
				d[x + 3] = (s[3] << 2) | ((lo >> 6) & 3);
			}
			for (int k = 0; x < w; x++, k++) d[x] = (s[k] << 2) | ((s[4] >> (2 * k)) & 3);
			break;
		}
		case FMT_RAW12: {
			int x = 0;
			for (; x + 2 <= w; x += 2, s += 3) {
				d[x] = (s[0] << 4) | (s[2] & 15);
				d[x + 1] = (s[1] << 4) | (s[2] >> 4);
			}
			if (x < w) d[x] = (s[0] << 4) | (s[2] & 15);
			break;
		}
		case FMT_RAW8:
			for (int x = 0; x < w; x++) d[x] = s[x];
			break;
		default:
			for (int x = 0; x < w; x++) {
				unsigned v = r->be16 ? (s[2 * x] << 8) | s[2 * x + 1] : s[2 * x] | (s[2 * x + 1] << 8);
				d[x] = v >> r->shift16;
			}
		}
	}
	free(buf);
}

/* ---- source stage ---- */
static void src_rows(Stage *s, int y0, int y1)
{
	RawIn *r = s->p;
	int w = r->w;
	uint16_t *tmp = xmalloc(sizeof(uint16_t) * w * (y1 - y0));
	rawin_read_rows(r, y0, y1 - y0, tmp);
	float scale[3], off[3];
	for (int c = 0; c < 3; c++) {
		float range = r->white - r->black[c];
		if (range < 1) range = 1;
		scale[c] = r->gain[c] / range;
		off[c] = r->black[c];
	}
	for (int y = y0; y < y1; y++) {
		float *o = srow_w(s, y);
		const uint16_t *t = tmp + (size_t)w * (y - y0);
		/* colour per x repeats with period 2n: build it once per row */
		int per = 2 * r->geo.n, col[6];
		for (int k = 0; k < per; k++) col[k] = geo_color(&r->geo, k, y);
		for (int x = 0, k = 0; x < w; x++) {
			int c = col[k];
			o[x] = (t[x] - off[c]) * scale[c];
			if (++k == per) k = 0;
		}
	}
	free(tmp);
}

Stage *rawin_stage(RawIn *r)
{
	return stage_new("raw", r->w, r->h, 1, 2 * r->geo.n, src_rows, r);
}

/* ---- gray-world statistics ---- */
typedef struct {
	RawIn *r;
	int pieces;
	double (*acc)[4]; /* sum R, G, B per piece + count */
	double (*cnt)[3];
} MeanCtx;

static void mean_piece(void *vc, int a, int b)
{
	MeanCtx *m = vc;
	RawIn *r = m->r;
	int w = r->w;
	uint16_t *tmp = xmalloc(sizeof(uint16_t) * w * 2);
	for (int p = a; p < b; p++) {
		int y0 = (int)((long long)r->h * p / m->pieces), y1 = (int)((long long)r->h * (p + 1) / m->pieces);
		double sum[3] = { 0 }, n[3] = { 0 };
		for (int y = y0; y < y1; y++) {
			rawin_read_rows(r, y, 1, tmp);
			for (int x = 0; x < w; x++) {
				int c = geo_color(&r->geo, x, y);
				float v = (tmp[x] - r->black[c]) / (r->white - r->black[c]);
				/* skip clipped and deep-shadow pixels */
				if (v > 0.002f && v < 0.92f) {
					sum[c] += v;
					n[c] += 1;
				}
			}
		}
		for (int c = 0; c < 3; c++) {
			m->acc[p][c] = sum[c];
			m->cnt[p][c] = n[c];
		}
	}
	free(tmp);
}

double rawin_means(RawIn *r, double mean[3])
{
	MeanCtx m = { r, 256, NULL, NULL };
	m.acc = xcalloc(m.pieces, sizeof *m.acc);
	m.cnt = xcalloc(m.pieces, sizeof *m.cnt);
	pool_for(m.pieces, 1, mean_piece, &m);
	double s[3] = { 0 }, n[3] = { 0 };
	for (int p = 0; p < m.pieces; p++)
		for (int c = 0; c < 3; c++) {
			s[c] += m.acc[p][c];
			n[c] += m.cnt[p][c];
		}
	for (int c = 0; c < 3; c++) mean[c] = n[c] > 0 ? s[c] / n[c] : 0;
	free(m.acc);
	free(m.cnt);
	return n[0] + n[1] + n[2];
}

/* ---- layout detection ----
 * Same-colour neighbours differ less than different-colour ones. Horizontal and vertical
 * neighbour differences binned by coordinate mod 6 show where the colour changes:
 * Bayer = every step, tetracell = every 2nd, nonacell = every 3rd. */
void rawin_detect(RawIn *r)
{
	int w = r->w, h = r->h;
	double dx[6] = { 0 }, dy[6] = { 0 }, nx[6] = { 0 }, ny[6] = { 0 };
	uint16_t *a = xmalloc(sizeof(uint16_t) * w * 2);
	int step = h > 2400 ? h / 1200 : 1;
	for (int y = 0; y + 1 < h; y += step) {
		rawin_read_rows(r, y, 2, a);
		uint16_t *p = a, *q = a + w;
		for (int x = 0; x + 1 < w; x++) {
			dx[x % 6] += abs(p[x + 1] - p[x]);
			nx[x % 6]++;
			dy[y % 6] += abs(q[x] - p[x]);
			ny[y % 6]++;
		}
	}
	printf("mean |p(x+1)-p(x)| by x mod 6:");
	for (int k = 0; k < 6; k++) printf(" %.2f", dx[k] / nx[k]);
	printf("\nmean |p(y+1)-p(y)| by y mod 6:");
	for (int k = 0; k < 6; k++) printf(" %.2f", dy[k] / ny[k]);
	printf("\n");
	/* score each block size n by how much larger the boundary steps are */
	int best_n = 1, best_px = 0, best_py = 0;
	double best = 1.08; /* need at least 8% contrast to call it a cell pattern */
	for (int n = 2; n <= 3; n++)
		for (int ph = 0; ph < n; ph++) {
			/* boundary between x and x+1 when (x + 1 + ph) % n == 0 */
			double bx = 0, ix = 0, by = 0, iy = 0;
			int nb = 0, ni = 0;
			for (int k = 0; k < 6; k++) {
				if ((k + 1 + ph) % n == 0) bx += dx[k] / nx[k], by += dy[k] / ny[k], nb++;
				else ix += dx[k] / nx[k], iy += dy[k] / ny[k], ni++;
			}
			double score = (bx / nb + by / nb) / (ix / ni + iy / ni + 1e-9);
			if (score > best * 1.02) {
				best = score;
				best_n = n;
				best_px = best_py = ph;
			}
		}
	if (best_n > 1) {
		/* refine x and y phases separately */
		double bxs = -1, bys = -1;
		for (int ph = 0; ph < best_n; ph++) {
			double bx = 0, ix = 0, by = 0, iy = 0;
			for (int k = 0; k < 6; k++)
				if ((k + 1 + ph) % best_n == 0) bx += dx[k] / nx[k], by += dy[k] / ny[k];
				else ix += dx[k] / nx[k], iy += dy[k] / ny[k];
			if (bx / (ix + 1e-9) > bxs) bxs = bx / (ix + 1e-9), best_px = ph;
			if (by / (iy + 1e-9) > bys) bys = by / (iy + 1e-9), best_py = ph;
		}
	}
	printf("layout: cell %d (%s), block phase x %d y %d (pixel x=%d starts a block), contrast %.2f\n", best_n,
	       best_n == 3 ? "nonacell" : best_n == 2 ? "tetracell" : "Bayer", best_px, best_py,
	       (best_n - best_px) % best_n, best);
	/* which diagonal of the 2x2 block quad holds the greens: the two green blocks match */
	int n = best_n;
	double d0 = 0, d1 = 0, m[2][2] = { { 0 } };
	long cnt = 0;
	for (int y = 0; y + 2 * n <= h; y += step * 2 * n > 2 * n ? step * 2 * n : 2 * n) {
		/* first full tile row starts at phase-adjusted origin */
		int y0 = y + (n - best_py) % n;
		if (y0 + 2 * n > h) break;
		uint16_t *t = xmalloc(sizeof(uint16_t) * w * 2 * n);
		rawin_read_rows(r, y0, 2 * n, t);
		for (int x = (n - best_px) % n; x + 2 * n <= w; x += 2 * n) {
			double b[2][2] = { { 0 } };
			for (int j = 0; j < 2 * n; j++)
				for (int i = 0; i < 2 * n; i++) b[j / n][i / n] += t[(size_t)j * w + x + i];
			d0 += fabs(b[0][0] - b[1][1]);
			d1 += fabs(b[0][1] - b[1][0]);
			for (int j = 0; j < 2; j++)
				for (int i = 0; i < 2; i++) m[j][i] += b[j][i] / (n * n);
			cnt++;
		}
		free(t);
	}
	if (cnt) {
		printf("block quad means (relative to that origin): %.1f %.1f / %.1f %.1f\n", m[0][0] / cnt, m[0][1] / cnt,
		       m[1][0] / cnt, m[1][1] / cnt);
		printf("greens on the %s diagonal (|TL-BR| %.1f vs |TR-BL| %.1f): CFA is %s from that origin\n",
		       d0 < d1 ? "main" : "anti", d0 / cnt / (n * n), d1 / cnt / (n * n),
		       d0 < d1 ? "GRBG or GBRG" : "RGGB or BGGR");
		printf("(R vs B cannot be told apart from statistics: check with a red/blue target)\n");
		/* translate to rawtool options: with --phase p (< n) the first full block, at
		 * s = (n - p) % n, is tile column/row 1 when s > 0, so the quad seen from s is the
		 * tile with columns (and rows) swapped */
		int sx = (n - best_px) % n, sy = (n - best_py) % n;
		int ppx = (n - sx) % n, ppy = (n - sy) % n;
		const char *quads[2][2] = { { "grbg", "gbrg" }, { "rggb", "bggr" } };
		printf("suggest: --cell %d --phase %d,%d --cfa", n, ppx, ppy);
		for (int k = 0; k < 2; k++) {
			Cfa q, t;
			cfa_parse(quads[d0 < d1 ? 0 : 1][k], &q);
			for (int j = 0; j < 2; j++)
				for (int i = 0; i < 2; i++) t.c[j][i] = q.c[sy ? 1 - j : j][sx ? 1 - i : i];
			char nm[5];
			cfa_name(&t, nm);
			for (int c = 0; c < 4; c++) nm[c] |= 0x20;
			printf("%s %s", k ? " or" : "", nm);
		}
		printf("\n");
	}
	free(a);
}

/* ---- raw dump as 16-bit PGM (sensor values as read, no processing) ---- */
void rawin_unpack_pgm(RawIn *r, const char *path)
{
	FILE *f = fopen(path, "wb");
	if (!f) die("cannot create %s", path);
	fprintf(f, "P5\n%d %d\n65535\n", r->w, r->h);
	int rows = 64;
	uint16_t *t = xmalloc(sizeof(uint16_t) * r->w * rows);
	uint8_t *b = xmalloc(2 * (size_t)r->w * rows);
	for (int y = 0; y < r->h; y += rows) {
		int n = r->h - y < rows ? r->h - y : rows;
		rawin_read_rows(r, y, n, t);
		for (size_t i = 0; i < (size_t)r->w * n; i++) {
			b[2 * i] = t[i] >> 8;
			b[2 * i + 1] = t[i] & 255;
		}
		fwrite(b, 2, (size_t)r->w * n, f);
	}
	free(t);
	free(b);
	if (fclose(f)) die("write error on %s", path);
}
