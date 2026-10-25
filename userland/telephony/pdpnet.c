/*
 * pdpnet: the cellular data call (qmitool datacall, kernel 0232 pdp_ip0) as a network service for
 * configd, as CommCenter would publish it: State:/Network/Service/<id>/IPv4 (address, peer, router,
 * interface) and .../DNS (the call's servers). configd's IPMonitor then makes it the primary service
 * (default route, State:/Network/Global/IPv4) and hands the DNS servers to mDNSResponder, so every
 * app resolves names over the data call.
 *
 *   pdpnet up         publish (from /var/tmp/datacall.ip: "address gateway dns1 dns2")
 *   pdpnet down       remove
 *   pdpnet show       the keys as configd has them
 *   pdpnet resolve N  getaddrinfo(N), the way apps resolve
 */
#include <CoreFoundation/CoreFoundation.h>
/* the SDK marks SCDynamicStore macOS-only; iOS has it (configd, CommCenter use it) */
#undef API_UNAVAILABLE
#define API_UNAVAILABLE(...)
#include <SystemConfiguration/SystemConfiguration.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>

#define SERVICE "State:/Network/Service/sapphire-pdp_ip0"

static CFStringRef
cfs(const char *s)
{
	return CFStringCreateWithCString(NULL, s, kCFStringEncodingUTF8);
}

static CFArrayRef
cfa1(const char *s)
{
	CFStringRef v = cfs(s);
	CFArrayRef a = CFArrayCreate(NULL, (const void **)&v, 1, &kCFTypeArrayCallBacks);
	CFRelease(v);
	return a;
}

static void
show(SCDynamicStoreRef st, const char *key)
{
	CFStringRef k = cfs(key);
	CFPropertyListRef v = SCDynamicStoreCopyValue(st, k);
	printf("%s:\n", key);
	if (v != NULL) {
		CFStringRef d = CFCopyDescription(v);
		char buf[2048];
		if (CFStringGetCString(d, buf, sizeof(buf), kCFStringEncodingUTF8)) {
			printf("%s\n", buf);
		}
		CFRelease(d);
		CFRelease(v);
	} else {
		printf("  (none)\n");
	}
	CFRelease(k);
}

static int
up(SCDynamicStoreRef st)
{
	char ip[32], gw[32], d1[32], d2[32];
	FILE *f = fopen("/var/tmp/datacall.ip", "r");
	if (f == NULL) {
		printf("no data call (/var/tmp/datacall.ip)\n");
		return 1;
	}
	int n = fscanf(f, "%31s %31s %31s %31s", ip, gw, d1, d2);
	fclose(f);
	if (n < 2) {
		printf("datacall.ip: no address\n");
		return 1;
	}

	CFMutableDictionaryRef v4 = CFDictionaryCreateMutable(NULL, 0, &kCFTypeDictionaryKeyCallBacks,
	    &kCFTypeDictionaryValueCallBacks);
	CFArrayRef a;
	CFStringRef s;
	CFDictionarySetValue(v4, CFSTR("Addresses"), a = cfa1(ip));
	CFRelease(a);
	CFDictionarySetValue(v4, CFSTR("DestAddresses"), a = cfa1(gw));
	CFRelease(a);
	CFDictionarySetValue(v4, CFSTR("SubnetMasks"), a = cfa1("255.255.255.255"));
	CFRelease(a);
	CFDictionarySetValue(v4, CFSTR("Router"), s = cfs(gw));
	CFRelease(s);
	CFDictionarySetValue(v4, CFSTR("InterfaceName"), CFSTR("pdp_ip0"));
	CFDictionarySetValue(v4, CFSTR("ConfigMethod"), CFSTR("Manual"));

	CFMutableDictionaryRef dns = CFDictionaryCreateMutable(NULL, 0, &kCFTypeDictionaryKeyCallBacks,
	    &kCFTypeDictionaryValueCallBacks);
	CFMutableArrayRef srv = CFArrayCreateMutable(NULL, 0, &kCFTypeArrayCallBacks);
	for (int i = 0; i < 2 && i + 2 < n; i++) {
		const char *d = i == 0 ? d1 : d2;
		if (strcmp(d, "0.0.0.0") != 0) {
			CFArrayAppendValue(srv, s = cfs(d));
			CFRelease(s);
		}
	}
	if (CFArrayGetCount(srv) == 0) {
		CFArrayAppendValue(srv, CFSTR("8.8.8.8"));
	}
	CFDictionarySetValue(dns, CFSTR("ServerAddresses"), srv);
	CFDictionarySetValue(dns, CFSTR("InterfaceName"), CFSTR("pdp_ip0"));

	CFMutableDictionaryRef all = CFDictionaryCreateMutable(NULL, 0, &kCFTypeDictionaryKeyCallBacks,
	    &kCFTypeDictionaryValueCallBacks);
	CFDictionarySetValue(all, CFSTR(SERVICE "/IPv4"), v4);
	CFDictionarySetValue(all, CFSTR(SERVICE "/DNS"), dns);
	Boolean ok = SCDynamicStoreSetMultiple(st, all, NULL, NULL);
	printf("configd        %s (address %s, router %s, %ld DNS server%s)\n", ok ? "service published" : "refused", ip,
	    gw, (long)CFArrayGetCount(srv), CFArrayGetCount(srv) == 1 ? "" : "s");
	if (!ok) {
		printf("  %s\n", SCErrorString(SCError()));
	}
	CFRelease(all);
	CFRelease(srv);
	CFRelease(dns);
	CFRelease(v4);
	return ok ? 0 : 1;
}

