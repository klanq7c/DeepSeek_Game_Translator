/* ================================================================
 * godot_warmup.c — Godot resource cache warmup scanner
 * ----------------------------------------------------------------
 * This module owns Godot-specific text discovery. The interface is
 * intentionally small: warmup.c passes a game directory and TextList,
 * then this module scans likely Godot resources and adds candidates.
 *
 * Current Godot support is resource/cache-warmup only:
 *   - no generic runtime hook is injected
 *   - .pck/.translation files and embedded-PCK exe sections are read-only scan inputs
 *   - misses are only queued through /prefetch by warmup.c later
 * ================================================================ */

#include "warmup_internal.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#define GODOT_RESOURCE_SCAN_MAX_BYTES (64u * 1024u * 1024u)
#define GODOT_SCAN_MAX_DEPTH 10
#define GODOT_PCK_MAGIC 0x43504447u
#define GODOT_PCK_V1_HEADER_SIZE 88u
#define GODOT_PCK_V2_HEADER_SIZE 100u
#define GODOT_PCK_MAX_FILES 200000u
#define GODOT_PCK_MAX_PATH_BYTES 4096u

typedef struct {
    uint32_t format;
    uint32_t header_size;
    uint32_t entry_meta_size;
    uint64_t file_base;
    uint32_t file_count;
    int offsets_are_absolute;
} GodotPckInfo;

/* Godot resources mix player text with paths, node metadata, resource ids, and
   editor/project settings. This filter keeps the common game-facing text while
   rejecting strings that would only pollute the shared translation cache. */
static int should_warm_godot_text(const char *s) {
    if (!should_warm_text(s)) return 0;
    size_t len = strlen(s);
    if (!contains_any(s, " \t.?!,:;'-\"")) {
        if (len < 3 || len > 32) return 0;
        for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
            if (*p < 0x80 && !((*p >= 'A' && *p <= 'Z') ||
                               (*p >= 'a' && *p <= 'z') ||
                               (*p >= '0' && *p <= '9'))) {
                return 0;
            }
        }
    }
    if (strstr(s, "res://") || strstr(s, "user://") || strstr(s, "uid://")) return 0;
    if (strstr(s, ".tscn") || strstr(s, ".tres") || strstr(s, ".gd") ||
        strstr(s, ".import") || strstr(s, ".shader") || strstr(s, ".material") ||
        strstr(s, ".png") || strstr(s, ".jpg") || strstr(s, ".webp") ||
        strstr(s, ".svg") || strstr(s, ".ogg") || strstr(s, ".wav") ||
        strstr(s, ".mp3") || strstr(s, ".pck")) {
        return 0;
    }
    if (strstr(s, "ExtResource") || strstr(s, "SubResource") ||
        strstr(s, "NodePath") || strstr(s, "PackedScene") ||
        strstr(s, "ResourceUID") || strstr(s, "ProjectSettings")) {
        return 0;
    }
    if (!strcmp(s, "RSRC") || !strcmp(s, "OptimizedTranslation") ||
        !strcmp(s, "messages") || !strcmp(s, "locale") ||
        !strcmp(s, "strings") || !strcmp(s, "resource_name")) {
        return 0;
    }
    if (!strncmp(s, "_", 1) || !strncmp(s, "@", 1) || !strncmp(s, "{", 1) ||
        starts_with_word_i(s, "script") || starts_with_word_i(s, "resource") ||
        starts_with_word_i(s, "node") || starts_with_word_i(s, "signal")) {
        return 0;
    }
    return 1;
}

/* Targeted strings come from explicit translation-bearing properties or calls.
   They may be short labels like "Start", so they are allowed through the base
   Godot filter without the stricter loose-text punctuation requirement. */
static void collect_godot_string(char *s, TextList *prefetch) {
    char *t = trim_ascii(s);
    if (should_warm_godot_text(t)) textlist_add(prefetch, t);
}

/* Loose strings are unlabelled lines or generic quoted literals. They need a
   stronger signal so node names, constants, and short ids do not flood warmup. */
static void collect_godot_free_string(char *s, TextList *prefetch) {
    char *t = trim_ascii(s);
    if (!should_warm_godot_text(t)) return;
    if (contains_any(t, " \t.?!,:;'-\"") || strlen(t) >= 18) textlist_add(prefetch, t);
}

