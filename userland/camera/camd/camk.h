/* camd's view of the kernel camera driver (bsd/dev/sweet_cam.c): the ops, the frame reads and
 * the exposure, for both the 0275 interface (single frames, op 23) and 0276's (preview ring op 30,
 * still op 32, kern.sweet_cam_exp, kern.sweet_cam_latest, kern.sweet_cam_sel). */
#ifndef CAMD_CAMK_H
#define CAMD_CAMK_H
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

typedef struct {
	bool present; /* kern.sweet_cam */
	bool ring;    /* 0276: ops 30/31/32, kern.sweet_cam_latest, kern.sweet_cam_sel */
	bool kexp;    /* 0276: kern.sweet_cam_exp */
	bool i2c;     /* kern.sweet_cam_i2c (the exposure fallback) */
} CamCaps;
extern CamCaps camk;

void camk_detect(void);
/* sets kern.sweet_cam_arg (unless arg < 0), runs the op; 0 or an errno. A failure copies the
 * kernel's log of the op into camk_log. */
int camk_op(int op, int arg);
extern char camk_log[2048];

long camk_frame_len(void);
int camk_select(int sel); /* kern.sweet_cam_sel: -1 newest preview, 0..3 ring buffer, 100 still */
/* the frame (from offset 0) into dst / into a file, in 1 MB pieces; progress(done, total, ctx) may be NULL */
typedef void (*camk_progress)(size_t done, size_t total, void *ctx);
int camk_read(uint8_t *dst, size_t len);
int camk_read_to_fd(int fd, size_t len, camk_progress pr, void *ctx);

typedef struct {
	bool running;
	uint64_t seq, t_ns, ovf;
	int buf;
	uint32_t bytes, w, h, mode, fault;
} CamLatest;
int camk_latest(CamLatest *l);

typedef struct {
	unsigned mode, w, h, fll, line_ns, coarse, max_lines, gain_x1000;
	bool streaming;
} CamExp;
int camk_exp_read(CamExp *e);
int camk_exp_write(unsigned lines, unsigned gain_x1000);
/* 0275 fallback: the same registers through kern.sweet_cam_i2c (byte writes, group hold) */
int camk_i2c_exposure(unsigned lines, unsigned gain_x1000);

#endif
