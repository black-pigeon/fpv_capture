/* Exercise production hardware backend ordering with no actual RF or MMIO. */
#include <assert.h>
#include <stdarg.h>
#include <errno.h>
/* Parse fortified libc wrappers before redirecting calls to fake I/O. */
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "ad9361_api.h"
#include "e200_radio.h"
#include "fpv_backend.h"
static unsigned inits, begins, commits, configs, los, gains, closes;
static int inhibited, fail_lo, fail_read;
static unsigned selected_channel;
static int fake_open(const char *path,int flags,...) { (void)flags;assert(!strcmp(path,"/dev/fpv-capture"));return 42; }
static int fake_close(int fd) { assert(fd==42);closes++;return 0; }
static void put_word(unsigned char *p,uint32_t value) {for(unsigned i=0;i<4;i++)p[i]=(unsigned char)(value>>(8*i));}
static ssize_t fake_read(int fd,void *buf,size_t count)
{
    assert(fd==42 && !inhibited);
    if(fail_read){errno=ETIMEDOUT;return -1;}
    unsigned char *p=buf;size_t size=720*576+20;assert(count>=size);memset(buf,0,size);
    put_word(p,0x46505647);put_word(p+4,(2U<<24)|(selected_channel<<17));
    put_word(p+8,(720U<<16)|576);put_word(p+12,720*576/4);put_word(p+size-4,0x454e4400);
    return (ssize_t)size;
}
static int fake_ioctl(int fd,unsigned long request,...)
{
    assert(fd==42);va_list ap;va_start(ap,request);void *arg=va_arg(ap,void *);va_end(ap);
    if(request==FPV_CAPTURE_GET_CONFIG_V2)memset(arg,0,sizeof(struct fpv_capture_config_v2));
    else if(request==FPV_CAPTURE_SCAN_STATUS) {
        struct fpv_capture_scan_status *s=arg;memset(s,0,sizeof(*s));s->epoch=begins;s->tuning=inhibited;
    } else if(request==FPV_CAPTURE_TUNE) {
        struct fpv_capture_tune *t=arg;
        if(t->action==FPV_CAPTURE_TUNE_BEGIN){assert(!inhibited);inhibited=1;begins++;}
        else {assert(inhibited);inhibited=0;commits++;}
        t->epoch=begins;
    } else if(request==FPV_CAPTURE_CONFIG_V2){
        struct fpv_capture_config_v2 *c=arg;
        assert(c->config.enable_mask&(1U<<c->config.channel));
        selected_channel=c->config.channel;configs++;
    }
    else assert(0);
    return 0;
}
int e200_radio_init(struct ad9361_rf_phy **phy,uint64_t rx,uint64_t tx,uint32_t rate,uint32_t bw,uint32_t gain,uint32_t atten,bool fir)
{
    assert(rx==tx&&rate==61440000&&bw==56000000&&gain==20&&atten==89750&&!fir);
    inits++;*phy=(struct ad9361_rf_phy *)(uintptr_t)1;return 0;
}
int32_t ad9361_set_rx_lo_freq(struct ad9361_rf_phy *phy,uint64_t hz)
{assert(phy&&inhibited&&hz>=70000000);los++;return fail_lo?-1:0;}
int32_t ad9361_set_rx_rf_gain(struct ad9361_rf_phy *phy,uint8_t ch,int32_t gain)
{assert(phy&&inhibited&&(ch==RX1||ch==RX2)&&(gain==20||gain==21));gains++;return 0;}
int32_t ad9361_remove(struct ad9361_rf_phy *phy) {assert(phy);return 0;}
#define open fake_open
#define close fake_close
#define read fake_read
#define ioctl fake_ioctl
#include "../scan/fpv_backend.c"
#undef open
#undef close
#undef read
#undef ioctl
int main(void)
{
    struct fpv_backend b;struct fpv_scan_config cfg;struct fpv_scan_hop h;
    fpv_scan_defaults(&cfg);assert(!fpv_scan_plan(&cfg,0,&h));fpv_backend_init(&b,0);
    assert(!fpv_backend_open(&b,&cfg,h.lo_hz)&&inits==1);
    assert(!fpv_backend_tune(&b,&h,20)&&inhibited&&b.epoch==1&&configs==1);
    assert(!fpv_backend_commit(&b)&&!inhibited&&commits==1);
    unsigned standard,height;unsigned char *gray=NULL;
    for(unsigned i=0;i<3;i++) {
        assert(!fpv_backend_frame(&b,0,&standard,&gray,&height));free(gray);
        assert(configs==1 && begins==1); /* repeated frames never re-apply DDC settings */
    }
    assert(!fpv_backend_frame(&b,1,&standard,&gray,&height));free(gray);assert(configs==2);
    assert(!fpv_backend_frame(&b,0,&standard,&gray,&height));free(gray);assert(configs==3);
    fail_read=1;assert(fpv_backend_frame(&b,0,&standard,&gray,&height)<0 && errno==ETIMEDOUT && !gray);
    fail_read=0;assert(!fpv_backend_frame(&b,0,&standard,&gray,&height));free(gray);assert(configs==4);
    assert(los==0&&gains==0&&b.rf_changed);
    assert(!fpv_backend_open(&b,&cfg,h.lo_hz)&&inits==1);
    assert(!fpv_scan_plan(&cfg,4,&h) && h.phase==1 && h.phases==2);
    assert(!fpv_backend_tune(&b,&h,20)&&inhibited&&b.epoch==2);
    assert(!fpv_backend_commit(&b)&&!inhibited);
    assert(los==0&&gains==0&&!b.rf_changed);
    assert(!fpv_scan_plan(&cfg,7,&h) && h.phase==0);
    assert(!fpv_backend_tune(&b,&h,20)&&b.rf_changed&&los==1&&gains==0);
    assert(!fpv_backend_commit(&b));
    assert(!fpv_backend_tune(&b,&h,21)&&b.rf_changed&&los==1&&gains==2);
    assert(!fpv_backend_commit(&b));
    assert(!fpv_backend_tune(&b,&h,21)&&!b.rf_changed&&los==1&&gains==2);
    assert(!fpv_backend_commit(&b));
    h.lo_hz+=40000000;
    fail_lo=1;assert(fpv_backend_tune(&b,&h,21)<0&&inhibited&&commits==5&&configs==8);
    fpv_backend_close(&b);assert(closes==1&&inits==1&&los==2);
    puts("PASS persistent no-OS init, BEGIN before LO, same-LO epochs, failed tune stays inhibited");
}
