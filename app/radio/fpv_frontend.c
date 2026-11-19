#include "fpv_frontend.h"
#include <errno.h>
#include <fcntl.h>
#include <linux/gpio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>
static void route_values(unsigned char *values,int port)
{ values[0]=values[2]=port==0;values[1]=values[3]=port==1; }
void fpv_frontend_close(struct fpv_frontend *f)
{ if(f->fd>=0)close(f->fd);f->fd=-1;f->valid=0; }
int fpv_frontend_open(struct fpv_frontend *f,const struct fpv_board *board,uint64_t hz)
{
 *f=(struct fpv_frontend){.board=board,.fd=-1};
 int port=fpv_board_rx_port(board,hz);if(port<0){errno=EINVAL;return -1;}
 if(board->rx_boundary_hz) {
  const char *chip=getenv("NO_OS_GPIOCHIP");if(!chip||!*chip)chip="/dev/gpiochip0";
  int fd=open(chip,O_RDONLY|O_CLOEXEC);if(fd<0)return -1;
  struct gpiohandle_request req={.lines=4,.flags=GPIOHANDLE_REQUEST_OUTPUT};
  memcpy(req.lineoffsets,board->rx_switch,sizeof(board->rx_switch));
  route_values(req.default_values,port);
  strcpy(req.consumer_label,"fpv-rx-route");
  int ret=ioctl(fd,GPIO_GET_LINEHANDLE_IOCTL,&req),saved=errno;close(fd);
  if(ret<0){errno=saved;return -1;}
  f->fd=req.fd;
  if(fcntl(f->fd,F_SETFD,FD_CLOEXEC)<0){saved=errno;fpv_frontend_close(f);errno=saved;return -1;}
 }
 f->port=port;f->valid=1;return 0;
}
int fpv_frontend_select(struct fpv_frontend *f,uint64_t hz)
{
 int port=fpv_board_rx_port(f->board,hz);if(port<0){errno=EINVAL;return -1;}
 if(f->valid&&f->port==port)return 0;
 if(f->board->rx_boundary_hz) {
  if(f->fd<0){errno=EBADF;return -1;}
  struct gpiohandle_data values={0};route_values(values.values,port);
  if(ioctl(f->fd,GPIOHANDLE_SET_LINE_VALUES_IOCTL,&values)<0){f->valid=0;return -1;}
 }
 f->port=port;f->valid=1;return 0;
}
