/*
 * ccstart: CommCenter's launchd program on sapphire. launchd drops DYLD_* variables for system
 * daemons, so this sets DYLD_INSERT_LIBRARIES=/redwood/lib/ccbridge.dylib and execs the real
 * CommCenter (same pid: launchd's job, its Mach services and check-ins stay as they are).
 */
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#define COMMCENTER "/System/Library/Frameworks/CoreTelephony.framework/Support/CommCenter"

int
main(int argc, char **argv)
{
	(void)argc;
	setenv("DYLD_INSERT_LIBRARIES", "/redwood/lib/ccbridge.dylib", 1);
	argv[0] = COMMCENTER;
	execv(COMMCENTER, argv);
	perror("ccstart: exec CommCenter");
	return 1;
}
