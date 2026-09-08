/* ui_probe.c — --ui-probe-and-exit：把主窗口客户区渲染成确定的一帧
 * ----------------------------------------------------------------
 * 语言迁移阶段 3 的 UI 比对手段。窗口层无法像 detect/deploy/warmup 那样比对
 * 文本输出，所以这里把"画出来的东西"本身变成可比对的产物：
 *
 *   1. 固定 DPI 96、冻结动画时钟、中性化两处运行时标识、给定服务器状态，
 *      消除同一台机器上两次渲染之间的全部可变量；
 *   2. 创建一个不可见的 WS_POPUP 主窗口，客户区精确为 <w>×<h>；
 *   3. 先 paint_background 画父窗口，再按 z 序把每个子控件画进同一张位图
 *      （自绘按钮合成 DRAWITEMSTRUCT 走 draw_button，标准控件走 WM_PRINTCLIENT）；
 *   4. 位图存成 32bpp 无压缩 BMP，同时把 compute_layout 的每个字段和每个控件的
 *      类名/ID/矩形/文本打印到 stdout。
 *
 * tests\launcher_parity 对 BMP 取 SHA-256、对 stdout 逐行比对，C 与 C# 必须完全
 * 一致。冻结时钟取 0 是有意的：sinf(-pi/2) 与 cosf(0) 在此处落在精确可表示的
 * -1.0f / 1.0f 上，避免两套 libm 的最后一位差异被放大成像素差异。
 */

#include "globals.h"
#include "fsutil.h"
#include "ui.h"

#include <commctrl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <wchar.h>

/* 探针渲染的控件清单，顺序即 WM_CREATE 的创建顺序（也就是 z 序）。 */
typedef struct ProbeControl {
    int id;
    const WCHAR *name;
    int owner_draw;
} ProbeControl;

static const ProbeControl g_probe_controls[] = {
    { IDC_TITLE,         L"title",       0 },
    { IDC_SUBTITLE,      L"subtitle",    0 },
    { IDC_STATUS,        L"status",      0 },
    { IDC_PATH_LABEL,    L"path_label",  0 },
    { IDC_PATH,          L"path",        0 },
    { IDC_BROWSE,        L"browse",      1 },
    { IDC_OPEN,          L"open",        1 },
    { IDC_START,         L"start",       1 },
    { IDC_RESTORE,       L"restore",     1 },
    { IDC_SERVER_TOGGLE, L"server",      1 },
    { IDC_API_CONFIG,    L"api",         1 },
    { IDC_ENGINE,        L"engine",      0 },
    { IDC_SERVER,        L"server_val",  0 },
    { IDC_CACHE,         L"cache",       0 },
    { IDC_CLEAR_CACHE,   L"clear_cache", 1 },
    { IDC_LOG,           L"log",         0 },
};
#define PROBE_CONTROL_COUNT (sizeof(g_probe_controls) / sizeof(g_probe_controls[0]))

static void probe_line(const WCHAR *fmt, ...) {
    WCHAR buf[2048];
    va_list ap;
    va_start(ap, fmt);
    _vsnwprintf(buf, 2048, fmt, ap);
    va_end(ap);
    buf[2047] = 0;
    write_stdout_utf8(buf);
}

/* 32bpp 无压缩、自上而下的 BMP。像素直接来自 DIB section，无调色板与行填充歧义。 */
static int write_bmp32(const WCHAR *path, const void *pixels, int width, int height) {
    DWORD image_bytes = (DWORD)width * (DWORD)height * 4u;
    BITMAPFILEHEADER fh;
    BITMAPINFOHEADER ih;
    ZeroMemory(&fh, sizeof fh);
    ZeroMemory(&ih, sizeof ih);
    fh.bfType = 0x4D42; /* 'BM' */
    fh.bfOffBits = sizeof fh + sizeof ih;
    fh.bfSize = fh.bfOffBits + image_bytes;
    ih.biSize = sizeof ih;
    ih.biWidth = width;
    ih.biHeight = -height;  /* 负高度 = 自上而下，与 DIB section 的存储顺序一致 */
    ih.biPlanes = 1;
    ih.biBitCount = 32;
    ih.biCompression = BI_RGB;
    ih.biSizeImage = image_bytes;

    HANDLE f = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE) {
        probe_line(L"bmp_error=%lu\n", GetLastError());
        return 0;
    }
    DWORD written = 0;
    int ok = WriteFile(f, &fh, sizeof fh, &written, NULL) && written == sizeof fh;
    ok = ok && WriteFile(f, &ih, sizeof ih, &written, NULL) && written == sizeof ih;
    ok = ok && WriteFile(f, pixels, image_bytes, &written, NULL) && written == image_bytes;
    if (!ok) probe_line(L"bmp_error=%lu\n", GetLastError());
    CloseHandle(f);
    return ok;
}

