/* ================================================================
 * api_config.c — API 配置对话框实现
 * ----------------------------------------------------------------
 * 创建模态窗口，包含 API 地址、模型名称、API Key 三个编辑框，
 * 用户填写后点"保存"写入 INI 配置文件，供本地 C 服务器启动时读取。
 * ================================================================ */

#include "api_config.h"
#include "fsutil.h"
#include "ui.h"

#include <string.h>

/* 对话框内部状态：保存各控件句柄和完成标志 */
typedef struct {
    HWND endpoint;   /* API 地址编辑框 */
    HWND model;      /* 模型名称编辑框 */
    HWND key;        /* API Key 编辑框 */
    int done;        /* 对话框结束标志 */
} ApiDialog;

/* ----------------------------------------------------------------
 * api_wndproc — API 配置窗口的消息处理
 *
 * WM_CREATE：从 INI 文件读取现有配置并填充编辑框
 * WM_COMMAND：保存按钮将编辑框内容写回 INI；取消按钮关闭窗口
 * WM_CLOSE：设置完成标志并关闭
 * ---------------------------------------------------------------- */
static LRESULT CALLBACK api_wndproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    ApiDialog *d = (ApiDialog *)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
    switch (msg) {
    case WM_CREATE: {
        d = (ApiDialog *)((CREATESTRUCTW *)lp)->lpCreateParams;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)d);
        /* 从 INI 读取已有配置，若不存在则使用默认值 */
        WCHAR cfg[MAX_PATH * 4];
        get_api_config_path(cfg, MAX_PATH * 4);
        WCHAR endpoint[1024], model[256], key[1024];
        GetPrivateProfileStringW(L"api", L"endpoint", L"https://api.deepseek.com/v1/chat/completions", endpoint, 1024, cfg);
        GetPrivateProfileStringW(L"api", L"model", L"deepseek-chat", model, 256, cfg);
        GetPrivateProfileStringW(L"api", L"key", L"", key, 1024, cfg);

        /* 创建标签 + 编辑框 + 按钮 */
        CreateWindowW(L"STATIC", L"API 地址", WS_CHILD | WS_VISIBLE, 24, 24, 120, 22, hwnd, NULL, g_inst, NULL);
        d->endpoint = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", endpoint, WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
                                      24, 50, 520, 30, hwnd, (HMENU)IDC_API_ENDPOINT, g_inst, NULL);
        CreateWindowW(L"STATIC", L"模型", WS_CHILD | WS_VISIBLE, 24, 94, 120, 22, hwnd, NULL, g_inst, NULL);
        d->model = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", model, WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
                                   24, 120, 240, 30, hwnd, (HMENU)IDC_API_MODEL, g_inst, NULL);
        CreateWindowW(L"STATIC", L"API Key", WS_CHILD | WS_VISIBLE, 24, 164, 120, 22, hwnd, NULL, g_inst, NULL);
        d->key = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", key, WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL | ES_PASSWORD,
                                 24, 190, 520, 30, hwnd, (HMENU)IDC_API_KEY, g_inst, NULL);
        CreateWindowW(L"BUTTON", L"保存", WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON, 318, 246, 104, 34, hwnd, (HMENU)IDC_API_SAVE, g_inst, NULL);
        CreateWindowW(L"BUTTON", L"取消", WS_CHILD | WS_VISIBLE, 440, 246, 104, 34, hwnd, (HMENU)IDC_API_CANCEL, g_inst, NULL);
        /* 应用全局字体 */
        SendMessageW(d->endpoint, WM_SETFONT, (WPARAM)g_font_body, TRUE);
        SendMessageW(d->model, WM_SETFONT, (WPARAM)g_font_body, TRUE);
        SendMessageW(d->key, WM_SETFONT, (WPARAM)g_font_body, TRUE);
        return 0;
    }
    case WM_COMMAND:
        if (LOWORD(wp) == IDC_API_SAVE && d) {
            /* 将编辑框内容写入 INI 文件 */
            WCHAR cfgdir[MAX_PATH * 4], cfg[MAX_PATH * 4], endpoint[1024], model[256], key[1024];
            get_config_dir(cfgdir, MAX_PATH * 4);
            ensure_dir(cfgdir);
            get_api_config_path(cfg, MAX_PATH * 4);
            GetWindowTextW(d->endpoint, endpoint, 1024);
            GetWindowTextW(d->model, model, 256);
            GetWindowTextW(d->key, key, 1024);
            WritePrivateProfileStringW(L"api", L"endpoint", endpoint, cfg);
            WritePrivateProfileStringW(L"api", L"model", model, cfg);
            WritePrivateProfileStringW(L"api", L"key", key, cfg);
            append_log(L"API 配置已保存：%s", cfg);
            set_status(L"API 配置已保存");
            d->done = 1;
            DestroyWindow(hwnd);
            return 0;
        }
        if (LOWORD(wp) == IDC_API_CANCEL && d) {
            d->done = 1;
            DestroyWindow(hwnd);
            return 0;
        }
        break;
    case WM_CLOSE:
        if (d) d->done = 1;
        DestroyWindow(hwnd);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

/* ----------------------------------------------------------------
 * show_api_config — 显示 API 配置对话框（模态）
 *
 * 注册窗口类（仅首次），创建窗口并运行局部消息循环，
 * 直到对话框关闭（d.done == 1）。期间禁用主窗口。
 * 对话框关闭后恢复主窗口并重新激活。
 * ---------------------------------------------------------------- */
void show_api_config(void) {
    static int registered = 0;
    if (!registered) {
        WNDCLASSW wc;
        ZeroMemory(&wc, sizeof wc);
        wc.lpfnWndProc = api_wndproc;
        wc.hInstance = g_inst;
        wc.hCursor = LoadCursor(NULL, IDC_ARROW);
        wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
        wc.lpszClassName = L"DSTApiConfigDialog";
        RegisterClassW(&wc);
        registered = 1;
    }
    ApiDialog d;
    ZeroMemory(&d, sizeof d);
    EnableWindow(g_main, FALSE);
    HWND dlg = CreateWindowExW(WS_EX_DLGMODALFRAME, L"DSTApiConfigDialog", L"配置 API",
                               WS_CAPTION | WS_SYSMENU | WS_VISIBLE,
                               CW_USEDEFAULT, CW_USEDEFAULT, 590, 330,
                               g_main, NULL, g_inst, &d);
    if (!dlg) {
        EnableWindow(g_main, TRUE);
        SetForegroundWindow(g_main);
        return;
    }
    ShowWindow(dlg, SW_SHOW);
    MSG m;
    while (!d.done) {
        BOOL got = GetMessageW(&m, NULL, 0, 0);
        if (got == 0) {
            PostQuitMessage((int)m.wParam);
            break;
        }
        if (got < 0) break;
        if (!IsDialogMessageW(dlg, &m)) {
            TranslateMessage(&m);
            DispatchMessageW(&m);
        }
    }
    EnableWindow(g_main, TRUE);
    SetForegroundWindow(g_main);
}
