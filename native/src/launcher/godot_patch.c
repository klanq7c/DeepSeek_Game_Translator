#include "godot_patch.h"

#include "engine.h"
#include "fsutil.h"
#include "ui.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <winhttp.h>

#define GODOT_PCK_MAGIC 0x43504447u
#define GODOT_PCK_V1_HEADER_SIZE 88u
#define GODOT_PCK_V2_HEADER_SIZE 100u
#define GODOT_PATCH_MAX_FILES 200000u
#define GODOT_PATCH_MAX_PATH 4096u
#define GODOT_PATCH_MAX_ENTRY_BYTES (2u * 1024u * 1024u)
#define GODOT_PATCH_MAX_FONT_BYTES (32u * 1024u * 1024u)
#define GODOT_PATCH_MAX_FONT_REPLACEMENTS 16u
#define GODOT_PATCH_LOOSE_FONT_MIN_PRIORITY 20
#define GODOT_PATCH_MAX_STRINGS_PER_RESOURCE 1800u
#define GODOT_PATCH_MAX_SOURCE_TEXT_BYTES 2400u
#define GODOT_PATCH_MAX_TRANSLATION_TEXT_BYTES 4096u
/* Keep first-run patch refresh bounded; warmup queues the rest for later cache-only rebuilds. */
#define GODOT_PATCH_LIVE_LIMIT 1024u
#define GODOT_PATCH_BATCH 128u
#define GODOT_PATCH_SMALL_DIALOGIC_BYTES (16u * 1024u)
#define GODOT_PATCH_PACK_NAME L"dst_godot_patch.pck"
#define GODOT_PATCH_NEXT_NAME L"dst_godot_patch.next.pck"
#define GODOT_PATCH_BUILDING_NAME L"dst_godot_patch.building"
#define GODOT_RUNTIME_SCRIPT_NAME L"dst_godot_runtime.gd"

typedef struct {
    char **v;
    size_t n;
    size_t cap;
} StrList;

typedef struct {
    WCHAR path[MAX_PATH * 4];
    uint64_t offset;
    uint64_t size;
} PckSource;

typedef struct {
    uint32_t format;
    uint32_t header_size;
    uint32_t entry_meta_size;
    uint64_t file_base;
    uint32_t file_count;
    int offsets_are_absolute;
} PckInfo;

typedef struct {
    char *path;
    uint64_t rel;
    uint64_t size;
    uint64_t meta_pos;
    int kind;
    int font_priority;
} PckEntry;

enum {
    GODOT_PATCH_ENTRY_TRANSLATION = 1,
    GODOT_PATCH_ENTRY_TEXT_RESOURCE = 2,
    GODOT_PATCH_ENTRY_FONT = 3,
    GODOT_PATCH_ENTRY_GDSCRIPT_BYTECODE = 4
};

typedef struct {
    uint32_t start;
    uint32_t end;
    int source_index;
} TextPatch;

typedef struct {
    TextPatch *v;
    size_t n;
    size_t cap;
} TextPatchList;

typedef struct {
    HINTERNET ses;
    HINTERNET con;
} PatchHttp;

typedef struct {
    char *path;
    char *data;
    DWORD size;
} LoosePatchItem;

typedef struct {
    LoosePatchItem *v;
    size_t n;
    size_t cap;
} LoosePatchList;

typedef struct {
    uint32_t rec_off;
    uint32_t old_offset;
    uint32_t comp_size;
    uint32_t uncomp_size;
    int source_index;
} OptItem;

static uint32_t read_u32le(const unsigned char *p) {
    return ((uint32_t)p[0]) |
           ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static void write_u32le(unsigned char *p, uint32_t v) {
    p[0] = (unsigned char)(v & 0xff);
    p[1] = (unsigned char)((v >> 8) & 0xff);
    p[2] = (unsigned char)((v >> 16) & 0xff);
    p[3] = (unsigned char)((v >> 24) & 0xff);
}

static void write_u64le(unsigned char *p, uint64_t v) {
    write_u32le(p, (uint32_t)(v & 0xffffffffu));
    write_u32le(p + 4, (uint32_t)(v >> 32));
}

static uint32_t godot_variant_pad4(uint32_t n) {
    uint32_t extra = 4u - (n % 4u);
    return extra < 4u ? extra : 0u;
}

static uint64_t read_u64le(const unsigned char *p) {
    uint64_t lo = read_u32le(p);
    uint64_t hi = read_u32le(p + 4);
    return lo | (hi << 32);
}

static int parse_pck_info(const unsigned char *header, DWORD header_bytes, uint64_t pck_size, PckInfo *info) {
    memset(info, 0, sizeof *info);
    if (header_bytes < GODOT_PCK_V1_HEADER_SIZE || pck_size < GODOT_PCK_V1_HEADER_SIZE) return 0;
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
        if (header_bytes < GODOT_PCK_V2_HEADER_SIZE || pck_size < GODOT_PCK_V2_HEADER_SIZE) return 0;
        info->format = format;
        info->header_size = GODOT_PCK_V2_HEADER_SIZE;
        info->entry_meta_size = 8u + 8u + 16u + 4u;
        info->file_base = read_u64le(header + 24);
        info->file_count = read_u32le(header + 96);
        info->offsets_are_absolute = 0;
    } else {
        return 0;
    }
    if (info->file_count == 0 || info->file_count > GODOT_PATCH_MAX_FILES || info->file_base > pck_size) return 0;
    return 1;
}

/*
 * Smaz decompression table used by Godot OptimizedTranslation.
 * Copyright (c) 2006-2009, Salvatore Sanfilippo. BSD-3-Clause.
 * Only decompression is needed here: patched translations are written back as
 * uncompressed UTF-8 strings, while untouched compressed entries are copied as-is.
 */
static const char *GODOT_SMAZ_RCB[254] = {
    " ", "the", "e", "t", "a", "of", "o", "and", "i", "n", "s", "e ", "r", " th",
    " t", "in", "he", "th", "h", "he ", "to", "\r\n", "l", "s ", "d", " a", "an",
    "er", "c", " o", "d ", "on", " of", "re", "of ", "t ", ", ", "is", "u", "at",
    "   ", "n ", "or", "which", "f", "m", "as", "it", "that", "\n", "was", "en",
    "  ", " w", "es", " an", " i", "\r", "f ", "g", "p", "nd", " s", "nd ", "ed ",
    "w", "ed", "http://", "for", "te", "ing", "y ", "The", " c", "ti", "r ", "his",
    "st", " in", "ar", "nt", ",", " to", "y", "ng", " h", "with", "le", "al", "to ",
    "b", "ou", "be", "were", " b", "se", "o ", "ent", "ha", "ng ", "their", "\"",
    "hi", "from", " f", "in ", "de", "ion", "me", "v", ".", "ve", "all", "re ",
    "ri", "ro", "is ", "co", "f t", "are", "ea", ". ", "her", " m", "er ", " p",
    "es ", "by", "they", "di", "ra", "ic", "not", "s, ", "d t", "at ", "ce", "la",
    "h ", "ne", "as ", "tio", "on ", "n t", "io", "we", " a ", "om", ", a", "s o",
    "ur", "li", "ll", "ch", "had", "this", "e t", "g ", "e\r\n", " wh", "ere",
    " co", "e o", "a ", "us", " d", "ss", "\n\r\n", "\r\n\r", "=\"", " be", " e",
    "s a", "ma", "one", "t t", "or ", "but", "el", "so", "l ", "e s", "s,", "no",
    "ter", " wa", "iv", "ho", "e a", " r", "hat", "s t", "ns", "ch ", "wh", "tr",
    "ut", "/", "have", "ly ", "ta", " ha", " on", "tha", "-", " l", "ati", "en ",
    "pe", " re", "there", "ass", "si", " fo", "wa", "ec", "our", "who", "its", "z",
    "fo", "rs", ">", "ot", "un", "<", "im", "th ", "nc", "ate", "><", "ver", "ad",
    " we", "ly", "ee", " n", "id", " cl", "ac", "il", "</", "rt", " wi", "div",
    "e, ", " it", "whi", " ma", "ge", "x", "e c", "men", ".com"
};

static int godot_smaz_decompress(const char *in, int inlen, char *out, int outlen) {
    const unsigned char *c = (const unsigned char *)in;
    char *start = out;
    int original_outlen = outlen;
    while (inlen > 0) {
        if (*c == 254) {
            if (inlen < 2 || outlen < 1) return original_outlen + 1;
            *out++ = (char)c[1];
            outlen--;
            c += 2;
            inlen -= 2;
        } else if (*c == 255) {
            if (inlen < 2) return original_outlen + 1;
            int len = ((int)c[1]) + 1;
            if (inlen < 2 + len || outlen < len) return original_outlen + 1;
            memcpy(out, c + 2, len);
            out += len;
            outlen -= len;
            c += 2 + len;
            inlen -= 2 + len;
        } else {
            const char *s = GODOT_SMAZ_RCB[*c];
            int len = (int)strlen(s);
            if (outlen < len) return original_outlen + 1;
            memcpy(out, s, len);
            out += len;
            outlen -= len;
            c++;
            inlen--;
        }
    }
    return (int)(out - start);
}

static int write_at_exact(HANDLE h, uint64_t offset, const void *buf, DWORD size) {
    LARGE_INTEGER pos;
    pos.QuadPart = (LONGLONG)offset;
    if (!SetFilePointerEx(h, pos, NULL, FILE_BEGIN)) return 0;
    DWORD done = 0;
    while (done < size) {
        DWORD wrote = 0;
        if (!WriteFile(h, (const char *)buf + done, size - done, &wrote, NULL) || wrote == 0) return 0;
        done += wrote;
    }
    return 1;
}

static int read_at_exact(HANDLE h, uint64_t offset, void *buf, DWORD size) {
    LARGE_INTEGER pos;
    pos.QuadPart = (LONGLONG)offset;
    if (!SetFilePointerEx(h, pos, NULL, FILE_BEGIN)) return 0;
    DWORD done = 0;
    while (done < size) {
        DWORD got = 0;
        if (!ReadFile(h, (char *)buf + done, size - done, &got, NULL) || got == 0) return 0;
        done += got;
    }
    return 1;
}

static int append_file_bytes(HANDLE h, const char *buf, DWORD size, uint64_t *offset) {
    LARGE_INTEGER zero, end;
    zero.QuadPart = 0;
    if (!SetFilePointerEx(h, zero, &end, FILE_END)) return 0;
    if (offset) *offset = (uint64_t)end.QuadPart;
    DWORD done = 0;
    while (done < size) {
        DWORD wrote = 0;
        if (!WriteFile(h, buf + done, size - done, &wrote, NULL) || wrote == 0) return 0;
        done += wrote;
    }
    return 1;
}

static int append_file_from_path(HANDLE dst, const WCHAR *src, uint64_t *offset, uint64_t *size) {
    HANDLE in = CreateFileW(src, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                            NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (in == INVALID_HANDLE_VALUE) return 0;
    int ok = 0;
    LARGE_INTEGER src_size;
    if (!GetFileSizeEx(in, &src_size) || src_size.QuadPart <= 0 ||
        src_size.QuadPart > GODOT_PATCH_MAX_FONT_BYTES) {
        goto done;
    }

    LARGE_INTEGER zero, end;
    zero.QuadPart = 0;
    if (!SetFilePointerEx(dst, zero, &end, FILE_END)) goto done;
    if (offset) *offset = (uint64_t)end.QuadPart;
    if (size) *size = (uint64_t)src_size.QuadPart;

    char buf[64 * 1024];
    uint64_t copied = 0;
    while (copied < (uint64_t)src_size.QuadPart) {
        DWORD want = (DWORD)sizeof buf;
        uint64_t remain = (uint64_t)src_size.QuadPart - copied;
        if (remain < want) want = (DWORD)remain;
        DWORD got = 0;
        if (!ReadFile(in, buf, want, &got, NULL) || got == 0) goto done;
        DWORD wrote_total = 0;
        while (wrote_total < got) {
            DWORD wrote = 0;
            if (!WriteFile(dst, buf + wrote_total, got - wrote_total, &wrote, NULL) || wrote == 0) goto done;
            wrote_total += wrote;
        }
        copied += got;
    }
    ok = 1;

done:
    CloseHandle(in);
    return ok;
}

static int ascii_ends_with_i(const char *s, const char *suffix) {
    size_t n = strlen(s), m = strlen(suffix);
    if (n < m) return 0;
    s += n - m;
    for (size_t i = 0; i < m; i++) {
        char a = s[i], b = suffix[i];
        if (a >= 'A' && a <= 'Z') a = (char)(a - 'A' + 'a');
        if (b >= 'A' && b <= 'Z') b = (char)(b - 'A' + 'a');
        if (a != b) return 0;
    }
    return 1;
}

static int godot_english_translation_path(const char *path) {
    return ascii_ends_with_i(path, ".en.translation") ||
           ascii_ends_with_i(path, ".en_us.translation") ||
           ascii_ends_with_i(path, ".en-us.translation");
}

static int ascii_contains_i(const char *s, const char *needle);

static int godot_patch_text_resource_path(const char *path) {
    if (!path) return 0;
    if (ascii_contains_i(path, "/.import/") || ascii_contains_i(path, "res://.import/")) return 0;
    return ascii_ends_with_i(path, ".tscn") ||
           ascii_ends_with_i(path, ".tres") ||
           ascii_ends_with_i(path, ".json");
}

static int godot_patch_gdscript_bytecode_path(const char *path) {
    if (!path || !ascii_ends_with_i(path, ".gdc")) return 0;
    if (ascii_contains_i(path, "/.import/") || ascii_contains_i(path, "res://.import/")) return 0;
    if (ascii_contains_i(path, "/scripts/menus/") ||
        ascii_contains_i(path, "/scripts/scenario/")) {
        return 1;
    }
    return ascii_ends_with_i(path, "/in_game_editor/scripts/resource/scenario_action_data.gdc") ||
           ascii_ends_with_i(path, "/in_game_editor/scripts/static/target_type.gdc") ||
           ascii_ends_with_i(path, "/in_game_editor/scripts/static/unit_stat.gdc");
}

static const char *godot_patch_basename(const char *path) {
    const char *base = path;
    if (!path) return "";
    for (const char *p = path; *p; p++) {
        if (*p == '/' || *p == '\\') base = p + 1;
    }
    return base;
}

static int godot_patch_font_resource_priority(const char *path) {
    if (!path) return 0;
    if (ascii_contains_i(path, "/.import/") || ascii_contains_i(path, "res://.import/")) return 0;
    if (!ascii_ends_with_i(path, ".ttf") && !ascii_ends_with_i(path, ".otf")) return 0;

    const char *name = godot_patch_basename(path);
    const char *skip[] = {
        "display", "decor", "title", "logo", "icon", "symbol", "emoji",
        "material", "awesome", "rocker", "script", "dingbat", "glyph",
        "cursor"
    };
    for (size_t i = 0; i < sizeof(skip) / sizeof(skip[0]); i++) {
        if (ascii_contains_i(name, skip[i])) return 0;
    }

    int score = 10;
    if (ascii_contains_i(path, "font")) score += 5;
    if (ascii_contains_i(name, "regular")) score += 80;
    if (ascii_contains_i(name, "normal") || ascii_contains_i(name, "book")) score += 70;
    if (ascii_contains_i(name, "default") || ascii_contains_i(name, "text")) score += 40;
    if (ascii_contains_i(name, "medium") ||
        ascii_contains_i(name, "bold") || ascii_contains_i(name, "italic") ||
        ascii_contains_i(name, "light") || ascii_contains_i(name, "semi") ||
        ascii_contains_i(name, "extra")) {
        score += 25;
    }
    if (ascii_contains_i(name, "sans") || ascii_contains_i(name, "arial") ||
        ascii_contains_i(name, "roboto") || ascii_contains_i(name, "noto")) {
        score += 20;
    }
    return score;
}

static int find_system_cjk_font(WCHAR *out, size_t cap) {
    WCHAR windir[MAX_PATH];
    if (!out || !cap || !GetWindowsDirectoryW(windir, MAX_PATH)) return 0;
    const WCHAR *names[] = { L"simhei.ttf", L"msyh.ttf", L"msyh.ttc", L"simsun.ttc" };
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        _snwprintf(out, cap, L"%s\\Fonts\\%s", windir, names[i]);
        out[cap - 1] = 0;
        if (exists_path(out)) return 1;
    }
    out[0] = 0;
    return 0;
}

