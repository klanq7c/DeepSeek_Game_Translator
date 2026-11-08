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
#include "api_connection.h"
#include "dialog_theme.h"

#include <string.h>
#include <wchar.h>
#include <stdlib.h>

#define IDC_API_REFRESH 206
#define IDC_API_MODEL_STATUS 207
#define IDC_API_TEST 212
#define IDC_API_TEST_STATUS 213

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

typedef struct {
    LONG refs;
    WCHAR endpoint[1024], model[256], key[1024];
    ApiConnection result;
} ConnectionJob;
static void release_connection_job(ConnectionJob *job) {
    if (InterlockedDecrement(&job->refs) == 0) { SecureZeroMemory(job, sizeof(*job)); free(job); }
}
static DWORD WINAPI test_connection_worker(void *arg) {
    ConnectionJob *job = arg;
    api_connection_test(job->endpoint, job->model, job->key, &job->result);
    OutputDebugStringW(job->result.message);
    release_connection_job(job); return 0;
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
    HWND test, test_status;
    HANDLE test_thread;
    ConnectionJob *test_job;
    int test_changed;
    DialogTheme theme;
    HANDLE thread;
    ModelJob *job;
    int changed;
    int catalog_ready;
    ApiModels catalog;
    /* 仅在本次窗口内按提供商保留 Key；关闭时清零，取消不写入配置。 */
    WCHAR provider_keys[API_PROVIDER_COUNT][1024];
    SYSTEMTIME fetched_at;
    int probe;
    int done;        /* 对话框结束标志 */
} ApiDialog;

