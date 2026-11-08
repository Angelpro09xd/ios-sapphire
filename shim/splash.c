/*
 * The boot screen: the shim puts the picture on the panel and starts xnu straight away, with
 * sweet_splash=<the picture's physical address> in the boot-args (loader.c). xnu (0299) draws no log
 * and paints the boot screen itself while it boots: the picture, then a few lines typed out in the
 * console's white letters, until iOS draws the Apple logo. Volume up already held when the shim
 * runs restarts into recovery here; held later, xnu does it (sweet_keys.c), as Power + Volume up
 * does on a normal boot.
 *
 * Skipped with sweet_splash=0 in the payload's boot-args. The picture is splash.bin
 * (gen_splash.py), linked behind the stacks (shim.ld) so start.S's adr reach is unchanged.
 *
 * MMU off: every data access is Device memory, so the picture is read and the panel written in
 * aligned 32-bit words. The PMIC is only read, except for the one register a restart to recovery
 * writes (PM6125 PON_SOFT_RB_SPARE, as xnu's sweet_spmi_set_reboot_reason does).
 */
#include <stdint.h>
#include "console.h"
#include "font.h"
#include "splash.h"

#define BLACK		0xff000000U
#define GREY		0xff8a8a8aU
#define HEART		0xffff3b5cU

/* SPMI (PMIC arbiter v5), as bsd/dev/sweet_keys.c and sweet_spmi.c */
#define SPMI_CORE	0x01c40000UL
#define SPMI_CNFG	0x01c0a000UL
#define SPMI_OBS	0x03e00000UL
#define SPMI_CHNLS	0x01e00000UL
#define NO_APID		0xffffU
#define VOLUP_PPID	0x0c4		/* PM6125 (SID 0) GPIO5 at 0xc400: Volume up, active low */
#define VOLUP_RT	0xc410
#define PON_PPID	0x008
#define PON_SPARE	0x088f		/* PON_SOFT_RB_SPARE: bits 7:1 = mode, recovery = 1 */
#define IMEM_RESTART_REASON	0x0c12565cUL
#define RESTART_RECOVERY	0x77665502U
#define PSCI_SYSTEM_RESET	0x84000009UL

extern const uint32_t splash_bin[];	/* splash_data.S */

static inline uint32_t rd32(uintptr_t a) { return *(volatile uint32_t *)a; }
static inline void wr32(uintptr_t a, uint32_t v) { *(volatile uint32_t *)a = v; }

static uint64_t now(void)
{
	uint64_t v;
	__asm__ volatile("isb; mrs %0, cntpct_el0" : "=r"(v));
	return v;
}

static uint64_t ticks_per_ms(void)
{
	uint64_t f;
	__asm__ volatile("mrs %0, cntfrq_el0" : "=r"(f));
	return f / 1000;
}

/* ---- Volume up ---- */

static uint16_t volup_apid = NO_APID;
static int volup_held;		/* polls in a row that saw it pressed */
static uint64_t volup_next;

static uint16_t find_apid(unsigned ppid, unsigned count)
{
	uint16_t found = NO_APID;
	for (unsigned i = 0; i < count; i++) {
		uint32_t map = rd32(SPMI_CORE + 0x900 + 4 * i);
		if (!map || ((map >> 8) & 0xfff) != ppid)
			continue;
		if (found == NO_APID || (rd32(SPMI_CNFG + 0x700 + 4 * i) & 7) == 0)
			found = (uint16_t)i;
	}
	return found;
}

static unsigned spmi_count(void)
{
	uint32_t ver = rd32(SPMI_CORE), n = rd32(SPMI_CORE + 4) & 0x7ff;
	if (ver < 0x50000000U || ver >= 0x70000000U || n == 0 || n > 512)
		return 0;
	return n;
}

/* One byte through the observer channel (EXT_READL); -1 on error or timeout. */
static int spmi_read(uint16_t apid, unsigned addr)
{
	uintptr_t ch = SPMI_OBS + 0x80UL * apid;
	__asm__ volatile("dmb oshst" ::: "memory");
	wr32(ch, (1U << 27) | ((addr & 0xff) << 4));
	__asm__ volatile("dsb osh" ::: "memory");
	for (int i = 0; i < 100000; i++) {
		uint32_t st = rd32(ch + 0x08);
		if (st & 1)
			return (st & 0x0e) ? -1 : (int)(rd32(ch + 0x18) & 0xff);
	}
	return -1;
}

