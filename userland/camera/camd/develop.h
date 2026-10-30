/* camd develop: one raw RAW10 file -> JPEG (+ DNG) with the camera pipeline library (rawtool's
 * graph, linked in). Meant to run in a forked child: the library keeps global state and exits on
 * errors, so each capture gets a fresh process and its memory back at the end. */
#ifndef CAMD_DEVELOP_H
#define CAMD_DEVELOP_H
#include <stdint.h>
#include <stddef.h>

typedef struct {
	const char *raw;          /* input file */
	int w, h, cell;           /* 12000x9000 cell 3 (108 MP), 4000x3000 cell 1 (12 MP) */
	long stride;              /* bytes per line, 0 = packed */
	float black, white;
	float wb[3];              /* 0,0,0: gray world from the frame itself */
	float ev;                 /* exposure compensation for the JPEG (EV) */
	float expect_g;           /* expected plain mean green (linear) from the preview, 0: no correction */
	const char *jpeg, *dng;   /* outputs (dng may be NULL) */
	int quality, chunk, threads;
	float sharpen;
	const uint8_t *app1;      /* Exif segment for the JPEG */
	size_t app1_len;
	const char *sensor;
	int progress_fd;          /* "P n" (0..100) and "CAL g" lines go here, -1 none */
} DevJob;

/* 0 when the outputs were written (the library exits the process with 1 on errors) */
int develop_run(const DevJob *j);

#endif
