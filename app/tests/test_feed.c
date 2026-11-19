#include "fpv_feed.h"
#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static void put(struct fpv_feed *feed, uint64_t hz, unsigned char value)
{
    struct fpv_feed_meta meta = {.target_hz=hz,.frequency_hz=hz,.width=720,.height=576};
    assert(fpv_feed_publish(feed,&meta,&value,1)==0);
}
static void *waiting(void *p)
{
    struct fpv_feed *feed=p;struct fpv_feed_frame *f=NULL;uint64_t cursor=0,dropped=0;
    int result=fpv_feed_next(feed,&cursor,123,10000,&f,&dropped);
    assert(result==-1&&errno==ECANCELED&&f==NULL);return NULL;
}
int main(void)
{
    struct fpv_feed feed,other;assert(fpv_feed_init(&feed)==0);assert(fpv_feed_init(&other)==0);
    assert(strlen(feed.session_id)==32&&strcmp(feed.session_id,other.session_id));
    struct fpv_feed_frame *held=NULL,*frame=NULL;uint64_t cursor=0,dropped=0;
    assert(fpv_feed_next(&feed,&cursor,0,0,&frame,&dropped)==0);
    put(&feed,5800000000ULL,11);
    assert(fpv_feed_next(&feed,&cursor,0,0,&held,&dropped)==1);
    assert(cursor==1&&held->meta.frame_id==1&&held->data[0]==11&&dropped==0);
    for(unsigned i=0;i<FPV_FEED_CAPACITY+3;i++)put(&feed,i&1?5800000000ULL:5860000000ULL,(unsigned char)i);
    assert(held->data[0]==11); /* Retained client reference survives ring eviction. */
    assert(fpv_feed_next(&feed,&cursor,5860000000ULL,0,&frame,&dropped)==1);
    assert(dropped==3&&frame->meta.target_hz==5860000000ULL&&frame->meta.frame_id==6);
    fpv_feed_release(&feed,frame);fpv_feed_release(&feed,held);
    cursor=0;dropped=0;
    unsigned received=0;
    while(fpv_feed_next(&feed,&cursor,0,0,&frame,&dropped)==1){received++;fpv_feed_release(&feed,frame);}
    assert(received==FPV_FEED_CAPACITY&&cursor==FPV_FEED_CAPACITY+4&&dropped==4);
    assert(fpv_feed_next(&feed,&cursor,0,1,&frame,&dropped)==0);
    put(&feed,5800000000ULL,99);
    assert(fpv_feed_next(&feed,&cursor,0,0,&frame,&dropped)==1&&frame->data[0]==99);
    fpv_feed_release(&feed,frame);
    pthread_t thread;assert(pthread_create(&thread,NULL,waiting,&other)==0);
    usleep(10000);fpv_feed_close(&other);pthread_join(thread,NULL);
    assert(fpv_feed_publish(&other,&(struct fpv_feed_meta){0},"x",1)==-1&&errno==ECANCELED);
    fpv_feed_destroy(&other);fpv_feed_destroy(&feed);
    puts("PASS feed: order, filters, overflow, ref lifetime, timeout and close");return 0;
}
