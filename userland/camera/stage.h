/* Pull-based row streaming.
 *
 * A Stage produces rows of `ch` interleaved float channels. Each stage keeps only a ring
 * of `cap` rows; consumers ask for rows with stage_ensure() and read them with srow().
 * Output row y of a stage needs input rows [y*vs - ctx, y*vs + vs - 1 + ctx] of each input.
 * Rows are computed in chunks of `chunk` rows, split over the thread pool.
 */
#ifndef CAM_STAGE_H
#define CAM_STAGE_H
#include "common.h"

typedef struct Stage Stage;
typedef void (*stage_fn)(Stage *s, int y0, int y1);

struct Stage {
	const char *name;
	int w, h, ch;
	int period;  /* edge handling: CFA period (phase-preserving) or 1 = clamp */
	int vs, ctx; /* input row mapping, see above */
	Stage *in[3];
	int nin;
	stage_fn fn;
	void *p;
	/* ring */
	int cap;
	size_t rowlen; /* floats per ring row including the padding */
	float *ring;
	int hi;        /* rows [hi-cap, hi) are valid */
	int need;      /* rows consumers may pull ahead (computed before allocation) */
	int back;      /* rows consumers may still look back */
	double t;      /* seconds spent computing (wall, all threads) */
	Stage *next_all;
};

Stage *stage_new(const char *name, int w, int h, int ch, int period, stage_fn fn, void *p);
void stage_add_input(Stage *s, Stage *in);
void stage_set_map(Stage *s, int vs, int ctx);
/* declare a non-stage consumer (an output writer) reading `rows` rows at a time */
void stage_sink_need(Stage *s, int rows);
/* allocate the rings of all stages (call once, after the graph is built) */
size_t stages_alloc(int chunk);
void stage_ensure(Stage *s, int yend);
const float *srow(Stage *s, int y);  /* row y (edge-clamped), x in [-PAD, w+PAD) valid */
float *srow_w(Stage *s, int y);       /* producer's view of its own row */
void stages_report(FILE *f);
extern int g_chunk;

#endif