static int ascii_contains_i(const char *s, const char *needle) {
    size_t n = strlen(needle);
    if (!n) return 1;
    for (; *s; s++) {
        size_t i = 0;
        while (i < n && s[i]) {
            char a = s[i], b = needle[i];
            if (a >= 'A' && a <= 'Z') a = (char)(a - 'A' + 'a');
            if (b >= 'A' && b <= 'Z') b = (char)(b - 'A' + 'a');
            if (a != b) break;
            i++;
        }
        if (i == n) return 1;
    }
    return 0;
}

static int godot_patch_story_text_path(const char *path) {
    return ascii_contains_i(path, "dialog") ||
           ascii_contains_i(path, "cutscene") ||
           ascii_contains_i(path, "scenario") ||
           ascii_contains_i(path, "story") ||
           ascii_contains_i(path, "quest") ||
           ascii_contains_i(path, "mission") ||
           ascii_contains_i(path, "tutorial") ||
           ascii_contains_i(path, "conversation") ||
           ascii_contains_i(path, "event");
}

static int godot_patch_startup_text_path(const char *path) {
    return ascii_contains_i(path, "/prologue/") ||
           ascii_contains_i(path, "/intro/") ||
           ascii_contains_i(path, "tutorial");
}

static int godot_patch_low_value_tooling_path(const char *path) {
    return ascii_contains_i(path, "/editor/") ||
           ascii_contains_i(path, "/editors/") ||
           ascii_contains_i(path, "/debug/") ||
           ascii_contains_i(path, "/test/") ||
           ascii_contains_i(path, "/tests/");
}

static int godot_patch_ui_data_path(const char *path) {
    if (!path || !ascii_contains_i(path, "/data/")) return 0;
    return ascii_ends_with_i(path, "/skills.json") ||
           ascii_ends_with_i(path, "/heroes.json") ||
           ascii_ends_with_i(path, "/weapons.json") ||
           ascii_ends_with_i(path, "/items.json") ||
           ascii_ends_with_i(path, "/status_effects.json") ||
           ascii_ends_with_i(path, "/date_traits.json") ||
           ascii_ends_with_i(path, "/date_actions.json") ||
           ascii_ends_with_i(path, "/conquest.json") ||
           ascii_ends_with_i(path, "/general.json");
}

static int godot_patch_entry_priority(const char *path, uint64_t size) {
    int dialogic = ascii_contains_i(path, "localization_dialogic/");
    int small_dialogic = dialogic && size <= GODOT_PATCH_SMALL_DIALOGIC_BYTES;
    int json = ascii_ends_with_i(path, ".json");
    int scene_text = ascii_ends_with_i(path, ".tscn") || ascii_ends_with_i(path, ".tres");
    int gdscript_bytecode = ascii_ends_with_i(path, ".gdc");
    int p = 50;
    if (gdscript_bytecode && ascii_contains_i(path, "/scripts/menus/")) p = 4;
    else if (gdscript_bytecode && ascii_contains_i(path, "/scripts/scenario/")) p = 4;
    else if (gdscript_bytecode && ascii_contains_i(path, "scenario_action_data.gdc")) p = 4;
    else if (gdscript_bytecode && ascii_contains_i(path, "target_type.gdc")) p = 4;
    else if (gdscript_bytecode && ascii_contains_i(path, "unit_stat.gdc")) p = 4;
    else if (json && godot_patch_ui_data_path(path)) p = 5;
    else if ((json || scene_text) && godot_patch_startup_text_path(path)) p = json ? 6 : 16;
    else if (dialogic) p = small_dialogic ? 8 : 10;
    else if (ascii_contains_i(path, "localization/")) p = 20;
    else if (json) p = godot_patch_story_text_path(path) ? 24 : 34;
    else if (scene_text) p = godot_patch_story_text_path(path) ? 38 : 62;

    /* Keep small Dialogic scene/combat timelines ahead of large repeatable packs,
       even when names contain bond/negotation markers. */
    if (!small_dialogic) {
        if (ascii_contains_i(path, "bond")) p += 80;
        if (ascii_contains_i(path, "negotation") || ascii_contains_i(path, "negotiation")) p += 70;
        if (ascii_contains_i(path, "waizatsuhi")) p += 70;
    }
    if ((json || scene_text) && ascii_contains_i(path, "/data/")) p -= 4;
    if ((json || scene_text) && godot_patch_low_value_tooling_path(path)) p += 40;
    return p;
}

static int compare_pck_entries_for_patch(const void *a, const void *b) {
    const PckEntry *ea = (const PckEntry *)a;
    const PckEntry *eb = (const PckEntry *)b;
    int pa = godot_patch_entry_priority(ea->path, ea->size);
    int pb = godot_patch_entry_priority(eb->path, eb->size);
    if (pa != pb) return pa - pb;
    if (ea->size < eb->size) return -1;
    if (ea->size > eb->size) return 1;
    return strcmp(ea->path, eb->path);
}

static void strlist_free(StrList *l) {
    if (!l) return;
    for (size_t i = 0; i < l->n; i++) free(l->v[i]);
    free(l->v);
    memset(l, 0, sizeof *l);
}

static int strlist_contains(StrList *l, const char *s) {
    for (size_t i = 0; i < l->n; i++) {
        if (!strcmp(l->v[i], s)) return 1;
    }
    return 0;
}

static int strlist_push_unique(StrList *l, const char *s) {
    if (!s || !*s || strlist_contains(l, s)) return 1;
    if (l->n >= GODOT_PATCH_MAX_STRINGS_PER_RESOURCE) return 1;
    if (l->n == l->cap) {
        size_t nc = l->cap ? l->cap * 2 : 64;
        char **nv = (char **)realloc(l->v, nc * sizeof *nv);
        if (!nv) return 0;
        l->v = nv;
        l->cap = nc;
    }
    l->v[l->n] = _strdup(s);
    if (!l->v[l->n]) return 0;
    l->n++;
    return 1;
}

static int strlist_push_unique_index(StrList *l, const char *s) {
    if (!s || !*s) return -1;
    for (size_t i = 0; i < l->n; i++) {
        if (!strcmp(l->v[i], s)) return (int)i;
    }
    if (!strlist_push_unique(l, s)) return -1;
    return (int)(l->n - 1);
}

static int valid_utf8_payload(const char *s, size_t n) {
    int need = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s, (int)n, NULL, 0);
    return need > 0;
}

static int has_cjk_utf8(const char *s) {
    int need = MultiByteToWideChar(CP_UTF8, 0, s, -1, NULL, 0);
    if (need <= 0) return 0;
    WCHAR *w = (WCHAR *)malloc((size_t)need * sizeof *w);
    if (!w) return 0;
    int ok = MultiByteToWideChar(CP_UTF8, 0, s, -1, w, need);
    int found = 0;
    if (ok > 0) {
        for (int i = 0; w[i]; i++) {
            if (w[i] >= 0x4e00 && w[i] <= 0x9fff) {
                found = 1;
                break;
            }
        }
    }
    free(w);
    return found;
}

/* Godot Translation replacement is renderer-local. Do not insert synthetic hard
   line breaks here: many Godot UI controls already know their text box width,
   and fixed-column wrapping makes dialogue stack in the wrong part of the box. */
static char *godot_wrap_cjk_translation(const char *s) {
    (void)s;
    return NULL;
}

static int godot_source_looks_like_path_or_url(const char *s) {
    if (!s) return 0;
    if (!strncmp(s, "res://", 6) || strstr(s, "://")) return 1;
    if (s[0] == '/' || s[0] == '\\') return 1;
    if (!strchr(s, '/') && !strchr(s, '\\')) return 0;
    static const char *file_suffixes[] = {
        ".png", ".jpg", ".jpeg", ".webp", ".bmp", ".gif", ".svg", ".ogg", ".wav",
        ".mp3", ".webm", ".mp4", ".tscn", ".tres", ".res", ".stex", ".json",
        ".gd", ".gdc", ".pck", ".ttf", ".otf", ".fnt", ".import"
    };
    for (size_t i = 0; i < sizeof file_suffixes / sizeof file_suffixes[0]; i++) {
        if (ascii_ends_with_i(s, file_suffixes[i])) return 1;
    }
    return 0;
}

static int wanted_source_text(const char *s) {
    if (!s) return 0;
    size_t n = strlen(s);
    if (n < 2 || n > GODOT_PATCH_MAX_SOURCE_TEXT_BYTES) return 0;
    if (has_cjk_utf8(s)) return 0;
    if (!strcmp(s, "RSRC") || !strcmp(s, "OptimizedTranslation") ||
        !strcmp(s, "Translation") || !strcmp(s, "messages") ||
        !strcmp(s, "locale") || !strcmp(s, "strings") ||
        !strcmp(s, "resource_name")) return 0;
    if (!strncmp(s, "res://", 6) || strstr(s, ".translation") || strstr(s, ".import")) return 0;
    if (godot_source_looks_like_path_or_url(s)) return 0;
    static const char *file_suffixes[] = {
        ".png", ".jpg", ".jpeg", ".webp", ".bmp", ".gif", ".svg", ".ogg", ".wav",
        ".mp3", ".webm", ".mp4", ".tscn", ".tres", ".res", ".stex", ".json",
        ".gd", ".gdc", ".pck", ".ttf", ".otf", ".fnt", ".import"
    };
    for (size_t i = 0; i < sizeof file_suffixes / sizeof file_suffixes[0]; i++) {
        if (ascii_ends_with_i(s, file_suffixes[i])) return 0;
    }
    int alpha = 0;
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        if ((*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z')) alpha++;
        if (*p < 9 || (*p > 13 && *p < 32)) return 0;
    }
    return alpha >= 2;
}

static int wanted_translation_text(const char *s) {
    if (!s) return 0;
    size_t n = strlen(s);
    if (n < 1 || n > GODOT_PATCH_MAX_TRANSLATION_TEXT_BYTES) return 0;
    if (!strcmp(s, "translation_unavailable") || !strcmp(s, "missing_text")) return 0;
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        if (*p < 9 || (*p > 13 && *p < 32)) return 0;
    }
    return 1;
}

static void bb_json_string(ByteBuf *b, const char *s) {
    bb_add(b, "\"", 1);
    for (const unsigned char *p = (const unsigned char *)s; p && *p; p++) {
        if (*p == '\\' || *p == '"') {
            char esc[2] = {'\\', (char)*p};
            bb_add(b, esc, 2);
        } else if (*p == '\n') {
            bb_add(b, "\\n", 2);
        } else if (*p == '\r') {
            bb_add(b, "\\r", 2);
        } else if (*p == '\t') {
            bb_add(b, "\\t", 2);
        } else if (*p < 32) {
            char tmp[7];
            _snprintf(tmp, sizeof tmp, "\\u%04x", (unsigned)*p);
            bb_add(b, tmp, strlen(tmp));
        } else {
            bb_add(b, (const char *)p, 1);
        }
    }
    bb_add(b, "\"", 1);
}

