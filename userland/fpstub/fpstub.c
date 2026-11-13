// fpstub: stands in for fairplayd.H2, which on this port exits (code 0) before it ever checks in its Mach services, so
// launchd holds com.apple.fairplayd.versioned with a full queue and every synchronous MIG caller waits forever --
// locationd among them (msgh_id 0x1513, no timeout), and with locationd stuck, Settings > Privacy and the app panes
// that show a Location row hang (tester bugs 2/8/27). This checks in the job's services and answers every request with
// a MIG error reply (MIG_SERVER_DIED), so callers get "not available" at once. FairPlay itself does nothing here either way.
#include <mach/mach.h>
#include <mach/mig_errors.h>
extern mach_port_t bootstrap_port;
kern_return_t bootstrap_check_in(mach_port_t, const char *, mach_port_t *);
#include <stdio.h>
#include <string.h>

static const char *kServices[] = {"com.apple.fairplayd", "com.apple.fairplayd.versioned", "com.apple.unfreed"};

int main(void) {
	mach_port_t set;
	if (mach_port_allocate(mach_task_self(), MACH_PORT_RIGHT_PORT_SET, &set) != KERN_SUCCESS) return 1;
	int got = 0;
	for (unsigned i = 0; i < sizeof(kServices) / sizeof(*kServices); i++) {
		mach_port_t p = MACH_PORT_NULL;
		kern_return_t kr = bootstrap_check_in(bootstrap_port, kServices[i], &p);
		fprintf(stderr, "fpstub: check in %s: %s\n", kServices[i], kr == KERN_SUCCESS ? "ok" : mach_error_string(kr));
		if (kr == KERN_SUCCESS && mach_port_move_member(mach_task_self(), p, set) == KERN_SUCCESS) got++;
	}
	if (got == 0) return 1;
	union {
		mach_msg_header_t h;
		uint8_t b[16384];
	} in;
	unsigned answered = 0;
	for (;;) {
		memset(&in.h, 0, sizeof(in.h));
		kern_return_t kr = mach_msg(&in.h, MACH_RCV_MSG | MACH_RCV_LARGE, 0, sizeof(in), set, MACH_MSG_TIMEOUT_NONE, MACH_PORT_NULL);
		if (kr == MACH_RCV_TOO_LARGE) {
			// (too big for us: received and dropped; its reply port gets a send-once notification when destroyed)
			mach_msg(&in.h, MACH_RCV_MSG | MACH_RCV_LARGE | MACH_RCV_TIMEOUT, 0, 0, set, 0, MACH_PORT_NULL);
			continue;
		}
		if (kr != KERN_SUCCESS) continue;
		if (in.h.msgh_remote_port != MACH_PORT_NULL && MACH_MSGH_BITS_REMOTE(in.h.msgh_bits) != 0) {
			mig_reply_error_t r;
			memset(&r, 0, sizeof(r));
			r.Head.msgh_bits = MACH_MSGH_BITS(MACH_MSGH_BITS_REMOTE(in.h.msgh_bits), 0);
			r.Head.msgh_remote_port = in.h.msgh_remote_port;
			r.Head.msgh_size = sizeof(r);
			r.Head.msgh_id = in.h.msgh_id + 100;
			r.NDR = NDR_record;
			r.RetCode = MIG_SERVER_DIED;
			kern_return_t sk = mach_msg(&r.Head, MACH_SEND_MSG | MACH_SEND_TIMEOUT, sizeof(r), 0, MACH_PORT_NULL, 100, MACH_PORT_NULL);
			if (sk == KERN_SUCCESS) {
				// the reply right went with the reply: not in the request any more (destroying the name again is a
				// fatal kGUARD_EXC_INVALID_NAME under launchd)
				in.h.msgh_remote_port = MACH_PORT_NULL;
				in.h.msgh_bits &= ~MACH_MSGH_BITS_REMOTE_MASK;
			}
			if (answered++ < 20) fprintf(stderr, "fpstub: request %d answered with MIG_SERVER_DIED\n", in.h.msgh_id);
		}
		mach_msg_destroy(&in.h);
	}
}
