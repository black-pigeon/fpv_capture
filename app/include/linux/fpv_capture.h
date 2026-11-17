/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
#ifndef _UAPI_FPV_CAPTURE_H
#define _UAPI_FPV_CAPTURE_H
#include <linux/types.h>
#include <linux/ioctl.h>
/* Fixed-width, pointer-free ABI, identical on ARM32 and host. */
struct fpv_capture_config {
    __u32 channel;
    __u32 standard_mask; /* bit n: 0 PAL, 1 NTSC */
    __u32 enable_mask;
    __u32 timeout_ms;
    __u32 cfo[4];        /* low 16 bits: signed DDS phase increment */
};
struct fpv_capture_config_v2 {
    struct fpv_capture_config config;
    __u32 auto_mask;     /* bit n: detect PAL/NTSC automatically */
};
/* state is the coherent hardware word: generation[31:8], reserved[7:4],
 * confidence[3:2], detected_valid[1], detected_NTSC[0]. Periods are in
 * DDC valid samples; invalid detections must be displayed as UNKNOWN. */
struct fpv_capture_detect_channel {
    __u32 state;
    __u32 field_samples;
    __u32 line_samples;
};
struct fpv_capture_detect {
    __u32 auto_mask;
    struct fpv_capture_detect_channel channel[4];
};
#define FPV_CAPTURE_DETECT_NTSC 1U
#define FPV_CAPTURE_DETECT_VALID 2U
#define FPV_CAPTURE_DETECT_CONFIDENCE_SHIFT 2U
#define FPV_CAPTURE_DETECT_CONFIDENCE_MASK 12U
#define FPV_CAPTURE_DETECT_GENERATION_SHIFT 8U
struct fpv_capture_info {
    __u32 status;
    __u32 lock_mask;
    __u32 fault_mask;
    __u32 frame_count[4];
    __u32 last_error;
};
#define FPV_CAPTURE_CONFIG _IOW('F', 1, struct fpv_capture_config)
#define FPV_CAPTURE_INFO   _IOR('F', 2, struct fpv_capture_info)
#define FPV_CAPTURE_GET_CONFIG _IOR('F', 3, struct fpv_capture_config)
/* Legacy CONFIG explicitly selects manual operation on all four channels. */
#define FPV_CAPTURE_CONFIG_V2 _IOW('F', 4, struct fpv_capture_config_v2)
#define FPV_CAPTURE_GET_CONFIG_V2 _IOR('F', 5, struct fpv_capture_config_v2)
#define FPV_CAPTURE_DETECT _IOR('F', 6, struct fpv_capture_detect)
/* v6 tuning transaction: BEGIN must return before touching the RF LO.
 * COMMIT releases acquisition after RF settling/configuration. On failure leave
 * inhibited; a later owner can inspect SCAN_STATUS and complete the transaction. */
struct fpv_capture_tune { __u32 action; __u32 epoch; };
#define FPV_CAPTURE_TUNE_BEGIN 1U
#define FPV_CAPTURE_TUNE_COMMIT 2U
struct fpv_capture_sync_quality {
    __u32 counts;    /* real HSYNC total[15:0], regular intervals[31:16] */
    __u32 intervals; /* minimum[15:0], maximum[31:16], in valid samples */
};
struct fpv_capture_scan_status {
    struct fpv_capture_detect detect;
    __u32 epoch;
    __u32 tuning;
    __u32 lock_mask;
    __u32 fault_mask;
    struct fpv_capture_sync_quality quality[4];
};
#define FPV_CAPTURE_TUNE _IOWR('F', 7, struct fpv_capture_tune)
#define FPV_CAPTURE_SCAN_STATUS _IOR('F', 8, struct fpv_capture_scan_status)
#define FPV_CAPTURE_WIDTH 720U
#define FPV_CAPTURE_PAL_HEIGHT 576U
#define FPV_CAPTURE_NTSC_HEIGHT 480U
#define FPV_CAPTURE_MAX_BYTES (720U * 576U + 20U)
#endif
