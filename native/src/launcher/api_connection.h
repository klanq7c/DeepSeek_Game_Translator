#pragma once
#include <windows.h>

/* 配置草稿的一次短聊天请求；不依赖本地翻译服务，不读写配置或缓存。
   实现在 api_models.c 中复用有界 JSON 索引；响应正文与 Key 不进入结果。 */
typedef struct { int success; WCHAR message[192]; } ApiConnection;
int api_connection_wait_ms(const WCHAR *endpoint);
void api_connection_test(const WCHAR *endpoint, const WCHAR *model, const WCHAR *key, ApiConnection *result);
