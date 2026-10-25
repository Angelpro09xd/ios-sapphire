/*
 * ccbridge.dylib: loaded into Apple's CommCenter (DYLD_INSERT_LIBRARIES in its launchd job) so
 * that iOS's own telephony stack runs on the sapphire's Qualcomm modem. Apple's Qualcomm iPhones
 * ("Maverick" basebands) speak QMI in QMUX frames over PCIe channels (libPCITransport.dylib);
 * ours speaks the same QMI over QRTR (/dev/redwood-qmiN, kernel 0220). This library patches
 * the places where CommCenter would talk to Apple hardware we do not have.
 *
 * Addresses are iOS 16.7.16 (20H392) iPhone10,3 shared cache addresses, unslid; every patch
 * first checks the instruction bytes it expects and leaves the function alone if they differ.
 * Log: /var/tmp/ccbridge.log.
 *
 * Step 0: ApplePDPHelperInterface::init() (the cellular data "IP appender" kext, AppleIPAppender,
 *   which we do not have) returns true, so getInstance() is not NULL and the callers get
 *   "service is not open" errors instead of a NULL std::mutex (CommCenter SIGSEGV at start).
 */
#include <dlfcn.h>
#include <libkern/OSCacheControl.h>
#include <stdlib.h>
#include <fcntl.h>
#include <mach-o/dyld.h>
#include <mach/mach.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#define CACHE_BASE              0x180000000UL

extern const void *_dyld_get_shared_cache_range(size_t *length);

static uintptr_t slide;
static int log_fd = -1;

static void
say(const char *fmt, ...)
{
	char buf[512];
	struct timeval tv;
	gettimeofday(&tv, NULL);
	int n = snprintf(buf, sizeof(buf), "%ld.%03d [%d] ", (long)tv.tv_sec, (int)(tv.tv_usec / 1000), getpid());
	va_list ap;
	va_start(ap, fmt);
	n += vsnprintf(buf + n, sizeof(buf) - (size_t)n, fmt, ap);
	va_end(ap);
	if (n > (int)sizeof(buf) - 2) n = (int)sizeof(buf) - 2;
	buf[n++] = '\n';
	if (log_fd < 0) {
		log_fd = open("/var/tmp/ccbridge.log", O_WRONLY | O_CREAT | O_APPEND, 0644);
	}
	if (log_fd >= 0) {
		write(log_fd, buf, (size_t)n);
	}
}

/* overwrite instructions in the shared cache (a private copy of the page, then r-x again) */
static int
patch_code(const char *what, uintptr_t addr, uint64_t expect, const uint32_t *insns, unsigned count)
{
	uint32_t *p = (uint32_t *)(addr + slide);
	uint64_t have;
	memcpy(&have, p, 8);
	if (have != expect) {
		say("%s: %#lx holds %016llx, not %016llx: left alone", what, addr, (unsigned long long)have,
		    (unsigned long long)expect);
		return -1;
	}
	vm_address_t page = (vm_address_t)p & ~(vm_address_t)(vm_page_size - 1);
	vm_size_t size = ((vm_address_t)(p + count) - page + vm_page_size - 1) & ~(vm_size_t)(vm_page_size - 1);
	kern_return_t kr = vm_protect(mach_task_self(), page, size, FALSE, VM_PROT_READ | VM_PROT_WRITE | VM_PROT_COPY);
	if (kr != KERN_SUCCESS) {
		say("%s: not writable (%d)", what, kr);
		return -1;
	}
	memcpy(p, insns, count * 4);
	kr = vm_protect(mach_task_self(), page, size, FALSE, VM_PROT_READ | VM_PROT_EXECUTE);
	sys_icache_invalidate(p, count * 4);
	say("%s: patched at %#lx%s", what, addr, kr == KERN_SUCCESS ? "" : " (r-x again FAILED)");
	return 0;
}

#define A64_MOV_W0_1    0x52800020u     /* mov w0, #1 */
#define A64_RET         0xd65f03c0u

__attribute__((constructor)) static void
ccbridge_init(void)
{
	size_t length = 0;
	const void *cache = _dyld_get_shared_cache_range(&length);
	if (cache == NULL) {
		say("no shared cache: nothing patched");
		return;
	}
	slide = (uintptr_t)cache - CACHE_BASE;
	say("ccbridge in %s, cache slide %#lx", getprogname(), slide);

	/* ApplePDPHelperInterface::init(): return true without AppleIPAppender */
	static const uint32_t pdp_init[] = { A64_MOV_W0_1, A64_RET };
	patch_code("ApplePDPHelperInterface::init", 0x1f2efe170UL, 0xa90157f6d10103ffULL, pdp_init, 2);

	/*
	 * Step 1: the TelephonyBaseband controller's init (libTelephonyBasebandDynamic, opens the
	 * AppleBaseband GPIO/power driver) returns true: our modem is already up and the kernel's.
	 */
	patch_code("TelephonyBasebandController init", 0x1ea614a8cUL, 0x6d0e23e9d10483ffULL, pdp_init, 2);
}
