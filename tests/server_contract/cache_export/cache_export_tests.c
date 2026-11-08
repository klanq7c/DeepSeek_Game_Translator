/* 在真实 JSON 编码边界暂停，验证并发持久化/扩容不会阻塞或破坏导出快照。 */
#include "cache.h"
#include "util.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static Cache cache;
static HANDLE encode_started, write_finished;
static LONG pause_encoding;
static int writer_finished_during_encode;
static int passed, failed;

static void check(int ok, const char *label) {
    if (ok) passed++;
    else { failed++; fprintf(stderr, "FAIL %s\n", label); }
}
static void test_json(Buf *out, const char *text) {
    if (InterlockedCompareExchange(&pause_encoding, 0, 1) == 1) {
        SetEvent(encode_started);
        DWORD status = WaitForSingleObject(write_finished, 1000);
        if (status == WAIT_FAILED) die("WaitForSingleObject failed");
        writer_finished_during_encode = status == WAIT_OBJECT_0;
    }
    buf_json(out, text);
}
#define buf_json test_json
#include "../../../native/src/server/cache.c"
#undef buf_json

static const char *keys[] = {
    "RenPy {color=#fff}[name]{/color}", "RPGMaker \\C[2]\\V[1]",
    "Unity <link=\"a>b\">{0}</link>", "Godot [color=red]%s[/color]"
};
static const char *const values[] = {
    "\xe4\xb8\xad {color=#fff}[name]{/color}", "\xe6\x96\x87 \\C[2]\\V[1]\n",
    "\xe4\xb8\xad <link=\"a>b\">{0}</link>", "\xe6\x96\x87 [color=red]%s[/color]\t\"\\"
};
static const char *new_values[] = {"new-1", "new-2", "new-3", "new-4"};
static void destroy(Cache *c) {
    if (c->persist_f && fclose(c->persist_f) != 0) die("fixture close failed");
    for (size_t i = 0; i < c->cap; i++) if (c->e[i].used) {
        free(c->e[i].k); free(c->e[i].v);
    }
    free(c->e);
}
static DWORD WINAPI mutate(void *unused) {
    (void)unused;
    if (WaitForSingleObject(encode_started, 30000) != WAIT_OBJECT_0) die("encode start timeout");
    CachePersistResult result = cache_set_many_persist_result(&cache, keys, new_values, 4);
    if (result.status != CACHE_PERSIST_ALL || result.persisted != 4) die("fixture persist failed");
    /* 真实插入触发 rehash，使原表和原值都失效；不能只复制借用指针。 */
    for (size_t i = 0; i < 4096; i++) {
        char key[64];
        snprintf(key, sizeof key, "added-%zu", i);
        cache_set(&cache, key, "added-value");
    }
    SetEvent(write_finished);
    return 0;
}
static size_t emit(Buf *out, int entries) {
    return entries ? cache_emit_json_entries(&cache, out) : cache_emit_json_map(&cache, out);
}
static void concurrency_test(const char *path, int entries) {
    cache_init(&cache, path);
    for (size_t i = 0; i < 4; i++) cache_set(&cache, keys[i], values[i]);
    Buf expected, actual;
    buf_init(&expected); buf_init(&actual);
    buf_add(&expected, "prefix:"); buf_add(&actual, "prefix:");
    check(emit(&expected, entries) == 4, "initial export count");
    encode_started = CreateEventW(NULL, FALSE, FALSE, NULL);
    write_finished = CreateEventW(NULL, FALSE, FALSE, NULL);
    if (!encode_started || !write_finished) die("CreateEvent failed");
    HANDLE thread = CreateThread(NULL, 0, mutate, NULL, 0, NULL);
    if (!thread) die("CreateThread failed");
    writer_finished_during_encode = 0;
    InterlockedExchange(&pause_encoding, 1);
    size_t count = emit(&actual, entries);
    check(writer_finished_during_encode, entries ? "entries serializes outside cache lock" : "map serializes outside cache lock");
    if (WaitForSingleObject(thread, 30000) != WAIT_OBJECT_0) die("writer timeout");
    check(count == 4, "export keeps snapshot count during insertion");
    check(actual.len == expected.len && memcmp(actual.data, expected.data, expected.len + 1) == 0,
          "snapshot owns all original key/value bytes across update and rehash");
    check(cache_size(&cache) == 4100, "concurrent rehash/inserts complete");
    for (size_t i = 0; i < 4; i++) {
        char *value = cache_get(&cache, keys[i]);
        check(value && strcmp(value, new_values[i]) == 0, "concurrent update visible");
        free(value);
    }
    Cache reloaded;
    cache_init(&reloaded, path);
    cache_load(&reloaded);
    check(cache_size(&reloaded) == 4, "persistent update survives reload");
    for (size_t i = 0; i < 4; i++) {
        char *value = cache_get(&reloaded, keys[i]);
        check(value && strcmp(value, new_values[i]) == 0, "persistent values survive export");
        free(value);
    }
    destroy(&reloaded); destroy(&cache);
    CloseHandle(thread); CloseHandle(encode_started); CloseHandle(write_finished);
    buf_free(&expected); buf_free(&actual);
}
static void golden_test(int entries) {
    cache_init(&cache, "unused-golden.tsv");
    Buf out; buf_init(&out); buf_add(&out, "prefix:");
    check(emit(&out, entries) == 0 && strcmp(out.data, "prefix:") == 0, "empty export preserves prefix");
    cache_set(&cache, "key\"\\\n", "value\t\r\001\xe4\xb8\xad");
    const char *golden = entries ?
        "prefix:{\"key\":\"key\\\"\\\\\\n\",\"value\":\"value\\t\\r\\u0001\xe4\xb8\xad\"}" :
        "prefix:\"key\\\"\\\\\\n\":\"value\\t\\r\\u0001\xe4\xb8\xad\"";
    check(emit(&out, entries) == 1, "golden count");
    check(strcmp(out.data, golden) == 0, "exact JSON bytes and prefix");
    buf_free(&out); destroy(&cache);
}
int main(int argc, char **argv) {
    if (argc != 3) { fputs("usage: cache_export_tests map_fixture.tsv entries_fixture.tsv\n", stderr); return 2; }
    golden_test(0); golden_test(1);
    concurrency_test(argv[1], 0); concurrency_test(argv[2], 1);
    printf("Cache export: %d passed, %d failed\n", passed, failed);
    return failed ? 1 : 0;
}
