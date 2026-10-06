#ifndef MINIGPT_PERF_H
#define MINIGPT_PERF_H
#include <stdint.h>
enum {MG_PERF_OTHER,MG_PERF_READ,MG_PERF_MATRIX,MG_PERF_ATTENTION,MG_PERF_UI,MG_PERF_COUNT};
#if defined(MG_PROFILE) || defined(MG_DIAGNOSTICS)
/* Exclusive nested wall time using the firmware's existing raw tick.
 * No timer, PLL, divider or voltage register is written. */
void mg_perf_begin(void);
void mg_perf_enter(unsigned);
void mg_perf_leave(void);
void mg_perf_bytes(uint32_t);
void mg_perf_decode(void);
void mg_perf_end(void);
#else
#define mg_perf_begin() ((void)0)
#define mg_perf_enter(stage) ((void)0)
#define mg_perf_leave() ((void)0)
#define mg_perf_bytes(bytes) ((void)0)
#define mg_perf_decode() ((void)0)
#define mg_perf_end() ((void)0)
#endif
#endif
