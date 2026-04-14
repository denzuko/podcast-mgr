/* arena.h — minimal region allocator for testing */
#pragma once
#include <stdlib.h>
#include <string.h>
typedef struct { char *buf; size_t cap; size_t pos; } Arena;
static inline Arena arena_init(size_t cap) {
    return (Arena){ .buf = calloc(1, cap), .cap = cap, .pos = 0 };
}
static inline void *arena_alloc(Arena *a, size_t n) {
    if (a->pos + n > a->cap) return NULL;
    void *p = a->buf + a->pos;
    a->pos += n;
    return p;
}
static inline void *arena_realloc(Arena *a, void *old, size_t old_sz, size_t new_sz) {
    void *p = arena_alloc(a, new_sz);
    if (p && old) memcpy(p, old, old_sz < new_sz ? old_sz : new_sz);
    return p;
}
static inline void arena_destroy(Arena *a) { free(a->buf); a->buf = NULL; }
