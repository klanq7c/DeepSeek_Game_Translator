#include "globals.h"

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

WCHAR g_root[MAX_PATH * 4];
WCHAR g_game[MAX_PATH * 4];

PROCESS_INFORMATION g_server_pi;
int g_server_started;

HFONT g_font_title;
HFONT g_font_heading;
HFONT g_font_body;
HFONT g_font_small;
HFONT g_font_mono;
HFONT g_font_mono_small;

HBRUSH g_brush_page;
HBRUSH g_brush_edit;
HBRUSH g_brush_log;
HBRUSH g_brush_transparent;

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
