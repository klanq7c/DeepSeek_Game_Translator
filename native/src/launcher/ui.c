#include "ui.h"
#include "deploy.h"
#include "engine.h"
#include "fsutil.h"
#include "server_proc.h"
#include "warmup.h"

#include <commctrl.h>
#include <objbase.h>
#include <shlobj.h>
#include <shellapi.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#define LOG_SOFT_LIMIT 900000
#define LOG_TRIM_TARGET 600000
#define LOG_MAX_LINES 2000

static int g_log_extent_px = 0;

void invalidate_control_area(HWND ctl, int pad) {
    if (!ctl || !IsWindow(ctl) || !g_main || !IsWindow(g_main)) return;
    RECT rc;
    if (!GetWindowRect(ctl, &rc)) return;
    MapWindowPoints(NULL, g_main, (POINT *)&rc, 2);
    InflateRect(&rc, sc(pad), sc(pad));
    RedrawWindow(g_main, &rc, NULL, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN);
}

void append_log(const WCHAR *fmt, ...) {
    if (!g_log || !IsWindow(g_log)) return;
    SYSTEMTIME st;
    GetLocalTime(&st);
    WCHAR prefix[64];
    _snwprintf(prefix, 64, L"[%02d:%02d:%02d] ", st.wHour, st.wMinute, st.wSecond);
    prefix[63] = 0;

    WCHAR body[4096];
    va_list ap;
    va_start(ap, fmt);
    _vsnwprintf(body, 4096, fmt, ap);
    va_end(ap);
    body[4095] = 0;

    WCHAR line[8192];
    _snwprintf(line, 8192, L"%s%s", prefix, body);
    line[8191] = 0;

    for (WCHAR *p = line; *p; p++) {
        if (*p == L'\r' || *p == L'\n' || *p == L'\t') *p = L' ';
    }

    LRESULT count = SendMessageW(g_log, LB_GETCOUNT, 0, 0);
    while (count >= LOG_MAX_LINES) {
        SendMessageW(g_log, LB_DELETESTRING, 0, 0);
        count--;
    }

    int idx = (int)SendMessageW(g_log, LB_ADDSTRING, 0, (LPARAM)line);
    if (idx >= 0) {
        HDC dc = GetDC(g_log);
        if (dc) {
            HGDIOBJ old = SelectObject(dc, g_font_mono);
            SIZE sz = {0, 0};
            if (GetTextExtentPoint32W(dc, line, (int)wcslen(line), &sz)) {
                int extent = sz.cx + sc(32);
                if (extent > g_log_extent_px) {
                    g_log_extent_px = extent;
                    SendMessageW(g_log, LB_SETHORIZONTALEXTENT, (WPARAM)g_log_extent_px, 0);
                }
            }
            SelectObject(dc, old);
            ReleaseDC(g_log, dc);
        }
        SendMessageW(g_log, LB_SETTOPINDEX, (WPARAM)idx, 0);
    }
}

void set_status(const WCHAR *text) {
    if (g_status && IsWindow(g_status)) {
        WCHAR buf[512];
        _snwprintf(buf, 512, L"STATUS  ·  %s", text ? text : L"");
        buf[511] = 0;
        invalidate_control_area(g_status, 6);
        SetWindowTextW(g_status, buf);
        invalidate_control_area(g_status, 6);
    }
}

void update_cache_card(void) {
    if (!g_cache || !IsWindow(g_cache)) return;
    invalidate_control_area(g_cache, 4);
    WCHAR cache_path[MAX_PATH * 4];
    path_join(cache_path, MAX_PATH * 4, g_root, L"translation_memory_c.tsv");
    WIN32_FILE_ATTRIBUTE_DATA data;
    if (GetFileAttributesExW(cache_path, GetFileExInfoStandard, &data)) {
        ULONGLONG bytes = ((ULONGLONG)data.nFileSizeHigh << 32) | data.nFileSizeLow;
        WCHAR text[128];
        _snwprintf(text, 128, L"%.1f MB", (double)bytes / 1024.0 / 1024.0);
        text[127] = 0;
        invalidate_control_area(g_cache, 4);
        SetWindowTextW(g_cache, text);
        invalidate_control_area(g_cache, 4);
    } else {
        SetWindowTextW(g_cache, L"未找到");
    }
    invalidate_control_area(g_cache, 4);
}

