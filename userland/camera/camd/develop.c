/* camd develop, see develop.h. The graph is rawtool's (rawtool.c main) with camd's choices:
 * guided remosaic (cell sensors) or MHC demosaic (Bayer), white balance from the preview, the
 * "typical" colour matrix (the same one the preview uses), sRGB, Exif in the JPEG. */
#include "develop.h"
#include "../rawio.h"
#include "../remosaic.h"
#include "../color.h"
#include "../out.h"
#include <pthread.h>
#include <stdarg.h>
#include <unistd.h>

static const float ccm_typical[9] = { 1.62f, -0.48f, -0.14f, -0.24f, 1.52f, -0.28f, -0.04f, -0.56f, 1.60f };

static void mat3_inv(const float *m, float *o)
{
	double a = m[0], b = m[1], c = m[2], d = m[3], e = m[4], f = m[5], g = m[6], h = m[7], i = m[8];
	double det = a * (e * i - f * h) - b * (d * i - f * g) + c * (d * h - e * g);
	if (fabs(det) < 1e-9) die("CCM is singular");
	double r[9] = { e * i - f * h, c * h - b * i, b * f - c * e, f * g - d * i, a * i - c * g,
			c * d - a * f, d * h - e * g, b * g - a * h, a * e - b * d };
	for (int k = 0; k < 9; k++) o[k] = (float)(r[k] / det);
}

static void mat3_mul(const float *a, const float *b, float *o)
{
	for (int i = 0; i < 3; i++)
		for (int j = 0; j < 3; j++) o[3 * i + j] = a[3 * i] * b[j] + a[3 * i + 1] * b[3 + j] + a[3 * i + 2] * b[6 + j];
}

