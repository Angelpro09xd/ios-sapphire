// fpctl COMMAND [ARGS...]: one command to fpd's control socket (/var/run/fpd.sock, protocol in fpd.c), its answer
// printed. ENROLL_BEGIN and TEST stream until their end line (DONE / FAILED / TEST ...); Ctrl-C cancels.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>

static int fd = -1;
static const char *cancel_cmd;

static void
on_int(int s)
{
	(void)s;
	if (fd >= 0 && cancel_cmd != NULL) {
		write(fd, cancel_cmd, strlen(cancel_cmd));
	}
}

int
main(int argc, char **argv)
{
	if (argc < 2) {
		fprintf(stderr, "usage: fpctl STATUS | LIST | RENAME slot name | DELETE slot | DELETE_ALL | ENROLL_BEGIN [name] |"
		    " TEST | LOG [n] | PREFS\n");
		return 2;
	}
	char cmd[1024] = "";
	for (int i = 1; i < argc; i++) {
		strlcat(cmd, argv[i], sizeof cmd);
		strlcat(cmd, i + 1 < argc ? " " : "\n", sizeof cmd);
	}
	int stream = strncmp(cmd, "ENROLL_BEGIN", 12) == 0 || strncmp(cmd, "TEST\n", 5) == 0;
	cancel_cmd = strncmp(cmd, "ENROLL", 6) == 0 ? "ENROLL_CANCEL\n" : "TEST_CANCEL\n";
	fd = socket(AF_UNIX, SOCK_STREAM, 0);
	struct sockaddr_un a = { .sun_family = AF_UNIX };
	snprintf(a.sun_path, sizeof a.sun_path, "/var/run/fpd.sock");
	if (fd < 0 || connect(fd, (struct sockaddr *)&a, sizeof a) != 0) {
		perror("/var/run/fpd.sock");
		return 1;
	}
	signal(SIGINT, on_int);
	write(fd, cmd, strlen(cmd));
	char buf[4096], line[2048];
	size_t ln = 0;
	ssize_t r;
	while ((r = read(fd, buf, sizeof buf)) > 0) {
		for (ssize_t i = 0; i < r; i++) {
			if (buf[i] != '\n') {
				if (ln < sizeof line - 1) {
					line[ln++] = buf[i];
				}
				continue;
			}
			line[ln] = 0;
			ln = 0;
			puts(line);
			fflush(stdout);
			int end = stream ? (strncmp(line, "DONE", 4) == 0 || strncmp(line, "FAILED", 6) == 0 ||
			                    strncmp(line, "TEST ", 5) == 0 || strncmp(line, "ERR", 3) == 0 ||
			                    strcmp(line, "CANCELLED") == 0)
			                 : (strcmp(line, "END") == 0 || strncmp(line, "OK", 2) == 0 || strncmp(line, "ERR", 3) == 0 ||
			                    strcmp(line, "CANCELLED") == 0);
			if (end) {
				close(fd);
				return strncmp(line, "ERR", 3) == 0 || strncmp(line, "FAILED", 6) == 0;
			}
		}
	}
	close(fd);
	return 1;
}
