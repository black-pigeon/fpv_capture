#include "fpv_feed.h"
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/random.h>
#include <time.h>

static void release_locked(struct fpv_feed_frame *frame)
{
    if(frame && --frame->references==0)free(frame);
}
int fpv_feed_init(struct fpv_feed *feed)
{
    memset(feed,0,sizeof(*feed));
    unsigned char random[16];size_t offset=0;
    while(offset<sizeof(random)) {
        ssize_t n=getrandom(random+offset,sizeof(random)-offset,0);
        if(n<0 && errno==EINTR)continue;
        if(n<=0){if(!n)errno=EIO;return -1;}
        offset+=(size_t)n;
    }
    static const char hex[]="0123456789abcdef";
    for(unsigned i=0;i<sizeof(random);i++) {
        feed->session_id[2*i]=hex[random[i]>>4];feed->session_id[2*i+1]=hex[random[i]&15];
    }
    int rc=pthread_mutex_init(&feed->mutex,NULL);
    if(rc){errno=rc;return -1;}
    pthread_condattr_t attr;
    rc=pthread_condattr_init(&attr);
    if(rc){pthread_mutex_destroy(&feed->mutex);errno=rc;return -1;}
    rc=pthread_condattr_setclock(&attr,CLOCK_MONOTONIC);
    if(!rc)rc=pthread_cond_init(&feed->changed,&attr);
    pthread_condattr_destroy(&attr);
    if(rc){pthread_mutex_destroy(&feed->mutex);errno=rc;return -1;}
    return 0;
}
void fpv_feed_close(struct fpv_feed *feed)
{
    pthread_mutex_lock(&feed->mutex);feed->closed=1;pthread_cond_broadcast(&feed->changed);pthread_mutex_unlock(&feed->mutex);
}
void fpv_feed_destroy(struct fpv_feed *feed)
{
    fpv_feed_close(feed);
    for(unsigned i=0;i<FPV_FEED_CAPACITY;i++)release_locked(feed->ring[i]);
    pthread_cond_destroy(&feed->changed);pthread_mutex_destroy(&feed->mutex);
}
uint64_t fpv_feed_cursor(struct fpv_feed *feed)
{
    pthread_mutex_lock(&feed->mutex);uint64_t seq=feed->sequence;pthread_mutex_unlock(&feed->mutex);return seq;
}
int fpv_feed_publish(struct fpv_feed *feed,const struct fpv_feed_meta *meta,const void *bmp,size_t size)
{
    if(!size || size>FPV_FEED_MAX_BYTES || !bmp || !meta){errno=EINVAL;return -1;}
    struct fpv_feed_frame *frame=malloc(sizeof(*frame)+size);
    if(!frame)return -1;
    frame->meta=*meta;frame->size=size;frame->references=1;memcpy(frame->data,bmp,size);
    pthread_mutex_lock(&feed->mutex);
    if(feed->closed || feed->sequence==UINT64_MAX) {
        pthread_mutex_unlock(&feed->mutex);free(frame);errno=ECANCELED;return -1;
    }
    frame->meta.frame_id=++feed->sequence;
    unsigned slot=(unsigned)((feed->sequence-1)%FPV_FEED_CAPACITY);
    release_locked(feed->ring[slot]);feed->ring[slot]=frame;
    pthread_cond_broadcast(&feed->changed);pthread_mutex_unlock(&feed->mutex);return 0;
}
int fpv_feed_next(struct fpv_feed *feed,uint64_t *cursor,uint64_t target,int timeout_ms,
                  struct fpv_feed_frame **frame,uint64_t *dropped)
{
    *frame=NULL;
    struct timespec deadline;clock_gettime(CLOCK_MONOTONIC,&deadline);
    if(timeout_ms<0){errno=EINVAL;return -1;}
    deadline.tv_sec+=timeout_ms/1000;deadline.tv_nsec+=(long)(timeout_ms%1000)*1000000;
    if(deadline.tv_nsec>=1000000000){deadline.tv_sec++;deadline.tv_nsec-=1000000000;}
    pthread_mutex_lock(&feed->mutex);
    for(;;) {
        if(feed->closed){pthread_mutex_unlock(&feed->mutex);errno=ECANCELED;return -1;}
        uint64_t oldest=feed->sequence>FPV_FEED_CAPACITY?feed->sequence-FPV_FEED_CAPACITY+1:1;
        if(*cursor<oldest-1){*dropped+=oldest-1-*cursor;*cursor=oldest-1;}
        while(*cursor<feed->sequence) {
            struct fpv_feed_frame *next=feed->ring[(size_t)(*cursor%FPV_FEED_CAPACITY)];
            (*cursor)++;
            if(target && target!=next->meta.target_hz)continue;
            next->references++;*frame=next;pthread_mutex_unlock(&feed->mutex);return 1;
        }
        if(!timeout_ms){pthread_mutex_unlock(&feed->mutex);return 0;}
        int rc=pthread_cond_timedwait(&feed->changed,&feed->mutex,&deadline);
        if(rc){pthread_mutex_unlock(&feed->mutex);if(rc==ETIMEDOUT)return 0;errno=rc;return -1;}
    }
}
void fpv_feed_release(struct fpv_feed *feed,struct fpv_feed_frame *frame)
{
    pthread_mutex_lock(&feed->mutex);release_locked(frame);pthread_mutex_unlock(&feed->mutex);
}
