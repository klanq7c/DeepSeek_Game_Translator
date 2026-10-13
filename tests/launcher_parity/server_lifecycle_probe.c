/* 生命周期回归宿主：调用生产服务管理，只将窗口日志送到标准输出。 */
#include "server_proc.h"
#include "ui.h"
#include <stdarg.h>
#include <stdio.h>

void append_log(const WCHAR *fmt, ...) {
    WCHAR line[4096]; char bytes[16384];
    va_list args; va_start(args, fmt);
    _vsnwprintf(line, 4095, fmt, args); va_end(args); line[4095] = 0;
    int n = WideCharToMultiByte(CP_UTF8, 0, line, -1, bytes, sizeof bytes, NULL, NULL);
    if (n > 0) fwrite(bytes, 1, (size_t)n - 1, stdout);
    puts("");
}
void set_status(const WCHAR *text) { append_log(L"status=%s", text); }
void invalidate_control_area(HWND ctl, int pad) { (void)ctl; (void)pad; }
int translation_flow_running(void) { return 0; }

int wmain(int argc, WCHAR **argv) {
    if (argc != 2) return 2;
    wcsncpy(g_root, argv[1], MAX_PATH * 4 - 1);
    refresh_server_status();
    printf("before=%d\n", g_server_started);
    DWORD start = GetTickCount();
    stop_server();
    printf("stop_ms=%lu\nafter=%d\nalive=%d\n", GetTickCount() - start, g_server_started, server_alive());
    refresh_server_status();
    printf("refreshed=%d\n", g_server_started);
    return 0;
}
