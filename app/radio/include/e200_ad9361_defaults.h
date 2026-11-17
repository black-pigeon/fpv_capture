#ifndef E200_AD9361_DEFAULTS_H
#define E200_AD9361_DEFAULTS_H

#include "ad9361_api.h"
#include "axi_adc_core.h"
#include "axi_dac_core.h"

extern struct axi_adc_init e200_default_rx_adc_init;
extern struct axi_dac_init e200_default_tx_dac_init;
extern AD9361_InitParam e200_ad9361_default_init_param_template;
extern AD9361_RXFIRConfig e200_ad9361_rx_fir_config_template;
extern AD9361_TXFIRConfig e200_ad9361_tx_fir_config_template;

#endif
