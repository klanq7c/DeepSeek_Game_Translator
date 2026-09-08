/*
 * api.c —— 远程翻译 API 客户端实现（详见 api.h）。
 *
 * 用 WinHTTP 调用 OpenAI 兼容 chat completions（默认 DeepSeek，可指向任意
 * OpenAI 兼容提供商或本机回环服务），把英文游戏文本译成简体中文。
 * 并发模型：维护一个固定大小的通道池（ApiChannel），每个通道独占一对
 * session/connect 句柄与一把锁。worker 按轮询（round-robin）取一个空闲通道，
 * 通道内串行。这样最多 cfg->concurrency 个请求可同时在飞，且某通道传输失败
 * 只重置该通道，不影响其它通道上正在跑的请求。
 *
 * 翻译结果清洗：normalize_translation 去除模型偶发的 markdown 围栏 / <<<>>> 包裹，
 * 保证回写缓存的是干净译文。
 */
#include "api.h"
#include "buf.h"
#include "json.h"
#include "util.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <windows.h>
#include <winhttp.h>

#define API_MAX_RESPONSE_BYTES (2u * 1024u * 1024u)

static int endpoint_host_boundary(char c) {
    return c == 0 || c == ':' || c == '/' || c == '?' || c == '#';
}

/* 判定端点主机是否为本机回环（localhost/127.0.0.1/[::1]）。
   明文 HTTP 与空 API key 都只允许落在回环端点上。 */
static int endpoint_is_loopback(const char *endpoint) {
    if (!endpoint) return 0;
    const char *host = strstr(endpoint, "://");
    host = host ? host + 3 : endpoint;
    static const char localhost[] = "localhost";
    static const char ipv4[] = "127.0.0.1";
    static const char ipv6[] = "[::1]";
    if (!_strnicmp(host, localhost, sizeof localhost - 1) &&
        endpoint_host_boundary(host[sizeof localhost - 1])) return 1;
    if (!strncmp(host, ipv4, sizeof ipv4 - 1) &&
        endpoint_host_boundary(host[sizeof ipv4 - 1])) return 1;
    return !strncmp(host, ipv6, sizeof ipv6 - 1) &&
           endpoint_host_boundary(host[sizeof ipv6 - 1]);
}

/*
 * 授权头绝不能通过明文 HTTP 离开本机。回环 HTTP 仍支持本地 OpenAI 兼容提供方
 * 和测试工具；所有非回环提供方必须使用 HTTPS。
 */
static int endpoint_transport_allowed(const char *endpoint) {
    if (!endpoint) return 0;
    if (!_strnicmp(endpoint, "https://", 8)) return 1;
    if (_strnicmp(endpoint, "http://", 7)) return 0;
    return endpoint_is_loopback(endpoint);
}

static int is_official_deepseek_endpoint(const char *endpoint) {
    static const char host_name[] = "api.deepseek.com";
    const char *host;
    size_t host_len = sizeof host_name - 1;
    if (!endpoint) return 0;
    host = strstr(endpoint, "://");
    host = host ? host + 3 : endpoint;
    if (_strnicmp(host, host_name, host_len) != 0) return 0;
    return host[host_len] == 0 || host[host_len] == '/' ||
           host[host_len] == ':' || host[host_len] == '?';
}

/* DeepSeek 已于 2026-07-24 停用 deepseek-chat 别名。兼容迁移仅限官方端点，
   使用户自定义 OpenAI 兼容提供方继续完全拥有自己的模型名。这里只修改加载后的
   运行时值，绝不重写 api.ini。 */
static void migrate_deprecated_deepseek_model(ApiConfig *cfg) {
    if (!is_official_deepseek_endpoint(cfg->endpoint)) return;
    if (strcmp(cfg->model, "deepseek-chat") != 0) return;
    snprintf(cfg->model, sizeof cfg->model, "%s", "deepseek-v4-flash");
    fprintf(stderr,
            "[api] migrated retired official model deepseek-chat to "
            "deepseek-v4-flash for this process; api.ini was not rewritten\n");
    fflush(stderr);
}

static int should_disable_deepseek_thinking(const ApiConfig *cfg) {
    return cfg && is_official_deepseek_endpoint(cfg->endpoint) &&
           !strncmp(cfg->model, "deepseek-v4-", strlen("deepseek-v4-"));
}

/* 安全默认值：15s 超时、并发上限、DeepSeek 官方端点与模型。
   enabled 留给 api_config_load 在读到 key 后再置位。 */
void api_config_init(ApiConfig *cfg) {
    memset(cfg, 0, sizeof *cfg);
    cfg->timeout_ms = 15000;
    cfg->concurrency = API_CONCURRENCY_MAX;
    snprintf(cfg->endpoint, sizeof cfg->endpoint, "%s", "https://api.deepseek.com/v1/chat/completions");
    snprintf(cfg->model, sizeof cfg->model, "%s", "deepseek-v4-flash");
}

/* 从 ini [api] 段加载配置。GetPrivateProfileString 的默认值传当前字段值，
   实现"未配置则沿用默认"。timeout/concurrency 做区间钳制，防止异常值。
   本地模型（Ollama/LM Studio 等回环端点）单批生成可能远慢于云 API：
   回环端点在未显式配置 timeout_ms 时默认 180s、上限放宽到 300s；
   远程端点维持 15s 默认与 60s 上限，防止慢接口拖垮前台队列。
   enabled 要求 endpoint+model 齐备，且 key 非空或端点为本机回环
   （本地 OpenAI 兼容服务如 Ollama/LM Studio 通常无需鉴权）。 */
