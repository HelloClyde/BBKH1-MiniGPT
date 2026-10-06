#include "engine.h"
#include "perf.h"
#ifdef MG_INT4
#include "q4_dot.h"
#endif
#ifdef MG_MXU
#include "mxu_dot.h"
#endif
#include <stdlib.h>
#include <string.h>

float mg_rsqrt(float x)
{
    union {float f;uint32_t u;} a={x};
    a.u=0x5f3759dfu-(a.u>>1);
    float y=a.f,h=x*0.5f;
    y=y*(1.5f-h*y*y);y=y*(1.5f-h*y*y);y=y*(1.5f-h*y*y);
    return y;
}
float mg_exp(float x)
{
    if(x<-80.0f)return 0.0f;
    if(x>80.0f)x=80.0f;
    float z=x*1.4426950408889634f;
    int n=(int)(z+(z>=0?0.5f:-0.5f));
    float r=(x-n*0.693145751953125f)-n*1.428606765330187e-6f;
    float p=1.0f+r*(1.0f+r*(0.5f+r*(0.1666666667f+r*(0.0416666667f+r*(0.0083333333f+r*(0.0013888889f+r*0.0001984127f))))));
    union {uint32_t u;float f;} a={(uint32_t)(n+127)<<23};
    return p*a.f;
}
static int valid_floats(const void *p,unsigned n,float limit,int positive)
{
    const float *a=(const float *)p;
    for(unsigned i=0;i<n;i++){
        if(!(a[i]>=-limit && a[i]<=limit) || (positive && !(a[i]>0)))return 0;
    }
    return 1;
}
static int valid_tensor(const mg_tensor *t,const void *p)
{
#ifdef MG_INT4
#ifdef MG_Q4_EMBED_INT8
    if(t->kind==2)return valid_floats(p,t->rows,100.0f,1);
#endif
    if(t->kind)return mg_q4_valid(p,t->rows*(t->cols/64));
#endif
    return valid_floats(p,t->rows,t->kind?100.0f:1000.0f,t->kind!=0);
}
static uint32_t crc_table[256];
static uint32_t crc_update(uint32_t crc,const uint8_t *p,uint32_t bytes)
{while(bytes--)crc=(crc>>8)^crc_table[(crc^*p++)&255];return crc;}
void mg_close(mg_model *m)
{
#ifdef MG_LAYER_CACHE
    for(unsigned l=0;l<MG_LAYERS;l++)free(m->layer_cache[l]);
#endif
    if(!m->resident){free(m->embedding);free(m->layer);}
    free(m->resident);free(m->cache);free(m->work);
    memset(m,0,sizeof *m);
}
#ifdef MG_LAYER_CACHE
/* Optional complete layers, after all required work/DMA buffers. Hold a
 * contiguous reserve while allocating so prompt batches and the system IME
 * still have room. A failed optional allocation leaves streaming usable. */
