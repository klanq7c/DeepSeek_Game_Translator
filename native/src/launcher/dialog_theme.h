#pragma once
#include "globals.h"

/* 二级窗口独立持有 DPI、字体与画刷，跨屏移动不能改变主窗口的绘制状态。 */
typedef struct {
    int dpi;
    HFONT title, body, small;
    HBRUSH card, field;
} DialogTheme;
int dialog_sc(const DialogTheme *t, int value);
int dialog_fit_dpi(HWND owner, int width, int height, int dpi);
void dialog_theme_init(DialogTheme *t, int dpi);
void dialog_theme_free(DialogTheme *t);
void dialog_place(HWND hwnd, HWND owner, int width, int height, int dpi);
HWND dialog_control(HWND hwnd, DialogTheme *t, const WCHAR *cls, const WCHAR *text, DWORD style,
                    int id, int x, int y, int width, int height);
void dialog_paint(HWND hwnd, HDC dc, DialogTheme *t, const WCHAR *title, const WCHAR *subtitle);
void dialog_round(HDC dc, RECT r, COLORREF fill, COLORREF line, int radius);
void dialog_button(const DRAWITEMSTRUCT *di, DialogTheme *t, int primary);
void dialog_combo(const DRAWITEMSTRUCT *di, DialogTheme *t, const WCHAR *label);
LRESULT dialog_color(HDC dc, DialogTheme *t, int field);
int dialog_key(HWND hwnd, MSG *m, int accept, int cancel);
int themed_message(HWND owner, const WCHAR *text, const WCHAR *title, UINT flags);
void themed_message_probe(int dpi);
int dialog_render_probe(HWND hwnd, const WCHAR *path);
