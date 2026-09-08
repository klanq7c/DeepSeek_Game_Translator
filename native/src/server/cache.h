/*
 * cache.h —— 本地翻译缓存。
 *
 * 运行时契约（AGENTS.md 要求，改动务必保持）：
 *   - 本地缓存优先，命中即返回；
 *   - API 工作中或缺失时立即响应，绝不因远程 API 延迟阻塞游戏运行时；
 *   - 缺失/排队/透传原文一律不得回写为"已翻译成功"。
 *
 * 实现：开放寻址哈希表（线性探测），键为原文，值为译文。可选地把新条目
 * 以 Base64 包裹的 TSV 形式追加落盘，用于跨进程重启后的持久化。
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef _WIN32
#include <windows.h>
#endif

#include "buf.h"

/* 单个哈希桶。used==0 表示空槽。h 为键的缓存哈希值，避免每次比较都算。 */
typedef struct {
    char *k;
    char *v;
    uint64_t h;
    int used;
    int persisted; /* 当前值已确认存在于 TSV 中 */
} CacheEntry;

typedef enum {
    CACHE_PERSIST_ALL = 0,
    CACHE_PERSIST_PARTIAL,
    CACHE_PERSIST_FAILED
} CachePersistStatus;

typedef struct {
    CachePersistStatus status;
    size_t accepted;  /* 已应用到内存的有效非同文条目 */
    size_t persisted; /* 本次调用后已确认落盘的条目数 */
    size_t rejected;  /* 空值/非法或归一化后的原文回显条目 */
} CachePersistResult;

typedef struct {
    CacheEntry *e;     /* 桶数组，长度为 cap（恒为 2 的幂） */
    size_t cap;        /* 桶数 */
    size_t len;        /* 已用条目数 */
#ifdef _WIN32
    SRWLOCK lock;
    /* 只串行化 TSV 追加。磁盘 IO 绝不能在 `lock` 下运行，否则面向游戏的查询读取
       会被每次持久化阻塞。 */
    SRWLOCK io_lock;
#endif
    void *persist_f; /* FILE*，惰性打开，进程存活期间持有 */
    char path[MAX_PATH * 4];
} Cache;

void cache_init(Cache *c, const char *path);                 /* 初始化空表，绑定持久化文件路径 */
void cache_set(Cache *c, const char *k, const char *v);      /* 仅写内存 */
void cache_set_persist(Cache *c, const char *k, const char *v); /* 写内存 + 追加落盘 */
CachePersistResult cache_set_persist_result(Cache *c, const char *k, const char *v);
char *cache_get(Cache *c, const char *k);                    /* 命中返回新分配拷贝，未命中返回 NULL */
int cache_contains(Cache *c, const char *k);                 /* 仅探测是否命中，不分配值副本 */
void cache_set_many_persist(Cache *c, const char **keys, const char **values,
                            size_t count); /* 一次映射锁 + 一次磁盘刷盘 */
CachePersistResult cache_set_many_persist_result(Cache *c, const char **keys,
                                                  const char **values, size_t count);
/* 查询 k，并把带引号的 JSON 转义值写入 out。命中返回 1，未命中返回 0；
   相比 cache_get 可省去 malloc、复制和 free。 */
int cache_emit_json(Cache *c, const char *k, Buf *out);
size_t cache_emit_json_map(Cache *c, Buf *out);     /* 导出为 {"k":"v",...} 形式，返回条目数 */
size_t cache_emit_json_entries(Cache *c, Buf *out); /* 导出为 [{"key":"k","value":"v"},...]，返回条目数 */
void cache_load(Cache *c);                          /* 从 path 加载已有 TSV 到内存表 */
size_t cache_size(Cache *c);                        /* 返回当前条目数（线程安全） */
