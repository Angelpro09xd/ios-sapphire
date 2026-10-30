/* rawtool: raw sensor dump -> DNG / JPEG / PNG / PPM for the sapphire cameras.
 * Run `rawtool -h` for the options; see README.md. */
#include "rawio.h"
#include "remosaic.h"
#include "color.h"
#include "out.h"

static void usage(void)
{
	puts("usage: rawtool [options] INPUT            process a raw dump\n"
	     "       rawtool detect [input options] INPUT  guess cell layout/phase/green diagonal\n"
	     "       rawtool psnr A.ppm B.ppm [--border N]  compare two PPM/PGM images\n"
	     "\n"
	     "input:\n"
	     "  --preset NAME      hm6 (12000x9000 nonacell), hm6-bin (4000x3000), hm6-crop (4000x3000 nonacell),\n"
	     "                     4h7, ov16a1q, gc16b3c, sc202 (sets size, cell, cfa, black, white, fmt)\n"
	     "  --size WxH         image size in pixels\n"
	     "  --fmt F            raw10 (MIPI packed, default), raw12, raw8, raw16 (LE), raw16be, pgm\n"
	     "  --stride N         bytes per line (default: from file size, else unpadded)\n"
	     "  --offset N         bytes to skip at the start of the file\n"
	     "  --shift N          raw16: right shift (MSB-aligned data)\n"
	     "  --cfa ORDER        Bayer order of the colour blocks: rggb grbg gbrg bggr\n"
	     "  --cell N           1 Bayer, 2 tetracell, 3 nonacell\n"
	     "  --phase X,Y        image origin inside the 2N x 2N tile (default 0,0)\n"
	     "  --black N|R,Gr,Gb,B  black level (sensor units)   --white N  white level\n"
	     "processing:\n"
	     "  --wb auto|R,G,B    white balance gains (auto = gray world)\n"
	     "  --ccm M            identity (default), typical, or 9 numbers row-major (camera -> sRGB)\n"
	     "  --exposure EV      --gamma srgb|linear|G   --denoise S (0 off, 1 light)   --sharpen A (0 off)\n"
	     "  --demosaic mhc|bilinear  (Bayer input or --via-bayer)\n"
	     "  --via-bayer        nonacell: remosaic to Bayer then demosaic (default: RGB straight from the remosaic)\n"
	     "  --out-cfa ORDER    Bayer order written to the DNG/--bayer for cell sensors (default rggb)\n"
	     "  --rm-sigma A,B     remosaic chroma guide (relative, absolute)   --rm-eps E  direction regulariser\n"
	     "  --scale K          box-downscale the RGB outputs by K\n"
	     "outputs:\n"
	     "  --dng F  --jpeg F  --quality Q  --png F  --ppm F  --bits 8|16  --bayer F.pgm  --unpack F.pgm\n"
	     "  --preview F.jpg    fast path: bin cells (or 2x2 for Bayer), demosaic, JPEG\n"
	     "  --preview-scale K  extra downscale of the preview (default 1)\n"
	     "  --make S --model S  DNG camera names\n"
	     "misc:  --threads N  --chunk ROWS (default 64)  -v");
	exit(2);
}

static void parse_size(const char *s, int *w, int *h)
{
	if (sscanf(s, "%dx%d", w, h) != 2 || *w <= 0 || *h <= 0) die("bad size %s", s);
}

static int parse_floats(const char *s, float *v, int max)
{
	int n = 0;
	while (n < max && *s) {
		char *e;
		v[n++] = strtof(s, &e);
		if (e == s) die("bad number list %s", s);
		s = e;
		if (*s == ',') s++;
	}
	return n;
}

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

/* ---- PSNR between two PNM files, streamed ---- */
typedef struct {
	FILE *f;
	int w, h, ch, maxv;
} Pnm;

static int pnm_tok(FILE *f)
{
	int c, v = 0;
	do {
		c = fgetc(f);
		if (c == '#')
			while (c != '\n' && c != EOF) c = fgetc(f);
	} while (c == ' ' || c == '\n' || c == '\r' || c == '\t');
	if (c < '0' || c > '9') return -1;
	while (c >= '0' && c <= '9') {
		v = v * 10 + c - '0';
		c = fgetc(f);
	}
	return v;
}

