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

void api_config_init(ApiConfig *cfg) {
    memset(cfg, 0, sizeof *cfg);
    cfg->timeout_ms = 15000;
    cfg->concurrency = API_CONCURRENCY_MAX;
    snprintf(cfg->endpoint, sizeof cfg->endpoint, "%s", "https://api.deepseek.com/v1/chat/completions");
    snprintf(cfg->model, sizeof cfg->model, "%s", "deepseek-chat");
}

int api_config_load(ApiConfig *cfg, const char *path) {
    api_config_init(cfg);
    if (!path || !*path) return 0;
    GetPrivateProfileStringA("api", "endpoint", cfg->endpoint, cfg->endpoint, sizeof cfg->endpoint, path);
    GetPrivateProfileStringA("api", "model", cfg->model, cfg->model, sizeof cfg->model, path);
    GetPrivateProfileStringA("api", "key", "", cfg->key, sizeof cfg->key, path);
    cfg->timeout_ms = (int)GetPrivateProfileIntA("api", "timeout_ms", (UINT)cfg->timeout_ms, path);
    if (cfg->timeout_ms < 3000) cfg->timeout_ms = 3000;
    if (cfg->timeout_ms > 60000) cfg->timeout_ms = 60000;
    cfg->concurrency = (int)GetPrivateProfileIntA("api", "concurrency", (UINT)cfg->concurrency, path);
    if (cfg->concurrency < 1) cfg->concurrency = 1;
    if (cfg->concurrency > API_CONCURRENCY_MAX) cfg->concurrency = API_CONCURRENCY_MAX;
    cfg->endpoint[sizeof cfg->endpoint - 1] = 0;
    cfg->model[sizeof cfg->model - 1] = 0;
    cfg->key[sizeof cfg->key - 1] = 0;
    cfg->enabled = cfg->endpoint[0] && cfg->model[0] && cfg->key[0];
    return cfg->enabled;
}

static int utf8_to_wide(const char *s, WCHAR *out, int cap) {
    int n = MultiByteToWideChar(CP_UTF8, 0, s ? s : "", -1, out, cap);
    if (!n) n = MultiByteToWideChar(CP_ACP, 0, s ? s : "", -1, out, cap);
    if (!n && cap > 0) out[0] = 0;
    return n != 0;
}

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

static void trim_inplace(char *s) {
    char *p = s;
    while (*p && isspace((unsigned char)*p)) p++;
    if (p != s) memmove(s, p, strlen(p) + 1);
    size_t n = strlen(s);
    while (n && isspace((unsigned char)s[n - 1])) s[--n] = 0;
}

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
    buf_add(body, "}],\"temperature\":0}");
    buf_free(&user);
}

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
    buf_add(body, "}],\"temperature\":0}");
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
    char endpoint_id[1024];
    char key_id[1024];
    int ready;
} ApiChannel;

static ApiChannel g_channels[API_CONCURRENCY_MAX]; /* zero-init == SRWLOCK_INIT */
static volatile LONG g_rr;

static void api_reset_locked(ApiChannel *ch) {
    if (ch->connect) WinHttpCloseHandle(ch->connect);
    if (ch->session) WinHttpCloseHandle(ch->session);
    ch->connect = NULL;
    ch->session = NULL;
    ch->ready = 0;
    ch->endpoint_id[0] = 0;
    ch->key_id[0] = 0;
}