static void volup_init(void)
{
	unsigned n = spmi_count();
	if (!n)
		return;
	uint16_t apid = find_apid(VOLUP_PPID, n);
	if (apid == NO_APID)
		return;
	/* only if the firmware left GPIO5 enabled as an input (sweet_keys.c checks the same) */
	int mode = spmi_read(apid, 0xc440), en = spmi_read(apid, 0xc446), sub = spmi_read(apid, 0xc405);
	if (mode < 0 || en < 0 || sub < 0 || !(en & 0x80))
		return;
	unsigned dir = sub >= 0x10 && sub <= 0x13 ? (unsigned)mode & 3 : ((unsigned)mode >> 4) & 7;
	if (dir != 0 && dir != 2)
		return;
	volup_apid = apid;
}

/* Polled every 20 ms; 1 once Volume up has been down for three polls in a row. */
static int volup_poll(void)
{
	if (volup_apid == NO_APID)
		return 0;
	uint64_t t = now();
	if (t < volup_next)
		return volup_held >= 3;
	volup_next = t + 20 * ticks_per_ms();
	int v = spmi_read(volup_apid, VOLUP_RT);
	if (v >= 0 && !(v & 1))
		volup_held++;
	else if (v >= 0)
		volup_held = 0;
	return volup_held >= 3;
}

/* ---- drawing ---- */

/*
 * Clean and invalidate the whole frame in every cache, to the point of coherency: lines the UEFI
 * left there (it drew its screen through the cache) would otherwise be written back over the boot
 * screen once xnu turns the caches on and they are evicted -- the panel went black right when
 * xnu started. By VA (= PA with the MMU off), broadcast to every core.
 */
static void fb_cache_flush(void)
{
	uint64_t ctr;
	__asm__ volatile("mrs %0, ctr_el0" : "=r"(ctr));
	uintptr_t line = 4UL << ((ctr >> 16) & 0xf);
	for (uintptr_t a = FB_BASE; a < FB_BASE + (uintptr_t)FB_STRIDE * FB_H; a += line)
		__asm__ volatile("dc civac, %0" : : "r"(a) : "memory");
	__asm__ volatile("dsb sy" ::: "memory");
}

static void fill(uint32_t c)
{
	volatile uint32_t *fb = con_fb;
	for (int i = 0; i < FB_W * FB_H; i++)
		fb[i] = c;
}

static const unsigned char heart8x16[16] = {
	0x00, 0x00, 0x00, 0x00, 0x6c, 0xfe, 0xfe, 0xfe, 0xfe, 0x7c, 0x38, 0x10, 0x00, 0x00, 0x00, 0x00,
};

static void glyph(int x0, int y0, const unsigned char *g, int scale, uint32_t fg)
{
	for (int gy = 0; gy < 16; gy++)
		for (int sy = 0; sy < scale; sy++) {
			int y = y0 + gy * scale + sy;
			if (y < 0 || y >= FB_H)
				continue;
			volatile uint32_t *line = con_fb + y * (FB_STRIDE / 4);
			for (int gx = 0; gx < 8; gx++) {
				if (!(g[gy] & (0x80 >> gx)))
					continue;
				for (int sx = 0; sx < scale; sx++) {
					int x = x0 + gx * scale + sx;
					if (x >= 0 && x < FB_W)
						line[x] = fg;
				}
			}
		}
}

static int slen(const char *s)
{
	int n = 0;
	while (s[n])
		n++;
	return n;
}

/* abort_flag: set once Volume up is held, and the typing stops there */
static int want_recovery;

static void wait_ms(unsigned ms)
{
	uint64_t end = now() + ms * ticks_per_ms();
	while (now() < end && !want_recovery)
		if (volup_poll())
			want_recovery = 1;
}

/*
 * One line centred at y, typed out like the boot log (char_ms per character, 0: at once).
 * '\x01' is a red heart.
 */
static void line(int y, const char *s, int scale, uint32_t fg, unsigned char_ms)
{
	int w = slen(s) * 8 * scale, x = (FB_W - w) / 2;
	for (; *s && !(char_ms && want_recovery); s++, x += 8 * scale) {
		char ch = *s;
		if (ch == '\x01')
			glyph(x, y, heart8x16, scale, HEART);
		else if (ch > 0x20 && ch <= 0x7e)
			glyph(x, y, font8x16[ch - 0x20], scale, fg);
		if (char_ms)
			wait_ms(char_ms);
	}
}

