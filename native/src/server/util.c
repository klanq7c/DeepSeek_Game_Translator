/*
 * util.c —— 通用工具函数实现（详见 util.h）。
 *
 * 本文件不含任何引擎相关逻辑，被 server 与 launcher 路径共同依赖，
 * 因此改动需谨慎，避免影响跨模块行为。
 */
#include "util.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* 汉字按完整 UTF-8 码点识别；不将无效字节作为可保护的中文。 */
static size_t mixed_han_len(const char *s) {
    const unsigned char *p = (const unsigned char *)s;
    if ((p[0] & 0xf0) != 0xe0 || !p[1] || !p[2]
        || (p[1] & 0xc0) != 0x80 || (p[2] & 0xc0) != 0x80) return 0;
    unsigned cp = ((p[0] & 15) << 12) | ((p[1] & 63) << 6) | (p[2] & 63);
    return cp >= 0x4e00 && cp <= 0x9fff ? 3 : 0;
}

static char *mixed_prefix(const char *source) {
    char *prefix = xmalloc(48);
    size_t salt = 0;
    do { snprintf(prefix, 48, "__DST_HAN%zu_", salt++); } while (strstr(source, prefix));
    return prefix;
}

char *mixed_text_protect(const char *source) {
    const char *han = source;
    while (*han && !mixed_han_len(han)) han++;
    if (!*han) return xstrdup(source);
    char *prefix = mixed_prefix(source);
    size_t source_len = strlen(source), prefix_len = strlen(prefix);
    size_t runs = 0;
    for (const char *p = han; *p;) {
        if (!mixed_han_len(p)) { p++; continue; }
        runs++;
        while (mixed_han_len(p)) p += 3;
    }
    if (runs > (SIZE_MAX - source_len - 1) / (prefix_len + 24)) die("mixed text too large");
    char *out = xmalloc(source_len + runs * (prefix_len + 24) + 1), *write = out;
    size_t token = 0;
    for (const char *p = source; *p;) {
        if (!mixed_han_len(p)) { *write++ = *p++; continue; }
        while (mixed_han_len(p)) p += 3;
        write += sprintf(write, "%s%zu__", prefix, token++);
    }
    *write = 0;
    free(prefix);
    return out;
}

/* 模型必须按顺序保留每个中文标记且恰好一次。NULL 由 API 边界记录，
   不能把受损中文作为成功结果，也不对共享缓存中已有译文做替换。 */
char *mixed_text_restore(const char *source, const char *reply) {
    if (!reply) return NULL;
    char *prefix = mixed_prefix(source);
    size_t source_len = strlen(source), reply_len = strlen(reply);
    if (source_len > SIZE_MAX - reply_len - 1) die("mixed text too large");
    char *out = xmalloc(source_len + reply_len + 1), *write = out;
    char *key = xmalloc(strlen(prefix) + 24);
    const char *copied = reply;
    size_t token = 0;
    for (const char *p = source; *p;) {
        if (!mixed_han_len(p)) { p++; continue; }
        const char *start = p;
        while (mixed_han_len(p)) p += 3;
        sprintf(key, "%s%zu__", prefix, token++);
        const char *pos = strstr(copied, key);
        if (!pos || strstr(pos + strlen(key), key)) { free(key); free(prefix); free(out); return NULL; }
        memcpy(write, copied, (size_t)(pos - copied)); write += pos - copied;
        memcpy(write, start, (size_t)(p - start)); write += p - start;
        copied = pos + strlen(key);
    }
    strcpy(write, copied);
    if (token && strstr(out, prefix)) { free(out); out = NULL; }
    free(key); free(prefix);
    return out;
}

#ifdef _WIN32
#include <windows.h>
#endif

/* 仅测试构建统计受管堆分配次数；正式构建展开为空操作，不增加热路径成本。 */
#ifdef DST_TEST_ALLOC_COUNTERS
static size_t g_dst_test_alloc_count;
#define DST_TEST_COUNT_ALLOCATION() (g_dst_test_alloc_count++)

void dst_test_alloc_reset(void) {
    g_dst_test_alloc_count = 0;
}

size_t dst_test_alloc_count(void) {
    return g_dst_test_alloc_count;
}
#else
#define DST_TEST_COUNT_ALLOCATION() ((void)0)
#endif

/* 打印错误到 stderr 后终止进程：Windows 用 ExitProcess 避免析构副作用，其他平台用 exit。 */
void die(const char *m) {
    fputs(m, stderr);
    fputc('\n', stderr);
#ifdef _WIN32
    ExitProcess(1);
#else
    exit(1);
#endif
}

/* malloc，OOM 即终止。n 为 0 时分配 1 字节，保证返回非空且可 free 的指针。 */
void *xmalloc(size_t n) {
    DST_TEST_COUNT_ALLOCATION();
    void *p = malloc(n ? n : 1);
    if (!p) die("oom");
    return p;
}

/* calloc，统一处理乘法溢出和 OOM。返回内存始终可 free 且已经清零。 */
void *xcalloc(size_t count, size_t size) {
    if (!count || !size) return xmalloc(1);
    if (count > SIZE_MAX / size) die("allocation too large");
    DST_TEST_COUNT_ALLOCATION();
    void *p = calloc(count, size);
    if (!p) die("oom");
    return p;
}

/* realloc，OOM 即终止。同样对 n==0 做归一化。 */
void *xrealloc(void *p, size_t n) {
    DST_TEST_COUNT_ALLOCATION();
    void *r = realloc(p, n ? n : 1);
    if (!r) die("oom");
    return r;
}