static int api_prepare_locked(ApiChannel *ch, ApiConfig *cfg) {
    if (ch->ready && !strcmp(ch->endpoint_id, cfg->endpoint) && !strcmp(ch->key_id, cfg->key)) return 1;
    api_reset_locked(ch);

    WCHAR url[2048];
    if (!utf8_to_wide(cfg->endpoint, url, 2048)) return 0;
    if (!utf8_to_wide(cfg->key, ch->key, 1400)) return 0;

    URL_COMPONENTSW uc;
    memset(&uc, 0, sizeof uc);
    uc.dwStructSize = sizeof uc;
    uc.lpszHostName = ch->host;
    uc.dwHostNameLength = 512;
    uc.lpszUrlPath = ch->path;
    uc.dwUrlPathLength = 2048;
    if (!WinHttpCrackUrl(url, 0, 0, &uc)) return 0;
    if (uc.dwHostNameLength < 512) ch->host[uc.dwHostNameLength] = 0;
    else ch->host[511] = 0;
    if (uc.dwUrlPathLength < 2048) ch->path[uc.dwUrlPathLength] = 0;
    else ch->path[2047] = 0;

    ch->port = uc.nPort;
    ch->flags = uc.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0;
    ch->session = WinHttpOpen(L"DeepSeek Game Translator/3.1",
                              WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                              WINHTTP_NO_PROXY_NAME,
                              WINHTTP_NO_PROXY_BYPASS, 0);
    if (!ch->session) return 0;

    ch->connect = WinHttpConnect(ch->session, ch->host, ch->port, 0);
    if (!ch->connect) {
        api_reset_locked(ch);
        return 0;
    }

    snprintf(ch->endpoint_id, sizeof ch->endpoint_id, "%s", cfg->endpoint);
    snprintf(ch->key_id, sizeof ch->key_id, "%s", cfg->key);
    ch->ready = 1;
    return 1;
}

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

static int read_response(HINTERNET req, char **out) {
    Buf b;
    buf_init(&b);
    for (;;) {
        DWORD avail = 0;
        if (!WinHttpQueryDataAvailable(req, &avail)) break;
        if (!avail) break;
        char *tmp = (char *)xmalloc(avail + 1);
        DWORD rd = 0;
        if (!WinHttpReadData(req, tmp, avail, &rd)) {
            free(tmp);
            break;
        }
        tmp[rd] = 0;
        if (b.len + rd > 2 * 1024 * 1024) {
            free(tmp);
            break;
        }
        buf_addn(&b, tmp, rd);
        free(tmp);
    }
    if (!b.len) {
        buf_free(&b);
        return 0;
    }
    *out = b.data;
    return 1;
}

static int send_chat_request(ApiConfig *cfg, Buf *body, char **content_out) {
    ApiChannel *ch = api_acquire_channel(cfg);
    if (!api_prepare_locked(ch, cfg)) {
        ReleaseSRWLockExclusive(&ch->lock);
        return 0;
    }

    HINTERNET req = WinHttpOpenRequest(ch->connect, L"POST", ch->path, NULL, WINHTTP_NO_REFERER,
                                       WINHTTP_DEFAULT_ACCEPT_TYPES, ch->flags);
    if (!req) {
        api_reset_locked(ch);
        ReleaseSRWLockExclusive(&ch->lock);
        return 0;
    }

    DWORD timeout = (DWORD)cfg->timeout_ms;
    WinHttpSetTimeouts(req, timeout, timeout, timeout, timeout);

    WCHAR headers[1800];
    _snwprintf(headers, 1800, L"Content-Type: application/json\r\nAuthorization: Bearer %s\r\n", ch->key);
    headers[1799] = 0;

    int ok = 0;
    int transport_ok = 0;
    if (WinHttpSendRequest(req, headers, (DWORD)-1, body->data, (DWORD)body->len, (DWORD)body->len, 0) &&
        WinHttpReceiveResponse(req, NULL)) {
        transport_ok = 1;
        DWORD status = 0, sz = sizeof status;
        WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                            WINHTTP_HEADER_NAME_BY_INDEX, &status, &sz, WINHTTP_NO_HEADER_INDEX);
        char *raw = NULL;
        if (status >= 200 && status < 300 && read_response(req, &raw)) {
            char *content = json_get_str(raw, "content");
            if (content) {
                normalize_translation(content);
                if (*content) {
                    *content_out = content;
                    content = NULL;
                    ok = 1;
                }
                free(content);
            }
            free(raw);
        }
    }

    WinHttpCloseHandle(req);
    if (!transport_ok) api_reset_locked(ch);
    ReleaseSRWLockExclusive(&ch->lock);
    return ok;
}

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

int api_translate(ApiConfig *cfg, const char *text, char **out) {
    if (!cfg || !cfg->enabled || !text || !*text || has_cjk(text)) return 0;

    Buf body;
    buf_init(&body);
    build_request(cfg, text, &body);

    int ok = send_chat_request(cfg, &body, out);
    buf_free(&body);
    return ok;
}

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
