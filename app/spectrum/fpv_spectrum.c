#include "fpv_spectrum.h"
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define BIN 30000ULL
#define EDGE 27000000LL
#define MAX_POINTS 200000U
struct point { float db; uint32_t distance; uint64_t ms,lo; uint32_t epoch; };
struct band { uint64_t start,end; unsigned offset,count; };
struct fpv_spectrum_store {
    struct point *p;struct band bands[FPV_SCAN_BANDS];
    unsigned bands_count,count,gain,revision;uint32_t epoch;
    uint64_t lo,last_ms;int available,error;
};
struct fpv_spectrum_store *fpv_spectrum_create(void) {return calloc(1,sizeof(struct fpv_spectrum_store));}
void fpv_spectrum_destroy(struct fpv_spectrum_store *s) {if(s){free(s->p);free(s);}}
int fpv_spectrum_reset(struct fpv_spectrum_store *s,const struct fpv_scan_config *c)
{
    free(s->p);s->p=NULL;s->count=0;s->bands_count=0;s->available=0;s->last_ms=0;
    struct band bands[FPV_SCAN_BANDS]={0};unsigned count=0,n=c->mode==FPV_SCAN_MODE_LOCK?1:c->band_count;
    if(n>FPV_SCAN_BANDS){errno=EINVAL;return -1;}
    for(unsigned i=0;i<n;i++) {
        uint64_t start=c->mode==FPV_SCAN_MODE_LOCK?c->lock_hz-EDGE:c->bands[i].start_hz;
        uint64_t end=c->mode==FPV_SCAN_MODE_LOCK?c->lock_hz+EDGE:c->bands[i].end_hz;
        if(end<start||end-start>6000000000ULL){errno=EINVAL;return -1;}
        unsigned size=(unsigned)((end-start)/BIN+1);
        if(size>MAX_POINTS-count){errno=E2BIG;return -1;}
        bands[i]=(struct band){start,end,count,size};count+=size;
    }
    struct point *p=calloc(count?count:1,sizeof(*p));if(!p)return -1;
    free(s->p);s->p=p;memcpy(s->bands,bands,n*sizeof(*bands));
    s->count=count;s->bands_count=n;s->available=0;s->last_ms=0;s->error=0;s->revision++;return 0;
}
void fpv_spectrum_error(struct fpv_spectrum_store *s,int e) {s->error=e;}
int fpv_spectrum_add(struct fpv_spectrum_store *s,uint64_t lo,unsigned gain,uint32_t epoch,uint64_t ms,const uint32_t *power,size_t n)
{
    if(!s||!s->p||n!=2048||gain>60||!ms){errno=EINVAL;return -1;}
    if(s->available && gain!=s->gain)memset(s->p,0,s->count*sizeof(*s->p));
    for(unsigned b=0;b<s->bands_count;b++)for(unsigned j=0;j<s->bands[b].count;j++) {
        struct band *band=&s->bands[b];uint64_t hz=band->start+j*BIN;
        int64_t offset=(int64_t)hz-(int64_t)lo;
        if(offset < -EDGE || offset > EDGE)continue;
        unsigned k=(unsigned)((offset+30720000+15000)/30000);
        struct point *p=&s->p[band->offset+j];
        uint32_t distance=(uint32_t)llabs(offset);
        /* Prefer the interior of overlapping windows; refresh an old pass. */
        if(p->ms && p->ms+2000>=ms && distance>p->distance && lo!=s->lo)continue;
        p->db=power[k]?10.0f*log10f((float)power[k]/268435456.0f):-120.0f;
        p->distance=distance;p->ms=ms;p->lo=lo;p->epoch=epoch;
    }
    s->lo=lo;s->gain=gain;s->epoch=epoch;s->last_ms=ms;s->available=1;s->error=0;s->revision++;return 0;
}
int fpv_spectrum_value(struct fpv_spectrum_store *s,uint64_t hz,unsigned *index,float *db)
{
    for(unsigned b=0;b<s->bands_count;b++)if(hz>=s->bands[b].start && hz<=s->bands[b].end) {
        unsigned k=s->bands[b].offset+(unsigned)((hz-s->bands[b].start+BIN/2)/BIN);
        if(k>=s->bands[b].offset+s->bands[b].count||!s->p[k].ms)return -1;
        *index=k;*db=s->p[k].db;return 0;
    }
    return -1;
}
/* A displayed bucket uses its strongest bin and that same bin's metadata. */
static const struct point *bucket(const struct fpv_spectrum_store *s,const struct band *b,unsigned j,unsigned stride)
{
    const struct point *peak=NULL;
    for(unsigned k=j;k<j+stride && k<b->count;k++) {
        const struct point *p=&s->p[b->offset+k];
        if(p->ms && (!peak||p->db>peak->db))peak=p;
    }
    return peak;
}
char *fpv_spectrum_json(struct fpv_spectrum_store *s,uint64_t now)
{
    char *out=NULL;size_t len=0;FILE *f=open_memstream(&out,&len);if(!f)return NULL;
    fprintf(f,"{\"available\":%s,\"error\":%d,\"revision\":%u,\"bin_hz\":30000,\"fft_size\":2048,\"averages\":16,\"sample_rate\":61440000,\"unit\":\"dBFS\",\"gain_db\":%u,\"epoch\":%u,\"lo_hz\":%llu,\"age_ms\":%llu,\"bands\":[",s->available?"true":"false",s->error,s->revision,s->gain,s->epoch,(unsigned long long)s->lo,(unsigned long long)(s->last_ms&&now>=s->last_ms?now-s->last_ms:0));
    for(unsigned b=0;b<s->bands_count;b++) {
        struct band *band=&s->bands[b];unsigned stride=(band->count+2047)/2048;
        fprintf(f,"%s{\"start_hz\":%llu,\"end_hz\":%llu,\"step_hz\":%llu,\"values\":[",b?",":"",(unsigned long long)band->start,(unsigned long long)band->end,(unsigned long long)(BIN*stride));
        const char *keys[]={"values","age_ms","epoch","lo_hz"};
        for(unsigned field=0;field<4;field++) {
            if(field)fprintf(f,"],\"%s\":[",keys[field]);
            for(unsigned j=0;j<band->count;j+=stride) {
                const struct point *p=bucket(s,band,j,stride);
                if(j)fputc(',',f);
                if(!p)fputs("null",f);
                else if(field==0)fprintf(f,"%.2f",p->db);
                else if(field==1)fprintf(f,"%llu",(unsigned long long)(now>=p->ms?now-p->ms:0));
                else if(field==2)fprintf(f,"%u",p->epoch);
                else fprintf(f,"%llu",(unsigned long long)p->lo);
            }
        }
        fputs("]}",f);
    }
    fputs("]}",f);fclose(f);return out;
}
