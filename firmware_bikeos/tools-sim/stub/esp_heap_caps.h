#pragma once
#include <stdlib.h>
#define MALLOC_CAP_SPIRAM 1
#define MALLOC_CAP_INTERNAL 2
#define MALLOC_CAP_8BIT 4
#define MALLOC_CAP_DMA 8
static inline void *heap_caps_malloc(size_t n, unsigned c) { (void)c; return malloc(n); }
static inline void *heap_caps_calloc(size_t a, size_t n, unsigned c) { (void)c; return calloc(a, n); }
static inline size_t heap_caps_get_free_size(unsigned c) { (void)c; return 0; }
