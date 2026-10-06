/* Host harness uses precisely the same engine and tokenizer as the BDA. */
#include "engine.h"
#include "tokenizer.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#ifdef MG_HOST_ALLOC_TEST
static int fail_batch,fail_pair;
void *__real_malloc(size_t);
void *__wrap_malloc(size_t n){return (fail_batch && n==160*MG_DIM*sizeof(float)) || (fail_pair && n==MG_WORK_BYTES)?0:__real_malloc(n);}
#endif
static int readat(void *io,uint32_t off,void *p,uint32_t n)
{FILE *f=io;return !fseek(f,(long)off,SEEK_SET) && fread(p,1,n,f)==n;}
static int cancel_step(void *ui,int layer,int phase)
{(void)ui;(void)layer;(void)phase;return 1;}
#ifdef MG_PREFIX_CACHE
static int cached_checks(mg_model *m)
{
    mg_model reference;if(mg_load(&reference,m->read,m->io,0))return 1;
    int ids[MG_CONTEXT],counts[]={160,160,160,73,1,160};
    for(int i=0;i<MG_CONTEXT;i++)ids[i]=(5134+i*73)%MG_VOCAB;
    mg_reset(m);
    for(unsigned trial=0;trial<sizeof counts/sizeof counts[0];trial++){
        int count=counts[trial];if(trial==2)ids[159]=(ids[159]+1)%MG_VOCAB;
        mg_reset(&reference);float *expected=mg_prompt(&reference,ids,count),*actual=mg_prompt_cached(m,ids,count);
        if(!expected || !actual || memcmp(expected,actual,MG_VOCAB*sizeof(float)) || m->position!=count)return 2;
        if((trial==0 && m->prompt_reused) || (trial==1 && m->prompt_reused!=159) || (trial==2 && m->prompt_reused!=159) || (trial==3 && m->prompt_reused!=72) || (trial==4 && m->prompt_reused))return 3;
        for(int l=0;l<MG_LAYERS*2;l++)if(memcmp(m->cache+l*MG_CONTEXT*128,reference.cache+l*MG_CONTEXT*128,(size_t)count*128*sizeof(float)))return 4;
        for(int t=0;t<2;t++){
            expected=mg_forward(&reference,ids[t]);actual=mg_forward(m,ids[t]);
            if(!expected || !actual || memcmp(expected,actual,MG_VOCAB*sizeof(float)))return 5;
        }
    }
    m->yield=cancel_step;if(mg_prompt_cached(m,ids,160) || m->prefix_tokens)return 6;
    m->yield=0;mg_reset(&reference);
    float *expected=mg_prompt(&reference,ids,160),*actual=mg_prompt_cached(m,ids,160);
    if(!expected || !actual || m->prompt_reused || memcmp(expected,actual,MG_VOCAB*sizeof(float)))return 7;
    int pos=m->position;ids[0]=-1;
    if(mg_prompt_cached(m,ids,160) || mg_prompt_cached(m,ids,MG_CONTEXT+1) || mg_prompt_cached(m,0,1) || m->position!=pos)return 8;
    mg_reset(m);if(m->prefix_tokens || m->prompt_reused)return 9;
    mg_close(&reference);return 0;
}
#endif
int main(int argc,char **argv)
{
    char input[2048],prompt[2304];int ids[MG_CONTEXT];
    if(argc<3){fprintf(stderr,"usage: host model.mg8 prompt.txt [max_tokens] [stream] [logits.bin]\n       host --encode prompt.txt\n");return 2;}
    FILE *in=fopen(argv[2],"rb");if(!in)return 3;
    size_t n=fread(input,1,sizeof input-1,in);int extra=fgetc(in);fclose(in);input[n]=0;if(extra!=EOF)return 4;
    if(!strcmp(argv[1],"--encode")){
        int count=mg_encode(input,ids,MG_CONTEXT);printf("%d",count);for(int i=0;i<count;i++)printf(" %d",ids[i]);puts("");return count<0;
    }
    snprintf(prompt,sizeof prompt,"<|im_start|>system\nYou are a helpful assistant<|im_end|>\n<|im_start|>user\n%s<|im_end|>\n<|im_start|>assistant\n",input);
    int count=mg_encode(prompt,ids,MG_CONTEXT);if(count<=0 || count>192)return 5;
    FILE *f=fopen(argv[1],"rb");if(!f)return 6;mg_model model;
    int error=mg_load(&model,readat,f,argc<5 || strcmp(argv[4],"stream"));if(error){fprintf(stderr,"load error %d\n",error);fclose(f);return 7;}
    if(argc>4 && !strcmp(argv[4],"checks")){
        int pair[2]={5134,2207};
        model.yield=cancel_step;
        if(mg_forward(&model,5134) || mg_prefill(&model,5134) || mg_prompt(&model,pair,2) || model.position || model.prompt_total)return 11;
        model.yield=0;
        for(int i=0;i<MG_CONTEXT;i++)if(!mg_forward(&model,5134))return 12;
        if(mg_forward(&model,5134) || mg_forward(&model,-1) || mg_forward(&model,6400) || mg_prefill(&model,5134) || mg_prefill(&model,-1) || mg_prefill(&model,6400))return 13;
        mg_reset(&model);if(!mg_forward(&model,5134) || model.position!=1)return 14;
        int batch_ids[160];float expected[MG_VOCAB],*cache=malloc(MG_LAYERS*MG_CONTEXT*128*2*sizeof(float));if(!cache)return 15;
        mg_reset(&model);memset(model.cache,0,MG_LAYERS*MG_CONTEXT*128*2*sizeof(float));float *last=0;
        for(int i=0;i<160;i++){batch_ids[i]=(5134+i*73)%MG_VOCAB;if(!(last=mg_forward(&model,batch_ids[i])))return 16;}
        memcpy(expected,last,sizeof expected);memcpy(cache,model.cache,MG_LAYERS*MG_CONTEXT*128*2*sizeof(float));
        mg_reset(&model);memset(model.cache,0,MG_LAYERS*MG_CONTEXT*128*2*sizeof(float));
        if(!mg_prompt(&model,batch_ids,80) || !(last=mg_prompt(&model,batch_ids+80,80)) || model.position!=160)return 17;
        if(memcmp(expected,last,sizeof expected) || memcmp(cache,model.cache,MG_LAYERS*MG_CONTEXT*128*2*sizeof(float)))return 18;
#ifdef MG_HOST_ALLOC_TEST
        mg_reset(&model);memset(model.cache,0,MG_LAYERS*MG_CONTEXT*128*2*sizeof(float));fail_batch=1;
        last=mg_prompt(&model,batch_ids,160);fail_batch=0;
        if(!last || model.position!=160 || model.prompt_total || memcmp(expected,last,sizeof expected) || memcmp(cache,model.cache,MG_LAYERS*MG_CONTEXT*128*2*sizeof(float)))return 21;
#endif
#ifdef MG_INT4
        mg_reset(&model);memset(model.cache,0,MG_LAYERS*MG_CONTEXT*128*2*sizeof(float));
        if(!mg_prompt(&model,batch_ids,73) || !(last=mg_prompt(&model,batch_ids+73,87)) || model.position!=160)return 22;
        if(memcmp(expected,last,sizeof expected) || memcmp(cache,model.cache,MG_LAYERS*MG_CONTEXT*128*2*sizeof(float)))return 23;
#ifdef MG_HOST_ALLOC_TEST
        mg_reset(&model);memset(model.cache,0,MG_LAYERS*MG_CONTEXT*128*2*sizeof(float));fail_pair=1;
        last=mg_prompt(&model,batch_ids,160);fail_pair=0;
        if(!last || model.position!=160 || model.prompt_total || memcmp(expected,last,sizeof expected) || memcmp(cache,model.cache,MG_LAYERS*MG_CONTEXT*128*2*sizeof(float)))return 24;
#endif
#endif
        free(cache);
        if(mg_prompt(&model,0,1) || mg_prompt(&model,pair,0) || mg_prompt(&model,pair,-1) || mg_prompt(&model,pair,97) || model.position!=160)return 19;
        pair[0]=-1;if(mg_prompt(&model,pair,2) || model.position!=160)return 20;
#ifdef MG_PREFIX_CACHE
        int cache_error=cached_checks(&model);if(cache_error){fprintf(stderr,"prefix cache check %d failed\n",cache_error);return 25;}
#endif
        mg_close(&model);fclose(f);puts("cancel, bounds, reset PASS");return 0;
    }
    float *logits=0;clock_t start=clock();
    int full_prefill=argc>6 && !strcmp(argv[6],"full-prefill");
    if(full_prefill){for(int i=0;i<count;i++)if(!(logits=mg_forward(&model,ids[i])))return 8;}
    else if(!(logits=mg_prompt(&model,ids,count)))return 8;
    if(argc>5){FILE *out=fopen(argv[5],"wb");if(!out)return 9;fwrite(logits,4,6400,out);fclose(out);}
    int maximum=argc>3?atoi(argv[3]):64;
    for(int i=0;i<maximum && model.position<MG_CONTEXT;i++){
        int token=mg_argmax(logits);fprintf(stderr,"%d ",token);if(token==2 || token==0)break;
        int len;const uint8_t *piece=mg_piece(token,&len);fwrite(piece,1,len,stdout);fflush(stdout);
        if(i+1<maximum){logits=mg_forward(&model,token);if(!logits)return 10;}
    }
    fprintf(stderr,"\npositions=%d resident=%d seconds=%.3f\n",model.position,model.resident_mode,(double)(clock()-start)/CLOCKS_PER_SEC);
    mg_close(&model);fclose(f);return 0;
}
