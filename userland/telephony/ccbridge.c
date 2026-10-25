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
 * The Apple drivers it opens (AppleBaseband: power, reset, GPIO; AppleIPAppender: cellular data)
 * are answered by a fake IOKit in this library (fake_classes); Apple's AppleBasebandManager takes
 * its fake-boot path (no firmware download). ApplePDPHelperInterface::init() patched to return
 * true was the first try: registerHandlerInternal() then asserts (no notification port).
 */
#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#include <dispatch/dispatch.h>
#include <dlfcn.h>
#include <sys/mman.h>
#include <pthread.h>
#include <mach/thread_act.h>
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
#include <errno.h>
#include <spawn.h>
#include <sys/ioctl.h>
#include <sys/select.h>

#define CACHE_BASE              0x180000000UL

extern const void *_dyld_get_shared_cache_range(size_t *length);

static uintptr_t slide;
static int verbose_bridge = 1;
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

/*
 * Inline hooks with trampolines: the first 4 instructions of the target go to an executable
 * buffer followed by a jump back; the target starts with "ldr x16, #8; br x16; .quad hook".
 * Only for targets whose first 4 instructions do not depend on the PC (checked).
 */
static int
pc_relative(uint32_t i)
{
	return (i & 0x9f000000u) == 0x90000000u ||    /* adrp */
	    (i & 0x9f000000u) == 0x10000000u ||       /* adr */
	    (i & 0x7c000000u) == 0x14000000u ||       /* b, bl */
	    (i & 0xff000010u) == 0x54000000u ||       /* b.cond */
	    (i & 0x7e000000u) == 0x34000000u ||       /* cbz, cbnz */
	    (i & 0x7e000000u) == 0x36000000u ||       /* tbz, tbnz */
	    (i & 0x3b000000u) == 0x18000000u;         /* ldr (literal) */
}

static uint32_t *tramp_pool;
static unsigned tramp_used;

static void *hook_at(const char *name, uint32_t *target, void *replacement);

static void *
hook(const char *name, void *replacement)
{
	uint32_t *target = (uint32_t *)dlsym(RTLD_DEFAULT, name);
	if (target == NULL) {
		say("hook %s: no such symbol", name);
		return NULL;
	}
	return hook_at(name, target, replacement);
}

/* a cache function by its unslid address (for images not loaded yet: the whole cache is mapped) */
static void *
hook_cache(const char *name, uintptr_t addr, void *replacement)
{
	return hook_at(name, (uint32_t *)(addr + slide), replacement);
}

