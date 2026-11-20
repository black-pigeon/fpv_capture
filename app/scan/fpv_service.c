#include "fpv_service.h"
#include <errno.h>
#include "fpv_spectrum_device.h"
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static uint64_t now_ms(void) { struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return (uint64_t)t.tv_sec*1000+t.tv_nsec/1000000; }
/* RF worker is the only capture owner: gain/LO cannot change during a read. */
static void spectrum_capture(struct fpv_service *s)
{
    uint64_t ms=now_ms();if(ms<s->spectrum_ms)return;
    uint32_t power[2048];int e=0;
    if(fpv_spectrum_device_read(&s->spectrum_fd,s->backend.simulate,s->backend.radio_lo_hz,power)<0)e=errno;
    pthread_mutex_lock(&s->mutex);
    if(e)fpv_spectrum_error(s->spectrum,e);
    else fpv_spectrum_add(s->spectrum,s->backend.radio_lo_hz,s->config.gain_db,s->backend.epoch,ms,power,2048);
    pthread_mutex_unlock(&s->mutex);s->spectrum_ms=ms+(e?1000:50);
}
char *fpv_service_spectrum(struct fpv_service *s)
{
    pthread_mutex_lock(&s->mutex);char *json=fpv_spectrum_json(s->spectrum,now_ms());
    pthread_mutex_unlock(&s->mutex);return json;
}
static int stopped(struct fpv_service *s)
{
    pthread_mutex_lock(&s->mutex);int v=s->stopping||s->quitting||s->reconfigure;pthread_mutex_unlock(&s->mutex);return v;
}
static int pause_ms(struct fpv_service *s,unsigned ms)
{
    uint64_t until=now_ms()+ms;
    while(now_ms()<until){if(stopped(s))return -1;struct timespec t={0,10000000};nanosleep(&t,NULL);}
    return stopped(s)?-1:0;
}
static void phase(struct fpv_service *s,const char *p)
{ pthread_mutex_lock(&s->mutex);snprintf(s->phase,sizeof(s->phase),"%s",p);pthread_mutex_unlock(&s->mutex); }
static void failure(struct fpv_service *s,const char *where)
{
    int e=errno;
    pthread_mutex_lock(&s->mutex);
    snprintf(s->error,sizeof(s->error),"%s: %s (%d)%s",where,strerror(e),e,e==EOPNOTSUPP?"; FPGA/driver v6 required":"");
    pthread_mutex_unlock(&s->mutex);
}
static void clear_locked(struct fpv_service *s)
{ for(unsigned i=0;i<FPV_SCAN_TRACKS;i++){free(s->tracks[i].bmp);memset(&s->tracks[i],0,sizeof(s->tracks[i]));} }
static void publish(struct fpv_service *s,const struct fpv_scan_hop *h,unsigned ch,unsigned std,unsigned height,
                    const unsigned char *gray,const struct fpv_capture_scan_status *status)
{
    size_t size;unsigned char *bmp=fpv_scan_bmp(gray,720,height,&size);if(!bmp)return;
    pthread_mutex_lock(&s->mutex);
    unsigned slot=0;uint64_t oldest=UINT64_MAX;
    for(unsigned i=0;i<FPV_SCAN_TRACKS;i++) {
        if(s->tracks[i].bmp && s->tracks[i].target_hz==h->target_hz[ch] && s->tracks[i].standard==std){slot=i;break;}
        if(!s->tracks[i].bmp){slot=i;oldest=0;}
        else if(oldest && s->tracks[i].captured_ms<oldest){oldest=s->tracks[i].captured_ms;slot=i;}
    }
    struct fpv_track *t=&s->tracks[slot];
    unsigned revision=t->revision+1;
    unsigned id=(t->bmp && t->target_hz==h->target_hz[ch] && t->standard==std)?t->id:++s->next_id;
    free(t->bmp);
    unsigned counts=status->quality[ch].counts,total=counts&65535,good=counts>>16;
    *t=(struct fpv_track){.id=id,.revision=revision,.channel=ch,.standard=std,.height=height,
        .quality=total?100*good/total:0,.frequency_hz=h->actual_hz[ch],.target_hz=h->target_hz[ch],
        .lo_hz=h->lo_hz,.captured_ms=now_ms(),.epoch=status->epoch,.active=1,.bmp=bmp,.size=size};
    struct fpv_feed_meta meta={.captured_mono_ms=t->captured_ms,.frequency_hz=t->frequency_hz,
        .target_hz=t->target_hz,.lo_hz=t->lo_hz,.channel=ch,.standard=std,.width=720,
        .height=height,.epoch=t->epoch,.quality=t->quality,.simulate=s->backend.simulate};
    /* A bounded copy publishes metadata and pixels together. No network I/O. */
    (void)fpv_feed_publish(&s->feed,&meta,bmp,size);
    s->captured_frames++;
    pthread_mutex_unlock(&s->mutex);
}
/* A tune epoch belongs to the entire fixed-frequency session, never a frame. */
static int tune_hop(struct fpv_service *s,const struct fpv_scan_config *cfg,
                    const struct fpv_scan_hop *h,unsigned point)
{
    struct fpv_backend *b=&s->backend;
    pthread_mutex_lock(&s->mutex);s->current=*h;s->completed=point;pthread_mutex_unlock(&s->mutex);
    phase(s,"tuning");
    if(fpv_backend_tune(b,h,cfg->gain_db)<0){failure(s,"RF tune");return -1;}
    pthread_mutex_lock(&s->mutex);s->ddc_tunes++;if(b->rf_changed)s->rf_tunes++;pthread_mutex_unlock(&s->mutex);
    phase(s,b->rf_changed?"rf_settle":"ddc_tuning");
    if(b->rf_changed && pause_ms(s,cfg->settle_ms)<0)return 1;
    if(stopped(s))return 1;
    if(fpv_backend_commit(b)<0){failure(s,"tune commit");return -1;}
    return 0;
}
/* Records retain their images. Only a fresh observation of the same search
 * frequency changes its live marker, including a PAL/NTSC change or lost sync.
 * Other scan windows remain unchanged until the worker revisits them. */
