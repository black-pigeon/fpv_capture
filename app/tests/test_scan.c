#include "fpv_scan.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
int main(void)
{
    struct fpv_scan_config cfg;
    struct fpv_scan_hop hop;
    char error[160];
    fpv_scan_defaults(&cfg);assert(cfg.gain_db==50);
    assert(fpv_scan_config_parse("bands=5650-5657&step_mhz=2&dwell_ms=150&settle_ms=20&gain_db=20&repeat=1",&cfg,error,sizeof(error))==0);
    assert(fpv_scan_points(&cfg)==5);
    assert(fpv_scan_plan(&cfg,0,&hop)==0 && hop.enable_mask==15 && hop.lo_hz==5653500000ULL);
    assert(hop.target_hz[0]==5650000000ULL && hop.target_hz[3]==5657000000ULL);
    assert(fpv_scan_plan(&cfg,4,&hop)==0 && hop.enable_mask==1 && hop.target_hz[0]==5652000000ULL);
    assert(fpv_scan_plan(&cfg,5,&hop)<0);
    assert(fpv_scan_config_parse("bands=70-70%0A6000-6000&step_mhz=2",&cfg,error,sizeof(error))==0);
    assert(fpv_scan_plan(&cfg,0,&hop)==0 && hop.enable_mask==1 && hop.lo_hz==70000000);
    assert(fpv_scan_plan(&cfg,1,&hop)==0 && hop.lo_hz==6000000000ULL);
    const char *bad[]={"bands=69-71","bands=6000-6001","bands=5900-5800","bands=NaN-5900","bands=5650-5900&step_mhz=0", "bands=5650-5900&dwell_ms=2","bands=5650-5900&gain_db=80","bands=5650-5900&repeat=3","bands=5650-5900&bands=5700-5800","bands=5650-5900&shell=x","bands=5650-5900%00evil"};
    for(unsigned i=0;i<sizeof(bad)/sizeof(bad[0]);i++)assert(fpv_scan_config_parse(bad[i],&cfg,error,sizeof(error))<0);
    /* Every requested center is visited once, all phases within a window
     * keep their LO; endpoint and tuning limits include NCO rounding. */
    const unsigned steps[]={100000,2000000,6000000,8000000,10000000};
    const unsigned spans[]={0,1,7,38,40,41,79,300};
    for(unsigned a=0;a<sizeof(steps)/sizeof(steps[0]);a++)for(unsigned j=0;j<8;j++) {
        fpv_scan_defaults(&cfg);cfg.step_hz=steps[a];
        cfg.bands[0]=(struct fpv_scan_band){70000000,70000000+(uint64_t)spans[j]*1000000+123};
        unsigned total=fpv_scan_points(&cfg),seen[4096]={0},prev_window=~0U;
        uint64_t lo=0;
        for(unsigned p=0;p<total;p+=hop.count) {
            assert(!fpv_scan_plan(&cfg,p,&hop));
            assert(hop.count>0&&hop.count<=4&&hop.enable_mask==((1U<<hop.count)-1));
            if(prev_window==hop.window)assert(lo==hop.lo_hz);
            prev_window=hop.window;lo=hop.lo_hz;
            assert(lo>=70000000&&lo<=6000000000ULL);
            for(unsigned ch=0;ch<hop.count;ch++) {
                uint64_t f=hop.target_hz[ch];
                unsigned idx=f==cfg.bands[0].end_hz?total-1:(unsigned)((f-cfg.bands[0].start_hz)/cfg.step_hz);
                assert(idx<total&&!seen[idx]++);
                long long d=(long long)f-(long long)lo;
                assert(d>=-20000000&&d<=20000000);
                long long err=(long long)hop.actual_hz[ch]-(long long)f;
                assert(err>=-469&&err<=469);
                assert((long long)lo+((int16_t)hop.cfo[ch])*937.5>=f-469.0);
            }
        }
        for(unsigned p=0;p<total;p++)assert(seen[p]==1);
    }
    assert(!fpv_scan_config_parse("bands=5650-5950&step_mhz=8",&cfg,error,sizeof(error)));
    assert(!fpv_scan_plan(&cfg,0,&hop)&&hop.count==4&&hop.phases==2);
    assert(hop.target_hz[3]-hop.target_hz[0]==40000000);
    assert(fpv_scan_config_parse("bands=5650-5950&step_mhz=10.1",&cfg,error,sizeof(error))<0);
    /* Upper edge and band boundaries must not share one RF window. */
    assert(!fpv_scan_config_parse("bands=5947-6000%0A70-111&step_mhz=2",&cfg,error,sizeof(error)));
    unsigned visited=0;
    for(unsigned p=0;p<fpv_scan_points(&cfg);p+=hop.count) {
        assert(!fpv_scan_plan(&cfg,p,&hop));
        assert(hop.lo_hz>=70000000&&hop.lo_hz<=6000000000ULL);
        for(unsigned ch=0;ch<hop.count;ch++) {
            uint64_t f=hop.target_hz[ch];
            assert((f>=5947000000ULL&&f<=6000000000ULL)||(f>=70000000&&f<=111000000));
            visited++;
        }
    }
    assert(visited==fpv_scan_points(&cfg));
    fpv_scan_defaults(&cfg);
    assert(cfg.step_hz==6000000);
    /* A 6 MHz grid fills a window with seven centers rather than
     * changing the RF LO after its first four-channel observation. */
    assert(!fpv_scan_plan(&cfg,0,&hop));
    uint64_t first_lo=hop.lo_hz;
    assert(first_lo==5668000000ULL && hop.count==4 && hop.phases==2);
    assert(hop.target_hz[0]==5650000000ULL && hop.target_hz[1]==5662000000ULL);
    assert(hop.target_hz[2]==5674000000ULL && hop.target_hz[3]==5686000000ULL);
    assert(!fpv_scan_plan(&cfg,4,&hop));
    assert(hop.lo_hz==first_lo && hop.count==3 && hop.phase==1 && hop.phases==2);
    assert(hop.target_hz[0]==5656000000ULL && hop.target_hz[1]==5668000000ULL);
    assert(hop.target_hz[2]==5680000000ULL);
    assert(!fpv_scan_plan(&cfg,7,&hop));
    assert(hop.lo_hz==first_lo+42000000 && hop.phase==0);
    unsigned settings=0,windows=0;uint64_t prev_lo=0;
    for(unsigned p=0;p<fpv_scan_points(&cfg);p+=hop.count) {
        assert(!fpv_scan_plan(&cfg,p,&hop));settings++;
        if(hop.lo_hz!=prev_lo)windows++;
        prev_lo=hop.lo_hz;
    }
    assert(fpv_scan_points(&cfg)==51&&settings==15&&windows==8);
    assert(!fpv_scan_config_parse("mode=lock&frequency_mhz=5800.125&gain_db=20",&cfg,error,sizeof(error)));
    assert(cfg.mode==FPV_SCAN_MODE_LOCK && cfg.lock_hz==5800125000ULL && fpv_scan_points(&cfg)==1);
    assert(!fpv_scan_plan(&cfg,0,&hop) && hop.count==1 && hop.enable_mask==1 && hop.cfo[0]==0);
    assert(hop.lo_hz==cfg.lock_hz && hop.actual_hz[0]==cfg.lock_hz && hop.target_hz[0]==cfg.lock_hz);
    assert(fpv_scan_plan(&cfg,1,&hop)<0);
    const char *bad_lock[]={"mode=lock", "mode=other&frequency_mhz=5800", "mode=lock&frequency_mhz=nan",
        "mode=lock&frequency_mhz=69", "mode=lock&frequency_mhz=6001", "frequency_mhz=5800&bands=5790-5810",
        "mode=lock&frequency_mhz=5800&frequency_mhz=5801", "mode=lock&mode=scan&frequency_mhz=5800"};
    for(unsigned i=0;i<sizeof(bad_lock)/sizeof(bad_lock[0]);i++)
        assert(fpv_scan_config_parse(bad_lock[i],&cfg,error,sizeof(error))<0);
    struct fpv_capture_scan_status status={0};
    status.epoch=4;status.lock_mask=1;status.detect.channel[0].state=14;status.quality[0].counts=(65U<<16)|67;
    assert(fpv_scan_candidate(&status,4,0));
    assert(!fpv_scan_candidate(&status,3,0));
    status.tuning=1;assert(!fpv_scan_candidate(&status,4,0));status.tuning=0;
    status.fault_mask=1;assert(!fpv_scan_candidate(&status,4,0));status.fault_mask=0;
    status.quality[0].counts=67;assert(!fpv_scan_candidate(&status,4,0));
    puts("PASS distributed four-lane coverage, RF window reuse, edge bands, NCO quantization, strict config and freshness");
}
