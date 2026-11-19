/* Exercise grouped Linux GPIO requests without touching hardware. */
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/gpio.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include "fpv_board.h"
#include "fpv_frontend.h"
static unsigned requests, updates, releases;
static int fail_set, fail_request;
static unsigned bits;
static int fake_open(const char *p,int flags,...) { (void)flags;assert(!strcmp(p,"/dev/gpiochip0"));return 41; }
static int fake_close(int fd) { assert(fd==41||fd==42);if(fd==42)releases++;return 0; }
static int fake_ioctl(int fd,unsigned long req,...)
{
 va_list ap;va_start(ap,req);void *arg=va_arg(ap,void*);va_end(ap);
 if(req==GPIO_GET_LINEHANDLE_IOCTL) {
  assert(fd==41);struct gpiohandle_request *r=arg;assert(r->lines==4 && r->flags==GPIOHANDLE_REQUEST_OUTPUT);
  const unsigned pins[]={54,55,58,59};bits=0;
  for(unsigned i=0;i<4;i++){assert(r->lineoffsets[i]==pins[i]);bits|=r->default_values[i]<<i;}
  requests++;if(fail_request){errno=EBUSY;return -1;}r->fd=42;
 } else {assert(fd==42 && req==GPIOHANDLE_SET_LINE_VALUES_IOCTL);struct gpiohandle_data *d=arg;
  updates++;if(fail_set){errno=EIO;return -1;}bits=0;for(unsigned i=0;i<4;i++)bits|=d->values[i]<<i;
 }
 return 0;
}
static int fake_fcntl(int fd,int cmd,...) {assert(fd==42&&cmd==F_SETFD);return 0;}
#define fcntl fake_fcntl
#define open fake_open
#define close fake_close
#define ioctl fake_ioctl
#include "../radio/fpv_frontend.c"
#undef fcntl
#undef open
#undef close
#undef ioctl
int main(void)
{
 const struct fpv_board *e200=fpv_board_get(FPV_BOARD_E200),*e310=fpv_board_get(FPV_BOARD_E310);
 assert(!strcmp(e200->name,"e200")&&!strcmp(e310->name,"e310"));
 assert(e200->gpio_reset==68 && e200->gpio_sync==67 && e310->gpio_reset==100 && e310->gpio_sync==99);
 assert(fpv_board_rx_port(e310,70000000)==1 && fpv_board_rx_port(e310,2999999999ULL)==1);
 assert(fpv_board_rx_port(e310,3000000000ULL)==0 && fpv_board_rx_port(e310,6000000000ULL)==0);
 assert(fpv_board_rx_port(e310,69999999)<0 && fpv_board_rx_port(e310,6000000001ULL)<0);
 assert(fpv_board_rx_port(e200,1000000000)==0);
 struct fpv_frontend f;
 assert(!fpv_frontend_open(&f,e200,1000000000));assert(!requests&&f.fd==-1);
 assert(!fpv_frontend_select(&f,5805000000ULL));assert(!requests&&!updates);fpv_frontend_close(&f);
 assert(!fpv_frontend_open(&f,e310,2999999999ULL));assert(requests==1&&bits==10&&f.port==1);
 assert(!fpv_frontend_select(&f,2400000000ULL)&&updates==0);
 assert(!fpv_frontend_select(&f,3000000000ULL)&&updates==1&&bits==5&&f.port==0);
 assert(!fpv_frontend_select(&f,6000000000ULL)&&updates==1);
 fail_set=1;assert(fpv_frontend_select(&f,900000000)<0 && !f.valid);fail_set=0;
 assert(!fpv_frontend_select(&f,900000000)&&updates==3&&bits==10&&f.valid);
 fpv_frontend_close(&f);fpv_frontend_close(&f);assert(releases==1);
 fail_request=1;assert(fpv_frontend_open(&f,e310,2400000000ULL)<0 && !f.valid && f.fd==-1);fpv_frontend_close(&f);assert(releases==1);
 puts("PASS E200/E310 profile, exact boundaries, atomic held GPIO routes, failed route and cleanup");
}
