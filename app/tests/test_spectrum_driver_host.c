#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <errno.h>
#include <string.h>
#include <sys/types.h>
#include <linux/fpv_spectrum.h>
typedef uint32_t u32;typedef uintptr_t dma_addr_t;typedef int dma_cookie_t;
struct miscdevice {int x;};struct mutex {int held;};struct kref{int x;};
struct completion{int x;};struct dma_chan{int x;};struct device{int x;};
struct file{void *private_data;};
struct dma_async_tx_descriptor{void (*callback)(void *);void *callback_param;};
#define __user
#define __iomem
#define ERESTARTSYS 512
#define DMA_DEV_TO_MEM 0
#define DMA_PREP_INTERRUPT 1
#define DMA_CTRL_ACK 2
#define DMA_COMPLETE 0
#define msecs_to_jiffies(x) (x)
#define dma_rmb() ((void)0)
static uint32_t regs[8];static struct dma_async_tx_descriptor tx;
static int issued,started,terminated,aborted,mode;
static u32 readl(void *p){return *(u32 *)p;}
static void writel(u32 v,void *p){
    *(u32 *)p=v;
    if(p==&regs[2]&&v==1){assert(issued);started++;regs[3]=0;}
    else if(p==&regs[2]&&v==2){assert(terminated);aborted++;regs[3]=mode==5?1:0;}
}
#define readl_poll_timeout(address,value,condition,delay,timeout) \
    ({(value)=readl(address);(condition)?0:-ETIMEDOUT;})
static int mutex_lock_interruptible(struct mutex *m){assert(!m->held);m->held=1;return 0;}
static void mutex_unlock(struct mutex *m){assert(m->held);m->held=0;}
static void reinit_completion(struct completion *p){}
static int copy_to_user(void *p,const void *q,size_t n){memcpy(p,q,n);return 0;}
static void spectrum_done(void *p){}
static struct dma_async_tx_descriptor *dmaengine_prep_slave_single(struct dma_chan *p,dma_addr_t a,size_t n,int d,int f){assert(n==8192);return mode==1?NULL:&tx;}
static int dmaengine_submit(struct dma_async_tx_descriptor *p){return mode==2?-EIO:1;}
static int dma_submit_error(int c){return c<0?c:0;}
static void dma_async_issue_pending(struct dma_chan *p){issued=1;}
static long wait_for_completion_interruptible_timeout(struct completion *p,unsigned t){assert(started&&t==250);return (mode==3||mode==6)?0:mode==4?-ERESTARTSYS:1;}
static int dma_async_is_tx_complete(struct dma_chan *p,int c,void *a,void *b){return 0;}
static int dmaengine_terminate_sync(struct dma_chan *p){terminated++;return mode==6?-EIO:0;}
#include "spectrum_functions.inc"
int main(void)
{
    struct spectrum s={0};unsigned char buf[8192]={0},out[8192];struct file f={&s};loff_t pos=0;
    s.regs=regs;s.buffer=buf;
    assert(spectrum_read(&f,(char *)out,8191,&pos)==-EMSGSIZE);
    assert(!started);
    for(mode=0;mode<=6;mode++){
        issued=started=terminated=aborted=0;s.broken=false;regs[3]=mode==5?1:0;
        int ret=spectrum_read(&f,(char *)out,8192,&pos);
        if(mode==0){assert(ret==8192&&!terminated);}
        else if(mode==6){assert(ret<0&&terminated==1&&!aborted&&s.broken);}
        else {assert(ret<0&&terminated==1&&aborted==1);}
        if(mode==5){assert(s.broken);assert(spectrum_read(&f,(char *)out,8192,&pos)==-EIO);}
        assert(!s.lock.held);
        if(mode>=1&&mode<=4){
            int failed_mode=mode;mode=0;issued=started=terminated=aborted=0;
            assert(spectrum_read(&f,(char *)out,8192,&pos)==8192);
            assert(started&&!terminated&&!aborted);mode=failed_mode;
        }
    }
    struct fpv_spectrum_info info;
    assert(!spectrum_ioctl(&f,FPV_SPECTRUM_INFO,(unsigned long)&info));assert(info.bins==2048&&info.averages==16);
    s.gone=true;assert(spectrum_read(&f,(char *)out,8192,&pos)==-ENODEV);
    assert(spectrum_ioctl(&f,FPV_SPECTRUM_INFO,(unsigned long)&info)==-ENODEV);
    return 0;
}
