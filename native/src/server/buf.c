#include "buf.h"
#include "util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void buf_init(Buf *b) {
    b->cap = 256;
    b->len = 0;
    b->data = xmalloc(b->cap);
    b->data[0] = 0;
}

void buf_free(Buf *b) {
    free(b->data);
    b->data = NULL;
    b->len = b->cap = 0;
}

void buf_grow(Buf *b, size_t n) {
    size_t need = b->len + n + 1;
    if (need <= b->cap) return;
    if (b->cap == 0) b->cap = 16;
    while (b->cap < need) b->cap *= 2;
    b->data = xrealloc(b->data, b->cap);
}

void buf_addn(Buf *b, const char *s, size_t n) {
    buf_grow(b, n);
    memcpy(b->data + b->len, s, n);
    b->len += n;
    b->data[b->len] = 0;
}

void buf_add(Buf *b, const char *s) {
    buf_addn(b, s, strlen(s));
}

void buf_ch(Buf *b, char c) {
    buf_grow(b, 1);
    b->data[b->len++] = c;
    b->data[b->len] = 0;
}

void buf_int(Buf *b, long long v) {
    char tmp[64];
    snprintf(tmp, sizeof tmp, "%lld", v);
    buf_add(b, tmp);
}

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