static void *
hook_at(const char *name, uint32_t *target, void *replacement)
{
	for (int k = 0; k < 4; k++) {
		if (pc_relative(target[k])) {
			say("hook %s: instruction %d (%08x) depends on the PC: not hooked", name, k, target[k]);
			return NULL;
		}
	}
	if (tramp_pool == NULL) {
		tramp_pool = mmap(NULL, 0x4000, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
		if (tramp_pool == MAP_FAILED) {
			tramp_pool = NULL;
			return NULL;
		}
	}
	uint32_t *t = tramp_pool + tramp_used;
	tramp_used += 8;
	mprotect(tramp_pool, 0x4000, PROT_READ | PROT_WRITE);
	memcpy(t, target, 16);
	t[4] = 0x58000050u;                     /* ldr x16, #8 */
	t[5] = 0xd61f0200u;                     /* br x16 */
	uint64_t back = (uint64_t)(target + 4);
	memcpy(t + 6, &back, 8);
	mprotect(tramp_pool, 0x4000, PROT_READ | PROT_EXEC);
	sys_icache_invalidate(t, 32);

	uint32_t detour[4] = { 0x58000050u, 0xd61f0200u, 0, 0 };
	uint64_t to = (uint64_t)replacement;
	memcpy(detour + 2, &to, 8);
	vm_address_t page = (vm_address_t)target & ~(vm_address_t)(vm_page_size - 1);
	vm_size_t size = ((vm_address_t)(target + 4) - page + vm_page_size - 1) & ~(vm_size_t)(vm_page_size - 1);
	if (vm_protect(mach_task_self(), page, size, FALSE, VM_PROT_READ | VM_PROT_WRITE | VM_PROT_COPY) != KERN_SUCCESS) {
		say("hook %s: not writable", name);
		return NULL;
	}
	memcpy(target, detour, 16);
	vm_protect(mach_task_self(), page, size, FALSE, VM_PROT_READ | VM_PROT_EXECUTE);
	sys_icache_invalidate(target, 16);
	say("hook %s: on", name);
	return t;
}

/*
 * A fake IOKit for the Apple drivers CommCenter wants and we do not have. A fake service or
 * connection is a mach port of our own (a unique name the real IOKit never hands out here);
 * the calls on it are answered and logged.
 */
#define FAKE_MAX        32
static const char *fake_classes[] = { "AppleBaseband", "AppleIPAppender", "AppleBasebandPCIControl", NULL };
static struct { mach_port_t port; const char *cls; int kind; } fakes[FAKE_MAX];   /* kind 1 service, 2 connection, 3 iterator */
static unsigned nfakes;

static mach_port_t
fake_new(const char *cls, int kind)
{
	mach_port_t p = MACH_PORT_NULL;
	if (nfakes >= FAKE_MAX || mach_port_allocate(mach_task_self(), MACH_PORT_RIGHT_RECEIVE, &p) != KERN_SUCCESS) {
		return MACH_PORT_NULL;
	}
	/* a send right too, with extra references: IOObjectRetain/Release work on it like on a real one */
	mach_port_insert_right(mach_task_self(), p, p, MACH_MSG_TYPE_MAKE_SEND);
	mach_port_mod_refs(mach_task_self(), p, MACH_PORT_RIGHT_SEND, 1000);
	fakes[nfakes].port = p;
	fakes[nfakes].cls = cls;
	fakes[nfakes].kind = kind;
	nfakes++;
	return p;
}

static int
fake_find(mach_port_t p, const char **cls)
{
	for (unsigned i = 0; i < nfakes; i++) {
		if (fakes[i].port == p && p != MACH_PORT_NULL) {
			if (cls) *cls = fakes[i].cls;
			return fakes[i].kind;
		}
	}
	return 0;
}

/* the class a matching dictionary asks for, if it is one we fake */
static const char *
fake_class_of(CFDictionaryRef d, char *desc, size_t descsz)
{
	desc[0] = 0;
	if (d == NULL || CFGetTypeID(d) != CFDictionaryGetTypeID()) {
		return NULL;
	}
	static const char *keys[] = { "IOProviderClass", "IONameMatch", "IOClass" };
	for (int k = 0; k < 3; k++) {
		CFStringRef key = CFStringCreateWithCString(NULL, keys[k], kCFStringEncodingUTF8);
		CFTypeRef v = CFDictionaryGetValue(d, key);
		CFRelease(key);
		char buf[128] = "";
		if (v != NULL && CFGetTypeID(v) == CFStringGetTypeID()) {
			CFStringGetCString(v, buf, sizeof(buf), kCFStringEncodingUTF8);
		} else if (v != NULL && CFGetTypeID(v) == CFArrayGetTypeID() && CFArrayGetCount(v) > 0) {
			CFTypeRef e = CFArrayGetValueAtIndex(v, 0);
			if (CFGetTypeID(e) == CFStringGetTypeID()) CFStringGetCString(e, buf, sizeof(buf), kCFStringEncodingUTF8);
		}
		if (buf[0]) {
			snprintf(desc, descsz, "%s=%s", keys[k], buf);
			for (int c = 0; fake_classes[c]; c++) {
				if (strcmp(buf, fake_classes[c]) == 0) return fake_classes[c];
			}
			return NULL;
		}
	}
	snprintf(desc, descsz, "(other matching)");
	return NULL;
}

static io_service_t (*real_GetMatchingService)(mach_port_t, CFDictionaryRef);
static kern_return_t (*real_GetMatchingServices)(mach_port_t, CFDictionaryRef, io_iterator_t *);
static kern_return_t (*real_AddMatchingNotification)(IONotificationPortRef, const io_name_t, CFDictionaryRef,
    IOServiceMatchingCallback, void *, io_iterator_t *);
static io_object_t (*real_IteratorNext)(io_iterator_t);
static kern_return_t (*real_ServiceOpen)(io_service_t, task_port_t, uint32_t, io_connect_t *);
static kern_return_t (*real_ServiceClose)(io_connect_t);
static kern_return_t (*real_CallMethod)(mach_port_t, uint32_t, const uint64_t *, uint32_t, const void *, size_t,
    uint64_t *, uint32_t *, void *, size_t *);
static kern_return_t (*real_CallAsyncMethod)(mach_port_t, uint32_t, mach_port_t, uint64_t *, uint32_t,
    const uint64_t *, uint32_t, const void *, size_t, uint64_t *, uint32_t *, void *, size_t *);
static CFTypeRef (*real_SearchCFProperty)(io_registry_entry_t, const io_name_t, CFStringRef, CFAllocatorRef, IOOptionBits);
static kern_return_t (*real_AddInterestNotification)(IONotificationPortRef, io_service_t, const io_name_t,
    IOServiceInterestCallback, void *, io_object_t *);

static io_service_t
my_GetMatchingService(mach_port_t main, CFDictionaryRef matching)
{
	char desc[160];
	const char *cls = fake_class_of(matching, desc, sizeof(desc));
	if (cls != NULL) {
		if (matching) CFRelease(matching);  /* consumed, as by IOKit */
		io_service_t s = fake_new(cls, 1);
		say("IOServiceGetMatchingService %s -> fake %#x", desc, s);
		return s;
	}
	io_service_t s = real_GetMatchingService(main, matching);
	say("IOServiceGetMatchingService %s -> %#x", desc, s);
	return s;
}

static kern_return_t
my_GetMatchingServices(mach_port_t main, CFDictionaryRef matching, io_iterator_t *it)
{
	char desc[160];
	const char *cls = fake_class_of(matching, desc, sizeof(desc));
	if (cls != NULL) {
		if (matching) CFRelease(matching);
		*it = fake_new(cls, 3);
		say("IOServiceGetMatchingServices %s -> fake iterator %#x", desc, *it);
		return KERN_SUCCESS;
	}
	kern_return_t kr = real_GetMatchingServices(main, matching, it);
	say("IOServiceGetMatchingServices %s -> %#x", desc, kr);
	return kr;
}

static kern_return_t
my_AddMatchingNotification(IONotificationPortRef port, const io_name_t type, CFDictionaryRef matching,
    IOServiceMatchingCallback cb, void *ref, io_iterator_t *it)
{
	char desc[160];
	const char *cls = fake_class_of(matching, desc, sizeof(desc));
	if (cls != NULL) {
		if (matching) CFRelease(matching);
		*it = fake_new(cls, 3);
		say("IOServiceAddMatchingNotification %s %s -> fake iterator %#x", type, desc, *it);
		return KERN_SUCCESS;
	}
	kern_return_t kr = real_AddMatchingNotification(port, type, matching, cb, ref, it);
	say("IOServiceAddMatchingNotification %s %s -> %#x", type, desc, kr);
	return kr;
}

/* a fake iterator gives one fake service of its class, then nothing */
static io_object_t
my_IteratorNext(io_iterator_t it)
{
	const char *cls;
	if (fake_find(it, &cls) == 3) {
		for (unsigned i = 0; i < nfakes; i++) {
			if (fakes[i].port == it) {
				if (fakes[i].kind == 3 && fakes[i].cls != NULL) {
					fakes[i].cls = NULL;    /* drained */
					io_service_t s = fake_new(cls, 1);
					say("IOIteratorNext fake %#x -> fake service %#x (%s)", it, s, cls);
					return s;
				}
			}
		}
		return MACH_PORT_NULL;
	}
	return real_IteratorNext(it);
}

static kern_return_t
my_ServiceOpen(io_service_t s, task_port_t task, uint32_t type, io_connect_t *c)
{
	const char *cls;
	if (fake_find(s, &cls) == 1) {
		*c = fake_new(cls, 2);
		say("IOServiceOpen fake %s type %u -> %#x", cls, type, *c);
		return KERN_SUCCESS;
	}
	return real_ServiceOpen(s, task, type, c);
}

static kern_return_t
my_ServiceClose(io_connect_t c)
{
	if (fake_find(c, NULL) == 2) {
		return KERN_SUCCESS;
	}
	return real_ServiceClose(c);
}

static kern_return_t
fake_call(const char *how, mach_port_t c, const char *cls, uint32_t sel, const uint64_t *in, uint32_t inCnt,
    size_t inStructCnt, uint64_t *out, uint32_t *outCnt, void *outStruct, size_t *outStructCnt)
{
	char ins[128] = "";
	for (uint32_t k = 0, o = 0; k < inCnt && k < 6 && o < sizeof(ins) - 24; k++) {
		o += (uint32_t)snprintf(ins + o, sizeof(ins) - o, " %#llx", (unsigned long long)in[k]);
	}
	say("%s %s sel %u in[%u]:%s struct %zu -> out %u, struct %zu", how, cls, sel, inCnt, ins, inStructCnt,
	    outCnt ? *outCnt : 0, outStructCnt ? *outStructCnt : 0);
	if (outCnt) {
		for (uint32_t k = 0; k < *outCnt; k++) out[k] = 0;
	}
	if (outStruct && outStructCnt) {
		memset(outStruct, 0, *outStructCnt);
	}
	return KERN_SUCCESS;
}

static kern_return_t
my_CallMethod(mach_port_t c, uint32_t sel, const uint64_t *in, uint32_t inCnt, const void *inStruct, size_t inStructCnt,
    uint64_t *out, uint32_t *outCnt, void *outStruct, size_t *outStructCnt)
{
	const char *cls;
	if (fake_find(c, &cls) == 2) {
		return fake_call("IOConnectCallMethod", c, cls, sel, in, inCnt, inStructCnt, out, outCnt, outStruct, outStructCnt);
	}
	return real_CallMethod(c, sel, in, inCnt, inStruct, inStructCnt, out, outCnt, outStruct, outStructCnt);
}

static kern_return_t
my_CallAsyncMethod(mach_port_t c, uint32_t sel, mach_port_t wake, uint64_t *ref, uint32_t refCnt, const uint64_t *in,
    uint32_t inCnt, const void *inStruct, size_t inStructCnt, uint64_t *out, uint32_t *outCnt, void *outStruct,
    size_t *outStructCnt)
{
	const char *cls;
	if (fake_find(c, &cls) == 2) {
		return fake_call("IOConnectCallAsyncMethod", c, cls, sel, in, inCnt, inStructCnt, out, outCnt, outStruct, outStructCnt);
	}
	return real_CallAsyncMethod(c, sel, wake, ref, refCnt, in, inCnt, inStruct, inStructCnt, out, outCnt, outStruct, outStructCnt);
}

static kern_return_t (*real_CallScalar)(mach_port_t, uint32_t, const uint64_t *, uint32_t, uint64_t *, uint32_t *);
static kern_return_t (*real_CallStruct)(mach_port_t, uint32_t, const void *, size_t, void *, size_t *);
static kern_return_t (*real_CallAsyncScalar)(mach_port_t, uint32_t, mach_port_t, uint64_t *, uint32_t, const uint64_t *,
    uint32_t, uint64_t *, uint32_t *);
static kern_return_t (*real_CallAsyncStruct)(mach_port_t, uint32_t, mach_port_t, uint64_t *, uint32_t, const void *, size_t,
    void *, size_t *);

static kern_return_t
my_CallScalar(mach_port_t c, uint32_t sel, const uint64_t *in, uint32_t inCnt, uint64_t *out, uint32_t *outCnt)
{
	const char *cls;
	if (fake_find(c, &cls) == 2) {
		return fake_call("IOConnectCallScalarMethod", c, cls, sel, in, inCnt, 0, out, outCnt, NULL, NULL);
	}
	return real_CallScalar(c, sel, in, inCnt, out, outCnt);
}

static kern_return_t
my_CallStruct(mach_port_t c, uint32_t sel, const void *inS, size_t inSCnt, void *outS, size_t *outSCnt)
{
	const char *cls;
	if (fake_find(c, &cls) == 2) {
		return fake_call("IOConnectCallStructMethod", c, cls, sel, NULL, 0, inSCnt, NULL, NULL, outS, outSCnt);
	}
	return real_CallStruct(c, sel, inS, inSCnt, outS, outSCnt);
}

static kern_return_t
my_CallAsyncScalar(mach_port_t c, uint32_t sel, mach_port_t wake, uint64_t *ref, uint32_t refCnt, const uint64_t *in,
    uint32_t inCnt, uint64_t *out, uint32_t *outCnt)
{
	const char *cls;
	if (fake_find(c, &cls) == 2) {
		return fake_call("IOConnectCallAsyncScalarMethod", c, cls, sel, in, inCnt, 0, out, outCnt, NULL, NULL);
	}
	return real_CallAsyncScalar(c, sel, wake, ref, refCnt, in, inCnt, out, outCnt);
}

static kern_return_t
my_CallAsyncStruct(mach_port_t c, uint32_t sel, mach_port_t wake, uint64_t *ref, uint32_t refCnt, const void *inS,
    size_t inSCnt, void *outS, size_t *outSCnt)
{
	const char *cls;
	if (fake_find(c, &cls) == 2) {
		return fake_call("IOConnectCallAsyncStructMethod", c, cls, sel, NULL, 0, inSCnt, NULL, NULL, outS, outSCnt);
	}
	return real_CallAsyncStruct(c, sel, wake, ref, refCnt, inS, inSCnt, outS, outSCnt);
}

static CFTypeRef
my_SearchCFProperty(io_registry_entry_t e, const io_name_t plane, CFStringRef key, CFAllocatorRef a, IOOptionBits o)
{
	const char *cls;
	if (fake_find(e, &cls) != 0) {
		char k[128] = "";
		if (key) CFStringGetCString(key, k, sizeof(k), kCFStringEncodingUTF8);
		say("property %s of fake %s: none", k, cls);
		return NULL;
	}
	return real_SearchCFProperty(e, plane, key, a, o);
}

static kern_return_t
my_AddInterestNotification(IONotificationPortRef port, io_service_t s, const io_name_t type, IOServiceInterestCallback cb,
    void *ref, io_object_t *n)
{
	const char *cls;
	if (fake_find(s, &cls) == 1) {
		*n = fake_new(cls, 4);
		say("IOServiceAddInterestNotification %s on fake %s -> %#x", type, cls, *n);
		return KERN_SUCCESS;
	}
	return real_AddInterestNotification(port, s, type, cb, ref, n);
}

/*
 * Spies on AppleBasebandManager's BootModule (libBasebandManager, exported C++ methods): which
 * stage runs, with which state. They log and go on to the original.
 */
static void *bm_this;
static void (*real_bm_initialize)(void *, void *);
static void (*real_bm_start)(void *, void *);
static void (*real_bm_finish)(void *);
static void (*real_bm_change)(void *, int);
static void (*real_bm_boot)(void *);

static void my_bm_initialize(void *t, void *g) { bm_this = t; say("BootModule::initialize(%p)", t); real_bm_initialize(t, g); }
static void my_bm_start(void *t, void *g) { bm_this = t; say("BootModule::start(%p)", t); real_bm_start(t, g); }
static void my_bm_finish(void *t) { say("BootModule::finishInitialization_sync(%p)", t); real_bm_finish(t); }
static void *mav_controller;
#define MAX_SHIMS 4
static void *qmi_shims[MAX_SHIMS];
static unsigned qmi_iface[MAX_SHIMS], nshims;
static void *pci_shims_seen[4];
static unsigned npci_seen;
static int (*real_shim_init)(void *);
static int (*real_shim_engage)(void *, void *);
static int my_shim_init(void *t) { int r = real_shim_init(t); if (npci_seen < 4) pci_shims_seen[npci_seen++] = t; say("pci_shim::init(%p) -> %d", t, r); return r; }
static int (*real_ctl_engage)(void *);
static void *ipc_controller;
static void (*real_ipc_change)(void *, int);
static void my_ipc_change(void *t, int st) { ipc_controller = t; say("RouterIPCController::changeState_sync(%p, %d)", t, st); real_ipc_change(t, st); }
static void (*real_ipc_ctor)(void *, void *);
static void (*real_ipc_activate)(void *);
static void my_ipc_ctor(void *t, void *w) { ipc_controller = t; say("RouterIPCController %p", t); real_ipc_ctor(t, w); }
static int ipc_activated, bm_booting, engaged_once;
static void engage_later(void *arg);
/* engage once both have happened: the IPC controller activated (kDisengaged) and the fake boot began */
static void
maybe_engage(void)
{
	if (ipc_activated && bm_booting && !__sync_lock_test_and_set(&engaged_once, 1)) {
		dispatch_after_f(dispatch_time(DISPATCH_TIME_NOW, NSEC_PER_SEC / 2),
		    dispatch_get_global_queue(QOS_CLASS_DEFAULT, 0), NULL, engage_later);
	}
}
static void my_ipc_activate(void *t) { ipc_controller = t; say("RouterIPCController::activate(%p)", t); real_ipc_activate(t); ipc_activated = 1; maybe_engage(); }

static void
engage_later(void *arg)
{
	(void)arg;
	if (ipc_controller != NULL) {
		/* RouterIPCController::engage_sync(): what ABM does once the baseband is up (setup, router engage, kEngaged, workers) */
		int (*engage_sync)(void *) = (int (*)(void *))(0x1b7fa05a0UL + slide);
		say("RouterIPCController::engage_sync(%p) ourselves", ipc_controller);
		int r = engage_sync(ipc_controller);
		say("  engage_sync -> %d", r);
	} else if (mav_controller != NULL && real_ctl_engage != NULL) {
		say("engaging mav_router controller %p ourselves", mav_controller);
		real_ctl_engage(mav_controller);
		say("  engage returned");
		/* the controller engages only the first PCI device: the second QMI channel too */
		for (unsigned i = 0; i < npci_seen; i++) {
			int done = 0;
			for (unsigned k = 0; k < nshims; k++) done |= qmi_shims[k] == pci_shims_seen[i];
			if (!done && real_shim_engage != NULL) {
				dispatch_group_t g = dispatch_group_create();
				say("engaging pci_shim %p ourselves", pci_shims_seen[i]);
				real_shim_engage(pci_shims_seen[i], g);
			}
		}
	}
}

static void
my_bm_change(void *t, int st)
{
	say("BootModule::changeState_sync(%p, %d)", t, st);
	real_bm_change(t, st);
	if (st == 2) {      /* Booting (fake boot): nothing engages the PCI devices, we do, before QMux starts */
		bm_booting = 1;
		maybe_engage();
	}
}
static void my_bm_boot(void *t) { say("BootModule::boot_sync(%p)", t); real_bm_boot(t); }

static void *(*real_abm_init)(void *, void *, void *, void *);
static void *(*real_abm_start)(void *, void *, void *, void *);
static void *(*real_srv_start)(void *, void *);
static void *my_abm_init(void *a, void *b, void *c, void *d) { say("AppleBasebandManagerInitServer(%p, %p)", a, b); void *r = real_abm_init(a, b, c, d); say("  InitServer -> %p", r); return r; }
static void *my_abm_start(void *a, void *b, void *c, void *d) { say("AppleBasebandManagerStartServer(%p, %p)", a, b); void *r = real_abm_start(a, b, c, d); say("  StartServer -> %p", r); return r; }
static void *my_srv_start(void *t, void *g) { say("ABMServer::start(%p)", t); return real_srv_start(t, g); }

/* every thread's stack (frame pointer walk), into the log: who waits on what */
static void
dump_stacks(const char *why)
{
	thread_act_array_t th;
	mach_msg_type_number_t n;
	if (task_threads(mach_task_self(), &th, &n) != KERN_SUCCESS) {
		return;
	}
	say("--- stacks (%s): %u threads", why, n);
	for (mach_msg_type_number_t i = 0; i < n; i++) {
		if (th[i] == mach_thread_self()) {
			continue;
		}
		arm_thread_state64_t st;
		mach_msg_type_number_t cnt = ARM_THREAD_STATE64_COUNT;
		if (thread_get_state(th[i], ARM_THREAD_STATE64, (thread_state_t)&st, &cnt) != KERN_SUCCESS) {
			continue;
		}
		char line[400];
		int o = snprintf(line, sizeof(line), "T%u pc %llx lr %llx:", i, st.__pc - slide, st.__lr - slide);
		uint64_t fp = st.__fp;
		for (int k = 0; k < 14 && fp && (fp & 7) == 0 && o < (int)sizeof(line) - 20; k++) {
			uint64_t *f = (uint64_t *)fp;
			uint64_t ret = f[1];
			if (ret == 0) break;
			o += snprintf(line + o, sizeof(line) - (size_t)o, " %llx", ret - slide);
			if (f[0] <= fp) break;
			fp = f[0];
		}
		say("%s", line);
	}
}

static void *
watchdog(void *arg)
{
	(void)arg;
	sleep(20);
	dump_stacks("20 s");
	return NULL;
}

/*
 * libPCITransport replaced for the QMI channels: TelephonyBasebandPCITransportCreate fills its
 * 0x58-byte object (+0x00 Write, +0x08 Read, +0x10 UnblockRead, +0x18 FlushRead, +0x20 SendImage,
 * +0x28 ReadRegister, +0x30 Free, +0x38 IsValid, +0x50 internal) with ours. mav_router's pci_shim
 * keeps it at shim+0x38; what the shim transmits is taken in handle_transmit (below), what the
 * modem answers goes in through pci_shim::handleRead.
 */

static int tp_ok(void) { return 1; }
static int tp_no(void) { return 0; }

static int (*real_tp_create)(void *, void *);
static int
my_tp_create(void *out, void *params)
{
	const uint8_t *p = params;
	uint32_t iface;
	memcpy(&iface, p, 4);
	char hex[0x58 * 3 + 1];
	for (int i = 0; i < 0x58; i++) snprintf(hex + 3 * i, 4, "%02x ", p[i]);
	say("TelephonyBasebandPCITransportCreate(%p, interface %u, params: %s)", out, iface, hex);
	void **o = out;
	memset(out, 0, 0x58);
	o[0] = (void *)tp_ok;           /* Write: never reached, handle_transmit is ours */
	o[1] = (void *)tp_no;           /* Read */
	o[2] = (void *)tp_ok;           /* UnblockRead */
	o[3] = (void *)tp_ok;           /* FlushRead */
	o[4] = (void *)tp_no;           /* SendImage */
	o[5] = (void *)tp_no;           /* ReadRegister */
	o[6] = (void *)tp_ok;           /* Free */
	o[7] = (void *)tp_ok;           /* IsValid */
	o[10] = (void *)out;            /* internal: anything not NULL */
	if (nshims < MAX_SHIMS) {
		qmi_shims[nshims] = (char *)out - 0x38;
		qmi_iface[nshims] = iface;
		say("  QMI transport %u: shim %p", nshims, qmi_shims[nshims]);
		nshims++;
	}
	return 1;
}

/*
 * The QMUX <-> QRTR bridge. mav_router's PCI control channels carry standard Qualcomm QMUX frames
 * (I/F type 1, u16 length, control flags, service, client id, then the QMI SDU), as libqmi
 * speaks to a USB modem. Our modem speaks QMI over QRTR (/dev/redwood-qmiN, kernel 0220):
 *   - QMI_CTL (service 0) is answered here: SYNC, client ids, version info.
 *   - every other service gets one QRTR client port (one /dev/redwood-qmi node) shared by all of
 *     CommCenter's QMUX clients of that service on that channel; requests get a fresh txn,
 *     answers go back to the client that asked with its own txn, indications to every client.
 *   - services the modem does not have (Apple's own 0xe1..) answer "not supported".
 * Frames to CommCenter go in through pci_shim::handleRead (0x1ea5fe404).
 */
struct sqhdr { uint32_t node, port; };
struct sqlookup { uint32_t svc, inst, index, node, port, found_inst; };
#define SQ_LOOKUP       _IOWR('q', 1, struct sqlookup)

#define BR_SVCS         24
#define BR_CIDS         16
struct br_svc {
	uint8_t svc;
	int fd;                         /* -1: not on the modem, answered here */
	uint32_t node, port;
	uint8_t cids[BR_CIDS];
	unsigned ncids;
	struct { uint16_t txn; uint8_t cid; uint8_t used; uint16_t orig; uint16_t as_ind; uint16_t msg; uint64_t t; } pend[64];
	uint16_t next_txn;
	uint8_t ind_on[256];            /* indication switches some client turned on (br_merge_ind_reg) */
};
struct br_chan {
	void *shim;
	struct br_svc svcs[BR_SVCS];
	unsigned nsvcs;
	uint8_t next_cid[256];
};
static struct br_chan br_chans[MAX_SHIMS];
static pthread_mutex_t br_lock = PTHREAD_RECURSIVE_MUTEX_INITIALIZER;
static void (*shim_handle_read)(void *, const void *, unsigned);
static pthread_t br_reader;
static int br_reader_started;

static void
hexsay(const char *what, const uint8_t *b, size_t n)
{
	char hex[3 * 48 + 1] = "";
	for (size_t i = 0; i < n && i < 48; i++) snprintf(hex + 3 * i, 4, "%02x ", b[i]);
	say("%s (%zu): %s", what, n, hex);
}

/* one QMUX frame to CommCenter */
static void
br_to_cc(struct br_chan *c, uint8_t svc, uint8_t cid, const uint8_t *sdu, size_t n)
{
	/* handleRead's dispatch_data takes the buffer and frees it (DISPATCH_DATA_DESTRUCTOR_FREE) */
	uint8_t *f = malloc(n + 6);
	if (f == NULL) return;
	f[0] = 0x01;
	uint16_t l = (uint16_t)(n + 5);
	memcpy(f + 1, &l, 2);
	f[3] = 0x80;                    /* from the service */
	f[4] = svc;
	f[5] = cid;
	memcpy(f + 6, sdu, n);
	if (verbose_bridge) hexsay("  -> cc", f, n + 6);
	/*
	 * Delivered from our own serial queue, never under br_lock: handleRead may wait on
	 * mav_router's queue, which may be in my_shim_tx waiting for br_lock (the reader thread and
	 * the router deadlocked: no modem answer reached CommCenter).
	 */
	static dispatch_queue_t q;
	static dispatch_once_t once;
	dispatch_once(&once, ^{ q = dispatch_queue_create("ccbridge.deliver", DISPATCH_QUEUE_SERIAL); });
	void *shim = c->shim;
	unsigned len = (unsigned)(n + 6);
	dispatch_async(q, ^{ shim_handle_read(shim, f, len); });
}

static struct br_chan *
br_chan_of(void *shim)
{
	for (unsigned i = 0; i < MAX_SHIMS; i++) {
		if (br_chans[i].shim == shim) return &br_chans[i];
	}
	for (unsigned i = 0; i < MAX_SHIMS; i++) {
		if (br_chans[i].shim == NULL) { br_chans[i].shim = shim; return &br_chans[i]; }
	}
	return NULL;
}

static int br_test_block(uint8_t svc);

static struct br_svc *
br_svc_of(struct br_chan *c, uint8_t svc, int create)
{
	for (unsigned i = 0; i < c->nsvcs; i++) {
		if (c->svcs[i].svc == svc) return &c->svcs[i];
	}
	if (!create || c->nsvcs >= BR_SVCS) return NULL;
	struct br_svc *v = &c->svcs[c->nsvcs++];
	memset(v, 0, sizeof(*v));
	v->svc = svc;
	v->fd = -1;
	v->next_txn = 1;
	/*
	 * kept away from the modem, answered here (QMI_ERR_NOT_SUPPORTED): with CommCenter talking
	 * to it the modem died minutes after registering ("dog_hb.c: Task starvation: dcc"),
	 * never with qmitool alone. First suspects: COEX (0x22, re-configured every 30 s), QOS (0x04)
	 * and 0x2a.
	 */
	if (svc == 0x22 || svc == 0x04 || svc == 0x2a || br_test_block(svc)) {
		say("bridge: service 0x%x kept from the modem (answered here)", svc);
		return v;
	}
	char path[32];
	for (int i = 0; i < 32; i++) {
		snprintf(path, sizeof(path), "/dev/redwood-qmi%d", i);
		int fd = open(path, O_RDWR);
		if (fd < 0) continue;
		struct sqlookup lk = { svc, ~0u, 0, 0, 0, 0 };
		if (ioctl(fd, SQ_LOOKUP, &lk) != 0) {
			close(fd);
			say("bridge: service 0x%x is not on the modem (answered here)", svc);
			break;
		}
		v->fd = fd;
		v->node = lk.node;
		v->port = lk.port;
		say("bridge: service 0x%x -> %s (modem %u:%x)", svc, path, lk.node, lk.port);
		break;
	}
	return v;
}

/* a QMI_CTL answer: flags 1, txn, msg, TLVs (result first) */
static void
br_ctl_reply(struct br_chan *c, uint8_t txn, uint16_t msg, const uint8_t *tlvs, size_t tn, uint16_t err)
{
	uint8_t sdu[512];
	sdu[0] = 0x01;
	sdu[1] = txn;
	memcpy(sdu + 2, &msg, 2);
	uint8_t res[7] = { 0x02, 4, 0, (uint8_t)(err ? 1 : 0), 0, (uint8_t)err, (uint8_t)(err >> 8) };
	uint16_t len = (uint16_t)(7 + tn);
	memcpy(sdu + 4, &len, 2);
	memcpy(sdu + 6, res, 7);
	if (tn) memcpy(sdu + 13, tlvs, tn);
	br_to_cc(c, 0, 0, sdu, 13 + tn);
}

static void
br_ctl(struct br_chan *c, const uint8_t *sdu, size_t n)
{
	if (n < 6) return;
	uint8_t txn = sdu[1];
	uint16_t msg, len;
	memcpy(&msg, sdu + 2, 2);
	memcpy(&len, sdu + 4, 2);
	const uint8_t *t = sdu + 6;
	switch (msg) {
	case 0x0022: {                  /* GET_CLIENT_ID: TLV 1 service */
		uint8_t svc = (len >= 4 && t[0] == 0x01) ? t[3] : 0;
		struct br_svc *v = br_svc_of(c, svc, 1);
		uint8_t cid = ++c->next_cid[svc];
		if (cid == 0 || cid == 0xff) cid = c->next_cid[svc] = 1;
		if (v && v->ncids < BR_CIDS) v->cids[v->ncids++] = cid;
		uint8_t r[5] = { 0x01, 2, 0, svc, cid };
		say("bridge: CTL client id for service 0x%x: %u", svc, cid);
		br_ctl_reply(c, txn, msg, r, 5, 0);
		break;
	}
	case 0x0023: {                  /* RELEASE_CLIENT_ID: TLV 1 {service, cid} */
		uint8_t svc = len >= 5 ? t[3] : 0, cid = len >= 5 ? t[4] : 0;
		struct br_svc *v = br_svc_of(c, svc, 0);
		for (unsigned i = 0; v && i < v->ncids; i++) {
			if (v->cids[i] == cid) { v->cids[i] = v->cids[--v->ncids]; break; }
		}
		uint8_t r[5] = { 0x01, 2, 0, svc, cid };
		br_ctl_reply(c, txn, msg, r, 5, 0);
		break;
	}
	case 0x0021: {                  /* GET_VERSION_INFO: TLV 1 {count, {svc, major u16, minor u16}...} */
		static const uint8_t svcs[] = { 0, 1, 2, 3, 4, 5, 6, 9, 0x0a, 0x0b, 0x0c, 0x10, 0x1a, 0x24, 0x2f, 0xe1, 0xe2, 0xe4, 0xe8 };
		uint8_t r[3 + 1 + sizeof(svcs) * 5];
		uint16_t l = (uint16_t)(1 + sizeof(svcs) * 5);
		r[0] = 0x01;
		memcpy(r + 1, &l, 2);
		r[3] = sizeof(svcs);
		for (unsigned i = 0; i < sizeof(svcs); i++) {
			uint8_t *e = r + 4 + i * 5;
			e[0] = svcs[i]; e[1] = 1; e[2] = 0; e[3] = 50; e[4] = 0;
		}
		br_ctl_reply(c, txn, msg, r, sizeof(r), 0);
		break;
	}
	default:                        /* SYNC (0x27), SET_INSTANCE_ID (0x20), data format...: fine */
		say("bridge: CTL msg 0x%04x answered ok", msg);
		br_ctl_reply(c, txn, msg, NULL, 0, 0);
		break;
	}
}

static void *br_reader_main(void *arg);

/*
 * NAS: CommCenter asks its registration through Apple's own NAS messages (0x556a, 0x555b, not
 * on this modem) and otherwise waits for indications, which the modem sends only on a change:
 * a modem that registered before CommCenter started never tells it. The bridge asks GET SERVING
 * SYSTEM (0x24) itself and hands the answer in as the 0x24 indication; now (after an indication
 * register) and every 20 s.
 */
static void
br_nas_query(struct br_chan *c, uint16_t msg, uint16_t as_ind)
{
	struct br_svc *v = br_svc_of(c, 0x03, 0);
	if (v == NULL || v->fd < 0 || v->ncids == 0) {
		say("bridge: NAS poll 0x%04x on channel %p: no NAS client (%p)", msg, c->shim, (void *)v);
		return;
	}
	unsigned slot = v->next_txn % 64;
	uint16_t nt = v->next_txn++;
	if (v->next_txn == 0) v->next_txn = 1;
	v->pend[slot].txn = nt;
	v->pend[slot].cid = 0;
	v->pend[slot].orig = 0;
	v->pend[slot].used = 1;
	v->pend[slot].as_ind = as_ind;
	v->pend[slot].msg = msg;
	v->pend[slot].t = clock_gettime_nsec_np(CLOCK_MONOTONIC);
	uint8_t buf[sizeof(struct sqhdr) + 7];
	struct sqhdr h = { v->node, v->port };
	memcpy(buf, &h, sizeof(h));
	uint8_t q[7] = { 0x00, 0, 0, 0, 0, 0, 0 };
	memcpy(q + 1, &nt, 2);
	memcpy(q + 3, &msg, 2);
	memcpy(buf + sizeof(h), q, 7);
	ssize_t w = write(v->fd, buf, sizeof(buf));
	say("bridge: NAS poll 0x%04x txn %u -> %zd", msg, nt, w);
}

static void
br_nas_poll(void *arg)
{
	(void)arg;
	pthread_mutex_lock(&br_lock);
	for (unsigned i = 0; i < MAX_SHIMS; i++) {
		if (br_chans[i].shim == NULL) continue;
		br_nas_query(&br_chans[i], 0x0024, 0x0024);
		/*
		 * SYS_INFO: CommCenter never asks for it and the modem only indicates changes, so
		 * after CommCenter starts it has none and shows "No Service". The answer as indication
		 * 0x4e keeps only TLVs 0x10-0x1d (service status and system info of each RAT), which
		 * the answer and the indication number alike; the later ones differ, and passing
		 * them made Apple's parser throw tlv::TlvParseException.
		 */
		br_nas_query(&br_chans[i], 0x004d, 0x004e);
	}
	pthread_mutex_unlock(&br_lock);
}

static void
br_nas_poll_soon(struct br_chan *c)
{
	(void)c;
	static int timer_on;
	dispatch_after_f(dispatch_time(DISPATCH_TIME_NOW, NSEC_PER_SEC), dispatch_get_global_queue(QOS_CLASS_DEFAULT, 0),
	    NULL, br_nas_poll);
	if (!timer_on) {
		timer_on = 1;
		dispatch_source_t t = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0,
		    dispatch_get_global_queue(QOS_CLASS_UTILITY, 0));
		dispatch_source_set_timer(t, dispatch_time(DISPATCH_TIME_NOW, 20 * NSEC_PER_SEC), 20 * NSEC_PER_SEC, NSEC_PER_SEC);
		dispatch_source_set_event_handler_f(t, br_nas_poll);
		dispatch_resume(t);
	}
}