static int alpha_word_len_at_least(const char *s, size_t min_len) {
    size_t n = 0;
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        if (!((*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z'))) return 0;
        n++;
    }
    return n >= min_len;
}

/* Binary exports often contain NUL-separated UTF-8 payloads. Allow compact menu
   words such as "Start", while still rejecting tiny binary noise like "bin". */
static void collect_godot_binary_string(char *s, TextList *prefetch) {
    char *t = trim_ascii(s);
    if (!should_warm_godot_text(t)) return;
    if (strchr(t, '\r') || strchr(t, '\n') || strchr(t, '\t')) return;
    size_t len = strlen(t);
    if (len >= 5 || alpha_word_len_at_least(t, 4)) {
        textlist_add(prefetch, t);
    }
}

static int godot_key_equals(const char *start, const char *end, const char *name) {
    while (start < end && (*start == ' ' || *start == '\t')) start++;
    while (end > start && (end[-1] == ' ' || end[-1] == '\t')) end--;
    size_t len = (size_t)(end - start);
    return strlen(name) == len && !_strnicmp(start, name, len);
}

/* Scene/resource headers carry useful engine metadata but not player-facing
   prose. Skipping these quotes is what keeps [node name="..."] from becoming
   translation work. */
static int godot_metadata_quote(const char *buf_start, const char *quote) {
    const char *line = quote;
    while (line > buf_start && line[-1] != '\n' && line[-1] != '\r') line--;
    while (*line == ' ' || *line == '\t') line++;
    if (*line == '[') return 1;

    const char *eq = quote;
    while (eq > line && eq[-1] != '\n' && eq[-1] != '\r' && eq[-1] != '=') eq--;
    if (eq <= line || eq[-1] != '=') return 0;

    const char *key_end = eq - 1;
    while (key_end > line && (key_end[-1] == ' ' || key_end[-1] == '\t')) key_end--;
    const char *key_start = key_end;
    while (key_start > line) {
        char c = key_start[-1];
        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
              (c >= '0' && c <= '9') || c == '_')) {
            break;
        }
        key_start--;
    }

    return godot_key_equals(key_start, key_end, "name") ||
           godot_key_equals(key_start, key_end, "type") ||
           godot_key_equals(key_start, key_end, "parent") ||
           godot_key_equals(key_start, key_end, "instance") ||
           godot_key_equals(key_start, key_end, "script") ||
           godot_key_equals(key_start, key_end, "path") ||
           godot_key_equals(key_start, key_end, "resource_path") ||
           godot_key_equals(key_start, key_end, "uid") ||
           godot_key_equals(key_start, key_end, "id");
}

static int godot_translatable_property_key(const char *start, const char *end) {
    return godot_key_equals(start, end, "text") ||
           godot_key_equals(start, end, "bbcode_text") ||
           godot_key_equals(start, end, "placeholder_text") ||
           godot_key_equals(start, end, "tooltip_text") ||
           godot_key_equals(start, end, "hint_tooltip") ||
           godot_key_equals(start, end, "dialog_text") ||
           godot_key_equals(start, end, "title") ||
           godot_key_equals(start, end, "window_title") ||
           godot_key_equals(start, end, "message") ||
           godot_key_equals(start, end, "caption") ||
           godot_key_equals(start, end, "description") ||
           godot_key_equals(start, end, "display_text");
}

static int godot_call_name_before_quote(const char *line, const char *quote, const char *name) {
    const char *p = quote;
    while (p > line && (p[-1] == ' ' || p[-1] == '\t')) p--;
    if (p <= line || p[-1] != '(') return 0;
    p--;
    while (p > line && (p[-1] == ' ' || p[-1] == '\t')) p--;
    const char *end = p;
    while (p > line) {
        char c = p[-1];
        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
              (c >= '0' && c <= '9') || c == '_' || c == '.')) {
            break;
        }
        p--;
    }
    size_t len = (size_t)(end - p);
    size_t want = strlen(name);
    return len >= want && !_strnicmp(end - want, name, want) &&
           (len == want || *(end - want - 1) == '.');
}

/* Classify the quote before parsing it. Targeted contexts get permissive label
   handling; metadata contexts are skipped; everything else goes through the
   loose-string collector. */
static int godot_translatable_quote(const char *buf_start, const char *quote) {
    const char *line = quote;
    while (line > buf_start && line[-1] != '\n' && line[-1] != '\r') line--;
    while (*line == ' ' || *line == '\t') line++;
    if (*line == '[') return 0;

    const char *eq = quote;
    while (eq > line && eq[-1] != '\n' && eq[-1] != '\r' && eq[-1] != '=') eq--;
    if (eq > line && eq[-1] == '=') {
        const char *key_end = eq - 1;
        while (key_end > line && (key_end[-1] == ' ' || key_end[-1] == '\t')) key_end--;
        const char *key_start = key_end;
        while (key_start > line) {
            char c = key_start[-1];
            if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                  (c >= '0' && c <= '9') || c == '_')) {
                break;
            }
            key_start--;
        }
        if (godot_translatable_property_key(key_start, key_end)) return 1;
    }

    return godot_call_name_before_quote(line, quote, "tr") ||
           godot_call_name_before_quote(line, quote, "translate") ||
           godot_call_name_before_quote(line, quote, "N_") ||
           godot_call_name_before_quote(line, quote, "RTR");
}

