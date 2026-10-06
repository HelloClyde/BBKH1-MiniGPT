/* Exercise the actual loader/overlay with bounded allocation and device reads. */
#include "engine.h"
#include "dma_hook.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
extern void dma_probe_work(void);
extern void mg_hook_host_init(const char *);
extern volatile uint32_t mg_dma_status[8];
static struct {void *p;size_t bytes;} allocations[128];
static unsigned outstanding,quota=8,layer_allocations;
static int reserve_failure,simulate_pending=1,read_failure=-1,corrupt_layer=-1;
static uint32_t layer_bytes,layer_start;
static uint64_t read_bytes;
static mg_model *clock_model;static uint32_t clock_value;static int prefer_cache;
static uint32_t clock_test(void)
{clock_value+=((clock_model->layer_next!=0)==prefer_cache)?100u:50u;return clock_value;}
static int tune_cancel(void *ui,int layer,int phase)
{(void)ui;(void)layer;(void)phase;return clock_model->cache_tune_phase==2;}

void *cache_test_malloc(size_t n)
{
    if(reserve_failure && n==MG_LAYER_CACHE_RESERVE && layer_allocations>=2)return 0;
    /* The first two equal-sized allocations are the mandatory and DMA buffers. */
    if(n==layer_bytes && layer_allocations++>=2+quota)return 0;
    void *p=malloc(n);if(!p)return 0;
    for(unsigned i=0;i<128;i++)if(!allocations[i].p){allocations[i].p=p;allocations[i].bytes=n;outstanding++;return p;}
    abort();
}
void cache_test_free(void *p)
{
    if(!p)return;
    for(unsigned i=0;i<128;i++)if(allocations[i].p==p){allocations[i].p=0;outstanding--;free(p);return;}
    abort();
}
static int readat(void *io,uint32_t off,void *p,uint32_t n)
{
    if(fseek(io,off,SEEK_SET))return 0;
    if(n==layer_bytes && off==layer_start+layer_bytes*(uint32_t)read_failure)return 0;
    for(unsigned done=0;done<n;){
        unsigned block=n-done;if(block>512)block=512;
        if(simulate_pending && mg_dma_hook_status[1])dma_probe_work();
        if(fread((uint8_t *)p+done,1,block,io)!=block)return 0;
        done+=block;read_bytes+=block;
    }
    if(n==layer_bytes && off==layer_start+layer_bytes*(uint32_t)corrupt_layer){
        uint32_t nan=0x7fc00000;memcpy(p,&nan,4);
    }
    return 1;
}
static int cancel(void *ui,int layer,int phase)
{(void)ui;return layer==2 && phase==1;}
static int same(mg_model *a,mg_model *b,float *x,float *y)
{
    if(!x || !y){fprintf(stderr,"null logits %p %p\n",(void *)x,(void *)y);return 0;}
    for(unsigned i=0;i<MG_VOCAB;i++)if(memcmp(x+i,y+i,4)){fprintf(stderr,"logit %u %.9g %.9g\n",i,x[i],y[i]);return 0;}
    for(unsigned i=0;i<MG_LAYERS*MG_CONTEXT*128*2;i++)if(memcmp(a->cache+i,b->cache+i,4)){fprintf(stderr,"KV %u %.9g %.9g\n",i,a->cache[i],b->cache[i]);return 0;}
    return 1;
}
#define CHECK(condition) do {if(!(condition)){fprintf(stderr,"FAIL line %d quota %u\n",__LINE__,quota);return 1;}} while(0)
int main(int argc,char **argv)
{
    if(argc!=3)return 2;mg_hook_host_init(argv[2]);
    FILE *a=fopen(argv[1],"rb"),*b=fopen(argv[1],"rb");CHECK(a && b);
    mg_model base,test;CHECK(mg_load(&base,readat,a,1)==0 && base.resident_mode);
    unsigned baseline_allocations=outstanding;
    layer_bytes=base.layer_bytes;layer_start=base.t[3].offset;
    int ids[29];for(unsigned i=0;i<29;i++)ids[i]=(5134+i*73)%6400;
    for(quota=0;quota<=8;quota++){
        layer_allocations=0;CHECK(mg_load(&test,readat,b,0)==0);
        CHECK(test.cached_layers==quota && test.layer_next && !test.resident_mode);
        CHECK(outstanding==baseline_allocations+5+quota); /* KV, work, embed, 2 buffers */
        for(unsigned mode=0;mode<3;mode++){
            simulate_pending=mode==0;uint8_t *second=test.layer_next;
            if(mode==2)test.layer_next=0;
            mg_reset(&base);mg_reset(&test);
            memset(base.cache,0,MG_LAYERS*MG_CONTEXT*128*2*sizeof(float));
            memset(test.cache,0,MG_LAYERS*MG_CONTEXT*128*2*sizeof(float));
            float *x=mg_prompt(&base,ids,29);uint64_t before=read_bytes;
            float *y=mg_prompt(&test,ids,29);CHECK(same(&base,&test,x,y));
            CHECK(read_bytes-before==(8-quota)*(uint64_t)layer_bytes);
            for(unsigned step=0;step<3;step++){
                int token=mg_argmax(x);x=mg_forward(&base,token);before=read_bytes;
                uint32_t hits=test.layer_cache_hits,misses=test.layer_cache_misses;
                y=mg_forward(&test,token);if(!same(&base,&test,x,y)){fprintf(stderr,"mode=%u step=%u position=%d DMA_ready jobs=%u\n",mode,step,test.position,mg_dma_status[1]);return 1;}
                CHECK(read_bytes-before==(8-quota)*(uint64_t)layer_bytes);
                CHECK(test.layer_cache_hits-hits==quota && test.layer_cache_misses-misses==8-quota);
            }
            if(mode==2)test.layer_next=second;
        }
        simulate_pending=1;test.yield=cancel;mg_reset(&test);
        CHECK(!mg_forward(&test,5134) && !test.position);test.yield=0;
        mg_reset(&base);mg_reset(&test);
        memset(base.cache,0,MG_LAYERS*MG_CONTEXT*128*2*sizeof(float));
        memset(test.cache,0,MG_LAYERS*MG_CONTEXT*128*2*sizeof(float));
        CHECK(same(&base,&test,mg_forward(&base,5134),mg_forward(&test,5134)));
        if(quota<8){
            read_failure=(int)quota;mg_reset(&test);CHECK(!mg_forward(&test,5134));read_failure=-1;
            CHECK(!mg_dma_hook_status[1] && mg_dma_hook_status[2]==mg_dma_hook_status[3]);
            mg_reset(&base);mg_reset(&test);
            memset(base.cache,0,MG_LAYERS*MG_CONTEXT*128*2*sizeof(float));
            memset(test.cache,0,MG_LAYERS*MG_CONTEXT*128*2*sizeof(float));
            CHECK(same(&base,&test,mg_forward(&base,5134),mg_forward(&test,5134)));
        }
        mg_close(&test);CHECK(outstanding==baseline_allocations);
    }
    /* Force each strategy with a controlled clock while running the actual
     * inference and pointer swaps. Compare both subsequent KV/logits and frees. */
    quota=3;clock_model=&test;
    for(prefer_cache=0;prefer_cache<2;prefer_cache++){
        layer_allocations=0;CHECK(mg_load(&test,readat,b,0)==0);clock_value=0;
        CHECK(mg_select_layer_cache(&test,clock_test));
        CHECK(test.cached_layers==(unsigned)(3+prefer_cache));
        CHECK((test.layer_next==0)==prefer_cache && !test.position && !test.prefix_tokens);
        for(unsigned l=0;l<test.cached_layers;l++)CHECK(test.layer_cache[l]!=test.layer);
        mg_reset(&base);memset(base.cache,0,MG_LAYERS*MG_CONTEXT*128*2*sizeof(float));
        memset(test.cache,0,MG_LAYERS*MG_CONTEXT*128*2*sizeof(float));
        CHECK(same(&base,&test,mg_prompt(&base,ids,29),mg_prompt_cached(&test,ids,29)));
        CHECK(same(&base,&test,mg_forward(&base,5134),mg_forward(&test,5134)));
        mg_close(&test);CHECK(outstanding==baseline_allocations);
    }
    layer_allocations=0;CHECK(mg_load(&test,readat,b,0)==0);test.yield=tune_cancel;
    CHECK(mg_select_layer_cache(&test,clock_test) && test.cache_tune_canceled && !test.position);
    CHECK(test.cached_layers==3 && test.layer_next && !mg_dma_hook_status[1]);
    test.yield=0;mg_reset(&base);
    memset(base.cache,0,MG_LAYERS*MG_CONTEXT*128*2*sizeof(float));
    memset(test.cache,0,MG_LAYERS*MG_CONTEXT*128*2*sizeof(float));
    CHECK(same(&base,&test,mg_forward(&base,5134),mg_forward(&test,5134)));
    mg_close(&test);CHECK(outstanding==baseline_allocations);
    layer_allocations=0;CHECK(mg_load(&test,readat,b,0)==0);read_failure=3;
    CHECK(mg_select_layer_cache(&test,clock_test) && test.cache_tune_canceled && test.cached_layers==3);
    read_failure=-1;mg_close(&test);CHECK(outstanding==baseline_allocations);
    layer_allocations=0;CHECK(mg_load(&test,readat,b,0)==0);corrupt_layer=3;
    CHECK(!mg_select_layer_cache(&test,clock_test));corrupt_layer=-1;
    mg_close(&test);CHECK(outstanding==baseline_allocations);
    quota=8;layer_allocations=0;reserve_failure=1;
    CHECK(mg_load(&test,readat,b,0)==0 && !test.cached_layers && test.layer_next);
    reserve_failure=0;mg_close(&test);CHECK(outstanding==baseline_allocations);
    layer_allocations=0;read_failure=1;
    CHECK(mg_load(&test,readat,b,0)==0 && test.cached_layers==1 && test.layer_cache_read_aborts==1);
    CHECK(!test.layer_cache[1] && !test.layer_cache_warming);read_failure=-1;
    mg_reset(&base);mg_reset(&test);
    memset(base.cache,0,MG_LAYERS*MG_CONTEXT*128*2*sizeof(float));
    memset(test.cache,0,MG_LAYERS*MG_CONTEXT*128*2*sizeof(float));
    CHECK(same(&base,&test,mg_prompt(&base,ids,29),mg_prompt(&test,ids,29)));
    CHECK(same(&base,&test,mg_forward(&base,5134),mg_forward(&test,5134)));
    mg_close(&test);CHECK(outstanding==baseline_allocations);
    layer_allocations=0;corrupt_layer=1;
    CHECK(mg_load(&test,readat,b,0)==-1);CHECK(outstanding==baseline_allocations);
    CHECK(!test.read && !test.cached_layers && !test.layer_next);corrupt_layer=-1;
    CHECK(!mg_dma_hook_status[1] && mg_dma_hook_status[2]==mg_dma_hook_status[3]);
    CHECK(mg_dma_status[3]>0 && !mg_dma_status[5]);
    mg_close(&base);CHECK(!outstanding);fclose(a);fclose(b);
    puts("PASS 0..8 cached layers: exact KV/logits, exact avoided reads, DMA/no-progress/no-buffer, cancel/restart, failed reserve/allocation/read/validation, no leaks");
    return 0;
}
