#include "fpv_spectrum_device.h"
#include <linux/fpv_spectrum.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <sys/ioctl.h>
#include <unistd.h>
int fpv_spectrum_device_read(int *fd,int simulate,uint64_t lo,uint32_t power[2048])
{
    if(simulate) {
        for(unsigned i=0;i<2048;i++) {
            double hz=(double)lo-30720000.0+i*30000.0;
            double d=(hz-5800000000.0)/3500000.0;
            double e=(hz-5860000000.0)/3500000.0;
            power[i]=(uint32_t)(50+500000*exp(-d*d)+350000*exp(-e*e));
        }
        return 0;
    }
    if(*fd<0) {
        *fd=open("/dev/fpv-spectrum",O_RDONLY|O_CLOEXEC);if(*fd<0)return -1;
        struct fpv_spectrum_info info;
        if(ioctl(*fd,FPV_SPECTRUM_INFO,&info)<0||info.version!=1||info.bins!=2048||info.sample_rate!=61440000||info.averages!=16) {
            close(*fd);*fd=-1;errno=EOPNOTSUPP;return -1;
        }
    }
    ssize_t n;do{n=read(*fd,power,FPV_SPECTRUM_BYTES);}while(n<0&&errno==EINTR);
    if(n!=(ssize_t)FPV_SPECTRUM_BYTES){if(n>=0)errno=EPROTO;return -1;}
    /* Public protocol is little-endian on both ARM and host. */
    for(unsigned i=0;i<2048;i++) {unsigned char *p=(unsigned char *)&power[i];power[i]=(uint32_t)p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24;}
    return 0;
}
