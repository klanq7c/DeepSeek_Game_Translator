/*
 * main.c —— 本地翻译服务器（dst_server）入口。
 *
 * 职责：解析命令行参数 -> 初始化并加载缓存 -> 加载 API 配置 ->
 * 初始化 Winsock -> 绑定并监听 127.0.0.1:port -> 进入 accept 循环，
 * 每个连接派发一个 http_serve_thread 线程处理。
 *
 * 关闭：收到 /shutdown（http.c 置 G_STOP 并把 G_SRV 换成 INVALID_SOCKET）
 * 使本循环退出，随后清理套接字与 Winsock。
 *
 * 命令行：
 *   --port <n>           监听端口（默认 19999）
 *   --cache <path>       缓存 TSV 文件（默认 translation_memory_c.tsv）
 *   --api-config <path>  API 配置 ini 路径
 *
 * 仅 Windows。仅监听回环地址，不对外暴露。
 */
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

static Cache G_CACHE;                 /* 全局唯一缓存实例 */
static volatile LONG G_STOP = 0;      /* 停止标志，跨线程用 Interlocked 访问 */
static SOCKET G_SRV = INVALID_SOCKET; /* 监听套接字；shutdown 时被原子替换为 INVALID_SOCKET */

/* Winsock 初始化失败时的统一退出路径：关闭套接字、清理 Winsock、die。 */
static void die_wsa(SOCKET srv, const char *msg) {
    if (srv != INVALID_SOCKET) closesocket(srv);
    WSACleanup();
    die(msg);
}

int main(int argc, char **argv) {
    /* 解析命令行参数，提供合理默认值。 */
    const char *cache = CACHE_DEFAULT;
    const char *api_config = NULL;
    int port = HTTP_PORT_DEFAULT;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--port") && i + 1 < argc) port = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--cache") && i + 1 < argc) cache = argv[++i];
        else if (!strcmp(argv[i], "--api-config") && i + 1 < argc) api_config = argv[++i];
    }

    /* 先加载缓存（启动期全量读盘），再加载 API 配置。
       api_config_load 找不到文件会返回 0 并保持默认（enabled=0，纯缓存模式）。 */
    cache_init(&G_CACHE, cache);
    cache_load(&G_CACHE);
    ApiConfig api;
    api_config_init(&api);
    api_config_load(&api, api_config);

    /* 初始化 Winsock 2.2。 */
    WSADATA w;
    if (WSAStartup(MAKEWORD(2, 2), &w)) die("WSAStartup failed");
    SOCKET srv = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (srv == INVALID_SOCKET) die_wsa(INVALID_SOCKET, "socket failed");
    G_SRV = srv;

    /* SO_REUSEADDR：重启时避免 TIME_WAIT 占端口。 */
    BOOL yes = TRUE;
    setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, (char *)&yes, sizeof yes);

    /* 仅绑定回环地址：本服务仅供本机翻译钩子访问，不对外。 */
    struct sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port = htons((u_short)port);

    if (bind(srv, (struct sockaddr *)&a, sizeof a) == SOCKET_ERROR) die_wsa(srv, "bind failed");
    if (listen(srv, SOMAXCONN) == SOCKET_ERROR) die_wsa(srv, "listen failed");

    /* 组装并下发 HTTP 上下文，供工作线程访问缓存/停止标志/API。 */
    HttpCtx ctx = {
        .cache = &G_CACHE,
        .stop = &G_STOP,
        .server_sock = &G_SRV,
        .api = &api,
        .started = time(NULL),
    };
    http_set_ctx(&ctx);

    fprintf(stderr, "dst_server listening on http://127.0.0.1:%d\n", port);

    /* accept 循环：每个连接派发独立线程（一连接一线程模型）。
       线程创建失败则关闭该连接，避免句柄泄漏。
       G_STOP 被置位时退出循环。 */
    while (!InterlockedCompareExchange(&G_STOP, 0, 0)) {
        SOCKET c = accept(srv, NULL, NULL);
        if (c == INVALID_SOCKET) continue;
        HANDLE th = CreateThread(NULL, 64 * 1024, http_serve_thread,
                                 (LPVOID)(uintptr_t)c, STACK_SIZE_PARAM_IS_A_RESERVATION, NULL);
        if (th) CloseHandle(th);
        else closesocket(c);
    }

    /* 关停清理：原子取出并关闭监听套接字，清理 Winsock。 */
    SOCKET prev = (SOCKET)(uintptr_t)InterlockedExchangePointer(
        (PVOID volatile *)&G_SRV, (PVOID)(uintptr_t)INVALID_SOCKET);
    if (prev != INVALID_SOCKET) closesocket(prev);
    WSACleanup();
    return 0;
}