int api_config_load(ApiConfig *cfg, const char *path) {
    api_config_init(cfg);
    if (!path || !*path) return 0;
    GetPrivateProfileStringA("api", "endpoint", cfg->endpoint, cfg->endpoint, sizeof cfg->endpoint, path);
    GetPrivateProfileStringA("api", "model", cfg->model, cfg->model, sizeof cfg->model, path);
    GetPrivateProfileStringA("api", "key", "", cfg->key, sizeof cfg->key, path);
    /* loopback 判定须在 endpoint 读取之后；timeout 未配置时按端点类型取默认。 */
    int loopback = endpoint_is_loopback(cfg->endpoint);
    int default_timeout = loopback ? 180000 : 15000;
    int max_timeout = loopback ? 300000 : 60000;
    char timeout_present[16];
    GetPrivateProfileStringA("api", "timeout_ms", "", timeout_present, sizeof timeout_present, path);
    if (timeout_present[0]) {
        cfg->timeout_ms = (int)GetPrivateProfileIntA("api", "timeout_ms", default_timeout, path);
    } else {
        cfg->timeout_ms = default_timeout;
    }
    if (cfg->timeout_ms < 3000) cfg->timeout_ms = 3000;
    if (cfg->timeout_ms > max_timeout) cfg->timeout_ms = max_timeout;
    cfg->concurrency = (int)GetPrivateProfileIntA("api", "concurrency", cfg->concurrency, path);
    if (cfg->concurrency < 1) cfg->concurrency = 1;
    if (cfg->concurrency > API_CONCURRENCY_MAX) cfg->concurrency = API_CONCURRENCY_MAX;
    cfg->endpoint[sizeof cfg->endpoint - 1] = 0;
    cfg->model[sizeof cfg->model - 1] = 0;
    cfg->key[sizeof cfg->key - 1] = 0;
    migrate_deprecated_deepseek_model(cfg);
    cfg->enabled = cfg->endpoint[0] && cfg->model[0] &&
                   (cfg->key[0] || endpoint_is_loopback(cfg->endpoint)) &&
                   endpoint_transport_allowed(cfg->endpoint);
    if (cfg->endpoint[0] && cfg->model[0] && cfg->key[0] && !cfg->enabled) {
        fprintf(stderr,
                "[api] disabled non-loopback plaintext or malformed endpoint; "
                "use HTTPS for remote providers or HTTP on loopback\n");
        fflush(stderr);
    }
    if (cfg->endpoint[0] && cfg->model[0] && !cfg->key[0] &&
        !endpoint_is_loopback(cfg->endpoint)) {
        fprintf(stderr,
                "[api] disabled: empty API key is only allowed for "
                "loopback/local endpoints\n");
        fflush(stderr);
    }
    return cfg->enabled;
}

/* UTF-8 -> 宽字符。WinHTTP 接受宽字符。失败时回退到系统 ANSI 代码页，
   保证非 UTF-8 输入也能尽量转换成功。 */
static int utf8_to_wide(const char *s, WCHAR *out, int cap) {
    int n = MultiByteToWideChar(CP_UTF8, 0, s ? s : "", -1, out, cap);
    if (!n) n = MultiByteToWideChar(CP_ACP, 0, s ? s : "", -1, out, cap);
    if (!n && cap > 0) out[0] = 0;
    return n != 0;
}

/* 扫描 UTF-8 判断是否含 CJK 汉字。已是中文的文本不必再翻译，
   既省 API 调用又避免把中文翻坏。与 http.c 中 utf8_has_cjk 同源逻辑。 */
static int has_cjk(const char *s) {
    const unsigned char *p = (const unsigned char *)s;
    while (*p) {
        unsigned cp = 0;
        if (*p < 0x80) {
            cp = *p++;
        } else if ((*p & 0xe0) == 0xc0 && p[1]) {
            cp = ((*p & 0x1f) << 6) | (p[1] & 0x3f);
            p += 2;
        } else if ((*p & 0xf0) == 0xe0 && p[1] && p[2]) {
            cp = ((*p & 0x0f) << 12) | ((p[1] & 0x3f) << 6) | (p[2] & 0x3f);
            p += 3;
        } else {
            p++;
        }
        if (cp >= 0x4e00 && cp <= 0x9fff) return 1;
    }
    return 0;
}

/* 原地去掉首尾空白。 */
static void trim_inplace(char *s) {
    char *p = s;
    while (*p && isspace((unsigned char)*p)) p++;
    if (p != s) memmove(s, p, strlen(p) + 1);
    size_t n = strlen(s);
    while (n && isspace((unsigned char)s[n - 1])) s[--n] = 0;
}

/* 清洗模型返回的译文：先去首尾空白，再剥离两种常见包裹——
   <<<...>>> 标记块，以及 ```...``` 代码块围栏。
   模型偶尔不守规矩加这些，留着会污染缓存，必须在此统一去掉。 */
static void normalize_translation(char *s) {
    trim_inplace(s);
    if (!strncmp(s, "<<<", 3)) {
        char *p = s + 3;
        while (*p && isspace((unsigned char)*p)) p++;
        char *end = strstr(p, ">>>");
        if (end) {
            *end = 0;
            memmove(s, p, strlen(p) + 1);
            trim_inplace(s);
        }
    }
    if (!strncmp(s, "```", 3)) {
        char *p = strchr(s + 3, '\n');
        char *end = strstr(s + 3, "```");
        if (p && end && end > p) {
            *end = 0;
            memmove(s, p + 1, strlen(p + 1) + 1);
            trim_inplace(s);
        }
    }
}

/* 三种请求模式共享的系统提示词前缀：稳定的开头让提供商的前缀缓存
   在单条/批量/上下文模式之间互相命中（缓存命中输入价约为全价的 1/10）。 */
static const char *const API_SYSTEM_PREFIX =
    "You are localizing game text to Simplified Chinese. Preserve tags, placeholders, variables, numbers, and line breaks exactly. Already Chinese stays unchanged. Do not add explanations.";

