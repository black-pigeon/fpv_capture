#ifndef FPV_BOARD_H
#define FPV_BOARD_H
#include <stdint.h>
enum fpv_board_id { FPV_BOARD_E200, FPV_BOARD_E310 };
struct fpv_board {
 const char *name;
 int gpio_reset, gpio_sync;
 unsigned gpio_en_agc, gpio_ctl_base, gpio_status_base;
 uint64_t rx_boundary_hz;
 unsigned rx_channels;
 unsigned rx_switch[4]; /* RX1 high/low, RX2 high/low, Zynq controller offsets */
};
const struct fpv_board *fpv_board_get(enum fpv_board_id id);
const struct fpv_board *fpv_board_current(void);
/* AD9361 balanced RX port: 0=A, 1=B, or negative for unsupported frequency. */
int fpv_board_rx_port(const struct fpv_board *board,uint64_t frequency_hz);
#endif