/*
 * All of CommCenter's clients of a service share one client on the modem, and the modem keeps
 * one set of indication switches per client: the last "register indications" won. NAS client 1
 * asked for SYS_INFO (0x4e, TLV 0x18 = 1) and the next ones, with 0x18 = 0, turned it off again,
 * so CommCenter never heard that the modem had registered. A switch any client turned on stays on
 * (the bridge hands each indication to every client anyway).
 */
/* bisecting the IPA assert: services listed in /var/tmp/ccbridge.block (hex, one per line) stay here */
static int
br_test_block(uint8_t svc)
{
	FILE *f = fopen("/var/tmp/ccbridge.block", "r");
	unsigned v;
	int hit = 0;
	if (f == NULL) return 0;
	while (!hit && fscanf(f, "%x", &v) == 1) hit = v == svc;
	fclose(f);
	return hit;
}

/* bisecting: "SVC MSG" lines (hex) in /var/tmp/ccbridge.okmsg are answered success here */
static int
br_test_ok(uint8_t svc, uint16_t msg)
{
	FILE *f = fopen("/var/tmp/ccbridge.okmsg", "r");
	unsigned a, b;
	int hit = 0;
	if (f == NULL) return 0;
	while (!hit && fscanf(f, "%x %x", &a, &b) == 2) hit = a == svc && b == msg;
	fclose(f);
	return hit;
}

