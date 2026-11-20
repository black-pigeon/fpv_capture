#ifndef FPV_SPECTRUM_DEVICE_H
#define FPV_SPECTRUM_DEVICE_H
#include <stdint.h>
int fpv_spectrum_device_read(int *fd,int simulate,uint64_t lo,uint32_t power[2048]);
#endif
