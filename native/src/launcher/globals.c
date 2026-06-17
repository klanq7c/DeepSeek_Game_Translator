/*
 * globals.c —— 启动器全局变量定义（声明见 globals.h）。
 */
#include "globals.h"

/* 窗口/控件句柄，初始值 NULL，由 ui.c 的 CreateWindow 赋值。 */
HINSTANCE g_inst;
HWND g_main;
HWND g_title;
HWND g_subtitle;
HWND g_path_label;
HWND g_path;
HWND g_engine;
HWND g_server;
HWND g_cache;
HWND g_status;
HWND g_log;
HWND g_btn_server;
HWND g_btn_api;

/* 路径缓冲——MAX_PATH*4 预留长路径（游戏目录可能嵌套很深）。 */
WCHAR g_root[MAX_PATH * 4];
WCHAR g_game[MAX_PATH * 4];

/* 服务器子进程信息——server_proc.c 写入，deploy.c/ui.c 读取。 */
PROCESS_INFORMATION g_server_pi;
int g_server_started;

/* 字体句柄——ui.c 在 WM_CREATE 时创建，WM_DESTROY 时 DeleteObject。 */
HFONT g_font_title;
HFONT g_font_heading;
HFONT g_font_body;
HFONT g_font_small;
HFONT g_font_mono;
HFONT g_font_mono_small;

/* 预创建画刷——避免每帧重复 CreateSolidBrush。 */
HBRUSH g_brush_page;
HBRUSH g_brush_edit;
HBRUSH g_brush_log;
HBRUSH g_brush_transparent;

/* 暗色主题色板定义。背景由深到浅，文本由亮到暗，accent 用于重点高亮。 */
const COLORREF C_PAGE        = RGB(8, 14, 24);
const COLORREF C_RAIL        = RGB(13, 20, 32);
const COLORREF C_CARD        = RGB(20, 28, 44);
const COLORREF C_CARD_ELEV   = RGB(28, 38, 56);
const COLORREF C_LINE        = RGB(36, 50, 72);
const COLORREF C_LINE_BRIGHT = RGB(56, 78, 108);
const COLORREF C_TEXT        = RGB(232, 240, 248);
const COLORREF C_TEXT_DIM    = RGB(165, 180, 205);
const COLORREF C_MUTED       = RGB(108, 124, 152);
const COLORREF C_ACCENT      = RGB(94, 222, 210);
const COLORREF C_ACCENT_DARK = RGB(48, 174, 162);
const COLORREF C_ACCENT_DEEP = RGB(18, 88, 84);
const COLORREF C_GREEN       = RGB(72, 220, 156);
const COLORREF C_DANGER      = RGB(248, 113, 113);
const COLORREF C_LOG         = RGB(5, 10, 18);
const COLORREF C_LOG_TEXT    = RGB(180, 230, 220);
