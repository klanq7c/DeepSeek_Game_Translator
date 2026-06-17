/*
 * buf.c —— 动态字节缓冲区实现（详见 buf.h）。
 */
#include "buf.h"
#include "util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* 初始化为 256 字节空缓冲区。data 始终以 '\0' 结尾，便于当字符串用。 */
void buf_init(Buf *b) {
    b->cap = 256;
    b->len = 0;
    b->data = xmalloc(b->cap);
    b->data[0] = 0;
}

/* 释放内部缓冲并置空字段，buf 可重新 init 复用。 */
void buf_free(Buf *b) {
    free(b->data);
    b->data = NULL;
    b->len = b->cap = 0;
}

/* 预留至少 n 字节可用空间（已含结尾 '\0' 的余量）。容量按 2 倍指数扩张，
   摊还代价 O(1)。need = len + n + 1 中的 +1 为结尾 '\0' 预留。 */
void buf_grow(Buf *b, size_t n) {
    size_t need = b->len + n + 1;
    if (need <= b->cap) return;
    if (b->cap == 0) b->cap = 16;
    while (b->cap < need) b->cap *= 2;
    b->data = xrealloc(b->data, b->cap);
}

/* 追加定长 n 字节并补 '\0'。 */
void buf_addn(Buf *b, const char *s, size_t n) {
    buf_grow(b, n);
    memcpy(b->data + b->len, s, n);
    b->len += n;
    b->data[b->len] = 0;
}

/* 追加以 '\0' 结尾的字符串。 */
void buf_add(Buf *b, const char *s) {
    buf_addn(b, s, strlen(s));
}

/* 追加单个字符。 */
void buf_ch(Buf *b, char c) {
    buf_grow(b, 1);
    b->data[b->len++] = c;
    b->data[b->len] = 0;
}

/* 追加整数的十进制文本。64 字节缓冲足够存放任意 64 位整。 */
void buf_int(Buf *b, long long v) {
    char tmp[64];
    snprintf(tmp, sizeof tmp, "%lld", v);
    buf_add(b, tmp);
}

/* 追加一个 JSON 字符串字面量：首尾加引号，内部做转义。
   规则："/\\ 反斜杠转义；\n\r\t 用短转义；其余 <32 控制字符用 \uXXXX；
   其余字节原样输出（缓存值多为 UTF-8 文本，原样保留即可）。 */
void buf_json(Buf *b, const char *s) {
    buf_ch(b, '"');
    for (; s && *s; s++) {
        unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\') {
            buf_ch(b, '\\');
            buf_ch(b, (char)c);
        } else if (c == '\n') buf_add(b, "\\n");
        else if (c == '\r') buf_add(b, "\\r");
        else if (c == '\t') buf_add(b, "\\t");
        else if (c < 32) {
            char tmp[8];
            snprintf(tmp, sizeof tmp, "\\u%04x", c);
            buf_add(b, tmp);
        } else buf_ch(b, (char)c);
    }
    buf_ch(b, '"');
}
