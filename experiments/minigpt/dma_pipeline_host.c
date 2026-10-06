/* Exact KV/logits equivalence for the generated DMA pipeline overlay. */
#include "engine.h"
#include "tokenizer.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
extern void dma_probe_work(void);
extern volatile uint32_t mg_dma_status[8];
#ifdef MG_DMA_AUTO_HOST
#include "dma_hook.h"
extern void mg_hook_host_init(const char *path);
#endif
#ifdef MG_MXU
#include "mxu_dot.h"
static unsigned reference_scalar_calls;
static float *reference_forward(mg_model *m,int token)
{
    uint32_t saved=mg_mxu_status[0],before=mg_mxu_status[3];mg_mxu_status[0]=(uint32_t)-1;
    float *p=mg_forward(m,token);reference_scalar_calls+=mg_mxu_status[3]-before;mg_mxu_status[0]=saved;return p;
}
static float *reference_prompt(mg_model *m,const int *ids,int count)
{
    uint32_t saved=mg_mxu_status[0],before=mg_mxu_status[3];mg_mxu_status[0]=(uint32_t)-1;
    float *p=mg_prompt(m,ids,count);reference_scalar_calls+=mg_mxu_status[3]-before;mg_mxu_status[0]=saved;return p;
}
#else
#define reference_forward mg_forward
#define reference_prompt mg_prompt
#endif
static int fail_read,simulate_pending=1;
static uint32_t failure_layer_bytes,failure_offset;
static int readat(void *io,uint32_t off,void *p,uint32_t n)
{
    if(fseek(io,off,SEEK_SET))return 0;
    unsigned done=0;
    while(done<n){unsigned block=n-done;if(block>512)block=512;
        if(fail_read && n==failure_layer_bytes && off==failure_offset && done>=65536)return 0;
        /* CPU jobs run before the next mocked device block is copied. */
        if(simulate_pending
#ifdef MG_DMA_AUTO_HOST
           && mg_dma_hook_status[1]
#endif
        )dma_probe_work();
        if(fread((unsigned char *)p+done,1,block,io)!=block)return 0;done+=block;
    }
    return 1;
}
static int cancel(void *ui,int layer,int phase)
{(void)ui;return layer==2 && phase==1;}
int main(int argc,char **argv)
{
#ifdef MG_DMA_AUTO_HOST
    if(argc!=3)return 1;mg_hook_host_init(argv[2]);
#else
    if(argc!=2)return 1;
#endif
    FILE *a=fopen(argv[1],"rb"),*b=fopen(argv[1],"rb");if(!a || !b)return 2;
    mg_model base,test;if(mg_load(&base,readat,a,1) || mg_load(&test,readat,b,0) || !test.layer_next)return 3;
    failure_layer_bytes=test.layer_bytes;failure_offset=test.t[12].offset;
    const unsigned cache_bytes=MG_LAYERS*MG_CONTEXT*128*2*sizeof(float);
    memset(base.cache,0,cache_bytes);memset(test.cache,0,cache_bytes);
    int ids[160];for(int i=0;i<160;i++)ids[i]=(5134+i*73)%6400;
    float *x=reference_prompt(&base,ids,160),*y=mg_prompt(&test,ids,160);
    if(!x || !y || memcmp(x,y,6400*sizeof(float)) || memcmp(base.cache,test.cache,cache_bytes))return 4;
    for(int i=0;i<32;i++){
        int token=mg_argmax(x);x=reference_forward(&base,token);y=mg_forward(&test,token);
        if(!x || !y || memcmp(x,y,6400*sizeof(float)) || memcmp(base.cache,test.cache,cache_bytes))return 5;
    }
    if(!mg_dma_status[3] || !mg_dma_status[6] || mg_dma_status[5])return 6;
    /* No callback progress, and no second buffer, both preserve outputs. */
    for(int mode=0;mode<
#ifdef MG_DMA_AUTO_HOST
        3
#else
        2
#endif
        ;mode++){
        mg_reset(&base);mg_reset(&test);memset(base.cache,0,cache_bytes);memset(test.cache,0,cache_bytes);
        simulate_pending=mode==2;uint8_t *saved=test.layer_next;if(mode==1)test.layer_next=0;
#ifdef MG_DMA_AUTO_HOST
        if(mode==2)mg_dma_hook_status[0]=2;
#endif
        x=reference_forward(&base,5134);y=mg_forward(&test,5134);if(mode==1)test.layer_next=saved;
        if(!x || !y || memcmp(x,y,6400*sizeof(float)) || memcmp(base.cache,test.cache,cache_bytes))return 7;
    }
#ifdef MG_DMA_AUTO_HOST
    mg_dma_hook_status[0]=0;
#endif
    simulate_pending=1;mg_reset(&test);test.yield=cancel;
    if(mg_forward(&test,5134) || test.position)return 8;
    test.yield=0;mg_reset(&base);mg_reset(&test);memset(base.cache,0,cache_bytes);memset(test.cache,0,cache_bytes);
    x=reference_forward(&base,5134);y=mg_forward(&test,5134);
    if(!x || !y || memcmp(x,y,6400*sizeof(float)) || memcmp(base.cache,test.cache,cache_bytes))return 9;
    fail_read=1;if(mg_forward(&test,5134))return 10;fail_read=0;
    mg_reset(&base);mg_reset(&test);memset(base.cache,0,cache_bytes);memset(test.cache,0,cache_bytes);
    x=reference_forward(&base,5134);y=mg_forward(&test,5134);
    if(!x || !y || memcmp(x,y,6400*sizeof(float)) || memcmp(base.cache,test.cache,cache_bytes))return 11;
#ifdef MG_DMA_AUTO_HOST
    if(mg_dma_hook_status[1] || mg_dma_hook_status[2]!=mg_dma_hook_status[3] || mg_dma_hook_status[6])return 12;
#endif
#ifdef MG_MXU
    if(mg_mxu_status[0]!=1 || !mg_mxu_status[1] || !mg_mxu_status[2] || !reference_scalar_calls || mg_mxu_status[3]!=reference_scalar_calls || mg_mxu_status[4])return 13;
#endif
    mg_close(&base);mg_close(&test);fclose(a);fclose(b);
    puts("PASS 160-token prefill + 32 decode positions: full KV/logits byte-identical; zero-progress/buffer fallback, cancel/restart and read errors");
    return 0;
}
