#include "server_proc.h"
#include "fsutil.h"
#include "ui.h"

#include <stdio.h>
#include <string.h>
#include <winhttp.h>

static int server_http_alive(DWORD timeout) {
    int ok = 0;
    HINTERNET ses = WinHttpOpen(L"DeepSeek Game Translator Launcher/3.1",
                                WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                WINHTTP_NO_PROXY_NAME,
                                WINHTTP_NO_PROXY_BYPASS, 0);
    if (!ses) return 0;

    HINTERNET con = WinHttpConnect(ses, L"127.0.0.1", 19999, 0);
    if (con) {
        HINTERNET req = WinHttpOpenRequest(con, L"GET", L"/health", NULL,
                                           WINHTTP_NO_REFERER,
                                           WINHTTP_DEFAULT_ACCEPT_TYPES, 0);
        if (req) {
            DWORD status = 0;
            DWORD status_len = sizeof status;
            WinHttpSetTimeouts(req, timeout, timeout, timeout, timeout);
            if (WinHttpSendRequest(req, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                                   WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
                WinHttpReceiveResponse(req, NULL) &&
                WinHttpQueryHeaders(req,
                                    WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                                    WINHTTP_HEADER_NAME_BY_INDEX,
                                    &status,
                                    &status_len,
                                    WINHTTP_NO_HEADER_INDEX)) {
                ok = (status == 200);
            }
            WinHttpCloseHandle(req);
        }
        WinHttpCloseHandle(con);
    }
    WinHttpCloseHandle(ses);
    return ok;
}

int server_alive(void) {
    if (!g_server_started) return 0;
    if (g_server_pi.hProcess &&
        WaitForSingleObject(g_server_pi.hProcess, 0) == WAIT_TIMEOUT) {
        return 1;
    }
    return server_http_alive(120);
}

static void reset_server_handle(void) {
    if (g_server_pi.hProcess) CloseHandle(g_server_pi.hProcess);
    if (g_server_pi.hThread) CloseHandle(g_server_pi.hThread);
    ZeroMemory(&g_server_pi, sizeof g_server_pi);
    g_server_started = 0;
}

static void set_server_label(const WCHAR *text) {
    if (g_server && IsWindow(g_server)) {
        invalidate_control_area(g_server, 4);
        SetWindowTextW(g_server, text);
        invalidate_control_area(g_server, 4);
    }
}

static void set_btn_label(const WCHAR *text) {
    if (g_btn_server && IsWindow(g_btn_server)) SetWindowTextW(g_btn_server, text);
}

static void request_server_shutdown(void) {
    HINTERNET ses = WinHttpOpen(L"DeepSeek Game Translator Launcher/3.1",
                                WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                WINHTTP_NO_PROXY_NAME,
                                WINHTTP_NO_PROXY_BYPASS, 0);
    if (!ses) return;

    HINTERNET con = WinHttpConnect(ses, L"127.0.0.1", 19999, 0);
    if (!con) {
        WinHttpCloseHandle(ses);
        return;
    }

    HINTERNET req = WinHttpOpenRequest(con, L"POST", L"/shutdown", NULL,
                                       WINHTTP_NO_REFERER,
                                       WINHTTP_DEFAULT_ACCEPT_TYPES, 0);
    if (req) {
        DWORD timeout = 500;
        WinHttpSetTimeouts(req, timeout, timeout, timeout, timeout);
        if (WinHttpSendRequest(req, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                               WINHTTP_NO_REQUEST_DATA, 0, 0, 0)) {
            WinHttpReceiveResponse(req, NULL);
        }
        WinHttpCloseHandle(req);
    }

    WinHttpCloseHandle(con);
    WinHttpCloseHandle(ses);
}

int start_server(void) {
    if (g_server_started && server_alive()) return 1;
    if (g_server_started) reset_server_handle();

    if (server_http_alive(200)) {
        g_server_started = 1;
        set_server_label(L"\u8FD0\u884C\u4E2D \u00B7 19999");
        set_btn_label(L"\u505C\u6B62\u670D\u52A1\u5668");
        append_log(L"\u68C0\u6D4B\u5230\u5DF2\u8FD0\u884C\u7684 C \u670D\u52A1\u7AEF\uFF1A127.0.0.1:19999");
        if (g_main && IsWindow(g_main)) InvalidateRect(g_main, NULL, TRUE);
        return 1;
    }

    WCHAR exe[MAX_PATH * 4], cache[MAX_PATH * 4], api_cfg[MAX_PATH * 4], cmd[MAX_PATH * 12];
    path_join(exe, MAX_PATH * 4, g_root, L"native\\dst_server.exe");
    path_join(cache, MAX_PATH * 4, g_root, L"translation_memory_c.tsv");
    get_api_config_path(api_cfg, MAX_PATH * 4);
    if (!exists_path(exe)) {
        append_log(L"\u627E\u4E0D\u5230 C \u670D\u52A1\u7AEF\uFF1A%s", exe);
        return 0;
    }
    _snwprintf(cmd, MAX_PATH * 12,
               L"\"%s\" --port 19999 --cache \"%s\" --api-config \"%s\"",
               exe, cache, api_cfg);
    cmd[MAX_PATH * 12 - 1] = 0;

    STARTUPINFOW si;
    ZeroMemory(&si, sizeof si);
    ZeroMemory(&g_server_pi, sizeof g_server_pi);
    si.cb = sizeof si;
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    if (!CreateProcessW(NULL, cmd, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL,
                        g_root, &si, &g_server_pi)) {
        append_log(L"\u670D\u52A1\u7AEF\u542F\u52A8\u5931\u8D25\uFF0C\u53EF\u80FD 19999 \u5DF2\u88AB\u5360\u7528\u3002\u9519\u8BEF\uFF1A%lu", GetLastError());
        return 0;
    }
    g_server_started = 1;
    set_server_label(L"\u8FD0\u884C\u4E2D \u00B7 19999");
    set_btn_label(L"\u505C\u6B62\u670D\u52A1\u5668");
    append_log(L"C \u670D\u52A1\u7AEF\u5DF2\u542F\u52A8\uFF1A127.0.0.1:19999");
    Sleep(600);
    if (g_main && IsWindow(g_main)) InvalidateRect(g_main, NULL, TRUE);
    return 1;
}

void stop_server(void) {
    if (!g_server_started && !server_http_alive(200)) {
        set_server_label(L"\u672A\u542F\u52A8");
        set_btn_label(L"\u542F\u52A8\u670D\u52A1\u5668");
        return;
    }
    if (server_alive() || server_http_alive(200)) {
        request_server_shutdown();
        if (g_server_pi.hProcess &&
            WaitForSingleObject(g_server_pi.hProcess, 1500) == WAIT_TIMEOUT) {
            TerminateProcess(g_server_pi.hProcess, 1);
            WaitForSingleObject(g_server_pi.hProcess, 500);
        } else {
            Sleep(500);
        }
    }
    reset_server_handle();
    set_server_label(L"\u5DF2\u505C\u6B62");
    set_btn_label(L"\u542F\u52A8\u670D\u52A1\u5668");
    append_log(L"C \u670D\u52A1\u7AEF\u5DF2\u505C\u6B62\u3002");
    if (g_main && IsWindow(g_main)) InvalidateRect(g_main, NULL, TRUE);
}

void toggle_server(void) {
    if (g_server_started && server_alive()) {
        stop_server();
        set_status(L"\u670D\u52A1\u5668\u5DF2\u505C\u6B62");
        return;
    }
    set_status(L"\u6B63\u5728\u542F\u52A8\u670D\u52A1\u5668...");
    if (start_server()) set_status(L"\u72B6\u6001\uFF1A\u670D\u52A1\u5668\u8FD0\u884C\u4E2D");
    else set_status(L"\u72B6\u6001\uFF1A\u670D\u52A1\u5668\u542F\u52A8\u5931\u8D25");
}
