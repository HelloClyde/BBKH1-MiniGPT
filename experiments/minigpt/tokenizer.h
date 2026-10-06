#ifndef MINIGPT_TOKENIZER_H
#define MINIGPT_TOKENIZER_H
#include <stdint.h>
/* UTF-8 -> pinned ByteLevel BPE. Negative result means malformed/too long. */
int mg_encode(const char *,int *,int);
const uint8_t *mg_piece(int,int *);
/* Returns next Unicode scalar, rejects overlong/surrogate/out-of-range UTF-8. */
int mg_utf8(const char **);
#endif