static void observe_tracks(struct fpv_service *s,const struct fpv_scan_hop *h,
                           const struct fpv_capture_scan_status *status)
{
    pthread_mutex_lock(&s->mutex);
    for(unsigned i=0;i<FPV_SCAN_TRACKS;i++) {
        struct fpv_track *t=&s->tracks[i];if(!t->bmp)continue;
        for(unsigned ch=0;ch<h->count;ch++)if(t->target_hz==h->target_hz[ch]) {
            t->active=fpv_scan_candidate(status,s->backend.epoch,ch) &&
                t->standard==(status->detect.channel[ch].state&1);
            break;
        }
    }
    pthread_mutex_unlock(&s->mutex);
}
static int capture_one(struct fpv_service *s,const struct fpv_scan_hop *h,unsigned ch,
                       const struct fpv_capture_scan_status *before)
{
    struct fpv_backend *b=&s->backend;
    if(!fpv_scan_candidate(before,b->epoch,ch))return 0;
    phase(s,"capturing");unsigned char *gray=NULL;unsigned std,height;
    if(fpv_backend_frame(b,ch,&std,&gray,&height)<0) {
        pthread_mutex_lock(&s->mutex);s->capture_errors++;pthread_mutex_unlock(&s->mutex);return 0;
    }
    struct fpv_capture_scan_status after;
    if(fpv_backend_status(b,&after)<0){free(gray);failure(s,"frame freshness");return -1;}
    observe_tracks(s,h,&after);
    if(!stopped(s) && fpv_scan_candidate(&after,b->epoch,ch) &&
        (before->detect.channel[ch].state>>8)==(after.detect.channel[ch].state>>8) &&
        std==(before->detect.channel[ch].state&1))publish(s,h,ch,std,height,gray,&after);
    free(gray);return 0;
}
/* Called only between captures, on the same worker that owns the RF device.
 * Newer requests may arrive during SPI access; acknowledge only this target. */