static int hex_val(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static void bb_utf8_codepoint(ByteBuf *b, unsigned cp) {
    char out[4];
    if (cp < 0x80) {
        out[0] = (char)cp;
        bb_add(b, out, 1);
    } else if (cp < 0x800) {
        out[0] = (char)(0xc0 | (cp >> 6));
        out[1] = (char)(0x80 | (cp & 0x3f));
        bb_add(b, out, 2);
    } else {
        out[0] = (char)(0xe0 | (cp >> 12));
        out[1] = (char)(0x80 | ((cp >> 6) & 0x3f));
        out[2] = (char)(0x80 | (cp & 0x3f));
        bb_add(b, out, 3);
    }
}

static char *json_parse_string(const char **pp) {
    const char *p = *pp;
    if (*p != '"') return NULL;
    p++;
    ByteBuf b = {0};
    while (*p && *p != '"') {
        if (*p == '\\') {
            p++;
            if (*p == 'n') { bb_add(&b, "\n", 1); p++; }
            else if (*p == 'r') { bb_add(&b, "\r", 1); p++; }
            else if (*p == 't') { bb_add(&b, "\t", 1); p++; }
            else if (*p == '"' || *p == '\\' || *p == '/') { bb_add(&b, p, 1); p++; }
            else if (*p == 'u' &&
                     hex_val(p[1]) >= 0 && hex_val(p[2]) >= 0 &&
                     hex_val(p[3]) >= 0 && hex_val(p[4]) >= 0) {
                unsigned cp = (unsigned)((hex_val(p[1]) << 12) |
                                         (hex_val(p[2]) << 8) |
                                         (hex_val(p[3]) << 4) |
                                         hex_val(p[4]));
                bb_utf8_codepoint(&b, cp);
                p += 5;
            } else {
                p++;
            }
        } else {
            bb_add(&b, p, 1);
            p++;
        }
    }
    if (*p != '"') {
        free(b.data);
        return NULL;
    }
    p++;
    bb_add(&b, "\0", 1);
    *pp = p;
    return b.data;
}

static void skip_ws(const char **pp) {
    while (**pp == ' ' || **pp == '\r' || **pp == '\n' || **pp == '\t') (*pp)++;
}

static size_t parse_results_array(const char *json, char **out, size_t cap) {
    const char *p = strstr(json, "\"results\"");
    if (!p) return 0;
    p = strchr(p, '[');
    if (!p) return 0;
    p++;
    size_t n = 0;
    while (*p && n < cap) {
        skip_ws(&p);
        if (*p == ']') break;
        if (*p == '"') {
            out[n++] = json_parse_string(&p);
        } else {
            while (*p && *p != ',' && *p != ']') p++;
        }
        skip_ws(&p);
        if (*p == ',') p++;
    }
    return n;
}

static void translate_strings(PatchHttp *http, StrList *texts, char **translations, size_t *live_used);
static int gdscript_exact(const char *s, const char **values, size_t n);

static void textpatch_free(TextPatchList *l) {
    free(l->v);
    memset(l, 0, sizeof *l);
}

static int textpatch_push(TextPatchList *l, uint32_t start, uint32_t end, int source_index) {
    if (source_index < 0 || start >= end) return 1;
    if (l->n == l->cap) {
        size_t nc = l->cap ? l->cap * 2 : 64;
        TextPatch *nv = (TextPatch *)realloc(l->v, nc * sizeof *nv);
        if (!nv) return 0;
        l->v = nv;
        l->cap = nc;
    }
    l->v[l->n].start = start;
    l->v[l->n].end = end;
    l->v[l->n].source_index = source_index;
    l->n++;
    return 1;
}

static int key_norm_char(char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A' + 'a';
    if (c >= 'a' && c <= 'z') return c;
    if (c >= '0' && c <= '9') return c;
    return 0;
}

static int godot_key_equals_normalized(const char *key, const char *want) {
    size_t i = 0, j = 0;
    while (key[i] || want[j]) {
        int a = 0, b = 0;
        while (key[i] && !(a = key_norm_char(key[i]))) i++;
        while (want[j] && !(b = key_norm_char(want[j]))) j++;
        if (!a || !b) return a == b;
        if (a != b) return 0;
        i++;
        j++;
    }
    return 1;
}

static int godot_display_text_key(const char *key) {
    static const char *keys[] = {
        "text", "disabledtext", "displaytext", "bbcodetext", "placeholdertext",
        "tooltiptext", "hinttooltip", "dialogtext", "title", "windowtitle",
        "message", "caption", "description", "desc", "shortdescription",
        "longdescription", "body", "content", "prompt", "choice", "buttontext",
        "label", "name", "editorname", "displayname", "flavortext", "summary",
        "kin", "traittype", "groupname", "category"
    };
    if (!key || !*key) return 0;
    for (size_t i = 0; i < sizeof keys / sizeof keys[0]; i++) {
        if (godot_key_equals_normalized(key, keys[i])) return 1;
    }
    return 0;
}

static char *dup_trimmed_key(const char *start, const char *end) {
    while (start < end && (*start == ' ' || *start == '\t')) start++;
    while (end > start && (end[-1] == ' ' || end[-1] == '\t')) end--;
    if (end <= start || (size_t)(end - start) > 128u) return NULL;
    char *s = (char *)malloc((size_t)(end - start) + 1);
    if (!s) return NULL;
    memcpy(s, start, (size_t)(end - start));
    s[end - start] = 0;
    return s;
}

static char *json_parse_string_range(const char **pp, const char **start, const char **end) {
    const char *p = *pp;
    if (*p != '"') return NULL;
    if (start) *start = p;
    char *s = json_parse_string(&p);
    if (!s) return NULL;
    if (end) *end = p;
    *pp = p;
    return s;
}

static int is_json_key_token(const char *end) {
    const char *p = end;
    skip_ws(&p);
    return *p == ':';
}

static char *json_key_before_value(const char *buf, const char *quote) {
    const char *p = quote;
    while (p > buf && (p[-1] == ' ' || p[-1] == '\t' || p[-1] == '\r' || p[-1] == '\n')) p--;
    if (p <= buf || p[-1] != ':') return NULL;
    p--;
    while (p > buf && (p[-1] == ' ' || p[-1] == '\t' || p[-1] == '\r' || p[-1] == '\n')) p--;
    if (p <= buf || p[-1] != '"') return NULL;

    const char *key_end_quote = p - 1;
    const char *key_start = key_end_quote;
    while (key_start > buf) {
        key_start--;
        if (*key_start != '"') continue;
        int slashes = 0;
        const char *s = key_start;
        while (s > buf && s[-1] == '\\') {
            slashes++;
            s--;
        }
        if ((slashes % 2) == 0) break;
    }
    if (*key_start != '"') return NULL;
    const char *cursor = key_start;
    char *key = json_parse_string(&cursor);
    if (!key || cursor != key_end_quote + 1) {
        free(key);
        return NULL;
    }
    return key;
}

static int scene_property_key_before_quote(const char *buf, const char *quote, char **out_key) {
    const char *line = quote;
    while (line > buf && line[-1] != '\n' && line[-1] != '\r') line--;
    if (*line == '[') return 0;

    const char *eq = NULL;
    for (const char *p = line; p < quote; p++) {
        if (*p == '=') eq = p;
    }
    if (!eq) return 0;
    char *key = dup_trimmed_key(line, eq);
    if (!key) return 0;
    if (!godot_display_text_key(key)) {
        free(key);
        return 0;
    }
    *out_key = key;
    return 1;
}

static int collect_json_text_patches(const char *buf, DWORD size, StrList *texts, TextPatchList *patches) {
    const char *p = buf;
    const char *end_buf = buf + size;
    while (p < end_buf && *p) {
        if (*p != '"') {
            p++;
            continue;
        }
        const char *start = NULL, *end = NULL;
        const char *cursor = p;
        char *value = json_parse_string_range(&cursor, &start, &end);
        if (!value) {
            p++;
            continue;
        }
        if (!is_json_key_token(end)) {
            char *key = json_key_before_value(buf, start);
            if (key && godot_display_text_key(key) && wanted_source_text(value)) {
                int idx = strlist_push_unique_index(texts, value);
                if (!textpatch_push(patches, (uint32_t)(start - buf), (uint32_t)(end - buf), idx)) {
                    free(key);
                    free(value);
                    return 0;
                }
            }
            free(key);
        }
        free(value);
        p = cursor;
    }
    return 1;
}

static int collect_scene_text_patches(const char *buf, DWORD size, StrList *texts, TextPatchList *patches) {
    const char *p = buf;
    const char *end_buf = buf + size;
    while (p < end_buf && *p) {
        if (*p != '"') {
            p++;
            continue;
        }
        const char *start = NULL, *end = NULL;
        const char *cursor = p;
        char *value = json_parse_string_range(&cursor, &start, &end);
        if (!value) {
            p++;
            continue;
        }
        char *key = NULL;
        if (scene_property_key_before_quote(buf, start, &key) && wanted_source_text(value)) {
            int idx = strlist_push_unique_index(texts, value);
            if (!textpatch_push(patches, (uint32_t)(start - buf), (uint32_t)(end - buf), idx)) {
                free(key);
                free(value);
                return 0;
            }
        }
        free(key);
        free(value);
        p = cursor;
    }
    return 1;
}

static char *build_text_resource_override(const unsigned char *buf, DWORD size,
                                          TextPatchList *patches, char **translations,
                                          size_t *patched_strings, DWORD *out_size) {
    *patched_strings = 0;
    ByteBuf out = {0};
    uint32_t last = 0;
    for (size_t i = 0; i < patches->n; i++) {
        TextPatch *p = &patches->v[i];
        if (p->start < last || p->end > size) {
            free(out.data);
            return NULL;
        }
        bb_add(&out, (const char *)buf + last, p->start - last);
        const char *translated = p->source_index >= 0 ? translations[p->source_index] : NULL;
        if (translated && wanted_translation_text(translated)) {
            char *wrapped = godot_wrap_cjk_translation(translated);
            const char *final_text = wrapped ? wrapped : translated;
            bb_json_string(&out, final_text);
            free(wrapped);
            (*patched_strings)++;
        } else {
            bb_add(&out, (const char *)buf + p->start, p->end - p->start);
        }
        last = p->end;
    }
    bb_add(&out, (const char *)buf + last, size - last);
    if (!*patched_strings || out.len > UINT32_MAX) {
        free(out.data);
        return NULL;
    }
    *out_size = (DWORD)out.len;
    return out.data;
}

static char *build_text_resource_patch(const char *path, const unsigned char *buf, DWORD size,
                                       PatchHttp *http, size_t *live_used,
                                       size_t *patched_strings, DWORD *out_size) {
    *patched_strings = 0;
    if (!path || !buf || !size || !valid_utf8_payload((const char *)buf, size)) return NULL;
    StrList texts = {0};
    TextPatchList patches = {0};
    int ok = 0;
    if (ascii_ends_with_i(path, ".json")) {
        ok = collect_json_text_patches((const char *)buf, size, &texts, &patches);
    } else {
        ok = collect_scene_text_patches((const char *)buf, size, &texts, &patches);
    }
    if (!ok || !texts.n || !patches.n) {
        strlist_free(&texts);
        textpatch_free(&patches);
        return NULL;
    }

    char **translations = (char **)calloc(texts.n, sizeof *translations);
    if (!translations) {
        strlist_free(&texts);
        textpatch_free(&patches);
        return NULL;
    }
    translate_strings(http, &texts, translations, live_used);
    char *resource = build_text_resource_override(buf, size, &patches, translations, patched_strings, out_size);
    for (size_t i = 0; i < texts.n; i++) free(translations[i]);
    free(translations);
    strlist_free(&texts);
    textpatch_free(&patches);
    return resource;
}

static void bb_add_u32le(ByteBuf *b, uint32_t v) {
    char tmp[4];
    tmp[0] = (char)(v & 0xff);
    tmp[1] = (char)((v >> 8) & 0xff);
    tmp[2] = (char)((v >> 16) & 0xff);
    tmp[3] = (char)((v >> 24) & 0xff);
    bb_add(b, tmp, sizeof tmp);
}

static int count_substr(const char *s, const char *needle) {
    int n = 0;
    size_t m = strlen(needle);
    if (!s || !needle || !m) return 0;
    while ((s = strstr(s, needle)) != NULL) {
        n++;
        s += m;
    }
    return n;
}

static int gdscript_translation_preserves_format_tokens(const char *source, const char *translated) {
    static const char *tokens[] = { "%s", "%d", "%1.2f", "%%" };
    if (!source || !translated) return 0;
    if (strchr(source, '%') && !strchr(translated, '%')) return 0;
    for (size_t i = 0; i < sizeof tokens / sizeof tokens[0]; i++) {
        if (count_substr(source, tokens[i]) != count_substr(translated, tokens[i])) return 0;
    }
    return 1;
}

static int gdscript_exact(const char *s, const char **values, size_t n) {
    if (!s) return 0;
    for (size_t i = 0; i < n; i++) {
        if (!strcmp(s, values[i])) return 1;
    }
    return 0;
}

static int wanted_gdscript_constant_text(const char *path, const char *s) {
    if (!path || !s || !wanted_source_text(s)) return 0;
    if (s[0] == '_' || strchr(s, '_')) return 0;
    if (!strcmp(s, "pressed") || !strcmp(s, "cancel_pressed") ||
        !strcmp(s, "confirm_pressed") || !strcmp(s, "ui_cancel") ||
        !strcmp(s, "stackable") || !strcmp(s, "normal_font") ||
        !strcmp(s, "RichTextLabel")) {
        return 0;
    }
    if (!strncmp(s, "ERROR:", 6)) return 0;

    if (ascii_ends_with_i(path, "/in_game_editor/scripts/resource/scenario_action_data.gdc")) {
        static const char *allowed[] = {
            "Sub Actions: %s", "Special Effects: %s\n", "SP: %d", "HP: %d",
            "Maximum Uses: %d", "Cooldown: %d", "Type: ", "Weapon", "Passive %s",
            "Target Type: %s\t", "Target Type: Empty Space\t", "Target Range: ",
            "Hit Range: -", "Hit Range: ", "Teleport Range: -", "Teleport Range: ",
            "Element: %s", "Resist: %s", "Hits: %d", "Hits: %d-%d", "Apply: "
        };
        return gdscript_exact(s, allowed, sizeof allowed / sizeof allowed[0]);
    }

    if (ascii_ends_with_i(path, "/in_game_editor/scripts/static/target_type.gdc")) {
        static const char *allowed[] = {
            "Unit", "Empty Space", "Any", "Self", "Ally", "Other Ally",
            "Enemy", "Anyone", "Anyone Else"
        };
        return gdscript_exact(s, allowed, sizeof allowed / sizeof allowed[0]);
    }

    if (ascii_ends_with_i(path, "/in_game_editor/scripts/static/unit_stat.gdc")) {
        static const char *allowed[] = {
            "HP", "SP", "ATK", "DEF", "RES", "SKL", "SPD", "MOV", "JH",
            "Health Points", "Special Points", "Attack", "Defense", "Resistance",
            "Speed", "Skill", "Movement", "Jump Height"
        };
        return gdscript_exact(s, allowed, sizeof allowed / sizeof allowed[0]);
    }

    if (ascii_contains_i(path, "/scripts/menus/") || ascii_contains_i(path, "/scripts/scenario/")) {
        if (strchr(s, '\n') || strchr(s, '?') || strchr(s, '!')) return 1;
        if (strchr(s, ' ') && !strchr(s, '%')) return 1;
        if (ascii_ends_with_i(path, "/confirm_popup.gdc") && !strcmp(s, "Title")) return 1;
    }

    return 0;
}

static int gdscript_variant_payload_size(uint32_t type, uint32_t *payload_size) {
    switch (type) {
    case 0: *payload_size = 0; return 1;
    case 1:
    case 2:
    case 3:
    case 16:
    case 17:
        *payload_size = 4; return 1;
    case 5: *payload_size = 8; return 1;
    case 6:
    case 9:
    case 10:
    case 14:
        *payload_size = 16; return 1;
    case 7: *payload_size = 12; return 1;
    case 8:
    case 11:
        *payload_size = 24; return 1;
    case 12: *payload_size = 36; return 1;
    case 13: *payload_size = 48; return 1;
    default:
        return 0;
    }
}

static int collect_gdscript_bytecode_patches(const char *path, const unsigned char *buf, DWORD size,
                                             StrList *texts, TextPatchList *patches) {
    if (!path || !buf || size < 24 || memcmp(buf, "GDSC", 4)) return 0;
    uint32_t version = read_u32le(buf + 4);
    uint32_t identifier_count = read_u32le(buf + 8);
    uint32_t constant_count = read_u32le(buf + 12);
    if (version != 13 || identifier_count > 100000u || constant_count > 100000u) return 0;

    uint32_t pos = 24;
    for (uint32_t i = 0; i < identifier_count; i++) {
        if (pos > size || size - pos < 4) return 0;
        uint32_t n = read_u32le(buf + pos);
        pos += 4;
        if (n > size - pos) return 0;
        pos += n;
    }

    for (uint32_t i = 0; i < constant_count; i++) {
        if (pos > size || size - pos < 4) return 0;
        uint32_t type_pos = pos;
        uint32_t type = read_u32le(buf + pos);
        pos += 4;
        if (type == 4) {
            if (pos > size || size - pos < 4) return 0;
            uint32_t n = read_u32le(buf + pos);
            pos += 4;
            uint32_t padded = n + godot_variant_pad4(n);
            if (padded < n || padded > size - pos) return 0;
            if (n > 0 && n <= GODOT_PATCH_MAX_SOURCE_TEXT_BYTES &&
                valid_utf8_payload((const char *)buf + pos, n)) {
                char *s = (char *)malloc((size_t)n + 1u);
                if (!s) return 0;
                memcpy(s, buf + pos, n);
                s[n] = 0;
                if (wanted_gdscript_constant_text(path, s)) {
                    int idx = strlist_push_unique_index(texts, s);
                    if (!textpatch_push(patches, type_pos, pos + padded, idx)) {
                        free(s);
                        return 0;
                    }
                }
                free(s);
            }
            pos += padded;
        } else {
            uint32_t payload_size = 0;
            if (!gdscript_variant_payload_size(type, &payload_size)) return 0;
            if (payload_size > size - pos) return 0;
            pos += payload_size;
        }
    }
    return 1;
}

static char *build_gdscript_bytecode_override(const unsigned char *buf, DWORD size,
                                              TextPatchList *patches, char **translations,
                                              StrList *texts, size_t *patched_strings,
                                              DWORD *out_size) {
    *patched_strings = 0;
    ByteBuf out = {0};
    uint32_t last = 0;
    for (size_t i = 0; i < patches->n; i++) {
        TextPatch *p = &patches->v[i];
        if (p->start < last || p->end > size) {
            free(out.data);
            return NULL;
        }
        bb_add(&out, (const char *)buf + last, p->start - last);
        const char *source = p->source_index >= 0 ? texts->v[p->source_index] : NULL;
        const char *translated = p->source_index >= 0 ? translations[p->source_index] : NULL;
        if (translated && wanted_translation_text(translated) &&
            gdscript_translation_preserves_format_tokens(source, translated)) {
            size_t n = strlen(translated);
            if (n > UINT32_MAX) {
                free(out.data);
                return NULL;
            }
            uint32_t pad = godot_variant_pad4((uint32_t)n);
            bb_add_u32le(&out, 4u);
            bb_add_u32le(&out, (uint32_t)n);
            bb_add(&out, translated, n);
            for (uint32_t j = 0; j < pad; j++) bb_add(&out, "\0", 1);
            (*patched_strings)++;
        } else {
            bb_add(&out, (const char *)buf + p->start, p->end - p->start);
        }
        last = p->end;
    }
    bb_add(&out, (const char *)buf + last, size - last);
    if (!*patched_strings || out.len > UINT32_MAX) {
        free(out.data);
        return NULL;
    }
    *out_size = (DWORD)out.len;
    return out.data;
}

static char *build_gdscript_bytecode_patch(const char *path, const unsigned char *buf, DWORD size,
                                           PatchHttp *http, size_t *live_used,
                                           size_t *patched_strings, DWORD *out_size) {
    *patched_strings = 0;
    StrList texts = {0};
    TextPatchList patches = {0};
    if (!collect_gdscript_bytecode_patches(path, buf, size, &texts, &patches) ||
        !texts.n || !patches.n) {
        strlist_free(&texts);
        textpatch_free(&patches);
        return NULL;
    }

    char **translations = (char **)calloc(texts.n, sizeof *translations);
    if (!translations) {
        strlist_free(&texts);
        textpatch_free(&patches);
        return NULL;
    }
    translate_strings(http, &texts, translations, live_used);
    char *resource = build_gdscript_bytecode_override(buf, size, &patches, translations, &texts,
                                                      patched_strings, out_size);
    for (size_t i = 0; i < texts.n; i++) free(translations[i]);
    free(translations);
    strlist_free(&texts);
    textpatch_free(&patches);
    return resource;
}

static int http_open(PatchHttp *h) {
    memset(h, 0, sizeof *h);
    h->ses = WinHttpOpen(L"DS-GodotPatch/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                         WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!h->ses) return 0;
    h->con = WinHttpConnect(h->ses, L"127.0.0.1", 19999, 0);
    if (!h->con) {
        WinHttpCloseHandle(h->ses);
        memset(h, 0, sizeof *h);
        return 0;
    }
    return 1;
}

static void http_close(PatchHttp *h) {
    if (h->con) WinHttpCloseHandle(h->con);
    if (h->ses) WinHttpCloseHandle(h->ses);
    memset(h, 0, sizeof *h);
}

static char *http_read_body(HINTERNET req) {
    ByteBuf b = {0};
    for (;;) {
        DWORD avail = 0;
        if (!WinHttpQueryDataAvailable(req, &avail)) break;
        if (!avail) break;
        char *tmp = (char *)malloc(avail);
        if (!tmp) break;
        DWORD got = 0;
        if (!WinHttpReadData(req, tmp, avail, &got)) {
            free(tmp);
            break;
        }
        bb_add(&b, tmp, got);
        free(tmp);
    }
    bb_add(&b, "\0", 1);
    return b.data;
}

static int http_post_batch(PatchHttp *h, char **texts, size_t count, int cache_only, char **out) {
    if (!h->con || !count) return 0;
    ByteBuf body = {0};
    bb_add(&body, "{\"texts\":[", strlen("{\"texts\":["));
    for (size_t i = 0; i < count; i++) {
        if (i) bb_add(&body, ",", 1);
        bb_json_string(&body, texts[i]);
    }
    const char *tail = cache_only ? "],\"cache_only\":true}" : "],\"cache_only\":false}";
    bb_add(&body, tail, strlen(tail));

    HINTERNET req = WinHttpOpenRequest(h->con, L"POST", L"/batch", NULL, WINHTTP_NO_REFERER,
                                       WINHTTP_DEFAULT_ACCEPT_TYPES, 0);
    if (!req) {
        free(body.data);
        return 0;
    }
    WinHttpSetTimeouts(req, 3000, 3000, 12000, 45000);
    int ok = WinHttpSendRequest(req, L"Content-Type: application/json\r\n", (DWORD)-1,
                                body.data, (DWORD)body.len, (DWORD)body.len, 0) &&
             WinHttpReceiveResponse(req, NULL);
    free(body.data);
    if (!ok) {
        WinHttpCloseHandle(req);
        return 0;
    }
    DWORD status = 0, sz = sizeof status;
    WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                        WINHTTP_HEADER_NAME_BY_INDEX, &status, &sz, WINHTTP_NO_HEADER_INDEX);
    char *resp = http_read_body(req);
    WinHttpCloseHandle(req);
    if (status != 200 || !resp) {
        free(resp);
        return 0;
    }
    size_t n = parse_results_array(resp, out, count);
    free(resp);
    return n == count;
}

static size_t godot_leading_rich_tag_span(const char *s) {
    if (!s || s[0] != '<') return 0;
    if (s[1] != 'f' && s[1] != 'F') return 0;
    if (s[2] && s[2] != '>' && s[2] != ' ' && s[2] != '\t') return 0;
    for (size_t i = 3; s[i] && i < 160u; i++) {
        if (s[i] == '\r' || s[i] == '\n') return 0;
        if (s[i] == '>') return i + 1u;
    }
    return 0;
}

static char *godot_dup_patch_query_text(const char *source) {
    size_t prefix = godot_leading_rich_tag_span(source);
    if (!prefix || !source[prefix]) return NULL;
    char *body = _strdup(source + prefix);
    if (!body) return NULL;
    if (!wanted_source_text(body)) {
        free(body);
        return NULL;
    }
    return body;
}

static char *godot_restore_patch_query_text(const char *source, const char *query, char *translated) {
    if (!query || !translated) return translated;
    size_t prefix = godot_leading_rich_tag_span(source);
    if (!prefix) return translated;
    if (!strncmp(source, translated, prefix)) return translated;
    size_t translated_len = strlen(translated);
    char *out = (char *)malloc(prefix + translated_len + 1u);
    if (!out) return NULL;
    memcpy(out, source, prefix);
    memcpy(out + prefix, translated, translated_len + 1u);
    return out;
}

static void translate_strings(PatchHttp *http, StrList *texts, char **translations, size_t *live_used) {
    size_t i = 0;
    while (i < texts->n) {
        size_t n = texts->n - i;
        if (n > GODOT_PATCH_BATCH) n = GODOT_PATCH_BATCH;
        int live = *live_used < GODOT_PATCH_LIVE_LIMIT;
        if (live && *live_used + n > GODOT_PATCH_LIVE_LIMIT) n = GODOT_PATCH_LIVE_LIMIT - *live_used;
        if (!n) {
            n = texts->n - i;
            if (n > GODOT_PATCH_BATCH) n = GODOT_PATCH_BATCH;
            live = 0;
        }
        char *queries[GODOT_PATCH_BATCH] = {0};
        char *batch_texts[GODOT_PATCH_BATCH] = {0};
        for (size_t j = 0; j < n; j++) {
            queries[j] = godot_dup_patch_query_text(texts->v[i + j]);
            batch_texts[j] = queries[j] ? queries[j] : texts->v[i + j];
        }
        char *results[GODOT_PATCH_BATCH] = {0};
        if (http_post_batch(http, batch_texts, n, !live, results)) {
            for (size_t j = 0; j < n; j++) {
                if (results[j] && strcmp(results[j], batch_texts[j]) && wanted_translation_text(results[j])) {
                    char *final_text = godot_restore_patch_query_text(texts->v[i + j], queries[j], results[j]);
                    if (final_text) {
                        translations[i + j] = final_text;
                        if (final_text == results[j]) results[j] = NULL;
                    }
                }
                free(results[j]);
            }
        }
        for (size_t j = 0; j < n; j++) free(queries[j]);
        if (live) *live_used += n;
        i += n;
    }
}

static int find_packed_i32_property(const unsigned char *buf, DWORD size, uint32_t prop_index,
                                    uint32_t *data_off, uint32_t *count) {
    for (DWORD i = 0; i + 12 <= size; i++) {
        if (read_u32le(buf + i) != prop_index || read_u32le(buf + i + 4) != 0x20u) continue;
        uint32_t n = read_u32le(buf + i + 8);
        uint64_t bytes = (uint64_t)n * 4u;
        if (n > 1000000u || i + 12u > size || bytes > size - (i + 12u)) continue;
        *data_off = i + 12u;
        *count = n;
        return 1;
    }
    return 0;
}

static int find_packed_byte_property(const unsigned char *buf, DWORD size, uint32_t prop_index,
                                     uint32_t *len_off, uint32_t *data_off, uint32_t *len) {
    for (DWORD i = 0; i + 12 <= size; i++) {
        if (read_u32le(buf + i) != prop_index || read_u32le(buf + i + 4) != 0x1fu) continue;
        uint32_t n = read_u32le(buf + i + 8);
        uint32_t data = i + 12u;
        uint64_t aligned_end = (uint64_t)data + n + godot_variant_pad4(n);
        if (n > GODOT_PATCH_MAX_ENTRY_BYTES || aligned_end > size) continue;
        *len_off = i + 8u;
        *data_off = data;
        *len = n;
        return 1;
    }
    return 0;
}

static int optitem_push(OptItem **items, size_t *n, size_t *cap, OptItem item) {
    if (*n == *cap) {
        size_t nc = *cap ? *cap * 2 : 64;
        OptItem *nv = (OptItem *)realloc(*items, nc * sizeof *nv);
        if (!nv) return 0;
        *items = nv;
        *cap = nc;
    }
    (*items)[(*n)++] = item;
    return 1;
}

static char *dup_blob_string(const unsigned char *blob, uint32_t off, uint32_t n) {
    if (!n) return NULL;
    if (blob[off + n - 1] == 0) n--;
    char *s = (char *)malloc((size_t)n + 1);
    if (!s) return NULL;
    memcpy(s, blob + off, n);
    s[n] = 0;
    return s;
}

static char *dup_optimized_translation_string(const unsigned char *blob, uint32_t blob_len,
                                              uint32_t off, uint32_t comp_size,
                                              uint32_t uncomp_size) {
    if (!comp_size || off > blob_len || comp_size > blob_len - off) return NULL;
    if (comp_size == uncomp_size) return dup_blob_string(blob, off, comp_size);
    if (!uncomp_size || uncomp_size > GODOT_PATCH_MAX_ENTRY_BYTES) return NULL;
    char *s = (char *)malloc((size_t)uncomp_size + 1);
    if (!s) return NULL;
    int got = godot_smaz_decompress((const char *)blob + off, (int)comp_size, s, (int)uncomp_size);
    if (got != (int)uncomp_size) {
        free(s);
        return NULL;
    }
    s[uncomp_size] = 0;
    return s;
}

static int collect_optimized_translation_items(const unsigned char *buf, DWORD size,
                                               StrList *texts, OptItem **items, size_t *item_n,
                                               uint32_t *strings_len_off, uint32_t *strings_data_off,
                                               uint32_t *strings_len) {
    uint32_t bucket_off = 0, bucket_count = 0;
    if (!find_packed_i32_property(buf, size, 5u, &bucket_off, &bucket_count)) return 0;
    if (!find_packed_byte_property(buf, size, 6u, strings_len_off, strings_data_off, strings_len)) return 0;

    const unsigned char *strings = buf + *strings_data_off;
    size_t cap = 0;
    uint32_t pos = 0;
    while (pos + 2u <= bucket_count) {
        const unsigned char *bucket = buf + bucket_off + (uint64_t)pos * 4u;
        uint32_t bucket_size = read_u32le(bucket);
        if (bucket_size == 0 || bucket_size > 100000u) return 0;
        uint64_t next = (uint64_t)pos + 2u + (uint64_t)bucket_size * 4u;
        if (next > bucket_count) return 0;
        for (uint32_t j = 0; j < bucket_size; j++) {
            uint32_t rec = bucket_off + (pos + 2u + j * 4u) * 4u;
            uint32_t str_off = read_u32le(buf + rec + 4);
            uint32_t comp_size = read_u32le(buf + rec + 8);
            uint32_t uncomp_size = read_u32le(buf + rec + 12);
            if (comp_size > *strings_len || str_off > *strings_len - comp_size) continue;
            OptItem item;
            item.rec_off = rec;
            item.old_offset = str_off;
            item.comp_size = comp_size;
            item.uncomp_size = uncomp_size;
            item.source_index = -1;
            if (comp_size > 0) {
                char *s = dup_optimized_translation_string(strings, *strings_len, str_off, comp_size, uncomp_size);
                if (s) {
                    size_t n = strlen(s);
                    if (valid_utf8_payload(s, n) && wanted_source_text(s)) {
                        item.source_index = strlist_push_unique_index(texts, s);
                    }
                    free(s);
                }
            }
            if (!optitem_push(items, item_n, &cap, item)) return 0;
        }
        pos = (uint32_t)next;
    }
    return *item_n > 0;
}

static char *build_optimized_translation_resource(const unsigned char *buf, DWORD size,
                                                   OptItem *items, size_t item_n,
                                                   char **translations,
                                                  uint32_t strings_len_off,
                                                  uint32_t strings_data_off,
                                                  uint32_t old_strings_len,
                                                  int *has_translated,
                                                  DWORD *out_size) {
    *has_translated = 0;
    unsigned char *work = (unsigned char *)malloc(size);
    if (!work) return NULL;
    memcpy(work, buf, size);

    ByteBuf strings = {0};
    for (size_t i = 0; i < item_n; i++) {
        OptItem *it = &items[i];
        uint32_t new_off = (uint32_t)strings.len;
        const char *translated = NULL;
        if (it->source_index >= 0) translated = translations[it->source_index];
        if (translated && wanted_translation_text(translated)) {
            char *wrapped = godot_wrap_cjk_translation(translated);
            const char *final_text = wrapped ? wrapped : translated;
            size_t final_len = strlen(final_text);
            bb_add(&strings, final_text, final_len);
            bb_add(&strings, "\0", 1);
            uint32_t n = (uint32_t)(final_len + 1);
            write_u32le(work + it->rec_off + 4, new_off);
            write_u32le(work + it->rec_off + 8, n);
            write_u32le(work + it->rec_off + 12, n);
            *has_translated = 1;
            free(wrapped);
        } else {
            if (it->old_offset > old_strings_len || it->comp_size > old_strings_len - it->old_offset) {
                free(work);
                free(strings.data);
                return NULL;
            }
            bb_add(&strings, (const char *)buf + strings_data_off + it->old_offset, it->comp_size);
            write_u32le(work + it->rec_off + 4, new_off);
            write_u32le(work + it->rec_off + 8, it->comp_size);
            write_u32le(work + it->rec_off + 12, it->uncomp_size);
        }
    }

    if (!*has_translated || strings.len > UINT32_MAX) {
        free(work);
        free(strings.data);
        return NULL;
    }
    write_u32le(work + strings_len_off, (uint32_t)strings.len);

    uint32_t old_tail = strings_data_off + old_strings_len + godot_variant_pad4(old_strings_len);
    if (old_tail > size) {
        free(work);
        free(strings.data);
        return NULL;
    }
    uint32_t new_pad = godot_variant_pad4((uint32_t)strings.len);
    uint64_t total = (uint64_t)strings_data_off + strings.len + new_pad + (size - old_tail);
    if (total > UINT32_MAX) {
        free(work);
        free(strings.data);
        return NULL;
    }

    char *out = (char *)malloc((size_t)total);
    if (!out) {
        free(work);
        free(strings.data);
        return NULL;
    }
    memcpy(out, work, strings_data_off);
    memcpy(out + strings_data_off, strings.data, strings.len);
    if (new_pad) out[strings_data_off + strings.len] = 0;
    memcpy(out + strings_data_off + strings.len + new_pad, work + old_tail, size - old_tail);
    *out_size = (DWORD)total;
    free(work);
    free(strings.data);
    return out;
}

static int copy_range_to_file(const WCHAR *src_path, uint64_t offset, uint64_t size, const WCHAR *dst_path) {
    HANDLE src = CreateFileW(src_path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                             NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (src == INVALID_HANDLE_VALUE) return 0;
    HANDLE dst = CreateFileW(dst_path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (dst == INVALID_HANDLE_VALUE) {
        CloseHandle(src);
        return 0;
    }
    LARGE_INTEGER pos;
    pos.QuadPart = (LONGLONG)offset;
    int ok = SetFilePointerEx(src, pos, NULL, FILE_BEGIN);
    char *buf = (char *)malloc(1024 * 1024);
    if (!buf) ok = 0;
    uint64_t left = size;
    while (ok && left) {
        DWORD want = left > 1024 * 1024 ? 1024 * 1024 : (DWORD)left;
        DWORD got = 0, wrote = 0;
        if (!ReadFile(src, buf, want, &got, NULL) || got == 0) {
            ok = 0;
            break;
        }
        if (!WriteFile(dst, buf, got, &wrote, NULL) || wrote != got) {
            ok = 0;
            break;
        }
        left -= got;
    }
    free(buf);
    CloseHandle(dst);
    CloseHandle(src);
    return ok;
}

static int normalize_embedded_pck_v1_offsets(const WCHAR *pack_path, uint64_t original_base) {
    if (!original_base) return 1;
    HANDLE h = CreateFileW(pack_path, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return 0;

    int ok = 0;
    LARGE_INTEGER li;
    if (!GetFileSizeEx(h, &li) || li.QuadPart < 0) goto done;
    uint64_t pack_size = (uint64_t)li.QuadPart;
    unsigned char header[GODOT_PCK_V2_HEADER_SIZE];
    DWORD header_bytes = pack_size >= GODOT_PCK_V2_HEADER_SIZE ? GODOT_PCK_V2_HEADER_SIZE : GODOT_PCK_V1_HEADER_SIZE;
    PckInfo info;
    if (!read_at_exact(h, 0, header, header_bytes) ||
        !parse_pck_info(header, header_bytes, pack_size, &info)) goto done;
    if (info.format != 1 || !info.offsets_are_absolute) {
        ok = 1;
        goto done;
    }

    uint64_t pos = info.header_size;
    for (uint32_t i = 0; i < info.file_count; i++) {
        unsigned char lenbuf[4];
        if (!read_at_exact(h, pos, lenbuf, sizeof lenbuf)) goto done;
        pos += 4;
        uint32_t path_len = read_u32le(lenbuf);
        if (path_len == 0 || path_len > GODOT_PATCH_MAX_PATH) goto done;
        if (pos > pack_size || path_len > pack_size - pos) goto done;
        pos += path_len;

        unsigned char offbuf[8];
        if (pos > pack_size || info.entry_meta_size > pack_size - pos ||
            !read_at_exact(h, pos, offbuf, sizeof offbuf)) goto done;
        uint64_t old_off = read_u64le(offbuf);
        if (old_off >= original_base) {
            uint64_t new_off = old_off - original_base;
            write_u64le(offbuf, new_off);
            if (!write_at_exact(h, pos, offbuf, sizeof offbuf)) goto done;
        }
        pos += info.entry_meta_size;
    }
    ok = 1;

done:
    CloseHandle(h);
    return ok;
}

static void loose_patch_list_free(LoosePatchList *l) {
    if (!l) return;
    for (size_t i = 0; i < l->n; i++) {
        free(l->v[i].path);
        free(l->v[i].data);
    }
    free(l->v);
    memset(l, 0, sizeof *l);
}

static int loose_patch_list_push_take(LoosePatchList *l, const char *path, char *data, DWORD size) {
    if (!l || !path || !data || !size) return 0;
    if (l->n == l->cap) {
        size_t nc = l->cap ? l->cap * 2 : 64;
        LoosePatchItem *nv = (LoosePatchItem *)realloc(l->v, nc * sizeof *nv);
        if (!nv) return 0;
        l->v = nv;
        l->cap = nc;
    }
    l->v[l->n].path = _strdup(path);
    if (!l->v[l->n].path) return 0;
    l->v[l->n].data = data;
    l->v[l->n].size = size;
    l->n++;
    return 1;
}

static char *wide_rel_to_pack_path(const WCHAR *rel) {
    if (!rel || !*rel) return NULL;
    int need = WideCharToMultiByte(CP_UTF8, 0, rel, -1, NULL, 0, NULL, NULL);
    if (need <= 0 || need > (int)GODOT_PATCH_MAX_PATH) return NULL;
    char *tmp = (char *)malloc((size_t)need);
    if (!tmp) return NULL;
    if (!WideCharToMultiByte(CP_UTF8, 0, rel, -1, tmp, need, NULL, NULL)) {
        free(tmp);
        return NULL;
    }
    for (char *p = tmp; *p; p++) {
        if (*p == '\\') *p = '/';
    }
    size_t n = strlen(tmp);
    char *out = (char *)malloc(n + 7u);
    if (!out) {
        free(tmp);
        return NULL;
    }
    memcpy(out, "res://", 6);
    memcpy(out + 6, tmp, n + 1u);
    free(tmp);
    return out;
}

static int loose_skip_dir_name(const WCHAR *name) {
    return !name || !wcscmp(name, L".") || !wcscmp(name, L"..") ||
           !_wcsicmp(name, L"backup") || !_wcsicmp(name, L".import");
}

static int loose_skip_file_name(const WCHAR *name) {
    return !name || !_wcsicmp(name, GODOT_PATCH_PACK_NAME) ||
           !_wcsicmp(name, GODOT_PATCH_NEXT_NAME) ||
           !_wcsicmp(name, GODOT_PATCH_BUILDING_NAME);
}

static int loose_project_text_path(const char *path) {
    if (!path) return 0;
    if (ascii_contains_i(path, "/.import/") || ascii_contains_i(path, "res://.import/")) return 0;
    return ascii_ends_with_i(path, ".tscn") ||
           ascii_ends_with_i(path, ".tres") ||
           ascii_ends_with_i(path, ".json");
}

static int write_loose_patch_pack(const WCHAR *pack_path, LoosePatchList *items) {
    if (!pack_path || !items || !items->n || items->n > UINT32_MAX) return 0;
    uint64_t table_size = 0;
    for (size_t i = 0; i < items->n; i++) {
        size_t path_len = strlen(items->v[i].path) + 1u;
        if (path_len > GODOT_PATCH_MAX_PATH) return 0;
        table_size += 4u + path_len + 32u;
    }
    uint64_t data_off = GODOT_PCK_V1_HEADER_SIZE + table_size;
    if (data_off > UINT32_MAX) return 0;

    HANDLE h = CreateFileW(pack_path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return 0;
    int ok = 0;
    unsigned char header[GODOT_PCK_V1_HEADER_SIZE] = {0};
    write_u32le(header, GODOT_PCK_MAGIC);
    write_u32le(header + 4, 1u);
    write_u32le(header + 84, (uint32_t)items->n);
    uint64_t ignored = 0;
    if (!append_file_bytes(h, (const char *)header, sizeof header, &ignored)) goto done;

    uint64_t off = data_off;
    for (size_t i = 0; i < items->n; i++) {
        uint32_t path_len = (uint32_t)strlen(items->v[i].path) + 1u;
        unsigned char lenbuf[4];
        unsigned char meta[32] = {0};
        write_u32le(lenbuf, path_len);
        write_u64le(meta, off);
        write_u64le(meta + 8, items->v[i].size);
        if (!append_file_bytes(h, (const char *)lenbuf, sizeof lenbuf, &ignored) ||
            !append_file_bytes(h, items->v[i].path, path_len, &ignored) ||
            !append_file_bytes(h, (const char *)meta, sizeof meta, &ignored)) {
            goto done;
        }
        off += items->v[i].size;
    }
    for (size_t i = 0; i < items->n; i++) {
        if (!append_file_bytes(h, items->v[i].data, items->v[i].size, &ignored)) goto done;
    }
    ok = 1;

done:
    CloseHandle(h);
    return ok;
}

static int collect_loose_project_overrides(const WCHAR *root, const WCHAR *rel_dir, LoosePatchList *items,
                                           PatchHttp *http, size_t *live_used,
                                           size_t *patched_resources, size_t *patched_strings,
                                           const WCHAR *cjk_font, int has_cjk_font,
                                           size_t *font_replacements) {
    WCHAR dir[MAX_PATH * 4];
    if (rel_dir && rel_dir[0]) path_join(dir, MAX_PATH * 4, root, rel_dir);
    else wcsncpy(dir, root, MAX_PATH * 4 - 1);
    dir[MAX_PATH * 4 - 1] = 0;

    WCHAR pat[MAX_PATH * 4];
    path_join(pat, MAX_PATH * 4, dir, L"*");
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) return 1;
    int ok = 1;
    do {
        WCHAR child_rel[MAX_PATH * 4];
        if (rel_dir && rel_dir[0]) path_join(child_rel, MAX_PATH * 4, rel_dir, fd.cFileName);
        else wcsncpy(child_rel, fd.cFileName, MAX_PATH * 4 - 1);
        child_rel[MAX_PATH * 4 - 1] = 0;

        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            if (loose_skip_dir_name(fd.cFileName)) continue;
            if (!collect_loose_project_overrides(root, child_rel, items, http, live_used,
                                                 patched_resources, patched_strings,
                                                 cjk_font, has_cjk_font, font_replacements)) {
                ok = 0;
                break;
            }
            continue;
        }
        if (loose_skip_file_name(fd.cFileName)) continue;
        if (!_wcsicmp(child_rel, L"project.godot")) continue;

        char *pack_path = wide_rel_to_pack_path(child_rel);
        if (!pack_path) continue;
        WCHAR abs_path[MAX_PATH * 4];
        path_join(abs_path, MAX_PATH * 4, root, child_rel);

        if (loose_project_text_path(pack_path)) {
            char *buf = NULL;
            DWORD size = 0;
            if (read_file_bytes(abs_path, &buf, &size) && size <= GODOT_PATCH_MAX_ENTRY_BYTES) {
                DWORD out_size = 0;
                size_t resource_patched_strings = 0;
                char *resource = build_text_resource_patch(pack_path, (const unsigned char *)buf, size,
                                                           http, live_used,
                                                           &resource_patched_strings, &out_size);
                if (resource && resource_patched_strings > 0 &&
                    loose_patch_list_push_take(items, pack_path, resource, out_size)) {
                    (*patched_resources)++;
                    *patched_strings += resource_patched_strings;
                    resource = NULL;
                }
                free(resource);
            }
            free(buf);
        } else if (has_cjk_font && *font_replacements < GODOT_PATCH_MAX_FONT_REPLACEMENTS) {
            int font_priority = godot_patch_font_resource_priority(pack_path);
            if (font_priority >= GODOT_PATCH_LOOSE_FONT_MIN_PRIORITY) {
                char *font_buf = NULL;
                DWORD font_size = 0;
                if (read_file_bytes(cjk_font, &font_buf, &font_size) &&
                    font_size > 0 && font_size <= GODOT_PATCH_MAX_FONT_BYTES &&
                    loose_patch_list_push_take(items, pack_path, font_buf, font_size)) {
                    (*patched_resources)++;
                    (*font_replacements)++;
                    font_buf = NULL;
                }
                free(font_buf);
            }
        }
        free(pack_path);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return ok;
}

static int build_loose_project_patch_pack(const WCHAR *dir, const WCHAR *build_pack,
                                          size_t *patched_resources, size_t *patched_strings) {
    WCHAR project[MAX_PATH * 4];
    path_join(project, MAX_PATH * 4, dir, L"project.godot");
    if (!exists_path(project)) return 0;

    PatchHttp http;
    if (!http_open(&http)) {
        append_log(L"Godot: local server was not reachable while building loose project patch pack.");
        return 0;
    }

    LoosePatchList items = {0};
    int ok = 0;
    char *project_buf = NULL;
    DWORD project_size = 0;
    if (!read_file_bytes(project, &project_buf, &project_size) ||
        !loose_patch_list_push_take(&items, "res://project.godot", project_buf, project_size)) {
        free(project_buf);
        goto done;
    }
    project_buf = NULL;

    WCHAR cjk_font[MAX_PATH * 4] = {0};
    int has_cjk_font = find_system_cjk_font(cjk_font, MAX_PATH * 4);
    size_t live_used = 0;
    size_t font_replacements = 0;
    if (!collect_loose_project_overrides(dir, L"", &items, &http, &live_used,
                                         patched_resources, patched_strings,
                                         cjk_font, has_cjk_font, &font_replacements)) {
        goto done;
    }
    if (*patched_resources == 0 || *patched_strings == 0) goto done;
    ok = write_loose_patch_pack(build_pack, &items);

done:
    http_close(&http);
    loose_patch_list_free(&items);
    return ok;
}

static int find_sidecar_pck(const WCHAR *dir, PckSource *src) {
    WCHAR pat[MAX_PATH * 4];
    path_join(pat, MAX_PATH * 4, dir, L"*.pck");
    WCHAR preferred[MAX_PATH * 4] = {0};
    WCHAR exe[MAX_PATH * 4];
    if (find_exe(dir, exe, MAX_PATH * 4)) {
        const WCHAR *name = wcsrchr(exe, L'\\');
        name = name ? name + 1 : exe;
        wcsncpy(preferred, name, MAX_PATH * 4 - 1);
        preferred[MAX_PATH * 4 - 1] = 0;
        WCHAR *dot = wcsrchr(preferred, L'.');
        if (dot) *dot = 0;
        wcsncat(preferred, L".pck", MAX_PATH * 4 - wcslen(preferred) - 1);
    }
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) return 0;
    int ok = 0;
    int best_score = -1;
    WCHAR best_path[MAX_PATH * 4] = {0};
    uint64_t best_size = 0;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        if (!_wcsicmp(fd.cFileName, GODOT_PATCH_PACK_NAME) ||
            !_wcsicmp(fd.cFileName, GODOT_PATCH_NEXT_NAME) ||
            !_wcsicmp(fd.cFileName, GODOT_PATCH_BUILDING_NAME)) continue;
        WCHAR path[MAX_PATH * 4];
        path_join(path, MAX_PATH * 4, dir, fd.cFileName);
        LARGE_INTEGER sz;
        HANDLE f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                               NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
        unsigned char header[GODOT_PCK_V2_HEADER_SIZE] = {0};
        PckInfo info;
        if (f != INVALID_HANDLE_VALUE && GetFileSizeEx(f, &sz) &&
            sz.QuadPart >= GODOT_PCK_V1_HEADER_SIZE) {
            DWORD want = sz.QuadPart >= GODOT_PCK_V2_HEADER_SIZE
                ? GODOT_PCK_V2_HEADER_SIZE : GODOT_PCK_V1_HEADER_SIZE;
            if (read_at_exact(f, 0, header, want) &&
                parse_pck_info(header, want, (uint64_t)sz.QuadPart, &info)) {
                int score = preferred[0] && !_wcsicmp(fd.cFileName, preferred) ? 100 : 0;
                if (!ok || score > best_score ||
                    (score == best_score && _wcsicmp(path, best_path) < 0)) {
                    wcsncpy(best_path, path, MAX_PATH * 4 - 1);
                    best_path[MAX_PATH * 4 - 1] = 0;
                    best_size = (uint64_t)sz.QuadPart;
                    best_score = score;
                    ok = 1;
                }
            }
        }
        if (f != INVALID_HANDLE_VALUE) CloseHandle(f);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    if (ok) {
        wcsncpy(src->path, best_path, MAX_PATH * 4 - 1);
        src->path[MAX_PATH * 4 - 1] = 0;
        src->offset = 0;
        src->size = best_size;
    }
    return ok;
}

static int find_embedded_pck(const WCHAR *dir, PckSource *src) {
    WCHAR exe[MAX_PATH * 4];
    if (!find_exe(dir, exe, MAX_PATH * 4)) return 0;
    HANDLE h = CreateFileW(exe, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return 0;
    int ok = 0;
    LARGE_INTEGER file_size;
    IMAGE_DOS_HEADER dos;
    if (GetFileSizeEx(h, &file_size) &&
        file_size.QuadPart >= (LONGLONG)sizeof dos &&
        read_at_exact(h, 0, &dos, sizeof dos) &&
        dos.e_magic == IMAGE_DOS_SIGNATURE && dos.e_lfanew > 0) {
        uint64_t pe = (uint64_t)dos.e_lfanew;
        DWORD sig = 0;
        IMAGE_FILE_HEADER fh;
        if (pe <= (uint64_t)file_size.QuadPart - sizeof sig - sizeof fh &&
            read_at_exact(h, pe, &sig, sizeof sig) && sig == IMAGE_NT_SIGNATURE &&
            read_at_exact(h, pe + sizeof sig, &fh, sizeof fh) &&
            fh.NumberOfSections > 0 && fh.NumberOfSections <= 128) {
            uint64_t sections = pe + sizeof sig + sizeof fh + fh.SizeOfOptionalHeader;
            for (WORD i = 0; i < fh.NumberOfSections; i++) {
                IMAGE_SECTION_HEADER sh;
                uint64_t off = sections + (uint64_t)i * sizeof sh;
                if (off > (uint64_t)file_size.QuadPart - sizeof sh ||
                    !read_at_exact(h, off, &sh, sizeof sh)) break;
                if (sh.Name[0] == 'p' && sh.Name[1] == 'c' && sh.Name[2] == 'k' && sh.Name[3] == 0 &&
                    sh.PointerToRawData > 0 && sh.SizeOfRawData >= GODOT_PCK_V1_HEADER_SIZE) {
                    unsigned char magic[4];
                    if (read_at_exact(h, sh.PointerToRawData, magic, sizeof magic) &&
                        read_u32le(magic) == GODOT_PCK_MAGIC) {
                        wcsncpy(src->path, exe, MAX_PATH * 4 - 1);
                        src->path[MAX_PATH * 4 - 1] = 0;
                        src->offset = sh.PointerToRawData;
                        src->size = sh.SizeOfRawData;
                        ok = 1;
                        break;
                    }
                }
            }
        }
    }
    CloseHandle(h);
    return ok;
}

int godot_is_loose_project(const WCHAR *dir) {
    if (!dir || !dir[0]) return 0;
    WCHAR project[MAX_PATH * 4];
    path_join(project, MAX_PATH * 4, dir, L"project.godot");
    if (!exists_path(project)) return 0;

    PckSource src;
    memset(&src, 0, sizeof src);
    if (find_sidecar_pck(dir, &src)) return 0;
    memset(&src, 0, sizeof src);
    if (find_embedded_pck(dir, &src)) return 0;
    return 1;
}

/* Godot 3 and 4 have incompatible JSON, signal and font APIs. Keep the
   renderer bridge version-local instead of weakening the shared server API. */
static const char GODOT_RUNTIME_SIDECAR_G3[] =
"extends SceneTree\n"
"\n"
"const DST_URL = \"http://127.0.0.1:19999/batch\"\n"
"const DST_MAX_TEXT = 1200\n"
"const DST_MAX_QUEUE = 1536\n"
"const DST_PER_SCAN = 900\n"
"const DST_BATCH_SIZE = 48\n"
"const DST_MAX_INFLIGHT = 4\n"
"const DST_MAX_CACHE = 4096\n"
"const DST_MAX_MISS = 4096\n"
"const DST_MISS_BACKOFF_MS = 8000\n"
"const DST_FONT_PATHS = [\"C:/Windows/Fonts/simhei.ttf\", \"C:/Windows/Fonts/msyh.ttf\", \"C:/Windows/Fonts/simsun.ttc\"]\n"
"\n"
"var _dst_cache = {}\n"
"var _dst_cache_order = []\n"
"var _dst_pending = {}\n"
"var _dst_queue = []\n"
"var _dst_http_pool = []\n"
"var _dst_busy = {}\n"
"var _dst_miss_until = {}\n"
"var _dst_miss_order = []\n"
"var _dst_timer = null\n"
"var _dst_seen_this_scan = 0\n"
"var _dst_font = null\n"
"var _dst_error_counts = {}\n"
"\n"
"func _dst_report_error(context, detail):\n"
"	var count = int(_dst_error_counts.get(context, 0)) + 1\n"
"	_dst_error_counts[context] = count\n"
"	if count <= 3 or (count & (count - 1)) == 0:\n"
"		push_warning(\"[DeepSeek Godot] %s failed #%d: %s\" % [context, count, str(detail)])\n"
"\n"
"func _initialize():\n"
"	call_deferred(\"_dst_start\")\n"
"\n"
"func _dst_start():\n"
"	for i in range(DST_MAX_INFLIGHT):\n"
"		var req = HTTPRequest.new()\n"
"		get_root().add_child(req)\n"
"		_dst_http_pool.append(req)\n"
"		req.connect(\"request_completed\", self, \"_dst_http_done\", [req.get_instance_id()])\n"
"	_dst_timer = Timer.new()\n"
"	_dst_timer.wait_time = 0.15\n"
"	_dst_timer.one_shot = false\n"
"	get_root().add_child(_dst_timer)\n"
"	_dst_timer.connect(\"timeout\", self, \"_dst_scan\")\n"
"	_dst_timer.start()\n"
"	var scene = str(ProjectSettings.get_setting(\"application/run/main_scene\"))\n"
"	if scene != \"\":\n"
"		change_scene(scene)\n"
"\n"
"func _dst_scan():\n"
"	_dst_seen_this_scan = 0\n"
"	_dst_scan_node(get_root())\n"
"	_dst_pump()\n"
"\n"
"func _dst_scan_node(node):\n"
"	if node == null or _dst_seen_this_scan >= DST_PER_SCAN:\n"
"		return\n"
"	if node is CanvasItem and not node.is_visible_in_tree():\n"
"		return\n"
"	if node.get(\"bbcode_enabled\") == true:\n"
"		_dst_apply_property(node, \"bbcode_text\")\n"
"	else:\n"
"		_dst_apply_property(node, \"text\")\n"
"		_dst_apply_property(node, \"bbcode_text\")\n"
"	_dst_apply_items(node)\n"
"	for child in node.get_children():\n"
"		_dst_scan_node(child)\n"
"		if _dst_seen_this_scan >= DST_PER_SCAN:\n"
"			return\n"
"\n"
"func _dst_apply_property(node, prop):\n"
"	var value = node.get(prop)\n"
"	if typeof(value) != TYPE_STRING:\n"
"		return\n"
"	var source = str(value)\n"
"	if not _dst_wanted(source):\n"
"		return\n"
"	_dst_seen_this_scan += 1\n"
"	var translated = _dst_render_cached(source)\n"
"	if translated != \"\" and translated != source:\n"
"		_dst_apply_cjk_font(node)\n"
"		node.set(prop, translated)\n"
"	else:\n"
"		_dst_queue_text(source)\n"
"\n"
"func _dst_apply_items(node):\n"
"	if not node.has_method(\"get_item_count\") or not node.has_method(\"get_item_text\") or not node.has_method(\"set_item_text\"):\n"
"		return\n"
"	var count = node.get_item_count()\n"
"	for i in range(count):\n"
"		if _dst_seen_this_scan >= DST_PER_SCAN:\n"
"			return\n"
"		var source = str(node.get_item_text(i))\n"
"		if not _dst_wanted(source):\n"
"			continue\n"
"		_dst_seen_this_scan += 1\n"
"		var translated = _dst_render_cached(source)\n"
"		if translated != \"\" and translated != source:\n"
"			_dst_apply_cjk_font(node)\n"
"			node.set_item_text(i, translated)\n"
"		else:\n"
"			_dst_queue_text(source)\n"
"\n"
"func _dst_queue_text(source):\n"
"	for part in _dst_split_bbcode(str(source)):\n"
"		if part.tag:\n"
"			continue\n"
"		var query = str(part.text).strip_edges()\n"
"		if not _dst_plain_wanted(query):\n"
"			continue\n"
"		if _dst_cache.has(query) or _dst_pending.has(query) or _dst_recent_miss(query) or _dst_queue.size() >= DST_MAX_QUEUE:\n"
"			continue\n"
"		_dst_pending[query] = true\n"
"		_dst_queue.append(query)\n"
"\n"
"func _dst_pump():\n"
"	if _dst_queue.empty():\n"
"		return\n"
"	for req in _dst_http_pool:\n"
"		if _dst_queue.empty():\n"
"			return\n"
"		var id = req.get_instance_id()\n"
"		if _dst_busy.has(id):\n"
"			continue\n"
"		var batch = []\n"
"		while not _dst_queue.empty() and batch.size() < DST_BATCH_SIZE:\n"
"			batch.append(_dst_queue.pop_front())\n"
"		if batch.empty():\n"
"			continue\n"
"		_dst_busy[id] = batch\n"
"		var body = to_json({\"texts\": batch})\n"
"		var err = req.request(DST_URL, [\"Content-Type: application/json\"], false, HTTPClient.METHOD_POST, body)\n"
"		if err != OK:\n"
"			_dst_report_error(\"batch-request\", \"error=%d request_id=%d\" % [err, id])\n"
"			for source in batch:\n"
"				_dst_pending.erase(source)\n"
"			_dst_busy.erase(id)\n"
"			_dst_backoff_batch(batch)\n"
"\n"
"func _dst_http_done(result, response_code, headers, body, request_id):\n"
"	if not _dst_busy.has(request_id):\n"
"		_dst_report_error(\"unknown-request-callback\", \"request_id=%d result=%d status=%d\" % [request_id, result, response_code])\n"
"		return\n"
"	var batch = _dst_busy[request_id]\n"
"	_dst_busy.erase(request_id)\n"
"	for source in batch:\n"
"		_dst_pending.erase(source)\n"
"	if response_code != 200:\n"
"		_dst_report_error(\"batch-http\", \"request_id=%d result=%d status=%d\" % [request_id, result, response_code])\n"
"		_dst_backoff_batch(batch)\n"
"		_dst_pump()\n"
"		return\n"
"	var parsed = JSON.parse(body.get_string_from_utf8())\n"
"	if parsed.error != OK or typeof(parsed.result) != TYPE_DICTIONARY:\n"
"		_dst_report_error(\"batch-json\", \"request_id=%d parse_error=%d\" % [request_id, parsed.error])\n"
"		_dst_backoff_batch(batch)\n"
"		_dst_pump()\n"
"		return\n"
"	var data = parsed.result\n"
"	if not data.has(\"translations\") or typeof(data[\"translations\"]) != TYPE_DICTIONARY:\n"
"		_dst_report_error(\"batch-schema\", \"request_id=%d missing translations dictionary\" % request_id)\n"
"		_dst_backoff_batch(batch)\n"
"		_dst_pump()\n"
"		return\n"
"	var translations = data[\"translations\"]\n"
"	for query in batch:\n"
"		if translations.has(query):\n"
"			var translated = str(translations[query])\n"
"			if translated != \"\" and translated != query:\n"
"				_dst_cache_put(query, translated)\n"
"			else:\n"
"				_dst_mark_miss(query)\n"
"		else:\n"
"			_dst_mark_miss(query)\n"
"	_dst_pump()\n"
"\n"
"func _dst_backoff_batch(batch):\n"
"	for query in batch:\n"
"		_dst_mark_miss(query)\n"
"\n"
"func _dst_cache_put(query, translated):\n"
"	if not _dst_cache.has(query):\n"
"		_dst_cache_order.append(query)\n"
"	_dst_cache[query] = translated\n"
"	while _dst_cache_order.size() > DST_MAX_CACHE:\n"
"		var old = _dst_cache_order.pop_front()\n"
"		_dst_cache.erase(old)\n"
"\n"
"func _dst_mark_miss(query):\n"
"	if not _dst_miss_until.has(query):\n"
"		_dst_miss_order.append(query)\n"
"	_dst_miss_until[query] = OS.get_ticks_msec() + DST_MISS_BACKOFF_MS\n"
"	_dst_trim_miss()\n"
"\n"
"func _dst_recent_miss(query):\n"
"	if not _dst_miss_until.has(query):\n"
"		return false\n"
"	if OS.get_ticks_msec() < int(_dst_miss_until[query]):\n"
"		return true\n"
"	_dst_miss_until.erase(query)\n"
"	_dst_miss_order.erase(query)\n"
"	return false\n"
"\n"
"func _dst_trim_miss():\n"
"	while _dst_miss_order.size() > DST_MAX_MISS:\n"
"		var old = _dst_miss_order.pop_front()\n"
"		_dst_miss_until.erase(old)\n"
"\n"
"func _dst_render_cached(source):\n"
"	var out = \"\"\n"
"	var changed = false\n"
"	for part in _dst_split_bbcode(str(source)):\n"
"		if part.tag:\n"
"			out += str(part.text)\n"
"			continue\n"
"		var raw = str(part.text)\n"
"		var query = raw.strip_edges()\n"
"		if not _dst_plain_wanted(query):\n"
"			out += raw\n"
"			continue\n"
"		if not _dst_cache.has(query):\n"
"			return \"\"\n"
"		var translated = str(_dst_cache[query])\n"
"		out += raw.replace(query, translated)\n"
"		changed = true\n"
"	return out if changed else \"\"\n"
"\n"
"func _dst_split_bbcode(text):\n"
"	var parts = []\n"
"	var rest = str(text)\n"
"	while rest != \"\":\n"
"		var start = rest.find(\"[\")\n"
"		if start < 0:\n"
"			parts.append({\"tag\": false, \"text\": rest})\n"
"			break\n"
"		if start > 0:\n"
"			parts.append({\"tag\": false, \"text\": rest.substr(0, start)})\n"
"			rest = rest.substr(start, rest.length() - start)\n"
"		var end = rest.find(\"]\")\n"
"		if end < 0:\n"
"			parts.append({\"tag\": false, \"text\": rest})\n"
"			break\n"
"		parts.append({\"tag\": true, \"text\": rest.substr(0, end + 1)})\n"
"		rest = rest.substr(end + 1, rest.length() - end - 1)\n"
"	return parts\n"
"\n"
"func _dst_apply_cjk_font(node):\n"
"	var font = _dst_get_font()\n"
"	if font == null or node == null or not node.has_method(\"add_font_override\"):\n"
"		return\n"
"	node.add_font_override(\"font\", font)\n"
"	node.add_font_override(\"normal_font\", font)\n"
"	node.add_font_override(\"bold_font\", font)\n"
"	node.add_font_override(\"italics_font\", font)\n"
"	node.add_font_override(\"bold_italics_font\", font)\n"
"	node.add_font_override(\"mono_font\", font)\n"
"\n"
"func _dst_get_font():\n"
"	if _dst_font != null:\n"
"		return _dst_font\n"
"	var file = File.new()\n"
"	for path in DST_FONT_PATHS:\n"
"		if file.file_exists(path):\n"
"			var data = DynamicFontData.new()\n"
"			data.font_path = path\n"
"			var font = DynamicFont.new()\n"
"			font.font_data = data\n"
"			font.size = 22\n"
"			_dst_font = font\n"
"			return _dst_font\n"
"	return null\n"
"\n"
"func _dst_wanted(text):\n"
"	if text == null:\n"
"		return false\n"
"	for part in _dst_split_bbcode(str(text)):\n"
"		if not part.tag and _dst_plain_wanted(str(part.text).strip_edges()):\n"
"			return true\n"
"	return false\n"
"\n"
"func _dst_plain_wanted(s):\n"
"	if s.length() < 2 or s.length() > DST_MAX_TEXT:\n"
"		return false\n"
"	if s.find(\"res://\") >= 0 or s.find(\"user://\") >= 0 or s.find(\"/\") >= 0 or s.find(\"\\\\\") >= 0:\n"
"		return false\n"
"	var has_latin = false\n"
"	for i in range(s.length()):\n"
"		var c = s.ord_at(i)\n"
"		if c >= 0x4e00 and c <= 0x9fff:\n"
"			return false\n"
"		if (c >= 65 and c <= 90) or (c >= 97 and c <= 122):\n"
"			has_latin = true\n"
"	return has_latin\n";

static const char GODOT_RUNTIME_SIDECAR_G4[] =
"extends SceneTree\n"
"\n"
"const DST_URL = \"http://127.0.0.1:19999/batch\"\n"
"const DST_MAX_TEXT = 1200\n"
"const DST_QUEUE_LIMIT = 1536\n"
"const DST_SCAN_LIMIT = 900\n"
"const DST_BATCH_SIZE = 48\n"
"const DST_MAX_CACHE = 4096\n"
"const DST_MAX_MISS = 4096\n"
"const DST_MISS_BACKOFF_MS = 8000\n"
"const DST_FONT_PATHS = [\"C:/Windows/Fonts/simhei.ttf\", \"C:/Windows/Fonts/msyh.ttf\", \"C:/Windows/Fonts/simsun.ttc\"]\n"
"\n"
"var _dst_cache = {}\n"
"var _dst_cache_order = []\n"
"var _dst_miss_until = {}\n"
"var _dst_miss_order = []\n"
"var _dst_pending = {}\n"
"var _dst_queue = []\n"
"var _dst_busy = {}\n"
"var _dst_requests = []\n"
"var _dst_font = null\n"
"var _dst_seen = 0\n"
"var _dst_error_counts = {}\n"
"\n"
"func _dst_report_error(context, detail):\n"
"	var count = int(_dst_error_counts.get(context, 0)) + 1\n"
"	_dst_error_counts[context] = count\n"
"	if count <= 3 or (count & (count - 1)) == 0:\n"
"		push_warning(\"[DeepSeek Godot] %s failed #%d: %s\" % [context, count, str(detail)])\n"
"\n"
"func _initialize():\n"
"\tcall_deferred(\"_dst_start\")\n"
"\n"
"func _dst_start():\n"
"\tfor i in range(4):\n"
"\t\tvar req = HTTPRequest.new()\n"
"\t\tget_root().add_child(req)\n"
"\t\t_dst_requests.append(req)\n"
"\t\treq.request_completed.connect(Callable(self, \"_dst_done\").bind(req.get_instance_id()))\n"
"\tvar timer = Timer.new()\n"
"\ttimer.wait_time = 0.15\n"
"\ttimer.one_shot = false\n"
"\tget_root().add_child(timer)\n"
"\ttimer.timeout.connect(_dst_scan)\n"
"\ttimer.start()\n"
"\tvar scene = str(ProjectSettings.get_setting(\"application/run/main_scene\"))\n"
"\tif scene != \"\":\n"
"\t\tchange_scene_to_file(scene)\n"
"\n"
"func _dst_scan():\n"
"\t_dst_seen = 0\n"
"\t_dst_scan_node(get_root())\n"
"\t_dst_pump()\n"
"\n"
"func _dst_scan_node(node):\n"
"\tif node == null or _dst_seen >= DST_SCAN_LIMIT:\n"
"\t\treturn\n"
"\tif node is CanvasItem and not node.is_visible_in_tree():\n"
"\t\treturn\n"
"\tif node.get(\"bbcode_enabled\") == true:\n"
"\t\t_dst_apply(node, \"bbcode_text\")\n"
"\telse:\n"
"\t\t_dst_apply(node, \"text\")\n"
"\t\t_dst_apply(node, \"bbcode_text\")\n"
"\tif node.has_method(\"get_item_count\") and node.has_method(\"get_item_text\") and node.has_method(\"set_item_text\"):\n"
"\t\tfor i in range(node.get_item_count()):\n"
"\t\t\t_dst_apply_item(node, i)\n"
"\tfor child in node.get_children():\n"
"\t\t_dst_scan_node(child)\n"
"\n"
"func _dst_apply(node, prop):\n"
"\tvar source = node.get(prop)\n"
"\tif typeof(source) != TYPE_STRING or not _dst_wanted(source):\n"
"\t\treturn\n"
"\t_dst_seen += 1\n"
"\tvar translated = _dst_render(source)\n"
"\tif translated != \"\":\n"
"\t\t_dst_font_for(node)\n"
"\t\tnode.set(prop, translated)\n"
"\telse:\n"
"\t\t_dst_queue_text(source)\n"
"\n"
"func _dst_apply_item(node, index):\n"
"\tif _dst_seen >= DST_SCAN_LIMIT:\n"
"\t\treturn\n"
"\tvar source = str(node.get_item_text(index))\n"
"\tif not _dst_wanted(source):\n"
"\t\treturn\n"
"\t_dst_seen += 1\n"
"\tvar translated = _dst_render(source)\n"
"\tif translated != \"\":\n"
"\t\t_dst_font_for(node)\n"
"\t\tnode.set_item_text(index, translated)\n"
"\telse:\n"
"\t\t_dst_queue_text(source)\n"
"\n"
"func _dst_queue_text(source):\n"
"\tfor part in _dst_split(str(source)):\n"
"\t\tif part.tag:\n"
"\t\t\tcontinue\n"
"\t\tvar query = str(part.text).strip_edges()\n"
"\t\tif not _dst_plain_wanted(query) or _dst_cache.has(query) or _dst_pending.has(query) or _dst_recent_miss(query) or _dst_queue.size() >= DST_QUEUE_LIMIT:\n"
"\t\t\tcontinue\n"
"\t\t_dst_pending[query] = true\n"
"\t\t_dst_queue.append(query)\n"
"\n"
"func _dst_pump():\n"
"\tfor req in _dst_requests:\n"
"\t\tif _dst_queue.is_empty():\n"
"\t\t\treturn\n"
"\t\tvar id = req.get_instance_id()\n"
"\t\tif _dst_busy.has(id):\n"
"\t\t\tcontinue\n"
"\t\tvar batch = []\n"
"\t\twhile not _dst_queue.is_empty() and batch.size() < DST_BATCH_SIZE:\n"
"\t\t\tbatch.append(_dst_queue.pop_front())\n"
"\t\t_dst_busy[id] = batch\n"
"\t\tvar err = req.request(DST_URL, [\"Content-Type: application/json\"], HTTPClient.METHOD_POST, JSON.stringify({\"texts\": batch}))\n"
"\t\tif err != OK:\n"
"\t\t\t_dst_report_error(\"batch-request\", \"error=%d request_id=%d\" % [err, id])\n"
"\t\t\t_dst_busy.erase(id)\n"
"\t\t\tfor query in batch:\n"
"\t\t\t\t_dst_pending.erase(query)\n"
"\t\t\t\t_dst_mark_miss(query)\n"
"\n"
"func _dst_done(result, response_code, headers, body, request_id):\n"
"\tif not _dst_busy.has(request_id):\n"
"\t\t_dst_report_error(\"unknown-request-callback\", \"request_id=%d result=%d status=%d\" % [request_id, result, response_code])\n"
"\t\treturn\n"
"\tvar batch = _dst_busy[request_id]\n"
"\t_dst_busy.erase(request_id)\n"
"\tfor query in batch:\n"
"\t\t_dst_pending.erase(query)\n"
"\tif response_code != 200:\n"
"\t\t_dst_report_error(\"batch-http\", \"request_id=%d result=%d status=%d\" % [request_id, result, response_code])\n"
"\t\tfor query in batch:\n"
"\t\t\t_dst_mark_miss(query)\n"
"\t\t_dst_pump()\n"
"\t\treturn\n"
"\tvar json = JSON.new()\n"
"\tif json.parse(body.get_string_from_utf8()) != OK or typeof(json.data) != TYPE_DICTIONARY or not json.data.has(\"translations\"):\n"
"\t\t_dst_report_error(\"batch-json-schema\", \"request_id=%d parse_error=%s\" % [request_id, json.get_error_message()])\n"
"\t\tfor query in batch:\n"
"\t\t\t_dst_mark_miss(query)\n"
"\t\t_dst_pump()\n"
"\t\treturn\n"
"\tvar translations = json.data[\"translations\"]\n"
"\tfor query in batch:\n"
"\t\tif translations.has(query) and str(translations[query]) != query and str(translations[query]) != \"\":\n"
"\t\t\t_dst_cache_put(query, str(translations[query]))\n"
"\t\telse:\n"
"\t\t\t_dst_mark_miss(query)\n"
"\t_dst_pump()\n"
"\n"
"func _dst_cache_put(query, translated):\n"
"\tif not _dst_cache.has(query):\n"
"\t\t_dst_cache_order.append(query)\n"
"\t_dst_cache[query] = translated\n"
"\twhile _dst_cache_order.size() > DST_MAX_CACHE:\n"
"\t\t_dst_cache.erase(_dst_cache_order.pop_front())\n"
"\n"
"func _dst_mark_miss(query):\n"
"\tif not _dst_miss_until.has(query):\n"
"\t\t_dst_miss_order.append(query)\n"
"\t_dst_miss_until[query] = Time.get_ticks_msec() + DST_MISS_BACKOFF_MS\n"
"\twhile _dst_miss_order.size() > DST_MAX_MISS:\n"
"\t\t_dst_miss_until.erase(_dst_miss_order.pop_front())\n"
"\n"
"func _dst_recent_miss(query):\n"
"\tif not _dst_miss_until.has(query):\n"
"\t\treturn false\n"
"\tif Time.get_ticks_msec() < int(_dst_miss_until[query]):\n"
"\t\treturn true\n"
"\t_dst_miss_until.erase(query)\n"
"\t_dst_miss_order.erase(query)\n"
"\treturn false\n"
"\n"
"func _dst_render(source):\n"
"\tvar out = \"\"\n"
"\tvar changed = false\n"
"\tfor part in _dst_split(str(source)):\n"
"\t\tif part.tag:\n"
"\t\t\tout += str(part.text)\n"
"\t\t\tcontinue\n"
"\t\tvar raw = str(part.text)\n"
"\t\tvar query = raw.strip_edges()\n"
"\t\tif not _dst_plain_wanted(query):\n"
"\t\t\tout += raw\n"
"\t\telif not _dst_cache.has(query):\n"
"\t\t\treturn \"\"\n"
"\t\telse:\n"
"\t\t\tout += raw.replace(query, str(_dst_cache[query]))\n"
"\t\t\tchanged = true\n"
"\treturn out if changed else \"\"\n"
"\n"
"func _dst_split(text):\n"
"\tvar parts = []\n"
"\tvar rest = str(text)\n"
"\twhile rest != \"\":\n"
"\t\tvar start = rest.find(\"[\")\n"
"\t\tif start < 0:\n"
"\t\t\tparts.append({\"tag\": false, \"text\": rest})\n"
"\t\t\tbreak\n"
"\t\tif start > 0:\n"
"\t\t\tparts.append({\"tag\": false, \"text\": rest.substr(0, start)})\n"
"\t\t\trest = rest.substr(start)\n"
"\t\tvar end = rest.find(\"]\")\n"
"\t\tif end < 0:\n"
"\t\t\tparts.append({\"tag\": false, \"text\": rest})\n"
"\t\t\tbreak\n"
"\t\tparts.append({\"tag\": true, \"text\": rest.substr(0, end + 1)})\n"
"\t\trest = rest.substr(end + 1)\n"
"\treturn parts\n"
"\n"
"func _dst_font_for(node):\n"
"\tif _dst_font == null:\n"
"\t\tfor path in DST_FONT_PATHS:\n"
"\t\t\tif FileAccess.file_exists(path):\n"
"\t\t\t\tvar font = FontFile.new()\n"
"\t\t\t\tif font.load_dynamic_font(path) == OK:\n"
"\t\t\t\t\t_dst_font = font\n"
"\t\t\t\t\tbreak\n"
"\tif _dst_font != null and node.has_method(\"add_theme_font_override\"):\n"
"\t\tnode.add_theme_font_override(\"font\", _dst_font)\n"
"\t\tnode.add_theme_font_override(\"normal_font\", _dst_font)\n"
"\n"
"func _dst_wanted(text):\n"
"\tfor part in _dst_split(str(text)):\n"
"\t\tif not part.tag and _dst_plain_wanted(str(part.text).strip_edges()):\n"
"\t\t\treturn true\n"
"\treturn false\n"
"\n"
"func _dst_plain_wanted(s):\n"
"\tif s.length() < 2 or s.length() > DST_MAX_TEXT or s.find(\"res://\") >= 0 or s.find(\"user://\") >= 0 or s.find(\"/\") >= 0 or s.find(\"\\\\\") >= 0:\n"
"\t\treturn false\n"
"\tvar latin = false\n"
"\tfor i in range(s.length()):\n"
"\t\tvar c = s.unicode_at(i)\n"
"\t\tif c >= 0x4e00 and c <= 0x9fff:\n"
"\t\t\treturn false\n"
"\t\tif (c >= 65 and c <= 90) or (c >= 97 and c <= 122):\n"
"\t\t\tlatin = true\n"
"\treturn latin\n";

static int godot_runtime_major_from_source(const PckSource *src) {
    if (!src || !src->path[0] || src->size < GODOT_PCK_V1_HEADER_SIZE) return 0;
    HANDLE h = CreateFileW(src->path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return 0;
    unsigned char header[GODOT_PCK_V2_HEADER_SIZE] = {0};
    DWORD want = src->size >= GODOT_PCK_V2_HEADER_SIZE ? GODOT_PCK_V2_HEADER_SIZE : GODOT_PCK_V1_HEADER_SIZE;
    PckInfo info;
    int ok = read_at_exact(h, src->offset, header, want) &&
             parse_pck_info(header, want, src->size, &info);
    CloseHandle(h);
    if (!ok) return 0;
    return info.format == 1 ? 3 : 4;
}

static int godot_runtime_major_from_project(const WCHAR *dir) {
    WCHAR project[MAX_PATH * 4];
    path_join(project, MAX_PATH * 4, dir, L"project.godot");
    char *buf = NULL;
    DWORD size = 0;
    int major = 3;
    if (read_file_bytes(project, &buf, &size)) {
        if (ascii_contains_i(buf, "config_version=5") ||
            ascii_contains_i(buf, "PackedStringArray(\"4")) {
            major = 4;
        }
    }
    free(buf);
    return major;
}

static int godot_runtime_major(const WCHAR *dir) {
    PckSource src;
    memset(&src, 0, sizeof src);
    if (find_sidecar_pck(dir, &src) || find_embedded_pck(dir, &src)) {
        int major = godot_runtime_major_from_source(&src);
        if (major) return major;
    }
    return godot_runtime_major_from_project(dir);
}

static int write_godot_runtime_sidecar_if_changed(const WCHAR *script, const char *sidecar) {
    if (!script || !sidecar) return 0;
    size_t len = strlen(sidecar);
    if (len > UINT32_MAX) return 0;

    char *existing = NULL;
    DWORD existing_size = 0;
    if (read_file_bytes(script, &existing, &existing_size)) {
        int same = existing_size == (DWORD)len &&
                   !memcmp(existing, sidecar, len);
        free(existing);
        if (same) return 1;
    }
    return write_file_bytes(script, sidecar, (DWORD)len);
}

int godot_prepare_runtime_sidecar(const WCHAR *dir) {
    if (!dir || !dir[0]) return 0;
    WCHAR script[MAX_PATH * 4];
    path_join(script, MAX_PATH * 4, dir, GODOT_RUNTIME_SCRIPT_NAME);
    int major = godot_runtime_major(dir);
    const char *sidecar = major >= 4 ? GODOT_RUNTIME_SIDECAR_G4 : GODOT_RUNTIME_SIDECAR_G3;
    if (!write_godot_runtime_sidecar_if_changed(script, sidecar)) {
        append_log(L"Godot: failed to write runtime translation sidecar.");
        return 0;
    }
    append_log(L"Godot: prepared Godot %d runtime translation sidecar.", major);
    return 1;
}

static char *trim_pack_path(char *path, size_t n) {
    if (!path || !n) return path;
    path[n - 1] = 0;
    for (size_t i = 0; i < n; i++) {
        if (path[i] == 0) break;
    }
    return path;
}

static void free_entries(PckEntry *entries, size_t n) {
    for (size_t i = 0; i < n; i++) free(entries[i].path);
    free(entries);
}

static int patch_pack_file(const WCHAR *pack_path, size_t *patched_resources, size_t *patched_strings) {
    *patched_resources = 0;
    *patched_strings = 0;
    HANDLE h = CreateFileW(pack_path, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return 0;
    int ok = 0;
    LARGE_INTEGER pack_size_li;
    if (!GetFileSizeEx(h, &pack_size_li) || pack_size_li.QuadPart < 0) goto done;
    uint64_t pack_size = (uint64_t)pack_size_li.QuadPart;
    unsigned char header[GODOT_PCK_V2_HEADER_SIZE];
    DWORD header_bytes = pack_size >= GODOT_PCK_V2_HEADER_SIZE ? GODOT_PCK_V2_HEADER_SIZE : GODOT_PCK_V1_HEADER_SIZE;
    PckInfo info;
    if (!read_at_exact(h, 0, header, header_bytes) ||
        !parse_pck_info(header, header_bytes, pack_size, &info)) goto done;

    PckEntry *entries = (PckEntry *)calloc(info.file_count, sizeof *entries);
    if (!entries) goto done;
    size_t entry_n = 0;
    uint64_t pos = info.header_size;
    for (uint32_t i = 0; i < info.file_count; i++) {
        unsigned char lenbuf[4];
        if (!read_at_exact(h, pos, lenbuf, sizeof lenbuf)) break;
        pos += 4;
        uint32_t path_len = read_u32le(lenbuf);
        if (path_len == 0 || path_len > GODOT_PATCH_MAX_PATH) break;
        char *path = (char *)malloc((size_t)path_len + 1);
        if (!path) break;
        if (!read_at_exact(h, pos, path, path_len)) {
            free(path);
            break;
        }
        path[path_len] = 0;
        trim_pack_path(path, (size_t)path_len + 1);
        pos += path_len;
        unsigned char meta[8 + 8 + 16 + 4];
        uint64_t meta_pos = pos;
        if (!read_at_exact(h, pos, meta, info.entry_meta_size)) {
            free(path);
            break;
        }
        pos += info.entry_meta_size;
        int kind = 0;
        int font_priority = 0;
        if (godot_english_translation_path(path)) {
            kind = GODOT_PATCH_ENTRY_TRANSLATION;
        } else if (godot_patch_text_resource_path(path)) {
            kind = GODOT_PATCH_ENTRY_TEXT_RESOURCE;
        } else if ((font_priority = godot_patch_font_resource_priority(path)) > 0) {
            kind = GODOT_PATCH_ENTRY_FONT;
        } else if (godot_patch_gdscript_bytecode_path(path)) {
            kind = GODOT_PATCH_ENTRY_GDSCRIPT_BYTECODE;
        }
        if (kind) {
            entries[entry_n].path = path;
            entries[entry_n].meta_pos = meta_pos;
            entries[entry_n].rel = read_u64le(meta);
            entries[entry_n].size = read_u64le(meta + 8);
            entries[entry_n].kind = kind;
            entries[entry_n].font_priority = font_priority;
            entry_n++;
        } else {
            free(path);
        }
    }

    if (entry_n > 1) qsort(entries, entry_n, sizeof *entries, compare_pck_entries_for_patch);

    PatchHttp http;
    if (!http_open(&http)) {
        append_log(L"Godot: local server was not reachable while building patch pack.");
        free_entries(entries, entry_n);
        goto done;
    }

    WCHAR cjk_font[MAX_PATH * 4] = {0};
    int has_cjk_font = find_system_cjk_font(cjk_font, MAX_PATH * 4);
    int warned_missing_cjk_font = 0;
    size_t font_replacements = 0;
    size_t live_used = 0;
    for (size_t i = 0; i < entry_n; i++) {
        if (entries[i].kind == GODOT_PATCH_ENTRY_FONT) {
            if (font_replacements >= GODOT_PATCH_MAX_FONT_REPLACEMENTS) continue;
            if (!has_cjk_font) {
                if (!warned_missing_cjk_font) {
                    append_log(L"Godot: no system CJK font found (simhei.ttf/msyh.ttf/msyh.ttc); Chinese glyphs may render as boxes.");
                    warned_missing_cjk_font = 1;
                }
                continue;
            }
            uint64_t data_abs = 0, font_size = 0;
            if (append_file_from_path(h, cjk_font, &data_abs, &font_size) &&
                (info.offsets_are_absolute || data_abs >= info.file_base)) {
                uint64_t new_rel = info.offsets_are_absolute ? data_abs : data_abs - info.file_base;
                unsigned char meta[8 + 8 + 16 + 4] = {0};
                write_u64le(meta, new_rel);
                write_u64le(meta + 8, font_size);
                /* MD5 is optional for unencrypted packs; leave it zeroed. */
                if (write_at_exact(h, entries[i].meta_pos, meta, info.entry_meta_size)) {
                    (*patched_resources)++;
                    font_replacements++;
                }
            }
            continue;
        }
        if (entries[i].size == 0 || entries[i].size > GODOT_PATCH_MAX_ENTRY_BYTES) continue;
        uint64_t abs = info.offsets_are_absolute ? entries[i].rel : info.file_base + entries[i].rel;
        if (entries[i].size > UINT32_MAX || abs > pack_size || entries[i].size > pack_size - abs) continue;
        unsigned char *buf = (unsigned char *)malloc((size_t)entries[i].size);
        if (!buf) continue;
        if (!read_at_exact(h, abs, buf, (DWORD)entries[i].size)) {
            free(buf);
            continue;
        }

        int has_translated = 0;
        DWORD resource_size = 0;
        size_t resource_patched_strings = 0;
        char *resource = NULL;
        if (entries[i].kind == GODOT_PATCH_ENTRY_TRANSLATION) {
            StrList texts = {0};
            OptItem *items = NULL;
            size_t item_n = 0;
            uint32_t strings_len_off = 0, strings_data_off = 0, strings_len = 0;
            int parsed = collect_optimized_translation_items(buf, (DWORD)entries[i].size,
                                                             &texts, &items, &item_n,
                                                             &strings_len_off, &strings_data_off,
                                                             &strings_len);
            if (parsed && texts.n) {
                char **translations = (char **)calloc(texts.n, sizeof *translations);
                if (translations) {
                    translate_strings(&http, &texts, translations, &live_used);
                    resource = build_optimized_translation_resource(buf, (DWORD)entries[i].size,
                                                                    items, item_n,
                                                                    translations,
                                                                    strings_len_off,
                                                                    strings_data_off,
                                                                    strings_len,
                                                                    &has_translated,
                                                                    &resource_size);
                    for (size_t j = 0; j < texts.n; j++) {
                        if (translations[j]) resource_patched_strings++;
                        free(translations[j]);
                    }
                    free(translations);
                }
            }
            free(items);
            strlist_free(&texts);
        } else if (entries[i].kind == GODOT_PATCH_ENTRY_TEXT_RESOURCE) {
            resource = build_text_resource_patch(entries[i].path, buf, (DWORD)entries[i].size,
                                                 &http, &live_used,
                                                 &resource_patched_strings, &resource_size);
            has_translated = resource != NULL && resource_patched_strings > 0;
        } else if (entries[i].kind == GODOT_PATCH_ENTRY_GDSCRIPT_BYTECODE) {
            resource = build_gdscript_bytecode_patch(entries[i].path, buf, (DWORD)entries[i].size,
                                                     &http, &live_used,
                                                     &resource_patched_strings, &resource_size);
            has_translated = resource != NULL && resource_patched_strings > 0;
        }

        if (resource && has_translated) {
            uint64_t data_abs = 0;
            if (append_file_bytes(h, resource, resource_size, &data_abs) && data_abs >= info.file_base) {
                uint64_t new_rel = info.offsets_are_absolute ? data_abs : data_abs - info.file_base;
                unsigned char meta[8 + 8 + 16 + 4] = {0};
                write_u64le(meta, new_rel);
                write_u64le(meta + 8, resource_size);
                /* MD5 is optional for unencrypted packs; leave it zeroed. */
                if (write_at_exact(h, entries[i].meta_pos, meta, info.entry_meta_size)) {
                    (*patched_resources)++;
                    *patched_strings += resource_patched_strings;
                }
            }
        }
        free(resource);
        free(buf);
    }
    http_close(&http);
    free_entries(entries, entry_n);
    ok = 1;

done:
    CloseHandle(h);
    return ok;
}

int godot_prepare_patch_pack(const WCHAR *dir, WCHAR *out_pack, size_t cap) {
    if (!dir || !out_pack || cap == 0) return 0;
    WCHAR final_pack[MAX_PATH * 4];
    WCHAR build_pack[MAX_PATH * 4];
    WCHAR next_pack[MAX_PATH * 4];
    path_join(final_pack, MAX_PATH * 4, dir, GODOT_PATCH_PACK_NAME);
    path_join(build_pack, MAX_PATH * 4, dir, GODOT_PATCH_BUILDING_NAME);
    path_join(next_pack, MAX_PATH * 4, dir, GODOT_PATCH_NEXT_NAME);
    wcsncpy(out_pack, final_pack, cap - 1);
    out_pack[cap - 1] = 0;
    DeleteFileW(build_pack);

    if (godot_is_loose_project(dir)) {
        DeleteFileW(final_pack);
        DeleteFileW(next_pack);
        if (!godot_prepare_runtime_sidecar(dir)) return 0;
        path_join(out_pack, cap, dir, GODOT_RUNTIME_SCRIPT_NAME);
        append_log(L"Godot: loose project detected; prepared runtime sidecar instead of a --main-pack overlay.");
        return 1;
    }

    PckSource src;
    memset(&src, 0, sizeof src);
    int has_pck_source = find_sidecar_pck(dir, &src) || find_embedded_pck(dir, &src);
    append_log(L"Godot: building external translation patch pack...");
    size_t patched_resources = 0, patched_strings = 0;
    int should_try_loose_project = 0;
    if (has_pck_source) {
        if (!copy_range_to_file(src.path, src.offset, src.size, build_pack)) {
            DeleteFileW(build_pack);
            should_try_loose_project = 1;
            append_log(L"Godot: failed to copy original pack into patch pack; trying loose project overrides.");
        } else if (!normalize_embedded_pck_v1_offsets(build_pack, src.offset)) {
            DeleteFileW(build_pack);
            should_try_loose_project = 1;
            append_log(L"Godot: failed to normalize embedded PCK v1 offsets; trying loose project overrides.");
        } else if (!patch_pack_file(build_pack, &patched_resources, &patched_strings) || !patched_resources) {
            DeleteFileW(build_pack);
            should_try_loose_project = 1;
            append_log(L"Godot: PCK source had no translated overrides; trying loose project overrides.");
        }
        if (should_try_loose_project) {
            patched_resources = 0;
            patched_strings = 0;
            if (!build_loose_project_patch_pack(dir, build_pack, &patched_resources, &patched_strings)) {
                append_log(L"Godot: PCK source and loose project overrides had no translated resource overrides yet.");
                return 0;
            }
            append_log(L"Godot: using loose project override pack.");
        }
    } else {
        if (!build_loose_project_patch_pack(dir, build_pack, &patched_resources, &patched_strings)) {
            DeleteFileW(build_pack);
            append_log(L"Godot: no readable PCK source or loose project text overrides found for runtime patch pack.");
            return 0;
        }
    }

    if (MoveFileExW(build_pack, final_pack, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        append_log(L"Godot: patch pack ready: %s", final_pack);
    } else {
        DWORD replace_err = GetLastError();
        if (!MoveFileExW(build_pack, next_pack, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
            DWORD stage_err = GetLastError();
            DeleteFileW(build_pack);
            append_log(L"Godot: failed to install refreshed patch pack (replace error %lu, stage error %lu).", replace_err, stage_err);
            return 0;
        }
        wcsncpy(out_pack, next_pack, cap - 1);
        out_pack[cap - 1] = 0;
        append_log(L"Godot: active patch pack is busy; staged refreshed pack for next launch: %s", next_pack);
    }
    append_log(L"Godot: patched %Iu resources with %Iu translated strings.", patched_resources, patched_strings);
    return 1;
}

int godot_promote_staged_patch_pack(const WCHAR *dir) {
    if (!dir) return 0;
    WCHAR final_pack[MAX_PATH * 4];
    WCHAR next_pack[MAX_PATH * 4];
    path_join(final_pack, MAX_PATH * 4, dir, GODOT_PATCH_PACK_NAME);
    path_join(next_pack, MAX_PATH * 4, dir, GODOT_PATCH_NEXT_NAME);
    if (!exists_path(next_pack)) return 0;
    if (MoveFileExW(next_pack, final_pack, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        append_log(L"Godot: promoted refreshed patch pack from previous run.");
        return 1;
    }
    append_log(L"Godot: refreshed patch pack exists but could not be promoted yet. Windows error: %lu", GetLastError());
    return 0;
}
