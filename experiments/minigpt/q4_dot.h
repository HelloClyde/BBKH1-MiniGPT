#ifndef MG_Q4_DOT_H
#define MG_Q4_DOT_H
#include <stdint.h>
/* Low nibble is the earlier coefficient; signed two's complement -8..7. */
int32_t mg_q4_dot(const uint8_t *,const int16_t *,unsigned);
int32_t mg_mxu_q4_dot(const uint8_t *,const int16_t *,unsigned);
void mg_mxu_q4_pair(const uint8_t *,const int16_t *,const int16_t *,int32_t *);
void mg_q4_row_pair(const uint8_t *,const uint16_t *,const int16_t *,const int16_t *,unsigned,float *);
extern volatile uint32_t mg_q4_pair_status[3]; /* paired groups, terms, scalar groups */
float mg_q4_scale(uint16_t);
int mg_q4_valid(const uint16_t *,unsigned);
float mg_q4_row(const uint8_t *,const uint16_t *,const int16_t *,unsigned);
int mg_q4_selftest(void);
#endif
