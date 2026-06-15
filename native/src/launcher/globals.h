#pragma once

#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <windows.h>

#define IDC_PATH 101
#define IDC_BROWSE 102
#define IDC_START 103
#define IDC_OPEN 104
#define IDC_STATUS 105
#define IDC_LOG 106
#define IDC_ENGINE 107
#define IDC_TITLE 108
#define IDC_SUBTITLE 109
#define IDC_SERVER 110
#define IDC_CACHE 111
#define IDC_PATH_LABEL 112
#define IDC_API_CONFIG 114
#define IDC_SERVER_TOGGLE 115

#define IDC_API_ENDPOINT 201
#define IDC_API_MODEL 202
#define IDC_API_KEY 203
#define IDC_API_SAVE 204
#define IDC_API_CANCEL 205

#define RAIL_W 256

extern HINSTANCE g_inst;
extern HWND g_main;
extern HWND g_title;
extern HWND g_subtitle;
extern HWND g_path_label;
extern HWND g_path;
extern HWND g_engine;
extern HWND g_server;
extern HWND g_cache;
extern HWND g_status;
extern HWND g_log;
extern HWND g_btn_server;
extern HWND g_btn_api;

extern WCHAR g_root[MAX_PATH * 4];
extern WCHAR g_game[MAX_PATH * 4];

extern PROCESS_INFORMATION g_server_pi;
extern int g_server_started;

extern HFONT g_font_title;
extern HFONT g_font_heading;
extern HFONT g_font_body;
extern HFONT g_font_small;
extern HFONT g_font_mono;
extern HFONT g_font_mono_small;

extern HBRUSH g_brush_page;
extern HBRUSH g_brush_edit;
extern HBRUSH g_brush_log;
extern HBRUSH g_brush_transparent;

extern const COLORREF C_PAGE;
extern const COLORREF C_RAIL;
extern const COLORREF C_CARD;
extern const COLORREF C_CARD_ELEV;
extern const COLORREF C_LINE;
extern const COLORREF C_LINE_BRIGHT;
extern const COLORREF C_TEXT;
extern const COLORREF C_TEXT_DIM;
extern const COLORREF C_MUTED;
extern const COLORREF C_ACCENT;
extern const COLORREF C_ACCENT_DARK;
extern const COLORREF C_ACCENT_DEEP;
extern const COLORREF C_GREEN;
extern const COLORREF C_DANGER;
extern const COLORREF C_LOG;
extern const COLORREF C_LOG_TEXT;
