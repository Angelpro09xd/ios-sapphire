// fpsave: copy files out of the kernel's Goodix store (kern.sweet_qsee_filesel + kern.sweet_qsee_fileout) into a directory
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/sysctl.h>

int main(int argc, char **argv) {
	if (argc < 3) { fprintf(stderr, "usage: fpsave DIR NAME...\n"); return 2; }
	for (int i = 2; i < argc; i++) {
		if (sysctlbyname("kern.sweet_qsee_filesel", NULL, NULL, argv[i], strlen(argv[i])) != 0) { printf("%s: not in the store\n", argv[i]); continue; }
		size_t n = 0;
		if (sysctlbyname("kern.sweet_qsee_fileout", NULL, &n, NULL, 0) != 0) { perror("size"); continue; }
		char *buf = malloc(n ? n : 1);
		if (sysctlbyname("kern.sweet_qsee_fileout", buf, &n, NULL, 0) != 0) { perror("read"); free(buf); continue; }
		char path[1024]; snprintf(path, sizeof path, "%s/%s", argv[1], argv[i]);
		FILE *f = fopen(path, "wb");
		if (!f || fwrite(buf, 1, n, f) != n) { perror(path); free(buf); if (f) fclose(f); continue; }
		fclose(f); free(buf);
		printf("%s: %zu bytes saved\n", path, n);
	}
	return 0;
}