static void
br_merge_ind_reg(struct br_svc *v, uint16_t msg, uint8_t *sdu, size_t sn)
{
	int reg = (v->svc == 0x03 && msg == 0x0003) ||     /* NAS indication register */
	    (v->svc == 0x09 && msg == 0x0003) ||           /* VOICE indication register */
	    (v->svc == 0x02 && msg == 0x0001) ||           /* DMS set event report */
	    (v->svc == 0x01 && msg == 0x0001) ||           /* WDS set event report */
	    (v->svc == 0x05 && msg == 0x0001);             /* WMS set event report */
	if (!reg || sn < 7) return;
	size_t end = 7 + (size_t)(sdu[5] | sdu[6] << 8);
	if (end > sn) end = sn;
	for (size_t o = 7; o + 3 <= end;) {
		uint8_t type = sdu[o];
		size_t len = sdu[o + 1] | sdu[o + 2] << 8;
		if (o + 3 + len > end) break;
		if (len == 1) {
			if (sdu[o + 3]) {
				v->ind_on[type] = 1;
			} else if (v->ind_on[type]) {
				sdu[o + 3] = 1;
				say("bridge: service 0x%x msg 0x%04x: indication switch 0x%02x kept on", v->svc, msg, type);
			}
		}
		o += 3 + len;
	}
}

