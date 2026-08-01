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

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CACHE_MAX_ENCODED_LINE_BYTES (8 * 1024 * 1024)

/* 插入/覆盖一个条目，调用者必须已持有 lock。
   接管 k、v 的所有权。persisted 表示传入值已存在于 TSV。
   返回 0=当前值已持久化，1=新增，2=覆盖，3=同值但仍待持久化。 */
static int cache_insert_locked(Cache *c, char *k, char *v, int persisted) {
    uint64_t h = h64(k);
    size_t m = c->cap - 1;
    size_t i = (size_t)h & m;
    while (c->e[i].used) {
        if (c->e[i].h == h && strcmp(c->e[i].k, k) == 0) {
            if (strcmp(c->e[i].v, v) == 0) {
                int needs_persist = !c->e[i].persisted;
                if (persisted) c->e[i].persisted = 1;
                free(k);
                free(v);
                return needs_persist ? 3 : 0;
            }
            free(c->e[i].v);
            free(k);
            c->e[i].v = v;
            c->e[i].persisted = persisted;
            return 2;
        }
        i = (i + 1) & m;
    }
    c->e[i].used = 1;
    c->e[i].h = h;
    c->e[i].k = k;
    c->e[i].v = v;
    c->e[i].persisted = persisted;
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
        if (old[i].used) {
            (void)cache_insert_locked(c, old[i].k, old[i].v, old[i].persisted);
        }
    }
    free(old);
}

/* 初始化空表：32768 个桶起步，绑定持久化文件路径。 */
void cache_init(Cache *c, const char *path) {
    if (!c || !path || !path[0]) die("invalid cache path");
    c->cap = 1 << 15;
    c->len = 0;
    c->e = xcalloc(c->cap, sizeof *c->e);
    InitializeSRWLock(&c->lock);
    InitializeSRWLock(&c->io_lock);
    c->persist_f = NULL;
    int written = snprintf(c->path, sizeof c->path, "%s", path);
    if (written < 0 || (size_t)written >= sizeof c->path) {
        die("cache path too long");
    }
}

/* 仅写内存。空键/空值直接忽略（绝不能把空值当作翻译结果写入）。 */
void cache_set(Cache *c, const char *k, const char *v) {
    if (!k || !v || !*k || !*v) return;
    char *clean = xstrdup(v);
    normalize_translation_result(clean);
    if (!*clean || strcmp(k, clean) == 0) {
        free(clean);
        return;
    }
    AcquireSRWLockExclusive(&c->lock);
    if ((c->len + 1) * 10 > c->cap * 7) cache_rehash_locked(c);
    (void)cache_insert_locked(c, xstrdup(k), clean, 0);
    ReleaseSRWLockExclusive(&c->lock);
}

/* 写内存，并在新增或值变化时追加落盘。相同值不会重复扩大 TSV。
   io_lock 串行化持久化写者的更新顺序，但磁盘 IO 不占用表锁。 */
void cache_set_persist(Cache *c, const char *k, const char *v) {
    const char *keys[] = {k};
    const char *values[] = {v};
    cache_set_many_persist(c, keys, values, 1);
}

CachePersistResult cache_set_persist_result(Cache *c, const char *k, const char *v) {
    const char *keys[] = {k};
    const char *values[] = {v};
    return cache_set_many_persist_result(c, keys, values, 1);
}

/* 磁盘持久化/加载失败属于外部边界（文件系统：磁盘满、权限、句柄耗尽），无法
   在上游修复；诊断只记录原因/errno/路径，不含键值内容。限速策略与 api.c 的
   api_diag 一致：前 3 次全报，此后仅 2 的幂次，避免磁盘满时刷屏。失败绝不会
   伪装成成功写入：内存态不受影响，落盘缺失在加载侧自然表现为缓存未命中。 */
typedef enum {
    CACHE_DIAG_PERSIST_OPEN,
    CACHE_DIAG_PERSIST_WRITE,
    CACHE_DIAG_LOAD_READ,
    CACHE_DIAG_LOAD_LINE_TOO_LARGE,
    CACHE_DIAG_COUNT
} CacheDiag;

static volatile LONG g_cache_diag_counts[CACHE_DIAG_COUNT];

static void cache_diag(CacheDiag reason, int err, const char *path) {
    static const char *names[CACHE_DIAG_COUNT] = {
        "persist-open", "persist-write", "load-read", "load-line-too-large"
    };
    LONG count = InterlockedIncrement(&g_cache_diag_counts[reason]);
    if (count <= 3 || (count & (count - 1)) == 0) {
        fprintf(stderr, "[cache] %s failed #%ld (errno=%d, path=%s)\n",
                names[reason], (long)count, err, path ? path : "");
        fflush(stderr);
    }
}

void cache_set_many_persist(Cache *c, const char **keys, const char **values, size_t count) {
    (void)cache_set_many_persist_result(c, keys, values, count);
}

/* Persist a provider batch as one transaction-sized append. Normalization and
   Base64 work happen before either lock. The map lock only covers in-memory
   changes; all TSV writes remain outside it, and one fflush covers the batch.
   Valid translations remain available in memory if the external filesystem
   fails; the returned counts make that restart-loss boundary explicit. */
