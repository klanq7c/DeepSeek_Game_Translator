/*
 * self_update.c —— 启动器内嵌自有 payload 的同步实现。
 *
 * 真实入口：wWinMain 在创建主窗口前调用 sync_embedded_payloads()。
 * build_native.bat 把本项目自有的服务端、配置示例、安装脚本和 Unity
 * 插件作为 RCDATA 写进启动器；这里负责把它们同步到 g_root 下的固定位置。
 *
 * 边界约束：
 *   - 只同步本项目自有文件，BepInEx、XUnity、Unity 程序集等第三方运行时
 *     仍由 install_runtime_payloads.ps1 安装；
 *   - 不触碰真实 api.ini、翻译记忆、日志或游戏目录；
 *   - 目标内容相同就不重写，避免无意义地改变时间戳；
 *   - 更新采用同目录临时文件 + 原子替换，失败时保留原文件。
 */
#include "self_update.h"

#include "fsutil.h"
#include "globals.h"
#include "ui.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

#define IDR_PAYLOAD_DST_SERVER          101
#define IDR_PAYLOAD_INSTALLER           102
#define IDR_PAYLOAD_API_EXAMPLE         103
#define IDR_PAYLOAD_LAUNCHER_EXAMPLE    104
#define IDR_PAYLOAD_UNITY_MONO5         201
#define IDR_PAYLOAD_UNITY_MONO6         202
#define IDR_PAYLOAD_XUNITY_ENDPOINT     203
#define IDR_PAYLOAD_TMP_FALLBACK        204

typedef struct {
    int id;                       /* 启动器资源表中的 RCDATA ID */
    const WCHAR *relative_path;   /* 相对 g_root 的释放位置 */
    const WCHAR *label;           /* 仅用于日志，不参与路径计算 */
    int important;                /* 缺失时是否报告“构建不完整” */
} EmbeddedPayload;

/* 资源 ID 必须与 build_native.bat 生成的 launcher_payloads.rc 保持一致。
   Unity DLL 是可选资源：缺少本地编译引用时仍允许构建启动器。 */
static const EmbeddedPayload EMBEDDED_PAYLOADS[] = {
    { IDR_PAYLOAD_DST_SERVER,       L"native\\dst_server.exe", L"dst_server.exe", 1 },
    { IDR_PAYLOAD_INSTALLER,        L"scripts\\install_runtime_payloads.ps1", L"install_runtime_payloads.ps1", 1 },
    { IDR_PAYLOAD_API_EXAMPLE,      L"config\\api.ini.example", L"api.ini.example", 1 },
    { IDR_PAYLOAD_LAUNCHER_EXAMPLE, L"config\\launcher.ini.example", L"launcher.ini.example", 1 },
    { IDR_PAYLOAD_UNITY_MONO5,      L"payloads\\UnityTranslator\\UnityTranslator.dll", L"UnityTranslator.dll", 0 },
    { IDR_PAYLOAD_UNITY_MONO6,      L"payloads\\UnityTranslator\\UnityTranslator.BepInEx6.dll", L"UnityTranslator.BepInEx6.dll", 0 },
    { IDR_PAYLOAD_XUNITY_ENDPOINT,  L"payloads\\UnityIL2CPP\\DeepSeekXUnityTranslator\\DeepSeekTranslate.dll", L"DeepSeekTranslate.dll", 0 },
    { IDR_PAYLOAD_TMP_FALLBACK,     L"payloads\\UnityIL2CPP\\DeepSeekTMPFontFallback\\BepInEx\\plugins\\DeepSeekTMPFontFallback\\DeepSeekTMPFontFallback.dll", L"DeepSeekTMPFontFallback.dll", 0 },
};

/* 取得编译进启动器的只读资源视图。data 不转移所有权，调用方不得释放；
   资源内存由 Windows 模块加载器持有，直到进程退出。 */
static int get_resource_bytes(int id, const unsigned char **data, DWORD *size) {
    HRSRC res = FindResourceW(g_inst, MAKEINTRESOURCEW(id), RT_RCDATA);
    if (!res) return 0;
    DWORD sz = SizeofResource(g_inst, res);
    if (sz == 0) return 0;
    HGLOBAL loaded = LoadResource(g_inst, res);
    if (!loaded) return 0;
    const void *ptr = LockResource(loaded);
    if (!ptr) return 0;
    /* LockResource 返回的内存归模块资源所有，生命周期覆盖整个进程；
       调用方只能读取，不能 free。 */
    *data = (const unsigned char *)ptr;
    *size = sz;
    return 1;
}

