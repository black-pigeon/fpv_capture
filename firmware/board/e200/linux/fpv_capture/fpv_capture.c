// SPDX-License-Identifier: GPL-2.0
/* E200 selected-channel capture. DMA is armed before PL START; every failed
 * transfer terminates the provider before resetting the shared PL FIFO. */
#include <linux/completion.h>
#include <linux/delay.h>
#include <linux/dmaengine.h>
#include <linux/dma-mapping.h>
#include <linux/fs.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/jiffies.h>
#include <linux/kref.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/sched/signal.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <asm/unaligned.h>
#include "fpv_capture.h"

#define REG_ID      0x00
#define REG_VERSION 0x04
#define REG_COMMAND 0x08
#define REG_STATUS  0x0c
#define REG_CHANNEL 0x10
#define REG_STD     0x14
#define REG_ENABLE  0x18
#define REG_CFO     0x20
#define REG_LOCK    0x30
#define REG_EVENTS  0x34
#define REG_MASK    0x38
#define REG_ERROR   0x3c
#define REG_FRAMES  0x40
#define REG_FAULT   0x50
#define REG_AUTO    0x58
#define REG_EXPECTED 0x5c
#define REG_TUNE    0x60
#define REG_EPOCH   0x64
#define REG_SNAP_EPOCH 0x68
#define REG_QUALITY 0xb0
#define REG_SNAP_FLAGS 0xd0
#define REG_DETECT  0x80
#define CMD_BEGIN_TUNE BIT(4)
#define CMD_COMMIT_TUNE BIT(5)
#define CMD_APPLY   BIT(0)
#define CMD_START   BIT(1)
#define CMD_ABORT   BIT(2)
#define CMD_SNAPSHOT BIT(3)
#define STATUS_SNAPSHOT_PENDING BIT(4)
#define STATUS_READY BIT(3)
#define STATUS_BUSY (GENMASK(2, 0) | STATUS_SNAPSHOT_PENDING)

struct fpv_capture {
	struct miscdevice misc;
	struct mutex mutex;
	struct kref refs;
	struct completion complete;
	void __iomem *regs;
	struct dma_chan *chan;
	struct device *dma_dev;
	void *buffer;
	dma_addr_t dma_addr;
	struct fpv_capture_config config;
	u32 auto_mask;
	u32 version;
	bool opened, gone, broken;
};

static void fpv_free(struct kref *ref)
{
	kfree(container_of(ref, struct fpv_capture, refs));
}

static int fpv_wait_idle(struct fpv_capture *fpv)
{
	u32 value;

	return readl_poll_timeout(fpv->regs + REG_STATUS, value,
		!(value & STATUS_BUSY) && (value & STATUS_READY), 20, 100000);
}

/* Idle may still describe the state preceding a command. Confirm the
 * transaction's state and epoch before allowing RF access or frame capture.
 * A command which left both unchanged may be retried; an unexpected epoch
 * is never accepted or retried as a different tuning transaction. */
static int fpv_tune_command(struct fpv_capture *fpv, u32 command,
                           u32 expected_tune, u32 expected_epoch)
{
	u32 value, previous_epoch = expected_epoch - expected_tune;
	int attempt, ret;

	for (attempt = 0; attempt < 3; attempt++) {
		ret = fpv_wait_idle(fpv);
		if (ret)
			return ret;
		if (attempt) {
			value = readl(fpv->regs + REG_TUNE);
			if (value == expected_tune &&
			    readl(fpv->regs + REG_EPOCH) == expected_epoch)
				return fpv_wait_idle(fpv);
			if (value != !expected_tune ||
			    readl(fpv->regs + REG_EPOCH) != previous_epoch)
				return -ESTALE;
		}
		writel(command, fpv->regs + REG_COMMAND);
		ret = readl_poll_timeout(fpv->regs + REG_TUNE, value,
			value == expected_tune, 20, 100000);
		if (!ret) {
			ret = fpv_wait_idle(fpv);
			if (ret)
				return ret;
			return readl(fpv->regs + REG_EPOCH) == expected_epoch ?
				0 : -ESTALE;
		}
		if (readl(fpv->regs + REG_EPOCH) != previous_epoch)
			return -ESTALE;
		pr_warn_ratelimited("fpv-capture: command %#x not acknowledged "
			"(tune=%u epoch=%u status=%#x), attempt %u/3\n",
			command, readl(fpv->regs + REG_TUNE),
			readl(fpv->regs + REG_EPOCH),
			readl(fpv->regs + REG_STATUS), attempt + 1);
	}
	return ret;
}

