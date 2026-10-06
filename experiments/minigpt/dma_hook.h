/* Guarded H1 V1.41 RAM hook, paired around one synchronous layer read. */
#ifndef MG_DMA_HOOK_H
#define MG_DMA_HOOK_H
#include <stdint.h>
extern volatile uint32_t mg_dma_hook_status[8];
/* ready (0 unknown, 1 matched, 2 rejected), active, installs, restores,
 * signature/word rejects, DMA busy/reentry rejects, conflicts, cache syncs. */
int mg_dma_hook_enter(void);
int mg_dma_hook_leave(void);
#endif
