/* ================================================================
 * api_config.c — API 配置对话框实现
 * ----------------------------------------------------------------
 * 创建模态窗口：顶部"提供商"下拉预设常见 OpenAI 兼容服务，选中后
 * 自动填入 API 地址与示例模型名（仍可手动修改）；下方是 API 地址、
 * 模型名称、API Key 三个编辑框。保存时仍只写 endpoint/model/key
 * 三个键，ini 结构与服务器契约不变；打开时按当前 endpoint 反查
 * 预设做预选，无匹配显示"自定义"。
 * ================================================================ */

#include "api_config.h"
#include "fsutil.h"
#include "resource.h"
#include "ui.h"

#include <string.h>
#include <wchar.h>

/* 提供商预设表：显示名 / endpoint / 示例模型。仅用于自动填充，不落盘。
   示例模型名可能随平台更新，以各平台当前模型列表为准。
   结构体声明在 api_config.h（C# 侧 ApiConfig.Providers 必须逐项一致）。 */
static const ApiProviderPreset g_providers[] = {
    { L"自定义", NULL, NULL },
    { L"DeepSeek", L"https://api.deepseek.com/v1/chat/completions", L"deepseek-v4-flash" },
    { L"硅基流动 SiliconFlow", L"https://api.siliconflow.cn/v1/chat/completions", L"deepseek-ai/DeepSeek-V3" },
    { L"Moonshot Kimi", L"https://api.moonshot.cn/v1/chat/completions", L"moonshot-v1-8k" },
    { L"智谱 GLM", L"https://open.bigmodel.cn/api/paas/v4/chat/completions", L"glm-4-flash" },
    { L"阿里百炼 DashScope", L"https://dashscope.aliyuncs.com/compatible-mode/v1/chat/completions", L"qwen-plus" },
    { L"OpenAI", L"https://api.openai.com/v1/chat/completions", L"gpt-4o-mini" },
    { L"Claude (OpenAI 兼容)", L"https://api.anthropic.com/v1/chat/completions", L"claude-sonnet-4-5" },
    { L"Gemini (OpenAI 兼容)", L"https://generativelanguage.googleapis.com/v1beta/openai/chat/completions", L"gemini-2.5-flash" },
    { L"Ollama (本地, Key 可留空)", L"http://127.0.0.1:11434/v1/chat/completions", L"qwen2.5:7b" },
    { L"LM Studio (本地, Key 可留空)", L"http://127.0.0.1:1234/v1/chat/completions", L"local-model" },
};
#define API_PROVIDER_COUNT ((int)(sizeof g_providers / sizeof g_providers[0]))

int api_config_preset_count(void) { return API_PROVIDER_COUNT; }

const ApiProviderPreset *api_config_preset(int index) {
    if (index < 0 || index >= API_PROVIDER_COUNT) return NULL;
    return &g_providers[index];
}

/* ----------------------------------------------------------------
 * 数据层：api.ini 的读写与预设反查。对话框与诊断子命令共用同一实现，
 * C# 侧 native/src/launcher_cs/ApiConfig.cs 是它的逐函数镜像。
 * ---------------------------------------------------------------- */
void api_config_load(WCHAR *endpoint, WCHAR *model, WCHAR *key) {
    WCHAR cfg[MAX_PATH * 4];
    get_api_config_path(cfg, MAX_PATH * 4);
    GetPrivateProfileStringW(L"api", L"endpoint", L"https://api.deepseek.com/v1/chat/completions", endpoint, 1024, cfg);
    GetPrivateProfileStringW(L"api", L"model", L"deepseek-v4-flash", model, 256, cfg);
    GetPrivateProfileStringW(L"api", L"key", L"", key, 1024, cfg);
}

int api_config_preset_index(const WCHAR *endpoint) {
    for (int i = 1; i < API_PROVIDER_COUNT; i++) {
        if (g_providers[i].endpoint && wcscmp(endpoint, g_providers[i].endpoint) == 0) return i;
    }
    return 0;
}

