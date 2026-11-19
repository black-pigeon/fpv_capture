#include "fpv_board.h"
#include <stddef.h>
static const struct fpv_board boards[] = {
 { .name="e200", .gpio_reset=68, .gpio_sync=67,
   .gpio_en_agc=66, .gpio_ctl_base=62, .gpio_status_base=54,
   .rx_boundary_hz=0, .rx_channels=2 },
 /* Match the standalone antsdre310 LVDS shell: switches EMIO0/1,4/5;
  * status32..39, control40..43, EN_AGC44, SYNC45, RESET46. */
 { .name="e310", .gpio_reset=100, .gpio_sync=99,
   .gpio_en_agc=98, .gpio_ctl_base=94, .gpio_status_base=86,
   .rx_boundary_hz=3000000000ULL, .rx_channels=2,
   .rx_switch={54,55,58,59} }
};
const struct fpv_board *fpv_board_get(enum fpv_board_id id)
{ return id==FPV_BOARD_E200||id==FPV_BOARD_E310?&boards[id]:NULL; }
const struct fpv_board *fpv_board_current(void)
{
#ifdef FPV_TARGET_E310
 return &boards[FPV_BOARD_E310];
#else
 return &boards[FPV_BOARD_E200];
#endif
}
int fpv_board_rx_port(const struct fpv_board *b,uint64_t hz)
{
 if(!b || hz<70000000 || hz>6000000000ULL)return -1;
 return b->rx_boundary_hz && hz<b->rx_boundary_hz?1:0;
}
