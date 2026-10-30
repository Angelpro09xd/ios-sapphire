/* camctl: one camd command from the shell, e.g.
 *   camctl STATUS            camctl START 3            camctl CAPTURE 108 dng
 *   camctl -s 10 START       (keeps the connection, and so the preview, for 10 s)
 * Prints every reply line; CAPTURE waits for DONE or ERR. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/un.h>
#include "camd_proto.h"

int main(int argc, char **argv)
{
	int stay = 0, i = 1;
	if (argc > 2 && !strcmp(argv[1], "-s")) {
		stay = atoi(argv[2]);
		i = 3;
	}
	if (i >= argc) {
		fprintf(stderr, "usage: camctl [-s SECONDS] COMMAND [ARGS...]\n");
		return 2;
	}
	char line[512] = "";
	for (; i < argc; i++) {
		strncat(line, argv[i], sizeof line - strlen(line) - 2);
		if (i + 1 < argc) strncat(line, " ", sizeof line - strlen(line) - 2);
	}
	int capture = !strncmp(line, "CAPTURE", 7);
	strcat(line, "\n");
	int fd = socket(AF_UNIX, SOCK_STREAM, 0);
	struct sockaddr_un a = { .sun_family = AF_UNIX };
	snprintf(a.sun_path, sizeof a.sun_path, "%s", CAMD_SOCK_PATH);
	if (connect(fd, (struct sockaddr *)&a, sizeof a) != 0) {
		perror(CAMD_SOCK_PATH);
		return 1;
	}
	struct timeval tv = { capture ? 120 : 10, 0 };
	setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
	if (write(fd, line, strlen(line)) < 0) return 1;
	char buf[4096];
	size_t n = 0;
	int lines = 0, rc = 0;
	time_t until = time(NULL) + stay;
	for (;;) {
		ssize_t k = read(fd, buf + n, sizeof buf - 1 - n);
		if (k <= 0) break;
		n += (size_t)k;
		buf[n] = 0;
		char *s = buf, *nl;
		int done = 0;
		while ((nl = strchr(s, '\n'))) {
			*nl = 0;
			puts(s);
			fflush(stdout);
			lines++;
			if (!strncmp(s, "ERR", 3)) rc = 1, done = 1;
			if (!capture || !strncmp(s, "DONE", 4)) done = 1;
			s = nl + 1;
		}
		n = strlen(s);
		memmove(buf, s, n + 1);
		if (done) break;
	}
	while (time(NULL) < until) sleep(1);
	close(fd);
	return lines ? rc : 1;
}