/* Validate a NUL-delimited binary slice before treating it as UTF-8 text. This
   keeps arbitrary packed bytes out of the cache while allowing non-ASCII menu
   text from exported .pck/.translation files. */
static int valid_utf8_text_payload(const unsigned char *p, size_t n) {
    int signal = 0;
    for (size_t i = 0; i < n;) {
        unsigned char c = p[i];
        if (c == 9 || c == 10 || c == 13) {
            i++;
            continue;
        }
        if (c >= 32 && c <= 126) {
            if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')) signal = 1;
            i++;
            continue;
        }
        unsigned cp = 0;
        size_t need = 0;
        if ((c & 0xe0) == 0xc0) {
            cp = c & 0x1f;
            need = 2;
        } else if ((c & 0xf0) == 0xe0) {
            cp = c & 0x0f;
            need = 3;
        } else if ((c & 0xf8) == 0xf0) {
            cp = c & 0x07;
            need = 4;
        } else {
            return 0;
        }
        if (i + need > n) return 0;
        for (size_t j = 1; j < need; j++) {
            if ((p[i + j] & 0xc0) != 0x80) return 0;
            cp = (cp << 6) | (p[i + j] & 0x3f);
        }
        if ((need == 2 && cp < 0x80) ||
            (need == 3 && cp < 0x800) ||
            (need == 4 && (cp < 0x10000 || cp > 0x10ffff)) ||
            (cp >= 0xd800 && cp <= 0xdfff)) {
            return 0;
        }
        signal = 1;
        i += need;
    }
    return signal;
}

static int godot_text_file_name(const WCHAR *name) {
    return wide_ends_with_i(name, L".tscn") ||
           wide_ends_with_i(name, L".tres") ||
           wide_ends_with_i(name, L".gd") ||
           wide_ends_with_i(name, L".json") ||
           wide_ends_with_i(name, L".csv") ||
           wide_ends_with_i(name, L".po") ||
           wide_ends_with_i(name, L".txt") ||
           !_wcsicmp(name, L"project.godot");
}

static int godot_binary_file_name(const WCHAR *name) {
    return wide_ends_with_i(name, L".pck") ||
           wide_ends_with_i(name, L".translation") ||
           !_wcsicmp(name, L"godot_project.binary");
}

static int ascii_ends_with_i(const char *s, const char *suffix) {
    if (!s || !suffix) return 0;
    size_t sl = strlen(s), tl = strlen(suffix);
    return sl >= tl && !_stricmp(s + sl - tl, suffix);
}

static int godot_pack_text_path(const char *path) {
    return ascii_ends_with_i(path, ".tscn") ||
           ascii_ends_with_i(path, ".tres") ||
           ascii_ends_with_i(path, ".gd") ||
           ascii_ends_with_i(path, ".json") ||
           ascii_ends_with_i(path, ".csv") ||
           ascii_ends_with_i(path, ".po") ||
           ascii_ends_with_i(path, ".txt") ||
           !_stricmp(path, "project.godot");
}

static int ascii_locale_char(char c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
           (c >= '0' && c <= '9') || c == '_' || c == '-';
}

static int godot_locale_token(const char *s, size_t n) {
    if (n < 2 || n > 8) return 0;
    if (!((s[0] >= 'A' && s[0] <= 'Z') || (s[0] >= 'a' && s[0] <= 'z')) ||
        !((s[1] >= 'A' && s[1] <= 'Z') || (s[1] >= 'a' && s[1] <= 'z'))) {
        return 0;
    }
    for (size_t i = 2; i < n; i++) {
        if (!ascii_locale_char(s[i])) return 0;
    }
    return 1;
}

static int godot_pack_translation_path(const char *path) {
    static const char suffix[] = ".translation";
    size_t len = strlen(path);
    size_t suffix_len = sizeof(suffix) - 1;
    if (len < suffix_len || _stricmp(path + len - suffix_len, suffix)) return 0;

    const char *end = path + len - suffix_len;
    const char *token = end;
    while (token > path && token[-1] != '.' && token[-1] != '/' && token[-1] != '\\') token--;
    if (token > path && token[-1] == '.') {
        size_t token_len = (size_t)(end - token);
        if (godot_locale_token(token, token_len)) {
            return !_strnicmp(token, "en", 2) &&
                   (token_len == 2 || token[2] == '_' || token[2] == '-');
        }
    }
    return 1;
}

