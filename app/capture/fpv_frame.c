#include "fpv_frame.h"
#include <linux/fpv_capture.h>
#include <errno.h>
#include <string.h>
static uint32_t le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1]<<8) |
           ((uint32_t)p[2]<<16) | ((uint32_t)p[3]<<24);
}
int fpv_frame_parse(const void *packet, size_t size, struct fpv_frame_info *info,
                    uint8_t *gray, size_t capacity)
{
    const uint8_t *p=packet;
    uint32_t tag, shape;
    size_t pixels;
    unsigned h, y, field, row=0;
    if (!p || !info || size<20) return -EINVAL;
    tag=le32(p+4); shape=le32(p+8);
    h=(tag & (1u<<16)) ? FPV_CAPTURE_NTSC_HEIGHT : FPV_CAPTURE_PAL_HEIGHT;
    pixels=FPV_CAPTURE_WIDTH*h;
    if (le32(p)!=0x46505647 || (tag>>24)!=2 || (tag & 0x00f80000) ||
        shape!=((FPV_CAPTURE_WIDTH<<16)|h) || le32(p+12)!=pixels/4 ||
        size!=pixels+20 || le32(p+size-4)!=0x454e4400) return -EBADMSG;
    if (gray && capacity<pixels) return -ENOSPC;
    info->channel=(tag>>17)&3; info->standard=(tag>>16)&1;
    info->sequence=tag&0xffff; info->width=FPV_CAPTURE_WIDTH; info->height=h;
    if (gray)
        for(field=0;field<2;field++)
            for(y=field;y<h;y+=2,row++)
                memcpy(gray+y*FPV_CAPTURE_WIDTH,p+16+row*FPV_CAPTURE_WIDTH,
                       FPV_CAPTURE_WIDTH);
    return 0;
}
