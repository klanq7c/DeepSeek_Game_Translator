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
#define CACHE_INITIAL_CAPACITY (1u << 10)

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

/* 持锁查找并返回桶地址；返回值只在调用方继续持有表锁时有效。 */
static CacheEntry *cache_find_locked(Cache *c, const char *k, uint64_t h) {
    size_t m = c->cap - 1;
    size_t i = (size_t)h & m;
    while (c->e[i].used) {
        if (c->e[i].h == h && strcmp(c->e[i].k, k) == 0) return &c->e[i];
        i = (i + 1) & m;
    }
    return NULL;
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

/* 初始化空表：从小桶表起步并按 70% 负载因子自动扩容，避免小缓存固定占用约 1 MiB。 */
void cache_init(Cache *c, const char *path) {
    if (!c || !path || !path[0]) die("invalid cache path");
    c->cap = CACHE_INITIAL_CAPACITY;
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

/* 每项只保留所有权和连续快照偏移，避免为状态、快照和编码结果分别分配。 */
typedef struct {
    char *clean;
    size_t normalized_offset;
    unsigned char changed;
    unsigned char written;
} CacheBatchItem;

/* 把提供方批次作为一次事务大小的追加持久化。归一化在加锁前完成并存入连续快照；
   Base64 只处理确认发生变更的条目，并复用一块行缓冲。映射锁只覆盖内存修改，
   所有 TSV 写入都在映射锁外完成，并用一次 fflush 覆盖整批。
   外部文件系统故障时，有效译文仍保留在内存中；返回计数会明确暴露重启丢失边界。 */
CachePersistResult cache_set_many_persist_result(Cache *c, const char **keys,
                                                  const char **values, size_t count) {
    CachePersistResult result = {CACHE_PERSIST_ALL, 0, 0, 0};
    if (!c || !keys || !values || !count) return result;

    CacheBatchItem *items = xcalloc(count, sizeof *items);
    Buf normalized_values = {0};
    Buf journal_line = {0};
    size_t normalized_bytes = 0;

    for (size_t i = 0; i < count; i++) {
        if (!keys[i] || !values[i] || !*keys[i] || !*values[i]) {
            result.rejected++;
            continue;
        }
        items[i].clean = xstrdup(values[i]);
        normalize_translation_result(items[i].clean);
        if (!*items[i].clean || strcmp(keys[i], items[i].clean) == 0) {
            free(items[i].clean);
            items[i].clean = NULL;
            result.rejected++;
            continue;
        }
        result.accepted++;
        size_t value_bytes = strlen(items[i].clean) + 1;
        if (!value_bytes || normalized_bytes > SIZE_MAX - value_bytes) {
            die("cache batch too large");
        }
        normalized_bytes += value_bytes;
    }

    /* 归一化快照集中存放，写后校验仍按原值比较，但不再为每项复制一块堆内存。 */
    if (normalized_bytes) {
        buf_grow(&normalized_values, normalized_bytes);
        for (size_t i = 0; i < count; i++) {
            if (!items[i].clean) continue;
            items[i].normalized_offset = normalized_values.len;
            buf_add(&normalized_values, items[i].clean);
            buf_ch(&normalized_values, '\0');
        }
    }

    AcquireSRWLockExclusive(&c->io_lock);
    AcquireSRWLockExclusive(&c->lock);
    for (size_t i = 0; i < count; i++) {
        if (!items[i].clean) continue;
        if ((c->len + 1) * 10 > c->cap * 7) cache_rehash_locked(c);
        items[i].changed = (unsigned char)cache_insert_locked(
            c, xstrdup(keys[i]), items[i].clean, 0);
        if (!items[i].changed) result.persisted++;
        items[i].clean = NULL;
    }
    ReleaseSRWLockExclusive(&c->lock);

    FILE *f = (FILE *)c->persist_f;
    size_t wrote = 0;
    int io_failed = 0;
    for (size_t i = 0; i < count; i++) {
        if (!items[i].changed) continue;
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
        /* 只有确认条目确有变更后才编码，并在整个批次内复用同一行缓冲。 */
        journal_line.len = 0;
        if (journal_line.data) journal_line.data[0] = 0;
        b64enc_append(&journal_line, keys[i]);
        buf_ch(&journal_line, '\t');
        b64enc_append(&journal_line,
                      normalized_values.data + items[i].normalized_offset);
        buf_ch(&journal_line, '\n');
        if (fprintf(f, "%s", journal_line.data) < 0) {
            /* 写失败（如磁盘满）：停止本批，截断行之后的行不再写，也不把这次
               写入当作成功；关闭句柄并置 NULL，让下一批重开重试。 */
            cache_diag(CACHE_DIAG_PERSIST_WRITE, errno, c->path);
            io_failed = 1;
            break;
        }
        items[i].written = 1;
        wrote++;
    }
    if (wrote && fflush(f) != 0) {
        cache_diag(CACHE_DIAG_PERSIST_WRITE, errno, c->path);
        io_failed = 1;
    }
    /* fprintf 失败后，即使随后的 fflush 恰好成功，流缓冲与磁盘之间的边界仍不确定。
       只有全部写入和最终刷新都成功才确认整批；否则条目保持脏状态，并在下一次相同
       更新时重试。 */
    if (wrote && !io_failed) {
        AcquireSRWLockExclusive(&c->lock);
        for (size_t i = 0; i < count; i++) {
            if (!items[i].written) continue;
            uint64_t h = h64(keys[i]);
            CacheEntry *entry = cache_find_locked(c, keys[i], h);
            const char *normalized =
                normalized_values.data + items[i].normalized_offset;
            if (entry && strcmp(entry->v, normalized) == 0) {
                entry->persisted = 1;
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
        free(items[i].clean);
    }
    buf_free(&normalized_values);
    buf_free(&journal_line);
    free(items);

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
    CacheEntry *entry = cache_find_locked(c, k, h);
    char *result = entry ? xstrdup(entry->v) : NULL;
    ReleaseSRWLockShared(&c->lock);
    return result;
}

/* 只检查键是否存在，不复制译文。供预热去重等只关心命中的热路径使用。 */
int cache_contains(Cache *c, const char *k) {
    if (!c || !k) return 0;
    uint64_t h = h64(k);
    AcquireSRWLockShared(&c->lock);
    int found = cache_find_locked(c, k, h) != NULL;
    ReleaseSRWLockShared(&c->lock);
    return found;
}

/* 命中时直接把 JSON 转义后的值写入 out（含引号），省去 malloc+copy+free。
   返回 1=命中，0=未命中。用于 HTTP 响应拼装的热路径。 */
int cache_emit_json(Cache *c, const char *k, Buf *out) {
    if (!k) return 0;
    uint64_t h = h64(k);
    AcquireSRWLockShared(&c->lock);
    CacheEntry *entry = cache_find_locked(c, k, h);
    int hit = entry != NULL;
    if (entry) buf_json(out, entry->v);
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
        /* 上方已经消费到该记录的换行结尾。返回一条空记录，使 cache_load 跳过它并
           继续处理后续记录。 */
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
    /* 整个加载过程独占：服务尚未开始接受连接，因此没有并发读取者。相比逐条插入加锁，
       可省去 N 次 Acquire/Release。 */
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