/* 复制 s 的前 n 字节并补 '\0'，调用者负责 free。 */
char *xstrndup(const char *s, size_t n) {
    if (n == SIZE_MAX) die("string too large");
    char *p = xmalloc(n + 1);
    memcpy(p, s, n);
    p[n] = 0;
    return p;
}

/* 全串复制；NULL 输入归一化为空串，避免对 NULL 调 strlen。 */
char *xstrdup(const char *s) {
    return xstrndup(s ? s : "", strlen(s ? s : ""));
}

static void trim_translation_result(char *s) {
    char *p = s;
    while (*p && isspace((unsigned char)*p)) p++;
    if (p != s) memmove(s, p, strlen(p) + 1);
    size_t n = strlen(s);
    while (n && isspace((unsigned char)s[n - 1])) s[--n] = 0;
}

static int translation_prefix_equal(const char *text, const char *prefix) {
    while (*prefix) {
        unsigned char a = (unsigned char)*text++;
        unsigned char b = (unsigned char)*prefix++;
        if (!a) return 0;
        if (a < 0x80 && b < 0x80) {
            if (tolower(a) != tolower(b)) return 0;
        } else if (a != b) {
            return 0;
        }
    }
    return 1;
}

/* 必须存在真实分隔符和非空余文，避免误改仅以相似短语开头的普通译文。 */
static const char *translation_after_prompt_prefix(const char *text, const char *prefix) {
    if (!translation_prefix_equal(text, prefix)) return NULL;
    const char *p = text + strlen(prefix);
    while (*p == ' ' || *p == '\t') p++;
    if (*p == ':') {
        p++;
    } else if ((unsigned char)p[0] == 0xef &&
               (unsigned char)p[1] == 0xbc &&
               (unsigned char)p[2] == 0x9a) {
        p += 3; /* U+FF1A 全角冒号 */
    } else if (*p != '\r' && *p != '\n') {
        return NULL;
    }
    while (*p && isspace((unsigned char)*p)) p++;
    return *p ? p : NULL;
}

static const char *translation_after_any_prompt_prefix(const char *s) {
    static const char *const prefixes[] = {
        u8"\u7ffb\u8bd1\u6210\u7b80\u4f53\u4e2d\u6587",
        u8"\u7ffb\u8bd1\u4e3a\u7b80\u4f53\u4e2d\u6587",
        u8"\u8bd1\u6210\u7b80\u4f53\u4e2d\u6587",
        u8"\u7b80\u4f53\u4e2d\u6587\u7ffb\u8bd1",
        u8"\u7b80\u4f53\u4e2d\u6587\u8bd1\u6587",
        "Simplified Chinese translation",
        "Translation to Simplified Chinese",
        "Translate to Simplified Chinese",
        "Translated into Simplified Chinese",
        "Translate this exact game text to Simplified Chinese. Return only the translation."
    };
    const char *candidate = s;
    while (*candidate && isspace((unsigned char)*candidate)) candidate++;
    for (size_t i = 0; i < sizeof prefixes / sizeof prefixes[0]; i++) {
        const char *rest = translation_after_prompt_prefix(candidate, prefixes[i]);
        if (!rest) continue;
        return rest;
    }
    return NULL;
}

static size_t translation_prompt_depth(const char *s) {
    size_t depth = 0;
    if (!s) return 0;
    const char *rest;
    while ((rest = translation_after_any_prompt_prefix(s)) != NULL) {
        depth++;
        s = rest;
    }
    return depth;
}

/* 实时 API 响应和所有缓存入口共用。正常值逐字节保持不变；从磁盘加载的提示词回显
   只在内存中修复。 */
void normalize_translation_result(char *s) {
    normalize_translation_result_for_source(s, NULL);
}

/* 原文已有的提示词形状前缀属于游戏文字，模型额外添加的前缀才可剥离。
   API 传中文保护后的请求原文，所有缓存入口传原文键；最多仍清理三层。 */
void normalize_translation_result_for_source(char *s, const char *source) {
    if (!s || !*s) return;
    size_t passes = 3, original_depth = translation_prompt_depth(source);
    if (original_depth) {
        size_t depth = translation_prompt_depth(s);
        passes = depth > original_depth ? depth - original_depth : 0;
        if (passes > 3) passes = 3;
    }
    for (size_t pass = 0; pass < passes && *s; pass++) {
        const char *rest = translation_after_any_prompt_prefix(s);
        if (!rest) break;
        memmove(s, rest, strlen(rest) + 1);
        trim_translation_result(s);
    }
}

/* 不区分大小写比较是否相等；两个串都到尾且相等时返回 1。 */
int ieq(const char *a, const char *b) {
    while (*a && *b) {
        if (tolower((unsigned char)*a++) != tolower((unsigned char)*b++)) return 0;
    }
    return *a == *b;
}

/* 不区分大小写的子串查找，返回 hay 中首次出现处的指针，未找到返回 NULL。 */
char *istrstr(char *hay, const char *needle) {
    size_t nn = strlen(needle);
    if (!nn) return hay;
    for (; *hay; hay++) {
        size_t i = 0;
        while (i < nn && hay[i] && tolower((unsigned char)hay[i]) == tolower((unsigned char)needle[i])) i++;
        if (i == nn) return hay;
    }
    return NULL;
}

/* FNV-1a 64 位哈希，用于缓存键定位桶。
   返回值保证非 0（结果为 0 时归一化为 1），避免与"空槽"标记冲突。 */
uint64_t h64(const char *s) {
    uint64_t h = 1469598103934665603ULL;          /* FNV 偏移基数 */
    while (*s) {
        h ^= (unsigned char)*s++;
        h *= 1099511628211ULL;                     /* FNV 质数乘子 */
    }
    return h ? h : 1;
}
