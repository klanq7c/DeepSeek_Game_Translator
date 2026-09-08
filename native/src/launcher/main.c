/* main.c — 启动器 WinMain 入口与窗口消息循环
 * 负责注册窗口类、创建主窗口、处理所有 UI 消息分发。
 * 关闭窗口时不终止翻译服务端，游戏仍可继续请求实时翻译。 */

#include "globals.h"
#include "api_config.h"
#include "deploy.h"
#include "engine.h"
#include "fsutil.h"
#include "godot_patch.h"
#include "godot_preflight_cache.h"
#include "godot_probe.h"
#include "resource.h"
#include "server_proc.h"
#include "self_update.h"
#include "ui.h"
#include "warmup.h"

#include <commctrl.h>
#include <objbase.h>
#include <shellapi.h>
#include <stdio.h>
#include <stdlib.h>
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
        g_brush_card      = CreateSolidBrush(C_CARD);
        g_brush_edit      = CreateSolidBrush(C_LOG);
        g_brush_log       = CreateSolidBrush(C_LOG);
        g_brush_transparent = (HBRUSH)GetStockObject(HOLLOW_BRUSH);

        DWORD static_style = WS_CHILD | WS_VISIBLE | SS_NOPREFIX | SS_ENDELLIPSIS;
        g_title    = CreateWindowW(L"STATIC", L"无感翻译控制台",        static_style, 0,0,0,0, hwnd, (HMENU)IDC_TITLE,    g_inst, NULL);
        g_subtitle = CreateWindowW(L"STATIC", ui_subtitle_text(), static_style, 0,0,0,0, hwnd, (HMENU)IDC_SUBTITLE, g_inst, NULL);
        g_status   = CreateWindowW(L"STATIC", L"STATUS  ·  READY",       static_style, 0,0,0,0, hwnd, (HMENU)IDC_STATUS,   g_inst, NULL);

        g_path_label = CreateWindowW(L"STATIC", L"游戏目录",             static_style, 0,0,0,0, hwnd, (HMENU)IDC_PATH_LABEL, g_inst, NULL);
        g_path       = CreateWindowExW(0, L"EDIT", L"", WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL, 0,0,0,0, hwnd, (HMENU)IDC_PATH, g_inst, NULL);

        CreateWindowW(L"BUTTON", L"浏览",       WS_CHILD | WS_VISIBLE | BS_OWNERDRAW, 0,0,0,0, hwnd, (HMENU)IDC_BROWSE, g_inst, NULL);
        CreateWindowW(L"BUTTON", L"打开目录",   WS_CHILD | WS_VISIBLE | BS_OWNERDRAW, 0,0,0,0, hwnd, (HMENU)IDC_OPEN,   g_inst, NULL);
        CreateWindowW(L"BUTTON", L"开始汉化", WS_CHILD | WS_VISIBLE | BS_OWNERDRAW, 0,0,0,0, hwnd, (HMENU)IDC_START,  g_inst, NULL);
        g_btn_restore = CreateWindowW(L"BUTTON", L"还原游戏",            WS_CHILD | WS_VISIBLE | BS_OWNERDRAW, 0,0,0,0, hwnd, (HMENU)IDC_RESTORE,       g_inst, NULL);
        g_btn_server = CreateWindowW(L"BUTTON", L"启动服务器",          WS_CHILD | WS_VISIBLE | BS_OWNERDRAW, 0,0,0,0, hwnd, (HMENU)IDC_SERVER_TOGGLE, g_inst, NULL);
        g_btn_api    = CreateWindowW(L"BUTTON", L"配置 API",            WS_CHILD | WS_VISIBLE | BS_OWNERDRAW, 0,0,0,0, hwnd, (HMENU)IDC_API_CONFIG,    g_inst, NULL);

        g_engine = CreateWindowW(L"STATIC", L"未选择", static_style, 0,0,0,0, hwnd, (HMENU)IDC_ENGINE, g_inst, NULL);
        g_server = CreateWindowW(L"STATIC", L"未启动", static_style, 0,0,0,0, hwnd, (HMENU)IDC_SERVER, g_inst, NULL);
        g_cache  = CreateWindowW(L"STATIC", L"检查中", static_style, 0,0,0,0, hwnd, (HMENU)IDC_CACHE,  g_inst, NULL);
        g_btn_clear_cache = CreateWindowW(L"BUTTON", L"清除缓存", WS_CHILD | WS_VISIBLE | BS_OWNERDRAW, 0,0,0,0, hwnd, (HMENU)IDC_CLEAR_CACHE, g_inst, NULL);

        g_log = CreateWindowExW(0, L"LISTBOX", L"",
                                WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_HSCROLL |
                                LBS_NOINTEGRALHEIGHT | LBS_DISABLENOSCROLL,
                                0,0,0,0, hwnd, (HMENU)IDC_LOG, g_inst, NULL);

        apply_fonts();
        install_button_hover_tracking(hwnd);
        apply_window_chrome(hwnd);
        /* --ui-probe-and-exit：到此为止就够画一帧了。后面的 payload 同步、缓存卡片、
           日志、上次目录恢复、服务器探测和动画定时器都会随机器状态变化，各自另有
           parity 场景覆盖，探针跳过它们以获得确定的一帧。 */
        if (g_ui_probe) {
            layout(hwnd);
            return 0;
        }
        sync_embedded_payloads();
        update_cache_card();
        append_log(L"原生启动器已就绪。");
        WCHAR last_game[MAX_PATH * 4];
        if (load_last_game_dir(last_game, MAX_PATH * 4)) {
            SetWindowTextW(g_path, last_game);
            refresh_engine();
            append_log(L"已恢复上次游戏目录：%s", last_game);
            append_log(L"直接点击开始汉化即可。");
        } else {
            append_log(L"选择游戏目录后点击开始汉化。");
        }
        refresh_server_status();
        layout(hwnd);
        /* 约 60fps 的动画心跳：驱动数据线光束、呼吸灯与按钮悬停渐变 */
        SetTimer(hwnd, 2, 16, NULL);
        return 0;
    }
    /* ---- WM_PAINT：绘制深色页面背景 ---- */
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        paint_background_buffered(hwnd, dc, &ps.rcPaint);
        EndPaint(hwnd, &ps);
        return 0;
    }
    /* PrintWindow 截屏、远程桌面捕获与辅助功能工具需要完整的客户区渲染，
       即使活动窗口当前只失效了一小块动画区域。父窗口背景绘制完成后，
       DefWindowProc 的 WM_PRINT 遍历会接着打印子控件。 */
    case WM_PRINTCLIENT:
        paint_background(hwnd, (HDC)wp);
        return 0;
    /* ---- WM_SIZE：窗口尺寸变化时重新布局并重绘 ---- */
    case WM_SIZE:
        layout(hwnd);
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
        } else if (wp == 2) {
            tick_ui_animation(hwnd);
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
        RedrawWindow(hwnd, NULL, NULL, RDW_INVALIDATE | RDW_ALLCHILDREN | RDW_NOERASE);
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
        if (ctl == g_path_label || ctl == g_engine || ctl == g_server || ctl == g_cache) {
            /* 卡片内文字底色匹配所在高度的面板渐变，文本框无缝衔接 */
            COLORREF card_bg;
            HBRUSH card_brush = card_text_brush(ctl == g_path_label, &card_bg);
            SetBkMode(dc, OPAQUE);
            SetBkColor(dc, card_bg);
            SetTextColor(dc, ctl == g_path_label ? C_MUTED : C_TEXT);
            return (LRESULT)card_brush;
        }
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, C_TEXT);
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
        else if (LOWORD(wp) == IDC_RESTORE) restore_selected_game();
        else if (LOWORD(wp) == IDC_CLEAR_CACHE) clear_translation_cache();
        else if (LOWORD(wp) == IDC_SERVER_TOGGLE) {
            toggle_server();
            InvalidateRect(hwnd, NULL, FALSE);
        }
        else if (LOWORD(wp) == IDC_API_CONFIG) show_api_config();
        else if (LOWORD(wp) == IDC_OPEN) {
            GetWindowTextW(g_path, g_game, MAX_PATH * 4);
            if (is_dir(g_game)) ShellExecuteW(hwnd, L"open", g_game, NULL, NULL, SW_SHOWNORMAL);
        } else if (LOWORD(wp) == IDC_PATH && HIWORD(wp) == EN_CHANGE) {
        /* 防抖：detect_engine 会扫描目录树，需要合并短时间内的连续触发。 */
            SetTimer(hwnd, 1, 200, NULL);
        }
        return 0;
    /* ---- WM_CLOSE / WM_DESTROY：关闭窗口并释放 GDI 资源 ---- */
    case WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        /* 启动器窗口关闭后仍保持本地翻译服务器运行。游戏启动后会继续请求实时
           翻译；显式服务器开关仍是由用户控制的停止路径。 */
        KillTimer(hwnd, 2);
        DeleteObject(g_font_title);
        DeleteObject(g_font_heading);
        DeleteObject(g_font_body);
        DeleteObject(g_font_small);
        DeleteObject(g_font_mono);
        DeleteObject(g_font_mono_small);
        DeleteObject(g_brush_page);
        DeleteObject(g_brush_card);
        DeleteObject(g_brush_edit);
        DeleteObject(g_brush_log);
        free_card_text_brushes();
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

