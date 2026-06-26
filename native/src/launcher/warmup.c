/* ================================================================
 * warmup.c — 翻译缓存预热实现
 * ----------------------------------------------------------------
 * 本文件是启动器的核心预热模块，功能包括：
 *
 *   1. 文本采集：扫描各引擎的数据文件，提取需要翻译的字符串
 *      - Ren'Py：解析 .rpy 脚本中的字符串字面量
 *      - RPG Maker MV/MZ：解析 www/data/ 下 JSON 文件中的对话/名称字段
 *      - Unity：扫描 *_Data/ 下 assets 二进制文件中的 ASCII 字符串，
 *        以及 XUnity 的 Translation/zh-CN/Text/ 下翻译文件
 *
 *   2. 文本过滤：通过 should_warm_text 等函数过滤掉 URL、文件名、
 *      已含中文的文本、Unity 内部标识符等不需要翻译的内容
 *
 *   3. 去重：使用开放寻址哈希表（dedup_contains/dedup_add）
 *      实现 O(1) 去重，替代旧的 O(n) 线性扫描
 *
 *   4. 批量提交：通过复用的 WinHTTP 会话（LocalHttp）将文本
 *      分批 POST 到本地服务器的 /prefetch（后台排队）或
 *      /cache/import（导入已有翻译）端点
 *
 *   5. XUnity 翻译文件回填：对已有翻译文件中空值的条目，
 *      查询本地缓存补全翻译并回写文件
 * ================================================================ */

#include "warmup.h"
#include "fsutil.h"
#include "ui.h"

#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <wchar.h>
#include <windows.h>
#include <winhttp.h>

/* ---- 预热容量与扫描限制 ---- */
#define WARMUP_MAX_ITEMS 1200              /* 未指定引擎容量时的保守默认值 */
#define RPGM_WARMUP_MAX_ITEMS 40000        /* 大型 RPG Maker 游戏的数据与插件文本上限 */
#define UNITY_WARMUP_MAX_ITEMS 8000        /* Unity 资源扫描最大采集条数 */
/* Ren'Py 脚本全是高质量对话，且服务器会去重并异步排队缺失项，
   因此可以预热整个脚本。1200 条只够覆盖典型 VN 的前几个文件，
   导致大部分台词在首次显示时才翻译（中英文闪烁）。 */
#define RENPY_WARMUP_MAX_ITEMS 30000       /* Ren'Py 脚本最大采集条数 */
#define WARMUP_BATCH_ITEMS 256             /* 每批提交到服务器的条数 */
#define WARMUP_MAX_TEXT_BYTES 1200         /* 单条文本最大字节数 */
#define UNITY_ASSET_SCAN_MAX_BYTES (64u * 1024u * 1024u)   /* Unity .assets 扫描上限 64MB */
#define RENPY_SCRIPT_SCAN_MAX_BYTES (8u * 1024u * 1024u)   /* 单个 .rpy 扫描上限 8MB */
#define RENPY_SCAN_MAX_DEPTH 12            /* .rpy 目录递归最大深度 */

/* 键值对列表：用于 XUnity 翻译文件导入（key=原文, value=译文） */
typedef struct {
    char **keys;
    char **vals;
    size_t n;
    size_t cap;
    const char **seen; /* 开放寻址去重索引，按 key 去重（延迟分配） */
    size_t seen_cap;
} PairList;

/* 纯文本列表：用于后台预翻译排队 */
typedef struct {
    char **items;
    size_t n;
    size_t cap;
    const char **seen; /* 开放寻址去重索引（延迟分配） */
    size_t seen_cap;
    size_t max_items; /* 0 表示使用 WARMUP_MAX_ITEMS */
} TextList;

/* 返回列表的最大条目数上限 */
static size_t textlist_limit(const TextList *l) {
    return l->max_items ? l->max_items : WARMUP_MAX_ITEMS;
}

static void textlist_add(TextList *l, const char *s);
static int bb_init(ByteBuf *b, size_t cap);

/* ======================== 通用字符串工具 ======================== */

static int wide_ends_with_i(const WCHAR *s, const WCHAR *suffix) {
    size_t sl = s ? wcslen(s) : 0;
    size_t tl = suffix ? wcslen(suffix) : 0;
    return sl >= tl && !_wcsicmp(s + sl - tl, suffix);
}

/* 复制 s 的前 n 个字节到新分配的内存（自动加 NUL 终止） */
static char *dup_range(const char *s, size_t n) {
    char *p = (char *)malloc(n + 1);
    if (!p) return NULL;
    memcpy(p, s, n);
    p[n] = 0;
    return p;
}

/* 去除字符串首尾的空白字符（空格/制表/回车/换行），原地修改 */
static char *trim_ascii(char *s) {
    while (*s == ' ' || *s == '\t' || *s == '\r' || *s == '\n') s++;
    size_t n = strlen(s);
    while (n && (s[n - 1] == ' ' || s[n - 1] == '\t' || s[n - 1] == '\r' || s[n - 1] == '\n')) s[--n] = 0;
    return s;
}

/* ----------------------------------------------------------------
 * has_cjk_utf8 — 检测 UTF-8 字符串是否含 CJK 汉字（U+4E00..U+9FFF）
 * 手动解码 UTF-8 多字节序列，避免依赖外部库。
 * ---------------------------------------------------------------- */
static int has_cjk_utf8(const char *s) {
    const unsigned char *p = (const unsigned char *)s;
    while (*p) {
        unsigned cp = 0;
        if (*p < 0x80) {
            cp = *p++;
        } else if ((*p & 0xe0) == 0xc0 && p[1]) {
            cp = ((*p & 0x1f) << 6) | (p[1] & 0x3f);
            p += 2;
        } else if ((*p & 0xf0) == 0xe0 && p[1] && p[2]) {
            cp = ((*p & 0x0f) << 12) | ((p[1] & 0x3f) << 6) | (p[2] & 0x3f);
            p += 3;
        } else {
            p++;
        }
        if (cp >= 0x4e00 && cp <= 0x9fff) return 1;
    }
    return 0;
}

/* ----------------------------------------------------------------
 * should_warm_text — 判断通用文本是否值得预热
 *
 * 过滤规则：
 *   - 长度过短（<2）或过长（>1200字节）
 *   - 含路径分隔符、URL、媒体文件扩展名（.png/.ogg/.m4a）
 *   - 不含任何字母或非 ASCII 字符（纯数字/符号）
 *   - 已含 CJK 汉字（不需要翻译）
 * ---------------------------------------------------------------- */
