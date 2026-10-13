#include "font_bundle.h"
#include "embedded.h"
#include "fsutil.h"
#include "globals.h"
#include "resource.h"
#include "ui.h"
#include <stdlib.h>
#include <string.h>

/* 外部边界：系统没有可用 CJK 字体时，原版 Noto 字体和 OFL 从启动器资源释放。
   不能从游戏上游修复系统字体缺失；已有不同字节的目标不会被覆盖。
   失败会记录资源 ID/路径/Win32 错误，并返回失败，绝不伪报字体已安装。 */
static int ensure_asset(const WCHAR *path, int id) {
    const unsigned char *data = NULL;
    DWORD size = 0;
    if (!embedded_resource_bytes(id, &data, &size)) {
        append_log(L"CJK font bundle resource %d is missing or empty.", id);
        return 0;
    }
    if (exists_path(path)) {
        if (path_has_reparse_point(path, 1)) {
            append_log(L"CJK font bundle asset is a reparse point: %s", path);
            return 0;
        }
        char *bytes = NULL;
        DWORD got = 0;
        int matches = read_file_bytes(path, &bytes, &got) && got == size &&
                      memcmp(bytes, data, size) == 0;
        free(bytes);
        if (!matches) append_log(L"CJK font bundle asset differs from embedded bytes: %s", path);
        return matches;
    }
    if (!write_file_bytes_if_absent_safe(path, (const char *)data, size)) {
        append_log(L"CJK font bundle asset could not be created (Windows error %lu): %s", GetLastError(), path);
        return 0;
    }
    return 1;
}

int font_bundle_source(WCHAR *font, size_t font_cap, WCHAR *license, size_t license_cap) {
    if (!font || !license || !g_root[0]) return 0;
    path_join(font, font_cap, g_root, L"assets\\fonts\\NotoSansCJKsc-Regular.otf");
    path_join(license, license_cap, g_root, L"assets\\fonts\\OFL.txt");
    if (!font[0] || !license[0]) return 0;
    /* 授权文件先落盘，避免字体已经可用却漏带 OFL。 */
    return ensure_asset(license, IDR_PAYLOAD_CJK_FONT_LICENSE) &&
           ensure_asset(font, IDR_PAYLOAD_CJK_FONT);
}

/* Godot 外置 PCK/运行时会引用或嵌入备用字体；在游戏目录保留 OFL 副本。
   已有同名用户文件只在字节一致时复用，标记仅认领本次新建文件。 */
int font_bundle_game_license(const WCHAR *game_dir) {
    WCHAR font[MAX_PATH * 4], source[MAX_PATH * 4], dst[MAX_PATH * 4], marker[MAX_PATH * 4];
    if (!game_dir || !font_bundle_source(font, MAX_PATH * 4, source, MAX_PATH * 4)) return 0;
    path_join(dst, MAX_PATH * 4, game_dir, L"dst_godot_patch.OFL.txt");
    if (!dst[0] || !path_append_suffix(marker, MAX_PATH * 4, dst, L".dst-owned")) return 0;
    if (exists_path(dst)) {
        if (files_equal_safe(dst, source)) return 1;
        append_log(L"Godot: existing font license differs; patch build cancelled: %s", dst);
        return 0;
    }
    if (exists_path(marker)) {
        append_log(L"Godot: font license ownership marker already exists; preserved: %s", marker);
        return 0;
    }
    if (!copy_file_if_absent_safe(source, dst)) {
        append_log(L"Godot: could not create bundled font license (Windows error %lu): %s", GetLastError(), dst);
        return 0;
    }
    if (!write_text_file_utf8(marker, FONT_BUNDLE_GODOT_LICENSE_OWNER)) {
        DWORD error = GetLastError();
        append_log(L"Godot: font license was copied but ownership could not be recorded (Windows error %lu): %s", error, dst);
    }
    return 1;
}
