/* Ren'Py 源码预热专用词法层。Godot 仍使用原有 renpy_string_at，
   不能把 Ren'Py 的空白折叠和双括号转义应用到其他引擎。 */
#pragma once

static void renpy_prevs_ensure(TextList *prefetch);

static int renpy_quote(char c) { return c == '\'' || c == '"' || c == '`'; }
static int renpy_ident(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
}

/* 只接受屏幕/输入的首个静态正文参数，不把 style/action 的字符串当正文。 */
static const char *renpy_expression_quote(const char *p) {
    while (*p == ' ' || *p == '\t') p++;
    if (*p == '_') {
        p++; while (*p == ' ' || *p == '\t') p++;
        if (*p != '(') return NULL;
        p++; while (*p == ' ' || *p == '\t') p++;
    }
    while (*p == 'r' || *p == 'R' || *p == 'u' || *p == 'U') p++;
    return renpy_quote(*p) ? p : NULL;
}

static char *renpy_unsupported_expression(ByteBuf *b) {
    free(b->data);
    append_log(L"RenPy 预热：Python 字面量包含不可提交的转义，交由运行时发现。");
    return NULL;
}

/* 找到完整字面量边界；反斜杠保护引号在 raw 字符串中同样有效。 */
static const char *renpy_literal_end(const char *p, int *triple) {
    char quote = *p;
    *triple = p[1] == quote && p[2] == quote;
    int width = *triple ? 3 : 1;
    p += width;
    while (*p) {
        if (*p == '\\' && p[1]) { p += 2; continue; }
        if (*p == quote && (!*triple || (p[1] == quote && p[2] == quote))) return p;
        p++;
    }
    return NULL;
}

static const char *renpy_input_argument(const char *line) {
    for (const char *p = line; *p; p++) {
        if (*p == '#') break;
        if (renpy_quote(*p)) {
            int triple = 0; const char *end = renpy_literal_end(p, &triple);
            if (!end) return NULL;
            p = end + (triple ? 3 : 1) - 1; continue;
        }
        if (!strncmp(p, "renpy.input", 11) && (p == line || (!renpy_ident(p[-1]) && p[-1] != '.'))) {
            const char *arg = p + 11;
            while (*arg == ' ' || *arg == '\t') arg++;
            if (*arg == '(') return arg + 1;
        }
    }
    return NULL;
}

/* 引擎先折叠源码中的普通空格/换行，再处理显式转义；\ 空格因此仍能保留。 */
static char *renpy_decode_value(const char *start, size_t len, int raw, int expression) {
    ByteBuf b = {0};
    if (!bb_init(&b, len * 2 + 1)) { append_log(L"RenPy 预热：字符串分配失败，保留运行时发现。"); return NULL; }
    /* Python 2 的 raw Unicode 与 Python 3 不同；未知运行时不能猜缓存键。 */
    if (raw && expression) for (size_t i = 0; i + 1 < len; i++)
        if (start[i] == '\\' && (start[i+1] == 'u' || start[i+1] == 'U' || start[i+1] == 'N')) return renpy_unsupported_expression(&b);
    for (size_t i = 0; i < len;) {
        unsigned char c = (unsigned char)start[i++];
        if (!raw && !expression && (c == ' ' || c == '\n')) {
            while (i < len && (start[i] == ' ' || start[i] == '\n')) i++;
            bb_ch(&b, ' '); continue;
        }
        if (!raw && c == '\\' && i < len) {
            char e = start[i++];
            if (expression) {
                /* 屏幕参数属于 Python，不使用对白的空白折叠与双括号规则。 */
                if (e == '\n') continue;
                if (e == 'x' || e == 'u' || e == 'U') {
                    unsigned cp = 0; int width = e == 'x' ? 2 : e == 'u' ? 4 : 8;
                    for (int k = 0; k < width; k++) {
                        if (i >= len || hex_value(start[i]) < 0) return renpy_unsupported_expression(&b);
                        cp = (cp << 4) | (unsigned)hex_value(start[i++]);
                    }
                    if (!cp || cp > 0x10ffff || (cp >= 0xd800 && cp <= 0xdfff)) return renpy_unsupported_expression(&b);
                    bb_utf8(&b, cp); continue;
                }
                if (e >= '0' && e <= '7') {
                    unsigned cp = (unsigned)(e - '0'); int count = 1;
                    while (i < len && count < 3 && start[i] >= '0' && start[i] <= '7') { cp = cp * 8 + (unsigned)(start[i++] - '0'); count++; }
                    if (!cp) return renpy_unsupported_expression(&b);
                    bb_utf8(&b, cp); continue;
                }
                if (e == 'N') return renpy_unsupported_expression(&b);
                if (e == 'a') bb_ch(&b, '\a');
                else if (e == 'b') bb_ch(&b, '\b');
                else if (e == 'f') bb_ch(&b, '\f');
                else if (e == 'n') bb_ch(&b, '\n');
                else if (e == 'r') bb_ch(&b, '\r');
                else if (e == 't') bb_ch(&b, '\t');
                else if (e == 'v') bb_ch(&b, '\v');
                else { if (e != '\\' && e != '\'' && e != '"') bb_ch(&b, '\\'); bb_ch(&b, e); }
                continue;
            }
            if (!expression && (e == ' ' || e == '\n')) {
                /* 空白折叠先于转义：反斜杠后的整个源码空白段也是一个空格。 */
                while (i < len && (start[i] == ' ' || start[i] == '\n')) i++;
                bb_ch(&b, ' '); continue;
            }
            if (e == 'u') {
                unsigned cp = 0; int count = 0;
                while (i < len && count < 4 && hex_value(start[i]) >= 0) { cp = cp * 16 + (unsigned)hex_value(start[i++]); count++; }
                if (count) {
                    /* 保留既有代理对兼容；非法孤立代理仍不进入 UTF-8 缓存键。 */
                    unsigned lo = 0;
                    if (cp >= 0xd800 && cp <= 0xdbff && i + 6 <= len && start[i] == '\\' && start[i+1] == 'u' &&
                        json_u4(start + i + 2, &lo) && lo >= 0xdc00 && lo <= 0xdfff) {
                        cp = 0x10000 + ((cp - 0xd800) << 10) + lo - 0xdc00; i += 6;
                    } else if (cp >= 0xd800 && cp <= 0xdfff) cp = 0xfffd;
                    bb_utf8(&b, cp); continue;
                }
            }
            if (!expression && (e == '[' || e == '{' || e == '%')) bb_ch(&b, e);
            if (e == 'n') bb_ch(&b, '\n');
            else if (expression && e == 'r') bb_ch(&b, '\r');
            else if (expression && e == 't') bb_ch(&b, '\t');
            else bb_ch(&b, e);
            continue;
        }
        bb_ch(&b, (char)c);
    }
    return b.data;
}