static void
br_from_cc(struct br_chan *c, const uint8_t *f, size_t n)
{
	if (n < 6 || f[0] != 0x01) {
		hexsay("bridge: not a QMUX frame", f, n);
		return;
	}
	uint8_t svc = f[4], cid = f[5];
	const uint8_t *sdu = f + 6;
	size_t sn = n - 6;
	if (svc == 0) {
		br_ctl(c, sdu, sn);
		return;
	}
	if (sn < 7) return;
	struct br_svc *v = br_svc_of(c, svc, 1);
	if (v == NULL) return;
	uint16_t txn, msg;
	memcpy(&txn, sdu + 1, 2);
	memcpy(&msg, sdu + 3, 2);
	/*
	 * WDS stays away from the modem: its data call task hangs as soon as the data path is set
	 * up (no IPA on our side) and ten seconds later the modem watchdog kills it ("dog_hb.c:367:
	 * Task starvation: dcc"), with qmitool alone as well: WDS 0x0085 alone did it, 0x00cd after
	 * CommCenter's other WDS set-up too. With WDS let through but 0x0085 held back, the modem
	 * died of "ipa_dl_opt_lte.c:551: IPA Assert: new_free_space > 0" instead.
	 */
	if (br_test_ok(svc, msg)) {
		uint8_t r[14] = { 0x02, 0, 0, 0, 0, 7, 0, 0x02, 4, 0, 0, 0, 0, 0 };
		memcpy(r + 1, &txn, 2);
		memcpy(r + 3, &msg, 2);
		say("bridge: service 0x%x msg 0x%04x answered ok here", svc, msg);
		br_to_cc(c, svc, cid, r, sizeof(r));
		return;
	}
	if (v->fd < 0 || svc == 0x01) {
		/*
		 * not on the modem (Apple's own services: BSP 0xe2, AUDIO 0xe1, ...): QMI_ERR_NOT_SUPPORTED
		 * (0x5e). An empty success is worse: BSP 0xb001/0xb004 (CPMS power budget) then read an
		 * empty vector with at() and CommCenter dies of std::out_of_range.
		 */
		uint8_t r[14] = { 0x02, 0, 0, 0, 0, 7, 0, 0x02, 4, 0, 1, 0, 0x5e, 0 };
		memcpy(r + 1, &txn, 2);
		memcpy(r + 3, &msg, 2);
		br_to_cc(c, svc, cid, r, sizeof(r));
		return;
	}
	unsigned slot = v->next_txn % 64;
	uint16_t nt = v->next_txn++;
	if (v->next_txn == 0) v->next_txn = 1;
	v->pend[slot].txn = nt;
	v->pend[slot].cid = cid;
	v->pend[slot].orig = txn;
	v->pend[slot].used = 1;
	v->pend[slot].as_ind = 0;
	v->pend[slot].msg = msg;
	v->pend[slot].t = clock_gettime_nsec_np(CLOCK_MONOTONIC);
	if (svc == 0x03 && msg == 0x0003) {
		br_nas_poll_soon(c);    /* CommCenter registered for NAS indications: tell it where we are */
	}
	uint8_t buf[sizeof(struct sqhdr) + 0x4000];
	struct sqhdr h = { v->node, v->port };
	memcpy(buf, &h, sizeof(h));
	memcpy(buf + sizeof(h), sdu, sn);
	memcpy(buf + sizeof(h) + 1, &nt, 2);
	br_merge_ind_reg(v, msg, buf + sizeof(h), sn);
	if (write(v->fd, buf, sizeof(h) + sn) < 0) {
		say("bridge: write to service 0x%x: %s", svc, strerror(errno));
	}
	if (!br_reader_started) {
		br_reader_started = 1;
		pthread_create(&br_reader, NULL, br_reader_main, NULL);
	}
}