/* 探针窗口类复用主窗口过程，使被渲染的那一帧走的就是真实的消息处理路径。 */
WNDPROC ui_probe_wndproc(void) {
    return wndproc;
}

/* --ui-probe-and-exit <w> <h> <alive> <out.bmp>：把客户区渲染成确定的一帧。
   --ui-identity-and-exit：打印本二进制真实的运行时标签与副标题（探针会把它们
   中性化以便逐像素比对，真实取值由这里单独暴露给 parity 断言）。 */
static int run_ui_probe_from_cmd(PWSTR cmd) {
    int argc = 0;
    LPWSTR *argv = CommandLineToArgvW(cmd ? cmd : L"", &argc);
    if (!argv) return -1;

    int code = -1;
    for (int i = 0; i < argc; i++) {
        if (wcscmp(argv[i], L"--ui-identity-and-exit") == 0) {
            write_stdout_utf8(L"runtime_tag=");
            write_stdout_utf8(ui_runtime_tag());
            write_stdout_utf8(L"\nsubtitle=");
            write_stdout_utf8(ui_subtitle_text());
            write_stdout_utf8(L"\n");
            code = 0;
            break;
        }
        if (wcscmp(argv[i], L"--ui-probe-and-exit") != 0) continue;
        if (i + 4 >= argc) {
            code = 2;
            break;
        }
        code = run_ui_probe(_wtoi(argv[i + 1]), _wtoi(argv[i + 2]), _wtoi(argv[i + 3]), argv[i + 4]);
        break;
    }

    LocalFree(argv);
    return code;
}