/* Snapshot commands are exclusive. Both a previous pending handshake and
 * this request consume the same interruptible deadline, including no-clock
 * operation. No snapshot is requested during probe. */
static int fpv_snapshot_wait(struct fpv_capture *fpv, unsigned long deadline)
{
	for (;;) {
		if (signal_pending(current))
			return -ERESTARTSYS;
		if (time_after_eq(jiffies, deadline))
			return -ETIMEDOUT;
		if (!(readl(fpv->regs + REG_STATUS) & STATUS_BUSY))
			return 0;
		usleep_range(1000, 2000);
	}
}

static int fpv_snapshot(struct fpv_capture *fpv,
			struct fpv_capture_detect *detect,
			unsigned long deadline)
{
	int i, ret;

	ret = fpv_snapshot_wait(fpv, deadline);
	if (ret)
		return ret;
	writel(CMD_SNAPSHOT, fpv->regs + REG_COMMAND);
	ret = fpv_snapshot_wait(fpv, deadline);
	if (ret)
		return ret;
	detect->auto_mask = fpv->auto_mask;
	/* The AXI copies remain stable until another snapshot command. */
	for (i = 0; i < 4; i++) {
		detect->channel[i].state = readl(fpv->regs + REG_DETECT + 12 * i);
		detect->channel[i].field_samples = readl(fpv->regs + REG_DETECT + 12 * i + 4);
		detect->channel[i].line_samples = readl(fpv->regs + REG_DETECT + 12 * i + 8);
	}
	return 0;
}

static int fpv_detect_standard(struct fpv_capture *fpv, u32 *state,
			       unsigned long deadline)
{
	struct fpv_capture_detect detect;
	int ret;

	for (;;) {
		ret = fpv_snapshot(fpv, &detect, deadline);
		if (ret)
			return ret;
		*state = detect.channel[fpv->config.channel].state;
		/* detected_valid is the detector's stable classification. LOCK is
		 * decoder lock, independently required before arming DMA. */
		if ((*state & FPV_CAPTURE_DETECT_VALID) &&
		    (readl(fpv->regs + REG_LOCK) & BIT(fpv->config.channel)))
			return 0;
		usleep_range(1000, 2000);
	}
}

static int fpv_recover(struct fpv_capture *fpv)
{
	int ret;

	ret = dmaengine_terminate_sync(fpv->chan);
	if (ret) {
		fpv->broken = true;
		return ret;
	}
	writel(CMD_ABORT, fpv->regs + REG_COMMAND);
	ret = fpv_wait_idle(fpv);
	fpv->broken = !!ret;
	return ret;
}

static int fpv_apply(struct fpv_capture *fpv,
		     const struct fpv_capture_config *cfg, u32 auto_mask)
{
	int i, ret = fpv_wait_idle(fpv);

	if (ret)
		return ret;
	writel(cfg->channel, fpv->regs + REG_CHANNEL);
	writel(cfg->standard_mask, fpv->regs + REG_STD);
	writel(cfg->enable_mask, fpv->regs + REG_ENABLE);
	writel(auto_mask, fpv->regs + REG_AUTO);
	for (i = 0; i < 4; i++)
		writel(cfg->cfo[i], fpv->regs + REG_CFO + 4 * i);
	writel(CMD_APPLY, fpv->regs + REG_COMMAND);
	return fpv_wait_idle(fpv);
}

