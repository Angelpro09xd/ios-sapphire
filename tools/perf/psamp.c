/* psamp PID SECONDS THREADNAME OUT [PERIOD_US]: samples one thread of PID (pc + frame-pointer backtrace) every PERIOD_US
 * (default 500) for SECONDS, by wall clock. Writes text to OUT: "I base path" per image of the target (from dyld's
 * all_image_infos), then "S state pc lr ret..." per sample (state: R running/runnable, W waiting). Symbolicate offline. */
#include <mach/mach.h>
#include <mach/mach_time.h>
#include <mach-o/dyld_images.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#define DEPTH 40
#define M 0x0000000fffffffffull
static int rd(task_t t, uint64_t a, void *b, size_t n) { vm_size_t g = 0; return vm_read_overwrite(t, a, n, (vm_address_t)b, &g) == KERN_SUCCESS && g == n; }
int main(int argc, char **argv)
{
	if (argc < 5) { fprintf(stderr, "usage: psamp PID SECONDS THREADNAME OUT [PERIOD_US]\n"); return 2; }
	task_t task; int secs = atoi(argv[2]); unsigned period = argc > 5 ? atoi(argv[5]) : 500;
	if (task_for_pid(mach_task_self(), atoi(argv[1]), &task) != KERN_SUCCESS) { fprintf(stderr, "task_for_pid refused\n"); return 1; }
	FILE *o = fopen(argv[4], "w"); if (!o) return 1;
	struct task_dyld_info di; mach_msg_type_number_t c = TASK_DYLD_INFO_COUNT;
	if (task_info(task, TASK_DYLD_INFO, (task_info_t)&di, &c) == KERN_SUCCESS) {
		struct dyld_all_image_infos ai;
		if (rd(task, di.all_image_info_addr, &ai, sizeof ai)) {
			fprintf(o, "C %llx\n", (unsigned long long)ai.sharedCacheBaseAddress);
			for (unsigned i = 0; i < ai.infoArrayCount; i++) {
				struct dyld_image_info ii; char path[512] = "";
				if (!rd(task, (uint64_t)ai.infoArray + i * sizeof ii, &ii, sizeof ii)) break;
				for (int k = 0; k < 511; k++) { if (!rd(task, (uint64_t)ii.imageFilePath + k, path + k, 1) || !path[k]) break; }
				fprintf(o, "I %llx %s\n", (unsigned long long)ii.imageLoadAddress, path);
			}
		}
	}
	thread_act_array_t th; mach_msg_type_number_t n; thread_act_t t = MACH_PORT_NULL;
	task_threads(task, &th, &n);
	for (unsigned i = 0; i < n; i++) {
		thread_extended_info_data_t e; mach_msg_type_number_t ec = THREAD_EXTENDED_INFO_COUNT;
		if (thread_info(th[i], THREAD_EXTENDED_INFO, (thread_info_t)&e, &ec) == KERN_SUCCESS && strstr(e.pth_name, argv[3])) { t = th[i]; break; }
	}
	if (t == MACH_PORT_NULL) { fprintf(stderr, "no thread named %s\n", argv[3]); return 1; }
	mach_timebase_info_data_t tb; mach_timebase_info(&tb);
	uint64_t per = (uint64_t)period * 1000 * tb.denom / tb.numer, start = mach_absolute_time(), end = start + (uint64_t)secs * 1000000000ull * tb.denom / tb.numer;
	uint64_t next = start; unsigned long ns = 0;
	while (mach_absolute_time() < end) {
		next += per; uint64_t now = mach_absolute_time(); if (next < now) next = now;
		mach_wait_until(next);
		arm_thread_state64_t st; mach_msg_type_number_t sc = ARM_THREAD_STATE64_COUNT;
		thread_basic_info_data_t bi; mach_msg_type_number_t bc = THREAD_BASIC_INFO_COUNT;
		uint64_t pcs[DEPTH]; int d = 0; char state = '?';
		if (thread_info(t, THREAD_BASIC_INFO, (thread_info_t)&bi, &bc) == KERN_SUCCESS) state = bi.run_state == TH_STATE_RUNNING ? 'R' : bi.run_state == TH_STATE_WAITING ? 'W' : 'o';
		if (thread_suspend(t) != KERN_SUCCESS) break;
		if (thread_get_state(t, ARM_THREAD_STATE64, (thread_state_t)&st, &sc) == KERN_SUCCESS) {
			uint64_t fp = arm_thread_state64_get_fp(st) & M;
			pcs[d++] = arm_thread_state64_get_pc(st) & M; pcs[d++] = arm_thread_state64_get_lr(st) & M;
			while (d < DEPTH && fp) { uint64_t f[2]; if (!rd(task, fp, f, sizeof f) || !f[1]) break; pcs[d++] = f[1] & M; if ((f[0] & M) <= fp) break; fp = f[0] & M; }
		}
		thread_resume(t);
		fprintf(o, "S %c", state); for (int i = 0; i < d; i++) fprintf(o, " %llx", (unsigned long long)pcs[i]); fputc('\n', o); ns++;
	}
	fclose(o);
	printf("psamp: %lu samples in %d s\n", ns, secs);
	return 0;
}
