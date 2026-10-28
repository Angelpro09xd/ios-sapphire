/*
 * arplay: AudioReach packets to the audio DSP through /dev/redwood-gpr (kernel 0243).
 *
 *   arplay SCRIPT.arp SECTION    a section ("start", "stop") of a script made by arconv.py
 *   arplay setup                 the kernel's memory mapped to the DSP
 *   arplay playback 0|1          iOS's playback graph closed / open again
 *   arplay gains EAR AUX         the RX macro's digital gains, dB (signed; -84 is off)
 *   arplay volume LEVEL          the voice call's volume: VCPM_PARAM_ID_CAL_KEYS, level 0..10
 *   arplay codec 0|1             the codec's capture path (the handset microphones) off / on
 *   arplay watch VOICE [MIC [EAR AUX]]  VOICE's start / stop as calls begin and end, MIC's record
 *                                graph (arconv.py --record) for /dev/redwood-mic in between (see watch())
 *
 * Stops at the first packet the DSP refuses and says which.
 */
#include <dispatch/dispatch.h>
#include <errno.h>
#include <fcntl.h>
#include <notify.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/sysctl.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

struct sweet_gpr_cmd {
	uint32_t op, arg;
	uint32_t src_port, dst_port, opcode, token, rsp_opcode, wait_ms;
	uint32_t flags, oob_offset, oob_len, len;
	int32_t status;
	uint32_t got_opcode, got_n, got[8];
	uint8_t payload[512];
};
struct sweet_gpr_mem { uint32_t offset, len; uint64_t ptr; };

#define SWEET_GPR_CALL  _IOWR('g', 1, struct sweet_gpr_cmd)
#define SWEET_GPR_WRITE _IOW('g', 2, struct sweet_gpr_mem)

#define VSID 0x11C05000u                        /* VOICEMMODE1: SUB1's CS voice */

static int fd = -1;
static uint32_t token = 0x1000;

static int
call(struct sweet_gpr_cmd *c)
{
	if (ioctl(fd, SWEET_GPR_CALL, c) != 0) {
		fprintf(stderr, "arplay: ioctl: %s\n", strerror(errno));
		return -1;
	}
	return c->status;
}

static int
op(uint32_t o, uint32_t arg)
{
	struct sweet_gpr_cmd c = { .op = o, .arg = arg };
	int r = call(&c);
	if (o == 1) {
		printf("setup: %d (handle %x, %u bytes)\n", r, c.got[0], c.got[1]);
	}
	return r;
}

static int
packet(uint32_t src, uint32_t dst, uint32_t opcode, uint32_t rsp, uint32_t flags, uint32_t off, uint32_t olen,
    const void *inband, uint32_t len)
{
	struct sweet_gpr_cmd c = { .op = 2, .src_port = src, .dst_port = dst, .opcode = opcode, .token = token++,
		                   .rsp_opcode = rsp, .wait_ms = 3000, .flags = flags, .oob_offset = off, .oob_len = olen, .len = len };
	if (len > sizeof(c.payload)) {
		fprintf(stderr, "arplay: %u bytes in band\n", len);
		return -1;
	}
	memcpy(c.payload, inband, len);
	int r = call(&c);
	printf("  %08x -> port %x%s (%u bytes): %d", opcode, dst, flags ? " oob" : "", flags ? olen : len, r);
	if (c.got_n != 0) {
		printf("  [%08x:", c.got_opcode);
		for (uint32_t i = 0; i < c.got_n && i < 4; i++) {
			printf(" %08x", c.got[i]);
		}
		printf("]");
	}
	printf("\n");
	return r;
}

static int
script(const char *path, const char *section)
{
	FILE *f = fopen(path, "rb");
	if (f == NULL) {
		perror(path);
		return 1;
	}
	fseek(f, 0, SEEK_END);
	long size = ftell(f);
	fseek(f, 0, SEEK_SET);
	uint8_t *b = malloc((size_t)size);
	if (b == NULL || fread(b, 1, (size_t)size, f) != (size_t)size || size < 4 || memcmp(b, "ARP1", 4) != 0) {
		fprintf(stderr, "arplay: %s: not a script\n", path);
		return 1;
	}
	fclose(f);
	/* a stop goes on past a refusal: what was open is closed and iOS's playback comes back */
	int in = 0, failed = 0, n = 0, all = strcmp(section, "stop") == 0;
	for (long at = 4; at + 8 <= size && (!failed || all);) {
		uint32_t t, len;
		memcpy(&t, b + at, 4);
		memcpy(&len, b + at + 4, 4);
		const uint8_t *p = b + at + 8;
		at += 8 + len;
		if (at > size) {
			break;
		}
		if (t == 6) {
			in = strcmp((const char *)p, section) == 0;
			continue;
		}
		if (!in) {
			continue;
		}
		uint32_t w[7];
		switch (t) {
		case 1: {
			uint32_t off;
			memcpy(&off, p, 4);
			struct sweet_gpr_mem m = { off, len - 4, (uint64_t)(uintptr_t)(p + 4) };
			if (ioctl(fd, SWEET_GPR_WRITE, &m) != 0) {
				fprintf(stderr, "arplay: write %u bytes at %x: %s\n", len - 4, off, strerror(errno));
				failed |= 1;
			}
			break;
		}
		case 2:
			memcpy(w, p, sizeof(w));
			n++;
			failed |= packet(w[0], w[1], w[2], w[3], w[4], w[5], w[6], p + sizeof(w), len - (uint32_t)sizeof(w)) != 0;
			break;
		case 3:
			memcpy(w, p, 4);
			printf("  iOS's playback graph %s: %d\n", w[0] ? "open" : "closed", op(3, w[0]));
			break;
		case 7: {
			struct sweet_gpr_cmd c = { .op = 5 };
			memcpy(w, p, 12);
			c.src_port = w[0];
			c.dst_port = w[1];
			c.arg = w[2];
			c.len = len - 12;
			memcpy(c.payload, p + 12, c.len);
			int r = call(&c);
			printf("  /dev/redwood-mic: record graph (endpoint %x) given to the kernel: %d\n", w[1], r);
			failed |= r != 0;
			break;
		}
		}
	}
	printf("%s: %d packets%s\n", section, n, failed ? (all ? ", some refused" : ", stopped at a refusal") : "");
	free(b);
	return failed;
}

