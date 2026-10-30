// camdump: copy the captured frame (kern.sweet_cam_frame, 1 MB per read from kern.sweet_cam_seek) into a file
#include <stdio.h>
#include <stdlib.h>
#include <sys/sysctl.h>

int main(int argc, char **argv) {
	if (argc < 2) { fprintf(stderr, "usage: camdump OUT\n"); return 2; }
	long long len = 0; size_t l = sizeof(len); int ilen = 0;
	if (sysctlbyname("kern.sweet_cam_frame_len", &len, &l, NULL, 0) != 0) { perror("frame_len"); return 1; }
	if (l == 4) { ilen = *(int *)&len; len = ilen; }
	FILE *f = fopen(argv[1], "wb");
	if (!f) { perror(argv[1]); return 1; }
	int zero = 0;
	sysctlbyname("kern.sweet_cam_seek", NULL, NULL, &zero, sizeof(zero));
	char *buf = malloc(1 << 20);
	long long got = 0;
	while (got < len) {
		size_t n = 1 << 20;
		if (sysctlbyname("kern.sweet_cam_frame", buf, &n, NULL, 0) != 0) { perror("frame"); break; }
		if (n == 0) break;
		fwrite(buf, 1, n, f); got += n;
	}
	fclose(f);
	printf("%lld of %lld bytes\n", got, len);
	return got == len ? 0 : 1;
}
