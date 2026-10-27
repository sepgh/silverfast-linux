/* Unit tests for firmware preparation and the upload handshake, using a
 * scripted fake device instead of USB. */
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/firmware.h"

struct fake {
    uint8_t written[200000];
    size_t  written_len;
    int     writes;
    const uint8_t *replies;
    size_t  replies_len, replies_pos;
};

static int fake_write(void *ctx, const uint8_t *buf, size_t len)
{
    struct fake *f = ctx;
    memcpy(f->written + f->written_len, buf, len);
    f->written_len += len;
    f->writes++;
    return 0;
}

static int fake_read(void *ctx, uint8_t *buf, size_t len)
{
    struct fake *f = ctx;
    if (f->replies_pos + len > f->replies_len) return -EIO;
    memcpy(buf, f->replies + f->replies_pos, len);
    f->replies_pos += len;
    return 0;
}

static void test_prepare(void)
{
    uint8_t file[SFL_FW_HEADER_SIZE + 5] = { 'E', 'D', 'L', 'A' };
    uint8_t *payload;
    size_t len;

    file[SFL_FW_HEADER_SIZE + 0] = 0x10;
    file[SFL_FW_HEADER_SIZE + 1] = 0xf0;
    file[SFL_FW_HEADER_SIZE + 2] = 0x05;
    file[SFL_FW_HEADER_SIZE + 3] = 0x01;
    file[SFL_FW_HEADER_SIZE + 4] = 0xaa; /* placeholder, replaced by the checksum */

    payload = sfl_fw_prepare(file, sizeof(file), &len);
    assert(payload && len == 5);
    assert(payload[0] == 0x10 && payload[3] == 0x01);
    assert(payload[4] == (uint8_t)(0x10 + 0xf0 + 0x05 + 0x01));
    free(payload);

    assert(!sfl_fw_prepare(file, SFL_FW_HEADER_SIZE, &len));
}

static void test_is_loaded(void)
{
    static const uint8_t booting[] = { 0x28, 0x00 }, running[] = { 0x18, 0x81 };
    struct fake f = { .replies = booting, .replies_len = 2 };
    struct sfl_fw_io io = { &f, fake_write, fake_read };

    assert(sfl_fw_is_loaded(&io) == 0);
    assert(f.written_len == 2 && f.written[0] == 0x1b && f.written[1] == 0x03);

    memset(&f, 0, sizeof(f));
    f.replies = running; f.replies_len = 2;
    assert(sfl_fw_is_loaded(&io) == 1);
}

static void test_upload_sequence(void)
{
    static const uint8_t acks[] = { 6, 6, 6, 6 };
    size_t len = 0x1fff0 + 10, i; /* more than one chunk */
    uint8_t *payload = malloc(len);
    struct fake *f = calloc(1, sizeof(*f));
    struct sfl_fw_io io = { f, fake_write, fake_read };

    for (i = 0; i < len; i++) payload[i] = (uint8_t)i;
    f->replies = acks; f->replies_len = sizeof(acks);

    assert(sfl_fw_upload(&io, payload, len) == 0);
    assert(f->replies_pos == 4);
    /* ESC 06, size, 3 chunks (0xfff0, 0xfff0, 9), checksum byte, ESC 16, 0x80 */
    assert(f->writes == 1 + 1 + 3 + 1 + 1 + 1);
    assert(f->written[0] == 0x1b && f->written[1] == 0x06);
    assert(f->written[2] == (len & 0xff) && f->written[3] == ((len >> 8) & 0xff) && f->written[4] == ((len >> 16) & 0xff));
    assert(memcmp(f->written + 6, payload, len) == 0);
    assert(f->written[6 + len] == 0x1b && f->written[7 + len] == 0x16 && f->written[8 + len] == 0x80);
    free(payload);
    free(f);
}

static void test_upload_nak(void)
{
    static const uint8_t nak[] = { 0x15 };
    uint8_t payload[4] = { 1, 2, 3, 6 };
    struct fake f = { .replies = nak, .replies_len = 1 };
    struct sfl_fw_io io = { &f, fake_write, fake_read };

    assert(sfl_fw_upload(&io, payload, sizeof(payload)) == -EPROTO);
    assert(f.writes == 1); /* stops right after ESC 06 */
}

int main(void)
{
    test_prepare();
    test_is_loaded();
    test_upload_sequence();
    test_upload_nak();
    puts("test_firmware: ok");
    return 0;
}
