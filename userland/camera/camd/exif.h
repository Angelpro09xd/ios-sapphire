/* a minimal Exif APP1 segment for camd's JPEGs: orientation, date, camera, exposure */
#ifndef CAMD_EXIF_H
#define CAMD_EXIF_H
#include <stdint.h>
#include <stddef.h>

typedef struct {
	int orientation;          /* TIFF orientation: 1 upright, 6 rotate 90 CW, 3 180, 8 rotate 90 CCW */
	const char *datetime;     /* "YYYY:MM:DD HH:MM:SS" */
	const char *make, *model, *software;
	unsigned exposure_us;     /* 0: left out */
	unsigned iso;             /* 0: left out */
	unsigned width, height;   /* pixels as stored (before the orientation) */
} ExifInfo;

/* writes the whole segment (FF E1, length, "Exif\0\0", TIFF) into buf; returns its size, 0 if cap is too small */
size_t exif_build(uint8_t *buf, size_t cap, const ExifInfo *e);

#endif
