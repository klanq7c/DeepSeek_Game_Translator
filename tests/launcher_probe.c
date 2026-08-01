#include "deploy.h"
#include "engine.h"
#include "fsutil.h"

#include <stdarg.h>
#include <stdio.h>
#include <wchar.h>

WCHAR g_root[MAX_PATH * 4];
WCHAR g_game[MAX_PATH * 4];

void append_log(const WCHAR *fmt, ...) {
    (void)fmt;
}

static int fail(const WCHAR *msg) {
    fwprintf(stderr, L"%ls\n", msg);
    return 1;
}

static int expect_engine(const WCHAR *dir, Engine want, const WCHAR *label) {
    Engine got = detect_engine(dir);
    if (got != want) {
        fwprintf(stderr, L"%ls: expected %ls, got %ls\n", label, engine_name(want), engine_name(got));
        return 1;
    }
    return 0;
}

static void join(WCHAR *out, const WCHAR *base, const WCHAR *leaf) {
    path_join(out, MAX_PATH * 4, base, leaf);
}

static int probe_pe_machine(const WCHAR *path) {
    FILE *f = _wfopen(path, L"rb");
    if (!f) return 0;
    unsigned char header[512];
    size_t size = fread(header, 1, sizeof(header), f);
    fclose(f);
    if (size < 0x40 || header[0] != 'M' || header[1] != 'Z') return 0;
    unsigned int pe = *(unsigned int *)(header + 0x3c);
    if (pe >= size || size - pe <= 6 || memcmp(header + pe, "PE\0\0", 4)) return 0;
    return *(unsigned short *)(header + pe + 4);
}

static int file_contains_ascii(const WCHAR *path, const char *needle) {
    char *bytes = NULL;
    DWORD size = 0;
    if (!read_file_bytes(path, &bytes, &size)) return 0;
    size_t needle_len = strlen(needle);
    int found = 0;
    for (DWORD i = 0; needle_len <= size - i; i++) {
        if (!memcmp(bytes + i, needle, needle_len)) {
            found = 1;
            break;
        }
    }
    free(bytes);
    return found;
}

static int append_ascii(const WCHAR *path, const char *text) {
    FILE *f = _wfopen(path, L"ab");
    if (!f) return 0;
    size_t length = strlen(text);
    int ok = fwrite(text, 1, length, f) == length && fflush(f) == 0;
    fclose(f);
    return ok;
}

static int replace_ascii_once(const WCHAR *path, const char *old_text,
                              const char *new_text) {
    char *bytes = NULL;
    DWORD size = 0;
    if (!read_file_bytes(path, &bytes, &size)) return 0;
    char *hit = strstr(bytes, old_text);
    if (!hit) {
        free(bytes);
        return 0;
    }
    ByteBuf out = {0};
    bb_add(&out, bytes, (size_t)(hit - bytes));
    bb_add(&out, new_text, strlen(new_text));
    bb_add(&out, hit + strlen(old_text),
           size - (size_t)(hit - bytes) - strlen(old_text));
    int ok = out.data && out.len <= MAXDWORD &&
             write_file_bytes(path, out.data, (DWORD)out.len);
    free(out.data);
    free(bytes);
    return ok;
}