static int godot_pack_binary_path(const char *path) {
    return godot_pack_translation_path(path) ||
           !_stricmp(path, "godot_project.binary");
}

/* Skip generated/cache-heavy folders. Godot addons also contain editor/plugin
   strings that are poor warmup candidates for a translated game session. */
static int skip_godot_scan_directory(const WCHAR *name) {
    return !_wcsicmp(name, L".godot") ||
           !_wcsicmp(name, L".import") ||
           !_wcsicmp(name, L"addons") ||
           !_wcsicmp(name, L"export_presets") ||
           !_wcsicmp(name, L"saves") ||
           !_wcsicmp(name, L"save");
}

static int skip_godot_generated_file(const WCHAR *name) {
    return !_wcsicmp(name, L"dst_godot_runtime.gd") ||
           !_wcsicmp(name, L"dst_godot_patch.pck") ||
           !_wcsicmp(name, L"dst_godot_patch.next.pck") ||
           !_wcsicmp(name, L"dst_godot_patch.building");
}

static void scan_godot_quoted_strings(char *buf, TextList *prefetch) {
    for (const char *p = buf; *p && prefetch->n < textlist_limit(prefetch);) {
        if (*p != '"' && *p != '\'') {
            p++;
            continue;
        }
        int targeted = godot_translatable_quote(buf, p);
        if (!targeted && godot_metadata_quote(buf, p)) {
            p++;
            continue;
        }
        const char *cursor = p;
        char *text = renpy_string_at(&cursor);
        if (text) {
            if (targeted) collect_godot_string(text, prefetch);
            else collect_godot_free_string(text, prefetch);
            free(text);
            p = cursor;
        } else {
            p++;
        }
    }
}

/* Plain-line fallback for hand-authored text resources. It ignores assignments,
   comments, PO directives, and quoted/code-looking lines already handled above. */
static void scan_godot_lines(char *buf, TextList *prefetch) {
    char *line = buf;
    if (line[0] && line[1] && line[2] &&
        (unsigned char)line[0] == 0xef &&
        (unsigned char)line[1] == 0xbb &&
        (unsigned char)line[2] == 0xbf) {
        line += 3;
    }
    for (char *p = line;; p++) {
        if (*p != '\r' && *p != '\n' && *p != 0) continue;
        char separator = *p;
        *p = 0;
        char *text = trim_ascii(line);
        if (*text && *text != '#' && *text != ';' && *text != '[' &&
            !strchr(text, '=') && !strchr(text, '"') && !strchr(text, '\'') &&
            strncmp(text, "msgid", 5) && strncmp(text, "msgstr", 6) && strncmp(text, "msgctxt", 6)) {
            collect_godot_free_string(text, prefetch);
        }
        if (!separator || prefetch->n >= textlist_limit(prefetch)) break;
        if (separator == '\r' && p[1] == '\n') p++;
        line = p + 1;
    }
}

static void godot_po_flush(ByteBuf *current, int *active, TextList *prefetch) {
    if (*active && current->data) collect_godot_string(current->data, prefetch);
    free(current->data);
    current->data = NULL;
    current->len = 0;
    current->cap = 0;
    *active = 0;
}

static void godot_po_append_quoted(const char *line, ByteBuf *current) {
    const char *q = strchr(line, '"');
    if (!q) return;
    char *s = renpy_string_at(&q);
    if (!s) return;
    if (!current->data && !bb_init(current, strlen(s) + 1)) {
        free(s);
        return;
    }
    bb_add(current, s, strlen(s));
    free(s);
}

/* Godot PO files contain source strings in msgid/msgid_plural. msgstr is an
   existing target translation and must not be re-queued as an original. */
