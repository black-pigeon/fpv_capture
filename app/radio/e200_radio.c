#include <errno.h>
#include <getopt.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ad9361_api.h"
#include "axi_dac_core.h"
#include "e200_ad9361_config.h"
#include "linux_gpio.h"
#include "no_os_delay.h"
#include "no_os_gpio.h"

#include "e200_radio.h"
#include "fpv_radio.h"

static int check_step(const char *step, int ret)
{
	if (ret < 0) {
		fprintf(stderr, "%s failed: %d\n", step, ret);
		return -1;
	}

	return 0;
}

static int configure_gpio_outputs(const struct fpv_board *board)
{
	struct no_os_gpio_desc *en_agc = NULL;
	struct no_os_gpio_desc *ctl[4] = {0};
	struct no_os_gpio_init_param en_agc_param = {
		.number = board->gpio_en_agc,
		.platform_ops = &linux_gpio_ops,
		.extra = NULL,
	};
	int ret = 0;
	int i;

	ret = no_os_gpio_get(&en_agc, &en_agc_param);
	if (ret < 0)
		goto out;
	ret = no_os_gpio_direction_output(en_agc, NO_OS_GPIO_HIGH);
	if (ret < 0)
		goto out;

	for (i = 0; i < 4; i++) {
		struct no_os_gpio_init_param param = {
			.number = board->gpio_ctl_base + i,
			.platform_ops = &linux_gpio_ops,
			.extra = NULL,
		};

		ret = no_os_gpio_get(&ctl[i], &param);
		if (ret < 0)
			goto out;
		ret = no_os_gpio_direction_output(ctl[i], NO_OS_GPIO_LOW);
		if (ret < 0)
			goto out;
	}

out:
	for (i = 0; i < 4; i++)
		no_os_gpio_remove(ctl[i]);
	no_os_gpio_remove(en_agc);
	return ret;
}

static int print_status_gpios(const struct fpv_board *board)
{
	struct no_os_gpio_desc *status[8] = {0};
	uint8_t value = 0;
	uint32_t status_word = 0;
	int ret = 0;
	int i;

	for (i = 0; i < 8; i++) {
		struct no_os_gpio_init_param param = {
			.number = board->gpio_status_base + i,
			.platform_ops = &linux_gpio_ops,
			.extra = NULL,
		};

		ret = no_os_gpio_get(&status[i], &param);
		if (ret < 0)
			goto out;
		ret = no_os_gpio_direction_input(status[i]);
		if (ret < 0)
			goto out;
		ret = no_os_gpio_get_value(status[i], &value);
		if (ret < 0)
			goto out;
		status_word |= (value ? 1u : 0u) << i;
	}

	printf("AD9361 CTRL_OUT/status[7:0]=0x%02" PRIx32 "\n", status_word);

out:
	for (i = 0; i < 8; i++)
		no_os_gpio_remove(status[i]);
	return ret;
}

int fpv_ad9361_init(struct ad9361_rf_phy **out, const struct fpv_board *board, uint64_t rx_lo, uint64_t tx_lo, uint32_t sample_rate,
		       uint32_t bandwidth, uint32_t rx_gain,
		       uint32_t tx_atten_mdb, bool enable_fir)
{
	struct ad9361_rf_phy *phy = NULL;
	*out = NULL;
	fpv_ad9361_config_init(board);

	e200_default_init_param.spi_param.device_id = E200_SPI_DEVICE_ID;
	e200_default_init_param.spi_param.chip_select = E200_SPI_CS;
	e200_default_init_param.gpio_resetb.number = board->gpio_reset;
	e200_default_init_param.gpio_sync.number = board->gpio_sync;
	e200_default_init_param.rx_rf_port_input_select = fpv_board_rx_port(board, rx_lo);
	e200_default_init_param.rx_synthesizer_frequency_hz = rx_lo;
	e200_default_init_param.tx_synthesizer_frequency_hz = tx_lo;
	e200_default_init_param.rf_rx_bandwidth_hz = bandwidth;
	e200_default_init_param.rf_tx_bandwidth_hz = bandwidth;
	e200_default_init_param.rx_adc_init = &e200_rx_adc_init;
	e200_default_init_param.tx_dac_init = &e200_tx_dac_init;

	if (check_step("Board GPIO setup", configure_gpio_outputs(board)))
		return -1;
	if (check_step("AD9361 init",
		       ad9361_init(&phy, &e200_default_init_param)) || !phy)
		return -1;

	*out = phy;
	if (check_step("AD9361 AXI DAC init",
		       axi_dac_init(&phy->tx_dac, &e200_tx_dac_init)))
		return -1;

	if (enable_fir) {
		if (check_step("AD9361 TX FIR config",
			       ad9361_set_tx_fir_config(phy, e200_tx_fir_config)))
			return -1;
		if (check_step("AD9361 RX FIR config",
			       ad9361_set_rx_fir_config(phy, e200_rx_fir_config)))
			return -1;
		if (check_step("AD9361 FIR enable",
			       ad9361_set_trx_fir_en_dis(phy, 1)))
			return -1;
	}

	if (sample_rate) {
		if (check_step("AD9361 RX sample rate",
			       ad9361_set_rx_sampling_freq(phy, sample_rate)))
			return -1;
		if (check_step("AD9361 TX sample rate",
			       ad9361_set_tx_sampling_freq(phy, sample_rate)))
			return -1;
	}

	if (bandwidth) {
		if (check_step("AD9361 RX bandwidth",
			       ad9361_set_rx_rf_bandwidth(phy, bandwidth)))
			return -1;
		if (check_step("AD9361 TX bandwidth",
			       ad9361_set_tx_rf_bandwidth(phy, bandwidth)))
			return -1;
	}

	if (check_step("AD9361 RX1 gain mode",
		       ad9361_set_rx_gain_control_mode(phy, RX1, RF_GAIN_MGC)))
		return -1;
	if (board->rx_channels > 1 && check_step("AD9361 RX2 gain mode",
		       ad9361_set_rx_gain_control_mode(phy, RX2, RF_GAIN_MGC)))
		return -1;
	if (check_step("AD9361 RX1 gain",
		       ad9361_set_rx_rf_gain(phy, RX1, (int32_t)rx_gain)))
		return -1;
	if (board->rx_channels > 1 && check_step("AD9361 RX2 gain",
		       ad9361_set_rx_rf_gain(phy, RX2, (int32_t)rx_gain)))
		return -1;
	if (check_step("AD9361 TX1 attenuation",
		       ad9361_set_tx_attenuation(phy, TX1, tx_atten_mdb)))
		return -1;
	if (board->rx_channels > 1 && check_step("AD9361 TX2 attenuation",
		       ad9361_set_tx_attenuation(phy, TX2, tx_atten_mdb)))
		return -1;
	if (check_step("AD9361 RX LO", ad9361_set_rx_lo_freq(phy, rx_lo)))
		return -1;
	if (check_step("AD9361 TX LO", ad9361_set_tx_lo_freq(phy, tx_lo)))
		return -1;

	if (check_step("AD9361 AXI DAC data select",
		       axi_dac_set_datasel(phy->tx_dac, -1,
					   AXI_DAC_DATA_SEL_DMA)))
		return -1;

	return print_status_gpios(board);
}

/* Compatibility entry point for existing E200 applications. */
int e200_radio_init(struct ad9361_rf_phy **out,uint64_t rx,uint64_t tx,
 uint32_t rate,uint32_t bw,uint32_t gain,uint32_t att,bool fir)
{
 return fpv_ad9361_init(out,fpv_board_get(FPV_BOARD_E200),rx,tx,rate,bw,gain,att,fir);
}
