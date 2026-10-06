"""Generate the reviewable DMA engine overlay used by the default build.

Q/K/V integer projections of the current layer advance while fread prefetches
the next layer through a RAM-only driver callback. All remaining work uses the
original layer math. No thread, reentrant filesystem call or callback GUI work.
The original source is retained for the synchronous reference. The default
build pairs a guarded RAM hook; the older debugger-installed prototype does not.
"""
from pathlib import Path
import difflib
ROOT=Path(__file__).resolve().parents[1]
JOB=r'''
volatile uint32_t mg_dma_status[8]; /* buffer, jobs, pending, overlap, fallback, IRQ errors, swaps, read errors */
static struct {
    const uint8_t *data[3]; float *out[3]; int16_t *q;
    float s; unsigned mat,row,col;
#ifdef MG_INT4
    float sum;
#else
    int32_t sum;
#endif
    int active;
} dma_job;
static int dma_ready;
static void dma_tile(void)
{
    if(dma_job.mat==3)return;
    unsigned rows=dma_job.mat?128:512,end=dma_job.col+128;
    const uint8_t *data=dma_job.data[dma_job.mat];
#ifdef MG_INT4
    const uint16_t *scale=(const uint16_t *)data+dma_job.row*8;
    const uint8_t *w=data+rows*8*2+dma_job.row*256;
    for(unsigned i=dma_job.col;i<end;i+=64)
        dma_job.sum+=(float)mg_q4_dot(w+i/2,dma_job.q+i,64)*mg_q4_scale(scale[i/64]);
#else
    const int8_t *w=(const int8_t *)(data+rows*4)+dma_job.row*512;
#ifdef MG_MXU
    dma_job.sum+=mg_dot(w+dma_job.col,dma_job.q+dma_job.col,128);
#else
    for(unsigned i=dma_job.col;i<end;i++)dma_job.sum+=(int32_t)w[i]*dma_job.q[i];
#endif
#endif
    dma_job.col=end;
    if(end==512){
#ifdef MG_INT4
        dma_job.out[dma_job.mat][dma_job.row]=dma_job.sum*dma_job.s;
#else
        const float *scale=(const float *)data;
        dma_job.out[dma_job.mat][dma_job.row]=(float)dma_job.sum*(scale[dma_job.row]*dma_job.s);
#endif
        dma_job.col=0;dma_job.sum=0;dma_job.row++;
        if(dma_job.row==rows){dma_job.row=0;dma_job.mat++;}
    }
}
void dma_probe_work(void)
{
    if(!dma_job.active || dma_job.mat==3)return;
    uint32_t sr=0;
#ifndef MG_DMA_HOST
    __asm__ volatile("mfc0 %0,$12":"=r"(sr));
#endif
    if(sr&1)mg_dma_status[5]++;
    if(
#ifdef MG_DMA_HOST
       1
#else
       *(volatile uint32_t *)0xb30200a8u
#endif
    ){mg_dma_status[2]++;dma_tile();mg_dma_status[3]++;}
}
static void dma_prepare(mg_model *m,const uint8_t **p)
{
    float *z=m->work+512; rms(z,m->work,(const float *)p[0],512);
    dma_job.data[0]=p[1];dma_job.data[1]=p[2];dma_job.data[2]=p[3];
    dma_job.out[0]=m->work+1024;dma_job.out[1]=m->work+2560;dma_job.out[2]=m->work+2688;
    dma_job.q=(int16_t *)(m->work+512*5+128*2+1408*2+MG_CONTEXT+6400);
    float max=0;for(int i=0;i<512;i++){float a=z[i]<0?-z[i]:z[i];if(a>max)max=a;}
    dma_job.s=max>1e-20f?max*(1.0f/4095.0f):1.0f;float inv=1.0f/dma_job.s;
    for(int i=0;i<512;i++)dma_job.q[i]=(int16_t)(z[i]*inv+(z[i]>=0?0.5f:-0.5f));
    dma_job.mat=dma_job.row=dma_job.col=0;dma_job.sum=0;dma_job.active=1;mg_dma_status[1]++;
}
'''
LOOP=r'''
    if(!m->resident && m->layer_next){
        const uint8_t *p[9],*next[9];if(!read_layer(m,0,p))return 0;
        for(int l=0;l<8;l++){
            if(l<7){
                if(m->yield && m->yield(m->ui,l,0))return 0;
#ifdef MG_LAYER_CACHE
                if(m->layer_cache[l+1]){
                    if(!read_layer(m,l+1,next))return 0;
                }else
#endif
                {
                dma_prepare(m,p);
                uint8_t *current=m->layer;m->layer=m->layer_next;
                int ok=read_layer(m,l+1,next);m->layer=current;dma_job.active=0;
                if(!ok){mg_dma_status[7]++;return 0;}
                while(dma_job.mat<3){dma_tile();mg_dma_status[4]++;}
                dma_ready=1;
                }
            }
            if(!layer(m,l,m->position,p))return 0;
            if(l<7){uint8_t *current=m->layer;m->layer=m->layer_next;m->layer_next=current;memcpy(p,next,sizeof p);mg_dma_status[6]++;}
        }
    }else{
        for(int l=0;l<8;l++){
            const uint8_t *p[9];if(!read_layer(m,l,p) || !layer(m,l,m->position,p))return 0;
        }
    }
'''
def write_overlay(out:Path, auto_hook=False):
    original=(ROOT/'experiments/minigpt/engine.c').read_text(encoding='utf-8');s=original
    def replace(old,new):
        nonlocal s
        assert s.count(old)==1,old
        s=s.replace(old,new)
    replace('    free(m->resident);free(m->cache);free(m->work);','    free(m->layer_next);\n    free(m->resident);free(m->cache);free(m->work);')
    replace('    return 0;\n}\nstatic void rms','    if(!m->resident)m->layer_next=malloc(m->layer_bytes);\n'
            '#ifdef MG_LAYER_CACHE\n'
            '    if(!mg_layer_cache_init(m)){mg_close(m);return -1;}\n'
            '#endif\n    return 0;\n}\nstatic void rms')
    replace('static void rotary(float *x,int heads,const float *cs)',JOB+'\nstatic void rotary(float *x,int heads,const float *cs)')
    replace('        if(m->yield && m->yield(m->ui,l,0))return 0;\n        rms(z,x,(const float *)p[0],512);\n        matrix(q,p[1],z,512,512,qx);matrix(k,p[2],z,128,512,qx);matrix(v,p[3],z,128,512,qx);',
            '        if(!dma_ready){\n        if(m->yield && m->yield(m->ui,l,0))return 0;\n        rms(z,x,(const float *)p[0],512);\n        matrix(q,p[1],z,512,512,qx);matrix(k,p[2],z,128,512,qx);matrix(v,p[3],z,128,512,qx);\n        }\n        dma_ready=0;')
    replace('    for(int l=0;l<8;l++){\n        const uint8_t *p[9];if(!read_layer(m,l,p) || !layer(m,l,m->position,p))return 0;\n    }',LOOP)
    replace('    embed(m,m->work,token);','    mg_dma_status[0]=m->layer_next!=0;\n    embed(m,m->work,token);')
    if auto_hook:
        replace('#include "perf.h"','#include "perf.h"\n#include "dma_hook.h"')
        replace('                int ok=read_layer(m,l+1,next);m->layer=current;dma_job.active=0;',
                '                int hooked=mg_dma_hook_enter();\n'
                '                int ok=read_layer(m,l+1,next);m->layer=current;dma_job.active=0;\n'
                '                if(hooked && !mg_dma_hook_leave())ok=0;')
    # Header lookup is shared with app.c, so the extra model field must be in
    # engine.h behind MG_DMA_PIPELINE. Keep the original header in the overlay.
    (out/'engine.h').write_bytes((ROOT/'experiments/minigpt/engine.h').read_bytes())
    (out/'engine-dma.c').write_text(s,encoding='utf-8')
    (out/'engine-dma.patch').write_text(''.join(difflib.unified_diff(original.splitlines(True),s.splitlines(True),fromfile='engine.c',tofile='engine-dma.c')),encoding='utf-8')
    return out/'engine-dma.c'