/* 为目标文件创建父目录；path 本身仍视为文件路径，不会创建最后一段。 */
static int ensure_parent_dir(const WCHAR *path) {
    WCHAR parent[MAX_PATH * 4];
    size_t len = wcslen(path);
    if (len >= MAX_PATH * 4) return 0;
    memcpy(parent, path, (len + 1) * sizeof(WCHAR));
    WCHAR *slash = wcsrchr(parent, L'\\');
    if (!slash) return 1;
    *slash = 0;
    return ensure_dir(parent);
}

/* 流式比较磁盘文件与嵌入资源，避免为大型 payload 再分配一份完整副本。 */
static int file_matches_bytes(const WCHAR *path, const unsigned char *data, DWORD size) {
    HANDLE h = CreateFileW(path, GENERIC_READ,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return 0;

    LARGE_INTEGER li;
    if (!GetFileSizeEx(h, &li) || li.QuadPart < 0 || li.QuadPart != (LONGLONG)size) {
        CloseHandle(h);
        return 0;
    }

    unsigned char buf[64 * 1024];
    DWORD offset = 0;
    int ok = 1;
    while (offset < size) {
        DWORD want = size - offset;
        if (want > sizeof(buf)) want = sizeof(buf);
        DWORD got = 0;
        if (!ReadFile(h, buf, want, &got, NULL) || got != want ||
            memcmp(buf, data + offset, want) != 0) {
            ok = 0;
            break;
        }
        offset += got;
    }
    CloseHandle(h);
    return ok;
}

/* 在目标同目录写临时文件后替换正式文件。失败时删除临时文件并保留原文件，
   防止启动器中断或磁盘写入失败留下半个可执行文件。 */
static int write_bytes_atomic(const WCHAR *path, const unsigned char *data, DWORD size) {
    if (!ensure_parent_dir(path)) return 0;

    WCHAR tmp[MAX_PATH * 4];
    _snwprintf(tmp, MAX_PATH * 4, L"%s.dstmp", path);
    tmp[MAX_PATH * 4 - 1] = 0;

    HANDLE h = CreateFileW(tmp, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return 0;

    DWORD written_total = 0;
    int ok = 1;
    while (written_total < size) {
        DWORD want = size - written_total;
        if (want > 1024 * 1024) want = 1024 * 1024;
        DWORD written = 0;
        if (!WriteFile(h, data + written_total, want, &written, NULL) || written != want) {
            ok = 0;
            break;
        }
        written_total += written;
    }
    CloseHandle(h);

    if (!ok) {
        DeleteFileW(tmp);
        return 0;
    }

    /* 临时文件和目标位于同一目录，正常情况下是原子替换；COPY_ALLOWED
       只为异常卷布局兜底，WRITE_THROUGH 尽量保证返回前内容已落盘。 */
    if (!MoveFileExW(tmp, path, MOVEFILE_REPLACE_EXISTING | MOVEFILE_COPY_ALLOWED | MOVEFILE_WRITE_THROUGH)) {
        DeleteFileW(tmp);
        return 0;
    }
    return 1;
}

/* 把单文件启动器中的运行时 payload 同步到工作目录。
   important 资源缺失会被计入失败；可选的引擎 payload 缺失则保持兼容降级。 */
void sync_embedded_payloads(void) {
    int updated = 0;
    int failed = 0;
    int missing_important = 0;

    for (size_t i = 0; i < sizeof(EMBEDDED_PAYLOADS) / sizeof(EMBEDDED_PAYLOADS[0]); i++) {
        const EmbeddedPayload *p = &EMBEDDED_PAYLOADS[i];
        const unsigned char *data = NULL;
        DWORD size = 0;
        if (!get_resource_bytes(p->id, &data, &size)) {
            if (p->important) missing_important++;
            continue;
        }

        WCHAR dst[MAX_PATH * 4];
        path_join(dst, MAX_PATH * 4, g_root, p->relative_path);
        if (file_matches_bytes(dst, data, size)) continue;

        if (write_bytes_atomic(dst, data, size)) {
            updated++;
        } else {
            failed++;
            append_log(L"Built-in component update failed: %s", p->label);
        }
    }

    if (updated > 0) {
        append_log(L"Built-in components synced: %d file(s).", updated);
    }
    if (missing_important > 0) {
        append_log(L"Warning: launcher was built without %d required embedded component(s).", missing_important);
    }
    if (failed > 0) {
        append_log(L"Warning: %d built-in component(s) could not be updated. Close running games/server and restart the launcher.", failed);
    }
}
