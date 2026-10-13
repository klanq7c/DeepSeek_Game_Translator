#include "api_models.h"
#include <winhttp.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#define MODEL_BYTES (2 * 1024 * 1024)
#define MODEL_LIMIT 4096
#define TOKEN_LIMIT 131072
#define PAGE_LIMIT 64
enum { CATALOG_COMPATIBLE, CATALOG_ANTHROPIC, CATALOG_DASHSCOPE };

/* 有界 JSON 索引：只提取协议指定数组的直接模型 ID，其他字段仍完整校验。
   不使用宽松字符串搜索，避免把错误响应、嵌套 id 或截断内容当作目录。 */
typedef struct { int start, end, next; WCHAR type; } Token;
typedef struct { const WCHAR *s; int pos, count; Token *t; int invalid; } Parser;
static void ws(Parser *p) { while (p->s[p->pos] && wcschr(L" \r\n\t", p->s[p->pos])) p->pos++; }
static int hex(WCHAR c) {
    if (c >= L'0' && c <= L'9') return c - L'0';
    if (c >= L'a' && c <= L'f') return c - L'a' + 10;
    if (c >= L'A' && c <= L'F') return c - L'A' + 10;
    return -1;
}
static int value(Parser *p, int depth) {
    ws(p);
    if (depth > 64 || p->count == TOKEN_LIMIT) return -1;
    int n = p->count++;
    Token *t = p->t + n;
    t->start = p->pos; t->type = p->s[p->pos];
    WCHAR c = p->s[p->pos++];
    if (c == L'"') {
        while (p->s[p->pos] && p->s[p->pos] != L'"') {
            c = p->s[p->pos++];
            if (c < 32) return -1;
            if (c == L'\\') {
                c = p->s[p->pos++];
                if (!c) return -1;
                if (c == L'u') { for (int k = 0; k < 4; k++) if (hex(p->s[p->pos++]) < 0) return -1; }
                else if (!wcschr(L"\"\\/bfnrt", c)) return -1;
            }
        }
        if (p->s[p->pos++] != L'"') return -1;
    } else if (c == L'{' || c == L'[') {
        WCHAR end = c == L'{' ? L'}' : L']';
        ws(p);
        if (p->s[p->pos] != end) for (;;) {
            if (c == L'{') {
                ws(p);
                if (p->s[p->pos] != L'"' || value(p, depth + 1) < 0) return -1;
                ws(p); if (p->s[p->pos++] != L':') return -1;
            }
            if (value(p, depth + 1) < 0) return -1;
            ws(p);
            if (p->s[p->pos] != L',') break;
            p->pos++;
        }
        if (p->s[p->pos++] != end) return -1;
    } else if (c == L't' || c == L'f' || c == L'n') {
        const WCHAR *literal = c == L't' ? L"true" : c == L'f' ? L"false" : L"null";
        size_t len = wcslen(literal);
        if (wcsncmp(p->s + t->start, literal, len)) return -1;
        p->pos = t->start + (int)len;
    } else {
        p->pos = t->start;
        if (p->s[p->pos] == L'-') p->pos++;
        if (p->s[p->pos] == L'0') p->pos++;
        else {
            if (p->s[p->pos] < L'1' || p->s[p->pos] > L'9') return -1;
            while (p->s[p->pos] >= L'0' && p->s[p->pos] <= L'9') p->pos++;
        }
        if (p->s[p->pos] == L'.') {
            p->pos++; int start = p->pos;
            while (p->s[p->pos] >= L'0' && p->s[p->pos] <= L'9') p->pos++;
            if (start == p->pos) return -1;
        }
        if (p->s[p->pos] == L'e' || p->s[p->pos] == L'E') {
            p->pos++;
            if (p->s[p->pos] == L'+' || p->s[p->pos] == L'-') p->pos++;
            int start = p->pos;
            while (p->s[p->pos] >= L'0' && p->s[p->pos] <= L'9') p->pos++;
            if (start == p->pos) return -1;
        }
    }
    t->end = p->pos; t->next = p->count;
    return n;
}
static int string_value(Parser *p, int n, WCHAR *out, int cap) {
    Token *t = p->t + n;
    if (t->type != L'"') return 0;
    int used = 0;
    for (int i = t->start + 1; i < t->end - 1; i++) {
        WCHAR c = p->s[i];
        if (c == L'\\') {
            c = p->s[++i];
            if (c == L'u') { c = 0; for (int k = 0; k < 4; k++) c = (WCHAR)(c * 16 + hex(p->s[++i])); }
            else if (c == L'b') c = L'\b'; else if (c == L'f') c = L'\f';
            else if (c == L'n') c = L'\n'; else if (c == L'r') c = L'\r'; else if (c == L't') c = L'\t';
        }
        if (!c || used + 1 >= cap) return 0;
        out[used++] = c;
    }
    out[used] = 0; return 1;
}
static int field(Parser *p, int obj, const WCHAR *name) {
    if (p->t[obj].type != L'{') return -1;
    int found = -1;
    for (int i = obj + 1; i < p->t[obj].next;) {
        WCHAR key[64]; int v = i + 1;
        if (string_value(p, i, key, 64) && !wcscmp(key, name)) {
            if (found >= 0) { p->invalid = 1; return -1; }
            found = v;
        }
        i = p->t[v].next;
    }
    return found;
}
static int number_field(Parser *p, int obj, const WCHAR *name) {
    int n = field(p, obj, name);
    if (n < 0 || (p->t[n].type != L'-' && (p->t[n].type < L'0' || p->t[n].type > L'9'))) return -1;
    double result = wcstod(p->s + p->t[n].start, NULL);
    if (!(result >= 0 && result <= MODEL_LIMIT)) return -1;
    return result == (int)result ? (int)result : -1;
}
static int parse_models(const char *body, int bytes, ApiModels *r, int kind, int page,
                        int *received, int *total, int *has_more, WCHAR *cursor) {
    int len = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, body, bytes, NULL, 0);
    if (!len || memchr(body, 0, (size_t)bytes)) return 0;
    WCHAR *wide = calloc((size_t)len + 1, sizeof(WCHAR));
    Token *tokens = calloc(TOKEN_LIMIT, sizeof(Token));
    if (!wide || !tokens) { free(wide); free(tokens); return 0; }
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, body, bytes, wide, len);
    Parser p = { wide, 0, 0, tokens, 0 };
    int ok = value(&p, 0) == 0;
    if (ok) ws(&p);
    if (!ok || wide[p.pos]) { free(wide); free(tokens); return 0; }
    int data = field(&p, 0, L"data"), more = field(&p, 0, L"has_more"), count = 0;
    *has_more = 0; cursor[0] = 0;
    if (kind == CATALOG_DASHSCOPE) {
        int output = field(&p, 0, L"output"), success = field(&p, 0, L"success");
        if (output < 0 || (success >= 0 && tokens[success].type != L't')) ok = 0;
        else {
            data = field(&p, output, L"models");
            int expected = number_field(&p, output, L"total");
            if (expected < 0 || (*total >= 0 && *total != expected) || number_field(&p, output, L"page_no") != page) ok = 0;
            *total = expected;
        }
    } else {
        if (more >= 0 && tokens[more].type != L't' && tokens[more].type != L'f') ok = 0;
        if (more >= 0 && tokens[more].type == L't') {
            int last = field(&p, 0, L"last_id");
            if (kind != CATALOG_ANTHROPIC || last < 0 || !string_value(&p, last, cursor, 256) || !cursor[0]) ok = 0;
            *has_more = 1;
        }
    }
    if (data < 0 || tokens[data].type != L'[') ok = 0;
    for (int i = ok ? data + 1 : p.count; ok && i < tokens[data].next; i = tokens[i].next) {
        int id = field(&p, i, kind == CATALOG_DASHSCOPE ? L"model" : L"id"); WCHAR model[256];
        count++;
        if (id < 0 || !string_value(&p, id, model, 256) || !model[0]) { ok = 0; break; }
        int utf8 = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, model, -1, NULL, 0, NULL, NULL);
        if (utf8 < 2 || utf8 > 256) { ok = 0; break; }
        for (int j = 0; model[j]; j++) if (model[j] <= 32 || model[j] == 127) ok = 0;
        int duplicate = 0;
        for (int j = 0; j < r->count; j++) if (!wcscmp(r->items[j], model)) duplicate = 1;
        if (duplicate || !ok) continue;
        if (r->count == MODEL_LIMIT) { ok = 0; break; }
        void *next = realloc(r->items, ((size_t)r->count + 1) * sizeof(*r->items));
        if (!next) { ok = 0; break; }
        r->items = next; wcscpy(r->items[r->count++], model);
    }
    *received += count;
    if (kind == CATALOG_DASHSCOPE) {
        if (*received > *total || (!count && *received < *total)) ok = 0;
        *has_more = *received < *total;
    }
    if (*has_more && !count) ok = 0;
    free(wide); free(tokens); return ok && !p.invalid;
}

