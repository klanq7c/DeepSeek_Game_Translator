#pragma once

#include <winsock2.h>
#include <time.h>

#include "api.h"
#include "cache.h"

#define HTTP_PORT_DEFAULT 19999
#define HTTP_RECV_INITIAL (64 * 1024)
#define HTTP_MAX_REQ (2 * 1024 * 1024)
#define HTTP_RECV_TIMEOUT_MS 5000

typedef struct {
    Cache *cache;
    volatile LONG *stop;
    SOCKET *server_sock;
    ApiConfig *api;
    time_t started;
} HttpCtx;

void http_set_ctx(HttpCtx *ctx);
DWORD WINAPI http_serve_thread(LPVOID arg);