CachePersistResult cache_set_many_persist_result(Cache *c, const char **keys,
                                                  const char **values, size_t count) {
    CachePersistResult result = {CACHE_PERSIST_ALL, 0, 0, 0};
    if (!c || !keys || !values || !count) return result;

    char **clean = xcalloc(count, sizeof *clean);
    char **normalized_values = xcalloc(count, sizeof *normalized_values);
    char **encoded_keys = xcalloc(count, sizeof *encoded_keys);
    char **encoded_values = xcalloc(count, sizeof *encoded_values);
    unsigned char *changed = xcalloc(count, sizeof *changed);
    unsigned char *written = xcalloc(count, sizeof *written);

    for (size_t i = 0; i < count; i++) {
        if (!keys[i] || !values[i] || !*keys[i] || !*values[i]) {
            result.rejected++;
            continue;
        }
        clean[i] = xstrdup(values[i]);
        normalize_translation_result(clean[i]);
        if (!*clean[i] || strcmp(keys[i], clean[i]) == 0) {
            free(clean[i]);
            clean[i] = NULL;
            result.rejected++;
            continue;
        }
        result.accepted++;
        normalized_values[i] = xstrdup(clean[i]);
        encoded_keys[i] = b64enc(keys[i]);
        encoded_values[i] = b64enc(clean[i]);
    }

    AcquireSRWLockExclusive(&c->io_lock);
    AcquireSRWLockExclusive(&c->lock);
    for (size_t i = 0; i < count; i++) {
        if (!clean[i]) continue;
        if ((c->len + 1) * 10 > c->cap * 7) cache_rehash_locked(c);
        changed[i] = (unsigned char)cache_insert_locked(c, xstrdup(keys[i]), clean[i], 0);
        if (!changed[i]) result.persisted++;
        clean[i] = NULL;
    }
    ReleaseSRWLockExclusive(&c->lock);

    FILE *f = (FILE *)c->persist_f;
    size_t wrote = 0;
    int io_failed = 0;
    for (size_t i = 0; i < count; i++) {
        if (!changed[i]) continue;
        if (!f) {
            f = fopen(c->path, "ab");
            c->persist_f = f;
            if (!f) {
                /* 打开失败：整批落盘被丢弃（内存已更新）。persist_f 保持 NULL，
                   下一批会重试打开。 */
                cache_diag(CACHE_DIAG_PERSIST_OPEN, errno, c->path);
                break;
            }
        }
        if (fprintf(f, "%s\t%s\n", encoded_keys[i], encoded_values[i]) < 0) {
            /* 写失败（如磁盘满）：停止本批，截断行之后的行不再写，也不把这次
               写入当作成功；关闭句柄并置 NULL，让下一批重开重试。 */
            cache_diag(CACHE_DIAG_PERSIST_WRITE, errno, c->path);
            io_failed = 1;
            break;
        }
        written[i] = 1;
        wrote++;
    }
    if (wrote && fflush(f) != 0) {
        cache_diag(CACHE_DIAG_PERSIST_WRITE, errno, c->path);
        io_failed = 1;
    }
    /* A failed fprintf leaves the stream's buffered/on-disk boundary
       uncertain even if the following fflush happens to return success.
       Confirm the batch only when every write and the final flush succeeded;
       otherwise the entries stay dirty and the next identical update retries. */
    if (wrote && !io_failed) {
        AcquireSRWLockExclusive(&c->lock);
        for (size_t i = 0; i < count; i++) {
            if (!written[i]) continue;
            uint64_t h = h64(keys[i]);
            size_t m = c->cap - 1;
            size_t slot = (size_t)h & m;
            while (c->e[slot].used) {
                if (c->e[slot].h == h && strcmp(c->e[slot].k, keys[i]) == 0) {
                    if (strcmp(c->e[slot].v, normalized_values[i]) == 0) {
                        c->e[slot].persisted = 1;
                    }
                    break;
                }
                slot = (slot + 1) & m;
            }
        }
        ReleaseSRWLockExclusive(&c->lock);
        result.persisted += wrote;
    }
    if (io_failed && f) {
        fclose(f);
        c->persist_f = NULL;
    }
    ReleaseSRWLockExclusive(&c->io_lock);

    for (size_t i = 0; i < count; i++) {
        free(clean[i]);
        free(normalized_values[i]);
        free(encoded_keys[i]);
        free(encoded_values[i]);
    }
    free(clean);
    free(normalized_values);
    free(encoded_keys);
    free(encoded_values);
    free(changed);
    free(written);

    if (result.persisted == result.accepted) {
        result.status = CACHE_PERSIST_ALL;
    } else if (result.persisted) {
        result.status = CACHE_PERSIST_PARTIAL;
    } else {
        result.status = CACHE_PERSIST_FAILED;
    }
    return result;
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
   因此一行即一条记录。读取出错（非 EOF，如介质/权限故障）时诊断并
   返回 NULL：否则调用方会拿着空行反复重读同一份错误，形成死循环。 */
static char *cache_read_line(FILE *f, const char *path) {
    Buf b;
    char chunk[1 << 16];
    int oversized = 0;
    buf_init(&b);
    while (fgets(chunk, sizeof chunk, f)) {
        size_t len = strlen(chunk);
        if (!oversized) {
            if (len > CACHE_MAX_ENCODED_LINE_BYTES - b.len) {
                oversized = 1;
                cache_diag(CACHE_DIAG_LOAD_LINE_TOO_LARGE, 0, path);
            } else {
                buf_addn(&b, chunk, len);
            }
        }
        if (len > 0 && chunk[len - 1] == '\n') break;
    }
    if (ferror(f)) {
        cache_diag(CACHE_DIAG_LOAD_READ, errno, path);
        buf_free(&b);
        return NULL;
    }
    if (oversized) {
        /* The remainder was consumed through the record newline above. Return
           one empty row so cache_load skips it and continues with later rows. */
        buf_free(&b);
        return xstrdup("");
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
    while ((line = cache_read_line(f, c->path)) != NULL) {
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
        if (*k && *v && strcmp(k, v) != 0) {
            if ((c->len + 1) * 10 > c->cap * 7) cache_rehash_locked(c);
            (void)cache_insert_locked(c, k, v, 1);
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
