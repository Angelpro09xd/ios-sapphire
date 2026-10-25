/*
 * ccstart: CommCenter's launchd program on sapphire. launchd drops DYLD_* variables for system
 * daemons, so this sets DYLD_INSERT_LIBRARIES=/redwood/lib/ccbridge.dylib and execs the real
 * CommCenter (same pid: launchd's job, its Mach services and check-ins stay as they are).
 * It also starts the mobile data call in the background (qmitool autodata, one instance: it
 * stays when CommCenter restarts); /var/tmp/autodata.off keeps it from starting.
 */
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>

#define AUTODATA "/redwood/bin/qmitool"     /* setuid root */
#define COMMCENTER "/System/Library/Frameworks/CoreTelephony.framework/Support/CommCenter"

int
main(int argc, char **argv)
{
	(void)argc;
	if (access(AUTODATA, X_OK) == 0 && access("/var/tmp/autodata.off", F_OK) != 0 && fork() == 0) {
		setsid();
		int fd = open("/var/tmp/autodata.log", O_WRONLY | O_CREAT | O_APPEND, 0644);
		if (fd >= 0) {
			dup2(fd, 1);
			dup2(fd, 2);
			close(fd);
		}
		execl(AUTODATA, "qmitool", "autodata", (char *)NULL);
		_exit(127);
	}
	setenv("DYLD_INSERT_LIBRARIES", "/redwood/lib/ccbridge.dylib", 1);
	argv[0] = COMMCENTER;
	execv(COMMCENTER, argv);
	perror("ccstart: exec CommCenter");
	return 1;
}
