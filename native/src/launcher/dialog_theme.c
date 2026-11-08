#include "dialog_theme.h"
#include "fsutil.h"
#include "ui.h"
#include "resource.h"
#include <commctrl.h>
#include <stdlib.h>

int dialog_sc(const DialogTheme *t, int value) { return MulDiv(value, t->dpi, 96); }
int dialog_fit_dpi(HWND owner, int width, int height, int dpi) {
    MONITORINFO mi = {0}; mi.cbSize = sizeof mi;
    if (!GetMonitorInfoW(MonitorFromWindow(owner, MONITOR_DEFAULTTONEAREST), &mi)) {
        append_log(L"二级窗口：无法计算可用尺寸（Windows 错误 %lu）", GetLastError()); return dpi;
    }
    int fit = min(MulDiv(mi.rcWork.right - mi.rcWork.left - 32, 96, width),
                  MulDiv(mi.rcWork.bottom - mi.rcWork.top - 64, 96, height));
    return min(dpi, max(72, fit));
}
static HFONT dialog_font(int dpi, int pt, int weight, const WCHAR *face) {
    return CreateFontW(-MulDiv(pt, dpi, 72), 0, 0, 0, weight, 0, 0, 0, DEFAULT_CHARSET,
                      OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, face);
}
void dialog_theme_init(DialogTheme *t, int dpi) {
    t->dpi = dpi;
    t->title = dialog_font(dpi, 22, FW_NORMAL, L"SimSun");
    t->body = dialog_font(dpi, 10, FW_NORMAL, L"Microsoft YaHei UI");
    t->small = dialog_font(dpi, 9, FW_NORMAL, L"Microsoft YaHei UI");
    t->card = CreateSolidBrush(C_CARD); t->field = CreateSolidBrush(C_LOG);
}
void dialog_theme_free(DialogTheme *t) {
    DeleteObject(t->title); DeleteObject(t->body); DeleteObject(t->small);
    DeleteObject(t->card); DeleteObject(t->field);
}
void dialog_place(HWND hwnd, HWND owner, int width, int height, int dpi) {
    RECT r = {0, 0, MulDiv(width, dpi, 96), MulDiv(height, dpi, 96)}, anchor;
    /* 客户区按实际 DPI 计算，边框尺寸从已创建窗口读取，兼容旧版 Windows。 */
    RECT outer, client; GetWindowRect(hwnd, &outer); GetClientRect(hwnd, &client);
    int w = r.right + outer.right - outer.left - client.right;
    int h = r.bottom + outer.bottom - outer.top - client.bottom;
    MONITORINFO mi = {0}; mi.cbSize = sizeof mi;
    if (!GetMonitorInfoW(MonitorFromWindow(owner ? owner : hwnd, MONITOR_DEFAULTTONEAREST), &mi)) {
        append_log(L"二级窗口：无法读取显示器工作区（Windows 错误 %lu）", GetLastError()); return;
    }
    anchor = mi.rcWork;
    if (owner) GetWindowRect(owner, &anchor);
    int x = anchor.left + (anchor.right - anchor.left - w) / 2;
    int y = anchor.top + (anchor.bottom - anchor.top - h) / 2;
    x = max(mi.rcWork.left, min(x, mi.rcWork.right - w));
    y = max(mi.rcWork.top, min(y, mi.rcWork.bottom - h));
    SetWindowPos(hwnd, NULL, x, y, w, h, SWP_NOZORDER | SWP_NOACTIVATE);
}
static LRESULT CALLBACK dialog_hover(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR id, DWORD_PTR data) {
    (void)data;
    if (msg == WM_MOUSEMOVE && !GetPropW(hwnd, L"DstDialogHot")) {
        TRACKMOUSEEVENT track = {sizeof track, TME_LEAVE, hwnd, 0};
        TrackMouseEvent(&track); SetPropW(hwnd, L"DstDialogHot", (HANDLE)1); InvalidateRect(hwnd, NULL, FALSE);
    } else if (msg == WM_MOUSELEAVE) {
        RemovePropW(hwnd, L"DstDialogHot"); InvalidateRect(hwnd, NULL, FALSE);
    } else if (msg == WM_SETFOCUS || msg == WM_KILLFOCUS) {
        InvalidateRect(GetParent(hwnd), NULL, FALSE);
    } else if (msg == WM_NCDESTROY) {
        RemovePropW(hwnd, L"DstDialogHot"); RemoveWindowSubclass(hwnd, dialog_hover, id);
    }
    return DefSubclassProc(hwnd, msg, wp, lp);
}
HWND dialog_control(HWND hwnd, DialogTheme *t, const WCHAR *cls, const WCHAR *text, DWORD style,
                    int id, int x, int y, int width, int height) {
    HWND ctl = CreateWindowExW(0, cls, text, WS_CHILD | WS_VISIBLE | style,
        dialog_sc(t, x), dialog_sc(t, y), dialog_sc(t, width), dialog_sc(t, height), hwnd, (HMENU)(INT_PTR)id, g_inst, NULL);
    SendMessageW(ctl, WM_SETFONT, (WPARAM)t->body, FALSE);
    if (!SetWindowSubclass(ctl, dialog_hover, 1, 0))
        append_log(L"二级窗口：无法安装控件焦点与悬停跟踪（控件 %d，Windows 错误 %lu）", id, GetLastError());
    return ctl;
}
void dialog_round(HDC dc, RECT r, COLORREF fill, COLORREF line, int radius) {
    HBRUSH b = CreateSolidBrush(fill); HPEN p = CreatePen(PS_SOLID, 1, line);
    HGDIOBJ ob = SelectObject(dc, b), op = SelectObject(dc, p);
    RoundRect(dc, r.left, r.top, r.right, r.bottom, radius, radius);
    SelectObject(dc, ob); SelectObject(dc, op); DeleteObject(b); DeleteObject(p);
}
static void dialog_text(HDC dc, DialogTheme *t, const WCHAR *text, int x, int y, int w, int h, HFONT font, COLORREF color) {
    RECT r = {dialog_sc(t, x), dialog_sc(t, y), dialog_sc(t, x + w), dialog_sc(t, y + h)};
    SelectObject(dc, font); SetTextColor(dc, color); DrawTextW(dc, text, -1, &r, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
}
void dialog_paint(HWND hwnd, HDC dc, DialogTheme *t, const WCHAR *title, const WCHAR *subtitle) {
    RECT r; GetClientRect(hwnd, &r); HBRUSH bg = CreateSolidBrush(C_CARD_ELEV); FillRect(dc, &r, bg); DeleteObject(bg);
    RECT top = {0, 0, r.right, dialog_sc(t, 152)}; bg = CreateSolidBrush(C_PAGE); FillRect(dc, &top, bg); DeleteObject(bg);
    SetBkMode(dc, TRANSPARENT);
    int width = MulDiv(r.right, 96, t->dpi);
    dialog_text(dc, t, L"游 戏 无 界  /  PLAY BEYOND WORDS", 28, 16, width - 56, 18, t->small, C_ACCENT);
    dialog_text(dc, t, title, 28, 42, width - 108, 34, t->title, C_CARD);
    dialog_text(dc, t, subtitle, 28, 84, width - 56, 20, t->small, RGB(174, 190, 207));
    /* 与主页面相同的月亮和星点；表单保持静态，避免输入时周期性重绘。 */
    HGDIOBJ ob = SelectObject(dc, t->card), op = SelectObject(dc, GetStockObject(NULL_PEN));
    bg = CreateSolidBrush(C_ACCENT); SelectObject(dc, bg);
    Ellipse(dc, r.right - dialog_sc(t, 64), dialog_sc(t, 43), r.right - dialog_sc(t, 36), dialog_sc(t, 71));
    HBRUSH cut = CreateSolidBrush(C_PAGE); SelectObject(dc, cut);
    Ellipse(dc, r.right - dialog_sc(t, 56), dialog_sc(t, 37), r.right - dialog_sc(t, 28), dialog_sc(t, 65));
    SelectObject(dc, ob); SelectObject(dc, op); DeleteObject(bg); DeleteObject(cut);
    RECT card = {dialog_sc(t, 20), dialog_sc(t, 120), r.right - dialog_sc(t, 20), r.bottom - dialog_sc(t, 76)};
    dialog_round(dc, card, C_CARD, C_LINE, dialog_sc(t, 18));
}
void dialog_button(const DRAWITEMSTRUCT *di, DialogTheme *t, int primary) {
    int enabled = IsWindowEnabled(di->hwndItem), hot = GetPropW(di->hwndItem, L"DstDialogHot") != NULL;
    COLORREF fill = primary ? C_ACCENT : C_CARD, line = primary ? C_ACCENT_DARK : C_LINE;
    if (enabled && (hot || (di->itemState & ODS_SELECTED))) { fill = primary ? C_ACCENT_DARK : C_CARD_ELEV; line = C_ACCENT_DEEP; }
    if (!enabled) fill = C_CARD_ELEV;
    FillRect(di->hDC, &di->rcItem, t->card);
    dialog_round(di->hDC, di->rcItem, fill, line, dialog_sc(t, 16));
    WCHAR text[128]; GetWindowTextW(di->hwndItem, text, 128); RECT r = di->rcItem;
    SetBkMode(di->hDC, TRANSPARENT); SetTextColor(di->hDC, enabled ? C_TEXT : C_MUTED); SelectObject(di->hDC, t->body);
    DrawTextW(di->hDC, text, -1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    if (di->itemState & ODS_FOCUS) { InflateRect(&r, -dialog_sc(t, 4), -dialog_sc(t, 4)); DrawFocusRect(di->hDC, &r); }
}
LRESULT dialog_color(HDC dc, DialogTheme *t, int field) {
    SetTextColor(dc, field ? C_TEXT : C_TEXT_DIM); SetBkColor(dc, field ? C_LOG : C_CARD);
    return (LRESULT)(field ? t->field : t->card);
}
void dialog_combo(const DRAWITEMSTRUCT *di, DialogTheme *t, const WCHAR *label) {
    int selected = (di->itemState & ODS_SELECTED) && !(di->itemState & ODS_COMBOBOXEDIT);
    HBRUSH bg = CreateSolidBrush(selected ? C_ACCENT : C_LOG); FillRect(di->hDC, &di->rcItem, bg); DeleteObject(bg);
    if (di->itemID == (UINT)-1) return;
    int length = (int)SendMessageW(di->hwndItem, CB_GETLBTEXTLEN, di->itemID, 0);
    if (length < 0 || length >= 1024) { append_log(L"二级窗口：下拉项长度无效（%d）", length); return; }
    WCHAR text[1024]; SendMessageW(di->hwndItem, CB_GETLBTEXT, di->itemID, (LPARAM)text);
    RECT r = di->rcItem; r.left += dialog_sc(t, 8); r.right -= dialog_sc(t, 4);
    SelectObject(di->hDC, t->body); SetBkMode(di->hDC, TRANSPARENT); SetTextColor(di->hDC, C_TEXT);
    DrawTextW(di->hDC, label ? label : text, -1, &r, DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX);
    if (di->itemState & ODS_FOCUS) DrawFocusRect(di->hDC, &di->rcItem);
}
int dialog_key(HWND hwnd, MSG *m, int accept, int cancel) {
    if (m->message == WM_KEYDOWN && (m->wParam == VK_ESCAPE || m->wParam == VK_RETURN)) {
        HWND focus = GetFocus(), combo = focus; WCHAR cls[32]; GetClassNameW(combo, cls, 32);
        if (lstrcmpiW(cls, L"Edit") == 0) combo = GetParent(focus);
        GetClassNameW(combo, cls, 32);
        /* 下拉展开时先交给系统确认/关闭列表，不能误触保存或退出整个表单。 */
        if (lstrcmpiW(cls, L"ComboBox") == 0 && SendMessageW(combo, CB_GETDROPPEDSTATE, 0, 0)) return 0;
        int id = m->wParam == VK_ESCAPE ? cancel : accept;
        GetClassNameW(focus, cls, 32);
        if (m->wParam == VK_RETURN && lstrcmpiW(cls, L"Button") == 0) id = GetDlgCtrlID(focus);
        SendMessageW(hwnd, WM_COMMAND, MAKEWPARAM(id, BN_CLICKED), (LPARAM)GetDlgItem(hwnd, id)); return 1;
    }
    return IsDialogMessageW(hwnd, m);
}

typedef struct { DialogTheme theme; const WCHAR *text, *title; UINT flags; int result, done, height; } MessageDialog;
static void message_layout(HWND hwnd, MessageDialog *d) {
    DialogTheme *t = &d->theme;
    const int ids[] = {300, IDNO, (d->flags & MB_TYPEMASK) == MB_YESNO ? IDYES : IDOK};
    const int xs[] = {44, 280, 404}, ys[] = {144, d->height - 56, d->height - 56};
    const int ws[] = {472, 112, 112}, hs[] = {d->height - 236, 36, 36};
    for (int i = 0; i < 3; i++) {
        HWND ctl = GetDlgItem(hwnd, ids[i]); if (!ctl) continue;
        SendMessageW(ctl, WM_SETFONT, (WPARAM)t->body, FALSE);
        SetWindowPos(ctl, NULL, dialog_sc(t, xs[i]), dialog_sc(t, ys[i]), dialog_sc(t, ws[i]), dialog_sc(t, hs[i]), SWP_NOZORDER | SWP_NOACTIVATE);
    }
}
static LRESULT CALLBACK message_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    MessageDialog *d = (MessageDialog *)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
    if (msg == WM_CREATE) {
        d = (MessageDialog *)((CREATESTRUCTW *)lp)->lpCreateParams; SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)d);
        dialog_control(hwnd, &d->theme, L"STATIC", d->text, SS_NOPREFIX, 300, 44, 144, 472, d->height - 236);
        int yesno = (d->flags & MB_TYPEMASK) == MB_YESNO;
        if (yesno) dialog_control(hwnd, &d->theme, L"BUTTON", L"取消", BS_OWNERDRAW | WS_TABSTOP, IDNO, 280, d->height - 56, 112, 36);
        dialog_control(hwnd, &d->theme, L"BUTTON", yesno ? L"确认" : L"知道了", BS_OWNERDRAW | WS_TABSTOP,
                       yesno ? IDYES : IDOK, 404, d->height - 56, 112, 36);
        return 0;
    }
    if (!d) return DefWindowProcW(hwnd, msg, wp, lp);
    switch (msg) {
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: { PAINTSTRUCT ps; HDC dc = BeginPaint(hwnd, &ps); dialog_paint(hwnd, dc, &d->theme, d->title,
        (d->flags & MB_TYPEMASK) == MB_YESNO ? L"请确认后继续" : L"来自翻译器的提示"); EndPaint(hwnd, &ps); return 0; }
    case WM_PRINTCLIENT: dialog_paint(hwnd, (HDC)wp, &d->theme, d->title,
        (d->flags & MB_TYPEMASK) == MB_YESNO ? L"请确认后继续" : L"来自翻译器的提示"); return 0;
    case WM_CTLCOLORSTATIC: return dialog_color((HDC)wp, &d->theme, 0);
    case WM_DRAWITEM: dialog_button((DRAWITEMSTRUCT *)lp, &d->theme, ((DRAWITEMSTRUCT *)lp)->CtlID != IDNO); return TRUE;
    case WM_DPICHANGED: {
        RECT *r = (RECT *)lp; SetWindowPos(hwnd, NULL, r->left, r->top, r->right-r->left, r->bottom-r->top, SWP_NOZORDER | SWP_NOACTIVATE);
        int dpi = dialog_fit_dpi(hwnd, 560, d->height, HIWORD(wp));
        dialog_theme_free(&d->theme); dialog_theme_init(&d->theme, dpi); message_layout(hwnd, d);
        dialog_place(hwnd, hwnd, 560, d->height, dpi); InvalidateRect(hwnd, NULL, TRUE); return 0;
    }
    case WM_COMMAND:
        if (LOWORD(wp) == IDOK || LOWORD(wp) == IDYES || LOWORD(wp) == IDNO || LOWORD(wp) == IDCANCEL) {
            d->result = LOWORD(wp) == IDCANCEL ? ((d->flags & MB_TYPEMASK) == MB_YESNO ? IDNO : IDOK) : LOWORD(wp);
            d->done = 1; DestroyWindow(hwnd); return 0;
        } break;
    case WM_CLOSE: d->result = (d->flags & MB_TYPEMASK) == MB_YESNO ? IDNO : IDOK; d->done = 1; DestroyWindow(hwnd); return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}