static int
resolve(const char *name)
{
	struct addrinfo hints, *res = NULL;
	struct timeval t0, t1;
	memset(&hints, 0, sizeof(hints));
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;
	gettimeofday(&t0, NULL);
	int e = getaddrinfo(name, "80", &hints, &res);
	gettimeofday(&t1, NULL);
	long ms = (long)((t1.tv_sec - t0.tv_sec) * 1000 + (t1.tv_usec - t0.tv_usec) / 1000);
	if (e != 0) {
		printf("%s: %s (%ld ms)\n", name, gai_strerror(e), ms);
		return 1;
	}
	for (struct addrinfo *r = res; r != NULL; r = r->ai_next) {
		char b[64];
		const void *p = r->ai_family == AF_INET ? (const void *)&((struct sockaddr_in *)r->ai_addr)->sin_addr :
		    (const void *)&((struct sockaddr_in6 *)r->ai_addr)->sin6_addr;
		printf("%s: %s\n", name, inet_ntop(r->ai_family, p, b, sizeof(b)));
	}
	printf("(%ld ms)\n", ms);
	freeaddrinfo(res);
	return 0;
}

int
main(int argc, char **argv)
{
	if (argc < 2) {
		fprintf(stderr, "usage: pdpnet up|down|show|resolve NAME\n");
		return 2;
	}
	if (strcmp(argv[1], "resolve") == 0 && argc > 2) {
		return resolve(argv[2]);
	}
	SCDynamicStoreRef st = SCDynamicStoreCreate(NULL, CFSTR("pdpnet"), NULL, NULL);
	if (st == NULL) {
		printf("SCDynamicStoreCreate: %s\n", SCErrorString(SCError()));
		return 1;
	}
	if (strcmp(argv[1], "up") == 0) {
		return up(st);
	}
	if (strcmp(argv[1], "down") == 0) {
		CFStringRef k[2] = { CFSTR(SERVICE "/IPv4"), CFSTR(SERVICE "/DNS") };
		CFArrayRef rm = CFArrayCreate(NULL, (const void **)k, 2, &kCFTypeArrayCallBacks);
		Boolean ok = SCDynamicStoreSetMultiple(st, NULL, rm, NULL);
		CFRelease(rm);
		printf("configd        %s\n", ok ? "service removed" : SCErrorString(SCError()));
		return ok ? 0 : 1;
	}
	if (strcmp(argv[1], "show") == 0) {
		show(st, SERVICE "/IPv4");
		show(st, SERVICE "/DNS");
		show(st, "State:/Network/Global/IPv4");
		show(st, "State:/Network/Global/DNS");
		return 0;
	}
	return 2;
}
