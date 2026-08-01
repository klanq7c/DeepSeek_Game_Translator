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
HWND g_btn_restore;
HWND g_btn_clear_cache;

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
HBRUSH g_brush_card;
HBRUSH g_brush_edit;
HBRUSH g_brush_log;
HBRUSH g_brush_transparent;

/* 暗色主题色板定义。背景由深到浅，文本由亮到暗，accent 用于重点高亮。 */
const COLORREF C_PAGE        = RGB(9, 12, 18);
const COLORREF C_RAIL        = RGB(11, 15, 23);
const COLORREF C_CARD        = RGB(17, 23, 33);
const COLORREF C_CARD_ELEV   = RGB(25, 33, 47);
const COLORREF C_LINE        = RGB(33, 43, 58);
const COLORREF C_LINE_BRIGHT = RGB(64, 80, 102);
const COLORREF C_TEXT        = RGB(238, 243, 248);
const COLORREF C_TEXT_DIM    = RGB(170, 181, 197);
const COLORREF C_MUTED       = RGB(106, 120, 142);
const COLORREF C_ACCENT      = RGB(78, 222, 201);
const COLORREF C_ACCENT_DARK = RGB(42, 176, 159);
const COLORREF C_ACCENT_DEEP = RGB(19, 86, 79);
const COLORREF C_GREEN       = RGB(96, 214, 148);
const COLORREF C_DANGER      = RGB(246, 106, 122);
const COLORREF C_BLUE        = RGB(98, 160, 255);
const COLORREF C_VIOLET      = RGB(180, 138, 255);
const COLORREF C_AMBER       = RGB(247, 188, 78);
const COLORREF C_LOG         = RGB(6, 9, 14);
const COLORREF C_LOG_TEXT    = RGB(178, 229, 219);
