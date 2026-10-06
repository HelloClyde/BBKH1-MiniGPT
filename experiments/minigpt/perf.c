#include "perf.h"
#if defined(MG_PROFILE) || defined(MG_DIAGNOSTICS)
#include "h1_sdk.h"
#include "platform/diagnostics.h"
#include <string.h>
static struct {uint32_t ticks[MG_PERF_COUNT],calls[MG_PERF_COUNT],phase_ticks[2][MG_PERF_COUNT],phase_calls[2][MG_PERF_COUNT],previous,rtc;uint64_t bytes,phase_bytes[2];unsigned phase,depth,stack[8],fault;uint32_t clocks[3];} perf;
static uint32_t clock_reg(unsigned off){return *(volatile uint32_t *)(uintptr_t)(0xb0000000u+off);}
static void charge(void)
{
    uint32_t now=h1_raw_tick_80hz();
    if(perf.depth){unsigned stage=perf.stack[perf.depth-1];uint32_t elapsed=now-perf.previous;perf.ticks[stage]+=elapsed;perf.phase_ticks[perf.phase][stage]+=elapsed;}
    perf.previous=now;
}
void mg_perf_begin(void)
{
    memset(&perf,0,sizeof perf);perf.clocks[0]=clock_reg(0);perf.clocks[1]=clock_reg(0x10);perf.clocks[2]=clock_reg(0x20);
    perf.rtc=*(volatile uint32_t *)(uintptr_t)0xb0003004u;
    perf.previous=h1_raw_tick_80hz();perf.stack[0]=MG_PERF_OTHER;perf.depth=1;
}
void mg_perf_enter(unsigned stage)
{
    if(!perf.depth)return;charge();
    if(perf.depth==8 || stage>=MG_PERF_COUNT){perf.fault=1;return;}
    perf.stack[perf.depth++]=stage;perf.calls[stage]++;perf.phase_calls[perf.phase][stage]++;
}
void mg_perf_leave(void){if(!perf.depth)return;charge();if(perf.depth<=1){perf.fault=1;return;}perf.depth--;}
void mg_perf_bytes(uint32_t bytes){if(perf.depth){perf.bytes+=bytes;perf.phase_bytes[perf.phase]+=bytes;}}
void mg_perf_decode(void){if(perf.depth){charge();perf.phase=1;}}
void mg_perf_end(void)
{
    if(!perf.depth)return;charge();if(perf.depth!=1)perf.fault=1;perf.depth=0;
    h1_diag("CLOCK cpccr=%08x cppcr=%08x clkgr=%08x unchanged=%u",perf.clocks[0],perf.clocks[1],perf.clocks[2],
        perf.clocks[0]==clock_reg(0) && perf.clocks[1]==clock_reg(0x10) && perf.clocks[2]==clock_reg(0x20));
    static const char *names[]={"other","read","matrix","attention","ui"};
    for(unsigned i=0;i<MG_PERF_COUNT;i++)h1_diag("PERF %s raw_ticks=%u calls=%u",names[i],perf.ticks[i],perf.calls[i]);
    for(unsigned p=0;p<2;p++){
        const char *phase=p?"decode":"prefill";
        for(unsigned i=0;i<MG_PERF_COUNT;i++)h1_diag("PHASE %s %s raw_ticks=%u calls=%u",phase,names[i],perf.phase_ticks[p][i],perf.phase_calls[p][i]);
        h1_diag("PHASE %s bytes=%llu",phase,(unsigned long long)perf.phase_bytes[p]);
    }
    h1_diag("PERF bytes=%llu fault=%u",(unsigned long long)perf.bytes,perf.fault);
    h1_diag("PERF rtc_start=%u rtc_end=%u",perf.rtc,*(volatile uint32_t *)(uintptr_t)0xb0003004u);
}
#endif
