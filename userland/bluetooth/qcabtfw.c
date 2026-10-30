/*
 * qcabtfw: bring the sapphire's WCN3988 Bluetooth up by hand over /dev/qcabt (kernel 0280):
 * kern.sweet_bt=1 and =2 (rails, UART, power pulse, HCI_Reset), EDL version, firmware download
 * (rampatch TLV + NVM, as Linux btqca.c), HCI_Reset, BD address, Read Local Version/BD_ADDR.
 *
 *   qcabtfw [-d dir] [-r rampatch.tlv] [-n nvm.bin] [-s speed] [-a aa:bb:cc:dd:ee:ff] [-N] [-I] [-2] [-v] [-V]
 *     -d  firmware directory (default /var/root/btfw); files picked from the version reply
 *     -s  operating speed, e.g. 3000000 (needs QCABT_SETBAUD in the kernel; else 115200)
 *     -a  BD address; default: a stable static-random address hashed from IOPlatformUUID
 *     -N  do not run kern.sweet_bt=1/2 (the SoC is already powered and pulsed)
 *     -I  keep in-band sleep / deep sleep enabled in the NVM
 *     -2  send only the type-2 NVM entry of the type-4 container (if the SoC refuses the whole file)
 *     -v  hex dump of every packet      -V  only the version query (no download)
 */
#include <CoreFoundation/CoreFoundation.h>
#include <CommonCrypto/CommonDigest.h>
#include <IOKit/IOKitLib.h>
#include <stdarg.h>
#include <sys/time.h>
#include "qca.c"

void
say(const char *fmt, ...)
{
	char buf[1024];
	struct timeval tv;
	gettimeofday(&tv, NULL);
	int n = snprintf(buf, sizeof(buf), "%ld.%03d ", (long)tv.tv_sec, (int)(tv.tv_usec / 1000));
	va_list ap;
	va_start(ap, fmt);
	n += vsnprintf(buf + n, sizeof(buf) - (size_t)n - 2, fmt, ap);
	va_end(ap);
	if (n > (int)sizeof(buf) - 2)
		n = (int)sizeof(buf) - 2;
	buf[n++] = '\n';
	(void)write(1, buf, (size_t)n);
}

/* a static random address (two top bits 11) from SHA-256 of a device-unique string; the input is
 * never printed */
static int
derived_addr(uint8_t a[6])
{
	char id[256] = { 0 };
	io_registry_entry_t root = IORegistryEntryFromPath(kIOMainPortDefault, "IOService:/");
	if (root) {
		CFTypeRef u = IORegistryEntryCreateCFProperty(root, CFSTR("IOPlatformUUID"), kCFAllocatorDefault, 0);
		if (u && CFGetTypeID(u) == CFStringGetTypeID())
			CFStringGetCString(u, id, sizeof(id), kCFStringEncodingUTF8);
		if (u)
			CFRelease(u);
		IOObjectRelease(root);
	}
	if (!id[0])
		return -1;
	uint8_t h[CC_SHA256_DIGEST_LENGTH];
	char in[300];
	int n = snprintf(in, sizeof(in), "sapphire-bt:%s", id);
	CC_SHA256(in, (CC_LONG)n, h);
	memcpy(a, h, 6);
	a[0] |= 0xc0;
	return 0;
}

static int
parse_addr(const char *s, uint8_t a[6])
{
	unsigned v[6];
	if (sscanf(s, "%x:%x:%x:%x:%x:%x", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5]) != 6)
		return -1;
	for (int i = 0; i < 6; i++)
		a[i] = (uint8_t)v[i];
	return 0;
}

int
main(int argc, char **argv)
{
	const char *dir = "/var/root/btfw", *ramp = NULL, *nvm = NULL;
	uint32_t speed = 0;
	uint8_t addr[6];
	int have_addr = 0, power = 1, only_version = 0, c;
	while ((c = getopt(argc, argv, "d:r:n:s:a:NI2vV")) != -1) {
		switch (c) {
		case 'd': dir = optarg; break;
		case 'r': ramp = optarg; break;
		case 'n': nvm = optarg; break;
		case 's': speed = (uint32_t)strtoul(optarg, NULL, 0); break;
		case 'a':
			if (parse_addr(optarg, addr) < 0) {
				fprintf(stderr, "bad address\n");
				return 2;
			}
			have_addr = 1;
			break;
		case 'N': power = 0; break;
		case 'I': qca_keep_ibs = 1; break;
		case '2': qca_nvm_inner = 1; break;
		case 'v': qca_verbose = 1; break;
		case 'V': only_version = 1; break;
		default:
			fprintf(stderr, "usage: qcabtfw [-d dir] [-r tlv] [-n nvm] [-s speed] [-a addr] [-N] [-I] [-v] [-V]\n");
			return 2;
		}
	}
	if (!have_addr && derived_addr(addr) < 0) {
		say("no IOPlatformUUID: pass -a");
		return 1;
	}
	say("BD address %02x:%02x:%02x:%02x:%02x:%02x", addr[0], addr[1], addr[2], addr[3], addr[4], addr[5]);
	if (qca_open(power) < 0)
		return 1;
	if (!power && qca_reset() != 0)
		say("HCI_Reset before download failed (continuing)");
	if (only_version)
		return qca_read_version() == 0 ? 0 : 1;
	if (qca_bringup(dir, ramp, nvm, speed, addr) < 0) {
		say("bring-up FAILED");
		return 1;
	}
	qca_print_identity();
	say("bring-up done at %u baud", qca_speed);
	return 0;
}
