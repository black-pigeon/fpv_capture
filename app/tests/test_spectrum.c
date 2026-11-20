#include "fpv_spectrum.h"
#include <assert.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
int main(void)
{
    struct fpv_spectrum_store *s=fpv_spectrum_create();assert(s);
    struct fpv_scan_config c;fpv_scan_defaults(&c);
    c.band_count=2;c.bands[0].start_hz=5700000000ULL;c.bands[0].end_hz=5800000000ULL;
    c.bands[1].start_hz=5900000000ULL;c.bands[1].end_hz=5950000000ULL;
    assert(!fpv_spectrum_reset(s,&c));
    uint32_t p[2048];for(unsigned i=0;i<2048;i++)p[i]=100;
    p[1024+100]=1000000;
    assert(!fpv_spectrum_add(s,5750000000ULL,20,7,1000,p,2048));
    char *json=fpv_spectrum_json(s,1100);assert(json);
    assert(strstr(json,"\"available\":true"));assert(strstr(json,"\"bin_hz\":30000"));
    assert(strstr(json,"null"));assert(strstr(json,"\"age_ms\":100"));free(json);
    unsigned k;float db;
    assert(!fpv_spectrum_value(s,5753000000ULL,&k,&db));assert(db>-40&&db<-10);
    assert(fpv_spectrum_value(s,5900000000ULL,&k,&db)<0);
    assert(fpv_spectrum_add(s,5750000000ULL,20,8,2000,p,2047)<0);
    /* Gain changes invalidate old measurements instead of mixing their levels. */
    assert(!fpv_spectrum_add(s,5920000000ULL,30,9,2100,p,2048));
    assert(fpv_spectrum_value(s,5753000000ULL,&k,&db)<0);
    /* Overlap must preserve the better interior measurement until stale. */
    c.mode=FPV_SCAN_MODE_LOCK;c.lock_hz=5800000000ULL;
    assert(!fpv_spectrum_reset(s,&c));
    for(unsigned i=0;i<2048;i++)p[i]=67108864;
    assert(!fpv_spectrum_add(s,5800000000ULL,20,10,3000,p,2048));
    assert(!fpv_spectrum_value(s,5800000000ULL,&k,&db));
    assert(fabsf(db+6.0206f)<0.001f);
    for(unsigned i=0;i<2048;i++)p[i]=268435456;
    assert(!fpv_spectrum_add(s,5820000000ULL,20,11,3100,p,2048));
    assert(!fpv_spectrum_value(s,5800000000ULL,&k,&db));
    assert(fabsf(db+6.0206f)<0.001f);
    json=fpv_spectrum_json(s,3200);assert(json);
    assert(strstr(json,"\"epoch\":["));assert(strstr(json,"\"lo_hz\":["));free(json);
    assert(!fpv_spectrum_add(s,5820000000ULL,20,12,6001,p,2048));
    assert(!fpv_spectrum_value(s,5800000000ULL,&k,&db));assert(fabsf(db)<0.001f);
    /* A lock window is cropped to reliable RF bandwidth, with no edge data. */
    assert(fpv_spectrum_value(s,5770000000ULL,&k,&db)<0);
    /* A bounded store rejects impractically large multi-band panoramas. */
    c.mode=FPV_SCAN_MODE_SCAN;c.band_count=2;
    c.bands[0].start_hz=c.bands[1].start_hz=70000000ULL;
    c.bands[0].end_hz=c.bands[1].end_hz=6000000000ULL;
    assert(fpv_spectrum_reset(s,&c)<0);
    json=fpv_spectrum_json(s,6100);assert(json);free(json);
    fpv_spectrum_destroy(s);return 0;
}
