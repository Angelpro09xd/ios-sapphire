/*
 * usamp PID SECONDS [THREAD]: a sampling profiler of another process's thread (THREAD: its index
 * in task_threads, 0 = the main thread, the default). Every millisecond the thread is suspended,
 * its pc and a frame-pointer backtrace taken, and it is resumed. At the end: the share of samples
 * by image (where the pc was: "self"), and the functions with the most samples, self and anywhere
 * in the backtrace ("total"). System frameworks are in the shared cache, at the same address in
 * every process, so dladdr() here names them; the frameworks SpringBoard uses are loaded first.
 */
#include <dlfcn.h>
#include <mach/mach.h>
#include <mach/thread_status.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define DEPTH   16
#define SLOTS   16384

struct slot { uint64_t key; unsigned self, total, seen; };

static struct slot fn[SLOTS], img[256], wt[1024];
static unsigned samples, idle;

static uint64_t
key_of(uint64_t pc, Dl_info *info, uint64_t *base)
{
	if (dladdr((const void *)pc, info) && info->dli_fbase != NULL) {
		*base = (uint64_t)info->dli_fbase;
		return info->dli_saddr != NULL ? (uint64_t)info->dli_saddr : pc & ~0xfffull;
	}
	*base = 0;
	return pc & ~0xfffull;
}

static struct slot *
find(struct slot *t, unsigned n, uint64_t key)
{
	unsigned h = (unsigned)((key >> 2) * 2654435761u) % n;
	while (t[h].key != 0 && t[h].key != key) {
		h = (h + 1) % n;
	}
	t[h].key = key;
	return &t[h];
}

static int
by_self(const void *a, const void *b)
{
	return (int)((const struct slot *)b)->self - (int)((const struct slot *)a)->self;
}

static int
by_total(const void *a, const void *b)
{
	return (int)((const struct slot *)b)->total - (int)((const struct slot *)a)->total;
}

static void
name(uint64_t key, char *out, size_t size)
{
	Dl_info info;
	if (dladdr((const void *)key, &info) && info.dli_fname != NULL) {
		const char *b = strrchr(info.dli_fname, '/');
		snprintf(out, size, "%s %s", b ? b + 1 : info.dli_fname, info.dli_sname ? info.dli_sname : "?");
	} else {
		snprintf(out, size, "%#llx", key);
	}
}

/* waiting in the kernel: a trap's pc (mach_msg, psynch, ...) */
static bool
waiting(uint64_t pc)
{
	Dl_info info;
	return dladdr((const void *)pc, &info) && info.dli_fname != NULL && strstr(info.dli_fname, "libsystem_kernel") != NULL;
}

