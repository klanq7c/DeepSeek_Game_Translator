#include "b64.h"
#include "buf.h"
#include "util.h"

#include <stdint.h>
#include <stdlib.h>

static int b64v(int c) {
    if ('A' <= c && c <= 'Z') return c - 'A';
    if ('a' <= c && c <= 'z') return c - 'a' + 26;
    if ('0' <= c && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

char *b64enc(const char *s) {
    static const char tab[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t n = 0;
    while (s && s[n]) n++;
    size_t out_n = ((n + 2) / 3) * 4;
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
