/* ring-buffered row stages, see stage.h */
#include "stage.h"

int g_chunk = 64;
static Stage *all_stages;

Stage *stage_new(const char *name, int w, int h, int ch, int period, stage_fn fn, void *p)
{
	Stage *s = xcalloc(1, sizeof *s);
	s->name = name;
	s->w = w;
	s->h = h;
	s->ch = ch;
	s->period = period < 1 ? 1 : period;
	s->vs = 1;
	s->fn = fn;
	s->p = p;
	if (w < 2 * PAD || h < 8) die("stage %s: image too small (%dx%d)", name, w, h);
	/* append (keeps creation order for the report) */
	Stage **pp = &all_stages;
	while (*pp) pp = &(*pp)->next_all;
	*pp = s;
	return s;
}

void stage_add_input(Stage *s, Stage *in)
{
	if (s->nin >= 3) die("too many inputs");
	s->in[s->nin++] = in;
}

void stage_set_map(Stage *s, int vs, int ctx)
{
	s->vs = vs;
	s->ctx = ctx;
}

void stage_sink_need(Stage *s, int rows)
{
	/* sinks consume up to `rows` rows per call, pulled right before use */
	if (rows + g_chunk > s->need) s->need = rows + g_chunk;
	if (rows > s->back) s->back = rows;
}

size_t stages_alloc(int chunk)
{
	size_t total = 0;
	g_chunk = chunk;
	/* A stage's ring must span from the oldest row any consumer still needs to the newest
	 * row the most eager consumer chain has pulled, both relative to the sinks' position r.
	 * Ahead: a consumer C at most `need(C)` rows ahead pulls its input up to
	 * vs*need(C) + ctx, plus up to one chunk of overshoot: need(in) = vs*need(C) + ctx + chunk.
	 * Behind: consumers never go back more than ctx + vs rows: back(in). Iterate to a fixed
	 * point (the graph is a DAG, so depth iterations suffice). */
	for (int changed = 1, it = 0; changed && it < 64; it++) {
		changed = 0;
		for (Stage *s = all_stages; s; s = s->next_all)
			for (int i = 0; i < s->nin; i++) {
				Stage *in = s->in[i];
				int n = s->vs * s->need + s->ctx + chunk, b = s->ctx + s->vs;
				if (n > in->need) in->need = n, changed = 1;
				if (b > in->back) in->back = b;
			}
	}
	for (Stage *s = all_stages; s; s = s->next_all) {
		s->cap = (s->need ? s->need : 2 * chunk) + s->back + 16;
		if (s->cap > s->h) s->cap = s->h;
		s->rowlen = (size_t)(s->w + 2 * PAD) * s->ch;
		s->ring = xmalloc(sizeof(float) * s->rowlen * s->cap);
		total += sizeof(float) * s->rowlen * s->cap;
		logv("  stage %-10s %5dx%-5d ch %d ring %4d rows %6.1f MB\n", s->name, s->w, s->h, s->ch, s->cap,
		     sizeof(float) * s->rowlen * s->cap / 1048576.0);
	}
	return total;
}

static inline float *rowptr(Stage *s, int y) { return s->ring + (size_t)(y % s->cap) * s->rowlen + PAD * s->ch; }

float *srow_w(Stage *s, int y) { return rowptr(s, y); }

const float *srow(Stage *s, int y)
{
	y = phase_clamp(y, s->h, s->period);
	if (y >= s->hi || y < s->hi - s->cap)
		die("internal: stage %s row %d not in ring [%d,%d)", s->name, y, s->hi - s->cap, s->hi);
	return rowptr(s, y);
}

/* fill the horizontal padding of a freshly computed row (phase-preserving) */
static void fill_pad(Stage *s, float *r)
{
	int ch = s->ch, w = s->w;
	for (int k = 1; k <= PAD; k++) {
		int xl = phase_clamp(-k, w, s->period), xr = phase_clamp(w - 1 + k, w, s->period);
		for (int c = 0; c < ch; c++) {
			r[-k * ch + c] = r[xl * ch + c];
			r[(w - 1 + k) * ch + c] = r[xr * ch + c];
		}
	}
}

typedef struct {
	Stage *s;
	int y0;
} RunCtx;

static void run_rows(void *vctx, int a, int b)
{
	RunCtx *c = vctx;
	Stage *s = c->s;
	s->fn(s, c->y0 + a, c->y0 + b);
	for (int y = c->y0 + a; y < c->y0 + b; y++) fill_pad(s, rowptr(s, y));
}

void stage_ensure(Stage *s, int yend)
{
	if (yend > s->h) yend = s->h;
	while (s->hi < yend) {
		int a = s->hi, b = a + g_chunk;
		if (b > s->h) b = s->h;
		/* inputs first */
		for (int i = 0; i < s->nin; i++) {
			Stage *in = s->in[i];
			int need = b * s->vs + s->ctx;
			stage_ensure(in, need < in->h ? need : in->h);
			int lo = a * s->vs - s->ctx;
			if (lo < 0) lo = 0;
			if (lo < in->hi - in->cap)
				die("internal: ring of %s too small for %s (row %d, valid from %d)", in->name, s->name, lo,
				    in->hi - in->cap);
		}
		/* the rows we are about to write must not still be needed: the cap check in the
		 * consumers' srow() catches that case if it ever happens */
		double t0 = now_sec();
		RunCtx c = { s, a };
		int grain = (b - a) / (pool_threads() * 2);
		if (grain < 1) grain = 1;
		pool_for(b - a, grain, run_rows, &c);
		s->t += now_sec() - t0;
		s->hi = b;
	}
}

void stages_report(FILE *f)
{
	/* stage times include the time of their inputs computed on demand inside, so print
	 * self times: own time minus inputs is not tracked; inputs are pulled before the timer
	 * starts, so these are already self times. */
	for (Stage *s = all_stages; s; s = s->next_all)
		fprintf(f, "  %-12s %5dx%-5d %6.2f s\n", s->name, s->w, s->h, s->t);
}
