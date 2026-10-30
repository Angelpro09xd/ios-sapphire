/*
 * qcabt.h: the userland view of the kernel's /dev/qcabt (sapphire kernel 0280, bsd/dev/sweet_bt.c).
 * Raw bytes of QUP0 SE3 (the WCN3988's HCI UART) in both directions, no framing in the kernel.
 * The kernel side must use the same numbers; change both together.
 *
 *   read()   blocks until at least one byte arrived (O_NONBLOCK: EAGAIN), returns what is buffered
 *   write()  queues bytes for TX (RTS/CTS honoured when flow control is on), returns when queued
 *   QCABT_POWER    int: 1 = vote the rails (L9/L16/L17/L23), mux GPIO 8-11, clock SE3, UART at
 *                  115200 8N1 with flow control on; 0 = UART off (rails stay voted: Wi-Fi shares them)
 *   QCABT_SETBAUD  uint32_t: host UART speed (2400 ... 3200000)
 *   QCABT_FLOW     int: 1 = automatic RTS/CTS, 0 = off (RTS held asserted)
 *   QCABT_FLUSH    drop the RX buffer and anything not yet sent
 *   QCABT_SWCTRL   int out: level of GPIO 87 (the WCN's SW_CTRL)
 *   QCABT_DRAIN    wait until the TX FIFO is empty (so a baud change cannot cut a command)
 * The power pulse (0xFC at 2400 baud) is done from userland with SETBAUD/FLOW/write.
 */
#ifndef QCABT_H
#define QCABT_H
#include <sys/ioccom.h>
#include <stdint.h>

#define QCABT_DEV       "/dev/qcabt"
#define QCABT_POWER     _IOW('q', 1, int)
#define QCABT_SETBAUD   _IOW('q', 2, uint32_t)
#define QCABT_FLOW      _IOW('q', 3, int)
#define QCABT_FLUSH     _IO('q', 4)
#define QCABT_SWCTRL    _IOR('q', 5, int)
#define QCABT_DRAIN     _IO('q', 6)

#endif
