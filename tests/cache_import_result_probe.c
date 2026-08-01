#include "cache.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int check_result(const char *phase, CachePersistResult result,
                        CachePersistStatus status, size_t accepted,
                        size_t persisted, size_t rejected) {
    if (result.status == status && result.accepted == accepted &&
        result.persisted == persisted && result.rejected == rejected) {
        return 1;
    }
    fprintf(stderr,
            "%s: status=%d accepted=%zu persisted=%zu rejected=%zu\n",
            phase, (int)result.status, result.accepted, result.persisted,
            result.rejected);
    return 0;
}

int main(int argc, char **argv) {
    if (argc != 3) {
        fprintf(stderr,
                "usage: cache_import_result_probe <cache-path> <fault-directory>\n");
        return 2;
    }

    Cache cache;
    cache_init(&cache, argv[1]);

    CachePersistResult all =
        cache_set_persist_result(&cache, "durable-key", "durable-value");
    if (!check_result("all", all, CACHE_PERSIST_ALL, 1, 1, 0)) return 1;

    const char *echo_keys[] = {"exact-echo", "cleaned-echo"};
    const char *echo_values[] = {
        "exact-echo",
        "Simplified Chinese translation: cleaned-echo"
    };
    CachePersistResult rejected =
        cache_set_many_persist_result(&cache, echo_keys, echo_values, 2);
    if (!check_result("rejected", rejected, CACHE_PERSIST_ALL, 0, 0, 2)) return 1;
    char *echo = cache_get(&cache, "cleaned-echo");
    if (echo) {
        fprintf(stderr, "cleaned source echo entered memory\n");
        free(echo);
        return 1;
    }

    if (!cache.persist_f) {
        fprintf(stderr, "successful persist did not retain its stream\n");
        return 1;
    }
    fclose((FILE *)cache.persist_f);
    cache.persist_f = NULL;
    snprintf(cache.path, sizeof cache.path, "%s", argv[2]);

    const char *partial_keys[] = {"durable-key", "memory-key"};
    const char *partial_values[] = {"durable-value", "memory-value"};
    CachePersistResult partial =
        cache_set_many_persist_result(&cache, partial_keys, partial_values, 2);
    if (!check_result("partial", partial, CACHE_PERSIST_PARTIAL, 2, 1, 0)) return 1;
    char *memory = cache_get(&cache, "memory-key");
    int memory_ok = memory && strcmp(memory, "memory-value") == 0;
    free(memory);
    if (!memory_ok) {
        fprintf(stderr, "write failure discarded the valid memory entry\n");
        return 1;
    }

    CachePersistResult failed =
        cache_set_persist_result(&cache, "failed-key", "failed-value");
    if (!check_result("failed", failed, CACHE_PERSIST_FAILED, 1, 0, 0)) return 1;

    snprintf(cache.path, sizeof cache.path, "%s", argv[1]);
    CachePersistResult retry =
        cache_set_persist_result(&cache, "failed-key", "failed-value");
    if (!check_result("retry", retry, CACHE_PERSIST_ALL, 1, 1, 0)) return 1;
    if (cache.persist_f) {
        fclose((FILE *)cache.persist_f);
        cache.persist_f = NULL;
    }
    return 0;
}
