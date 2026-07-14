#include "../native/src/launcher/deploy.h"
#include "../native/src/launcher/engine.h"
#include "../native/src/launcher/fsutil.h"
#include "../native/src/launcher/godot_patch.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

WCHAR g_root[MAX_PATH * 4];
WCHAR g_game[MAX_PATH * 4];

void append_log(const WCHAR *fmt, ...) {
    (void)fmt;
}

static int fail(const char *message) {
    fprintf(stderr, "%s\n", message);
    return 1;
}

static void put_u32le(unsigned char *p, unsigned int value) {
    p[0] = (unsigned char)(value & 0xffu);
    p[1] = (unsigned char)((value >> 8) & 0xffu);
    p[2] = (unsigned char)((value >> 16) & 0xffu);
    p[3] = (unsigned char)((value >> 24) & 0xffu);
}

static int write_pck_header(const WCHAR *path, unsigned int format) {
    unsigned char header[100] = {0};
    put_u32le(header, 0x43504447u);
    put_u32le(header + 4, format);
    if (format == 1) {
        put_u32le(header + 84, 1u);
        return write_file_bytes(path, (const char *)header, 88);
    }
    put_u32le(header + 96, 1u);
    return write_file_bytes(path, (const char *)header, 100);
}

static int generated_sidecar_contains(const WCHAR *dir, const char *must_have, const char *must_not_have) {
    WCHAR path[MAX_PATH * 4];
    path_join(path, MAX_PATH * 4, dir, L"dst_godot_runtime.gd");
    char *buf = NULL;
    DWORD size = 0;
    int ok = read_file_bytes(path, &buf, &size) &&
             strstr(buf, must_have) != NULL &&
             (!must_not_have || strstr(buf, must_not_have) == NULL);
    free(buf);
    return ok;
}

static int prepare_existing_readonly_sidecar(const WCHAR *dir) {
    WCHAR path[MAX_PATH * 4];
    path_join(path, MAX_PATH * 4, dir, L"dst_godot_runtime.gd");
    DWORD attrs = GetFileAttributesW(path);
    if (attrs == INVALID_FILE_ATTRIBUTES) return 0;
    if (!SetFileAttributesW(path, attrs | FILE_ATTRIBUTE_READONLY)) return 0;
    int ok = godot_prepare_runtime_sidecar(dir);
    SetFileAttributesW(path, attrs & ~FILE_ATTRIBUTE_READONLY);
    return ok;
}

static void cleanup_dir(const WCHAR *dir, const WCHAR *pck_name, int has_project) {
    WCHAR path[MAX_PATH * 4];
    path_join(path, MAX_PATH * 4, dir, L"dst_godot_runtime.gd");
    DeleteFileW(path);
    if (pck_name) {
        path_join(path, MAX_PATH * 4, dir, pck_name);
        DeleteFileW(path);
    }
    if (has_project) {
        path_join(path, MAX_PATH * 4, dir, L"project.godot");
        DeleteFileW(path);
    }
    RemoveDirectoryW(dir);
}

static void cleanup_multi_pck_dir(const WCHAR *dir) {
    WCHAR path[MAX_PATH * 4];
    const WCHAR *files[] = {
        L"dst_godot_runtime.gd", L"aaa.pck", L"game.pck", L"game.exe"
    };
    for (size_t i = 0; i < sizeof(files) / sizeof(files[0]); i++) {
        path_join(path, MAX_PATH * 4, dir, files[i]);
        DeleteFileW(path);
    }
    RemoveDirectoryW(dir);
}

