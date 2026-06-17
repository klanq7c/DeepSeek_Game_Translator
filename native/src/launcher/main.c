/* main.c — 启动器 WinMain 入口与窗口消息循环
 * 负责注册窗口类、创建主窗口、处理所有 UI 消息分发。
 * 关闭窗口时不终止翻译服务端，游戏仍可继续请求实时翻译。 */

#include "globals.h"
#include "api_config.h"
#include "fsutil.h"
#include "server_proc.h"
#include "ui.h"

#include <commctrl.h>
#include <objbase.h>
#include <shellapi.h>
#include <stdio.h>
#include <wchar.h>

/* 主窗口过程：处理创建、绘制、DPI 变化、命令分发、颜色主题、销毁等消息 */
static LRESULT CALLBACK wndproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    /* ---- WM_CREATE：初始化 DPI、字体、画刷、所有子控件 ---- */
    case WM_CREATE: {
        dpi_set_for_window(hwnd);
        g_font_title       = make_font(22, FW_SEMIBOLD, L"Microsoft YaHei UI");
        g_font_heading     = make_font(11, FW_SEMIBOLD, L"Microsoft YaHei UI");
        g_font_body        = make_font(10, FW_NORMAL,   L"Microsoft YaHei UI");
        g_font_small       = make_font(9,  FW_NORMAL,   L"Microsoft YaHei UI");
        g_font_mono        = make_font(10, FW_NORMAL,   L"Consolas");
        g_font_mono_small  = make_font(8,  FW_BOLD,     L"Consolas");

        g_brush_page      = CreateSolidBrush(C_PAGE);
        g_brush_edit      = CreateSolidBrush(C_LOG);
        g_brush_log       = CreateSolidBrush(C_LOG);
        g_brush_transparent = (HBRUSH)GetStockObject(HOLLOW_BRUSH);

        g_title    = CreateWindowW(L"STATIC", L"无感翻译控制台",        WS_CHILD | WS_VISIBLE, 0,0,0,0, hwnd, (HMENU)IDC_TITLE,    g_inst, NULL);
        g_subtitle = CreateWindowW(L"STATIC", L"C native - local cache + live batch API - tags / vars / color safe", WS_CHILD | WS_VISIBLE, 0,0,0,0, hwnd, (HMENU)IDC_SUBTITLE, g_inst, NULL);
        g_status   = CreateWindowW(L"STATIC", L"STATUS  ·  READY",       WS_CHILD | WS_VISIBLE, 0,0,0,0, hwnd, (HMENU)IDC_STATUS,   g_inst, NULL);

        g_path_label = CreateWindowW(L"STATIC", L"游戏目录",             WS_CHILD | WS_VISIBLE, 0,0,0,0, hwnd, (HMENU)IDC_PATH_LABEL, g_inst, NULL);
        g_path       = CreateWindowExW(0, L"EDIT", L"", WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL, 0,0,0,0, hwnd, (HMENU)IDC_PATH, g_inst, NULL);

        CreateWindowW(L"BUTTON", L"浏览",       WS_CHILD | WS_VISIBLE | BS_OWNERDRAW, 0,0,0,0, hwnd, (HMENU)IDC_BROWSE, g_inst, NULL);
        CreateWindowW(L"BUTTON", L"打开目录",   WS_CHILD | WS_VISIBLE | BS_OWNERDRAW, 0,0,0,0, hwnd, (HMENU)IDC_OPEN,   g_inst, NULL);
        CreateWindowW(L"BUTTON", L"⚡  开始汉化", WS_CHILD | WS_VISIBLE | BS_OWNERDRAW, 0,0,0,0, hwnd, (HMENU)IDC_START,  g_inst, NULL);
        g_btn_server = CreateWindowW(L"BUTTON", L"启动服务器",          WS_CHILD | WS_VISIBLE | BS_OWNERDRAW, 0,0,0,0, hwnd, (HMENU)IDC_SERVER_TOGGLE, g_inst, NULL);
        g_btn_api    = CreateWindowW(L"BUTTON", L"配置 API",            WS_CHILD | WS_VISIBLE | BS_OWNERDRAW, 0,0,0,0, hwnd, (HMENU)IDC_API_CONFIG,    g_inst, NULL);

        g_engine = CreateWindowW(L"STATIC", L"未选择", WS_CHILD | WS_VISIBLE, 0,0,0,0, hwnd, (HMENU)IDC_ENGINE, g_inst, NULL);
        g_server = CreateWindowW(L"STATIC", L"未启动", WS_CHILD | WS_VISIBLE, 0,0,0,0, hwnd, (HMENU)IDC_SERVER, g_inst, NULL);
        g_cache  = CreateWindowW(L"STATIC", L"检查中", WS_CHILD | WS_VISIBLE, 0,0,0,0, hwnd, (HMENU)IDC_CACHE,  g_inst, NULL);

        g_log = CreateWindowExW(0, L"LISTBOX", L"",
                                WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_HSCROLL |
                                LBS_NOINTEGRALHEIGHT | LBS_DISABLENOSCROLL,
                                0,0,0,0, hwnd, (HMENU)IDC_LOG, g_inst, NULL);

        apply_fonts();
        update_cache_card();
        append_log(L"原生启动器已就绪。");
        WCHAR last_game[MAX_PATH * 4];
        if (load_last_game_dir(last_game, MAX_PATH * 4)) {
            SetWindowTextW(g_path, last_game);
            refresh_engine();
            append_log(L"已恢复上次游戏目录：%s", last_game);
            append_log(L"直接点击 ⚡ 开始汉化即可。");
        } else {
            append_log(L"选择游戏目录后点击 ⚡ 开始汉化。");
        }
        layout(hwnd);
        return 0;
    }
    /* ---- WM_PAINT：绘制深色页面背景 ---- */
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        paint_background(hwnd, dc);
        EndPaint(hwnd, &ps);
        return 0;
    }
    /* ---- WM_SIZE：窗口尺寸变化时重新布局并重绘 ---- */
    case WM_SIZE:
        layout(hwnd);
        InvalidateRect(hwnd, NULL, TRUE);
        return 0;
    /* ---- WM_GETMINMAXINFO：限制窗口最小尺寸 ---- */
    case WM_GETMINMAXINFO: {
        MINMAXINFO *mmi = (MINMAXINFO *)lp;
        mmi->ptMinTrackSize.x = sc(1080);
        mmi->ptMinTrackSize.y = sc(700);
        return 0;
    }
    /* ---- WM_TIMER：路径编辑框内容变化防抖，200ms 后重新检测引擎 ---- */
    case WM_TIMER:
        if (wp == 1) {
            KillTimer(hwnd, 1);
            refresh_engine();
        }
        return 0;
    /* ---- WM_DPICHANGED：DPI 变化时重建字体、重新布局 ---- */
    case WM_DPICHANGED: {
        g_scale_dpi = HIWORD(wp);
        RECT *prc = (RECT *)lp;
        SetWindowPos(hwnd, NULL, prc->left, prc->top,
                     prc->right - prc->left, prc->bottom - prc->top,
                     SWP_NOZORDER | SWP_NOACTIVATE);
        HFONT old[6] = {g_font_title, g_font_heading, g_font_body, g_font_small, g_font_mono, g_font_mono_small};
        g_font_title       = make_font(22, FW_SEMIBOLD, L"Microsoft YaHei UI");
        g_font_heading     = make_font(11, FW_SEMIBOLD, L"Microsoft YaHei UI");
        g_font_body        = make_font(10, FW_NORMAL,   L"Microsoft YaHei UI");
        g_font_small       = make_font(9,  FW_NORMAL,   L"Microsoft YaHei UI");
        g_font_mono        = make_font(10, FW_NORMAL,   L"Consolas");
        g_font_mono_small  = make_font(8,  FW_BOLD,     L"Consolas");
        apply_fonts();
        for (int i = 0; i < 6; i++) DeleteObject(old[i]);
        InvalidateRect(hwnd, NULL, TRUE);
        return 0;
    }
    /* ---- WM_ERASEBKGND：禁止系统擦除背景（由 WM_PAINT 自绘） ---- */
    case WM_ERASEBKGND:
        return 1;
    /* ---- WM_DRAWITEM：自绘按钮 ---- */
    case WM_DRAWITEM:
        draw_button((const DRAWITEMSTRUCT *)lp);
        return TRUE;
    /* ---- WM_CTLCOLORSTATIC：静态文本颜色主题 ---- */
    case WM_CTLCOLORSTATIC: {
        HDC dc = (HDC)wp;
        HWND ctl = (HWND)lp;
        if (ctl == g_title || ctl == g_subtitle || ctl == g_status) {
            SetBkMode(dc, OPAQUE);
            SetBkColor(dc, C_PAGE);
            if (ctl == g_title) SetTextColor(dc, C_TEXT);
            else if (ctl == g_subtitle) SetTextColor(dc, C_TEXT_DIM);
            else SetTextColor(dc, C_MUTED);
            return (LRESULT)g_brush_page;
        }
        SetBkMode(dc, TRANSPARENT);
        if (ctl == g_path_label) SetTextColor(dc, C_MUTED);
        else if (ctl == g_engine || ctl == g_server || ctl == g_cache) SetTextColor(dc, C_TEXT);
        else SetTextColor(dc, C_TEXT);
        return (LRESULT)g_brush_transparent;
    }
    /* ---- WM_CTLCOLOREDIT：路径编辑框颜色主题 ---- */
    case WM_CTLCOLOREDIT: {
        HDC dc = (HDC)wp;
        SetTextColor(dc, C_TEXT);
        SetBkColor(dc, C_LOG);
        return (LRESULT)g_brush_edit;
    }
    /* ---- WM_CTLCOLORLISTBOX：日志列表框颜色主题 ---- */
    case WM_CTLCOLORLISTBOX: {
        HDC dc = (HDC)wp;
        HWND ctl = (HWND)lp;
        if (ctl == g_log) {
            SetTextColor(dc, C_LOG_TEXT);
            SetBkColor(dc, C_LOG);
            return (LRESULT)g_brush_log;
        }
        return DefWindowProcW(hwnd, msg, wp, lp);
    }
    /* ---- WM_COMMAND：按钮/菜单命令分发 ---- */
    case WM_COMMAND:
        if (LOWORD(wp) == IDC_BROWSE) browse_folder();
        else if (LOWORD(wp) == IDC_START) start_translation();
        else if (LOWORD(wp) == IDC_SERVER_TOGGLE) {
            toggle_server();
            InvalidateRect(hwnd, NULL, TRUE);
        }
        else if (LOWORD(wp) == IDC_API_CONFIG) show_api_config();
        else if (LOWORD(wp) == IDC_OPEN) {
            GetWindowTextW(g_path, g_game, MAX_PATH * 4);
            if (is_dir(g_game)) ShellExecuteW(hwnd, L"open", g_game, NULL, NULL, SW_SHOWNORMAL);
        } else if (LOWORD(wp) == IDC_PATH && HIWORD(wp) == EN_CHANGE) {
            /* Debounce: detect_engine scans the directory tree; coalesce bursts. */
            SetTimer(hwnd, 1, 200, NULL);
        }
        return 0;
    /* ---- WM_CLOSE / WM_DESTROY：关闭窗口并释放 GDI 资源 ---- */
    case WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        /* Keep the local translation server alive after the launcher window
           closes. Games continue to request live translations after launch;
           the explicit server toggle remains the user-controlled stop path. */
        DeleteObject(g_font_title);
        DeleteObject(g_font_heading);
        DeleteObject(g_font_body);
        DeleteObject(g_font_small);
        DeleteObject(g_font_mono);
        DeleteObject(g_font_mono_small);
        DeleteObject(g_brush_page);
        DeleteObject(g_brush_edit);
        DeleteObject(g_brush_log);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

