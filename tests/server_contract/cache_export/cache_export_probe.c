/* 源链接真实缓存和序列化；只在测试编译单元观测锁边界，不改生产代码。 */
#include "cache.h"
#include "util.h"
#include <psapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static Cache cache;
static HANDLE writer_start, writer_done, writer_ready;
static DWORD main_thread;
static LARGE_INTEGER frequency, lock_start;
static double lock_ms, write_ms, writer_wait_ms;
static int measuring;
static volatile LONG stopping;

static double elapsed(LARGE_INTEGER start) {
    LARGE_INTEGER end;
    QueryPerformanceCounter(&end);
    return (double)(end.QuadPart - start.QuadPart) * 1000.0 / frequency.QuadPart;
}
static void wait_event(HANDLE event, const char *operation) {
    if (WaitForSingleObject(event, 30000) != WAIT_OBJECT_0) die(operation);
}
static HANDLE make_event(void) {
    HANDLE event = CreateEventW(NULL, FALSE, FALSE, NULL);
    if (!event) die("CreateEvent failed");
    return event;
}
static void probe_shared(PSRWLOCK lock) {
    AcquireSRWLockShared(lock);
    if (measuring && lock == &cache.lock && GetCurrentThreadId() == main_thread) {
        QueryPerformanceCounter(&lock_start);
        SetEvent(writer_start);
    }
}
static void probe_shared_release(PSRWLOCK lock) {
    if (measuring && lock == &cache.lock && GetCurrentThreadId() == main_thread)
        lock_ms += elapsed(lock_start);
    ReleaseSRWLockShared(lock);
}
static void probe_exclusive(PSRWLOCK lock) {
    LARGE_INTEGER start;
    QueryPerformanceCounter(&start);
    AcquireSRWLockExclusive(lock);
    if (measuring && lock == &cache.lock && GetCurrentThreadId() != main_thread)
        writer_wait_ms += elapsed(start);
}

#define AcquireSRWLockShared probe_shared
#define ReleaseSRWLockShared probe_shared_release
#define AcquireSRWLockExclusive probe_exclusive
#ifndef DST_CACHE_SOURCE
#define DST_CACHE_SOURCE "../../../native/src/server/cache.c"
#endif
#include DST_CACHE_SOURCE
#undef AcquireSRWLockShared
#undef ReleaseSRWLockShared
#undef AcquireSRWLockExclusive

