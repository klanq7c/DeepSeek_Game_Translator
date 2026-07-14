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

#ifdef _WIN32
#include <windows.h>
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
    void *p = malloc(n ? n : 1);
    if (!p) die("oom");
    return p;
}

/* calloc，统一处理乘法溢出和 OOM。返回内存始终可 free 且已经清零。 */
void *xcalloc(size_t count, size_t size) {
    if (!count || !size) return xmalloc(1);
    if (count > SIZE_MAX / size) die("allocation too large");
    void *p = calloc(count, size);
    if (!p) die("oom");
    return p;
}

/* realloc，OOM 即终止。同样对 n==0 做归一化。 */
void *xrealloc(void *p, size_t n) {
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

/* Require a real separator and non-empty remainder so ordinary translations
   that merely start with a similar phrase are left untouched. */
static const char *translation_after_prompt_prefix(const char *text, const char *prefix) {
    if (!translation_prefix_equal(text, prefix)) return NULL;
    const char *p = text + strlen(prefix);
    while (*p == ' ' || *p == '\t') p++;
    if (*p == ':') {
        p++;
    } else if ((unsigned char)p[0] == 0xef &&
               (unsigned char)p[1] == 0xbc &&
               (unsigned char)p[2] == 0x9a) {
        p += 3; /* U+FF1A FULLWIDTH COLON */
    } else if (*p != '\r' && *p != '\n') {
        return NULL;
    }
    while (*p && isspace((unsigned char)*p)) p++;
    return *p ? p : NULL;
}

static int strip_translation_prompt_echo(char *s) {
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
    char *candidate = s;
    while (*candidate && isspace((unsigned char)*candidate)) candidate++;
    for (size_t i = 0; i < sizeof prefixes / sizeof prefixes[0]; i++) {
        const char *rest = translation_after_prompt_prefix(candidate, prefixes[i]);
        if (!rest) continue;
        memmove(s, rest, strlen(rest) + 1);
        trim_translation_result(s);
        return 1;
    }
    return 0;
}

/* Shared by live API responses and every cache ingress. Normal values remain
   byte-for-byte unchanged; disk-loaded prompt echoes heal only in memory. */
void normalize_translation_result(char *s) {
    if (!s || !*s) return;
    for (int pass = 0; pass < 3 && *s; pass++) {
        if (!strip_translation_prompt_echo(s)) break;
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
    uint64_t h = 1469598103934665603ULL;          /* FNV offset basis */
    while (*s) {
        h ^= (unsigned char)*s++;
        h *= 1099511628211ULL;                     /* FNV prime */
    }
    return h ? h : 1;
}
