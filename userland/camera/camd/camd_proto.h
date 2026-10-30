/* camd <-> app: the preview's shared memory layout and the names (also copied into the
 * SapphireCamera app as SCCamdProtocol.h: keep the two identical).
 *
 * /var/tmp/camd-preview.bin (written by camd, mapped read-only by the app):
 *   header (CAMD_SHM_HEADER bytes), then CAMD_SHM_SLOTS slots of slot_size bytes each.
 *   A frame is written into the slot after `latest`, its slot record filled, then `latest` and
 *   `seq` are stored (release order) and com.sapphire.cam.frame is posted with the state = seq.
 *   Pixels: 32-bit BGRX (little-endian: B, G, R, 255), already rotated upright for portrait.
 *   A slot is rewritten CAMD_SHM_SLOTS - 1 frames later.
 */
#ifndef CAMD_PROTO_H
#define CAMD_PROTO_H
#include <stdint.h>

#define CAMD_SOCK_PATH   "/var/run/camd.sock"
#define CAMD_SHM_PATH    "/var/tmp/camd-preview.bin"
#define CAMD_NOTIFY_FRAME "com.sapphire.cam.frame"
#define CAMD_NOTIFY_PHOTO "com.sapphire.cam.photo"
#define CAMD_NOTIFY_STATE "com.sapphire.cam.state"

#define CAMD_SHM_MAGIC   0x444D4143u /* "CAMD" */
#define CAMD_SHM_VERSION 1
#define CAMD_SHM_HEADER  4096
#define CAMD_SHM_SLOTS   3
#define CAMD_SHM_MAXDIM  1280
#define CAMD_SHM_SLOT_SIZE (CAMD_SHM_MAXDIM * CAMD_SHM_MAXDIM * 4)

typedef struct {
	uint64_t seq;         /* frame number (0 = never written) */
	uint64_t ts_ns;       /* capture time (kernel or camd clock, ns) */
	uint32_t width, height, stride; /* pixels, pixels, bytes */
	uint32_t mode;        /* sensor mode the frame came from */
	float exposure_us, gain; /* sensor exposure of the frame */
	float wb[3];          /* white balance used */
	float luma;           /* metered mean green (linear 0..1) */
} CamdSlot;

typedef struct {
	uint32_t magic, version;
	uint32_t header_size, slot_size, nslots;
	uint32_t latest;      /* slot of the newest frame */
	uint64_t seq;         /* its frame number */
	uint32_t running;     /* 1 while the preview runs */
	uint32_t reserved[7];
	CamdSlot slot[CAMD_SHM_SLOTS];
} CamdShm;

/*
 * Control socket /var/run/camd.sock (stream, mode 0666, peers root or mobile), one command per
 * line, every reply one line:
 *   HELLO                     -> OK camd 1 kernel=ring|frames|none exp=sysctl|i2c|none
 *   STATUS                    -> STATUS key=value ... (state, mode, exposure, wb, busy, dir, ...)
 *   START [mode]              -> OK preview <w> <h> <shm path>   (mode 1, 3 or 4; default per kernel)
 *   STOP                      -> OK
 *   FRAME                     -> FRAME <shm path> <seq> <w> <h> <stride> <ts_ns>
 *   CAPTURE [108|12] [dng|nodng]
 *                             -> OK capture <mp>, then SHUTTER, PROGRESS <0-100> ..., DONE <jpeg path>
 *                                [DNG <path>] (or ERR <reason>), to the client that asked
 *   EXPOSURE auto | EXPOSURE <lines>,<gain>   (gain as a factor, e.g. 2.5)  -> OK
 *   EV <-3..3>                -> OK         exposure compensation of the auto exposure
 *   METER <x>,<y> | METER off -> OK         spot metering at a point of the portrait preview (0..1)
 *   WB auto | WB <r>,<g>,<b>  -> OK
 *   SET <key> <value>         -> OK         dng 0|1, rotate 0|90|180|270, quality 50-100,
 *                                           preview_mode 1|3|4 (saved in /var/root/camd.conf)
 * Errors: ERR <text>.
 */

#endif
