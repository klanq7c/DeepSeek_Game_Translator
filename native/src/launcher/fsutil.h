/*
 * fsutil.h —— 文件系统与 DPI 工具函数声明。
 *
 * 提供：路径拼接、存在/目录判定、目录创建（递归）、文件读写、文件/目录树拷贝、
 * 配置路径管理、DPI 感知与缩放。被 launcher 各模块广泛引用。
 */
#pragma once

#include "globals.h"

/* 简单字节缓冲（类似 server 的 Buf，但 C 运行时不共享，launcher 独立一份）。 */
typedef struct {
    char *data;
    size_t len;
    size_t cap;
} ByteBuf;

void path_join(WCHAR *out, size_t cap, const WCHAR *a, const WCHAR *b); /* 路径拼接，自动补 '\' */
int exists_path(const WCHAR *p);                    /* 文件或目录是否存在 */
int is_dir(const WCHAR *p);                        /* 是否为目录 */
int ensure_dir(const WCHAR *path);                  /* 递归创建目录，已存在则跳过 */

int write_text_file_utf8(const WCHAR *path, const char *bytes);   /* 以 UTF-8 写纯文本 */
int read_file_bytes(const WCHAR *path, char **out, DWORD *size);    /* 读取文件全部字节，*out 新分配 */
int write_file_bytes(const WCHAR *path, const char *buf, DWORD size); /* 写入原始字节 */
int copy_file_safe(const WCHAR *from, const WCHAR *to);             /* 复制单个文件，自动创建目标父目录 */
int copy_tree_safe(const WCHAR *from, const WCHAR *to);            /* 递归复制目录树 */

void bb_add(ByteBuf *b, const char *s, size_t n);  /* 往 ByteBuf 追加字节 */

/* 配置路径辅助——全部基于 g_root 派生。 */
void get_config_dir(WCHAR *out, size_t cap);                /* g_root\config */
void get_api_config_path(WCHAR *out, size_t cap);          /* g_root\config\api.ini */
void get_launcher_config_path(WCHAR *out, size_t cap);      /* g_root\config\launcher.ini */
int save_last_game_dir(const WCHAR *dir);                    /* 记住上次选择的目录 */
int load_last_game_dir(WCHAR *out, size_t cap);              /* 读取上次目录（校验存在） */

/* DPI 与字体——让 UI 在高分屏上不模糊。 */
HFONT make_font(int pt, int weight, const WCHAR *face);      /* 按 DPI 缩放创建字体 */

extern int g_scale_dpi;                                     /* 当前 DPI 值（默认 96） */
int sc(int v);                                              /* 缩放像素值：v * dpi / 96 */
void dpi_set_for_window(HWND hwnd);                         /* 检测窗口 DPI 并设置 g_scale_dpi */
void dpi_enable_awareness(void);                             /* 声明进程为 DPI 感知 */
