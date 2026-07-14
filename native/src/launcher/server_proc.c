/* ================================================================
 * server_proc.c — 本地 C 服务器子进程管理实现
 * ----------------------------------------------------------------
 * 通过 CreateProcess 启动 dst_server.exe，使用 WinHTTP 进行
 * 健康检查（GET /health）和优雅关停（POST /shutdown）。
 * 服务器以隐藏窗口运行，用户通过启动器 UI 控制生命周期。
 * ================================================================ */

#include "server_proc.h"
#include "fsutil.h"
#include "ui.h"

#include <stdio.h>
#include <string.h>
#include <winhttp.h>

#define SERVER_READY_TIMEOUT_MS 15000
#define SERVER_READY_POLL_MS 200

/* 1 when the launcher spawned dst_server.exe itself; 0 when it merely adopted
   a healthy localhost service that was already running. */
static int g_server_owned = 0;

/* ----------------------------------------------------------------
 * server_http_alive — 通过 HTTP 请求检查服务器是否响应
 *
 * 向 127.0.0.1:19999/health 发送 GET 请求，超时由调用者指定。
 * 作为进程句柄检查的补充：进程可能已被外部启动。
 * ---------------------------------------------------------------- */
static int server_http_alive(DWORD timeout) {
    int ok = 0;
    HINTERNET ses = WinHttpOpen(L"ds-game-translator Launcher/3.1",
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

static int wait_for_server_ready(DWORD timeout_ms) {
    DWORD waited = 0;
    for (;;) {
        if (server_http_alive(200)) return 1;
        if (g_server_pi.hProcess &&
            WaitForSingleObject(g_server_pi.hProcess, 0) != WAIT_TIMEOUT) {
            return 0;
        }
        if (waited >= timeout_ms) return 0;
        Sleep(SERVER_READY_POLL_MS);
        waited += SERVER_READY_POLL_MS;
    }
}

/* ----------------------------------------------------------------
 * server_alive — 公开的存活检查
 *
 * 先检查 launcher 自己启动的进程句柄是否仍在运行（WAIT_TIMEOUT），
 * 再回退到 HTTP 检查以覆盖接管的外部服务端。
 * ---------------------------------------------------------------- */
int server_alive(void) {
    if (!g_server_started) return 0;
    if (g_server_pi.hProcess &&
        WaitForSingleObject(g_server_pi.hProcess, 0) == WAIT_TIMEOUT) {
        return 1;
    }
    return server_http_alive(120);
}

/* 关闭服务器进程句柄并重置状态标志 */
static void reset_server_handle(void) {
    if (g_server_pi.hProcess) CloseHandle(g_server_pi.hProcess);
    if (g_server_pi.hThread) CloseHandle(g_server_pi.hThread);
    ZeroMemory(&g_server_pi, sizeof g_server_pi);
    g_server_started = 0;
    g_server_owned = 0;
}

/* 更新 UI 中的服务器状态标签（带双缓冲重绘） */
static void set_server_label(const WCHAR *text) {
    if (g_server && IsWindow(g_server)) {
        invalidate_control_area(g_server, 4);
        SetWindowTextW(g_server, text);
        invalidate_control_area(g_server, 4);
    }
}

/* 更新服务器控制按钮的文本 */
static void set_btn_label(const WCHAR *text) {
    if (g_btn_server && IsWindow(g_btn_server)) SetWindowTextW(g_btn_server, text);
}

/* ----------------------------------------------------------------
 * refresh_server_status — 对齐 UI 与实际服务端状态
 *
 * 不启动新进程；只探测 127.0.0.1:19999/health。如果已有健康服务端，
 * 将其作为外部进程接管显示，避免 launcher 启动后状态卡在“未启动”。
 * ---------------------------------------------------------------- */
void refresh_server_status(void) {
    if (g_server_started && server_alive()) {
        set_server_label(L"\u8FD0\u884C\u4E2D \u00B7 19999");
        set_btn_label(L"\u505C\u6B62\u670D\u52A1\u5668");
        if (g_main && IsWindow(g_main)) InvalidateRect(g_main, NULL, TRUE);
        return;
    }

    if (g_server_started) reset_server_handle();

    if (server_http_alive(200)) {
        g_server_started = 1;
        g_server_owned = 0;
        set_server_label(L"\u8FD0\u884C\u4E2D \u00B7 19999");
        set_btn_label(L"\u505C\u6B62\u670D\u52A1\u5668");
        append_log(L"\u68C0\u6D4B\u5230\u5DF2\u8FD0\u884C\u7684 C \u670D\u52A1\u7AEF\uFF1A127.0.0.1:19999");
    } else {
        set_server_label(L"\u672A\u542F\u52A8");
        set_btn_label(L"\u542F\u52A8\u670D\u52A1\u5668");
    }

    if (g_main && IsWindow(g_main)) InvalidateRect(g_main, NULL, TRUE);
}

/* ----------------------------------------------------------------
 * request_server_shutdown — 通过 HTTP 请求优雅关停服务器
 *
 * 发送 POST /shutdown 请求，让服务器自行清理并退出。
 * 超时设为 500ms，快速失败以避免阻塞 UI。
 * ---------------------------------------------------------------- */
static int request_server_shutdown(void) {
    int ok = 0;
    HINTERNET ses = WinHttpOpen(L"ds-game-translator Launcher/3.1",
                                WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                WINHTTP_NO_PROXY_NAME,
                                WINHTTP_NO_PROXY_BYPASS, 0);
    if (!ses) {
        append_log(L"\u65E0\u6CD5\u521B\u5EFA\u670D\u52A1\u7AEF\u5173\u95ED\u4F1A\u8BDD\u3002WinHTTP \u9519\u8BEF\uFF1A%lu", GetLastError());
        return 0;
    }

    HINTERNET con = WinHttpConnect(ses, L"127.0.0.1", 19999, 0);
    if (!con) {
        append_log(L"\u65E0\u6CD5\u8FDE\u63A5\u672C\u5730\u670D\u52A1\u7AEF\u6267\u884C\u5173\u95ED\u3002WinHTTP \u9519\u8BEF\uFF1A%lu", GetLastError());
        WinHttpCloseHandle(ses);
        return 0;
    }

    HINTERNET req = WinHttpOpenRequest(con, L"POST", L"/shutdown", NULL,
                                       WINHTTP_NO_REFERER,
                                       WINHTTP_DEFAULT_ACCEPT_TYPES, 0);
    if (req) {
        DWORD timeout = 500;
        WinHttpSetTimeouts(req, timeout, timeout, timeout, timeout);
        if (WinHttpSendRequest(req, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                               WINHTTP_NO_REQUEST_DATA, 0, 0, 0)) {
            if (WinHttpReceiveResponse(req, NULL)) {
                DWORD status = 0;
                DWORD status_len = sizeof status;
                if (WinHttpQueryHeaders(req,
                                        WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                                        WINHTTP_HEADER_NAME_BY_INDEX,
                                        &status,
                                        &status_len,
                                        WINHTTP_NO_HEADER_INDEX) &&
                    status >= 200 && status < 300) {
                    ok = 1;
                } else {
                    append_log(L"\u670D\u52A1\u7AEF\u5173\u95ED\u8BF7\u6C42\u8FD4\u56DE\u5F02\u5E38\u72B6\u6001\uFF1A%lu\uFF08WinHTTP \u9519\u8BEF\uFF1A%lu\uFF09",
                               status, GetLastError());
                }
            } else {
                append_log(L"\u63A5\u6536\u670D\u52A1\u7AEF\u5173\u95ED\u54CD\u5E94\u5931\u8D25\u3002WinHTTP \u9519\u8BEF\uFF1A%lu", GetLastError());
            }
        } else {
            append_log(L"\u53D1\u9001\u670D\u52A1\u7AEF\u5173\u95ED\u8BF7\u6C42\u5931\u8D25\u3002WinHTTP \u9519\u8BEF\uFF1A%lu", GetLastError());
        }
        WinHttpCloseHandle(req);
    } else {
        append_log(L"\u65E0\u6CD5\u521B\u5EFA\u670D\u52A1\u7AEF\u5173\u95ED\u8BF7\u6C42\u3002WinHTTP \u9519\u8BEF\uFF1A%lu", GetLastError());
    }

    WinHttpCloseHandle(con);
    WinHttpCloseHandle(ses);
    return ok;
}

/* ----------------------------------------------------------------
 * start_server — 启动本地 C 服务器子进程
 *
 * 流程：
 *   1. 如果已有进程且存活，直接返回
 *   2. 检查端口是否被外部进程占用（HTTP /health 检测）
 *   3. 构造命令行并 CreateProcess 启动 dst_server.exe
 *   4. 轮询 /health，只有 HTTP 200 后才允许部署 hook/启动游戏
 * ---------------------------------------------------------------- */
int start_server(void) {
    if (g_server_started && server_alive()) return 1;
    if (g_server_started) reset_server_handle();

    /* 端口可能被外部进程占用 */
    if (server_http_alive(200)) {
        g_server_started = 1;
        g_server_owned = 0;
        set_server_label(L"\u8FD0\u884C\u4E2D \u00B7 19999");
        set_btn_label(L"\u505C\u6B62\u670D\u52A1\u5668");
        append_log(L"\u68C0\u6D4B\u5230\u5DF2\u8FD0\u884C\u7684 C \u670D\u52A1\u7AEF\uFF1A127.0.0.1:19999");
        if (g_main && IsWindow(g_main)) InvalidateRect(g_main, NULL, TRUE);
        return 1;
    }

    /* 构造命令行：dst_server.exe --port 19999 --cache <tsv> --api-config <ini> */
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

    /* 以隐藏窗口启动服务器进程 */
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
    g_server_owned = 1;
    set_server_label(L"\u6B63\u5728\u542F\u52A8 \u00B7 19999");
    set_btn_label(L"\u505C\u6B62\u670D\u52A1\u5668");
    append_log(L"C \u670D\u52A1\u7AEF\u5DF2\u542F\u52A8\uFF0C\u6B63\u5728\u7B49\u5F85 /health\uFF1A127.0.0.1:19999");
    if (!wait_for_server_ready(SERVER_READY_TIMEOUT_MS)) {
        DWORD exit_code = STILL_ACTIVE;
        if (g_server_pi.hProcess &&
            WaitForSingleObject(g_server_pi.hProcess, 0) != WAIT_TIMEOUT &&
            GetExitCodeProcess(g_server_pi.hProcess, &exit_code)) {
            append_log(L"C \u670D\u52A1\u7AEF\u5728\u5065\u5EB7\u68C0\u67E5\u524D\u9000\u51FA\uFF0C\u9000\u51FA\u7801\uFF1A%lu\u3002", exit_code);
        } else {
            append_log(L"C \u670D\u52A1\u7AEF\u5728 %lu ms \u5185\u672A\u901A\u8FC7 /health \u5065\u5EB7\u68C0\u67E5\u3002",
                       (DWORD)SERVER_READY_TIMEOUT_MS);
        }
        if (g_server_pi.hProcess &&
            WaitForSingleObject(g_server_pi.hProcess, 0) == WAIT_TIMEOUT) {
            (void)request_server_shutdown();
            if (WaitForSingleObject(g_server_pi.hProcess, 1000) == WAIT_TIMEOUT) {
                append_log(L"\u670D\u52A1\u7AEF\u672A\u5728\u4F18\u96C5\u5173\u95ED\u671F\u9650\u5185\u9000\u51FA\uFF0C\u6B63\u5728\u7EC8\u6B62\u542F\u52A8\u5668\u521B\u5EFA\u7684\u8FDB\u7A0B\u3002");
                TerminateProcess(g_server_pi.hProcess, 1);
                WaitForSingleObject(g_server_pi.hProcess, 500);
            }
        }
        reset_server_handle();
        set_server_label(L"\u672A\u542F\u52A8");
        set_btn_label(L"\u542F\u52A8\u670D\u52A1\u5668");
        if (g_main && IsWindow(g_main)) InvalidateRect(g_main, NULL, TRUE);
        return 0;
    }
    set_server_label(L"\u8FD0\u884C\u4E2D \u00B7 19999");
    append_log(L"C \u670D\u52A1\u7AEF\u5065\u5EB7\u68C0\u67E5\u901A\u8FC7\uFF1A127.0.0.1:19999");
    if (g_main && IsWindow(g_main)) InvalidateRect(g_main, NULL, TRUE);
    return 1;
}

/* ----------------------------------------------------------------
 * stop_server — 停止本地 C 服务器
 *
 * 先通过 HTTP /shutdown 优雅关停，等待最多 1500ms；
 * 超时则 TerminateProcess 强制终止。最后重置所有状态和 UI。
 * ---------------------------------------------------------------- */
void stop_server(void) {
    int owned = g_server_owned;
    if (!g_server_started && !server_http_alive(200)) {
        set_server_label(L"\u672A\u542F\u52A8");
        set_btn_label(L"\u542F\u52A8\u670D\u52A1\u5668");
        return;
    }
    if (server_alive() || server_http_alive(200)) {
        (void)request_server_shutdown();
        if (g_server_pi.hProcess &&
            WaitForSingleObject(g_server_pi.hProcess, 1500) == WAIT_TIMEOUT) {
            append_log(L"\u670D\u52A1\u7AEF\u672A\u5728\u4F18\u96C5\u5173\u95ED\u671F\u9650\u5185\u9000\u51FA\uFF0C\u6B63\u5728\u7EC8\u6B62\u542F\u52A8\u5668\u521B\u5EFA\u7684\u8FDB\u7A0B\u3002");
            TerminateProcess(g_server_pi.hProcess, 1);
            WaitForSingleObject(g_server_pi.hProcess, 500);
        }
    }
    reset_server_handle();
    set_server_label(L"\u5DF2\u505C\u6B62");
    set_btn_label(L"\u542F\u52A8\u670D\u52A1\u5668");
    append_log(owned ? L"C \u670D\u52A1\u7AEF\u5DF2\u505C\u6B62\u3002" :
                       L"\u5DF2\u5411\u5916\u90E8 C \u670D\u52A1\u7AEF\u53D1\u9001\u505C\u6B62\u8BF7\u6C42\u3002");
    if (g_main && IsWindow(g_main)) InvalidateRect(g_main, NULL, TRUE);
}

/* ----------------------------------------------------------------
 * toggle_server — 切换服务器运行状态
 *
 * 服务器按钮的回调：运行中则停止，否则启动。
 * ---------------------------------------------------------------- */
void toggle_server(void) {
    if (g_server_started && server_alive()) {
        stop_server();
        if (g_server_started && server_alive()) {
            set_status(L"\u72B6\u6001\uFF1A\u670D\u52A1\u5668\u4ECD\u7531\u5916\u90E8\u8FDB\u7A0B\u8FD0\u884C");
        } else {
            set_status(L"\u670D\u52A1\u5668\u5DF2\u505C\u6B62");
        }
        return;
    }
    set_status(L"\u6B63\u5728\u542F\u52A8\u670D\u52A1\u5668...");
    if (start_server()) set_status(L"\u72B6\u6001\uFF1A\u670D\u52A1\u5668\u8FD0\u884C\u4E2D");
    else set_status(L"\u72B6\u6001\uFF1A\u670D\u52A1\u5668\u542F\u52A8\u5931\u8D25");
}