static int themed_message_impl(HWND owner, const WCHAR *text, const WCHAR *title, UINT flags, int probe, int dpi) {
    static int registered;
    if (!registered) {
        WNDCLASSEXW wc = {0}; wc.cbSize = sizeof wc; wc.lpfnWndProc = message_proc; wc.hInstance = g_inst;
        wc.hCursor = LoadCursor(NULL, IDC_ARROW); wc.lpszClassName = L"DstThemedMessage";
        if (!RegisterClassExW(&wc)) { append_log(L"无法注册提示窗口（Windows 错误 %lu）", GetLastError()); return 0; }
        registered = 1;
    }
    MessageDialog d = {0}; d.text = text; d.title = title; d.flags = flags;
    dialog_theme_init(&d.theme, dpi);
    HDC dc = GetDC(owner); HGDIOBJ old = SelectObject(dc, d.theme.body);
    RECT measure = {0, 0, dialog_sc(&d.theme, 472), 0}; DrawTextW(dc, text, -1, &measure, DT_CALCRECT | DT_WORDBREAK | DT_NOPREFIX);
    SelectObject(dc, old); ReleaseDC(owner, dc); d.height = max(286, MulDiv(measure.bottom, 96, d.theme.dpi) + 248);
    if (!probe) {
        int fit = dialog_fit_dpi(owner, 560, d.height, dpi);
        if (fit != dpi) { dialog_theme_free(&d.theme); dialog_theme_init(&d.theme, fit); }
    }
    HWND dlg = CreateWindowExW(WS_EX_DLGMODALFRAME, L"DstThemedMessage", title, WS_CAPTION | WS_SYSMENU | WS_CLIPCHILDREN,
        CW_USEDEFAULT, CW_USEDEFAULT, 560, d.height, owner, NULL, g_inst, &d);
    if (!dlg) { append_log(L"无法创建提示窗口（Windows 错误 %lu）", GetLastError()); dialog_theme_free(&d.theme); return 0; }
    int enabled = owner && IsWindowEnabled(owner); HWND focus = GetFocus(); if (enabled) EnableWindow(owner, FALSE);
    dialog_place(dlg, owner, 560, d.height, d.theme.dpi); apply_window_chrome(dlg);
    int accept = (flags & MB_TYPEMASK) == MB_YESNO ? ((flags & MB_DEFBUTTON2) ? IDNO : IDYES) : IDOK;
    if (probe) {
        WCHAR bmp[MAX_PATH * 4];
        DWORD chars = GetEnvironmentVariableW(L"DST_API_UI_BMP", bmp, MAX_PATH * 4 - 16);
        if (probe == 1 && chars) {
            if (chars < MAX_PATH * 4 - 16) wcscat(bmp, L".message.bmp");
            write_stdout_utf8(chars < MAX_PATH * 4 - 16 && dialog_render_probe(dlg, bmp) ? L"message_render=1\n" : L"message_render=0\n");
        }
        MSG key = {0}; key.hwnd = dlg; key.message = WM_KEYDOWN; key.wParam = probe == 2 ? VK_ESCAPE : VK_RETURN;
        if (probe == 3) SendMessageW(dlg, WM_COMMAND, IDYES, 0);
        else if (probe == 4) SendMessageW(dlg, WM_CLOSE, 0, 0);
        else dialog_key(dlg, &key, accept, IDCANCEL);
    } else { ShowWindow(dlg, SW_SHOW); SetFocus(GetDlgItem(dlg, accept)); }
    MSG m;
    while (!d.done) {
        int got = GetMessageW(&m, NULL, 0, 0);
        if (got <= 0) { if (!got) PostQuitMessage((int)m.wParam); else append_log(L"提示窗口消息循环失败（Windows 错误 %lu）", GetLastError()); break; }
        if (!dialog_key(dlg, &m, accept, IDCANCEL)) { TranslateMessage(&m); DispatchMessageW(&m); }
    }
    if (IsWindow(dlg)) DestroyWindow(dlg);
    if (enabled) { EnableWindow(owner, TRUE); SetForegroundWindow(owner); if (IsWindow(focus)) SetFocus(focus); }
    dialog_theme_free(&d.theme); return d.result;
}
int themed_message(HWND owner, const WCHAR *text, const WCHAR *title, UINT flags) {
    return themed_message_impl(owner, text, title, flags, 0, g_scale_dpi);
}
void themed_message_probe(int dpi) {
    const WCHAR *text = L"这将清除共享翻译缓存。\n\n游戏文件不会改变。是否继续？";
    const WCHAR *names[] = {L"message_enter=", L"message_escape=", L"message_yes=", L"message_close=", L"message_info="};
    for (int i = 1; i <= 5; i++) {
        int r = themed_message_impl(NULL, text, L"清除缓存", i == 5 ? MB_ICONINFORMATION : MB_ICONWARNING | MB_YESNO | MB_DEFBUTTON2, i, dpi);
        write_stdout_utf8(names[i-1]); write_stdout_utf8(r == (i == 3 ? IDYES : i == 5 ? IDOK : IDNO) ? L"1\n" : L"0\n");
    }
}