/* wWinMain：程序入口
 * 初始化 DPI 感知 → 获取可执行文件路径 → 初始化 COM/公共控件 →
 * 注册窗口类 → 创建主窗口 → 进入消息循环 */
/* 启动器使用的隐藏辅助模式，用于在游戏已启动后刷新大型 Godot 补丁包。 */
static int run_godot_patch_worker_from_cmd(PWSTR cmd) {
    int argc = 0;
    LPWSTR *argv = CommandLineToArgvW(cmd ? cmd : L"", &argc);
    if (!argv) return -1;

    int handled = 0;
    int code = 0;
    for (int i = 0; i < argc; i++) {
        if (wcscmp(argv[i], L"--godot-patch-worker") != 0) continue;
        handled = 1;
        if (i + 1 >= argc || !argv[i + 1][0]) {
            code = 2;
            break;
        }

        if (!start_server()) {
            code = 3;
            break;
        }

        warmup_translations(argv[i + 1], ENGINE_GODOT);
        WCHAR out[MAX_PATH * 4];
        if (!godot_prepare_patch_pack(argv[i + 1], out, MAX_PATH * 4)) {
            code = 4;
        }
        break;
    }

    LocalFree(argv);
    return handled ? code : -1;
}

/* 诊断模式（配置与 Godot 预检层，不弹窗、不启动进程），字段与
   native/src/launcher_cs/Program.cs 的 --api-config / --api-config-set /
   --godot-probe / --godot-preflight 逐字一致：
 *   --api-config-and-exit                     预设表 + 当前 api.ini 三个键 + 预选下标
 *   --api-config-set-and-exit <e> <m> <k>     写回 api.ini，末行 result=<0|1>
 *   --godot-probe-and-exit <text>...          每个参数一行 reject[i]=<0|1>
 *   --godot-preflight-and-exit <kind> <file> <text> <put>
 *                                             按 file+text 组签名，打印 sig=/get=，
 *                                             put >= 0 时写入该结论后再打印 get2=
 * 参数不足时退出码 2。 */