/* 把一个子控件画进父窗口的位图：视口原点移到控件左上角，再让它自己打印。 */
static void print_child(HWND hwnd, HDC dc, const ProbeControl *pc, RECT rc) {
    HWND ctl = GetDlgItem(hwnd, pc->id);
    if (!ctl || !IsWindow(ctl)) return;

    int saved = SaveDC(dc);
    IntersectClipRect(dc, rc.left, rc.top, rc.right, rc.bottom);
    if (pc->owner_draw) {
        /* BS_OWNERDRAW 按钮平时由父窗口的 WM_DRAWITEM 绘制，这里直接合成同样的
           结构体：rcItem 用客户区坐标，hDC 就是位图 DC，draw_button 内部只用这两项。 */
        DRAWITEMSTRUCT di;
        ZeroMemory(&di, sizeof di);
        di.CtlType = ODT_BUTTON;
        di.CtlID = (UINT)pc->id;
        di.itemAction = ODA_DRAWENTIRE;
        di.itemState = 0;
        di.hwndItem = ctl;
        di.hDC = dc;
        di.rcItem = rc;
        draw_button(&di);
    } else {
        SetViewportOrgEx(dc, rc.left, rc.top, NULL);
        SendMessageW(ctl, WM_PRINTCLIENT, (WPARAM)dc, PRF_CLIENT);
    }
    RestoreDC(dc, saved);
}

/* 渲染并输出报告。窗口已创建且客户区尺寸已确定。 */
static int probe_render(HWND hwnd, const WCHAR *bmp_path) {
    RECT client;
    GetClientRect(hwnd, &client);
    int width = client.right;
    int height = client.bottom;
    if (width <= 0 || height <= 0) {
        probe_line(L"result=0\n");
        return 0;
    }

    BITMAPINFO bi;
    ZeroMemory(&bi, sizeof bi);
    bi.bmiHeader.biSize = sizeof bi.bmiHeader;
    bi.bmiHeader.biWidth = width;
    bi.bmiHeader.biHeight = -height;   /* 自上而下 */
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;

    HDC screen = GetDC(NULL);
    HDC dc = CreateCompatibleDC(screen);
    void *pixels = NULL;
    HBITMAP bmp = dc ? CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &pixels, NULL, 0) : NULL;
    ReleaseDC(NULL, screen);
    if (!dc || !bmp || !pixels) {
        /* GDI 分配失败归操作系统所有，无法在上游修复；如实报告并按失败退出，
           不能退化成"渲染成功但内容为空"的假绿。 */
        probe_line(L"gdi_error=%lu\nresult=0\n", GetLastError());
        if (bmp) DeleteObject(bmp);
        if (dc) DeleteDC(dc);
        return 0;
    }
    HGDIOBJ old_bmp = SelectObject(dc, bmp);

    paint_background(hwnd, dc);

    for (size_t i = 0; i < PROBE_CONTROL_COUNT; i++) {
        HWND ctl = GetDlgItem(hwnd, g_probe_controls[i].id);
        if (!ctl || !IsWindow(ctl)) continue;
        RECT rc;
        GetWindowRect(ctl, &rc);
        MapWindowPoints(NULL, hwnd, (POINT *)&rc, 2);
        print_child(hwnd, dc, &g_probe_controls[i], rc);
    }

    GdiFlush();
    int ok = write_bmp32(bmp_path, pixels, width, height);

    SelectObject(dc, old_bmp);
    DeleteObject(bmp);
    DeleteDC(dc);
    return ok;
}

