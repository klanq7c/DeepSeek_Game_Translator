#ifndef DST_LAUNCHER_EMBEDDED_H
#define DST_LAUNCHER_EMBEDDED_H

#include <windows.h>

/*
 * embedded.h —— 启动器内嵌资源（RCDATA）的统一读取入口。
 *
 * build_native.bat 把服务端、示例配置、Unity 插件以及各引擎的运行时脚本
 * （payloads/RenPy、payloads/RPGMaker、payloads/Godot）以 RCDATA 写进启动器。
 * 资源 ID 定义在 resource.h（脚本）与 self_update.c（二进制 payload）。
 */

/* 取得只读资源视图。data 指向模块资源内存，生命周期覆盖整个进程，调用方不得释放。
   资源缺失或为空时返回 0，不记录日志（由调用方决定缺失是否致命）。 */
int embedded_resource_bytes(int id, const unsigned char **data, DWORD *size);

/* 取得以 NUL 结尾的脚本文本副本（RCDATA 原始字节不带终止符）。
   首次访问时复制并缓存，之后返回同一指针，生命周期覆盖整个进程，调用方不得释放。
   资源缺失时返回 NULL 并记录一次日志：这表示启动器构建不完整，部署方必须失败
   而不是写出空脚本。 */
const char *embedded_script_text(int id);

#endif