static const char *const hot_key = "fixture-000000";
static DWORD WINAPI writer(void *unused) {
    (void)unused;
    unsigned int round = 0;
    for (;;) {
        SetEvent(writer_ready);
        wait_event(writer_start, "writer start timed out");
        if (InterlockedCompareExchange(&stopping, 0, 0)) return 0;
        LARGE_INTEGER start;
        QueryPerformanceCounter(&start);
        cache_set(&cache, hot_key, round++ & 1 ? "updated-A" : "updated-B");
        write_ms = elapsed(start);
        SetEvent(writer_done);
    }
}
static void fill_cache(size_t count, size_t bytes) {
    /* 四引擎常见标签与 UTF-8 混合；固定语料，不读取用户缓存。 */
    const char *unit = "\xe4\xb8\xad\xe6\x96\x87 <b>{name}</b> [color=red]%s[/color] \\C[2] \"quoted\"\n";
    size_t unit_len = strlen(unit);
    char *value = xmalloc(bytes + unit_len + 1);
    size_t length = 0;
    while (length < bytes) {
        memcpy(value + length, unit, unit_len);
        length += unit_len;
    }
    value[length] = 0;
    cache_init(&cache, "unused-benchmark-cache.tsv");
    for (size_t i = 0; i < count; i++) {
        char key[48];
        snprintf(key, sizeof key, "fixture-%06zu", i);
        cache_set(&cache, key, value);
    }
    free(value);
}
static void destroy_cache(void) {
    for (size_t i = 0; i < cache.cap; i++) if (cache.e[i].used) {
        free(cache.e[i].k);
        free(cache.e[i].v);
    }
    free(cache.e);
}
static PROCESS_MEMORY_COUNTERS_EX memory_info(void) {
    PROCESS_MEMORY_COUNTERS_EX info = {0};
    info.cb = sizeof info;
    if (!GetProcessMemoryInfo(GetCurrentProcess(), (PROCESS_MEMORY_COUNTERS *)&info, sizeof info))
        die("GetProcessMemoryInfo failed");
    return info;
}
static size_t export_cache(Buf *out, int entries) {
    buf_add(out, entries ? "[" : "{");
    size_t count = entries ? cache_emit_json_entries(&cache, out) : cache_emit_json_map(&cache, out);
    buf_add(out, entries ? "]" : "}");
    return count;
}
int main(int argc, char **argv) {
    if (argc != 5) {
        fputs("usage: cache_export_probe count value_bytes map|entries rounds\n", stderr);
        return 2;
    }
    size_t count = strtoull(argv[1], NULL, 10);
    size_t bytes = strtoull(argv[2], NULL, 10);
    int entries = strcmp(argv[3], "entries") == 0;
    int rounds = atoi(argv[4]);
    if (!count || count > 1000000 || !bytes || bytes > 8192 || rounds < 1 || rounds > 1000 ||
        (!entries && strcmp(argv[3], "map") != 0)) return 2;
    QueryPerformanceFrequency(&frequency);
    main_thread = GetCurrentThreadId();
    writer_start = make_event(); writer_done = make_event(); writer_ready = make_event();
    fill_cache(count, bytes);
    PROCESS_MEMORY_COUNTERS_EX initial = memory_info();
    HANDLE thread = CreateThread(NULL, 0, writer, NULL, 0, NULL);
    if (!thread) die("CreateThread failed");
    for (int round = -3; round < rounds; round++) {
        wait_event(writer_ready, "writer ready timed out");
        Buf out; buf_init(&out);
#ifdef DST_PROBE_PREALLOC
        /* 对照组把容量准备移到计时外，只区分编码与扩容成本。 */
        buf_grow(&out, count * (bytes * 2 + 512));
#endif
        LARGE_INTEGER start;
        lock_ms = writer_wait_ms = write_ms = 0;
        measuring = 1;
        QueryPerformanceCounter(&start);
        size_t actual = export_cache(&out, entries);
        double export_ms = elapsed(start);
        wait_event(writer_done, "writer done timed out");
        measuring = 0;
        if (actual != count || out.len == 0 || out.data[out.len] != 0) die("incorrect export");
        PROCESS_MEMORY_COUNTERS_EX current = memory_info();
        if (round >= 0)
            printf("{\"round\":%d,\"count\":%zu,\"value_bytes_min\":%zu,\"format\":\"%s\","
                   "\"export_ms\":%.6f,\"read_lock_ms\":%.6f,\"writer_ms\":%.6f,"
                   "\"writer_lock_wait_ms\":%.6f,\"output_bytes\":%zu,"
                   "\"initial_private_bytes\":%zu,\"private_bytes\":%zu,"
                   "\"peak_commit_bytes\":%zu,\"peak_working_set_bytes\":%zu}\n",
                   round, count, bytes, argv[3], export_ms, lock_ms, write_ms, writer_wait_ms,
                   out.len, (size_t)initial.PrivateUsage, (size_t)current.PrivateUsage,
                   (size_t)current.PeakPagefileUsage, (size_t)current.PeakWorkingSetSize);
        buf_free(&out);
    }
    wait_event(writer_ready, "final writer ready timed out");
    InterlockedExchange(&stopping, 1);
    SetEvent(writer_start);
    wait_event(thread, "writer exit timed out");
    CloseHandle(thread);
    CloseHandle(writer_start); CloseHandle(writer_done); CloseHandle(writer_ready);
    destroy_cache();
    return 0;
}
