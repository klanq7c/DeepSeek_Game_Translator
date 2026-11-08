#include "api_models.h"
#include "api_connection.h"
#include <stdio.h>
#include <winhttp.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <math.h>

#define MODEL_BYTES (2 * 1024 * 1024)
#define MODEL_LIMIT 4096
#define TOKEN_LIMIT 131072
#define PAGE_LIMIT 64
enum { CATALOG_COMPATIBLE, CATALOG_ANTHROPIC, CATALOG_DASHSCOPE };

/* 有界 JSON 索引：只提取协议指定数组的直接模型 ID 与可选显示名称，其他字段仍完整校验。
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
        if (!isfinite(wcstod(p->s + t->start, NULL))) return -1;
    }
    t->end = p->pos; t->next = p->count;
    return n;
}
typedef struct { int object; const WCHAR *start, *end; } Property;
/* 比较解码后的完整键，不受协议字段提取缓冲区的长度或 NUL 限制。 */
static WCHAR property_char(const WCHAR **read) {
    WCHAR c = *(*read)++;
    if (c == L'\\') {
        c = *(*read)++;
        if (c == L'u') { c = 0; for (int k = 0; k < 4; k++) c = (WCHAR)(c * 16 + hex(*(*read)++)); }
        else if (c == L'b') c = L'\b'; else if (c == L'f') c = L'\f';
        else if (c == L'n') c = L'\n'; else if (c == L'r') c = L'\r'; else if (c == L't') c = L'\t';
    }
    return c;
}
static int property_compare(const void *left, const void *right) {
    const Property *a = left, *b = right;
    if (a->object != b->object) return a->object < b->object ? -1 : 1;
    const WCHAR *x = a->start, *y = b->start;
    while (x < a->end && y < b->end) {
        WCHAR xc = property_char(&x), yc = property_char(&y);
        if (xc != yc) return xc < yc ? -1 : 1;
    }
    return (x < a->end) - (y < b->end);
}
/* 外部响应可能含有重复的未使用字段；和 C# 严格解析保持一致，拒绝整份歧义响应。
   按对象和完整键排序，避免大量元数据造成逐键两两比较的平方耗时；内存由 token 上限约束。 */
static int unique_properties(const Parser *p) {
    if (p->count < 3) return 1;
    Property *properties = malloc((size_t)(p->count / 2) * sizeof(*properties));
    if (!properties) return 0;
    int count = 0, ok = 1;
    for (int obj = 0; obj < p->count; obj++) if (p->t[obj].type == L'{') {
        for (int key = obj + 1; key < p->t[obj].next; key = p->t[key + 1].next) {
            properties[count++] = (Property){obj, p->s + p->t[key].start + 1, p->s + p->t[key].end - 1};
        }
    }
    qsort(properties, (size_t)count, sizeof(*properties), property_compare);
    for (int i = 1; i < count; i++) if (property_compare(properties + i - 1, properties + i) == 0) { ok = 0; break; }
    free(properties); return ok;
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
    if (!ok || wide[p.pos] || !unique_properties(&p)) { free(wide); free(tokens); return 0; }
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
        WCHAR name[256] = {0}; int named = field(&p, i, L"name");
        /* 厂商可选名称可能格式错误，无法在本地修正上游元数据；只降级显示 ID，
           不改变可调用 ID。降级数量进入结果状态及启动器日志，不掩盖目录错误。 */
        if (named >= 0 && tokens[named].type != L'n') {
            int valid = string_value(&p, named, name, 256);
            for (int j = 0; valid && name[j]; j++) if (name[j] < 32 || name[j] == 127) valid = 0;
            if (valid && !WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, name, -1, NULL, 0, NULL, NULL)) valid = 0;
            if (!valid) { name[0] = 0; r->invalid_names++; }
        }
        int duplicate = 0;
        for (int j = 0; j < r->count; j++) if (!wcscmp(r->items[j].id, model)) duplicate = 1;
        if (duplicate || !ok) continue;
        if (r->count == MODEL_LIMIT) { ok = 0; break; }
        void *next = realloc(r->items, ((size_t)r->count + 1) * sizeof(*r->items));
        if (!next) { ok = 0; break; }
        r->items = next; wcscpy(r->items[r->count].id, model); wcscpy(r->items[r->count++].name, name);
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
static const WCHAR *http_error_stage(DWORD status) {
    switch (status) {
    case 401: return L"认证失败：检查当前厂商的 API Key、账号权限或 IP 限制";
    case 403: return L"访问被拒绝：检查模型目录权限或服务地区限制";
    case 404: return L"未找到模型目录：检查 API 地址及厂商的目录支持";
    case 407: return L"代理要求认证：请检查代理连接设置";
    case 429: return L"请求受限：请检查调用频率、额度或账单";
    default: return status >= 500 ? L"服务暂时不可用：请稍后重试" : L"厂商返回 HTTP 错误";
    }
}
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
        stage = http_error_stage(status);
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
    if (r->invalid_names) _snwprintf(r->message, 192, L"已更新 %d 个模型；%d 个显示名称无效，已使用模型 ID", r->count, r->invalid_names);
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

