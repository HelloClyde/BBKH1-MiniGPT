/* Host-only substitute for the hardware leaf. The actual packed MIPS leaf
 * has its separate instruction/state tests; this permits full-model tests
 * of the exact dispatcher, matrix calls and DMA callback integration. */
#include "mxu_dot.h"
#include <assert.h>
int32_t mg_mxu_dot(const int8_t *w,const int16_t *q,unsigned n)
{
    assert(n && n<=1408 && !(n&3) && !(((uintptr_t)w|(uintptr_t)q)&3));
    int64_t sum=0;
    for(unsigned i=0;i<n;i++){assert(q[i]>=-4095 && q[i]<=4095);sum+=(int64_t)w[i]*q[i];}
    assert(sum>=-2147483647 && sum<=2147483647);return (int32_t)sum;
}