/* wWinMain：程序入口
 * 初始化 DPI 感知 → 获取可执行文件路径 → 初始化 COM/公共控件 →
 * 注册窗口类 → 创建主窗口 → 进入消息循环 */
int WINAPI wWinMain(HINSTANCE h, HINSTANCE prev, PWSTR cmd, int show) {
    (void)prev;
    (void)cmd;
    g_inst = h;
    dpi_enable_awareness();
    GetModuleFileNameW(NULL, g_root, MAX_PATH * 4);
    WCHAR *slash = wcsrchr(g_root, L'\\');
    if (slash) *slash = 0;

    INITCOMMONCONTROLSEX ic = { sizeof(ic), ICC_STANDARD_CLASSES };
    InitCommonControlsEx(&ic);
    CoInitialize(NULL);

    /* Use primary monitor DPI to size initial window (window may move to a
       different monitor afterwards; WM_DPICHANGED handles that). */
    int primary_dpi = 96;
    {
        HDC sdc = GetDC(NULL);
        primary_dpi = GetDeviceCaps(sdc, LOGPIXELSY);
        ReleaseDC(NULL, sdc);
        if (primary_dpi <= 0) primary_dpi = 96;
    }

    WNDCLASSW wc;
    ZeroMemory(&wc, sizeof wc);
    wc.lpfnWndProc = wndproc;
    wc.hInstance = h;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = NULL;
    wc.lpszClassName = L"DSTNativeLauncher";
    RegisterClassW(&wc);

    int initW = MulDiv(1200, primary_dpi, 96);
    int initH = MulDiv(780,  primary_dpi, 96);
    g_main = CreateWindowW(wc.lpszClassName, L"DeepSeek Game Translator", WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                           CW_USEDEFAULT, CW_USEDEFAULT, initW, initH, NULL, NULL, h, NULL);
    ShowWindow(g_main, show);
    UpdateWindow(g_main);

    MSG m;
    while (GetMessageW(&m, NULL, 0, 0)) {
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
    CoUninitialize();
    return (int)m.wParam;
}
