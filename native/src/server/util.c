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

/* realloc，OOM 即终止。同样对 n==0 做归一化。 */
void *xrealloc(void *p, size_t n) {
    void *r = realloc(p, n ? n : 1);
    if (!r) die("oom");
    return r;
}

/* 复制 s 的前 n 字节并补 '\0'，调用者负责 free。 */
char *xstrndup(const char *s, size_t n) {
    char *p = xmalloc(n + 1);
    memcpy(p, s, n);
    p[n] = 0;
    return p;
}

/* 全串复制；NULL 输入归一化为空串，避免对 NULL 调 strlen。 */
char *xstrdup(const char *s) {
    return xstrndup(s ? s : "", strlen(s ? s : ""));
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