static int run_config_diagnostic_from_cmd(PWSTR cmd) {
    int argc = 0;
    LPWSTR *argv = CommandLineToArgvW(cmd ? cmd : L"", &argc);
    if (!argv) return -1;

    int handled = 0;
    int code = 0;
    for (int i = 0; i < argc; i++) {
        int api_show = wcscmp(argv[i], L"--api-config-and-exit") == 0;
        int api_set = wcscmp(argv[i], L"--api-config-set-and-exit") == 0;
        int probe = wcscmp(argv[i], L"--godot-probe-and-exit") == 0;
        int preflight = wcscmp(argv[i], L"--godot-preflight-and-exit") == 0;
        if (!api_show && !api_set && !probe && !preflight) continue;
        handled = 1;
        g_log_to_stdout = 1;
        WCHAR line[MAX_PATH * 8];

        if (api_show) {
            _snwprintf(line, MAX_PATH * 8, L"presets=%d\n", api_config_preset_count());
            line[MAX_PATH * 8 - 1] = 0;
            write_stdout_utf8(line);
            for (int p = 0; p < api_config_preset_count(); p++) {
                const ApiProviderPreset *preset = api_config_preset(p);
                _snwprintf(line, MAX_PATH * 8, L"preset%d=%s|%s|%s\n", p, preset->name,
                           preset->endpoint ? preset->endpoint : L"-",
                           preset->model ? preset->model : L"-");
                line[MAX_PATH * 8 - 1] = 0;
                write_stdout_utf8(line);
            }
            WCHAR endpoint[1024], model[256], key[1024];
            api_config_load(endpoint, model, key);
            _snwprintf(line, MAX_PATH * 8, L"endpoint=%s\nmodel=%s\nkey=%s\nselected=%d\nresult=1\n",
                       endpoint, model, key, api_config_preset_index(endpoint));
            line[MAX_PATH * 8 - 1] = 0;
            write_stdout_utf8(line);
            break;
        }
        if (api_set) {
            if (i + 3 >= argc) {
                code = 2;
                break;
            }
            int saved = api_config_save(argv[i + 1], argv[i + 2], argv[i + 3]);
            _snwprintf(line, 64, L"result=%d\n", saved);
            line[63] = 0;
            write_stdout_utf8(line);
            break;
        }
        if (probe) {
            for (int a = i + 1; a < argc; a++) {
                char utf8[4096];
                int need = WideCharToMultiByte(CP_UTF8, 0, argv[a], -1, utf8, (int)sizeof utf8, NULL, NULL);
                _snwprintf(line, 64, L"reject%d=%d\n", a - i - 1,
                           need > 0 ? godot_output_explicitly_rejects_main_pack(utf8) : -1);
                line[63] = 0;
                write_stdout_utf8(line);
            }
            write_stdout_utf8(L"result=1\n");
            break;
        }
        /* preflight */
        if (i + 4 >= argc) {
            code = 2;
            break;
        }
        WCHAR sig[MAX_PATH * 8];
        godot_preflight_sig_init(sig, MAX_PATH * 8);
        godot_preflight_sig_add_file(sig, MAX_PATH * 8, argv[i + 2]);
        godot_preflight_sig_add_text(sig, MAX_PATH * 8, argv[i + 3]);
        write_stdout_utf8(L"sig=");
        write_stdout_utf8(sig);
        write_stdout_utf8(L"\n");
        _snwprintf(line, 64, L"get=%d\n", godot_preflight_cache_get(argv[i + 1], sig));
        line[63] = 0;
        write_stdout_utf8(line);
        int put = _wtoi(argv[i + 4]);
        if (put >= 0) {
            godot_preflight_cache_put(argv[i + 1], sig, put);
            _snwprintf(line, 64, L"get2=%d\n", godot_preflight_cache_get(argv[i + 1], sig));
            line[63] = 0;
            write_stdout_utf8(line);
        }
        write_stdout_utf8(L"result=1\n");
        break;
    }

    LocalFree(argv);
    return handled ? code : -1;
}

