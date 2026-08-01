/*
 * api.c —— 远程翻译 API 客户端实现（详见 api.h）。
 *
 * 用 WinHTTP 调用 DeepSeek chat completions，把英文游戏文本译成简体中文。
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

/*
 * Authorization headers must not leave the machine over cleartext HTTP.
 * Loopback HTTP remains supported for local OpenAI-compatible providers and
 * the test harness; every non-loopback provider must use HTTPS.
 */
static int endpoint_transport_allowed(const char *endpoint) {
    if (!endpoint) return 0;
    if (!_strnicmp(endpoint, "https://", 8)) return 1;
    if (_strnicmp(endpoint, "http://", 7)) return 0;

    const char *host = endpoint + 7;
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

/* DeepSeek retired the deepseek-chat alias on 2026-07-24. Limit the
   compatibility migration to its official endpoint so user-defined
   OpenAI-compatible providers keep full ownership of their model names.
   This changes only the loaded runtime value and never rewrites api.ini. */
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
   enabled 仅当 endpoint+model+key 三者齐备时才为真。 */
int api_config_load(ApiConfig *cfg, const char *path) {
    api_config_init(cfg);
    if (!path || !*path) return 0;
    GetPrivateProfileStringA("api", "endpoint", cfg->endpoint, cfg->endpoint, sizeof cfg->endpoint, path);
    GetPrivateProfileStringA("api", "model", cfg->model, cfg->model, sizeof cfg->model, path);
    GetPrivateProfileStringA("api", "key", "", cfg->key, sizeof cfg->key, path);
    cfg->timeout_ms = (int)GetPrivateProfileIntA("api", "timeout_ms", cfg->timeout_ms, path);
    if (cfg->timeout_ms < 3000) cfg->timeout_ms = 3000;
    if (cfg->timeout_ms > 60000) cfg->timeout_ms = 60000;
    cfg->concurrency = (int)GetPrivateProfileIntA("api", "concurrency", cfg->concurrency, path);
    if (cfg->concurrency < 1) cfg->concurrency = 1;
    if (cfg->concurrency > API_CONCURRENCY_MAX) cfg->concurrency = API_CONCURRENCY_MAX;
    cfg->endpoint[sizeof cfg->endpoint - 1] = 0;
    cfg->model[sizeof cfg->model - 1] = 0;
    cfg->key[sizeof cfg->key - 1] = 0;
    migrate_deprecated_deepseek_model(cfg);
    cfg->enabled = cfg->endpoint[0] && cfg->model[0] && cfg->key[0] &&
                   endpoint_transport_allowed(cfg->endpoint);
    if (cfg->endpoint[0] && cfg->model[0] && cfg->key[0] && !cfg->enabled) {
        fprintf(stderr,
                "[api] disabled non-loopback plaintext or malformed endpoint; "
                "use HTTPS for remote providers or HTTP on loopback\n");
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

/* 构造单条翻译的 chat completions 请求体。
   system 提示强调保留标签/占位符/变量/数字/换行，且已是中文则保持不变；
   user 提示要求只返回译文。temperature=0 保证稳定输出。 */
static void build_request(ApiConfig *cfg, const char *text, Buf *body) {
    Buf user;
    buf_init(&user);
    buf_add(&user, "Translate this exact game text to Simplified Chinese. Return only the translation.\n");
    buf_add(&user, text);

    buf_add(body, "{\"model\":");
    buf_json(body, cfg->model);
    buf_add(body, ",\"messages\":[{\"role\":\"system\",\"content\":");
    buf_json(body,
             "Game localization to Simplified Chinese. Preserve tags, placeholders, variables, numbers, and line breaks. Already Chinese stays unchanged.");
    buf_add(body, "},{\"role\":\"user\",\"content\":");
    buf_json(body, user.data);
    buf_add(body, "}],\"temperature\":0");
    if (should_disable_deepseek_thinking(cfg)) {
        buf_add(body, ",\"thinking\":{\"type\":\"disabled\"}");
    }
    buf_ch(body, '}');
    buf_free(&user);
}

/* 构造批量翻译请求体：把多条文本放进一个 JSON 数组，要求模型返回等长同序的数组。
   批量可大幅减少请求次数与 token 开销，是预热/大批量的主路径。 */
static void build_batch_request(ApiConfig *cfg, char **texts, size_t count, Buf *body) {
    Buf user;
    buf_init(&user);
    buf_ch(&user, '[');
    for (size_t i = 0; i < count; i++) {
        if (i) buf_ch(&user, ',');
        buf_json(&user, texts[i]);
    }
    buf_ch(&user, ']');

    buf_add(body, "{\"model\":");
    buf_json(body, cfg->model);
    buf_add(body, ",\"messages\":[{\"role\":\"system\",\"content\":");
    buf_json(body,
             "Translate each game text in the JSON array to Simplified Chinese. Return only a valid JSON array with the same length and order. Preserve tags, placeholders, variables, numbers, and line breaks.");
    buf_add(body, "},{\"role\":\"user\",\"content\":");
    buf_json(body, user.data);
    buf_add(body, "}],\"temperature\":0");
    if (should_disable_deepseek_thinking(cfg)) {
        buf_add(body, ",\"thinking\":{\"type\":\"disabled\"}");
    }
    buf_ch(body, '}');
    buf_free(&user);
}

/* Pool of independent WinHTTP channels so up to cfg->concurrency requests can
   be in flight at once. Each channel owns its session/connect handles and is
   guarded by its own lock, so a transport failure resets only that channel
   and never races with requests running on other channels. */
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

/* Remote transport/provider fallbacks cannot be repaired above this client:
   the endpoint and response are external. They never turn a failure into a
   cache hit; every fallback is rate-limited here with its exact stage and no
   request text or API key, so the original fault remains diagnosable. */
static void api_diag(ApiDiag reason, DWORD winhttp_error, DWORD status, const char *detail) {
    static const char *names[API_DIAG_COUNT] = {
        "endpoint-encoding", "key-encoding", "crack-url", "open-session",
        "connect", "open-request", "set-timeouts", "send", "receive",
        "query-status", "http-status", "query-body", "read-body",
        "body-too-large", "empty-body", "missing-content", "empty-content",
        "batch-shape"
    };
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
        /* WinHTTP reports an unsigned 32-bit byte count. Bound it before both
           the +1 allocation and WinHttpReadData so a hostile/custom endpoint
           cannot wrap the allocation size or force an oversized transient. */
        if ((size_t)avail > API_MAX_RESPONSE_BYTES - b.len) {
            api_diag(API_DIAG_BODY_TOO_LARGE, 0, 0, "limit=2097152");
            buf_free(&b);
            return 0;
        }
        char *tmp = (char *)xmalloc((size_t)avail + 1);
        DWORD rd = 0;
        if (!WinHttpReadData(req, tmp, avail, &rd)) {
            api_diag(API_DIAG_READ_BODY, GetLastError(), 0, NULL);
            free(tmp);
            buf_free(&b);
            return 0;
        }
        tmp[rd] = 0;
        if ((size_t)rd > API_MAX_RESPONSE_BYTES - b.len) {
            api_diag(API_DIAG_BODY_TOO_LARGE, 0, 0, "limit=2097152");
            free(tmp);
            buf_free(&b);
            return 0;
        }
        buf_addn(&b, tmp, rd);
        free(tmp);
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
    _snwprintf(headers, 1800, L"Content-Type: application/json\r\nAuthorization: Bearer %s\r\n", ch->key);
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
