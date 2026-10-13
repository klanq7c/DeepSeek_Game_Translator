#include <windows.h>
#include <stdio.h>
#include <wchar.h>
#include "globals.h"
#include "fsutil.h"

/* 仅在本探针中模拟完全没有 Windows 字体；不更改系统或游戏。 */
static UINT empty_windows_fonts(LPWSTR out, UINT cap) {
    if (cap) out[0] = 0;
    SetLastError(ERROR_PATH_NOT_FOUND);
    return 0;
}
#define GetWindowsDirectoryW empty_windows_fonts
#include "../../native/src/launcher/deploy.c"

void append_log(const WCHAR *fmt, ...) { (void)fmt; }
int find_exe(const WCHAR *dir, WCHAR *out, size_t cap) { (void)dir; (void)out; (void)cap; return 0; }
int rpgm_content_root(const WCHAR *dir, WCHAR *out, size_t cap) {
    path_join(out, cap, dir, L"www");
    return 1;
}

static int check_file(const WCHAR *dir, const WCHAR *name) {
    WCHAR path[MAX_PATH * 4];
    path_join(path, MAX_PATH * 4, dir, name);
    return exists_path(path);
}

int wmain(int argc, WCHAR **argv) {
    if (argc != 3) return 2;
    g_inst = GetModuleHandleW(NULL);
    wcsncpy(g_root, argv[1], MAX_PATH * 4 - 1);
    g_root[MAX_PATH * 4 - 1] = 0;
    WCHAR game[MAX_PATH * 4], font[MAX_PATH * 4];
    path_join(game, MAX_PATH * 4, argv[2], L"renpy");
    if (!ensure_dir(game)) return 3;
    deploy_renpy_font(game);
    path_join(font, MAX_PATH * 4, game, L"ds_font.otf");
    if (!check_file(game, L"ds_font.otf") || !check_file(game, L"ds_font.otf.OFL.txt") ||
        !check_file(game, L"ds_font.otf.dst-owned")) return 4;
    RestoreStats stats = {0};
    restore_bundled_cjk_font(font, &stats);
    if (stats.failed || stats.preserved || check_file(game, L"ds_font.otf") ||
        check_file(game, L"ds_font.otf.OFL.txt")) return 5;

    path_join(game, MAX_PATH * 4, argv[2], L"rpgm");
    deploy_rpgm_font(game);
    path_join(font, MAX_PATH * 4, game, L"fonts\\ds_font.otf");
    if (!exists_path(font) || !check_file(game, L"fonts\\ds_font.otf.OFL.txt")) return 6;
    stats = (RestoreStats){0};
    restore_bundled_cjk_font(font, &stats);
    if (stats.failed || stats.preserved || exists_path(font)) return 7;

    path_join(game, MAX_PATH * 4, argv[2], L"unity");
    if (!ensure_dir(game) || !deploy_unity_cjk_font_if_needed(game)) return 8;
    path_join(font, MAX_PATH * 4, game, L"TranslationFonts\\NotoSansCJKsc-Regular.otf");
    if (!exists_path(font)) return 9;
    stats = (RestoreStats){0};
    restore_bundled_cjk_font(font, &stats);
    if (stats.failed || stats.preserved || exists_path(font)) return 10;

    path_join(game, MAX_PATH * 4, argv[2], L"godot");
    if (!ensure_dir(game) || !font_bundle_game_license(game) ||
        !check_file(game, L"dst_godot_patch.OFL.txt")) return 19;
    WCHAR font_source[MAX_PATH * 4], license_source[MAX_PATH * 4], game_license[MAX_PATH * 4];
    if (!font_bundle_source(font_source, MAX_PATH * 4, license_source, MAX_PATH * 4)) return 20;
    path_join(game_license, MAX_PATH * 4, game, L"dst_godot_patch.OFL.txt");
    stats = (RestoreStats){0};
    restore_owned_bundled_file(game_license, license_source, FONT_BUNDLE_GODOT_LICENSE_OWNER, &stats);
    if (stats.failed || stats.preserved || exists_path(game_license)) return 21;

    path_join(game, MAX_PATH * 4, argv[2], L"user");
    if (!ensure_dir(game)) return 11;
    path_join(font, MAX_PATH * 4, game, L"ds_font.otf");
    if (!write_file_bytes(font, "USER", 4)) return 12;
    deploy_renpy_font(game);
    char *bytes = NULL; DWORD size = 0;
    if (!read_file_bytes(font, &bytes, &size)) return 13;
    int unchanged = size == 4 && memcmp(bytes, "USER", 4) == 0;
    free(bytes);
    if (!unchanged) return 14;
    stats = (RestoreStats){0};
    restore_bundled_cjk_font(font, &stats);
    if (!exists_path(font) || stats.preserved != 1) return 15;

    path_join(game, MAX_PATH * 4, argv[2], L"modified");
    if (!ensure_dir(game)) return 16;
    deploy_renpy_font(game);
    path_join(font, MAX_PATH * 4, game, L"ds_font.otf");
    if (!write_file_bytes(font, "MOD", 3)) return 17;
    stats = (RestoreStats){0};
    restore_bundled_cjk_font(font, &stats);
    if (!exists_path(font) || !check_file(game, L"ds_font.otf.OFL.txt") || stats.preserved != 1) return 18;
    puts("font bundle probe PASS: no OS fonts, RenPy/RPGM/Unity/Godot OFL, restore, user and modified files");
    return 0;
}
