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
    int id;
    const WCHAR *relative_path;
    const WCHAR *label;
    int important;
} EmbeddedPayload;

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

static int get_resource_bytes(int id, const unsigned char **data, DWORD *size) {
    HRSRC res = FindResourceW(g_inst, MAKEINTRESOURCEW(id), RT_RCDATA);
    if (!res) return 0;
    DWORD sz = SizeofResource(g_inst, res);
    if (sz == 0) return 0;
    HGLOBAL loaded = LoadResource(g_inst, res);
    if (!loaded) return 0;
    const void *ptr = LockResource(loaded);
    if (!ptr) return 0;
    *data = (const unsigned char *)ptr;
    *size = sz;
    return 1;
}

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

    if (!MoveFileExW(tmp, path, MOVEFILE_REPLACE_EXISTING | MOVEFILE_COPY_ALLOWED | MOVEFILE_WRITE_THROUGH)) {
        DeleteFileW(tmp);
        return 0;
    }
    return 1;
}

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
