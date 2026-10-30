/*
 * SapphireBTCapability: MobileGestalt says "bluetooth" = false on sapphire (it is computed from the device tree,
 * which has no bluetooth node yet), so Settings shows no Bluetooth row and Control Center no toggle. This ElleKit
 * tweak hooks MGGetBoolAnswer / MGCopyAnswer and answers
 *   "bluetooth" (obfuscated XSLlJd/8sMyXO0qtvvUTBQ)        -> true
 *   "BluetoothAddress" (k5lVWbXuiZHLA17KGiVUAA)            -> the address bluetoothd uses (/redwood/share/btshim/bdaddr, readable by mobile:
 *                                                             written by install.sh; else nothing)
 * and, for BluetoothManager ("Could not get bluetooth address": it reads local-mac-address from the IOService
 * named "bluetooth", which the kernel does not publish yet), the same IOKit answers as btbridge.dylib gives
 * bluetoothd: IOServiceGetMatchingService("bluetooth") -> the registry root, whose "local-mac-address" is the
 * address above. Everything else goes to MobileGestalt / IOKit unchanged. Filter: SpringBoard, Preferences and anything that loads
 * BluetoothManager.framework. /var/jb/var/sapphire-btcap-off disables it. Remove the dylib to undo.
 * The proper fix is a bluetooth node in the device tree (mkadt) and a fresh MobileGestalt cache.
 */
#import <Foundation/Foundation.h>
#include <IOKit/IOKitLib.h>
#include <dlfcn.h>
#include <unistd.h>

#define TAG "SapphireBTCapability: "

typedef void (*MSHookFunction_t)(void *, void *, void **);
static Boolean (*o_getbool)(CFStringRef);
static CFTypeRef (*o_copy)(CFStringRef, CFDictionaryRef);

static int
is_key(CFStringRef k, CFStringRef plain, CFStringRef obf)
{
	return k && CFGetTypeID(k) == CFStringGetTypeID() && (CFEqual(k, plain) || CFEqual(k, obf));
}

static CFStringRef
bt_address(void)
{
	static CFStringRef a;
	if (!a) {
		NSString *s = [NSString stringWithContentsOfFile:@"/redwood/share/btshim/bdaddr" encoding:NSUTF8StringEncoding error:nil];
		s = [s stringByTrimmingCharactersInSet:[NSCharacterSet whitespaceAndNewlineCharacterSet]];
		if (s.length == 17)
			a = CFStringCreateCopy(NULL, (__bridge CFStringRef)s);
	}
	return a;
}

static Boolean
my_getbool(CFStringRef k)
{
	if (is_key(k, CFSTR("bluetooth"), CFSTR("XSLlJd/8sMyXO0qtvvUTBQ")))
		return 1;
	return o_getbool(k);
}

static CFTypeRef
my_copy(CFStringRef k, CFDictionaryRef opts)
{
	if (is_key(k, CFSTR("bluetooth"), CFSTR("XSLlJd/8sMyXO0qtvvUTBQ")))
		return kCFBooleanTrue;
	if (is_key(k, CFSTR("BluetoothAddress"), CFSTR("k5lVWbXuiZHLA17KGiVUAA"))) {
		CFStringRef a = bt_address();
		if (a)
			return CFRetain(a);
	}
	return o_copy(k, opts);
}

/* ---- IOKit: an IOService "bluetooth" with local-mac-address (as btbridge.c) */
static io_service_t (*o_gms)(mach_port_t, CFDictionaryRef);
static CFTypeRef (*o_search)(io_registry_entry_t, const io_name_t, CFStringRef, CFAllocatorRef, IOOptionBits);
static CFTypeRef (*o_create)(io_registry_entry_t, CFStringRef, CFAllocatorRef, IOOptionBits);
static io_registry_entry_t fake_bt;

static io_service_t
my_gms(mach_port_t port, CFDictionaryRef m)
{
	int bt = 0;
	if (m && CFGetTypeID(m) == CFDictionaryGetTypeID()) {
		CFTypeRef v = CFDictionaryGetValue(m, CFSTR(kIONameMatchKey));
		bt = v && CFGetTypeID(v) == CFStringGetTypeID() && CFEqual(v, CFSTR("bluetooth"));
	}
	io_service_t s = o_gms(port, m);
	if (s || !bt || !bt_address())
		return s;
	if (!fake_bt)
		fake_bt = IORegistryEntryFromPath(port, "IOService:/");
	if (fake_bt)
		IOObjectRetain(fake_bt);
	return fake_bt;
}

static CFTypeRef
fake_prop(io_registry_entry_t e, CFStringRef key)
{
	if (!fake_bt || e != fake_bt || !key || !CFEqual(key, CFSTR("local-mac-address")))
		return NULL;
	unsigned v[6];
	uint8_t a[6];
	char s[32];
	if (!CFStringGetCString(bt_address(), s, sizeof(s), kCFStringEncodingUTF8) ||
	    sscanf(s, "%x:%x:%x:%x:%x:%x", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5]) != 6)
		return NULL;
	for (int i = 0; i < 6; i++)
		a[i] = (uint8_t)v[i];
	return CFDataCreate(NULL, a, 6);
}

static CFTypeRef
my_search(io_registry_entry_t e, const io_name_t plane, CFStringRef key, CFAllocatorRef al, IOOptionBits o)
{
	CFTypeRef f = fake_prop(e, key);
	return f ? f : o_search(e, plane, key, al, o);
}

static CFTypeRef
my_create(io_registry_entry_t e, CFStringRef key, CFAllocatorRef al, IOOptionBits o)
{
	CFTypeRef f = fake_prop(e, key);
	return f ? f : o_create(e, key, al, o);
}

__attribute__((constructor)) static void
init(void)
{
	if (access("/var/jb/var/sapphire-btcap-off", F_OK) == 0)
		return;
	MSHookFunction_t mshook = (MSHookFunction_t)dlsym(RTLD_DEFAULT, "MSHookFunction");
	const char *libs[] = { "/var/jb/usr/lib/libellekit.dylib", "/var/jb/usr/lib/libsubstrate.dylib" };
	for (int i = 0; !mshook && i < 2; i++) {
		void *h = dlopen(libs[i], RTLD_LAZY);
		if (h)
			mshook = (MSHookFunction_t)dlsym(h, "MSHookFunction");
	}
	void *mg = dlopen("/usr/lib/libMobileGestalt.dylib", RTLD_LAZY);
	void *gb = mg ? dlsym(mg, "MGGetBoolAnswer") : NULL, *ca = mg ? dlsym(mg, "MGCopyAnswer") : NULL;
	if (!mshook || !gb || !ca) {
		NSLog(@TAG "not hooked (MSHookFunction %p, MGGetBoolAnswer %p, MGCopyAnswer %p)", mshook, gb, ca);
		return;
	}
	mshook(gb, (void *)my_getbool, (void **)&o_getbool);
	mshook(ca, (void *)my_copy, (void **)&o_copy);
	mshook((void *)IOServiceGetMatchingService, (void *)my_gms, (void **)&o_gms);
	mshook((void *)IORegistryEntrySearchCFProperty, (void *)my_search, (void **)&o_search);
	mshook((void *)IORegistryEntryCreateCFProperty, (void *)my_create, (void **)&o_create);
	NSLog(@TAG "MobileGestalt bluetooth -> true, IOService \"bluetooth\" answered");
}
