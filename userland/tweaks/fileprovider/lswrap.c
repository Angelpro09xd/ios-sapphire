/*
 * lswrap: LocalStorageFileProvider's executable on sapphire, so that libsapphire-fpfix.dylib is loaded
 * into it (see fpfix.c). Extensions are launched by runningboardd/launchd as XPC services outside any
 * .app/, where neither the kernel's DYLD_INSERT_LIBRARIES for apps (xnu 0238) nor a launchd environment
 * (DYLD_* dropped) reaches them. Installed in place of
 * /System/Library/Frameworks/FileProvider.framework/PlugIns/LocalStorageFileProvider.appex/LocalStorageFileProvider;
 * Apple's binary is kept next to it as LocalStorageFileProvider.pre-files and exec'd from there (same
 * pid, same bundle directory: the XPC check-in and NSBundle.mainBundle stay as they were).
 * Undo: move LocalStorageFileProvider.pre-files back over LocalStorageFileProvider.
 */
#include <mach-o/dyld.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define FPFIX "/redwood/lib/libsapphire-fpfix.dylib"

int
main(int argc, char **argv)
{
	char self[1024], real[1100];
	uint32_t size = sizeof(self);

	(void)argc;
	if (_NSGetExecutablePath(self, &size) != 0) {
		return 1;
	}
	snprintf(real, sizeof(real), "%s.pre-files", self);
	if (access(FPFIX, R_OK) == 0) {
		setenv("DYLD_INSERT_LIBRARIES", FPFIX, 1);
	}
	execv(real, argv);
	perror("lswrap: exec");
	return 1;
}