/* 系统提示词 = 共享前缀 + 模式差异段 + 按命中过滤的术语表。
   前缀在此统一拼接，保证三种模式共享同一段可缓存的开头。
   术语表段行格式固定为 "- 术语 => 译文"（api_glossary_load_file 生成），
   按行解析、仅附加当前批次文本里实际出现的条目——长术语表在
   冷缓存请求上的固定开销从最多数千 token 降为按需。 */
static void build_system_content(const char *mode_suffix, const ApiConfig *cfg, Buf *sys,
                                 char **texts, size_t count) {
    buf_add(sys, API_SYSTEM_PREFIX);
    buf_add(sys, mode_suffix);
    if (!cfg || !cfg->glossary[0]) return;

    Buf all;
    buf_init(&all);
    for (size_t i = 0; i < count; i++) {
        if (texts[i]) buf_add(&all, texts[i]);
        buf_ch(&all, '\n');
    }

    Buf hits;
    buf_init(&hits);
    const char *p = cfg->glossary;
    while (*p) {
        const char *nl = strchr(p, '\n');
        size_t len = nl ? (size_t)(nl - p) : strlen(p);
        /* 行格式："- 术语 => 译文" */
        if (len > 4 && p[0] == '-' && p[1] == ' ') {
            const char *sep = strstr(p, " => ");
            if (sep && sep > p + 2 && (size_t)(sep - p) < len) {
                size_t term_len = (size_t)(sep - p) - 2;
                char term[192];
                if (term_len < sizeof term) {
                    memcpy(term, p + 2, term_len);
                    term[term_len] = 0;
                    if (istrstr(all.data, term)) {
                        buf_ch(&hits, '\n');
                        buf_addn(&hits, p, len);
                        if (!hits.len || hits.data[hits.len - 1] != '\n') buf_ch(&hits, '\n');
                    }
                }
            }
        }
        if (!nl) break;
        p = nl + 1;
    }
    if (hits.len > 0) {
        buf_add(sys, "\n\nMandatory glossary (use these exact translations):");
        buf_add(sys, hits.data);
    }
    buf_free(&all);
    buf_free(&hits);
}

/* 输出上限：中文译文 token 数通常不超过源文本字节量（英→中约 0.3-0.5
   token/字节），上限按"输入字节数 + 常量余量"取值，为正常译文留出约
   2 倍余量，同时封住模型偶发超长输出的计费。本地小模型分词器效率偏低
   时也有余量；8192 为主流模型单次输出上限，更长的文本应由上游分段
   （各引擎钩子已有分段/页规划）。 */
static int estimate_max_tokens(char **texts, size_t count) {
    size_t chars = 0;
    for (size_t i = 0; i < count; i++) {
        if (texts[i]) chars += strlen(texts[i]);
    }
    double est = 96.0 + (double)chars + 4.0 * (double)count;
    if (est < 512.0) est = 512.0;
    if (est > 8192.0) est = 8192.0;
    return (int)est;
}

/* 构造单条翻译的 chat completions 请求体。
   system 提示强调保留标签/占位符/变量/数字/换行，且已是中文则保持不变；
   user 提示要求只返回译文。temperature=0 保证稳定输出。 */
static void build_request(ApiConfig *cfg, const char *text, Buf *body) {
    char *texts[1];
    texts[0] = (char *)text;
    int max_tokens = estimate_max_tokens(texts, 1);

    Buf user;
    buf_init(&user);
    buf_add(&user, "Translate this exact game text to Simplified Chinese. Return only the translation.\n");
    buf_add(&user, text);

    Buf sys;
    buf_init(&sys);
    build_system_content("", cfg, &sys, texts, 1);

    buf_add(body, "{\"model\":");
    buf_json(body, cfg->model);
    buf_add(body, ",\"messages\":[{\"role\":\"system\",\"content\":");
    buf_json(body, sys.data);
    buf_add(body, "},{\"role\":\"user\",\"content\":");
    buf_json(body, user.data);
    buf_add(body, "}],\"temperature\":0,\"max_tokens\":");
    buf_int(body, max_tokens);
    if (should_disable_deepseek_thinking(cfg)) {
        buf_add(body, ",\"thinking\":{\"type\":\"disabled\"}");
    }
    buf_ch(body, '}');
    buf_free(&user);
    buf_free(&sys);
}

/* 构造批量翻译请求体：把多条文本放进一个 JSON 数组，要求模型返回等长同序的数组。
   批量可大幅减少请求次数与 token 开销，是预热/大批量的主路径。 */
static void build_batch_request(ApiConfig *cfg, char **texts, size_t count, Buf *body) {
    int max_tokens = estimate_max_tokens(texts, count);

    Buf user;
    buf_init(&user);
    buf_ch(&user, '[');
    for (size_t i = 0; i < count; i++) {
        if (i) buf_ch(&user, ',');
        buf_json(&user, texts[i]);
    }
    buf_ch(&user, ']');

    Buf sys;
    buf_init(&sys);
    build_system_content(" Translate each element of the user's JSON array in order. Return only a valid JSON array with the same length and order.",
                         cfg, &sys, texts, count);

    buf_add(body, "{\"model\":");
    buf_json(body, cfg->model);
    buf_add(body, ",\"messages\":[{\"role\":\"system\",\"content\":");
    buf_json(body, sys.data);
    buf_add(body, "},{\"role\":\"user\",\"content\":");
    buf_json(body, user.data);
    buf_add(body, "}],\"temperature\":0,\"max_tokens\":");
    buf_int(body, max_tokens);
    if (should_disable_deepseek_thinking(cfg)) {
        buf_add(body, ",\"thinking\":{\"type\":\"disabled\"}");
    }
    buf_ch(body, '}');
    buf_free(&user);
    buf_free(&sys);
}

