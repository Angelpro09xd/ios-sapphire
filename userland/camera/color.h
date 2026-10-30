#ifndef CAM_COLOR_H
#define CAM_COLOR_H
#include "stage.h"

Stage *stage_denoise(Stage *in, float strength);
Stage *stage_color(Stage *in, const float ccm[9], float exposure, int clip);
Stage *stage_sharpen(Stage *in, float amount);
Stage *stage_downscale(Stage *in, int k);

#endif
