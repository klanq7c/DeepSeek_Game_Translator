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

static HttpCtx *g_ctx;

void http_set_ctx(HttpCtx *ctx) { g_ctx = ctx; }

/* Sized for whole-script Ren'Py warmups (30k lines): jobs are ~150 bytes, so
   the worst case stays around 10 MB while the queue drains through the API. */
#define ASYNC_QUEUE_LIMIT 65536
#define ASYNC_BATCH_MAX 16
#define ASYNC_BATCH_CHAR_BUDGET 3200
#define ASYNC_BATCH_COALESCE_MS 25
#define ASYNC_FOREGROUND_YIELD_MS 25
#define ASYNC_KNOWN_BUCKETS 8192
#define LIVE_BATCH_MAX 16
#define LIVE_BATCH_CHAR_BUDGET 3200
#define LIVE_BATCH_COALESCE_MS 35

typedef struct AsyncJob {
    char *text;
    uint64_t hash;
    struct AsyncJob *qnext;
    struct AsyncJob *knext;
} AsyncJob;

typedef struct LiveJob {
    char *text;
    char *result;
    int done;
    CONDITION_VARIABLE cv;
    struct LiveJob *next;
} LiveJob;

static SRWLOCK g_async_lock = SRWLOCK_INIT;
static CONDITION_VARIABLE g_async_cv = CONDITION_VARIABLE_INIT;
static AsyncJob *g_async_head;
static AsyncJob *g_async_tail;
static AsyncJob *g_async_known[ASYNC_KNOWN_BUCKETS];
static size_t g_async_len;
static volatile LONG g_async_started;

static SRWLOCK g_live_lock = SRWLOCK_INIT;
static CONDITION_VARIABLE g_live_cv = CONDITION_VARIABLE_INIT;
static LiveJob *g_live_head;
static LiveJob *g_live_tail;
static size_t g_live_len;
static volatile LONG g_live_started;
static volatile LONG g_foreground_requests;

static DWORD WINAPI async_worker(LPVOID arg);
static DWORD WINAPI live_worker(LPVOID arg);

static size_t async_queue_len(void) {
    AcquireSRWLockShared(&g_async_lock);
    size_t n = g_async_len;
    ReleaseSRWLockShared(&g_async_lock);
    return n;
}

static size_t live_queue_len(void) {
    AcquireSRWLockShared(&g_live_lock);
    size_t n = g_live_len;
    ReleaseSRWLockShared(&g_live_lock);
    return n;
}

static void foreground_enter(void) {
    InterlockedIncrement(&g_foreground_requests);
}

static void foreground_leave(void) {
    InterlockedDecrement(&g_foreground_requests);
}

static int foreground_active(void) {
    return InterlockedCompareExchange(&g_foreground_requests, 0, 0) > 0 || live_queue_len() > 0;
}

static int server_stopping(void) {
    return !g_ctx || InterlockedCompareExchange(g_ctx->stop, 0, 0) != 0;
}

static void async_wait_for_foreground(void) {
    while (!server_stopping() && foreground_active()) {
        Sleep(ASYNC_FOREGROUND_YIELD_MS);
    }
}

static int sendall(SOCKET s, const char *p, size_t n) {
    while (n) {
        int r = send(s, p, n > INT_MAX ? INT_MAX : (int)n, 0);
        if (r <= 0) return 0;
        p += r;
        n -= (size_t)r;
    }
    return 1;
}

static void resp(SOCKET s, int code, const char *msg, const char *body) {
    const char *ctype = "application/json; charset=utf-8";
    char h[512];
    size_t n = strlen(body);
    int k = snprintf(h, sizeof h,
                     "HTTP/1.1 %d %s\r\n"
                     "Content-Type: %s\r\n"
                     "Content-Length: %zu\r\n"
                     "Access-Control-Allow-Origin: *\r\n"
                     "Access-Control-Allow-Methods: GET, POST, OPTIONS\r\n"
                     "Access-Control-Allow-Headers: Content-Type\r\n"
                     "Connection: close\r\n\r\n",
                     code, msg, ctype, n);
    if (k < 0 || (size_t)k >= sizeof h) return; /* never read past the buffer */
    sendall(s, h, (size_t)k);
    sendall(s, body, n);
}

