/*
 * gxfreq: the real rate of the GPU's core clock (gpu_cc_gx_gfx3d_clk), measured by the GPU.
 *
 * Method (Linux's, drivers/gpu/drm/msm/msm_gpu.c retire_submit, v6.12 L657-668, and a6xx_gpu.c
 * L92-101, L208-217, L1010, L1019): CP performance counter 0 with the countable PERF_CP_ALWAYS_COUNT
 * counts one core clock cycle per edge; CP_ALWAYS_ON_COUNTER counts the 19.2 MHz crystal. Two samples of
 * both, taken by the CP itself with CP_REG_TO_MEM:
 *
 *     f_core = (cycles_B - cycles_A) * 19 200 000 / (aon_B - aon_A)
 *
 * As an independent check, RBBM counter 0 with PERF_RBBM_ALWAYS_COUNT is read as well.
 *
 * Talks to the kernel's 'SGPU' client (SapphireGpu.cpp, IOServiceOpen type 'SGPU' on IOAcceleratorES):
 * selector 1 makes a buffer, selector 5 runs an indirect buffer and waits.
 * Writes three GPU registers (RBBM_PERFCTR_CNTL = 1, CP_PERFCTR_CP_SEL_0 = 0, RBBM_PERFCTR_RBBM_SEL_0 = 0):
 * the values Linux and kgsl leave there in their hw_init. Does not touch any clock.
 *
 * Usage: gxfreq [ms between samples, default 200] [rounds, default 5]
 * Build: clang -target arm64-apple-ios16.0 -isysroot <iPhoneOS16.5.sdk> -O2 gxfreq.c -framework IOKit
 *        -framework CoreFoundation -o gxfreq && codesign -s - gxfreq
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <mach/mach.h>
#include <mach/mach_time.h>

typedef unsigned int io_object_t;
extern const mach_port_t kIOMasterPortDefault;
void *IOServiceMatching(const char *name);
io_object_t IOServiceGetMatchingService(mach_port_t master, void *matching);
kern_return_t IOServiceOpen(io_object_t service, task_port_t task, uint32_t type, io_object_t *conn);
kern_return_t IOServiceClose(io_object_t conn);
kern_return_t IOConnectCallScalarMethod(io_object_t conn, uint32_t selector, const uint64_t *in, uint32_t in_cnt,
    uint64_t *out, uint32_t *out_cnt);
kern_return_t IOConnectMapMemory64(io_object_t conn, uint32_t type, task_port_t task, mach_vm_address_t *addr,
    mach_vm_size_t *size, uint32_t options);

#define SGPU_TYPE                   0x53475055u     /* 'SGPU' */
#define kIOMapAnywhere              0x00000001u

/* registers/adreno/a6xx.xml (Linux v6.12): offsets in dwords */
#define REG_RBBM_PERFCTR_CP_0       0x0400          /* L2550 */
#define REG_RBBM_PERFCTR_RBBM_0     0x041c          /* L2551 */
#define REG_RBBM_PERFCTR_CNTL       0x0500          /* L2596 */
#define REG_RBBM_PERFCTR_RBBM_SEL_0 0x0507          /* L2603 */
#define REG_CP_PERFCTR_CP_SEL_0     0x08d0          /* L2367 */
#define REG_CP_ALWAYS_ON_COUNTER    0x0980          /* L2445 */
#define CP_REG_TO_MEM               0x3e            /* adreno_pm4.xml L291, fields L1215-1229 */
#define CP_NOP                      0x10

static uint32_t
par(uint32_t v)
{
	return (0x9669u >> (0xFu & (v ^ (v >> 4) ^ (v >> 8) ^ (v >> 12) ^ (v >> 16) ^ (v >> 20) ^ (v >> 24) ^ (v >> 28)))) & 1;
}
static uint32_t
pkt7(uint32_t op, uint32_t cnt)
{
	return 0x70000000u | cnt | (par(cnt) << 15) | ((op & 0x7F) << 16) | (par(op) << 23);
}
static uint32_t
pkt4(uint32_t reg, uint32_t cnt)
{
	return 0x40000000u | cnt | (par(cnt) << 7) | ((reg & 0x3FFFF) << 8) | (par(reg) << 27);
}

static io_object_t conn;

static int
call(uint32_t sel, const uint64_t *in, uint32_t nin, uint64_t *out, uint32_t nout)
{
	uint32_t n = nout;
	return IOConnectCallScalarMethod(conn, sel, in, nin, out, &n);
}

struct bo { uint64_t gpu; void *cpu; };

static int
bo_new(uint64_t size, struct bo *b)
{
	uint64_t in[2] = { size, 0 }, out[2] = { 0 };
	mach_vm_address_t addr = 0;
	mach_vm_size_t len = 0;
	int r = call(1, in, 2, out, 2);
	if (r != 0) {
		return r;
	}
	b->gpu = out[1];
	r = IOConnectMapMemory64(conn, (uint32_t)out[0], mach_task_self(), &addr, &len, kIOMapAnywhere);
	b->cpu = (void *)addr;
	return r;
}

