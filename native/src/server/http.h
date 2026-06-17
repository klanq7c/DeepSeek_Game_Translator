/*
 * http.h —— 内嵌 HTTP/1.x 服务器（Winsock）。
 *
 * 负责：监听本地端口、接收请求、分发到 api.c 的路由、拼装响应。
 * 这是游戏侧与本地服务器之间的契约边界，请求/响应字段和路由必须保持稳定
 * （AGENTS.md：不得随意改 public API）。
 *
 * 运行模型：每个连接一个线程，请求体上限 HTTP_MAX_REQ 防止恶意/异常大包。
 * 默认端口 19999，仅监听本地，供翻译钩子访问。
 */
#pragma once

#include <winsock2.h>
#include <time.h>

#include "api.h"
#include "cache.h"

#define HTTP_PORT_DEFAULT 19999                /* 默认监听端口 */
#define HTTP_RECV_INITIAL (64 * 1024)          /* 首次接收缓冲大小 */
#define HTTP_MAX_REQ (2 * 1024 * 1024)         /* 单请求体上限 2MB，防止异常连接耗尽内存 */
#define HTTP_RECV_TIMEOUT_MS 5000              /* 单次 recv 超时，避免慢连接占线程 */

/* 贯穿连接处理全程的上下文：缓存、停止标志、监听套接字、API 配置、启动时刻。 */
typedef struct {
    Cache *cache;
    volatile LONG *stop;
    SOCKET *server_sock;
    ApiConfig *api;
    time_t started;
} HttpCtx;

void http_set_ctx(HttpCtx *ctx);               /* 设置线程级上下文，供工作线程取用 */
DWORD WINAPI http_serve_thread(LPVOID arg);    /* 连接处理线程入口，arg 为 SOCKET* */
