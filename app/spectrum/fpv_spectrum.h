#ifndef FPV_SPECTRUM_H
#define FPV_SPECTRUM_H
#include "fpv_scan.h"
struct fpv_spectrum_store;
struct fpv_spectrum_store *fpv_spectrum_create(void);
void fpv_spectrum_destroy(struct fpv_spectrum_store *s);
int fpv_spectrum_reset(struct fpv_spectrum_store *s,const struct fpv_scan_config *c);
int fpv_spectrum_add(struct fpv_spectrum_store *s,uint64_t lo,unsigned gain,uint32_t epoch,uint64_t ms,const uint32_t *power,size_t n);
int fpv_spectrum_value(struct fpv_spectrum_store *s,uint64_t hz,unsigned *index,float *db);
char *fpv_spectrum_json(struct fpv_spectrum_store *s,uint64_t now);
void fpv_spectrum_error(struct fpv_spectrum_store *s,int error);
#endif
