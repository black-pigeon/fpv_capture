/* Used by test_capture_driver.py with the current driver's real functions. */
#define _GNU_SOURCE
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <linux/fpv_capture.h>

typedef uint32_t u32;
typedef uint8_t u8;
typedef uintptr_t dma_addr_t;
typedef int dma_cookie_t;
struct miscdevice { int unused; };
struct mutex { int held; };
struct kref { int unused; };
struct completion { int done; };
struct dma_chan { int unused; };
struct device { int unused; };
struct file { void *private_data; int f_flags; };
struct dma_async_tx_descriptor { void (*callback)(void *); void *callback_param; };
#define __iomem
#define __user
#define ERESTARTSYS 512
#define BIT(n) (1U << (n))
#define GENMASK(high, low) ((UINT32_MAX >> (31 - (high))) & (UINT32_MAX << (low)))
#define DMA_DEV_TO_MEM 0
#define DMA_PREP_INTERRUPT 1
#define DMA_CTRL_ACK 2
#define DMA_COMPLETE 0
#define current NULL
#define time_after_eq(a, b) ((long)((a) - (b)) >= 0)
#define time_before(a, b) ((long)((a) - (b)) < 0)
#define msecs_to_jiffies(ms) (ms)
#define dma_rmb() ((void)0)
static unsigned long jiffies;
static int signal_pending(void *task);
static u32 readl(void *address);
static void writel(u32 value, void *address);
static void usleep_range(unsigned low, unsigned high);
static int dmaengine_terminate_sync(struct dma_chan *chan);
static struct dma_async_tx_descriptor *dmaengine_prep_slave_single(
    struct dma_chan *chan, dma_addr_t address, size_t len, int direction, int flags);
static dma_cookie_t dmaengine_submit(struct dma_async_tx_descriptor *desc);
static int dma_submit_error(dma_cookie_t cookie) { return cookie < 0 ? cookie : 0; }
static void dma_async_issue_pending(struct dma_chan *chan);
static int dma_async_is_tx_complete(struct dma_chan *chan, dma_cookie_t cookie,
                                    void *first, void *last) { return DMA_COMPLETE; }
static long wait_for_completion_interruptible_timeout(struct completion *done, unsigned long ticks);
static void complete(struct completion *done) { done->done = 1; }
static void reinit_completion(struct completion *done) { done->done = 0; }
static int mutex_lock_interruptible(struct mutex *lock) { assert(!lock->held); lock->held = 1; return 0; }
static void mutex_unlock(struct mutex *lock) { assert(lock->held); lock->held = 0; }
static int copy_to_user(void *to, const void *from, size_t size) { memcpy(to, from, size); return 0; }
#define copy_from_user copy_to_user
static u32 get_unaligned_le32(const u8 *p)
{ return p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24); }
#define readl_poll_timeout(address, value, condition, delay, timeout) \
    ({ int poll_result = -ETIMEDOUT; unsigned poll_i; \
       for (poll_i = 0; poll_i < 100; poll_i++) { \
           (value) = readl(address); if (condition) { poll_result = 0; break; } \
           usleep_range(1000, 2000); \
       } poll_result; })

#include "driver_functions.inc"

