/* ================================================================
 * godot_preflight_cache.c — Godot headless 预检结论缓存实现（详见头文件）
 * ----------------------------------------------------------------
 * 存储为 config\godot_preflight.ini，节名为"类别 + 全键哈希"的十六进制，
 * 节内保存完整签名串用于防碰撞校验。读取与写入都走 Windows 标准
 * Profile API，与启动器其余配置文件保持一致。
 * ================================================================ */

#include "godot_preflight_cache.h"
#include "fsutil.h"

#include <stdio.h>
#include <string.h>

#ifndef DS_TRANSLATOR_VERSION
#define DS_TRANSLATOR_VERSION "unknown"
#endif

/* FNV-1a：先哈希翻译器版本（ANSI），再哈希类别与签名串（宽字符）。
   翻译器升级会使全部缓存条目自然失效。 */
static unsigned long long godot_preflight_hash(const WCHAR *kind, const WCHAR *sig) {
    static const char version[] = DS_TRANSLATOR_VERSION;
    unsigned long long h = 1469598103934665603ULL;
    for (const char *p = version; *p; p++) {
        h ^= (unsigned char)*p;
        h *= 1099511628211ULL;
    }
    h ^= 0x1f;
    h *= 1099511628211ULL;
    for (const WCHAR *p = kind; *p; p++) {
        h ^= (unsigned long long)*p;
        h *= 1099511628211ULL;
    }
    h ^= 0x2f;
    h *= 1099511628211ULL;
    for (const WCHAR *p = sig; *p; p++) {
        h ^= (unsigned long long)*p;
        h *= 1099511628211ULL;
    }
    return h;
}

static void godot_preflight_cache_path(WCHAR *out, size_t cap) {
    WCHAR cfgdir[MAX_PATH * 4];
    get_config_dir(cfgdir, MAX_PATH * 4);
    path_join(out, cap, cfgdir, L"godot_preflight.ini");
}

void godot_preflight_sig_init(WCHAR *out, size_t cap) {
    if (!out || cap == 0) return;
    out[0] = 0;
}

static void godot_preflight_sig_append(WCHAR *out, size_t cap, const WCHAR *text) {
    if (!out || cap == 0 || !text) return;
    size_t len = wcslen(out);
    if (len + 1 >= cap) return;
    size_t avail = cap - len - 1;
    size_t tl = wcslen(text);
    if (tl > avail) tl = avail;
    memcpy(out + len, text, tl * sizeof(WCHAR));
    out[len + tl] = 0;
}

void godot_preflight_sig_add_text(WCHAR *out, size_t cap, const WCHAR *text) {
    godot_preflight_sig_append(out, cap, L"|");
    godot_preflight_sig_append(out, cap, text ? text : L"");
}

void godot_preflight_sig_add_file(WCHAR *out, size_t cap, const WCHAR *path) {
    godot_preflight_sig_append(out, cap, L"|");
    godot_preflight_sig_append(out, cap, path ? path : L"");

    /* 文件特征 = 最后修改时间 + 大小；不存在时特征为 0。 */
    WIN32_FILE_ATTRIBUTE_DATA fad;
    memset(&fad, 0, sizeof fad);
    if (!GetFileAttributesExW(path ? path : L"", GetFileExInfoStandard, &fad)) {
        godot_preflight_sig_append(out, cap, L"|0|0|");
        return;
    }
    unsigned long long mtime =
        ((unsigned long long)fad.ftLastWriteTime.dwHighDateTime << 32) |
        fad.ftLastWriteTime.dwLowDateTime;
    unsigned long long size =
        ((unsigned long long)fad.nFileSizeHigh << 32) | fad.nFileSizeLow;
    WCHAR nums[48];
    swprintf(nums, sizeof nums / sizeof nums[0], L"|%016llx|%llu|", mtime, size);
    godot_preflight_sig_append(out, cap, nums);
}

int godot_preflight_cache_get(const WCHAR *kind, const WCHAR *sig) {
    if (!kind || !sig) return -1;
    WCHAR path[MAX_PATH * 4];
    godot_preflight_cache_path(path, MAX_PATH * 4);

    WCHAR section[40];
    swprintf(section, sizeof section / sizeof section[0], L"p_%016llx",
             godot_preflight_hash(kind, sig));

    WCHAR stored_sig[MAX_PATH * 8];
    GetPrivateProfileStringW(section, L"sig", L"", stored_sig,
                             MAX_PATH * 8, path);
    if (stored_sig[0] == 0 || wcscmp(stored_sig, sig) != 0) return -1;

    WCHAR stored_result[16];
    GetPrivateProfileStringW(section, L"result", L"", stored_result,
                             sizeof stored_result / sizeof stored_result[0], path);
    if (_wcsicmp(stored_result, L"ok") == 0) return 1;
    if (_wcsicmp(stored_result, L"reject") == 0) return 0;
    return -1;
}

void godot_preflight_cache_put(const WCHAR *kind, const WCHAR *sig, int result) {
    if (!kind || !sig) return;
    WCHAR cfgdir[MAX_PATH * 4];
    get_config_dir(cfgdir, MAX_PATH * 4);
    ensure_dir(cfgdir);

    WCHAR path[MAX_PATH * 4];
    godot_preflight_cache_path(path, MAX_PATH * 4);

    WCHAR section[40];
    swprintf(section, sizeof section / sizeof section[0], L"p_%016llx",
             godot_preflight_hash(kind, sig));

    WCHAR result_text[16];
    swprintf(result_text, sizeof result_text / sizeof result_text[0],
             L"%ls", result ? L"ok" : L"reject");
    WritePrivateProfileStringW(section, L"sig", sig, path);
    WritePrivateProfileStringW(section, L"result", result_text, path);
}
