#ifndef FPV_SERVICE_H
#define FPV_SERVICE_H
#include "fpv_backend.h"
#include <pthread.h>
struct fpv_track {
    unsigned id, revision, channel, standard, height, quality;
    uint64_t frequency_hz, target_hz, lo_hz, captured_ms;
    uint32_t epoch;
    unsigned char *bmp;
    size_t size;
};
struct fpv_service {
    pthread_mutex_t mutex;
    pthread_cond_t condition;
    pthread_t worker;
    struct fpv_backend backend;
    struct fpv_scan_config config;
    struct fpv_scan_hop current;
    struct fpv_track tracks[FPV_SCAN_TRACKS];
    unsigned next_id, completed, total, rounds, capture_errors, rf_tunes, ddc_tunes;
    int running, stopping, quitting;
    char phase[32], error[192];
};
int fpv_service_init(struct fpv_service *s,int simulate);
void fpv_service_destroy(struct fpv_service *s);
int fpv_service_start(struct fpv_service *s,const struct fpv_scan_config *c);
void fpv_service_stop(struct fpv_service *s);
int fpv_service_clear(struct fpv_service *s);
char *fpv_service_status(struct fpv_service *s);
unsigned char *fpv_service_image(struct fpv_service *s,unsigned id,size_t *size);
#endif
