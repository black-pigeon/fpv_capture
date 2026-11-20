// SPDX-License-Identifier: GPL-2.0
/* Independent, bounded, synchronous FFT snapshot DMA; RF ownership stays in
 * userspace. Failures terminate DMA before resetting the spectrum producer. */
#include <linux/completion.h>
#include <linux/dmaengine.h>
#include <linux/dma-mapping.h>
#include <linux/fs.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/kref.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include "fpv_spectrum.h"
struct spectrum {
    struct miscdevice misc;
    struct mutex lock;
    struct kref refs;
    struct completion done;
    struct dma_chan *chan;
    struct device *dma_dev;
    void __iomem *regs;
    void *buffer;
    dma_addr_t address;
    bool opened,gone,broken;
};
static void spectrum_free(struct kref *r) {kfree(container_of(r,struct spectrum,refs));}
static void spectrum_done(void *p) {complete(&((struct spectrum *)p)->done);}
static int spectrum_idle(struct spectrum *s)
{
    u32 v;return readl_poll_timeout(s->regs+12,v,!(v&1),20,100000);
}
static int spectrum_open(struct inode *inode,struct file *f)
{
    struct spectrum *s=container_of(f->private_data,struct spectrum,misc);int ret=0;
    mutex_lock(&s->lock);
    if(s->gone)ret=-ENODEV;
    else if(s->opened)ret=-EBUSY;
    else {s->opened=true;kref_get(&s->refs);f->private_data=s;}
    mutex_unlock(&s->lock);return ret;
}
static int spectrum_release(struct inode *inode,struct file *f)
{
    struct spectrum *s=f->private_data;
    mutex_lock(&s->lock);s->opened=false;mutex_unlock(&s->lock);
    kref_put(&s->refs,spectrum_free);return 0;
}
static ssize_t spectrum_read(struct file *f,char __user *buf,size_t len,loff_t *off)
{
    struct spectrum *s=f->private_data;struct dma_async_tx_descriptor *tx;
    dma_cookie_t cookie;long waited;int ret;
    if(len<FPV_SPECTRUM_BYTES)return -EMSGSIZE;
    if(mutex_lock_interruptible(&s->lock))return -ERESTARTSYS;
    if(s->gone){ret=-ENODEV;goto out;}
    if(s->broken){ret=-EIO;goto out;}
    ret=spectrum_idle(s);if(ret)goto recover;
    reinit_completion(&s->done);
    tx=dmaengine_prep_slave_single(s->chan,s->address,FPV_SPECTRUM_BYTES,
        DMA_DEV_TO_MEM,DMA_PREP_INTERRUPT|DMA_CTRL_ACK);
    if(!tx){ret=-EIO;goto recover;}
    tx->callback=spectrum_done;tx->callback_param=s;
    cookie=dmaengine_submit(tx);ret=dma_submit_error(cookie);if(ret)goto recover;
    dma_async_issue_pending(s->chan);
    writel(1,s->regs+8);
    waited=wait_for_completion_interruptible_timeout(&s->done,msecs_to_jiffies(250));
    if(waited<=0){ret=waited?waited:-ETIMEDOUT;goto recover;}
    if(dma_async_is_tx_complete(s->chan,cookie,NULL,NULL)!=DMA_COMPLETE){ret=-EIO;goto recover;}
    ret=spectrum_idle(s);if(ret)goto recover;
    if(readl(s->regs+12)&2){ret=-EOVERFLOW;goto recover;}
    dma_rmb();
    ret=copy_to_user(buf,s->buffer,FPV_SPECTRUM_BYTES)?-EFAULT:(int)FPV_SPECTRUM_BYTES;
    goto out;
recover:
    if(dmaengine_terminate_sync(s->chan)){s->broken=true;goto out;}
    writel(2,s->regs+8);
    if(spectrum_idle(s))s->broken=true;
out:
    mutex_unlock(&s->lock);return ret;
}
static long spectrum_ioctl(struct file *f,unsigned int cmd,unsigned long arg)
{
    struct spectrum *s=f->private_data;int ret;
    struct fpv_spectrum_info info={1,FPV_SPECTRUM_BINS,FPV_SPECTRUM_RATE,FPV_SPECTRUM_AVERAGES};
    if(cmd!=FPV_SPECTRUM_INFO)return -ENOTTY;
    if(mutex_lock_interruptible(&s->lock))return -ERESTARTSYS;
    ret=s->gone?-ENODEV:(copy_to_user((void __user *)arg,&info,sizeof(info))?-EFAULT:0);
    mutex_unlock(&s->lock);return ret;
}
static const struct file_operations spectrum_fops={
    .owner=THIS_MODULE,.open=spectrum_open,.release=spectrum_release,
    .read=spectrum_read,.unlocked_ioctl=spectrum_ioctl,.compat_ioctl=spectrum_ioctl,.llseek=no_llseek,
};
static int spectrum_probe(struct platform_device *pdev)
{
    struct spectrum *s;int ret;
    s=kzalloc(sizeof(*s),GFP_KERNEL);if(!s)return -ENOMEM;
    kref_init(&s->refs);mutex_init(&s->lock);init_completion(&s->done);
    s->regs=devm_platform_ioremap_resource(pdev,0);
    if(IS_ERR(s->regs)){ret=PTR_ERR(s->regs);goto free;}
    if(readl(s->regs)!=0x53504631 || readl(s->regs+4)!=1 ||
        readl(s->regs+16)!=2048 || readl(s->regs+20)!=16){ret=-ENODEV;goto free;}
    s->chan=dma_request_chan(&pdev->dev,"rx");
    if(IS_ERR(s->chan)){ret=PTR_ERR(s->chan);goto free;}
    s->dma_dev=dmaengine_get_dma_device(s->chan);
    s->buffer=dma_alloc_coherent(s->dma_dev,FPV_SPECTRUM_BYTES,&s->address,GFP_KERNEL);
    if(!s->buffer){ret=-ENOMEM;goto channel;}
    s->misc=(struct miscdevice){.minor=MISC_DYNAMIC_MINOR,.name="fpv-spectrum",.fops=&spectrum_fops,.parent=&pdev->dev};
    platform_set_drvdata(pdev,s);ret=misc_register(&s->misc);if(!ret)return 0;
    dma_free_coherent(s->dma_dev,FPV_SPECTRUM_BYTES,s->buffer,s->address);
channel:
    dma_release_channel(s->chan);
free:
    kref_put(&s->refs,spectrum_free);return ret;
}
static int spectrum_remove(struct platform_device *pdev)
{
    struct spectrum *s=platform_get_drvdata(pdev);
    misc_deregister(&s->misc);mutex_lock(&s->lock);s->gone=true;
    dmaengine_terminate_sync(s->chan);writel(2,s->regs+8);
    dma_free_coherent(s->dma_dev,FPV_SPECTRUM_BYTES,s->buffer,s->address);
    dma_release_channel(s->chan);mutex_unlock(&s->lock);
    kref_put(&s->refs,spectrum_free);return 0;
}
static const struct of_device_id spectrum_ids[]={{.compatible="microphase,fpv-spectrum-1.0"},{}};
MODULE_DEVICE_TABLE(of,spectrum_ids);
static struct platform_driver spectrum_driver={.probe=spectrum_probe,.remove=spectrum_remove,
    .driver={.name="fpv-spectrum",.of_match_table=spectrum_ids}};
module_platform_driver(spectrum_driver);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("AntSDR independent FFT snapshot DMA");
