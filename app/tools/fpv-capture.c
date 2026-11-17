#define _POSIX_C_SOURCE 200809L
#include "fpv_frame.h"
#include <linux/fpv_capture.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

static void usage(FILE *out)
{
    fprintf(out,"Usage: fpv-capture [--device PATH] --status\n"
        "       fpv-capture --channel 0..3 [--standard auto|pal|ntsc] --output FILE\n"
        "                   [--timeout-ms 100..5000] [--device PATH]\n"
        "Configure one selected channel, receive its next frame, save PGM.\n"
        "Automatic standard detection is the default; other channels retain their settings.\n"
        "Initialize AD9361 manually with fpv-radio-probe first.\n");
}
static int number(const char *s, unsigned low, unsigned high, unsigned *out)
{
    char *end;
    unsigned long n;
    errno=0;n=strtoul(s,&end,10);
    if(errno || !*s || *end || n<low || n>high) return -1;
    *out=(unsigned)n;return 0;
}
int main(int argc, char **argv)
{
    const char *device="/dev/fpv-capture", *output=NULL;
    struct fpv_capture_config_v2 cfg_v2={
        .config={0,0,15,1500,{0xa000,0xe000,0x2000,0x6000}}, .auto_mask=15};
    struct fpv_capture_config *cfg=&cfg_v2.config;
    struct fpv_capture_detect detect;
    struct fpv_capture_info status;
    struct fpv_frame_info frame;
    uint8_t *packet=NULL,*gray=NULL;
    unsigned standard=0, requested_channel, requested_timeout;
    int ch,fd=-1,ret=1,show_status=0,have_channel=0,automatic=1;
    FILE *file=NULL;
    size_t bytes,pixels;
    ssize_t received;
    static const struct option options[]={
        {"channel",required_argument,NULL,'c'}, {"standard",required_argument,NULL,'s'},
        {"output",required_argument,NULL,'o'}, {"timeout-ms",required_argument,NULL,'t'},
        {"device",required_argument,NULL,'d'}, {"status",no_argument,NULL,'S'},
        {"help",no_argument,NULL,'h'}, {NULL,0,NULL,0}};
    while((ch=getopt_long(argc,argv,"c:s:o:t:d:Sh",options,NULL))!=-1) {
        switch(ch) {
        case 'c': if(number(optarg,0,3,&cfg->channel)) goto bad_args;have_channel=1;break;
        case 's': if(!strcmp(optarg,"auto")){automatic=1;break;}
                  if(!strcmp(optarg,"pal"))standard=0;
                  else if(!strcmp(optarg,"ntsc"))standard=1;else goto bad_args;
                  automatic=0;break;
        case 'o':output=optarg;break;
        case 't':if(number(optarg,100,5000,&cfg->timeout_ms))goto bad_args;break;
        case 'd':device=optarg;break;
        case 'S':show_status=1;break;
        case 'h':usage(stdout);return 0;
        default:goto bad_args;
        }
    }
    if(optind!=argc || (!show_status && (!output||!have_channel)))goto bad_args;
    fd=open(device,O_RDWR|O_CLOEXEC);
    if(fd<0){perror(device);goto done;}
    if(ioctl(fd,FPV_CAPTURE_INFO,&status)<0){perror("capture status");goto done;}
    if(show_status) {
        printf("status=0x%02x lock=0x%x ddc_fault=0x%x last_error=0x%x\n",
               status.status,status.lock_mask,status.fault_mask,status.last_error);
        if(ioctl(fd,FPV_CAPTURE_DETECT,&detect)<0){perror("detect standard");goto done;}
        for(unsigned i=0;i<4;i++) {
            uint32_t state=detect.channel[i].state;
            const char *name=!(state&FPV_CAPTURE_DETECT_VALID)?"UNKNOWN":
                (state&FPV_CAPTURE_DETECT_NTSC)?"NTSC":"PAL";
            printf("channel %u: %s mode=%s confidence=%u field_samples=%u "
                   "line_samples=%u generation=%u frames=%u locked=%u\n",
                   i,name,(detect.auto_mask>>i)&1?"auto":"manual",
                   (state&FPV_CAPTURE_DETECT_CONFIDENCE_MASK)>>FPV_CAPTURE_DETECT_CONFIDENCE_SHIFT,
                   detect.channel[i].field_samples,detect.channel[i].line_samples,
                   state>>FPV_CAPTURE_DETECT_GENERATION_SHIFT,
                   status.frame_count[i],(status.lock_mask>>i)&1);
        }
        ret=0;goto done;
    }
    requested_channel=cfg->channel;requested_timeout=cfg->timeout_ms;
    /* Preserve other channels' modes and DDS settings across invocations. */
    if(ioctl(fd,FPV_CAPTURE_GET_CONFIG_V2,&cfg_v2)<0){perror("capture config");goto done;}
    cfg->channel=requested_channel;cfg->timeout_ms=requested_timeout;
    cfg->enable_mask|=1u<<cfg->channel;
    if(automatic)cfg_v2.auto_mask|=1u<<cfg->channel;
    else {
        cfg_v2.auto_mask&=~(1u<<cfg->channel);
        cfg->standard_mask=(cfg->standard_mask & ~(1u<<cfg->channel)) | (standard<<cfg->channel);
    }
    if(ioctl(fd,FPV_CAPTURE_CONFIG_V2,&cfg_v2)<0){perror("configure capture");goto done;}
    bytes=automatic?FPV_CAPTURE_MAX_BYTES:720u*(standard?480u:576u)+20u;
    packet=malloc(bytes);gray=malloc(bytes-20);
    if(!packet||!gray){perror("allocate frame");goto done;}
    received=read(fd,packet,bytes);
    if(received<0){perror("receive frame");goto done;}
    if((size_t)received>bytes ||
       fpv_frame_parse(packet,(size_t)received,&frame,gray,bytes-20)<0 ||
       frame.channel!=cfg->channel || (!automatic && frame.standard!=standard)){
        fprintf(stderr,"Invalid, incomplete or damaged frame packet\n");goto done;
    }
    pixels=(size_t)frame.width*frame.height;
    file=fopen(output,"wb");if(!file){perror(output);goto done;}
    if(fprintf(file,"P5\n%u %u\n255\n",frame.width,frame.height)<0 ||
       fwrite(gray,1,pixels,file)!=pixels){perror("write image");goto done;}
    if(fclose(file)){file=NULL;perror("close image");goto done;}file=NULL;
    printf("Saved channel %u %s frame %u: %ux%u to %s\n",frame.channel,
           frame.standard?"NTSC":"PAL",frame.sequence,frame.width,frame.height,output);
    ret=0;
done:
    if(file)fclose(file);
    if(fd>=0)close(fd);
    free(packet);free(gray);return ret;
bad_args:
    usage(stderr);return 2;
}
