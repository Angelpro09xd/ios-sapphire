/*
 * The Adreno from user space: a thin wrapper over the kernel's SapphireGpuClient
 * (xnu patch 0148, IOServiceOpen type 'SGPU' on IOAcceleratorES). Everything the Metal
 * driver does with the GPU goes through these calls. No libc headers (-nostdinc build).
 */
#ifndef SGPU_H
#define SGPU_H

typedef unsigned char uint8_t;
typedef unsigned int uint32_t;
typedef unsigned long long uint64_t;
typedef unsigned long sgpu_size_t;

typedef struct sgpu {
	uint32_t conn;                  /* io_connect_t */
	uint64_t va_base, va_end;       /* the GPU addresses the kernel hands out */
	uint32_t gpu_id, chip_id, tables_free;
} sgpu_t;

typedef struct sgpu_bo {
	uint32_t handle;
	uint64_t gpu;                   /* GPU address of byte 0 */
	uint64_t size;
	void    *cpu;                   /* write-combined mapping (buffers), NULL for surfaces */
} sgpu_bo_t;

/* All return 0 or the kern_return_t / IOReturn of the failing call. */
int sgpu_open(sgpu_t *g, uint32_t service);
void sgpu_close(sgpu_t *g);
int sgpu_buffer(sgpu_t *g, uint64_t size, uint64_t fixed_gpu, sgpu_bo_t *bo);
int sgpu_surface(sgpu_t *g, uint32_t surface_id, sgpu_bo_t *bo);
int sgpu_release(sgpu_t *g, sgpu_bo_t *bo);
int sgpu_sync(sgpu_t *g, const sgpu_bo_t *bo);
int sgpu_submit(sgpu_t *g, uint64_t ib_gpu, uint32_t dwords, uint32_t timeout_ms, uint32_t *seqno, uint32_t *us);

/* The startup self-test (sgpu_selftest.c): turnip's draw, replayed from user space. */
int sgpu_selftest(uint32_t service);

#endif