static void renpy_add_source_value(TextList *prefetch, char *text, char **previous) {
    if (!text) return;
    size_t before = prefetch->n;
    collect_renpy_string(text, prefetch);
    if (prefetch->n > before) {
        renpy_prevs_ensure(prefetch);
        if (prefetch->prev_items) prefetch->prev_items[before] = *previous;
        *previous = prefetch->items[before];
    }
    free(text);
}

static void renpy_add_source_literal(TextList *prefetch, const char *p, const char *end,
                                     int triple, int raw, int expression, char **previous) {
    int width = triple ? 3 : 1;
    const char *body = p + width;
    size_t len = (size_t)(end - body);
    if (!triple || raw || expression) {
        renpy_add_source_value(prefetch, renpy_decode_value(body, len, raw, expression), previous); return;
    }
    /* 默认独白分隔符为空行；{clear} 由引擎生成 nvl clear，不能提交翻译。 */
    ByteBuf normalized = {0};
    if (!bb_init(&normalized, len + 1)) { append_log(L"RenPy 预热：独白分配失败，保留运行时发现。"); return; }
    for (size_t i = 0; i < len;) {
        if (body[i] == ' ') {
            size_t j = i; while (j < len && body[j] == ' ') j++;
            if (j < len && body[j] == '\n') { i = j; continue; }
        }
        char ch = body[i++]; bb_ch(&normalized, ch);
        if (ch == '\n') while (i < len && body[i] == ' ') i++;
    }
    char *part = normalized.data;
    while (*part) {
        char *delimiter = strstr(part, "\n\n");
        if (delimiter) *delimiter = 0;
        char *trimmed = trim_ascii(part);
        if (*trimmed && strcmp(trimmed, "{clear}"))
            renpy_add_source_value(prefetch, renpy_decode_value(trimmed, strlen(trimmed), 0, 0), previous);
        if (!delimiter) break;
        part = delimiter + 2;
    }
    free(normalized.data);
}

