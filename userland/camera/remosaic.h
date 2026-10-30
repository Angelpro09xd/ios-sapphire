#ifndef CAM_REMOSAIC_H
#define CAM_REMOSAIC_H
#include "stage.h"

typedef struct {
	float sigma_rel, sigma_abs; /* range kernel of the chroma interpolation (relative/absolute) */
	float eps;                  /* direction weight regulariser */
	int iter;                   /* guided refinement passes (0 = off) */
	float blend;                /* weight of the previous green in a refinement pass */
	int simple;                 /* baseline for comparisons: per-plane inverse-distance interpolation */
} RemosaicParams;

/* cell (n = 2 or 3) pattern -> full RGB (linear) */
Stage *remosaic_rgb(Stage *raw, const Geo *g, const RemosaicParams *rp);
/* RGB -> Bayer mosaic of the given order (period 2) */
Stage *mosaic_sample(Stage *rgb, const Cfa *out);
/* cell pattern -> binned Bayer (block averages, w/n x h/n); *out_cfa receives its order */
Stage *cell_bin(Stage *raw, const Geo *g, Cfa *out_cfa);
/* the plain 2x2 order of a Bayer (n = 1) input with its phase applied */
Cfa geo_bayer_cfa(const Geo *g);

/* demosaic.c */
Stage *demosaic_bilinear(Stage *bayer, const Cfa *cfa);
Stage *demosaic_mhc(Stage *bayer, const Cfa *cfa);

#endif
