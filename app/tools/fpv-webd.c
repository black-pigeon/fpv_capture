/* Offline HTTP interface; hardware ownership is confined to the scan worker. */
#include "fpv_service.h"
#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <getopt.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>

static volatile sig_atomic_t quitting;
static void stop_signal(int n) { (void)n;quitting=1; }
static int send_all(int fd,const void *buf,size_t size)
{
    const char *p=buf;
    while(size){ssize_t n=send(fd,p,size,MSG_NOSIGNAL);if(n<0&&errno==EINTR)continue;if(n<=0)return -1;p+=n;size-=(size_t)n;}return 0;
}
static void response(int fd,int status,const char *kind,const void *body,size_t size)
{
    char header[640];
    int n=snprintf(header,sizeof(header),"HTTP/1.1 %d %s\r\nContent-Type: %s\r\nContent-Length: %zu\r\nConnection: close\r\nCache-Control: no-store\r\nX-Content-Type-Options: nosniff\r\nContent-Security-Policy: default-src 'self'; img-src 'self'; style-src 'self'; script-src 'self'; frame-ancestors 'none'\r\n\r\n",status,status==200?"OK":"Error",kind,size);
    if(n>0&&(size_t)n<sizeof(header)&&send_all(fd,header,(size_t)n)==0)send_all(fd,body,size);
}
static void message(int fd,int status,const char *text) { response(fd,status,"text/plain; charset=utf-8",text,strlen(text)); }
static void asset(int fd,const char *root,const char *name,const char *type)
{
    char path[1024];int n=snprintf(path,sizeof(path),"%s/%s",root,name);
    if(n<0||(size_t)n>=sizeof(path)){message(fd,500,"Asset path too long");return;}
    FILE *f=fopen(path,"rb");if(!f){message(fd,404,"Web asset missing");return;}
    if(fseek(f,0,SEEK_END)){fclose(f);message(fd,500,"Asset read failed");return;}
    long size=ftell(f);rewind(f);
    if(size<0||size>262144){fclose(f);message(fd,500,"Asset too large");return;}
    char *buf=malloc((size_t)size+1);
    if(!buf){fclose(f);message(fd,500,"Out of memory");return;}
    size_t got=fread(buf,1,(size_t)size,f);fclose(f);
    if(got==(size_t)size)response(fd,200,type,buf,got);else message(fd,500,"Asset read failed");
    free(buf);
}
static void client(int fd,struct fpv_service *s,const char *root)
{
    char req[10240];size_t used=0,header_len=0,body_len=0;
    struct timeval timeout={1,0};setsockopt(fd,SOL_SOCKET,SO_RCVTIMEO,&timeout,sizeof(timeout));setsockopt(fd,SOL_SOCKET,SO_SNDTIMEO,&timeout,sizeof(timeout));
    while(!header_len) {
        if(used>=8192){message(fd,413,"Headers too large");return;}
        ssize_t n=recv(fd,req+used,8192-used,0);
        if(n<=0)return;
        if(memchr(req+used,0,(size_t)n)){message(fd,400,"NUL in request");return;}
        used+=(size_t)n;req[used]=0;
        char *end=strstr(req,"\r\n\r\n");if(end)header_len=(size_t)(end-req)+4;
    }
    char method[8],path[256],version[16],tail;
    char *line=strstr(req,"\r\n");if(!line){message(fd,400,"Invalid request");return;}*line=0;
    if(sscanf(req,"%7s %255s %15s %c",method,path,version,&tail)!=3 || (strcmp(version,"HTTP/1.1")&&strcmp(version,"HTTP/1.0"))){message(fd,400,"Invalid request line");return;}
    char host[256]="",origin[300]="",content_type[128]="";unsigned seen_length=0,seen_host=0,seen_origin=0;
    char *p=line+2;
    while(p<req+header_len-2) {
        char *end=strstr(p,"\r\n");if(!end){message(fd,400,"Invalid header");return;}*end=0;
        char *v=strchr(p,':');if(!v){message(fd,400,"Invalid header");return;}*v++=0;while(*v==' '||*v=='\t')v++;
        if(!strcasecmp(p,"Content-Length")) {
            char *numend;errno=0;unsigned long n=strtoul(v,&numend,10);
            if(seen_length++||errno||numend==v||*numend||n>2047){message(fd,400,"Invalid Content-Length");return;}body_len=n;
        } else if(!strcasecmp(p,"Transfer-Encoding")){message(fd,400,"Transfer-Encoding unsupported");return;}
        else if(!strcasecmp(p,"Host")){if(seen_host++||strlen(v)>=sizeof(host)){message(fd,400,"Invalid Host");return;}strcpy(host,v);}
        else if(!strcasecmp(p,"Origin")){if(seen_origin++||strlen(v)>=sizeof(origin)){message(fd,400,"Invalid Origin");return;}strcpy(origin,v);}
        else if(!strcasecmp(p,"Content-Type")){if(strlen(v)>=sizeof(content_type)){message(fd,400,"Invalid Content-Type");return;}strcpy(content_type,v);}
        p=end+2;
    }
    if(!strcmp(version,"HTTP/1.1")&&!host[0]){message(fd,400,"Host required");return;}
    while(used<header_len+body_len) {
        ssize_t n=recv(fd,req+used,header_len+body_len-used,0);if(n<=0)return;used+=(size_t)n;
    }
    if(memchr(req+header_len,0,body_len)){message(fd,400,"NUL in body");return;}
    req[header_len+body_len]=0;
    /* A browser request may mutate RF state only from this same origin. */
    if(!strcmp(method,"POST")) {
        char expected[300];snprintf(expected,sizeof(expected),"http://%s",host);
        if(origin[0]&&strcmp(origin,expected)){message(fd,403,"Cross-origin control denied");return;}
        if(body_len && strcmp(content_type,"application/x-www-form-urlencoded")){message(fd,415,"Use application/x-www-form-urlencoded");return;}
        if(!strcmp(path,"/api/start")) {
            struct fpv_scan_config cfg;char error[256];
            if(fpv_scan_config_parse(req+header_len,&cfg,error,sizeof(error))<0){message(fd,400,error);return;}
            if(fpv_service_start(s,&cfg)<0){message(fd,409,"Acquisition is busy. Wait for switching/stopping, or stop before starting another scan.");return;}
        } else if(!strcmp(path,"/api/stop"))fpv_service_stop(s);
        else if(!strcmp(path,"/api/clear")){if(fpv_service_clear(s)<0){message(fd,409,"Stop the scan first.");return;}}
        else {message(fd,404,"Not found");return;}
        message(fd,200,"OK");return;
    }
    if(strcmp(method,"GET")){message(fd,405,"Method not allowed");return;}
    if(!strcmp(path,"/api/status")) {
        char *json=fpv_service_status(s);if(!json){message(fd,500,"Out of memory");return;}
        response(fd,200,"application/json; charset=utf-8",json,strlen(json));free(json);return;
    }
    unsigned id;int consumed=0;
    if(sscanf(path,"/image/%u.bmp%n",&id,&consumed)==1&&consumed>0&&(path[consumed]==0||path[consumed]=='?')) {
        size_t size=0;unsigned char *bmp=fpv_service_image(s,id,&size);
        if(!bmp){message(fd,404,"Frame expired");return;}response(fd,200,"image/bmp",bmp,size);free(bmp);return;
    }
    if(!strcmp(path,"/")||!strcmp(path,"/index.html"))asset(fd,root,"index.html","text/html; charset=utf-8");
    else if(!strcmp(path,"/style.css"))asset(fd,root,"style.css","text/css; charset=utf-8");
    else if(!strcmp(path,"/app.js"))asset(fd,root,"app.js","text/javascript; charset=utf-8");
    else message(fd,404,"Not found");
}
static void usage(const char *name)
{
    printf("Usage: %s [--listen IPv4] [--port 80] [--web-root /usr/share/e200-fpv/web] [--simulate]\n",name);
    puts("Starts the web UI only. RF initialization begins when Start is pressed.\n--simulate is host testing only and clearly labels generated results.");
}
int main(int argc,char **argv)
{
    const char *listen_ip="0.0.0.0",*root="/usr/share/e200-fpv/web";unsigned port=80;int simulate=0,opt;
    static const struct option options[]={{"listen",required_argument,NULL,'l'},{"port",required_argument,NULL,'p'},{"web-root",required_argument,NULL,'w'},{"simulate",no_argument,NULL,'s'},{"help",no_argument,NULL,'h'},{NULL,0,NULL,0}};
    while((opt=getopt_long(argc,argv,"l:p:w:sh",options,NULL))!=-1) {
        if(opt=='l')listen_ip=optarg;
        else if(opt=='w')root=optarg;
        else if(opt=='s')simulate=1;
        else if(opt=='p'){char *end;unsigned long n=strtoul(optarg,&end,10);if(end==optarg||*end||n<1||n>65535){usage(argv[0]);return 2;}port=(unsigned)n;}
        else {usage(argv[0]);return opt=='h'?0:2;}
    }
    if(optind!=argc){usage(argv[0]);return 2;}
    struct sockaddr_in address={.sin_family=AF_INET,.sin_port=htons((uint16_t)port)};
    if(inet_pton(AF_INET,listen_ip,&address.sin_addr)!=1){fputs("Invalid IPv4 address\n",stderr);return 2;}
    int fd=socket(AF_INET,SOCK_STREAM|SOCK_CLOEXEC,0);if(fd<0){perror("socket");return 1;}
    int reuse=1;setsockopt(fd,SOL_SOCKET,SO_REUSEADDR,&reuse,sizeof(reuse));
    if(bind(fd,(struct sockaddr *)&address,sizeof(address))<0||listen(fd,8)<0){perror("HTTP listen");close(fd);return 1;}
    struct fpv_service service;
    if(fpv_service_init(&service,simulate)){fputs("Scan worker failed\n",stderr);close(fd);return 1;}
    struct sigaction action={0};action.sa_handler=stop_signal;sigemptyset(&action.sa_mask);sigaction(SIGINT,&action,NULL);sigaction(SIGTERM,&action,NULL);
    printf("E200 FPV web listening on %s:%u (%s)\n",listen_ip,port,simulate?"SIMULATION":"RF hardware");fflush(stdout);
    while(!quitting) {
        fd_set set;FD_ZERO(&set);FD_SET(fd,&set);struct timeval timeout={0,250000};
        int ret=select(fd+1,&set,NULL,NULL,&timeout);
        if(ret<0){if(errno==EINTR)continue;perror("select");break;}
        if(!ret)continue;
        int peer=accept4(fd,NULL,NULL,SOCK_CLOEXEC);if(peer<0)continue;
        client(peer,&service,root);close(peer);
    }
    close(fd);fpv_service_destroy(&service);return 0;
}
