#include "fpv_backend.h"
#include "fpv_frame.h"
#include "fpv_radio.h"
#include "ad9361_api.h"
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

void fpv_backend_init(struct fpv_backend *b,int simulate) { memset(b,0,sizeof(*b));b->fd=-1;b->radio.frontend.fd=-1;b->simulate=simulate; }
void fpv_backend_close(struct fpv_backend *b)
{
    if(b->fd>=0)close(b->fd);
    fpv_radio_close(&b->radio);
    b->fd=-1;b->phy=NULL;b->radio_valid=0;b->rf_pending=0;b->frame_config_valid=0;
}
int fpv_backend_open(struct fpv_backend *b,const struct fpv_scan_config *cfg,uint64_t lo)
{
    setenv("NO_OS_GPIOCHIP","/dev/gpiochip0",0);
    if(!b->radio_valid) {
        if(!b->simulate)fpv_radio_close(&b->radio);
        if(!b->simulate && fpv_radio_init(&b->radio,fpv_board_current(),lo,lo,61440000,56000000,cfg->gain_db,89750,false)<0) { errno=EIO;return -1; }
        b->phy=b->radio.phy;
        b->radio_lo_hz=lo;b->radio_gain=cfg->gain_db;b->radio_valid=1;b->rf_pending=1;
    }
    if(b->simulate)return 0;
    if(b->fd<0)b->fd=open("/dev/fpv-capture",O_RDWR|O_CLOEXEC);
    if(b->fd<0)return -1;
    if(ioctl(b->fd,FPV_CAPTURE_GET_CONFIG_V2,&b->config)<0)return -1;
    struct fpv_capture_scan_status status;
    if(ioctl(b->fd,FPV_CAPTURE_SCAN_STATUS,&status)<0)return -1;
    /* An interrupted previous owner may have left acquisition inhibited.
     * A fresh RF initialization has finished; complete it before next BEGIN. */
    if(status.tuning) {
        struct fpv_capture_tune t={FPV_CAPTURE_TUNE_COMMIT,0};
        if(ioctl(b->fd,FPV_CAPTURE_TUNE,&t)<0)return -1;
    }
    return 0;
}
int fpv_backend_tune(struct fpv_backend *b,const struct fpv_scan_hop *h,unsigned gain)
{
    b->hop=*h;b->frame_config_valid=0;
    int lo_changed=!b->radio_valid||b->radio_lo_hz!=h->lo_hz;
    int gain_changed=!b->radio_valid||b->radio_gain!=gain;
    b->rf_changed=b->rf_pending||lo_changed||gain_changed;
    if(b->simulate){b->radio_lo_hz=h->lo_hz;b->radio_gain=gain;b->radio_valid=1;b->rf_pending=0;b->epoch++;return 0;}
    struct fpv_capture_tune t={FPV_CAPTURE_TUNE_BEGIN,0};
    if(ioctl(b->fd,FPV_CAPTURE_TUNE,&t)<0)return -1;
    b->epoch=t.epoch;
    /* Every digital phase still starts a fresh inhibited epoch. Only an RF
     * state change invokes no-OS SPI/calibration; failed RF state is invalid
     * and acquisition remains inhibited until the owner closes/reopens. */
    if((lo_changed && fpv_radio_set_lo(&b->radio,h->lo_hz)<0) ||
       (gain_changed && (ad9361_set_rx_rf_gain(b->phy,RX1,(int32_t)gain)<0 ||
                         (fpv_board_current()->rx_channels>1 && ad9361_set_rx_rf_gain(b->phy,RX2,(int32_t)gain)<0)))) {
        b->radio_valid=0;errno=EIO;return -1;
    }
    b->radio_lo_hz=h->lo_hz;b->radio_gain=gain;b->radio_valid=1;b->rf_pending=0;
    b->config=(struct fpv_capture_config_v2){.config={.channel=0,.enable_mask=h->enable_mask,.timeout_ms=350},.auto_mask=h->enable_mask};
    for(unsigned i=0;i<4;i++)b->config.config.cfo[i]=h->cfo[i];
    int ret=ioctl(b->fd,FPV_CAPTURE_CONFIG_V2,&b->config);
    b->frame_config_valid=ret==0;return ret;
}
int fpv_backend_commit(struct fpv_backend *b)
{
    if(b->simulate)return 0;
    struct fpv_capture_tune t={FPV_CAPTURE_TUNE_COMMIT,0};
    if(ioctl(b->fd,FPV_CAPTURE_TUNE,&t)<0)return -1;
    if(t.epoch!=b->epoch){errno=ESTALE;return -1;}
    return 0;
}
int fpv_backend_set_gain(struct fpv_backend *b,unsigned gain)
{
    if(gain>60){errno=EINVAL;return -1;}
    if(!b->radio_valid||b->rf_pending){errno=ENODEV;return -1;}
    if(gain==b->radio_gain)return 0;
    if(b->simulate){b->radio_gain=gain;b->epoch++;return 0;}
    struct fpv_capture_tune t={FPV_CAPTURE_TUNE_BEGIN,0};
    if(ioctl(b->fd,FPV_CAPTURE_TUNE,&t)<0)return -1;
    b->epoch=t.epoch;
    if(ad9361_set_rx_rf_gain(b->phy,RX1,(int32_t)gain)<0 ||
       (fpv_board_current()->rx_channels>1 && ad9361_set_rx_rf_gain(b->phy,RX2,(int32_t)gain)<0)) {
        b->radio_valid=0;errno=EIO;return -1;
    }
    if(fpv_backend_commit(b)<0){b->radio_valid=0;return -1;}
    b->radio_gain=gain;
    return 0;
}
int fpv_backend_status(struct fpv_backend *b,struct fpv_capture_scan_status *s)
{
    if(!b->simulate)return ioctl(b->fd,FPV_CAPTURE_SCAN_STATUS,s);
    memset(s,0,sizeof(*s));s->epoch=b->epoch;s->detect.auto_mask=b->hop.enable_mask;
    for(unsigned ch=0;ch<b->hop.count;ch++) {
        /* Host-only PAL at 5800 MHz and NTSC at 5860 MHz. */
        int64_t pal=(int64_t)b->hop.actual_hz[ch]-5800000000LL;
        int64_t ntsc=(int64_t)b->hop.actual_hz[ch]-5860000000LL;
        if(llabs(pal)<=1000000 || llabs(ntsc)<=1000000) {
            unsigned standard=llabs(ntsc)<=1000000;
            s->lock_mask|=1U<<ch;s->detect.channel[ch]=(struct fpv_capture_detect_channel){(b->epoch<<8)|14|standard,307200,983};
            s->quality[ch]=(struct fpv_capture_sync_quality){(65U<<16)|67,(983U<<16)|983};
        }
    }
    return 0;
}
int fpv_backend_frame(struct fpv_backend *b,unsigned ch,unsigned *std,unsigned char **gray,unsigned *height)
{
    *gray=malloc(720*576);if(!*gray)return -1;
    if(b->simulate) {
        *std=llabs((int64_t)b->hop.actual_hz[ch]-5860000000LL)<=1000000;
        *height=*std?480:576;
        for(unsigned y=0;y<*height;y++)for(unsigned x=0;x<720;x++)(*gray)[y*720+x]=(unsigned char)((x/90)*32+(y/72+b->epoch)%16);
        return 0;
    }
    /* CONFIG applies FPGA settings. Reuse it while staying on the same
     * channel, and re-apply only after a selection change or failed capture. */
    if(!b->frame_config_valid || b->config.config.channel!=ch) {
        b->config.config.channel=ch;
        if(ioctl(b->fd,FPV_CAPTURE_CONFIG_V2,&b->config)<0)goto fail;
        b->frame_config_valid=1;
    }
    unsigned char *packet=malloc(FPV_CAPTURE_MAX_BYTES);if(!packet)goto fail;
    ssize_t n=read(b->fd,packet,FPV_CAPTURE_MAX_BYTES);
    struct fpv_frame_info info;
    int ok=n>0?fpv_frame_parse(packet,(size_t)n,&info,*gray,720*576):-1;
    free(packet);
    if(ok<0){if(n>=0)errno=EBADMSG;goto fail;}
    if(info.channel!=ch){errno=EBADMSG;goto fail;}
    *height=info.height;*std=info.standard;return 0;
fail:
    b->frame_config_valid=0;
    free(*gray);*gray=NULL;return -1;
}
