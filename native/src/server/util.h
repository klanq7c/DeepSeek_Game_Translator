/*
 * util.h —— 本地翻译服务器的通用工具函数声明。
 *
 * 这里集中了全模块共用的内存分配、字符串处理、哈希等基础原语。
 * 所有分配失败一律 fatal（调用 die()），简化调用处的错误处理。
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

/* 打印错误信息后终止进程。两种声明用于兼容 MSVC 与 C11 的 noreturn 写法。 */
#ifdef _MSC_VER
__declspec(noreturn) void die(const char *m);
#else
_Noreturn void die(const char *m);
#endif

/* x* 系列分配：失败即 die("oom")，永不返回 NULL；n 为 0 时按 1 分配，避免实现定义行为。 */
void *xmalloc(size_t n);
void *xrealloc(void *p, size_t n);
char *xstrndup(const char *s, size_t n);   /* 复制前 n 字节，自动追加 '\0' */
char *xstrdup(const char *s);              /* 全串复制，s 为 NULL 时视为空串 */

int ieq(const char *a, const char *b);                 /* 不区分大小写比较相等 */
char *istrstr(char *hay, const char *needle);          /* 不区分大小写子串查找 */
uint64_t h64(const char *s);                           /* 64 位 FNV-1a 哈希，缓存键使用 */
