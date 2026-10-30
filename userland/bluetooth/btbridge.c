/*
 * btbridge.dylib: loaded into Apple's bluetoothd (DYLD_INSERT_LIBRARIES, set by "btshim bdstart", its launchd
 * program: launchd drops DYLD_* for system daemons, as for CommCenter/ccstart).
 *
 * Why: bluetoothd (20H392, main 0x1000268a4) parses BT_DEVICE_ADDRESS from the environment and then ZEROES it
 * (release build) and then skips the registry, so BT_DEVICE_ADDRESS must NOT be set for it; its local address
 * then comes from FindLocalMacAddress (0x100027920):
 * IOServiceGetMatchingService(IOServiceNameMatching("marconi-bt" | "bluetooth")) +
 * IORegistryEntrySearchCFProperty(.., "IODeviceTree", "local-mac-address", recursive), 6 bytes. Without it:
 * "Not a valid MAC address (all 00s)", "No bluetooth on this device.", exit.
 * Our kernel has no IOService named "bluetooth" (yet), so this library answers for one: the lookup of "bluetooth"
 * returns the registry root, and on that entry "local-mac-address" is the address in BTBRIDGE_ADDRESS
 * (aa:bb:cc:dd:ee:ff, display order), "transport-encoding" 1 (H4) and "transport-speed" 3000000. Every other
 * lookup goes to IOKit unchanged. When the kernel publishes a real "bluetooth" service, its answers win.
 * Log: /var/tmp/btbridge.log.
 *
 * Code patch (bluetoothd 20H392 only, the bytes are checked first): when a device with audio connects, bluetoothd
 * asks the device object whether it "supports Skywalk Path" (0x10022c488, virtual call) and then builds a
 * BTAudioSkywalk channel on an audio Skywalk nexus our kernel does not have; os_channel_get_fd(NULL) crashed it
 * (SIGSEGV at 0, queue com.apple.bluetooth.media, 0x1003effb4) on every connection of a paired phone. The
 * "cbz w0" after the call at 0x10022c48c becomes "b" to the non-Skywalk path (0x10022c620).
 */
#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <mach/mach.h>
#include <mach-o/dyld.h>
#include <libkern/OSCacheControl.h>

static io_registry_entry_t fake_bt;     /* the registry root, standing in for "bluetooth" */

static void
blog(const char *fmt, ...)
{
	char b[512];
	va_list ap;
	va_start(ap, fmt);
	int n = vsnprintf(b, sizeof(b) - 1, fmt, ap);
	va_end(ap);
	if (n < 0)
		return;
	if (n > (int)sizeof(b) - 2)
		n = (int)sizeof(b) - 2;
	b[n++] = '\n';
	int fd = open("/var/tmp/btbridge.log", O_WRONLY | O_CREAT | O_APPEND, 0666);
	if (fd >= 0) {
		(void)write(fd, b, (size_t)n);
		close(fd);
	}
}

static int
is_bluetooth_match(CFDictionaryRef m)
{
	if (!m || CFGetTypeID(m) != CFDictionaryGetTypeID())
		return 0;
	CFTypeRef v = CFDictionaryGetValue(m, CFSTR(kIONameMatchKey));
	if (!v)
		return 0;
	if (CFGetTypeID(v) == CFStringGetTypeID())
		return CFEqual(v, CFSTR("bluetooth"));
	if (CFGetTypeID(v) == CFArrayGetTypeID() && CFArrayGetCount(v) == 1)
		return CFEqual(CFArrayGetValueAtIndex(v, 0), CFSTR("bluetooth"));
	return 0;
}

static io_service_t
my_IOServiceGetMatchingService(mach_port_t port, CFDictionaryRef matching)
{
	int bt = is_bluetooth_match(matching);
	io_service_t s = IOServiceGetMatchingService(port, matching);  /* consumes matching */
	if (s || !bt)
		return s;
	if (!fake_bt)
		fake_bt = IORegistryEntryFromPath(port, "IOService:/");
	if (!fake_bt)
		return 0;
	IOObjectRetain(fake_bt);
	blog("IOServiceGetMatchingService(\"bluetooth\"): no such service, answering with the registry root");
	return fake_bt;
}

