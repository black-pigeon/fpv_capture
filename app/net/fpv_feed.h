#ifndef FPV_FEED_H
#define FPV_FEED_H
#include <pthread.h>
#include <stddef.h>
#include <stdint.h>

#define FPV_FEED_CAPACITY 8
#define FPV_FEED_MAX_BYTES (1078U + 720U * 576U)
struct fpv_feed_meta {
    uint64_t frame_id, captured_mono_ms, frequency_hz, target_hz, lo_hz;
    uint32_t channel, standard, width, height, epoch, quality;
    int simulate;
};
/* Immutable until release; references and ring ownership use the feed mutex. */
struct fpv_feed_frame {
    struct fpv_feed_meta meta;
    size_t size;
    unsigned references;
    unsigned char data[];
};
struct fpv_feed {
    pthread_mutex_t mutex;
    pthread_cond_t changed;
    char session_id[33];
    uint64_t sequence;
    int closed;
    struct fpv_feed_frame *ring[FPV_FEED_CAPACITY];
};
int fpv_feed_init(struct fpv_feed *feed);
void fpv_feed_close(struct fpv_feed *feed);
/* Join consumers/release their references before destruction. */
void fpv_feed_destroy(struct fpv_feed *feed);
uint64_t fpv_feed_cursor(struct fpv_feed *feed);
int fpv_feed_publish(struct fpv_feed *feed,const struct fpv_feed_meta *meta,const void *bmp,size_t size);
/* 1=frame, 0=timeout, -1=closed/error. target=0 subscribes to all centers.
 * Cursor advances across nonmatching targets. dropped counts globally evicted
 * frames; it accumulates across calls so a heartbeat can report an overrun. */
int fpv_feed_next(struct fpv_feed *feed,uint64_t *cursor,uint64_t target,int timeout_ms,
                  struct fpv_feed_frame **frame,uint64_t *dropped);
void fpv_feed_release(struct fpv_feed *feed,struct fpv_feed_frame *frame);
#endif
