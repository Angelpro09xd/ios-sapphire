#ifndef CAM_RAWIO_H
#define CAM_RAWIO_H
#include "stage.h"

enum { FMT_RAW10 = 0, FMT_RAW12, FMT_RAW16, FMT_RAW8, FMT_PGM };

typedef struct {
	int fd;
	int fmt;
	long long offset; /* bytes before the first row */
	long stride;      /* bytes per row */
	int w, h;
	int be16;         /* raw16/pgm: big-endian samples (PGM always is) */
	int shift16;      /* raw16: right shift applied after reading (MSB-aligned data) */
	Geo geo;
	float black[3];   /* per colour */
	float white;
	float gain[3];    /* white-balance gains applied in the source stage */
} RawIn;

void rawin_open(RawIn *r, const char *path);   /* fmt/w/h/offset/stride must be set (or PGM) */
void rawin_read_rows(RawIn *r, int y0, int n, uint16_t *dst); /* n rows of w samples */
Stage *rawin_stage(RawIn *r);                   /* normalised, black-subtracted, WB-gained */
/* gray-world: per-colour means of the normalised raw (no gains); returns pixels used */
double rawin_means(RawIn *r, double mean[3]);
void rawin_detect(RawIn *r);
void rawin_unpack_pgm(RawIn *r, const char *path);
long raw_min_stride(int fmt, int w);

#endif
