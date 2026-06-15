#include "api_config.h"
#include "fsutil.h"
#include "ui.h"

#include <string.h>

typedef struct {
    HWND endpoint;
    HWND model;
    HWND key;
    int done;
} ApiDialog;

static LRESULT CALLBACK api_wndproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    ApiDialog *d = (ApiDialog *)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
    switch (msg) {
    case WM_CREATE: {
        d = (ApiDialog *)((CREATESTRUCTW *)lp)->lpCreateParams;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)d);
        WCHAR cfg[MAX_PATH * 4];
        get_api_config_path(cfg, MAX_PATH * 4);
        WCHAR endpoint[1024], model[256], key[1024];
        GetPrivateProfileStringW(L"api", L"endpoint", L"https://api.deepseek.com/v1/chat/completions", endpoint, 1024, cfg);
        GetPrivateProfileStringW(L"api", L"model", L"deepseek-chat", model, 256, cfg);
        GetPrivateProfileStringW(L"api", L"key", L"", key, 1024, cfg);

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
        SendMessageW(d->endpoint, WM_SETFONT, (WPARAM)g_font_body, TRUE);
        SendMessageW(d->model, WM_SETFONT, (WPARAM)g_font_body, TRUE);
        SendMessageW(d->key, WM_SETFONT, (WPARAM)g_font_body, TRUE);
        return 0;
    }
    case WM_COMMAND:
        if (LOWORD(wp) == IDC_API_SAVE && d) {
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