static void picture(int y0)
{
	const uint32_t *b = splash_bin;
	if (b[0] != 0x484c5053U)	/* "SPLH" */
		return;
	uint32_t w = b[1], h = b[2];
	const volatile uint32_t *px = (const volatile uint32_t *)(b + 4);
	int x0 = (FB_W - (int)w) / 2;
	for (uint32_t y = 0; y < h && y0 + (int)y < FB_H; y++) {
		volatile uint32_t *dst = con_fb + (y0 + y) * (FB_STRIDE / 4) + x0;
		for (uint32_t x = 0; x < w; x++)
			dst[x] = px[y * w + x];
		if ((y & 63) == 0 && volup_poll())
			want_recovery = 1;
	}
}

/* ---- recovery ---- */

static int spmi_write_pon_spare(uint8_t val)
{
	unsigned n = spmi_count();
	uint16_t apid = n ? find_apid(PON_PPID, n) : NO_APID;
	if (apid == NO_APID || (rd32(SPMI_CNFG + 0x700 + 4 * apid) & 7) != 0)
		return -1;	/* not our channel */
	uintptr_t ch = SPMI_CHNLS + 0x10000UL * apid;
	wr32(ch + 0x10, val);				/* WDATA0 */
	__asm__ volatile("dmb oshst" ::: "memory");
	wr32(ch, (PON_SPARE & 0xff) << 4);		/* EXT_WRITEL, one byte */
	__asm__ volatile("dsb osh" ::: "memory");
	for (int i = 0; i < 100000; i++) {
		uint32_t st = rd32(ch + 0x08);
		if (st & 1)
			return (st & 0x0e) ? -1 : 0;
	}
	return -1;
}

static void __attribute__((noreturn)) reboot_recovery(void)
{
	fill(BLACK);
	line(FB_H / 2 - 64, "Rebooting to", 4, WHITE, 0);
	line(FB_H / 2 + 16, "Recovery...", 4, WHITE, 0);

	unsigned n = spmi_count();
	uint16_t pon = n ? find_apid(PON_PPID, n) : NO_APID;
	int old = pon != NO_APID ? spmi_read(pon, PON_SPARE) : -1;
	int ok = old >= 0 && spmi_write_pon_spare((uint8_t)((old & 1) | (1 << 1))) == 0;
	if (!ok)
		line(FB_H / 2 + 160, "(PMIC reason not set)", 2, GREY, 0);
	wr32(IMEM_RESTART_REASON, RESTART_RECOVERY);
	__asm__ volatile("dsb sy" ::: "memory");

	uint64_t end = now() + 800 * ticks_per_ms();
	while (now() < end)
		;
	register uint64_t x0 __asm__("x0") = PSCI_SYSTEM_RESET;
	__asm__ volatile("smc #0" : "+r"(x0) : : "x1", "x2", "x3", "memory");
	wr32(0x0440b000UL, 0);	/* PS_HOLD */
	for (;;)
		__asm__ volatile("wfe");
}

/* ---- the screen ---- */

uintptr_t splash_shown;

/* sweet_splash=0 in the payload's boot-args (cmdline at +0x48 of the SWEETXNU header behind _end) */
extern char _end[];

static int splash_disabled(void)
{
	const volatile char *p = (const volatile char *)(((uintptr_t)_end + 0xfff) & ~0xfffUL);
	static const char magic[] = "SWEETXNU", key[] = "sweet_splash=0";
	for (int i = 0; i < 8; i++)
		if (p[i] != magic[i])
			return 0;
	const volatile char *c = p + 0x48;
	for (int i = 0; i < 608 && c[i]; i++) {
		if (i && c[i - 1] != ' ')
			continue;
		int k = 0;
		while (key[k] && i + k < 608 && c[i + k] == key[k])
			k++;
		if (!key[k] && (c[i + k] == ' ' || c[i + k] == 0))
			return 1;
	}
	return 0;
}

void splash_run(void)
{
	if (splash_disabled())
		return;

	/* I-cache on while drawing (fine with the MMU off), restored after */
	uint64_t sctlr;
	__asm__ volatile("mrs %0, sctlr_el1" : "=r"(sctlr));
	__asm__ volatile("msr sctlr_el1, %0; isb" : : "r"(sctlr | (1UL << 12)));

	volup_init();
	fb_cache_flush();
	fill(BLACK);
	picture(380);	/* (xnu draws it at the same place, with the lines: splash in sweet_splash_thread) */
	line(FB_H - 150, "Hold VOL+ for Recovery", 2, GREY, 0);

	wait_ms(100);	/* Volume up already held: three polls */
	if (want_recovery)
		reboot_recovery();
	fb_cache_flush();
	splash_shown = (uintptr_t)splash_bin;
	__asm__ volatile("msr sctlr_el1, %0; isb" : : "r"(sctlr));
}