static int mg_layer_cache_init(mg_model *m)
{
    if(m->resident)return 1;
    void *reserve=malloc(MG_LAYER_CACHE_RESERVE);
    if(!reserve)return 1;
    for(unsigned l=0;l<MG_LAYERS;l++){
        m->layer_cache_warming=l+1;
        uint8_t *data=(uint8_t *)malloc(m->layer_bytes);
        if(!data)break;
        m->layer_cache[l]=data;
        unsigned base=3+l*9;
        if(!m->read(m->io,m->t[base].offset,data,m->layer_bytes)){
            /* The model already passed its complete CRC scan. A cancelled or
             * incomplete OPTIONAL warmup must not destroy that valid model.
             * Discard only the partially filled layer; keep earlier caches. */
            free(data);m->layer_cache[l]=0;m->layer_cache_read_aborts++;break;
        }
        for(unsigned j=0;j<9;j++)
            if(!valid_tensor(&m->t[base+j],data+m->t[base+j].offset-m->t[base].offset)){free(reserve);return 0;}
        m->cached_layers++;
    }
    m->layer_cache_warming=0;free(reserve);return 1;
}
#endif
void mg_reset(mg_model *m){
    m->position=m->prompt_done=m->prompt_total=0;
#ifdef MG_PREFIX_CACHE
    m->prefix_tokens=m->prompt_reused=0;
#endif
}
int mg_load(mg_model *m,mg_reader read,void *io,int try_resident)
{
    uint32_t h[16];memset(m,0,sizeof *m);m->read=read;m->io=io;
    if(!read(io,0,h,64) || memcmp(h,MG_MODEL_MAGIC,8) || h[2]!=MG_MODEL_VERSION || h[3]!=512 || h[4]!=1408 || h[5]!=8 || h[6]!=8 || h[7]!=2 || h[8]!=6400 || h[9]!=256 || h[10]!=75 || h[11]!=MG_MODEL_BYTES)return -1;
#ifdef MG_INT4
    if(h[14]!=64 || h[15]!=16)return -1;
#endif
    union {uint32_t u;float f;} eps={h[13]};if(eps.f!=1e-5f)return -1;
    if(!read(io,64,m->t,sizeof m->t))return -1;
    uint32_t end=64+sizeof m->t;
    for(unsigned i=0;i<75;i++){
        unsigned rows,cols,kind;
        if(i==0){rows=6400;cols=512;kind=1;}
        else if(i==1){rows=512;cols=1;kind=0;}
        else if(i==2){rows=256*64;cols=1;kind=0;}
        else {
            unsigned j=(i-3)%9;kind=j!=0 && j!=5;
            rows=j==2 || j==3?128:j==6 || j==7?1408:512;
            cols=!kind?1:j==8?1408:512;
        }
        mg_tensor *t=&m->t[i];unsigned bytes=kind?rows*4+rows*cols:rows*4;
#ifdef MG_INT4
        if(kind)bytes=rows*(cols/64)*2+rows*cols/2;
#endif
#ifdef MG_Q4_EMBED_INT8
        if(i==0){kind=2;bytes=rows*4+rows*cols;}
#endif
        if(t->offset!=end || t->rows!=rows || t->cols!=cols || t->kind!=kind || t->bytes!=bytes)return -1;
        end+=bytes;
    }
    if(end!=h[11])return -1;
    for(unsigned i=0;i<256;i++){uint32_t c=i;for(int j=0;j<8;j++)c=(c>>1)^((c&1)?0xedb88320u:0);crc_table[i]=c;}
    m->file_bytes=end;m->layer_bytes=m->t[12].offset-m->t[3].offset;
    m->cache=(float *)malloc((size_t)MG_LAYERS*MG_CONTEXT*128*2*sizeof(float));
    m->work=(float *)malloc(MG_WORK_BYTES);
    if(!m->cache || !m->work){mg_close(m);return -2;}
    if(try_resident)m->resident=(uint8_t *)malloc(end);
    if(m->resident){
        if(!read(io,0,m->resident,end)){mg_close(m);return -1;}
        if((crc_update(0xffffffffu,m->resident+64,end-64)^0xffffffffu)!=h[12]){mg_close(m);return -1;}
        m->resident_mode=1;m->embedding=m->resident+m->t[0].offset;
        for(unsigned i=0;i<75;i++)if(!valid_tensor(&m->t[i],m->resident+m->t[i].offset)){mg_close(m);return -1;}
    }else{
        m->embedding=(uint8_t *)malloc(m->t[0].bytes);
        m->layer=(uint8_t *)malloc(m->layer_bytes);
        if(!m->embedding || !m->layer){mg_close(m);return -2;}
        uint32_t crc=0xffffffffu;
        for(uint32_t off=64;off<end;){unsigned n=end-off>65536?65536:end-off;if(!read(io,off,m->layer,n)){mg_close(m);return -1;}crc=crc_update(crc,m->layer,n);off+=n;}
        if((crc^0xffffffffu)!=h[12]){mg_close(m);return -1;}
        if(!read(io,m->t[0].offset,m->embedding,m->t[0].bytes) || !valid_tensor(&m->t[0],m->embedding)){mg_close(m);return -1;}
    }
    if(!read(io,m->t[1].offset,m->norm,sizeof m->norm) || !read(io,m->t[2].offset,m->rope,sizeof m->rope) || !valid_floats(m->norm,512,1000,0) || !valid_floats(m->rope,256*64,1.01f,0)){mg_close(m);return -1;}
#ifdef MG_MXU
    mg_mxu_init();
#endif
#ifdef MG_INT4
    if(!mg_q4_selftest()){mg_close(m);return -1;}
#endif
#if defined(MG_LAYER_CACHE) && !defined(MG_DMA_PIPELINE)
    if(!mg_layer_cache_init(m)){mg_close(m);return -1;}
#endif
    return 0;
}
static void rms(float *out,const float *x,const float *weight,int n)
{
    float sum=0;for(int i=0;i<n;i++)sum+=x[i]*x[i];
    float scale=mg_rsqrt(sum/n+1e-5f);
    for(int i=0;i<n;i++)out[i]=x[i]*scale*weight[i];
}
static void matrix(float *out,const uint8_t *data,const float *x,int rows,int cols,int16_t *q)
{
    mg_perf_enter(MG_PERF_MATRIX);
#ifdef MG_INT4
    const uint16_t *scale=(const uint16_t *)data;const uint8_t *w=data+rows*(cols/64)*2;
#else
    const float *scale=(const float *)data;const int8_t *w=(const int8_t *)(data+rows*4);
#endif
    float max=0;
    for(int i=0;i<cols;i++){float a=x[i]<0?-x[i]:x[i];if(a>max)max=a;}
    float s=max>1e-20f?max*(1.0f/4095.0f):1.0f,inv=1.0f/s;
    for(int i=0;i<cols;i++)q[i]=(int16_t)(x[i]*inv+(x[i]>=0?0.5f:-0.5f));
    for(int row=0;row<rows;row++){
        /* cols <= 1408: 1408 * 127 * 4095 < INT32_MAX. */
#ifdef MG_INT4
        out[row]=mg_q4_row(w+row*(cols/2),scale+row*(cols/64),q,(unsigned)cols)*s;
#else
        int32_t sum=0;
        const int8_t *p=w+row*cols;
#ifdef MG_MXU
        sum=mg_dot(p,q,(unsigned)cols);
#else
        for(int j=0;j<cols;j+=4){sum+=(int32_t)p[j]*q[j];sum+=(int32_t)p[j+1]*q[j+1];sum+=(int32_t)p[j+2]*q[j+2];sum+=(int32_t)p[j+3]*q[j+3];}
#endif
        out[row]=(float)sum*(scale[row]*s);
#endif
    }
    mg_perf_leave();
}
#ifdef MG_Q4_EMBED_INT8
static void classifier_q8(float *out,const uint8_t *data,const float *x,int16_t *q)
{
    mg_perf_enter(MG_PERF_MATRIX);
    const float *scale=(const float *)data;const int8_t *w=(const int8_t *)(data+6400*4);
    float max=0;for(int i=0;i<512;i++){float a=x[i]<0?-x[i]:x[i];if(a>max)max=a;}
    float s=max>1e-20f?max*(1.0f/4095.0f):1.0f,inv=1.0f/s;
    for(int i=0;i<512;i++)q[i]=(int16_t)(x[i]*inv+(x[i]>=0?0.5f:-0.5f));
    for(int row=0;row<6400;row++){
        const int8_t *p=w+row*512;int32_t sum=0;
#ifdef MG_MXU
        sum=mg_dot(p,q,512);
#else
        for(int i=0;i<512;i+=4){sum+=(int32_t)p[i]*q[i];sum+=(int32_t)p[i+1]*q[i+1];sum+=(int32_t)p[i+2]*q[i+2];sum+=(int32_t)p[i+3]*q[i+3];}
#endif
        out[row]=(float)sum*(scale[row]*s);
    }
    mg_perf_leave();
}
#endif
static void rotary(float *x,int heads,const float *cs)
{
    for(int h=0;h<heads;h++)for(int i=0;i<32;i++){
        float a=x[h*64+i],b=x[h*64+i+32],c=cs[i],s=cs[i+32];
        x[h*64+i]=a*c-b*s;x[h*64+i+32]=b*c+a*s;
    }
}
static int read_layer(mg_model *m,int l,const uint8_t **p)
{
        int base=3+l*9;
#ifdef MG_LAYER_CACHE
        if(!m->resident && m->layer_cache[l]){
            const uint8_t *data=m->layer_cache[l];m->layer_cache_hits++;
            for(int j=0;j<9;j++)p[j]=data+(m->t[base+j].offset-m->t[base].offset);
            return 1;
        }
        if(!m->resident)m->layer_cache_misses++;
#endif
        if(!m->resident){
            mg_perf_enter(MG_PERF_READ);
            int ok=m->read(m->io,m->t[base].offset,m->layer,m->layer_bytes);
            mg_perf_bytes(m->layer_bytes);mg_perf_leave();if(!ok)return 0;
        }
        for(int j=0;j<9;j++){
            p[j]=m->resident?m->resident+m->t[base+j].offset:m->layer+(m->t[base+j].offset-m->t[base].offset);
            if(!m->resident && !valid_tensor(&m->t[base+j],p[j]))return 0;
        }
        return 1;
}
static void embed(mg_model *m,float *x,int token)
{
#ifdef MG_Q4_EMBED_INT8
    const float *es=(const float *)m->embedding;const int8_t *ew=(const int8_t *)(m->embedding+6400*4);
    for(int i=0;i<512;i++)x[i]=ew[token*512+i]*es[token];
#elif defined(MG_INT4)
    const uint16_t *scales=(const uint16_t *)m->embedding+token*8;const uint8_t *row=m->embedding+6400*8*2+token*256;
    for(int group=0;group<8;group++){float s=mg_q4_scale(scales[group]);for(int j=0;j<64;j+=2){int i=group*64+j;unsigned b=row[i/2];x[i]=((int)((b&15)^8)-8)*s;x[i+1]=((int)((b>>4)^8)-8)*s;}}
#else
    const float *es=(const float *)m->embedding;const int8_t *ew=(const int8_t *)(m->embedding+6400*4);
    for(int i=0;i<512;i++)x[i]=ew[token*512+i]*es[token];
#endif
}
static void attention(mg_model *m,int l,int pos,float *q,float *k,float *v,float *a,float *scores)
{
        rotary(q,8,m->rope+pos*64);rotary(k,2,m->rope+pos*64);
        float *kc=m->cache+(l*2)*MG_CONTEXT*128,*vc=kc+MG_CONTEXT*128;
        memcpy(kc+pos*128,k,128*4);memcpy(vc+pos*128,v,128*4);
        mg_perf_enter(MG_PERF_ATTENTION);
        for(int h=0;h<8;h++){
            int kh=h/4;float max=-1e30f,sum=0;
            for(int t=0;t<=pos;t++){
                float s=0;for(int i=0;i<64;i++)s+=q[h*64+i]*kc[t*128+kh*64+i];
                scores[t]=s*0.125f;if(scores[t]>max)max=scores[t];
            }
            for(int t=0;t<=pos;t++){scores[t]=mg_exp(scores[t]-max);sum+=scores[t];}
            for(int i=0;i<64;i++)a[h*64+i]=0;
            float inv=1.0f/sum;
            for(int t=0;t<=pos;t++){float s=scores[t]*inv;for(int i=0;i<64;i++)a[h*64+i]+=s*vc[t*128+kh*64+i];}
        }
        mg_perf_leave();
}
static int layer(mg_model *m,int l,int pos,const uint8_t **p)
{
    float *x=m->work,*z=x+512,*q=z+512,*a=q+512,*o=a+512,*k=o+512,*v=k+128,*gate=v+128,*up=gate+1408,*scores=up+1408,*logits=scores+MG_CONTEXT;
    int16_t *qx=(int16_t *)(logits+6400);
        if(m->yield && m->yield(m->ui,l,0))return 0;
        rms(z,x,(const float *)p[0],512);
        matrix(q,p[1],z,512,512,qx);matrix(k,p[2],z,128,512,qx);matrix(v,p[3],z,128,512,qx);
        attention(m,l,pos,q,k,v,a,scores);
        matrix(o,p[4],a,512,512,qx);for(int i=0;i<512;i++)x[i]+=o[i];
        rms(z,x,(const float *)p[5],512);
        if(m->yield && m->yield(m->ui,l,1))return 0;
        matrix(gate,p[6],z,1408,512,qx);matrix(up,p[7],z,1408,512,qx);
        for(int i=0;i<1408;i++)gate[i]=gate[i]/(1.0f+mg_exp(-gate[i]))*up[i];
        matrix(o,p[8],gate,512,1408,qx);for(int i=0;i<512;i++)x[i]+=o[i];
    return 1;
}