/* everything the modem says, back into CommCenter as QMUX */
static void *
br_reader_main(void *arg)
{
	(void)arg;
	for (;;) {
		fd_set fds;
		FD_ZERO(&fds);
		int maxfd = -1;
		pthread_mutex_lock(&br_lock);
		for (unsigned i = 0; i < MAX_SHIMS; i++) {
			for (unsigned k = 0; k < br_chans[i].nsvcs; k++) {
				int fd = br_chans[i].svcs[k].fd;
				if (fd >= 0) { FD_SET(fd, &fds); if (fd > maxfd) maxfd = fd; }
			}
		}
		pthread_mutex_unlock(&br_lock);
		struct timeval tv = { 0, 200000 };
		int ready = select(maxfd + 1, &fds, NULL, NULL, &tv);
		pthread_mutex_lock(&br_lock);
		/*
		 * a request the modem did not answer in 4 s gets QMI_ERR_INTERNAL from us: ATCS's own
		 * timeout (ATCS_TIMEOUT) makes ABM hard-reset the "baseband" and CommCenter drop the
		 * registration (seen with WDS SetEventReport)
		 */
		uint64_t now = clock_gettime_nsec_np(CLOCK_MONOTONIC);
		for (unsigned i = 0; i < MAX_SHIMS; i++) {
			struct br_chan *c = &br_chans[i];
			for (unsigned k = 0; k < c->nsvcs; k++) {
				struct br_svc *v = &c->svcs[k];
				for (unsigned m = 0; m < 64; m++) {
					if (!v->pend[m].used || now - v->pend[m].t < 4000000000ULL) continue;
					v->pend[m].used = 0;
					say("bridge: service 0x%x msg 0x%04x: no answer from the modem in 4 s, QMI_ERR_INTERNAL", v->svc, v->pend[m].msg);
					if (v->pend[m].as_ind) continue;
					uint8_t r[14] = { 0x02, 0, 0, 0, 0, 7, 0, 0x02, 4, 0, 1, 0, 0x03, 0 };
					memcpy(r + 1, &v->pend[m].orig, 2);
					memcpy(r + 3, &v->pend[m].msg, 2);
					br_to_cc(c, v->svc, v->pend[m].cid, r, sizeof(r));
				}
			}
		}
		if (ready <= 0) {
			pthread_mutex_unlock(&br_lock);
			continue;
		}
		for (unsigned i = 0; i < MAX_SHIMS; i++) {
			struct br_chan *c = &br_chans[i];
			for (unsigned k = 0; k < c->nsvcs; k++) {
				struct br_svc *v = &c->svcs[k];
				if (v->fd < 0 || !FD_ISSET(v->fd, &fds)) continue;
				uint8_t buf[sizeof(struct sqhdr) + 0x4000];
				ssize_t r = read(v->fd, buf, sizeof(buf));
				if (r < (ssize_t)sizeof(struct sqhdr) + 7) continue;
				uint8_t *sdu = buf + sizeof(struct sqhdr);
				size_t sn = (size_t)r - sizeof(struct sqhdr);
				uint16_t txn;
				memcpy(&txn, sdu + 1, 2);
				if (sdu[0] == 0x04) {           /* indication: every client of the service */
					for (unsigned m = 0; m < v->ncids; m++) br_to_cc(c, v->svc, v->cids[m], sdu, sn);
					continue;
				}
				unsigned slot = txn % 64;
				if (v->pend[slot].used && v->pend[slot].txn == txn && v->pend[slot].as_ind) {
					/*
					 * our own query (br_nas_poll): its answer goes to every NAS client as the
					 * indication carrying the same TLVs, without the result TLV 0x02
					 */
					v->pend[slot].used = 0;
					say("bridge: NAS poll answer 0x%04x (%zu bytes) -> indication to %u clients", v->pend[slot].as_ind, sn, v->ncids);
					uint8_t ind[0x4000];
					size_t in = 7;
					ind[0] = 0x04;
					memset(ind + 1, 0, 2);
					memcpy(ind + 3, &v->pend[slot].as_ind, 2);
					for (size_t o = 7; o + 3 <= sn;) {
						uint16_t tl;
						memcpy(&tl, sdu + o + 1, 2);
						if (o + 3 + tl > sn) break;
						int keep = sdu[o] != 0x02;
						if (v->pend[slot].as_ind == 0x004e) keep = sdu[o] >= 0x10 && sdu[o] <= 0x1d;
						if (keep && in + 3 + tl < sizeof(ind)) {
							memcpy(ind + in, sdu + o, 3 + tl);
							in += 3 + tl;
						}
						o += 3 + tl;
					}
					uint16_t tlen = (uint16_t)(in - 7);
					memcpy(ind + 5, &tlen, 2);
					for (unsigned m = 0; m < v->ncids; m++) br_to_cc(c, v->svc, v->cids[m], ind, in);
					continue;
				}
				if (v->pend[slot].used && v->pend[slot].txn == txn) {
					v->pend[slot].used = 0;
					memcpy(sdu + 1, &v->pend[slot].orig, 2);
					br_to_cc(c, v->svc, v->pend[slot].cid, sdu, sn);
				} else {
					say("bridge: service 0x%x answer with unknown txn %u dropped", v->svc, txn);
				}
			}
		}
		pthread_mutex_unlock(&br_lock);
	}
	return NULL;
}

