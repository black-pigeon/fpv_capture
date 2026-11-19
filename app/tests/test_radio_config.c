#include <assert.h>
#include <stdio.h>
#include "e200_ad9361_config.h"
#include "fpv_board.h"
int main(void)
{
 fpv_ad9361_config_init(fpv_board_get(FPV_BOARD_E310));
 assert(e200_default_init_param.gpio_resetb.number==100);
 assert(e200_default_init_param.gpio_sync.number==99);
 assert(e200_default_init_param.lvds_mode_enable);
 assert(!e200_default_init_param.full_port_enable);
 assert(!e200_default_init_param.swap_ports_enable);
 assert(!e200_default_init_param.digital_interface_tune_fir_disable);
 assert(e200_default_init_param.tx_fb_clock_delay==7);
 assert(e200_default_init_param.lvds_bias_mV==150);
 assert(e200_default_init_param.tx_data_delay==0);
 assert(e200_default_init_param.pp_tx_swap_enable&&e200_default_init_param.pp_rx_swap_enable);
 assert(e200_default_init_param.rx_frame_pulse_mode_enable);
 assert(e200_default_init_param.two_rx_two_tx_mode_enable);
 assert(e200_rx_adc_init.num_channels==4);
 assert(e200_tx_dac_init.num_channels==4);
 assert(e200_default_init_param.xo_disable_use_ext_refclk_enable);
 assert(fpv_board_get(FPV_BOARD_E310)->gpio_ctl_base==94);
 assert(fpv_board_get(FPV_BOARD_E310)->gpio_status_base==86);
 /* Reinitializing another profile must restore its independent interface. */
 fpv_ad9361_config_init(fpv_board_get(FPV_BOARD_E200));
 assert(e200_default_init_param.gpio_resetb.number==68);
 assert(e200_default_init_param.two_rx_two_tx_mode_enable);
 assert(e200_rx_adc_init.num_channels==4);
 assert(e200_tx_dac_init.num_channels==4);
 assert(e200_default_init_param.gpio_sync.number==67);
 assert(e200_default_init_param.lvds_mode_enable);
 assert(!e200_default_init_param.full_port_enable);
 assert(!e200_default_init_param.swap_ports_enable);
 assert(!e200_default_init_param.digital_interface_tune_fir_disable);
 assert(e200_default_init_param.tx_fb_clock_delay==4);
 assert(e200_default_init_param.tx_data_delay==0);
 puts("PASS E310 LVDS settings and independent E200 LVDS defaults");
}
