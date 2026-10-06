#ifndef MINIGPT_ENGINE_H
#define MINIGPT_ENGINE_H
#include <stdint.h>
#if defined(MG_INT4) || defined(MG_LAYER_CACHE)
#define MG_PREFIX_CACHE 1
#endif
#define MG_DIM 512
#define MG_FFN 1408
#define MG_LAYERS 8
#define MG_VOCAB 6400
#define MG_CONTEXT 256
#define MG_WORK_FLOATS (512*5+128*2+1408*2+MG_CONTEXT+6400)
#define MG_WORK_BYTES (MG_WORK_FLOATS*sizeof(float)+1408*sizeof(int16_t))
#ifdef MG_INT4
#ifdef MG_Q4_EMBED_INT8
#define MG_MODEL_BYTES 15381020u
#define MG_MODEL_VERSION 3u
#else
#define MG_MODEL_BYTES 13819420u
#define MG_MODEL_VERSION 2u
#endif
#define MG_MODEL_MAGIC "MGPTQ4\0\0"
#else
#define MG_MODEL_VERSION 1u
#define MG_MODEL_BYTES 26096156u
#define MG_MODEL_MAGIC "MGPTQ8\0\0"
#endif
typedef int (*mg_reader)(void *,uint32_t,void *,uint32_t);
/* Called between matrices/layers; returning nonzero cancels the current step. */
typedef int (*mg_yield)(void *,int,int);
typedef struct {uint32_t offset,bytes,rows,cols,kind;} mg_tensor;
typedef struct {
    mg_reader read; void *io; mg_yield yield; void *ui;
    mg_tensor t[75]; uint8_t *resident,*embedding,*layer;
    float *cache,*work; float norm[512],rope[MG_CONTEXT*64];
    int position,resident_mode,prompt_done,prompt_total; uint32_t file_bytes,layer_bytes;
#ifdef MG_LAYER_CACHE
    uint8_t *layer_cache[MG_LAYERS];
    uint32_t cached_layers,layer_cache_hits,layer_cache_misses;
    uint32_t layer_cache_warming,layer_cache_read_aborts;
    uint32_t cache_tune_phase,cache_tune_sample,cache_tune_dma_ticks,cache_tune_sync_ticks;
    uint32_t cache_tune_initial_layers,cache_tune_selected,cache_tune_canceled;
#define MG_LAYER_CACHE_RESERVE (2u*1024u*1024u)
#endif
#ifdef MG_PREFIX_CACHE
    int prefix_ids[MG_CONTEXT],prefix_tokens,prompt_reused;
#endif
#ifdef MG_DMA_PIPELINE
    uint8_t *layer_next; /* packed next-layer buffer for guarded DMA prefetch */
#endif
} mg_model;
/* 0=success; -1=format/read; -2=RAM. auto resident allocation falls back to streaming. */
int mg_load(mg_model *,mg_reader,void *,int);
void mg_close(mg_model *);
void mg_reset(mg_model *);
/* The returned logits remain valid until the next step. NULL=cancel/error. */
float *mg_forward(mg_model *,int);
/* Advance KV state without the unused final norm/classifier during prefill.
 * 1=success, 0=cancel/error; mg_forward is still used for the final input token. */
int mg_prefill(mg_model *,int);
/* Layer-major prompt processing: one layer read per prompt, <=320 KiB at the
 * application's 160-token limit. Falls back to serial prefill if malloc fails.
 * Returns only the final token's logits. On error, reset before another prompt. */
float *mg_prompt(mg_model *,const int *,int);
#ifdef MG_PREFIX_CACHE
/* An independent question: reuse only an identical leading token sequence.
 * No previous generated answer becomes part of this question's context. */
float *mg_prompt_cached(mg_model *,const int *,int);
#endif
#if defined(MG_LAYER_CACHE) && defined(MG_DMA_PIPELINE)
/* Measure one additional resident layer against DMA using the same buffers.
 * clock returns monotonic raw ticks; no user context survives the probes. */
int mg_select_layer_cache(mg_model *,uint32_t (*clock)(void));
#endif
int mg_argmax(const float *);
float mg_exp(float);
float mg_rsqrt(float);
#endif