#ifdef MG_INT4
/* One extra 51 KiB work area; weight bytes remain packed and shared. */
typedef struct {float *x,*z,*q,*a,*o,*k,*v,*gate,*up,*scores;int16_t *qx;} prompt_work;
static prompt_work prompt_workspace(float *w)
{
    prompt_work s={w,w+512,w+1024,w+1536,w+2048,w+2560,w+2688,w+2816,w+4224,w+5632,(int16_t *)(w+MG_WORK_FLOATS)};
    return s;
}
static float quantize_input(int16_t *q,const float *x,int cols)
{
    float max=0;
    for(int i=0;i<cols;i++){float a=x[i]<0?-x[i]:x[i];if(a>max)max=a;}
    float s=max>1e-20f?max*(1.0f/4095.0f):1.0f,inv=1.0f/s;
    for(int i=0;i<cols;i++)q[i]=(int16_t)(x[i]*inv+(x[i]>=0?0.5f:-0.5f));
    return s;
}
static void matrix_pair(float *out0,float *out1,const uint8_t *data,const float *x0,const float *x1,int rows,int cols,int16_t *q0,int16_t *q1)
{
    mg_perf_enter(MG_PERF_MATRIX);
    float s0=quantize_input(q0,x0,cols),s1=quantize_input(q1,x1,cols);
    const uint16_t *scale=(const uint16_t *)data;
    const uint8_t *w=data+rows*(cols/64)*2;
    for(int row=0;row<rows;row++){
        float sums[2];mg_q4_row_pair(w+row*(cols/2),scale+row*(cols/64),q0,q1,(unsigned)cols,sums);
        out0[row]=sums[0]*s0;out1[row]=sums[1]*s1;
    }
    mg_perf_leave();
}
static int layer_pair(mg_model *m,int l,int pos,const uint8_t **p,float *second)
{
    prompt_work s[2]={prompt_workspace(m->work),prompt_workspace(second)};
    if(m->yield && m->yield(m->ui,l,0))return 0;
    for(int t=0;t<2;t++)rms(s[t].z,s[t].x,(const float *)p[0],512);
    matrix_pair(s[0].q,s[1].q,p[1],s[0].z,s[1].z,512,512,s[0].qx,s[1].qx);
    matrix_pair(s[0].k,s[1].k,p[2],s[0].z,s[1].z,128,512,s[0].qx,s[1].qx);
    matrix_pair(s[0].v,s[1].v,p[3],s[0].z,s[1].z,128,512,s[0].qx,s[1].qx);
    for(int t=0;t<2;t++)attention(m,l,pos+t,s[t].q,s[t].k,s[t].v,s[t].a,s[t].scores);
    matrix_pair(s[0].o,s[1].o,p[4],s[0].a,s[1].a,512,512,s[0].qx,s[1].qx);
    for(int t=0;t<2;t++){
        for(int i=0;i<512;i++)s[t].x[i]+=s[t].o[i];
        rms(s[t].z,s[t].x,(const float *)p[5],512);
    }
    if(m->yield && m->yield(m->ui,l,1))return 0;
    matrix_pair(s[0].gate,s[1].gate,p[6],s[0].z,s[1].z,1408,512,s[0].qx,s[1].qx);
    matrix_pair(s[0].up,s[1].up,p[7],s[0].z,s[1].z,1408,512,s[0].qx,s[1].qx);
    for(int t=0;t<2;t++)for(int i=0;i<1408;i++)s[t].gate[i]=s[t].gate[i]/(1.0f+mg_exp(-s[t].gate[i]))*s[t].up[i];
    matrix_pair(s[0].o,s[1].o,p[8],s[0].gate,s[1].gate,512,1408,s[0].qx,s[1].qx);
    for(int t=0;t<2;t++)for(int i=0;i<512;i++)s[t].x[i]+=s[t].o[i];
    return 1;
}
#endif
static float *final_logits(mg_model *m,int output)
{
    float *x=m->work,*z=x+512,*logits=x+512*5+128*2+1408*2+MG_CONTEXT;
    int16_t *qx=(int16_t *)(logits+6400);
    if(output){rms(z,x,m->norm,512);
#ifdef MG_Q4_EMBED_INT8
        classifier_q8(logits,m->embedding,z,qx);
#else
        matrix(logits,m->embedding,z,6400,512,qx);
#endif
    }
    return logits;
}
static float *step(mg_model *m,int token,int output)
{
    if(token<0 || token>=6400 || m->position>=MG_CONTEXT || !m->work)return 0;
    embed(m,m->work,token);
    for(int l=0;l<8;l++){
        const uint8_t *p[9];if(!read_layer(m,l,p) || !layer(m,l,m->position,p))return 0;
    }
    float *logits=final_logits(m,output);m->position++;return logits;
}
float *mg_forward(mg_model *m,int token){return step(m,token,1);}
int mg_prefill(mg_model *m,int token){return step(m,token,0)!=0;}
float *mg_prompt(mg_model *m,const int *tokens,int count)
{
    if(!tokens || count<=0 || count>MG_CONTEXT-m->position || !m->work)return 0;
    for(int i=0;i<count;i++)if(tokens[i]<0 || tokens[i]>=MG_VOCAB)return 0;
    if(count==1)return mg_forward(m,tokens[0]);
    float *batch=malloc((size_t)count*MG_DIM*sizeof(float));
    if(!batch){
        m->prompt_total=count;
        for(int i=0;i<count-1;i++){
            m->prompt_done=i*MG_LAYERS;
            if(!mg_prefill(m,tokens[i])){m->prompt_total=0;return 0;}
        }
        m->prompt_done=(count-1)*MG_LAYERS;float *logits=mg_forward(m,tokens[count-1]);
        m->prompt_total=0;return logits;
    }
#ifdef MG_INT4
    float *second=malloc(MG_WORK_BYTES);
#endif
    for(int i=0;i<count;i++)embed(m,batch+i*MG_DIM,tokens[i]);
    m->prompt_done=0;m->prompt_total=count;
    for(int l=0;l<MG_LAYERS;l++){
        const uint8_t *p[9];if(!read_layer(m,l,p))goto fail;
        for(int i=0;i<count;i++){
#ifdef MG_INT4
            if(second && i+1<count){
                memcpy(m->work,batch+i*MG_DIM,MG_DIM*sizeof(float));
                memcpy(second,batch+(i+1)*MG_DIM,MG_DIM*sizeof(float));
                m->prompt_done=l*count+i;
                if(!layer_pair(m,l,m->position+i,p,second))goto fail;
                memcpy(batch+i*MG_DIM,m->work,MG_DIM*sizeof(float));
                memcpy(batch+(i+1)*MG_DIM,second,MG_DIM*sizeof(float));
                i++;continue;
            }
#endif
            memcpy(m->work,batch+i*MG_DIM,MG_DIM*sizeof(float));
            m->prompt_done=l*count+i;
            if(!layer(m,l,m->position+i,p))goto fail;
            memcpy(batch+i*MG_DIM,m->work,MG_DIM*sizeof(float));
        }
    }
    memcpy(m->work,batch+(count-1)*MG_DIM,MG_DIM*sizeof(float));
#ifdef MG_INT4
    free(second);
#endif
    /* Only the final prompt token needs vocabulary logits. */
    m->position+=count;m->prompt_done=count*MG_LAYERS;m->prompt_total=0;
    free(batch);return final_logits(m,1);
fail:
#ifdef MG_INT4
    free(second);
#endif
    m->prompt_total=0;free(batch);return 0;
}
#ifdef MG_PREFIX_CACHE
float *mg_prompt_cached(mg_model *m,const int *tokens,int count)
{
    if(!tokens || count<=0 || count>MG_CONTEXT || !m->work)return 0;
    for(int i=0;i<count;i++)if(tokens[i]<0 || tokens[i]>=MG_VOCAB)return 0;
    int shared=0;
    /* Recompute the final token, whose logits were overwritten by decoding. */
    while(shared<count-1 && shared<m->prefix_tokens && tokens[shared]==m->prefix_ids[shared])shared++;
    mg_reset(m);m->position=shared;m->prompt_reused=shared;
    float *logits=mg_prompt(m,tokens+shared,count-shared);
    if(logits){memcpy(m->prefix_ids,tokens,(size_t)count*sizeof(int));m->prefix_tokens=count;}
    return logits;
}
#endif
int mg_argmax(const float *logits)
{
    int best=0;for(int i=1;i<6400;i++)if(logits[i]>logits[best])best=i;return best;
}

