#include "json.h"
#include "buf.h"
#include "util.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

void list_push(List *l, char *s) {
    if (l->n == l->cap) {
        l->cap = l->cap ? l->cap * 2 : 8;
        l->v = xrealloc(l->v, l->cap * sizeof *l->v);
    }
    l->v[l->n++] = s;
}

void list_free(List *l) {
    for (size_t i = 0; i < l->n; i++) free(l->v[i]);
    free(l->v);
    l->v = NULL;
    l->n = l->cap = 0;
}

const char *json_skipws(const char *p) {
    while (*p && isspace((unsigned char)*p)) p++;
    return p;
}

static void putcp(Buf *b, unsigned cp) {
    if (cp < 0x80) buf_ch(b, (char)cp);
    else if (cp < 0x800) {
        buf_ch(b, (char)(0xc0 | (cp >> 6)));
        buf_ch(b, (char)(0x80 | (cp & 63)));
    } else if (cp < 0x10000) {
        buf_ch(b, (char)(0xe0 | (cp >> 12)));
        buf_ch(b, (char)(0x80 | ((cp >> 6) & 63)));
        buf_ch(b, (char)(0x80 | (cp & 63)));
    } else if (cp <= 0x10ffff) {
        buf_ch(b, (char)(0xf0 | (cp >> 18)));
        buf_ch(b, (char)(0x80 | ((cp >> 12) & 63)));
        buf_ch(b, (char)(0x80 | ((cp >> 6) & 63)));
        buf_ch(b, (char)(0x80 | (cp & 63)));
    }
}

static int hx(char c) {
    if ('0' <= c && c <= '9') return c - '0';
    if ('a' <= c && c <= 'f') return c - 'a' + 10;
    if ('A' <= c && c <= 'F') return c - 'A' + 10;
    return -1;
}

static int u4(const char *p, unsigned *out) {
    if (!p[0] || !p[1] || !p[2] || !p[3]) return 0;
    int a = hx(p[0]), b = hx(p[1]), c = hx(p[2]), d = hx(p[3]);
    if (a < 0 || b < 0 || c < 0 || d < 0) return 0;
    *out = (unsigned)((a << 12) | (b << 8) | (c << 4) | d);
    return 1;
}

char *json_str(const char **pp) {
    const char *p = json_skipws(*pp);
    if (*p != '"') return NULL;
    p++;
    Buf b;
    buf_init(&b);
    while (*p && *p != '"') {
        unsigned char c = (unsigned char)*p++;
        if (c == '\\') {
            char e = *p;
            if (!e) break; /* dangling backslash at end of buffer */
            p++;
            if (e == 'n') buf_ch(&b, '\n');
            else if (e == 'r') buf_ch(&b, '\r');
            else if (e == 't') buf_ch(&b, '\t');
            else if (e == 'u') {
                unsigned cp = 0;
                if (!u4(p, &cp)) break;
                p += 4;
                if (cp >= 0xd800 && cp <= 0xdbff) {
                    unsigned lo = 0;
                    if (p[0] == '\\' && p[1] == 'u' && u4(p + 2, &lo) && lo >= 0xdc00 && lo <= 0xdfff) {
                        cp = 0x10000 + ((cp - 0xd800) << 10) + (lo - 0xdc00);
                        p += 6;
                    } else {
                        cp = 0xfffd;
                    }
                } else if (cp >= 0xdc00 && cp <= 0xdfff) {
                    cp = 0xfffd;
                }
                putcp(&b, cp);
            } else buf_ch(&b, e);
        } else buf_ch(&b, (char)c);
    }
    if (*p == '"') p++;
    *pp = p;
    return b.data;
}

const char *json_key(const char *json, const char *key) {
    const char *p = json;
    while ((p = strchr(p, '"'))) {
        const char *q = p;
        char *s = json_str(&q);
        if (!s) { p++; continue; }
        int ok = strcmp(s, key) == 0;
        free(s);
        q = json_skipws(q);
        if (ok && *q == ':') return json_skipws(q + 1);
        p = q;
    }
    return NULL;
}

char *json_get_str(const char *json, const char *key) {
    const char *p = json_key(json, key);
    return p ? json_str(&p) : NULL;
}

List json_array(const char *json, const char *key) {
    List l = {0};
    const char *p = json_key(json, key);
    if (!p) return l;
    p = json_skipws(p);
    if (*p == '"') {
        char *s = json_str(&p);
        if (s) list_push(&l, s);
        return l;
    }
    if (*p++ != '[') return l;
    for (;;) {
        p = json_skipws(p);
        if (*p == ']') break;
        if (*p != '"') break;
        char *s = json_str(&p);
        if (!s) break;
        list_push(&l, s);
        p = json_skipws(p);
        if (*p == ',') p++;
    }
    return l;
}