/* mav_router's PCI shim transmit: the QMUX frames CommCenter sends towards the baseband */
static int (*real_shim_tx)(void *, void *);
static int
my_shim_tx(void *shim, void *msg)
{
	const void *buf = NULL;
	size_t len = 0;
	dispatch_data_t map = dispatch_data_create_map((dispatch_data_t)msg, &buf, &len);
	if (verbose_bridge) hexsay("cc ->", buf, len);
	pthread_mutex_lock(&br_lock);
	struct br_chan *c = br_chan_of(shim);
	/* a mav_router message may carry several frames back to back */
	const uint8_t *p = buf;
	size_t off = 0;
	while (c && off + 3 <= len) {
		uint16_t l;
		memcpy(&l, p + off + 1, 2);
		size_t fl = (size_t)l + 1;
		if (p[off] != 0x01 || off + fl > len) { hexsay("bridge: bad frame", p + off, len - off); break; }
		br_from_cc(c, p + off, fl);
		off += fl;
	}
	pthread_mutex_unlock(&br_lock);
	if (map) dispatch_release(map);
	return 0;                       /* MAV_ROUTER_SUCCESS */
}

static int (*real_ctl_engage_blk)(void *, void *);
static void (*real_shim_error)(void *, int);
static void (*real_ctl_ctor)(void *, void *);
static void my_ctl_ctor(void *t, void *w) { mav_controller = t; say("mav_router controller %p", t); real_ctl_ctor(t, w); }
static int my_shim_engage(void *t, void *g) { say("pci_shim::engage(%p)", t); int r = real_shim_engage(t, g); say("  pci_shim::engage -> %d", r); return r; }
static int my_ctl_engage_blk(void *t, void *b) { say("controller::engage(%p, block)", t); int r = real_ctl_engage_blk(t, b); say("  controller::engage(block) -> %d", r); return r; }
static int my_ctl_engage(void *t) { say("controller::engage(%p)", t); int r = real_ctl_engage(t); say("  controller::engage -> %d", r); return r; }
static void my_shim_error(void *t, int e) { say("pci_shim::handleError(%p, %d)", t, e); real_shim_error(t, e); }

static void (*real_atcs_assert)(const char *, unsigned, const char *, const char *, const char *, unsigned long, unsigned long);
static void
my_atcs_assert(const char *a, unsigned line, const char *b, const char *c, const char *d, unsigned long e, unsigned long f)
{
	say("ATCS assertion: '%s' line %u '%s' '%s' '%s' %#lx %#lx", a ? a : "", line, b ? b : "", c ? c : "", d ? d : "", e, f);
	dump_stacks("ATCS assertion");
	real_atcs_assert(a, line, b, c, d, e, f);
}

/*
 * CommCenter (main binary) 0x1006ea6d0: "Baseband service provisioned state changing from %s to
 * %s" (byte +0x648). Anything but 2 (provisioned) reports registration as "not registered" or
 * "emergency only" ("as report emergency only is true"). Apple's baseband says provisioned through
 * its BSP service (0xe2), which this modem does not have: always 2.
 */
static void (*real_cc_regstat)(void *, int);
static void
my_cc_regstat(void *t, int st)
{
	uintptr_t main_slide = (uintptr_t)_dyld_get_image_header(0) - 0x100000000UL;
	void **gate = *(void ***)((char *)t + 0x5a8);
	uintptr_t fn = 0;
	if (gate != NULL) {
		fn = (*(uintptr_t **)gate)[0x80 / 8];
	}
	Dl_info di = { 0 };
	dladdr((void *)fn, &di);
	say("CommCenter provisioned state %d -> 2 (raw [+0x20] %d, shown [+0x648] %d); gate %p vfunc+0x80 %#lx = %s %s+%#lx (main %#lx)",
	    st, *(int *)((char *)t + 0x20), *(uint8_t *)((char *)t + 0x648), (void *)gate, fn,
	    di.dli_fname ? di.dli_fname : "?", di.dli_sname ? di.dli_sname : "?",
	    di.dli_saddr ? fn - (uintptr_t)di.dli_saddr : 0, fn - main_slide);
	real_cc_regstat(t, 2);
	/*
	 * "baseband activated" (byte +0x5f0) is Apple's activation too: set here. (Calling the
	 * operator name evaluation, vfunc +0x140, from this hook trapped CommCenter.)
	 */
	if (!*((uint8_t *)t + 0x5f0)) {
		*((uint8_t *)t + 0x5f0) = 1;
		say("CommCenter: baseband marked activated");
	}
}

/* every C++ exception CommCenter throws: its type and the throwing stack */
static void (*real_cxa_throw)(void *, void *, void (*)(void *));
static void
my_cxa_throw(void *obj, void *tinfo, void (*dest)(void *))
{
	const char *name = tinfo ? ((const char *const *)tinfo)[1] : "?";
	char line[600];
	int o = snprintf(line, sizeof(line), "C++ throw %s:", name);
	uint64_t *fp = __builtin_frame_address(0);
	for (int k = 0; k < 16 && fp && ((uintptr_t)fp & 7) == 0 && o < (int)sizeof(line) - 20; k++) {
		uint64_t ret = fp[1];
		if (ret == 0) break;
		o += snprintf(line + o, sizeof(line) - (size_t)o, " %llx", ret - slide);
		if ((uint64_t *)fp[0] <= fp) break;
		fp = (uint64_t *)fp[0];
	}
	say("%s", line);
	real_cxa_throw(obj, tinfo, dest);
}

