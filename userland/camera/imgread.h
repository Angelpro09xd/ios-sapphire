#ifndef CAM_IMGREAD_H
#define CAM_IMGREAD_H
#include "common.h"

typedef struct {
	int w, h;
	float *px; /* RGB, 0..1, as stored (sRGB-encoded) */
} Image;

void image_load(Image *im, const char *path); /* PNG or binary PPM/PGM */

#endif
