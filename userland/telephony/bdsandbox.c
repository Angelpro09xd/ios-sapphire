/*
 * libsapphire-bdsandbox.dylib: incoming SMS for Messages.
 *
 * imagent hands every incoming SMS to MessagesBlastDoorService (BlastDoor's sandboxed parser) before
 * Messages sees it. BlastDoor checks at start that it runs in a sandbox (Sandbox.swift: sandbox_check on
 * its own pid must say 1, "BlastDoor cannot run unsandboxed.", a Swift fatal error); there is no
 * sandbox here (no Sandbox.kext), so the service died at every start, imagent retried for ever, and
 * the SMS never arrived. Loaded into the service (DYLD_INSERT_LIBRARIES in its XPCService
 * EnvironmentVariables, MessagesBlastDoorService.xpc/Info.plist; original kept as Info.plist.pre-bdsandbox),
 * this makes the check's two calls answer 1: in BlastDoor (iOS 16.7, 20H392), func 0x1a9c55b50 at
 * image offset 0x99b64 "bl __bd_sandbox_check" and 0x99b70 "bl __bd_sandbox_enable_state_flag".
 * The instructions are checked first; anything else is left alone. Log: /var/tmp/bdsandbox.log.
 */
#include <fcntl.h>
#include <libkern/OSCacheControl.h>
#include <mach-o/dyld.h>
#include <mach/mach.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define BL_CHECK        0x940000a5u     /* both calls happen to encode alike (same distance) */
#define MOV_W0_1        0x52800020u

static void
say(const char *s)
{
	int fd = open("/var/tmp/bdsandbox.log", O_WRONLY | O_CREAT | O_APPEND, 0644);
	if (fd >= 0) {
		char line[160];
		int n = snprintf(line, sizeof(line), "[%d] %s\n", getpid(), s);
		write(fd, line, (size_t)n);
		close(fd);
	}
}

static int
patch(uint32_t *p)
{
	if (*p != BL_CHECK) {
		return -1;
	}
	vm_address_t page = (vm_address_t)p & ~(vm_address_t)(vm_page_size - 1);
	if (vm_protect(mach_task_self(), page, vm_page_size, FALSE, VM_PROT_READ | VM_PROT_WRITE | VM_PROT_COPY) != KERN_SUCCESS) {
		return -2;
	}
	*p = MOV_W0_1;
	vm_protect(mach_task_self(), page, vm_page_size, FALSE, VM_PROT_READ | VM_PROT_EXECUTE);
	sys_icache_invalidate(p, 4);
	return 0;
}

__attribute__((constructor)) static void
install(void)
{
	for (uint32_t i = 0; i < _dyld_image_count(); i++) {
		const char *name = _dyld_get_image_name(i);
		size_t n = name != NULL ? strlen(name) : 0;
		if (n < 30 || strcmp(name + n - 30, "/BlastDoor.framework/BlastDoor") != 0) {
			continue;
		}
		uint8_t *base = (uint8_t *)_dyld_get_image_header(i);
		int a = patch((uint32_t *)(base + 0x99b64)), b = patch((uint32_t *)(base + 0x99b70));
		char line[96];
		snprintf(line, sizeof(line), "BlastDoor sandbox check: %s, state flag: %s", a == 0 ? "patched" : "left alone",
		    b == 0 ? "patched" : "left alone");
		say(line);
		return;
	}
	say("no BlastDoor image");
}
