#pragma once
#include <windows.h>

/* 仅由 UI 线程调用：先预留操作（包含确认弹窗），再启动或取消。
   工作线程不释放所有权；真实线程完成后由窗口心跳收尾。 */
typedef void (*UiOperationCallback)(void *);
int ui_operation_busy(void);
int ui_operation_reserve(void);
void ui_operation_cancel(void);
int ui_operation_start(UiOperationCallback work, void *context, UiOperationCallback complete);
int ui_operation_poll(void);
int ui_operation_request_close(void);