/* 连通检测只接受完整聊天响应里的非空文本，不把 HTTP 200、目录、错误对象
   或仅有推理内容当作模型可用。正文只在当前工作线程内解析，绝不写入日志。 */
#define CONNECTION_BYTES 65536
/* 本机推理的首次加载发生在提供商边界，目录可访问不代表模型已经驻留。
   给回环地址单独的响应等待预算；不重试、不改模型或将超时当成功。
   无效地址在正式请求入口诊断，此处只为网络与窗口提供一致的等待策略。 */
int api_connection_wait_ms(const WCHAR *endpoint) {
    WCHAR host[1024]; URL_COMPONENTS u; ZeroMemory(&u, sizeof(u)); u.dwStructSize = sizeof(u);
    u.lpszHostName = host; u.dwHostNameLength = 1024;
    u.dwUserNameLength = (DWORD)-1; u.dwPasswordLength = (DWORD)-1;
    if (!WinHttpCrackUrl(endpoint, 0, 0, &u) || u.dwUserNameLength || u.dwPasswordLength ||
        (u.nScheme != INTERNET_SCHEME_HTTP && u.nScheme != INTERNET_SCHEME_HTTPS)) return 15000;
    return !_wcsicmp(host, L"localhost") || !wcscmp(host, L"127.0.0.1") ||
        !wcscmp(host, L"[::1]") || !wcscmp(host, L"::1") ? 120000 : 15000;
}
static int connection_has_text(const WCHAR *s) {
    for (; *s; s++) if (!(*s == 32 || (*s >= 9 && *s <= 13) || *s == 0x85 ||
        *s == 0xa0 || *s == 0x1680 || (*s >= 0x2000 && *s <= 0x200a) ||
        *s == 0x2028 || *s == 0x2029 || *s == 0x202f || *s == 0x205f || *s == 0x3000)) return 1;
    return 0;
}
static int connection_response(const char *body, int bytes) {
    int len = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, body, bytes, NULL, 0), ok = 0;
    if (!len || memchr(body, 0, (size_t)bytes)) return 0;
    WCHAR *wide = calloc((size_t)len + 1, sizeof(WCHAR)), *content = calloc((size_t)len + 1, sizeof(WCHAR));
    Token *tokens = calloc(TOKEN_LIMIT, sizeof(Token));
    if (!wide || !tokens || !content) goto done;
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, body, bytes, wide, len);
    Parser p = {wide, 0, 0, tokens, 0};
    if (value(&p, 0) != 0) goto done;
    ws(&p); if (wide[p.pos] || !unique_properties(&p)) goto done;
    int error = field(&p, 0, L"error"), choices = field(&p, 0, L"choices");
    if (error >= 0 || choices < 0 || tokens[choices].type != L'[' || choices + 1 == tokens[choices].next) goto done;
    int message = field(&p, choices + 1, L"message");
    if (message < 0) goto done;
    int text = field(&p, message, L"content");
    if (text >= 0 && string_value(&p, text, content, len + 1) && connection_has_text(content) && !p.invalid &&
        WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, content, -1, NULL, 0, NULL, NULL)) ok = 1;