static CFTypeRef
fake_prop(io_registry_entry_t e, CFStringRef key)
{
	if (!fake_bt || e != fake_bt || !key)
		return NULL;
	if (CFEqual(key, CFSTR("local-mac-address"))) {
		const char *s = getenv("BTBRIDGE_ADDRESS");
		unsigned v[6];
		uint8_t a[6];
		if (!s || sscanf(s, "%x:%x:%x:%x:%x:%x", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5]) != 6)
			return NULL;
		for (int i = 0; i < 6; i++)
			a[i] = (uint8_t)v[i];
		blog("local-mac-address -> %s", s);
		return CFDataCreate(kCFAllocatorDefault, a, 6);
	}
	if (CFEqual(key, CFSTR("transport-encoding"))) {
		uint32_t t = 1;
		return CFDataCreate(kCFAllocatorDefault, (const uint8_t *)&t, 4);
	}
	if (CFEqual(key, CFSTR("transport-speed"))) {
		uint32_t t = 3000000;
		return CFDataCreate(kCFAllocatorDefault, (const uint8_t *)&t, 4);
	}
	return NULL;
}

static CFTypeRef
my_IORegistryEntrySearchCFProperty(io_registry_entry_t e, const io_name_t plane, CFStringRef key,
    CFAllocatorRef alloc, IOOptionBits options)
{
	CFTypeRef f = fake_prop(e, key);
	return f ? f : IORegistryEntrySearchCFProperty(e, plane, key, alloc, options);
}

static CFTypeRef
my_IORegistryEntryCreateCFProperty(io_registry_entry_t e, CFStringRef key, CFAllocatorRef alloc, IOOptionBits options)
{
	CFTypeRef f = fake_prop(e, key);
	return f ? f : IORegistryEntryCreateCFProperty(e, key, alloc, options);
}

__attribute__((used, section("__DATA,__interpose"))) static const struct {
	const void *replacement, *original;
} interposers[] = {
	{ (const void *)my_IOServiceGetMatchingService, (const void *)IOServiceGetMatchingService },
	{ (const void *)my_IORegistryEntrySearchCFProperty, (const void *)IORegistryEntrySearchCFProperty },
	{ (const void *)my_IORegistryEntryCreateCFProperty, (const void *)IORegistryEntryCreateCFProperty },
};

static intptr_t main_slide = -1;

static void
patch_insn(uint64_t unslid, uint32_t expect, uint32_t insn, const char *what)
{
	intptr_t slide = main_slide;
	uint32_t *p = (uint32_t *)(uintptr_t)(unslid + (uint64_t)slide);
	if (*p != expect) {
		blog("%s: not patched, found %#x at %#llx (expected %#x)", what, *p, unslid, expect);
		return;
	}
	vm_address_t page = (vm_address_t)p & ~(vm_address_t)(vm_page_size - 1);
	if (vm_protect(mach_task_self(), page, vm_page_size, FALSE, VM_PROT_READ | VM_PROT_WRITE | VM_PROT_COPY) != KERN_SUCCESS) {
		blog("%s: page not writable", what);
		return;
	}
	*p = insn;
	vm_protect(mach_task_self(), page, vm_page_size, FALSE, VM_PROT_READ | VM_PROT_EXECUTE);
	sys_icache_invalidate(p, 4);
	blog("%s: patched", what);
}

__attribute__((constructor)) static void
btbridge_init(void)
{
	blog("btbridge loaded into pid %d", getpid());
	/* image 0 is not the main executable while inserted libraries initialise: find bluetoothd by name */
	for (uint32_t i = 0; i < _dyld_image_count(); i++) {
		const char *n = _dyld_get_image_name(i);
		size_t l = n ? strlen(n) : 0;
		if (l >= 11 && !strcmp(n + l - 11, "/bluetoothd")) {
			main_slide = _dyld_get_image_vmaddr_slide(i);
			break;
		}
	}
	if (main_slide != -1 && access("/var/jb/var/sapphire-bt-skywalk", F_OK) != 0)
		patch_insn(0x10022c48cull, 0x34000ca0u, 0x14000065u, "no Skywalk audio path (cbz -> b)");
}
