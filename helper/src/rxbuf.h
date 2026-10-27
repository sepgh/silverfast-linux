/* Packet-aligned receive buffer.
 *
 * SilverFast asks for exact byte counts, but the scanner sometimes sends more
 * than was asked for. Reading an exact count from a bulk endpoint then fails
 * with LIBUSB_ERROR_OVERFLOW, so every bulk read is rounded up to whole
 * packets and the surplus is kept here for the next request.
 */
#ifndef SFL_RXBUF_H
#define SFL_RXBUF_H

#include <stddef.h>
#include <stdint.h>

/* Reads up to cap bytes from the device into buf; returns the byte count or a
 * negative errno. A result that is not a multiple of the packet size means
 * the device ended the transfer (short packet). */
typedef int (*sfl_bulk_read_fn)(void *ctx, uint8_t *buf, size_t cap);

struct sfl_rxbuf {
    uint8_t *data;
    size_t   len, cap;
    size_t   packet_size;
};

void sfl_rxbuf_init(struct sfl_rxbuf *rx, size_t packet_size);
void sfl_rxbuf_clear(struct sfl_rxbuf *rx);
void sfl_rxbuf_free(struct sfl_rxbuf *rx);

/* Fills out with up to want bytes. Returns the number of bytes delivered
 * (fewer than want only if the device ended the transfer early) or a negative
 * errno. Surplus bytes stay buffered. */
long sfl_rxbuf_read(struct sfl_rxbuf *rx, uint8_t *out, size_t want, sfl_bulk_read_fn fn, void *ctx);

#endif
