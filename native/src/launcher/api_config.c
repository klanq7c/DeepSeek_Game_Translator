/* ================================================================
 * api_config.c — API 配置对话框实现
 * ----------------------------------------------------------------
 * 创建模态窗口：顶部"提供商"下拉预设常见 OpenAI 兼容服务，选中后
 * 自动填入 API 地址；模型框从厂商目录刷新可选项，也可手动
 * 输入。网络请求在后台运行，窗口只接收仍匹配当前地址和 Key 的结果。
 * 保存时仍只写 endpoint/model/key
 * 三个键，ini 结构与服务器契约不变；打开时按当前 endpoint 反查
 * 预设做预选，无匹配显示"自定义"。
 * ================================================================ */

#include "api_config.h"
#include "fsutil.h"
#include "resource.h"
#include "ui.h"
#include "api_models.h"

#include <string.h>
#include <wchar.h>
#include <stdlib.h>

#define IDC_API_REFRESH 206
#define IDC_API_MODEL_STATUS 207

/* 工作线程只拥有输入快照和结果，不访问 HWND。关闭窗口释放 UI 引用，
   网络完成后释放线程引用，避免旧窗口结果写入新窗口或悬空栈地址。 */
typedef struct {
    LONG refs;
    WCHAR endpoint[1024], key[1024];
    ApiModels result;
} ModelJob;
static void release_model_job(ModelJob *job) {
    if (InterlockedDecrement(&job->refs) == 0) {
        api_models_free(&job->result);
        SecureZeroMemory(job, sizeof(*job)); free(job);
    }
}
static DWORD WINAPI fetch_models_worker(void *arg) {
    ModelJob *job = arg;
    api_models_fetch(job->endpoint, job->key, &job->result);
    OutputDebugStringW(job->result.message);
    release_model_job(job); return 0;
}

/* 提供商预设只存地址，不维护会过期的模型名或擅自推断“最新”模型。
   结构体声明在 api_config.h（C# 侧 ApiConfig.Providers 必须逐项一致）。 */
static const ApiProviderPreset g_providers[] = {
    { L"自定义", NULL },
    { L"DeepSeek", L"https://api.deepseek.com/v1/chat/completions" },
    { L"硅基流动 SiliconFlow", L"https://api.siliconflow.cn/v1/chat/completions" },
    { L"Moonshot Kimi", L"https://api.moonshot.cn/v1/chat/completions" },
    { L"智谱 GLM", L"https://open.bigmodel.cn/api/paas/v4/chat/completions" },
    { L"阿里百炼 DashScope", L"https://dashscope.aliyuncs.com/compatible-mode/v1/chat/completions" },
    { L"OpenAI", L"https://api.openai.com/v1/chat/completions" },
    { L"Claude (OpenAI 兼容)", L"https://api.anthropic.com/v1/chat/completions" },
    { L"Gemini (OpenAI 兼容)", L"https://generativelanguage.googleapis.com/v1beta/openai/chat/completions" },
    { L"Ollama (本地, Key 可留空)", L"http://127.0.0.1:11434/v1/chat/completions" },
    { L"LM Studio (本地, Key 可留空)", L"http://127.0.0.1:1234/v1/chat/completions" },
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
    GetPrivateProfileStringW(L"api", L"model", L"", model, 256, cfg);
    GetPrivateProfileStringW(L"api", L"key", L"", key, 1024, cfg);
}

int api_config_preset_index(const WCHAR *endpoint) {
    for (int i = 1; i < API_PROVIDER_COUNT; i++) {
        if (g_providers[i].endpoint && wcscmp(endpoint, g_providers[i].endpoint) == 0) return i;
    }
    return 0;
}

static int needs_unicode_ini(const WCHAR *value) {
    if (GetACP() == CP_UTF8) return 0;
    BOOL replaced = FALSE;
    int bytes = WideCharToMultiByte(CP_ACP, WC_NO_BEST_FIT_CHARS, value, -1, NULL, 0, NULL, &replaced);
    return !bytes || replaced;
}

