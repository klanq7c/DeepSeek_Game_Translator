/* Native HTTP load generator used by tests/test_endurance.ps1.
   Spawns N worker threads, each hitting POST /translate continuously for
   `duration` seconds. Prints request count on exit so the PS harness can read it.
   Build: gcc -O2 bench_client.c -lws2_32 -o bench_client.exe */
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static const char *HOST = "127.0.0.1";
static int g_port = 19999;
static volatile LONG g_total = 0;
static volatile LONG g_stop = 0;
static int g_threads = 4;
static int g_duration = 60;

static int send_one(SOCKET s, const char *body) {
    char req[2048];
    size_t blen = strlen(body);
    int n = snprintf(req, sizeof req,
                     "POST /translate HTTP/1.1\r\nHost: 127.0.0.1\r\n"
                     "Content-Type: application/json\r\nContent-Length: %zu\r\n"
                     "Connection: close\r\n\r\n%s",
                     blen, body);
    if (send(s, req, n, 0) != n) return 0;
    char buf[8192];
    int got = 0, r;
    while ((r = recv(s, buf, sizeof buf, 0)) > 0) got += r;
    return got > 0;
}

static DWORD WINAPI worker(LPVOID arg) {
    int id = (int)(intptr_t)arg;
    char body[128];
    int i = 0;
    while (!InterlockedCompareExchange(&g_stop, 0, 0)) {
        snprintf(body, sizeof body, "{\"text\":\"endur_%d_%d\"}", id, i++);
        SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (s == INVALID_SOCKET) continue;
        struct sockaddr_in a;
        memset(&a, 0, sizeof a);
        a.sin_family = AF_INET;
        a.sin_port = htons((u_short)g_port);
        inet_pton(AF_INET, HOST, &a.sin_addr);
        if (connect(s, (struct sockaddr *)&a, sizeof a) == 0) {
            if (send_one(s, body)) InterlockedIncrement(&g_total);
        }
        closesocket(s);
    }
    return 0;
}

int main(int argc, char **argv) {
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--port") && i + 1 < argc) g_port = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--threads") && i + 1 < argc) g_threads = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--duration") && i + 1 < argc) g_duration = atoi(argv[++i]);
    }
    WSADATA w;
    WSAStartup(MAKEWORD(2, 2), &w);
    HANDLE *th = malloc(g_threads * sizeof(HANDLE));
    for (int i = 0; i < g_threads; i++)
        th[i] = CreateThread(NULL, 0, worker, (LPVOID)(intptr_t)i, 0, NULL);
    Sleep(g_duration * 1000);
    InterlockedExchange(&g_stop, 1);
    WaitForMultipleObjects(g_threads, th, TRUE, 5000);
    printf("%ld\n", g_total);
    fflush(stdout);
    for (int i = 0; i < g_threads; i++) CloseHandle(th[i]);
    free(th);
    WSACleanup();
    return 0;
}
