/* Scanners the helper knows how to drive. v1 supports the Epson Perfection
 * V600 only; see docs/plans/future-pure-wine-and-more-scanners.md for how
 * further models get added. */
#ifndef SFL_MODELS_H
#define SFL_MODELS_H

#include <stdint.h>

struct sfl_model {
    uint16_t    vid, pid;
    const char *name;
    const char *firmware;  /* file name inside the firmware directory, or NULL */
    uint8_t     ep_out, ep_in;
    uint16_t    packet_size;
};

static const struct sfl_model sfl_models[] = {
    { 0x04b8, 0x013a, "EPSON Perfection V600 (GT-X820)", "esfwa1.bin", 0x02, 0x81, 512 },
};

#define SFL_MODEL_COUNT (sizeof(sfl_models) / sizeof(sfl_models[0]))

#endif
