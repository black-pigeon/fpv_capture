#include "fpv_stream.h"
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

static int send_all(int fd,const void *data,size_t size)
{
    const unsigned char *p=data;
    while(size) {
        ssize_t n=send(fd,p,size,MSG_NOSIGNAL);
        if(n<0 && errno==EINTR)continue;
        if(n<=0)return -1;
        p+=n;size-=(size_t)n;
    }
    return 0;
}
static int part(int fd,const char *type,const char *metadata,const void *body,size_t size)
{
    char header[2048];
    int n=snprintf(header,sizeof(header),"--fpv-stream-v1\r\nContent-Type: %s\r\nContent-Length: %zu\r\n%s%s%s\r\n",
        type,size,metadata?"X-FPV-Metadata: ":"",metadata?metadata:"",metadata?"\r\n":"");
    if(n<0 || (size_t)n>=sizeof(header))return -1;
    if(send_all(fd,header,(size_t)n)<0 || send_all(fd,body,size)<0)return -1;
    return send_all(fd,"\r\n",2);
}
static int json_part(int fd,const char *json)
{
    return part(fd,"application/json",NULL,json,strlen(json));
}
static int frame_part(int fd,struct fpv_feed *feed,const struct fpv_feed_frame *frame,uint64_t dropped)
{
    const struct fpv_feed_meta *m=&frame->meta;char meta[1024];
    int n=snprintf(meta,sizeof(meta),
        "{\"api_version\":1,\"type\":\"frame\",\"session_id\":\"%s\",\"frame_id\":%"PRIu64
        ",\"target_hz\":%"PRIu64",\"frequency_hz\":%"PRIu64",\"lo_hz\":%"PRIu64
        ",\"channel\":%u,\"standard\":\"%s\",\"width\":%u,\"height\":%u,\"captured_mono_ms\":%"PRIu64
        ",\"epoch\":%u,\"sync_quality\":%u,\"simulate\":%s,\"dropped_before\":%"PRIu64"}",
        feed->session_id,m->frame_id,m->target_hz,m->frequency_hz,m->lo_hz,m->channel,
        m->standard?"NTSC":"PAL",m->width,m->height,m->captured_mono_ms,m->epoch,m->quality,
        m->simulate?"true":"false",dropped);
    if(n<0 || (size_t)n>=sizeof(meta))return -1;
    return part(fd,"image/bmp",meta,frame->data,frame->size);
}
static void serve(struct fpv_stream_client *client)
{
    struct fpv_feed *feed=client->server->feed;int fd=client->fd;
    struct timeval timeout={2,0};setsockopt(fd,SOL_SOCKET,SO_SNDTIMEO,&timeout,sizeof(timeout));
    static const char header[]="HTTP/1.1 200 OK\r\nContent-Type: multipart/mixed; boundary=fpv-stream-v1\r\nConnection: close\r\nCache-Control: no-store\r\nX-Content-Type-Options: nosniff\r\nX-Accel-Buffering: no\r\n\r\n";
    if(send_all(fd,header,sizeof(header)-1)<0)return;
    uint64_t latest=fpv_feed_cursor(feed),cursor=latest,dropped=0;
    const char *resume="live";
    if(client->resume) {
        if(strcmp(client->session_id,feed->session_id))resume="session_reset";
        else if(client->after>latest)resume="cursor_ahead";
        else {cursor=client->after;resume="resumed";}
    }
    char json[512];
    snprintf(json,sizeof(json),"{\"api_version\":1,\"type\":\"hello\",\"session_id\":\"%s\",\"latest_frame_id\":%"PRIu64
        ",\"cursor\":%"PRIu64",\"resume\":\"%s\",\"history_frames\":%u,\"target_hz\":%"PRIu64"}",
        feed->session_id,latest,cursor,resume,FPV_FEED_CAPACITY,client->target_hz);
    if(json_part(fd,json)<0)return;
    for(;;) {
        struct fpv_feed_frame *frame=NULL;
        int result=fpv_feed_next(feed,&cursor,client->target_hz,1000,&frame,&dropped);
        if(result<0)return;
        if(result>0) {
            int sent=frame_part(fd,feed,frame,dropped);
            fpv_feed_release(feed,frame);if(sent<0)return;dropped=0;
        } else {
            snprintf(json,sizeof(json),"{\"api_version\":1,\"type\":\"heartbeat\",\"session_id\":\"%s\",\"cursor\":%"PRIu64
                ",\"latest_frame_id\":%"PRIu64",\"dropped_before\":%"PRIu64"}",feed->session_id,cursor,fpv_feed_cursor(feed),dropped);
            if(json_part(fd,json)<0)return;
            dropped=0;
        }
    }
}
static void *worker(void *arg)
{
    struct fpv_stream_client *client=arg;struct fpv_stream_server *s=client->server;
    pthread_mutex_lock(&s->mutex);
    while(!s->stopping) {
        while(client->fd<0 && !s->stopping)pthread_cond_wait(&s->changed,&s->mutex);
        if(s->stopping)break;
        pthread_mutex_unlock(&s->mutex);serve(client);pthread_mutex_lock(&s->mutex);
        close(client->fd);client->fd=-1;
    }
    if(client->fd>=0){close(client->fd);client->fd=-1;}
    pthread_mutex_unlock(&s->mutex);return NULL;
}
int fpv_stream_init(struct fpv_stream_server *s,struct fpv_feed *feed)
{
    memset(s,0,sizeof(*s));s->feed=feed;
    int rc=pthread_mutex_init(&s->mutex,NULL);if(rc){errno=rc;return -1;}
    rc=pthread_cond_init(&s->changed,NULL);if(rc){pthread_mutex_destroy(&s->mutex);errno=rc;return -1;}
    for(unsigned i=0;i<FPV_STREAM_CLIENTS;i++){s->clients[i].server=s;s->clients[i].fd=-1;}
    pthread_attr_t attr;rc=pthread_attr_init(&attr);
    if(rc){pthread_cond_destroy(&s->changed);pthread_mutex_destroy(&s->mutex);errno=rc;return -1;}
    rc=pthread_attr_setstacksize(&attr,256*1024);
    unsigned made=0;
    while(!rc && made<FPV_STREAM_CLIENTS) {
        rc=pthread_create(&s->clients[made].thread,&attr,worker,&s->clients[made]);
        if(!rc)made++;
    }
    pthread_attr_destroy(&attr);
    if(!rc)return 0;
    pthread_mutex_lock(&s->mutex);s->stopping=1;pthread_cond_broadcast(&s->changed);pthread_mutex_unlock(&s->mutex);
    for(unsigned i=0;i<made;i++)pthread_join(s->clients[i].thread,NULL);
    pthread_cond_destroy(&s->changed);pthread_mutex_destroy(&s->mutex);errno=rc;return -1;
}
void fpv_stream_destroy(struct fpv_stream_server *s)
{
    pthread_mutex_lock(&s->mutex);s->stopping=1;
    for(unsigned i=0;i<FPV_STREAM_CLIENTS;i++)if(s->clients[i].fd>=0)shutdown(s->clients[i].fd,SHUT_RDWR);
    pthread_cond_broadcast(&s->changed);pthread_mutex_unlock(&s->mutex);
    fpv_feed_close(s->feed);
    for(unsigned i=0;i<FPV_STREAM_CLIENTS;i++)pthread_join(s->clients[i].thread,NULL);
    pthread_cond_destroy(&s->changed);pthread_mutex_destroy(&s->mutex);
}
static int integer(const char *text,uint64_t *value)
{
    if(!*text || strspn(text,"0123456789")!=strlen(text))return -1;
    errno=0;char *end;unsigned long long n=strtoull(text,&end,10);
    if(errno || *end)return -1;
    *value=(uint64_t)n;return 0;
}
int fpv_stream_open(struct fpv_stream_server *s,int fd,const char *query)
{
    uint64_t target=0,after=0;char session[33]={0},buffer[256];unsigned seen=0;
    if(strlen(query)>=sizeof(buffer)){errno=EINVAL;return -1;}
    strcpy(buffer,query);
    if(*buffer) {
        if(buffer[0]!='?' || !buffer[1]){errno=EINVAL;return -1;}
        char *save=NULL;
        for(char *p=strtok_r(buffer+1,"&",&save);p;p=strtok_r(NULL,"&",&save)) {
            char *v=strchr(p,'=');unsigned bit;
            if(!v)goto invalid;
            *v++=0;
            if(!strcmp(p,"target_hz")){bit=1;if(integer(v,&target)<0 || !target)goto invalid;}
            else if(!strcmp(p,"after")){bit=2;if(integer(v,&after)<0)goto invalid;}
            else if(!strcmp(p,"session_id")){bit=4;if(strlen(v)!=32 || strspn(v,"0123456789abcdef")!=32)goto invalid;strcpy(session,v);}
            else goto invalid;
            if(seen&bit)goto invalid;
            seen|=bit;
        }
        if(!!(seen&2)!=!!(seen&4))goto invalid;
    }
    pthread_mutex_lock(&s->mutex);
    for(unsigned i=0;i<FPV_STREAM_CLIENTS && !s->stopping;i++)if(s->clients[i].fd<0) {
        int copy=fcntl(fd,F_DUPFD_CLOEXEC,0);if(copy<0){pthread_mutex_unlock(&s->mutex);return -1;}
        struct fpv_stream_client *client=&s->clients[i];
        client->target_hz=target;client->after=after;client->resume=!!(seen&2);strcpy(client->session_id,session);client->fd=copy;
        pthread_cond_broadcast(&s->changed);pthread_mutex_unlock(&s->mutex);return 0;
    }
    pthread_mutex_unlock(&s->mutex);errno=EBUSY;return -1;
invalid:
    errno=EINVAL;return -1;
}