/* Windows Profile API 对无 BOM 的 INI 使用系统代码页。无法表示新值时，只在用户
   保存操作中准备 UTF-16 临时副本，保留其他键和注释，全部写入成功后原子替换。
   外部格式限制不能在厂商处修复；失败保留原文件，并记录阶段和 Windows 错误。 */
static int save_unicode_ini(const WCHAR *cfgdir, const WCHAR *cfg, const WCHAR *endpoint,
                            const WCHAR *model, const WCHAR *key) {
    WCHAR tmp[MAX_PATH] = L""; char *bytes = NULL; WCHAR *wide = NULL;
    DWORD output_size = 0, size = 0, attrs = GetFileAttributesW(cfg), error = 0; int ok = 0;
    const WCHAR *stage = L"读取原配置";
    if (attrs == INVALID_FILE_ATTRIBUTES) {
        error = GetLastError();
        if (error != ERROR_FILE_NOT_FOUND && error != ERROR_PATH_NOT_FOUND) goto done;
    } else {
        if (attrs & (FILE_ATTRIBUTE_READONLY | FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) { error = ERROR_ACCESS_DENIED; goto done; }
        if (!read_file_bytes(cfg, &bytes, &size)) { error = GetLastError(); goto done; }
    }
    stage = L"转换配置编码";
    if (size > 1024 * 1024) { error = ERROR_FILE_TOO_LARGE; goto done; }
    int unicode = size >= 2 && (unsigned char)bytes[0] == 0xff && (unsigned char)bytes[1] == 0xfe;
    output_size = size;
    const char *output = bytes;
    if (!unicode) {
        int chars = size ? MultiByteToWideChar(CP_ACP, MB_ERR_INVALID_CHARS, bytes, (int)size, NULL, 0) : 0;
        if (size && !chars) { error = GetLastError(); goto done; }
        wide = malloc(((size_t)chars + 1) * sizeof(WCHAR));
        if (!wide) { error = ERROR_NOT_ENOUGH_MEMORY; goto done; }
        wide[0] = 0xfeff;
        if (chars) MultiByteToWideChar(CP_ACP, MB_ERR_INVALID_CHARS, bytes, (int)size, wide + 1, chars);
        output = (const char *)wide; output_size = (DWORD)(chars + 1) * sizeof(WCHAR);
    }
    stage = L"写入临时配置";
    if (!GetTempFileNameW(cfgdir, L"dsa", 0, tmp) || !write_file_bytes(tmp, output, output_size)) { error = GetLastError(); goto done; }
    if (!WritePrivateProfileStringW(L"api", L"endpoint", endpoint, tmp) ||
        !WritePrivateProfileStringW(L"api", L"model", model, tmp) ||
        !WritePrivateProfileStringW(L"api", L"key", key, tmp)) { error = GetLastError(); goto done; }
    /* 全空参数的 Profile 缓存刷新按 Windows 契约返回 0，不作为失败判断。 */
    WritePrivateProfileStringW(NULL, NULL, NULL, tmp);
    stage = L"刷新临时配置";
    HANDLE flush = CreateFileW(tmp, GENERIC_WRITE, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (flush == INVALID_HANDLE_VALUE) { error = GetLastError(); goto done; }
    int flushed = FlushFileBuffers(flush);
    error = flushed ? 0 : GetLastError();
    CloseHandle(flush);
    if (!flushed) goto done;
    stage = L"提交配置";
    if (attrs == INVALID_FILE_ATTRIBUTES) ok = MoveFileExW(tmp, cfg, MOVEFILE_WRITE_THROUGH);
    else ok = ReplaceFileW(cfg, tmp, NULL, 0, NULL, NULL);
    if (!ok) error = GetLastError();
done:
    if (!ok) append_log(L"API Unicode 配置保存失败：%s（Windows 错误：%lu）", stage, error);
    if (*tmp && !ok && !DeleteFileW(tmp) && GetLastError() != ERROR_FILE_NOT_FOUND)
        append_log(L"API 临时配置清理失败（Windows 错误：%lu）", GetLastError());
    if (bytes) { SecureZeroMemory(bytes, size); free(bytes); }
    if (wide) { SecureZeroMemory(wide, output_size); free(wide); }
    return ok;
}

int api_config_save(const WCHAR *endpoint, const WCHAR *model, const WCHAR *key) {
    int model_bytes = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, model, -1, NULL, 0, NULL, NULL);
    if (!model_bytes || model_bytes > 256) {
        append_log(L"API 配置保存失败：模型名不是有效 Unicode 或超过 255 字节"); return 0;
    }
    WCHAR cfgdir[MAX_PATH * 4], cfg[MAX_PATH * 4];
    get_config_dir(cfgdir, MAX_PATH * 4);
    ensure_dir(cfgdir);
    get_api_config_path(cfg, MAX_PATH * 4);
    if (needs_unicode_ini(endpoint) || needs_unicode_ini(model) || needs_unicode_ini(key)) {
        if (!save_unicode_ini(cfgdir, cfg, endpoint, model, key)) return 0;
        append_log(L"API 配置已保存：%s", cfg); return 1;
    }
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
    HWND refresh, status;
    HANDLE thread;
    ModelJob *job;
    int changed;
    int catalog_ready;
    SYSTEMTIME fetched_at;
    int probe;
    int done;        /* 对话框结束标志 */
} ApiDialog;

static void clear_model_catalog(ApiDialog *d) {
    WCHAR model[256]; GetWindowTextW(d->model, model, 256);
    d->catalog_ready = 0;
    SendMessageW(d->model, CB_RESETCONTENT, 0, 0); SetWindowTextW(d->model, model);
}
static int catalog_contains(ApiDialog *d, const WCHAR *model) {
    int count = (int)SendMessageW(d->model, CB_GETCOUNT, 0, 0);
    for (int i = 0; i < count; i++) {
        WCHAR item[256];
        SendMessageW(d->model, CB_GETLBTEXT, i, (LPARAM)item);
        if (!wcscmp(item, model)) return 1;
    }
    return 0;
}
static void update_model_status(ApiDialog *d) {
    if (!d->catalog_ready) return;
    WCHAR model[256], status[256]; GetWindowTextW(d->model, model, 256);
    const WCHAR *selection = !*model ? L"请选择聊天模型，也可手动输入。" :
        catalog_contains(d, model) ? L"当前模型在目录中；可自主选择聊天模型。" :
        L"当前模型未在目录中；可重选或手动保存。";
    _snwprintf(status, 256, L"已获取 %d 个模型（%02u:%02u:%02u）。\n%s",
        (int)SendMessageW(d->model, CB_GETCOUNT, 0, 0), d->fetched_at.wHour,
        d->fetched_at.wMinute, d->fetched_at.wSecond, selection);
    SetWindowTextW(d->status, status);
}
/* 目录只证明本次账号可见性，不证明别名失效；不替用户切换模型。 */
static int apply_model_result(ApiDialog *d, const WCHAR *endpoint, const WCHAR *key, const ApiModels *r) {
    WCHAR current_endpoint[1024], current_key[1024], model[256];
    GetWindowTextW(d->endpoint, current_endpoint, 1024); GetWindowTextW(d->key, current_key, 1024);
    int current = !d->changed && !wcscmp(endpoint, current_endpoint) && !wcscmp(key, current_key);
    SecureZeroMemory(current_key, sizeof(current_key));
    if (!current) return 0;
    GetWindowTextW(d->model, model, 256); clear_model_catalog(d);
    for (int i = 0; i < r->count; i++) {
        if (SendMessageW(d->model, CB_ADDSTRING, 0, (LPARAM)r->items[i]) < 0) {
            SetWindowTextW(d->model, model); clear_model_catalog(d);
            append_log(L"模型列表：无法填充模型控件，已丢弃不完整目录");
            SetWindowTextW(d->status, L"模型列表显示失败；可重试或手动输入"); return 1;
        }
    }
    SetWindowTextW(d->model, model);
    d->catalog_ready = r->count > 0;
    GetLocalTime(&d->fetched_at);
    if (d->catalog_ready) update_model_status(d);
    else SetWindowTextW(d->status, r->message);
    return 1;
}
static int has_text(const WCHAR *s) {
    /* 与 C# 的空白判断使用同一 Unicode White_Space 集合。 */
    for (; *s; s++) if (!(*s == 32 || (*s >= 9 && *s <= 13) || *s == 0x85 ||
        *s == 0xa0 || *s == 0x1680 || (*s >= 0x2000 && *s <= 0x200a) ||
        *s == 0x2028 || *s == 0x2029 || *s == 0x202f || *s == 0x205f || *s == 0x3000)) return 1;
    return 0;
}
static int can_save_models(const WCHAR *endpoint, const WCHAR *model) { return has_text(endpoint) && has_text(model); }

static void start_model_refresh(HWND hwnd, ApiDialog *d) {
    if (d->job) return;
    clear_model_catalog(d);
    ModelJob *job = calloc(1, sizeof(*job));
    if (!job) { append_log(L"模型列表：内存分配失败"); SetWindowTextW(d->status, L"模型列表内存分配失败；可手动输入"); return; }
    GetWindowTextW(d->endpoint, job->endpoint, 1024);
    GetWindowTextW(d->key, job->key, 1024);
    job->refs = 2;
    d->thread = CreateThread(NULL, 0, fetch_models_worker, job, 0, NULL);
    if (!d->thread) {
        append_log(L"模型列表：无法启动请求（Windows 错误 %lu）", GetLastError());
        SetWindowTextW(d->status, L"无法启动模型请求；可手动输入");
        release_model_job(job); release_model_job(job); return;
    }
    d->job = job; d->changed = 0;
    EnableWindow(d->refresh, FALSE);
    SetWindowTextW(d->status, L"正在获取厂商模型列表…");
    if (!SetTimer(hwnd, 1, 100, NULL)) {
        append_log(L"模型列表：无法等待请求（Windows 错误 %lu）", GetLastError());
        SetWindowTextW(d->status, L"无法等待请求；可重试或手动输入");
        CloseHandle(d->thread); d->thread = NULL; release_model_job(d->job); d->job = NULL;
        EnableWindow(d->refresh, TRUE);
    }
}

/* ----------------------------------------------------------------
 * api_wndproc — API 配置窗口的消息处理
 *
 * WM_CREATE：从 INI 文件读取现有配置，填充下拉框与编辑框
 * WM_COMMAND：下拉变化时按预设填充地址；保存按钮将编辑框
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

        /* 提供商下拉只填地址，模型由用户从当前目录选择，key 保持不动。 */
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
        d->model = CreateWindowW(L"COMBOBOX", NULL, WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWN | CBS_AUTOHSCROLL | WS_VSCROLL,
                                   24, 184, 360, 240, hwnd, (HMENU)IDC_API_MODEL, g_inst, NULL);
        SendMessageW(d->model, CB_LIMITTEXT, 255, 0);
        SetWindowTextW(d->model, model);
        d->refresh = CreateWindowW(L"BUTTON", L"刷新模型", WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                                   400, 184, 144, 30, hwnd, (HMENU)IDC_API_REFRESH, g_inst, NULL);
        CreateWindowW(L"STATIC", L"API Key（本地服务可留空）", WS_CHILD | WS_VISIBLE, 24, 228, 300, 22, hwnd, NULL, g_inst, NULL);
        d->key = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", key, WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL | ES_PASSWORD,
                                 24, 254, 520, 30, hwnd, (HMENU)IDC_API_KEY, g_inst, NULL);
        d->status = CreateWindowW(L"STATIC", L"填写地址和 Key 后点击刷新；模型也可手动输入", WS_CHILD | WS_VISIBLE,
                                   24, 300, 520, 56, hwnd, (HMENU)IDC_API_MODEL_STATUS, g_inst, NULL);
        CreateWindowW(L"BUTTON", L"保存", WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON, 318, 368, 104, 34, hwnd, (HMENU)IDC_API_SAVE, g_inst, NULL);
        CreateWindowW(L"BUTTON", L"取消", WS_CHILD | WS_VISIBLE, 440, 368, 104, 34, hwnd, (HMENU)IDC_API_CANCEL, g_inst, NULL);
        /* 应用全局字体 */
        SendMessageW(d->provider, WM_SETFONT, (WPARAM)g_font_body, TRUE);
        SendMessageW(d->endpoint, WM_SETFONT, (WPARAM)g_font_body, TRUE);
        SendMessageW(d->model, WM_SETFONT, (WPARAM)g_font_body, TRUE);
        SendMessageW(d->key, WM_SETFONT, (WPARAM)g_font_body, TRUE);
        SendMessageW(d->refresh, WM_SETFONT, (WPARAM)g_font_body, TRUE);
        SendMessageW(d->status, WM_SETFONT, (WPARAM)g_font_body, TRUE);
        SendMessageW(d->endpoint, EM_SETLIMITTEXT, 1023, 0);
        SendMessageW(d->key, EM_SETLIMITTEXT, 1023, 0);
        /* 仅打开已有配置时自动刷新；更换厂商后由用户确认 Key 再点刷新。 */
        if (!d->probe && (*key || api_config_preset_index(endpoint) >= 9)) start_model_refresh(hwnd, d);
        return 0;
    }
    case WM_TIMER:
        if (d && d->job && WaitForSingleObject(d->thread, 0) == WAIT_OBJECT_0) {
            KillTimer(hwnd, 1); CloseHandle(d->thread); d->thread = NULL;
            ModelJob *job = d->job;
            append_log(L"模型列表：%s", job->result.message);
            apply_model_result(d, job->endpoint, job->key, &job->result);
            d->job = NULL; release_model_job(job); EnableWindow(d->refresh, TRUE);
        }
        return 0;
    case WM_COMMAND:
        if (d && LOWORD(wp) == IDC_API_REFRESH) { start_model_refresh(hwnd, d); return 0; }
        if (d && d->status && (LOWORD(wp) == IDC_API_ENDPOINT || LOWORD(wp) == IDC_API_KEY) && HIWORD(wp) == EN_CHANGE) {
            clear_model_catalog(d);
            d->changed = 1;
            SetWindowTextW(d->status, L"地址或 Key 已变化；请刷新模型列表");
        }
        if (d && LOWORD(wp) == IDC_API_MODEL && (HIWORD(wp) == CBN_EDITCHANGE || HIWORD(wp) == CBN_SELCHANGE)) {
            if (HIWORD(wp) == CBN_SELCHANGE) {
                int sel = (int)SendMessageW(d->model, CB_GETCURSEL, 0, 0);
                if (sel >= 0) { WCHAR item[256]; SendMessageW(d->model, CB_GETLBTEXT, sel, (LPARAM)item); SetWindowTextW(d->model, item); }
            }
            update_model_status(d); return 0;
        }
        /* 换地址时清空模型；同地址或“自定义”保留当前选择。 */
        if (LOWORD(wp) == IDC_API_PROVIDER && HIWORD(wp) == CBN_SELCHANGE && d) {
            int sel = (int)SendMessageW(d->provider, CB_GETCURSEL, 0, 0);
            if (sel > 0 && sel < API_PROVIDER_COUNT && g_providers[sel].endpoint) {
                WCHAR endpoint[1024]; GetWindowTextW(d->endpoint, endpoint, 1024);
                if (wcscmp(endpoint, g_providers[sel].endpoint)) {
                    SetWindowTextW(d->endpoint, g_providers[sel].endpoint);
                    SetWindowTextW(d->model, L"");
                }
            }
            return 0;
        }
        if (LOWORD(wp) == IDC_API_SAVE && d) {
            /* 将编辑框内容写入 INI 文件 */
            WCHAR endpoint[1024], model[256], key[1024];
            GetWindowTextW(d->endpoint, endpoint, 1024);
            GetWindowTextW(d->model, model, 256);
            GetWindowTextW(d->key, key, 1024);
            if (!can_save_models(endpoint, model)) {
                append_log(L"API 配置保存被拒绝：地址或模型为空");
                MessageBoxW(hwnd, L"请填写 API 地址并选择或输入模型。", L"配置 API", MB_ICONWARNING);
                return 0;
            }
            /* 写入失败时提示并保持对话框打开，以便用户修正后重试。 */
            if (!api_config_save(endpoint, model, key)) {
                set_status(L"API 配置保存失败");
                MessageBoxW(hwnd, L"API 配置保存失败。模型名最多 255 个 UTF-8 字节；请检查文件权限并查看启动器日志。",
                            L"配置 API", MB_ICONWARNING);
                return 0;
            }
            set_status(L"API 配置已保存；重启翻译服务后生效");
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
    case WM_DESTROY:
        KillTimer(hwnd, 1);
        if (d && d->job) { CloseHandle(d->thread); release_model_job(d->job); d->job = NULL; }
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
static void show_api_config_impl(int probe) {
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
        if (!RegisterClassExW(&wc)) { append_log(L"无法注册 API 配置窗口（Windows 错误 %lu）", GetLastError()); return; }
        registered = 1;
    }
    ApiDialog d;
    ZeroMemory(&d, sizeof d);
    d.probe = probe;
    EnableWindow(g_main, FALSE);
    HWND dlg = CreateWindowExW(WS_EX_DLGMODALFRAME, L"DSTApiConfigDialog", L"配置 API",
                               WS_CAPTION | WS_SYSMENU | (probe ? 0 : WS_VISIBLE),
                               CW_USEDEFAULT, CW_USEDEFAULT, 590, 460,
                               g_main, NULL, g_inst, &d);
    if (!dlg) {
        append_log(L"无法创建 API 配置窗口（Windows 错误 %lu）", GetLastError());
        EnableWindow(g_main, TRUE);
        SetForegroundWindow(g_main);
        return;
    }
    if (probe) {
        /* 逐项登记配置窗口控件，不输出 API Key。探针只在测试目录读取配置。 */
        for (int id = IDC_API_PROVIDER; id <= IDC_API_MODEL_STATUS; id++) {
            HWND ctl = GetDlgItem(dlg, id); RECT rect;
            GetWindowRect(ctl, &rect); MapWindowPoints(NULL, dlg, (POINT *)&rect, 2);
            WCHAR line[128];
            _snwprintf(line, 128, L"control=%d,%ld,%ld,%ld,%ld\n", id, rect.left, rect.top, rect.right - rect.left, rect.bottom - rect.top);
            write_stdout_utf8(line);
        }
        SendMessageW(d.model, CB_ADDSTRING, 0, (LPARAM)L"catalog-new-model");
        SendMessageW(d.model, CB_SETCURSEL, 0, 0);
        WCHAR text[256]; GetWindowTextW(d.model, text, 256);
        write_stdout_utf8(!wcscmp(text, L"catalog-new-model") ? L"select=1\n" : L"select=0\n");
        SetWindowTextW(d.model, L"manual-model"); GetWindowTextW(d.model, text, 256);
        write_stdout_utf8(!wcscmp(text, L"manual-model") ? L"manual=1\n" : L"manual=0\n");
        /* 使用真实控件和结果接收边界验证选择策略，不联网、不保存配置。 */
        #define CHECK_MODEL(name, expr) do { write_stdout_utf8((expr) ? L##name L"=1\n" : L##name L"=0\n"); } while (0)
        #define PICK_PROVIDER(index) do { SendMessageW(d.provider, CB_SETCURSEL, index, 0); \
            SendMessageW(dlg, WM_COMMAND, MAKEWPARAM(IDC_API_PROVIDER, CBN_SELCHANGE), (LPARAM)d.provider); } while (0)
        WCHAR items[2][256] = { L"catalog-future-release", L"catalog-other" };
        ApiModels result = { items, 2, L"fixture" };
        SetWindowTextW(d.endpoint, g_providers[1].endpoint); SetWindowTextW(d.key, L""); d.changed = 0;
        apply_model_result(&d, g_providers[1].endpoint, L"", &result);
        GetWindowTextW(d.model, text, 256);
        CHECK_MODEL("catalog_preserves", !wcscmp(text, L"manual-model"));
        CHECK_MODEL("catalog_new_id", catalog_contains(&d, items[0]));
        GetWindowTextW(d.status, text, 256);
        CHECK_MODEL("catalog_unlisted", wcsstr(text, L"当前模型未在目录中") && can_save_models(g_providers[1].endpoint, L"manual-model"));
        PICK_PROVIDER(1); GetWindowTextW(d.model, text, 256);
        CHECK_MODEL("provider_same_keeps", !wcscmp(text, L"manual-model") && d.catalog_ready);
        PICK_PROVIDER(2); GetWindowTextW(d.model, text, 256);
        CHECK_MODEL("provider_change_clears", !*text && !d.catalog_ready && SendMessageW(d.model, CB_GETCOUNT, 0, 0) == 0);
        SetWindowTextW(d.model, L"manual-custom"); PICK_PROVIDER(0); GetWindowTextW(d.model, text, 256);
        CHECK_MODEL("provider_custom_keeps", !wcscmp(text, L"manual-custom"));
        CHECK_MODEL("empty_model_rejected", !can_save_models(L"url", L"") && !can_save_models(L"url", L" \t\x3000") && !can_save_models(L" ", L"model"));
        SetWindowTextW(d.endpoint, g_providers[1].endpoint); d.changed = 0;
        SetWindowTextW(d.key, L"changed"); SetWindowTextW(d.key, L"");
        CHECK_MODEL("stale_catalog_ignored", !apply_model_result(&d, g_providers[1].endpoint, L"", &result) && !d.catalog_ready);
        int all_clear = 1;
        for (int i = 1; i < API_PROVIDER_COUNT; i++) {
            SetWindowTextW(d.endpoint, L"https://custom.invalid/v1/chat/completions"); SetWindowTextW(d.model, L"old-provider-model");
            PICK_PROVIDER(i); GetWindowTextW(d.model, text, 256);
            if (*text) all_clear = 0;
        }
        CHECK_MODEL("all_providers_clear", all_clear);
        SetWindowTextW(d.endpoint, g_providers[1].endpoint); d.changed = 0;
        apply_model_result(&d, g_providers[1].endpoint, L"", &result);
        SendMessageW(d.model, CB_SETCURSEL, 0, 0);
        SendMessageW(dlg, WM_COMMAND, MAKEWPARAM(IDC_API_MODEL, CBN_SELCHANGE), (LPARAM)d.model);
        GetWindowTextW(d.status, text, 256);
        CHECK_MODEL("catalog_listed", wcsstr(text, L"当前模型在目录中"));
        SetWindowTextW(d.model, L"CATALOG-FUTURE-RELEASE");
        SendMessageW(dlg, WM_COMMAND, MAKEWPARAM(IDC_API_MODEL, CBN_EDITCHANGE), (LPARAM)d.model);
        GetWindowTextW(d.status, text, 256);
        CHECK_MODEL("catalog_case_sensitive", wcsstr(text, L"当前模型未在目录中"));
        result.count = 0; wcscpy(result.message, L"fixture HTTP 401");
        apply_model_result(&d, g_providers[1].endpoint, L"", &result); GetWindowTextW(d.model, text, 256);
        WCHAR status[256]; GetWindowTextW(d.status, status, 256);
        CHECK_MODEL("catalog_failure_keeps", !wcscmp(text, L"CATALOG-FUTURE-RELEASE") && !d.catalog_ready && !wcscmp(status, result.message));
        #undef PICK_PROVIDER
        #undef CHECK_MODEL
        DestroyWindow(dlg); return;
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
void show_api_config(void) { show_api_config_impl(0); }
void api_config_ui_probe(void) { show_api_config_impl(1); }
