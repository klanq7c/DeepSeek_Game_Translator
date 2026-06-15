#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#else
#error Windows-only runtime server.
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "cache.h"
#include "http.h"
#include "util.h"

#define CACHE_DEFAULT "translation_memory_c.tsv"

static Cache G_CACHE;
static volatile LONG G_STOP = 0;
static SOCKET G_SRV = INVALID_SOCKET;

static void die_wsa(SOCKET srv, const char *msg) {
    if (srv != INVALID_SOCKET) closesocket(srv);
    WSACleanup();
    die(msg);
}

int main(int argc, char **argv) {
    const char *cache = CACHE_DEFAULT;
    const char *api_config = NULL;
    int port = HTTP_PORT_DEFAULT;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--port") && i + 1 < argc) port = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--cache") && i + 1 < argc) cache = argv[++i];
        else if (!strcmp(argv[i], "--api-config") && i + 1 < argc) api_config = argv[++i];
    }

    cache_init(&G_CACHE, cache);
    cache_load(&G_CACHE);
    ApiConfig api;
    api_config_init(&api);
    api_config_load(&api, api_config);

    WSADATA w;
    if (WSAStartup(MAKEWORD(2, 2), &w)) die("WSAStartup failed");
    SOCKET srv = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (srv == INVALID_SOCKET) die_wsa(INVALID_SOCKET, "socket failed");
    G_SRV = srv;

    BOOL yes = TRUE;
    setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, (char *)&yes, sizeof yes);

    struct sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port = htons((u_short)port);

    if (bind(srv, (struct sockaddr *)&a, sizeof a) == SOCKET_ERROR) die_wsa(srv, "bind failed");
    if (listen(srv, SOMAXCONN) == SOCKET_ERROR) die_wsa(srv, "listen failed");

    HttpCtx ctx = {
        .cache = &G_CACHE,
        .stop = &G_STOP,
        .server_sock = &G_SRV,
        .api = &api,
        .started = time(NULL),
    };
    http_set_ctx(&ctx);

    fprintf(stderr, "dst_server listening on http://127.0.0.1:%d\n", port);

    while (!InterlockedCompareExchange(&G_STOP, 0, 0)) {
        SOCKET c = accept(srv, NULL, NULL);
        if (c == INVALID_SOCKET) continue;
        HANDLE th = CreateThread(NULL, 64 * 1024, http_serve_thread,
                                 (LPVOID)(uintptr_t)c, STACK_SIZE_PARAM_IS_A_RESERVATION, NULL);
        if (th) CloseHandle(th);
        else closesocket(c);
    }

    SOCKET prev = (SOCKET)(uintptr_t)InterlockedExchangePointer(
        (PVOID volatile *)&G_SRV, (PVOID)(uintptr_t)INVALID_SOCKET);
    if (prev != INVALID_SOCKET) closesocket(prev);
    WSACleanup();
    return 0;
}
