/* Unit tests for the packet-aligned receive buffer. The fake device mimics
 * the behaviour seen on the V600: it may send more bytes than requested. */
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>

#include "../src/rxbuf.h"

#define PACKET 512

struct fake {
    uint8_t stream[8192];
    size_t  len, pos;
    size_t  max_per_call;   /* how much the device hands out per transfer */
    size_t  last_ask;
    int     calls;
    int     fail;
};

static int fake_read(void *ctx, uint8_t *buf, size_t cap)
{
    struct fake *f = ctx;
    size_t n = f->len - f->pos;

    f->calls++;
    f->last_ask = cap;
    if (f->fail) return -EIO;
    assert(cap % PACKET == 0);
    if (n > cap) n = cap;
    if (n > f->max_per_call) n = f->max_per_call;
    memcpy(buf, f->stream + f->pos, n);
    f->pos += n;
    return (int)n;
}

static void fill(struct fake *f, size_t len)
{
    size_t i;
    memset(f, 0, sizeof(*f));
    for (i = 0; i < len; i++) f->stream[i] = (uint8_t)(i * 7);
    f->len = len;
    f->max_per_call = sizeof(f->stream);
}

static void test_surplus_is_kept(void)
{
    struct sfl_rxbuf rx;
    struct fake f;
    uint8_t out[600];

    /* Device answers a 2-byte request with a 300-byte transfer. */
    fill(&f, 300);
    sfl_rxbuf_init(&rx, PACKET);
    assert(sfl_rxbuf_read(&rx, out, 2, fake_read, &f) == 2);
    assert(f.last_ask == PACKET);
    assert(out[0] == f.stream[0] && out[1] == f.stream[1]);
    assert(rx.len == 298);

    /* Next request is served from the buffer without touching USB. */
    assert(sfl_rxbuf_read(&rx, out, 298, fake_read, &f) == 298);
    assert(f.calls == 1);
    assert(memcmp(out, f.stream + 2, 298) == 0);
    sfl_rxbuf_free(&rx);
}

static void test_multi_packet(void)
{
    struct sfl_rxbuf rx;
    struct fake f;
    uint8_t out[4096];

    /* Full packets keep the transfer going until the request is satisfied. */
    fill(&f, 3000);
    f.max_per_call = 1024;
    sfl_rxbuf_init(&rx, PACKET);
    assert(sfl_rxbuf_read(&rx, out, 2500, fake_read, &f) == 2500);
    assert(memcmp(out, f.stream, 2500) == 0);
    sfl_rxbuf_free(&rx);
}

static void test_short_transfer(void)
{
    struct sfl_rxbuf rx;
    struct fake f;
    uint8_t out[1024];

    /* Device has less than requested: a short packet ends the read early. */
    fill(&f, 100);
    sfl_rxbuf_init(&rx, PACKET);
    assert(sfl_rxbuf_read(&rx, out, 1000, fake_read, &f) == 100);
    assert(f.calls == 1);
    sfl_rxbuf_free(&rx);
}

static void test_error(void)
{
    struct sfl_rxbuf rx;
    struct fake f;
    uint8_t out[16];

    fill(&f, 100);
    f.fail = 1;
    sfl_rxbuf_init(&rx, PACKET);
    assert(sfl_rxbuf_read(&rx, out, 16, fake_read, &f) == -EIO);
    sfl_rxbuf_free(&rx);
}

int main(void)
{
    test_surplus_is_kept();
    test_multi_packet();
    test_short_transfer();
    test_error();
    puts("test_rxbuf: ok");
    return 0;
}
