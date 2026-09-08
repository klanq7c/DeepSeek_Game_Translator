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

/* 计算标准 Base64 的有效输出字节数，不含结尾 NUL；分配型接口会再预留 1 字节。 */
static size_t b64_encoded_size(const char *s, size_t *input_size) {
    size_t n = 0;
    while (s && s[n]) n++;
    if (n > SIZE_MAX - 2) die("base64 input too large");
    size_t groups = (n + 2) / 3;
    if (groups > (SIZE_MAX - 1) / 4) die("base64 output too large");
    if (input_size) *input_size = n;
    return groups * 4;
}

/* 把标准 RFC4648 Base64 写到调用方已预留的区域，不写结尾 NUL。 */
static void b64_encode_to(char *out, const char *s, size_t n) {
    static const char tab[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
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
}

/* 直接追加编码结果；同一批次可以复用一块 Buf。 */
void b64enc_append(Buf *out, const char *s) {
    size_t n = 0;
    size_t out_n = b64_encoded_size(s, &n);
    char *tail = buf_reserve(out, out_n);
    b64_encode_to(tail, s, n);
    buf_commit(out, out_n);
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
