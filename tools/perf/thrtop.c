/* thrtop PID SECONDS: CPU time per thread of PID over SECONDS (index, name, user ms, system ms, %). */
#include <mach/mach.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#define MAXT 512
struct snap { uint64_t tid, user, sys; char name[64]; };
static unsigned take(task_t task, struct snap *s)
{
	thread_act_array_t th; mach_msg_type_number_t n;
	if (task_threads(task, &th, &n) != KERN_SUCCESS) return 0;
	if (n > MAXT) n = MAXT;
	for (unsigned i = 0; i < n; i++) {
		thread_extended_info_data_t e; mach_msg_type_number_t c = THREAD_EXTENDED_INFO_COUNT;
		thread_identifier_info_data_t id; mach_msg_type_number_t ic = THREAD_IDENTIFIER_INFO_COUNT;
		memset(&s[i], 0, sizeof s[i]);
		if (thread_info(th[i], THREAD_EXTENDED_INFO, (thread_info_t)&e, &c) == KERN_SUCCESS) {
			s[i].user = e.pth_user_time; s[i].sys = e.pth_system_time; strlcpy(s[i].name, e.pth_name, 64);
		}
		if (thread_info(th[i], THREAD_IDENTIFIER_INFO, (thread_info_t)&id, &ic) == KERN_SUCCESS) s[i].tid = id.thread_id;
		mach_port_deallocate(mach_task_self(), th[i]);
	}
	vm_deallocate(mach_task_self(), (vm_address_t)th, n * sizeof(*th));
	return n;
}
int main(int argc, char **argv)
{
	static struct snap a[MAXT], b[MAXT];
	task_t task; if (argc < 3) return 2;
	int secs = atoi(argv[2]);
	if (task_for_pid(mach_task_self(), atoi(argv[1]), &task) != KERN_SUCCESS) { fprintf(stderr, "task_for_pid refused\n"); return 1; }
	unsigned na = take(task, a); sleep(secs); unsigned nb = take(task, b);
	double tot = 0;
	for (unsigned i = 0; i < nb; i++) {
		uint64_t u = b[i].user, s = b[i].sys;
		for (unsigned j = 0; j < na; j++) if (a[j].tid == b[i].tid) { u -= a[j].user; s -= a[j].sys; break; }
		double um = u / 1e6, sm = s / 1e6;
		tot += um + sm;
		if (um + sm >= 1.0) printf("%3u tid %-7llu user %8.1f ms sys %8.1f ms  %5.1f%%  %s\n", i, b[i].tid, um, sm, (um + sm) / (secs * 10.0), b[i].name);
	}
	printf("total %.1f ms over %d s (%.1f%% of one core), %u threads\n", tot, secs, tot / (secs * 10.0), nb);
	return 0;
}