static int should_warm_text(const char *s) {
    size_t len = strlen(s);
    if (len < 2 || len > WARMUP_MAX_TEXT_BYTES) return 0;
    if (strchr(s, '\\') || strstr(s, "://") || strstr(s, ".png") || strstr(s, ".ogg") || strstr(s, ".m4a")) return 0;
    int signal = 0;
    const unsigned char *p = (const unsigned char *)s;
    while (*p) {
        if (*p < 0x80) {
            if ((*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z')) signal = 1;
            p++;
        } else {
            signal = 1;
            p++;
        }
    }
    return signal && !has_cjk_utf8(s);
}

/* RPG Maker 文本允许 \C[20]、\n[1]、\I[3]、\FS[24] 等显示控制码。
   通用过滤会把所有反斜杠视为文件路径，因此这里先验证并移除合法控制码，
   再用可见文本做通用判断；未知或类似 C:\Users 的路径仍会被拒绝。 */
static int should_warm_rpgm_text(const char *s) {
    size_t len = strlen(s);
    if (len < 2 || len > WARMUP_MAX_TEXT_BYTES) return 0;
    char *visible = (char *)malloc(len + 1);
    if (!visible) return 0;

    size_t out = 0;
    for (size_t i = 0; i < len;) {
        if (s[i] != '\\') {
            visible[out++] = s[i++];
            continue;
        }

        size_t code = i + 1;
        if (code >= len) {
            free(visible);
            return 0;
        }
        if (strchr("{}$.|!><^", s[code])) {
            visible[out++] = ' ';
            i = code + 1;
            continue;
        }

        size_t end = code;
        while (end < len &&
               ((s[end] >= 'A' && s[end] <= 'Z') ||
                (s[end] >= 'a' && s[end] <= 'z'))) {
            end++;
        }
        size_t letters = end - code;
        int single_known = letters == 1 && strchr("CcNnVvPpIiGg", s[code]);
        int uppercase_code = letters >= 2 && letters <= 8;
        for (size_t j = code; uppercase_code && j < end; j++) {
            if (s[j] < 'A' || s[j] > 'Z') uppercase_code = 0;
        }
        if (!single_known && !uppercase_code) {
            free(visible);
            return 0;
        }

        if (end < len && s[end] == '[') {
            const char *close = strchr(s + end + 1, ']');
            if (!close || close - (s + end) > 64) {
                free(visible);
                return 0;
            }
            end = (size_t)(close - s) + 1;
        } else if (!single_known || !strchr("Gg", s[code])) {
            free(visible);
            return 0;
        }

        visible[out++] = ' ';
        i = end;
    }
    visible[out] = 0;
    int result = should_warm_text(visible);
    free(visible);
    return result;
}

/* 判断字符串是否纯 ASCII（无多字节 UTF-8 序列） */
static int ascii_only(const char *s) {
    const unsigned char *p = (const unsigned char *)s;
    while (*p) {
        if (*p >= 0x80) return 0;
        p++;
    }
    return 1;
}

/* 检查 s 是否包含 chars 中的任一字符 */
static int contains_any(const char *s, const char *chars) {
    for (; s && *s; s++) {
        if (strchr(chars, *s)) return 1;
    }
    return 0;
}

/* 不区分大小写检查 s 是否以 word 开头，且后面是分隔符或行尾 */
static int starts_with_word_i(const char *s, const char *word) {
    size_t n = strlen(word);
    return !_strnicmp(s, word, n) &&
           (s[n] == 0 || s[n] == ' ' || s[n] == '\t' || s[n] == ':' || s[n] == '(');
}

/* ----------------------------------------------------------------
 * should_warm_renpy_text — Ren'Py 专用文本过滤
 *
 * 在通用过滤基础上增加：
 *   - 排除过短（<3）的字符串
 *   - 排除不含标点且太短（<18）的字符串（可能是变量名）
 *   - 排除各种资源文件扩展名
 * ---------------------------------------------------------------- */
static int should_warm_renpy_text(const char *s) {
    if (!should_warm_text(s)) return 0;
    size_t len = strlen(s);
    if (len < 3) return 0;
    if (!contains_any(s, " \t.?!,:;'-\"") && len < 18) return 0;
    if (strstr(s, "://") || strstr(s, ".rpy") || strstr(s, ".png") ||
        strstr(s, ".jpg") || strstr(s, ".webp") || strstr(s, ".ogg") ||
        strstr(s, ".mp3") || strstr(s, ".wav")) {
        return 0;
    }
    return 1;
}

/* ----------------------------------------------------------------
 * should_warm_unity_asset_text — Unity 资源字符串专用过滤
 *
 * Unity .assets 文件含大量非对话字符串（材质名、动画状态名、
 * Shader 名等），需要严格过滤：
 *   - 仅接受纯 ASCII 文本
 *   - 排除 Unity 内部命名（Base Layer/Atlas/Material/Shader/...）
 *   - 排除以特定前缀开头的标识符（_ / { / Unity / Render / ...）
 *   - 排除带括号的命名（如 "XXX (Clone)"）
 * ---------------------------------------------------------------- */
static int should_warm_unity_asset_text(const char *s) {
    if (!should_warm_text(s) || !ascii_only(s)) return 0;
    if (!contains_any(s, " \t.?!,:;<>")) return 0;
    if (strstr(s, "Base Layer") || strstr(s, " -> ") ||
        strstr(s, ".assets") || strstr(s, ".resource") ||
        strstr(s, "Atlas") || strstr(s, "Material") ||
        strstr(s, "SDF") ||
        strstr(s, "DebugUI") || strstr(s, " Track") ||
        strstr(s, "Scrollbar") || strstr(s, "Sliding Area") ||
        strstr(s, "Signal ") || strstr(s, "Activation ") ||
        strstr(s, "Animation ") || strstr(s, "Cinemachine") ||
        strstr(s, "Audio Track") || strstr(s, "Override ") ||
        strstr(s, " Button") || strstr(s, "Font") ||
        strstr(s, "Texture") || strstr(s, "Shader") ||
        strstr(s, "Lightmap") || strstr(s, "Sprite")) {
        return 0;
    }
    size_t len = strlen(s);
    if (len > 3 && s[len - 1] == ')' && strstr(s, " (")) return 0;
    if (!strncmp(s, "_", 1) || !strncmp(s, "{", 1) ||
        !strncmp(s, "<noninit>", 9) ||
        !strncmp(s, "Unity", 5) || !strncmp(s, "Render", 6) ||
        !strncmp(s, "Recorded", 8) || !strncmp(s, "LineBreaking", 12)) {
        return 0;
    }
    return 1;
}

/* ======================== ByteBuf 与 JSON 解析辅助 ======================== */

/* 向 ByteBuf 追加单个字符 */
static void bb_ch(ByteBuf *b, char c) {
    bb_add(b, &c, 1);
}

/* 十六进制字符转数值（0-15），非法字符返回 -1 */
static int hex_value(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* 将 Unicode 码点编码为 UTF-8 并追加到 ByteBuf */
static void bb_utf8(ByteBuf *b, unsigned cp) {
    if (cp < 0x80) {
        bb_ch(b, (char)cp);
    } else if (cp < 0x800) {
        bb_ch(b, (char)(0xc0 | (cp >> 6)));
        bb_ch(b, (char)(0x80 | (cp & 63)));
    } else if (cp < 0x10000) {
        bb_ch(b, (char)(0xe0 | (cp >> 12)));
        bb_ch(b, (char)(0x80 | ((cp >> 6) & 63)));
        bb_ch(b, (char)(0x80 | (cp & 63)));
    } else if (cp <= 0x10ffff) {
        bb_ch(b, (char)(0xf0 | (cp >> 18)));
        bb_ch(b, (char)(0x80 | ((cp >> 12) & 63)));
        bb_ch(b, (char)(0x80 | ((cp >> 6) & 63)));
        bb_ch(b, (char)(0x80 | (cp & 63)));
    }
}

/* 解析 4 位十六进制 Unicode 转义（\uXXXX） */
static int json_u4(const char *p, unsigned *out) {
    if (!p[0] || !p[1] || !p[2] || !p[3]) return 0;
    int a = hex_value(p[0]), b = hex_value(p[1]), c = hex_value(p[2]), d = hex_value(p[3]);
    if (a < 0 || b < 0 || c < 0 || d < 0) return 0;
    *out = (unsigned)((a << 12) | (b << 8) | (c << 4) | d);
    return 1;
}

/* 跳过 JSON 空白字符 */
static const char *json_ws(const char *p) {
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
    return p;
}

/* ----------------------------------------------------------------
 * json_string_at — 解析 JSON 字符串字面量
 *
 * 处理标准 JSON 转义序列（\n \r \t \uXXXX），
 * 支持 UTF-16 代理对（surrogate pair）合并为完整码点。
 * 返回新分配的 UTF-8 字符串，调用者负责 free。
 * ---------------------------------------------------------------- */
static char *json_string_at(const char **pp) {
    const char *p = json_ws(*pp);
    if (*p != '"') return NULL;
    p++;
    ByteBuf b = {0};
    b.cap = 64;
    b.data = (char *)malloc(b.cap);
    if (!b.data) return NULL;
    b.data[0] = 0;
    while (*p && *p != '"') {
        unsigned char c = (unsigned char)*p++;
        if (c == '\\') {
            char e = *p;
            if (!e) break;
            p++;
            if (e == 'n') bb_ch(&b, '\n');
            else if (e == 'r') bb_ch(&b, '\r');
            else if (e == 't') bb_ch(&b, '\t');
            else if (e == 'u') {
                unsigned cp = 0;
                if (!json_u4(p, &cp)) break;
                p += 4;
                /* 处理 UTF-16 代理对：高位代理后跟低位代理 */
                if (cp >= 0xd800 && cp <= 0xdbff) {
                    unsigned lo = 0;
                    if (p[0] == '\\' && p[1] == 'u' && json_u4(p + 2, &lo) && lo >= 0xdc00 && lo <= 0xdfff) {
                        cp = 0x10000 + ((cp - 0xd800) << 10) + (lo - 0xdc00);
                        p += 6;
                    } else {
                        cp = 0xfffd;  /* 孤立高位代理 → 替换字符 */
                    }
                } else if (cp >= 0xdc00 && cp <= 0xdfff) {
                    cp = 0xfffd;  /* 孤立低位代理 → 替换字符 */
                }
                bb_utf8(&b, cp);
            } else {
                bb_ch(&b, e);
            }
        } else {
            bb_ch(&b, (char)c);
        }
    }
    if (*p == '"') p++;
    *pp = p;
    return b.data;
}

/* ======================== RPG Maker MV/MZ 文本字段判断 ======================== */

/* 判断 JSON key 是否为 RPG Maker 中含可翻译文本的字段名 */
static int rpgm_text_key(const char *key) {
    return !strcmp(key, "name") ||
           !strcmp(key, "nickname") ||
           !strcmp(key, "description") ||
           !strcmp(key, "profile") ||
           !strcmp(key, "displayName") ||
           !strcmp(key, "message1") ||
           !strcmp(key, "message2") ||
           !strcmp(key, "message3") ||
           !strcmp(key, "message4");
}

/* 判断 RPG Maker 事件命令 code 是否含对话/选项文本
 *   101 = Show Choices（选项）
 *   102 = Show Choices
 *   401 = Show Text（对话）
 *   405 = Show Scrolling Text（滚动文本）
 */
static int rpgm_text_command(int code) {
    return code == 101 || code == 102 || code == 401 || code == 405;
}

/* 采集 RPG Maker 字符串：保留原始控制码作为缓存键，只用可见文本做过滤。 */
static void collect_string(char *s, TextList *prefetch) {
    char *t = trim_ascii(s);
    if (should_warm_rpgm_text(t)) textlist_add(prefetch, t);
}

/* Ren'Py 使用更严格的过滤，避免把脚本标识符和资源名加入预热队列。 */
static void collect_renpy_string(char *s, TextList *prefetch) {
    char *t = trim_ascii(s);
    if (should_warm_renpy_text(t)) textlist_add(prefetch, t);
}

/* ----------------------------------------------------------------
 * collect_array_strings — 采集 JSON 数组中的所有字符串元素
 *
 * 遍历 [...] 结构，对每个字符串字面量调用 collect_string。
 * 用于 RPG Maker 的 message1..4、parameters 等数组字段。
 * ---------------------------------------------------------------- */
static void collect_array_strings(const char **pp, TextList *prefetch) {
    const char *p = json_ws(*pp);
    if (*p != '[') return;
    int depth = 0;
    do {
        if (*p == '[') {
            depth++;
            p++;
        } else if (*p == ']') {
            depth--;
            p++;
        } else if (*p == '"') {
            char *s = json_string_at(&p);
            if (s) {
                collect_string(s, prefetch);
                free(s);
            }
        } else {
            p++;
        }
    } while (*p && depth > 0);
    *pp = p;
}

/* ======================== Ren'Py 脚本解析 ======================== */

/* ----------------------------------------------------------------
 * renpy_string_at — 解析 Ren'Py 字符串字面量
 *
 * 支持 " 和 ' 两种引号，处理 \n \r \t \uXXXX 转义。
 * 跳过三引号字符串（""" 或 '''）——这些通常是文档字符串。
 * ---------------------------------------------------------------- */
static char *renpy_string_at(const char **pp) {
    const char *p = *pp;
    char quote = *p;
    if (quote != '"' && quote != '\'') return NULL;
    if (p[1] == quote && p[2] == quote) return NULL;  /* 三引号跳过 */
    p++;
    ByteBuf b = {0};
    if (!bb_init(&b, 64)) return NULL;
    while (*p && *p != quote && *p != '\r' && *p != '\n') {
        unsigned char c = (unsigned char)*p++;
        if (c == '\\') {
            char e = *p;
            if (!e) break;
            p++;
            if (e == 'n') bb_ch(&b, '\n');
            else if (e == 'r') bb_ch(&b, '\r');
            else if (e == 't') bb_ch(&b, '\t');
            else if (e == 'u') {
                unsigned cp = 0;
                if (json_u4(p, &cp)) {
                    p += 4;
                    bb_utf8(&b, cp);
                } else {
                    bb_ch(&b, 'u');
                }
            } else {
                bb_ch(&b, e);
            }
        } else {
            bb_ch(&b, (char)c);
        }
    }
    if (*p != quote) {
        free(b.data);
        *pp = p;
        return NULL;
    }
    p++;
    *pp = p;
    return b.data;
}

/* 找到一行中最先出现的单引号或双引号。 */
static const char *renpy_first_quote(const char *line) {
    const char *single = strchr(line, '\'');
    const char *dbl = strchr(line, '"');
    if (!single) return dbl;
    if (!dbl) return single;
    return single < dbl ? single : dbl;
}

/* ----------------------------------------------------------------
 * renpy_skip_statement — 判断 Ren'Py 行是否应跳过（非对话语句）
 *
 * 跳过注释、三引号、赋值语句、以及所有 Ren'Py 指令关键字
 * （image/scene/show/hide/play/jump/call/screen/python/if/...）。
 * first_quote 用于判断赋值号是否在引号之前（= 在引号前 → 赋值语句）。
 * ---------------------------------------------------------------- */
static int renpy_skip_statement(const char *line, const char *first_quote) {
    if (!line || !*line || *line == '#') return 1;
    if (strstr(line, "\"\"\"") || strstr(line, "'''")) return 1;
    const char *eq = strchr(line, '=');
    if (eq && first_quote && eq < first_quote) return 1;
    return starts_with_word_i(line, "image") ||
           starts_with_word_i(line, "scene") ||
           starts_with_word_i(line, "show") ||
           starts_with_word_i(line, "hide") ||
           starts_with_word_i(line, "play") ||
           starts_with_word_i(line, "queue") ||
           starts_with_word_i(line, "stop") ||
           starts_with_word_i(line, "with") ||
           starts_with_word_i(line, "jump") ||
           starts_with_word_i(line, "call") ||
           starts_with_word_i(line, "return") ||
           starts_with_word_i(line, "label") ||
           starts_with_word_i(line, "define") ||
           starts_with_word_i(line, "default") ||
           starts_with_word_i(line, "style") ||
           starts_with_word_i(line, "transform") ||
           starts_with_word_i(line, "screen") ||
           starts_with_word_i(line, "init") ||
           starts_with_word_i(line, "python") ||
           starts_with_word_i(line, "if") ||
           starts_with_word_i(line, "elif") ||
           starts_with_word_i(line, "else") ||
           starts_with_word_i(line, "for") ||
           starts_with_word_i(line, "while") ||
           *line == '#';
}

/* ----------------------------------------------------------------
 * bb_json — 将字符串 JSON 转义后追加到 ByteBuf
 * 转义 " \ \n \r \t，其他控制字符用 \uXXXX 形式输出
 * ---------------------------------------------------------------- */
static void bb_json(ByteBuf *b, const char *s) {
    bb_add(b, "\"", 1);
    for (; s && *s; s++) {
        unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\') {
            char tmp[2] = {'\\', (char)c};
            bb_add(b, tmp, 2);
        } else if (c == '\n') bb_add(b, "\\n", 2);
        else if (c == '\r') bb_add(b, "\\r", 2);
        else if (c == '\t') bb_add(b, "\\t", 2);
        else if (c < 32) {
            char tmp[7];
            _snprintf(tmp, sizeof tmp, "\\u%04x", c);
            bb_add(b, tmp, strlen(tmp));
        } else {
            bb_add(b, (const char *)&c, 1);
        }
    }
    bb_add(b, "\"", 1);
}

/* 初始化可增长字节缓冲。成功后 data 归 ByteBuf/调用方所有，最终必须 free。 */
static int bb_init(ByteBuf *b, size_t cap) {
    b->len = 0;
    b->cap = cap ? cap : 64;
    b->data = (char *)malloc(b->cap);
    if (!b->data) {
        b->cap = 0;
        return 0;
    }
    b->data[0] = 0;
    return 1;
}

/* 启动器私有的 FNV-1a；启动器不链接服务端 util.c，不能直接复用 h64。 */
static uint64_t warmup_hash(const char *s) {
    uint64_t h = 1469598103934665603ULL;
    while (*s) {
        h ^= (unsigned char)*s++;
        h *= 1099511628211ULL;
    }
    return h;
}

/* O(1) 去重表，替代旧版 O(n) 线性扫描。槽位只借用列表已持有字符串的指针，
   不拥有字符串；查询可使用候选内容，写入必须使用已经复制进列表的稳定地址。 */
static int dedup_contains(const char **slots, size_t cap, const char *s) {
    if (!slots || cap == 0) return 0;
    size_t mask = cap - 1;
    size_t i = (size_t)warmup_hash(s) & mask;
    while (slots[i]) {
        if (!strcmp(slots[i], s)) return 1;
        i = (i + 1) & mask;
    }
    return 0;
}

/* 把列表拥有的稳定字符串地址加入去重表。表按列表硬上限一次定容，
   保持负载率低于 0.5；分配失败时只退化为“无去重”，列表上限仍限制内存。 */
static void dedup_add(const char ***pslots, size_t *pcap, const char *s, size_t max_items) {
    if (*pcap == 0) {
        /* Size for load factor < 0.5 at the list's item cap (4096 slots for
           the default 1200-item lists), so the table never fills. */
        size_t want = 1u << 12;
        while (want < max_items * 2) want <<= 1;
        *pcap = want;
        *pslots = (const char **)calloc(*pcap, sizeof **pslots);
        if (!*pslots) { *pcap = 0; return; } /* OOM: dedup degrades, list cap still bounds growth */
    }
    const char **slots = *pslots;
    size_t mask = *pcap - 1;
    size_t i = (size_t)warmup_hash(s) & mask;
    while (slots[i]) {
        if (!strcmp(slots[i], s)) return;
        i = (i + 1) & mask;
    }
    slots[i] = s;
}

/* 复制并接管一条待预热文本；超长、重复或超过引擎容量上限时直接忽略。 */
static void textlist_add(TextList *l, const char *s) {
    if (!s || !*s || strlen(s) > WARMUP_MAX_TEXT_BYTES) return;
    if (dedup_contains(l->seen, l->seen_cap, s)) return;
    if (l->n >= textlist_limit(l)) return;
    if (l->n == l->cap) {
        size_t cap = l->cap ? l->cap * 2 : 64;
        char **p = (char **)malloc(cap * sizeof *l->items);
        if (!p) return;
        if (l->items) memcpy(p, l->items, l->n * sizeof *l->items);
        free(l->items);
        l->items = p;
        l->cap = cap;
    }
    char *copy = dup_range(s, strlen(s));
    if (!copy) return;
    l->items[l->n++] = copy;
    dedup_add(&l->seen, &l->seen_cap, copy, textlist_limit(l));
}

/* 复制并接管一组原文/译文，用于把已有 XUnity 翻译导入本地服务端缓存。
   去重键仅为原文：同一原文不会在一次预热中重复导入。 */
static void pairlist_add(PairList *l, const char *k, const char *v) {
    if (!k || !v || !*k || !*v || strlen(k) > WARMUP_MAX_TEXT_BYTES) return;
    if (dedup_contains(l->seen, l->seen_cap, k)) return;
    if (l->n >= WARMUP_MAX_ITEMS) return;
    if (l->n == l->cap) {
        size_t cap = l->cap ? l->cap * 2 : 64;
        char **keys = (char **)malloc(cap * sizeof *l->keys);
        char **vals = (char **)malloc(cap * sizeof *l->vals);
        if (!keys || !vals) {
            free(keys);
            free(vals);
            return;
        }
        if (l->keys) memcpy(keys, l->keys, l->n * sizeof *l->keys);
        if (l->vals) memcpy(vals, l->vals, l->n * sizeof *l->vals);
        free(l->keys);
        free(l->vals);
        l->keys = keys;
        l->vals = vals;
        l->cap = cap;
    }
    l->keys[l->n] = dup_range(k, strlen(k));
    l->vals[l->n] = dup_range(v, strlen(v));
    if (l->keys[l->n] && l->vals[l->n]) {
        dedup_add(&l->seen, &l->seen_cap, l->keys[l->n], WARMUP_MAX_ITEMS);
        l->n++;
    } else {
        free(l->keys[l->n]);
        free(l->vals[l->n]);
    }
}

/* 释放 TextList 拥有的文本、指针数组和只借用这些文本地址的去重表。 */
static void textlist_free(TextList *l) {
    for (size_t i = 0; i < l->n; i++) free(l->items[i]);
    free(l->items);
    free(l->seen);
}

/* 释放 PairList 对键和值的全部所有权；seen 不单独拥有键字符串。 */
static void pairlist_free(PairList *l) {
    for (size_t i = 0; i < l->n; i++) {
        free(l->keys[i]);
        free(l->vals[i]);
    }
    free(l->keys);
    free(l->vals);
    free(l->seen);
}

/* 可复用的 localhost 会话。预热可能提交数百批；每批重建 WinHTTP
   session/connection 会显著拖慢启动阶段。句柄由 local_http_close 统一释放。 */
typedef struct {
    HINTERNET ses;
    HINTERNET con;
} LocalHttp;

/* 建立到固定本地服务端的会话和连接；失败时回收已经创建的部分句柄。 */
static int local_http_open(LocalHttp *h) {
    h->ses = WinHttpOpen(L"ds-game-translator Launcher/3.1",
                         WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                         WINHTTP_NO_PROXY_NAME,
                         WINHTTP_NO_PROXY_BYPASS, 0);
    if (!h->ses) return 0;
    h->con = WinHttpConnect(h->ses, L"127.0.0.1", 19999, 0);
    if (!h->con) {
        WinHttpCloseHandle(h->ses);
        h->ses = NULL;
        return 0;
    }
    return 1;
}

/* 在复用连接上发送一批 JSON。请求句柄仅在本函数内存活；
   返回值表示请求和响应头是否成功，不代表后台翻译已经完成。 */
static int local_http_post(LocalHttp *h, const WCHAR *path, const char *body, DWORD timeout) {
    if (!h->con) return 0;
    HINTERNET req = WinHttpOpenRequest(h->con, L"POST", path, NULL, WINHTTP_NO_REFERER,
                                       WINHTTP_DEFAULT_ACCEPT_TYPES, 0);
    if (!req) return 0;
    WinHttpSetTimeouts(req, timeout, timeout, timeout, timeout);
    WCHAR headers[] = L"Content-Type: application/json\r\n";
    DWORD len = (DWORD)strlen(body);
    int ok = WinHttpSendRequest(req, headers, (DWORD)-1, (LPVOID)body, len, len, 0) &&
             WinHttpReceiveResponse(req, NULL);
    WinHttpCloseHandle(req);
    return ok;
}

/* 发送轻量 GET 并读取 HTTP 状态码，供就绪探测使用；不读取响应体。 */
static int local_http_get_status(LocalHttp *h, const WCHAR *path, DWORD timeout, DWORD *status_out) {
    if (status_out) *status_out = 0;
    if (!h->con) return 0;
    HINTERNET req = WinHttpOpenRequest(h->con, L"GET", path, NULL, WINHTTP_NO_REFERER,
                                       WINHTTP_DEFAULT_ACCEPT_TYPES, 0);
    if (!req) return 0;
    WinHttpSetTimeouts(req, timeout, timeout, timeout, timeout);
    DWORD status = 0;
    DWORD status_len = sizeof status;
    int ok = WinHttpSendRequest(req, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                                WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
             WinHttpReceiveResponse(req, NULL) &&
             WinHttpQueryHeaders(req,
                                 WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                                 WINHTTP_HEADER_NAME_BY_INDEX,
                                 &status,
                                 &status_len,
                                 WINHTTP_NO_HEADER_INDEX);
    WinHttpCloseHandle(req);
    if (status_out) *status_out = status;
    return ok;
}

/* 在总超时内轮询 /health，只有明确收到 HTTP 200 才允许提交预热批次。 */
static int local_http_wait_ready(LocalHttp *h, DWORD total_timeout_ms) {
    DWORD start = GetTickCount();
    for (;;) {
        DWORD status = 0;
        if (local_http_get_status(h, L"/health", 350, &status) && status == 200) {
            return 1;
        }
        if (GetTickCount() - start >= total_timeout_ms) {
            return 0;
        }
        Sleep(100);
    }
}

/* 关闭 LocalHttp 拥有的连接和会话；可安全用于部分初始化或重复清理。 */
static void local_http_close(LocalHttp *h) {
    if (h->con) WinHttpCloseHandle(h->con);
    if (h->ses) WinHttpCloseHandle(h->ses);
    h->con = NULL;
    h->ses = NULL;
}

/* 读取完整 WinHTTP 响应体。成功返回新分配的 NUL 结尾缓冲，
   所有权交给调用方；网络中断时返回已经读取到的内容或空缓冲。 */
static char *winhttp_read_all(HINTERNET req) {
    ByteBuf b = {0};
    if (!bb_init(&b, 1024)) return NULL;

    for (;;) {
        DWORD avail = 0;
        if (!WinHttpQueryDataAvailable(req, &avail) || !avail) break;
        char *tmp = (char *)malloc(avail);
        if (!tmp) break;
        DWORD rd = 0;
        if (!WinHttpReadData(req, tmp, avail, &rd) || !rd) {
            free(tmp);
            break;
        }
        bb_add(&b, tmp, rd);
        free(tmp);
    }
    return b.data;
}

/* RFC 3986 查询参数中可直接保留的 ASCII 字节。 */
static int url_unreserved(unsigned char c) {
    return (c >= 'A' && c <= 'Z') ||
           (c >= 'a' && c <= 'z') ||
           (c >= '0' && c <= '9') ||
           c == '-' || c == '_' || c == '.' || c == '~';
}

/* 按 UTF-8 原始字节做百分号编码；返回缓冲由调用方 free。 */
static char *url_encode_utf8(const char *s) {
    ByteBuf b = {0};
    if (!bb_init(&b, strlen(s) * 3 + 1)) return NULL;
    static const char hex[] = "0123456789ABCDEF";
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        if (url_unreserved(*p)) {
            bb_ch(&b, (char)*p);
        } else {
            char tmp[3] = {'%', hex[*p >> 4], hex[*p & 15]};
            bb_add(&b, tmp, 3);
        }
    }
    return b.data;
}

/* 把 UTF-8 字符串复制为 UTF-16；返回缓冲由调用方 free。 */
static WCHAR *utf8_to_wide_dup(const char *s) {
    int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, NULL, 0);
    if (n <= 0) return NULL;
    WCHAR *w = (WCHAR *)malloc((size_t)n * sizeof(WCHAR));
    if (!w) return NULL;
    if (!MultiByteToWideChar(CP_UTF8, 0, s, -1, w, n)) {
        free(w);
        return NULL;
    }
    return w;
}

/* 通过 cache_only GET 查询一条现有译文，不等待远程 API。
   成功时 *out 接管响应体，即使内容等于原文也由上层按缓存语义判断。 */
static int localhost_get_cached_translate(const char *text, char **out) {
    *out = NULL;
    char *enc = url_encode_utf8(text);
    if (!enc) return 0;

    ByteBuf path8 = {0};
    if (!bb_init(&path8, strlen(enc) + 64)) {
        free(enc);
        return 0;
    }
    bb_add(&path8, "/translate?cache_only=true&text=", 32);
    bb_add(&path8, enc, strlen(enc));
    free(enc);
    WCHAR *path = utf8_to_wide_dup(path8.data);
    free(path8.data);
    if (!path) return 0;

    HINTERNET ses = WinHttpOpen(L"ds-game-translator Launcher/3.1",
                                WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                WINHTTP_NO_PROXY_NAME,
                                WINHTTP_NO_PROXY_BYPASS, 0);
    if (!ses) {
        free(path);
        return 0;
    }
    HINTERNET con = WinHttpConnect(ses, L"127.0.0.1", 19999, 0);
    if (!con) {
        WinHttpCloseHandle(ses);
        free(path);
        return 0;
    }
    HINTERNET req = WinHttpOpenRequest(con, L"GET", path, NULL, WINHTTP_NO_REFERER,
                                       WINHTTP_DEFAULT_ACCEPT_TYPES, 0);
    free(path);
    if (!req) {
        WinHttpCloseHandle(con);
        WinHttpCloseHandle(ses);
        return 0;
    }
    DWORD timeout = 350;
    WinHttpSetTimeouts(req, timeout, timeout, timeout, timeout);
    int ok = WinHttpSendRequest(req, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                                WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
             WinHttpReceiveResponse(req, NULL);
    char *body = ok ? winhttp_read_all(req) : NULL;
    WinHttpCloseHandle(req);
    WinHttpCloseHandle(con);
    WinHttpCloseHandle(ses);
    if (!body) return 0;
    *out = body;
    return 1;
}

/* 找到 XUnity 文本行中第一个未转义的 '='，用于分隔原文和译文。 */
static char *xunity_find_separator(char *line) {
    int escaped = 0;
    for (char *p = line; *p; p++) {
        if (escaped) {
            escaped = 0;
            continue;
        }
        if (*p == '\\') {
            escaped = 1;
            continue;
        }
        if (*p == '=') return p;
    }
    return NULL;
}

/* 解码 XUnity 文本文件支持的转义；未知转义保持反斜杠，避免破坏原文。 */
static char *xunity_unescape(const char *s) {
    ByteBuf b = {0};
    if (!bb_init(&b, strlen(s) + 1)) return NULL;
    for (const char *p = s; *p; p++) {
        if (*p != '\\' || !p[1]) {
            bb_ch(&b, *p);
            continue;
        }
        p++;
        if (*p == 'n') bb_ch(&b, '\n');
        else if (*p == 'r') bb_ch(&b, '\r');
        else if (*p == 't') bb_ch(&b, '\t');
        else if (*p == '=' || *p == '\\') bb_ch(&b, *p);
        else {
            bb_ch(&b, '\\');
            bb_ch(&b, *p);
        }
    }
    return b.data;
}

/* 按 XUnity 文本格式转义换行、制表符、反斜杠和键值分隔符。 */
static void xunity_escape_to(ByteBuf *b, const char *s) {
    for (const char *p = s; p && *p; p++) {
        if (*p == '\n') bb_add(b, "\\n", 2);
        else if (*p == '\r') bb_add(b, "\\r", 2);
        else if (*p == '\t') bb_add(b, "\\t", 2);
        else if (*p == '=' || *p == '\\') {
            bb_ch(b, '\\');
            bb_ch(b, *p);
        } else {
            bb_ch(b, *p);
        }
    }
}

/* 首次修改翻译文件前保留一份固定后缀备份；已有备份不会被覆盖。 */
static void backup_once(const WCHAR *path) {
    WCHAR bak[MAX_PATH * 4];
    _snwprintf(bak, MAX_PATH * 4, L"%s.deepseek.bak", path);
    bak[MAX_PATH * 4 - 1] = 0;
    CopyFileW(path, bak, TRUE);
}

/* 把待翻译文本序列化后提交到 /prefetch。成功只表示服务端接受批次，
   实际翻译仍在后台进行，因此不得把原文当作成功译文写回文件。 */
static int post_prefetch_batch(LocalHttp *http, TextList *l, size_t start, size_t count) {
    ByteBuf b = {0};
    b.cap = 2048;
    b.data = (char *)malloc(b.cap);
    if (!b.data) return 0;
    b.data[0] = 0;
    bb_add(&b, "{\"texts\":[", 10);
    for (size_t i = 0; i < count; i++) {
        if (i) bb_add(&b, ",", 1);
        bb_json(&b, l->items[start + i]);
    }
    bb_add(&b, "]}", 2);
    int ok = local_http_post(http, L"/prefetch", b.data, 1500);
    free(b.data);
    return ok;
}

/* 把已有原文/译文对提交到 /cache/import；该路径只导入确定译文。 */
static int post_import_batch(LocalHttp *http, PairList *l, size_t start, size_t count) {
    ByteBuf b = {0};
    b.cap = 4096;
    b.data = (char *)malloc(b.cap);
    if (!b.data) return 0;
    b.data[0] = 0;
    bb_add(&b, "{\"entries\":[", 12);
    for (size_t i = 0; i < count; i++) {
        if (i) bb_add(&b, ",", 1);
        bb_add(&b, "{\"key\":", 7);
        bb_json(&b, l->keys[start + i]);
        bb_add(&b, ",\"value\":", 9);
        bb_json(&b, l->vals[start + i]);
        bb_add(&b, "}", 1);
    }
    bb_add(&b, "]}", 2);
    int ok = local_http_post(http, L"/cache/import", b.data, 1500);
    free(b.data);
    return ok;
}

/* 解析并按需修补一个 XUnity 翻译文件：
   已有译文进入 imports，空译文进入 prefetch；写回前先创建一次备份。 */
static void parse_translation_file(const WCHAR *path, PairList *imports, TextList *prefetch) {
    char *buf = NULL;
    DWORD size = 0;
    if (!read_file_bytes(path, &buf, &size)) return;

    ByteBuf out = {0};
    int changed = 0;
    if (!bb_init(&out, size + 256)) {
        free(buf);
        return;
    }

    char *p = buf;
    if (size >= 3 && (unsigned char)p[0] == 0xef && (unsigned char)p[1] == 0xbb && (unsigned char)p[2] == 0xbf) {
        bb_add(&out, "\xef\xbb\xbf", 3);
        p += 3;
    }

    while (*p) {
        char *line_start = p;
        while (*p && *p != '\r' && *p != '\n') p++;
        size_t line_len = (size_t)(p - line_start);
        int had_cr = (*p == '\r');
        if (*p == '\r') p++;
        int had_lf = (*p == '\n');
        if (*p == '\n') p++;

        char *work = dup_range(line_start, line_len);
        if (!work) continue;
        char *line = trim_ascii(work);
        int keep_original = 1;

        if (*line && strncmp(line, "//", 2) && *line != '#' && *line != ';') {
            char *eq = xunity_find_separator(line);
            if (eq) {
                *eq = 0;
                char *raw_key = line;
                char *raw_val = eq + 1;
                char *key = xunity_unescape(raw_key);
                char *val = xunity_unescape(raw_val);
                if (key && val && *key && should_warm_text(key)) {
                    if (*val && strcmp(key, val)) {
                        pairlist_add(imports, key, val);
                    } else {
                        char *cached = NULL;
                        if (localhost_get_cached_translate(key, &cached) &&
                            cached && *cached && strcmp(cached, key) && should_warm_text(key)) {
                            xunity_escape_to(&out, key);
                            bb_ch(&out, '=');
                            xunity_escape_to(&out, cached);
                            if (had_cr) bb_ch(&out, '\r');
                            if (had_lf) bb_ch(&out, '\n');
                            pairlist_add(imports, key, cached);
                            keep_original = 0;
                            changed = 1;
                        } else {
                            textlist_add(prefetch, key);
                            keep_original = 0;
                            changed = 1;
                        }
                        free(cached);
                    }
                }
                free(key);
                free(val);
            }
        }

        if (keep_original) {
            bb_add(&out, line_start, line_len);
            if (had_cr) bb_ch(&out, '\r');
            if (had_lf) bb_ch(&out, '\n');
        }
        free(work);
    }

    if (changed) {
        backup_once(path);
        write_file_bytes(path, out.data, (DWORD)out.len);
    }
    free(out.data);
    free(buf);
}

/* ----------------------------------------------------------------
 * scan_text_dir — 扫描 XUnity 翻译目录中的 .txt 文件
 *
 * 跳过 XUnity 内部配置文件（_Substitutions / _Preprocessors /
 * _Postprocessors），对其余文件调用 parse_translation_file。
 * ---------------------------------------------------------------- */
static void scan_text_dir(const WCHAR *dir, PairList *imports, TextList *prefetch) {
    if (!is_dir(dir)) return;
    WCHAR pat[MAX_PATH * 4];
    path_join(pat, MAX_PATH * 4, dir, L"*.txt");
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        if (!_wcsicmp(fd.cFileName, L"_Substitutions.txt") ||
            !_wcsicmp(fd.cFileName, L"_Preprocessors.txt") ||
            !_wcsicmp(fd.cFileName, L"_Postprocessors.txt")) {
            continue;
        }
        WCHAR p[MAX_PATH * 4];
        path_join(p, MAX_PATH * 4, dir, fd.cFileName);
        parse_translation_file(p, imports, prefetch);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

/* ----------------------------------------------------------------
 * unity_asset_file_name — 判断文件名是否为 Unity 资源文件
 *
 * 匹配 resources.assets、globalgamemanagers、sharedassets*.assets、
 * level<N>（纯数字后缀的场景文件）。
 * ---------------------------------------------------------------- */
static int unity_asset_file_name(const WCHAR *name) {
    if (!_wcsicmp(name, L"resources.assets") ||
        !_wcsicmp(name, L"globalgamemanagers")) {
        return 1;
    }
    if (!_wcsnicmp(name, L"sharedassets", 12) && wcsstr(name, L".assets")) return 1;
    if (!_wcsnicmp(name, L"level", 5)) {
        const WCHAR *p = name + 5;
        if (!*p) return 0;
        while (*p) {
            if (*p < L'0' || *p > L'9') return 0;
            p++;
        }
        return 1;
    }
    return 0;
}

/* 检查 .assets 文件大小是否在扫描上限内（防止扫描超大文件） */
static int small_enough_to_scan(const WCHAR *path) {
    WIN32_FILE_ATTRIBUTE_DATA d;
    if (!GetFileAttributesExW(path, GetFileExInfoStandard, &d)) return 0;
    if (d.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) return 0;
    if (d.nFileSizeHigh) return 0;
    return d.nFileSizeLow <= UNITY_ASSET_SCAN_MAX_BYTES;
}

/* ----------------------------------------------------------------
 * valid_ascii_payload — 验证字节序列是否为可打印 ASCII 文本
 *
 * 要求：仅含可打印字符（32-126）和空白（\t \n \r），
 * 且至少含一个字母（排除纯数字/符号的噪声）。
 * ---------------------------------------------------------------- */
static int valid_ascii_payload(const unsigned char *p, size_t n) {
    int alpha = 0;
    for (size_t i = 0; i < n; i++) {
        unsigned char c = p[i];
        if (c >= 'A' && c <= 'Z') alpha = 1;
        else if (c >= 'a' && c <= 'z') alpha = 1;
        if (c == 9 || c == 10 || c == 13) continue;
        if (c < 32 || c > 126) return 0;
    }
    return alpha;
}

/* ----------------------------------------------------------------
 * keep_rich_tag — 判断尖括号标签是否为富文本标签（应保留）
 *
 * 保留的标签：color/size/sprite/b/i（以及它们的闭合标签 </...>）。
 * 其他标签（如游戏玩法标记）会被 strip_gameplay_tags 移除。
 * ---------------------------------------------------------------- */
static int keep_rich_tag(const char *tag, size_t n) {
    while (n && (*tag == '/' || *tag == ' ' || *tag == '\t')) {
        tag++;
        n--;
    }
    return (n >= 5 && !_strnicmp(tag, "color", 5)) ||
           (n >= 4 && !_strnicmp(tag, "size", 4)) ||
           (n >= 6 && !_strnicmp(tag, "sprite", 6)) ||
           (n == 1 && (tag[0] == 'b' || tag[0] == 'i'));
}

/* ----------------------------------------------------------------
 * strip_gameplay_tags — 移除游戏玩法标记，保留富文本标签
 *
 * Unity 资源中的对话可能含 <color=...>富文本</color> 和
 * <customTag>玩法标记</customTag>。此函数移除非富文本标签，
 * 提取纯文本版本用于翻译（原始含标签版本也会单独翻译）。
 * ---------------------------------------------------------------- */
static char *strip_gameplay_tags(const char *s) {
    ByteBuf b = {0};
    if (!bb_init(&b, strlen(s) + 1)) return NULL;
    for (const char *p = s; *p; p++) {
        if (*p != '<') {
            bb_ch(&b, *p);
            continue;
        }
        const char *end = strchr(p, '>');
        if (!end || end - p > 96) {
            bb_ch(&b, *p);
            continue;
        }
        if (keep_rich_tag(p + 1, (size_t)(end - p - 1))) {
            bb_add(&b, p, (size_t)(end - p + 1));
        }
        p = end;
    }
    return b.data;
}

/* ----------------------------------------------------------------
 * collect_unity_asset_text — 采集 Unity 资源中的文本字符串
 *
 * 原始文本和去除玩法标签后的纯文本版本都会加入 prefetch
 * （如果两者不同），确保翻译能覆盖两种显示形式。
 * ---------------------------------------------------------------- */
static void collect_unity_asset_text(char *s, TextList *prefetch) {
    char *t = trim_ascii(s);
    if (!should_warm_unity_asset_text(t)) return;
    textlist_add(prefetch, t);

    if (strchr(t, '<')) {
        char *plain = strip_gameplay_tags(t);
        if (plain) {
            char *p = trim_ascii(plain);
            if (strcmp(p, t) && should_warm_unity_asset_text(p)) textlist_add(prefetch, p);
            free(plain);
        }
    }
}

/* ----------------------------------------------------------------
 * scan_unity_asset_file — 扫描 Unity .assets 二进制文件中的字符串
 *
 * Unity 序列化字符串以 小端 4 字节长度前缀 + UTF-8 数据 存储。
 * 遍历文件寻找长度合理的 ASCII 载荷，通过 valid_ascii_payload
 * 过滤非文本数据，再由 collect_unity_asset_text 采集。
 * ---------------------------------------------------------------- */
static void scan_unity_asset_file(const WCHAR *path, TextList *prefetch) {
    if (!small_enough_to_scan(path)) return;
    char *buf = NULL;
    DWORD size = 0;
    if (!read_file_bytes(path, &buf, &size)) return;

    const unsigned char *bytes = (const unsigned char *)buf;
    for (DWORD i = 0; i + 8 < size && prefetch->n < textlist_limit(prefetch); i++) {
        /* 读取小端 4 字节字符串长度 */
        unsigned n = (unsigned)bytes[i] |
                     ((unsigned)bytes[i + 1] << 8) |
                     ((unsigned)bytes[i + 2] << 16) |
                     ((unsigned)bytes[i + 3] << 24);
        if (n < 2 || n > WARMUP_MAX_TEXT_BYTES || i + 4 + n > size) continue;
        if (!valid_ascii_payload(bytes + i + 4, n)) continue;
        char *s = dup_range((const char *)bytes + i + 4, n);
        if (s) {
            collect_unity_asset_text(s, prefetch);
            free(s);
        }
    }
    free(buf);
}

/* 扫描单个 *_Data 目录下的所有资源文件 */
static void scan_unity_data_dir(const WCHAR *data_dir, TextList *prefetch) {
    if (!is_dir(data_dir)) return;
    WCHAR pat[MAX_PATH * 4];
    path_join(pat, MAX_PATH * 4, data_dir, L"*");
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        if (!unity_asset_file_name(fd.cFileName)) continue;
        WCHAR p[MAX_PATH * 4];
        path_join(p, MAX_PATH * 4, data_dir, fd.cFileName);
        scan_unity_asset_file(p, prefetch);
        if (prefetch->n >= textlist_limit(prefetch)) break;
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

/* 查找所有 *_Data 目录并扫描其中的资源文件 */
static void scan_unity_assets(const WCHAR *dir, TextList *prefetch) {
    WCHAR pat[MAX_PATH * 4];
    path_join(pat, MAX_PATH * 4, dir, L"*_Data");
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
        WCHAR data_dir[MAX_PATH * 4];
        path_join(data_dir, MAX_PATH * 4, dir, fd.cFileName);
        scan_unity_data_dir(data_dir, prefetch);
        if (prefetch->n >= textlist_limit(prefetch)) break;
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

/* ======================== RPG Maker MV/MZ 数据解析 ======================== */

/* ----------------------------------------------------------------
 * parse_rpgm_json_file — 解析 RPG Maker 数据 JSON 文件
 *
 * 解析 www/data/ 下的 JSON 文件，提取以下字段的文本：
 *   - code 字段：记录事件命令码，用于判断是否为对话/选项
 *   - parameters 数组：当 code 为对话命令（101/102/401/405）时采集
 *   - 文本字段（name/nickname/description/profile/displayName/message1-4）
 * ---------------------------------------------------------------- */
static void parse_rpgm_json_file(const WCHAR *path, TextList *prefetch) {
    char *buf = NULL;
    DWORD size = 0;
    if (!read_file_bytes(path, &buf, &size)) return;
    const char *p = buf;
    if (size >= 3 && (unsigned char)p[0] == 0xef && (unsigned char)p[1] == 0xbb && (unsigned char)p[2] == 0xbf) p += 3;
    int last_code = -1;

    while (*p) {
        p = json_ws(p);
        if (*p != '"') {
            p++;
            continue;
        }
        char *key = json_string_at(&p);
        if (!key) break;
        const char *v = json_ws(p);
        if (*v != ':') {
            free(key);
            p = v;
            continue;
        }
        v = json_ws(v + 1);

        if (!strcmp(key, "code")) {
            last_code = atoi(v);
        } else if (!strcmp(key, "parameters")) {
            if (rpgm_text_command(last_code)) collect_array_strings(&v, prefetch);
            last_code = -1;
        } else if (rpgm_text_key(key)) {
            if (*v == '"') {
                char *s = json_string_at(&v);
                if (s) {
                    collect_string(s, prefetch);
                    free(s);
                }
            } else if (*v == '[') {
                collect_array_strings(&v, prefetch);
            }
        }
        free(key);
        p = v;
        if (prefetch->n >= textlist_limit(prefetch)) break;
    }
    free(buf);
}

/* 扫描 www/data/ 目录下的所有 .json 数据文件 */
static void scan_rpgm_data_dir(const WCHAR *dir, TextList *prefetch) {
    WCHAR data[MAX_PATH * 4], pat[MAX_PATH * 4];
    path_join(data, MAX_PATH * 4, dir, L"www\\data");
    if (!is_dir(data)) return;
    path_join(pat, MAX_PATH * 4, data, L"*.json");
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        WCHAR p[MAX_PATH * 4];
        path_join(p, MAX_PATH * 4, data, fd.cFileName);
        parse_rpgm_json_file(p, prefetch);
        if (prefetch->n >= textlist_limit(prefetch)) break;
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

/* 采集 RPG Maker 插件外部文本的一行。Galv Quest Log 等插件把标题编码为
   <quest id:标题|难度|分类>，运行时只绘制“标题”，因此缓存键也必须单独提取。 */
static void collect_rpgm_text_line(char *line, TextList *prefetch) {
    char *text = trim_ascii(line);
    if (!*text || *text == '#') return;
    if (!_strnicmp(text, "<quest ", 7)) {
        char *colon = strchr(text + 7, ':');
        char *end = strchr(text + 7, '>');
        if (!colon || !end || colon >= end) return;
        char *pipe = strchr(colon + 1, '|');
        if (pipe && pipe < end) end = pipe;
        char saved = *end;
        *end = 0;
        collect_string(colon + 1, prefetch);
        *end = saved;
        return;
    }
    if (*text == '<') return; /* 结束标签和其他插件控制标签不是显示文本 */
    collect_string(text, prefetch);
}

/* 读取一个插件外部 .txt 文件并逐行采集。限制 8 MiB，防止把异常大文件
   当成本地化资源；只读扫描，不回写用户文件。 */
static void scan_rpgm_text_file(const WCHAR *path, TextList *prefetch) {
    char *buf = NULL;
    DWORD size = 0;
    if (!read_file_bytes(path, &buf, &size)) return;
    if (size > 8u * 1024u * 1024u) {
        free(buf);
        return;
    }
    char *line = buf;
    if (size >= 3 &&
        (unsigned char)line[0] == 0xef &&
        (unsigned char)line[1] == 0xbb &&
        (unsigned char)line[2] == 0xbf) {
        line += 3;
    }
    for (char *p = line;; p++) {
        if (*p != '\r' && *p != '\n' && *p != 0) continue;
        char separator = *p;
        *p = 0;
        collect_rpgm_text_line(line, prefetch);
        if (!separator || prefetch->n >= textlist_limit(prefetch)) break;
        if (separator == '\r' && p[1] == '\n') p++;
        line = p + 1;
    }
    free(buf);
}

static int skip_rpgm_text_directory(const WCHAR *name) {
    return !_wcsicmp(name, L"img") ||
           !_wcsicmp(name, L"audio") ||
           !_wcsicmp(name, L"fonts") ||
           !_wcsicmp(name, L"movies") ||
           !_wcsicmp(name, L"js") ||
           !_wcsicmp(name, L"save") ||
           !_wcsicmp(name, L"icon");
}

/* 递归扫描 www 下插件使用的外部 .txt 本地化资源。常见例子包括
   data/Recipes.txt、quest/Quests.txt；素材目录中的同名元数据会被跳过。 */
static void scan_rpgm_external_text_dir(const WCHAR *dir, TextList *prefetch, int depth) {
    if (depth > 6 || !is_dir(dir)) return;
    WCHAR pattern[MAX_PATH * 4];
    path_join(pattern, MAX_PATH * 4, dir, L"*");
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..")) continue;
        WCHAR path[MAX_PATH * 4];
        path_join(path, MAX_PATH * 4, dir, fd.cFileName);
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            if (!skip_rpgm_text_directory(fd.cFileName)) {
                scan_rpgm_external_text_dir(path, prefetch, depth + 1);
            }
        } else if (wide_ends_with_i(fd.cFileName, L".txt")) {
            scan_rpgm_text_file(path, prefetch);
        }
        if (prefetch->n >= textlist_limit(prefetch)) break;
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

static void scan_rpgm_external_texts(const WCHAR *dir, TextList *prefetch) {
    WCHAR www[MAX_PATH * 4];
    path_join(www, MAX_PATH * 4, dir, L"www");
    scan_rpgm_external_text_dir(www, prefetch, 0);
}

/* ======================== 各引擎预热入口 ======================== */

/* ----------------------------------------------------------------
 * warmup_xunity — Unity (Mono/IL2CPP) 预热
 *
 * 1. 扫描多个可能的翻译目录（BepInEx/Translation、Translation、
 *    AutoTranslator/Translation），解析已有翻译和待翻译文本
 * 2. 扫描 *_Data/ 下 assets 资源文件中的字符串
 * 3. 等待服务器就绪后，批量导入已有翻译 + 批量提交待翻译文本
 * ---------------------------------------------------------------- */
static void warmup_xunity(const WCHAR *dir) {
    PairList imports = {0};
    TextList prefetch = {0};
    prefetch.max_items = UNITY_WARMUP_MAX_ITEMS;
    const WCHAR *rels[] = {
        L"BepInEx\\Translation\\zh-CN\\Text",
        L"Translation\\zh-CN\\Text",
        L"AutoTranslator\\Translation\\zh-CN\\Text"
    };
    for (size_t i = 0; i < sizeof(rels) / sizeof(rels[0]); i++) {
        WCHAR p[MAX_PATH * 4];
        path_join(p, MAX_PATH * 4, dir, rels[i]);
        scan_text_dir(p, &imports, &prefetch);
    }

    scan_unity_assets(dir, &prefetch);

    size_t imported = 0;
    size_t queued = 0;
    LocalHttp http = {0};
    if ((imports.n || prefetch.n) && local_http_open(&http)) {
        if (local_http_wait_ready(&http, 8000)) {
            for (size_t i = 0; i < imports.n; i += WARMUP_BATCH_ITEMS) {
                size_t n = imports.n - i;
                if (n > WARMUP_BATCH_ITEMS) n = WARMUP_BATCH_ITEMS;
                if (post_import_batch(&http, &imports, i, n)) imported += n;
            }
            for (size_t i = 0; i < prefetch.n; i += WARMUP_BATCH_ITEMS) {
                size_t n = prefetch.n - i;
                if (n > WARMUP_BATCH_ITEMS) n = WARMUP_BATCH_ITEMS;
                if (post_prefetch_batch(&http, &prefetch, i, n)) queued += n;
            }
        } else {
            append_log(L"预热跳过：本地服务端未及时就绪。");
        }
        local_http_close(&http);
    }

    if (imported || queued) {
        append_log(L"预热翻译缓存：导入 %zu 条，后台排队 %zu 条。", imported, queued);
    }
    pairlist_free(&imports);
    textlist_free(&prefetch);
}

/* ----------------------------------------------------------------
 * post_prefetch_all — 将文本列表全部提交到服务器后台队列
 *
 * 等待服务器就绪，分批 POST 到 /prefetch 端点。
 * 返回成功排队的条数。
 * ---------------------------------------------------------------- */
static size_t post_prefetch_all(TextList *prefetch) {
    if (!prefetch->n) return 0;
    LocalHttp http = {0};
    if (!local_http_open(&http)) return 0;
    if (!local_http_wait_ready(&http, 8000)) {
        append_log(L"预热跳过：本地服务端未及时就绪。");
        local_http_close(&http);
        return 0;
    }
    size_t queued = 0;
    for (size_t i = 0; i < prefetch->n; i += WARMUP_BATCH_ITEMS) {
        size_t n = prefetch->n - i;
        if (n > WARMUP_BATCH_ITEMS) n = WARMUP_BATCH_ITEMS;
        if (post_prefetch_batch(&http, prefetch, i, n)) queued += n;
    }
    local_http_close(&http);
    return queued;
}

/* ----------------------------------------------------------------
 * scan_renpy_script_dir — 递归扫描目录中的 .rpy 脚本文件
 *
 * 逐行解析 Ren'Py 脚本，用 renpy_skip_statement 过滤非对话行，
 * 提取对话行中引号内的文本加入 prefetch 列表。
 * depth 控制递归深度（上限 RENPY_SCAN_MAX_DEPTH），
 * 超大的 .rpy 文件（> RENPY_SCRIPT_SCAN_MAX_BYTES）会被跳过。
 * ---------------------------------------------------------------- */
static void scan_renpy_script_dir(const WCHAR *dir, TextList *prefetch, int depth) {
    if (depth > RENPY_SCAN_MAX_DEPTH) return;
    if (!is_dir(dir)) return;
    WCHAR pat[MAX_PATH * 4];
    path_join(pat, MAX_PATH * 4, dir, L"*.rpy");
    WIN32_FIND_DATAW fd;
    HANDLE hf = FindFirstFileW(pat, &fd);
    if (hf == INVALID_HANDLE_VALUE) return;
    do {
        if (prefetch->n >= textlist_limit(prefetch)) { FindClose(hf); return; }
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        if (!wide_ends_with_i(fd.cFileName, L".rpy")) continue;
        /* 跳过超过大小上限的文件 */
        ULARGE_INTEGER fsz = {0};
        fsz.LowPart = fd.nFileSizeLow;
        fsz.HighPart = fd.nFileSizeHigh;
        if (fsz.QuadPart > (ULONGLONG)RENPY_SCRIPT_SCAN_MAX_BYTES) continue;
        WCHAR full[MAX_PATH * 4];
        path_join(full, MAX_PATH * 4, dir, fd.cFileName);
        char *buf = NULL; DWORD size = 0;
        if (!read_file_bytes(full, &buf, &size)) continue;
        char *line = buf;
        while (*line && prefetch->n < textlist_limit(prefetch)) {
            char *nl = strchr(line, '\n');
            if (nl) { *nl = 0; nl++; } else nl = line + strlen(line);
            const char *fq = renpy_first_quote(line);
            if (!renpy_skip_statement(line, fq) && fq) {
                const char *cursor = fq;
                char *text = renpy_string_at(&cursor);
                if (text) {
                    collect_renpy_string(text, prefetch);
                    free(text);
                }
            }
            line = nl;
        }
        free(buf);
    } while (FindNextFileW(hf, &fd));
    FindClose(hf);
    /* 递归扫描子目录 */
    path_join(pat, MAX_PATH * 4, dir, L"*");
    hf = FindFirstFileW(pat, &fd);
    if (hf == INVALID_HANDLE_VALUE) return;
    do {
        if (prefetch->n >= textlist_limit(prefetch)) { FindClose(hf); return; }
        if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) &&
            wcscmp(fd.cFileName, L".") != 0 && wcscmp(fd.cFileName, L"..") != 0) {
            WCHAR sub[MAX_PATH * 4];
            path_join(sub, MAX_PATH * 4, dir, fd.cFileName);
            scan_renpy_script_dir(sub, prefetch, depth + 1);
        }
    } while (FindNextFileW(hf, &fd));
    FindClose(hf);
}

/* RPG Maker MV/MZ 预热：扫描 www/data/ 的 JSON 并批量提交 */
static void warmup_rpgm(const WCHAR *dir) {
    TextList prefetch = {0};
    prefetch.max_items = RPGM_WARMUP_MAX_ITEMS;
    scan_rpgm_external_texts(dir, &prefetch);
    scan_rpgm_data_dir(dir, &prefetch);
    size_t queued = post_prefetch_all(&prefetch);
    if (queued) append_log(L"RPGM 预热翻译缓存：后台排队 %zu 条。", queued);
    textlist_free(&prefetch);
}

/* Ren'Py 预热：递归扫描 game/ 下的 .rpy 脚本并批量提交 */
static void warmup_renpy(const WCHAR *dir) {
    TextList prefetch = {0};
    prefetch.max_items = RENPY_WARMUP_MAX_ITEMS;
    WCHAR game[MAX_PATH * 4];
    path_join(game, MAX_PATH * 4, dir, L"game");
    scan_renpy_script_dir(game, &prefetch, 0);
    size_t queued = post_prefetch_all(&prefetch);
    if (queued) append_log(L"Ren'Py preheated translation cache: queued %zu texts.", queued);
    textlist_free(&prefetch);
}

/* ----------------------------------------------------------------
 * warmup_translations — 预热入口（由 launcher 主流程调用）
 *
 * 根据检测到的引擎类型，分派到对应的预热函数。
 * ---------------------------------------------------------------- */
void warmup_translations(const WCHAR *dir, Engine engine) {
    if (!dir || !dir[0]) return;
    if (engine == ENGINE_RENPY) warmup_renpy(dir);
    else if (engine == ENGINE_UNITY || engine == ENGINE_UNITY_IL2CPP) warmup_xunity(dir);
    else if (engine == ENGINE_RPGM_MV) warmup_rpgm(dir);
}
