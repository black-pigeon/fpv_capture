#include "e200_ad9361_config.h"

#include "e200_ad9361_defaults.h"
#include "fpv_board.h"

struct axi_adc_init e200_rx_adc_init = {
	"cf-ad9361-lpc",
	E200_AD9361_RX_BASEADDR,
	4
};

static struct axi_dac_channel e200_tx_dac_channels[4] = {
	{ .sel = AXI_DAC_DATA_SEL_DMA },
	{ .sel = AXI_DAC_DATA_SEL_DMA },
	{ .sel = AXI_DAC_DATA_SEL_DMA },
	{ .sel = AXI_DAC_DATA_SEL_DMA },
};

struct axi_dac_init e200_tx_dac_init = {
	"cf-ad9361-dds-core-lpc",
	E200_AD9361_TX_BASEADDR,
	4,
	e200_tx_dac_channels
};

AD9361_InitParam e200_default_init_param;
AD9361_RXFIRConfig e200_rx_fir_config;
AD9361_TXFIRConfig e200_tx_fir_config;

void e200_ad9361_config_init(void)
{
	e200_rx_adc_init.num_channels = 4;
	e200_tx_dac_init.num_channels = 4;
	e200_default_init_param = e200_ad9361_default_init_param_template;
	e200_rx_fir_config = e200_ad9361_rx_fir_config_template;
	e200_tx_fir_config = e200_ad9361_tx_fir_config_template;

	e200_default_init_param.spi_param.device_id = E200_SPI_DEVICE_ID;
	e200_default_init_param.spi_param.chip_select = E200_SPI_CS;
	e200_default_init_param.gpio_resetb.number = E200_GPIO_RESETB;
	e200_default_init_param.gpio_sync.number = E200_GPIO_SYNC;
	e200_default_init_param.gpio_cal_sw1.number = -1;
	e200_default_init_param.gpio_cal_sw2.number = -1;
	e200_default_init_param.xo_disable_use_ext_refclk_enable = 1;
	e200_default_init_param.digital_interface_tune_skip_mode = 0;
	e200_default_init_param.rx_adc_init = &e200_rx_adc_init;
	e200_default_init_param.tx_dac_init = &e200_tx_dac_init;
}

/* Start from independent defaults when selecting a board profile. */
void fpv_ad9361_config_init(const struct fpv_board *board)
{
	e200_ad9361_config_init();
	e200_default_init_param.gpio_resetb.number = board->gpio_reset;
	e200_default_init_param.gpio_sync.number = board->gpio_sync;
	if (board == fpv_board_get(FPV_BOARD_E310)) {
		/* antsdre310 LVDS reference; keep automatic digital tuning. */
		e200_default_init_param.tx_fb_clock_delay = 7;
		e200_default_init_param.lvds_bias_mV = 150;
	}
}
