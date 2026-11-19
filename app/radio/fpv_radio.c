#include "fpv_radio.h"
#include "ad9361_api.h"
#include <errno.h>
#include <stddef.h>
void fpv_radio_close(struct fpv_radio *r)
{
 if(r->phy)ad9361_remove(r->phy);
 r->phy=NULL;r->valid=0;fpv_frontend_close(&r->frontend);
}
int fpv_radio_init(struct fpv_radio *r,const struct fpv_board *b,
 uint64_t rx,uint64_t tx,uint32_t rate,uint32_t bw,uint32_t gain,uint32_t att,bool fir)
{
 *r=(struct fpv_radio){.frontend={.fd=-1}};
 if(fpv_frontend_open(&r->frontend,b,rx)<0)return -1;
 if(fpv_ad9361_init(&r->phy,b,rx,tx,rate,bw,gain,att,fir)<0) {
  int saved=errno;fpv_radio_close(r);errno=saved;return -1;
 }
 r->port=r->frontend.port;r->valid=1;return 0;
}
int fpv_radio_set_lo(struct fpv_radio *r,uint64_t hz)
{
 int port=fpv_board_rx_port(r->frontend.board,hz);
 if(!r->phy||port<0){errno=EINVAL;return -1;}
 int update_port=!r->valid||r->port!=port;
 r->valid=0;
 if(fpv_frontend_select(&r->frontend,hz)<0)return -1;
 if(update_port && ad9361_set_rx_rf_port_input(r->phy,(uint32_t)port)<0){errno=EIO;return -1;}
 if(ad9361_set_rx_lo_freq(r->phy,hz)<0){errno=EIO;return -1;}
 r->port=port;r->valid=1;return 0;
}