static int dashscope_host(const WCHAR *host) {
    if (!_wcsicmp(host, L"dashscope.aliyuncs.com") || !_wcsicmp(host, L"dashscope-intl.aliyuncs.com") ||
        !_wcsicmp(host, L"cn-hongkong.dashscope.aliyuncs.com")) return 1;
    const WCHAR *regions[] = { L".cn-beijing.maas.aliyuncs.com", L".ap-northeast-1.maas.aliyuncs.com",
        L".eu-central-1.maas.aliyuncs.com", L".us-east-1.maas.aliyuncs.com" };
    for (int i = 0; i < 4; i++) {
        size_t n = wcslen(host), s = wcslen(regions[i]);
        if (n > s && !_wcsicmp(host + n - s, regions[i])) {
            for (size_t j = 0; j < n - s; j++)
                if (!((host[j] >= L'a' && host[j] <= L'z') || (host[j] >= L'A' && host[j] <= L'Z') ||
                      (host[j] >= L'0' && host[j] <= L'9') || host[j] == L'-')) return 0;
            return 1;
        }
    }
    return 0;
}
static int catalog_url(const WCHAR *endpoint, WCHAR *url, size_t cap, int *kind) {
    const WCHAR *suffix = L"/chat/completions";
    size_t n = wcslen(endpoint), s = wcslen(suffix);
    *kind = CATALOG_COMPATIBLE;
    while (n && endpoint[n - 1] == L'/') n--;
    if (n < s || wcsncmp(endpoint + n - s, suffix, s) || n - s + 8 >= cap ||
        wcspbrk(endpoint, L"?#\r\n")) return 0;
    WCHAR host[1024], path[1100]; URL_COMPONENTS u; ZeroMemory(&u, sizeof(u)); u.dwStructSize = sizeof(u);
    u.lpszHostName = host; u.dwHostNameLength = 1024; u.lpszUrlPath = path; u.dwUrlPathLength = 1100;
    if (!WinHttpCrackUrl(endpoint, 0, 0, &u)) return 0;
    size_t origin = wcslen(endpoint) - wcslen(path), path_len = wcslen(path);
    while (path_len && path[path_len - 1] == L'/') path[--path_len] = 0;
    wmemcpy(url, endpoint, n - s); wcscpy(url + n - s, L"/models");
    const WCHAR *query = L"";
    if (!_wcsicmp(host, L"api.anthropic.com")) { *kind = CATALOG_ANTHROPIC; query = L"?limit=1000"; }
    else if (!_wcsicmp(host, L"api.siliconflow.cn") || !_wcsicmp(host, L"api.siliconflow.com")) query = L"?sub_type=chat";
    else if (dashscope_host(host) && !wcscmp(path, L"/compatible-mode/v1/chat/completions")) {
        /* 百炼目录与兼容聊天接口路径不同；保持原主机，绝不猜业务空间或跨主机发送 Key。 */
        *kind = CATALOG_DASHSCOPE;
        const WCHAR *catalog = L"/api/v1/models?capabilities=TG&capabilities=Reasoning&page_size=100";
        if (origin + wcslen(catalog) >= cap) return 0;
        wmemcpy(url, endpoint, origin); wcscpy(url + origin, catalog);
    }
    if (wcslen(url) + wcslen(query) >= cap) return 0;
    wcscat(url, query); return 1;
}
int api_models_url(const WCHAR *endpoint, WCHAR *url, size_t cap) { int kind; return catalog_url(endpoint, url, cap, &kind); }
void api_models_free(ApiModels *r) { free(r->items); r->items = NULL; r->count = 0; }

