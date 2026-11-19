#include "fpv_scan.h"
#include "fpv_board.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
/* An uneven endpoint may be the only center left in the final window.
 * Its LO must stay inside the band and use the same RF route as its target. */
static unsigned check_endpoint_band(uint64_t start,uint64_t end,unsigned step)
{
 struct fpv_scan_config cfg;fpv_scan_defaults(&cfg);
 cfg.bands[0]=(struct fpv_scan_band){start,end};cfg.step_hz=step;
 const struct fpv_board *board=fpv_board_current();
 unsigned total=fpv_scan_points(&cfg),visited=0,errors=0,previous_window=~0U;
 unsigned *seen=calloc(total,sizeof(*seen));assert(seen);
 uint64_t previous_lo=0;struct fpv_scan_hop h;
 for(unsigned point=0;point<total;point+=h.count) {
  assert(!fpv_scan_plan(&cfg,point,&h));assert(h.count>0&&h.count<=4);
  assert(h.enable_mask==((1U<<h.count)-1));
  if(previous_window==h.window)assert(previous_lo==h.lo_hz);
  previous_window=h.window;previous_lo=h.lo_hz;
  int bad=h.lo_hz<start||h.lo_hz>end||fpv_board_rx_port(board,h.lo_hz)<0;
  for(unsigned ch=0;ch<h.count;ch++) {
   uint64_t f=h.target_hz[ch];assert(f>=start&&f<=end);
   unsigned idx=f==end?total-1:(unsigned)((f-start)/step);
   assert(idx<total&&!seen[idx]++);
   assert(f==end||f==start+(uint64_t)idx*step);visited++;
   if(board->rx_boundary_hz&&fpv_board_rx_port(board,f)!=fpv_board_rx_port(board,h.lo_hz))bad=1;
   int64_t offset=(int64_t)f-(int64_t)h.lo_hz;
   int64_t error=(int64_t)h.actual_hz[ch]-(int64_t)f;
   assert(offset>=-20000000&&offset<=20000000);
   assert(error>=-469&&error<=469);
  }
  if(h.phases==1&&h.count==1&&
     (h.lo_hz!=h.target_hz[0]||h.cfo[0]!=0||h.actual_hz[0]!=h.target_hz[0]))bad=1;
  if(bad) {
   fprintf(stderr,"Invalid endpoint window: band=%llu..%llu step=%u point=%u LO=%llu target=%llu\n",
    (unsigned long long)start,(unsigned long long)end,step,point,
    (unsigned long long)h.lo_hz,(unsigned long long)h.target_hz[0]);
   errors++;
  }
 }
 assert(visited==total);for(unsigned i=0;i<total;i++)assert(seen[i]==1);
 free(seen);return errors;
}
int main(void)
{
 const unsigned steps[]={100000,600000,2000000,6000000,8000000,10000000};
 const struct fpv_board *board=fpv_board_current();
 for(unsigned s=0;s<6;s++)for(unsigned variant=0;variant<6;variant++) {
  struct fpv_scan_config cfg;fpv_scan_defaults(&cfg);cfg.step_hz=steps[s];
  cfg.bands[0]=(struct fpv_scan_band){2977000000ULL+variant*123,3023000000ULL+variant*321};
  unsigned total=fpv_scan_points(&cfg),seen[1024]={0},visited=0;
  struct fpv_scan_hop h;uint64_t previous_lo=0;unsigned previous_window=~0U;
  for(unsigned point=0;point<total;point+=h.count) {
   assert(!fpv_scan_plan(&cfg,point,&h));assert(h.count>0&&h.count<=4);
   if(previous_window==h.window)assert(previous_lo==h.lo_hz);
   previous_window=h.window;previous_lo=h.lo_hz;
   for(unsigned ch=0;ch<h.count;ch++) {
    uint64_t f=h.target_hz[ch];unsigned idx=f==cfg.bands[0].end_hz?total-1:(unsigned)((f-cfg.bands[0].start_hz)/cfg.step_hz);
    assert(idx<total&&!seen[idx]++);visited++;
    assert(f>=cfg.bands[0].start_hz&&f<=cfg.bands[0].end_hz);
    if(board->rx_boundary_hz)assert(fpv_board_rx_port(board,f)==fpv_board_rx_port(board,h.lo_hz));
   }
  }
  assert(visited==total);for(unsigned i=0;i<total;i++)assert(seen[i]==1);
 }
 struct fpv_scan_config cfg;struct fpv_scan_hop h;fpv_scan_defaults(&cfg);cfg.mode=FPV_SCAN_MODE_LOCK;
 cfg.lock_hz=3000000000ULL;assert(!fpv_scan_plan(&cfg,0,&h)&&h.lo_hz==3000000000ULL&&h.cfo[0]==0);
 cfg.lock_hz=6000000000ULL;assert(!fpv_scan_plan(&cfg,0,&h)&&h.lo_hz==6000000000ULL);
 /* Include the 2951..2999 and 2971..6000 MHz / 10 MHz reproductions,
  * then vary grid spacing and both endpoints around those cases. */
 const unsigned offsets[]={0,123,999999};unsigned errors=0;
 for(unsigned s=0;s<sizeof(steps)/sizeof(steps[0]);s++)
  for(unsigned a=0;a<sizeof(offsets)/sizeof(offsets[0]);a++)
   for(unsigned b=0;b<sizeof(offsets)/sizeof(offsets[0]);b++) {
    errors+=check_endpoint_band(2951000000ULL+offsets[a],2999000000ULL-offsets[b],steps[s]);
    errors+=check_endpoint_band(2971000000ULL+offsets[a],6000000000ULL-offsets[b],steps[s]);
   }
 assert(!errors);
 puts("PASS full grid, RF routes, endpoint-only windows, 6GHz scan tails and exact lock boundaries");
}