void refresh_engine(void) {
    if (!g_engine || !IsWindow(g_engine)) return;
    GetWindowTextW(g_path, g_game, MAX_PATH * 4);
    Engine e = detect_engine(g_game);
    invalidate_control_area(g_engine, 4);
    SetWindowTextW(g_engine, engine_name(e));
    invalidate_control_area(g_engine, 4);
}

void apply_fonts(void) {
    HWND controls[] = {g_title, g_subtitle, g_path_label, g_path, g_engine, g_server, g_cache, g_status, g_log, g_btn_server, g_btn_api};
    size_t n = sizeof(controls) / sizeof(controls[0]);
    for (size_t i = 0; i < n; i++) {
        if (controls[i] && IsWindow(controls[i])) SendMessageW(controls[i], WM_SETFONT, (WPARAM)g_font_body, TRUE);
    }
    if (IsWindow(g_title))    SendMessageW(g_title,    WM_SETFONT, (WPARAM)g_font_title,   TRUE);
    if (IsWindow(g_subtitle)) SendMessageW(g_subtitle, WM_SETFONT, (WPARAM)g_font_small,   TRUE);
    if (IsWindow(g_path_label))SendMessageW(g_path_label,WM_SETFONT,(WPARAM)g_font_heading,TRUE);
    if (IsWindow(g_engine))   SendMessageW(g_engine,   WM_SETFONT, (WPARAM)g_font_heading, TRUE);
    if (IsWindow(g_server))   SendMessageW(g_server,   WM_SETFONT, (WPARAM)g_font_heading, TRUE);
    if (IsWindow(g_cache))    SendMessageW(g_cache,    WM_SETFONT, (WPARAM)g_font_heading, TRUE);
    if (IsWindow(g_status))   SendMessageW(g_status,   WM_SETFONT, (WPARAM)g_font_mono_small, TRUE);
    if (IsWindow(g_log)) {
        SendMessageW(g_log, WM_SETFONT, (WPARAM)g_font_mono, TRUE);
        HDC dc = GetDC(g_log);
        if (dc) {
            HGDIOBJ old = SelectObject(dc, g_font_mono);
            TEXTMETRICW tm;
            if (GetTextMetricsW(dc, &tm)) {
                SendMessageW(g_log, LB_SETITEMHEIGHT, 0, (LPARAM)(tm.tmHeight + sc(6)));
            }
            SelectObject(dc, old);
            ReleaseDC(g_log, dc);
        }
    }
}

static void draw_round(HDC dc, RECT rc, COLORREF fill, COLORREF line, int radius) {
    HBRUSH b = CreateSolidBrush(fill);
    HPEN p = CreatePen(PS_SOLID, 1, line);
    HGDIOBJ ob = SelectObject(dc, b);
    HGDIOBJ op = SelectObject(dc, p);
    RoundRect(dc, rc.left, rc.top, rc.right, rc.bottom, radius, radius);
    SelectObject(dc, ob);
    SelectObject(dc, op);
    DeleteObject(b);
    DeleteObject(p);
}

static void draw_dot(HDC dc, int cx, int cy, int radius, COLORREF fill, COLORREF halo) {
    HBRUSH bh = CreateSolidBrush(halo);
    HGDIOBJ ob = SelectObject(dc, bh);
    HGDIOBJ op = SelectObject(dc, GetStockObject(NULL_PEN));
    Ellipse(dc, cx - radius - 3, cy - radius - 3, cx + radius + 3, cy + radius + 3);
    HBRUSH bf = CreateSolidBrush(fill);
    SelectObject(dc, bf);
    Ellipse(dc, cx - radius, cy - radius, cx + radius, cy + radius);
    SelectObject(dc, ob);
    SelectObject(dc, op);
    DeleteObject(bh);
    DeleteObject(bf);
}

static void draw_text_x(HDC dc, const WCHAR *text, int x, int y, int w, int h, COLORREF color, HFONT font, UINT flags) {
    SelectObject(dc, font);
    SetTextColor(dc, color);
    RECT r = {x, y, x + w, y + h};
    DrawTextW(dc, text, -1, &r, flags);
}

