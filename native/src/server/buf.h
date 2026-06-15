/*
 * buf.h —— 动态字节缓冲区（Buf）声明。
 *
 * 用于 HTTP 响应体、JSON 序列化、缓存行读取等需要动态拼接字节的场景。
 * 设计为朴素的可增长字节数组：容量按 2 倍扩张，内容总是以 '\0' 结尾，
 * 因此既可当字节流也可当 C 字符串使用。
 */
#pragma once

#include <stddef.h>

typedef struct {
    char *data;   /* 堆缓冲区，data[len]=='\0' */
    size_t len;   /* 当前已写入字节数（不含结尾 '\0'） */
    size_t cap;   /* 当前已分配容量 */
} Buf;

void buf_init(Buf *b);                          /* 初始化为 256 字节空缓冲区 */
void buf_free(Buf *b);                          /* 释放并清零字段 */
void buf_grow(Buf *b, size_t n);                /* 预留至少 n 字节可用空间（按需扩容） */
void buf_addn(Buf *b, const char *s, size_t n); /* 追加定长字节 */
void buf_add(Buf *b, const char *s);            /* 追加以 '\0' 结尾的字符串 */
void buf_ch(Buf *b, char c);                    /* 追加单个字符 */
void buf_int(Buf *b, long long v);              /* 追加整数的十进制文本 */
void buf_json(Buf *b, const char *s);           /* 追加 JSON 字符串字面量（含引号与转义） */
