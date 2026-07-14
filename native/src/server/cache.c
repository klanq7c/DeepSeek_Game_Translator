/*
 * cache.c —— 本地翻译缓存实现（详见 cache.h）。
 *
 * 线程模型：lock 保护哈希表本身；io_lock 仅串行化 TSV 追加写。
 * 关键约束——磁盘 IO 绝不能在 lock 下进行，否则游戏侧的读查询
 * 会被每次持久化阻塞（曾表现为游戏内卡顿）。
 */
#include "cache.h"
#include "b64.h"
#include "buf.h"
#include "util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* 插入/覆盖一个条目，调用者必须已持有 lock。
   接管 k、v 的所有权。返回 0=值未变化，1=新增，2=覆盖为新值。 */
static int cache_insert_locked(Cache *c, char *k, char *v) {
    uint64_t h = h64(k);
    size_t m = c->cap - 1;
    size_t i = (size_t)h & m;
    while (c->e[i].used) {
        if (c->e[i].h == h && strcmp(c->e[i].k, k) == 0) {
            if (strcmp(c->e[i].v, v) == 0) {
                free(k);
                free(v);
                return 0;
            }
            free(c->e[i].v);
            free(k);
            c->e[i].v = v;
            return 2;
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

/* 扩容到 2 倍并重插所有条目（rehash），调用者持锁。
   旧桶数组的 k/v 指针所有权转移给新表，只释放桶数组本身。 */
static void cache_rehash_locked(Cache *c) {
    CacheEntry *old = c->e;
    size_t oldcap = c->cap;
    if (c->cap > SIZE_MAX / 2) die("cache too large");
    c->cap *= 2;
    c->e = xcalloc(c->cap, sizeof *c->e);
    c->len = 0;
    for (size_t i = 0; i < oldcap; i++) {
        if (old[i].used) (void)cache_insert_locked(c, old[i].k, old[i].v);
    }
    free(old);
}

/* 初始化空表：32768 个桶起步，绑定持久化文件路径。 */
void cache_init(Cache *c, const char *path) {
    c->cap = 1 << 15;
    c->len = 0;
    c->e = xcalloc(c->cap, sizeof *c->e);
    InitializeSRWLock(&c->lock);
    InitializeSRWLock(&c->io_lock);
    c->persist_f = NULL;
    snprintf(c->path, sizeof c->path, "%s", path);
}

/* 仅写内存。空键/空值直接忽略（绝不能把空值当作翻译结果写入）。 */
void cache_set(Cache *c, const char *k, const char *v) {
    if (!k || !v || !*k || !*v) return;
    char *clean = xstrdup(v);
    normalize_translation_result(clean);
    if (!*clean) {
        free(clean);
        return;
    }
    AcquireSRWLockExclusive(&c->lock);
    if ((c->len + 1) * 10 > c->cap * 7) cache_rehash_locked(c);
    (void)cache_insert_locked(c, xstrdup(k), clean);
    ReleaseSRWLockExclusive(&c->lock);
}

/* 写内存，并在新增或值变化时追加落盘。相同值不会重复扩大 TSV。
   io_lock 串行化持久化写者的更新顺序，但磁盘 IO 不占用表锁。 */
void cache_set_persist(Cache *c, const char *k, const char *v) {
    if (!k || !v || !*k || !*v) return;

    char *clean = xstrdup(v);
    normalize_translation_result(clean);
    if (!*clean) {
        free(clean);
        return;
    }
    char *ek = b64enc(k);
    char *ev = b64enc(clean);
    AcquireSRWLockExclusive(&c->io_lock);
    AcquireSRWLockExclusive(&c->lock);
    if ((c->len + 1) * 10 > c->cap * 7) cache_rehash_locked(c);
    int changed = cache_insert_locked(c, xstrdup(k), clean);
    ReleaseSRWLockExclusive(&c->lock);

    if (changed) {
        /* Appending stays outside the map lock: game-facing cache reads must
           not wait for disk IO during warmup bursts. */
        FILE *f = (FILE *)c->persist_f;
        if (!f) {
            f = fopen(c->path, "ab");
            c->persist_f = f;
        }
        if (f) {
            fprintf(f, "%s\t%s\n", ek, ev);
            fflush(f);
        }
    }
    ReleaseSRWLockExclusive(&c->io_lock);
    free(ek);
    free(ev);
}

/* 查找：命中返回值的 xstrdup 拷贝（调用者负责 free），未命中返回 NULL。
   用共享锁，允许多读并发。 */
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

/* 命中时直接把 JSON 转义后的值写入 out（含引号），省去 malloc+copy+free。
   返回 1=命中，0=未命中。用于 HTTP 响应拼装的热路径。 */
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

/* 导出整个表为 JSON 对象 {"k":"v",...}，返回条目数。供缓存导出接口使用。 */
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

/* 导出整个表为 JSON 数组 [{"key":"k","value":"v"},...]，返回条目数。 */
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

/* 从 f 读取一行（可能跨多次 fgets），返回新分配的 NUL 结尾缓冲；
   文件结束且无数据时返回 NULL。Base64 编码保证值内部无换行，
   因此一行即一条记录。 */
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

/* 启动期加载：把持久化 TSV 全量读入内存表。
   每行形如 <base64 键>\t<base64 值>，解码后插入；空键/空值跳过。 */
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
        normalize_translation_result(v);
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

/* 线程安全地返回当前条目数。 */
size_t cache_size(Cache *c) {
    AcquireSRWLockShared(&c->lock);
    size_t n = c->len;
    ReleaseSRWLockShared(&c->lock);
    return n;
}