int wmain(int argc, WCHAR **argv) {
    if (argc == 3 && !wcscmp(argv[1], L"--prepare")) {
        return godot_prepare_runtime_sidecar(argv[2]) ? 0 : 1;
    }
    if (argc != 2) return fail("usage: godot_patch_probe <fixture-root>");
    WCHAR godot3[MAX_PATH * 4], godot4[MAX_PATH * 4], loose4[MAX_PATH * 4], multi4[MAX_PATH * 4], path[MAX_PATH * 4];
    path_join(godot3, MAX_PATH * 4, argv[1], L"godot3");
    path_join(godot4, MAX_PATH * 4, argv[1], L"godot4");
    path_join(loose4, MAX_PATH * 4, argv[1], L"loose4");
    path_join(multi4, MAX_PATH * 4, argv[1], L"multi4");
    if (!ensure_dir(godot3) || !ensure_dir(godot4) || !ensure_dir(loose4)) {
        return fail("failed to create Godot patch probe fixtures");
    }

    path_join(path, MAX_PATH * 4, godot3, L"game.pck");
    if (!write_pck_header(path, 1) || !godot_prepare_runtime_sidecar(godot3) ||
        !generated_sidecar_contains(godot3, "DynamicFontData.new", "FontFile.new") ||
        !generated_sidecar_contains(godot3, "_dst_report_error", NULL) ||
        !generated_sidecar_contains(godot3, "push_warning", NULL) ||
        !generated_sidecar_contains(godot3, "batch-request", NULL) ||
        !generated_sidecar_contains(godot3, "_dst_backoff_batch(batch)", NULL)) {
        cleanup_dir(godot3, L"game.pck", 0);
        cleanup_dir(godot4, L"game.pck", 0);
        cleanup_dir(loose4, NULL, 1);
        return fail("Godot 3 PCK should receive the Godot 3 runtime sidecar");
    }

    path_join(path, MAX_PATH * 4, godot4, L"game.pck");
    if (!write_pck_header(path, 2) || !godot_prepare_runtime_sidecar(godot4) ||
        !generated_sidecar_contains(godot4, "change_scene_to_file(scene)", "DynamicFontData.new") ||
        !generated_sidecar_contains(godot4, "FontFile.new", NULL) ||
        !generated_sidecar_contains(godot4, "_dst_report_error", NULL) ||
        !generated_sidecar_contains(godot4, "push_warning", NULL) ||
        !generated_sidecar_contains(godot4, "unknown-request-callback", NULL) ||
        !generated_sidecar_contains(godot4, "batch-json-schema", NULL) ||
        !prepare_existing_readonly_sidecar(godot4)) {
        cleanup_dir(godot3, L"game.pck", 0);
        cleanup_dir(godot4, L"game.pck", 0);
        cleanup_dir(loose4, NULL, 1);
        return fail("Godot 4 PCK should receive and reuse the Godot 4 runtime sidecar");
    }

    path_join(path, MAX_PATH * 4, loose4, L"project.godot");
    if (!write_file_bytes(path, "config_version=5\n[application]\n", 31) ||
        !godot_prepare_runtime_sidecar(loose4) ||
        !generated_sidecar_contains(loose4, "FileAccess.file_exists", "DynamicFontData.new")) {
        cleanup_dir(godot3, L"game.pck", 0);
        cleanup_dir(godot4, L"game.pck", 0);
        cleanup_dir(loose4, NULL, 1);
        return fail("Godot 4 loose project should receive the Godot 4 runtime sidecar");
    }

    if (!ensure_dir(multi4)) {
        cleanup_dir(godot3, L"game.pck", 0);
        cleanup_dir(godot4, L"game.pck", 0);
        cleanup_dir(loose4, NULL, 1);
        return fail("failed to create multi-PCK Godot fixture");
    }
    path_join(path, MAX_PATH * 4, multi4, L"aaa.pck");
    if (!write_file_bytes(path, "not-a-godot-pack", 16)) {
        cleanup_multi_pck_dir(multi4);
        return fail("failed to write invalid leading PCK fixture");
    }
    path_join(path, MAX_PATH * 4, multi4, L"game.pck");
    if (!write_pck_header(path, 2)) {
        cleanup_multi_pck_dir(multi4);
        return fail("failed to write preferred Godot PCK fixture");
    }
    path_join(path, MAX_PATH * 4, multi4, L"game.exe");
    if (!write_file_bytes(path, "game", 4) ||
        !godot_prepare_runtime_sidecar(multi4) ||
        !generated_sidecar_contains(multi4, "FontFile.new", "DynamicFontData.new")) {
        cleanup_dir(godot3, L"game.pck", 0);
        cleanup_dir(godot4, L"game.pck", 0);
        cleanup_dir(loose4, NULL, 1);
        cleanup_multi_pck_dir(multi4);
        return fail("Godot sidecar selection must ignore invalid PCKs and prefer the executable-matched pack");
    }

    cleanup_dir(godot3, L"game.pck", 0);
    cleanup_dir(godot4, L"game.pck", 0);
    cleanup_dir(loose4, NULL, 1);
    cleanup_multi_pck_dir(multi4);
    puts("Godot patch probe passed");
    return 0;
}
