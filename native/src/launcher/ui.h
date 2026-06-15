#pragma once

#include "globals.h"

void append_log(const WCHAR *fmt, ...);
void set_status(const WCHAR *text);
void invalidate_control_area(HWND ctl, int pad);
void update_cache_card(void);
void refresh_engine(void);

void apply_fonts(void);
void paint_background(HWND hwnd, HDC dc);
void layout(HWND hwnd);
void draw_button(const DRAWITEMSTRUCT *di);

void browse_folder(void);
void launch_game(const WCHAR *dir);
void start_translation(void);