/* 诊断模式 --detect-and-exit <dir>：只跑引擎检测并把结果打印到 stdout。 */
static void write_detect_report(const WCHAR *dir) {
    Engine engine = detect_engine(dir);
    WCHAR exe[MAX_PATH * 4] = {0};
    int has_exe = find_exe(dir, exe, MAX_PATH * 4);
    WCHAR rpgm_root[MAX_PATH * 4] = {0};
    int has_rpgm = rpgm_content_root(dir, rpgm_root, MAX_PATH * 4);
    int unity_data = find_subdir_suffix(dir, L"_Data");
    int il2cpp = unity_is_il2cpp(dir);

    WCHAR report[MAX_PATH * 4 * 3];
    _snwprintf(report, MAX_PATH * 4 * 3,
               L"engine=%s\nengine_id=%d\nexe=%s\nrpgm_root=%s\nunity_data=%d\nil2cpp=%d\n",
               engine_name(engine), (int)engine,
               has_exe ? exe : L"-",
               has_rpgm ? rpgm_root : L"-",
               unity_data ? 1 : 0, il2cpp ? 1 : 0);
    report[MAX_PATH * 4 * 3 - 1] = 0;
    write_stdout_utf8(report);
}

/* 诊断模式（不弹窗、不启动服务器、不启动游戏），输出字段与
   native/src/launcher_cs/Program.cs 逐字一致，tests/launcher_parity 用它们对比
   C 与 C# 两版的行为：
 *   --detect-and-exit <dir>   引擎检测报告
 *   --deploy-and-exit <dir>   检测引擎后执行 deploy_for_engine，日志镜像到 stdout，
 *                             末行 result=<deploy 返回值>
 *   --restore-and-exit <dir>  检测引擎后执行 restore_game，末行 result=<返回值>
 *   --warmup-and-exit <dir>   检测引擎后执行 warmup_translations 的扫描部分：不联网、
 *                             不查缓存、不回写文件，每个待提交批次输出
 *                             post=<path> <body>，日志镜像到 stdout，末行 result=0
 *   --godot-patch-and-exit <dir>     godot_prepare_patch_pack，成功时先输出
 *                                    pack=<路径>，末行 result=<返回值>
 *   --godot-promote-and-exit <dir>   godot_promote_staged_patch_pack，末行 result=<返回值>
 *   --godot-launcher-and-exit <dir>  godot_prepare_patch_launcher，成功时先输出
 *                                    launcher=<路径>，末行 result=<返回值>
 *   --launch-flow-and-exit <dir>     检测引擎后执行 run_engine_launch_flow：预热按
 *                                    --warmup-and-exit 的方式转储 post= 行，真正拉起
 *                                    进程的三处改为 spawn=<kind>|<exe>|<cmd>|<cwd>
 *                                    计划行，状态推进镜像为 status= 行，末行 result=1
 *   --clear-cache-and-exit           缓存卡片文本 cache=，删除共享缓存文件后再打印
 *                                    cache2=，末行 result=<是否已清除>（不涉及服务端）
 * 缺少目录参数时退出码 2。 */