static void pnm_open(Pnm *p, const char *path)
{
	p->f = fopen(path, "rb");
	if (!p->f) die("cannot open %s", path);
	if (fgetc(p->f) != 'P') die("%s: not PNM", path);
	int t = fgetc(p->f);
	if (t != '5' && t != '6') die("%s: only binary PGM/PPM", path);
	p->ch = t == '6' ? 3 : 1;
	p->w = pnm_tok(p->f);
	p->h = pnm_tok(p->f);
	p->maxv = pnm_tok(p->f);
	if (p->w <= 0 || p->h <= 0 || p->maxv <= 0) die("%s: bad header", path);
}

static void pnm_row(Pnm *p, float *o, uint8_t *tmp)
{
	int n = p->w * p->ch, b = p->maxv > 255 ? 2 : 1;
	if (fread(tmp, b, n, p->f) != (size_t)n) die("short PNM");
	for (int i = 0; i < n; i++) o[i] = (b == 2 ? (tmp[2 * i] << 8 | tmp[2 * i + 1]) : tmp[i]) / (float)p->maxv;
}

static int cmd_psnr(int argc, char **argv)
{
	const char *pa = NULL, *pb = NULL;
	int border = 8;
	for (int i = 2; i < argc; i++) {
		if (!strcmp(argv[i], "--border") && i + 1 < argc) border = atoi(argv[++i]);
		else if (!pa) pa = argv[i];
		else if (!pb) pb = argv[i];
		else usage();
	}
	if (!pb) usage();
	Pnm a, b;
	pnm_open(&a, pa);
	pnm_open(&b, pb);
	if (a.w != b.w || a.h != b.h || a.ch != b.ch)
		die("size mismatch: %dx%dx%d vs %dx%dx%d", a.w, a.h, a.ch, b.w, b.h, b.ch);
	float *ra = xmalloc(sizeof(float) * a.w * a.ch), *rb = xmalloc(sizeof(float) * a.w * a.ch);
	uint8_t *tmp = xmalloc(2 * (size_t)a.w * a.ch);
	double se[3] = { 0 };
	long long n = 0;
	for (int y = 0; y < a.h; y++) {
		pnm_row(&a, ra, tmp);
		pnm_row(&b, rb, tmp);
		if (y < border || y >= a.h - border) continue;
		for (int x = border; x < a.w - border; x++) {
			for (int c = 0; c < a.ch; c++) {
				double d = ra[x * a.ch + c] - rb[x * a.ch + c];
				se[c] += d * d;
			}
			n++;
		}
	}
	double tot = 0;
	printf("PSNR");
	for (int c = 0; c < a.ch; c++) {
		double mse = se[c] / n;
		tot += mse;
		printf(" %s %.2f", a.ch == 3 ? (c == 0 ? "R" : c == 1 ? "G" : "B") : "Y", mse > 0 ? 10 * log10(1 / mse) : 99.0);
	}
	tot /= a.ch;
	printf("  all %.2f dB\n", tot > 0 ? 10 * log10(1 / tot) : 99.0);
	return 0;
}