static void fpv_done(void *arg)
{
	struct fpv_capture *fpv = arg;

	complete(&fpv->complete);
}

static int fpv_open(struct inode *inode, struct file *file)
{
	struct fpv_capture *fpv = container_of(file->private_data,
					     struct fpv_capture, misc);
	int ret = 0;

	mutex_lock(&fpv->mutex);
	if (fpv->gone)
		ret = -ENODEV;
	else if (fpv->opened)
		ret = -EBUSY;
	else {
		fpv->opened = true;
		kref_get(&fpv->refs);
		file->private_data = fpv;
	}
	mutex_unlock(&fpv->mutex);
	return ret;
}

static int fpv_release(struct inode *inode, struct file *file)
{
	struct fpv_capture *fpv = file->private_data;

	mutex_lock(&fpv->mutex);
	fpv->opened = false;
	mutex_unlock(&fpv->mutex);
	kref_put(&fpv->refs, fpv_free);
	return 0;
}

static long fpv_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	struct fpv_capture *fpv = file->private_data;
	struct fpv_capture_config cfg;
	struct fpv_capture_config_v2 cfg_v2 = {};
	struct fpv_capture_detect detect;
	struct fpv_capture_info info = {};
	struct fpv_capture_tune tune;
	struct fpv_capture_scan_status scan = {};
	u32 flags;
	int i, ret;

	if (mutex_lock_interruptible(&fpv->mutex))
		return -ERESTARTSYS;
	ret = -ENODEV;
	if (fpv->gone)
		goto out;
	ret = -ENOTTY;
	if (cmd == FPV_CAPTURE_CONFIG || cmd == FPV_CAPTURE_CONFIG_V2) {
		ret = -EFAULT;
		if (cmd == FPV_CAPTURE_CONFIG_V2) {
			if (copy_from_user(&cfg_v2, (void __user *)arg, sizeof(cfg_v2)))
				goto out;
			cfg = cfg_v2.config;
		} else if (copy_from_user(&cfg, (void __user *)arg, sizeof(cfg))) {
			goto out;
		}
		/* The original ABI always selects manual standards, even if the
		 * previous caller configured automatic detection. */
		ret = -EINVAL;
		if (cfg.channel > 3 || cfg.standard_mask > 15 ||
		    cfg.enable_mask > 15 || cfg_v2.auto_mask > 15 ||
		    !(cfg.enable_mask & BIT(cfg.channel)) ||
		    cfg.timeout_ms < 100 || cfg.timeout_ms > 5000)
			goto out;
		for (i = 0; i < 4; i++)
			if (cfg.cfo[i] > 0xffff)
				goto out;
		if (fpv->broken) {
			ret = fpv_recover(fpv);
			if (ret)
				goto out;
		}
		ret = fpv_apply(fpv, &cfg, cfg_v2.auto_mask);
		if (!ret) {
			fpv->config = cfg;
			fpv->auto_mask = cfg_v2.auto_mask;
		} else {
			fpv->broken = true;
		}
	} else if (cmd == FPV_CAPTURE_TUNE) {
        ret = -EOPNOTSUPP;
        if (fpv->version < 6)
            goto out;
        ret = -EFAULT;
        if (copy_from_user(&tune, (void __user *)arg, sizeof(tune)))
            goto out;
        if (tune.action == FPV_CAPTURE_TUNE_BEGIN) {
            ret = -EBUSY;
            if (readl(fpv->regs + REG_TUNE))
                goto out;
            tune.epoch = readl(fpv->regs + REG_EPOCH) + 1;
            ret = fpv_recover(fpv);
            if (ret)
                goto out;
            ret = fpv_tune_command(fpv, CMD_BEGIN_TUNE, 1, tune.epoch);
        } else if (tune.action == FPV_CAPTURE_TUNE_COMMIT) {
            ret = -EINVAL;
            if (!readl(fpv->regs + REG_TUNE))
                goto out;
            tune.epoch = readl(fpv->regs + REG_EPOCH);
            ret = fpv_tune_command(fpv, CMD_COMMIT_TUNE, 0, tune.epoch);
        } else {
            ret = -EINVAL;
            goto out;
        }
        if (ret) {
            fpv->broken = true;
            goto out;
        }
        tune.epoch = readl(fpv->regs + REG_EPOCH);
        ret = copy_to_user((void __user *)arg, &tune, sizeof(tune)) ? -EFAULT : 0;
    } else if (cmd == FPV_CAPTURE_SCAN_STATUS) {
        ret = -EOPNOTSUPP;
        if (fpv->version < 6)
            goto out;
        ret = fpv_snapshot(fpv, &scan.detect,
            jiffies + msecs_to_jiffies(fpv->config.timeout_ms));
        if (ret)
            goto out;
        scan.epoch = readl(fpv->regs + REG_SNAP_EPOCH);
        flags = readl(fpv->regs + REG_SNAP_FLAGS);
        scan.tuning = (flags >> 8) & 1;
        scan.lock_mask = flags & 15;
        scan.fault_mask = (flags >> 4) & 15;
        for (i = 0; i < 4; i++) {
            scan.quality[i].counts = readl(fpv->regs + REG_QUALITY + 8*i);
            scan.quality[i].intervals = readl(fpv->regs + REG_QUALITY + 8*i + 4);
        }
        ret = copy_to_user((void __user *)arg, &scan, sizeof(scan)) ? -EFAULT : 0;
    } else if (cmd == FPV_CAPTURE_GET_CONFIG) {
		ret = copy_to_user((void __user *)arg, &fpv->config, sizeof(fpv->config)) ? -EFAULT : 0;
	} else if (cmd == FPV_CAPTURE_GET_CONFIG_V2) {
		cfg_v2.config = fpv->config;
		cfg_v2.auto_mask = fpv->auto_mask;
		ret = copy_to_user((void __user *)arg, &cfg_v2, sizeof(cfg_v2)) ? -EFAULT : 0;
	} else if (cmd == FPV_CAPTURE_DETECT) {
		ret = fpv_snapshot(fpv, &detect,
			jiffies + msecs_to_jiffies(fpv->config.timeout_ms));
		if (!ret)
			ret = copy_to_user((void __user *)arg, &detect, sizeof(detect)) ? -EFAULT : 0;
	} else if (cmd == FPV_CAPTURE_INFO) {
		info.status = readl(fpv->regs + REG_STATUS);
		info.lock_mask = readl(fpv->regs + REG_LOCK);
		info.fault_mask = readl(fpv->regs + REG_FAULT);
		info.last_error = readl(fpv->regs + REG_ERROR);
		for (i = 0; i < 4; i++)
			info.frame_count[i] = readl(fpv->regs + REG_FRAMES + 4 * i);
		ret = copy_to_user((void __user *)arg, &info, sizeof(info)) ? -EFAULT : 0;
	}
