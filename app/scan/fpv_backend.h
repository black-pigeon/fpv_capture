#ifndef FPV_BACKEND_H
#define FPV_BACKEND_H
#include "fpv_scan.h"
#include "fpv_radio.h"
struct ad9361_rf_phy;
struct fpv_backend {
    int fd, simulate;
    struct ad9361_rf_phy *phy;
    struct fpv_radio radio;
    struct fpv_capture_config_v2 config;
    struct fpv_scan_hop hop;
    uint32_t epoch;
    uint64_t radio_lo_hz;
    unsigned radio_gain;
    int radio_valid, rf_pending, rf_changed, frame_config_valid;
};
void fpv_backend_init(struct fpv_backend *b, int simulate);
int fpv_backend_open(struct fpv_backend *b,const struct fpv_scan_config *cfg,uint64_t initial_lo);
int fpv_backend_tune(struct fpv_backend *b,const struct fpv_scan_hop *hop,unsigned gain);
int fpv_backend_commit(struct fpv_backend *b);
/* Worker-only gain transaction: preserve the LO, routing and DDC settings. */
int fpv_backend_set_gain(struct fpv_backend *b,unsigned gain);
int fpv_backend_status(struct fpv_backend *b,struct fpv_capture_scan_status *status);
/* Returns allocated reordered grayscale pixels only after parser validation. */
int fpv_backend_frame(struct fpv_backend *b,unsigned ch,unsigned *standard,unsigned char **gray,unsigned *height);
void fpv_backend_close(struct fpv_backend *b);
#endif
