#include "mxu_dot.h"
#include <stddef.h>
volatile uint32_t mg_mxu_status[5];
static int32_t scalar(const int8_t *w,const int16_t *q,unsigned n)
{
    int32_t sum=0;unsigned i=0;
    for(;i+4<=n;i+=4){sum+=(int32_t)w[i]*q[i];sum+=(int32_t)w[i+1]*q[i+1];sum+=(int32_t)w[i+2]*q[i+2];sum+=(int32_t)w[i+3]*q[i+3];}
    for(;i<n;i++)sum+=(int32_t)w[i]*q[i];return sum;
}
void mg_mxu_init(void)
{
    if(mg_mxu_status[0])return;
    /* Small RAM-only tests also cover negative weights, alternating signs,
     * all packed byte positions, and the model's extreme activation values. */
    union {uint32_t align;int8_t v[32];} w;
    union {uint32_t align;int16_t v[32];} q;
    static const int8_t edge[]={-128,-127,-1,0,1,126,127};
    static const int16_t activation[]={-4095,-4094,-1,0,1,4094,4095};
    mg_mxu_status[0]=(uint32_t)-1;
    for(unsigned seed=0;seed<14;seed++){
        for(unsigned i=0;i<32;i++){w.v[i]=edge[(i+seed)%7];q.v[i]=activation[(i*3+seed)%7];}
        for(unsigned n=4;n<=32;n+=4)
            if(mg_mxu_dot(w.v,q.v,n)!=scalar(w.v,q.v,n)){mg_mxu_status[4]++;return;}
    }
    mg_mxu_status[0]=1;
}
int32_t mg_dot(const int8_t *w,const int16_t *q,unsigned n)
{
#ifdef MG_INT4
    const unsigned minimum=64;
#else
    const unsigned minimum=128;
#endif
    if(mg_mxu_status[0]==1 && n>=minimum && n<=1408 && !(n&3) && !(((uintptr_t)w|(uintptr_t)q)&3)){
        mg_mxu_status[1]++;mg_mxu_status[2]+=n;return mg_mxu_dot(w,q,n);
    }
    mg_mxu_status[3]++;return scalar(w,q,n);
}
