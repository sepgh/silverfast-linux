/* Firmware upload for Epson scanners that boot without firmware (e.g. the
 * Perfection V600 / GT-X820). The sequence mirrors what Epson's own Windows
 * driver does in CWIADriver::InitScanner and CWIADriver::DownLoadFirmware
 * (esxw2ud.dll); see docs/how-it-works.md for the derivation.
 */
#ifndef SFL_FIRMWARE_H
#define SFL_FIRMWARE_H

#include <stddef.h>
#include <stdint.h>

#define SFL_FW_HEADER_SIZE 256   /* "EDLA" header that is not sent to the device */
#define SFL_FW_CHUNK_SIZE  0xfff0
#define SFL_FW_ACK         0x06
#define SFL_FW_LOADED_BIT  0x10  /* set in the first status byte once firmware runs */

/* Raw bulk I/O used by the upload routine. Both return 0 on success. */
struct sfl_fw_io {
    void *ctx;
    int (*write)(void *ctx, const uint8_t *buf, size_t len);
    int (*read)(void *ctx, uint8_t *buf, size_t len);
};

/* Turns the contents of an esfw*.bin file into the payload that is sent to
 * the scanner: the header is dropped and the last byte is replaced by the
 * 8-bit sum of all bytes before it. Returns a malloc'd buffer or NULL. */
uint8_t *sfl_fw_prepare(const uint8_t *file, size_t file_len, size_t *payload_len);

/* Asks the scanner whether firmware is running (ESC 0x03). Returns 1 if
 * loaded, 0 if not, negative errno on I/O failure. */
int sfl_fw_is_loaded(const struct sfl_fw_io *io);

/* Uploads a payload produced by sfl_fw_prepare(). Returns 0 or negative errno. */
int sfl_fw_upload(const struct sfl_fw_io *io, const uint8_t *payload, size_t len);

#endif