static int encode_cursor(const WCHAR *cursor, WCHAR *out) {
    char bytes[256]; int n = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, cursor, -1, bytes, 256, NULL, NULL);
    if (n < 2) return 0;
    const WCHAR *hexes = L"0123456789ABCDEF";
    for (int i = 0; i < n - 1; i++) { unsigned char c = (unsigned char)bytes[i]; out[i * 3] = L'%'; out[i * 3 + 1] = hexes[c >> 4]; out[i * 3 + 2] = hexes[c & 15]; }
    out[(n - 1) * 3] = 0; return 1;
}

/* 分页复用同一连接和认证，不接受服务端给出的跳转 URL；任何页失败都丢弃整份目录。 */
static int fetch_catalog(const WCHAR *url, const WCHAR *key, int kind, ApiModels *r) {
    WCHAR host[1024], base_path[1100], path[2000], header[1200], cursor[256] = L"", previous[256] = L"";
    HINTERNET session = NULL, connect = NULL, request = NULL;
    char *body = NULL; DWORD status = 0, error = 0; int ok = 0;
    const WCHAR *stage = L"地址不支持；请使用以 /chat/completions 结尾的地址";
    ZeroMemory(r, sizeof(*r));
    URL_COMPONENTS u; ZeroMemory(&u, sizeof(u)); u.dwStructSize = sizeof(u);
    u.lpszHostName = host; u.dwHostNameLength = 1024;
    u.lpszUrlPath = base_path; u.dwUrlPathLength = 1100;
    u.dwUserNameLength = (DWORD)-1; u.dwPasswordLength = (DWORD)-1;
    if (!WinHttpCrackUrl(url, 0, 0, &u) || u.dwUserNameLength || u.dwPasswordLength) goto done;
    int local = !_wcsicmp(host, L"localhost") || !wcscmp(host, L"127.0.0.1") || !wcscmp(host, L"[::1]") || !wcscmp(host, L"::1");
    stage = L"远程服务需要 HTTPS";
    if (u.nScheme != INTERNET_SCHEME_HTTPS && !(u.nScheme == INTERNET_SCHEME_HTTP && local)) goto done;
    stage = L"API Key 含无效字符";
    if (wcslen(key) > 1023) goto done;
    for (const WCHAR *k = key; *k; k++) if (*k <= 32 || *k > 126) goto done;
    stage = L"网络请求失败";
    session = WinHttpOpen(L"DST-Models/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, NULL, NULL, 0);
    if (!session || !WinHttpSetTimeouts(session, 5000, 5000, 5000, 5000)) goto network;
    connect = WinHttpConnect(session, host, u.nPort, 0);
    if (!connect) goto network;
    body = malloc(MODEL_BYTES + 1);
    stage = L"模型列表内存分配失败";
    if (!body) goto done;
    int received = 0, total = -1, bytes_read = 0;
    ULONGLONG started = GetTickCount64();
    for (int page = 1; ; page++) {
        stage = L"模型目录分页超出限制或读取超时";
        if (page > PAGE_LIMIT || GetTickCount64() - started > 60000) goto done;
        wcscpy(path, base_path);
        if (kind == CATALOG_DASHSCOPE) {
            WCHAR number[32]; _snwprintf(number, 32, L"%spage_no=%d", wcschr(path, L'?') ? L"&" : L"?", page); wcscat(path, number);
        } else if (cursor[0]) {
            WCHAR encoded[768];
            stage = L"模型列表格式无效、不完整或超出限制";
            if (!encode_cursor(cursor, encoded)) goto done;
            wcscat(path, wcschr(path, L'?') ? L"&after_id=" : L"?after_id="); wcscat(path, encoded);
        }
        stage = L"网络请求失败";
        request = WinHttpOpenRequest(connect, L"GET", path, NULL, NULL, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                     u.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0);
        if (!request) goto network;
        DWORD redirect = WINHTTP_OPTION_REDIRECT_POLICY_NEVER;
        if (!WinHttpSetOption(request, WINHTTP_OPTION_REDIRECT_POLICY, &redirect, sizeof(redirect))) goto network;
        if (*key) {
            _snwprintf(header, 1200, kind == CATALOG_ANTHROPIC ? L"x-api-key: %s\r\nanthropic-version: 2023-06-01\r\n" : L"Authorization: Bearer %s\r\n", key);
            if (!WinHttpAddRequestHeaders(request, header, (DWORD)-1, WINHTTP_ADDREQ_FLAG_ADD)) goto network;
        }
        if (!WinHttpSendRequest(request, L"Accept: application/json\r\n", (DWORD)-1, NULL, 0, 0, 0) || !WinHttpReceiveResponse(request, NULL)) goto network;
        DWORD size = sizeof(status);
        if (!WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, NULL, &status, &size, NULL)) goto network;
        stage = L"厂商返回 HTTP 错误";
        if (status != 200) goto done;
        /* WinHTTP 的提前 EOF 不一定返回错误；有长度声明时必须验证完整接收。 */
        WCHAR length_text[64]; DWORD length_size = sizeof(length_text);
        int expected = -1;
        if (WinHttpQueryHeaders(request, WINHTTP_QUERY_CONTENT_LENGTH, NULL, length_text, &length_size, NULL)) {
            stage = L"模型列表过大或读取超时";
            expected = 0;
            for (const WCHAR *p = length_text; *p; p++) {
                if (*p < L'0' || *p > L'9' || expected >= MODEL_BYTES) goto done;
                expected = expected * 10 + *p - L'0';
            }
            if (expected >= MODEL_BYTES) goto done;
        } else if (GetLastError() != ERROR_WINHTTP_HEADER_NOT_FOUND) goto network;
        int used = 0; ULONGLONG start = GetTickCount64();
        for (;;) {
            DWORD got = 0;
            stage = L"模型列表过大或读取超时";
            if (used == MODEL_BYTES || GetTickCount64() - start > 15000) goto done;
            stage = L"网络读取失败";
            if (!WinHttpReadData(request, body + used, (DWORD)(MODEL_BYTES - used), &got)) goto network;
            if (!got) break;
            used += (int)got;
            bytes_read += (int)got;
            if (bytes_read > MODEL_BYTES * 8 || GetTickCount64() - started > 60000) { stage = L"模型目录分页超出限制或读取超时"; goto done; }
        }
        stage = L"模型列表响应未完整接收";
        if (expected >= 0 && used != expected) goto done;
        stage = L"模型列表格式无效、不完整或超出限制";
        int more = 0, before = r->count;
        wcscpy(previous, cursor);
        if (!parse_models(body, used, r, kind, page, &received, &total, &more, cursor)) goto done;
        WinHttpCloseHandle(request); request = NULL;
        if (!more) break;
        if (r->count == before || (kind == CATALOG_ANTHROPIC && !wcscmp(previous, cursor))) goto done;
    }
    stage = L"厂商未返回可选模型；可手动输入";
    if (!r->count) goto done;
    _snwprintf(r->message, 192, L"已更新 %d 个模型；请选择聊天模型，也可手动输入", r->count);
    ok = 1; goto done;
network:
    error = GetLastError();
done:
    if (!ok) { api_models_free(r); _snwprintf(r->message, 192, L"%s（HTTP %lu / 错误 %lu）；可手动输入", stage, status, error); }
    if (request) WinHttpCloseHandle(request);
    if (connect) WinHttpCloseHandle(connect);
    if (session) WinHttpCloseHandle(session);
    SecureZeroMemory(header, sizeof(header)); free(body); return ok;
}
int api_models_fetch(const WCHAR *endpoint, const WCHAR *key, ApiModels *r) {
    WCHAR url[1100]; int kind;
    if (!catalog_url(endpoint, url, 1100, &kind)) {
        ZeroMemory(r, sizeof(*r));
        wcscpy(r->message, L"地址不支持；请使用以 /chat/completions 结尾的地址（HTTP 0 / 错误 0）；可手动输入"); return 0;
    }
    return fetch_catalog(url, key, kind, r);
}
