/*
 * api.h —— 远程翻译 API（DeepSeek）客户端配置与调用声明。
 *
 * 这是本地服务器与远程大模型之间的边界。缓存未命中时由 http.c 的 worker
 * 调用这里发起 HTTP 请求。失败时返回 0，调用方据此决定降级（逐条重试 /
 * 透传原文），绝不把失败结果当作翻译写入缓存。
 */
#pragma once

#include <stddef.h>

/* API 调用所需的全部配置，由 api.ini 加载。固定大小数组避免动态分配。 */
typedef struct {
    int enabled;              /* 是否启用远程 API（0=纯缓存模式） */
    int timeout_ms;           /* 单次请求超时 */
    int concurrency;          /* 并发通道数，决定 worker 池大小（<=API_CONCURRENCY_MAX） */
    char endpoint[1024];      /* API 端点 URL */
    char model[256];          /* 模型名 */
    char key[1024];           /* API Key */
} ApiConfig;

#define API_CONCURRENCY_MAX 8   /* 并发通道上限，防止过载远程与本地资源 */

void api_config_init(ApiConfig *cfg);                                    /* 置为安全默认值 */
int api_config_load(ApiConfig *cfg, const char *path);                   /* 从 ini 文件加载，成功返回 1 */
int api_translate(ApiConfig *cfg, const char *text, char **out);         /* 翻译单条，成功返回 1，*out 为新分配结果 */
int api_translate_batch(ApiConfig *cfg, char **texts, size_t count, char ***out); /* 批量翻译，成功返回 1，*out 为结果数组（每元素新分配） */