static void
bm_spies_install(void)
{
	real_cxa_throw = hook("__cxa_throw", (void *)my_cxa_throw);
	{
		uintptr_t main_slide = (uintptr_t)_dyld_get_image_header(0) - 0x100000000UL;
		real_cc_regstat = hook_at("CommCenter registration status", (uint32_t *)(0x1006ea6d0UL + main_slide), (void *)my_cc_regstat);
		/*
		 * The gate that turns "registered" into "emergency only" (SOS): a virtual returning
		 * byte +0x69 of its object, set once in the constructor to !capabilities[0x166] (a
		 * libCommCenterBase capability this device gets wrong). Returns 1: registration shown as is.
		 */
		static const uint32_t ret_true[] = { 0x52800020u /* mov w0, #1 */, 0xd65f03c0u /* ret */ };
		uintptr_t gate = 0x1005aa60cUL + main_slide;
		patch_code("CommCenter emergency-only gate", gate - slide, 0xd65f03c03941a400ULL, ret_true, 2);
		/*
		 * The operator name waits for "baseband activated" (byte +0x5f0 of the registration
		 * model, "#N Operator name change aborted: not activated"), which Apple's activation
		 * sets; there is none here. The cbz on it becomes a nop.
		 */
		static const uint32_t nop[] = { 0xd503201fu };
		patch_code("CommCenter operator name activation check", 0x1006e7468UL + main_slide - slide,
		    0x3959226834000228ULL, nop, 1);
	}
	real_atcs_assert = (void *)hook_cache("_ATCSException::triggerAssertion(..., mm)", 0x19ed00350UL, (void *)my_atcs_assert);
	real_ctl_ctor = hook_cache("mav_router::device::controller::controller", 0x1ea5ff8d4UL, (void *)my_ctl_ctor);
	real_ipc_change = hook_cache("RouterIPCController::changeState_sync", 0x1b7f9f770UL, (void *)my_ipc_change);
	real_ipc_ctor = hook_cache("RouterIPCController::RouterIPCController", 0x1b7f9eed8UL, (void *)my_ipc_ctor);
	real_ipc_activate = hook_cache("RouterIPCController::activate", 0x1b7f9f5e0UL, (void *)my_ipc_activate);
	real_shim_init = hook_cache("pci_shim::init", 0x1ea604318UL, (void *)my_shim_init);
	real_shim_engage = hook_cache("pci_shim::engage", 0x1ea60445cUL, (void *)my_shim_engage);
	real_ctl_engage_blk = hook_cache("controller::engage(block)", 0x1ea5ffbf0UL, (void *)my_ctl_engage_blk);
	real_ctl_engage = hook_cache("controller::engage", 0x1ea5ffffcUL, (void *)my_ctl_engage);
	real_shim_error = hook_cache("pci_shim::handleError", 0x1ea604c04UL, (void *)my_shim_error);
	shim_handle_read = (void (*)(void *, const void *, unsigned))(0x1ea5fe404UL + slide);
	real_shim_tx = hook_cache("pci_shim::handle_transmit", 0x1ea5fe5a4UL, (void *)my_shim_tx);
	real_tp_create = hook_cache("TelephonyBasebandPCITransportCreate", 0x1e9f6df80UL, (void *)my_tp_create);
	pthread_t wt;
	pthread_create(&wt, NULL, watchdog, NULL);
	real_abm_init = hook_cache("AppleBasebandManagerInitServer", 0x1b80500e4UL, (void *)my_abm_init);
	real_abm_start = hook_cache("AppleBasebandManagerStartServer", 0x1b80504c8UL, (void *)my_abm_start);
	real_srv_start = hook_cache("ABMServer::start", 0x1b7eef454UL, (void *)my_srv_start);
	real_bm_initialize = hook_cache("BootModule::initialize", 0x1b7f45480UL, (void *)my_bm_initialize);
	real_bm_start = hook_cache("BootModule::start", 0x1b7f47e04UL, (void *)my_bm_start);
	real_bm_finish = hook_cache("BootModule::finishInitialization_sync", 0x1b7f47f6cUL, (void *)my_bm_finish);
	real_bm_change = hook_cache("BootModule::changeState_sync", 0x1b7f40150UL, (void *)my_bm_change);
	real_bm_boot = hook_cache("BootModule::boot_sync", 0x1b7f4ba2cUL, (void *)my_bm_boot);
}

static void
fake_iokit_install(void)
{
	real_GetMatchingService = hook("IOServiceGetMatchingService", (void *)my_GetMatchingService);
	real_GetMatchingServices = hook("IOServiceGetMatchingServices", (void *)my_GetMatchingServices);
	real_AddMatchingNotification = hook("IOServiceAddMatchingNotification", (void *)my_AddMatchingNotification);
	real_IteratorNext = hook("IOIteratorNext", (void *)my_IteratorNext);
	real_ServiceOpen = hook("IOServiceOpen", (void *)my_ServiceOpen);
	real_ServiceClose = hook("IOServiceClose", (void *)my_ServiceClose);
	real_CallMethod = hook("IOConnectCallMethod", (void *)my_CallMethod);
	real_CallAsyncMethod = hook("IOConnectCallAsyncMethod", (void *)my_CallAsyncMethod);
	real_CallScalar = hook("IOConnectCallScalarMethod", (void *)my_CallScalar);
	real_CallStruct = hook("IOConnectCallStructMethod", (void *)my_CallStruct);
	real_CallAsyncScalar = hook("IOConnectCallAsyncScalarMethod", (void *)my_CallAsyncScalar);
	real_CallAsyncStruct = hook("IOConnectCallAsyncStructMethod", (void *)my_CallAsyncStruct);
	real_SearchCFProperty = hook("IORegistryEntrySearchCFProperty", (void *)my_SearchCFProperty);
	real_AddInterestNotification = hook("IOServiceAddInterestNotification", (void *)my_AddInterestNotification);
	say("fake IOKit: %s", real_CallMethod && real_GetMatchingService && real_IteratorNext ? "on" : "INCOMPLETE");
	bm_spies_install();
}

#define A64_MOV_W0_1    0x52800020u     /* mov w0, #1 */
#define A64_RET         0xd65f03c0u

/*
 * the mobile data call (qmitool autodata, setuid root: address, route, configd), started with
 * CommCenter; one instance (its own lock), so a CommCenter restart leaves the running one alone.
 * /var/tmp/autodata.off keeps it from starting.
 */
extern char **environ;
static void
start_autodata(void)
{
	static const char *tool = "/redwood/bin/qmitool";
	if (access(tool, X_OK) != 0 || access("/var/tmp/autodata.off", F_OK) == 0) {
		return;
	}
	posix_spawn_file_actions_t fa;
	posix_spawnattr_t at;
	posix_spawn_file_actions_init(&fa);
	posix_spawn_file_actions_addopen(&fa, 0, "/dev/null", O_RDONLY, 0);
	posix_spawn_file_actions_addopen(&fa, 1, "/var/tmp/autodata.log", O_WRONLY | O_CREAT | O_APPEND, 0644);
	posix_spawn_file_actions_adddup2(&fa, 1, 2);
	posix_spawnattr_init(&at);
	posix_spawnattr_setflags(&at, POSIX_SPAWN_SETSID);
	char *argv[] = { (char *)"qmitool", (char *)"autodata", NULL };
	pid_t pid = 0;
	int e = posix_spawn(&pid, tool, &fa, &at, argv, environ);
	say("autodata: %s (pid %d)", e == 0 ? "started" : strerror(e), (int)pid);
	posix_spawn_file_actions_destroy(&fa);
	posix_spawnattr_destroy(&at);
}

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
	/* installed as /usr/appleinternal/lib/liblinkguard.dylib, which other daemons weak-link too */
	if (strcmp(getprogname(), "CommCenter") != 0) {
		return;
	}
	start_autodata();
	say("ccbridge in %s, cache slide %#lx", getprogname(), slide);


	/*
	 * Step 2: capabilities::bb::abm::shouldBoot() -> false. AppleBasebandManager then takes its
	 * own "Faking bootup" path (no power-on, no sahara firmware download, no PCIe link): our modem
	 * is already running. It waits for its transport clients instead ("Wait for Ready on Fake Boot").
	 */
	static const uint32_t ret_false[] = { 0x52800000u /* mov w0, #0 */, A64_RET };
	patch_code("capabilities::bb::abm::shouldBoot", 0x1ae1df4a8UL, 0x910003fda9bf7bfdULL, ret_false, 2);

	fake_iokit_install();
}
