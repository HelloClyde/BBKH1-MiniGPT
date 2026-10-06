#include "q4_dot.h"
#ifdef MG_MXU
#include "mxu_dot.h"
#endif
float mg_q4_scale(uint16_t half)
{
    unsigned mantissa=half&1023,exponent=(half>>10)&31;
    union {uint32_t u;float f;} value;
    if(!exponent){
        if(!mantissa){value.u=0;return value.f;}
        exponent=113;while(!(mantissa&1024)){mantissa<<=1;exponent--;}
        mantissa&=1023;
    }else exponent+=112;
    value.u=(exponent<<23)|(mantissa<<13);return value.f;
}
int mg_q4_valid(const uint16_t *scales,unsigned count)
{for(unsigned i=0;i<count;i++)if(!scales[i] || scales[i]>0x5640u)return 0;return 1;}
float mg_q4_row(const uint8_t *packed,const uint16_t *scales,const int16_t *q,unsigned count)
{
    float result=0;
    for(unsigned i=0;i<count;i+=64)result+=(float)mg_q4_dot(packed+i/2,q+i,64)*mg_q4_scale(scales[i/64]);
    return result;
}
volatile uint32_t mg_q4_pair_status[3];
void mg_q4_row_pair(const uint8_t *packed,const uint16_t *scales,const int16_t *q0,const int16_t *q1,unsigned count,float *out)
{
    float a=0,b=0;
    for(unsigned i=0;i<count;i+=64){
        int32_t sums[2];
#ifdef MG_MXU
        if(mg_mxu_status[0]==1 && !(((uintptr_t)packed|(uintptr_t)q0|(uintptr_t)q1)&3)){
            mg_mxu_q4_pair(packed+i/2,q0+i,q1+i,sums);
            mg_q4_pair_status[0]++;mg_q4_pair_status[1]+=128;
            mg_mxu_status[1]+=2;mg_mxu_status[2]+=128;
        }else
#endif
        {sums[0]=mg_q4_dot(packed+i/2,q0+i,64);sums[1]=mg_q4_dot(packed+i/2,q1+i,64);mg_q4_pair_status[2]++;}
        float scale=mg_q4_scale(scales[i/64]);
        a+=(float)sums[0]*scale;b+=(float)sums[1]*scale;
    }
    out[0]=a;out[1]=b;
}
int32_t mg_q4_dot(const uint8_t *packed,const int16_t *q,unsigned count)
{
#ifdef MG_MXU
    if(mg_mxu_status[0]==1 && count>=64 && count<=1408 && !(count&15) && !(((uintptr_t)packed|(uintptr_t)q)&3)){
        mg_mxu_status[1]++;mg_mxu_status[2]+=count;return mg_mxu_q4_dot(packed,q,count);
    }
#endif
    /* One unpacked row only; the full model/layer stays packed in RAM. */
    union {uint32_t align;int8_t v[1408];} row;
    if(!count || count>1408 || (count&1))return 0;
    for(unsigned i=0;i<count;i+=2){unsigned b=packed[i/2];row.v[i]=(int)((b&15)^8)-8;row.v[i+1]=(int)((b>>4)^8)-8;}
#ifdef MG_MXU
    return mg_dot(row.v,q,count);
#else
    int32_t sum=0;for(unsigned i=0;i<count;i++)sum+=(int32_t)row.v[i]*q[i];return sum;
#endif
}
int mg_q4_selftest(void)
{
    union {uint32_t align;uint8_t v[64];} p;
    union {uint32_t align;int16_t v[128];} q;
    for(unsigned seed=0;seed<16;seed++){
        int32_t reference=0;
        for(unsigned i=0;i<128;i++){
            int w=(int)((i+seed)&15)-8;q.v[i]=i&1?-4095:4095;
            if(!(i&1))p.v[i/2]=(uint8_t)(w&15);else p.v[i/2]|=(uint8_t)((w&15)<<4);
            reference+=w*q.v[i];
        }
        if(mg_q4_dot(p.v,q.v,128)!=reference)return 0;
    }
    float actual[2];uint16_t scales[2]={0x2000,0x3000};
    mg_q4_row_pair(p.v,scales,q.v,q.v,128,actual);
    float reference=mg_q4_row(p.v,scales,q.v,128);
    if(actual[0]!=reference || actual[1]!=reference)return 0;
    return 1;
}