static struct fpv_capture fpv;
static struct file capture_file;
static struct dma_async_tx_descriptor descriptor;
static u32 registers[64];
static u8 buffer[FPV_CAPTURE_MAX_BYTES], received[FPV_CAPTURE_MAX_BYTES];
static unsigned snapshots, starts, applies, aborts, terminated, prepared;
static unsigned invalid_until, lock_after, interrupt_at;
static unsigned long acknowledge_at;
static bool radio_clock, interrupted, dma_issued, stalled_dma;
static bool stale_source, lost_source, prepare_failure;
static int terminate_error;
static size_t dma_len;
static u32 states[4];
static unsigned reg_offset(void *address)
{ return (u8 *)address - (u8 *)registers; }
static u32 readl(void *address)
{
    unsigned offset = reg_offset(address);
    assert(offset < sizeof(registers) && !(offset & 3));
    if (offset == REG_STATUS && (registers[REG_STATUS / 4] & STATUS_SNAPSHOT_PENDING) &&
        radio_clock && jiffies >= acknowledge_at) {
        registers[REG_STATUS / 4] &= ~STATUS_SNAPSHOT_PENDING;
        registers[REG_LOCK / 4] = snapshots >= lock_after ? BIT(2) : 0;
        for (unsigned i = 0; i < 4; i++) {
            registers[(REG_DETECT + 12 * i) / 4] =
                snapshots <= invalid_until ? 0 : states[i];
            registers[(REG_DETECT + 12 * i + 4) / 4] = 1000 + i;
            registers[(REG_DETECT + 12 * i + 8) / 4] = 2000 + i;
        }
    }
    if (offset >= REG_DETECT)
        assert(!(registers[REG_STATUS / 4] & STATUS_SNAPSHOT_PENDING));
    return registers[offset / 4];
}
static void put32(u8 *p, u32 value)
{ p[0] = value; p[1] = value >> 8; p[2] = value >> 16; p[3] = value >> 24; }
static void writel(u32 value, void *address)
{
    unsigned offset = reg_offset(address);
    assert(offset < sizeof(registers) && !(offset & 3));
    registers[offset / 4] = value;
    if (offset != REG_COMMAND) return;
    if (value == CMD_BEGIN_TUNE) {
        registers[REG_TUNE/4]=1; registers[REG_EPOCH/4]++;
    } else if (value == CMD_COMMIT_TUNE) {
        registers[REG_TUNE/4]=0;
    } else if (value == CMD_SNAPSHOT) {
        assert(!(registers[REG_STATUS / 4] & STATUS_BUSY));
        snapshots++;
        registers[REG_SNAP_EPOCH/4]=registers[REG_EPOCH/4];
        registers[REG_SNAP_FLAGS/4]=registers[REG_TUNE/4]<<8;
        registers[REG_STATUS / 4] |= STATUS_SNAPSHOT_PENDING;
        acknowledge_at = jiffies + 1;
    } else if (value == CMD_APPLY) {
        assert(!(registers[REG_STATUS / 4] & STATUS_BUSY));
        assert(registers[REG_AUTO / 4] <= 15);
        applies++;
        if (!radio_clock) registers[REG_STATUS / 4] = 1;
    } else if (value == CMD_START) {
        u32 ntsc, height, tail = 0x454e4400;
        assert(dma_issued && prepared && !(registers[REG_EXPECTED / 4] & 0xfe));
        starts++;
        ntsc = fpv.auto_mask & BIT(fpv.config.channel) ?
            registers[REG_EXPECTED / 4] & 1 : (fpv.config.standard_mask >> fpv.config.channel) & 1;
        height = ntsc ? 480 : 576;
        assert(dma_len == 720 * height + 20);
        if (stale_source || lost_source) tail |= 0x40;
        put32(buffer, 0x46505647);
        put32(buffer + 4, (2U << 24) | (fpv.config.channel << 17) | (ntsc << 16));
        put32(buffer + 8, (720U << 16) | height);
        put32(buffer + 12, 720 * height / 4);
        put32(buffer + dma_len - 4, tail);
        if (!stalled_dma) descriptor.callback(descriptor.callback_param);
    } else if (value == CMD_ABORT) {
        assert(terminated); /* DMA must be terminated before FIFO reset. */
        aborts++;
        registers[REG_STATUS / 4] = radio_clock ? STATUS_READY : 1;
    } else {
        assert(!"combined or unknown command");
    }
}
static int signal_pending(void *task) { return interrupted; }
static void usleep_range(unsigned low, unsigned high)
{
    jiffies++;
    if (interrupt_at && jiffies >= interrupt_at) interrupted = true;
}
static int dmaengine_terminate_sync(struct dma_chan *chan)
{ terminated++; dma_issued = false; return terminate_error; }
static struct dma_async_tx_descriptor *dmaengine_prep_slave_single(
    struct dma_chan *chan, dma_addr_t address, size_t len, int direction, int flags)
{
    assert(direction == DMA_DEV_TO_MEM && flags == (DMA_PREP_INTERRUPT | DMA_CTRL_ACK));
    prepared++; dma_len = len;
    return prepare_failure ? NULL : &descriptor;
}
static dma_cookie_t dmaengine_submit(struct dma_async_tx_descriptor *desc) { return 1; }
static void dma_async_issue_pending(struct dma_chan *chan) { dma_issued = true; }
static long wait_for_completion_interruptible_timeout(struct completion *done, unsigned long ticks)
{
    assert(ticks <= fpv.config.timeout_ms);
    if (interrupted) return -ERESTARTSYS;
    if (done->done) return ticks ? ticks : 1;
    jiffies += ticks;
    return 0;
}
static void reset(void)
{
    memset(&fpv, 0, sizeof(fpv)); memset(registers, 0, sizeof(registers));
    fpv.version=6; fpv.regs = registers; fpv.buffer = buffer; fpv.auto_mask = 15;
    fpv.config = (struct fpv_capture_config){2, 4, 15, 100, {1, 2, 3, 4}};
    capture_file = (struct file){&fpv, 0};
    snapshots = starts = applies = aborts = terminated = prepared = 0;
    invalid_until = interrupt_at = 0; lock_after = 1;
    interrupted = dma_issued = stalled_dma = stale_source = lost_source = prepare_failure = false;
    terminate_error = 0; jiffies = 0; radio_clock = true; dma_len = 0;
    registers[REG_STATUS / 4] = STATUS_READY;
    for (unsigned i = 0; i < 4; i++) states[i] = ((9U + i) << 8) | 14;
}
static long do_ioctl(unsigned cmd, void *arg)
{ return fpv_ioctl(&capture_file, cmd, (unsigned long)arg); }
static ssize_t do_read(size_t count)
{ ssize_t ret = fpv_read(&capture_file, (char *)received, count, NULL); assert(!fpv.mutex.held); return ret; }
int main(void)
{
    struct fpv_capture_detect detect;
    struct fpv_capture_config old;
    struct fpv_capture_config_v2 v2;
    ssize_t ret;
    reset(); assert(do_ioctl(FPV_CAPTURE_DETECT, &detect) == 0);
    assert(snapshots == 1 && detect.auto_mask == 15);
    for (unsigned i = 0; i < 4; i++) {
        assert(detect.channel[i].state == states[i]);
        assert(detect.channel[i].field_samples == 1000 + i);
        assert(detect.channel[i].line_samples == 2000 + i);
    }
    reset(); radio_clock = false;
    assert(do_ioctl(FPV_CAPTURE_DETECT, &detect) == -ETIMEDOUT);
    assert(jiffies == 100 && snapshots == 1 && !starts && !prepared);
    reset(); interrupt_at = 1;
    assert(do_ioctl(FPV_CAPTURE_DETECT, &detect) == -ERESTARTSYS);
    assert(jiffies == 1 && snapshots == 1);
    reset(); interrupted = true;
    assert(do_ioctl(FPV_CAPTURE_DETECT, &detect) == -ERESTARTSYS && !snapshots);
    reset(); invalid_until = 2; lock_after = 4;
    ret = do_read(sizeof(received));
    assert(ret == 720 * 576 + 20 && snapshots == 4 && starts == 1);
    assert(registers[REG_EXPECTED / 4] == (11U << 8));
    assert(dma_len == (size_t)ret && !terminated);
    /* The configured manual standard deliberately disagrees with detection. */
    assert(fpv.config.standard_mask == 4);
    reset(); states[2] |= FPV_CAPTURE_DETECT_NTSC; fpv.config.standard_mask = 0;
    assert(do_read(sizeof(received)) == 720 * 480 + 20);
    assert(registers[REG_EXPECTED / 4] == ((11U << 8) | 1));
    reset(); assert(do_read(720 * 480 + 20) == -EMSGSIZE && !prepared && !starts);
    reset(); invalid_until = 1000;
    assert(do_read(sizeof(received)) == -ETIMEDOUT && jiffies <= 101 && !prepared);
    reset(); lock_after = 1000;
    assert(do_read(sizeof(received)) == -ETIMEDOUT && jiffies <= 101 && !prepared);
    reset(); radio_clock = false;
    assert(do_read(sizeof(received)) == -ETIMEDOUT && jiffies == 100 && !prepared);
    reset(); invalid_until = 1000; interrupt_at = 3;
    assert(do_read(sizeof(received)) == -ERESTARTSYS && jiffies == 3 && !prepared);
    reset(); stale_source = true;
    assert(do_read(sizeof(received)) == -EBADMSG && dma_len == 720 * 576 + 20);
    assert(terminated == 1 && aborts == 1 && !fpv.broken);
    reset(); lost_source = true;
    assert(do_read(sizeof(received)) == -EBADMSG && terminated == 1 && aborts == 1);
    reset(); stalled_dma = true; invalid_until = 2;
    assert(do_read(sizeof(received)) == -ETIMEDOUT && jiffies == 100);
    assert(terminated == 1 && aborts == 1);
    reset(); prepare_failure = true;
    assert(do_read(sizeof(received)) == -EIO && !starts && terminated == 1 && aborts == 1);
    reset(); stale_source = true; terminate_error = -EIO;
    assert(do_read(sizeof(received)) == -EBADMSG && terminated == 1 && !aborts && fpv.broken);
    reset(); old = fpv.config;
    assert(do_ioctl(FPV_CAPTURE_CONFIG, &old) == 0 && !fpv.auto_mask);
    assert(registers[REG_AUTO / 4] == 0 && applies == 1);
    assert(do_read(sizeof(received)) == 720 * 480 + 20 && !snapshots);
    reset(); v2 = (struct fpv_capture_config_v2){fpv.config, 5};
    assert(do_ioctl(FPV_CAPTURE_CONFIG_V2, &v2) == 0 && fpv.auto_mask == 5);
    memset(&v2, 0, sizeof(v2));
    assert(do_ioctl(FPV_CAPTURE_GET_CONFIG_V2, &v2) == 0 && v2.auto_mask == 5);
    assert(!memcmp(&v2.config, &fpv.config, sizeof(fpv.config)));
    v2.auto_mask = 16;
    assert(do_ioctl(FPV_CAPTURE_CONFIG_V2, &v2) == -EINVAL && fpv.auto_mask == 5 && applies == 1);
    reset(); radio_clock = false;
    v2 = (struct fpv_capture_config_v2){fpv.config, 1};
    assert(do_ioctl(FPV_CAPTURE_CONFIG_V2, &v2) == -ETIMEDOUT && fpv.auto_mask == 15 && fpv.broken);
    assert(jiffies <= 100);
    reset(); fpv.gone = true;
    assert(do_read(sizeof(received)) == -ENODEV && !prepared);
    struct fpv_capture_tune tune = {FPV_CAPTURE_TUNE_BEGIN, 0};
    struct fpv_capture_scan_status scan;
    reset(); fpv.version=5;
    assert(do_ioctl(FPV_CAPTURE_TUNE, &tune) == -EOPNOTSUPP);
    reset(); fpv.version=6;
    tune.action=FPV_CAPTURE_TUNE_COMMIT;
    assert(do_ioctl(FPV_CAPTURE_TUNE, &tune) == -EINVAL);
    tune.action=FPV_CAPTURE_TUNE_BEGIN;
    assert(do_ioctl(FPV_CAPTURE_TUNE, &tune) == 0 && tune.epoch==1);
    assert(do_read(sizeof(received)) == -EBUSY && !starts);
    assert(do_ioctl(FPV_CAPTURE_TUNE, &tune) == -EBUSY);
    tune.action=FPV_CAPTURE_TUNE_COMMIT;
    assert(do_ioctl(FPV_CAPTURE_TUNE, &tune) == 0 && tune.epoch==1);
    assert(do_ioctl(FPV_CAPTURE_SCAN_STATUS, &scan) == 0 && scan.epoch==1 && !scan.tuning);
    assert(do_read(sizeof(received)) > 0);
    puts("PASS driver coherent snapshots, bounded waits/signals, exact DMA mode/token, legacy/v2 config and ordered recovery");
    return 0;
}
