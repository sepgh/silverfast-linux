/* Wire protocol between sti.dll (inside Wine) and silverfast-usbd (on Linux).
 *
 * Every request is a 5-byte header followed by an optional payload:
 *     u8  op       one of the SFL_OP_* values below
 *     u32 length   little-endian; payload size for HELLO/WRITE,
 *                  number of bytes wanted for READ
 *
 * Every request gets exactly one response:
 *     i32 status   little-endian; 0 on success, negative errno on failure
 *     u32 length   little-endian; number of data bytes that follow
 *
 * The helper listens on a Unix socket and, for Wine builds without AF_UNIX
 * support, on a loopback TCP port. Every client must start with HELLO carrying
 * the session token from $SFL_USBD_TOKEN.
 *
 * This header is shared by both sides, so it must stay plain C that
 * compiles with gcc on Linux and with MinGW for Windows.
 */
#ifndef SFL_PROTOCOL_H
#define SFL_PROTOCOL_H

#define SFL_PROTOCOL_MAGIC "SFL1"

enum sfl_op {
    SFL_OP_HELLO = 'H', /* payload: SFL_PROTOCOL_MAGIC + session token; reply: device description.
                          Must be the first request on a connection. */
    SFL_OP_WRITE = 'W', /* payload: bytes for the bulk-out endpoint */
    SFL_OP_READ  = 'R', /* length: bytes wanted from the bulk-in endpoint */
};

#define SFL_HEADER_SIZE   5
#define SFL_REPLY_SIZE    8
#define SFL_MAX_TRANSFER  (16u * 1024u * 1024u)

#endif