static void say(int fd, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void say(int fd, const char *fmt, ...)
{
	if (fd < 0) return;
	char b[128];
	va_list ap;
	va_start(ap, fmt);
	int n = vsnprintf(b, sizeof b, fmt, ap);
	va_end(ap);
	if (n > 0) (void)!write(fd, b, (size_t)(n < (int)sizeof b ? n : (int)sizeof b - 1));
}

/* ---- progress: how far the source stage has been pulled ---- */
static Stage *g_src;
static int g_pfd = -1;
static volatile int g_done;

static void *progress_thread(void *arg)
{
	(void)arg;
	int last = -1;
	while (!g_done) {
		usleep(250000);
		if (!g_src) continue;
		int p = (int)((long long)g_src->hi * 100 / g_src->h);
		if (p > 99) p = 99;
		if (p != last) say(g_pfd, "P %d\n", p);
		last = p;
	}
	return NULL;
}

/* per-colour means of ~400 evenly spaced rows (linear, unclipped pixels only) */
static void sample_means(RawIn *r, double mean[3])
{
	uint16_t *t = xmalloc(sizeof(uint16_t) * r->w);
	double s[3] = { 0 }, n[3] = { 0 };
	int rows = 400, step = r->h / rows > 1 ? r->h / rows : 1;
	for (int y = step / 2; y < r->h; y += step) {
		rawin_read_rows(r, y, 1, t);
		for (int x = 0; x < r->w; x += 2) {
			int c = geo_color(&r->geo, x, y);
			float v = (t[x] - r->black[c]) / (r->white - r->black[c]);
			if (v > 0.002f && v < 0.92f) {
				s[c] += v;
				n[c] += 1;
			}
			c = geo_color(&r->geo, x + 1, y);
			v = (t[x + 1] - r->black[c]) / (r->white - r->black[c]);
			if (v > 0.002f && v < 0.92f) {
				s[c] += v;
				n[c] += 1;
			}
		}
	}
	for (int c = 0; c < 3; c++) mean[c] = n[c] > 0 ? s[c] / n[c] : 0;
	free(t);
}

int develop_run(const DevJob *j)
{
	g_pfd = j->progress_fd;
	g_chunk = j->chunk >= 16 ? (j->chunk + 15) / 16 * 16 : 32;
	pool_init(j->threads > 0 ? j->threads : ncpus());
	pthread_t pt;
	pthread_create(&pt, NULL, progress_thread, NULL);

	RawIn r;
	memset(&r, 0, sizeof r);
	r.fmt = FMT_RAW10;
	r.w = j->w;
	r.h = j->h;
	r.stride = j->stride;
	r.white = j->white;
	r.geo.n = j->cell;
	cfa_parse("grbg", &r.geo.cfa);
	for (int c = 0; c < 3; c++) r.black[c] = j->black;
	rawin_open(&r, j->raw);

	/* white balance and exposure check from a sample of the frame */
	double m[3];
	sample_means(&r, m);
	say(g_pfd, "CAL %.6f\n", m[CG]);
	float gain[3];
	if (j->wb[0] > 0 && j->wb[1] > 0 && j->wb[2] > 0) memcpy(gain, j->wb, sizeof gain);
	else
		for (int c = 0; c < 3; c++) gain[c] = m[c] > 1e-6 ? (float)(m[CG] / m[c]) : 1;
	float gmin = fminf(gain[0], fminf(gain[1], gain[2]));
	for (int c = 0; c < 3; c++) gain[c] /= gmin;
	memcpy(r.gain, gain, sizeof gain);
	float ev = j->ev;
	if (j->expect_g > 1e-4f && m[CG] > 1e-4) {
		/* the still came out brighter/darker than the preview said (mode sensitivity): fix in the JPEG */
		float fix = log2f(j->expect_g / (float)m[CG]);
		fix = fix < -1 ? -1 : fix > 1.5f ? 1.5f : fix;
		if (fabsf(fix) > 0.1f) ev += fix;
	}

	Stage *raw = rawin_stage(&r);
	g_src = raw;
	Stage *rgb, *bayer = NULL;
	Cfa bcfa;
	if (r.geo.n > 1) {
		RemosaicParams rp = { 0.04f, 0.002f, 0.002f, 1, 0.4f, 0 };
		rgb = remosaic_rgb(raw, &r.geo, &rp);
		cfa_parse("rggb", &bcfa);
		if (j->dng) bayer = mosaic_sample(rgb, &bcfa);
	} else {
		bayer = raw;
		bcfa = geo_bayer_cfa(&r.geo);
		rgb = demosaic_mhc(bayer, &bcfa);
	}
	rgb = stage_color(rgb, ccm_typical, powf(2, ev), 1);
	if (j->sharpen > 0) rgb = stage_sharpen(rgb, j->sharpen);

	Tone *tone = xmalloc(sizeof *tone);
	tone_init(tone, "srgb");
	Sink *sinks[2];
	int ns = 0;
	if (j->dng) {
		DngInfo di;
		memset(&di, 0, sizeof di);
		di.cfa = bcfa;
		memcpy(di.gain, gain, sizeof gain);
		di.black = r.black[CG] * 64;
		di.white = r.white * 64;
		for (int c = 0; c < 3; c++) di.neutral[c] = gain[CG] / gain[c];
		static const float xyz2srgb[9] = { 3.2404542f, -1.5371385f, -0.4985314f, -0.9692660f, 1.8760108f,
						   0.0415560f, 0.0556434f,  -0.2040259f, 1.0572252f };
		float inv[9];
		mat3_inv(ccm_typical, inv);
		mat3_mul(inv, xyz2srgb, di.colmat);
		static char uniq[96], desc[160];
		const char *sensor = j->sensor ? j->sensor : "S5KHM6";
		snprintf(uniq, sizeof uniq, "Xiaomi Redmi Note 13 %s", sensor);
		snprintf(desc, sizeof desc, "%s %sraw", sensor, r.geo.n == 3 ? "nonacell remosaiced to Bayer, " : "");
		di.make = "Xiaomi";
		di.model = "Redmi Note 13";
		di.unique = uniq;
		di.software = "sapphire camd 1.0";
		di.desc = desc;
		sinks[ns++] = sink_dng(bayer, j->dng, &di);
	}
	g_jpeg_app1 = j->app1;
	g_jpeg_app1_len = j->app1_len;
	sinks[ns++] = sink_jpeg(rgb, j->jpeg, j->quality > 0 ? j->quality : 92, tone);

	stages_alloc(g_chunk);
	sinks_run(sinks, ns);
	g_done = 1;
	pthread_join(pt, NULL);
	say(g_pfd, "P 100\n");
	return 0;
}
