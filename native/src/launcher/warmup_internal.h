#pragma once

/* Private warmup seam shared by warmup.c and engine scanner adapters.
   The public launcher interface remains warmup_translations() in warmup.h. */

#include "fsutil.h"

#include <stddef.h>
#include <windows.h>

#define WARMUP_MAX_ITEMS 1200
#define WARMUP_MAX_TEXT_BYTES 1200
#define GODOT_WARMUP_MAX_ITEMS 12000

typedef struct {
    char **items;
    size_t n;
    size_t cap;
    const char **seen; /* Borrowed pointers into items; TextList owns strings. */
    size_t seen_cap;
    size_t max_items; /* 0 means WARMUP_MAX_ITEMS. */
} TextList;

size_t textlist_limit(const TextList *l);
void textlist_add(TextList *l, const char *s);
void textlist_free(TextList *l);

int wide_ends_with_i(const WCHAR *s, const WCHAR *suffix);
char *dup_range(const char *s, size_t n);
char *trim_ascii(char *s);
int contains_any(const char *s, const char *chars);
int starts_with_word_i(const char *s, const char *word);
int should_warm_text(const char *s);

void bb_ch(ByteBuf *b, char c);
int bb_init(ByteBuf *b, size_t cap);

/* Shared single/double-quoted string parser. It started in the Ren'Py scanner,
   but Godot scene/script resources use the same literal shape. */
char *renpy_string_at(const char **pp);

int file_size_at_most(const WCHAR *path, DWORD max_bytes);

/* Scan RPG Maker MV/MZ resources from either the standard www/ content root
   or a flat Windows distribution whose web content lives at the game root. */
void warmup_scan_rpgm_resources(const WCHAR *dir, TextList *prefetch);

/* Godot keeps a deliberately narrow interface: scan resources into TextList.
   It does not post HTTP, import cache entries, inject hooks, or rewrite .pck. */
void warmup_scan_godot_resources(const WCHAR *dir, TextList *prefetch);