static void clear_model_catalog(ApiDialog *d) {
    WCHAR model[256]; GetWindowTextW(d->model, model, 256);
    d->catalog_ready = 0;
    api_models_free(&d->catalog); ZeroMemory(&d->catalog, sizeof(d->catalog));
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
/* 控件仍持有 API ID，显示名只参与绘制；选择和保存不能把版本标签当作请求 ID。 */
static void model_label(ApiDialog *d, const WCHAR *id, WCHAR *label) {
    wcscpy(label, id);
    for (int i = 0; i < d->catalog.count; i++) {
        const ApiModel *item = &d->catalog.items[i];
        if (!wcscmp(item->id, id) && item->name[0] && wcscmp(item->name, id)) {
            _snwprintf(label, 520, L"%s  (%s)", item->name, id); return;
        }
    }
}
static void update_model_status(ApiDialog *d) {
    if (!d->catalog_ready) return;
    WCHAR model[256], status[768], label[520], listed[600]; GetWindowTextW(d->model, model, 256);
    model_label(d, model, label); _snwprintf(listed, 600, L"当前模型在目录中：%s", label);
    const WCHAR *selection = !*model ? L"请选择聊天模型，也可手动输入。" :
        catalog_contains(d, model) ? listed :
        L"当前模型未在目录中；展开下拉列表选择新模型。";
    _snwprintf(status, 768, L"已获取 %d 个模型（%02u:%02u:%02u）。\n%s",
        (int)SendMessageW(d->model, CB_GETCOUNT, 0, 0), d->fetched_at.wHour,
        d->fetched_at.wMinute, d->fetched_at.wSecond, selection);
    if (d->catalog.invalid_names) _snwprintf(status, 768, L"已获取 %d 个模型；%d 个显示名称无效，已使用模型 ID。\n%s",
        d->catalog.count, d->catalog.invalid_names, selection);
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
    if (r->count) {
        d->catalog.items = malloc((size_t)r->count * sizeof(ApiModel));
        if (!d->catalog.items) {
            append_log(L"模型列表：无法保存显示名称，内存分配失败");
            SetWindowTextW(d->status, L"模型列表内存分配失败；请重试"); return 1;
        }
        memcpy(d->catalog.items, r->items, (size_t)r->count * sizeof(ApiModel));
        d->catalog.count = r->count; d->catalog.invalid_names = r->invalid_names;
    }
    for (int i = 0; i < r->count; i++) {
        if (SendMessageW(d->model, CB_ADDSTRING, 0, (LPARAM)r->items[i].id) < 0) {
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

static void invalidate_connection(ApiDialog *d) {
    d->test_changed = 1;
    if (d->test_status) SetWindowTextW(d->test_status, L"配置已变化，请重新检测。\n检测会发送短请求，可能产生少量费用。");
}
static int apply_connection_result(ApiDialog *d, const ConnectionJob *job) {
    WCHAR endpoint[1024], model[256], key[1024];
    GetWindowTextW(d->endpoint, endpoint, 1024); GetWindowTextW(d->model, model, 256); GetWindowTextW(d->key, key, 1024);
    int current = !d->test_changed && !wcscmp(endpoint, job->endpoint) && !wcscmp(model, job->model) && !wcscmp(key, job->key);
    SecureZeroMemory(key, sizeof(key));
    if (current) SetWindowTextW(d->test_status, job->result.message);
    return current;
}
static void start_connection_test(HWND hwnd, ApiDialog *d) {
    if (d->test_job) return;
    WCHAR endpoint[1024], model[256], key[1024];
    GetWindowTextW(d->endpoint, endpoint, 1024); GetWindowTextW(d->model, model, 256); GetWindowTextW(d->key, key, 1024);
    int preset = api_config_preset_index(endpoint), missing = !*key && preset > 0 && preset < 9;
    SecureZeroMemory(key, sizeof(key));
    if (!can_save_models(endpoint, model) || missing) {
        const WCHAR *message = missing ? L"请先填写当前提供商的 API Key，再检测连通。" : L"请先填写 API 地址和模型 ID，再检测连通。";
        append_log(L"检测连通：%s", message); SetWindowTextW(d->test_status, message); return;
    }
    ConnectionJob *job = calloc(1, sizeof(*job));
    if (!job) { append_log(L"检测连通：内存分配失败"); SetWindowTextW(d->test_status, L"检测内存分配失败；请重试"); return; }
    GetWindowTextW(d->endpoint, job->endpoint, 1024); GetWindowTextW(d->model, job->model, 256); GetWindowTextW(d->key, job->key, 1024);
    job->refs = 2;
    /* 先建立轮询，再启动线程；计时器失败时不会遗留无法跟踪的收费请求。 */
    if (!SetTimer(hwnd, 2, 100, NULL)) {
        append_log(L"检测连通：无法等待请求（Windows 错误 %lu）", GetLastError());
        SetWindowTextW(d->test_status, L"无法等待检测；请重试");
        release_connection_job(job); release_connection_job(job); return;
    }
    d->test_thread = CreateThread(NULL, 0, test_connection_worker, job, 0, NULL);
    if (!d->test_thread) {
        append_log(L"检测连通：无法启动请求（Windows 错误 %lu）", GetLastError());
        SetWindowTextW(d->test_status, L"无法启动检测；请重试"); KillTimer(hwnd, 2);
        release_connection_job(job); release_connection_job(job); return;
    }
    d->test_job = job; d->test_changed = 0; EnableWindow(d->test, FALSE);
    SetWindowTextW(d->test, L"检测中…");
    SetWindowTextW(d->test_status, api_connection_wait_ms(endpoint) > 15000 ?
        L"正在请求本地模型…\n首次加载可能较慢，响应等待上限 120 秒。" : L"正在请求当前模型…\n可继续编辑或关闭窗口。");
}

static void start_model_refresh(HWND hwnd, ApiDialog *d) {
    if (d->job) return;
    WCHAR endpoint[1024], key[1024];
    GetWindowTextW(d->endpoint, endpoint, 1024); GetWindowTextW(d->key, key, 1024);
    int preset = api_config_preset_index(endpoint), missing_key = !*key && preset > 0 && preset < 9;
    SecureZeroMemory(key, sizeof(key));
    if (missing_key) {
        clear_model_catalog(d);
        append_log(L"模型列表：当前提供商缺少 API Key，未发送请求");
        SetWindowTextW(d->status, L"请先填写当前提供商的 API Key，再刷新模型；各厂商 Key 不通用。"); return;
    }
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
/* 所有位置以 96 DPI 表示；标签也登记到探针，防止字号放大后被漏测。 */
typedef struct { int id; const WCHAR *cls, *text; DWORD style; int x, y, w, h; } ApiLayout;
static const ApiLayout api_layout[] = {
    {208, L"STATIC", L"提供商", SS_NOPREFIX, 44, 138, 532, 22},
    {200, L"COMBOBOX", L"", WS_TABSTOP | CBS_DROPDOWNLIST | CBS_OWNERDRAWFIXED | CBS_HASSTRINGS | WS_VSCROLL, 44, 164, 532, 280},
    {209, L"STATIC", L"API 地址", SS_NOPREFIX, 44, 208, 532, 22},
    {201, L"EDIT", L"", WS_TABSTOP | ES_AUTOHSCROLL, 56, 242, 508, 24},
    {210, L"STATIC", L"模型 · 支持手动输入", SS_NOPREFIX, 44, 280, 532, 22},
    {202, L"COMBOBOX", L"", WS_TABSTOP | CBS_DROPDOWN | CBS_AUTOHSCROLL | CBS_OWNERDRAWFIXED | CBS_HASSTRINGS | WS_VSCROLL, 44, 306, 386, 240},
    {206, L"BUTTON", L"刷新模型", WS_TABSTOP | BS_OWNERDRAW, 442, 304, 134, 36},
    {211, L"STATIC", L"API Key · 本地服务可留空", SS_NOPREFIX, 44, 350, 532, 22},
    {203, L"EDIT", L"", WS_TABSTOP | ES_AUTOHSCROLL | ES_PASSWORD, 56, 384, 508, 24},
    {207, L"STATIC", L"", SS_NOPREFIX, 52, 446, 516, 58},
    {212, L"BUTTON", L"检测连通", WS_TABSTOP | BS_OWNERDRAW, 44, 546, 134, 36},
    {213, L"STATIC", L"", SS_NOPREFIX, 196, 540, 380, 58},
    {205, L"BUTTON", L"取消", WS_TABSTOP | BS_OWNERDRAW, 352, 648, 100, 38},
    {204, L"BUTTON", L"保存配置", WS_TABSTOP | BS_OWNERDRAW, 464, 648, 128, 38}
};
#define API_LAYOUT_COUNT ((int)(sizeof api_layout / sizeof api_layout[0]))
static void api_layout_controls(HWND hwnd, ApiDialog *d) {
    for (int i = 0; i < API_LAYOUT_COUNT; i++) {
        const ApiLayout *a = &api_layout[i]; HWND ctl = GetDlgItem(hwnd, a->id);
        SendMessageW(ctl, WM_SETFONT, (WPARAM)d->theme.body, FALSE);
        SetWindowPos(ctl, NULL, dialog_sc(&d->theme, a->x), dialog_sc(&d->theme, a->y),
                     dialog_sc(&d->theme, a->w), dialog_sc(&d->theme, a->h), SWP_NOZORDER | SWP_NOACTIVATE);
    }
    SendMessageW(d->provider, CB_SETITEMHEIGHT, (WPARAM)-1, dialog_sc(&d->theme, 24));
    SendMessageW(d->provider, CB_SETITEMHEIGHT, 0, dialog_sc(&d->theme, 26));
    SendMessageW(d->model, CB_SETITEMHEIGHT, (WPARAM)-1, dialog_sc(&d->theme, 24));
    SendMessageW(d->model, CB_SETITEMHEIGHT, 0, dialog_sc(&d->theme, 26));
    SendMessageW(d->model, CB_SETDROPPEDWIDTH, dialog_sc(&d->theme, 532), 0);
}
static void api_paint(HWND hwnd, HDC dc, ApiDialog *d) {
    DialogTheme *t = &d->theme;
    dialog_paint(hwnd, dc, t, L"配置 API", L"连接翻译服务，让故事继续。 ");
    const int ys[] = {234, 376}; HWND edits[] = {d->endpoint, d->key};
    for (int i = 0; i < 2; i++) {
        RECT field = {dialog_sc(t, 44), dialog_sc(t, ys[i]), dialog_sc(t, 576), dialog_sc(t, ys[i] + 38)};
        dialog_round(dc, field, C_LOG, GetFocus() == edits[i] ? C_ACCENT_DEEP : C_LINE, dialog_sc(t, 10));
    }
    RECT r = {dialog_sc(t, 36), dialog_sc(t, 430), dialog_sc(t, 584), dialog_sc(t, 518)};
    dialog_round(dc, r, C_LOG, C_LINE, dialog_sc(t, 12));
    r = (RECT){dialog_sc(t, 36), dialog_sc(t, 524), dialog_sc(t, 584), dialog_sc(t, 612)};
    dialog_round(dc, r, C_LOG, C_LINE, dialog_sc(t, 12));
    r = (RECT){dialog_sc(t, 28), dialog_sc(t, 646), dialog_sc(t, 340), dialog_sc(t, 692)};
    SelectObject(dc, t->small); SetTextColor(dc, C_TEXT_DIM); SetBkMode(dc, TRANSPARENT);
    DrawTextW(dc, L"保存后，重启翻译服务生效。", -1, &r, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
}

static LRESULT CALLBACK api_wndproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    ApiDialog *d = (ApiDialog *)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
    switch (msg) {
    case WM_CREATE: {
        d = (ApiDialog *)((CREATESTRUCTW *)lp)->lpCreateParams;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)d);
        /* 从 INI 读取已有配置，若不存在则使用默认值 */
        WCHAR endpoint[1024], model[256], key[1024];
        api_config_load(endpoint, model, key);

        /* 表单仍使用原生输入/下拉控件，保留输入法、密码遮罩与辅助功能。 */
        ApiLayout const *a;
        for (int i = 0; i < API_LAYOUT_COUNT; i++) {
            a = &api_layout[i];
            dialog_control(hwnd, &d->theme, a->cls, a->text, a->style, a->id, a->x, a->y, a->w, a->h);
        }
        d->provider = GetDlgItem(hwnd, IDC_API_PROVIDER); d->endpoint = GetDlgItem(hwnd, IDC_API_ENDPOINT);
        d->model = GetDlgItem(hwnd, IDC_API_MODEL); d->key = GetDlgItem(hwnd, IDC_API_KEY);
        d->refresh = GetDlgItem(hwnd, IDC_API_REFRESH); d->status = GetDlgItem(hwnd, IDC_API_MODEL_STATUS);
        d->test = GetDlgItem(hwnd, IDC_API_TEST); d->test_status = GetDlgItem(hwnd, IDC_API_TEST_STATUS);
        api_layout_controls(hwnd, d);
        for (int i = 0; i < API_PROVIDER_COUNT; i++) SendMessageW(d->provider, CB_ADDSTRING, 0, (LPARAM)g_providers[i].name);
        SendMessageW(d->provider, CB_SETCURSEL, (WPARAM)api_config_preset_index(endpoint), 0);
        SendMessageW(d->model, CB_LIMITTEXT, 255, 0);
        SetWindowTextW(d->endpoint, endpoint); SetWindowTextW(d->model, model); SetWindowTextW(d->key, key);
        SetWindowTextW(d->status, L"填写地址和 Key 后点击刷新；模型也可手动输入");
        SetWindowTextW(d->test_status, L"发送一次短请求，可能产生少量费用。\n检测当前草稿，不会自动保存。");
        SendMessageW(d->endpoint, EM_SETLIMITTEXT, 1023, 0);
        SendMessageW(d->key, EM_SETLIMITTEXT, 1023, 0);
        /* 仅打开已有配置时自动刷新；更换厂商后由用户确认 Key 再点刷新。 */
        if (!d->probe && (*key || api_config_preset_index(endpoint) >= 9)) start_model_refresh(hwnd, d);
        return 0;
    }
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps; HDC dc = BeginPaint(hwnd, &ps); api_paint(hwnd, dc, d); EndPaint(hwnd, &ps); return 0;
    }
    case WM_PRINTCLIENT: api_paint(hwnd, (HDC)wp, d); return 0;
    case WM_CTLCOLORSTATIC: return dialog_color((HDC)wp, &d->theme, (HWND)lp == d->status || (HWND)lp == d->test_status);
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORLISTBOX: return dialog_color((HDC)wp, &d->theme, 1);
    case WM_MEASUREITEM: ((MEASUREITEMSTRUCT *)lp)->itemHeight = dialog_sc(&d->theme, 26); return TRUE;
    case WM_DRAWITEM: {
        DRAWITEMSTRUCT *di = (DRAWITEMSTRUCT *)lp; WCHAR label[520], id[256]; const WCHAR *display = NULL;
        if (di->CtlID == IDC_API_MODEL && di->itemID != (UINT)-1 &&
            SendMessageW(di->hwndItem, CB_GETLBTEXT, di->itemID, (LPARAM)id) != CB_ERR) {
            model_label(d, id, label); display = label;
        }
        if (di->CtlType == ODT_COMBOBOX) dialog_combo(di, &d->theme, display);
        else dialog_button(di, &d->theme, di->CtlID == IDC_API_SAVE);
        return TRUE;
    }
    case WM_DPICHANGED: {
        RECT *r = (RECT *)lp; SetWindowPos(hwnd, NULL, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
        dialog_theme_free(&d->theme); dialog_theme_init(&d->theme, d->probe ? HIWORD(wp) : dialog_fit_dpi(hwnd, 620, 706, HIWORD(wp))); api_layout_controls(hwnd, d);
        dialog_place(hwnd, hwnd, 620, 706, d->theme.dpi); InvalidateRect(hwnd, NULL, TRUE); return 0;
    }
    case WM_TIMER:
        if (d && d->test_job && WaitForSingleObject(d->test_thread, 0) == WAIT_OBJECT_0) {
            KillTimer(hwnd, 2); CloseHandle(d->test_thread); d->test_thread = NULL;
            ConnectionJob *job = d->test_job;
            append_log(L"检测连通：%s", job->result.message);
            apply_connection_result(d, job);
            d->test_job = NULL; release_connection_job(job); EnableWindow(d->test, TRUE); SetWindowTextW(d->test, L"检测连通");
        }
        if (d && d->job && WaitForSingleObject(d->thread, 0) == WAIT_OBJECT_0) {
            KillTimer(hwnd, 1); CloseHandle(d->thread); d->thread = NULL;
            ModelJob *job = d->job;
            append_log(L"模型列表：%s", job->result.message);
            apply_model_result(d, job->endpoint, job->key, &job->result);
            d->job = NULL; release_model_job(job); EnableWindow(d->refresh, TRUE);
        }
        return 0;
    case WM_COMMAND:
        if (d && LOWORD(wp) == IDC_API_TEST) { start_connection_test(hwnd, d); return 0; }
        if (d && LOWORD(wp) == IDC_API_REFRESH) { start_model_refresh(hwnd, d); return 0; }
        if (d && d->status && (LOWORD(wp) == IDC_API_ENDPOINT || LOWORD(wp) == IDC_API_KEY) && HIWORD(wp) == EN_CHANGE) {
            invalidate_connection(d);
            clear_model_catalog(d);
            d->changed = 1;
            SetWindowTextW(d->status, L"地址或 Key 已变化；请刷新模型列表");
        }
        if (d && LOWORD(wp) == IDC_API_MODEL && (HIWORD(wp) == CBN_EDITCHANGE || HIWORD(wp) == CBN_SELCHANGE)) {
            invalidate_connection(d);
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
                    int previous = api_config_preset_index(endpoint);
                    if (previous > 0) GetWindowTextW(d->key, d->provider_keys[previous], 1024);
                    SetWindowTextW(d->endpoint, g_providers[sel].endpoint);
                    SetWindowTextW(d->model, L"");
                    SetWindowTextW(d->key, d->provider_keys[sel]);
                    SetWindowTextW(d->status, L"已切换提供商；请确认对应的 API Key 后刷新模型。各厂商 Key 不通用。");
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
                themed_message(hwnd, L"请填写 API 地址并选择或输入模型。", L"配置 API", MB_ICONWARNING);
                return 0;
            }
            /* 写入失败时提示并保持对话框打开，以便用户修正后重试。 */
            if (!api_config_save(endpoint, model, key)) {
                set_status(L"API 配置保存失败");
                themed_message(hwnd, L"API 配置保存失败。模型名最多 255 个 UTF-8 字节；请检查文件权限并查看启动器日志。",
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
        KillTimer(hwnd, 2);
        if (d && d->test_job) { CloseHandle(d->test_thread); release_connection_job(d->test_job); d->test_job = NULL; }
        KillTimer(hwnd, 1);
        if (d) api_models_free(&d->catalog);
        if (d) SecureZeroMemory(d->provider_keys, sizeof(d->provider_keys));
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
        wc.hbrBackground = NULL;
        wc.lpszClassName = L"DSTApiConfigDialog";
        if (!RegisterClassExW(&wc)) { append_log(L"无法注册 API 配置窗口（Windows 错误 %lu）", GetLastError()); return; }
        registered = 1;
    }
    ApiDialog d;
    ZeroMemory(&d, sizeof d);
    d.probe = probe;
    int dpi = probe ? 96 : g_scale_dpi;
    WCHAR dpi_env[16];
    if (probe && GetEnvironmentVariableW(L"DST_API_UI_DPI", dpi_env, 16)) { int n = _wtoi(dpi_env); if (n >= 96 && n <= 240) dpi = n; }
    if (!probe) dpi = dialog_fit_dpi(g_main, 620, 706, dpi);
    dialog_theme_init(&d.theme, dpi);
    int enabled = g_main && IsWindowEnabled(g_main);
    if (enabled) EnableWindow(g_main, FALSE);
    HWND dlg = CreateWindowExW(WS_EX_DLGMODALFRAME, L"DSTApiConfigDialog", L"配置 API",
                               WS_CAPTION | WS_SYSMENU | WS_CLIPCHILDREN,
                               CW_USEDEFAULT, CW_USEDEFAULT, 590, 460,
                               g_main, NULL, g_inst, &d);
    if (!dlg) {
        append_log(L"无法创建 API 配置窗口（Windows 错误 %lu）", GetLastError());
        if (enabled) { EnableWindow(g_main, TRUE); SetForegroundWindow(g_main); }
        dialog_theme_free(&d.theme); return;
    }
    dialog_place(dlg, g_main, 620, 706, dpi); apply_window_chrome(dlg);
    if (probe) {
        /* 检查实际控件的缩放、可达性与密码样式，不输出配置内容。 */
        int bounds = 1, fonts = 1, tabs = 1; RECT client; GetClientRect(dlg, &client);
        for (int i = 0; i < API_LAYOUT_COUNT; i++) {
            HWND ctl = GetDlgItem(dlg, api_layout[i].id); RECT r;
            GetWindowRect(ctl, &r); MapWindowPoints(NULL, dlg, (POINT *)&r, 2);
            if (r.left < 0 || r.top < 0 || r.right > client.right || r.bottom > client.bottom) bounds = 0;
            if ((HFONT)SendMessageW(ctl, WM_GETFONT, 0, 0) != d.theme.body) fonts = 0;
        }
        const int order[] = {200, 201, 202, 206, 203, 212, 205, 204, 200}; HWND previous = NULL;
        for (int i = 0; i < 9; i++) { previous = GetNextDlgTabItem(dlg, previous, FALSE); if (GetDlgCtrlID(previous) != order[i]) tabs = 0; }
        write_stdout_utf8(bounds ? L"bounds=1\n" : L"bounds=0\n");
        write_stdout_utf8(fonts ? L"fonts=1\n" : L"fonts=0\n");
        write_stdout_utf8(tabs ? L"tabs=1\n" : L"tabs=0\n");
        write_stdout_utf8((GetWindowLongW(d.key, GWL_STYLE) & ES_PASSWORD) ? L"password=1\n" : L"password=0\n");
        WCHAR bmp[MAX_PATH * 4];
        DWORD chars = GetEnvironmentVariableW(L"DST_API_UI_BMP", bmp, MAX_PATH * 4);
        if (chars) write_stdout_utf8(chars < MAX_PATH * 4 && dialog_render_probe(dlg, bmp) ? L"render=1\n" : L"render=0\n");
        /* 逐项登记配置窗口控件，不输出 API Key。探针只在测试目录读取配置。 */
        for (int id = IDC_API_PROVIDER; id <= 213; id++) {
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
        ApiModel items[2] = { {L"catalog-future-release", L"Future Model 2099"}, {L"catalog-other", L""} };
        ApiModels result = { items, 2, L"fixture", 0 };
        SetWindowTextW(d.endpoint, g_providers[1].endpoint); SetWindowTextW(d.key, L""); d.changed = 0;
        apply_model_result(&d, g_providers[1].endpoint, L"", &result);
        GetWindowTextW(d.model, text, 256);
        CHECK_MODEL("catalog_preserves", !wcscmp(text, L"manual-model"));
        CHECK_MODEL("catalog_new_id", catalog_contains(&d, items[0].id));
        WCHAR label[520]; model_label(&d, items[0].id, label);
        CHECK_MODEL("catalog_display_name", !wcscmp(label, L"Future Model 2099  (catalog-future-release)"));
        GetWindowTextW(d.status, text, 256);
        CHECK_MODEL("catalog_unlisted", wcsstr(text, L"当前模型未在目录中") && can_save_models(g_providers[1].endpoint, L"manual-model"));
        PICK_PROVIDER(1); GetWindowTextW(d.model, text, 256);
        CHECK_MODEL("provider_same_keeps", !wcscmp(text, L"manual-model") && d.catalog_ready);
        SetWindowTextW(d.key, L"fixture-provider-one-key");
        PICK_PROVIDER(2); GetWindowTextW(d.model, text, 256);
        CHECK_MODEL("provider_change_clears", !*text && !d.catalog_ready && SendMessageW(d.model, CB_GETCOUNT, 0, 0) == 0);
        GetWindowTextW(d.key, text, 256);
        CHECK_MODEL("provider_key_isolated", !*text);
        SendMessageW(dlg, WM_COMMAND, IDC_API_REFRESH, 0);
        GetWindowTextW(d.status, text, 256);
        CHECK_MODEL("provider_missing_key", !d.job && wcsstr(text, L"请先填写当前提供商的 API Key"));
        SetWindowTextW(d.key, L"fixture-provider-two-key"); PICK_PROVIDER(1);
        GetWindowTextW(d.key, text, 256);
        CHECK_MODEL("provider_key_restored", !wcscmp(text, L"fixture-provider-one-key"));
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
        CHECK_MODEL("catalog_selected_name", wcsstr(text, L"Future Model 2099"));
        GetWindowTextW(d.model, text, 256);
        CHECK_MODEL("catalog_selected_id", !wcscmp(text, items[0].id));
        wcscpy(items[0].name, L"Future Model 2100");
        apply_model_result(&d, g_providers[1].endpoint, L"", &result); model_label(&d, items[0].id, label);
        CHECK_MODEL("catalog_refresh_name", !wcscmp(label, L"Future Model 2100  (catalog-future-release)") && SendMessageW(d.model, CB_GETCOUNT, 0, 0) == 2);
        model_label(&d, items[1].id, label);
        CHECK_MODEL("catalog_optional_name", !wcscmp(label, items[1].id));
        SetWindowTextW(d.model, L"CATALOG-FUTURE-RELEASE");
        SendMessageW(dlg, WM_COMMAND, MAKEWPARAM(IDC_API_MODEL, CBN_EDITCHANGE), (LPARAM)d.model);
        GetWindowTextW(d.status, text, 256);
        CHECK_MODEL("catalog_case_sensitive", wcsstr(text, L"当前模型未在目录中"));
        result.count = 0; wcscpy(result.message, L"fixture HTTP 401");
        apply_model_result(&d, g_providers[1].endpoint, L"", &result); GetWindowTextW(d.model, text, 256);
        WCHAR status[256]; GetWindowTextW(d.status, status, 256);
        CHECK_MODEL("catalog_failure_keeps", !wcscmp(text, L"CATALOG-FUTURE-RELEASE") && !d.catalog_ready && !wcscmp(status, result.message));
        ConnectionJob test_result = {0};
        wcscpy(test_result.endpoint, g_providers[1].endpoint); wcscpy(test_result.model, L"CATALOG-FUTURE-RELEASE");
        wcscpy(test_result.result.message, L"fixture connection success");
        SetWindowTextW(d.key, L""); d.test_changed = 0;
        CHECK_MODEL("connection_current", apply_connection_result(&d, &test_result));
        SendMessageW(dlg, WM_COMMAND, MAKEWPARAM(IDC_API_MODEL, CBN_EDITCHANGE), (LPARAM)d.model);
        CHECK_MODEL("connection_model_stale", !apply_connection_result(&d, &test_result));
        d.test_changed = 0; SetWindowTextW(d.key, L"changed"); SetWindowTextW(d.key, L"");
        CHECK_MODEL("connection_key_stale", !apply_connection_result(&d, &test_result));
        d.test_changed = 0; SetWindowTextW(d.endpoint, L"https://fixture.invalid"); SetWindowTextW(d.endpoint, test_result.endpoint);
        CHECK_MODEL("connection_endpoint_stale", !apply_connection_result(&d, &test_result));
        SendMessageW(dlg, WM_COMMAND, IDC_API_TEST, 0); GetWindowTextW(d.test_status, status, 256);
        CHECK_MODEL("connection_missing_key", !d.test_job && wcsstr(status, L"请先填写当前提供商的 API Key"));
        SetWindowTextW(d.model, L""); SetWindowTextW(d.key, L"fixture-key");
        SendMessageW(dlg, WM_COMMAND, IDC_API_TEST, 0); GetWindowTextW(d.test_status, status, 256);
        CHECK_MODEL("connection_missing_model", !d.test_job && wcsstr(status, L"请先填写 API 地址和模型 ID"));
        SetWindowTextW(d.model, L"CATALOG-FUTURE-RELEASE");
        #undef PICK_PROVIDER
        #undef CHECK_MODEL
        RECT before; GetWindowRect(dlg, &before);
        SendMessageW(dlg, WM_DPICHANGED, MAKEWPARAM(144, 144), (LPARAM)&before);
        GetWindowTextW(d.model, text, 256);
        write_stdout_utf8(!wcscmp(text, L"CATALOG-FUTURE-RELEASE") && d.theme.dpi == 144 ? L"dpi_preserves=1\n" : L"dpi_preserves=0\n");
        SendMessageW(dlg, WM_COMMAND, IDC_API_CANCEL, 0);
        write_stdout_utf8(d.done && !IsWindow(dlg) ? L"cancel=1\n" : L"cancel=0\n");
        dialog_theme_free(&d.theme);
        if (enabled) EnableWindow(g_main, TRUE);
        themed_message_probe(dpi);
        return;
    }
    ShowWindow(dlg, SW_SHOW); SetFocus(d.provider);
    MSG m;
    while (!d.done) {
        BOOL got = GetMessageW(&m, NULL, 0, 0);
        if (got == 0) {
            PostQuitMessage((int)m.wParam);
            break;
        }
        if (got < 0) { append_log(L"API 窗口消息循环失败（Windows 错误 %lu）", GetLastError()); break; }
        if (!dialog_key(dlg, &m, IDC_API_SAVE, IDC_API_CANCEL)) {
            TranslateMessage(&m);
            DispatchMessageW(&m);
        }
    }
    if (IsWindow(dlg)) DestroyWindow(dlg);
    dialog_theme_free(&d.theme);
    if (enabled) { EnableWindow(g_main, TRUE); SetForegroundWindow(g_main); }
}
void show_api_config(void) { show_api_config_impl(0); }
void api_config_ui_probe(void) { show_api_config_impl(1); }