static void resp_plain(SOCKET s, int code, const char *msg, const char *body) {
    char h[512];
    size_t n = strlen(body ? body : "");
    int k = snprintf(h, sizeof h,
                     "HTTP/1.1 %d %s\r\n"
                     "Content-Type: text/plain; charset=utf-8\r\n"
                     "Content-Length: %zu\r\n"
                     "Access-Control-Allow-Origin: *\r\n"
                     "Access-Control-Allow-Methods: GET, POST, OPTIONS\r\n"
                     "Access-Control-Allow-Headers: Content-Type\r\n"
                     "Connection: close\r\n\r\n",
                     code, msg, n);
    if (k < 0 || (size_t)k >= sizeof h) return; /* never read past the buffer */
    sendall(s, h, (size_t)k);
    sendall(s, body ? body : "", n);
}

static int hexval(char c) {
    if ('0' <= c && c <= '9') return c - '0';
    if ('a' <= c && c <= 'f') return c - 'a' + 10;
    if ('A' <= c && c <= 'F') return c - 'A' + 10;
    return -1;
}

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

static int mem_ieq(const char *a, const char *b, size_t n) {
    for (size_t i = 0; i < n; i++) {
        if (tolower((unsigned char)a[i]) != tolower((unsigned char)b[i])) return 0;
    }
    return 1;
}

static char *header_value(char *req, char *headers_end, const char *name) {
    size_t nl = strlen(name);
    char *p = strstr(req, "\r\n");
    if (!p || p >= headers_end) return NULL;
    p += 2;
    while (p < headers_end) {
        char *line_end = strstr(p, "\r\n");
        if (!line_end || line_end > headers_end) line_end = headers_end;
        char *colon = memchr(p, ':', (size_t)(line_end - p));
        if (colon && (size_t)(colon - p) == nl && mem_ieq(p, name, nl)) {
            char *v = colon + 1;
            while (v < line_end && (*v == ' ' || *v == '\t')) v++;
            return v;
        }
        if (line_end == headers_end) break;
        p = line_end + 2;
    }
    return NULL;
}

static int json_bool_true(const char *json, const char *key) {
    /* If the key text isn't even a substring of the body, it can't be a key;
       skip json_key's allocating full-body scan (it parses every string). */
    if (!strstr(json, key)) return 0;
    const char *p = json_key(json, key);
    if (!p) return 0;
    return !strncmp(p, "true", 4) || !strncmp(p, "1", 1);
}

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

static int should_translate_text(const char *text) {
    return text && *text && has_translation_signal(text) && !utf8_has_cjk(text);
}

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

static int ensure_worker_pool(volatile LONG *started, LPTHREAD_START_ROUTINE fn) {
    if (InterlockedCompareExchange(started, 1, 0) == 0) {
        int created = 0;
        int want = worker_pool_size();
        for (int i = 0; i < want; i++) {
            HANDLE th = CreateThread(NULL, 64 * 1024, fn, NULL,
                                     STACK_SIZE_PARAM_IS_A_RESERVATION, NULL);
            if (th) {
                CloseHandle(th);
                created++;
            }
        }
        if (!created) {
            InterlockedExchange(started, 0);
            return 0;
        }
    }
    return 1;
}

static int ensure_async_worker(void) {
    return ensure_worker_pool(&g_async_started, async_worker);
}

static int ensure_live_worker(void) {
    return ensure_worker_pool(&g_live_started, live_worker);
}

static int async_known_locked(const char *text, uint64_t hash) {
    size_t slot = (size_t)hash & (ASYNC_KNOWN_BUCKETS - 1);
    for (AsyncJob *j = g_async_known[slot]; j; j = j->knext) {
        if (j->hash == hash && strcmp(j->text, text) == 0) return 1;
    }
    return 0;
}