/* 构造带上下文的批量翻译请求体。与 build_batch_request 的区别：
   1) system 说明文本按剧情顺序排列、可利用"上一行"语境，但绝不翻译/输出语境行；
   2) user 先列出"批外断点"的上一行（prev 与批内前一条相同的条目跳过——
      那条文本本身就在待翻译数组里，重复发送只会让输入 token 翻倍），
      再给出待翻译数组。
   只有至少一条 prev 非空时才会走本函数（调用方保证），无 prev 时上层退回
   build_batch_request，保证旧路径提示词字节级不变。 */
static void build_ctx_request(ApiConfig *cfg, char **texts, const char *const *prevs,
                              size_t count, Buf *body) {
    int max_tokens = estimate_max_tokens(texts, count);

    Buf user;
    buf_init(&user);
    buf_add(&user, "Previous lines (context only, never translate or output them):\n");
    for (size_t i = 0; i < count; i++) {
        if (!prevs || !prevs[i] || !prevs[i][0]) continue;
        /* 批内连续文本的 prev 就是前一条本身——已在数组里，跳过重复。 */
        if (i > 0 && prevs[i - 1] && strcmp(prevs[i], texts[i - 1]) == 0) continue;
        char idx[32];
        snprintf(idx, sizeof idx, "%zu: ", i);
        buf_add(&user, idx);
        buf_add(&user, prevs[i]);
        buf_ch(&user, '\n');
    }
    buf_add(&user, "\nTexts to translate:\n");
    buf_ch(&user, '[');
    for (size_t i = 0; i < count; i++) {
        if (i) buf_ch(&user, ',');
        buf_json(&user, texts[i]);
    }
    buf_ch(&user, ']');

    Buf sys;
    buf_init(&sys);
    build_system_content(" The texts are consecutive in-game lines in story order; some entries list a previous line for context. Use that context to resolve pronouns, tone, and terminology, and keep character naming consistent, but never translate or output the context itself. Translate each element of the JSON array. Return only a valid JSON array with the same length and order.",
                         cfg, &sys, texts, count);

    buf_add(body, "{\"model\":");
    buf_json(body, cfg->model);
    buf_add(body, ",\"messages\":[{\"role\":\"system\",\"content\":");
    buf_json(body, sys.data);
    buf_add(body, "},{\"role\":\"user\",\"content\":");
    buf_json(body, user.data);
    buf_add(body, "}],\"temperature\":0,\"max_tokens\":");
    buf_int(body, max_tokens);
    if (should_disable_deepseek_thinking(cfg)) {
        buf_add(body, ",\"thinking\":{\"type\":\"disabled\"}");
    }
    buf_ch(body, '}');
    buf_free(&user);
    buf_free(&sys);
}

/* 独立 WinHTTP 通道池最多允许 cfg->concurrency 个请求同时进行。每个通道拥有自己的
   session/connect 句柄并由独立锁保护，因此传输故障只重置当前通道，不会与其他通道
   正在运行的请求竞争。 */
typedef struct {
    SRWLOCK lock;
    HINTERNET session;
    HINTERNET connect;
    WCHAR host[512];
    WCHAR path[2048];
    WCHAR key[1400];
    INTERNET_PORT port;
    DWORD flags;
    char endpoint_id[1024];   /* 记录当前通道绑定的 endpoint，变更时重建 */
    char key_id[1024];        /* 记录当前通道绑定的 key，变更时重建 */
    int ready;
} ApiChannel;

/* 通道池，下标 0..API_CONCURRENCY_MAX-1。零初始化使 lock == SRWLOCK_INIT。
   实际使用数量由 cfg->concurrency 决定，多余通道闲置。 */
static ApiChannel g_channels[API_CONCURRENCY_MAX];
typedef enum {
    API_DIAG_ENDPOINT_ENCODING,
    API_DIAG_KEY_ENCODING,
    API_DIAG_CRACK_URL,
    API_DIAG_OPEN_SESSION,
    API_DIAG_CONNECT,
    API_DIAG_OPEN_REQUEST,
    API_DIAG_SET_TIMEOUTS,
    API_DIAG_SEND,
    API_DIAG_RECEIVE,
    API_DIAG_QUERY_STATUS,
    API_DIAG_HTTP_STATUS,
    API_DIAG_QUERY_BODY,
    API_DIAG_READ_BODY,
    API_DIAG_BODY_TOO_LARGE,
    API_DIAG_EMPTY_BODY,
    API_DIAG_MISSING_CONTENT,
    API_DIAG_EMPTY_CONTENT,
    API_DIAG_BATCH_SHAPE,
    API_DIAG_COUNT
} ApiDiag;

static volatile LONG g_api_diag_counts[API_DIAG_COUNT];

/* 最近一次失败的分类，供 api_translate_split_retry 决定是否继续拆分：
   - API_FAIL_SPLIT：内容/形状类失败（模型漏/多数组元素、空内容、响应体超限、
     请求超时、HTTP 400/408/413/422）——缩小批次有机会成功；
   - API_FAIL_TERMINAL：端点不可达、句柄/编码失败、鉴权失败、限流、5xx——
     缩小批次只会把同一个错误重复几十次，必须停止；
   - API_FAIL_NONE：本次尝试没有经过 api_diag（前置守卫拒绝，如文本含 CJK）。
   线程局部：每个 worker 线程只读自己刚发出的那次请求的结论。 */
typedef enum {
    API_FAIL_NONE = 0,
    API_FAIL_SPLIT,
    API_FAIL_TERMINAL
} ApiFailClass;

static _Thread_local ApiFailClass g_api_last_fail;