static uint32_t *
reg_to_mem(uint32_t *p, uint32_t reg, uint64_t dst)
{
	*p++ = pkt7(CP_REG_TO_MEM, 3);
	*p++ = reg | (2u << 18) | (1u << 30);           /* REG, CNT = 2, 64B */
	*p++ = (uint32_t)dst;
	*p++ = (uint32_t)(dst >> 32);
	return p;
}

/* one sample: aon, cp cycles, rbbm cycles, aon again -> res[0..3] at GPU address dst */
static uint32_t
build_sample(uint32_t *ib, uint64_t dst, int setup)
{
	uint32_t *p = ib;
	if (setup) {
		*p++ = pkt4(REG_RBBM_PERFCTR_CNTL, 1);
		*p++ = 1;
		*p++ = pkt4(REG_CP_PERFCTR_CP_SEL_0, 1);
		*p++ = 0;                               /* PERF_CP_ALWAYS_COUNT */
		*p++ = pkt4(REG_RBBM_PERFCTR_RBBM_SEL_0, 1);
		*p++ = 0;                               /* PERF_RBBM_ALWAYS_COUNT */
	}
	p = reg_to_mem(p, REG_CP_ALWAYS_ON_COUNTER, dst + 0);
	p = reg_to_mem(p, REG_RBBM_PERFCTR_CP_0, dst + 8);
	p = reg_to_mem(p, REG_RBBM_PERFCTR_RBBM_0, dst + 16);
	p = reg_to_mem(p, REG_CP_ALWAYS_ON_COUNTER, dst + 24);
	return (uint32_t)(p - ib);
}

static int
submit(uint64_t gpu, uint32_t dwords, uint32_t *us)
{
	uint64_t in[3] = { gpu, dwords, 2000 }, out[2] = { 0 };
	int r = call(5, in, 3, out, 2);
	*us = (uint32_t)out[1];
	return r;
}

int
main(int argc, char **argv)
{
	uint32_t ms = argc > 1 ? (uint32_t)atoi(argv[1]) : 200, rounds = argc > 2 ? (uint32_t)atoi(argv[2]) : 5, us = 0;
	uint64_t info[5] = { 0 };
	struct bo ib, res;
	io_object_t svc = IOServiceGetMatchingService(kIOMasterPortDefault, IOServiceMatching("IOAcceleratorES"));
	int r;

	if (svc == 0) {
		printf("gxfreq: no IOAcceleratorES\n");
		return 1;
	}
	r = IOServiceOpen(svc, mach_task_self(), SGPU_TYPE, &conn);
	if (r != 0) {
		printf("gxfreq: IOServiceOpen 'SGPU': %#x (needs gpu=3 gpu_mmu=1 gpu_service=1)\n", r);
		return 1;
	}
	r = call(0, NULL, 0, info, 5);
	printf("gxfreq: gpu %llu chip %#llx, sel 0 -> %#x\n", info[0], info[1], r);
	if (r != 0 || bo_new(4096, &ib) != 0 || bo_new(4096, &res) != 0) {
		printf("gxfreq: no buffers\n");
		return 1;
	}
	uint32_t *cmds = (uint32_t *)ib.cpu;
	volatile uint64_t *v = (volatile uint64_t *)res.cpu;

	/* setup + a first sample that is thrown away (the counters start here if they were off) */
	uint32_t n = build_sample(cmds, res.gpu + 64, 1);
	r = submit(ib.gpu, n, &us);
	if (r != 0) {
		printf("gxfreq: setup submit: %#x\n", r);
		return 1;
	}
	usleep(20000);
	for (uint32_t i = 0; i < rounds; i++) {
		uint64_t a[4], b[4];
		n = build_sample(cmds, res.gpu + 0, 0);
		r = submit(ib.gpu, n, &us);
		if (r != 0) {
			printf("gxfreq: submit A: %#x\n", r);
			return 1;
		}
		memcpy(a, (void *)v, sizeof(a));
		usleep(ms * 1000);
		n = build_sample(cmds, res.gpu + 32, 0);
		r = submit(ib.gpu, n, &us);
		if (r != 0) {
			printf("gxfreq: submit B: %#x\n", r);
			return 1;
		}
		memcpy(b, (void *)(v + 4), sizeof(b));
		uint64_t daon = b[0] - a[0], dcp = b[1] - a[1], drb = b[2] - a[2];
		if (daon == 0) {
			printf("gxfreq: the always-on counter did not move (a %llx b %llx)\n", a[0], b[0]);
			return 1;
		}
		printf("gxfreq: %u: %.1f ms (aon %llu ticks; read skew %llu + %llu ticks): CP_ALWAYS_COUNT %llu cycles = %.2f MHz, "
		    "RBBM_ALWAYS_COUNT %llu cycles = %.2f MHz\n", i, (double)daon / 19200.0, daon, a[3] - a[0], b[3] - b[0], dcp,
		    (double)dcp * 19.2 / (double)daon, drb, (double)drb * 19.2 / (double)daon);
	}
	IOServiceClose(conn);
	return 0;
}