static COLORREF mix(COLORREF a, COLORREF b, float t) {
    int ar = GetRValue(a), ag = GetGValue(a), ab = GetBValue(a);
    int br = GetRValue(b), bg = GetGValue(b), bb = GetBValue(b);
    int r = ar + (int)((br - ar) * t);
    int g = ag + (int)((bg - ag) * t);
    int bl = ab + (int)((bb - ab) * t);
    return RGB(r, g, bl);
}

static void draw_vgradient(HDC dc, int x, int y, int w, int h, COLORREF top, COLORREF bot) {
    TRIVERTEX v[2] = {
        { x,     y,     (USHORT)(GetRValue(top) << 8), (USHORT)(GetGValue(top) << 8), (USHORT)(GetBValue(top) << 8), 0 },
        { x + w, y + h, (USHORT)(GetRValue(bot) << 8), (USHORT)(GetGValue(bot) << 8), (USHORT)(GetBValue(bot) << 8), 0 }
    };
    GRADIENT_RECT gr = { 0, 1 };
    GradientFill(dc, v, 2, &gr, 1, GRADIENT_FILL_RECT_V);
}

typedef struct UiLayout {
    int rail;
    int pad;
    int x;
    int w;
    int hero_right;
    RECT picker;
    int path_label_x;
    int path_label_y;
    int path_label_w;
    int path_label_h;
    int path_x;
    int path_y;
    int path_w;
    int path_h;
    int side_button_w;
    int side_gap;
    int browse_x;
    int open_x;
    int action_y;
    int action_h;
    int metric_gap;
    int metric_w;
    int metric_y;
    int metric_h;
    int metric_value_y;
    int metric_value_h;
    int log_top;
    int log_bottom;
    int log_edit_top;
    int log_edit_h;
} UiLayout;

static int max_i(int a, int b) {
    return a > b ? a : b;
}

static UiLayout compute_layout(HWND hwnd) {
    RECT r;
    GetClientRect(hwnd, &r);

    UiLayout ui;
    ui.rail = sc(RAIL_W);
    ui.pad = sc(28);
    ui.x = ui.rail + ui.pad;
    ui.w = max_i(r.right - ui.x - ui.pad, sc(560));
    ui.hero_right = sc(220);

    int picker_top = sc(136);
    int picker_h = sc(188);
    ui.picker.left = ui.x;
    ui.picker.top = picker_top;
    ui.picker.right = ui.x + ui.w;
    ui.picker.bottom = picker_top + picker_h;

    ui.path_label_x = ui.x + sc(24);
    ui.path_label_y = picker_top + sc(24);
    ui.path_label_w = sc(240);
    ui.path_label_h = sc(24);

    ui.path_x = ui.x + sc(24);
    ui.path_y = picker_top + sc(72);
    ui.path_h = sc(38);
    ui.side_button_w = sc(112);
    ui.side_gap = sc(12);
    ui.open_x = ui.x + ui.w - sc(24) - ui.side_button_w;
    ui.browse_x = ui.open_x - ui.side_gap - ui.side_button_w;
    ui.path_w = max_i(ui.browse_x - ui.path_x - ui.side_gap, sc(180));

    ui.action_y = picker_top + sc(128);
    ui.action_h = sc(42);

    ui.metric_gap = sc(16);
    ui.metric_w = max_i((ui.w - ui.metric_gap * 2) / 3, sc(150));
    ui.metric_y = ui.picker.bottom + sc(20);
    ui.metric_h = sc(100);
    ui.metric_value_y = ui.metric_y + sc(40);
    ui.metric_value_h = sc(34);

    ui.log_top = ui.metric_y + ui.metric_h + sc(20);
    ui.log_bottom = max_i(r.bottom - sc(24), ui.log_top + sc(96));
    ui.log_edit_top = ui.log_top + sc(56);
    ui.log_edit_h = max_i(ui.log_bottom - sc(18) - ui.log_edit_top, sc(60));

    return ui;
}