static ApiFailClass api_fail_class(ApiDiag reason, DWORD winhttp_error, DWORD status) {
    switch (reason) {
    case API_DIAG_SEND:
    case API_DIAG_RECEIVE:
        /* 大批次在慢速本地模型上可能超时，缩小批次有意义；其他传输错误不然。 */
        return winhttp_error == ERROR_WINHTTP_TIMEOUT ? API_FAIL_SPLIT : API_FAIL_TERMINAL;
    case API_DIAG_HTTP_STATUS:
        return (status == 400 || status == 408 || status == 413 || status == 422)
            ? API_FAIL_SPLIT : API_FAIL_TERMINAL;
    case API_DIAG_BODY_TOO_LARGE:
    case API_DIAG_EMPTY_BODY:
    case API_DIAG_MISSING_CONTENT:
    case API_DIAG_EMPTY_CONTENT:
    case API_DIAG_BATCH_SHAPE:
        return API_FAIL_SPLIT;
    default:
        return API_FAIL_TERMINAL;
    }
}

/* 远程传输或提供方降级无法在本客户端上游修复，因为端点和响应都属于外部边界。
   降级绝不会把故障变成缓存命中；每种降级都会在这里按准确阶段限频记录，且不包含
   请求文本或 API 密钥，使原始故障仍可诊断。 */
static void api_diag(ApiDiag reason, DWORD winhttp_error, DWORD status, const char *detail) {
    static const char *names[API_DIAG_COUNT] = {
        "endpoint-encoding", "key-encoding", "crack-url", "open-session",
        "connect", "open-request", "set-timeouts", "send", "receive",
        "query-status", "http-status", "query-body", "read-body",
        "body-too-large", "empty-body", "missing-content", "empty-content",
        "batch-shape"
    };
    g_api_last_fail = api_fail_class(reason, winhttp_error, status);
    LONG count = InterlockedIncrement(&g_api_diag_counts[reason]);
    if (count <= 3 || (count & (count - 1)) == 0) {
        fprintf(stderr,
                "[api] %s failed #%ld (winhttp=%lu, status=%lu%s%s)\n",
                names[reason], count, winhttp_error, status,
                detail ? ", detail=" : "", detail ? detail : "");
        fflush(stderr);
    }
}
static volatile LONG g_rr;    /* 轮询计数器，Interlocked 递增保证线程安全 */

/* 持锁：关闭并清空通道的句柄与就绪标志，用于配置变更或传输失败后重建。 */
static void api_reset_locked(ApiChannel *ch) {
    if (ch->connect) WinHttpCloseHandle(ch->connect);
    if (ch->session) WinHttpCloseHandle(ch->session);
    ch->connect = NULL;
    ch->session = NULL;
    ch->ready = 0;
    ch->endpoint_id[0] = 0;
    ch->key_id[0] = 0;
}

/* 持锁：确保通道已准备好（session+connect 已建，URL 已拆分）。
   若 endpoint/key 与缓存一致且 ready，直接复用；否则重建。
   返回 1=就绪，0=失败（已 reset，调用方释放锁即可）。 */
static int api_prepare_locked(ApiChannel *ch, ApiConfig *cfg) {
    if (ch->ready && !strcmp(ch->endpoint_id, cfg->endpoint) && !strcmp(ch->key_id, cfg->key)) return 1;
    api_reset_locked(ch);

    WCHAR url[2048];
    if (!utf8_to_wide(cfg->endpoint, url, 2048)) {
        api_diag(API_DIAG_ENDPOINT_ENCODING, GetLastError(), 0, NULL);
        return 0;
    }
    if (!utf8_to_wide(cfg->key, ch->key, 1400)) {
        api_diag(API_DIAG_KEY_ENCODING, GetLastError(), 0, NULL);
        return 0;
    }

    URL_COMPONENTSW uc;
    memset(&uc, 0, sizeof uc);
    uc.dwStructSize = sizeof uc;
    uc.lpszHostName = ch->host;
    uc.dwHostNameLength = 512;
    uc.lpszUrlPath = ch->path;
    uc.dwUrlPathLength = 2048;
    if (!WinHttpCrackUrl(url, 0, 0, &uc)) {
        api_diag(API_DIAG_CRACK_URL, GetLastError(), 0, NULL);
        return 0;
    }
    if (uc.dwHostNameLength < 512) ch->host[uc.dwHostNameLength] = 0;
    else ch->host[511] = 0;
    if (uc.dwUrlPathLength < 2048) ch->path[uc.dwUrlPathLength] = 0;
    else ch->path[2047] = 0;

    ch->port = uc.nPort;
    ch->flags = uc.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0;
    ch->session = WinHttpOpen(L"ds-game-translator/3.1",
                              WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                              WINHTTP_NO_PROXY_NAME,
                              WINHTTP_NO_PROXY_BYPASS, 0);
    if (!ch->session) {
        api_diag(API_DIAG_OPEN_SESSION, GetLastError(), 0, NULL);
        return 0;
    }

    ch->connect = WinHttpConnect(ch->session, ch->host, ch->port, 0);
    if (!ch->connect) {
        api_diag(API_DIAG_CONNECT, GetLastError(), 0, NULL);
        api_reset_locked(ch);
        return 0;
    }

    snprintf(ch->endpoint_id, sizeof ch->endpoint_id, "%s", cfg->endpoint);
    snprintf(ch->key_id, sizeof ch->key_id, "%s", cfg->key);
    ch->ready = 1;
    return 1;
}

/* 轮询获取一个可用通道：从 ticket 起在 [0,concurrency) 范围内尝试无锁获取，
   全忙则阻塞等待起始通道。返回已持锁的通道，调用方负责释放。 */
static ApiChannel *api_acquire_channel(ApiConfig *cfg) {
    int n = cfg->concurrency;
    if (n < 1) n = 1;
    if (n > API_CONCURRENCY_MAX) n = API_CONCURRENCY_MAX;

    DWORD ticket = (DWORD)InterlockedIncrement(&g_rr);
    for (int i = 0; i < n; i++) {
        ApiChannel *ch = &g_channels[(ticket + (DWORD)i) % (DWORD)n];
        if (TryAcquireSRWLockExclusive(&ch->lock)) return ch;
    }
    ApiChannel *ch = &g_channels[ticket % (DWORD)n];
    AcquireSRWLockExclusive(&ch->lock);
    return ch;
}

