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
        cache_set_persist(&cache, "overwrite-key", "first-value");
        cache_set_persist(&cache, "overwrite-key", "second-value");
        return 0;
    }
    if (strcmp(argv[1], "read") == 0) {
        cache_load(&cache);
        char *value = cache_get(&cache, "overwrite-key");
        int ok = value && strcmp(value, "second-value") == 0;
        if (!ok) fprintf(stderr, "expected second-value, got %s\n", value ? value : "(null)");
        free(value);
        return ok ? 0 : 1;
    }
    return 2;
}