static int run_diagnostic_from_cmd(PWSTR cmd) {
    int argc = 0;
    LPWSTR *argv = CommandLineToArgvW(cmd ? cmd : L"", &argc);
    if (!argv) return -1;

    int handled = 0;
    int code = 0;
    for (int i = 0; i < argc; i++) {
        int detect = wcscmp(argv[i], L"--detect-and-exit") == 0;
        int deploy = wcscmp(argv[i], L"--deploy-and-exit") == 0;
        int restore = wcscmp(argv[i], L"--restore-and-exit") == 0;
        int warmup = wcscmp(argv[i], L"--warmup-and-exit") == 0;
        int godot_patch = wcscmp(argv[i], L"--godot-patch-and-exit") == 0;
        int godot_promote = wcscmp(argv[i], L"--godot-promote-and-exit") == 0;
        int godot_launcher = wcscmp(argv[i], L"--godot-launcher-and-exit") == 0;
        int launch_flow = wcscmp(argv[i], L"--launch-flow-and-exit") == 0;
        int clear_cache = wcscmp(argv[i], L"--clear-cache-and-exit") == 0;
        if (!detect && !deploy && !restore && !warmup && !launch_flow && !clear_cache &&
            !godot_patch && !godot_promote && !godot_launcher) continue;
        handled = 1;
        if (clear_cache) {
            /* 缓存卡片文本 + 删除共享缓存文件（不含服务端停止/重启与弹窗）。 */
            g_log_to_stdout = 1;
            WCHAR text[128];
            cache_size_text(text, 128);
            write_stdout_utf8(L"cache=");
            write_stdout_utf8(text);
            write_stdout_utf8(L"\n");
            int cleared = clear_cache_file();
            cache_size_text(text, 128);
            write_stdout_utf8(L"cache2=");
            write_stdout_utf8(text);
            write_stdout_utf8(L"\n");
            WCHAR cc[64];
            _snwprintf(cc, 64, L"result=%d\n", cleared);
            cc[63] = 0;
            write_stdout_utf8(cc);
            break;
        }
        if (i + 1 >= argc || !argv[i + 1][0]) {
            code = 2;
            break;
        }
        const WCHAR *dir = argv[i + 1];
        if (detect) {
            write_detect_report(dir);
            break;
        }
        g_log_to_stdout = 1;
        if (godot_patch || godot_promote || godot_launcher) {
            WCHAR out[MAX_PATH * 4];
            out[0] = 0;
            int godot_result;
            if (godot_patch) {
                godot_result = godot_prepare_patch_pack(dir, out, MAX_PATH * 4);
                if (godot_result) {
                    write_stdout_utf8(L"pack=");
                    write_stdout_utf8(out);
                    write_stdout_utf8(L"\n");
                }
            } else if (godot_launcher) {
                godot_result = godot_prepare_patch_launcher(dir, out, MAX_PATH * 4);
                if (godot_result) {
                    write_stdout_utf8(L"launcher=");
                    write_stdout_utf8(out);
                    write_stdout_utf8(L"\n");
                }
            } else {
                godot_result = godot_promote_staged_patch_pack(dir);
            }
            WCHAR godot_line[64];
            _snwprintf(godot_line, 64, L"result=%d\n", godot_result);
            godot_line[63] = 0;
            write_stdout_utf8(godot_line);
            break;
        }
        Engine engine = detect_engine(dir);
        if (warmup) {
            g_warmup_dump_stdout = 1;
            warmup_translations(dir, engine);
            write_stdout_utf8(L"result=0\n");
            break;
        }
        if (launch_flow) {
            /* 一键流程中"服务器已就绪之后"的部分：预热改为 post= 转储，
               真正拉起进程的三处改为 spawn= 计划行，其余（引擎检测、
               Godot 预检探测、sidecar/补丁包准备）照常真跑。 */
            g_warmup_dump_stdout = 1;
            g_launch_dry_run = 1;
            run_engine_launch_flow(dir, engine);
            write_stdout_utf8(L"result=1\n");
            break;
        }
        int result = deploy ? deploy_for_engine(dir, engine) : restore_game(dir, engine);
        WCHAR line[64];
        _snwprintf(line, 64, L"result=%d\n", result);
        line[63] = 0;
        write_stdout_utf8(line);
        break;
    }

    LocalFree(argv);
    return handled ? code : -1;
}