/* 读取完整响应体到新分配缓冲。响应上限 2MB，防止异常大响应耗尽内存。 */
static int read_response(HINTERNET req, char **out) {
    Buf b;
    buf_init(&b);
    for (;;) {
        DWORD avail = 0;
        if (!WinHttpQueryDataAvailable(req, &avail)) {
            api_diag(API_DIAG_QUERY_BODY, GetLastError(), 0, NULL);
            buf_free(&b);
            return 0;
        }
        if (!avail) break;
        /* WinHTTP 返回无符号 32 位字节数。在 +1 分配和 WinHttpReadData 前先限制它，
           防止恶意或自定义端点使分配大小回绕，或强制产生超大临时缓冲。 */
        if ((size_t)avail > API_MAX_RESPONSE_BYTES - b.len) {
            api_diag(API_DIAG_BODY_TOO_LARGE, 0, 0, "limit=2097152");
            buf_free(&b);
            return 0;
        }
        char *tail = buf_reserve(&b, (size_t)avail);
        DWORD rd = 0;
        if (!WinHttpReadData(req, tail, avail, &rd)) {
            api_diag(API_DIAG_READ_BODY, GetLastError(), 0, NULL);
            buf_free(&b);
            return 0;
        }
        if ((size_t)rd > API_MAX_RESPONSE_BYTES - b.len) {
            api_diag(API_DIAG_BODY_TOO_LARGE, 0, 0, "limit=2097152");
            buf_free(&b);
            return 0;
        }
        buf_commit(&b, (size_t)rd);
    }
    if (!b.len) {
        api_diag(API_DIAG_EMPTY_BODY, 0, 0, NULL);
        buf_free(&b);
        return 0;
    }
    *out = b.data;
    return 1;
}

/* 发送一次 chat completions 请求并解析出 content 文本。
   流程：取通道 -> 准备 -> 开请求 -> 设超时 -> 加鉴权头 -> 发送+收响应 ->
   校验 2xx -> 读体 -> 取 content 字段 -> normalize。
   传输层失败时重置该通道（下次重建句柄）。返回 1=成功拿到非空译文。 */
static int send_chat_request(ApiConfig *cfg, Buf *body, char **content_out) {
    ApiChannel *ch = api_acquire_channel(cfg);
    if (!api_prepare_locked(ch, cfg)) {
        ReleaseSRWLockExclusive(&ch->lock);
        return 0;
    }

    HINTERNET req = WinHttpOpenRequest(ch->connect, L"POST", ch->path, NULL, WINHTTP_NO_REFERER,
                                       WINHTTP_DEFAULT_ACCEPT_TYPES, ch->flags);
    if (!req) {
        api_diag(API_DIAG_OPEN_REQUEST, GetLastError(), 0, NULL);
        api_reset_locked(ch);
        ReleaseSRWLockExclusive(&ch->lock);
        return 0;
    }

    int timeout = cfg->timeout_ms;
    if (!WinHttpSetTimeouts(req, timeout, timeout, timeout, timeout)) {
        api_diag(API_DIAG_SET_TIMEOUTS, GetLastError(), 0, NULL);
        WinHttpCloseHandle(req);
        api_reset_locked(ch);
        ReleaseSRWLockExclusive(&ch->lock);
        return 0;
    }

    WCHAR headers[1800];
    /* 回环端点允许空 key（本地服务无需鉴权），此时省略 Authorization 头，
       避免部分本地服务对空 Bearer 报错。 */
    if (ch->key[0]) {
        _snwprintf(headers, 1800, L"Content-Type: application/json\r\nAuthorization: Bearer %s\r\n", ch->key);
    } else {
        _snwprintf(headers, 1800, L"Content-Type: application/json\r\n");
    }
    headers[1799] = 0;

    int ok = 0;
    int transport_ok = 0;
    if (!WinHttpSendRequest(req, headers, (DWORD)-1, body->data,
                            (DWORD)body->len, (DWORD)body->len, 0)) {
        api_diag(API_DIAG_SEND, GetLastError(), 0, NULL);
    } else if (!WinHttpReceiveResponse(req, NULL)) {
        api_diag(API_DIAG_RECEIVE, GetLastError(), 0, NULL);
    } else {
        transport_ok = 1;
        DWORD status = 0, sz = sizeof status;
        if (!WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                                 WINHTTP_HEADER_NAME_BY_INDEX, &status, &sz,
                                 WINHTTP_NO_HEADER_INDEX)) {
            api_diag(API_DIAG_QUERY_STATUS, GetLastError(), 0, NULL);
            transport_ok = 0;
        } else if (status < 200 || status >= 300) {
            api_diag(API_DIAG_HTTP_STATUS, 0, status, NULL);
        } else {
            char *raw = NULL;
            if (!read_response(req, &raw)) {
                transport_ok = 0;
            } else {
                char *content = json_get_str(raw, "content");
                if (!content) {
                    api_diag(API_DIAG_MISSING_CONTENT, 0, status, NULL);
                } else {
                    normalize_translation(content);
                    normalize_translation_result(content);
                    if (*content) {
                        *content_out = content;
                        content = NULL;
                        ok = 1;
                    } else {
                        api_diag(API_DIAG_EMPTY_CONTENT, 0, status, NULL);
                    }
                    free(content);
                }
                free(raw);
            }
        }
    }

    WinHttpCloseHandle(req);
    if (!transport_ok) api_reset_locked(ch);
    ReleaseSRWLockExclusive(&ch->lock);
    return ok;
}