/* VCPM_PARAM_ID_CAL_KEYS (what PAL sends when Android sets the call's volume) */
static int
volume(uint32_t level)
{
	uint32_t p[] = { 0, 0, 0, 0x30, 4, 0x0800116B, 0x20, 0, VSID, 3, 0x08001166, level, 0x080011B6, 0, 0x08001167, 0 };
	return packet(0x2010, 4, 0x01001006, 0, 0, 0, 0, p, sizeof(p));
}

/*
 * arplay watch SCRIPT [EAR AUX]: the voice graphs opened while a call has sound and closed after,
 * following ccbridge's notification "com.sapphire.call.audio" (state 1: on). While they are open,
 * the RX macro's gains are EAR and AUX dB (default: the top speaker at -6 dB, the bottom one off:
 * the phone held to the ear); iOS's own volume comes back with its next sound.
 */
/* the record graph for /dev/redwood-mic: away during a call (the call's TX graph has the same microphones) */
static const char *mic_script;

static void
mic(int on)
{
	if (mic_script == NULL) {
		return;
	}
	if (on) {
		/* refused: still open from an arplay before this one; closed, then opened again */
		if (script(mic_script, "open") != 0) {
			(void)script(mic_script, "close");
			if (script(mic_script, "open") != 0) {
				(void)script(mic_script, "close");
			}
		}
	} else {
		struct sweet_gpr_cmd c = { .op = 5 };
		(void)call(&c);
		(void)script(mic_script, "close");
	}
}

/*
 * The call's speakers: with the receiver as iOS's route (RedwoodAudio.driver's notification
 * "com.sapphire.audio.receiver", state 1) the top speaker at EAR dB and the bottom one at AUX dB; with
 * the speaker button on, the bottom one at 0 dB and the top one off.
 */
static int receiver = 1, call_ear, call_aux;

static void
gains(void)
{
	int ear = receiver ? call_ear : -84, aux = receiver ? call_aux : 0;
	printf("arplay: %s: top %d dB, bottom %d dB: %d\n", receiver ? "receiver" : "speaker", ear, aux,
	    op(4, ((uint32_t)ear & 0xFF) | ((uint32_t)aux & 0xFF) << 8));
}

/*
 * Mute ("com.sapphire.call.mute", state 1, from the call screen's tweak userland/tweaks/callbuttons): the
 * WCD937x's ADC1 and ADC2 off (bit 7 of TX_CH1 0x300E / TX_CH2 0x300F), through kern.sweet_audio
 * (op 3 read / 2 write, target 1 the WCD937x).
 */
static uint64_t
wcd(uint32_t op_, uint32_t reg, uint32_t v)
{
	uint64_t q = (uint64_t)op_ << 56 | 1ull << 48 | (uint64_t)reg << 32 | v, a = 0;
	size_t n = sizeof(a);
	if (sysctlbyname("kern.sweet_audio", NULL, NULL, &q, sizeof(q)) != 0 ||
	    sysctlbyname("kern.sweet_audio", &a, &n, NULL, 0) != 0) {
		return ~0ull;
	}
	return a;
}

static void
mute_mics(int muted)
{
	for (uint32_t reg = 0x300E; reg <= 0x300F; reg++) {
		uint64_t v = wcd(3, reg, 0);
		if (v <= 0xFF) {
			(void)wcd(2, reg, (uint32_t)(muted ? v & ~0x80u : v | 0x80u));
		}
	}
	printf("arplay: microphones %s\n", muted ? "muted" : "on");
}

