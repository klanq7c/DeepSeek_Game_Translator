/*
 * http.c —— 内嵌 HTTP 服务器实现（详见 http.h）。
 *
 * 这是整个翻译系统的中枢。两类翻译任务在此分流：
 *
 *   1. 异步队列（async_*）：用于预热/预取。游戏侧发起 prefetch 后立即返回，
 *      实际翻译在后台 worker 池中完成并写入缓存，绝不阻塞游戏。
 *   2. 实时队列（live_*）：游戏侧发起的同步翻译。请求线程入队后阻塞等待，
 *      由 live_worker 翻译后通过条件变量唤醒返回。
 *
 * 关键的优先级规则（AGENTS.md 运行时契约的体现）：实时（前台）请求优先于
 * 异步（后台）请求。async_worker 在调用 API 前会等待前台清空，避免后台
 * 预热把 API 通道占满而让游戏中实时出现的文本排队。
 *
 * 缓存契约：未命中/排队/透传原文一律不得当作"已翻译"回写缓存
 * （见 is_resolved_translation 的守卫）。
 */
#include "http.h"
#include "buf.h"
#include "cache.h"
#include "json.h"
#include "util.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* 线程级上下文，工作线程通过它访问缓存、停止标志与 API 配置。 */
static HttpCtx *g_ctx;

/* 由主线程在启动工作线程前调用一次，设置全局上下文。 */
void http_set_ctx(HttpCtx *ctx) { g_ctx = ctx; }

/* Sized for whole-script Ren'Py warmups (30k lines): jobs are ~150 bytes, so
   the worst case stays around 10 MB while the queue drains through the API. */
#define ASYNC_QUEUE_LIMIT 65536              /* 异步队列上限，防失控占满内存 */
#define ASYNC_QUEUE_BYTE_LIMIT (32u * 1024u * 1024u)
#define ASYNC_BATCH_MAX 48                   /* 单批最多合并的文本数 */
#define ASYNC_BATCH_CHAR_BUDGET 9600         /* 单批字符预算，控制单次 API 请求体量 */
#define ASYNC_BATCH_COALESCE_MS 25           /* 合批等待窗口：攒够一批或超时即发 */
#define ASYNC_FOREGROUND_YIELD_MS 25         /* 后台等待前台清空的轮询间隔 */
#define ASYNC_KNOWN_BUCKETS 8192             /* 去重哈希桶数（须为 2 的幂） */
#define LIVE_BATCH_MAX 48
#define LIVE_BATCH_CHAR_BUDGET 9600
#define LIVE_BATCH_COALESCE_MS 35
#define LIVE_QUEUE_BYTE_LIMIT (32u * 1024u * 1024u)
#define HTTP_REQUEST_BUFFER_POOL_LIMIT 32    /* 仅缓存固定 64 KiB 初始块，常驻上限约 2 MiB */

/* 异步任务节点。同时挂在两个结构上：
   qnext 串成 FIFO 等待队列；knext 串成去重桶的冲突链。 */
typedef struct AsyncJob {
    char *text;
    size_t memory_bytes;
    uint64_t hash;
    struct AsyncJob *qnext;
    struct AsyncJob *knext;
} AsyncJob;

/* 实时任务节点。result/done/cv 构成"请求线程阻塞等待 worker 完成"的握手。 */
typedef struct LiveJob {
    char *text;
    char *result;
    size_t memory_bytes;
    int done;
    CONDITION_VARIABLE cv;
    struct LiveJob *next;
} LiveJob;

/*
 * HTTP 请求缓冲属于最短生命周期的一代：每个连接都需要一块初始 64 KiB。
 * 这里复用固定大小块以减少高并发短连接造成的堆碎片；扩容过的请求属于“大对象”，
 * 完成后直接 free，绝不进入池。池本身是进程级资源，但有硬上限，不会随流量增长。
 */
typedef struct RequestBufferNode {
    struct RequestBufferNode *next;
} RequestBufferNode;

static SRWLOCK g_request_buffer_pool_lock = SRWLOCK_INIT;
static RequestBufferNode *g_request_buffer_pool;
static size_t g_request_buffer_pool_count;
static volatile LONG g_request_buffer_pool_hits;
static volatile LONG g_request_buffer_pool_misses;

/* 异步队列全局状态：锁 + 条件变量 + FIFO 头尾 + 去重桶 + 计数。 */
static SRWLOCK g_async_lock = SRWLOCK_INIT;
static CONDITION_VARIABLE g_async_cv = CONDITION_VARIABLE_INIT;
static AsyncJob *g_async_head;
static AsyncJob *g_async_tail;
static AsyncJob *g_async_known[ASYNC_KNOWN_BUCKETS];
static size_t g_async_len;
static size_t g_async_memory_bytes;
static volatile LONG g_async_started;        /* worker 池是否已启动（一次性） */
static volatile LONG g_async_budget_rejections;

/* 实时队列全局状态，结构对称。 */
static SRWLOCK g_live_lock = SRWLOCK_INIT;
static CONDITION_VARIABLE g_live_cv = CONDITION_VARIABLE_INIT;
static LiveJob *g_live_head;
static LiveJob *g_live_tail;
static size_t g_live_len;
static size_t g_live_memory_bytes;
static volatile LONG g_live_started;
static volatile LONG g_live_budget_rejections;
static SRWLOCK g_worker_start_lock = SRWLOCK_INIT;
static volatile LONG g_worker_start_failures;
/* 当前活跃的前台（实时）请求数，用于后台 worker 判断是否该让路。 */
static volatile LONG g_foreground_requests;
static volatile LONG g_origin_rejections;
static _Thread_local char g_cors_allow_origin[64] = "null";

static DWORD WINAPI async_worker(LPVOID arg);
static DWORD WINAPI live_worker(LPVOID arg);

static char *request_buffer_acquire(void) {
    RequestBufferNode *node = NULL;
    AcquireSRWLockExclusive(&g_request_buffer_pool_lock);
    if (g_request_buffer_pool) {
        node = g_request_buffer_pool;
        g_request_buffer_pool = node->next;
        g_request_buffer_pool_count--;
    }
    ReleaseSRWLockExclusive(&g_request_buffer_pool_lock);

    if (node) {
        InterlockedIncrement(&g_request_buffer_pool_hits);
        return (char *)node;
    }
    InterlockedIncrement(&g_request_buffer_pool_misses);
    return xmalloc(HTTP_RECV_INITIAL + 1);
}

static void request_buffer_release(char *buffer, size_t capacity) {
    if (!buffer) return;
    if (capacity != HTTP_RECV_INITIAL) {
        free(buffer);
        return;
    }

    RequestBufferNode *node = (RequestBufferNode *)buffer;
    int cached = 0;
    AcquireSRWLockExclusive(&g_request_buffer_pool_lock);
    if (g_request_buffer_pool_count < HTTP_REQUEST_BUFFER_POOL_LIMIT) {
        node->next = g_request_buffer_pool;
        g_request_buffer_pool = node;
        g_request_buffer_pool_count++;
        cached = 1;
    }
    ReleaseSRWLockExclusive(&g_request_buffer_pool_lock);
    if (!cached) free(buffer);
}

static size_t request_buffer_pool_count(void) {
    AcquireSRWLockShared(&g_request_buffer_pool_lock);
    size_t n = g_request_buffer_pool_count;
    ReleaseSRWLockShared(&g_request_buffer_pool_lock);
    return n;
}

/* 线程安全地读取异步队列长度（health 接口用）。 */
static size_t async_queue_len(void) {
    AcquireSRWLockShared(&g_async_lock);
    size_t n = g_async_len;
    ReleaseSRWLockShared(&g_async_lock);
    return n;
}

static size_t async_memory_bytes(void) {
    AcquireSRWLockShared(&g_async_lock);
    size_t n = g_async_memory_bytes;
    ReleaseSRWLockShared(&g_async_lock);
    return n;
}

/* 线程安全地读取实时队列长度。 */
static size_t live_queue_len(void) {
    AcquireSRWLockShared(&g_live_lock);
    size_t n = g_live_len;
    ReleaseSRWLockShared(&g_live_lock);
    return n;
}

static size_t live_memory_bytes(void) {
    AcquireSRWLockShared(&g_live_lock);
    size_t n = g_live_memory_bytes;
    ReleaseSRWLockShared(&g_live_lock);
    return n;
}

static void report_live_budget_rejection(size_t used) {
    LONG count = InterlockedIncrement(&g_live_budget_rejections);
    if (count <= 3 || (count > 0 && (count & (count - 1)) == 0)) {
        fprintf(stderr,
                "[live] rejected translation memory budget "
                "(used=%zu limit=%u occurrence=%ld)\n",
                used, (unsigned)LIVE_QUEUE_BYTE_LIMIT, (long)count);
        fflush(stderr);
    }
}

/* 进入前台临界区：实时请求开始前调用，计数 +1。 */
static void foreground_enter(void) {
    InterlockedIncrement(&g_foreground_requests);
}