static void scan_godot_po_strings(char *buf, TextList *prefetch) {
    ByteBuf current = {0};
    int in_msgid = 0;
    char *line = buf;
    if (line[0] && line[1] && line[2] &&
        (unsigned char)line[0] == 0xef &&
        (unsigned char)line[1] == 0xbb &&
        (unsigned char)line[2] == 0xbf) {
        line += 3;
    }
    for (char *p = line;; p++) {
        if (*p != '\r' && *p != '\n' && *p != 0) continue;
        char separator = *p;
        *p = 0;
        char *text = trim_ascii(line);
        if (!strncmp(text, "msgid", 5) &&
            (text[5] == ' ' || text[5] == '\t' || text[5] == '_' || text[5] == 0)) {
            godot_po_flush(&current, &in_msgid, prefetch);
            in_msgid = 1;
            godot_po_append_quoted(text, &current);
        } else if (!strncmp(text, "msgstr", 6) || !strncmp(text, "msgctxt", 6)) {
            godot_po_flush(&current, &in_msgid, prefetch);
        } else if (in_msgid && *text == '"') {
            godot_po_append_quoted(text, &current);
        } else if (*text && *text != '#') {
            godot_po_flush(&current, &in_msgid, prefetch);
        }
        if (!separator || prefetch->n >= textlist_limit(prefetch)) break;
        if (separator == '\r' && p[1] == '\n') p++;
        line = p + 1;
    }
    godot_po_flush(&current, &in_msgid, prefetch);
}

static char *godot_csv_next_cell(const char **pp) {
    const char *p = *pp;
    ByteBuf b = {0};
    if (!bb_init(&b, 64)) return NULL;
    if (*p == '"') {
        p++;
        while (*p) {
            if (*p == '"' && p[1] == '"') {
                bb_ch(&b, '"');
                p += 2;
                continue;
            }
            if (*p == '"') {
                p++;
                break;
            }
            bb_ch(&b, *p++);
        }
        while (*p == ' ' || *p == '\t') p++;
        if (*p == ',') p++;
    } else {
        while (*p && *p != ',' && *p != '\r' && *p != '\n') bb_ch(&b, *p++);
        if (*p == ',') p++;
    }
    *pp = p;
    return b.data;
}

static int godot_csv_source_column_score(char *cell) {
    char *t = trim_ascii(cell);
    if (!*t) return 0;
    if (!_stricmp(t, "en") ||
        !_stricmp(t, "en_us") ||
        !_stricmp(t, "en-us") ||
        (!_strnicmp(t, "en_", 3) && godot_locale_token(t, strlen(t))) ||
        (!_strnicmp(t, "en-", 3) && godot_locale_token(t, strlen(t)))) {
        return 100;
    }
    if (!_stricmp(t, "source") ||
        !_stricmp(t, "source_text") ||
        !_stricmp(t, "original") ||
        !_stricmp(t, "english") ||
        !_stricmp(t, "msgid")) {
        return 90;
    }
    if (!_stricmp(t, "text") ||
        !_stricmp(t, "display_text") ||
        !_stricmp(t, "body")) {
        return 80;
    }
    return 0;
}

static int godot_csv_source_column(const char *header) {
    const char *cellp = header;
    int best_col = -1;
    int best_score = 0;
    for (int col = 0; *cellp; col++) {
        char *cell = godot_csv_next_cell(&cellp);
        if (!cell) break;
        int score = godot_csv_source_column_score(cell);
        free(cell);
        if (score > best_score) {
            best_score = score;
            best_col = col;
        }
        while (*cellp == ' ' || *cellp == '\t') cellp++;
    }
    return best_col;
}

static int collect_godot_csv_source_cell(const char *line, int source_col, TextList *prefetch) {
    if (source_col < 0) return 0;
    const char *cellp = line;
    for (int col = 0; *cellp; col++) {
        char *cell = godot_csv_next_cell(&cellp);
        if (!cell) break;
        int wanted = col == source_col;
        if (wanted) collect_godot_string(cell, prefetch);
        free(cell);
        if (wanted) return 1;
        while (*cellp == ' ' || *cellp == '\t') cellp++;
    }
    return 1;
}

/* Godot CSV translations commonly use key,locale columns. After the header,
   prefer the source/English column when it is labelled. Unknown layouts keep
   the old first-natural-text fallback so unusual hand-authored CSV still warms. */
static void scan_godot_csv_strings(char *buf, TextList *prefetch) {
    char *line = buf;
    int row = 0;
    int source_col = -1;
    if (line[0] && line[1] && line[2] &&
        (unsigned char)line[0] == 0xef &&
        (unsigned char)line[1] == 0xbb &&
        (unsigned char)line[2] == 0xbf) {
        line += 3;
    }
    for (char *p = line;; p++) {
        if (*p != '\r' && *p != '\n' && *p != 0) continue;
        char separator = *p;
        *p = 0;
        char *text = trim_ascii(line);
        if (row == 0) {
            source_col = godot_csv_source_column(text);
        } else if (*text && *text != '#') {
            const char *cellp = text;
            size_t before = prefetch->n;
            if (!collect_godot_csv_source_cell(text, source_col, prefetch)) {
                while (*cellp && prefetch->n == before) {
                    char *cell = godot_csv_next_cell(&cellp);
                    if (!cell) break;
                    collect_godot_string(cell, prefetch);
                    free(cell);
                    while (*cellp == ' ' || *cellp == '\t') cellp++;
                }
            }
        }
        row++;
        if (!separator || prefetch->n >= textlist_limit(prefetch)) break;
        if (separator == '\r' && p[1] == '\n') p++;
        line = p + 1;
    }
}

