/* Independent host substitute for the packed MXU leaf; never built into BDA. */
#include "q4_dot.h"
#include <assert.h>
int32_t mg_mxu_q4_dot(const uint8_t *p,const int16_t *q,unsigned count)
{
    assert(count>=64 && count<=1408 && !(count&15) && !(((uintptr_t)p|(uintptr_t)q)&3));
    int64_t sum=0;
    for(unsigned i=0;i<count;i++){unsigned raw=(p[i/2]>>(4*(i&1)))&15;int w=raw>=8?(int)raw-16:(int)raw;sum+=(int64_t)w*q[i];}
    assert(sum>=INT32_MIN && sum<=INT32_MAX);return (int32_t)sum;
}

void mg_mxu_q4_pair(const uint8_t *p,const int16_t *q0,const int16_t *q1,int32_t *out)
{out[0]=mg_mxu_q4_dot(p,q0,64);out[1]=mg_mxu_q4_dot(p,q1,64);}
