#ifndef E200_RADIO_H
#define E200_RADIO_H
#include <stdint.h>
#include <stdbool.h>
struct ad9361_rf_phy;
/* Caller removes *out even if a later initialization step fails. */
int e200_radio_init(struct ad9361_rf_phy **out, uint64_t rx_lo, uint64_t tx_lo,
    uint32_t sample_rate, uint32_t bandwidth, uint32_t rx_gain,
    uint32_t tx_atten_mdb, bool enable_fir);
#endif
