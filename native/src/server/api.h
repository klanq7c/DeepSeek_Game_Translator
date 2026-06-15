#pragma once

#include <stddef.h>

typedef struct {
    int enabled;
    int timeout_ms;
    int concurrency;
    char endpoint[1024];
    char model[256];
    char key[1024];
} ApiConfig;

#define API_CONCURRENCY_MAX 8

void api_config_init(ApiConfig *cfg);
int api_config_load(ApiConfig *cfg, const char *path);
int api_translate(ApiConfig *cfg, const char *text, char **out);
int api_translate_batch(ApiConfig *cfg, char **texts, size_t count, char ***out);
