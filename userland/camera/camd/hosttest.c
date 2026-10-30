/* hosttest: camd's preview renderer and develop step on the PC (build.sh test).
 *   hosttest RAW W H CELL ROTATE PREVIEW.ppm OUT.jpg [OUT.dng]
 * RAW is a packed RAW10 frame (rawsynth output). Prints the preview statistics. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "preview.h"
#include "develop.h"
#include "exif.h"

static double now(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec + ts.tv_nsec * 1e-9;
}

int main(int argc, char **argv)
{
	if (argc < 8) {
		fprintf(stderr, "usage: hosttest RAW W H CELL ROTATE PREVIEW.ppm OUT.jpg [OUT.dng]\n");
		return 2;
	}
	int w = atoi(argv[2]), h = atoi(argv[3]), cell = atoi(argv[4]), rot = atoi(argv[5]);
	FILE *f = fopen(argv[1], "rb");
	if (!f) return 1;
	fseek(f, 0, SEEK_END);
	long len = ftell(f);
	fseek(f, 0, SEEK_SET);
	uint8_t *raw = malloc((size_t)len);
	if (fread(raw, 1, (size_t)len, f) != (size_t)len) return 1;
	fclose(f);
	long stride = len / h;
	PvRaw r = { w, h, stride, cell, 64, 1023 };
	PvParams p = { { 1.818182f, 1, 1.428571f }, 1, rot, w <= 1280 ? 1280 : 1000, -1, -1 };
	int B, S, ow, oh;
	pv_geometry(&r, &p, &B, &S, &ow, &oh);
	long ds = ((long)ow * 4 + 63) & ~63L;
	uint8_t *dst = calloc((size_t)ds, (size_t)oh);
	PvStats st;
	memset(&st, 0, sizeof st);
	double t0 = now();
	pv_render_rows(raw, &r, &p, 0, (h - B) / S + 1, dst, ds, &st);
	double t1 = now();
	pv_stats_finish(&st);
	printf("preview %dx%d (block %d step %d) in %.1f ms: gmean %.4f gplain %.4f p98 %.3f gray-world %.4f %.4f %.4f clipped %.0f/%.0f\n",
	       ow, oh, B, S, (t1 - t0) * 1000, st.gmean, st.gplain, st.gp98, st.mean[0], st.mean[1], st.mean[2], st.clipped, st.blocks);
	f = fopen(argv[6], "wb");
	fprintf(f, "P6\n%d %d\n255\n", ow, oh);
	for (int y = 0; y < oh; y++)
		for (int x = 0; x < ow; x++) {
			uint8_t *q = dst + ds * y + 4 * x;
			fputc(q[2], f);
			fputc(q[1], f);
			fputc(q[0], f);
		}
	fclose(f);

	uint8_t app1[1024];
	ExifInfo ei = { rot == 90 ? 6 : rot == 270 ? 8 : rot == 180 ? 3 : 1, "2026:10:30 12:00:00", NULL, NULL, "hosttest",
			10000, 100, (unsigned)w, (unsigned)h };
	size_t n = exif_build(app1, sizeof app1, &ei);
	printf("exif %zu bytes\n", n);
	DevJob j;
	memset(&j, 0, sizeof j);
	j.raw = argv[1];
	j.w = w;
	j.h = h;
	j.cell = cell;
	j.stride = stride;
	j.black = 64;
	j.white = 1023;
	j.wb[0] = 1.818182f;
	j.wb[1] = 1;
	j.wb[2] = 1.428571f;
	j.expect_g = (float)st.gplain;
	j.jpeg = argv[7];
	j.dng = argc > 8 ? argv[8] : NULL;
	j.quality = 92;
	j.chunk = 16;
	j.app1 = app1;
	j.app1_len = n;
	j.progress_fd = 1;
	t0 = now();
	int rc = develop_run(&j);
	printf("develop %d in %.2f s\n", rc, now() - t0);
	return rc;
}