void paint_background(HWND hwnd, HDC dc) {
    RECT r;
    GetClientRect(hwnd, &r);

    UiLayout ui = compute_layout(hwnd);
    int rail = ui.rail;
    int pad = ui.pad;

    FillRect(dc, &r, g_brush_page);

    /* Rail with subtle top→bottom gradient */
    draw_vgradient(dc, 0, 0, rail, r.bottom, C_RAIL, C_PAGE);

    /* Rail right divider */
    HPEN dvpen = CreatePen(PS_SOLID, 1, C_LINE);
    HGDIOBJ odv = SelectObject(dc, dvpen);
    MoveToEx(dc, rail, 0, NULL);
    LineTo(dc, rail, r.bottom);
    SelectObject(dc, odv);
    DeleteObject(dvpen);

    SetBkMode(dc, TRANSPARENT);

    /* Brand mark: diamond + DeepSeek */
    {
        POINT diamond[4] = {{sc(28), sc(40)}, {sc(38), sc(30)}, {sc(48), sc(40)}, {sc(38), sc(50)}};
        HBRUSH ab = CreateSolidBrush(C_ACCENT);
        HGDIOBJ ob = SelectObject(dc, ab);
        HGDIOBJ op = SelectObject(dc, GetStockObject(NULL_PEN));
        Polygon(dc, diamond, 4);
        SelectObject(dc, ob);
        SelectObject(dc, op);
        DeleteObject(ab);
    }
    draw_text_x(dc, L"DeepSeek",   sc(60), sc(22), rail - sc(80), sc(28), C_TEXT, g_font_heading, DT_LEFT | DT_SINGLELINE | DT_VCENTER);
    draw_text_x(dc, L"Translator", sc(60), sc(48), rail - sc(80), sc(20), C_MUTED, g_font_small,   DT_LEFT | DT_SINGLELINE | DT_VCENTER);

    /* Section divider */
    HPEN sd = CreatePen(PS_SOLID, 1, C_LINE);
    HGDIOBJ osd = SelectObject(dc, sd);
    MoveToEx(dc, sc(24), sc(90), NULL);
    LineTo(dc, rail - sc(24), sc(90));
    SelectObject(dc, osd);
    DeleteObject(sd);

    /* Active nav item with left accent rail */
    int navY = sc(112);
    int navH = sc(42);
    RECT navBg = {sc(16), navY, rail - sc(16), navY + navH};
    draw_round(dc, navBg, C_CARD_ELEV, C_LINE, sc(8));
    RECT navAcc = {sc(16), navY + sc(8), sc(19), navY + navH - sc(8)};
    HBRUSH ab2 = CreateSolidBrush(C_ACCENT);
    FillRect(dc, &navAcc, ab2);
    DeleteObject(ab2);
    draw_text_x(dc, L"运行时汉化", sc(34), navY, rail - sc(50), navH, C_TEXT, g_font_body, DT_LEFT | DT_SINGLELINE | DT_VCENTER);

    /* Capability bullets */
    int capY = navY + sc(64);
    int capStep = sc(28);
    const WCHAR *caps[] = {L"本地缓存优先", L"运行时不等待 API", L"标签/变量保护"};
    for (int i = 0; i < 3; i++) {
        int yy = capY + i * capStep;
        draw_dot(dc, sc(28), yy + sc(10), sc(2), C_ACCENT, C_ACCENT_DEEP);
        draw_text_x(dc, caps[i], sc(40), yy, rail - sc(56), sc(22), C_TEXT_DIM, g_font_small, DT_LEFT | DT_SINGLELINE | DT_VCENTER);
    }

    /* Rail footer: version chip + runtime tag */
    int footY = r.bottom - sc(58);
    int footH = sc(26);
    RECT chip = {sc(20), footY, sc(88), footY + footH};
    draw_round(dc, chip, C_CARD_ELEV, C_LINE_BRIGHT, sc(6));
    draw_text_x(dc, L"v3.1.70", sc(20), footY, sc(68), footH, C_ACCENT, g_font_mono_small, DT_CENTER | DT_SINGLELINE | DT_VCENTER);
    draw_text_x(dc, L"C native runtime", sc(94), footY, rail - sc(102), footH, C_MUTED, g_font_small, DT_LEFT | DT_SINGLELINE | DT_VCENTER);

    /* Main area geometry */
    int x = ui.x;
    int w = ui.w;

    int alive = g_server_started && server_alive();

    /* Status pill */
    int pillW = sc(176);
    int pillH = sc(36);
    int pillX = r.right - pad - pillW;
    int pillY = sc(30);
    RECT pill = {pillX, pillY, pillX + pillW, pillY + pillH};
    draw_round(dc, pill, C_CARD, alive ? C_ACCENT : C_LINE_BRIGHT, pillH / 2);
    draw_dot(dc, pillX + sc(20), pillY + pillH / 2, sc(4), alive ? C_GREEN : C_DANGER, alive ? RGB(20, 60, 44) : RGB(60, 25, 25));
    draw_text_x(dc, alive ? L"ONLINE" : L"OFFLINE", pillX + sc(36), pillY, pillW - sc(46), pillH,
                alive ? C_GREEN : C_DANGER, g_font_mono_small, DT_LEFT | DT_SINGLELINE | DT_VCENTER);

    /* Picker card */
    draw_round(dc, ui.picker, C_CARD, C_LINE, sc(12));

    /* Metric cards */
    int gap = ui.metric_gap;
    int cardW = ui.metric_w;
    int mY = ui.metric_y;
    int mH = ui.metric_h;
    const WCHAR *labels[3] = {L"ENGINE", L"SERVER", L"CACHE"};
    for (int i = 0; i < 3; i++) {
        int cx = x + (cardW + gap) * i;
        RECT m = {cx, mY, cx + cardW, mY + mH};
        draw_round(dc, m, C_CARD, C_LINE, sc(12));
        /* Accent left bar */
        RECT bar = {cx + sc(1), mY + sc(14), cx + sc(4), mY + mH - sc(14)};
        HBRUSH bb = CreateSolidBrush(C_ACCENT);
        FillRect(dc, &bar, bb);
        DeleteObject(bb);
        draw_text_x(dc, labels[i], cx + sc(18), mY + sc(14), cardW - sc(30), sc(20), C_MUTED, g_font_mono_small, DT_LEFT | DT_SINGLELINE | DT_VCENTER);
    }

    /* Log card with header bar */
    int logCardTop = ui.log_top;
    RECT log_card = {x, logCardTop, x + w, ui.log_bottom};
    draw_round(dc, log_card, C_LOG, C_LINE, sc(12));

    /* Log header */
    int hdrY = logCardTop + sc(12);
    int hdrH = sc(24);
    draw_text_x(dc, L"ACTIVITY LOG", x + sc(18), hdrY, w - sc(140), hdrH, C_TEXT_DIM, g_font_mono_small, DT_LEFT | DT_SINGLELINE | DT_VCENTER);

    int liveCX = x + w - sc(74);
    int liveCY = hdrY + hdrH / 2;
    draw_dot(dc, liveCX, liveCY, sc(4), C_ACCENT, C_ACCENT_DEEP);
    draw_text_x(dc, L"LIVE", liveCX + sc(12), hdrY, sc(60), hdrH, C_ACCENT, g_font_mono_small, DT_LEFT | DT_SINGLELINE | DT_VCENTER);

    HPEN hp = CreatePen(PS_SOLID, 1, C_LINE);
    HGDIOBJ ohp = SelectObject(dc, hp);
    MoveToEx(dc, x + sc(18), hdrY + sc(30), NULL);
    LineTo(dc, x + w - sc(18), hdrY + sc(30));
    SelectObject(dc, ohp);
    DeleteObject(hp);

    /* Border around the path EDIT */
    int peX = ui.path_x;
    int peY = ui.path_y;
    int peH = ui.path_h;
    int peW = ui.path_w;
    RECT peBorder = {peX - sc(2), peY - sc(2), peX + peW + sc(2), peY + peH + sc(2)};
    draw_round(dc, peBorder, C_LOG, C_LINE_BRIGHT, sc(8));
}

