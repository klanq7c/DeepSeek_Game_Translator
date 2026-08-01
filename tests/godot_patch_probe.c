#include "../native/src/launcher/deploy.h"
#include "../native/src/launcher/engine.h"
#include "../native/src/launcher/fsutil.h"
#include "../native/src/launcher/godot_patch.h"

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
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

static void put_u64le(unsigned char *p, unsigned long long value) {
    put_u32le(p, (unsigned int)(value & 0xffffffffu));
    put_u32le(p + 4, (unsigned int)(value >> 32));
}

static uint32_t get_u32le(const unsigned char *p) {
    return ((uint32_t)p[0]) |
           ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static uint64_t get_u64le(const unsigned char *p) {
    return (uint64_t)get_u32le(p) | ((uint64_t)get_u32le(p + 4) << 32);
}

static int bytes_contain(const unsigned char *haystack, size_t haystack_size,
                         const char *needle) {
    size_t needle_size = strlen(needle);
    if (!needle_size || needle_size > haystack_size) return 0;
    for (size_t i = 0; i <= haystack_size - needle_size; i++) {
        if (!memcmp(haystack + i, needle, needle_size)) return 1;
    }
    return 0;
}

static int write_pck_header(const WCHAR *path, unsigned int format) {
    unsigned char header[232] = {0};
    put_u32le(header, 0x43504447u);
    put_u32le(header + 4, format);
    put_u32le(header + 8, format == 1 ? 3u : 4u);
    if (format == 1) {
        put_u32le(header + 84, 1u);
        return write_file_bytes(path, (const char *)header, 88);
    }
    if (format == 3) {
        put_u32le(header + 12, 6u);
        put_u32le(header + 16, 2u);
        put_u64le(header + 24, 112u);
        put_u64le(header + 32, 120u);
        memcpy(header + 112, "ECFG", 4);
        put_u32le(header + 116, 0u);
        put_u32le(header + 120, 2u);
        put_u32le(header + 124, 12u);
        memcpy(header + 128, "dummy.txt", 9);
        put_u32le(header + 176, 16u);
        memcpy(header + 180, "project.binary", 15);
        put_u64le(header + 196, 0u);
        put_u64le(header + 204, 8u);
        return write_file_bytes(path, (const char *)header, 232);
    }
    put_u32le(header + 96, 1u);
    return write_file_bytes(path, (const char *)header, 100);
}

static int pck3_entry_contains(const WCHAR *path, const char *entry_name, const char *needle) {
    char *buf = NULL;
    DWORD size = 0;
    if (!read_file_bytes(path, &buf, &size) || size < 116) {
        free(buf);
        return 0;
    }
    const unsigned char *p = (const unsigned char *)buf;
    uint64_t file_base = get_u64le(p + 24);
    uint64_t directory = get_u64le(p + 32);
    if (get_u32le(p) != 0x43504447u || get_u32le(p + 4) != 3u ||
        directory > size - 4u) {
        free(buf);
        return 0;
    }
    uint64_t pos = directory;
    uint32_t count = get_u32le(p + pos);
    pos += 4;
    int found = 0;
    for (uint32_t i = 0; i < count && pos <= size - 4u; i++) {
        uint32_t path_len = get_u32le(p + pos);
        pos += 4;
        if (!path_len || path_len > size - pos || pos + path_len > size - 36u) break;
        const char *name = (const char *)p + pos;
        pos += path_len;
        uint64_t rel = get_u64le(p + pos);
        uint64_t entry_size = get_u64le(p + pos + 8);
        pos += 36;
        if (!strcmp(name, entry_name) && file_base <= size && rel <= size - file_base) {
            uint64_t abs = file_base + rel;
            if (entry_size <= size - abs && entry_size <= SIZE_MAX - 1u) {
                char *entry = (char *)malloc((size_t)entry_size + 1u);
                if (entry) {
                    memcpy(entry, p + abs, (size_t)entry_size);
                    entry[entry_size] = 0;
                    found = bytes_contain((const unsigned char *)entry,
                                          (size_t)entry_size, needle);
                    free(entry);
                }
            }
            break;
        }
    }
    free(buf);
    return found;
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
    const WCHAR *generated[] = {
        L"dst_godot_patch.pck", L"dst_godot_patch.next.pck", L"dst_godot_patch.building",
        L"dst_godot_patch.exe", L"dst_godot_patch.exe.dst-owned", L"game.exe"
    };
    for (size_t i = 0; i < sizeof(generated) / sizeof(generated[0]); i++) {
        path_join(path, MAX_PATH * 4, dir, generated[i]);
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
    if (argc == 3 && !wcscmp(argv[1], L"--patch")) {
        WCHAR patch[MAX_PATH * 4];
        if (!godot_prepare_patch_pack(argv[2], patch, MAX_PATH * 4)) return 1;
        wprintf(L"%s\n", patch);
        return 0;
    }
    if (argc == 3 && !wcscmp(argv[1], L"--launcher")) {
        WCHAR launcher[MAX_PATH * 4];
        if (!godot_prepare_patch_launcher(argv[2], launcher, MAX_PATH * 4)) return 1;
        wprintf(L"%s\n", launcher);
        return 0;
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
        !generated_sidecar_contains(godot3, "--dst-preflight", NULL) ||
        !generated_sidecar_contains(godot3, "batch-request", NULL) ||
        !generated_sidecar_contains(godot3, "_dst_backoff_batch(batch)", NULL) ||
        !generated_sidecar_contains(godot3, "_dst_queue_head", "_dst_queue.pop_front()") ||
        !generated_sidecar_contains(godot3, "batch.append(_dst_queue[_dst_queue_head])", NULL)) {
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
        !generated_sidecar_contains(godot4, "_dst_apply(node, \"text\")", "bbcode_text") ||
        !generated_sidecar_contains(godot4, "const DST_MAX_TRACKED_CONTROLS = 4096", NULL) ||
        !generated_sidecar_contains(godot4, "node_added.connect", NULL) ||
        !generated_sidecar_contains(godot4, "req.process_mode = Node.PROCESS_MODE_ALWAYS", "if self is Node") ||
        !generated_sidecar_contains(godot4, "timer.process_mode = Node.PROCESS_MODE_ALWAYS", "self.set(\"process_mode\"") ||
        !generated_sidecar_contains(godot4, "_dst_track_control", NULL) ||
        !generated_sidecar_contains(godot4, "_dst_scan_controls()", NULL) ||
        !generated_sidecar_contains(godot4, "_dst_control_cursor", NULL) ||
        !generated_sidecar_contains(godot4, "while scanned < count", NULL) ||
        !generated_sidecar_contains(godot4, "--dst-preflight", NULL) ||
        !generated_sidecar_contains(godot4, "unknown-request-callback", NULL) ||
        !generated_sidecar_contains(godot4, "batch-json-schema", NULL) ||
        !generated_sidecar_contains(godot4, "_dst_queue_head", "_dst_queue.pop_front()") ||
        !generated_sidecar_contains(godot4, "batch.append(_dst_queue[_dst_queue_head])", NULL) ||
        !prepare_existing_readonly_sidecar(godot4)) {
        cleanup_dir(godot3, L"game.pck", 0);
        cleanup_dir(godot4, L"game.pck", 0);
        cleanup_dir(loose4, NULL, 1);
        return fail("Godot 4 PCK should receive and reuse the Godot 4 runtime sidecar");
    }

    if (!write_pck_header(path, 3) || !godot_prepare_runtime_sidecar(godot4) ||
        !generated_sidecar_contains(godot4, "change_scene_to_file(scene)", "DynamicFontData.new") ||
        !generated_sidecar_contains(godot4, "FontFile.new", NULL)) {
        cleanup_dir(godot3, L"game.pck", 0);
        cleanup_dir(godot4, L"game.pck", 0);
        cleanup_dir(loose4, NULL, 1);
        return fail("Godot 4 PCK format 3 should receive the Godot 4 runtime sidecar");
    }

    WCHAR patch_pack[MAX_PATH * 4], stale_next[MAX_PATH * 4];
    path_join(stale_next, MAX_PATH * 4, godot4, L"dst_godot_patch.next.pck");
    if (!write_file_bytes(stale_next, "stale", 5) ||
        !godot_prepare_patch_pack(godot4, patch_pack, MAX_PATH * 4)) {
        cleanup_dir(godot3, L"game.pck", 0);
        cleanup_dir(godot4, L"game.pck", 0);
        cleanup_dir(loose4, NULL, 1);
        return fail("Godot 4 PCK format 3 patch should be generated");
    }
    if (exists_path(stale_next)) {
        cleanup_dir(godot3, L"game.pck", 0);
        cleanup_dir(godot4, L"game.pck", 0);
        cleanup_dir(loose4, NULL, 1);
        return fail("a successfully installed Godot patch must remove a stale staged pack");
    }
    const char *runtime_failure = NULL;
    if (!godot_patch_pack_has_runtime_autoload(patch_pack)) {
        runtime_failure = "Godot 4 PCK format 3 patch should expose its runtime autoload";
    } else if (!pck3_entry_contains(patch_pack, "res://dst_godot_runtime.gd", "FontFile.new")) {
        runtime_failure = "Godot 4 PCK format 3 patch should embed the SceneTree runtime sidecar";
    } else if (!pck3_entry_contains(patch_pack, "res://dst_godot_autoload.gd", "extends Node")) {
        runtime_failure = "Godot 4 PCK format 3 patch should embed the Node runtime autoload";
    } else if (!pck3_entry_contains(patch_pack, "res://override.cfg", "[autoload_prepend]") ||
               !pck3_entry_contains(patch_pack, "res://override.cfg", "DeepSeekTranslator")) {
        runtime_failure = "Godot 4.6 PCK format 3 override.cfg should prepend the runtime autoload";
    }
    if (runtime_failure) {
        cleanup_dir(godot3, L"game.pck", 0);
        cleanup_dir(godot4, L"game.pck", 0);
        cleanup_dir(loose4, NULL, 1);
        return fail(runtime_failure);
    }

    WCHAR game_exe[MAX_PATH * 4], patch_exe[MAX_PATH * 4], selected_exe[MAX_PATH * 4];
    path_join(game_exe, MAX_PATH * 4, godot4, L"game.exe");
    if (!write_file_bytes(game_exe, "game", 4) ||
        !godot_prepare_patch_launcher(godot4, patch_exe, MAX_PATH * 4) ||
        !exists_path(patch_exe) ||
        !find_exe(godot4, selected_exe, MAX_PATH * 4) ||
        _wcsicmp(selected_exe, game_exe)) {
        cleanup_dir(godot3, L"game.pck", 0);
        cleanup_dir(godot4, L"game.pck", 0);
        cleanup_dir(loose4, NULL, 1);
        return fail("Godot patch launcher must be owned and excluded from normal executable selection");
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
