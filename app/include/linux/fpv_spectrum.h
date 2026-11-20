/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
#ifndef _UAPI_FPV_SPECTRUM_H
#define _UAPI_FPV_SPECTRUM_H
#include <linux/types.h>
#include <linux/ioctl.h>
#define FPV_SPECTRUM_BINS 2048U
#define FPV_SPECTRUM_RATE 61440000U
#define FPV_SPECTRUM_AVERAGES 16U
#define FPV_SPECTRUM_BYTES (FPV_SPECTRUM_BINS * 4U)
/* read() returns exactly 2048 little-endian u32 linear powers, FFT shifted.
 * XFFT scales by 1/N, Hann window coherent gain 0.5, 16 powers averaged.
 * The reader must own RF tuning until read completes; no implicit RF control. */
struct fpv_spectrum_info { __u32 version, bins, sample_rate, averages; };
#define FPV_SPECTRUM_INFO _IOR('S', 1, struct fpv_spectrum_info)
#endif
