/*
 * fsutil.c —— 文件系统与 DPI 工具函数实现（详见 fsutil.h）。
 */
#include "fsutil.h"

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <wchar.h>

/* 拼接两个路径段：a 末尾无 '\' 时自动补上。 */
void path_join(WCHAR *out, size_t cap, const WCHAR *a, const WCHAR *b) {
    _snwprintf(out, cap, L"%s%s%s", a,
               (a[0] && a[wcslen(a) - 1] != L'\\') ? L"\\" : L"", b);
    out[cap - 1] = 0;
}

/* 路径是否存在（文件或目录均可）。 */
int exists_path(const WCHAR *p) {
    DWORD a = GetFileAttributesW(p);
    return a != INVALID_FILE_ATTRIBUTES;
}

/* 是否为目录。 */
int is_dir(const WCHAR *p) {
    DWORD a = GetFileAttributesW(p);
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

/* 递归创建目录：逐级扫描 '\' 分隔符，逐级 CreateDirectory。
   已存在的中间目录跳过，最终 CreateDirectory 失败但 ERROR_ALREADY_EXISTS 也算成功。 */
int ensure_dir(const WCHAR *path) {
    if (is_dir(path)) return 1;
    WCHAR tmp[MAX_PATH * 4];
    size_t len = wcslen(path);
    if (len >= MAX_PATH * 4) return 0;
    memcpy(tmp, path, (len + 1) * sizeof(WCHAR));
    for (WCHAR *p = tmp; *p; p++) {
        if (*p == L'\\') {
            WCHAR old = *p;
            *p = 0;
            if (wcslen(tmp) > 2 && !is_dir(tmp)) CreateDirectoryW(tmp, NULL);
            *p = old;
        }
    }
    return CreateDirectoryW(tmp, NULL) || GetLastError() == ERROR_ALREADY_EXISTS;
}

/* 以 CREATE_ALWAYS 写 UTF-8 文本，返回是否写入完整。 */
int write_text_file_utf8(const WCHAR *path, const char *bytes) {
    HANDLE h = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return 0;
    size_t len = strlen(bytes);
    if (len > UINT32_MAX) {
        CloseHandle(h);
        return 0;
    }
    DWORD written = 0;
    int ok = WriteFile(h, bytes, (DWORD)len, &written, NULL);
    CloseHandle(h);
    return ok && written == (DWORD)len;
}

/* 读取文件全部内容到新分配缓冲。返回 1=成功，*out 和 *size 由调用方负责。
   限制文件大小 < 4GB 且 != UINT32_MAX，超出则拒绝。 */
int read_file_bytes(const WCHAR *path, char **out, DWORD *size) {
    HANDLE h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return 0;
    LARGE_INTEGER li;
    if (!GetFileSizeEx(h, &li) || li.QuadPart < 0 || li.QuadPart > UINT32_MAX - 1) {
        CloseHandle(h);
        return 0;
    }
    DWORD sz = (DWORD)li.QuadPart;
    char *buf = (char *)malloc(sz + 1);
    if (!buf) { CloseHandle(h); return 0; }
    DWORD rd = 0;
    int ok = ReadFile(h, buf, sz, &rd, NULL);
    CloseHandle(h);
    if (!ok) { free(buf); return 0; }
    buf[rd] = 0;  /* 方便当 C 字符串用 */
    *out = buf;
    *size = rd;
    return 1;
}

/* 写入原始字节到文件，CREATE_ALWAYS 覆盖。 */
int write_file_bytes(const WCHAR *path, const char *buf, DWORD size) {
    HANDLE h = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return 0;
    DWORD wr = 0;
    int ok = WriteFile(h, buf, size, &wr, NULL);
    CloseHandle(h);
    return ok && wr == size;
}

/* 复制单个文件。自动确保目标父目录已存在。FALSE = 覆盖已有。 */
int copy_file_safe(const WCHAR *from, const WCHAR *to) {
    WCHAR parent[MAX_PATH * 4];
    size_t len = wcslen(to);
    if (len >= MAX_PATH * 4) return 0;
    memcpy(parent, to, (len + 1) * sizeof(WCHAR));
    WCHAR *slash = wcsrchr(parent, L'\\');
    if (slash) { *slash = 0; ensure_dir(parent); }
    return CopyFileW(from, to, FALSE);
}

/* 递归复制目录树。跳过 . 和 ..，目录递归，文件走 copy_file_safe。 */
int copy_tree_safe(const WCHAR *from, const WCHAR *to) {
    if (!is_dir(from)) return 0;
    if (!ensure_dir(to)) return 0;

    WCHAR pat[MAX_PATH * 4];
    path_join(pat, MAX_PATH * 4, from, L"*");
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) return 0;

    int ok = 1;
    do {
        if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..")) continue;
        WCHAR src[MAX_PATH * 4], dst[MAX_PATH * 4];
        path_join(src, MAX_PATH * 4, from, fd.cFileName);
        path_join(dst, MAX_PATH * 4, to, fd.cFileName);
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            if (!copy_tree_safe(src, dst)) ok = 0;
        } else {
            if (!copy_file_safe(src, dst)) ok = 0;
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return ok;
}

/* 往 ByteBuf 追加 n 字节。含溢出保护和按需 2 倍扩容。 */
void bb_add(ByteBuf *b, const char *s, size_t n) {
    if (!b || !s || !n) return;
    if (b->len > SIZE_MAX - n - 1) return;
    if (!b->data || b->cap == 0) {
        b->cap = 64;
        b->len = 0;
        b->data = (char *)malloc(b->cap);
        if (!b->data) {
            b->cap = 0;
            return;
        }
        b->data[0] = 0;
    }
    if (b->len + n + 1 > b->cap) {
        size_t need = b->len + n + 1;
        size_t new_cap = b->cap;
        while (need > new_cap) {
            if (new_cap > SIZE_MAX / 2) {
                new_cap = need;
                break;
            }
            new_cap *= 2;
        }
        char *p = (char *)realloc(b->data, new_cap);
        if (!p) return;
        b->data = p;
        b->cap = new_cap;
    }
    memcpy(b->data + b->len, s, n);
    b->len += n;
    b->data[b->len] = 0;
}

/* 配置路径派生——全部基于 g_root。 */
void get_config_dir(WCHAR *out, size_t cap) {
    path_join(out, cap, g_root, L"config");
}

void get_api_config_path(WCHAR *out, size_t cap) {
    path_join(out, cap, g_root, L"config\\api.ini");
}

void get_launcher_config_path(WCHAR *out, size_t cap) {
    path_join(out, cap, g_root, L"config\\launcher.ini");
}

/* 把上次用户选择的游戏目录写入 launcher.ini，下次启动自动填充。 */
int save_last_game_dir(const WCHAR *dir) {
    if (!dir || !dir[0] || !is_dir(dir)) return 0;
    WCHAR cfgdir[MAX_PATH * 4], cfg[MAX_PATH * 4];
    get_config_dir(cfgdir, MAX_PATH * 4);
    ensure_dir(cfgdir);
    get_launcher_config_path(cfg, MAX_PATH * 4);
    return WritePrivateProfileStringW(L"launcher", L"last_game_dir", dir, cfg);
}

/* 从 launcher.ini 读取上次目录。校验路径确实存在（可能被用户删了）。 */
int load_last_game_dir(WCHAR *out, size_t cap) {
    if (!out || cap == 0) return 0;
    WCHAR cfg[MAX_PATH * 4];
    get_launcher_config_path(cfg, MAX_PATH * 4);
    out[0] = 0;
    GetPrivateProfileStringW(L"launcher", L"last_game_dir", L"", out, (DWORD)cap, cfg);
    out[cap - 1] = 0;
    return is_dir(out);
}

/* DPI 全局状态：默认 96（100% 缩放）。 */
int g_scale_dpi = 96;

/* 把像素值按当前 DPI 缩放（96 基准）。所有 UI 尺寸必须过 sc()。 */
int sc(int v) {
    return MulDiv(v, g_scale_dpi, 96);
}

/* 声明进程为 DPI 感知。优先用 Win8.1+ 的 Per-Monitor V2，降级到 V1，再降级到
   WinVista 的 System-DPI。不设则系统会虚拟化，导致 UI 模糊。 */
void dpi_enable_awareness(void) {
    HMODULE u = GetModuleHandleW(L"user32.dll");
    if (!u) return;
    typedef BOOL (WINAPI *PFN_SCTX)(HANDLE);
    PFN_SCTX p = (PFN_SCTX)(void *)GetProcAddress(u, "SetProcessDpiAwarenessContext");
    if (p) {
        /* DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2 = -4, V1 = -3 */
        if (p((HANDLE)(LONG_PTR)-4)) return;
        if (p((HANDLE)(LONG_PTR)-3)) return;
    }
    typedef BOOL (WINAPI *PFN_AWARE)(void);
    PFN_AWARE q = (PFN_AWARE)(void *)GetProcAddress(u, "SetProcessDPIAware");
    if (q) q();
}

/* 为指定窗口检测 DPI 并更新全局 g_scale_dpi。优先用 Win10+ 的 GetDpiForWindow，
   降级到 DC 的 GetDeviceCaps。DPI <= 0 时回退到 96。 */
void dpi_set_for_window(HWND hwnd) {
    HMODULE u = GetModuleHandleW(L"user32.dll");
    int dpi = 96;
    if (u) {
        typedef UINT (WINAPI *PFN_GET)(HWND);
        PFN_GET p = (PFN_GET)(void *)GetProcAddress(u, "GetDpiForWindow");
        if (p && hwnd) dpi = (int)p(hwnd);
    }
    if (dpi <= 0) {
        HDC dc = GetDC(hwnd);
        dpi = GetDeviceCaps(dc, LOGPIXELSY);
        ReleaseDC(hwnd, dc);
    }
    if (dpi <= 0) dpi = 96;
    g_scale_dpi = dpi;
}

/* 按 DPI 缩放创建字体。高度取负值表示 pt -> 像素（CreateFontW 约定）。 */
HFONT make_font(int pt, int weight, const WCHAR *face) {
    int h = -MulDiv(pt, g_scale_dpi, 72);
    return CreateFontW(h, 0, 0, 0, weight, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                       OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                       DEFAULT_PITCH | FF_DONTCARE, face);
}