done:
    free(wide); free(content); free(tokens); return ok;
}
static const WCHAR *connection_http_error(DWORD status) {
    if (status == 401) return L"认证失败；请检查 API Key";
    if (status == 403) return L"访问被拒绝；请检查账号和模型权限";
    if (status == 404) return L"接口或模型不存在；请检查地址和模型 ID";
    if (status == 407) return L"代理需要认证；请检查系统代理";
    if (status == 429) return L"请求受限；请检查额度或稍后重试";
    if (status >= 300 && status < 400) return L"接口发生重定向；请填写最终地址";
    if (status >= 500) return L"提供商暂时不可用；请稍后重试";
    return L"请求被拒绝；请检查模型、请求兼容性或账号额度";
}
void api_connection_test(const WCHAR *endpoint, const WCHAR *model, const WCHAR *key, ApiConnection *r) {
    HINTERNET session = NULL, connect = NULL, request = NULL;
    WCHAR host[1024], path[1100], header[1200] = {0};
    char utf8[256], escaped[1536], payload[2048], *body = NULL;
    DWORD status = 0, error = 0;
    const WCHAR *stage = L"请填写有效的完整 API 地址";
    ZeroMemory(r, sizeof(*r));
    if (!*endpoint || wcslen(endpoint) > 1023 || wcspbrk(endpoint, L" \r\n\t#\\")) goto done;
    URL_COMPONENTS u; ZeroMemory(&u, sizeof(u)); u.dwStructSize = sizeof(u);
    u.lpszHostName = host; u.dwHostNameLength = 1024;
    u.lpszUrlPath = path; u.dwUrlPathLength = 1100;
    u.dwUserNameLength = (DWORD)-1; u.dwPasswordLength = (DWORD)-1;
    if (!WinHttpCrackUrl(endpoint, 0, 0, &u) || u.dwUserNameLength || u.dwPasswordLength) goto done;
    int local = !_wcsicmp(host, L"localhost") || !wcscmp(host, L"127.0.0.1") || !wcscmp(host, L"[::1]") || !wcscmp(host, L"::1");
    stage = L"远程服务需要 HTTPS";
    if (u.nScheme != INTERNET_SCHEME_HTTPS && !(u.nScheme == INTERNET_SCHEME_HTTP && local)) goto done;
    stage = L"请填写模型 ID（最多 255 个 UTF-8 字节）";
    if (!connection_has_text(model) || !WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, model, -1, utf8, sizeof(utf8), NULL, NULL)) goto done;
    stage = L"API Key 含无效字符";
    if (wcslen(key) > 1023) goto done;
    for (const WCHAR *k = key; *k; k++) if (*k <= 32 || *k > 126) goto done;
    int used = 0;
    for (const unsigned char *s = (const unsigned char *)utf8; *s; s++) {
        if (*s < 32) { snprintf(escaped + used, sizeof(escaped) - (size_t)used, "\\u%04x", *s); used += 6; }
        else { if (*s == '"' || *s == '\\') escaped[used++] = '\\'; escaped[used++] = (char)*s; }
    }
    escaped[used] = 0;
    /* 与翻译请求采用相同的非思考选项，但模型 ID 严格使用当前草稿，不做升级替换。 */
    const char *option = "";
    if (!_wcsicmp(host, L"api.deepseek.com") && (!wcscmp(model, L"deepseek-flash") || !wcsncmp(model, L"deepseek-v4-", 12)))
        option = ",\"thinking\":{\"type\":\"disabled\"}";
    if (local && u.nPort == 11434 && (!wcscmp(path, L"/v1/chat/completions") || !wcscmp(path, L"/v1/chat/completions/") ||
        !wcsncmp(path, L"/v1/chat/completions?", 21) || !wcsncmp(path, L"/v1/chat/completions/?", 22))) option = ",\"reasoning_effort\":\"none\"";
    snprintf(payload, sizeof(payload), "{\"model\":\"%s\",\"messages\":[{\"role\":\"user\",\"content\":\"Reply with OK.\"}],\"temperature\":0,\"max_tokens\":128,\"stream\":false%s}", escaped, option);
    stage = L"网络请求失败";
    session = WinHttpOpen(L"DST-Connection/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, NULL, NULL, 0);
    if (!session || !WinHttpSetTimeouts(session, 5000, 5000, 10000, api_connection_wait_ms(endpoint))) goto network;
    connect = WinHttpConnect(session, host, u.nPort, 0);
    if (!connect) goto network;
    request = WinHttpOpenRequest(connect, L"POST", path, NULL, NULL, WINHTTP_DEFAULT_ACCEPT_TYPES,
        u.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0);
    if (!request) goto network;
    DWORD redirect = WINHTTP_OPTION_REDIRECT_POLICY_NEVER;
    if (!WinHttpSetOption(request, WINHTTP_OPTION_REDIRECT_POLICY, &redirect, sizeof(redirect))) goto network;
    if (*key) {
        _snwprintf(header, 1200, L"Authorization: Bearer %s\r\n", key);
        if (!WinHttpAddRequestHeaders(request, header, (DWORD)-1, WINHTTP_ADDREQ_FLAG_ADD)) goto network;
    }
    if (!WinHttpSendRequest(request, L"Content-Type: application/json\r\nAccept: application/json\r\n", (DWORD)-1,
        payload, (DWORD)strlen(payload), (DWORD)strlen(payload), 0) || !WinHttpReceiveResponse(request, NULL)) goto network;
    /* 收到响应头后恢复短读取超时；首次加载预算不能放大响应体挂起的等待。 */
    if (!WinHttpSetTimeouts(request, 5000, 5000, 10000, 15000)) goto network;
    DWORD size = sizeof(status);
    if (!WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, NULL, &status, &size, NULL)) goto network;
    stage = connection_http_error(status); if (status != 200) goto done;
    DWORD expected = 0, length_size = sizeof(expected); int has_length;
    has_length = WinHttpQueryHeaders(request, WINHTTP_QUERY_CONTENT_LENGTH | WINHTTP_QUERY_FLAG_NUMBER, NULL, &expected, &length_size, NULL);
    if (!has_length && GetLastError() != ERROR_WINHTTP_HEADER_NOT_FOUND) goto network;
    stage = L"检测响应过大"; if (has_length && expected >= CONNECTION_BYTES) goto done;
    stage = L"检测内存分配失败"; body = malloc(CONNECTION_BYTES); if (!body) goto done;
    used = 0; ULONGLONG started = GetTickCount64();
    for (;;) {
        DWORD got = 0;
        stage = L"检测响应过大或读取超时";
        if (used == CONNECTION_BYTES || GetTickCount64() - started > 30000) goto done;
        stage = L"网络读取失败";
        if (!WinHttpReadData(request, body + used, CONNECTION_BYTES - (DWORD)used, &got)) goto network;
        if (!got) break;
        used += (int)got;
    }
    stage = L"检测响应未完整接收"; if (has_length && (DWORD)used != expected) goto done;
    stage = L"未获得有效模型回复；请检查接口兼容性或模型输出";
    if (!connection_response(body, used)) goto done;
    r->success = 1; wcscpy(r->message, L"连通成功：当前模型已返回有效回复"); goto done;
network:
    error = GetLastError();
    if (error == ERROR_WINHTTP_TIMEOUT) stage = L"检测超时；请检查网络或等待模型加载后重试";
    else if (error == ERROR_WINHTTP_SECURE_FAILURE) stage = L"TLS 证书验证失败；请检查证书和系统时间";
    else if (error == ERROR_WINHTTP_NAME_NOT_RESOLVED) stage = L"无法解析服务地址；请检查地址或 DNS";
done:
    if (!r->success) _snwprintf(r->message, 192, L"%s（HTTP %lu / 错误 %lu）", stage, status, error);
    if (request) WinHttpCloseHandle(request);
    if (connect) WinHttpCloseHandle(connect);
    if (session) WinHttpCloseHandle(session);
    SecureZeroMemory(header, sizeof(header)); free(body);
}
