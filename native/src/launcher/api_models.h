#pragma once
#include <windows.h>

/* 模型目录属于配置窗口；不修改服务器配置，也不调用翻译接口。 */
typedef struct {
    WCHAR (*items)[256];
    int count;
    WCHAR message[192];
} ApiModels;
int api_models_url(const WCHAR *endpoint, WCHAR *url, size_t cap);
int api_models_fetch(const WCHAR *endpoint, const WCHAR *key, ApiModels *result);
void api_models_free(ApiModels *result);