/* compute_layout 的字段与每个控件的位置/文本，作为像素之外的第二道断言。 */
static void probe_report(HWND hwnd) {
    RECT client;
    GetClientRect(hwnd, &client);
    probe_line(L"client=%d|%d\n", (int)client.right, (int)client.bottom);
    probe_line(L"dpi=%d\n", g_scale_dpi);
    probe_line(L"runtime_tag=%s\n", ui_runtime_tag());
    probe_line(L"subtitle=%s\n", ui_subtitle_text());
    probe_line(L"alive=%d\n", g_server_started ? 1 : 0);

    for (size_t i = 0; i < PROBE_CONTROL_COUNT; i++) {
        HWND ctl = GetDlgItem(hwnd, g_probe_controls[i].id);
        if (!ctl || !IsWindow(ctl)) {
            probe_line(L"ctl=%s|%d|missing\n", g_probe_controls[i].name, g_probe_controls[i].id);
            continue;
        }
        RECT rc;
        GetWindowRect(ctl, &rc);
        MapWindowPoints(NULL, hwnd, (POINT *)&rc, 2);
        WCHAR text[512];
        text[0] = 0;
        GetWindowTextW(ctl, text, 512);
        for (WCHAR *p = text; *p; p++) {
            if (*p == L'\r' || *p == L'\n' || *p == L'\t' || *p == L'|') *p = L' ';
        }
        probe_line(L"ctl=%s|%d|%d,%d,%d,%d|%s\n", g_probe_controls[i].name, g_probe_controls[i].id,
                   (int)rc.left, (int)rc.top, (int)rc.right, (int)rc.bottom, text);
    }
}

/* --ui-probe-and-exit <w> <h> <alive> <out.bmp> */
int run_ui_probe(int width, int height, int alive, const WCHAR *bmp_path) {
    if (width <= 0 || height <= 0 || !bmp_path || !bmp_path[0]) return 2;

    g_ui_probe = 1;
    g_ui_probe_neutral = 1;
    g_anim_tick_override = 0;
    g_server_started = alive ? 1 : 0;

    INITCOMMONCONTROLSEX ic = { sizeof(ic), ICC_STANDARD_CLASSES };
    InitCommonControlsEx(&ic);

    WNDCLASSEXW wc;
    ZeroMemory(&wc, sizeof wc);
    wc.cbSize = sizeof wc;
    wc.lpfnWndProc = ui_probe_wndproc();
    wc.hInstance = g_inst;
    wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
    wc.hbrBackground = NULL;
    wc.lpszClassName = L"DSTNativeLauncherProbe";
    if (!RegisterClassExW(&wc)) {
        probe_line(L"class_error=%lu\nresult=0\n", GetLastError());
        return 1;
    }

    /* WS_POPUP 没有边框和标题栏，窗口尺寸即客户区尺寸，两版无需再对齐非客户区。 */
    g_main = CreateWindowExW(0, wc.lpszClassName, L"ds probe", WS_POPUP | WS_CLIPCHILDREN,
                             0, 0, width, height, NULL, NULL, g_inst, NULL);
    if (!g_main) {
        probe_line(L"window_error=%lu\nresult=0\n", GetLastError());
        return 1;
    }

    /* DPI 固定为 96：WM_CREATE 里的 dpi_set_for_window 会按实际显示器改写它，
       而两版必须用同一个缩放系数。改完重新建字体并重新布局。 */
    if (g_scale_dpi != 96) {
        HFONT old[6] = {g_font_title, g_font_heading, g_font_body, g_font_small, g_font_mono, g_font_mono_small};
        g_scale_dpi = 96;
        g_font_title      = make_font(22, FW_SEMIBOLD, L"Microsoft YaHei UI");
        g_font_heading    = make_font(11, FW_SEMIBOLD, L"Microsoft YaHei UI");
        g_font_body       = make_font(10, FW_NORMAL,   L"Microsoft YaHei UI");
        g_font_small      = make_font(9,  FW_NORMAL,   L"Microsoft YaHei UI");
        g_font_mono       = make_font(10, FW_NORMAL,   L"Consolas");
        g_font_mono_small = make_font(8,  FW_BOLD,     L"Consolas");
        apply_fonts();
        for (int i = 0; i < 6; i++) DeleteObject(old[i]);
        layout(g_main);
    }

    probe_report(g_main);
    int ok = probe_render(g_main, bmp_path);
    probe_line(L"result=%d\n", ok ? 1 : 0);

    DestroyWindow(g_main);
    g_main = NULL;
    return ok ? 0 : 1;
}
