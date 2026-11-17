#include "fpv_scan.h"
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

void fpv_scan_defaults(struct fpv_scan_config *c)
{
    memset(c,0,sizeof(*c)); c->bands[0]=(struct fpv_scan_band){5650000000ULL,5950000000ULL};
    c->band_count=1;c->step_hz=2000000;c->dwell_ms=150;c->settle_ms=20;c->gain_db=20;c->repeat=1;
}
static unsigned band_points(const struct fpv_scan_config *c,unsigned b)
{
    uint64_t span=c->bands[b].end_hz-c->bands[b].start_hz;
    return (unsigned)((span+c->step_hz-1)/c->step_hz+1);
}
unsigned fpv_scan_points(const struct fpv_scan_config *c)
{
    unsigned n=0;for(unsigned b=0;b<c->band_count;b++)n+=band_points(c,b);return n;
}
static int hex(int c) { if(c>='0'&&c<='9')return c-'0';c=tolower((unsigned char)c);return c>='a'&&c<='f'?c-'a'+10:-1; }
static int decode(char *s)
{
    char *d=s;
    while(*s) {
        unsigned char c=(unsigned char)*s++;
        if(c=='%') { if(!s[0]||!s[1]||hex(s[0])<0||hex(s[1])<0)return -1;c=(unsigned char)(hex(s[0])*16+hex(s[1]));s+=2; }
        else if(c=='+')c=' ';
        if(c==0 || (c<32 && c!='\n' && c!='\r' && c!='\t'))return -1;
        *d++=(char)c;
    }
    *d=0;return 0;
}
static int number(const char *s,double min,double max,double *v)
{
    char *end;errno=0;*v=strtod(s,&end);
    return errno||end==s||*end||!isfinite(*v)||*v<min||*v>max?-1:0;
}
int fpv_scan_config_parse(const char *body,struct fpv_scan_config *c,char *error,size_t cap)
{
    char buf[2048],*save=NULL;unsigned seen=0;double v;
    fpv_scan_defaults(c);
    if(strlen(body)>=sizeof(buf))goto bad;
    strcpy(buf,body);
    for(char *p=strtok_r(buf,"&",&save);p;p=strtok_r(NULL,"&",&save)) {
        char *value=strchr(p,'=');if(!value)goto bad;*value++=0;
        if(decode(p)||decode(value))goto bad;
        unsigned bit;
        if(!strcmp(p,"bands")) {
            bit=1;c->band_count=0;char *rows=NULL;
            for(char *r=strtok_r(value,"\r\n,;",&rows);r;r=strtok_r(NULL,"\r\n,;",&rows)) {
                while(isspace((unsigned char)*r))r++;
                char *tail=r+strlen(r);while(tail>r&&isspace((unsigned char)tail[-1]))*--tail=0;
                char *dash=strchr(r,'-');double a,b;
                if(!dash||c->band_count==FPV_SCAN_BANDS)goto bad;
                *dash++=0;
                if(number(r,70,6000,&a)||number(dash,70,6000,&b)||a>b)goto bad;
                c->bands[c->band_count++]=(struct fpv_scan_band){(uint64_t)llround(a*1e6),(uint64_t)llround(b*1e6)};
            }
            if(!c->band_count)goto bad;
        } else if(!strcmp(p,"step_mhz")) {
            bit=2;if(number(value,0.1,10,&v))goto bad;c->step_hz=(unsigned)llround(v*1e6);
        } else if(!strcmp(p,"dwell_ms")) {
            bit=4;if(number(value,100,1000,&v)||v!=floor(v))goto bad;c->dwell_ms=(unsigned)v;
        } else if(!strcmp(p,"settle_ms")) {
            bit=8;if(number(value,0,1000,&v)||v!=floor(v))goto bad;c->settle_ms=(unsigned)v;
        } else if(!strcmp(p,"gain_db")) {
            bit=16;if(number(value,0,60,&v)||v!=floor(v))goto bad;c->gain_db=(unsigned)v;
        } else if(!strcmp(p,"repeat")) {
            bit=32;if(number(value,0,1,&v)||v!=floor(v))goto bad;c->repeat=(unsigned)v;
        } else goto bad;
        if(seen&bit)goto bad;
        seen|=bit;
    }
    if(!(seen&1)||fpv_scan_points(c)>65536)goto bad;
    if(cap)error[0]=0;
    return 0;
bad:
    snprintf(error,cap,"Invalid settings: bands 70..6000 MHz, grid 0.1..10 MHz, observation 100..1000 ms, gain 0..60 dB; up to 8 bands / 65536 points.");
    return -1;
}
int fpv_scan_plan(const struct fpv_scan_config *c,unsigned point,struct fpv_scan_hop *h)
{
    memset(h,0,sizeof(*h));
    unsigned b;
    for(b=0;b<c->band_count;b++){unsigned n=band_points(c,b);if(point<n)break;point-=n;}
    if(b==c->band_count)return -1;
    /* Full windows contain a multiple of four points so no observation
     * slots are wasted. Four contiguous grid lanes are visited in parallel;
     * changing phase walks each lane with the requested fine grid step. */
    unsigned capacity=FPV_SCAN_CENTER_SPAN_HZ/c->step_hz+1;
    if(capacity>=4)capacity-=capacity%4;
    unsigned first=(point/capacity)*capacity;
    unsigned n=band_points(c,b)-first;if(n>capacity)n=capacity;
    unsigned local=point-first;
    if(local%4)return -1; /* point is the number of completed scheduled slots */
    unsigned phase=local/4,base=n/4,extra=n%4;
    h->window=first/capacity;h->phase=phase;h->phases=(n+3)/4;
    uint64_t start=c->bands[b].start_hz+(uint64_t)first*c->step_hz;
    uint64_t end=c->bands[b].start_hz+(uint64_t)(first+n-1)*c->step_hz;
    if(end>c->bands[b].end_hz)end=c->bands[b].end_hz;
    h->lo_hz=(start+end)/2;
    for(unsigned ch=0;ch<4;ch++) {
        unsigned length=base+(ch<extra);
        if(phase>=length)continue;
        unsigned index=first+ch*base+(ch<extra?ch:extra)+phase;
        uint64_t f=c->bands[b].start_hz+(uint64_t)index*c->step_hz;
        h->target_hz[h->count++]=f>c->bands[b].end_hz?c->bands[b].end_hz:f;
    }
    h->enable_mask=(1U<<h->count)-1;
    for(unsigned i=0;i<h->count;i++) {
        int32_t word=(int32_t)llround((double)((int64_t)h->target_hz[i]-(int64_t)h->lo_hz)*65536.0/61440000.0);
        h->cfo[i]=(uint16_t)word;
        h->actual_hz[i]=(uint64_t)((int64_t)h->lo_hz+llround((double)word*61440000.0/65536.0));
    }
    return 0;
}
int fpv_scan_candidate(const struct fpv_capture_scan_status *s,uint32_t epoch,unsigned ch)
{
    if(ch>3||s->epoch!=epoch||s->tuning||!(s->lock_mask&(1U<<ch))||(s->fault_mask&(1U<<ch))||!(s->detect.channel[ch].state&FPV_CAPTURE_DETECT_VALID))return 0;
    unsigned total=s->quality[ch].counts&65535,regular=s->quality[ch].counts>>16;
    /* Equalizing/vertical pulses may add irregular intervals. No image texture
     * gate: static/black video is still legitimate video. */
    return total>=40&&regular>=40&&regular*100>=total*70;
}
static void put32(unsigned char *p,uint32_t v) { for(unsigned i=0;i<4;i++)p[i]=(unsigned char)(v>>(i*8)); }
unsigned char *fpv_scan_bmp(const unsigned char *gray,unsigned w,unsigned h,size_t *size)
{
    if(!w||w>720||!h||h>576)return NULL;
    unsigned stride=(w+3)&~3U;*size=1078+(size_t)stride*h;
    unsigned char *p=calloc(1,*size);if(!p)return NULL;
    p[0]='B';p[1]='M';put32(p+2,(uint32_t)*size);put32(p+10,1078);put32(p+14,40);
    put32(p+18,w);put32(p+22,(uint32_t)-(int32_t)h);p[26]=1;p[28]=8;put32(p+34,stride*h);put32(p+46,256);
    for(unsigned i=0;i<256;i++)p[54+4*i]=p[55+4*i]=p[56+4*i]=(unsigned char)i;
    for(unsigned y=0;y<h;y++)memcpy(p+1078+(size_t)y*stride,gray+(size_t)y*w,w);
    return p;
}