/* 逐个逻辑语句处理，跨行字符串/括号与注释先定边界，再判断是否为可见正文。 */
static void scan_renpy_normalized_source(const char *source, TextList *prefetch) {
    const char *p = source;
    if (!strncmp(p, "\xef\xbb\xbf", 3)) p += 3;
    int excluded_indent = -1, screen_indent = -1;
    char *previous = NULL;
    while (*p && prefetch->n < textlist_limit(prefetch)) {
        int indent = 0;
        while (*p == ' ' || *p == '\t') { indent++; p++; }
        const char *content = p;
        int depth = 0;
        while (*p && (*p != '\n' || depth > 0)) {
            if (renpy_quote(*p)) {
                int triple = 0;
                const char *end = renpy_literal_end(p, &triple);
                if (!end) { append_log(L"RenPy 预热：未闭合字符串，余文交由运行时发现。"); return; }
                p = end + (triple ? 3 : 1); continue;
            }
            if (*p == '#') { while (*p && *p != '\n') p++; if (!depth) break; continue; }
            if (*p == '(' || *p == '[' || *p == '{') depth++;
            if ((*p == ')' || *p == ']' || *p == '}') && depth > 0) depth--;
            p++;
        }
        char *line = dup_range(content, (size_t)(p - content));
        if (*p == '\n') p++;
        if (!line) { append_log(L"RenPy 预热：语句分配失败，余文交由运行时发现。"); return; }
        if (!*line || *line == '#' || *line == '\r') { free(line); continue; }
        if (indent <= excluded_indent) excluded_indent = -1;
        if (indent <= screen_indent) screen_indent = -1;
        if (excluded_indent >= 0) { free(line); continue; }
        int in_screen = screen_indent >= 0;
        if (starts_with_word_i(line, "screen")) { screen_indent = indent; free(line); continue; }
        if (starts_with_word_i(line, "python") || (starts_with_word_i(line, "init") && strstr(line, "python")) ||
            starts_with_word_i(line, "image") || starts_with_word_i(line, "transform") || starts_with_word_i(line, "style")) {
            excluded_indent = indent; free(line); continue;
        }
        int ui_text = in_screen && (starts_with_word_i(line, "text") || starts_with_word_i(line, "textbutton") || starts_with_word_i(line, "label"));
        const char *cursor = line;
        int expression = ui_text;
        if (*line == '$') {
            const char *input = renpy_input_argument(line);
            if (!input) { free(line); continue; }
            cursor = input; expression = 1;
        } else if (ui_text) {
            while (*cursor && *cursor != ' ' && *cursor != '\t') cursor++;
        } else if (in_screen) { free(line); continue; }
        else if (!ui_text) {
            const char *skip[] = { "scene", "show", "hide", "play", "queue", "stop", "with", "jump", "call", "return", "label", "define", "default", "init", "if", "elif", "else", "for", "while", "menu", "translate" };
            int ignored = 0;
            for (size_t k = 0; k < sizeof skip / sizeof skip[0]; k++) if (starts_with_word_i(line, skip[k])) { ignored = 1; break; }
            if (ignored) { free(line); continue; }
        }
        size_t seen = 0;
        if (expression) {
            cursor = renpy_expression_quote(cursor);
            if (!cursor) { free(line); continue; }
        } else while (*cursor && !renpy_quote(*cursor) && *cursor != '#') cursor++;
        while (renpy_quote(*cursor) && seen++ < 16u) {
            int triple = 0;
            const char *end = renpy_literal_end(cursor, &triple);
            if (!end) break;
            if (expression) {
                const char *tail = end + (triple ? 3 : 1);
                while (*tail == ' ' || *tail == '\t' || *tail == ')') tail++;
                /* 拼接、格式化和条件表达式交给引擎求值，不能把一段字面量当最终键。 */
                if (renpy_quote(*tail) || (*tail && strchr("+-%*/.[(", *tail)) ||
                    starts_with_word_i(tail, "if") || starts_with_word_i(tail, "and") || starts_with_word_i(tail, "or") ||
                    ((*tail == 'r' || *tail == 'R' || *tail == 'u' || *tail == 'U') && renpy_quote(tail[1]))) break;
            }
            const char *prefix = cursor;
            int raw = 0;
            while (prefix > line && (prefix[-1] == 'r' || prefix[-1] == 'R' || prefix[-1] == 'u' || prefix[-1] == 'U')) { prefix--; if (*prefix == 'r' || *prefix == 'R') raw = 1; }
            raw = raw && (prefix == line || !renpy_ident(prefix[-1]));
            renpy_add_source_literal(prefetch, cursor, end, triple, raw, expression, &previous);
            cursor = end + (triple ? 3 : 1);
            while (*cursor == ' ' || *cursor == '\t') cursor++;
            if (*cursor == 'r' && renpy_quote(cursor[1])) cursor++;
        }
        free(line);
    }
}

static void scan_renpy_source(const char *source, TextList *prefetch) {
    char *normalized = dup_range(source, strlen(source));
    if (!normalized) { append_log(L"RenPy 预热：文件分配失败，保留运行时发现。"); return; }
    char *write = normalized;
    for (const char *read = source; *read; read++) {
        if (*read == '\r') { *write++ = '\n'; if (read[1] == '\n') read++; }
        else *write++ = *read;
    }
    *write = 0;
    scan_renpy_normalized_source(normalized, prefetch);
    free(normalized);
}