/* ---- main ---- */
int main(int argc, char **argv)
{
	if (argc < 2) usage();
	if (!strcmp(argv[1], "psnr")) return cmd_psnr(argc, argv);
	int detect = !strcmp(argv[1], "detect");

	RawIn r;
	memset(&r, 0, sizeof r);
	r.fmt = FMT_RAW10;
	r.white = 1023;
	r.geo.n = 1;
	cfa_parse("rggb", &r.geo.cfa);
	float black4[4] = { 64, 64, 64, 64 };
	const char *input = NULL, *wb = "auto", *ccm_s = "identity", *gamma = "srgb", *dem = "mhc";
	const char *o_dng = NULL, *o_jpeg = NULL, *o_png = NULL, *o_ppm = NULL, *o_bayer = NULL, *o_unpack = NULL,
		   *o_prev = NULL;
	const char *make = "Xiaomi", *model = "Redmi Note 13", *sensor = "";
	int quality = 92, bits = 8, threads = ncpus(), chunk = 64, via_bayer = 0, scale = 1, pscale = 1;
	float exposure_ev = 0, denoise = 0, sharpen = 0;
	RemosaicParams rp = { 0.04f, 0.002f, 0.002f, 1, 0.4f, 0 };
	Cfa out_cfa;
	cfa_parse("rggb", &out_cfa);

	for (int i = detect ? 2 : 1; i < argc; i++) {
		const char *a = argv[i];
#define VAL() (i + 1 < argc ? argv[++i] : (die("%s needs a value", a), ""))
		if (!strcmp(a, "-h") || !strcmp(a, "--help")) usage();
		else if (!strcmp(a, "-v")) g_verbose = 1;
		else if (!strcmp(a, "--preset")) {
			const char *p = VAL();
			int w, h, n = 1;
			if (!strcmp(p, "hm6")) w = 12000, h = 9000, n = 3, sensor = "S5KHM6";
			else if (!strcmp(p, "hm6-bin")) w = 4000, h = 3000, sensor = "S5KHM6";
			else if (!strcmp(p, "hm6-crop")) w = 4000, h = 3000, n = 3, sensor = "S5KHM6";
			else if (!strcmp(p, "4h7")) w = 3264, h = 2448, sensor = "S5K4H7";
			else if (!strcmp(p, "ov16a1q")) w = 2304, h = 1728, sensor = "OV16A1Q";
			else if (!strcmp(p, "gc16b3c")) w = 2304, h = 1728, sensor = "GC16B3C";
			else if (!strcmp(p, "sc202")) w = 1600, h = 1200, sensor = "SC202PCS";
			else die("unknown preset %s", p);
			r.w = w, r.h = h, r.geo.n = n;
			r.fmt = FMT_RAW10;
			r.white = 1023;
			/* CamX sensor modules: colorFilterArrangement 2 (GRBG), black 0x40 (SC202: 0x41) */
			cfa_parse("grbg", &r.geo.cfa);
			for (int k = 0; k < 4; k++) black4[k] = !strcmp(p, "sc202") ? 65 : 64;
		} else if (!strcmp(a, "--size")) parse_size(VAL(), &r.w, &r.h);
		else if (!strcmp(a, "--fmt")) {
			const char *f = VAL();
			r.be16 = 0;
			if (!strcmp(f, "raw10")) r.fmt = FMT_RAW10;
			else if (!strcmp(f, "raw12")) r.fmt = FMT_RAW12;
			else if (!strcmp(f, "raw8")) r.fmt = FMT_RAW8;
			else if (!strcmp(f, "raw16")) r.fmt = FMT_RAW16;
			else if (!strcmp(f, "raw16be")) r.fmt = FMT_RAW16, r.be16 = 1;
			else if (!strcmp(f, "pgm")) r.fmt = FMT_PGM;
			else die("unknown format %s", f);
		} else if (!strcmp(a, "--stride")) r.stride = atol(VAL());
		else if (!strcmp(a, "--offset")) r.offset = atoll(VAL());
		else if (!strcmp(a, "--shift")) r.shift16 = atoi(VAL());
		else if (!strcmp(a, "--cfa")) {
			if (cfa_parse(VAL(), &r.geo.cfa)) die("bad --cfa");
		} else if (!strcmp(a, "--out-cfa")) {
			if (cfa_parse(VAL(), &out_cfa)) die("bad --out-cfa");
		} else if (!strcmp(a, "--cell")) {
			r.geo.n = atoi(VAL());
			if (r.geo.n < 1 || r.geo.n > 3) die("--cell must be 1, 2 or 3");
		} else if (!strcmp(a, "--phase")) {
			if (sscanf(VAL(), "%d,%d", &r.geo.px, &r.geo.py) != 2) die("bad --phase");
		} else if (!strcmp(a, "--black")) {
			float v[4];
			int n = parse_floats(VAL(), v, 4);
			for (int k = 0; k < 4; k++) black4[k] = n == 4 ? v[k] : v[0];
		} else if (!strcmp(a, "--white")) r.white = atof(VAL());
		else if (!strcmp(a, "--wb")) wb = VAL();
		else if (!strcmp(a, "--ccm")) ccm_s = VAL();
		else if (!strcmp(a, "--exposure")) exposure_ev = atof(VAL());
		else if (!strcmp(a, "--gamma")) gamma = VAL();
		else if (!strcmp(a, "--denoise")) denoise = atof(VAL());
		else if (!strcmp(a, "--sharpen")) sharpen = atof(VAL());
		else if (!strcmp(a, "--demosaic")) dem = VAL();
		else if (!strcmp(a, "--via-bayer")) via_bayer = 1;
		else if (!strcmp(a, "--rm-sigma")) {
			float v[2];
			if (parse_floats(VAL(), v, 2) != 2) die("--rm-sigma A,B");
			rp.sigma_rel = v[0], rp.sigma_abs = v[1];
		} else if (!strcmp(a, "--rm-eps")) rp.eps = atof(VAL());
		else if (!strcmp(a, "--rm-iter")) rp.iter = atoi(VAL());
		else if (!strcmp(a, "--rm-blend")) rp.blend = atof(VAL());
		else if (!strcmp(a, "--remosaic")) {
			const char *v = VAL();
			if (!strcmp(v, "simple")) rp.simple = 1;
			else if (strcmp(v, "guided")) die("--remosaic guided|simple");
		}
		else if (!strcmp(a, "--scale")) scale = atoi(VAL());
		else if (!strcmp(a, "--dng")) o_dng = VAL();
		else if (!strcmp(a, "--jpeg")) o_jpeg = VAL();
		else if (!strcmp(a, "--quality")) quality = atoi(VAL());
		else if (!strcmp(a, "--png")) o_png = VAL();
		else if (!strcmp(a, "--ppm")) o_ppm = VAL();
		else if (!strcmp(a, "--bits")) bits = atoi(VAL()) == 16 ? 16 : 8;
		else if (!strcmp(a, "--bayer")) o_bayer = VAL();
		else if (!strcmp(a, "--unpack")) o_unpack = VAL();
		else if (!strcmp(a, "--preview")) o_prev = VAL();
		else if (!strcmp(a, "--preview-scale")) pscale = atoi(VAL());
		else if (!strcmp(a, "--make")) make = VAL();
		else if (!strcmp(a, "--model")) model = VAL();
		else if (!strcmp(a, "--threads")) threads = atoi(VAL());
		else if (!strcmp(a, "--chunk")) chunk = atoi(VAL());
		else if (a[0] == '-' && a[1]) die("unknown option %s (try -h)", a);
		else if (!input) input = a;
		else die("more than one input");
#undef VAL
	}
	if (!input) usage();
	if (chunk < 16) chunk = 16;
	chunk = (chunk + 15) / 16 * 16;
	g_chunk = chunk;
	pool_init(threads);
	double t_start = now_sec();

	Geo *g = &r.geo;
	g->px = ((g->px % (2 * g->n)) + 2 * g->n) % (2 * g->n);
	g->py = ((g->py % (2 * g->n)) + 2 * g->n) % (2 * g->n);
	/* per-colour black: R, Gr (green next to red), Gb, B */
	r.black[CR] = black4[0];
	r.black[CG] = 0.5f * (black4[1] + black4[2]);
	r.black[CB] = black4[3];
	rawin_open(&r, input);

	if (detect) {
		rawin_detect(&r);
		return 0;
	}
	if (o_unpack) rawin_unpack_pgm(&r, o_unpack);
	if (!o_dng && !o_jpeg && !o_png && !o_ppm && !o_bayer && !o_prev) {
		if (o_unpack) return 0;
		die("no output requested (--dng/--jpeg/--png/--ppm/--bayer/--preview)");
	}

	/* white balance */
	float gain[3] = { 1, 1, 1 };
	double t_wb = now_sec();
	if (!strcmp(wb, "auto")) {
		double m[3];
		rawin_means(&r, m);
		for (int c = 0; c < 3; c++) gain[c] = m[c] > 1e-6 ? (float)(m[CG] / m[c]) : 1;
		logv("gray world means R %.4f G %.4f B %.4f\n", m[0], m[1], m[2]);
	} else if (parse_floats(wb, gain, 3) != 3) die("--wb auto or R,G,B");
	float gmin = fminf(gain[0], fminf(gain[1], gain[2]));
	for (int c = 0; c < 3; c++) gain[c] /= gmin;
	memcpy(r.gain, gain, sizeof gain);
	t_wb = now_sec() - t_wb;

	/* colour matrix */
	float ccm[9] = { 1, 0, 0, 0, 1, 0, 0, 0, 1 };
	if (!strcmp(ccm_s, "typical")) {
		/* generic phone-sensor correction (approximation until the module is calibrated) */
		static const float t[9] = { 1.62f, -0.48f, -0.14f, -0.24f, 1.52f, -0.28f, -0.04f, -0.56f, 1.60f };
		memcpy(ccm, t, sizeof ccm);
	} else if (strcmp(ccm_s, "identity") && parse_floats(ccm_s, ccm, 9) != 9) die("--ccm needs 9 numbers");

	/* ---- graph ---- */
	Stage *raw = rawin_stage(&r);
	Stage *rgbfull = NULL, *bayer = NULL, *rgb = NULL;
	Cfa bcfa;
	int want_rgb = o_jpeg || o_png || o_ppm;
	int want_full = want_rgb || o_dng || o_bayer;
	if (g->n > 1) {
		if (want_full) rgbfull = remosaic_rgb(raw, g, &rp);
		bcfa = out_cfa;
		if (o_dng || o_bayer || (want_rgb && via_bayer)) bayer = mosaic_sample(rgbfull, &bcfa);
	} else {
		bayer = raw;
		bcfa = geo_bayer_cfa(g);
	}
	if (want_rgb) {
		if (g->n > 1 && !via_bayer) rgb = rgbfull;
		else rgb = !strcmp(dem, "bilinear") ? demosaic_bilinear(bayer, &bcfa) : demosaic_mhc(bayer, &bcfa);
		if (denoise > 0) rgb = stage_denoise(rgb, denoise);
		rgb = stage_color(rgb, ccm, powf(2, exposure_ev), 1);
		if (sharpen > 0) rgb = stage_sharpen(rgb, sharpen);
		if (scale > 1) rgb = stage_downscale(rgb, scale);
	}
	Stage *prev = NULL;
	if (o_prev) {
		Cfa pc;
		Stage *pb;
		if (g->n > 1) pb = cell_bin(raw, g, &pc);
		else pb = raw, pc = bcfa;
		prev = demosaic_mhc(pb, &pc);
		if (g->n == 1) prev = stage_downscale(prev, 2);
		prev = stage_color(prev, ccm, powf(2, exposure_ev), 1);
		if (pscale > 1) prev = stage_downscale(prev, pscale);
	}

	Tone *tone = xmalloc(sizeof *tone);
	tone_init(tone, gamma);
	Sink *sinks[8];
	int ns = 0;
	if (o_dng) {
		DngInfo di;
		memset(&di, 0, sizeof di);
		di.cfa = bcfa;
		memcpy(di.gain, gain, sizeof gain);
		di.black = r.black[CG] * 64;
		di.white = r.white * 64;
		for (int c = 0; c < 3; c++) di.neutral[c] = gain[CG] / gain[c];
		/* ColorMatrix1 (XYZ -> camera) assuming CCM maps camera to linear sRGB (D65) */
		static const float xyz2srgb[9] = { 3.2404542f, -1.5371385f, -0.4985314f, -0.9692660f, 1.8760108f,
						   0.0415560f, 0.0556434f,  -0.2040259f, 1.0572252f };
		float inv[9];
		mat3_inv(ccm, inv);
		mat3_mul(inv, xyz2srgb, di.colmat);
		static char uniq[96], desc[160];
		snprintf(uniq, sizeof uniq, "%s %s%s%s", make, model, sensor[0] ? " " : "", sensor);
		snprintf(desc, sizeof desc, "%s%s%sraw", sensor, sensor[0] ? " " : "",
			 g->n == 3 ? "nonacell remosaiced to Bayer, " : g->n == 2 ? "tetracell remosaiced to Bayer, " : "");
		di.make = make;
		di.model = model;
		di.unique = uniq;
		di.software = "sapphire rawtool 1.0";
		di.desc = desc;
		sinks[ns++] = sink_dng(bayer, o_dng, &di);
	}
	if (o_bayer) sinks[ns++] = sink_pgm(bayer, o_bayer, &bcfa, gain);
	if (o_jpeg) sinks[ns++] = sink_jpeg(rgb, o_jpeg, quality, tone);
	if (o_png) sinks[ns++] = sink_png(rgb, o_png, bits, tone);
	if (o_ppm) sinks[ns++] = sink_ppm(rgb, o_ppm, bits, tone);
	if (o_prev) sinks[ns++] = sink_jpeg(prev, o_prev, quality, tone);

	size_t ringbytes = stages_alloc(chunk);
	double t0 = now_sec();
	sinks_run(sinks, ns);
	double t1 = now_sec();

	char cn[5], on[5];
	cfa_name(&g->cfa, cn);
	cfa_name(&bcfa, on);
	fprintf(stderr, "%dx%d %s cell %d -> %s | wb gains R %.3f G %.3f B %.3f | %d threads, chunk %d\n", r.w, r.h, cn,
		g->n, on, gain[0], gain[1], gain[2], pool_threads(), chunk);
	fprintf(stderr, "stage times (wall, all threads):\n");
	if (t_wb > 0.0005) fprintf(stderr, "  %-12s %13s %6.2f s\n", "wb-stats", "", t_wb);
	stages_report(stderr);
	for (int i = 0; i < ns; i++)
		fprintf(stderr, "  %-12s %5dx%-5d %6.2f s  %.1f MB\n", sinks[i]->name, sinks[i]->src->w, sinks[i]->h,
			sinks[i]->t, sinks[i]->bytes / 1048576.0);
	fprintf(stderr, "total %.2f s (pipeline %.2f s, %.1f MP/s) | rings %.0f MB, peak RSS %.0f MB\n", now_sec() - t_start,
		t1 - t0, (double)r.w * r.h / 1e6 / (t1 - t0), ringbytes / 1048576.0, peak_rss_kb() / 1024.0);
	return 0;
}