static int async_enqueue_miss(const char *text) {
    if (!g_ctx || !g_ctx->api || !g_ctx->api->enabled || !should_translate_text(text)) return 0;

    char *hit = cache_get(g_ctx->cache, text);
    if (hit) {
        free(hit);
        return 0;
    }
    if (!ensure_async_worker()) return 0;

    uint64_t h = h64(text);
    AcquireSRWLockExclusive(&g_async_lock);
    if (g_async_len >= ASYNC_QUEUE_LIMIT || async_known_locked(text, h)) {
        ReleaseSRWLockExclusive(&g_async_lock);
        return 0;
    }

    AsyncJob *job = xmalloc(sizeof *job);
    job->text = xstrdup(text);
    job->hash = h;
    job->qnext = NULL;
    size_t slot = (size_t)h & (ASYNC_KNOWN_BUCKETS - 1);
    job->knext = g_async_known[slot];
    g_async_known[slot] = job;
    if (g_async_tail) g_async_tail->qnext = job;
    else g_async_head = job;
    g_async_tail = job;
    g_async_len++;
    WakeConditionVariable(&g_async_cv);
    ReleaseSRWLockExclusive(&g_async_lock);
    return 1;
}

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

static AsyncJob *async_pop_locked(void) {
    AsyncJob *job = g_async_head;
    if (!job) return NULL;
    g_async_head = job->qnext;
    if (!g_async_head) g_async_tail = NULL;
    g_async_len--;
    job->qnext = NULL;
    return job;
}

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

static void async_finish_jobs(AsyncJob **jobs, size_t count) {
    AcquireSRWLockExclusive(&g_async_lock);
    for (size_t i = 0; i < count; i++) async_forget_locked(jobs[i]);
    ReleaseSRWLockExclusive(&g_async_lock);
    for (size_t i = 0; i < count; i++) {
        free(jobs[i]->text);
        free(jobs[i]);
    }
}

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
        for (size_t i = 0; i < pn; i++) {
            if (is_resolved_translation(pending[i]->text, translated[i])) {
                cache_set_persist(g_ctx->cache, pending[i]->text, translated[i]);
            }
            free(translated[i]);
        }
        free(translated);
        return;
    }

    for (size_t i = 0; i < pn; i++) {
        char *live = NULL;
        async_wait_for_foreground();
        if (server_stopping() || !g_ctx->api || !g_ctx->api->enabled) break;
        if (api_translate(g_ctx->api, pending[i]->text, &live) &&
            is_resolved_translation(pending[i]->text, live)) {
            cache_set_persist(g_ctx->cache, pending[i]->text, live);
        }
        free(live);
    }
}

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

static void live_enqueue_locked(LiveJob *job) {
    job->next = NULL;
    if (g_live_tail) g_live_tail->next = job;
    else g_live_head = job;
    g_live_tail = job;
    g_live_len++;
    WakeConditionVariable(&g_live_cv);
}

static LiveJob *live_pop_locked(void) {
    LiveJob *job = g_live_head;
    if (!job) return NULL;
    g_live_head = job->next;
    if (!g_live_head) g_live_tail = NULL;
    g_live_len--;
    job->next = NULL;
    return job;
}

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

static void live_finish_batch(LiveJob **jobs, char **results, size_t count) {
    AcquireSRWLockExclusive(&g_live_lock);
    for (size_t i = 0; i < count; i++) {
        jobs[i]->result = results[i] ? results[i] : xstrdup(jobs[i]->text);
        jobs[i]->done = 1;
        WakeConditionVariable(&jobs[i]->cv);
    }
    ReleaseSRWLockExclusive(&g_live_lock);
}

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
        for (size_t i = 0; i < pn; i++) {
            if (is_resolved_translation(pending[i]->text, translated[i])) {
                pending[i]->result = NULL;
                cache_set_persist(g_ctx->cache, pending[i]->text, translated[i]);
            }
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
            cache_set_persist(g_ctx->cache, texts[0], live);
            for (size_t i = 0; i < count; i++) {
                if (jobs[i] == pending[0] && !results[i]) {
                    results[i] = live;
                    live = NULL;
                    break;
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

static char *live_translate_batched(const char *text) {
    if (!g_ctx || !g_ctx->api || !g_ctx->api->enabled || !should_translate_text(text)) return NULL;
    if (!ensure_live_worker()) return NULL;

    LiveJob job;
    memset(&job, 0, sizeof job);
    job.text = xstrdup(text);
    InitializeConditionVariable(&job.cv);

    foreground_enter();
    AcquireSRWLockExclusive(&g_live_lock);
    live_enqueue_locked(&job);
    while (!job.done) {
        SleepConditionVariableSRW(&job.cv, &g_live_lock, INFINITE, 0);
    }
    ReleaseSRWLockExclusive(&g_live_lock);
    foreground_leave();

    free(job.text);
    return job.result;
}

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
            cache_set_persist(g_ctx->cache, text, live);
            if (source) *source = "api_batch";
            return live;
        }
        free(live);
    }
    if (source) *source = "miss";
    return xstrdup(text);
}