/* 离开前台临界区：实时请求结束后调用，计数 -1。 */
static void foreground_leave(void) {
    InterlockedDecrement(&g_foreground_requests);
}

/* 是否有前台请求在进行（含实时队列未清空）。后台 worker 据此让路。 */
static int foreground_active(void) {
    return InterlockedCompareExchange(&g_foreground_requests, 0, 0) > 0 || live_queue_len() > 0;
}

/* 服务器是否正在关闭（stop 标志被置位，或上下文未初始化）。 */
static int server_stopping(void) {
    return !g_ctx || InterlockedCompareExchange(g_ctx->stop, 0, 0) != 0;
}

/* 后台 worker 在调 API 前调用：自旋等待直到没有前台请求，确保实时优先。
   关闭信号到来时立即返回，避免关停卡死。 */
static void async_wait_for_foreground(void) {
    while (!server_stopping() && foreground_active()) {
        Sleep(ASYNC_FOREGROUND_YIELD_MS);
    }
}

/* 把 n 字节可靠地全部发送出去，处理部分发送。失败返回 0。 */
static int sendall(SOCKET s, const char *p, size_t n) {
    while (n) {
        int r = send(s, p, n > INT_MAX ? INT_MAX : (int)n, 0);
        if (r <= 0) return 0;
        p += r;
        n -= (size_t)r;
    }
    return 1;
}

/* 发送 JSON 响应：固定 CORS 头 + Connection: close。code/msg 为状态行，body 为正文。 */
static void resp(SOCKET s, int code, const char *msg, const char *body) {
    const char *ctype = "application/json; charset=utf-8";
    char h[640];
    size_t n = strlen(body);
    int k = snprintf(h, sizeof h,
                     "HTTP/1.1 %d %s\r\n"
                     "Content-Type: %s\r\n"
                     "Content-Length: %zu\r\n"
                     "Access-Control-Allow-Origin: %s\r\n"
                     "Access-Control-Allow-Methods: GET, POST, OPTIONS\r\n"
                     "Access-Control-Allow-Headers: Content-Type\r\n"
                     "Connection: close\r\n\r\n",
                     code, msg, ctype, n, g_cors_allow_origin);
    if (k < 0 || (size_t)k >= sizeof h) return; /* never read past the buffer */
    sendall(s, h, (size_t)k);
    sendall(s, body, n);
}

/* 发送纯文本响应（/translate?text=... 这类 GET 走纯文本协议）。 */
static void resp_plain(SOCKET s, int code, const char *msg, const char *body) {
    char h[640];
    size_t n = strlen(body ? body : "");
    int k = snprintf(h, sizeof h,
                     "HTTP/1.1 %d %s\r\n"
                     "Content-Type: text/plain; charset=utf-8\r\n"
                     "Content-Length: %zu\r\n"
                     "Access-Control-Allow-Origin: %s\r\n"
                     "Access-Control-Allow-Methods: GET, POST, OPTIONS\r\n"
                     "Access-Control-Allow-Headers: Content-Type\r\n"
                     "Connection: close\r\n\r\n",
                     code, msg, n, g_cors_allow_origin);
    if (k < 0 || (size_t)k >= sizeof h) return; /* never read past the buffer */
    sendall(s, h, (size_t)k);
    sendall(s, body ? body : "", n);
}

