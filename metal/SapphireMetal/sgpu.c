/* See sgpu.h. */
#include "sgpu.h"

typedef int kern_return_t;
extern uint32_t mach_task_self_;
kern_return_t IOServiceOpen(uint32_t service, uint32_t task, uint32_t type, uint32_t *conn);
kern_return_t IOServiceClose(uint32_t conn);
kern_return_t IOConnectCallScalarMethod(uint32_t conn, uint32_t selector, const uint64_t *in, uint32_t in_cnt,
    uint64_t *out, uint32_t *out_cnt);
kern_return_t IOConnectMapMemory64(uint32_t conn, uint32_t type, uint32_t task, uint64_t *addr, uint64_t *size,
    uint32_t options);
kern_return_t IOConnectUnmapMemory64(uint32_t conn, uint32_t type, uint32_t task, uint64_t addr);

#define SGPU_TYPE               0x53475055u     /* 'SGPU' */
#define kIOMapAnywhere          0x00000001u

static int
call(sgpu_t *g, uint32_t sel, const uint64_t *in, uint32_t nin, uint64_t *out, uint32_t nout)
{
	uint32_t n = nout;
	return IOConnectCallScalarMethod(g->conn, sel, in, nin, out, &n);
}

int
sgpu_open(sgpu_t *g, uint32_t service)
{
	uint64_t out[5] = { 0 };
	int r = IOServiceOpen(service, mach_task_self_, SGPU_TYPE, &g->conn);
	if (r != 0) {
		g->conn = 0;
		return r;
	}
	r = call(g, 0, 0, 0, out, 5);
	if (r != 0) {
		sgpu_close(g);
		return r;
	}
	g->gpu_id = (uint32_t)out[0];
	g->chip_id = (uint32_t)out[1];
	g->va_base = out[2];
	g->va_end = out[3];
	g->tables_free = (uint32_t)out[4];
	return 0;
}

void
sgpu_close(sgpu_t *g)
{
	if (g->conn != 0) {
		IOServiceClose(g->conn);
		g->conn = 0;
	}
}

int
sgpu_buffer(sgpu_t *g, uint64_t size, uint64_t fixed_gpu, sgpu_bo_t *bo)
{
	uint64_t in[2] = { size, fixed_gpu }, out[2] = { 0 }, addr = 0, len = 0;
	int r = call(g, 1, in, 2, out, 2);
	if (r != 0) {
		return r;
	}
	bo->handle = (uint32_t)out[0];
	bo->gpu = out[1];
	bo->size = (size + 4095) & ~4095ull;
	/* cacheable, never write-combined (that panics the kernel); it flushes around submits */
	r = IOConnectMapMemory64(g->conn, bo->handle, mach_task_self_, &addr, &len, kIOMapAnywhere);
	if (r != 0) {
		uint64_t h = bo->handle;
		(void)call(g, 2, &h, 1, 0, 0);
		return r;
	}
	bo->cpu = (void *)(sgpu_size_t)addr;
	return 0;
}

int
sgpu_surface(sgpu_t *g, uint32_t surface_id, sgpu_bo_t *bo)
{
	uint64_t in = surface_id, out[3] = { 0 };
	int r = call(g, 3, &in, 1, out, 3);
	if (r == 0) {
		bo->handle = (uint32_t)out[0];
		bo->gpu = out[1];
		bo->size = out[2];
		bo->cpu = 0;
	}
	return r;
}

int
sgpu_release(sgpu_t *g, sgpu_bo_t *bo)
{
	uint64_t h = bo->handle;
	if (bo->cpu != 0) {
		(void)IOConnectUnmapMemory64(g->conn, bo->handle, mach_task_self_, (uint64_t)(sgpu_size_t)bo->cpu);
		bo->cpu = 0;
	}
	return call(g, 2, &h, 1, 0, 0);
}

int
sgpu_sync(sgpu_t *g, const sgpu_bo_t *bo)
{
	uint64_t h = bo->handle;
	return call(g, 4, &h, 1, 0, 0);
}

int
sgpu_submit(sgpu_t *g, uint64_t ib_gpu, uint32_t dwords, uint32_t timeout_ms, uint32_t *seqno, uint32_t *us)
{
	uint64_t in[3] = { ib_gpu, dwords, timeout_ms }, out[2] = { 0 };
	int r = call(g, 5, in, 3, out, 2);
	if (seqno != 0) {
		*seqno = (uint32_t)out[0];
	}
	if (us != 0) {
		*us = (uint32_t)out[1];
	}
	return r;
}
