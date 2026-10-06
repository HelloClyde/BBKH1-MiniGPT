#ifndef MG_MXU_DOT_H
#define MG_MXU_DOT_H
#include <stdint.h>
/* backend (1 MXU, -1 scalar), vector calls, terms, scalar calls, selftest errors */
extern volatile uint32_t mg_mxu_status[5];
void mg_mxu_init(void);
int32_t mg_dot(const int8_t *weights,const int16_t *activation,unsigned count);
/* Internal leaf: aligned input, count > 0 and divisible by 4, count <= 1408,
 * |activation| <= 4095. Restores XR1..9, MXU control and exact CP0 Status. */
int32_t mg_mxu_dot(const int8_t *,const int16_t *,unsigned);
#endif
