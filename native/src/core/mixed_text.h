#pragma once
#include <stddef.h>
#include <string.h>

/* 与 MixedText.HasUntranslatedLatin 对齐：只检查可见英文单词，忽略标签、变量、
   控制码和路径。HP/FPS 等短大写缩写以及一两个字母的单位不触发中文重译。 */
static inline int dst_mixed_latin_format(const char *s, int renpy) {
    if (!s) return 0;
    size_t i = 0; int square = 0, curly = 0;
    while (s[i]) {
        char c = s[i];
        if (!renpy && c == '_' && s[i+1] == '_') {
            const char *end = strstr(s + i + 2, "__");
            if (end) { i = (size_t)(end - s) + 2; continue; }
        }
        if (c == '%' && s[i+1] == '(') {
            const char *end = strchr(s + i + 2, ')');
            if (end) { i = (size_t)(end - s) + 1; continue; }
        }
        if ((!renpy && c == '<') || c == '[' || c == '{') {
            if (renpy && s[i+1] == c) { i += 2; continue; }
            char close = c == '<' ? '>' : c == '[' ? ']' : '}';
            char quote = 0; size_t end = i + 1;
            for (; s[end] && (c == '<' || (s[end] != '\n' && s[end] != '\r')); end++) {
                if (c == '<' && (s[end] == '\'' || s[end] == '"')) {
                    if (!quote) quote = s[end]; else if (quote == s[end]) quote = 0;
                } else if (!quote && s[end] == close) break;
            }
            if (s[end] == close) {
                if (!renpy) { i = end + 1; continue; }
                size_t chars = 0;
                for (size_t j = i + 1; j < end; j++) if (((unsigned char)s[j] & 0xc0) != 0x80) chars++;
                int *count = c == '[' ? &square : &curly;
                if (*count < 64 && chars > 0 && chars <= 96) { (*count)++; i = end + 1; continue; }
            }
        }
        if (!renpy && (c == '\\' || c == 27)) {
            size_t end = i + 1;
            while ((s[end] >= 'A' && s[end] <= 'Z') || (s[end] >= 'a' && s[end] <= 'z')) end++;
            if (end > i + 1) { i = end; continue; }
        }
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')) {
            size_t end = i; int lower = 0;
            while ((s[end] >= 'A' && s[end] <= 'Z') || (s[end] >= 'a' && s[end] <= 'z')) {
                if (s[end] >= 'a' && s[end] <= 'z') lower = 1;
                end++;
            }
            size_t scheme = end;
            while ((s[scheme] >= 'A' && s[scheme] <= 'Z') || (s[scheme] >= 'a' && s[scheme] <= 'z')
                || (s[scheme] >= '0' && s[scheme] <= '9') || s[scheme] == '+' || s[scheme] == '.' || s[scheme] == '-') scheme++;
            if ((s[scheme] == ':' && (s[scheme+1] == '/' || s[scheme+1] == '\\'))) {
                end = scheme;
                while ((unsigned char)s[end] >= 33 && (unsigned char)s[end] <= 126) end++;
                i = end; continue;
            }
            if ((lower && end - i >= 3) || end - i >= 4) return 1;
            i = end; continue;
        }
        i++;
    }
    return 0;
}

static inline int dst_mixed_latin(const char *s) { return dst_mixed_latin_format(s, 0); }
/* Ren'Py 的字面双括号与尖括号按可见正文判定，仅用于显式引擎请求。 */
static inline int dst_mixed_latin_renpy(const char *s) { return dst_mixed_latin_format(s, 1); }
