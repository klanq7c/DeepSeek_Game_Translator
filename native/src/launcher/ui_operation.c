#include "ui_operation.h"
#include "ui.h"

/* HWND 在操作完成前始终归 UI 线程持有；不在工作线程投递裸窗口回调。
   完成弹窗期间仍保留预留，避免嵌套消息循环开始另一次生命周期操作。 */
static int busy, closing;
static HANDLE thread;
static UiOperationCallback operation_work, operation_complete;
static void *operation_context;

int ui_operation_busy(void) { return busy; }

int ui_operation_reserve(void) {
    if (busy || closing) {
        append_log(L"操作已忽略：启动器操作仍在进行或窗口正在关闭。");
        set_status(L"操作仍在进行，请稍候。");
        return 0;
    }
    busy = 1;
    return 1;
}

void ui_operation_cancel(void) { busy = 0; }

static DWORD WINAPI operation_thread(void *unused) {
    (void)unused;
    operation_work(operation_context);
    return 0;
}

#ifdef DST_UI_OPERATION_TEST
HANDLE ui_operation_create_thread(LPTHREAD_START_ROUTINE work, void *arg);
#else
static HANDLE ui_operation_create_thread(LPTHREAD_START_ROUTINE work, void *arg) {
    return CreateThread(NULL, 0, work, arg, 0, NULL);
}
#endif

int ui_operation_start(UiOperationCallback work, void *context, UiOperationCallback complete) {
    if (closing) { ui_operation_cancel(); return 0; }
    operation_work = work;
    operation_context = context;
    operation_complete = complete;
    thread = ui_operation_create_thread(operation_thread, NULL);
    if (thread) return 1;
    DWORD error = GetLastError();
    append_log(L"后台操作创建线程失败（Win32=%lu），未执行操作。", error);
    set_status(L"无法创建后台操作，请查看日志后重试。");
    operation_work = operation_complete = NULL;
    operation_context = NULL;
    ui_operation_cancel();
    return 0;
}

int ui_operation_poll(void) {
    if (thread) {
        DWORD state = WaitForSingleObject(thread, 0);
        if (state == WAIT_OBJECT_0) {
            CloseHandle(thread);
            thread = NULL;
            UiOperationCallback complete = operation_complete;
            void *context = operation_context;
            operation_work = operation_complete = NULL;
            operation_context = NULL;
            if (!closing && complete) complete(context);
            busy = 0;
        } else if (state == WAIT_FAILED) {
            /* 自有句柄等待失败不能伪装成任务完成；保留窗口和所有权并限频诊断。 */
            static int wait_error_reported;
            if (!wait_error_reported) {
                wait_error_reported = 1;
                append_log(L"后台操作完成检查失败（Win32=%lu），窗口保留等待。", GetLastError());
            }
        }
    }
    return closing && !busy;
}

int ui_operation_request_close(void) {
    if (!closing && busy) {
        append_log(L"窗口将在当前操作完成后关闭。");
        set_status(L"正在等待当前操作完成后关闭...");
    }
    closing = 1;
    return !busy;
}
