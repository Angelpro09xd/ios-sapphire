/*
 * The Metal driver's first GPU work, at device creation: the same draw the kernel replayed
 * (turnip's clear, textured quad and blit; metal/capture/), recaptured at GPU address
 * 0x01000000 and run entirely from user space through the kernel interface: one buffer at
 * that fixed address for all of turnip's buffers, filled through its CPU mapping, one
 * submit, every pixel of both images checked by the CPU. It proves create, map, submit and
 * fence before anything real depends on them.
 */
#include "sgpu.h"

typedef struct __sFILE FILE;
extern FILE *__stderrp;
int fprintf(FILE *, const char *, ...) __attribute__((format(printf, 2, 3)));
int getpid(void);
void *memcpy(void *, const void *, sgpu_size_t);
void *memset(void *, int, sgpu_size_t);
unsigned int sleep(unsigned int);
typedef struct _opaque_pthread_t *pthread_t;
int pthread_create(pthread_t *, const void *, void *(*)(void *), void *);
int pthread_detach(pthread_t);
int IOObjectRetain(uint32_t);
int IOObjectRelease(uint32_t);

#include "SapphireReplayUser.h"

#define TLOG(fmt, ...) fprintf(__stderrp, "SAPPHIRE GPU[%d]: " fmt "\n", getpid(), ##__VA_ARGS__)
#define RT 0x01026000u
#define TX 0x01066000u
#define DS 0x010a6000u
#define W  256u

static uint32_t
check(const volatile uint32_t *img, uint32_t *bad_x, uint32_t *bad_y, uint32_t *bad_v)
{
	uint32_t good = 0;

	*bad_x = 0xFFFFFFFFu;
	for (uint32_t y = 0; y < W; y++) {
		for (uint32_t x = 0; x < W; x++) {
			uint32_t v = img[y * W + x], ok;
			if (x >= 64 && x < 192 && y >= 64 && y < 192) {
				uint32_t r = (v >> 16) & 0xff, g = (v >> 8) & 0xff, er = 2 * (x - 64), eg = 2 * (y - 64);
				ok = (v >> 24) == 0xff && (v & 0xff) == 0 && r - er <= 1 && g - eg <= 1;
			} else {
				ok = v == 0xff000080u;
			}
			if (ok) {
				good++;
			} else if (*bad_x == 0xFFFFFFFFu) {
				*bad_x = x;
				*bad_y = y;
				*bad_v = v;
			}
		}
	}
	return good;
}

static int
selftest_once(uint32_t service)
{
	sgpu_t g = { 0 };
	sgpu_bo_t bo = { 0 };
	uint32_t seq = 0, us = 0, bx, by, bv;
	const uint32_t span = urp_VA_END - urp_VA_START;

	int r = sgpu_open(&g, service);
	if (r != 0) {
		return r;
	}
	TLOG("selftest: Adreno %u (chip %08x), GPU addresses %llx-%llx, %u page tables free", g.gpu_id, g.chip_id,
	    g.va_base, g.va_end, g.tables_free);
	r = sgpu_buffer(&g, span, urp_VA_START, &bo);
	if (r != 0) {
		TLOG("selftest: no %u KB buffer at %x (0x%x)", span / 1024, urp_VA_START, r);
		sgpu_close(&g);
		return r;
	}
	TLOG("selftest: buffer %u, GPU %llx, CPU %p, %llu KB", bo.handle, bo.gpu, bo.cpu, bo.size / 1024);
	uint8_t *cpu = (uint8_t *)bo.cpu;
	const uint32_t *run = urp_page_runs;
	for (uint32_t p = 0; p < sizeof(urp_page_va) / sizeof(urp_page_va[0]); p++) {
		uint32_t *pg = (uint32_t *)(cpu + (urp_page_va[p] - urp_VA_START));
		while (*run != 0xFFFFFFFFu) {
			memcpy(pg + run[0], run + 2, run[1] * 4);
			run += 2 + run[1];
		}
		run++;
	}
	volatile uint32_t *tx = (volatile uint32_t *)(cpu + (TX - urp_VA_START));
	volatile uint32_t *rt = (volatile uint32_t *)(cpu + (RT - urp_VA_START));
	volatile uint32_t *ds = (volatile uint32_t *)(cpu + (DS - urp_VA_START));
	for (uint32_t i = 0; i < W * W; i++) {
		tx[i] = 0xff000000u | ((i / W) << 8) | (i % W);
		rt[i] = 0xDEADBEEFu;
		ds[i] = 0xDEADBEEFu;
	}
	__asm__ volatile("dsb sy" ::: "memory");        /* stores done before the submit (the kernel cleans the caches) */
	/* explicit syncs too, so this also runs on an 0148 kernel that does not flush around submits */
	r = sgpu_sync(&g, &bo);
	for (uint32_t i = 0; r == 0 && i < sizeof(urp_cmds) / sizeof(urp_cmds[0]); i++) {
		r = sgpu_submit(&g, urp_cmds[i].va, urp_cmds[i].dwords, 500, &seq, &us);
	}
	if (r == 0) {
		r = sgpu_sync(&g, &bo);
	}
	TLOG("selftest: submit 0x%x, fence %u, %u us; rt (0,0) %08x (128,128) %08x (255,255) %08x", r, seq, us, rt[0],
	    rt[128 * W + 128], rt[W * W - 1]);
	uint32_t good_rt = check(rt, &bx, &by, &bv);
	if (bx != 0xFFFFFFFFu) {
		TLOG("selftest: render target first wrong (%u,%u) = %08x", bx, by, bv);
	}
	uint32_t good_ds = check(ds, &bx, &by, &bv);
	if (bx != 0xFFFFFFFFu) {
		TLOG("selftest: blit copy first wrong (%u,%u) = %08x", bx, by, bv);
	}
	int ok = r == 0 && good_rt == W * W && good_ds == W * W;
	TLOG("selftest: render target %u/%u, blit %u/%u pixels right%s", good_rt, W * W, good_ds, W * W,
	    ok ? ": *** the Adreno drew for user space ***" : "");
	(void)sgpu_release(&g, &bo);
	sgpu_close(&g);
	return ok ? 0 : -1;
}

/*
 * The GPU service comes up when the kernel's tests finish (gpu_delay seconds into the boot),
 * often after backboardd made its Metal device: so the test waits on its own thread, trying
 * every two seconds for two minutes, and never holds up the caller.
 */
static void *
selftest_thread(void *arg)
{
	uint32_t service = (uint32_t)(sgpu_size_t)arg;
	int r = -1;

	for (int i = 0; i < 60; i++) {
		r = selftest_once(service);
		if (r != (int)0xe00002d8) {             /* kIOReturnNotReady: the service is not up yet */
			break;
		}
		sleep(2);
	}
	if (r == (int)0xe00002d8) {
		TLOG("selftest: the GPU service never came up (kernel without 0148, or gpu_service=1 missing)");
	} else if (r != 0 && r != -1) {
		TLOG("selftest: failed, 0x%x", r);
	}
	IOObjectRelease(service);
	return 0;
}

int
sgpu_selftest(uint32_t service)
{
	pthread_t t;
	IOObjectRetain(service);                    /* Metal releases its port after the device is made */
	int r = pthread_create(&t, 0, selftest_thread, (void *)(sgpu_size_t)service);
	if (r == 0) {
		pthread_detach(t);
	} else {
		IOObjectRelease(service);
	}
	return r;
}
