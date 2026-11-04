/*
 * bbstart PROGRAM [ARGS...]: launchd's program for the daemons that wait for Apple's AppleBaseband
 * driver (locationd, abm-helper: their TelephonyBasebandController throws when it has not appeared
 * after 15 s, and launchd restarts them for ever). launchd drops DYLD_* variables for system daemons,
 * so this sets DYLD_INSERT_LIBRARIES=/redwood/lib/libsapphire-fakebb.dylib (the fake driver, fakebb.c)
 * and execs PROGRAM: same pid, launchd's job otherwise unchanged. install-bbstart.sh sets the jobs up.
 */
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

int
main(int argc, char **argv)
{
	if (argc < 2) {
		fprintf(stderr, "usage: bbstart PROGRAM [ARGS...]\n");
		return 2;
	}
	if (access("/redwood/lib/libsapphire-fakebb.dylib", R_OK) == 0) {
		setenv("DYLD_INSERT_LIBRARIES", "/redwood/lib/libsapphire-fakebb.dylib", 1);
	}
	execv(argv[1], argv + 1);
	perror("bbstart: exec");
	return 1;
}
