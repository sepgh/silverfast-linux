#include "rxbuf.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

void sfl_rxbuf_init(struct sfl_rxbuf *rx, size_t packet_size)
{
    memset(rx, 0, sizeof(*rx));
    rx->packet_size = packet_size ? packet_size : 512;
}

void sfl_rxbuf_clear(struct sfl_rxbuf *rx)
{
    rx->len = 0;
}

void sfl_rxbuf_free(struct sfl_rxbuf *rx)
{
    free(rx->data);
    memset(rx, 0, sizeof(*rx));
}

static int reserve(struct sfl_rxbuf *rx, size_t need)
{
    uint8_t *p;
    size_t cap = rx->cap ? rx->cap : 4096;

    if (need <= rx->cap) return 0;
    while (cap < need) cap *= 2;
    if (!(p = realloc(rx->data, cap))) return -ENOMEM;
    rx->data = p;
    rx->cap = cap;
    return 0;
}

long sfl_rxbuf_read(struct sfl_rxbuf *rx, uint8_t *out, size_t want, sfl_bulk_read_fn fn, void *ctx)
{
    size_t give;

    while (rx->len < want)
    {
        size_t missing = want - rx->len;
        size_t ask = (missing + rx->packet_size - 1) / rx->packet_size * rx->packet_size;
        int got, ret;

        if ((ret = reserve(rx, rx->len + ask))) return ret;
        got = fn(ctx, rx->data + rx->len, ask);
        if (got < 0) return got;
        rx->len += got;
        if (got == 0 || got % rx->packet_size) break; /* short packet: transfer is over */
    }

    give = rx->len < want ? rx->len : want;
    memcpy(out, rx->data, give);
    memmove(rx->data, rx->data + give, rx->len - give);
    rx->len -= give;
    return (long)give;
}
