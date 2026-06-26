/*
 * b64.c —— Base64 编解码实现（详见 b64.h）。
 */
#include "b64.h"
#include "buf.h"
#include "util.h"

#include <stdint.h>
#include <stdlib.h>

/* 把单个 Base64 字符映射为 0..63 的值，非法字符返回 -1（解码时跳过）。 */
static int b64v(int c) {
    if ('A' <= c && c <= 'Z') return c - 'A';
    if ('a' <= c && c <= 'z') return c - 'a' + 26;
    if ('0' <= c && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

/* 标准 Base64 编码。每 3 字节输入 -> 4 字节输出，不足 3 字节用 '=' 填充。
   输出长度公式 ((n+2)/3)*4 已含填充，再 +1 给结尾 '\0'。 */
char *b64enc(const char *s) {
    static const char tab[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t n = 0;
    while (s && s[n]) n++;
    if (n > SIZE_MAX - 2) die("base64 input too large");
    size_t groups = (n + 2) / 3;
    if (groups > (SIZE_MAX - 1) / 4) die("base64 output too large");
    size_t out_n = groups * 4;
    char *out = xmalloc(out_n + 1);
    size_t j = 0;
    for (size_t i = 0; i < n; i += 3) {
        unsigned a = (unsigned char)s[i];
        unsigned b = (i + 1 < n) ? (unsigned char)s[i + 1] : 0;
        unsigned c = (i + 2 < n) ? (unsigned char)s[i + 2] : 0;
        out[j++] = tab[(a >> 2) & 63];
        out[j++] = tab[((a & 3) << 4) | ((b >> 4) & 15)];
        out[j++] = (i + 1 < n) ? tab[((b & 15) << 2) | ((c >> 6) & 3)] : '=';
        out[j++] = (i + 2 < n) ? tab[c & 63] : '=';
    }
    out[j] = 0;
    return out;
}

/* 流式解码：把字符值逐个压入 6 位累加器 acc，每凑够 8 位输出一字节。
   遇到 '='（填充）立即结束；非法字符跳过，保证对带空白/换行的输入也稳健。
   末尾 realloc 收紧到实际长度，减少内存浪费。 */
char *b64dec(const char *s, size_t n) {
    Buf b;
    buf_init(&b);
    uint32_t acc = 0;
    int bits = 0;
    for (size_t i = 0; i < n; i++) {
        if (s[i] == '=') break;
        int v = b64v((unsigned char)s[i]);
        if (v < 0) continue;
        acc = (acc << 6) | (uint32_t)v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            buf_ch(&b, (char)((acc >> bits) & 255));
        }
    }
    char *tight = realloc(b.data, b.len + 1);
    if (tight) b.data = tight;
    return b.data;
}
