#ifndef FPV_STREAM_H
#define FPV_STREAM_H
#include "fpv_feed.h"
#define FPV_STREAM_CLIENTS 4
struct fpv_stream_server;
struct fpv_stream_client {
    struct fpv_stream_server *server;
    pthread_t thread;
    int fd, resume;
    uint64_t target_hz, after;
    char session_id[33];
};
struct fpv_stream_server {
    pthread_mutex_t mutex;
    pthread_cond_t changed;
    struct fpv_feed *feed;
    int stopping;
    struct fpv_stream_client clients[FPV_STREAM_CLIENTS];
};
int fpv_stream_init(struct fpv_stream_server *server,struct fpv_feed *feed);
/* Stops/join sockets before the feed or acquisition service is destroyed. */
void fpv_stream_destroy(struct fpv_stream_server *server);
/* Takes a duplicate fd on success. Caller always retains its own fd.
 * Errors: EINVAL query; EBUSY client limit; otherwise resource failure. */
int fpv_stream_open(struct fpv_stream_server *server,int fd,const char *query);
#endif
