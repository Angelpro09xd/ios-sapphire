/* utilities: errors, timing, memory, a small persistent thread pool, CFA helpers */
#include "common.h"
#include <stdarg.h>
#include <pthread.h>
#include <stdatomic.h>
#include <time.h>
#include <unistd.h>
#include <sys/resource.h>

int g_verbose;

void die(const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	fputs("error: ", stderr);
	vfprintf(stderr, fmt, ap);
	fputc('\n', stderr);
	va_end(ap);
	exit(1);
}

void logv(const char *fmt, ...)
{
	if (!g_verbose) return;
	va_list ap;
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
}

void *xmalloc(size_t n)
{
	void *p = malloc(n ? n : 1);
	if (!p) die("out of memory (%zu bytes)", n);
	return p;
}

void *xcalloc(size_t n, size_t m)
{
	void *p = calloc(n ? n : 1, m ? m : 1);
	if (!p) die("out of memory (%zu x %zu bytes)", n, m);
	return p;
}

double now_sec(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec + ts.tv_nsec * 1e-9;
}

long peak_rss_kb(void)
{
	struct rusage ru;
	getrusage(RUSAGE_SELF, &ru);
#ifdef __APPLE__
	return ru.ru_maxrss / 1024; /* bytes on Darwin */
#else
	return ru.ru_maxrss; /* KiB on Linux */
#endif
}

int ncpus(void)
{
	long n = sysconf(_SC_NPROCESSORS_ONLN);
	return n < 1 ? 1 : n > 64 ? 64 : (int)n;
}

/* ---- thread pool ---- */
#define MAXTH 64
static struct {
	int n; /* worker threads (excluding the caller) */
	pthread_t th[MAXTH];
	pthread_mutex_t m;
	pthread_cond_t go, done;
	unsigned gen;
	int busy;
	range_fn fn;
	void *ctx;
	int total, grain;
	atomic_int next;
} P = { .m = PTHREAD_MUTEX_INITIALIZER, .go = PTHREAD_COND_INITIALIZER, .done = PTHREAD_COND_INITIALIZER };

static void run_items(void)
{
	for (;;) {
		int a = atomic_fetch_add(&P.next, P.grain);
		if (a >= P.total) break;
		int b = a + P.grain;
		if (b > P.total) b = P.total;
		P.fn(P.ctx, a, b);
	}
}

static void *worker(void *arg)
{
	(void)arg;
	unsigned seen = 0;
	pthread_mutex_lock(&P.m);
	for (;;) {
		while (P.gen == seen) pthread_cond_wait(&P.go, &P.m);
		seen = P.gen;
		pthread_mutex_unlock(&P.m);
		run_items();
		pthread_mutex_lock(&P.m);
		if (--P.busy == 0) pthread_cond_signal(&P.done);
	}
	return NULL;
}

void pool_init(int nthreads)
{
	if (nthreads < 1) nthreads = 1;
	if (nthreads > MAXTH) nthreads = MAXTH;
	P.n = nthreads - 1;
	for (int i = 0; i < P.n; i++)
		if (pthread_create(&P.th[i], NULL, worker, NULL)) die("pthread_create");
}

int pool_threads(void) { return P.n + 1; }

void pool_for(int n, int grain, range_fn fn, void *ctx)
{
	if (n <= 0) return;
	if (grain < 1) grain = 1;
	if (P.n == 0 || n <= grain) {
		fn(ctx, 0, n);
		return;
	}
	pthread_mutex_lock(&P.m);
	P.fn = fn;
	P.ctx = ctx;
	P.total = n;
	P.grain = grain;
	atomic_store(&P.next, 0);
	P.busy = P.n;
	P.gen++;
	pthread_cond_broadcast(&P.go);
	pthread_mutex_unlock(&P.m);
	run_items();
	pthread_mutex_lock(&P.m);
	while (P.busy) pthread_cond_wait(&P.done, &P.m);
	pthread_mutex_unlock(&P.m);
}

/* ---- CFA ---- */
int cfa_parse(const char *s, Cfa *out)
{
	if (!s || strlen(s) != 4) return -1;
	int k = 0;
	for (int i = 0; i < 4; i++) {
		int c;
		switch (s[i] | 0x20) {
		case 'r': c = CR; break;
		case 'g': c = CG; break;
		case 'b': c = CB; break;
		default: return -1;
		}
		out->c[i >> 1][i & 1] = c;
		k |= 1 << c;
	}
	if (k != 7) return -1;
	/* must be a Bayer quad: the two greens on a diagonal */
	if (!((out->c[0][0] == CG && out->c[1][1] == CG) || (out->c[0][1] == CG && out->c[1][0] == CG))) return -1;
	return 0;
}

void cfa_name(const Cfa *c, char out[5])
{
	static const char nm[] = "RGB";
	for (int i = 0; i < 4; i++) out[i] = nm[c->c[i >> 1][i & 1]];
	out[4] = 0;
}

/* ---- sRGB transfer ---- */
float srgb_encode(float v)
{
	if (v <= 0.0031308f) return v < 0 ? 0 : 12.92f * v;
	if (v >= 1) return 1;
	return 1.055f * powf(v, 1 / 2.4f) - 0.055f;
}

float srgb_decode(float v)
{
	if (v <= 0.04045f) return v < 0 ? 0 : v / 12.92f;
	if (v >= 1) return 1;
	return powf((v + 0.055f) / 1.055f, 2.4f);
}
