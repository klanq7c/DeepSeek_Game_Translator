/*
 * embedded.c —— 启动器内嵌资源（RCDATA）的统一读取实现。
 *
 * 引擎运行时脚本原先以 C 字符串常量写在 deploy.c / godot_patch.c 里，既没有
 * 语法检查也无法单独测试；现在它们是 payloads/ 下的真实脚本文件，由
 * build_native.bat 嵌入。这里只负责把资源字节交给部署方，不做任何内容改写。
 */
#include "embedded.h"

#include "globals.h"
#include "ui.h"

#include <stdlib.h>
#include <string.h>

int embedded_resource_bytes(int id, const unsigned char **data, DWORD *size) {
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

/* 脚本文本缓存：条目数上限对应 resource.h 里的脚本资源数量；缺失的 ID 也占一条，
   保证"构建不完整"只记录一次日志而不是每次部署都刷屏。 */
#define EMBEDDED_SCRIPT_CACHE_MAX 8

typedef struct {
    int id;
    char *text;      /* NULL 表示已确认缺失 */
} EmbeddedScript;

static SRWLOCK g_embedded_lock = SRWLOCK_INIT;
static EmbeddedScript g_embedded_scripts[EMBEDDED_SCRIPT_CACHE_MAX];
static size_t g_embedded_count;

const char *embedded_script_text(int id) {
    AcquireSRWLockExclusive(&g_embedded_lock);
    for (size_t i = 0; i < g_embedded_count; i++) {
        if (g_embedded_scripts[i].id == id) {
            const char *text = g_embedded_scripts[i].text;
            ReleaseSRWLockExclusive(&g_embedded_lock);
            return text;
        }
    }

    const unsigned char *data = NULL;
    DWORD size = 0;
    char *copy = NULL;
    int cacheable = 1;
    if (embedded_resource_bytes(id, &data, &size)) {
        copy = (char *)malloc((size_t)size + 1);
        if (copy) {
            memcpy(copy, data, size);
            copy[size] = 0;
            if (strlen(copy) != size) {
                /* 脚本内含 NUL 会让下游按 C 字符串写文件时截断：视为构建产物损坏。 */
                append_log(L"Embedded script resource %d contains a NUL byte; refusing to deploy a truncated script.", id);
                free(copy);
                copy = NULL;
            }
        } else {
            /* 内存不足是瞬时状态，不缓存结论，下次调用重试。 */
            append_log(L"Embedded script resource %d could not be copied (out of memory).", id);
            cacheable = 0;
        }
    } else {
        append_log(L"Embedded script resource %d is missing: the launcher was built without payloads/ scripts.", id);
    }

    if (cacheable && g_embedded_count < EMBEDDED_SCRIPT_CACHE_MAX) {
        g_embedded_scripts[g_embedded_count].id = id;
        g_embedded_scripts[g_embedded_count].text = copy;
        g_embedded_count++;
    }
    ReleaseSRWLockExclusive(&g_embedded_lock);
    return copy;
}
