#ifndef CAM_OUT_H
#define CAM_OUT_H
#include "stage.h"

/* output transfer curve: float linear -> 16-bit code (LUT) */
typedef struct {
	uint16_t lut[65536 + 1];
} Tone;
void tone_init(Tone *t, const char *curve); /* "srgb", "linear", or a gamma value like "2.2" */
static inline unsigned tone16(const Tone *t, float v)
{
	if (!(v > 0)) return t->lut[0];
	if (v >= 1) return t->lut[65535];
	return t->lut[(int)(v * 65535.0f + 0.5f)];
}

typedef struct Sink Sink;
struct Sink {
	const char *name;
	Stage *src;
	int h, gran, done;
	void (*consume)(Sink *s, int y0, int y1);
	void (*finish)(Sink *s);
	void *p;
	double t;
	long long bytes;
};

Sink *sink_ppm(Stage *rgb, const char *path, int bits, const Tone *tone);
Sink *sink_png(Stage *rgb, const char *path, int bits, const Tone *tone);
Sink *sink_jpeg(Stage *rgb, const char *path, int quality, const Tone *tone);
/* 1-channel mosaic as a 16-bit PGM: value = clamp(v / gain[colour]) * 65535 */
Sink *sink_pgm(Stage *bayer, const char *path, const Cfa *cfa, const float gain[3]);

typedef struct {
	Cfa cfa;
	float gain[3];     /* WB gains present in the stage values (divided out) */
	float black, white; /* 16-bit code values written for 0 and 1 */
	float neutral[3];   /* AsShotNeutral */
	float colmat[9];    /* ColorMatrix1 (XYZ -> camera) */
	const char *make, *model, *unique, *software, *desc;
} DngInfo;
Sink *sink_dng(Stage *bayer, const char *path, const DngInfo *info);

void sinks_run(Sink **s, int n);

/* jpeg.c */
typedef struct JpegEnc JpegEnc;
/* an optional complete APP1 segment (marker included) written after the JFIF header */
extern const uint8_t *g_jpeg_app1;
extern size_t g_jpeg_app1_len;
JpegEnc *jpeg_new(int w, int h, int quality);
void jpeg_write_header(JpegEnc *e, FILE *f);
/* encode one 16-row MCU row from 8-bit RGB rows (rows[k] for k<16, may repeat); thread-safe.
 * Returns a malloc'd buffer of entropy-coded bytes (byte-stuffed, bit-padded). */
uint8_t *jpeg_encode_mcu_row(JpegEnc *e, const uint8_t *const rows[16], size_t *len);
void jpeg_write_mcu_row(JpegEnc *e, FILE *f, int index, const uint8_t *buf, size_t len);
void jpeg_finish(JpegEnc *e, FILE *f);
int jpeg_mcu_rows(const JpegEnc *e);

#endif