void layout(HWND hwnd) {
    UiLayout ui = compute_layout(hwnd);
    int x = ui.x;
    int w = ui.w;

    /* Hero band */
    int hero_right = ui.hero_right;
    MoveWindow(g_title,    x, sc(18), w - hero_right, sc(44), TRUE);
    MoveWindow(g_subtitle, x, sc(66), w - hero_right, sc(22), TRUE);
    MoveWindow(g_status,   x, sc(94), w - hero_right, sc(22), TRUE);

    /* Picker card content */
    MoveWindow(g_path_label, ui.path_label_x, ui.path_label_y, ui.path_label_w, ui.path_label_h, TRUE);
    MoveWindow(g_path, ui.path_x, ui.path_y, ui.path_w, ui.path_h, TRUE);
    MoveWindow(GetDlgItem(hwnd, IDC_BROWSE), ui.browse_x, ui.path_y, ui.side_button_w, ui.path_h, TRUE);
    MoveWindow(GetDlgItem(hwnd, IDC_OPEN),   ui.open_x,   ui.path_y, ui.side_button_w, ui.path_h, TRUE);

    /* Action buttons row */
    int abY = ui.action_y;
    int abH = ui.action_h;
    MoveWindow(GetDlgItem(hwnd, IDC_START), x + sc(24),  abY, sc(176), abH, TRUE);
    MoveWindow(g_btn_server,                x + sc(216), abY, sc(156), abH, TRUE);
    MoveWindow(g_btn_api,                   x + sc(388), abY, sc(132), abH, TRUE);

    /* Metric cards: position values in the lower half of each card */
    int gap = ui.metric_gap;
    int cardW = ui.metric_w;
    int mValY = ui.metric_value_y;
    int mValH = ui.metric_value_h;
    MoveWindow(g_engine, x + sc(18),                  mValY, cardW - sc(30), mValH, TRUE);
    MoveWindow(g_server, x + cardW + gap + sc(18),    mValY, cardW - sc(30), mValH, TRUE);
    MoveWindow(g_cache,  x + (cardW + gap) * 2 + sc(18), mValY, cardW - sc(30), mValH, TRUE);

    /* Log content area inside log card (header is painted) */
    MoveWindow(g_log, x + sc(18), ui.log_edit_top, w - sc(36), ui.log_edit_h, TRUE);
}

