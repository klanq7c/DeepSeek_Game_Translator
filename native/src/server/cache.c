#include "cache.h"
#include "b64.h"
#include "buf.h"
#include "util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int cache_insert_locked(Cache *c, char *k, char *v) {
    uint64_t h = h64(k);
    size_t m = c->cap - 1;
    size_t i = (size_t)h & m;
    while (c->e[i].used) {
        if (c->e[i].h == h && strcmp(c->e[i].k, k) == 0) {
            free(c->e[i].v);
            free(k);
            c->e[i].v = v;
            return 0;
        }
        i = (i + 1) & m;
    }
    c->e[i].used = 1;
    c->e[i].h = h;
    c->e[i].k = k;
    c->e[i].v = v;
    c->len++;
    return 1;
}

static void cache_rehash_locked(Cache *c) {
    CacheEntry *old = c->e;
    size_t oldcap = c->cap;
    c->cap *= 2;
    c->e = calloc(c->cap, sizeof *c->e);
    if (!c->e) die("oom");
    c->len = 0;
    for (size_t i = 0; i < oldcap; i++) {
        if (old[i].used) (void)cache_insert_locked(c, old[i].k, old[i].v);
    }
    free(old);
}

void cache_init(Cache *c, const char *path) {
    c->cap = 1 << 15;
    c->len = 0;
    c->e = calloc(c->cap, sizeof *c->e);
    if (!c->e) die("oom");
    InitializeSRWLock(&c->lock);
    InitializeSRWLock(&c->io_lock);
    c->persist_f = NULL;
    snprintf(c->path, sizeof c->path, "%s", path);
}

void cache_set(Cache *c, const char *k, const char *v) {
    if (!k || !v || !*k || !*v) return;
    AcquireSRWLockExclusive(&c->lock);
    if ((c->len + 1) * 10 > c->cap * 7) cache_rehash_locked(c);
    (void)cache_insert_locked(c, xstrdup(k), xstrdup(v));
    ReleaseSRWLockExclusive(&c->lock);
}

void cache_set_persist(Cache *c, const char *k, const char *v) {
    if (!k || !v || !*k || !*v) return;
    AcquireSRWLockExclusive(&c->lock);
    if ((c->len + 1) * 10 > c->cap * 7) cache_rehash_locked(c);
    int is_new = cache_insert_locked(c, xstrdup(k), xstrdup(v));
    ReleaseSRWLockExclusive(&c->lock);
    if (!is_new) return;

    /* Encode and append outside the map lock: during warmup bursts the API
       workers persist continuously, and disk IO held under `lock` stalled
       every game-facing lookup (visible as in-game hitches). */
    char *ek = b64enc(k);
    char *ev = b64enc(v);
    AcquireSRWLockExclusive(&c->io_lock);
    FILE *f = (FILE *)c->persist_f;
    if (!f) {
        f = fopen(c->path, "ab");
        c->persist_f = f;
    }
    if (f) {
        fprintf(f, "%s\t%s\n", ek, ev);
        fflush(f);
    }
    ReleaseSRWLockExclusive(&c->io_lock);
    free(ek);
    free(ev);
}

char *cache_get(Cache *c, const char *k) {
    if (!k) return NULL;
    uint64_t h = h64(k);
    AcquireSRWLockShared(&c->lock);
    size_t m = c->cap - 1;
    size_t i = (size_t)h & m;
    char *result = NULL;
    while (c->e[i].used) {
        if (c->e[i].h == h && strcmp(c->e[i].k, k) == 0) {
            result = xstrdup(c->e[i].v);
            break;
        }
        i = (i + 1) & m;
    }
    ReleaseSRWLockShared(&c->lock);
    return result;
}

int cache_emit_json(Cache *c, const char *k, Buf *out) {
    if (!k) return 0;
    uint64_t h = h64(k);
    AcquireSRWLockShared(&c->lock);
    size_t m = c->cap - 1;
    size_t i = (size_t)h & m;
    int hit = 0;
    while (c->e[i].used) {
        if (c->e[i].h == h && strcmp(c->e[i].k, k) == 0) {
            buf_json(out, c->e[i].v);
            hit = 1;
            break;
        }
        i = (i + 1) & m;
    }
    ReleaseSRWLockShared(&c->lock);
    return hit;
}

size_t cache_emit_json_map(Cache *c, Buf *out) {
    int first = 1;
    size_t n = 0;
    AcquireSRWLockShared(&c->lock);
    for (size_t i = 0; i < c->cap; i++) {
        if (!c->e[i].used) continue;
        if (!first) buf_ch(out, ',');
        first = 0;
        buf_json(out, c->e[i].k);
        buf_ch(out, ':');
        buf_json(out, c->e[i].v);
        n++;
    }
    ReleaseSRWLockShared(&c->lock);
    return n;
}

size_t cache_emit_json_entries(Cache *c, Buf *out) {
    int first = 1;
    size_t n = 0;
    AcquireSRWLockShared(&c->lock);
    for (size_t i = 0; i < c->cap; i++) {
        if (!c->e[i].used) continue;
        if (!first) buf_ch(out, ',');
        first = 0;
        buf_add(out, "{\"key\":");
        buf_json(out, c->e[i].k);
        buf_add(out, ",\"value\":");
        buf_json(out, c->e[i].v);
        buf_ch(out, '}');
        n++;
    }
    ReleaseSRWLockShared(&c->lock);
    return n;
}

static char *cache_read_line(FILE *f) {
    Buf b;
    char chunk[1 << 16];
    buf_init(&b);
    while (fgets(chunk, sizeof chunk, f)) {
        size_t len = strlen(chunk);
        buf_addn(&b, chunk, len);
        if (len > 0 && chunk[len - 1] == '\n') break;
    }
    if (b.len == 0 && feof(f)) {
        buf_free(&b);
        return NULL;
    }
    return b.data;
}

void cache_load(Cache *c) {
    FILE *f = fopen(c->path, "rb");
    if (!f) return;
    size_t n = 0;
    /* Exclusive for the whole load: no concurrent readers yet (server hasn't
       started accepting). Saves N×Acquire/Release vs locking per insert. */
    AcquireSRWLockExclusive(&c->lock);
    char *line;
    while ((line = cache_read_line(f)) != NULL) {
        char *tab = strchr(line, '\t');
        if (!tab) {
            free(line);
            continue;
        }
        *tab++ = 0;
        char *end = tab + strlen(tab);
        while (end > tab && (end[-1] == '\n' || end[-1] == '\r')) *--end = 0;
        char *k = b64dec(line, strlen(line));
        char *v = b64dec(tab, strlen(tab));
        if (*k && *v) {
            if ((c->len + 1) * 10 > c->cap * 7) cache_rehash_locked(c);
            (void)cache_insert_locked(c, k, v);
            n++;
        } else {
            free(k);
            free(v);
        }
        free(line);
    }
    ReleaseSRWLockExclusive(&c->lock);
    fclose(f);
    fprintf(stderr, "loaded %zu cache entries\n", n);
}

size_t cache_size(Cache *c) {
    AcquireSRWLockShared(&c->lock);
    size_t n = c->len;
    ReleaseSRWLockShared(&c->lock);
    return n;
}
