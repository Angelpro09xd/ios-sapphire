/*
 * sensload ROOT: every file under ROOT goes into the kernel's file store for the ADSP's sensors PD
 * (kern.sweet_frpc_file = "path\0bytes", RAM only), with its path below ROOT as the full path the PD
 * asks for: ROOT/vendor/etc/sensors/sns_reg_config -> /vendor/etc/sensors/sns_reg_config.
 * Then kern.sweet_frpc_files' first line (count, bytes).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/sysctl.h>

static int loaded, failed;
static size_t rootlen;

static void
load(const char *full)
{
	FILE *f = fopen(full, "rb");
	if (f == NULL) {
		perror(full);
		failed++;
		return;
	}
	fseek(f, 0, SEEK_END);
	long n = ftell(f);
	fseek(f, 0, SEEK_SET);
	const char *path = full + rootlen;
	size_t pl = strlen(path) + 1;
	char *buf = malloc(pl + (size_t)n);
	if (buf == NULL || n < 0) {
		fclose(f);
		failed++;
		return;
	}
	memcpy(buf, path, pl);
	if (fread(buf + pl, 1, (size_t)n, f) != (size_t)n) {
		perror(full);
		failed++;
	} else if (sysctlbyname("kern.sweet_frpc_file", NULL, NULL, buf, pl + (size_t)n) != 0) {
		fprintf(stderr, "%s: %s\n", path, strerror(errno));
		failed++;
	} else {
		loaded++;
	}
	fclose(f);
	free(buf);
}

static void
walk(const char *dir)
{
	DIR *d = opendir(dir);
	struct dirent *e;
	if (d == NULL) {
		perror(dir);
		failed++;
		return;
	}
	while ((e = readdir(d)) != NULL) {
		char p[1024];
		struct stat st;
		if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) {
			continue;
		}
		snprintf(p, sizeof(p), "%s/%s", dir, e->d_name);
		if (lstat(p, &st) != 0) {
			continue;
		}
		if (S_ISDIR(st.st_mode)) {
			walk(p);
		} else if (S_ISREG(st.st_mode)) {
			load(p);
		}
	}
	closedir(d);
}

int
main(int argc, char **argv)
{
	if (argc != 2) {
		fprintf(stderr, "usage: sensload ROOT   (ROOT/vendor/..., ROOT/mnt/vendor/persist/..., ROOT/sys/...)\n");
		return 2;
	}
	char root[1024];
	snprintf(root, sizeof(root), "%s", argv[1]);
	while (strlen(root) > 1 && root[strlen(root) - 1] == '/') {
		root[strlen(root) - 1] = 0;
	}
	rootlen = strlen(root);
	walk(root);
	char out[256];
	size_t ol = sizeof(out) - 1;
	if (sysctlbyname("kern.sweet_frpc_files", NULL, &ol, NULL, 0) == 0 || errno == ENOMEM) {
		/* only the first line: read it into a big enough buffer */
		size_t big = 256 * 1024;
		char *b = malloc(big);
		if (b != NULL && sysctlbyname("kern.sweet_frpc_files", b, &big, NULL, 0) == 0) {
			char *nl = strchr(b, '\n');
			if (nl != NULL) {
				*nl = 0;
			}
			snprintf(out, sizeof(out), "%s", b);
			printf("kernel store: %s\n", out);
		}
		free(b);
	}
	printf("%d files loaded, %d failed\n", loaded, failed);
	return failed != 0;
}
