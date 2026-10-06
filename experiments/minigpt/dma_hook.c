#include "dma_hook.h"
#include <stddef.h>

#define WAIT_ADDRESS 0x8004755cu
#define ORIGINAL_0 0x3c02b302u
#define ORIGINAL_1 0x8c4200a8u
volatile uint32_t mg_dma_hook_status[8];
static uint32_t installed_jump;

#ifdef MG_DMA_HOOK_HOST
/* Host tests provide RAM, CP0 and cache-operation substitutes. No MIPS code. */
extern volatile uint32_t *mg_hook_memory(uint32_t address);
extern uint32_t mg_hook_lock(void);
extern void mg_hook_unlock(uint32_t status);
extern void mg_hook_cache_sync(void);
extern uint32_t mg_hook_target(void);
#else
extern void dma_probe_trampoline(void);
static volatile uint32_t *mg_hook_memory(uint32_t address)
{return (volatile uint32_t *)(uintptr_t)address;}
static uint32_t mg_hook_target(void)
{return (uint32_t)(uintptr_t)dma_probe_trampoline;}
static uint32_t mg_hook_lock(void)
{
    uint32_t saved,masked;
    __asm__ volatile("mfc0 %0,$12":"=r"(saved)::"memory");
    masked=saved&~1u;
    __asm__ volatile("mtc0 %0,$12\n\tnop\n\tnop\n\tnop"::"r"(masked):"memory");
    return saved;
}
static void mg_hook_unlock(uint32_t saved)
{__asm__ volatile("mtc0 %0,$12\n\tnop\n\tnop\n\tnop"::"r"(saved):"memory");}
static void mg_hook_cache_sync(void)
{
    /* The two words straddle a 32-byte line at 0x80047560. Use the
     * established loader/JIT CACHE sequence, covering both 16/32-byte lines. */
    __asm__ volatile("sync":::"memory");
    for(uint32_t a=WAIT_ADDRESS&~15u;a<WAIT_ADDRESS+8;a+=16)
        __asm__ volatile("cache 0x15,0(%0)"::"r"(a):"memory");
    __asm__ volatile("sync":::"memory");
    for(uint32_t a=WAIT_ADDRESS&~15u;a<WAIT_ADDRESS+8;a+=16)
        __asm__ volatile("cache 0x10,0(%0)"::"r"(a):"memory");
    __asm__ volatile("sync\n\tnop\n\tnop\n\tnop":::"memory");
}
#endif

static uint32_t crc(uint32_t address,unsigned bytes)
{
    const volatile uint8_t *p=(const volatile uint8_t *)mg_hook_memory(address);
    uint32_t c=~0u;
    while(bytes--){c^=*p++;for(int i=0;i<8;i++)c=(c>>1)^((c&1)?0xedb88320u:0);}
    return c^~0u;
}
static int signature(void)
{
    /* Fingerprints of the unmodified reader, IRQ wrapper and ECC entry in
     * the verified private V1.41 project.bin. This is a selected-code guard,
     * not a claim that every byte of the running firmware was authenticated. */
    return crc(0x80047310u,0x36c)==0x3d39ee4fu &&
           crc(0x80004978u,0x2c)==0x2a9148b0u &&
           crc(0x8004b93cu,0x70)==0x8432e6bbu &&
           crc(0x800486ecu,0x40)==0xf0abfbbbu;
}
int mg_dma_hook_enter(void)
{
    uint32_t saved=mg_hook_lock();int ok=0;
    volatile uint32_t *p=mg_hook_memory(WAIT_ADDRESS);
    if(mg_dma_hook_status[1] || *mg_hook_memory(0xb30200a8u)){
        mg_dma_hook_status[5]++;goto out;
    }
    if(mg_dma_hook_status[0]==2)goto out;
    uint32_t target=mg_hook_target();
    if((target&3) || (target>>28)!=8 || p[0]!=ORIGINAL_0 || p[1]!=ORIGINAL_1 ||
       (!mg_dma_hook_status[0] && !signature())){
        mg_dma_hook_status[0]=2;mg_dma_hook_status[4]++;goto out;
    }
    mg_dma_hook_status[0]=1;
    installed_jump=0x08000000u|((target>>2)&0x03ffffffu);
    p[0]=installed_jump;p[1]=0;mg_hook_cache_sync();mg_dma_hook_status[7]++;
    mg_dma_hook_status[1]=1;mg_dma_hook_status[2]++;ok=1;
out:
    mg_hook_unlock(saved);return ok;
}
int mg_dma_hook_leave(void)
{
    uint32_t saved=mg_hook_lock();int ok=1;
    volatile uint32_t *p=mg_hook_memory(WAIT_ADDRESS);
    if(!mg_dma_hook_status[1])goto out;
    /* A paired SDK read must have drained DMA before returning. Keep the
     * jump until it drains; its compute job has already been deactivated. */
    while(*mg_hook_memory(0xb30200a8u)){}
    if(p[0]!=installed_jump || p[1]!=0){
        mg_dma_hook_status[6]++;mg_dma_hook_status[0]=2;ok=0;
    }
    if(p[0]==installed_jump){
        /* Remove our jump even if its delay slot was changed. Do not replace
         * a foreign jump with the old firmware words. */
        p[0]=ORIGINAL_0;p[1]=ORIGINAL_1;mg_hook_cache_sync();mg_dma_hook_status[7]++;
    }
    mg_dma_hook_status[1]=0;mg_dma_hook_status[3]++;
out:
    mg_hook_unlock(saved);return ok;
}
