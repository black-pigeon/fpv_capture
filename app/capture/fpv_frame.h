#ifndef FPV_FRAME_H
#define FPV_FRAME_H
#include <stdint.h>
#include <stddef.h>
struct fpv_frame_info {
    unsigned channel, standard, sequence, width, height;
};
/* Rejects damaged/error packets; optionally restores even/odd fields to rows. */
int fpv_frame_parse(const void *packet, size_t size, struct fpv_frame_info *info,
                    uint8_t *gray, size_t capacity);
#endif
