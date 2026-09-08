/*
 * api.h —— 远程翻译 API（OpenAI 兼容，默认 DeepSeek）客户端配置与调用声明。
 *
 * 这是本地服务器与远程大模型之间的边界。缓存未命中时由 http.c 的 worker
 * 调用这里发起 HTTP 请求。失败时返回 0，调用方据此决定降级（逐条重试 /
 * 透传原文），绝不把失败结果当作翻译写入缓存。
 */
#pragma once

#include <stddef.h>

#define API_CONCURRENCY_MAX 8   /* 并发通道上限，防止过载远程与本地资源 */
#define API_GLOSSARY_BYTES 8192 /* 术语表提示词段缓冲上限 */

/* API 调用所需的全部配置，由 api.ini 加载。固定大小数组避免动态分配。 */
typedef struct {
    int enabled;              /* 是否启用远程 API（0=纯缓存模式） */
    int timeout_ms;           /* 单次请求超时 */
    int concurrency;          /* 并发通道数，决定 worker 池大小（<=API_CONCURRENCY_MAX） */
    char endpoint[1024];      /* API 端点 URL */
    char model[256];          /* 模型名 */
    char key[1024];           /* API Key（本机回环端点可留空） */
    char glossary[API_GLOSSARY_BYTES]; /* 术语表提示词段（启动时从 glossary.tsv 构建，空=未启用） */
} ApiConfig;

void api_config_init(ApiConfig *cfg);                                    /* 置为安全默认值 */
int api_config_load(ApiConfig *cfg, const char *path);                   /* 从 ini 文件加载，成功返回 1 */
void api_glossary_load_file(ApiConfig *cfg, const char *path);           /* 从 TSV 术语表构建提示词段（文件不存在则保持为空） */
int api_translate(ApiConfig *cfg, const char *text, char **out);         /* 翻译单条，成功返回 1，*out 为新分配结果 */
int api_translate_batch(ApiConfig *cfg, char **texts, size_t count, char ***out); /* 批量翻译，成功返回 1，*out 为结果数组（每元素新分配） */
/* 带上下文的批量翻译：prevs[i] 为 texts[i] 在原作中的上一行（可为空串）。
   仅预热路径使用——把相邻台词一起呈现给模型，使其能结合语境处理代词、
   语气与命名一致性。响应解析与 api_translate_batch 完全相同。 */
int api_translate_batch_ctx(ApiConfig *cfg, char **texts, const char *const *prevs,
                            size_t count, char ***out);
/* 整批失败后的二分重试：对半递归定位最小失败单元（≤4 条时逐条兜底），
   返回 count 长度数组，成功槽位为新分配译文、失败槽位为 NULL。
   调用方以 NULL 槽位计 miss，绝不把原文回显当翻译。 */
char **api_translate_split_retry(ApiConfig *cfg, char **texts,
                                 const char *const *prevs, size_t count);
