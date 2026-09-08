#pragma once

/* warmup.c 与各引擎扫描适配器共享的私有预热接缝。启动器公开接口仍是
   warmup.h 中的 warmup_translations()。 */

#include "fsutil.h"

#include <stddef.h>
#include <windows.h>

#define WARMUP_MAX_ITEMS 1200
#define WARMUP_MAX_TEXT_BYTES 1200
#define GODOT_WARMUP_MAX_ITEMS 12000

typedef struct {
    char **items;
    /* 可选平行数组：prev_items[i] 是 items[i] 在原作中的上一行文本。
       仅 Ren'Py 脚本扫描器维护（对话连续性最强）；字符串所有权仍归 items，
       这里只存放借用指针或 NULL，释放时只回收指针数组本身。 */
    char **prev_items;
    size_t n;
    size_t cap;
    const char **seen; /* 指向 items 的借用指针；字符串所有权归 TextList。 */
    size_t seen_cap;
    size_t max_items; /* 0 表示使用 WARMUP_MAX_ITEMS。 */
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

/* 共享的单引号或双引号字符串解析器。它最初用于 Ren'Py 扫描器，但 Godot
   场景与脚本资源也使用相同的字面量形式。 */
char *renpy_string_at(const char **pp);

int file_size_at_most(const WCHAR *path, DWORD max_bytes);

/* 从标准 www/ 内容根目录，或网页内容位于游戏根目录的扁平 Windows 发行版中，
   扫描 RPG Maker MV/MZ 资源。 */
void warmup_scan_rpgm_resources(const WCHAR *dir, TextList *prefetch);

/* Godot 接口刻意保持狭窄：只把资源扫描进 TextList；不发送 HTTP、不导入缓存
   条目、不注入钩子，也不改写 .pck。 */
void warmup_scan_godot_resources(const WCHAR *dir, TextList *prefetch);