/* 解析模型返回的 JSON 字符串数组（批量翻译用）。要求是严格的 ["a","b",...]，
   每个元素做 normalize。任何结构不符都判失败并释放已解析部分，
   因为批量必须保证长度与顺序对应，部分结果无法安全使用。 */
static int parse_json_string_array(const char *json, List *out) {
    const char *p = json_skipws(json);
    if (*p != '[') return 0;
    p++;
    for (;;) {
        p = json_skipws(p);
        if (*p == ']') return 1;
        if (*p != '"') {
            list_free(out);
            return 0;
        }
        char *s = json_str(&p);
        if (!s) {
            list_free(out);
            return 0;
        }
        normalize_translation(s);
        normalize_translation_result(s);
        list_push(out, s);
        p = json_skipws(p);
        if (*p == ',') {
            p++;
            continue;
        }
        if (*p == ']') return 1;
        list_free(out);
        return 0;
    }
}

/* 翻译单条文本。前置守卫：未启用/空/已是中文 直接返回 0（不调 API）。
   成功时 *out 为新分配的译文，调用方负责 free。 */
int api_translate(ApiConfig *cfg, const char *text, char **out) {
    if (!cfg || !cfg->enabled || !text || !*text || has_cjk(text)) return 0;

    Buf body;
    buf_init(&body);
    build_request(cfg, text, &body);

    int ok = send_chat_request(cfg, &body, out);
    buf_free(&body);
    return ok;
}

/* 批量翻译。count==1 时走单条路径（避免数组解析开销）。
   批量返回后必须校验解析出的元素数 == 输入数，否则判失败——长度不匹配
   无法与输入对齐，宁可让上层降级也不可错位回写。成功时 *out 为长度 count
   的新分配指针数组，每个元素需调用方 free，数组本身也需 free。 */
int api_translate_batch(ApiConfig *cfg, char **texts, size_t count, char ***out) {
    if (!cfg || !cfg->enabled || !texts || !count) return 0;
    if (count == 1) {
        char *one = NULL;
        if (!api_translate(cfg, texts[0], &one)) return 0;
        char **arr = xmalloc(sizeof *arr);
        arr[0] = one;
        *out = arr;
        return 1;
    }

    Buf body;
    buf_init(&body);
    build_batch_request(cfg, texts, count, &body);

    char *content = NULL;
    int ok = send_chat_request(cfg, &body, &content);
    buf_free(&body);
    if (!ok) return 0;

    List parsed = {0};
    ok = parse_json_string_array(content, &parsed);
    free(content);
    if (!ok || parsed.n != count) {
        char detail[96];
        snprintf(detail, sizeof detail, "expected=%zu, parsed=%zu", count, parsed.n);
        api_diag(API_DIAG_BATCH_SHAPE, 0, 0, detail);
        list_free(&parsed);
        return 0;
    }

    /* 接管 List 内的指针到裸数组，释放 List 容器但保留元素。 */
    char **arr = xmalloc(count * sizeof *arr);
    for (size_t i = 0; i < count; i++) {
        arr[i] = parsed.v[i];
        parsed.v[i] = NULL;
    }
    free(parsed.v);
    *out = arr;
    return 1;
}

/* 从 TSV 术语表文件构建提示词段，存入 cfg->glossary。
   文件格式：每行 `术语<TAB>译文`，'#' 开头为注释，空行跳过。
   失败透明性说明：
   1) 防止的外部异常：文件不存在（用户未配置术语表）或单行超长/缺 TAB 的
      畸形行——这些是可选配置的自然状态，不构成错误；
   2) 为何不在上游修复：术语表是用户手编文件，服务器必须容忍任意内容；
   3) 是否会掩盖错误：不会——加载失败仅表现为"无术语表"，翻译主流程
      完全不受影响；真正的读文件系统错误会以 stderr 诊断记录（限频）；
   4) 诊断记录位置：本函数内的 fprintf(stderr, "[api] glossary ...")。 */
void api_glossary_load_file(ApiConfig *cfg, const char *path) {
    if (!cfg || !path || !*path) return;
    FILE *f = fopen(path, "rb");
    if (!f) return; /* 未配置术语表：正常情况，保持为空 */
    /* 跳过 UTF-8 BOM（Windows 记事本默认写入），否则首行会被误判为畸形行。 */
    int c1 = fgetc(f), c2 = fgetc(f), c3 = fgetc(f);
    if (!(c1 == 0xEF && c2 == 0xBB && c3 == 0xBF) && c1 != EOF) {
        fseek(f, 0, SEEK_SET); /* 无 BOM：回到文件头重新按行读取 */
    }
    Buf section;
    buf_init(&section);
    buf_add(&section, "Mandatory glossary (use these exact translations):\n");
    char line[512];
    int entries = 0;
    int malformed = 0;
    while (fgets(line, sizeof line, f)) {
        size_t n = strlen(line);
        while (n && (line[n - 1] == '\n' || line[n - 1] == '\r')) line[--n] = 0;
        if (!n || line[0] == '#') continue;
        if (n >= sizeof line - 1) { malformed++; continue; } /* 超长行整行放弃，避免半行术语 */
        char *tab = strchr(line, '\t');
        if (!tab || tab == line || !tab[1]) { malformed++; continue; }
        *tab = 0;
        buf_add(&section, "- ");
        buf_add(&section, line);
        buf_add(&section, " => ");
        buf_add(&section, tab + 1);
        buf_ch(&section, '\n');
        entries++;
        if (entries >= 256 || section.len > API_GLOSSARY_BYTES / 2) break; /* 提示词预算上限 */
    }
    fclose(f);
    if (entries) {
        snprintf(cfg->glossary, sizeof cfg->glossary, "%s", section.data);
        fprintf(stderr, "[api] glossary loaded: %d entries from %s\n", entries, path);
        fflush(stderr);
    }
    if (malformed) {
        fprintf(stderr, "[api] glossary skipped %d malformed line(s) in %s\n",
                malformed, path);
        fflush(stderr);
    }
    buf_free(&section);
}

