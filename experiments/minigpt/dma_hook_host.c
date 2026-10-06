/* Host substitutions and failure tests for the exact RAM-hook state machine. */
#include "dma_hook.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
static unsigned char *ram,*original;
static unsigned ram_size;
static uint32_t sr=0x10000401u,dtc,target=0x83c12340u,syncs;
volatile uint32_t *mg_hook_memory(uint32_t a)
{
    if(a==0xb30200a8u)return &dtc;
    assert(a>=0x80004000u && a-0x80004000u+4<=ram_size);
    return (volatile uint32_t *)(ram+a-0x80004000u);
}
uint32_t mg_hook_lock(void){uint32_t s=sr;sr&=~1u;return s;}
void mg_hook_unlock(uint32_t s){sr=s;}
void mg_hook_cache_sync(void){assert(!(sr&1));syncs++;}
uint32_t mg_hook_target(void){return target;}
void mg_hook_host_init(const char *path)
{
    FILE *f=fopen(path,"rb");assert(f);assert(!fseek(f,0,SEEK_END));ram_size=(unsigned)ftell(f);
    rewind(f);ram=malloc(ram_size);original=malloc(ram_size);assert(ram && original);
    assert(fread(ram,1,ram_size,f)==ram_size);fclose(f);memcpy(original,ram,ram_size);
}
#ifdef MG_DMA_HOOK_TEST
static void reset(void)
{
    memcpy(ram,original,ram_size);memset((void *)mg_dma_hook_status,0,sizeof(uint32_t)*8);
    sr=0x10000401u;dtc=syncs=0;target=0x83c12340u;
}
int main(int argc,char **argv)
{
    assert(argc==2);mg_hook_host_init(argv[1]);volatile uint32_t *p=mg_hook_memory(0x8004755cu);
    for(unsigned enabled=0;enabled<2;enabled++){
        reset();sr=0x10000400u|enabled;uint32_t saved=sr;
        assert(mg_dma_hook_enter() && sr==saved && p[0]!=0x3c02b302u && !p[1]);
        assert(!mg_dma_hook_enter() && sr==saved && mg_dma_hook_status[5]==1);
        assert(mg_dma_hook_leave() && sr==saved && p[0]==0x3c02b302u && p[1]==0x8c4200a8u);
        assert(!mg_dma_hook_status[1] && syncs==2 && mg_dma_hook_status[7]==2);
        assert(mg_dma_hook_leave() && syncs==2);
        assert(mg_dma_hook_enter() && mg_dma_hook_leave() && syncs==4);
    }
    /* Each guarded region must reject a changed byte without patching RAM. */
    uint32_t addresses[]={0x80047310u,0x80004978u,0x8004b93cu,0x800486ecu};
    for(unsigned i=0;i<4;i++){
        reset();*(volatile uint8_t *)mg_hook_memory(addresses[i])^=1;
        assert(!mg_dma_hook_enter() && mg_dma_hook_status[0]==2 && !syncs && p[0]==0x3c02b302u);
        assert(sr==0x10000401u);
    }
    reset();p[0]=0x09000000u;assert(!mg_dma_hook_enter() && p[0]==0x09000000u && !syncs);
    reset();dtc=128;assert(!mg_dma_hook_enter() && !mg_dma_hook_status[0] && !syncs);
    dtc=0;assert(mg_dma_hook_enter() && mg_dma_hook_leave());
    reset();target=0x93c12340u;assert(!mg_dma_hook_enter() && !syncs);
    reset();target|=1;assert(!mg_dma_hook_enter() && !syncs);
    reset();assert(mg_dma_hook_enter());p[1]=0x12345678;
    assert(!mg_dma_hook_leave() && p[0]==0x3c02b302u && p[1]==0x8c4200a8u && !mg_dma_hook_status[1]);
    assert(mg_dma_hook_status[6]==1 && !mg_dma_hook_enter());
    reset();assert(mg_dma_hook_enter());p[0]=0x09000000u;
    assert(!mg_dma_hook_leave() && p[0]==0x09000000u && !mg_dma_hook_status[1] && syncs==1);
    puts("PASS signature guards, foreign code, busy/reentry, exact CP0 restore, paired removal and conflict handling");
    free(ram);free(original);return 0;
}
#endif