out:
	mutex_unlock(&fpv->mutex);
	return ret;
}

static bool fpv_packet_valid(struct fpv_capture *fpv, size_t len, u32 height,
			     u32 standard)
{
	u8 *p = fpv->buffer;
	u32 meta = get_unaligned_le32(p + 4);
	u32 expected = (2U << 24) | (fpv->config.channel << 17) |
		(standard << 16);

	return get_unaligned_le32(p) == 0x46505647 &&
		(meta & 0xffff0000) == expected &&
		get_unaligned_le32(p + 8) == ((720U << 16) | height) &&
		get_unaligned_le32(p + 12) == 720 * height / 4 &&
		get_unaligned_le32(p + len - 4) == 0x454e4400;
}

static ssize_t fpv_read(struct file *file, char __user *buf, size_t count,
			loff_t *position)
{
	struct fpv_capture *fpv = file->private_data;
	struct dma_async_tx_descriptor *desc;
	dma_cookie_t cookie;
	unsigned long deadline, remaining;
	long waited;
	ssize_t ret;
	u32 height, standard, state = 0;
	size_t len;

	if (file->f_flags & O_NONBLOCK)
		return -EOPNOTSUPP;
	if (mutex_lock_interruptible(&fpv->mutex))
		return -ERESTARTSYS;
	ret = -ENODEV;
	if (fpv->gone)
		goto out;
	ret = -EBUSY;
	if (fpv->version >= 6 && readl(fpv->regs + REG_TUNE))
		goto out;
	ret = -EIO;
	if (fpv->broken)
		goto out;
	deadline = jiffies + msecs_to_jiffies(fpv->config.timeout_ms);
	standard = (fpv->config.standard_mask >> fpv->config.channel) & 1;
	if (fpv->auto_mask & BIT(fpv->config.channel)) {
		ret = fpv_detect_standard(fpv, &state, deadline);
		if (ret)
			goto out;
		standard = state & FPV_CAPTURE_DETECT_NTSC;
	}
	height = standard ? 480 : 576;
	len = 720 * height + 20;
	ret = -EMSGSIZE;
	if (count < len)
		goto out;
	ret = fpv_wait_idle(fpv);
	if (ret)
		goto recover;
	/* Only the generation and expected standard belong in this token.
	 * PL rechecks it in radio; a stale or lost source produces a fixed-size
	 * error packet rather than changing the armed DMA transfer length. */
	writel((state & GENMASK(31, 8)) | standard, fpv->regs + REG_EXPECTED);
	/* A premature TLAST must not accidentally validate an older trailer. */
	memset(fpv->buffer, 0, len);
	reinit_completion(&fpv->complete);
	desc = dmaengine_prep_slave_single(fpv->chan, fpv->dma_addr, len,
			DMA_DEV_TO_MEM, DMA_PREP_INTERRUPT | DMA_CTRL_ACK);
	ret = -EIO;
	if (!desc)
		goto recover;
	desc->callback = fpv_done;
	desc->callback_param = fpv;
	cookie = dmaengine_submit(desc);
	ret = dma_submit_error(cookie);
	if (ret)
		goto recover;
	if (signal_pending(current)) {
		ret = -ERESTARTSYS;
		goto recover;
	}
	if (time_after_eq(jiffies, deadline)) {
		ret = -ETIMEDOUT;
		goto recover;
	}
	dma_async_issue_pending(fpv->chan);
	/* Ordered MMIO START follows provider programming of S2MM buffer/length. */
	writel(7, fpv->regs + REG_EVENTS);
	writel(CMD_START, fpv->regs + REG_COMMAND);
	remaining = jiffies;
	remaining = time_before(remaining, deadline) ? deadline - remaining : 0;
	waited = wait_for_completion_interruptible_timeout(&fpv->complete, remaining);
	if (waited <= 0) {
		ret = waited < 0 ? waited : -ETIMEDOUT;
		goto recover;
	}
	ret = -EIO;
	if (dma_async_is_tx_complete(fpv->chan, cookie, NULL, NULL) != DMA_COMPLETE)
		goto recover;
	ret = fpv_wait_idle(fpv);
	if (ret)
		goto recover;
	dma_rmb();
	ret = -EBADMSG;
	if (!fpv_packet_valid(fpv, len, height, standard))
		goto recover;
	ret = copy_to_user(buf, fpv->buffer, len) ? -EFAULT : (ssize_t)len;
	goto out;
recover:
	/* Keep original error; broken prevents reuse when recovery itself fails. */
	fpv_recover(fpv);
out:
	mutex_unlock(&fpv->mutex);
	return ret;
}

