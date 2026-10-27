#include "firmware.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

uint8_t *sfl_fw_prepare(const uint8_t *file, size_t file_len, size_t *payload_len)
{
    size_t len, i;
    uint8_t *payload, sum = 0;

    if (file_len <= SFL_FW_HEADER_SIZE + 1) return NULL;
    len = file_len - SFL_FW_HEADER_SIZE;
    if (!(payload = malloc(len))) return NULL;
    memcpy(payload, file + SFL_FW_HEADER_SIZE, len);
    for (i = 0; i < len - 1; i++) sum += payload[i];
    payload[len - 1] = sum;
    *payload_len = len;
    return payload;
}

static int expect_ack(const struct sfl_fw_io *io)
{
    uint8_t reply;
    int ret = io->read(io->ctx, &reply, 1);

    if (ret) return ret;
    return reply == SFL_FW_ACK ? 0 : -EPROTO;
}

static int send_and_ack(const struct sfl_fw_io *io, const uint8_t *buf, size_t len)
{
    int ret = io->write(io->ctx, buf, len);
    return ret ? ret : expect_ack(io);
}

int sfl_fw_is_loaded(const struct sfl_fw_io *io)
{
    static const uint8_t status_cmd[2] = { 0x1b, 0x03 };
    uint8_t status[2];
    int ret;

    if ((ret = io->write(io->ctx, status_cmd, sizeof(status_cmd)))) return ret;
    if ((ret = io->read(io->ctx, status, sizeof(status)))) return ret;
    return (status[0] & SFL_FW_LOADED_BIT) ? 1 : 0;
}

int sfl_fw_upload(const struct sfl_fw_io *io, const uint8_t *payload, size_t len)
{
    static const uint8_t begin[2] = { 0x1b, 0x06 }, commit[2] = { 0x1b, 0x16 }, start[1] = { 0x80 };
    uint8_t size_le[4];
    size_t off;
    int ret;

    if (len < 2) return -EINVAL;
    if ((ret = send_and_ack(io, begin, sizeof(begin)))) return ret;

    size_le[0] = len & 0xff; size_le[1] = (len >> 8) & 0xff;
    size_le[2] = (len >> 16) & 0xff; size_le[3] = (len >> 24) & 0xff;
    if ((ret = io->write(io->ctx, size_le, sizeof(size_le)))) return ret;

    /* Everything except the trailing checksum byte, in driver-sized chunks. */
    for (off = 0; off < len - 1; off += SFL_FW_CHUNK_SIZE)
    {
        size_t n = len - 1 - off < SFL_FW_CHUNK_SIZE ? len - 1 - off : SFL_FW_CHUNK_SIZE;
        if ((ret = io->write(io->ctx, payload + off, n))) return ret;
    }
    if ((ret = send_and_ack(io, payload + len - 1, 1))) return ret;

    if ((ret = send_and_ack(io, commit, sizeof(commit)))) return ret;
    return send_and_ack(io, start, sizeof(start));
}
