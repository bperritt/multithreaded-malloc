#ifndef XMALLOC_H
#define XMALLOC_H

#include <stddef.h>
#include <pthread.h>
#include <stdint.h>

void* xmalloc(size_t bytes);
void  xfree(void* ptr);
void* xrealloc(void* prev, size_t bytes);

typedef struct bucket_header {
    size_t size;
    int fill;
    int arena;
    uint8_t bitmap[1024];
} bucket_header;

typedef struct arena {
    int id;
    int in_use;
    pthread_mutex_t lock;
    bucket_header* bucket_head[8];
} arena;

#endif