static const struct file_operations fpv_fops = {
	.owner = THIS_MODULE,
	.open = fpv_open,
	.release = fpv_release,
	.read = fpv_read,
	.unlocked_ioctl = fpv_ioctl,
	.llseek = no_llseek,
};

static int fpv_probe(struct platform_device *pdev)
{
	struct fpv_capture *fpv;
	int ret;

	fpv = kzalloc(sizeof(*fpv), GFP_KERNEL);
	if (!fpv)
		return -ENOMEM;
	mutex_init(&fpv->mutex);
	kref_init(&fpv->refs);
	init_completion(&fpv->complete);
	fpv->regs = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(fpv->regs)) {
		ret = PTR_ERR(fpv->regs);
		goto free;
	}
	fpv->version = readl(fpv->regs + REG_VERSION);
	if (readl(fpv->regs + REG_ID) != 0x46505632 ||
	    (fpv->version != 5 && fpv->version != 6)) {
		ret = -ENODEV;
		goto free;
	}
	fpv->chan = dma_request_chan(&pdev->dev, "rx");
	if (IS_ERR(fpv->chan)) {
		ret = dev_err_probe(&pdev->dev, PTR_ERR(fpv->chan), "rx DMA unavailable\n");
		goto free;
	}
	fpv->dma_dev = dmaengine_get_dma_device(fpv->chan);
	fpv->buffer = dma_alloc_coherent(fpv->dma_dev, FPV_CAPTURE_MAX_BYTES,
					&fpv->dma_addr, GFP_KERNEL);
	if (!fpv->buffer) {
		ret = -ENOMEM;
		goto release_dma;
	}
	fpv->config = (struct fpv_capture_config) {
		.channel = 0, .standard_mask = 0, .enable_mask = 15,
		.timeout_ms = 1500, .cfo = {0xa000, 0xe000, 0x2000, 0x6000},
	};
	fpv->auto_mask = 15;
	/* Status is polled through INFO. Control IRQ is reserved for a later
	 * notification interface; DMA completion uses the DMAengine IRQ. */
	writel(0, fpv->regs + REG_MASK);
	/* AD9361/radio clock is initialized manually after boot. Probe must not
	 * require that clock: first CONFIG performs reset/apply after RF init. */
	fpv->broken = true;
	fpv->misc = (struct miscdevice) {
		.minor = MISC_DYNAMIC_MINOR, .name = "fpv-capture",
		.fops = &fpv_fops, .parent = &pdev->dev,
	};
	ret = misc_register(&fpv->misc);
	if (ret)
		goto free_buffer;
	platform_set_drvdata(pdev, fpv);
	return 0;
