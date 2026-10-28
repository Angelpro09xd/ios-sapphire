/*
 * fpstart: com.apple.FileProvider's launchd Program on sapphire. launchd drops DYLD_* variables for
 * system daemons (see telephony/ccstart.c), so this sets DYLD_INSERT_LIBRARIES to libsapphire-fpfix.dylib
 * and execs the real fileproviderd (same pid: the job's Mach services and check-ins stay as they are).
 * Installed as /redwood/bin/fpstart; the job's Program in /System/Library/xpc/launchd.plist points here.
 */
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#define FILEPROVIDERD "/System/Library/Frameworks/FileProvider.framework/Support/fileproviderd"
#define FPFIX "/redwood/lib/libsapphire-fpfix.dylib"

int
main(int argc, char **argv)
{
	(void)argc;
	if (access(FPFIX, R_OK) == 0) {
		setenv("DYLD_INSERT_LIBRARIES", FPFIX, 1);
	}
	argv[0] = FILEPROVIDERD;
	execv(FILEPROVIDERD, argv);
	perror("fpstart: exec fileproviderd");
	return 1;
}
