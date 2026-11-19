#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include "fpv_radio.h"
#include "ad9361_api.h"
static unsigned event, port_calls, lo_calls, removed, released;
static int fail_port, fail_lo, fail_init;
int fpv_frontend_open(struct fpv_frontend *f,const struct fpv_board *b,uint64_t hz)
{assert(event++==0);*f=(struct fpv_frontend){.board=b,.fd=42,.valid=1,.port=fpv_board_rx_port(b,hz)};return 0;}
int fpv_frontend_select(struct fpv_frontend *f,uint64_t hz)
{f->port=fpv_board_rx_port(f->board,hz);f->valid=1;event++;return 0;}
void fpv_frontend_close(struct fpv_frontend *f) {if(f->fd>=0)released++;f->fd=-1;f->valid=0;}
int fpv_ad9361_init(struct ad9361_rf_phy **p,const struct fpv_board *b,uint64_t rx,uint64_t tx,uint32_t rate,uint32_t bw,uint32_t gain,uint32_t att,bool fir)
{assert(event++==1&&b->gpio_reset==100&&rx==tx&&rate==61440000&&bw==56000000&&gain==20&&att==89750&&!fir);*p=(void*)(uintptr_t)1;if(fail_init){errno=EIO;return -1;}return 0;}
int32_t ad9361_set_rx_rf_port_input(struct ad9361_rf_phy *p,uint32_t port)
{assert(p&&port<=1);port_calls++;return fail_port?-1:0;}
int32_t ad9361_set_rx_lo_freq(struct ad9361_rf_phy *p,uint64_t hz)
{assert(p&&hz>=70000000);lo_calls++;return fail_lo?-1:0;}
int32_t ad9361_remove(struct ad9361_rf_phy *p) {assert(p);removed++;return 0;}
#include "../radio/fpv_radio.c"
int main(void)
{
 struct fpv_radio r;
 assert(!fpv_radio_init(&r,fpv_board_get(FPV_BOARD_E310),2400000000ULL,2400000000ULL,61440000,56000000,20,89750,0));
 assert(r.port==1&&r.valid);
 assert(!fpv_radio_set_lo(&r,2500000000ULL)&&port_calls==0&&lo_calls==1);
 assert(!fpv_radio_set_lo(&r,3000000000ULL)&&port_calls==1&&lo_calls==2&&r.port==0);
 assert(!fpv_radio_set_lo(&r,5805000000ULL)&&port_calls==1&&lo_calls==3);
 fail_port=1;assert(fpv_radio_set_lo(&r,900000000)<0&&!r.valid&&lo_calls==3);fail_port=0;
 assert(!fpv_radio_set_lo(&r,900000000)&&port_calls==3&&lo_calls==4&&r.valid);
 fail_lo=1;assert(fpv_radio_set_lo(&r,1000000000)<0&&!r.valid);fail_lo=0;
 assert(!fpv_radio_set_lo(&r,1000000000)&&r.valid);
 fpv_radio_close(&r);fpv_radio_close(&r);assert(removed==1&&released==1&&!r.phy);
 event=0;fail_init=1;
 assert(fpv_radio_init(&r,fpv_board_get(FPV_BOARD_E310),2400000000ULL,2400000000ULL,61440000,56000000,20,89750,0)<0);
 assert(errno==EIO&&!r.phy&&r.frontend.fd==-1&&removed==2&&released==2);
 puts("PASS route-before-init, port-before-LO, cached route, failures and radio cleanup");
}
