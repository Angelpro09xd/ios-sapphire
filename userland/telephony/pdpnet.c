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
 *   pdpnet watch      follow the cellular data switch (Control Center / Settings), as CommCenter
 *                     would: off takes the address off pdp_ip0 and the service out of configd, on
 *                     puts both back (the data call itself stays up on the modem)
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
#include <sys/ioctl.h>
#include <sys/sockio.h>
#include <net/if.h>
#include <netinet/in.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <fcntl.h>
#include <sys/file.h>
#include <time.h>
#include <sys/time.h>

#define SERVICE "State:/Network/Service/sapphire-pdp_ip0"

/* CoreTelephony's private client (what Settings uses): CommCenter's own cellular data switch */
typedef struct { int domain; int error; } CTError;
extern const void *_CTServerConnectionCreate(CFAllocatorRef, void *, void *);
extern CTError _CTServerConnectionGetCellularDataIsEnabled(const void *, Boolean *);

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


static void
ct_cb(void)
{
}

/* "a.b.c.d" in network order */
static int
sin_of(struct sockaddr_in *a, const char *s)
{
	memset(a, 0, sizeof(*a));
	a->sin_len = sizeof(*a);
	a->sin_family = AF_INET;
	return inet_pton(AF_INET, s, &a->sin_addr) == 1 ? 0 : -1;
}

struct ifaliasreq4 {
	char ifra_name[IFNAMSIZ];
	struct sockaddr_in ifra_addr, ifra_broadaddr, ifra_mask;
};

/* the call's address on pdp_ip0 (on) or off it (off) */
static int
pdp_addr(int on)
{
	char ip[32], gw[32];
	FILE *f = fopen("/var/tmp/datacall.ip", "r");
	if (f == NULL) {
		return -1;
	}
	int n = fscanf(f, "%31s %31s", ip, gw);
	fclose(f);
	if (n < 2) {
		return -1;
	}
	int s = socket(AF_INET, SOCK_DGRAM, 0);
	if (s < 0) {
		return -1;
	}
	int e;
	if (on) {
		struct ifaliasreq4 ifra;
		memset(&ifra, 0, sizeof(ifra));
		strlcpy(ifra.ifra_name, "pdp_ip0", sizeof(ifra.ifra_name));
		sin_of(&ifra.ifra_addr, ip);
		sin_of(&ifra.ifra_broadaddr, gw);
		sin_of(&ifra.ifra_mask, "255.255.255.255");
		e = ioctl(s, SIOCAIFADDR, &ifra);
	} else {
		struct ifreq ifr;
		memset(&ifr, 0, sizeof(ifr));
		strlcpy(ifr.ifr_name, "pdp_ip0", sizeof(ifr.ifr_name));
		sin_of((struct sockaddr_in *)&ifr.ifr_addr, ip);
		e = ioctl(s, SIOCDIFADDR, &ifr);
		if (e < 0 && errno == EADDRNOTAVAIL) {
			e = 0;
		}
	}
	if (e < 0) {
		printf("pdp_ip0        %s: %s\n", on ? "SIOCAIFADDR" : "SIOCDIFADDR", strerror(errno));
	}
	close(s);
	return e;
}

static int
down(SCDynamicStoreRef st)
{
	CFStringRef k[2] = { CFSTR(SERVICE "/IPv4"), CFSTR(SERVICE "/DNS") };
	CFArrayRef rm = CFArrayCreate(NULL, (const void **)k, 2, &kCFTypeArrayCallBacks);
	Boolean ok = SCDynamicStoreSetMultiple(st, NULL, rm, NULL);
	CFRelease(rm);
	printf("configd        %s\n", ok ? "service removed" : SCErrorString(SCError()));
	return ok ? 0 : 1;
}

static int
watch(SCDynamicStoreRef st)
{
	const void *ct = _CTServerConnectionCreate(NULL, (void *)ct_cb, NULL);
	int state = -1;
	int lk = open("/var/tmp/pdpnet.lock", O_RDWR | O_CREAT, 0644);
	if (lk >= 0 && flock(lk, LOCK_EX | LOCK_NB) < 0) {
		/* one watcher already runs (an earlier call): bring this call's address in line, once */
		Boolean on = 0;
		CTError e = _CTServerConnectionGetCellularDataIsEnabled(ct, &on);
		if (e.error == 0 && on) {
			return pdp_addr(1) == 0 ? up(st) : 1;
		}
		return e.error == 0 ? (down(st), pdp_addr(0), 0) : 1;
	}
	signal(SIGPIPE, SIG_IGN);
	for (;;) {
		Boolean on = 0;
		CTError e = _CTServerConnectionGetCellularDataIsEnabled(ct, &on);
		if (e.error == 0 && (int)on != state) {
			time_t t = time(NULL);
			printf("%.8s cellular data %s\n", ctime(&t) + 11, on ? "on" : "off");
			if (on) {
				if (pdp_addr(1) == 0) {
					up(st);
				}
			} else {
				down(st);
				pdp_addr(0);
			}
			fflush(stdout);
			state = on;
		}
		usleep(500000);
	}
}

int
main(int argc, char **argv)
{
	if (argc < 2) {
		fprintf(stderr, "usage: pdpnet up|down|watch|show|keys REGEX|resolve NAME\n");
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
		return down(st);
	}
	if (strcmp(argv[1], "watch") == 0) {
		return watch(st);
	}
	if (strcmp(argv[1], "keys") == 0 && argc > 2) {        /* keys matching a regex, with their values */
		CFStringRef re = cfs(argv[2]);
		CFArrayRef ks = SCDynamicStoreCopyKeyList(st, re);
		for (CFIndex i = 0; ks != NULL && i < CFArrayGetCount(ks); i++) {
			char k[256];
			CFStringGetCString(CFArrayGetValueAtIndex(ks, i), k, sizeof(k), kCFStringEncodingUTF8);
			show(st, k);
		}
		return 0;
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