static void op_health(Buf *b) {
    buf_add(b, "{\"status\":\"ok\",\"server\":\"dst_server_c\",\"cache_size\":");
    buf_int(b, (long long)cache_size(g_ctx->cache));
    buf_add(b, ",\"async_queue\":");
    buf_int(b, (long long)async_queue_len());
    buf_add(b, ",\"live_queue\":");
    buf_int(b, (long long)live_queue_len());
    buf_add(b, ",\"api_enabled\":");
    buf_add(b, (g_ctx->api && g_ctx->api->enabled) ? "true" : "false");
    buf_add(b, ",\"runtime_cache_only\":false,\"uptime_seconds\":");
    buf_int(b, (long long)(time(NULL) - g_ctx->started));
    buf_add(b, "}");
}

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

static void op_cache_dump(Buf *b) {
    buf_add(b, "{\"cache\":{");
    size_t n = cache_emit_json_map(g_ctx->cache, b);
    buf_add(b, "},\"count\":");
    buf_int(b, (long long)n);
    buf_add(b, "}");
}

static void op_cache_export(Buf *b) {
    buf_add(b, "{\"entries\":[");
    size_t n = cache_emit_json_entries(g_ctx->cache, b);
    buf_add(b, "],\"count\":");
    buf_int(b, (long long)n);
    buf_add(b, "}");
}

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

    char **vals = calloc(l->n ? l->n : 1, sizeof *vals);
    const char **sources = calloc(l->n ? l->n : 1, sizeof *sources);
    size_t *miss = calloc(l->n ? l->n : 1, sizeof *miss);
    if (!vals) die("oom");
    if (!sources) die("oom");
    if (!miss) die("oom");
    size_t miss_n = 0;

    for (size_t i = 0; i < l->n; i++) {
        for (size_t j = 0; j < i; j++) {
            if (vals[j] && strcmp(l->v[i], l->v[j]) == 0) {
                vals[i] = xstrdup(vals[j]);
                sources[i] = sources[j];
                break;
            }
        }
        if (vals[i]) continue;

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

    if (miss_n) foreground_enter();
    for (size_t start = 0; start < miss_n; start += ASYNC_BATCH_MAX) {
        size_t n = miss_n - start;
        if (n > ASYNC_BATCH_MAX) n = ASYNC_BATCH_MAX;
        char *texts[ASYNC_BATCH_MAX];
        for (size_t j = 0; j < n; j++) texts[j] = l->v[miss[start + j]];

        char **translated = NULL;
        if (api_translate_batch(g_ctx->api, texts, n, &translated)) {
            for (size_t j = 0; j < n; j++) {
                size_t idx = miss[start + j];
                if (is_resolved_translation(l->v[idx], translated[j])) {
                    vals[idx] = translated[j];
                    sources[idx] = "api_batch";
                    cache_set_persist(g_ctx->cache, l->v[idx], translated[j]);
                    translated[j] = NULL;
                }
                free(translated[j]);
            }
            free(translated);
        }
        for (size_t j = 0; j < n; j++) {
            size_t idx = miss[start + j];
            if (vals[idx]) continue;
            char *live = NULL;
            if (api_translate(g_ctx->api, l->v[idx], &live) &&
                is_resolved_translation(l->v[idx], live)) {
                vals[idx] = live;
                sources[idx] = "api";
                cache_set_persist(g_ctx->cache, l->v[idx], live);
                live = NULL;
            }
            free(live);
        }
    }
    if (miss_n) foreground_leave();

    for (size_t i = 0; i < l->n; i++) {
        if (!vals[i]) {
            vals[i] = xstrdup(l->v[i]);
            sources[i] = "miss";
        }
        if (!sources[i]) sources[i] = "miss";
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

static void op_import(Buf *b, const char *json) {
    const char *p = json_key(json, "entries");
    int n = 0;
    if (p && *p == '[') {
        p++;
        while (*p) {
            const char *obj = strchr(p, '{');
            if (!obj) break;
            const char *end = json_object_end(obj);
            if (!end) break;
            char *tmp = xstrndup(obj, (size_t)(end - obj));
            char *k = json_get_str(tmp, "key");
            char *v = json_get_str(tmp, "value");
            if (k && v && *k && *v) {
                cache_set(g_ctx->cache, k, v);
                n++;
            }
            free(k);
            free(v);
            free(tmp);
            p = end;
        }
    }
    buf_add(b, "{\"status\":\"ok\",\"imported\":");
    buf_int(b, n);
    buf_add(b, "}");
}

static void op_prefetch(Buf *b, List *l) {
    int queued = 0;
    for (size_t i = 0; i < l->n; i++) {
        queued += async_enqueue_miss(l->v[i]);
    }
    buf_add(b, "{\"status\":\"queued\",\"queued\":");
    buf_int(b, queued);
    buf_add(b, "}");
}

static void serve_one(SOCKET s) {
    DWORD timeout = HTTP_RECV_TIMEOUT_MS;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char *)&timeout, sizeof timeout);
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, (const char *)&timeout, sizeof timeout);
    BOOL nodelay = TRUE;
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (const char *)&nodelay, sizeof nodelay);

    /* Grow-on-demand: most requests fit in 64 KB; only OOM-sized ones realloc. */
    size_t cap = HTTP_RECV_INITIAL;
    char *req = xmalloc(cap + 1);
    req[0] = 0;
    size_t n = 0;
    int r;
    for (;;) {
        if (n + 1 >= cap) {
            if (cap >= HTTP_MAX_REQ) break;
            size_t new_cap = cap * 2;
            if (new_cap > HTTP_MAX_REQ) new_cap = HTTP_MAX_REQ;
            req = xrealloc(req, new_cap + 1);
            cap = new_cap;
        }
        r = recv(s, req + n, (int)(cap - n), 0);
        if (r <= 0) break;
        n += (size_t)r;
        req[n] = 0;
        char *he = strstr(req, "\r\n\r\n");
        if (he) {
            size_t hl = (size_t)(he + 4 - req);
            char *cl = header_value(req, he, "Content-Length");
            size_t need = 0;
            if (cl) {
                unsigned long long parsed = strtoull(cl, NULL, 10);
                size_t budget = (hl <= HTTP_MAX_REQ) ? (HTTP_MAX_REQ - hl) : 0;
                need = parsed > budget ? budget : (size_t)parsed;
            }
            if (n >= hl + need) break;
        }
    }

    char method[16] = {0}, path[8192] = {0};
    sscanf(req, "%15s %8191s", method, path);
    char *query = NULL;
    char *q = strchr(path, '?');
    if (q) {
        *q = 0;
        query = q + 1;
    }
    for (char *p = path; *p; p++) *p = (char)tolower((unsigned char)*p);

    char *body = strstr(req, "\r\n\r\n");
    body = body ? body + 4 : "";

    Buf out;
    buf_init(&out);

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
                resp_plain(s, 503, "Service Unavailable", "translation_unavailable");
            } else {
                resp_plain(s, 200, "OK", v);
            }
            free(v);
        }
        free(text);
        free(cache_only_s);
    } else if (ieq(method, "POST") && ieq(path, "/shutdown")) {
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
        List l = json_array(body, "texts");
        op_lookup(&out, &l);
        list_free(&l);
        resp(s, 200, "OK", out.data);
    } else if (ieq(method, "POST") && (ieq(path, "/translate") || ieq(path, "/batch") || ieq(path, "/"))) {
        List l = json_array(body, "texts");
        char *one = NULL;
        if (!l.n) {
            one = json_get_str(body, "text");
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
        List l = json_array(body, "texts");
        char *one = NULL;
        if (!l.n) {
            one = json_get_str(body, "text");
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
    free(req);
    closesocket(s);
}

DWORD WINAPI http_serve_thread(LPVOID arg) {
    serve_one((SOCKET)(uintptr_t)arg);
    return 0;
}