/* 带上下文的批量翻译（见 api.h）。count==1 也走数组模式：解析器
   （parse_json_string_array）对单元素数组同样适用，避免再造一套
   单条+语境的响应路径。prevs 元素为空串/NULL 表示该条无上文。 */
int api_translate_batch_ctx(ApiConfig *cfg, char **texts, const char *const *prevs,
                            size_t count, char ***out) {
    if (!cfg || !cfg->enabled || !texts || !count) return 0;

    Buf body;
    buf_init(&body);
    build_ctx_request(cfg, texts, prevs, count, &body);

    char *content = NULL;
    int ok = send_chat_request(cfg, &body, &content);
    buf_free(&body);
    if (!ok) return 0;

    List parsed = {0};
    ok = parse_json_string_array(content, &parsed);
    free(content);
    if (!ok || parsed.n != count) {
        char detail[96];
        snprintf(detail, sizeof detail, "expected=%zu, parsed=%zu", count, parsed.n);
        api_diag(API_DIAG_BATCH_SHAPE, 0, 0, detail);
        list_free(&parsed);
        return 0;
    }

    char **arr = xmalloc(count * sizeof *arr);
    for (size_t i = 0; i < count; i++) {
        arr[i] = parsed.v[i];
        parsed.v[i] = NULL;
    }
    free(parsed.v);
    *out = arr;
    return 1;
}

#define API_SPLIT_MIN_ITEMS 4

static volatile LONG g_api_split_aborts;

/* 二分重试因终止类失败提前停止：限频记录，说明剩余多少条直接计 miss。
   这些条目在后台队列收尾时被遗忘，下次 miss 可再次排队，不会永久丢失。 */
static void api_split_abort_diag(size_t remaining) {
    LONG count = InterlockedIncrement(&g_api_split_aborts);
    if (count <= 3 || (count & (count - 1)) == 0) {
        fprintf(stderr,
                "[api] split-retry aborted #%ld (terminal failure, remaining=%zu)\n",
                (long)count, remaining);
        fflush(stderr);
    }
}

/* 整批失败后的二分重试（见 api.h）。范围尝试一次批量，失败且范围大于
   API_SPLIT_MIN_ITEMS 时对半递归；小于等于阈值改为逐条（少量文本的
   逐条开销低于继续拆分的请求开销）。逐条也带各自的语境。
   返回 count 长度数组：成功槽位为新分配译文，失败槽位为 NULL。
   只有内容/形状类失败（g_api_last_fail == API_FAIL_SPLIT）才继续拆分：
   端点不可达、鉴权失败、限流、5xx 属于终止类，若照样拆分，48 条整批全坏
   会展开成约 79 次请求（1+2+4+8+16×4），比它取代的逐条方案（49 次）还多，
   且每次都要等满 timeout_ms。最常见的"模型漏/多数组元素"部分失败场景，
   重试请求数仍是约 13 次。 */
char **api_translate_split_retry(ApiConfig *cfg, char **texts,
                                 const char *const *prevs, size_t count) {
    char **arr = xcalloc(count, sizeof *arr);
    if (!arr || !cfg || !cfg->enabled || !texts || !count) return arr;

    int has_prev = 0;
    for (size_t i = 0; i < count; i++) {
        if (prevs && prevs[i] && prevs[i][0]) { has_prev = 1; break; }
    }

    char **translated = NULL;
    g_api_last_fail = API_FAIL_NONE;
    int ok = has_prev
        ? api_translate_batch_ctx(cfg, texts, prevs, count, &translated)
        : api_translate_batch(cfg, texts, count, &translated);
    if (ok) {
        for (size_t i = 0; i < count; i++) arr[i] = translated[i];
        free(translated);
        return arr;
    }
    if (g_api_last_fail != API_FAIL_SPLIT) {
        /* 终止类失败，或前置守卫拒绝（无请求发出）：拆分无意义。 */
        if (g_api_last_fail == API_FAIL_TERMINAL) api_split_abort_diag(count);
        return arr;
    }

    if (count <= API_SPLIT_MIN_ITEMS) {
        for (size_t i = 0; i < count; i++) {
            char **one = NULL;
            int one_ok;
            g_api_last_fail = API_FAIL_NONE;
            if (prevs && prevs[i] && prevs[i][0]) {
                one_ok = api_translate_batch_ctx(cfg, &texts[i],
                                                 (const char *const *)&prevs[i], 1, &one);
            } else {
                one_ok = api_translate_batch(cfg, &texts[i], 1, &one);
            }
            if (one_ok && one) {
                arr[i] = one[0];
            } else if (g_api_last_fail == API_FAIL_TERMINAL) {
                /* 逐条阶段撞到终止类失败：剩余条目不再逐个重复同一个错误。
                   失败时 api_translate_batch 不写 *out，one 仍为 NULL。 */
                api_split_abort_diag(count - i);
                break;
            }
            free(one);
        }
        return arr;
    }

    size_t half = count / 2;
    /* prevs 可为 NULL（api.h 未要求非空）：NULL + half 是未定义行为，且递归
       内 prevs[i] 会解引用一个小的伪地址。 */
    const char *const *prevs_right = prevs ? prevs + half : NULL;
    char **a = api_translate_split_retry(cfg, texts, prevs, half);
    char **b = api_translate_split_retry(cfg, texts + half, prevs_right, count - half);
    if (a) {
        memcpy(arr, a, half * sizeof *arr);
        free(a);
    }
    if (b) {
        memcpy(arr + half, b, (count - half) * sizeof *arr);
        free(b);
    }
    return arr;
}