/* 单个十六进制字符转数值，非法返回 -1。 */
static int hexval(char c) {
    if ('0' <= c && c <= '9') return c - '0';
    if ('a' <= c && c <= 'f') return c - 'a' + 10;
    if ('A' <= c && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* URL 解码指定区间：'+' -> 空格，%XX -> 字节，其余原样。返回新分配缓冲。 */
static char *url_decode_range(const char *s, size_t n) {
    char *out = xmalloc(n + 1);
    size_t j = 0;
    for (size_t i = 0; i < n; i++) {
        if (s[i] == '+' ) {
            out[j++] = ' ';
        } else if (s[i] == '%' && i + 2 < n) {
            int a = hexval(s[i + 1]), b = hexval(s[i + 2]);
            if (a >= 0 && b >= 0) {
                out[j++] = (char)((a << 4) | b);
                i += 2;
            } else {
                out[j++] = s[i];
            }
        } else {
            out[j++] = s[i];
        }
    }
    out[j] = 0;
    return out;
}

/* 从查询串 q 中取参数 name 的值（已 URL 解码）。未找到返回 NULL。
   手写解析以避免引入完整 URL 库。 */
static char *query_get(const char *q, const char *name) {
    if (!q || !name) return NULL;
    size_t nl = strlen(name);
    const char *p = q;
    while (*p) {
        const char *e = strchr(p, '=');
        const char *amp = strchr(p, '&');
        if (!amp) amp = p + strlen(p);
        if (e && e < amp && (size_t)(e - p) == nl && !strncmp(p, name, nl)) {
            return url_decode_range(e + 1, (size_t)(amp - e - 1));
        }
        p = *amp ? amp + 1 : amp;
    }
    return NULL;
}

/* 前 n 字节不区分大小写比较。 */
static int mem_ieq(const char *a, const char *b, size_t n) {
    for (size_t i = 0; i < n; i++) {
        if (tolower((unsigned char)a[i]) != tolower((unsigned char)b[i])) return 0;
    }
    return 1;
}

/* 在请求头区域中查找指定头名的值起点（跳过冒号后空白）。
   headers_end 限定搜索边界（避免越过头部进入 body）。
   返回的指针指向 headers_end 之前，调用方按行尾截断取值。 */
static int parse_content_length(char *req, char *headers_end,
                                size_t *value_out, int *present_out) {
    static const char name[] = "Content-Length";
    const size_t nl = sizeof name - 1;
    char *p = strstr(req, "\r\n");
    *value_out = 0;
    *present_out = 0;
    if (!p || p >= headers_end) return 0;
    p += 2;
    while (p < headers_end) {
        char *line_end = strstr(p, "\r\n");
        if (!line_end || line_end > headers_end) line_end = headers_end;
        char *colon = memchr(p, ':', (size_t)(line_end - p));
        if (colon && (size_t)(colon - p) == nl && mem_ieq(p, name, nl)) {
            if (*present_out) return 0;
            char *v = colon + 1;
            while (v < line_end && (*v == ' ' || *v == '\t')) v++;
            if (v == line_end || *v < '0' || *v > '9') return 0;

            size_t value = 0;
            while (v < line_end && *v >= '0' && *v <= '9') {
                size_t digit = (size_t)(*v - '0');
                if (value > (SIZE_MAX - digit) / 10) return 0;
                value = value * 10 + digit;
                v++;
            }
            while (v < line_end && (*v == ' ' || *v == '\t')) v++;
            if (v != line_end) return 0;
            *value_out = value;
            *present_out = 1;
        }
        if (line_end == headers_end) break;
        p = line_end + 2;
    }
    return 1;
}

static int origin_port_suffix_valid(const char *value, size_t length) {
    if (!length) return 1;
    if (value[0] != ':' || length == 1 || length > 6) return 0;
    unsigned port = 0;
    for (size_t i = 1; i < length; i++) {
        if (value[i] < '0' || value[i] > '9') return 0;
        port = port * 10u + (unsigned)(value[i] - '0');
    }
    return port >= 1u && port <= 65535u;
}

static int origin_value_allowed(const char *value, size_t length) {
    static const char *const exact[] = {"null", "file://"};
    static const char *const local_prefixes[] = {
        "http://127.0.0.1", "https://127.0.0.1",
        "http://localhost", "https://localhost",
        "http://[::1]", "https://[::1]"
    };
    for (size_t i = 0; i < sizeof exact / sizeof exact[0]; i++) {
        size_t n = strlen(exact[i]);
        if (length == n && mem_ieq(value, exact[i], n)) return 1;
    }
    for (size_t i = 0; i < sizeof local_prefixes / sizeof local_prefixes[0]; i++) {
        size_t n = strlen(local_prefixes[i]);
        if (length >= n && mem_ieq(value, local_prefixes[i], n) &&
            origin_port_suffix_valid(value + n, length - n)) {
            return 1;
        }
    }
    return 0;
}

/* Browser game hooks need file/null-origin CORS, while arbitrary websites
   must not read translation memory or spend the configured provider account.
   Native engine clients omit Origin and remain unaffected. */
static int apply_origin_policy(char *req, char *headers_end, int *present_out) {
    static const char name[] = "Origin";
    const size_t name_len = sizeof name - 1;
    char *p = strstr(req, "\r\n");
    *present_out = 0;
    memcpy(g_cors_allow_origin, "null", 5);
    if (!p || p >= headers_end) return 0;
    p += 2;
    while (p < headers_end) {
        char *line_end = strstr(p, "\r\n");
        if (!line_end || line_end > headers_end) line_end = headers_end;
        char *colon = memchr(p, ':', (size_t)(line_end - p));
        if (colon && (size_t)(colon - p) == name_len &&
            mem_ieq(p, name, name_len)) {
            if (*present_out) return 0;
            *present_out = 1;
            char *value = colon + 1;
            while (value < line_end && (*value == ' ' || *value == '\t')) value++;
            char *value_end = line_end;
            while (value_end > value &&
                   (value_end[-1] == ' ' || value_end[-1] == '\t')) {
                value_end--;
            }
            size_t length = (size_t)(value_end - value);
            if (!length || length >= sizeof g_cors_allow_origin ||
                !origin_value_allowed(value, length)) {
                return 0;
            }
            memcpy(g_cors_allow_origin, value, length);
            g_cors_allow_origin[length] = 0;
        }
        if (line_end == headers_end) break;
        p = line_end + 2;
    }
    return 1;
}

static int browser_admin_path(const char *path) {
    return ieq(path, "/shutdown") || ieq(path, "/cache/import") ||
           ieq(path, "/cache/dump") || ieq(path, "/cache/export");
}

static void report_origin_rejection(const char *reason) {
    LONG count = InterlockedIncrement(&g_origin_rejections);
    if (count <= 3 || (count > 0 && (count & (count - 1)) == 0)) {
        fprintf(stderr, "[http] browser origin rejected #%ld (%s)\n",
                (long)count, reason);
        fflush(stderr);
    }
}

/* 取 JSON 体中某个布尔字段是否为真。先用 strstr 快速短路，避免对大 body
   做完整 json_key 扫描。识别 true/1。 */
static int json_bool_true(const char *json, const char *key) {
    /* If the key text isn't even a substring of the body, it can't be a key;
       skip json_key's allocating full-body scan (it parses every string). */
    if (!strstr(json, key)) return 0;
    const char *p = json_top_key(json, key);
    if (!p) return 0;
    return !strncmp(p, "true", 4) || !strncmp(p, "1", 1);
}

/* 逐码点扫描 UTF-8，判断字符串是否含 CJK 汉字（U+4E00..U+9FFF）。
   用于跳过已是中文的文本，避免无谓翻译。 */
static int utf8_has_cjk(const char *s) {
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

/* 是否存在"值得翻译的信号"：含字母（ASCII）或任何非 ASCII 字节。
   纯标点/数字/空白视为无需翻译。 */
static int has_translation_signal(const char *s) {
    const unsigned char *p = (const unsigned char *)s;
    while (*p) {
        if (*p < 0x80) {
            if (isalpha(*p)) return 1;
            p++;
        } else {
            return 1;
        }
    }
    return 0;
}

/* 综合判定：非空 + 有翻译信号 + 不含 CJK（已是中文则不翻）。 */
static int should_translate_text(const char *text) {
    return text && *text && has_translation_signal(text) && !utf8_has_cjk(text);
}

/* 判定是否为"有效翻译结果"：译文非空且与原文不同。
   这是回写缓存的守卫——原文/空串/排队未决结果都不得当作翻译写入。 */
static int is_resolved_translation(const char *original, const char *translated) {
    return original && translated && *translated && strcmp(original, translated) != 0;
}

/* One worker per API channel so the queue can keep every channel busy;
   with concurrency=1 this degrades to the previous single-worker behavior. */
static int worker_pool_size(void) {
    int n = (g_ctx && g_ctx->api) ? g_ctx->api->concurrency : 1;
    if (n < 1) n = 1;
    if (n > API_CONCURRENCY_MAX) n = API_CONCURRENCY_MAX;
    return n;
}

/* Prefetch must never occupy every remote channel before visible text arrives.
   With more than one configured channel, keep one available for the live lane;
   a single-channel provider necessarily shares its only connection. */
static int async_worker_pool_size(void) {
    int n = worker_pool_size();
    return n > 1 ? n - 1 : 1;
}

/* 首次需要 worker 时启动一整池（数量 = 并发通道数）。started 用 CAS 保证
   只启动一次；启动失败则回退标志，下次再试。返回是否已有可用 worker。 */
static int ensure_worker_pool(volatile LONG *started, LPTHREAD_START_ROUTINE fn,
                              int want, const char *lane) {
    DWORD last_error = ERROR_SUCCESS;
    AcquireSRWLockExclusive(&g_worker_start_lock);
    LONG current = InterlockedCompareExchange(started, 0, 0);
    int created = 0;
    for (int i = (int)current; i < want; i++) {
        HANDLE th = CreateThread(NULL, 64 * 1024, fn, NULL,
                                 STACK_SIZE_PARAM_IS_A_RESERVATION, NULL);
        if (th) {
            CloseHandle(th);
            created++;
        } else {
            last_error = GetLastError();
        }
    }
    LONG available = current + created;
    InterlockedExchange(started, available);
    ReleaseSRWLockExclusive(&g_worker_start_lock);

    if (available < want) {
        LONG count = InterlockedIncrement(&g_worker_start_failures);
        if (count <= 3 || (count > 0 && (count & (count - 1)) == 0)) {
            fprintf(stderr,
                    "[http] %s worker pool creation incomplete "
                    "(wanted=%d available=%ld error=%lu occurrence=%ld)\n",
                    lane, want, (long)available, (unsigned long)last_error,
                    (long)count);
            fflush(stderr);
        }
    }
    return available > 0;
}

/* 确保异步 worker 池已启动。 */
static int ensure_async_worker(void) {
    return ensure_worker_pool(&g_async_started, async_worker,
                              async_worker_pool_size(), "async");
}

/* 确保实时 worker 池已启动。 */
static int ensure_live_worker(void) {
    return ensure_worker_pool(&g_live_started, live_worker,
                              worker_pool_size(), "live");
}

/* 持锁查询某文本是否已在异步队列中（去重），避免重复排队。
   用 hash 先定位桶，再桶内逐个 strcmp 确认。 */
static int async_known_locked(const char *text, uint64_t hash) {
    size_t slot = (size_t)hash & (ASYNC_KNOWN_BUCKETS - 1);
    for (AsyncJob *j = g_async_known[slot]; j; j = j->knext) {
        if (j->hash == hash && strcmp(j->text, text) == 0) return 1;
    }
    return 0;
}

/* 把未命中的文本排入异步队列（若满足条件）。返回 1=已入队，0=未入队
   （已缓存/已排队/不可译/队列满/worker 启动失败等）。调用方据此判断
   source 标记为 queued 还是 miss。 */
static int async_enqueue_miss(const char *text) {
    if (!g_ctx || !g_ctx->api || !g_ctx->api->enabled || !should_translate_text(text)) return 0;

    char *hit = cache_get(g_ctx->cache, text);
    if (hit) {
        free(hit);
        return 0;
    }
    if (!ensure_async_worker()) return 0;

    uint64_t h = h64(text);
    size_t text_bytes = strlen(text) + 1;
    if (text_bytes > ASYNC_QUEUE_BYTE_LIMIT - sizeof(AsyncJob)) return 0;
    size_t memory_bytes = sizeof(AsyncJob) + text_bytes;
    AcquireSRWLockExclusive(&g_async_lock);
    if (g_async_len >= ASYNC_QUEUE_LIMIT || async_known_locked(text, h)) {
        ReleaseSRWLockExclusive(&g_async_lock);
        return 0;
    }
    if (g_async_memory_bytes > ASYNC_QUEUE_BYTE_LIMIT - memory_bytes) {
        size_t used_at_rejection = g_async_memory_bytes;
        ReleaseSRWLockExclusive(&g_async_lock);
        LONG count = InterlockedIncrement(&g_async_budget_rejections);
        if (count <= 3 || (count > 0 && (count & (count - 1)) == 0)) {
            fprintf(stderr,
                    "[http] async queue byte budget rejected #%ld "
                    "(used=%zu, limit=%u)\n",
                    (long)count, used_at_rejection,
                    (unsigned)ASYNC_QUEUE_BYTE_LIMIT);
            fflush(stderr);
        }
        return 0;
    }

    AsyncJob *job = xmalloc(sizeof *job);
    job->text = xstrdup(text);
    job->memory_bytes = memory_bytes;
    job->hash = h;
    job->qnext = NULL;
    size_t slot = (size_t)h & (ASYNC_KNOWN_BUCKETS - 1);
    job->knext = g_async_known[slot];
    g_async_known[slot] = job;
    if (g_async_tail) g_async_tail->qnext = job;
    else g_async_head = job;
    g_async_tail = job;
    g_async_len++;
    g_async_memory_bytes += memory_bytes;
    WakeConditionVariable(&g_async_cv);
    ReleaseSRWLockExclusive(&g_async_lock);
    return 1;
}

/* 持锁：从去重桶中摘除 job（翻译完成后调用，使相同文本可再次排队重译）。 */
static void async_forget_locked(AsyncJob *job) {
    size_t slot = (size_t)job->hash & (ASYNC_KNOWN_BUCKETS - 1);
    AsyncJob **pp = &g_async_known[slot];
    while (*pp) {
        if (*pp == job) {
            *pp = job->knext;
            return;
        }
        pp = &(*pp)->knext;
    }
}

/* 持锁：从 FIFO 队首弹出并摘除一个 job。 */
static AsyncJob *async_pop_locked(void) {
    AsyncJob *job = g_async_head;
    if (!job) return NULL;
    g_async_head = job->qnext;
    if (!g_async_head) g_async_tail = NULL;
    g_async_len--;
    job->qnext = NULL;
    return job;
}

/* worker 取一批 job（最多 cap 个，受字符预算约束）。队空时阻塞等待条件变量。
   合批逻辑：弹出首个后，若短暂等待窗口内还有新任务且不超字符预算，则继续攒，
   从而把多个小文本合并成一次 API 请求，提升吞吐。 */
static size_t async_pop_batch(AsyncJob **jobs, size_t cap) {
    size_t n = 0;
    size_t chars = 0;
    AcquireSRWLockExclusive(&g_async_lock);
    while (!g_async_head && g_ctx && !InterlockedCompareExchange(g_ctx->stop, 0, 0)) {
        SleepConditionVariableSRW(&g_async_cv, &g_async_lock, 1000, 0);
    }
    if (!g_async_head) {
        ReleaseSRWLockExclusive(&g_async_lock);
        return 0;
    }

    AsyncJob *job = async_pop_locked();
    jobs[n++] = job;
    chars += strlen(job->text);

    while (n < cap) {
        if (!g_async_head) {
            SleepConditionVariableSRW(&g_async_cv, &g_async_lock, ASYNC_BATCH_COALESCE_MS, 0);
        }
        if (!g_async_head) break;
        size_t next_len = strlen(g_async_head->text);
        if (chars + next_len > ASYNC_BATCH_CHAR_BUDGET && n > 0) break;
        job = async_pop_locked();
        jobs[n++] = job;
        chars += next_len;
    }
    ReleaseSRWLockExclusive(&g_async_lock);
    return n;
}

/* 一批 job 处理完毕：从去重桶摘除并释放内存。 */
static void async_finish_jobs(AsyncJob **jobs, size_t count) {
    AcquireSRWLockExclusive(&g_async_lock);
    for (size_t i = 0; i < count; i++) {
        async_forget_locked(jobs[i]);
        if (g_async_memory_bytes >= jobs[i]->memory_bytes) {
            g_async_memory_bytes -= jobs[i]->memory_bytes;
        } else {
            g_async_memory_bytes = 0;
        }
    }
    ReleaseSRWLockExclusive(&g_async_lock);
    for (size_t i = 0; i < count; i++) {
        free(jobs[i]->text);
        free(jobs[i]);
    }
}

/* 翻译一批异步 job：先过滤掉已缓存/不可译的，剩下的调 API。
   优先用批量接口 api_translate_batch；失败则降级为逐条 api_translate。
   仅把"有效翻译结果"（is_resolved_translation）回写缓存。
   每次调 API 前都等待前台清空 + 检查停止标志，保证实时优先与可关停。 */
static void async_translate_pending(AsyncJob **jobs, size_t count) {
    AsyncJob *pending[ASYNC_BATCH_MAX];
    char *texts[ASYNC_BATCH_MAX];
    size_t pn = 0;

    for (size_t i = 0; i < count; i++) {
        char *hit = cache_get(g_ctx->cache, jobs[i]->text);
        if (hit) {
            free(hit);
            continue;
        }
        if (g_ctx->api && g_ctx->api->enabled) {
            pending[pn] = jobs[i];
            texts[pn] = jobs[i]->text;
            pn++;
        }
    }
    if (!pn) return;

    char **translated = NULL;
    async_wait_for_foreground();
    if (server_stopping() || !g_ctx->api || !g_ctx->api->enabled) return;
    if (api_translate_batch(g_ctx->api, texts, pn, &translated)) {
        const char *persist_keys[ASYNC_BATCH_MAX];
        const char *persist_values[ASYNC_BATCH_MAX];
        size_t persist_n = 0;
        for (size_t i = 0; i < pn; i++) {
            if (is_resolved_translation(pending[i]->text, translated[i])) {
                persist_keys[persist_n] = pending[i]->text;
                persist_values[persist_n] = translated[i];
                persist_n++;
            }
        }
        cache_set_many_persist(g_ctx->cache, persist_keys, persist_values, persist_n);
        for (size_t i = 0; i < pn; i++) {
            free(translated[i]);
        }
        free(translated);
        return;
    }

    const char *persist_keys[ASYNC_BATCH_MAX];
    const char *persist_values[ASYNC_BATCH_MAX];
    char *owned_values[ASYNC_BATCH_MAX] = {0};
    size_t persist_n = 0;
    for (size_t i = 0; i < pn; i++) {
        char *live = NULL;
        async_wait_for_foreground();
        if (server_stopping() || !g_ctx->api || !g_ctx->api->enabled) break;
        if (api_translate(g_ctx->api, pending[i]->text, &live) &&
            is_resolved_translation(pending[i]->text, live)) {
            persist_keys[persist_n] = pending[i]->text;
            persist_values[persist_n] = live;
            owned_values[persist_n] = live;
            persist_n++;
            live = NULL;
        }
        free(live);
    }
    cache_set_many_persist(g_ctx->cache, persist_keys, persist_values, persist_n);
    for (size_t i = 0; i < persist_n; i++) free(owned_values[i]);
}

/* 异步 worker 主循环：取批 -> 翻译 -> 收尾，直到服务器关闭且队列空。 */
static DWORD WINAPI async_worker(LPVOID arg) {
    (void)arg;
    for (;;) {
        AsyncJob *jobs[ASYNC_BATCH_MAX];
        size_t n = async_pop_batch(jobs, ASYNC_BATCH_MAX);
        if (!n) {
            if (!g_ctx || InterlockedCompareExchange(g_ctx->stop, 0, 0)) break;
            continue;
        }
        async_translate_pending(jobs, n);
        async_finish_jobs(jobs, n);
    }
    return 0;
}

/* 持锁：把实时 job 追加到 FIFO 尾部并唤醒 worker。 */
static void live_enqueue_locked(LiveJob *job) {
    job->next = NULL;
    if (g_live_tail) g_live_tail->next = job;
    else g_live_head = job;
    g_live_tail = job;
    g_live_len++;
    WakeConditionVariable(&g_live_cv);
}

/* 持锁：从实时 FIFO 队首弹出并摘除一个 job。 */
static LiveJob *live_pop_locked(void) {
    LiveJob *job = g_live_head;
    if (!job) return NULL;
    g_live_head = job->next;
    if (!g_live_head) g_live_tail = NULL;
    g_live_len--;
    job->next = NULL;
    return job;
}

/* 实时 worker 取一批 job，合批逻辑与 async_pop_batch 对称（参数独立可单独调优）。 */
static size_t live_pop_batch(LiveJob **jobs, size_t cap) {
    size_t n = 0;
    size_t chars = 0;
    AcquireSRWLockExclusive(&g_live_lock);
    while (!g_live_head && g_ctx && !InterlockedCompareExchange(g_ctx->stop, 0, 0)) {
        SleepConditionVariableSRW(&g_live_cv, &g_live_lock, 1000, 0);
    }
    if (!g_live_head) {
        ReleaseSRWLockExclusive(&g_live_lock);
        return 0;
    }

    LiveJob *job = live_pop_locked();
    jobs[n++] = job;
    chars += strlen(job->text);

    while (n < cap) {
        if (!g_live_head) {
            SleepConditionVariableSRW(&g_live_cv, &g_live_lock, LIVE_BATCH_COALESCE_MS, 0);
        }
        if (!g_live_head) break;
        size_t next_len = strlen(g_live_head->text);
        if (chars + next_len > LIVE_BATCH_CHAR_BUDGET && n > 0) break;
        job = live_pop_locked();
        jobs[n++] = job;
        chars += next_len;
    }
    ReleaseSRWLockExclusive(&g_live_lock);
    return n;
}

/* 一批实时 job 翻译完成：写回结果、置 done、唤醒各自的等待条件变量。
   results[i] 为 NULL 时回退为原文（保证请求方总能拿到非空结果）。 */
static void live_finish_batch(LiveJob **jobs, char **results, size_t count) {
    AcquireSRWLockExclusive(&g_live_lock);
    for (size_t i = 0; i < count; i++) {
        jobs[i]->result = results[i] ? results[i] : xstrdup(jobs[i]->text);
        jobs[i]->done = 1;
        WakeConditionVariable(&jobs[i]->cv);
    }
    ReleaseSRWLockExclusive(&g_live_lock);
}

/* 翻译一批实时 job。先填缓存命中/不可译的结果，剩余调 API：
   优先批量，失败则单条；仍失败的丢入异步队列后台补译，避免英文永久漏过实时路径。
   注意：实时路径不等待前台（它本身就是前台），且失败时 results[i] 保持为 NULL，
   由 live_finish_batch 回退为原文。 */
static void live_translate_jobs(LiveJob **jobs, size_t count, char **results) {
    LiveJob *pending[LIVE_BATCH_MAX];
    char *texts[LIVE_BATCH_MAX];
    size_t pn = 0;

    for (size_t i = 0; i < count; i++) {
        results[i] = cache_get(g_ctx->cache, jobs[i]->text);
        if (results[i]) continue;
        if (!g_ctx->api || !g_ctx->api->enabled || !should_translate_text(jobs[i]->text)) {
            results[i] = xstrdup(jobs[i]->text);
            continue;
        }
        pending[pn] = jobs[i];
        texts[pn] = jobs[i]->text;
        pn++;
    }
    if (!pn) return;

    char **translated = NULL;
    if (api_translate_batch(g_ctx->api, texts, pn, &translated)) {
        const char *persist_keys[LIVE_BATCH_MAX];
        const char *persist_values[LIVE_BATCH_MAX];
        size_t persist_n = 0;
        for (size_t i = 0; i < pn; i++) {
            if (is_resolved_translation(pending[i]->text, translated[i])) {
                pending[i]->result = NULL;
                persist_keys[persist_n] = pending[i]->text;
                persist_values[persist_n] = translated[i];
                persist_n++;
            }
        }
        cache_set_many_persist(g_ctx->cache, persist_keys, persist_values, persist_n);
        for (size_t i = 0; i < pn; i++) {
            free(translated[i]);
        }
        free(translated);
        for (size_t i = 0; i < count; i++) {
            if (!results[i]) results[i] = cache_get(g_ctx->cache, jobs[i]->text);
        }
        return;
    }

    if (pn == 1) {
        char *live = NULL;
        if (api_translate(g_ctx->api, texts[0], &live) &&
            is_resolved_translation(texts[0], live)) {
            CachePersistResult persisted =
                cache_set_persist_result(g_ctx->cache, texts[0], live);
            if (persisted.accepted) {
                for (size_t i = 0; i < count; i++) {
                    if (jobs[i] == pending[0] && !results[i]) {
                        results[i] = live;
                        live = NULL;
                        break;
                    }
                }
            }
        }
        free(live);
    }

    /* Anything still unresolved (e.g. a multi-item batch call that failed)
       gets queued for background translation so the cache fills for next
       time, instead of permanently leaking English through the live path.
       async_enqueue_miss is a no-op for texts that just landed in the cache. */
    for (size_t i = 0; i < pn; i++) {
        async_enqueue_miss(pending[i]->text);
    }
}

/* 实时 worker 主循环：取批 -> 翻译 -> 唤醒等待方，直到关闭且队列空。 */
static DWORD WINAPI live_worker(LPVOID arg) {
    (void)arg;
    for (;;) {
        LiveJob *jobs[LIVE_BATCH_MAX];
        char *results[LIVE_BATCH_MAX] = {0};
        size_t n = live_pop_batch(jobs, LIVE_BATCH_MAX);
        if (!n) {
            if (!g_ctx || InterlockedCompareExchange(g_ctx->stop, 0, 0)) break;
            continue;
        }
        live_translate_jobs(jobs, n, results);
        live_finish_batch(jobs, results, n);
    }
    return 0;
}

/* 同步翻译单条文本（实时路径）。构造 job 入队后阻塞在条件变量上等结果，
   期间持有 live 锁（SleepConditionVariableSRW 会临时释放锁）。
   foreground_enter/leave 包住整个过程，使后台 worker 主动让路。
   返回新分配的结果（调用方负责 free），不可译/worker 不可用时返回 NULL。 */
static char *live_translate_batched(const char *text) {
    if (!g_ctx || !g_ctx->api || !g_ctx->api->enabled || !should_translate_text(text)) return NULL;
    if (!ensure_live_worker()) return NULL;

    LiveJob job;
    memset(&job, 0, sizeof job);
    size_t text_bytes = strlen(text) + 1;
    if (text_bytes > LIVE_QUEUE_BYTE_LIMIT - sizeof job) return NULL;
    job.memory_bytes = sizeof job + text_bytes;
    job.text = xstrdup(text);
    InitializeConditionVariable(&job.cv);

    foreground_enter();
    AcquireSRWLockExclusive(&g_live_lock);
    if (g_live_memory_bytes > LIVE_QUEUE_BYTE_LIMIT - job.memory_bytes) {
        size_t used_at_rejection = g_live_memory_bytes;
        ReleaseSRWLockExclusive(&g_live_lock);
        foreground_leave();
        free(job.text);
        report_live_budget_rejection(used_at_rejection);
        return NULL;
    }
    g_live_memory_bytes += job.memory_bytes;
    live_enqueue_locked(&job);
    while (!job.done) {
        SleepConditionVariableSRW(&job.cv, &g_live_lock, INFINITE, 0);
    }
    g_live_memory_bytes -= job.memory_bytes;
    ReleaseSRWLockExclusive(&g_live_lock);
    foreground_leave();

    free(job.text);
    return job.result;
}

/* Submit all unique misses from one HTTP batch to the live worker pool at
   once. Workers split them by item/character budget and can use independent
   API channels concurrently. Results retain input order; unresolved entries
   remain original echoes and are rejected by the caller. */
static void live_translate_group(char **texts, size_t count, char **results) {
    if (!texts || !count || !results || !g_ctx || !g_ctx->api ||
        !g_ctx->api->enabled || !ensure_live_worker()) return;
    if (count > LIVE_QUEUE_BYTE_LIMIT / sizeof(LiveJob)) {
        report_live_budget_rejection(live_memory_bytes());
        return;
    }

    LiveJob *jobs = xcalloc(count, sizeof *jobs);
    size_t memory_bytes = count * sizeof *jobs;
    for (size_t i = 0; i < count; i++) {
        size_t text_bytes = strlen(texts[i]) + 1;
        if (text_bytes > LIVE_QUEUE_BYTE_LIMIT - memory_bytes) {
            for (size_t j = 0; j < i; j++) free(jobs[j].text);
            free(jobs);
            report_live_budget_rejection(live_memory_bytes());
            return;
        }
        memory_bytes += text_bytes;
        jobs[i].memory_bytes = sizeof *jobs + text_bytes;
        jobs[i].text = xstrdup(texts[i]);
        InitializeConditionVariable(&jobs[i].cv);
    }

    foreground_enter();
    AcquireSRWLockExclusive(&g_live_lock);
    if (g_live_memory_bytes > LIVE_QUEUE_BYTE_LIMIT - memory_bytes) {
        size_t used_at_rejection = g_live_memory_bytes;
        ReleaseSRWLockExclusive(&g_live_lock);
        foreground_leave();
        for (size_t i = 0; i < count; i++) free(jobs[i].text);
        free(jobs);
        report_live_budget_rejection(used_at_rejection);
        return;
    }
    g_live_memory_bytes += memory_bytes;
    for (size_t i = 0; i < count; i++) live_enqueue_locked(&jobs[i]);
    for (size_t i = 0; i < count; i++) {
        while (!jobs[i].done) {
            SleepConditionVariableSRW(&jobs[i].cv, &g_live_lock, INFINITE, 0);
        }
        results[i] = jobs[i].result;
        jobs[i].result = NULL;
    }
    g_live_memory_bytes -= memory_bytes;
    ReleaseSRWLockExclusive(&g_live_lock);
    foreground_leave();

    for (size_t i = 0; i < count; i++) {
        free(jobs[i].text);
        free(jobs[i].result);
    }
    free(jobs);
}

/* 单条文本的核心翻译决策（被 /translate、op_batch 共用）。
   source 写入本次结果的来源标记（cache/pass/queued/miss/api_batch）。
   优先级：缓存命中 -> 不可译透传 -> cache_only 模式(可入队异步) -> 实时翻译。
   返回新分配字符串，调用方负责 free。 */
static char *translate_value(const char *text, int cache_only, int queue_miss, const char **source) {
    char *v = cache_get(g_ctx->cache, text);
    if (v) {
        if (source) *source = "cache";
        return v;
    }
    if (!should_translate_text(text)) {
        if (source) *source = "pass";
        return xstrdup(text);
    }
    if (cache_only) {
        int queued = queue_miss && async_enqueue_miss(text);
        if (source) *source = queued ? "queued" : "miss";
        return xstrdup(text);
    }
    if (!cache_only && g_ctx->api && g_ctx->api->enabled) {
        char *live = live_translate_batched(text);
        if (is_resolved_translation(text, live)) {
            CachePersistResult persisted =
                cache_set_persist_result(g_ctx->cache, text, live);
            if (persisted.accepted) {
                if (source) *source = "api_batch";
                return live;
            }
        }
        free(live);
    }
    if (source) *source = "miss";
    return xstrdup(text);
}

/* /health：返回服务运行状态——缓存条目数、异步/实时队列长度、请求缓冲池、API 是否启用、运行时长。
   供 launcher 与钩子做就绪探测与健康检查。runtime_cache_only 固定为 false，
   表示服务器具备调远程 API 的能力（非纯缓存模式）。 */
static void op_health(Buf *b) {
    buf_add(b, "{\"status\":\"ok\",\"server\":\"dst_server_c\",\"cache_size\":");
    buf_int(b, (long long)cache_size(g_ctx->cache));
    buf_add(b, ",\"async_queue\":");
    buf_int(b, (long long)async_queue_len());
    buf_add(b, ",\"async_memory_bytes\":");
    buf_int(b, (long long)async_memory_bytes());
    buf_add(b, ",\"async_memory_limit\":");
    buf_int(b, ASYNC_QUEUE_BYTE_LIMIT);
    buf_add(b, ",\"live_queue\":");
    buf_int(b, (long long)live_queue_len());
    buf_add(b, ",\"live_memory_bytes\":");
    buf_int(b, (long long)live_memory_bytes());
    buf_add(b, ",\"live_memory_limit\":");
    buf_int(b, LIVE_QUEUE_BYTE_LIMIT);
    buf_add(b, ",\"request_buffer_pool_cached\":");
    buf_int(b, (long long)request_buffer_pool_count());
    buf_add(b, ",\"request_buffer_pool_limit\":");
    buf_int(b, HTTP_REQUEST_BUFFER_POOL_LIMIT);
    buf_add(b, ",\"request_buffer_pool_hits\":");
    buf_int(b, (long long)(unsigned long)InterlockedCompareExchange(&g_request_buffer_pool_hits, 0, 0));
    buf_add(b, ",\"request_buffer_pool_misses\":");
    buf_int(b, (long long)(unsigned long)InterlockedCompareExchange(&g_request_buffer_pool_misses, 0, 0));
    buf_add(b, ",\"active_connections\":");
    buf_int(b, g_ctx->active_connections
        ? (long long)InterlockedCompareExchange(g_ctx->active_connections, 0, 0)
        : 0);
    buf_add(b, ",\"connection_limit\":");
    buf_int(b, (long long)g_ctx->connection_limit);
    buf_add(b, ",\"connection_rejections\":");
    buf_int(b, g_ctx->connection_rejections
        ? (long long)(unsigned long)InterlockedCompareExchange(g_ctx->connection_rejections, 0, 0)
        : 0);
    buf_add(b, ",\"connection_thread_failures\":");
    buf_int(b, g_ctx->connection_thread_failures
        ? (long long)(unsigned long)InterlockedCompareExchange(g_ctx->connection_thread_failures, 0, 0)
        : 0);
    buf_add(b, ",\"worker_start_failures\":");
    buf_int(b, (long long)(unsigned long)InterlockedCompareExchange(
        &g_worker_start_failures, 0, 0));
    buf_add(b, ",\"api_enabled\":");
    buf_add(b, (g_ctx->api && g_ctx->api->enabled) ? "true" : "false");
    buf_add(b, ",\"runtime_cache_only\":false,\"uptime_seconds\":");
    buf_int(b, (long long)(time(NULL) - g_ctx->started));
    buf_add(b, "}");
}

/* /cache/lookup：批量查缓存，只返回命中项。mark 记录写键前的位置，
   若值未命中则回滚 len 到 mark，丢弃这次半成品写入。 */
static void op_lookup(Buf *b, List *l) {
    int first = 1, hits = 0;
    buf_add(b, "{\"hits\":{");
    for (size_t i = 0; i < l->n; i++) {
        size_t mark = b->len;
        if (!first) buf_ch(b, ',');
        buf_json(b, l->v[i]);
        buf_ch(b, ':');
        if (!cache_emit_json(g_ctx->cache, l->v[i], b)) {
            b->len = mark;
            b->data[b->len] = 0;
            continue;
        }
        first = 0;
        hits++;
    }
    buf_add(b, "},\"hit_count\":");
    buf_int(b, hits);
    buf_add(b, ",\"miss_count\":");
    buf_int(b, (long long)l->n - hits);
    buf_add(b, "}");
}

/* /cache/dump：把整个缓存导出为 {cache:{k:v,...},count:n}。 */
static void op_cache_dump(Buf *b) {
    buf_add(b, "{\"cache\":{");
    size_t n = cache_emit_json_map(g_ctx->cache, b);
    buf_add(b, "},\"count\":");
    buf_int(b, (long long)n);
    buf_add(b, "}");
}

/* /cache/export：导出为 {entries:[{key,value},...],count:n}，便于外部备份/迁移。 */
static void op_cache_export(Buf *b) {
    buf_add(b, "{\"entries\":[");
    size_t n = cache_emit_json_entries(g_ctx->cache, b);
    buf_add(b, "],\"count\":");
    buf_int(b, (long long)n);
    buf_add(b, "}");
}

/* /translate(POST 单条)与 /batch(POST 批量)的核心实现。
   single=1 且仅一条时走轻量路径，复用 translate_value 并输出兼容旧契约的
   translation/translated_text 双字段。否则走批量路径：
     1) 先去重（同文本复用前面结果）、查缓存、判定可译性，把需要实时翻译的
        下标收集进 miss[]；
     2) foreground 包住 API 调用段（标记前台，让后台让路），按 ASYNC_BATCH_MAX
        分批调 api_translate_batch，失败则逐条 api_translate 兜底；
     3) 仍未得到的回退为原文（source=miss）；
     4) 拼装 translations 映射 + results 数组 + sources 数组三个视图。 */
static void op_batch(Buf *b, List *l, int single, int cache_only) {
    if (single && l->n == 1) {
        const char *source = "miss";
        char *v = translate_value(l->v[0], cache_only, cache_only, &source);
        buf_add(b, "{\"translation\":");
        buf_json(b, v);
        buf_add(b, ",\"translated_text\":");
        buf_json(b, v);
        buf_add(b, ",\"source\":\"");
        buf_add(b, source);
        buf_add(b, "\"}");
        free(v);
        return;
    }

    char **vals = xcalloc(l->n ? l->n : 1, sizeof *vals);
    const char **sources = xcalloc(l->n ? l->n : 1, sizeof *sources);
    size_t *miss = xcalloc(l->n ? l->n : 1, sizeof *miss);
    size_t *duplicate_of = xmalloc((l->n ? l->n : 1) * sizeof *duplicate_of);
    size_t miss_n = 0;

    /* 去重用开放寻址哈希表（h64 + 线性探测，负载因子 ≤ 0.5），桶内存放首个
       出现的下标+1（0=空桶）。语义与逐对 strcmp 一致（duplicate_of 仍指向
       首个相同文本的下标），但 30k 行整脚本批量从 O(n^2)（约 4.5 亿次比较）
       降为 O(n)。 */
    size_t dedup_cap = 16;
    while (dedup_cap < l->n * 2) {
        if (dedup_cap > SIZE_MAX / 2) die("batch too large");
        dedup_cap *= 2;
    }
    size_t *dedup_idx = xcalloc(dedup_cap, sizeof *dedup_idx);
    uint64_t *dedup_hash = xmalloc(dedup_cap * sizeof *dedup_hash);

    for (size_t i = 0; i < l->n; i++) {
        duplicate_of[i] = SIZE_MAX;
        uint64_t h = h64(l->v[i]);
        size_t slot = (size_t)h & (dedup_cap - 1);
        while (dedup_idx[slot]) {
            size_t j = dedup_idx[slot] - 1;
            if (dedup_hash[slot] == h && strcmp(l->v[j], l->v[i]) == 0) {
                duplicate_of[i] = j;
                break;
            }
            slot = (slot + 1) & (dedup_cap - 1);
        }
        if (duplicate_of[i] != SIZE_MAX) continue;
        dedup_idx[slot] = i + 1;
        dedup_hash[slot] = h;

        vals[i] = cache_get(g_ctx->cache, l->v[i]);
        if (vals[i]) {
            sources[i] = "cache";
            continue;
        }

        if (!should_translate_text(l->v[i])) {
            vals[i] = xstrdup(l->v[i]);
            sources[i] = "pass";
        } else if (cache_only) {
            sources[i] = async_enqueue_miss(l->v[i]) ? "queued" : "miss";
            vals[i] = xstrdup(l->v[i]);
        } else if (g_ctx->api && g_ctx->api->enabled) {
            miss[miss_n++] = i;
        } else {
            vals[i] = xstrdup(l->v[i]);
            sources[i] = "miss";
        }
    }

    if (miss_n) {
        char **texts = xmalloc(miss_n * sizeof *texts);
        char **translated = xcalloc(miss_n, sizeof *translated);
        for (size_t i = 0; i < miss_n; i++) texts[i] = l->v[miss[i]];
        live_translate_group(texts, miss_n, translated);
        for (size_t i = 0; i < miss_n; i++) {
            size_t idx = miss[i];
            if (is_resolved_translation(l->v[idx], translated[i])) {
                vals[idx] = translated[i];
                translated[i] = NULL;
                sources[idx] = "api_batch";
            }
            free(translated[i]);
        }
        free(translated);
        free(texts);
    }

    for (size_t i = 0; i < l->n; i++) {
        if (duplicate_of[i] != SIZE_MAX) continue;
        if (!vals[i]) {
            vals[i] = xstrdup(l->v[i]);
            sources[i] = "miss";
        }
        if (!sources[i]) sources[i] = "miss";
    }
    for (size_t i = 0; i < l->n; i++) {
        if (duplicate_of[i] == SIZE_MAX) continue;
        size_t original = duplicate_of[i];
        vals[i] = xstrdup(vals[original]);
        sources[i] = sources[original];
    }

    buf_add(b, "{\"translations\":{");
    int first = 1;
    for (size_t i = 0; i < l->n; i++) {
        if (!first) buf_ch(b, ',');
        first = 0;
        buf_json(b, l->v[i]);
        buf_ch(b, ':');
        buf_json(b, vals[i]);
    }
    buf_add(b, "},\"results\":[");
    for (size_t i = 0; i < l->n; i++) {
        if (i) buf_ch(b, ',');
        buf_json(b, vals[i]);
    }
    buf_add(b, "],\"sources\":[");
    for (size_t i = 0; i < l->n; i++) {
        if (i) buf_ch(b, ',');
        buf_json(b, sources[i]);
    }
    buf_add(b, "]}");
    for (size_t i = 0; i < l->n; i++) free(vals[i]);
    free(vals);
    free(sources);
    free(miss);
    free(duplicate_of);
    free(dedup_idx);
    free(dedup_hash);
}

/* Return the position just past the matching '}' for the object starting at
   '{', correctly skipping string literals (and their escapes) and nested
   objects. NULL if the braces are unbalanced. A plain strchr(obj,'}') breaks
   on any value that contains a '}'. */
static const char *json_object_end(const char *p) {
    if (*p != '{') return NULL;
    int depth = 0;
    int in_str = 0;
    for (; *p; p++) {
        char c = *p;
        if (in_str) {
            if (c == '\\') {
                if (!p[1]) return NULL;
                p++; /* skip the escaped char */
            } else if (c == '"') {
                in_str = 0;
            }
        } else if (c == '"') {
            in_str = 1;
        } else if (c == '{') {
            depth++;
        } else if (c == '}') {
            if (--depth == 0) return p + 1;
        }
    }
    return NULL;
}

/* /cache/import：从 {entries:[{key,value},...]} 导入条目到缓存并落盘（与正常
   翻译路径共享清洗/等值拒绝规则）。文件系统失败属于外部边界：有效译文仍留在
   内存供 cache-first 读取，但 status/imported/memory_only 必须明确指出重启风险，
   不能把未落盘条目伪装成完整导入成功。
   扫描以数组闭括号 ']' 为上界：逐项要求下一个非空白字符是 ','、']' 或 '{'，
   防止 strchr 越过 entries 末尾把后面的兄弟成员误当条目导入。
   用 json_object_end 逐个对象切片再解析，避免 value 含 '}' 时误切。 */
static void op_import(Buf *b, const char *json) {
    const char *p = json_top_key(json, "entries");
    size_t n = 0, cap = 0;
    const char **keys = NULL, **values = NULL;
    if (p && *p == '[') {
        p++;
        for (;;) {
            p = json_skipws(p);
            if (*p == ']') break;
            if (*p == ',') {
                p++;
                continue;
            }
            if (*p != '{') break;
            const char *end = json_object_end(p);
            if (!end) break;
            char *tmp = xstrndup(p, (size_t)(end - p));
            char *k = json_top_get_str(tmp, "key");
            char *v = json_top_get_str(tmp, "value");
            if (k && v && *k && *v) {
                if (n == cap) {
                    if (cap > SIZE_MAX / 2 / sizeof *keys) die("import too large");
                    cap = cap ? cap * 2 : 64;
                    keys = xrealloc(keys, cap * sizeof *keys);
                    values = xrealloc(values, cap * sizeof *values);
                }
                /* cache_set_many_persist 只读不接管，k/v 所有权留在本函数。 */
                keys[n] = k;
                values[n] = v;
                n++;
            } else {
                free(k);
                free(v);
            }
            free(tmp);
            p = end;
        }
    }
    CachePersistResult persisted =
        cache_set_many_persist_result(g_ctx->cache, keys, values, n);
    for (size_t i = 0; i < n; i++) {
        free((void *)keys[i]);
        free((void *)values[i]);
    }
    free(keys);
    free(values);
    const char *status = "ok";
    if (persisted.status == CACHE_PERSIST_FAILED) {
        status = "error";
    } else if (persisted.status == CACHE_PERSIST_PARTIAL ||
               (persisted.accepted && persisted.rejected)) {
        status = "partial";
    } else if (!persisted.accepted && persisted.rejected) {
        status = "rejected";
    }
    buf_add(b, "{\"status\":");
    buf_json(b, status);
    buf_add(b, ",\"imported\":");
    buf_int(b, (long long)persisted.persisted);
    buf_add(b, ",\"accepted\":");
    buf_int(b, (long long)persisted.accepted);
    buf_add(b, ",\"rejected\":");
    buf_int(b, (long long)persisted.rejected);
    buf_add(b, ",\"memory_only\":");
    buf_int(b, (long long)(persisted.accepted - persisted.persisted));
    buf_add(b, "}");
}

/* /prefetch、/warmup：把文本批量排入异步队列后台翻译，立即返回已排队数。
   不等待翻译完成——这正是预热的设计：游戏启动时先排进队列，worker 慢慢消化。 */
static void op_prefetch(Buf *b, List *l) {
    int queued = 0;
    for (size_t i = 0; i < l->n; i++) {
        queued += async_enqueue_miss(l->v[i]);
    }
    buf_add(b, "{\"status\":\"queued\",\"queued\":");
    buf_int(b, queued);
    buf_add(b, "}");
}

/* 处理单个连接的完整生命周期：收请求 -> 解析方法/路径/查询/body -> 路由分发 -> 响应 -> 关闭。
   这是 public API 契约的集中地，路由与响应字段改动需格外谨慎（AGENTS.md）。
   接收策略：先设收发超时与 TCP_NODELAY；按需扩容缓冲，读到头部完整后按
   Content-Length 判断 body 是否收齐，避免过度读取或阻塞。 */
static int parse_request_line(const char *request, const char *headers_end,
                              char *method, size_t method_cap,
                              char *path, size_t path_cap) {
    if (!request || !headers_end || !method || method_cap < 2 ||
        !path || path_cap < 2) return 0;
    const char *line_end = strstr(request, "\r\n");
    if (!line_end || line_end >= headers_end) return 0;
    const char *space1 = memchr(request, ' ', (size_t)(line_end - request));
    if (!space1 || space1 == request) return 0;
    const char *space2 = memchr(space1 + 1, ' ', (size_t)(line_end - space1 - 1));
    if (!space2 || space2 == space1 + 1) return 0;
    if (memchr(space2 + 1, ' ', (size_t)(line_end - space2 - 1))) return 0;

    size_t method_len = (size_t)(space1 - request);
    size_t path_len = (size_t)(space2 - space1 - 1);
    size_t version_len = (size_t)(line_end - space2 - 1);
    if (method_len >= method_cap || path_len >= path_cap) return 0;
    if (version_len != 8 ||
        (memcmp(space2 + 1, "HTTP/1.1", 8) &&
         memcmp(space2 + 1, "HTTP/1.0", 8))) return 0;
    for (size_t i = 0; i < method_len; i++) {
        if (!isalpha((unsigned char)request[i])) return 0;
    }
    for (size_t i = 0; i < path_len; i++) {
        unsigned char c = (unsigned char)space1[1 + i];
        if (c <= 0x20 || c == 0x7f) return 0;
    }
    if (space1[1] != '/' &&
        !(path_len == 1 && space1[1] == '*' &&
          method_len == 7 && !_strnicmp(request, "OPTIONS", 7))) {
        return 0;
    }

    memcpy(method, request, method_len);
    method[method_len] = 0;
    memcpy(path, space1 + 1, path_len);
    path[path_len] = 0;
    return 1;
}

static void serve_one(SOCKET s) {
    DWORD timeout = HTTP_RECV_TIMEOUT_MS;
    if (setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char *)&timeout, sizeof timeout) == SOCKET_ERROR ||
        setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, (const char *)&timeout, sizeof timeout) == SOCKET_ERROR) {
        /* 超时是连接线程生命周期的硬约束；设置失败时不能退化成无限阻塞。 */
        closesocket(s);
        return;
    }
    BOOL nodelay = TRUE;
    (void)setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (const char *)&nodelay, sizeof nodelay);

    /* 固定初始块来自有界池；超过 64 KiB 的请求按需扩容，结束后不回池。 */
    size_t cap = HTTP_RECV_INITIAL;
    char *req = request_buffer_acquire();
    req[0] = 0;
    size_t n = 0;
    size_t expected_total = 0;
    int headers_complete = 0;
    int request_error = 0;
    int r;
    for (;;) {
        if (n + 1 >= cap) {
            if (cap >= HTTP_MAX_REQ) {
                /* 缓冲已顶到上限。若头部已收齐且声明的总长未超上限（恰好 2MB
                   的合法请求），允许用缓冲内剩余空间再 recv 一次收尾，而不是
                   把差一字节的请求误报 413；否则维持拒绝。 */
                if (!headers_complete || expected_total > cap || n >= expected_total) {
                    request_error = 413;
                    break;
                }
            } else {
                size_t new_cap = cap * 2;
                if (new_cap > HTTP_MAX_REQ) new_cap = HTTP_MAX_REQ;
                req = xrealloc(req, new_cap + 1);
                cap = new_cap;
            }
        }
        r = recv(s, req + n, (int)(cap - n), 0);
        if (r <= 0) {
            if (!headers_complete || n < expected_total) request_error = 400;
            break;
        }
        n += (size_t)r;
        req[n] = 0;
        char *he = strstr(req, "\r\n\r\n");
        if (he) {
            size_t hl = (size_t)(he + 4 - req);
            size_t body_length = 0;
            int has_content_length = 0;
            if (!parse_content_length(req, he, &body_length, &has_content_length)) {
                request_error = 400;
                break;
            }
            (void)has_content_length;
            if (hl > HTTP_MAX_REQ || body_length > HTTP_MAX_REQ - hl) {
                request_error = 413;
                break;
            }
            expected_total = hl + body_length;
            headers_complete = 1;
            if (n >= expected_total) {
                n = expected_total;
                req[n] = 0;
                break;
            }
        }
    }

    if (!headers_complete && !request_error) request_error = 400;
    if (request_error) {
        if (request_error == 413) {
            resp(s, 413, "Payload Too Large", "{\"error\":\"payload_too_large\"}");
        } else {
            resp(s, 400, "Bad Request", "{\"error\":\"incomplete_request\"}");
        }
        request_buffer_release(req, cap);
        closesocket(s);
        return;
    }

    char *headers_end = strstr(req, "\r\n\r\n");
    int origin_present = 0;
    if (!headers_end || !apply_origin_policy(req, headers_end, &origin_present)) {
        report_origin_rejection("untrusted-or-malformed");
        resp(s, 403, "Forbidden", "{\"error\":\"origin_forbidden\"}");
        request_buffer_release(req, cap);
        closesocket(s);
        return;
    }

    /* 解析请求行（方法 + 路径），拆出查询串，路径统一转小写以简化匹配。 */
    char method[16] = {0}, path[8192] = {0};
    if (!parse_request_line(req, headers_end, method, sizeof method,
                            path, sizeof path)) {
        resp(s, 400, "Bad Request", "{\"error\":\"malformed_request_line\"}");
        request_buffer_release(req, cap);
        closesocket(s);
        return;
    }
    char *query = NULL;
    char *q = strchr(path, '?');
    if (q) {
        *q = 0;
        query = q + 1;
    }
    for (char *p = path; *p; p++) *p = (char)tolower((unsigned char)*p);

    if (origin_present && browser_admin_path(path)) {
        report_origin_rejection("administrative-route");
        resp(s, 403, "Forbidden", "{\"error\":\"origin_forbidden\"}");
        request_buffer_release(req, cap);
        closesocket(s);
        return;
    }

    char *body = strstr(req, "\r\n\r\n");
    body = body ? body + 4 : "";

    Buf out;
    buf_init(&out);

    /* === 路由分发 ===
       支持的端点（public API，保持稳定）：
         OPTIONS *               CORS 预检
         GET  /health            健康检查
         GET  /capabilities      能力声明（XUnity 等钩子据此选择协议）
         GET  /translate?text=   纯文本同步翻译（XUnity GET 协议）
         POST /shutdown          关停服务器
         POST /cache/import      导入缓存条目
         GET  /cache/dump        导出缓存为 map
         POST /cache/export      导出缓存为 entries 数组
         POST /cache/lookup      批量查缓存（仅命中）
         POST /translate,/batch,/   JSON 同步翻译（单条/批量）
         POST /prefetch,/warmup     异步预热排队
    */
    if (ieq(method, "OPTIONS")) {
        resp(s, 204, "No Content", "");
    } else if (ieq(method, "GET") && ieq(path, "/health")) {
        op_health(&out);
        resp(s, 200, "OK", out.data);
    } else if (ieq(method, "GET") && ieq(path, "/capabilities")) {
        resp(s, 200, "OK", "{\"supports\":{\"batch\":true,\"cache_only\":true,\"xunity_custom_get\":true,\"xunity_batch_endpoint\":true},\"runtime_cache_only\":false}");
    } else if (ieq(method, "GET") && ieq(path, "/translate")) {
        char *text = query_get(query, "text");
        char *cache_only_s = query_get(query, "cache_only");
        int cache_only = 0;
        if (cache_only_s) cache_only = !ieq(cache_only_s, "false") && strcmp(cache_only_s, "0");
        if (!text || !*text) {
            resp_plain(s, 400, "Bad Request", "missing_text");
        } else {
            const char *source = "miss";
            char *v = translate_value(text, cache_only, cache_only, &source);
            if (!cache_only && !strcmp(source, "miss") && should_translate_text(text)) {
                /* 非缓存模式却仍然 miss：说明 API 不可用/翻译失败。返回 503
                   而非原文，让 XUnity 这类客户端能区分"真未翻译"与"译为原文"。 */
                resp_plain(s, 503, "Service Unavailable", "translation_unavailable");
            } else {
                resp_plain(s, 200, "OK", v);
            }
            free(v);
        }
        free(text);
        free(cache_only_s);
    } else if (ieq(method, "POST") && ieq(path, "/shutdown")) {
        /* 置停止标志，并把监听套接字换成 INVALID_SOCKET 并关闭，
           使主 accept 循环立即退出。 */
        InterlockedExchange(g_ctx->stop, 1);
        resp(s, 200, "OK", "{\"status\":\"shutting_down\"}");
        SOCKET prev = (SOCKET)(uintptr_t)InterlockedExchangePointer(
            (PVOID volatile *)g_ctx->server_sock, (PVOID)(uintptr_t)INVALID_SOCKET);
        if (prev != INVALID_SOCKET) closesocket(prev);
    } else if (ieq(method, "POST") && ieq(path, "/cache/import")) {
        op_import(&out, body);
        resp(s, 200, "OK", out.data);
    } else if (ieq(method, "GET") && ieq(path, "/cache/dump")) {
        op_cache_dump(&out);
        resp(s, 200, "OK", out.data);
    } else if (ieq(method, "POST") && ieq(path, "/cache/export")) {
        op_cache_export(&out);
        resp(s, 200, "OK", out.data);
    } else if (ieq(method, "POST") && ieq(path, "/cache/lookup")) {
        List l = json_top_array(body, "texts");
        op_lookup(&out, &l);
        list_free(&l);
        resp(s, 200, "OK", out.data);
    } else if (ieq(method, "POST") && (ieq(path, "/translate") || ieq(path, "/batch") || ieq(path, "/"))) {
        /* JSON 批量/单条翻译。兼容 texts 数组与单 text 字段两种入参；
           "/" 别名用于某些钩子的默认端点。cache_only 从 body 读取。 */
        List l = json_top_array(body, "texts");
        char *one = NULL;
        if (!l.n) {
            one = json_top_get_str(body, "text");
            if (one) list_push(&l, xstrdup(one));
        }
        if (!l.n) {
            resp(s, 400, "Bad Request", "{\"error\":\"missing_text\"}");
        } else {
            op_batch(&out, &l, ieq(path, "/translate"), json_bool_true(body, "cache_only"));
            resp(s, 200, "OK", out.data);
        }
        free(one);
        list_free(&l);
    } else if (ieq(method, "POST") && (ieq(path, "/prefetch") || ieq(path, "/warmup"))) {
        /* 异步预热：入参同上，但不等待翻译，立即返回排队数。 */
        List l = json_top_array(body, "texts");
        char *one = NULL;
        if (!l.n) {
            one = json_top_get_str(body, "text");
            if (one) list_push(&l, xstrdup(one));
        }
        op_prefetch(&out, &l);
        free(one);
        list_free(&l);
        resp(s, 200, "OK", out.data);
    } else {
        resp(s, 404, "Not Found", "{\"error\":\"not_found\"}");
    }

    buf_free(&out);
    request_buffer_release(req, cap);
    closesocket(s);
}

/* 连接处理线程入口：每个 accept 到的连接派发一个线程跑 serve_one。 */
DWORD WINAPI http_serve_thread(LPVOID arg) {
    serve_one((SOCKET)(uintptr_t)arg);
    return 0;
}
