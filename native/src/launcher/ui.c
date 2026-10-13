/* ================================================================
 * ui.c — 启动器 UI 辅助函数与绘制实现
 * ----------------------------------------------------------------
 * 本文件实现启动器的所有 UI 逻辑：
 *
 *   1. 日志/状态管理：append_log 带时间戳追加并自动裁剪，
 *      支持水平滚动宽度计算
 *   2. 星夜主题绘制：paint_background 绘制品牌、星轨与月亮，
 *      下方暖白操作区包含选择器、指标卡片和日志卡片
 *   3. 布局计算：compute_layout 根据 DPI 缩放和窗口尺寸计算
 *      所有控件的精确坐标，layout 应用这些坐标
 *   4. 按钮自绘：draw_button 区分主按钮（强调色填充）、
 *      服务器按钮（运行态绿色）、普通按钮（卡片色）
 *   5. 用户操作：浏览文件夹、启动游戏、一键翻译流程
 *
 * 一键翻译流程（start_translation）：
 *   检测引擎 → 后台线程启动服务器+部署 hook+预热缓存+启动游戏
 *   （start_server 就绪轮询、deploy、预热均为同步阻塞操作，全部移出 UI 线程）
 * ================================================================ */

#include "ui.h"
#include "deploy.h"
#include "engine.h"
#include "fsutil.h"
#include "godot_patch.h"
#include "godot_preflight_cache.h"
#include "godot_probe.h"
#include "resource.h"
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
#include <math.h>
#include <wchar.h>

#ifndef DS_TRANSLATOR_VERSION
#define DS_TRANSLATOR_VERSION "dev"
#endif
#define DS_WIDEN2(x) L##x
#define DS_WIDEN(x) DS_WIDEN2(x)
#define DS_TRANSLATOR_VERSION_W L"v" DS_WIDEN(DS_TRANSLATOR_VERSION)

/* ---- 日志限制 ---- */
#define LOG_SOFT_LIMIT 900000   /* 日志缓冲区软上限（字节） */
#define LOG_TRIM_TARGET 600000  /* 裁剪后目标大小（字节） */
#define LOG_MAX_LINES 2000      /* 最大保留行数 */

/* 日志水平滚动的最大像素宽度（随最长行动态增长） */
static int g_log_extent_px = 0;

/* 一键翻译流程进行中标志：start_server 健康检查、deploy、预热与启动都在
   后台线程执行期间为 1，期间拒绝重复的开始/还原/清缓存/服务器切换，
   避免重复部署、并发 patch worker 写同一 pck 或争抢服务器进程状态。
   UI 线程置 1，工作线程结束时清 0。 */
static volatile int g_start_flow_running = 0;
static int g_godot_runtime_verified = 0;

int translation_flow_running(void) {
    return g_start_flow_running;
}

/* ----------------------------------------------------------------
 * 按钮悬停渐变状态机
 * ----------------------------------------------------------------
 * 工程未启用 comctl32 v6 视觉样式清单，ODS_HOTLIGHT 不可靠，因此
 * 通过 SetWindowSubclass 跟踪每个自绘按钮的 WM_MOUSEMOVE/
 * WM_MOUSELEAVE，得到稳定的悬停目标值；tick_ui_animation 以
 * 指数缓动把当前强度 t 推向目标，draw_button 按 t 混色，
 * 从而获得平滑的悬停淡入淡出。
 * ---------------------------------------------------------------- */
typedef struct ButtonHover {
    int id;        /* 控件 ID */
    float t;       /* 当前悬停强度 0..1 */
    int target;    /* 目标状态：鼠标悬停为 1 */
    int tracking;  /* 是否已注册 TrackMouseEvent */
} ButtonHover;

static ButtonHover g_btn_hover[] = {
    { IDC_BROWSE,        0.0f, 0, 0 },
    { IDC_OPEN,          0.0f, 0, 0 },
    { IDC_START,         0.0f, 0, 0 },
    { IDC_RESTORE,       0.0f, 0, 0 },
    { IDC_SERVER_TOGGLE, 0.0f, 0, 0 },
    { IDC_API_CONFIG,    0.0f, 0, 0 },
    { IDC_CLEAR_CACHE,   0.0f, 0, 0 },
};
#define BTN_HOVER_COUNT (sizeof(g_btn_hover) / sizeof(g_btn_hover[0]))

static ButtonHover *button_hover_for(int id) {
    for (size_t i = 0; i < BTN_HOVER_COUNT; i++) {
        if (g_btn_hover[i].id == id) return &g_btn_hover[i];
    }
    return NULL;
}

/* 系统减少动画设置只影响装饰和缓动，不改变任何操作结果。 */
static int g_motion_enabled = 1;
static DWORD g_motion_tick = 0;
static int g_motion_warning = 0;
static DWORD g_motion_last = 0;

void refresh_ui_motion(void) {
    BOOL enabled = FALSE;
    if (!SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION, 0, &enabled, 0)) {
        /* 设置由 Windows 所有，无法在上游恢复；读取失败时静态显示，
           不掩盖功能错误，并在每次设置刷新失败时记录系统错误码。 */
        if (!g_motion_warning) {
            WCHAR diagnostic[160];
            swprintf(diagnostic, 160, L"ds launcher: animation preference unavailable (%lu); using static decoration.\n", GetLastError());
            OutputDebugStringW(diagnostic);
            g_motion_warning = 1;
        }
    } else { g_motion_warning = 0; }
    g_motion_enabled = enabled != FALSE;
}

static float button_hover_value(int id) {
    ButtonHover *h = button_hover_for(id);
    return h ? (g_motion_enabled ? h->t : (h->target ? 1.0f : 0.0f)) : 0.0f;
}

static LRESULT CALLBACK button_hover_proc(HWND btn, UINT msg, WPARAM wp, LPARAM lp,
                                          UINT_PTR sub_id, DWORD_PTR ref) {
    ButtonHover *h = button_hover_for((int)sub_id);
    (void)ref;
    switch (msg) {
    case WM_MOUSEMOVE:
        if (h && !h->tracking) {
            TRACKMOUSEEVENT tme;
            ZeroMemory(&tme, sizeof tme);
            tme.cbSize = sizeof tme;
            tme.dwFlags = TME_LEAVE;
            tme.hwndTrack = btn;
            TrackMouseEvent(&tme);
            h->tracking = 1;
            h->target = 1;
            InvalidateRect(btn, NULL, FALSE);
        }
        break;
    case WM_MOUSELEAVE:
        if (h) {
            h->tracking = 0;
            h->target = 0;
            InvalidateRect(btn, NULL, FALSE);
        }
        break;
    case WM_NCDESTROY:
        RemoveWindowSubclass(btn, button_hover_proc, sub_id);
        break;
    }
    return DefSubclassProc(btn, msg, wp, lp);
}

/* 为主窗口所有自绘按钮安装悬停跟踪子类（WM_CREATE 时调用一次） */
void install_button_hover_tracking(HWND hwnd) {
    for (size_t i = 0; i < BTN_HOVER_COUNT; i++) {
        HWND btn = GetDlgItem(hwnd, g_btn_hover[i].id);
        if (btn && IsWindow(btn)) {
            SetWindowSubclass(btn, button_hover_proc, (UINT_PTR)g_btn_hover[i].id, 0);
        }
    }
}

/* 卡片内静态文字使用透明背景，文本更新时必须先让父窗口重绘其背后
 * 区域，再刷新控件本身，避免新旧文字叠影。父窗口动画带不与文本控件
 * 重叠，因此不会引起明暗闪烁。 */
void invalidate_control_area(HWND ctl, int pad) {
    if (!ctl || !IsWindow(ctl)) return;
    HWND parent = GetParent(ctl);
    if (parent) {
        RECT rc;
        GetWindowRect(ctl, &rc);
        MapWindowPoints(NULL, parent, (POINT *)&rc, 2);
        InflateRect(&rc, pad, pad);
        InvalidateRect(parent, &rc, FALSE);
    }
    RedrawWindow(ctl, NULL, NULL, RDW_INVALIDATE | RDW_NOERASE);
}

/* ----------------------------------------------------------------
 * append_log — 向活动日志追加一行
 *
 * 格式：[HH:MM:SS] <消息>
 * 自动将回车/换行/制表符替换为空格（日志为单行列表项）。
 * 超过 LOG_MAX_LINES 时从顶部删除旧行。
 * 计算新行的像素宽度，动态扩展水平滚动范围。
 * 日志列表框不存在时（如 --godot-patch-worker 无窗口模式）回退到
 * OutputDebugStringW，保证后台进程不丢诊断。
 * ---------------------------------------------------------------- */
int g_log_to_stdout = 0;
int g_launch_dry_run = 0;
int g_ui_probe = 0;
int g_ui_probe_neutral = 0;
int g_anim_tick_override = -1;

/* 日志卡片的运行时标签标识窗口实现，探针使用中性标签进行像素对照。
   玩家可见副标题在两版中保持一致。 */
const WCHAR *ui_runtime_tag(void) {
    return g_ui_probe_neutral ? L"runtime" : L"C native runtime";
}

const WCHAR *ui_subtitle_text(void) {
    return L"选择一个游戏，让熟悉的语言带你继续探索。";
}

/* 动画时钟：探针冻结后两版取到同一相位。 */
static DWORD anim_tick(void) {
    return g_anim_tick_override >= 0 ? (DWORD)g_anim_tick_override : g_motion_tick;
}

void write_stdout_utf8(const WCHAR *text) {
    HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
    if (!out || out == INVALID_HANDLE_VALUE || !text || !text[0]) return;
    int need = WideCharToMultiByte(CP_UTF8, 0, text, -1, NULL, 0, NULL, NULL);
    if (need <= 1) return;
    char *buf = (char *)malloc((size_t)need);
    if (!buf) return;
    WideCharToMultiByte(CP_UTF8, 0, text, -1, buf, need, NULL, NULL);
    DWORD written = 0;
    WriteFile(out, buf, (DWORD)(need - 1), &written, NULL);
    free(buf);
}

