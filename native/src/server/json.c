/*
 * json.c —— 极简 JSON 解析器实现（详见 json.h）。
 *
 * 设计取向：朴素字符串扫描，无递归下降、无抽象语法树。
 * 这对本服务的输入（DeepSeek API 响应 + 少量请求字段）足够且高效。
 */
#include "json.h"
#include "buf.h"
#include "util.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

/* 追加字符串指针，按需 2 倍扩容。所有权转移给 List。 */
void list_push(List *l, char *s) {
    if (l->n == l->cap) {
        l->cap = l->cap ? l->cap * 2 : 8;
        l->v = xrealloc(l->v, l->cap * sizeof *l->v);
    }
    l->v[l->n++] = s;
}

/* 释放每个元素字符串再释放数组本体，最后清零字段。 */
void list_free(List *l) {
    for (size_t i = 0; i < l->n; i++) free(l->v[i]);
    free(l->v);
    l->v = NULL;
    l->n = l->cap = 0;
}

/* 跳过空格/制表/换行等空白。 */
const char *json_skipws(const char *p) {
    while (*p && isspace((unsigned char)*p)) p++;
    return p;
}

/* 把 Unicode 码点 cp 编码为 UTF-8 写入缓冲区。
   JSON 中的 \uXXXX 是码点，最终要还原成 UTF-8 字节交给上层。 */
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

/* 单个十六进制字符转数值，非法返回 -1。 */
static int hx(char c) {
    if ('0' <= c && c <= '9') return c - '0';
    if ('a' <= c && c <= 'f') return c - 'a' + 10;
    if ('A' <= c && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* 解析 4 位十六进制（\uXXXX 的 XXXX）为码点，成功返回 1 并写入 *out。 */
static int u4(const char *p, unsigned *out) {
    if (!p[0] || !p[1] || !p[2] || !p[3]) return 0;
    int a = hx(p[0]), b = hx(p[1]), c = hx(p[2]), d = hx(p[3]);
    if (a < 0 || b < 0 || c < 0 || d < 0) return 0;
    *out = (unsigned)((a << 12) | (b << 8) | (c << 4) | d);
    return 1;
}

/* 解析 pp 指向的 JSON 字符串字面量（含起止引号），推进 *pp 到串尾之后，
   返回解码后的新分配缓冲（即使解析不完整也返回已解码部分）。
   处理：常见短转义；\uXXXX 码点（含代理对合并，孤立代理替换为 U+FFFD）。 */
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
                    /* 高代理：尝试合并紧跟的低代理组成完整码点 */
                    unsigned lo = 0;
                    if (p[0] == '\\' && p[1] == 'u' && u4(p + 2, &lo) && lo >= 0xdc00 && lo <= 0xdfff) {
                        cp = 0x10000 + ((cp - 0xd800) << 10) + (lo - 0xdc00);
                        p += 6;
                    } else {
                        cp = 0xfffd;
                    }
                } else if (cp >= 0xdc00 && cp <= 0xdfff) {
                    /* 孤立低代理，替换为替换字符 */
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

/* 在 json 中查找 "key": 结构，返回值起始位置（跳过冒号与空白）。
   采用逐个双引号扫描 + 解码比对的朴素实现，对当前输入规模足够。 */
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

/* 取 key 对应的字符串值并解码，键不存在或非字符串时返回 NULL。 */
char *json_get_str(const char *json, const char *key) {
    const char *p = json_key(json, key);
    return p ? json_str(&p) : NULL;
}

/* 取 key 对应的字符串数组。兼容两种形态：
   - 直接是一个字符串（包装成单元素列表）；
   - 标准数组 ["a","b",...]。
   解析中途遇到非法结构即停止，返回已收集到的部分。 */
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
