/* Exercise ABI negotiation and byte order without an SDR. */
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdint.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <linux/fpv_spectrum.h>
static int mode,closed;
static int fake_open(const char *path,int flags,...){assert(!strcmp(path,"/dev/fpv-spectrum"));if(mode==1){errno=ENOENT;return -1;}return 42;}
static int fake_close(int fd){assert(fd==42);closed++;return 0;}
static int fake_ioctl(int fd,unsigned long req,...){assert(fd==42&&req==FPV_SPECTRUM_INFO);va_list ap;va_start(ap,req);struct fpv_spectrum_info *p=va_arg(ap,void *);va_end(ap);*p=(struct fpv_spectrum_info){mode==2?2:1,2048,61440000,16};return 0;}
static ssize_t fake_read(int fd,void *buf,size_t n){assert(fd==42&&n==8192);unsigned char *b=buf;for(unsigned i=0;i<2048;i++){b[4*i]=i;b[4*i+1]=i>>8;b[4*i+2]=0x12;b[4*i+3]=0x34;}if(mode==3)return 8191;if(mode==4){errno=ETIMEDOUT;return -1;}return n;}
#define open fake_open
#define close fake_close
#define ioctl fake_ioctl
#define read fake_read
#include "../spectrum/fpv_spectrum_device.c"
int main(void)
{
    uint32_t p[2048];int fd=-1;
    mode=1;assert(fpv_spectrum_device_read(&fd,0,5800000000ULL,p)<0&&fd==-1&&errno==ENOENT);
    mode=2;assert(fpv_spectrum_device_read(&fd,0,5800000000ULL,p)<0&&fd==-1&&closed==1);
    mode=0;assert(!fpv_spectrum_device_read(&fd,0,5800000000ULL,p));assert(p[1]==0x34120001&&p[2047]==0x341207ff);
    mode=3;assert(fpv_spectrum_device_read(&fd,0,5800000000ULL,p)<0&&errno==EPROTO);
    mode=4;assert(fpv_spectrum_device_read(&fd,0,5800000000ULL,p)<0&&errno==ETIMEDOUT);
    return 0;
}
