#pragma once

#include "globals.h"

typedef struct {
    char *data;
    size_t len;
    size_t cap;
} ByteBuf;

void path_join(WCHAR *out, size_t cap, const WCHAR *a, const WCHAR *b);
int exists_path(const WCHAR *p);
int is_dir(const WCHAR *p);
int ensure_dir(const WCHAR *path);

int write_text_file_utf8(const WCHAR *path, const char *bytes);
int read_file_bytes(const WCHAR *path, char **out, DWORD *size);
int write_file_bytes(const WCHAR *path, const char *buf, DWORD size);
int copy_file_safe(const WCHAR *from, const WCHAR *to);
int copy_tree_safe(const WCHAR *from, const WCHAR *to);

void bb_add(ByteBuf *b, const char *s, size_t n);

void get_config_dir(WCHAR *out, size_t cap);
void get_api_config_path(WCHAR *out, size_t cap);
void get_launcher_config_path(WCHAR *out, size_t cap);
int save_last_game_dir(const WCHAR *dir);
int load_last_game_dir(WCHAR *out, size_t cap);

HFONT make_font(int pt, int weight, const WCHAR *face);

extern int g_scale_dpi;
int sc(int v);
void dpi_set_for_window(HWND hwnd);
void dpi_enable_awareness(void);
