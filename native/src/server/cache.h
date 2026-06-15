#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef _WIN32
#include <windows.h>
#endif

#include "buf.h"

typedef struct {
    char *k;
    char *v;
    uint64_t h;
    int used;
} CacheEntry;

typedef struct {
    CacheEntry *e;
    size_t cap;
    size_t len;
#ifdef _WIN32
    SRWLOCK lock;
    /* Serializes TSV appends only. Disk IO must never run under `lock`:
       readers (game-facing lookups) would stall behind every persist. */
    SRWLOCK io_lock;
#endif
    void *persist_f; /* FILE*, lazily opened, lives for the process */
    char path[MAX_PATH * 4];
} Cache;

void cache_init(Cache *c, const char *path);
void cache_set(Cache *c, const char *k, const char *v);
void cache_set_persist(Cache *c, const char *k, const char *v);
char *cache_get(Cache *c, const char *k);
/* Look up k and emit JSON-escaped value into out (with surrounding quotes).
   Returns 1 on hit, 0 on miss. Saves the malloc+copy+free vs cache_get. */
int cache_emit_json(Cache *c, const char *k, Buf *out);
size_t cache_emit_json_map(Cache *c, Buf *out);
size_t cache_emit_json_entries(Cache *c, Buf *out);
void cache_load(Cache *c);
size_t cache_size(Cache *c);