int wmain(int argc, WCHAR **argv) {
    WCHAR tiny_path[8];
    path_join(tiny_path, sizeof tiny_path / sizeof tiny_path[0],
              L"C:\\already-too-long", L"child");
    if (tiny_path[0]) {
        return fail(L"path_join must fail closed instead of returning a truncated path");
    }
    if (path_append_suffix(tiny_path,
                           sizeof tiny_path / sizeof tiny_path[0],
                           L"C:\\already-too-long", L".bak") ||
        tiny_path[0]) {
        return fail(L"path_append_suffix must fail closed instead of returning a truncated path");
    }
    if (wide_format_checked(tiny_path,
                            sizeof tiny_path / sizeof tiny_path[0],
                            L"\"%s\" --arg", L"C:\\already-too-long") ||
        tiny_path[0]) {
        return fail(L"wide_format_checked must fail closed instead of returning a truncated command line");
    }
    if (argc == 4 && !_wcsicmp(argv[1], L"--fsutil-reparse")) {
        WCHAR escaped_dir[MAX_PATH * 4], escaped_file[MAX_PATH * 4];
        path_join(escaped_dir, MAX_PATH * 4, argv[3], L"created-by-launcher");
        path_join(escaped_file, MAX_PATH * 4, argv[3], L"payload.bin");
        if (ensure_dir(escaped_dir)) {
            return fail(L"ensure_dir followed a destination reparse point");
        }
        if (copy_file_safe(argv[2], escaped_file)) {
            return fail(L"copy_file_safe followed a destination reparse point");
        }
        if (write_text_file_utf8(escaped_file, "unsafe") ||
            write_file_bytes(escaped_file, "unsafe", 6)) {
            return fail(L"file writer followed a destination reparse point");
        }
        wprintf(L"reparse_write_blocked=1\n");
        return 0;
    }
    if (argc == 3 && !_wcsicmp(argv[1], L"--restore-reparse")) {
        if (restore_game(argv[2], ENGINE_RENPY)) {
            return fail(L"restore followed a game subdirectory reparse point");
        }
        wprintf(L"reparse_restore_blocked=1\n");
        return 0;
    }
    if (argc == 3 && !_wcsicmp(argv[1], L"--detect")) {
        WCHAR content_root[MAX_PATH * 4];
        Engine engine = detect_engine(argv[2]);
        int has_rpgm_root = rpgm_content_root(argv[2], content_root, MAX_PATH * 4);
        wprintf(L"engine=%ls\n", engine_name(engine));
        if (has_rpgm_root) wprintf(L"rpgm_content_root=%ls\n", content_root);
        return engine == ENGINE_UNKNOWN ? 1 : 0;
    }
    if (argc == 3 && !_wcsicmp(argv[1], L"--deploy-rpgm")) {
        if (detect_engine(argv[2]) != ENGINE_RPGM_MV) {
            return fail(L"deploy target is not an RPG Maker MV/MZ game");
        }
        if (!deploy_rpgm(argv[2])) return fail(L"RPG Maker deploy failed");
        wprintf(L"deployed_rpgm=%ls\n", argv[2]);
        return 0;
    }
    if (argc != 3) return fail(L"usage: launcher_probe <repo-root> <fixture-root>");
    wcsncpy(g_root, argv[1], MAX_PATH * 4 - 1);
    g_root[MAX_PATH * 4 - 1] = 0;

    WCHAR renpy[MAX_PATH * 4], rpgm[MAX_PATH * 4], rpgm_flat[MAX_PATH * 4];
    WCHAR rpgm_flat_mv[MAX_PATH * 4];
    WCHAR generic_nw[MAX_PATH * 4], rpgm_fail[MAX_PATH * 4];
    WCHAR godot[MAX_PATH * 4], godot_embedded[MAX_PATH * 4], exe_select[MAX_PATH * 4];
    WCHAR unity_mono[MAX_PATH * 4], unity_stripped[MAX_PATH * 4], unity6_mono[MAX_PATH * 4];
    WCHAR unity6_mono_x86[MAX_PATH * 4], unity_il2cpp[MAX_PATH * 4], unity_custom[MAX_PATH * 4];
    join(renpy, argv[2], L"renpy");
    join(rpgm, argv[2], L"rpgm");
    join(rpgm_flat, argv[2], L"rpgm_flat");
    join(rpgm_flat_mv, argv[2], L"rpgm_flat_mv");
    join(generic_nw, argv[2], L"generic_nw");
    join(rpgm_fail, argv[2], L"rpgm_fail");
    join(godot, argv[2], L"godot");
    join(godot_embedded, argv[2], L"godot_embedded");
    join(exe_select, argv[2], L"exe_select");
    join(unity_mono, argv[2], L"unity_mono");
    join(unity_stripped, argv[2], L"unity_stripped");
    join(unity6_mono, argv[2], L"unity6_mono");
    join(unity6_mono_x86, argv[2], L"unity6_mono_x86");
    join(unity_il2cpp, argv[2], L"unity_il2cpp");
    join(unity_custom, argv[2], L"unity_custom");

    if (expect_engine(renpy, ENGINE_RENPY, L"renpy")) return 1;
    if (expect_engine(rpgm, ENGINE_RPGM_MV, L"rpgm")) return 1;
    if (expect_engine(rpgm_flat, ENGINE_RPGM_MV, L"rpgm_flat")) return 1;
    if (expect_engine(rpgm_flat_mv, ENGINE_RPGM_MV, L"rpgm_flat_mv")) return 1;
    if (expect_engine(generic_nw, ENGINE_UNKNOWN, L"generic_nw")) return 1;
    if (expect_engine(godot, ENGINE_GODOT, L"godot")) return 1;
    if (expect_engine(godot_embedded, ENGINE_GODOT, L"godot_embedded")) return 1;
    if (expect_engine(unity_mono, ENGINE_UNITY, L"unity_mono")) return 1;
    if (expect_engine(unity_stripped, ENGINE_UNITY, L"unity_stripped")) return 1;
    if (expect_engine(unity6_mono, ENGINE_UNITY, L"unity6_mono")) return 1;
    if (expect_engine(unity6_mono_x86, ENGINE_UNITY, L"unity6_mono_x86")) return 1;
    if (expect_engine(unity_il2cpp, ENGINE_UNITY_IL2CPP, L"unity_il2cpp")) return 1;

    WCHAR selected_exe[MAX_PATH * 4];
    if (!find_exe(exe_select, selected_exe, MAX_PATH * 4) ||
        _wcsicmp(wcsrchr(selected_exe, L'\\') + 1, L"RealGame.exe")) {
        return fail(L"find_exe should prefer the executable with the matching Unity _Data directory");
    }
    if (!find_exe(godot_embedded, selected_exe, MAX_PATH * 4) ||
        _wcsicmp(wcsrchr(selected_exe, L'\\') + 1, L"EmbeddedGodot.exe")) {
        return fail(L"find_exe should prefer the executable that owns an embedded Godot pack");
    }

    if (!deploy_renpy(renpy)) return fail(L"renpy deploy should write the say hook");
    WCHAR renpy_hook[MAX_PATH * 4], renpy_hook_compiled[MAX_PATH * 4];
    WCHAR renpy_font_ttc[MAX_PATH * 4], renpy_font_ttf[MAX_PATH * 4];
    join(renpy_hook, renpy, L"game\\iron_deepseek.rpy");
    join(renpy_hook_compiled, renpy, L"game\\iron_deepseek.rpyc");
    join(renpy_font_ttc, renpy, L"game\\ds_font.ttc");
    join(renpy_font_ttf, renpy, L"game\\ds_font.ttf");
    if (!exists_path(renpy_hook)) return fail(L"renpy hook missing after deploy");
    if (exists_path(renpy_hook_compiled)) {
        return fail(L"renpy deploy left stale launcher hook bytecode behind");
    }
    if (!exists_path(renpy_font_ttc) && !exists_path(renpy_font_ttf)) {
        return fail(L"renpy CJK font missing after deploy");
    }

    if (!deploy_rpgm(rpgm)) return fail(L"rpgm deploy should write the MV/MZ hook");
    WCHAR rpgm_hook[MAX_PATH * 4], rpgm_font_ttc[MAX_PATH * 4], rpgm_font_ttf[MAX_PATH * 4];
    join(rpgm_hook, rpgm, L"www\\js\\hook_rpgm_mv.js");
    join(rpgm_font_ttc, rpgm, L"www\\fonts\\ds_font.ttc");
    join(rpgm_font_ttf, rpgm, L"www\\fonts\\ds_font.ttf");
    if (!exists_path(rpgm_hook)) return fail(L"rpgm hook missing after deploy");
    if (!exists_path(rpgm_font_ttc) && !exists_path(rpgm_font_ttf)) {
        return fail(L"rpgm CJK font missing after deploy");
    }
    if (!deploy_rpgm(rpgm_flat)) return fail(L"flat rpgm deploy should use the game root");
    WCHAR rpgm_flat_hook[MAX_PATH * 4], rpgm_flat_index[MAX_PATH * 4];
    WCHAR rpgm_flat_backup[MAX_PATH * 4], rpgm_flat_font_ttc[MAX_PATH * 4];
    WCHAR rpgm_flat_font_ttf[MAX_PATH * 4];
    join(rpgm_flat_hook, rpgm_flat, L"js\\hook_rpgm_mv.js");
    join(rpgm_flat_index, rpgm_flat, L"index.html");
    join(rpgm_flat_backup, rpgm_flat, L"index.html.dst-backup");
    join(rpgm_flat_font_ttc, rpgm_flat, L"fonts\\ds_font.ttc");
    join(rpgm_flat_font_ttf, rpgm_flat, L"fonts\\ds_font.ttf");
    if (!exists_path(rpgm_flat_hook)) return fail(L"flat rpgm hook missing after deploy");
    if (!exists_path(rpgm_flat_backup)) return fail(L"flat rpgm index backup missing after deploy");
    if (!file_contains_ascii(rpgm_flat_index, "src=\"js/hook_rpgm_mv.js\"")) {
        return fail(L"flat rpgm index missing the deployed hook tag");
    }
    if (!exists_path(rpgm_flat_font_ttc) && !exists_path(rpgm_flat_font_ttf)) {
        return fail(L"flat rpgm CJK font missing after deploy");
    }
    if (deploy_rpgm(rpgm_fail)) {
        return fail(L"rpgm deploy must fail when index.html cannot be read or updated");
    }

    if (!deploy_godot(godot)) return fail(L"godot deploy should enable resource warmup mode");

    if (!deploy_unity(unity_mono)) return fail(L"unity_mono deploy should copy the bundled plugin");
    WCHAR mono_dll[MAX_PATH * 4], mono_json[MAX_PATH * 4], mono_font[MAX_PATH * 4];
    WCHAR mono_doorstop[MAX_PATH * 4], mono_loader[MAX_PATH * 4], mono_core[MAX_PATH * 4];
    join(mono_dll, unity_mono, L"BepInEx\\plugins\\UnityTranslator.dll");
    join(mono_json, unity_mono, L"BepInEx\\plugins\\Newtonsoft.Json.dll");
    join(mono_font, unity_mono, L"BepInEx\\font\\arialuni_sdf_u2019");
    join(mono_doorstop, unity_mono, L"doorstop_config.ini");
    join(mono_loader, unity_mono, L"winhttp.dll");
    join(mono_core, unity_mono, L"BepInEx\\core\\BepInEx.Preloader.dll");
    if (!exists_path(mono_dll)) return fail(L"unity_mono plugin missing after deploy");
    if (!exists_path(mono_json)) return fail(L"unity_mono Newtonsoft.Json dependency missing after deploy");
    if (!exists_path(mono_font)) return fail(L"unity_mono TMP font bundle missing after deploy");
    if (!exists_path(mono_doorstop)) return fail(L"partial Unity Mono install must repair missing doorstop_config.ini");
    if (!exists_path(mono_loader)) return fail(L"partial Unity Mono install must repair missing Doorstop loader");
    if (!exists_path(mono_core)) return fail(L"partial Unity Mono install must repair missing BepInEx core");
    WCHAR normal_corlib[MAX_PATH * 4], normal_font_patcher[MAX_PATH * 4];
    join(normal_corlib, unity_mono, L"BepInEx\\unstripped_corlib");
    join(normal_font_patcher, unity_mono, L"BepInEx\\patchers\\DeepSeekUnityFontPatcher.dll");
    if (exists_path(normal_corlib)) return fail(L"normal Unity Mono deploy must not install a corlib override");
    if (exists_path(normal_font_patcher)) return fail(L"normal Unity Mono deploy must not install the stripped-runtime font patcher");

    if (!deploy_unity(unity_stripped)) return fail(L"stripped Unity Mono deploy should install official corlib support");
    WCHAR stripped_corlib[MAX_PATH * 4], stripped_owner[MAX_PATH * 4];
    WCHAR stripped_doorstop[MAX_PATH * 4], stripped_bepinex_cfg[MAX_PATH * 4];
    WCHAR stripped_doorstop_owned[MAX_PATH * 4], stripped_bepinex_owned[MAX_PATH * 4];
    WCHAR stripped_font_patcher[MAX_PATH * 4], stripped_font_patcher_owned[MAX_PATH * 4];
    join(stripped_corlib, unity_stripped, L"BepInEx\\unstripped_corlib\\mscorlib.dll");
    join(stripped_owner, unity_stripped, L"BepInEx\\unstripped_corlib\\.dst-installed-by-ds");
    join(stripped_doorstop, unity_stripped, L"doorstop_config.ini");
    join(stripped_bepinex_cfg, unity_stripped, L"BepInEx\\config\\BepInEx.cfg");
    join(stripped_font_patcher, unity_stripped, L"BepInEx\\patchers\\DeepSeekUnityFontPatcher.dll");
    join(stripped_font_patcher_owned, unity_stripped, L"BepInEx\\patchers\\DeepSeekUnityFontPatcher.dll.dst-owned");
    _snwprintf(stripped_doorstop_owned, MAX_PATH * 4, L"%s.dst-stripped-owned", stripped_doorstop);
    stripped_doorstop_owned[MAX_PATH * 4 - 1] = 0;
    _snwprintf(stripped_bepinex_owned, MAX_PATH * 4, L"%s.dst-stripped-owned", stripped_bepinex_cfg);
    stripped_bepinex_owned[MAX_PATH * 4 - 1] = 0;
    if (!exists_path(stripped_corlib) || !file_contains_ascii(stripped_corlib, "WriteAllText")) {
        return fail(L"stripped Unity Mono deploy must install a complete official mscorlib");
    }
    if (!exists_path(stripped_owner)) return fail(L"stripped Unity Mono corlib must retain launcher ownership metadata");
    if (!file_contains_ascii(stripped_doorstop, "dll_search_path_override = BepInEx\\unstripped_corlib;BepInEx\\core")) {
        return fail(L"stripped Unity Mono Doorstop override must use ASCII relative paths");
    }
    if (!file_contains_ascii(stripped_bepinex_cfg, "UnityLogListening = false")) {
        return fail(L"stripped Unity Mono deploy must disable unavailable Unity log callbacks");
    }
    if (!exists_path(stripped_doorstop_owned) || !exists_path(stripped_bepinex_owned)) {
        return fail(L"stripped Unity Mono config edits must record ownership snapshots");
    }
    if (!exists_path(stripped_font_patcher) || !exists_path(stripped_font_patcher_owned)) {
        return fail(L"stripped Unity Mono BepInEx 5 deploy must install the owned font metadata patcher");
    }

    if (!deploy_unity(unity6_mono)) return fail(L"unity6_mono deploy should copy the BepInEx 6 runtime and plugin");
    WCHAR unity6_dll[MAX_PATH * 4], unity6_json[MAX_PATH * 4], unity6_core[MAX_PATH * 4], unity6_font[MAX_PATH * 4];
    join(unity6_dll, unity6_mono, L"BepInEx\\plugins\\UnityTranslator.dll");
    join(unity6_json, unity6_mono, L"BepInEx\\plugins\\Newtonsoft.Json.dll");
    join(unity6_core, unity6_mono, L"BepInEx\\core\\BepInEx.Unity.Mono.dll");
    join(unity6_font, unity6_mono, L"BepInEx\\font\\arialuni_sdf_u6000");
    if (!exists_path(unity6_dll)) return fail(L"unity6_mono plugin missing after deploy");
    if (!exists_path(unity6_json)) return fail(L"unity6_mono Newtonsoft.Json dependency missing after deploy");
    if (!exists_path(unity6_core)) return fail(L"unity6_mono BepInEx 6 Mono core missing after deploy");
    if (!exists_path(unity6_font)) return fail(L"unity6_mono Unity 6 TMP font bundle missing after deploy");
    WCHAR unity6_font_patcher[MAX_PATH * 4];
    join(unity6_font_patcher, unity6_mono, L"BepInEx\\patchers\\DeepSeekUnityFontPatcher.dll");
    if (exists_path(unity6_font_patcher)) return fail(L"BepInEx 6 Mono deploy must not install the BepInEx 5 stripped-runtime patcher");
    WCHAR unity6_loader[MAX_PATH * 4];
    join(unity6_loader, unity6_mono, L"winhttp.dll");
    if (probe_pe_machine(unity6_loader) != 0x8664) {
        return fail(L"x64 Unity 6 deploy must keep an x64 Doorstop loader");
    }

    if (!deploy_unity(unity6_mono_x86)) return fail(L"x86 unity6_mono deploy should repair and deploy the BepInEx 6 runtime");
    WCHAR unity6_x86_loader[MAX_PATH * 4], unity6_x86_core[MAX_PATH * 4], unity6_x86_doorstop[MAX_PATH * 4];
    join(unity6_x86_loader, unity6_mono_x86, L"winhttp.dll");
    join(unity6_x86_core, unity6_mono_x86, L"BepInEx\\core\\BepInEx.Unity.Mono.dll");
    join(unity6_x86_doorstop, unity6_mono_x86, L"doorstop_config.ini");
    if (!exists_path(unity6_x86_core)) return fail(L"x86 unity6_mono BepInEx 6 Mono core missing after deploy");
    if (!exists_path(unity6_x86_doorstop)) return fail(L"x86 Unity 6 deploy must repair missing doorstop_config.ini");
    if (probe_pe_machine(unity6_x86_loader) != 0x014c) {
        return fail(L"x86 Unity 6 deploy must replace a mismatched x64 Doorstop loader");
    }

    WCHAR custom_dll[MAX_PATH * 4], custom_disabled[MAX_PATH * 4];
    join(custom_dll, unity_custom, L"BepInEx\\plugins\\UnityTranslator.dll");
    _snwprintf(custom_disabled, MAX_PATH * 4, L"%s.disabled", custom_dll);
    deploy_unity_il2cpp(unity_custom);
    if (!exists_path(custom_dll)) return fail(L"custom IL2CPP plugin should be preserved");
    if (exists_path(custom_disabled)) return fail(L"custom IL2CPP plugin should not be disabled");
    WCHAR augmented_config[MAX_PATH * 4], augmented_owned[MAX_PATH * 4];
    join(augmented_config, unity_custom, L"BepInEx\\config\\AutoTranslatorConfig.ini");
    _snwprintf(augmented_owned, MAX_PATH * 4, L"%s.dst-owned", augmented_config);
    augmented_owned[MAX_PATH * 4 - 1] = 0;
    if (!replace_ascii_once(
            augmented_config,
            "MaxCharactersPerTranslation=2500",
            "MaxCharactersPerTranslation=400") ||
        !replace_ascii_once(
            augmented_owned,
            "MaxCharactersPerTranslation=2500",
            "MaxCharactersPerTranslation=400")) {
        return fail(L"could not create the previous owned XUnity limit fixture");
    }
    if (!append_ascii(
            augmented_config,
            "\n[ProviderDefaults]\nPreserveAfterOwnedConfigMigration=True\n")) {
        return fail(L"could not simulate XUnity adding provider defaults to its config");
    }
    if (!deploy_unity_il2cpp(unity_custom)) {
        return fail(L"Unity IL2CPP redeploy should accept XUnity-added defaults when owned settings are unchanged");
    }
    if (!file_contains_ascii(augmented_config, "MaxCharactersPerTranslation=2500") ||
        !file_contains_ascii(augmented_config, "PreserveAfterOwnedConfigMigration=True")) {
        return fail(L"Unity IL2CPP owned-config migration dropped the new limit or XUnity defaults");
    }

    WCHAR bundled_dll[MAX_PATH * 4], bundled_disabled[MAX_PATH * 4];
    join(bundled_dll, unity_il2cpp, L"BepInEx\\plugins\\UnityTranslator.dll");
    _snwprintf(bundled_disabled, MAX_PATH * 4, L"%s.disabled", bundled_dll);
    deploy_unity_il2cpp(unity_il2cpp);
    if (exists_path(bundled_dll)) return fail(L"bundled Mono plugin should be disabled for IL2CPP");
    if (!exists_path(bundled_disabled)) return fail(L"disabled bundled plugin not found");
    WCHAR il2cpp_config[MAX_PATH * 4];
    join(il2cpp_config, unity_il2cpp, L"BepInEx\\config\\AutoTranslatorConfig.ini");
    if (!file_contains_ascii(il2cpp_config, "MaxCharactersPerTranslation=2500")) {
        return fail(L"Unity IL2CPP deploy kept the obsolete 400-character translation limit");
    }
    WCHAR il2cpp_font_patcher[MAX_PATH * 4];
    join(il2cpp_font_patcher, unity_il2cpp, L"BepInEx\\patchers\\DeepSeekUnityFontPatcher.dll");
    if (exists_path(il2cpp_font_patcher)) return fail(L"Unity IL2CPP deploy must not install the Mono font patcher");

    wcsncpy(g_root, argv[2], MAX_PATH * 4 - 1);
    g_root[MAX_PATH * 4 - 1] = 0;
    WCHAR loaded[MAX_PATH * 4];
    if (!save_last_game_dir(unity_mono)) return fail(L"last game dir should be saved");
    if (!load_last_game_dir(loaded, MAX_PATH * 4)) return fail(L"last game dir should be loaded");
    if (wcscmp(loaded, unity_mono)) return fail(L"loaded last game dir mismatch");

    wprintf(L"launcher probe passed\n");
    return 0;
}