void draw_button(const DRAWITEMSTRUCT *di) {
    WCHAR text[128];
    GetWindowTextW(di->hwndItem, text, 128);
    int primary = di->CtlID == IDC_START;
    int server_btn = di->CtlID == IDC_SERVER_TOGGLE;
    int pressed = di->itemState & ODS_SELECTED;
    int server_running = server_btn && g_server_started && server_alive();

    COLORREF fill, edge, fg;
    if (primary) {
        fill = pressed ? C_ACCENT_DARK : C_ACCENT;
        edge = pressed ? C_ACCENT_DARK : C_ACCENT;
        fg = RGB(7, 24, 28);
    } else if (server_running) {
        fill = pressed ? C_ACCENT_DEEP : RGB(20, 56, 48);
        edge = C_GREEN;
        fg = C_GREEN;
    } else {
        fill = pressed ? C_CARD_ELEV : C_CARD;
        edge = pressed ? C_LINE_BRIGHT : C_LINE;
        fg = C_TEXT;
    }

    HBRUSH b = CreateSolidBrush(fill);
    HPEN p = CreatePen(PS_SOLID, 1, edge);
    HGDIOBJ ob = SelectObject(di->hDC, b);
    HGDIOBJ op = SelectObject(di->hDC, p);
    RoundRect(di->hDC, di->rcItem.left, di->rcItem.top, di->rcItem.right, di->rcItem.bottom, 10, 10);
    SelectObject(di->hDC, ob);
    SelectObject(di->hDC, op);
    DeleteObject(b);
    DeleteObject(p);

    /* Subtle accent underline for primary */
    if (primary) {
        HPEN ul = CreatePen(PS_SOLID, 1, mix(C_ACCENT, C_ACCENT_DARK, 0.5f));
        HGDIOBJ oul = SelectObject(di->hDC, ul);
        int uy = di->rcItem.bottom - 5;
        MoveToEx(di->hDC, di->rcItem.left + 16, uy, NULL);
        LineTo(di->hDC, di->rcItem.right - 16, uy);
        SelectObject(di->hDC, oul);
        DeleteObject(ul);
    }

    SetBkMode(di->hDC, TRANSPARENT);
    SetTextColor(di->hDC, fg);
    SelectObject(di->hDC, g_font_body);
    RECT t = di->rcItem;
    DrawTextW(di->hDC, text, -1, &t, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
}

void browse_folder(void) {
    BROWSEINFOW bi;
    ZeroMemory(&bi, sizeof bi);
    bi.hwndOwner = g_main;
    bi.lpszTitle = L"选择游戏根目录";
    bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
    PIDLIST_ABSOLUTE pid = SHBrowseForFolderW(&bi);
    if (pid) {
        WCHAR p[MAX_PATH * 4];
        if (SHGetPathFromIDListW(pid, p)) {
            SetWindowTextW(g_path, p);
            save_last_game_dir(p);
            refresh_engine();
        }
        CoTaskMemFree(pid);
    }
}

void launch_game(const WCHAR *dir) {
    WCHAR exe[MAX_PATH * 4];
    if (!find_exe(dir, exe, MAX_PATH * 4)) {
        append_log(L"未找到游戏 exe。");
        return;
    }
    append_log(L"启动游戏：%s", exe);
    ShellExecuteW(g_main, L"open", exe, NULL, dir, SW_SHOWNORMAL);
}

typedef struct {
    WCHAR dir[MAX_PATH * 4];
    Engine engine;
} WarmupLaunchArgs;

/* warmup scans up to tens of MB of asset files and does synchronous HTTP, so it
   runs on a worker thread to keep the UI responsive. launch_game uses
   ShellExecuteW (COM), hence CoInitializeEx here. The UI helpers it calls
   (append_log / set_status) marshal to the UI thread via SendMessage. */
static DWORD WINAPI warmup_launch_thread(LPVOID p) {
    WarmupLaunchArgs *a = (WarmupLaunchArgs *)p;
    HRESULT hr = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    if (a->engine == ENGINE_RENPY) {
        /* The Ren'Py hook only does cache_only lookups and never waits on the
           live API, so the game can start right away while the whole-script
           prefetch queues behind it. Unity/XUnity keep import-before-launch:
           their plugins issue live lookups that should hit imported rows. */
        launch_game(a->dir);
        set_status(L"已启动 · 正在后台预热剧本...");
        warmup_translations(a->dir, a->engine);
    } else {
        warmup_translations(a->dir, a->engine);
        launch_game(a->dir);
    }
    set_status(L"已启动 · 本地缓存 + 实时批量 API");
    if (SUCCEEDED(hr)) CoUninitialize();
    free(a);
    return 0;
}

void start_translation(void) {
    GetWindowTextW(g_path, g_game, MAX_PATH * 4);
    if (!is_dir(g_game)) {
        MessageBoxW(g_main, L"请先选择游戏目录。", L"DeepSeek Game Translator", MB_ICONWARNING);
        return;
    }
    save_last_game_dir(g_game);
    Engine e = detect_engine(g_game);
    refresh_engine();
    append_log(L"选择目录：%s", g_game);
    append_log(L"识别引擎：%s", engine_name(e));
    set_status(L"正在启动服务并部署...");
    start_server();
    int deployed = 0;
    if (e == ENGINE_RENPY) deployed = deploy_renpy(g_game);
    else if (e == ENGINE_RPGM_MV) deployed = deploy_rpgm(g_game);
    else if (e == ENGINE_UNITY) deployed = deploy_unity(g_game);
    else if (e == ENGINE_UNITY_IL2CPP) deployed = deploy_unity_il2cpp(g_game);
    else if (e == ENGINE_RPGM_LEGACY) append_log(L"RPGM XP/VX：离线写入器仍待迁移，当前保留本地缓存服务。");
    else append_log(L"未知引擎：只启动服务端和游戏。");
    append_log(deployed ? L"部署完成。" : L"部署跳过或未完成。");

    /* Offload warmup (heavy I/O + sync HTTP) and game launch so the UI thread
       doesn't freeze. Pass a private copy of the path so a concurrent path-box
       edit (which rewrites g_game via refresh_engine) can't change it midway. */
    WarmupLaunchArgs *args = (WarmupLaunchArgs *)malloc(sizeof *args);
    if (args) {
        wcsncpy(args->dir, g_game, MAX_PATH * 4);
        args->dir[MAX_PATH * 4 - 1] = 0;
        args->engine = e;
        set_status(L"正在预热缓存并启动游戏...");
        HANDLE th = CreateThread(NULL, 0, warmup_launch_thread, args, 0, NULL);
        if (th) {
            CloseHandle(th);
            return;
        }
        free(args); /* CreateThread failed: fall back to the synchronous path */
    }
    warmup_translations(g_game, e);
    launch_game(g_game);
    set_status(L"已启动 · 本地缓存 + 实时批量 API");
}