int
main(int argc, char **argv)
{
	static const char *frameworks[] = {
		"/System/Library/PrivateFrameworks/UIKitCore.framework/UIKitCore",
		"/System/Library/Frameworks/QuartzCore.framework/QuartzCore",
		"/System/Library/Frameworks/CoreText.framework/CoreText",
		"/System/Library/Frameworks/CoreGraphics.framework/CoreGraphics",
		"/System/Library/PrivateFrameworks/FrontBoard.framework/FrontBoard",
		"/System/Library/PrivateFrameworks/FrontBoardServices.framework/FrontBoardServices",
		"/System/Library/PrivateFrameworks/BaseBoard.framework/BaseBoard",
		"/System/Library/Frameworks/Foundation.framework/Foundation",
		"/System/Library/PrivateFrameworks/CoreUI.framework/CoreUI",
		"/System/Library/Frameworks/ImageIO.framework/ImageIO",
		"/System/Library/PrivateFrameworks/BoardServices.framework/BoardServices",
	};
	task_t task;
	thread_act_array_t threads;
	mach_msg_type_number_t nthreads;
	int pid, seconds, which;

	if (argc < 3) {
		fprintf(stderr, "usage: usamp PID SECONDS [THREAD]\n");
		return 2;
	}
	pid = atoi(argv[1]);
	seconds = atoi(argv[2]);
	which = argc > 3 && argv[3][0] >= '0' && argv[3][0] <= '9' ? atoi(argv[3]) : 0;
	for (unsigned i = 0; i < sizeof(frameworks) / sizeof(frameworks[0]); i++) {
		(void)dlopen(frameworks[i], RTLD_LAZY);
	}
	if (task_for_pid(mach_task_self(), pid, &task) != KERN_SUCCESS) {
		fprintf(stderr, "usamp: task_for_pid(%d) refused\n", pid);
		return 1;
	}
	if (task_threads(task, &threads, &nthreads) != KERN_SUCCESS) {
		fprintf(stderr, "usamp: no threads\n");
		return 1;
	}
	/* THREAD may be a part of the thread's name instead ("display"): the first that has it */
	for (unsigned i = 0; argc > 3 && i < nthreads; i++) {
		thread_extended_info_data_t ext;
		mach_msg_type_number_t c = THREAD_EXTENDED_INFO_COUNT;
		if (thread_info(threads[i], THREAD_EXTENDED_INFO, (thread_info_t)&ext, &c) == KERN_SUCCESS &&
		    (argv[3][0] < '0' || argv[3][0] > '9') && strstr(ext.pth_name, argv[3]) != NULL) {
			which = (int)i;
			printf("usamp: thread %u \"%s\"\n", i, ext.pth_name);
			break;
		}
	}
	if ((unsigned)which >= nthreads) {
		fprintf(stderr, "usamp: no thread %d\n", which);
		return 1;
	}
	thread_act_t t = threads[which];
	for (int n = 0; n < seconds * 1000; n++) {
		arm_thread_state64_t st;
		mach_msg_type_number_t count = ARM_THREAD_STATE64_COUNT;
		uint64_t pcs[DEPTH] = { 0 };
		int depth = 0;

		usleep(1000);
		if (thread_suspend(t) != KERN_SUCCESS) {
			break;
		}
		if (thread_get_state(t, ARM_THREAD_STATE64, (thread_state_t)&st, &count) == KERN_SUCCESS) {
			uint64_t fp = arm_thread_state64_get_fp(st) & 0x0000000fffffffffull;
			pcs[depth++] = arm_thread_state64_get_pc(st) & 0x0000000fffffffffull;
			pcs[depth++] = arm_thread_state64_get_lr(st) & 0x0000000fffffffffull;
			while (depth < DEPTH && fp != 0) {
				uint64_t frame[2];
				vm_size_t got = 0;
				if (vm_read_overwrite(task, fp, sizeof(frame), (vm_address_t)frame, &got) != KERN_SUCCESS || got != sizeof(frame)) {
					break;
				}
				if (frame[1] == 0) {
					break;
				}
				pcs[depth++] = frame[1] & 0x0000000fffffffffull;
				fp = frame[0] & 0x0000000fffffffffull;
			}
		}
		thread_resume(t);
		if (depth == 0) {
			continue;
		}
		samples++;
		if (waiting(pcs[0])) {
			/* where it waits: the first two callers outside the kernel's traps */
			int i = 1;
			while (i < depth && waiting(pcs[i])) {
				i++;
			}
			if (i < depth) {
				Dl_info info;
				uint64_t base;
				find(wt, 1024, key_of(pcs[i] - 4, &info, &base))->self++;
				if (i + 1 < depth) {
					find(wt, 1024, key_of(pcs[i + 1] - 4, &info, &base))->total++;
				}
			}
			idle++;
			continue;
		}
		unsigned stamp = samples;
		for (int i = 0; i < depth; i++) {
			Dl_info info;
			uint64_t base;
			uint64_t key = key_of(pcs[i] - (i > 0 ? 4 : 0), &info, &base);
			struct slot *s = find(fn, SLOTS, key);
			if (i == 0) {
				s->self++;
				find(img, 256, base ? base : 1)->self++;
			}
			if (s->seen != stamp) {
				s->seen = stamp;
				s->total++;
			}
		}
	}
	unsigned busy = samples - idle;
	printf("usamp: pid %d thread %d: %u samples, busy %u (%.0f%%)\n", pid, which, samples, busy,
	    samples ? 100.0 * busy / samples : 0);
	qsort(wt, 1024, sizeof(wt[0]), by_self);
	for (int i = 0; i < 8 && wt[i].self; i++) {
		char n[300];
		name(wt[i].key, n, sizeof(n));
		printf("  wait  %5.1f%%  %s @%#llx\n", 100.0 * wt[i].self / samples, n, wt[i].key);
	}
	qsort(wt, 1024, sizeof(wt[0]), by_total);
	for (int i = 0; i < 8 && wt[i].total; i++) {
		char n[300];
		name(wt[i].key, n, sizeof(n));
		printf("  wait< %5.1f%%  %s @%#llx\n", 100.0 * wt[i].total / samples, n, wt[i].key);
	}
	if (busy == 0) {
		return 0;
	}
	qsort(img, 256, sizeof(img[0]), by_self);
	for (int i = 0; i < 12 && img[i].self; i++) {
		char n[200];
		Dl_info info;
		if (img[i].key > 1 && dladdr((const void *)img[i].key, &info) && info.dli_fname) {
			const char *b = strrchr(info.dli_fname, '/');
			snprintf(n, sizeof(n), "%s", b ? b + 1 : info.dli_fname);
		} else {
			snprintf(n, sizeof(n), "(unknown)");
		}
		printf("  image %5.1f%%  %s\n", 100.0 * img[i].self / busy, n);
	}
	qsort(fn, SLOTS, sizeof(fn[0]), by_self);
	for (int i = 0; i < 30 && fn[i].self; i++) {
		char n[300];
		name(fn[i].key, n, sizeof(n));
		printf("  self  %5.1f%%  %s @%#llx\n", 100.0 * fn[i].self / busy, n, fn[i].key);
	}
	qsort(fn, SLOTS, sizeof(fn[0]), by_total);
	for (int i = 0; i < 45 && fn[i].total; i++) {
		char n[300];
		name(fn[i].key, n, sizeof(n));
		printf("  total %5.1f%%  %s @%#llx\n", 100.0 * fn[i].total / busy, n, fn[i].key);
	}
	return 0;
}
