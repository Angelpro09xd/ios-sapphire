/*
 * libsapphire-fakebb.dylib: loaded into locationd and abm-helper (bbstart sets DYLD_INSERT_LIBRARIES, as ccstart
 * does for CommCenter). locationd's TelephonyBasebandController waits 15 s for Apple's AppleBaseband
 * driver (an IOService we do not have: the modem is a Qualcomm one behind QRTR, see ccbridge.c) and
 * then throws, out of a QMI response handler: locationd aborted every ~18 s, for ever, and ReportCrash
 * spent half a CPU writing the reports. This is ccbridge.c's fake IOKit, for AppleBaseband alone:
 * the service is found at once, opened, and its calls answered with zeros (logged to
 * /var/tmp/fakebb.log, the first 200 lines).
 */
#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#include <dispatch/dispatch.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <libkern/OSCacheControl.h>
#include <mach/mach.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/time.h>
#include <unistd.h>

static int log_fd = -1;
static unsigned log_lines;

static void
say(const char *fmt, ...)
{
	char buf[512];
	struct timeval tv;
	if (log_lines >= 200) {
		return;
	}
	log_lines++;
	gettimeofday(&tv, NULL);
	int n = snprintf(buf, sizeof(buf), "%ld.%03d [%d] ", (long)tv.tv_sec, (int)(tv.tv_usec / 1000), getpid());
	va_list ap;
	va_start(ap, fmt);
	n += vsnprintf(buf + n, sizeof(buf) - (size_t)n, fmt, ap);
	va_end(ap);
	if (n > (int)sizeof(buf) - 2) n = (int)sizeof(buf) - 2;
	buf[n++] = '\n';
	if (log_fd < 0) {
		log_fd = open("/var/tmp/fakebb.log", O_WRONLY | O_CREAT | O_APPEND, 0644);
	}
	if (log_fd >= 0) {
		write(log_fd, buf, (size_t)n);
	}
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
static const char *fake_classes[] = { "AppleBaseband", NULL };
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

__attribute__((constructor)) static void
fakebb_install(void)
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
	say("fake AppleBaseband: %s", real_CallMethod && real_GetMatchingService && real_IteratorNext ? "on" : "INCOMPLETE");
}