static void
watch(const char *path, int ear, int aux)
{
	static int open_now, muted;
	int token, rtoken, mtoken;
	if (notify_register_dispatch("com.sapphire.call.mute", &mtoken, dispatch_get_main_queue(), ^(int t) {
		uint64_t st = 0;
		notify_get_state(t, &st);
		if (open_now && (int)(st != 0) != muted) {
			muted = st != 0;
			mute_mics(muted);
		}
		fflush(stdout);
	}) != NOTIFY_STATUS_OK) {
		fprintf(stderr, "arplay: no notification for mute\n");
	}
	call_ear = ear;
	call_aux = aux;
	if (notify_register_dispatch("com.sapphire.audio.receiver", &rtoken, dispatch_get_main_queue(), ^(int t) {
		uint64_t st = 1;
		notify_get_state(t, &st);
		receiver = st != 0;
		if (open_now) {
			gains();
		}
		fflush(stdout);
	}) != NOTIFY_STATUS_OK) {
		fprintf(stderr, "arplay: no notification for the receiver\n");
	}
	if (notify_register_dispatch("com.sapphire.call.audio", &token, dispatch_get_main_queue(), ^(int t) {
		uint64_t st = 0;
		notify_get_state(t, &st);
		printf("arplay: call sound %s (graphs %s)\n", st ? "on" : "off", open_now ? "open" : "closed");
		fflush(stdout);
		if (st && !open_now) {
			open_now = 1;
			receiver = 1;           /* a call starts on the receiver, whatever an app chose before */
			mic(0);
			if (script(path, "start") != 0) {
				(void)script(path, "stop");
			}
			gains();
		} else if (!st && open_now) {
			open_now = 0;
			if (muted) {
				muted = 0;
				mute_mics(0);
			}
			(void)script(path, "stop");
			mic(1);
		}
		fflush(stdout);
	}) != NOTIFY_STATUS_OK) {
		fprintf(stderr, "arplay: no notification\n");
		exit(1);
	}
	if (op(1, 0) != 0) {
		fprintf(stderr, "arplay: the kernel's memory is not mapped to the DSP\n");
		exit(1);
	}
	printf("arplay: the codec's microphones on: %d\n", op(6, 1));
	mic(1);
	printf("arplay: watching for calls (%s)\n", path);
	fflush(stdout);
	dispatch_main();
}

int
main(int argc, char **argv)
{
	fd = open("/dev/redwood-gpr", O_RDWR);
	/* as a launchd job: the kernel makes the device once the DSP's playback is up, ~45 s into the boot */
	for (int i = 0; fd < 0 && argc >= 3 && strcmp(argv[1], "watch") == 0 && i < 300; i++) {
		sleep(2);
		fd = open("/dev/redwood-gpr", O_RDWR);
	}
	if (fd < 0) {
		perror("/dev/redwood-gpr");
		return 1;
	}
	if (argc >= 3 && argc != 5 && argc <= 6 && strcmp(argv[1], "watch") == 0) {
		/*
		 * Started by launchd at boot, its notifications never came (ccbridge's "call sound on" was
		 * posted, nothing arrived), while the same job restarted later got them: registered only
		 * once the boot is 2 minutes old.
		 */
		struct timeval boot;
		size_t bn = sizeof(boot);
		if (sysctlbyname("kern.boottime", &boot, &bn, NULL, 0) == 0) {
			long up = (long)time(NULL) - boot.tv_sec;
			if (up >= 0 && up < 120) {
				printf("arplay: %ld s since the boot: waiting until 120\n", up);
				fflush(stdout);
				sleep((unsigned)(120 - up));
			}
		}
		mic_script = argc >= 4 ? argv[3] : NULL;
		watch(argv[2], argc == 6 ? atoi(argv[4]) : -6, argc == 6 ? atoi(argv[5]) : -84);
	}
	if (argc == 3 && strcmp(argv[1], "codec") == 0) {
		return op(6, (uint32_t)atoi(argv[2])) != 0;
	}
	if (argc == 3 && strcmp(argv[1], "playback") == 0) {
		return op(3, (uint32_t)atoi(argv[2])) != 0;
	}
	if (argc == 4 && strcmp(argv[1], "gains") == 0) {
		uint32_t ear = (uint32_t)atoi(argv[2]) & 0xFF, aux = (uint32_t)atoi(argv[3]) & 0xFF;
		return op(4, ear | aux << 8) != 0;
	}
	if (argc == 3 && strcmp(argv[1], "volume") == 0) {
		return volume((uint32_t)atoi(argv[2])) != 0;
	}
	if (argc == 2 && strcmp(argv[1], "setup") == 0) {
		return op(1, 0) != 0;
	}
	if (argc == 3) {
		if (op(1, 0) != 0) {
			fprintf(stderr, "arplay: the kernel's memory is not mapped to the DSP\n");
			return 1;
		}
		return script(argv[1], argv[2]);
	}
	fprintf(stderr, "usage: arplay SCRIPT SECTION | setup | playback 0|1 | gains EAR AUX | volume LEVEL\n");
	return 1;
}
