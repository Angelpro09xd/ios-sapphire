/* camd preview: a raw RAW10 frame (packed MIPI, GRBG blocks) -> a small BGRX image + exposure /
 * white-balance statistics, in one pass over the frame.
 *
 * The frame is cut into square blocks of B x B pixels, B a multiple of the CFA tile (2 for Bayer,
 * 6 for the nonacell modes), so every block holds all three colours; each block's colour means
 * become one output pixel. That is the binned image the sensor would give, without demosaicing.
 * A small Bayer frame (mode 3, 1280x720) uses overlapping 2x2 quads instead (step 1), which keeps
 * its full resolution.
 */
#ifndef CAMD_PREVIEW_H
#define CAMD_PREVIEW_H
#include <stdint.h>
#include <stddef.h>

typedef struct {
	int w, h;          /* raw size in pixels */
	long stride;       /* bytes per raw line (packed RAW10: w * 5 / 4) */
	int cell;          /* 1 Bayer (binned modes), 3 nonacell (modes 0 and 4) */
	int black, white;  /* 10-bit levels */
} PvRaw;

typedef struct {
	float wb[3];       /* white-balance gains R, G, B */
	float gain;        /* digital gain on top (1 = none) */
	int rotate;        /* clockwise rotation of the output: 0, 90, 180, 270 */
	int max_out;       /* longest output side in pixels (the block size is chosen from it) */
	float meter_x, meter_y; /* spot metering point (0..1 in the raw frame), < 0: centre-weighted */
} PvParams;

#define PV_HIST 256
typedef struct {
	double sum[3], n;  /* colour sums of the usable blocks (linear 0..1, before WB): gray world */
	double gw, gwn;    /* metering-weighted green sum and weight */
	double gsum, gn;   /* plain mean green over all blocks */
	double clipped;    /* blocks with a channel at white */
	double blocks;
	uint32_t hist[PV_HIST]; /* green, linear 0..1 */
	/* filled by pv_stats_finish */
	double gmean;      /* metering-weighted mean green */
	double gplain;     /* plain mean green */
	double gp98;       /* 98th percentile green */
	double mean[3];    /* gray-world means */
} PvStats;

/* block size, block step and output size (after rotation) */
void pv_geometry(const PvRaw *r, const PvParams *p, int *block, int *step, int *ow, int *oh);
/* block rows [by0, by1) of the landscape output: pixels into dst (BGRX, rotated), statistics
 * added into st. Different row ranges may run on different threads (separate PvStats). */
void pv_render_rows(const uint8_t *raw, const PvRaw *r, const PvParams *p, int by0, int by1, uint8_t *dst,
		    long dst_stride, PvStats *st);
void pv_stats_merge(PvStats *into, const PvStats *from);
void pv_stats_finish(PvStats *s);

/* the colour matrix shared with the still pipeline (rawtool's "typical") */
extern const float pv_ccm[9];

#endif
