#ifndef E200_AD9361_CONFIG_H
#define E200_AD9361_CONFIG_H

#include "ad9361_api.h"
#include "axi_adc_core.h"
#include "axi_dac_core.h"

#define E200_AD9361_BASEADDR       0x43C00000u
#define E200_AD9361_RX_BASEADDR    E200_AD9361_BASEADDR
#define E200_AD9361_TX_BASEADDR    (E200_AD9361_BASEADDR + 0x4000u)

#define E200_SPI_DEVICE_ID         0
#define E200_SPI_CS                0

/*
 * Zynq-7000 PS GPIO has 54 MIO lines before EMIO. The HDL connects:
 *   EMIO[ 7:0]  -> gpio_status[7:0]
 *   EMIO[11:8]  -> gpio_ctl[3:0]
 *   EMIO[12]    -> gpio_en_agc
 *   EMIO[13]    -> gpio_sync
 *   EMIO[14]    -> gpio_resetb
 */
#define E200_ZYNQ_GPIO_EMIO_BASE   54
#define E200_GPIO_STATUS_BASE      (E200_ZYNQ_GPIO_EMIO_BASE + 0)
#define E200_GPIO_CTL_BASE         (E200_ZYNQ_GPIO_EMIO_BASE + 8)
#define E200_GPIO_EN_AGC           (E200_ZYNQ_GPIO_EMIO_BASE + 12)
#define E200_GPIO_SYNC             (E200_ZYNQ_GPIO_EMIO_BASE + 13)
#define E200_GPIO_RESETB           (E200_ZYNQ_GPIO_EMIO_BASE + 14)

extern struct axi_adc_init e200_rx_adc_init;
extern struct axi_dac_init e200_tx_dac_init;
extern AD9361_InitParam e200_default_init_param;
extern AD9361_RXFIRConfig e200_rx_fir_config;
extern AD9361_TXFIRConfig e200_tx_fir_config;

void e200_ad9361_config_init(void);

#endif