#if defined(MG_LAYER_CACHE) && defined(MG_DMA_PIPELINE)
static int tune_read(mg_model *m,unsigned l)
{
    unsigned base=3+l*9;m->layer_cache_warming=l+1;
    int ok=m->read(m->io,m->t[base].offset,m->layer_next,m->layer_bytes);
    m->layer_cache_warming=0;
    if(!ok){m->layer_cache_read_aborts++;return -1;}
    for(unsigned j=0;j<9;j++)
        if(!valid_tensor(&m->t[base+j],m->layer_next+m->t[base+j].offset-m->t[base].offset))return 0;
    return 1;
}
static int tune_probe(mg_model *m,uint32_t (*clock)(void),unsigned phase,uint32_t *ticks)
{
    m->cache_tune_phase=phase;*ticks=0;
    for(unsigned sample=0;sample<2;sample++){
        m->cache_tune_sample=sample;mg_reset(m);
        /* Include UI polling in both measurements. Classifier is identical in
         * both modes and is excluded from this layer-read comparison. */
        uint32_t start=clock();int ok=mg_prefill(m,5134);*ticks+=clock()-start;
        if(!ok)return 0;
    }
    m->cache_tune_sample=2;return 1;
}
int mg_select_layer_cache(mg_model *m,uint32_t (*clock)(void))
{
    unsigned n=m->cached_layers;m->cache_tune_initial_layers=n;
    if(m->resident || !m->layer_next || n>=MG_LAYERS || m->layer_cache_read_aborts)return 1;
    int ok=tune_read(m,n);
    if(ok<=0){m->cache_tune_canceled=ok<0;mg_reset(m);return ok<0;}
    /* Transfer ownership, rather than allocate another layer. */
    m->layer_cache[n]=m->layer_next;m->layer_next=0;m->cached_layers=n+1;
    if(!tune_probe(m,clock,1,&m->cache_tune_sync_ticks)){
        m->cache_tune_selected=1;m->cache_tune_canceled=1;goto done;
    }
    m->layer_next=m->layer_cache[n];m->layer_cache[n]=0;m->cached_layers=n;
    if(!tune_probe(m,clock,2,&m->cache_tune_dma_ticks)){m->cache_tune_canceled=1;goto done;}
    /* Prefer DMA on a tie/noisy clock. A cache win must exceed two percent. */
    if((uint64_t)m->cache_tune_sync_ticks*100u<(uint64_t)m->cache_tune_dma_ticks*98u){
        /* DMA swaps the two scratch pointers. Reload into the CURRENT next
         * buffer; never attach the old pointer, which may now be m->layer. */
        ok=tune_read(m,n);if(ok==0){m->cache_tune_phase=0;mg_reset(m);return 0;}
        if(ok<0){m->cache_tune_canceled=1;goto done;}
        m->layer_cache[n]=m->layer_next;m->layer_next=0;m->cached_layers=n+1;m->cache_tune_selected=1;
    }
done:
    m->cache_tune_phase=m->cache_tune_sample=0;mg_reset(m);return 1;
}
#endif
