#ifndef FPV_RADIO_H
#define FPV_RADIO_H
#include <stdbool.h>
#include "fpv_frontend.h"
struct ad9361_rf_phy;
struct fpv_radio {
 struct ad9361_rf_phy *phy;
 struct fpv_frontend frontend;
 int port, valid;
};
int fpv_ad9361_init(struct ad9361_rf_phy **out,const struct fpv_board *board,
 uint64_t rx_lo,uint64_t tx_lo,uint32_t rate,uint32_t bandwidth,uint32_t gain,
 uint32_t attenuation,bool fir);
/* Initialize fresh/closed storage; close before reinitializing an owner. */
int fpv_radio_init(struct fpv_radio *radio,const struct fpv_board *board,
 uint64_t rx_lo,uint64_t tx_lo,uint32_t rate,uint32_t bandwidth,uint32_t gain,
 uint32_t attenuation,bool fir);
/* Caller holds an inhibited capture epoch during route/port/LO changes. */
int fpv_radio_set_lo(struct fpv_radio *radio,uint64_t hz);
void fpv_radio_close(struct fpv_radio *radio);
#endif
