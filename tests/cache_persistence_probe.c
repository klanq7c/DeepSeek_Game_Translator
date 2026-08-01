#include "cache.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv) {
    if (argc != 3) {
        fprintf(stderr, "usage: cache_persistence_probe <write|read> <cache-path>\n");
        return 2;
    }

    Cache cache;
    cache_init(&cache, argv[2]);
    if (strcmp(argv[1], "write") == 0) {
        const char *keys[] = {"batch-key-a", "batch-key-b", "batch-key-c"};
        const char *values[] = {"batch-value-a", "batch-value-b", "batch-value-c"};
        cache_set_many_persist(&cache, keys, values, 3);
        cache_set_persist(&cache, "overwrite-key", "first-value");
        cache_set_persist(&cache, "overwrite-key", "second-value");
        return 0;
    }
    if (strcmp(argv[1], "read") == 0) {
        cache_load(&cache);
        char *value = cache_get(&cache, "overwrite-key");
        char *batch_a = cache_get(&cache, "batch-key-a");
        char *batch_b = cache_get(&cache, "batch-key-b");
        char *batch_c = cache_get(&cache, "batch-key-c");
        int ok = value && strcmp(value, "second-value") == 0 &&
                 batch_a && strcmp(batch_a, "batch-value-a") == 0 &&
                 batch_b && strcmp(batch_b, "batch-value-b") == 0 &&
                 batch_c && strcmp(batch_c, "batch-value-c") == 0;
        if (!ok) fprintf(stderr, "expected second-value, got %s\n", value ? value : "(null)");
        free(value);
        free(batch_a);
        free(batch_b);
        free(batch_c);
        return ok ? 0 : 1;
    }
    return 2;
}