int api_config_save(const WCHAR *endpoint, const WCHAR *model, const WCHAR *key) {
    WCHAR cfgdir[MAX_PATH * 4], cfg[MAX_PATH * 4];
    get_config_dir(cfgdir, MAX_PATH * 4);
    ensure_dir(cfgdir);
    get_api_config_path(cfg, MAX_PATH * 4);
    /* 三个键都尝试写入（与原有行为一致）；任一失败则记录日志并返回失败。 */
    int ok_endpoint = WritePrivateProfileStringW(L"api", L"endpoint", endpoint, cfg);
    int ok_model = WritePrivateProfileStringW(L"api", L"model", model, cfg);
    int ok_key = WritePrivateProfileStringW(L"api", L"key", key, cfg);
    if (!ok_endpoint || !ok_model || !ok_key) {
        DWORD err = GetLastError();
        append_log(L"API 配置保存失败：%s（Windows 错误：%lu）", cfg, err);
        return 0;
    }
    append_log(L"API 配置已保存：%s", cfg);
    return 1;
}

/* 对话框内部状态：保存各控件句柄和完成标志 */
typedef struct {
    HWND provider;   /* 提供商下拉框 */
    HWND endpoint;   /* API 地址编辑框 */
    HWND model;      /* 模型名称编辑框 */
    HWND key;        /* API Key 编辑框 */
    int done;        /* 对话框结束标志 */
} ApiDialog;

/* ----------------------------------------------------------------
 * api_wndproc — API 配置窗口的消息处理
 *
 * WM_CREATE：从 INI 文件读取现有配置，填充下拉框与编辑框
 * WM_COMMAND：下拉变化时按预设填充地址/模型；保存按钮将编辑框
 *             内容写回 INI；取消按钮关闭窗口
 * WM_CLOSE：设置完成标志并关闭
 * ---------------------------------------------------------------- */
