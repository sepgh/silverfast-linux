/* Transport used by IStiDevice raw I/O: a connection to silverfast-usbd over
 * its Unix socket, or loopback TCP where Wine lacks AF_UNIX.
 * Kept behind this small interface so a future usbscan.sys based backend
 * (docs/plans/future-pure-wine-and-more-scanners.md) can replace it. */
#ifndef SFL_TRANSPORT_H
#define SFL_TRANSPORT_H

#include <windows.h>

/* Connects (if needed) and checks the helper is talking to a scanner.
 * Returns S_OK or an HRESULT error. */
HRESULT transport_connect(void);

/* Bulk-out write of exactly len bytes. */
HRESULT transport_write(const void *buf, DWORD len);

/* Bulk-in read of up to *len bytes; *len receives the byte count. */
HRESULT transport_read(void *buf, DWORD *len);

void transport_disconnect(void);

#endif
