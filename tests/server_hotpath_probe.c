#include "buf.h"
#include "b64.h"
#include "cache.h"
#include "util.h"

#include <stdio.h>
#include <string.h>

int main(void) {
    const char *b64_cases[] = {"", "f", "fo", "foo", "hello\tworld\n", "\xe4\xbd\xa0\xe5\xa5\xbd"};
    const char *b64_expected[] = {"", "Zg==", "Zm8=", "Zm9v", "aGVsbG8Jd29ybGQK", "5L2g5aW9"};
    Buf encoded = {0};
    for (size_t i = 0; i < sizeof b64_cases / sizeof b64_cases[0]; i++) {
        encoded.len = 0;
        if (encoded.data) encoded.data[0] = 0;
        b64enc_append(&encoded, b64_cases[i]);
        if (strcmp(encoded.data, b64_expected[i]) != 0) {
            fprintf(stderr, "Base64 encoding differs from RFC4648 vector %zu\n", i);
            buf_free(&encoded);
            return 1;
        }
    }
    buf_free(&encoded);

    Buf b;
    buf_init(&b);

    char *first = buf_reserve(&b, 9000);
    memset(first, 'A', 9000);
    buf_commit(&b, 9000);
    if (b.len != 9000 || b.data[9000] != '\0' ||
        b.data[0] != 'A' || b.data[8999] != 'A') {
        fprintf(stderr, "buf reserve/commit failed after growth\n");
        buf_free(&b);
        return 1;
    }

    char *tail = buf_reserve(&b, 3);
    memcpy(tail, "XYZ", 3);
    buf_commit(&b, 3);
    if (b.len != 9003 || strcmp(b.data + 9000, "XYZ") != 0) {
        fprintf(stderr, "buf reserve/commit failed while appending\n");
        buf_free(&b);
        return 1;
    }

    buf_free(&b);

    Cache cache;
    cache_init(&cache, "NUL");
    if (cache_contains(&cache, "hot-key")) {
        fprintf(stderr, "empty cache reported a hit\n");
        return 1;
    }
    cache_set(&cache, "hot-key", "hot-value");
    if (!cache_contains(&cache, "hot-key") ||
        cache_contains(&cache, "missing-key") || cache_contains(&cache, NULL)) {
        fprintf(stderr, "cache_contains returned an incorrect membership result\n");
        return 1;
    }

    size_t initial_cap = cache.cap;
    if (initial_cap < 16 || initial_cap > 4096 ||
        (initial_cap & (initial_cap - 1)) != 0) {
        fprintf(stderr, "cache initial capacity is not small and power-of-two: %zu\n",
                initial_cap);
        return 1;
    }
    for (int i = 0; i < 10000; i++) {
        char key[64];
        char value[64];
        snprintf(key, sizeof key, "growth-key-%d", i);
        snprintf(value, sizeof value, "growth-value-%d", i);
        cache_set(&cache, key, value);
    }
    if (cache.cap <= initial_cap || !cache_contains(&cache, "growth-key-0") ||
        !cache_contains(&cache, "growth-key-9999")) {
        fprintf(stderr, "cache growth lost an inserted key\n");
        return 1;
    }

#ifdef DST_TEST_ALLOC_COUNTERS
    const char *batch_keys[48];
    const char *batch_values[48];
    char key_storage[48][64];
    char value_storage[48][64];
    for (int i = 0; i < 48; i++) {
        snprintf(key_storage[i], sizeof key_storage[i], "journal-key-%d", i);
        snprintf(value_storage[i], sizeof value_storage[i], "journal-value-%d", i);
        batch_keys[i] = key_storage[i];
        batch_values[i] = value_storage[i];
    }
    dst_test_alloc_reset();
    CachePersistResult persisted =
        cache_set_many_persist_result(&cache, batch_keys, batch_values, 48);
    size_t allocation_count = dst_test_alloc_count();
    printf("cache-journal-allocations=%zu\n", allocation_count);
    if (persisted.status != CACHE_PERSIST_ALL || persisted.accepted != 48 ||
        persisted.persisted != 48 || persisted.rejected != 0 ||
        !cache_contains(&cache, "journal-key-47")) {
        fprintf(stderr, "48-item cache journal changed persistence semantics\n");
        return 1;
    }
    if (allocation_count > 110) {
        fprintf(stderr, "48-item cache journal used too many allocations: %zu\n",
                allocation_count);
        return 1;
    }
    if (cache.persist_f) {
        fclose((FILE *)cache.persist_f);
        cache.persist_f = NULL;
    }
#endif
    return 0;
}