int WINAPI wWinMain(HINSTANCE h, HINSTANCE prev, PWSTR cmd, int show) {
    (void)prev;
    g_inst = h;
    GetModuleFileNameW(NULL, g_root, MAX_PATH * 4);
    WCHAR *slash = wcsrchr(g_root, L'\\');
    if (slash) *slash = 0;
    int worker_code = run_godot_patch_worker_from_cmd(cmd);
    if (worker_code >= 0) return worker_code;
    int probe_code = run_ui_probe_from_cmd(cmd);
    if (probe_code >= 0) return probe_code;
    int config_code = run_config_diagnostic_from_cmd(cmd);
    if (config_code >= 0) return config_code;
    int diagnostic_code = run_diagnostic_from_cmd(cmd);
    if (diagnostic_code >= 0) return diagnostic_code;
    if (cmd && wcsstr(cmd, L"--sync-payloads-and-exit")) {
        /* 与其他诊断子命令一样把日志镜像到 stdout，并以 result= 收尾，
           供 tests/launcher_parity 与 C# 版 --sync-payloads 逐行对比。 */
        g_log_to_stdout = 1;
        int sync_code = sync_embedded_payloads() ? 0 : 5;
        WCHAR result_line[32];
        _snwprintf(result_line, 32, L"result=%d\n", sync_code);
        result_line[31] = 0;
        write_stdout_utf8(result_line);
        return sync_code;
    }

    dpi_enable_awareness();

    INITCOMMONCONTROLSEX ic = { sizeof(ic), ICC_STANDARD_CLASSES };
    InitCommonControlsEx(&ic);
    CoInitialize(NULL);

    /* 用主显示器的 DPI 决定初始窗口尺寸（窗口之后可能被移动到其他显示器，
       由 WM_DPICHANGED 处理）。 */
    int primary_dpi = 96;
    {
        HDC sdc = GetDC(NULL);
        primary_dpi = GetDeviceCaps(sdc, LOGPIXELSY);
        ReleaseDC(NULL, sdc);
        if (primary_dpi <= 0) primary_dpi = 96;
    }

    WNDCLASSEXW wc;
    ZeroMemory(&wc, sizeof wc);
    wc.cbSize = sizeof wc;
    wc.lpfnWndProc = wndproc;
    wc.hInstance = h;
    wc.hIcon = (HICON)LoadImageW(h, MAKEINTRESOURCEW(IDI_APP_ICON), IMAGE_ICON,
                                GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON), LR_SHARED);
    wc.hIconSm = (HICON)LoadImageW(h, MAKEINTRESOURCEW(IDI_APP_ICON), IMAGE_ICON,
                                  GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), LR_SHARED);
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = NULL;
    wc.lpszClassName = L"DSTNativeLauncher";
    RegisterClassExW(&wc);

    int initW = MulDiv(1200, primary_dpi, 96);
    int initH = MulDiv(780,  primary_dpi, 96);
    g_main = CreateWindowW(wc.lpszClassName, L"ds游戏翻译器", WS_OVERLAPPEDWINDOW | WS_VISIBLE | WS_CLIPCHILDREN,
                           CW_USEDEFAULT, CW_USEDEFAULT, initW, initH, NULL, NULL, h, NULL);
    ShowWindow(g_main, show);
    UpdateWindow(g_main);

    MSG m = {0};
    for (;;) {
        /* GetMessageW 返回 -1 表示出错（如传入无效句柄），0 为 WM_QUIT，两者都退出循环 */
        BOOL got = GetMessageW(&m, NULL, 0, 0);
        if (got <= 0) break;
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
    CoUninitialize();
    return (int)m.wParam;
}