static int apply_gain(struct fpv_service *s,struct fpv_scan_config *cfg)
{
    pthread_mutex_lock(&s->mutex);
    int pending=s->gain_pending&&!s->stopping&&!s->quitting&&!s->reconfigure;
    unsigned gain=s->gain_target_db;
    pthread_mutex_unlock(&s->mutex);
    if(!pending)return 0;
    int changed=gain!=cfg->gain_db;
    if(changed) {
        phase(s,"gain_update");
        if(fpv_backend_set_gain(&s->backend,gain)<0){failure(s,"RX gain");return -1;}
    }
    pthread_mutex_lock(&s->mutex);
    cfg->gain_db=s->config.gain_db=gain;
    if(s->gain_target_db==gain)s->gain_pending=0;
    if(changed){s->gain_updates++;s->signal_present=0;
        for(unsigned i=0;i<FPV_SCAN_TRACKS;i++)s->tracks[i].active=0;
    }
    pthread_mutex_unlock(&s->mutex);
    return changed;
}
static int scan_pass(struct fpv_service *s,struct fpv_scan_config *cfg)
{
    struct fpv_backend *b=&s->backend;
    struct fpv_scan_hop h;
    if(fpv_scan_plan(cfg,0,&h)<0)return -1;
    phase(s,"initializing");
    if(fpv_backend_open(b,cfg,h.lo_hz)<0){failure(s,"RF/device init");return -1;}
    for(unsigned p=0;p<fpv_scan_points(cfg)&&!stopped(s);p+=h.count) {
        if(fpv_scan_plan(cfg,p,&h)<0)return -1;
        int tuned=tune_hop(s,cfg,&h,p);if(tuned)return tuned<0?-1:0;
        if(apply_gain(s,cfg)<0)return -1;
        spectrum_capture(s);
        phase(s,"observing");if(pause_ms(s,cfg->dwell_ms)<0)return 0;
        struct fpv_capture_scan_status before;
        if(fpv_backend_status(b,&before)<0){failure(s,"FPGA snapshot");return -1;}
        observe_tracks(s,&h,&before);
        for(unsigned ch=0;ch<h.count&&!stopped(s);ch++)if(capture_one(s,&h,ch,&before)<0)return -1;
        pthread_mutex_lock(&s->mutex);s->completed=p+h.count;pthread_mutex_unlock(&s->mutex);
    }
    return 0;
}
static int lock_pass(struct fpv_service *s,struct fpv_scan_config *cfg)
{
    struct fpv_backend *b=&s->backend;
    struct fpv_scan_hop h;
    if(fpv_scan_plan(cfg,0,&h)<0)return -1;
    phase(s,"initializing");
    if(fpv_backend_open(b,cfg,h.lo_hz)<0){failure(s,"RF/device init");return -1;}
    int tuned=tune_hop(s,cfg,&h,0);if(tuned)return tuned<0?-1:0;
    pthread_mutex_lock(&s->mutex);s->lock_ready=1;pthread_mutex_unlock(&s->mutex);
    /* Only initial acquisition waits the observation interval. Frames then
     * reuse the LO, NCO, detector and DMA configuration until user control. */
    phase(s,"observing");if(pause_ms(s,cfg->dwell_ms)<0)return 0;
    while(!stopped(s)) {
        uint64_t tick=now_ms();
        int gain=apply_gain(s,cfg);if(gain<0)return -1;
        if(gain){phase(s,"observing");if(pause_ms(s,cfg->dwell_ms)<0)break;}
        spectrum_capture(s);
        struct fpv_capture_scan_status before;
        if(fpv_backend_status(b,&before)<0){failure(s,"FPGA snapshot");return -1;}
        observe_tracks(s,&h,&before);
        int present=fpv_scan_candidate(&before,b->epoch,0);
        pthread_mutex_lock(&s->mutex);s->signal_present=present;pthread_mutex_unlock(&s->mutex);
        if(present) {if(capture_one(s,&h,0,&before)<0)return -1;}
        else phase(s,"waiting_signal");
        uint64_t elapsed=now_ms()-tick;
        if(pause_ms(s,elapsed<50?(unsigned)(50-elapsed):1)<0)break;
    }
    return 0;
}
static void configure_locked(struct fpv_service *s,const struct fpv_scan_config *c)
{
    if(fpv_spectrum_reset(s->spectrum,c)<0)fpv_spectrum_error(s->spectrum,errno);
    s->spectrum_ms=0;
    s->config=*c;s->completed=0;s->total=fpv_scan_points(c);s->rounds=0;s->capture_errors=0;
    s->rf_tunes=0;s->ddc_tunes=0;s->captured_frames=0;s->lock_ready=0;s->signal_present=0;
    s->gain_target_db=c->gain_db;s->gain_updates=0;s->gain_pending=0;
    memset(&s->current,0,sizeof(s->current));s->error[0]=0;
    clear_locked(s);s->stopping=0;s->running=1;strcpy(s->phase,"starting");
}
static void *worker(void *arg)
{
    struct fpv_service *s=arg;
    pthread_mutex_lock(&s->mutex);
    while(!s->quitting) {
        while(!s->running&&!s->quitting)pthread_cond_wait(&s->condition,&s->mutex);
        if(s->quitting)break;
        struct fpv_scan_config cfg=s->config;
        pthread_mutex_unlock(&s->mutex);
        int ret;
        do {
            ret=cfg.mode==FPV_SCAN_MODE_LOCK?lock_pass(s,&cfg):scan_pass(s,&cfg);
            pthread_mutex_lock(&s->mutex);if(!ret&&!s->stopping&&!s->reconfigure&&cfg.mode==FPV_SCAN_MODE_SCAN&&s->completed==s->total)s->rounds++;pthread_mutex_unlock(&s->mutex);
        } while(!ret&&cfg.repeat&&!stopped(s));
        if(ret)fpv_backend_close(&s->backend);
        pthread_mutex_lock(&s->mutex);
        if(s->reconfigure&&!s->stopping&&!s->quitting) {
            struct fpv_scan_config next=s->pending_config;
            s->reconfigure=0;configure_locked(s,&next);continue;
        }
        s->reconfigure=0;s->lock_ready=0;s->signal_present=0;
        s->gain_pending=0;s->gain_target_db=s->config.gain_db;
        s->running=0;s->stopping=0;snprintf(s->phase,sizeof(s->phase),"%s",ret?"error":"idle");
    }
    pthread_mutex_unlock(&s->mutex);fpv_backend_close(&s->backend);return NULL;
}
int fpv_service_init(struct fpv_service *s,int simulate)
{
    memset(s,0,sizeof(*s));s->spectrum_fd=-1;
    s->spectrum=fpv_spectrum_create();if(!s->spectrum)return ENOMEM;
    if(fpv_feed_init(&s->feed)<0){int e=errno;fpv_spectrum_destroy(s->spectrum);return e;}
    int rc=pthread_mutex_init(&s->mutex,NULL);
    if(rc){fpv_feed_destroy(&s->feed);fpv_spectrum_destroy(s->spectrum);return rc;}
    rc=pthread_cond_init(&s->condition,NULL);
    if(rc){pthread_mutex_destroy(&s->mutex);fpv_feed_destroy(&s->feed);fpv_spectrum_destroy(s->spectrum);return rc;}
    fpv_scan_defaults(&s->config);s->gain_target_db=s->config.gain_db;fpv_backend_init(&s->backend,simulate);strcpy(s->phase,"idle");
    rc=pthread_create(&s->worker,NULL,worker,s);
    if(rc){pthread_cond_destroy(&s->condition);pthread_mutex_destroy(&s->mutex);fpv_feed_destroy(&s->feed);fpv_spectrum_destroy(s->spectrum);}
    return rc;
}
int fpv_service_start(struct fpv_service *s,const struct fpv_scan_config *c)
{
    pthread_mutex_lock(&s->mutex);
    if(s->running) {
        if(s->stopping||s->reconfigure||(c->mode==FPV_SCAN_MODE_SCAN&&s->config.mode==FPV_SCAN_MODE_SCAN)) {
            pthread_mutex_unlock(&s->mutex);return -1;
        }
        s->pending_config=*c;s->reconfigure=1;
        pthread_mutex_unlock(&s->mutex);return 0;
    }
    configure_locked(s,c);
    pthread_cond_signal(&s->condition);pthread_mutex_unlock(&s->mutex);return 0;
}
int fpv_service_set_gain(struct fpv_service *s,unsigned gain)
{
    if(gain>60){errno=EINVAL;return -1;}
    pthread_mutex_lock(&s->mutex);
    if(s->stopping||s->reconfigure||s->quitting) {
        pthread_mutex_unlock(&s->mutex);errno=EBUSY;return -1;
    }
    s->gain_target_db=gain;
    if(s->running)s->gain_pending=1;
    else {s->config.gain_db=gain;s->gain_pending=0;}
    pthread_mutex_unlock(&s->mutex);return 0;
}
void fpv_service_stop(struct fpv_service *s) { pthread_mutex_lock(&s->mutex);if(s->running){s->stopping=1;s->reconfigure=0;}pthread_mutex_unlock(&s->mutex); }
int fpv_service_clear(struct fpv_service *s)
{
    pthread_mutex_lock(&s->mutex);
    if(s->running){pthread_mutex_unlock(&s->mutex);return -1;}
    clear_locked(s);pthread_mutex_unlock(&s->mutex);return 0;
}
void fpv_service_destroy(struct fpv_service *s)
{
    pthread_mutex_lock(&s->mutex);s->quitting=1;pthread_cond_signal(&s->condition);pthread_mutex_unlock(&s->mutex);
    pthread_join(s->worker,NULL);if(s->spectrum_fd>=0)close(s->spectrum_fd);fpv_spectrum_destroy(s->spectrum);clear_locked(s);pthread_cond_destroy(&s->condition);pthread_mutex_destroy(&s->mutex);fpv_feed_destroy(&s->feed);
}
/* Escape all backend text; never interpolate arbitrary strings into JSON. */
static void json_string(FILE *f,const char *v)
{
    fputc('"',f);for(const unsigned char *p=(const unsigned char *)v;*p;p++) {
        if(*p=='"'||*p=='\\'){fputc('\\',f);fputc(*p,f);}
        else if(*p<32)fprintf(f,"\\u%04x",*p);else fputc(*p,f);
    }fputc('"',f);
}
char *fpv_service_status(struct fpv_service *s)
{
    char *out=NULL;size_t size;FILE *f=open_memstream(&out,&size);if(!f)return NULL;
    pthread_mutex_lock(&s->mutex);
    fprintf(f,"{\"simulate\":%s,\"running\":%s,\"stopping\":%s,\"phase\":",s->backend.simulate?"true":"false",s->running?"true":"false",s->stopping?"true":"false");
    json_string(f,s->phase);
    const struct fpv_board *board=fpv_board_current();
    fprintf(f,",\"board\":\"%s\",\"rx_port\":",board->name);
    int port=fpv_board_rx_port(board,s->current.lo_hz);
    if(port<0)fputs("null",f);else fprintf(f,"\"%s_BALANCED\"",port?"B":"A");
    fprintf(f,",\"mode\":\"%s\",\"switching\":%s,\"frequency_locked\":%s,\"signal_present\":%s,\"captured_frames\":%u,\"lock_hz\":%llu",
        s->config.mode==FPV_SCAN_MODE_LOCK?"lock":"scan",s->reconfigure?"true":"false",
        s->running&&s->lock_ready?"true":"false",s->running&&s->signal_present?"true":"false",
        s->captured_frames,(unsigned long long)s->config.lock_hz);
    fprintf(f,",\"gain_pending\":%s,\"gain_target_db\":%u,\"gain_updates\":%u",
        s->gain_pending?"true":"false",s->gain_target_db,s->gain_updates);
    fprintf(f,",\"error\":");json_string(f,s->error);
    fprintf(f,",\"completed_points\":%u,\"total_points\":%u,\"rounds\":%u,\"capture_errors\":%u,\"lo_hz\":%llu",s->completed,s->total,s->rounds,s->capture_errors,(unsigned long long)s->current.lo_hz);
    fprintf(f,",\"rf_tunes\":%u,\"ddc_tunes\":%u,\"window_phase\":%u,\"window_phases\":%u,\"center_span_hz\":%u,\"channels\":[",
        s->rf_tunes,s->ddc_tunes,s->current.phase+!!s->current.count,s->current.phases,FPV_SCAN_CENTER_SPAN_HZ);
    for(unsigned ch=0;ch<s->current.count;ch++)
        fprintf(f,"%s{\"channel\":%u,\"target_hz\":%llu,\"frequency_hz\":%llu,\"offset_hz\":%lld}",ch?",":"",ch,
            (unsigned long long)s->current.target_hz[ch],(unsigned long long)s->current.actual_hz[ch],
            (long long)s->current.actual_hz[ch]-(long long)s->current.lo_hz);
    fprintf(f,"],\"config\":{\"mode\":\"%s\",\"frequency_mhz\":%.6f,\"bands\":[",
        s->config.mode==FPV_SCAN_MODE_LOCK?"lock":"scan",s->config.lock_hz/1e6);
    for(unsigned b=0;b<s->config.band_count;b++)fprintf(f,"%s[%llu,%llu]",b?",":"",(unsigned long long)s->config.bands[b].start_hz,(unsigned long long)s->config.bands[b].end_hz);
    fprintf(f,"],\"step_mhz\":%.6f,\"dwell_ms\":%u,\"settle_ms\":%u,\"gain_db\":%u,\"repeat\":%u},\"tracks\":[",s->config.step_hz/1e6,s->config.dwell_ms,s->config.settle_ms,s->config.gain_db,s->config.repeat);
    unsigned count=0;uint64_t now=now_ms();
    for(unsigned i=0;i<FPV_SCAN_TRACKS;i++) {
        const struct fpv_track *t=&s->tracks[i];if(!t->bmp)continue;
        fprintf(f,"%s{\"id\":%u,\"revision\":%u,\"frequency_hz\":%llu,\"target_hz\":%llu,\"lo_hz\":%llu,\"channel\":%u,\"standard\":\"%s\",\"epoch\":%u,\"sync_quality\":%u,\"age_ms\":%llu,\"active\":%s,\"width\":720,\"height\":%u}",count++?",":"",t->id,t->revision,(unsigned long long)t->frequency_hz,(unsigned long long)t->target_hz,(unsigned long long)t->lo_hz,t->channel,t->standard?"NTSC":"PAL",t->epoch,t->quality,(unsigned long long)(now-t->captured_ms),
            s->running&&!s->stopping&&!s->reconfigure&&!s->error[0]&&t->active?"true":"false",t->height);
    }
    fputs("]}",f);pthread_mutex_unlock(&s->mutex);fclose(f);return out;
}
unsigned char *fpv_service_image(struct fpv_service *s,unsigned id,size_t *size)
{
    unsigned char *p=NULL;pthread_mutex_lock(&s->mutex);
    for(unsigned i=0;i<FPV_SCAN_TRACKS;i++)if(s->tracks[i].bmp&&s->tracks[i].id==id) {
        *size=s->tracks[i].size;p=malloc(*size);if(p)memcpy(p,s->tracks[i].bmp,*size);break;
    }
    pthread_mutex_unlock(&s->mutex);return p;
}