free_buffer:
	dma_free_coherent(fpv->dma_dev, FPV_CAPTURE_MAX_BYTES, fpv->buffer, fpv->dma_addr);
release_dma:
	dma_release_channel(fpv->chan);
free:
	kref_put(&fpv->refs, fpv_free);
	return ret;
}

static int fpv_remove(struct platform_device *pdev)
{
	struct fpv_capture *fpv = platform_get_drvdata(pdev);

	misc_deregister(&fpv->misc);
	mutex_lock(&fpv->mutex);
	fpv->gone = true;
	fpv_recover(fpv);
	/* Surviving descriptors retain software state only. Releasing while the
	 * platform device is still alive also removes DMA core sysfs links. */
	dma_free_coherent(fpv->dma_dev, FPV_CAPTURE_MAX_BYTES, fpv->buffer,
			  fpv->dma_addr);
	dma_release_channel(fpv->chan);
	mutex_unlock(&fpv->mutex);
	kref_put(&fpv->refs, fpv_free);
	return 0;
}

static const struct of_device_id fpv_of_match[] = {
	{ .compatible = "fpv,e200-capture-1.0" }, {}
};
MODULE_DEVICE_TABLE(of, fpv_of_match);
static struct platform_driver fpv_driver = {
	.probe = fpv_probe, .remove = fpv_remove,
	.driver = { .name = "e200-fpv-capture", .of_match_table = fpv_of_match },
};
module_platform_driver(fpv_driver);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("E200 shared-channel FPV DMA capture");