void append_log(const WCHAR *fmt, ...) {
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

    if (g_log_to_stdout) {
        /* 诊断子命令：正文（不含时间戳）逐行镜像到 stdout，格式 log=<正文>。 */
        write_stdout_utf8(L"log=");
        write_stdout_utf8(line + wcslen(prefix));
        write_stdout_utf8(L"\n");
    }

    if (!g_log || !IsWindow(g_log)) {
        OutputDebugStringW(line);
        OutputDebugStringW(L"\n");
        return;
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

/* 一键流程里"要拉起的进程"在演练模式下的统一出口，见 ui.h 的 g_launch_dry_run。
   kind 区分 shell（游戏 exe）/proc（Godot 引擎）/worker（补丁工作进程）。 */
static void write_spawn_plan(const WCHAR *kind, const WCHAR *exe, const WCHAR *cmd, const WCHAR *cwd) {
    write_stdout_utf8(L"spawn=");
    write_stdout_utf8(kind);
    write_stdout_utf8(L"|");
    write_stdout_utf8(exe ? exe : L"");
    write_stdout_utf8(L"|");
    write_stdout_utf8(cmd ? cmd : L"");
    write_stdout_utf8(L"|");
    write_stdout_utf8(cwd ? cwd : L"");
    write_stdout_utf8(L"\n");
}

/* 更新顶部状态栏（带 "STATUS · " 前缀） */
void set_status(const WCHAR *text) {
    if (g_log_to_stdout) {
        write_stdout_utf8(L"status=");
        write_stdout_utf8(text ? text : L"");
        write_stdout_utf8(L"\n");
    }
    if (g_status && IsWindow(g_status)) {
        WCHAR buf[512];
        _snwprintf(buf, 512, L"STATUS  ·  %s", text ? text : L"");
        buf[511] = 0;
        invalidate_control_area(g_status, 6);
        SetWindowTextW(g_status, buf);
        invalidate_control_area(g_status, 6);
    }
}

/* 缓存卡片文本：共享缓存文件大小，读不到属性时按 0 显示。
   返回是否读到了文件属性（调用方据此保持原有的重绘次数）。 */
int cache_size_text(WCHAR *out, int cap) {
    if (!out || cap <= 0) return 0;
    out[0] = 0;
    WCHAR cache_path[MAX_PATH * 4];
    path_join(cache_path, MAX_PATH * 4, g_root, L"translation_memory_c.tsv");
    WIN32_FILE_ATTRIBUTE_DATA data;
    if (GetFileAttributesExW(cache_path, GetFileExInfoStandard, &data)) {
        ULONGLONG bytes = ((ULONGLONG)data.nFileSizeHigh << 32) | data.nFileSizeLow;
        _snwprintf(out, cap, L"%.1f MB", (double)bytes / 1024.0 / 1024.0);
        out[cap - 1] = 0;
        return 1;
    }
    wcsncpy(out, L"0.0 MB", (size_t)cap - 1);
    out[cap - 1] = 0;
    return 0;
}

/* 刷新缓存卡片：读取 translation_memory_c.tsv 文件大小并显示 */
void update_cache_card(void) {
    if (!g_cache || !IsWindow(g_cache)) return;
    invalidate_control_area(g_cache, 4);
    WCHAR text[128];
    if (cache_size_text(text, 128)) {
        invalidate_control_area(g_cache, 4);
        SetWindowTextW(g_cache, text);
        invalidate_control_area(g_cache, 4);
    } else {
        SetWindowTextW(g_cache, text);
    }
    invalidate_control_area(g_cache, 4);
}

/* 删除共享缓存文件本身。文件不存在视为已清除；目标是目录或删除失败时保留原
   文件并记录 Windows 错误码，绝不把失败当成已清除。 */
int clear_cache_file(void) {
    WCHAR cache_path[MAX_PATH * 4];
    path_join(cache_path, MAX_PATH * 4, g_root, L"translation_memory_c.tsv");
    DWORD attr = GetFileAttributesW(cache_path);
    if (attr == INVALID_FILE_ATTRIBUTES) {
        DWORD error = GetLastError();
        if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND) return 1;
        append_log(L"清除缓存：无法检查 %s（Windows 错误 %lu）。", cache_path, error);
        return 0;
    }
    if (attr & FILE_ATTRIBUTE_DIRECTORY) {
        append_log(L"清除缓存：目标路径是目录，已保留：%s", cache_path);
        return 0;
    }
    if (delete_file_safe(cache_path)) return 1;
    append_log(L"清除缓存：无法删除 %s（Windows 错误 %lu）。", cache_path, GetLastError());
    return 0;
}

void clear_translation_cache(void) {
    if (g_start_flow_running) {
        MessageBoxW(g_main, L"翻译流程正在进行中，请完成后再清除缓存。", L"ds游戏翻译器", MB_ICONWARNING);
        return;
    }
    const WCHAR *message =
        L"这会永久删除本机共享翻译缓存 translation_memory_c.tsv。\n\n"
        L"清除后，已有译文需要重新请求 API。正在运行的游戏可能仍保留进程内存缓存，重启游戏后才会完全生效。\n\n"
        L"API 配置、日志和游戏目录不会被删除。确定继续吗？";
    if (MessageBoxW(g_main, message, L"清除缓存",
                    MB_ICONWARNING | MB_YESNO | MB_DEFBUTTON2) != IDYES) return;

    refresh_server_status();
    int was_running = g_server_started && server_alive();
    int server_stopped = 1;
    if (was_running) {
        set_status(L"正在停止服务并清除缓存...");
        append_log(L"清除缓存：正在停止本地服务，避免内存缓存再次写回磁盘。");
        stop_server();
        refresh_server_status();
        server_stopped = !(g_server_started && server_alive());
        if (!server_stopped) {
            append_log(L"清除缓存：本地服务仍在运行，已取消删除以避免旧内存缓存继续生效。");
        }
    } else {
        set_status(L"正在清除缓存...");
    }

    WCHAR cache_path[MAX_PATH * 4];
    path_join(cache_path, MAX_PATH * 4, g_root, L"translation_memory_c.tsv");
    int cleared = server_stopped ? clear_cache_file() : 0;

    int restarted = 1;
    if (was_running && server_stopped) {
        set_status(L"正在重新启动本地服务...");
        restarted = start_server();
    }
    update_cache_card();

    if (cleared) append_log(L"共享翻译缓存已清除：%s", cache_path);
    if (cleared && restarted) {
        set_status(L"缓存已清除。");
        MessageBoxW(g_main, L"共享翻译缓存已清除。正在运行的游戏请重启后再使用。",
                    L"清除缓存", MB_ICONINFORMATION);
    } else if (cleared) {
        set_status(L"缓存已清除，但本地服务重启失败。");
        MessageBoxW(g_main, L"缓存已清除，但本地服务重启失败，请查看日志。",
                    L"清除缓存", MB_ICONWARNING);
    } else {
        set_status(L"缓存清除失败，请查看日志。");
        MessageBoxW(g_main, L"缓存清除失败，原缓存文件已保留，请查看日志。",
                    L"清除缓存", MB_ICONWARNING);
    }
}

/* 刷新引擎卡片：从路径框读取目录，重新检测引擎类型并更新显示 */
void refresh_engine(void) {
    if (!g_engine || !IsWindow(g_engine)) return;
    GetWindowTextW(g_path, g_game, MAX_PATH * 4);
    Engine e = detect_engine(g_game);
    invalidate_control_area(g_engine, 4);
    SetWindowTextW(g_engine, engine_name(e));
    invalidate_control_area(g_engine, 4);
}

/* ----------------------------------------------------------------
 * apply_fonts — 为所有控件应用全局字体
 *
 * 所有控件先用正文字体，然后按角色覆盖：
 *   - 标题用大字体，副标题用小字体
 *   - 标签/引擎/服务器/缓存用标题字体
 *   - 状态栏用小等宽字体
 *   - 日志用等宽字体，并根据字体高度设置列表项行高
 * ---------------------------------------------------------------- */
void apply_fonts(void) {
    HWND controls[] = {g_title, g_subtitle, g_path_label, g_path, g_engine, g_server, g_cache, g_status, g_log, g_btn_server, g_btn_api, g_btn_restore, g_btn_clear_cache};
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
    if (IsWindow(g_btn_clear_cache)) SendMessageW(g_btn_clear_cache, WM_SETFONT, (WPARAM)g_font_small, TRUE);
    const int button_ids[] = {IDC_BROWSE, IDC_OPEN, IDC_START};
    for (size_t i = 0; i < sizeof(button_ids) / sizeof(button_ids[0]); i++) {
        HWND button = GetDlgItem(g_main, button_ids[i]);
        if (button && IsWindow(button)) SendMessageW(button, WM_SETFONT, (WPARAM)g_font_body, TRUE);
    }
    if (IsWindow(g_status))   SendMessageW(g_status,   WM_SETFONT, (WPARAM)g_font_small, TRUE);
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

void apply_window_chrome(HWND hwnd) {
    typedef HRESULT (WINAPI *DwmSetWindowAttributeFn)(HWND, DWORD, LPCVOID, DWORD);
    typedef HRESULT (WINAPI *SetWindowThemeFn)(HWND, LPCWSTR, LPCWSTR);

    HMODULE dwm = LoadLibraryW(L"dwmapi.dll");
    if (dwm) {
        DwmSetWindowAttributeFn set_attr =
            (DwmSetWindowAttributeFn)(void *)GetProcAddress(dwm, "DwmSetWindowAttribute");
        if (set_attr) {
            BOOL dark = TRUE;
            if (FAILED(set_attr(hwnd, 20, &dark, sizeof dark))) {
                (void)set_attr(hwnd, 19, &dark, sizeof dark);
            }
            COLORREF caption = C_PAGE;
            COLORREF border = C_LINE;
            COLORREF text = RGB(245, 240, 229);
            (void)set_attr(hwnd, 35, &caption, sizeof caption);
            (void)set_attr(hwnd, 34, &border, sizeof border);
            (void)set_attr(hwnd, 36, &text, sizeof text);
        }
        FreeLibrary(dwm);
    }

    HMODULE ux = LoadLibraryW(L"uxtheme.dll");
    if (ux) {
        SetWindowThemeFn set_theme =
            (SetWindowThemeFn)(void *)GetProcAddress(ux, "SetWindowTheme");
        if (set_theme) {
            if (g_log && IsWindow(g_log)) (void)set_theme(g_log, L"Explorer", NULL);
            if (g_path && IsWindow(g_path)) (void)set_theme(g_path, L"Explorer", NULL);
        }
        FreeLibrary(ux);
    }
}

/* ======================== 绘制原语 ======================== */

/* 绘制圆角矩形（指定填充色和边框色） */
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

/* 绘制带光晕的圆点（用于状态指示灯、列表标记） */
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

/* 在指定矩形内绘制文本（设置字体和颜色） */
static void draw_text_x(HDC dc, const WCHAR *text, int x, int y, int w, int h, COLORREF color, HFONT font, UINT flags) {
    SelectObject(dc, font);
    SetTextColor(dc, color);
    RECT r = {x, y, x + w, y + h};
    DrawTextW(dc, text, -1, &r, flags);
}

/* 线性插值混合两个颜色（t=0 返回 a，t=1 返回 b） */
static COLORREF mix(COLORREF a, COLORREF b, float t) {
    int ar = GetRValue(a), ag = GetGValue(a), ab = GetBValue(a);
    int br = GetRValue(b), bg = GetGValue(b), bb = GetBValue(b);
    int r = ar + (int)((float)(br - ar) * t);
    int g = ag + (int)((float)(bg - ag) * t);
    int bl = ab + (int)((float)(bb - ab) * t);
    return RGB(r, g, bl);
}

/* 绘制垂直渐变填充（top→bottom） */
static void draw_vgradient(HDC dc, int x, int y, int w, int h, COLORREF top, COLORREF bot) {
    TRIVERTEX v[2] = {
        { x,     y,     (USHORT)(GetRValue(top) << 8), (USHORT)(GetGValue(top) << 8), (USHORT)(GetBValue(top) << 8), 0 },
        { x + w, y + h, (USHORT)(GetRValue(bot) << 8), (USHORT)(GetGValue(bot) << 8), (USHORT)(GetBValue(bot) << 8), 0 }
    };
    GRADIENT_RECT gr = { 0, 1 };
    GradientFill(dc, v, 2, &gr, 1, GRADIENT_FILL_RECT_V);
}

/* ----------------------------------------------------------------
 * card_text_brush — 卡片内静态文字的不透明底色画刷
 * 主窗口带 WS_CLIPCHILDREN，父窗口不会绘制子控件背后的区域，
 * 透明背景的文字控件将无人铺底。静态文字保持不透明，底色与
 * 暖白面板一致，避免文字矩形露出不同底色。
 * picker_label 用于区分选择器标签与指标数值的画刷生命周期。
 * ---------------------------------------------------------------- */
static HBRUSH g_card_brush_label = NULL;
static HBRUSH g_card_brush_value = NULL;

HBRUSH card_text_brush(int picker_label, COLORREF *out_color) {
    COLORREF col = C_CARD;
    if (out_color) *out_color = col;
    HBRUSH *slot = picker_label ? &g_card_brush_label : &g_card_brush_value;
    if (!*slot) *slot = CreateSolidBrush(col);
    return *slot;
}

void free_card_text_brushes(void) {
    if (g_card_brush_label) { DeleteObject(g_card_brush_label); g_card_brush_label = NULL; }
    if (g_card_brush_value) { DeleteObject(g_card_brush_value); g_card_brush_value = NULL; }
}

/* 0..1 呼吸脉冲（正弦，周期 period_ms 毫秒） */
static float pulse01(DWORD period_ms) {
    DWORD now = anim_tick() % period_ms;
    return 0.5f + 0.5f * sinf((float)now / (float)period_ms * 6.2831853f - 1.5707963f);
}

/* 绘制水平渐变填充（left→right） */
static void draw_hgradient(HDC dc, int x, int y, int w, int h, COLORREF left, COLORREF right) {
    TRIVERTEX v[2] = {
        { x,     y,     (USHORT)(GetRValue(left) << 8),  (USHORT)(GetGValue(left) << 8),  (USHORT)(GetBValue(left) << 8),  0 },
        { x + w, y + h, (USHORT)(GetRValue(right) << 8), (USHORT)(GetGValue(right) << 8), (USHORT)(GetBValue(right) << 8), 0 }
    };
    GRADIENT_RECT gr = { 0, 1 };
    GradientFill(dc, v, 2, &gr, 1, GRADIENT_FILL_RECT_H);
}

/* 暖白卡片：薄边框与轻柔阴影 */
static void draw_panel_shell(HDC dc, RECT rc, COLORREF accent, int elevated) {
    (void)accent; (void)elevated;
    RECT shadow = rc;
    OffsetRect(&shadow, 0, sc(2));
    draw_round(dc, shadow, RGB(231, 224, 210), RGB(231, 224, 210), sc(18));
    draw_round(dc, rc, C_CARD, C_LINE, sc(18));
}

/* 英雄区数据线：发丝基线 + 正弦缓动往返的渐隐光束，尾部带暖灰残影。
 * base 为该行处的页面背景色，用于光束两端的无缝渐隐。 */
static void draw_hero_data_line(HDC dc, int left, int right, int y, COLORREF base) {
    HPEN base_pen = CreatePen(PS_SOLID, 1, mix(C_LINE, base, 0.35f));
    HGDIOBJ old = SelectObject(dc, base_pen);
    MoveToEx(dc, left, y, NULL);
    LineTo(dc, right, y);
    SelectObject(dc, old);
    DeleteObject(base_pen);

    int width = right - left;
    if (width <= 0) return;

    /* 9 秒一个往返，cos 缓动让光束两端减速，运动平滑 */
    DWORD now = anim_tick();
    float phase = (float)(now % 9000u) / 9000.0f;
    float eased = 0.5f - 0.5f * cosf(phase * 6.2831853f);
    int beam_w = sc(96);
    int travel = width - beam_w;
    if (travel < 1) travel = 1;
    int bx = left + (int)(eased * (float)travel);

    /* 暖灰拖尾（暗一档、短一截，反方向偏移） */
    int tail_w = sc(40);
    int tail_x = bx - (int)((eased - 0.5f) * (float)sc(48));
    draw_hgradient(dc, tail_x - tail_w / 2, y, tail_w / 2, 1, base, mix(C_VIOLET, base, 0.55f));
    draw_hgradient(dc, tail_x, y, tail_w / 2, 1, mix(C_VIOLET, base, 0.55f), base);

    /* 主光束：两端向背景色渐隐 */
    int fade = sc(28);
    int core = beam_w - fade * 2;
    if (core < 1) core = 1;
    draw_hgradient(dc, bx, y - 1, fade, 2, base, C_ACCENT);
    HBRUSH cb = CreateSolidBrush(C_ACCENT);
    RECT core_rc = {bx + fade, y - 1, bx + fade + core, y + 1};
    FillRect(dc, &core_rc, cb);
    DeleteObject(cb);
    draw_hgradient(dc, bx + fade + core, y - 1, fade, 2, C_ACCENT, base);
}

/* ======================== 布局计算 ======================== */

/* 所有 UI 元素的坐标集合，由 compute_layout 一次性计算 */
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
    int action_gap;
    int action_button_w;
    int action_x[4];
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

/* 整数最大值辅助 */
static int max_i(int a, int b) {
    return a > b ? a : b;
}

/* ----------------------------------------------------------------
 * compute_layout — 根据窗口尺寸和 DPI 计算所有 UI 元素坐标
 *
 * 所有尺寸通过 sc() 做 DPI 缩放。布局分区：
 *   - 顶部星夜主题区（旧 rail 字段保留为 0，保持布局报告兼容）
 *   - 主区域：英雄标题带 + 选择器卡片 + 指标卡片 + 日志卡片
 * 主区域最小宽度 560（缩放后），保证小窗口下仍可用。
 * ---------------------------------------------------------------- */
static UiLayout compute_layout(HWND hwnd) {
    RECT r;
    GetClientRect(hwnd, &r);

    UiLayout ui;
    ui.rail = 0;
    ui.pad = sc(40);
    ui.x = ui.rail + ui.pad;
    ui.w = max_i(r.right - ui.x - ui.pad, sc(560));
    ui.hero_right = ui.w / 2 - sc(20);

    int picker_top = sc(262);
    int picker_h = sc(166);
    ui.picker.left = ui.x;
    ui.picker.top = picker_top;
    ui.picker.right = ui.x + ui.w;
    ui.picker.bottom = picker_top + picker_h;

    ui.path_label_x = ui.x + sc(24);
    ui.path_label_y = picker_top + sc(18);
    ui.path_label_w = sc(240);
    ui.path_label_h = sc(24);

    ui.path_x = ui.x + sc(24);
    ui.path_y = picker_top + sc(56);
    ui.path_h = sc(38);
    ui.side_button_w = sc(112);
    ui.side_gap = sc(12);
    ui.open_x = ui.x + ui.w - sc(24) - ui.side_button_w;
    ui.browse_x = ui.open_x - ui.side_gap - ui.side_button_w;
    ui.path_w = max_i(ui.browse_x - ui.path_x - ui.side_gap, sc(180));

    ui.action_y = picker_top + sc(108);
    ui.action_h = sc(40);
    ui.action_gap = sc(12);
    int action_inner_w = ui.w - sc(48);
    ui.action_button_w = (action_inner_w - ui.action_gap * 3) / 4;
    int action_left = ui.x + sc(24);
    for (int i = 0; i < 4; i++)
        ui.action_x[i] = action_left + i * (ui.action_button_w + ui.action_gap);

    ui.metric_gap = sc(14);
    ui.metric_w = max_i((ui.w - ui.metric_gap * 2) / 3, sc(150));
    ui.metric_y = ui.picker.bottom + sc(14);
    ui.metric_h = sc(76);
    ui.metric_value_y = ui.metric_y + sc(34);
    ui.metric_value_h = sc(30);

    ui.log_top = ui.metric_y + ui.metric_h + sc(14);
    ui.log_bottom = max_i(r.bottom - sc(32), ui.log_top + sc(96));
    ui.log_edit_top = ui.log_top + sc(40);
    ui.log_edit_h = max_i(ui.log_bottom - sc(18) - ui.log_edit_top, sc(32));

    return ui;
}

/* 动画心跳（约 30fps）。后台、最小化与减少动画模式停止装饰刷新：
 *   - 英雄区数据线（位于状态文本之下，不与任何子控件重叠）
 *   - 状态药丸指示灯与右侧星轨（均为自绘区域）
 *   - 悬停过渡中的按钮（推进指数缓动并重绘按钮本身）
 * 文本控件不被定时器触碰，避免明暗闪烁。 */
void tick_ui_animation(HWND hwnd) {
    DWORD now = GetTickCount();
    DWORD elapsed = g_motion_last ? now - g_motion_last : 33u;
    g_motion_last = now;
    if (!hwnd || !IsWindow(hwnd) || IsIconic(hwnd) || GetForegroundWindow() != hwnd || g_ui_probe) return;
    if (elapsed > 100u) elapsed = 100u;
    float blend = g_motion_enabled ? 1.0f - expf(-(float)elapsed / 80.0f) : 1.0f;
    if (g_motion_enabled) {
        g_motion_tick += elapsed;
        UiLayout ui = compute_layout(hwnd);
        RECT sky = {ui.x + ui.w - ui.hero_right, sc(75), ui.x + ui.w, sc(240)};
        RECT data_line = {ui.x - sc(4), sc(230), ui.x + sc(212), sc(235)};
        RECT pill_dot = {ui.x + ui.w - sc(158), sc(27), ui.x + ui.w - sc(136), sc(47)};
        InvalidateRect(hwnd, &sky, FALSE);
        InvalidateRect(hwnd, &data_line, FALSE);
        InvalidateRect(hwnd, &pill_dot, FALSE);
    }

    /* 按钮悬停渐变：指数缓动逼近目标，过渡期间重绘按钮 */
    for (size_t i = 0; i < BTN_HOVER_COUNT; i++) {
        ButtonHover *h = &g_btn_hover[i];
        float target = h->target ? 1.0f : 0.0f;
        if (h->t == target) continue;
        float next = h->t + (target - h->t) * blend;
        if (fabsf(next - target) < 0.012f) next = target;
        if (next == h->t) continue;
        h->t = next;
        HWND btn = GetDlgItem(hwnd, h->id);
        if (btn && IsWindow(btn)) InvalidateRect(btn, NULL, FALSE);
    }
}

/* 星轨以固定种子生成，30 秒缓慢流动；只绘制右侧无子控件的插画区域。 */
static void draw_nocturne_sky(HDC dc, int left, int top, int right, int bottom) {
    RECT bounds = {left, top, right, bottom};
    if (!RectVisible(dc, &bounds)) return;
    int saved = SaveDC(dc);
    IntersectClipRect(dc, left, top, right, bottom);
    int width = right - left;
    int height = bottom - top;
    double phase = (double)(anim_tick() % 30000u) / 30000.0 * 6.283185307179586;
    int cx = left + width * 3 / 5;
    int cy = top + height / 2;
    HGDIOBJ old_pen = SelectObject(dc, GetStockObject(NULL_PEN));
    /* 暗蓝色光晕与成束笔触形成星云，不使用外部图像或额外后台线程。 */
    for (int i = 9; i >= 1; i--) {
        int rx = sc(12 + i * 15);
        int ry = sc(6 + i * 7);
        HBRUSH b = CreateSolidBrush(mix(C_PAGE, RGB(27, 76, 123), (float)(10 - i) * 0.035f));
        HGDIOBJ ob = SelectObject(dc, b);
        Ellipse(dc, cx - rx, cy - ry, cx + rx, cy + ry);
        SelectObject(dc, ob);
        DeleteObject(b);
    }
    SelectObject(dc, old_pen);
    for (int band = 0; band < 14; band++) {
        COLORREF color = mix(C_PAGE, band % 4 == 0 ? C_ACCENT : RGB(68, 126, 173), band % 4 == 0 ? 0.42f : 0.38f);
        HPEN p = CreatePen(PS_SOLID, 1, color);
        HGDIOBJ op = SelectObject(dc, p);
        for (int j = 0; j <= 70; j++) {
            double angle = (double)j * 0.085 + (double)band * 0.026 + sin(phase) * 0.12;
            double radius = (double)sc(15 + band * 7) + (double)j * (double)sc(1) * 0.60;
            int px = cx + (int)(cos(angle) * radius * 1.40);
            int py = cy + (int)(sin(angle) * radius * 0.48);
            if (j == 0) MoveToEx(dc, px, py, NULL);
            else LineTo(dc, px, py);
        }
        SelectObject(dc, op);
        DeleteObject(p);
    }
    for (int i = 0; i < 38; i++) {
        int sx = left + (width * ((i * 73 + 19) % 997)) / 997;
        int sy = top + sc(6) + ((height - sc(12)) * ((i * 151 + 61) % 991)) / 991;
        float glow = 0.46f + (float)(0.18 * sin(phase + (double)i));
        COLORREF star = mix(C_PAGE, C_ACCENT, glow);
        HPEN p = CreatePen(PS_SOLID, 1, star);
        HGDIOBJ op = SelectObject(dc, p);
        int size = sc(i % 9 == 0 ? 4 : 1);
        MoveToEx(dc, sx - size, sy, NULL); LineTo(dc, sx + size + 1, sy);
        MoveToEx(dc, sx, sy - size, NULL); LineTo(dc, sx, sy + size + 1);
        SelectObject(dc, op);
        DeleteObject(p);
    }
    /* 月亮使用实心暖金色与夜色遮片，不叠加高频闪烁。 */
    HGDIOBJ op = SelectObject(dc, GetStockObject(NULL_PEN));
    int mx = right - sc(62), my = top + sc(36);
    HBRUSH moon = CreateSolidBrush(C_ACCENT);
    HGDIOBJ ob = SelectObject(dc, moon);
    Ellipse(dc, mx - sc(15), my - sc(15), mx + sc(15), my + sc(15));
    HBRUSH shade = CreateSolidBrush(C_PAGE);
    SelectObject(dc, shade);
    Ellipse(dc, mx - sc(6), my - sc(19), mx + sc(20), my + sc(7));
    SelectObject(dc, ob); SelectObject(dc, op);
    DeleteObject(moon); DeleteObject(shade);
    RestoreDC(dc, saved);
}

/* 星夜首页：深蓝主题区、暖白操作区，以及独立的状态与活动记录。 */
void paint_background(HWND hwnd, HDC dc) {
    RECT r;
    GetClientRect(hwnd, &r);
    UiLayout ui = compute_layout(hwnd);
    int x = ui.x, w = ui.w;
    int alive = g_server_started;
    draw_vgradient(dc, 0, 0, r.right, r.bottom, C_PAGE, C_PAGE);
    draw_vgradient(dc, 0, sc(246), r.right, r.bottom - sc(246), RGB(245, 240, 229), RGB(249, 245, 235));
    SetBkMode(dc, TRANSPARENT);

    HICON icon = (HICON)LoadImageW(g_inst, MAKEINTRESOURCEW(IDI_APP_ICON), IMAGE_ICON, sc(28), sc(28), LR_SHARED);
    if (icon) DrawIconEx(dc, x, sc(20), icon, sc(28), sc(28), 0, NULL, DI_NORMAL);
    draw_text_x(dc, L"ds 游戏翻译器", x + sc(40), sc(17), sc(190), sc(24), RGB(245, 240, 229), g_font_heading, DT_LEFT | DT_SINGLELINE | DT_VCENTER);
    draw_text_x(dc, L"每一段故事，都值得抵达", x + sc(40), sc(40), sc(260), sc(16), RGB(162, 180, 200), g_font_small, DT_LEFT | DT_SINGLELINE | DT_VCENTER);
    draw_text_x(dc, L"Ren'Py   /   RPG Maker   /   Unity   /   Godot", x + w - sc(480), sc(24), sc(290), sc(24), RGB(174, 190, 207), g_font_small, DT_RIGHT | DT_SINGLELINE | DT_VCENTER);
    RECT pill = {x + w - sc(166), sc(21), x + w, sc(53)};
    draw_round(dc, pill, C_PAGE, RGB(70, 81, 96), sc(32));
    COLORREF status_color = alive ? RGB(148, 206, 166) : C_ACCENT;
    draw_dot(dc, pill.left + sc(18), sc(37), sc(3), status_color, mix(C_PAGE, status_color, 0.20f + 0.12f * pulse01(4200)));
    draw_text_x(dc, alive ? L"翻译服务已就绪" : L"等待开始", pill.left + sc(34), pill.top, sc(120), sc(32), status_color, g_font_small, DT_LEFT | DT_SINGLELINE | DT_VCENTER);
    HPEN line = CreatePen(PS_SOLID, 1, RGB(38, 54, 76));
    HGDIOBJ old = SelectObject(dc, line);
    MoveToEx(dc, x, sc(70), NULL); LineTo(dc, x + w, sc(70));
    SelectObject(dc, old); DeleteObject(line);

    draw_text_x(dc, L"游 戏 无 界  /  PLAY BEYOND WORDS", x, sc(84), sc(480), sc(20), C_ACCENT, g_font_small, DT_LEFT | DT_SINGLELINE | DT_VCENTER);
    draw_nocturne_sky(dc, x + w - ui.hero_right, sc(75), x + w, sc(240));
    draw_hero_data_line(dc, x, x + sc(208), sc(232), C_PAGE);

    RECT clip;
    GetClipBox(dc, &clip);
    if (clip.bottom <= sc(246)) return;
    draw_panel_shell(dc, ui.picker, C_ACCENT, 0);
    draw_text_x(dc, L"拖入游戏 EXE 或文件夹，即可开始汉化", x + sc(200), ui.picker.top + sc(18), w - sc(224), sc(24), C_MUTED, g_font_small, DT_RIGHT | DT_SINGLELINE | DT_VCENTER);
    const WCHAR *labels[] = {L"游戏引擎", L"本地服务", L"翻译记忆"};
    const WCHAR *numbers[] = {L"01", L"02", L"03"};
    for (int i = 0; i < 3; i++) {
        int cx = x + (ui.metric_w + ui.metric_gap) * i;
        RECT card = {cx, ui.metric_y, cx + ui.metric_w, ui.metric_y + ui.metric_h};
        draw_panel_shell(dc, card, C_ACCENT, 0);
        draw_text_x(dc, labels[i], cx + sc(18), ui.metric_y + sc(12), ui.metric_w - sc(70), sc(20), C_TEXT_DIM, g_font_small, DT_LEFT | DT_SINGLELINE | DT_VCENTER);
        draw_text_x(dc, numbers[i], cx + ui.metric_w - sc(48), ui.metric_y + sc(12), sc(28), sc(20), C_AMBER, g_font_mono_small, DT_RIGHT | DT_SINGLELINE | DT_VCENTER);
    }
    RECT log_card = {x, ui.log_top, x + w, ui.log_bottom};
    draw_panel_shell(dc, log_card, C_ACCENT, 0);
    draw_text_x(dc, L"旅程记录", x + sc(18), ui.log_top + sc(10), sc(110), sc(24), C_TEXT, g_font_heading, DT_LEFT | DT_SINGLELINE | DT_VCENTER);
    draw_text_x(dc, L"准备、部署与翻译进度，都在这里。", x + sc(124), ui.log_top + sc(10), w - sc(380), sc(24), C_MUTED, g_font_small, DT_LEFT | DT_SINGLELINE | DT_VCENTER);
    draw_text_x(dc, ui_runtime_tag(), x + w - sc(190), ui.log_top + sc(10), sc(170), sc(24), C_MUTED, g_font_small, DT_RIGHT | DT_SINGLELINE | DT_VCENTER);
    draw_text_x(dc, L"本地记忆  ·  即时翻译  ·  保留原有格式", x, r.bottom - sc(22), sc(430), sc(16), C_TEXT_DIM, g_font_small, DT_LEFT | DT_SINGLELINE | DT_VCENTER);
    draw_text_x(dc, DS_TRANSLATOR_VERSION_W, x + w - sc(130), r.bottom - sc(22), sc(130), sc(16), C_AMBER, g_font_mono_small, DT_RIGHT | DT_SINGLELINE | DT_VCENTER);
    RECT border = {ui.path_x - sc(2), ui.path_y - sc(2), ui.path_x + ui.path_w + sc(2), ui.path_y + ui.path_h + sc(2)};
    draw_round(dc, border, C_LOG, C_LINE, sc(10));
}

/* 缓冲只归主窗口的 UI 线程所有；仅复用存储，每次仍重绘整个脏区。
   调整尺寸、DPI、显示设备或销毁窗口时释放，不保留旧画面内容。 */
static HDC background_dc;
static HBITMAP background_bitmap;
static HWND background_owner;
static int background_width, background_height;
static unsigned background_failures;

static void report_background_failure(void) {
    /* GDI 资源不足或设备状态失效由 Windows 决定，上游无法保证分配/绘制成功。
       直接绘制可能恢复闪烁，但不会冒充双缓冲成功；按首次及 2 的幂次向调试器
       记录失败，避免动画反复触发时刷屏。释放失败也走相同诊断。 */
    unsigned count = ++background_failures;
    if (count == 1 || (count & (count - 1)) == 0)
        OutputDebugStringW(L"ds launcher: GDI back buffer operation failed; using direct paint when needed.\n");
}

void free_background_buffer(void) {
    /* 先销毁 DC，解除所有已选对象和保存状态，再删除自有位图。 */
    if (background_dc && !DeleteDC(background_dc)) report_background_failure();
    if (background_bitmap && !DeleteObject(background_bitmap)) report_background_failure();
    background_dc = NULL;
    background_bitmap = NULL;
    background_owner = NULL;
    background_width = background_height = 0;
}

void paint_background_buffered(HWND hwnd, HDC dc, const RECT *dirty) {
    if (!dirty || dirty->right <= dirty->left || dirty->bottom <= dirty->top) return;

    int width = dirty->right - dirty->left;
    int height = dirty->bottom - dirty->top;
    if (background_owner != hwnd || width > background_width || height > background_height) {
        int capacity_width = background_owner == hwnd && background_width > width ? background_width : width;
        int capacity_height = background_owner == hwnd && background_height > height ? background_height : height;
        free_background_buffer();
        background_dc = CreateCompatibleDC(dc);
        background_bitmap = background_dc ? CreateCompatibleBitmap(dc, capacity_width, capacity_height) : NULL;
        if (!background_dc || !background_bitmap) goto failed;
        HGDIOBJ previous = SelectObject(background_dc, background_bitmap);
        if (!previous || previous == HGDI_ERROR) goto failed;
        background_owner = hwnd;
        background_width = capacity_width;
        background_height = capacity_height;
    }

    int saved = SaveDC(background_dc);
    if (!saved || !SetViewportOrgEx(background_dc, -dirty->left, -dirty->top, NULL) ||
        IntersectClipRect(background_dc, dirty->left, dirty->top, dirty->right, dirty->bottom) == ERROR) goto failed;
    paint_background(hwnd, background_dc);
    if (!RestoreDC(background_dc, saved) ||
        !BitBlt(dc, dirty->left, dirty->top, width, height, background_dc, 0, 0, SRCCOPY)) goto failed;
    return;

failed:
    report_background_failure();
    free_background_buffer();
    paint_background(hwnd, dc);
}

static void position_control(HWND ctl, int x, int y, int w, int h, UINT flags) {
    if (ctl && IsWindow(ctl)) SetWindowPos(ctl, NULL, x, y, w, h, flags);
}

/* ----------------------------------------------------------------
 * layout — 根据计算好的布局移动所有子控件到正确位置
 *
 * 在 WM_SIZE 时调用，包括：英雄标题带、选择器卡片内容（路径标签/
 * 输入框/浏览/打开按钮）、操作按钮行（开始/服务器/API）、指标卡片值、
 * 日志列表。
 * ---------------------------------------------------------------- */
void layout(HWND hwnd) {
    UiLayout ui = compute_layout(hwnd);
    int x = ui.x;
    int w = ui.w;

    const UINT position_flags = SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOREDRAW;

    /* 英雄标题带 */
    int hero_right = ui.hero_right;
    position_control(g_title,    x, sc(112), w - hero_right, sc(50), position_flags);
    position_control(g_subtitle, x, sc(170), w - hero_right, sc(22), position_flags);
    position_control(g_status,   x, sc(204), w - hero_right, sc(22), position_flags);

    /* 选择器卡片内容 */
    position_control(g_path_label, ui.path_label_x, ui.path_label_y, ui.path_label_w, ui.path_label_h, position_flags);
    position_control(g_path, ui.path_x, ui.path_y, ui.path_w, ui.path_h, position_flags);
    position_control(GetDlgItem(hwnd, IDC_BROWSE), ui.browse_x, ui.path_y, ui.side_button_w, ui.path_h, position_flags);
    position_control(GetDlgItem(hwnd, IDC_OPEN),   ui.open_x,   ui.path_y, ui.side_button_w, ui.path_h, position_flags);

    /* 操作按钮行 */
    int abY = ui.action_y;
    int abH = ui.action_h;
    position_control(GetDlgItem(hwnd, IDC_START), ui.action_x[0], abY, ui.action_button_w, abH, position_flags);
    position_control(g_btn_restore,               ui.action_x[1], abY, ui.action_button_w, abH, position_flags);
    position_control(g_btn_server,                ui.action_x[2], abY, ui.action_button_w, abH, position_flags);
    int api_w = x + w - sc(24) - ui.action_x[3];
    position_control(g_btn_api,                   ui.action_x[3], abY, api_w, abH, position_flags);

    /* 指标卡片：值定位在每张卡片下半部 */
    int gap = ui.metric_gap;
    int cardW = ui.metric_w;
    int mValY = ui.metric_value_y;
    int mValH = ui.metric_value_h;
    position_control(g_engine, x + sc(18),               mValY, cardW - sc(30), mValH, position_flags);
    position_control(g_server, x + cardW + gap + sc(18), mValY, cardW - sc(30), mValH, position_flags);
    int cacheX = x + (cardW + gap) * 2;
    int clearW = sc(88);
    position_control(g_cache, cacheX + sc(18), mValY, cardW - clearW - sc(42), mValH, position_flags);
    position_control(g_btn_clear_cache, cacheX + cardW - clearW - sc(14), mValY + sc(2), clearW, mValH - sc(4), position_flags);

    /* 日志卡片内的日志内容区（标题栏由背景绘制） */
    position_control(g_log, x + sc(18), ui.log_edit_top, w - sc(36), ui.log_edit_h, position_flags);

    RedrawWindow(hwnd, NULL, NULL, RDW_INVALIDATE | RDW_ALLCHILDREN | RDW_NOERASE);
}

static int draw_button_icon(HDC dc, int id, int cx, int cy, COLORREF color) {
    int s = sc(7);
    HPEN pen = CreatePen(PS_SOLID, max_i(sc(2), 1), color);
    HGDIOBJ old_pen = SelectObject(dc, pen);
    HGDIOBJ old_brush = SelectObject(dc, GetStockObject(HOLLOW_BRUSH));
    int drawn = 1;

    if (id == IDC_START) {
        POINT bolt[6] = {
            {cx + sc(1), cy - s}, {cx - sc(5), cy + sc(1)},
            {cx - sc(1), cy + sc(1)}, {cx - sc(2), cy + s},
            {cx + sc(5), cy - sc(2)}, {cx + sc(1), cy - sc(2)}
        };
        HBRUSH fill = CreateSolidBrush(color);
        SelectObject(dc, fill);
        SelectObject(dc, GetStockObject(NULL_PEN));
        Polygon(dc, bolt, 6);
        SelectObject(dc, old_pen);
        SelectObject(dc, old_brush);
        DeleteObject(fill);
        DeleteObject(pen);
        return 1;
    } else if (id == IDC_BROWSE || id == IDC_OPEN) {
        MoveToEx(dc, cx - s, cy - sc(4), NULL);
        LineTo(dc, cx - sc(2), cy - sc(4));
        LineTo(dc, cx, cy - s);
        LineTo(dc, cx + s, cy - s);
        LineTo(dc, cx + s, cy + sc(5));
        LineTo(dc, cx - s, cy + sc(5));
        LineTo(dc, cx - s, cy - sc(4));
        if (id == IDC_OPEN) {
            MoveToEx(dc, cx, cy + sc(2), NULL);
            LineTo(dc, cx + s, cy - sc(5));
            MoveToEx(dc, cx + sc(2), cy - sc(5), NULL);
            LineTo(dc, cx + s, cy - sc(5));
            LineTo(dc, cx + s, cy + sc(1));
        }
    } else if (id == IDC_RESTORE) {
        Arc(dc, cx - s, cy - s, cx + s, cy + s, cx - s, cy, cx + sc(3), cy - s);
        POINT arrow[3] = {
            {cx - s, cy}, {cx - sc(2), cy - sc(4)}, {cx - sc(2), cy + sc(3)}
        };
        HBRUSH fill = CreateSolidBrush(color);
        SelectObject(dc, fill);
        SelectObject(dc, GetStockObject(NULL_PEN));
        Polygon(dc, arrow, 3);
        SelectObject(dc, pen);
        SelectObject(dc, GetStockObject(HOLLOW_BRUSH));
        DeleteObject(fill);
    } else if (id == IDC_SERVER_TOGGLE) {
        Arc(dc, cx - s, cy - s, cx + s, cy + s, cx - sc(4), cy - sc(4), cx + sc(4), cy - sc(4));
        MoveToEx(dc, cx, cy - s, NULL);
        LineTo(dc, cx, cy + sc(1));
    } else if (id == IDC_API_CONFIG) {
        for (int i = -1; i <= 1; i++) {
            int yy = cy + i * sc(5);
            MoveToEx(dc, cx - s, yy, NULL);
            LineTo(dc, cx + s, yy);
        }
        Ellipse(dc, cx - sc(4), cy - sc(7), cx, cy - sc(3));
        Ellipse(dc, cx + sc(1), cy - sc(2), cx + sc(5), cy + sc(2));
        Ellipse(dc, cx - sc(3), cy + sc(3), cx + sc(1), cy + sc(7));
    } else if (id == IDC_CLEAR_CACHE) {
        Rectangle(dc, cx - sc(5), cy - sc(4), cx + sc(5), cy + s);
        MoveToEx(dc, cx - s, cy - sc(6), NULL);
        LineTo(dc, cx + s, cy - sc(6));
        MoveToEx(dc, cx - sc(2), cy - s, NULL);
        LineTo(dc, cx + sc(2), cy - s);
    } else {
        drawn = 0;
    }

    SelectObject(dc, old_pen);
    SelectObject(dc, old_brush);
    DeleteObject(pen);
    return drawn;
}

/* 自绘按钮：处理 WM_DRAWITEM 消息，绘制渐变圆角按钮
 * 根据按钮类型（主按钮/服务器切换/危险操作/普通）选择配色，
 * 悬停强度 hover（0..1，由 tick_ui_animation 缓动推进）驱动平滑过渡。 */
void draw_button(const DRAWITEMSTRUCT *di) {
    WCHAR text[128];
    GetWindowTextW(di->hwndItem, text, 128);
    int primary = di->CtlID == IDC_START;
    int server_btn = di->CtlID == IDC_SERVER_TOGGLE;
    int restore_btn = di->CtlID == IDC_RESTORE;
    int clear_cache_btn = di->CtlID == IDC_CLEAR_CACHE;
    int pressed = di->itemState & ODS_SELECTED;
    int focused = di->itemState & ODS_FOCUS;
    int server_running = server_btn && g_server_started;
    float hover = button_hover_value((int)di->CtlID);
    if (pressed) hover = 0.0f;

    COLORREF fill_top, fill_bot, edge, fg;
    if (primary) {
        /* 主按钮：暖金渐变，悬停提亮，按下压暗 */
        fill_top = mix(C_ACCENT, C_CARD, 0.08f + 0.14f * hover);
        fill_bot = mix(C_ACCENT_DARK, C_ACCENT, 0.55f + 0.25f * hover);
        if (pressed) {
            fill_top = C_ACCENT_DARK;
            fill_bot = mix(C_ACCENT_DARK, C_ACCENT_DEEP, 0.55f);
        }
        edge = mix(C_ACCENT_DARK, C_ACCENT, 0.40f);
        fg = RGB(22, 37, 57);
    } else if (server_running) {
        /* 服务器运行中：浅绿状态色 */
        fill_top = mix(RGB(237, 247, 239), RGB(224, 240, 228), hover);
        fill_bot = mix(RGB(230, 242, 232), RGB(214, 234, 219), hover);
        if (pressed) { fill_top = RGB(207, 229, 214); fill_bot = RGB(199, 222, 206); }
        edge = mix(mix(C_LINE, C_GREEN, 0.45f), C_GREEN, hover);
        fg = C_GREEN;
    } else if (restore_btn || clear_cache_btn) {
        /* 危险操作：浅红提示色 */
        fill_top = mix(RGB(255, 249, 245), RGB(253, 237, 231), hover);
        fill_bot = mix(RGB(252, 244, 238), RGB(249, 232, 224), hover);
        if (pressed) { fill_top = RGB(242, 222, 212); fill_bot = RGB(237, 214, 204); }
        edge = mix(mix(C_LINE, C_DANGER, 0.45f), C_DANGER, hover);
        fg = C_DANGER;
    } else {
        /* 普通按钮：暖白底色，悬停时加深语义色边框 */
        fill_top = mix(C_CARD_ELEV, mix(C_CARD_ELEV, C_TEXT, 0.05f), hover);
        fill_bot = mix(mix(C_CARD, C_CARD_ELEV, 0.35f), C_CARD_ELEV, hover);
        if (pressed) { fill_top = C_CARD; fill_bot = mix(C_CARD, C_LINE, 0.4f); }
        COLORREF accent = C_LINE_BRIGHT;
        if (di->CtlID == IDC_API_CONFIG) accent = C_VIOLET;
        else if (di->CtlID == IDC_BROWSE || di->CtlID == IDC_OPEN) accent = C_BLUE;
        edge = pressed ? mix(C_LINE, C_TEXT, 0.15f) : mix(C_LINE, accent, 0.30f + 0.70f * hover);
        fg = C_TEXT;
    }

    /* 圆角渐变填充 + 发丝边框 */
            FillRect(di->hDC, &di->rcItem, g_brush_card);
    int radius = sc(32);
    HRGN rgn = CreateRoundRectRgn(di->rcItem.left, di->rcItem.top,
                                  di->rcItem.right + 1, di->rcItem.bottom + 1, radius, radius);
    if (rgn) {
        int saved = SaveDC(di->hDC);
        SelectClipRgn(di->hDC, rgn);
        draw_vgradient(di->hDC, di->rcItem.left, di->rcItem.top,
                       di->rcItem.right - di->rcItem.left,
                       di->rcItem.bottom - di->rcItem.top, fill_top, fill_bot);
        RestoreDC(di->hDC, saved);
        DeleteObject(rgn);
    } else {
        draw_vgradient(di->hDC, di->rcItem.left, di->rcItem.top,
                       di->rcItem.right - di->rcItem.left,
                       di->rcItem.bottom - di->rcItem.top, fill_top, fill_bot);
    }
    HPEN p = CreatePen(PS_SOLID, 1, edge);
    HGDIOBJ ob = SelectObject(di->hDC, GetStockObject(HOLLOW_BRUSH));
    HGDIOBJ op = SelectObject(di->hDC, p);
    RoundRect(di->hDC, di->rcItem.left, di->rcItem.top, di->rcItem.right, di->rcItem.bottom, radius, radius);
    SelectObject(di->hDC, ob);
    SelectObject(di->hDC, op);
    DeleteObject(p);

    /* 顶部内侧高光，强化玻璃质感（主按钮更明显） */
    {
        COLORREF gloss = primary ? mix(fill_top, C_CARD, 0.35f) : mix(edge, C_CARD, 0.18f);
        HPEN gl = CreatePen(PS_SOLID, 1, gloss);
        HGDIOBJ ogl = SelectObject(di->hDC, gl);
        MoveToEx(di->hDC, di->rcItem.left + radius / 2 + sc(2), di->rcItem.top + sc(1), NULL);
        LineTo(di->hDC, di->rcItem.right - radius / 2 - sc(2), di->rcItem.top + sc(1));
        SelectObject(di->hDC, ogl);
        DeleteObject(gl);
    }

    if (focused && !pressed) {
        HPEN focus_pen = CreatePen(PS_SOLID, 1, mix(edge, C_TEXT, 0.35f));
        HGDIOBJ old_focus = SelectObject(di->hDC, focus_pen);
        HGDIOBJ old_focus_brush = SelectObject(di->hDC, GetStockObject(HOLLOW_BRUSH));
        RoundRect(di->hDC, di->rcItem.left + sc(3), di->rcItem.top + sc(3),
                  di->rcItem.right - sc(3), di->rcItem.bottom - sc(3), sc(7), sc(7));
        SelectObject(di->hDC, old_focus);
        SelectObject(di->hDC, old_focus_brush);
        DeleteObject(focus_pen);
    }

    SetBkMode(di->hDC, TRANSPARENT);
    SetTextColor(di->hDC, fg);
    HFONT button_font = clear_cache_btn ? g_font_small : g_font_body;
    SelectObject(di->hDC, button_font);
    SIZE text_size = {0, 0};
    GetTextExtentPoint32W(di->hDC, text, (int)wcslen(text), &text_size);
    int icon_w = sc(14);
    int gap = sc(7);
    int group_w = icon_w + gap + text_size.cx;
    int available = (di->rcItem.right - di->rcItem.left) - sc(18);
    if (group_w > available) {
        RECT clipped = di->rcItem;
        InflateRect(&clipped, -sc(8), 0);
        DrawTextW(di->hDC, text, -1, &clipped,
                  DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
        return;
    }
    int left = di->rcItem.left + ((di->rcItem.right - di->rcItem.left) - group_w) / 2;
    int offset = pressed ? sc(1) : 0;
    int icon_drawn = draw_button_icon(di->hDC, (int)di->CtlID,
                                      left + icon_w / 2 + offset,
                                      (di->rcItem.top + di->rcItem.bottom) / 2 + offset, fg);
    RECT t = di->rcItem;
    if (icon_drawn) {
        t.left = left + icon_w + gap + offset;
        t.right = t.left + text_size.cx + sc(2);
        DrawTextW(di->hDC, text, -1, &t, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
    } else {
        DrawTextW(di->hDC, text, -1, &t, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
    }
}

/* 弹出文件夹浏览对话框，用户选择游戏根目录后更新路径栏并重新检测引擎 */
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

/* 启动游戏进程：在指定目录中查找可执行文件并用 ShellExecuteW 打开 */
static int launch_game_with_params(const WCHAR *dir, const WCHAR *params) {
    WCHAR exe[MAX_PATH * 4];
    if (!find_exe(dir, exe, MAX_PATH * 4)) {
        append_log(L"未找到游戏 exe。");
        return 0;
    }
    append_log(L"启动游戏：%s", exe);
    if (g_launch_dry_run) {
        write_spawn_plan(L"shell", exe, params, dir);
        return 1;
    }
    HINSTANCE exec_result = ShellExecuteW(g_main, L"open", exe, params, dir, SW_SHOWNORMAL);
    /* ShellExecuteW 返回值 <=32 表示失败（2=文件未找到、5=拒绝访问等） */
    if ((INT_PTR)exec_result <= 32) {
        append_log(L"启动游戏失败：%s（ShellExecuteW 错误码：%d）", exe, (int)(INT_PTR)exec_result);
        return 0;
    }
    return 1;
}

int launch_game(const WCHAR *dir) {
    return launch_game_with_params(dir, NULL);
}

void restore_selected_game(void) {
    if (g_start_flow_running) {
        MessageBoxW(g_main, L"翻译流程正在进行中，请完成后再还原游戏。", L"ds游戏翻译器", MB_ICONWARNING);
        return;
    }
    GetWindowTextW(g_path, g_game, MAX_PATH * 4);
    if (!is_dir(g_game)) {
        MessageBoxW(g_main, L"请先选择游戏目录。", L"ds游戏翻译器", MB_ICONWARNING);
        return;
    }

    Engine engine = detect_engine(g_game);
    refresh_engine();
    if (engine == ENGINE_UNKNOWN) {
        MessageBoxW(g_main, L"无法识别游戏引擎，未执行还原。", L"ds游戏翻译器", MB_ICONWARNING);
        return;
    }

    const WCHAR *message =
        L"请先完全退出游戏，然后再执行还原。\n\n"
        L"此操作只移除启动器部署的翻译文件，不会删除翻译缓存、用户模组或已有的 BepInEx。\n\n"
        L"确定要还原当前游戏吗？";
    if (MessageBoxW(g_main, message, L"还原游戏", MB_ICONWARNING | MB_YESNO | MB_DEFBUTTON2) != IDYES) return;

    save_last_game_dir(g_game);
    append_log(L"开始还原：%s（%s）", g_game, engine_name(engine));
    set_status(L"正在还原游戏...");
    int ok = restore_game(g_game, engine);
    set_status(ok ? L"游戏还原完成。" : L"游戏还原未完成，请查看日志。");
    MessageBoxW(g_main,
        ok ? L"还原完成。翻译缓存、用户模组和已有运行时均已保留。" : L"还原未完全完成，请查看启动器日志。未确认归属的文件已保留。",
        L"还原游戏", ok ? MB_ICONINFORMATION : MB_ICONWARNING);
}

/* 发行版模板可能缺少命令行脚本支持，或忽略编辑器专用的语法检查。
   桥接脚本持有一个私有参数：脚本真正加载后立即退出，从而让一个短暂的
   隐藏进程即可验证该支持是否可用。 */
static int godot_runtime_sidecar_preflight(const WCHAR *dir, const WCHAR *runtime_exe,
                                           const WCHAR *pack, const WCHAR *sig_pack,
                                           const WCHAR *script, int loose_project,
                                           int require_proof) {
    WCHAR exe[MAX_PATH * 4];
    if (!dir || !script || !script[0]) return 0;
    if (runtime_exe && runtime_exe[0]) {
        wcsncpy(exe, runtime_exe, MAX_PATH * 4 - 1);
        exe[MAX_PATH * 4 - 1] = 0;
    } else if (!find_exe(dir, exe, MAX_PATH * 4)) {
        return 0;
    }

    /* 预检结论缓存：引擎/游戏/脚本/补丁任一变化即未命中。
       只有成功结论入缓存——超时等瞬态失败下次启动会重新预检。 */
    WCHAR sig[MAX_PATH * 8];
    godot_preflight_sig_init(sig, MAX_PATH * 8);
    godot_preflight_sig_add_file(sig, MAX_PATH * 8, exe);
    godot_preflight_sig_add_text(sig, MAX_PATH * 8, dir);
    if (sig_pack && sig_pack[0]) godot_preflight_sig_add_file(sig, MAX_PATH * 8, sig_pack);
    else godot_preflight_sig_add_text(sig, MAX_PATH * 8, L"nopack");
    if (wcsncmp(script, L"res://", 6) == 0) godot_preflight_sig_add_text(sig, MAX_PATH * 8, script);
    else godot_preflight_sig_add_file(sig, MAX_PATH * 8, script);
    godot_preflight_sig_add_text(sig, MAX_PATH * 8, loose_project ? L"loose" : L"export");
    godot_preflight_sig_add_text(sig, MAX_PATH * 8, require_proof ? L"proof73" : L"legacy0");
    int cached = godot_preflight_cache_get(L"sidecar", sig);
    if (cached >= 0) {
        append_log(cached
            ? L"Godot: sidecar preflight cache hit (supported)."
            : L"Godot: sidecar preflight cache hit (not supported).");
        return cached;
    }

    WCHAR cmd[MAX_PATH * 12];
    int cmd_ok = 0;
    if (loose_project) {
        cmd_ok = wide_format_checked(
            cmd, MAX_PATH * 12,
            L"\"%s\" --headless --path \"%s\" --script \"res://dst_godot_runtime.gd\" -- --dst-preflight",
            exe, dir);
    } else if (pack && pack[0]) {
        cmd_ok = wide_format_checked(
            cmd, MAX_PATH * 12,
            L"\"%s\" --headless --main-pack \"%s\" --script \"%s\" -- --dst-preflight",
            exe, pack, script);
    } else {
        cmd_ok = wide_format_checked(
            cmd, MAX_PATH * 12,
            L"\"%s\" --headless --script \"%s\" -- --dst-preflight", exe, script);
    }
    if (!cmd_ok) {
        append_log(L"Godot: runtime sidecar preflight command is too long.");
        return 0;
    }

    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    memset(&si, 0, sizeof si);
    memset(&pi, 0, sizeof pi);
    si.cb = sizeof si;
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    if (!CreateProcessW(exe, cmd, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, dir, &si, &pi)) {
        append_log(L"Godot: runtime sidecar preflight could not start. Windows error: %lu", GetLastError());
        return 0;
    }
    CloseHandle(pi.hThread);

    DWORD waited = WaitForSingleObject(pi.hProcess, 5000);
    DWORD exit_code = 1;
    int ok = waited == WAIT_OBJECT_0 && GetExitCodeProcess(pi.hProcess, &exit_code) &&
             exit_code == (require_proof ? 73u : 0u);
    if (waited == WAIT_TIMEOUT) {
        TerminateProcess(pi.hProcess, 1);
        WaitForSingleObject(pi.hProcess, 1000);
        append_log(L"Godot: runtime sidecar preflight timed out.");
    } else if (!ok) {
        append_log(L"Godot: runtime sidecar preflight exited with code %lu.", exit_code);
    }
    CloseHandle(pi.hProcess);
    /* 只有成功结论入缓存；超时/非零退出可能是瞬态或游戏侧问题，不缓存。 */
    if (ok) godot_preflight_cache_put(L"sidecar", sig, 1);
    return ok;
}

/* 某些导出的 Godot 模板会同时拒绝 --main-pack 与 --script。Format 3 补丁包
   把翻译器注册为 autoload，因此匹配的启动器自有可执行文件只需一个私有
   用户参数即可验证该路径可用。 */
static int godot_runtime_autoload_preflight(const WCHAR *dir, const WCHAR *runtime_exe,
                                           const WCHAR *pack) {
    if (!dir || !runtime_exe || !runtime_exe[0]) return 0;

    /* 预检结论缓存（引擎 exe 特征为键；只有成功结论入缓存）。 */
    WCHAR sig[MAX_PATH * 8];
    godot_preflight_sig_init(sig, MAX_PATH * 8);
    godot_preflight_sig_add_file(sig, MAX_PATH * 8, runtime_exe);
    if (pack) godot_preflight_sig_add_file(sig, MAX_PATH * 8, pack);
    godot_preflight_sig_add_text(sig, MAX_PATH * 8, dir);
    godot_preflight_sig_add_text(sig, MAX_PATH * 8, L"autoload-ready-v1");
    int cached = godot_preflight_cache_get(L"autoload", sig);
    if (cached >= 0) {
        append_log(cached
            ? L"Godot: runtime autoload preflight cache hit (supported)."
            : L"Godot: runtime autoload preflight cache hit (not supported).");
        return cached;
    }

    WCHAR cmd[MAX_PATH * 12];
    if (!wide_format_checked(cmd, MAX_PATH * 12,
                             L"\"%s\" --headless -- --dst-autoload-preflight",
                             runtime_exe)) {
        append_log(L"Godot: runtime autoload preflight command is too long.");
        return 0;
    }

    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    memset(&si, 0, sizeof si);
    memset(&pi, 0, sizeof pi);
    si.cb = sizeof si;
    si.dwFlags = STARTF_USESHOWWINDOW | STARTF_USESTDHANDLES;
    si.wShowWindow = SW_HIDE;
    WCHAR temp_dir[MAX_PATH * 4], capture_path[MAX_PATH * 4];
    HANDLE capture = INVALID_HANDLE_VALUE, reader = INVALID_HANDLE_VALUE;
    SECURITY_ATTRIBUTES inherit = {sizeof inherit, NULL, TRUE};
    DWORD temp_len = GetTempPathW(MAX_PATH * 4, temp_dir);
    if (temp_len && temp_len < MAX_PATH * 4 && GetTempFileNameW(temp_dir, L"dsa", 0, capture_path)) {
        capture = CreateFileW(capture_path, GENERIC_WRITE,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, &inherit, CREATE_ALWAYS,
            FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, NULL);
        if (capture != INVALID_HANDLE_VALUE)
            reader = CreateFileW(capture_path, GENERIC_READ,
                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
        else DeleteFileW(capture_path);
    }
    if (capture == INVALID_HANDLE_VALUE || reader == INVALID_HANDLE_VALUE) {
        append_log(L"Godot: runtime autoload preflight output capture failed (Windows error %lu).", GetLastError());
        if (capture != INVALID_HANDLE_VALUE) CloseHandle(capture);
        if (reader != INVALID_HANDLE_VALUE) CloseHandle(reader);
        return 0;
    }
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    si.hStdOutput = capture; si.hStdError = capture;
    if (!CreateProcessW(runtime_exe, cmd, NULL, NULL, TRUE, CREATE_NO_WINDOW,
                        NULL, dir, &si, &pi)) {
        append_log(L"Godot: runtime autoload preflight could not start. Windows error: %lu",
                   GetLastError());
        CloseHandle(reader); CloseHandle(capture);
        return 0;
    }
    CloseHandle(pi.hThread);
    /* _ready 的证明由探测脚本输出；不让 SceneTree.quit 与游戏的后台资源加载竞争。
       独立读取句柄避免与子进程 stdout 共用文件位置。只接受完整证明行且进程仍存活。 */
    ULONGLONG started = GetTickCount64();
    DWORD waited;
    int ok = 0;
    do {
        waited = WaitForSingleObject(pi.hProcess, 50);
        char output[16385]; DWORD got = 0;
        LARGE_INTEGER zero; zero.QuadPart = 0;
        if (!SetFilePointerEx(reader, zero, NULL, FILE_BEGIN) ||
            !ReadFile(reader, output, sizeof(output) - 1, &got, NULL)) {
            append_log(L"Godot: runtime autoload preflight output read failed (Windows error %lu).", GetLastError());
            break;
        }
        output[got] = 0;
        const char *proof = strstr(output, "DST_GODOT_AUTOLOAD_READY_V1");
        if (proof && (proof == output || proof[-1] == '\n')) {
            const char *end = proof + strlen("DST_GODOT_AUTOLOAD_READY_V1");
            if (*end == '\r') end++;
            if (*end == '\n' && waited == WAIT_TIMEOUT) { ok = 1; break; }
        }
    } while (waited == WAIT_TIMEOUT && GetTickCount64() - started < 5000);
    DWORD exit_code = 1;
    /* 游戏进程可能在等待返回后、读取证明时自行退出；这是外部进程生命周期，
       无法由脚本保证。收尾前重新核对，停止失败时再区分自行退出与真正的拒绝。
       已退出的证明作废且不入缓存，日志记录真实退出码或查询错误。 */
    if (waited == WAIT_TIMEOUT) waited = WaitForSingleObject(pi.hProcess, 0);
    if (waited == WAIT_TIMEOUT) {
        if (!TerminateProcess(pi.hProcess, 1)) {
            DWORD stop_error = GetLastError();
            ok = 0;
            if (WaitForSingleObject(pi.hProcess, 0) == WAIT_OBJECT_0) waited = WAIT_OBJECT_0;
            else append_log(L"Godot: runtime autoload preflight probe termination failed (Windows error %lu).", stop_error);
        } else {
            DWORD stopped = WaitForSingleObject(pi.hProcess, 1000);
            if (stopped != WAIT_OBJECT_0) {
                DWORD stop_error = stopped == WAIT_FAILED ? GetLastError() : stopped;
                append_log(L"Godot: runtime autoload preflight probe termination failed (Windows error %lu).", stop_error);
                ok = 0;
            }
        }
        if (waited == WAIT_TIMEOUT) {
            if (ok) append_log(L"Godot: runtime autoload load proof confirmed; independent probe stopped.");
            else append_log(L"Godot: runtime autoload preflight timed out without a load proof.");
        }
    }
    if (waited == WAIT_OBJECT_0) {
        ok = 0;
        if (GetExitCodeProcess(pi.hProcess, &exit_code))
            append_log(L"Godot: runtime autoload preflight exited with code %lu.", exit_code);
        else append_log(L"Godot: runtime autoload preflight exit code read failed (Windows error %lu).", GetLastError());
    } else if (waited != WAIT_TIMEOUT) {
        ok = 0;
        append_log(L"Godot: runtime autoload preflight wait failed (Windows error %lu).", GetLastError());
    }
    CloseHandle(reader); CloseHandle(capture);
    CloseHandle(pi.hProcess);
    /* 只有成功结论入缓存；超时/非零退出可能是瞬态或游戏侧问题，不缓存。 */
    if (ok) godot_preflight_cache_put(L"autoload", sig, 1);
    return ok;
}

/* 进程以非零码退出并不足以证明 --main-pack 不受支持：导出的游戏可能在
   自身的 autoload、DRM、音频或启动代码中无头启动失败。只有明确提及该选项
   的命令行解析诊断才被归类为"拒绝"。其他失败仍会记录在日志中，并允许
   走到真实启动路径，而不是被静默禁用翻译。 */
/* launch_godot_with_pack 中的静态回退只能通过 --main-pack 加载补丁包，而
   某些导出模板会拒绝该参数。sidecar 预检失败只能证明 --main-pack 与
   --script 的组合被拒绝；这个最小探测单独隔离 --main-pack，避免启动器
   拉起一个立即退出的进程。超时意味着引擎已启动到足以继续运行的程度，
   视为支持（与之前的行为一致）。 */
static int godot_main_pack_supported(const WCHAR *dir, const WCHAR *runtime_exe, const WCHAR *pack) {
    WCHAR cmd[MAX_PATH * 12];
    if (!wide_format_checked(cmd, MAX_PATH * 12,
                             L"\"%s\" --headless --main-pack \"%s\" --quit",
                             runtime_exe, pack)) {
        append_log(L"Godot: --main-pack probe command is too long.");
        return 0;
    }

    /* 预检结论缓存：成功（含超时视为支持）与 Godot 明确拒绝都入缓存；
       非选项性启动错误属于结论不确定，不入缓存，下次仍会重新探测。 */
    WCHAR sig[MAX_PATH * 8];
    godot_preflight_sig_init(sig, MAX_PATH * 8);
    godot_preflight_sig_add_file(sig, MAX_PATH * 8, runtime_exe);
    godot_preflight_sig_add_file(sig, MAX_PATH * 8, pack);
    godot_preflight_sig_add_text(sig, MAX_PATH * 8, dir);
    int cached = godot_preflight_cache_get(L"mainpack", sig);
    if (cached >= 0) {
        append_log(cached
            ? L"Godot: --main-pack probe cache hit (supported)."
            : L"Godot: --main-pack probe cache hit (explicitly rejected).");
        return cached;
    }

    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    memset(&si, 0, sizeof si);
    memset(&pi, 0, sizeof pi);
    si.cb = sizeof si;
    si.dwFlags = STARTF_USESHOWWINDOW | STARTF_USESTDHANDLES;
    si.wShowWindow = SW_HIDE;

    WCHAR temp_dir[MAX_PATH * 4], capture_path[MAX_PATH * 4];
    HANDLE capture = INVALID_HANDLE_VALUE;
    SECURITY_ATTRIBUTES inherit;
    memset(&inherit, 0, sizeof inherit);
    inherit.nLength = sizeof inherit;
    inherit.bInheritHandle = TRUE;
    DWORD temp_len = GetTempPathW(MAX_PATH * 4, temp_dir);
    if (temp_len > 0 && temp_len < MAX_PATH * 4 &&
        GetTempFileNameW(temp_dir, L"dsg", 0, capture_path)) {
        capture = CreateFileW(capture_path, GENERIC_READ | GENERIC_WRITE,
                              FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                              &inherit, CREATE_ALWAYS,
                              FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, NULL);
    }
    if (capture == INVALID_HANDLE_VALUE) {
        append_log(L"Godot: --main-pack probe output capture could not be created; treating support as inconclusive (Windows error %lu).",
                   GetLastError());
        return 1;
    }
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    si.hStdOutput = capture;
    si.hStdError = capture;

    if (!CreateProcessW(runtime_exe, cmd, NULL, NULL, TRUE, CREATE_NO_WINDOW,
                        NULL, dir, &si, &pi)) {
        /* 结论不确定：让真实启动路径去尝试并记录它自己的错误。 */
        append_log(L"Godot: --main-pack probe could not start. Windows error: %lu", GetLastError());
        CloseHandle(capture);
        return 1;
    }
    CloseHandle(pi.hThread);

    DWORD waited = WaitForSingleObject(pi.hProcess, 5000);
    DWORD exit_code = 1;
    if (waited == WAIT_TIMEOUT) {
        TerminateProcess(pi.hProcess, 1);
        WaitForSingleObject(pi.hProcess, 1000);
        append_log(L"Godot: --main-pack probe kept running past the deadline; treating --main-pack as supported.");
    }

    char output[16385];
    DWORD got = 0;
    output[0] = 0;
    LARGE_INTEGER zero;
    zero.QuadPart = 0;
    if (SetFilePointerEx(capture, zero, NULL, FILE_BEGIN) &&
        ReadFile(capture, output, (DWORD)sizeof(output) - 1, &got, NULL)) {
        output[got] = 0;
    }
    CloseHandle(capture);

    int exited_nonzero = waited == WAIT_OBJECT_0 &&
                         GetExitCodeProcess(pi.hProcess, &exit_code) && exit_code != 0;
    int rejected = exited_nonzero && godot_output_explicitly_rejects_main_pack(output);
    if (rejected) {
        append_log(L"Godot: --main-pack probe received an explicit option-rejection diagnostic (exit %lu).",
                   exit_code);
    } else if (exited_nonzero) {
        append_log(L"Godot: --main-pack probe exited with code %lu for a non-option startup error; support remains inconclusive and translation launch will still be attempted.",
                   exit_code);
    } else if (waited != WAIT_OBJECT_0 && waited != WAIT_TIMEOUT) {
        append_log(L"Godot: --main-pack probe wait failed; support remains inconclusive (Windows error %lu).",
                   GetLastError());
    }
    CloseHandle(pi.hProcess);
    /* 确定性结论入缓存：Godot 明确拒绝（不支持）与确认支持（成功/超时保活）。
       非选项性启动错误 = 结论不确定，不入缓存，下次仍会重新探测。 */
    if (rejected) godot_preflight_cache_put(L"mainpack", sig, 0);
    else if (!exited_nonzero) godot_preflight_cache_put(L"mainpack", sig, 1);
    return !rejected;
}

static int launch_godot_with_pack(const WCHAR *dir, const WCHAR *pack) {
    WCHAR exe[MAX_PATH * 4], runtime_exe[MAX_PATH * 4];
    if (!find_exe(dir, exe, MAX_PATH * 4)) {
        append_log(L"Game exe not found.");
        return 0;
    }
    int has_patch_launcher = godot_prepare_patch_launcher(dir, runtime_exe, MAX_PATH * 4);
    if (!has_patch_launcher) {
        wcsncpy(runtime_exe, exe, MAX_PATH * 4 - 1);
        runtime_exe[MAX_PATH * 4 - 1] = 0;
    }

    WCHAR script[MAX_PATH * 4];
    path_join(script, MAX_PATH * 4, dir, L"dst_godot_runtime.gd");
    const WCHAR *runtime_script = script;
    int embedded_runtime_autoload = has_patch_launcher &&
                                    godot_patch_pack_has_runtime_autoload(pack);
    int has_runtime_autoload = embedded_runtime_autoload &&
                               godot_runtime_autoload_preflight(dir, runtime_exe, pack);
    if (embedded_runtime_autoload && !has_runtime_autoload) {
        append_log(L"Godot: runtime autoload preflight failed; trying script-launch compatibility.");
    }
    int embedded_runtime_bridge = godot_patch_pack_has_runtime_sidecar(pack);
    int prepared_runtime_bridge = embedded_runtime_bridge ||
                                  (godot_prepare_runtime_sidecar(dir) && exists_path(script));
    if (embedded_runtime_bridge) runtime_script = L"res://dst_godot_runtime.gd";
    WCHAR source_marker[MAX_PATH * 4];
    int require_proof = path_append_suffix(source_marker, MAX_PATH * 4, pack, L".dst-source") &&
                        exists_path(source_marker);
    int has_runtime_bridge = !has_runtime_autoload && prepared_runtime_bridge &&
                             godot_runtime_sidecar_preflight(
                                 dir, runtime_exe,
                                 has_patch_launcher ? NULL : pack, pack,
                                 runtime_script, 0, require_proof);
    if (prepared_runtime_bridge && !has_runtime_bridge && !has_runtime_autoload) {
        append_log(L"Godot: runtime sidecar preflight failed; falling back to the static launch path.");
    }

    WCHAR cmd[MAX_PATH * 12];
    int cmd_ok = 0;
    if (has_runtime_autoload) {
        cmd_ok = wide_format_checked(cmd, MAX_PATH * 12,
                                     L"\"%s\" --language en", runtime_exe);
    } else if (has_patch_launcher && has_runtime_bridge) {
        cmd_ok = wide_format_checked(cmd, MAX_PATH * 12,
                                     L"\"%s\" --script \"%s\" --language en",
                                     runtime_exe, runtime_script);
    } else if (has_patch_launcher) {
        cmd_ok = wide_format_checked(cmd, MAX_PATH * 12,
                                     L"\"%s\" --language en", runtime_exe);
    } else if (has_runtime_bridge) {
        cmd_ok = wide_format_checked(
            cmd, MAX_PATH * 12,
            L"\"%s\" --main-pack \"%s\" --script \"%s\" --language en",
            runtime_exe, pack, runtime_script);
    } else {
        /* 静态回退只能经 --main-pack 加载补丁包。sidecar preflight（同样带
           --main-pack）已失败时，单独探测该参数；模板拒绝则退回普通启动，
           而不是拉起一个会立即退出的进程。sidecar 本地准备失败（未执行
           preflight）时没有模板信息，保留原有静态尝试。 */
        if (prepared_runtime_bridge && !godot_main_pack_supported(dir, runtime_exe, pack)) {
            append_log(L"Godot: template rejects --main-pack; falling back to a normal launch without the translation patch pack.");
            return 0;
        }
        cmd_ok = wide_format_checked(
            cmd, MAX_PATH * 12,
            L"\"%s\" --main-pack \"%s\" --language en", runtime_exe, pack);
    }
    if (!cmd_ok) {
        append_log(L"Godot: translated launch command is too long; no process was started.");
        return 0;
    }

    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    memset(&si, 0, sizeof si);
    memset(&pi, 0, sizeof pi);
    si.cb = sizeof si;

    append_log((has_runtime_autoload || has_runtime_bridge)
        ? L"Launching Godot export with patch pack and runtime translator: %s"
        : L"Launching Godot export with static patch pack: %s", runtime_exe);
    if (g_launch_dry_run) {
        write_spawn_plan(L"proc", runtime_exe, cmd, dir);
        g_godot_runtime_verified = has_runtime_autoload || (has_runtime_bridge && require_proof);
        return 1;
    }
    if (!CreateProcessW(runtime_exe, cmd, NULL, NULL, FALSE, 0, NULL, dir, &si, &pi)) {
        append_log(L"Godot: failed to launch with patch pack. Windows error: %lu", GetLastError());
        return 0;
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    g_godot_runtime_verified = has_runtime_autoload || (has_runtime_bridge && require_proof);
    return 1;
}

static int launch_godot_export_with_runtime_sidecar(const WCHAR *dir) {
    WCHAR exe[MAX_PATH * 4], script[MAX_PATH * 4];
    if (!find_exe(dir, exe, MAX_PATH * 4)) {
        append_log(L"Game exe not found.");
        return 0;
    }
    if (!godot_prepare_runtime_sidecar(dir)) return 0;
    path_join(script, MAX_PATH * 4, dir, L"dst_godot_runtime.gd");
    if (!exists_path(script) || !godot_runtime_sidecar_preflight(dir, exe, NULL, NULL, script, 0, 1)) {
        append_log(L"Godot: runtime sidecar preflight failed; falling back to the static launch path.");
        return 0;
    }

    WCHAR cmd[MAX_PATH * 12];
    if (!wide_format_checked(cmd, MAX_PATH * 12,
                             L"\"%s\" --script \"%s\" --language en",
                             exe, script)) {
        append_log(L"Godot: export sidecar launch command is too long.");
        return 0;
    }

    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    memset(&si, 0, sizeof si);
    memset(&pi, 0, sizeof pi);
    si.cb = sizeof si;

    append_log(L"Launching Godot export with runtime translator before static patch is ready: %s", exe);
    if (g_launch_dry_run) {
        write_spawn_plan(L"proc", exe, cmd, dir);
        g_godot_runtime_verified = 1;
        return 1;
    }
    if (!CreateProcessW(exe, cmd, NULL, NULL, FALSE, 0, NULL, dir, &si, &pi)) {
        append_log(L"Godot: failed to launch export runtime sidecar. Windows error: %lu", GetLastError());
        return 0;
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    g_godot_runtime_verified = 1;
    return 1;
}

static int launch_godot_with_runtime_sidecar(const WCHAR *dir) {
    WCHAR exe[MAX_PATH * 4], script[MAX_PATH * 4];
    if (!find_exe(dir, exe, MAX_PATH * 4)) {
        append_log(L"Game exe not found.");
        return 0;
    }
    path_join(script, MAX_PATH * 4, dir, L"dst_godot_runtime.gd");
    if (!exists_path(script) || !godot_runtime_sidecar_preflight(dir, exe, NULL, NULL, script, 1, 1)) {
        append_log(L"Godot: runtime sidecar preflight failed; falling back to the normal launch path.");
        return 0;
    }

    WCHAR cmd[MAX_PATH * 12];
    if (!wide_format_checked(
            cmd, MAX_PATH * 12,
            L"\"%s\" --path \"%s\" --script \"res://dst_godot_runtime.gd\" --language en",
            exe, dir)) {
        append_log(L"Godot: loose-project sidecar launch command is too long.");
        return 0;
    }

    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    memset(&si, 0, sizeof si);
    memset(&pi, 0, sizeof pi);
    si.cb = sizeof si;

    append_log(L"Launching Godot loose project with runtime translator: %s", exe);
    if (g_launch_dry_run) {
        write_spawn_plan(L"proc", exe, cmd, dir);
        g_godot_runtime_verified = 1;
        return 1;
    }
    if (!CreateProcessW(exe, cmd, NULL, NULL, FALSE, 0, NULL, dir, &si, &pi)) {
        append_log(L"Godot: failed to launch with runtime sidecar. Windows error: %lu", GetLastError());
        return 0;
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    g_godot_runtime_verified = 1;
    return 1;
}

static int launch_game_for_engine(const WCHAR *dir, Engine engine) {
    if (engine == ENGINE_GODOT) {
        g_godot_runtime_verified = 0;
        if (godot_is_loose_project(dir)) {
            if (godot_prepare_runtime_sidecar(dir)) {
                append_log(L"Godot: loose project detected; launching with runtime translation sidecar.");
                if (launch_godot_with_runtime_sidecar(dir)) return 1;
                append_log(L"Godot: runtime-sidecar launch failed; falling back to normal game launch.");
            } else {
                append_log(L"Godot: runtime sidecar could not be prepared; falling back to normal game launch.");
            }
            return launch_game(dir);
        }
        godot_promote_staged_patch_pack(dir);
        WCHAR pack[MAX_PATH * 4];
        path_join(pack, MAX_PATH * 4, dir, L"dst_godot_patch.pck");
        if (exists_path(pack) && godot_patch_source_current(dir, pack)) {
            append_log(L"Godot: launching with external translation patch pack.");
            if (launch_godot_with_pack(dir, pack)) return 1;
            append_log(L"Godot: patch-pack launch failed; falling back to normal game launch.");
        } else {
            if (godot_prepare_runtime_pack(dir, pack, MAX_PATH * 4)) {
                if (launch_godot_with_pack(dir, pack)) return 1;
                append_log(L"Godot: initial runtime-pack launch failed; trying the generic sidecar.");
            }
            append_log(L"Godot: no patch pack yet; using the generic runtime translator for this launch.");
            if (launch_godot_export_with_runtime_sidecar(dir)) return 1;
            append_log(L"Godot: export runtime-sidecar launch failed; falling back to normal game launch.");
        }
    }
    return launch_game(dir);
}

/* 独立的补丁工作进程没有窗口，其 append_log 只会到达 OutputDebugStringW。
   父进程在此额外等待该工作进程并把退出码记入启动器日志，使失败的补丁
   构建保持可见（退出码：2=参数错误，3=服务器未就绪，4=补丁包构建失败）。 */
static DWORD WINAPI godot_patch_worker_watch_thread(LPVOID p) {
    HANDLE process = (HANDLE)p;
    WaitForSingleObject(process, INFINITE);
    DWORD code = 1;
    if (!GetExitCodeProcess(process, &code)) {
        append_log(L"Godot: patch refresh worker exit code unavailable. Windows error: %lu", GetLastError());
    } else if (code == 0) {
        append_log(L"Godot: patch refresh worker finished successfully.");
    } else {
        append_log(L"Godot: patch refresh worker exited with code %lu.", code);
    }
    CloseHandle(process);
    return 0;
}

/* 以隐藏窗口启动 --godot-patch-worker 子进程重建补丁包，并用监视线程记录其退出码 */
static int start_godot_patch_worker(const WCHAR *dir) {
    if (!dir || !dir[0]) return 0;

    WCHAR exe[MAX_PATH * 4];
    if (!GetModuleFileNameW(NULL, exe, MAX_PATH * 4)) return 0;

    WCHAR cmd[MAX_PATH * 12];
    if (!wide_format_checked(cmd, MAX_PATH * 12,
                             L"\"%s\" --godot-patch-worker \"%s\"",
                             exe, dir)) {
        append_log(L"Godot: patch worker command is too long.");
        return 0;
    }

    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    memset(&si, 0, sizeof si);
    memset(&pi, 0, sizeof pi);
    si.cb = sizeof si;
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;

    if (g_launch_dry_run) {
        write_spawn_plan(L"worker", exe, cmd, g_root);
        append_log(L"Godot: patch refresh worker started.");
        return 1;
    }

    DWORD flags = CREATE_NO_WINDOW;
    if (!CreateProcessW(NULL, cmd, NULL, NULL, FALSE, flags, NULL,
                        g_root[0] ? g_root : NULL, &si, &pi)) {
        append_log(L"Godot: failed to start patch refresh worker. Windows error: %lu", GetLastError());
        return 0;
    }

    CloseHandle(pi.hThread);
    HANDLE watch = CreateThread(NULL, 0, godot_patch_worker_watch_thread, pi.hProcess, 0, NULL);
    if (watch) {
        CloseHandle(watch);
    } else {
        append_log(L"Godot: could not monitor the patch refresh worker (Windows error: %lu); its exit code will not be recorded.", GetLastError());
        CloseHandle(pi.hProcess);
    }
    append_log(L"Godot: patch refresh worker started.");
    return 1;
}

/* 预热+启动工作线程参数：传递游戏目录和引擎类型到后台线程 */
typedef struct {
    WCHAR dir[MAX_PATH * 4];
    Engine engine;
} WarmupLaunchArgs;

int run_engine_launch_flow(const WCHAR *dir, Engine engine) {
    int launched = 0;
    if (engine == ENGINE_RENPY) {
        /* Ren'Py 渲染回调从不等待 HTTP；守护线程负责缓存与实时查询，因此
           游戏可以在整本剧本预热的后台排队期间先启动。Unity/XUnity 保持
           先导入后启动：其插件发出的实时查询应当命中已导入的行。 */
        launched = launch_game_for_engine(dir, engine);
        if (!launched) goto launch_failed;
        set_status(L"已启动 · 正在后台预热剧本...");
        warmup_translations(dir, engine);
    } else if (engine == ENGINE_GODOT) {
        WCHAR patch[MAX_PATH * 4];
        path_join(patch, MAX_PATH * 4, dir, L"dst_godot_patch.pck");
        int had_patch = exists_path(patch) && godot_patch_source_current(dir, patch);
        if (had_patch) {
            append_log(L"Godot: existing patch pack found; launching before cache warmup and patch refresh.");
            launched = launch_game_for_engine(dir, engine);
            if (!launched) goto launch_failed;
            set_status(L"Godot: 已启动，正在后台预热缓存...");
        } else {
            append_log(L"Godot: no patch pack yet; launching first, then warming resources for patch preparation.");
            launched = launch_game_for_engine(dir, engine);
            if (!launched) goto launch_failed;
            set_status(L"Godot: 已启动，正在后台准备翻译补丁...");
        }
        /* 本工作线程扫描并入队资源时游戏已经可以响应。在分离式重建之前
           先预热，让 Markdown 对话与编译场景的 BBCode 能先填充运行时
           缓存键。 */
        warmup_translations(dir, engine);
        if (!start_godot_patch_worker(dir)) {
            append_log(had_patch
                ? L"Godot: detached patch refresh did not start; current game continues with the existing pack."
                : L"Godot: detached patch preparation did not start; game was already started normally.");
        }
    } else {
        warmup_translations(dir, engine);
        launched = launch_game_for_engine(dir, engine);
        if (!launched) goto launch_failed;
    }
    if (engine == ENGINE_GODOT && !g_godot_runtime_verified)
        set_status(L"Godot: 游戏已启动，但实时翻译模块未确认加载；请查看日志。");
    else if (engine == ENGINE_UNKNOWN || engine == ENGINE_RPGM_LEGACY)
        set_status(L"游戏已启动；当前引擎未部署实时翻译模块，请查看日志。");
    else if (engine == ENGINE_GODOT)
        set_status(L"已启动 · 本地缓存 + 实时批量 API");
    else set_status(L"游戏已启动；翻译组件已部署，游戏内加载待确认。");
    return 1;

launch_failed:
    set_status(L"游戏启动失败；请查看日志。");
    return 0;
}

/* 本地翻译服务未就绪时取消部署与游戏启动（工作线程与同步回退共用）。 */
static void report_server_start_failure(void) {
    append_log(L"本地翻译服务未就绪，已取消部署和游戏启动。");
    set_status(L"状态：服务器启动失败，未启动游戏");
}

/* 按引擎部署翻译钩子；start_translation 的工作线程、同步回退与
   --deploy-and-exit 诊断子命令共用。 */
int deploy_for_engine(const WCHAR *dir, Engine e) {
    int deployed = 0;
    if (e == ENGINE_RENPY) deployed = deploy_renpy(dir);
    else if (e == ENGINE_RPGM_MV) deployed = deploy_rpgm(dir);
    else if (e == ENGINE_UNITY) deployed = deploy_unity(dir);
    else if (e == ENGINE_UNITY_IL2CPP) deployed = deploy_unity_il2cpp(dir);
    else if (e == ENGINE_GODOT) deployed = deploy_godot(dir);
    else if (e == ENGINE_RPGM_LEGACY) append_log(L"RPGM XP/VX：离线写入器仍待迁移，当前保留本地缓存服务。");
    else append_log(L"未知引擎：只启动服务端和游戏。");
    append_log(deployed ? L"部署完成。" : L"部署跳过或未完成。");
    return deployed;
}

int run_deploy_launch_flow(const WCHAR *dir, Engine engine) {
    int deployed = deploy_for_engine(dir, engine);
    if (!deployed && engine != ENGINE_UNKNOWN && engine != ENGINE_RPGM_LEGACY) {
        append_log(L"翻译组件部署失败，已取消游戏启动；请查看上方具体错误。");
        set_status(L"翻译组件部署失败，未启动游戏；请查看日志。");
        return 0;
    }
    set_status(L"正在预热缓存并启动游戏...");
    return run_engine_launch_flow(dir, engine);
}

/* start_server 的就绪轮询最长 15 秒、deploy 是同步文件 I/O、warmup 扫描
   数十 MB 资源并做同步 HTTP，全部在工作线程执行以保持 UI 响应。launch_game
   使用 ShellExecuteW (COM)，因此 CoInitializeEx。调用的 UI 辅助函数
   （append_log / set_status）经 SendMessage 封送到 UI 线程。 */
static DWORD WINAPI warmup_launch_thread(LPVOID p) {
    WarmupLaunchArgs *a = (WarmupLaunchArgs *)p;
    HRESULT hr = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    if (!start_server()) {
        report_server_start_failure();
    } else {
        run_deploy_launch_flow(a->dir, a->engine);
    }
    if (SUCCEEDED(hr)) CoUninitialize();
    free(a);
    g_start_flow_running = 0;
    return 0;
}

/* 开始翻译主流程入口：验证路径 → 检测引擎 → 创建后台线程执行启动服务、部署钩子、预热和启动游戏
 * start_server 的就绪轮询、deploy 与预热都是同步阻塞操作，全部放到工作线程，
 * 避免 UI 冻结；CreateThread 失败时才回退到 UI 线程同步执行。
 * g_start_flow_running 在流程期间拒绝重复点击以及并发的还原/清缓存/服务器切换。 */
void start_translation(void) {
    if (g_start_flow_running) {
        append_log(L"翻译流程仍在进行中，已忽略重复的开始请求。");
        set_status(L"翻译流程仍在进行中，请稍候。");
        return;
    }
    GetWindowTextW(g_path, g_game, MAX_PATH * 4);
    if (!is_dir(g_game)) {
        MessageBoxW(g_main, L"请先选择游戏目录。", L"ds游戏翻译器", MB_ICONWARNING);
        return;
    }
    save_last_game_dir(g_game);
    Engine e = detect_engine(g_game);
    refresh_engine();
    append_log(L"选择目录：%s", g_game);
    append_log(L"识别引擎：%s", engine_name(e));
    set_status(L"正在启动服务并部署...");
    g_start_flow_running = 1;

    /* 把服务器启动、部署、预热（重 I/O + 同步 HTTP）和游戏启动全部放到
       工作线程，避免 UI 冻结。传入路径的私有副本，防止并发的路径框编辑
       （会经 refresh_engine 重写 g_game）在中途改变它。 */
    WarmupLaunchArgs *args = (WarmupLaunchArgs *)malloc(sizeof *args);
    HANDLE th = NULL;
    if (args) {
        wcsncpy(args->dir, g_game, MAX_PATH * 4);
        args->dir[MAX_PATH * 4 - 1] = 0;
        args->engine = e;
        th = CreateThread(NULL, 0, warmup_launch_thread, args, 0, NULL);
        if (th) {
            CloseHandle(th);
            return;
        }
        free(args); /* CreateThread 失败：回退到同步路径 */
    }
    if (!start_server()) {
        report_server_start_failure();
        g_start_flow_running = 0;
        return;
    }
    run_deploy_launch_flow(g_game, e);
    g_start_flow_running = 0;
}
