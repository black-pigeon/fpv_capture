#ifndef FPV_SCAN_H
#define FPV_SCAN_H
#include <stddef.h>
#include <stdint.h>
#include <linux/fpv_capture.h>
#define FPV_SCAN_BANDS 8
#define FPV_SCAN_TRACKS 16
/* Center span reserves room for one 15.36 MSPS channel inside 56 MHz RF.
 * This is a scheduling guard, not a guarantee of FPV acquisition bandwidth. */
#define FPV_SCAN_CENTER_SPAN_HZ 40000000U
struct fpv_scan_band { uint64_t start_hz, end_hz; };
enum fpv_scan_mode { FPV_SCAN_MODE_SCAN, FPV_SCAN_MODE_LOCK };
struct fpv_scan_config {
    enum fpv_scan_mode mode;
    uint64_t lock_hz;
    struct fpv_scan_band bands[FPV_SCAN_BANDS];
    unsigned band_count, step_hz, dwell_ms, settle_ms, gain_db, repeat;
};
struct fpv_scan_hop {
    uint64_t lo_hz, target_hz[4], actual_hz[4];
    unsigned enable_mask, cfo[4], count;
    unsigned window, phase, phases;
};
void fpv_scan_defaults(struct fpv_scan_config *cfg);
int fpv_scan_config_parse(const char *body, struct fpv_scan_config *cfg, char *error, size_t cap);
unsigned fpv_scan_points(const struct fpv_scan_config *cfg);
/* point counts completed scheduled centers, not ascending grid indices.
 * Advance it by hop.count; lane phases visit centers in distributed order. */
int fpv_scan_plan(const struct fpv_scan_config *cfg, unsigned point, struct fpv_scan_hop *hop);
int fpv_scan_candidate(const struct fpv_capture_scan_status *s, uint32_t epoch, unsigned ch);
/* Header + grayscale palette + top-down 8-bit pixels. Caller owns result. */
unsigned char *fpv_scan_bmp(const unsigned char *gray, unsigned width, unsigned height, size_t *size);
#endif
