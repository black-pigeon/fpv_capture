/* Freshness/black-frame tests run the real worker with a controlled backend. */
#include "fpv_service.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <stdatomic.h>
static int scenario;
static unsigned snapshots;
static atomic_int video_present=1, video_standard=0;
static pthread_t owner;
void fpv_backend_init(struct fpv_backend *b,int simulate) {memset(b,0,sizeof(*b));b->fd=-1;b->simulate=simulate;}
int fpv_backend_open(struct fpv_backend *b,const struct fpv_scan_config *c,uint64_t lo) {(void)b;(void)c;(void)lo;owner=pthread_self();return 0;}
int fpv_backend_tune(struct fpv_backend *b,const struct fpv_scan_hop *h,unsigned gain) {(void)gain;b->rf_changed=b->hop.lo_hz!=h->lo_hz;b->hop=*h;b->epoch++;snapshots=0;return 0;}
int fpv_backend_commit(struct fpv_backend *b) {(void)b;return 0;}
int fpv_backend_set_gain(struct fpv_backend *b,unsigned gain)
{assert(pthread_equal(owner,pthread_self()));b->radio_gain=gain;b->epoch++;snapshots=0;return 0;}
void fpv_backend_close(struct fpv_backend *b) {(void)b;}
int fpv_backend_status(struct fpv_backend *b,struct fpv_capture_scan_status *s)
{
    memset(s,0,sizeof(*s));s->epoch=b->epoch;s->lock_mask=1;s->detect.channel[0].state=(4U<<8)|14|(unsigned)atomic_load(&video_standard);
    s->quality[0].counts=(65U<<16)|67;snapshots++;
    if(snapshots>1) {
        if(scenario==1)s->epoch++;
        if(scenario==2)s->detect.channel[0].state+=(1U<<8);
        if(scenario==3)s->lock_mask=0;
    }
    if(!atomic_load(&video_present))s->lock_mask=0;
    return 0;
}
int fpv_backend_frame(struct fpv_backend *b,unsigned ch,unsigned *std,unsigned char **gray,unsigned *height)
{(void)b;(void)ch;*std=(unsigned)atomic_load(&video_standard);*height=*std?480:576;*gray=calloc(1,720*576);assert(*gray);return 0;}
static void wait_idle(struct fpv_service *s)
{
    for(unsigned n=0;n<200;n++) {
        pthread_mutex_lock(&s->mutex);int running=s->running;pthread_mutex_unlock(&s->mutex);
        if(!running)return;
        struct timespec delay={0,10000000};nanosleep(&delay,NULL);
    }
    assert(!"worker timed out");
}
static void wait_frames(struct fpv_service *s,unsigned frames,uint64_t frequency)
{
    for(unsigned n=0;n<300;n++) {
        pthread_mutex_lock(&s->mutex);
        int ready=s->captured_frames>=frames && s->current.lo_hz==frequency;
        pthread_mutex_unlock(&s->mutex);
        if(ready)return;
        struct timespec delay={0,10000000};nanosleep(&delay,NULL);
    }
    assert(!"locked capture did not advance");
}
int main(void)
{
    struct fpv_service s;assert(!fpv_service_init(&s,1));
    struct fpv_scan_config c;fpv_scan_defaults(&c);c.bands[0]=(struct fpv_scan_band){5800000000ULL,5800000000ULL};c.repeat=0;c.dwell_ms=100;c.settle_ms=0;
    for(scenario=0;scenario<4;scenario++) {
        assert(!fpv_service_start(&s,&c));wait_idle(&s);
        unsigned tracks=0;
        pthread_mutex_lock(&s.mutex);
        for(unsigned i=0;i<FPV_SCAN_TRACKS;i++)if(s.tracks[i].bmp) {
            tracks++;assert(s.tracks[i].size==1078+720*576&&s.tracks[i].bmp[1078]==0);
        }
        pthread_mutex_unlock(&s.mutex);
        assert(tracks==(scenario==0?1U:0U));
    }
    c.settle_ms=1000;assert(!fpv_service_start(&s,&c));fpv_service_stop(&s);wait_idle(&s);
    scenario=0;c.mode=FPV_SCAN_MODE_LOCK;c.lock_hz=5800000000ULL;c.settle_ms=0;c.repeat=0;
    assert(!fpv_service_start(&s,&c));wait_frames(&s,3,c.lock_hz);
    pthread_mutex_lock(&s.mutex);
    assert(s.running && s.lock_ready && s.signal_present && s.ddc_tunes==1 && s.rf_tunes<=1 && s.rounds==0);
    pthread_mutex_unlock(&s.mutex);
    assert(fpv_service_set_gain(&s,61)<0);
    assert(!fpv_service_set_gain(&s,27));
    for(unsigned n=0;n<200;n++) {
        pthread_mutex_lock(&s.mutex);
        int applied=!s.gain_pending&&s.config.gain_db==27;
        pthread_mutex_unlock(&s.mutex);
        if(applied)break;
        struct timespec delay={0,10000000};nanosleep(&delay,NULL);
    }
    pthread_mutex_lock(&s.mutex);
    assert(!s.gain_pending&&s.config.gain_db==27&&s.gain_updates==1);
    assert(s.running&&s.current.lo_hz==c.lock_hz&&s.ddc_tunes==1);
    unsigned after_gain=s.captured_frames;
    pthread_mutex_unlock(&s.mutex);
    wait_frames(&s,after_gain+2,c.lock_hz);
    atomic_store(&video_present,0);
    struct timespec loss_wait={0,200000000};nanosleep(&loss_wait,NULL);
    pthread_mutex_lock(&s.mutex);
    unsigned prior=s.captured_frames;
    assert(s.running && s.lock_ready && !s.signal_present && s.ddc_tunes==1);
    pthread_mutex_unlock(&s.mutex);
    atomic_store(&video_present,1);wait_frames(&s,prior+2,c.lock_hz);
    pthread_mutex_lock(&s.mutex);prior=s.captured_frames;pthread_mutex_unlock(&s.mutex);
    atomic_store(&video_standard,1);wait_frames(&s,prior+2,c.lock_hz);
    pthread_mutex_lock(&s.mutex);
    unsigned ntsc=0;
    for(unsigned i=0;i<FPV_SCAN_TRACKS;i++)if(s.tracks[i].bmp && s.tracks[i].standard==1) {
        ntsc++;assert(s.tracks[i].height==480);
    }
    assert(ntsc==1 && s.ddc_tunes==1);
    pthread_mutex_unlock(&s.mutex);
    c.lock_hz=5860000000ULL;
    assert(!fpv_service_start(&s,&c));wait_frames(&s,3,c.lock_hz);
    pthread_mutex_lock(&s.mutex);assert(s.ddc_tunes==1 && s.config.lock_hz==c.lock_hz);pthread_mutex_unlock(&s.mutex);
    c.mode=FPV_SCAN_MODE_SCAN;c.repeat=0;
    assert(!fpv_service_start(&s,&c));wait_idle(&s);
    pthread_mutex_lock(&s.mutex);assert(s.completed==s.total && s.config.mode==FPV_SCAN_MODE_SCAN);pthread_mutex_unlock(&s.mutex);
    c.mode=FPV_SCAN_MODE_LOCK;assert(!fpv_service_start(&s,&c));wait_frames(&s,2,c.lock_hz);
    fpv_service_stop(&s);wait_idle(&s);
    fpv_service_destroy(&s);
    puts("PASS worker black-frame acceptance, stale epoch/generation and lost-lock frame rejection, stop during settle");
}