/* 只供隔离配置目录的 UI 探针使用；真实控件通过 WM_PRINT 合成，避免读取桌面内容。 */
int dialog_render_probe(HWND hwnd, const WCHAR *path) {
    RECT r; GetClientRect(hwnd, &r);
    BITMAPINFO bi = {0}; bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER); bi.bmiHeader.biWidth = r.right;
    bi.bmiHeader.biHeight = -r.bottom; bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32;
    HDC screen = GetDC(NULL), dc = CreateCompatibleDC(screen); void *pixels = NULL;
    HBITMAP bmp = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &pixels, NULL, 0); ReleaseDC(NULL, screen);
    if (!dc || !bmp || !pixels) { append_log(L"二级窗口探针：GDI 分配失败（Windows 错误 %lu）", GetLastError()); if (bmp) DeleteObject(bmp); if (dc) DeleteDC(dc); return 0; }
    HGDIOBJ old = SelectObject(dc, bmp);
    SendMessageW(hwnd, WM_PRINTCLIENT, (WPARAM)dc, PRF_CLIENT);
    for (HWND ctl = GetWindow(hwnd, GW_CHILD); ctl; ctl = GetWindow(ctl, GW_HWNDNEXT)) {
        RECT cr; GetWindowRect(ctl, &cr); MapWindowPoints(NULL, hwnd, (POINT *)&cr, 2);
        int saved = SaveDC(dc); IntersectClipRect(dc, cr.left, cr.top, cr.right, cr.bottom);
        SetViewportOrgEx(dc, cr.left, cr.top, NULL);
        SendMessageW(ctl, WM_PRINT, (WPARAM)dc, PRF_CLIENT | PRF_NONCLIENT | PRF_CHILDREN | PRF_ERASEBKGND);
        RestoreDC(dc, saved);
    }
    GdiFlush();
    DWORD bytes = r.right * r.bottom * 4, written;
    BITMAPFILEHEADER fh = {0}; fh.bfType = 0x4d42; fh.bfOffBits = 54; fh.bfSize = 54 + bytes; bi.bmiHeader.biSizeImage = bytes;
    HANDLE file = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    int ok = file != INVALID_HANDLE_VALUE;
    if (ok) {
        ok = WriteFile(file, &fh, sizeof fh, &written, NULL) && written == sizeof fh &&
             WriteFile(file, &bi.bmiHeader, 40, &written, NULL) && written == 40 &&
             WriteFile(file, pixels, bytes, &written, NULL) && written == bytes;
        CloseHandle(file);
    }
    if (!ok) append_log(L"二级窗口探针：位图写入失败（Windows 错误 %lu）", GetLastError());
    SelectObject(dc, old); DeleteObject(bmp); DeleteDC(dc); return ok;
}