static LRESULT CALLBACK api_wndproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    ApiDialog *d = (ApiDialog *)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
    switch (msg) {
    case WM_CREATE: {
        d = (ApiDialog *)((CREATESTRUCTW *)lp)->lpCreateParams;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)d);
        /* 从 INI 读取已有配置，若不存在则使用默认值 */
        WCHAR endpoint[1024], model[256], key[1024];
        api_config_load(endpoint, model, key);

        /* 提供商下拉：选中预设即填入地址与示例模型，key 保持不动 */
        CreateWindowW(L"STATIC", L"提供商", WS_CHILD | WS_VISIBLE, 24, 20, 120, 22, hwnd, NULL, g_inst, NULL);
        d->provider = CreateWindowW(L"COMBOBOX", NULL, WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL,
                                    24, 46, 320, 300, hwnd, (HMENU)IDC_API_PROVIDER, g_inst, NULL);
        for (int i = 0; i < API_PROVIDER_COUNT; i++) {
            SendMessageW(d->provider, CB_ADDSTRING, 0, (LPARAM)g_providers[i].name);
        }
        /* 按当前 INI 的 endpoint 反查预设预选；无匹配显示"自定义" */
        SendMessageW(d->provider, CB_SETCURSEL, (WPARAM)api_config_preset_index(endpoint), 0);

        /* 标签 + 编辑框 + 按钮 */
        CreateWindowW(L"STATIC", L"API 地址", WS_CHILD | WS_VISIBLE, 24, 88, 120, 22, hwnd, NULL, g_inst, NULL);
        d->endpoint = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", endpoint, WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
                                      24, 114, 520, 30, hwnd, (HMENU)IDC_API_ENDPOINT, g_inst, NULL);
        CreateWindowW(L"STATIC", L"模型", WS_CHILD | WS_VISIBLE, 24, 158, 120, 22, hwnd, NULL, g_inst, NULL);
        d->model = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", model, WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
                                   24, 184, 320, 30, hwnd, (HMENU)IDC_API_MODEL, g_inst, NULL);
        CreateWindowW(L"STATIC", L"API Key（本地服务可留空）", WS_CHILD | WS_VISIBLE, 24, 228, 300, 22, hwnd, NULL, g_inst, NULL);
        d->key = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", key, WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL | ES_PASSWORD,
                                 24, 254, 520, 30, hwnd, (HMENU)IDC_API_KEY, g_inst, NULL);
        CreateWindowW(L"BUTTON", L"保存", WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON, 318, 310, 104, 34, hwnd, (HMENU)IDC_API_SAVE, g_inst, NULL);
        CreateWindowW(L"BUTTON", L"取消", WS_CHILD | WS_VISIBLE, 440, 310, 104, 34, hwnd, (HMENU)IDC_API_CANCEL, g_inst, NULL);
        /* 应用全局字体 */
        SendMessageW(d->provider, WM_SETFONT, (WPARAM)g_font_body, TRUE);
        SendMessageW(d->endpoint, WM_SETFONT, (WPARAM)g_font_body, TRUE);
        SendMessageW(d->model, WM_SETFONT, (WPARAM)g_font_body, TRUE);
        SendMessageW(d->key, WM_SETFONT, (WPARAM)g_font_body, TRUE);
        return 0;
    }
    case WM_COMMAND:
        /* 下拉选中预设：自动填充地址与示例模型（仍可手改）；选"自定义"不动 */
        if (LOWORD(wp) == IDC_API_PROVIDER && HIWORD(wp) == CBN_SELCHANGE && d) {
            int sel = (int)SendMessageW(d->provider, CB_GETCURSEL, 0, 0);
            if (sel > 0 && sel < API_PROVIDER_COUNT && g_providers[sel].endpoint) {
                SetWindowTextW(d->endpoint, g_providers[sel].endpoint);
                SetWindowTextW(d->model, g_providers[sel].model);
            }
            return 0;
        }
        if (LOWORD(wp) == IDC_API_SAVE && d) {
            /* 将编辑框内容写入 INI 文件 */
            WCHAR endpoint[1024], model[256], key[1024];
            GetWindowTextW(d->endpoint, endpoint, 1024);
            GetWindowTextW(d->model, model, 256);
            GetWindowTextW(d->key, key, 1024);
            /* 写入失败时提示并保持对话框打开，以便用户修正后重试。 */
            if (!api_config_save(endpoint, model, key)) {
                set_status(L"API 配置保存失败");
                MessageBoxW(hwnd, L"API 配置写入失败，请检查配置文件目录的写入权限后重试。",
                            L"配置 API", MB_ICONWARNING);
                return 0;
            }
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
        WNDCLASSEXW wc;
        ZeroMemory(&wc, sizeof wc);
        wc.cbSize = sizeof wc;
        wc.lpfnWndProc = api_wndproc;
        wc.hInstance = g_inst;
        wc.hIcon = (HICON)LoadImageW(g_inst, MAKEINTRESOURCEW(IDI_APP_ICON), IMAGE_ICON,
                                    GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON), LR_SHARED);
        wc.hIconSm = (HICON)LoadImageW(g_inst, MAKEINTRESOURCEW(IDI_APP_ICON), IMAGE_ICON,
                                      GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), LR_SHARED);
        wc.hCursor = LoadCursor(NULL, IDC_ARROW);
        wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
        wc.lpszClassName = L"DSTApiConfigDialog";
        RegisterClassExW(&wc);
        registered = 1;
    }
    ApiDialog d;
    ZeroMemory(&d, sizeof d);
    EnableWindow(g_main, FALSE);
    HWND dlg = CreateWindowExW(WS_EX_DLGMODALFRAME, L"DSTApiConfigDialog", L"配置 API",
                               WS_CAPTION | WS_SYSMENU | WS_VISIBLE,
                               CW_USEDEFAULT, CW_USEDEFAULT, 590, 400,
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