static void scan_godot_binary_buffer(const unsigned char *bytes, DWORD size, TextList *prefetch) {
    DWORD start = 0;
    for (DWORD i = 0; i <= size && prefetch->n < textlist_limit(prefetch); i++) {
        int textual = 0;
        if (i < size) {
            unsigned char c = bytes[i];
            textual = (c >= 32 && c <= 126) || c == 9 || c == 10 || c == 13 || c >= 0x80;
        }
        if (textual) continue;
        DWORD n = i - start;
        if (n >= 3 && n <= WARMUP_MAX_TEXT_BYTES && valid_utf8_text_payload(bytes + start, n)) {
            char *s = dup_range((const char *)bytes + start, n);
            if (s) {
                collect_godot_binary_string(s, prefetch);
                free(s);
            }
        }
        start = i + 1;
    }
}

static uint32_t read_u32le(const unsigned char *p) {
    return ((uint32_t)p[0]) |
           ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static uint64_t read_u64le(const unsigned char *p) {
    uint64_t lo = read_u32le(p);
    uint64_t hi = read_u32le(p + 4);
    return lo | (hi << 32);
}

static int read_at_exact(HANDLE h, LONGLONG offset, void *buf, DWORD size);

static int read_godot_pck_info(HANDLE h, LONGLONG pck_base, uint64_t pck_size, GodotPckInfo *info) {
    unsigned char header[GODOT_PCK_V2_HEADER_SIZE];
    memset(info, 0, sizeof *info);
    if (pck_size < GODOT_PCK_V1_HEADER_SIZE) return 0;
    DWORD want = pck_size >= GODOT_PCK_V2_HEADER_SIZE ? GODOT_PCK_V2_HEADER_SIZE : GODOT_PCK_V1_HEADER_SIZE;
    if (!read_at_exact(h, pck_base, header, want)) return 0;
    if (read_u32le(header) != GODOT_PCK_MAGIC) return 0;

    uint32_t format = read_u32le(header + 4);
    if (format == 1) {
        info->format = format;
        info->header_size = GODOT_PCK_V1_HEADER_SIZE;
        info->entry_meta_size = 8u + 8u + 16u;
        info->file_base = 0;
        info->file_count = read_u32le(header + 84);
        info->offsets_are_absolute = 1;
    } else if (format == 2) {
        if (pck_size < GODOT_PCK_V2_HEADER_SIZE || want < GODOT_PCK_V2_HEADER_SIZE) return 0;
        info->format = format;
        info->header_size = GODOT_PCK_V2_HEADER_SIZE;
        info->entry_meta_size = 8u + 8u + 16u + 4u;
        info->file_base = read_u64le(header + 24);
        info->file_count = read_u32le(header + 96);
        info->offsets_are_absolute = 0;
    } else {
        return 0;
    }
    if (info->file_count > GODOT_PCK_MAX_FILES || info->file_base > pck_size) return 0;
    return 1;
}

static int read_at_exact(HANDLE h, LONGLONG offset, void *buf, DWORD size) {
    LARGE_INTEGER pos;
    pos.QuadPart = offset;
    if (!SetFilePointerEx(h, pos, NULL, FILE_BEGIN)) return 0;
    DWORD done = 0;
    while (done < size) {
        DWORD got = 0;
        if (!ReadFile(h, (char *)buf + done, size - done, &got, NULL) || got == 0) return 0;
        done += got;
    }
    return 1;
}

static void trim_pack_path(char *path, size_t n) {
    if (!path || n == 0) return;
    path[n - 1] = 0;
    for (size_t i = 0; i < n; i++) {
        if (path[i] == '\0') {
            path[i] = 0;
            return;
        }
    }
}

static void scan_godot_pack_entry(HANDLE h, LONGLONG data_abs, uint64_t size, const char *path, TextList *prefetch) {
    if (!path || !path[0] || size > GODOT_RESOURCE_SCAN_MAX_BYTES || size > UINT32_MAX) return;
    if (!godot_pack_text_path(path) && !godot_pack_binary_path(path)) return;

    char *buf = (char *)malloc((size_t)size + 1);
    if (!buf) return;
    if (!read_at_exact(h, data_abs, buf, (DWORD)size)) {
        free(buf);
        return;
    }
    buf[size] = 0;

    if (ascii_ends_with_i(path, ".po")) {
        scan_godot_po_strings(buf, prefetch);
    } else if (ascii_ends_with_i(path, ".csv")) {
        scan_godot_csv_strings(buf, prefetch);
    } else if (godot_pack_text_path(path)) {
        scan_godot_quoted_strings(buf, prefetch);
        scan_godot_lines(buf, prefetch);
    } else {
        scan_godot_binary_buffer((const unsigned char *)buf, (DWORD)size, prefetch);
    }
    free(buf);
}

static int scan_godot_pck_at(HANDLE h, LONGLONG pck_base, uint64_t pck_size, TextList *prefetch) {
    if (prefetch->n >= textlist_limit(prefetch)) return 1;

    GodotPckInfo info;
    if (!read_godot_pck_info(h, pck_base, pck_size, &info)) return 0;

    LONGLONG pos = pck_base + (LONGLONG)info.header_size;
    LONGLONG pck_end = pck_base + (LONGLONG)pck_size;
    for (uint32_t i = 0; i < info.file_count && prefetch->n < textlist_limit(prefetch); i++) {
        unsigned char lenbuf[4];
        if (pos > pck_end - 4 || !read_at_exact(h, pos, lenbuf, sizeof(lenbuf))) return 0;
        pos += 4;

        uint32_t path_len = read_u32le(lenbuf);
        if (path_len == 0 || path_len > GODOT_PCK_MAX_PATH_BYTES || pos > pck_end - path_len) return 0;
        char *path = (char *)malloc((size_t)path_len + 1);
        if (!path) return 0;
        if (!read_at_exact(h, pos, path, path_len)) {
            free(path);
            return 0;
        }
        path[path_len] = 0;
        trim_pack_path(path, (size_t)path_len + 1);
        pos += path_len;

        unsigned char entry[8 + 8 + 16 + 4];
        if (pos > pck_end - (LONGLONG)info.entry_meta_size ||
            !read_at_exact(h, pos, entry, info.entry_meta_size)) {
            free(path);
            return 0;
        }
        pos += info.entry_meta_size;

        uint64_t rel = read_u64le(entry);
        uint64_t size = read_u64le(entry + 8);
        uint64_t data_in_pack = 0;
        LONGLONG data_abs = 0;
        if (info.offsets_are_absolute) {
            if (rel < (uint64_t)pck_base || rel - (uint64_t)pck_base > pck_size) {
                free(path);
                continue;
            }
            data_in_pack = rel - (uint64_t)pck_base;
            data_abs = (LONGLONG)rel;
        } else if (rel <= UINT64_MAX - info.file_base) {
            data_in_pack = info.file_base + rel;
            data_abs = pck_base + (LONGLONG)data_in_pack;
        } else {
            free(path);
            continue;
        }
        if (data_in_pack <= pck_size && size <= pck_size - data_in_pack) {
            scan_godot_pack_entry(h, data_abs, size, path, prefetch);
        }
        free(path);
    }
    return 1;
}

static int find_pe_pck_section(const WCHAR *path, LONGLONG *pck_base, uint64_t *pck_size) {
    HANDLE h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return 0;

    int found = 0;
    LARGE_INTEGER file_size;
    IMAGE_DOS_HEADER dos;
    if (GetFileSizeEx(h, &file_size) &&
        file_size.QuadPart >= (LONGLONG)sizeof(dos) &&
        read_at_exact(h, 0, &dos, sizeof(dos)) &&
        dos.e_magic == IMAGE_DOS_SIGNATURE && dos.e_lfanew > 0) {
        LONGLONG pe = (LONGLONG)dos.e_lfanew;
        DWORD sig = 0;
        IMAGE_FILE_HEADER fh;
        if (pe <= file_size.QuadPart - (LONGLONG)(sizeof(sig) + sizeof(fh)) &&
            read_at_exact(h, pe, &sig, sizeof(sig)) &&
            sig == IMAGE_NT_SIGNATURE &&
            read_at_exact(h, pe + sizeof(sig), &fh, sizeof(fh)) &&
            fh.NumberOfSections > 0 && fh.NumberOfSections <= 128) {
            LONGLONG sections = pe + sizeof(sig) + sizeof(fh) + fh.SizeOfOptionalHeader;
            for (WORD i = 0; i < fh.NumberOfSections; i++) {
                IMAGE_SECTION_HEADER sh;
                LONGLONG off = sections + (LONGLONG)i * sizeof(sh);
                if (off > file_size.QuadPart - (LONGLONG)sizeof(sh) ||
                    !read_at_exact(h, off, &sh, sizeof(sh))) break;
                if (sh.Name[0] == 'p' && sh.Name[1] == 'c' && sh.Name[2] == 'k' && sh.Name[3] == 0 &&
                    sh.PointerToRawData > 0 && sh.SizeOfRawData >= GODOT_PCK_V1_HEADER_SIZE &&
                    (LONGLONG)sh.PointerToRawData <= file_size.QuadPart - (LONGLONG)sh.SizeOfRawData) {
                    unsigned char magic[4];
                    if (read_at_exact(h, sh.PointerToRawData, magic, sizeof(magic)) &&
                        read_u32le(magic) == GODOT_PCK_MAGIC) {
                        *pck_base = sh.PointerToRawData;
                        *pck_size = sh.SizeOfRawData;
                        found = 1;
                        break;
                    }
                }
            }
        }
    }
    CloseHandle(h);
    return found;
}

static int scan_godot_pck_file(const WCHAR *path, TextList *prefetch) {
    HANDLE h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return 0;
    int parsed = 0;
    LARGE_INTEGER size;
    if (GetFileSizeEx(h, &size) && size.QuadPart > 0) {
        parsed = scan_godot_pck_at(h, 0, (uint64_t)size.QuadPart, prefetch);
    }
    CloseHandle(h);
    return parsed;
}

static void scan_godot_embedded_pck_exe(const WCHAR *path, TextList *prefetch) {
    LONGLONG pck_base = 0;
    uint64_t pck_size = 0;
    if (!find_pe_pck_section(path, &pck_base, &pck_size)) return;

    HANDLE h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return;
    scan_godot_pck_at(h, pck_base, pck_size, prefetch);
    CloseHandle(h);
}

static void scan_godot_text_file(const WCHAR *path, TextList *prefetch) {
    if (!file_size_at_most(path, GODOT_RESOURCE_SCAN_MAX_BYTES)) return;
    char *buf = NULL;
    DWORD size = 0;
    if (!read_file_bytes(path, &buf, &size)) return;
    if (wide_ends_with_i(path, L".po")) {
        scan_godot_po_strings(buf, prefetch);
    } else if (wide_ends_with_i(path, L".csv")) {
        scan_godot_csv_strings(buf, prefetch);
    } else {
        scan_godot_quoted_strings(buf, prefetch);
        scan_godot_lines(buf, prefetch);
    }
    free(buf);
}

static void scan_godot_binary_file(const WCHAR *path, TextList *prefetch) {
    if (wide_ends_with_i(path, L".pck")) {
        if (scan_godot_pck_file(path, prefetch) ||
            prefetch->n >= textlist_limit(prefetch)) {
            return;
        }
    }
    if (!file_size_at_most(path, GODOT_RESOURCE_SCAN_MAX_BYTES)) return;
    char *buf = NULL;
    DWORD size = 0;
    if (!read_file_bytes(path, &buf, &size)) return;
    scan_godot_binary_buffer((const unsigned char *)buf, size, prefetch);
    free(buf);
}

static void scan_godot_resource_dir(const WCHAR *dir, TextList *prefetch, int depth) {
    if (depth > GODOT_SCAN_MAX_DEPTH || !is_dir(dir)) return;
    WCHAR pattern[MAX_PATH * 4];
    path_join(pattern, MAX_PATH * 4, dir, L"*");
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..")) continue;
        if (skip_godot_generated_file(fd.cFileName)) continue;
        WCHAR path[MAX_PATH * 4];
        path_join(path, MAX_PATH * 4, dir, fd.cFileName);
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            if (!skip_godot_scan_directory(fd.cFileName)) {
                scan_godot_resource_dir(path, prefetch, depth + 1);
            }
        } else if (godot_text_file_name(fd.cFileName)) {
            scan_godot_text_file(path, prefetch);
        } else if (godot_binary_file_name(fd.cFileName)) {
            scan_godot_binary_file(path, prefetch);
        } else if (wide_ends_with_i(fd.cFileName, L".exe")) {
            scan_godot_embedded_pck_exe(path, prefetch);
        }
        if (prefetch->n >= textlist_limit(prefetch)) break;
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

void warmup_scan_godot_resources(const WCHAR *dir, TextList *prefetch) {
    scan_godot_resource_dir(dir, prefetch, 0);
}
