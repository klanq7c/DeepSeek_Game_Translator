#include "deploy.h"
#include "fsutil.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

WCHAR g_root[MAX_PATH * 4];
WCHAR g_game[MAX_PATH * 4];

void append_log(const WCHAR *fmt, ...) {
    (void)fmt;
}

static int fail(const WCHAR *message) {
    fwprintf(stderr, L"%ls\n", message);
    return 1;
}

static void join(WCHAR *out, const WCHAR *base, const WCHAR *leaf) {
    path_join(out, MAX_PATH * 4, base, leaf);
}

static int file_contains(const WCHAR *path, const char *needle) {
    char *bytes = NULL;
    DWORD size = 0;
    int found = 0;
    if (read_file_bytes(path, &bytes, &size)) {
        found = strstr(bytes, needle) != NULL;
    }
    free(bytes);
    return found;
}

int wmain(int argc, WCHAR **argv) {
    if (argc != 3) return fail(L"usage: restore_probe <repo-root> <fixture-root>");
    wcsncpy(g_root, argv[1], MAX_PATH * 4 - 1);
    g_root[MAX_PATH * 4 - 1] = 0;

    WCHAR renpy[MAX_PATH * 4], rpgm[MAX_PATH * 4], rpgm_flat[MAX_PATH * 4];
    WCHAR rpgm_fail[MAX_PATH * 4], godot[MAX_PATH * 4];
    WCHAR unity_mono[MAX_PATH * 4], unity_stripped[MAX_PATH * 4], unity6_mono[MAX_PATH * 4];
    WCHAR unity_il2cpp[MAX_PATH * 4], unity_custom[MAX_PATH * 4];
    join(renpy, argv[2], L"renpy");
    join(rpgm, argv[2], L"rpgm");
    join(rpgm_flat, argv[2], L"rpgm_flat");
    join(rpgm_fail, argv[2], L"rpgm_fail");
    join(godot, argv[2], L"godot");
    join(unity_mono, argv[2], L"unity_mono");
    join(unity_stripped, argv[2], L"unity_stripped");
    join(unity6_mono, argv[2], L"unity6_mono");
    join(unity_il2cpp, argv[2], L"unity_il2cpp");
    join(unity_custom, argv[2], L"unity_custom");

    WCHAR path[MAX_PATH * 4];
    join(path, renpy, L"game\\iron_deepseek.rpyc");
    if (!write_text_file_utf8(path, "compiled launcher hook")) {
        return fail(L"could not create Ren'Py compiled-hook restore fixture");
    }
    if (!restore_game(renpy, ENGINE_RENPY)) return fail(L"Ren'Py restore should succeed");
    join(path, renpy, L"game\\iron_deepseek.rpy");
    if (exists_path(path)) return fail(L"Ren'Py restore left the launcher hook behind");
    join(path, renpy, L"game\\iron_deepseek.rpyc");
    if (exists_path(path)) return fail(L"Ren'Py restore left compiled launcher hook bytecode behind");
    join(path, renpy, L"game\\script.rpy");
    if (!exists_path(path)) return fail(L"Ren'Py restore removed a game script");
    join(path, renpy, L"game\\ds_font.ttf");
    if (exists_path(path)) return fail(L"Ren'Py restore left the launcher font behind");
    join(path, renpy, L"game\\ds_font.ttc");
    if (exists_path(path)) return fail(L"Ren'Py restore left the launcher font collection behind");

    if (!restore_game(rpgm, ENGINE_RPGM_MV)) return fail(L"RPG Maker restore should succeed");
    join(path, rpgm, L"www\\index.html");
    if (file_contains(path, "src=\"js/hook_rpgm_mv.js\"")) {
        return fail(L"RPG Maker restore left the owned script tag behind");
    }
    if (!file_contains(path, "window.assetName='hook_rpgm_mv.js'")) {
        return fail(L"RPG Maker restore removed an unrelated inline mention");
    }
    join(path, rpgm, L"www\\index.html.dst-backup");
    if (exists_path(path)) return fail(L"RPG Maker restore left its one-time backup behind");
    join(path, rpgm, L"www\\js\\hook_rpgm_mv.js");
    if (exists_path(path)) return fail(L"RPG Maker restore left the hook file behind");

    if (!restore_game(rpgm_flat, ENGINE_RPGM_MV)) {
        return fail(L"flat RPG Maker restore should succeed");
    }
    join(path, rpgm_flat, L"index.html");
    if (file_contains(path, "src=\"js/hook_rpgm_mv.js\"")) {
        return fail(L"flat RPG Maker restore left the owned script tag behind");
    }
    join(path, rpgm_flat, L"index.html.dst-backup");
    if (exists_path(path)) return fail(L"flat RPG Maker restore left its index backup behind");
    join(path, rpgm_flat, L"js\\hook_rpgm_mv.js");
    if (exists_path(path)) return fail(L"flat RPG Maker restore left the hook file behind");

    join(path, rpgm_fail, L"www\\index.html.dst-backup");
    if (!write_text_file_utf8(path, "original-index")) return fail(L"could not create RPG Maker backup fixture");
    if (restore_game(rpgm_fail, ENGINE_RPGM_MV)) {
        return fail(L"RPG Maker restore should fail when index.html is a directory");
    }
    if (!exists_path(path)) return fail(L"RPG Maker restore deleted the backup after an index failure");

    static const WCHAR *godot_files[] = {
        L"dst_godot_runtime.gd",
        L"dst_godot_patch.exe",
        L"dst_godot_patch.exe.dst-owned",
        L"dst_godot_patch.pck",
        L"dst_godot_patch.next.pck",
        L"dst_godot_patch.building"
    };
    for (size_t i = 0; i < sizeof(godot_files) / sizeof(godot_files[0]); i++) {
        join(path, godot, godot_files[i]);
        if (!write_text_file_utf8(path, "owned")) return fail(L"could not create Godot restore fixture");
    }
    if (!restore_game(godot, ENGINE_GODOT)) return fail(L"Godot restore should succeed");
    for (size_t i = 0; i < sizeof(godot_files) / sizeof(godot_files[0]); i++) {
        join(path, godot, godot_files[i]);
        if (exists_path(path)) return fail(L"Godot restore left a generated artifact behind");
    }
    join(path, godot, L"demo.pck");
    if (!exists_path(path)) return fail(L"Godot restore removed the original game pack");

    if (!restore_game(unity_mono, ENGINE_UNITY)) return fail(L"Unity Mono restore should succeed");
    join(path, unity_mono, L"BepInEx\\plugins\\UnityTranslator.dll");
    if (exists_path(path)) return fail(L"Unity Mono restore left the owned plugin behind");
    join(path, unity_mono, L"BepInEx\\plugins\\Newtonsoft.Json.dll");
    if (!exists_path(path)) return fail(L"Unity Mono restore removed a shared dependency");
    join(path, unity_mono, L"BepInEx\\font\\arialuni_sdf_u2019");
    if (!exists_path(path)) return fail(L"Unity Mono restore removed the shared TMP font payload");

    WCHAR corlib_user_file[MAX_PATH * 4];
    join(corlib_user_file, unity_stripped, L"BepInEx\\unstripped_corlib\\user-addon.dll");
    if (!write_text_file_utf8(corlib_user_file, "user-owned-corlib-extension")) {
        return fail(L"could not create user-owned corlib restore fixture");
    }
    if (restore_game(unity_stripped, ENGINE_UNITY)) {
        return fail(L"stripped Unity Mono restore should report a user-added corlib file as preserved");
    }
    join(path, unity_stripped, L"BepInEx\\patchers\\DeepSeekUnityFontPatcher.dll");
    if (exists_path(path)) return fail(L"stripped Unity Mono restore left the owned font patcher behind");
    join(path, unity_stripped, L"BepInEx\\patchers\\DeepSeekUnityFontPatcher.dll.dst-owned");
    if (exists_path(path)) return fail(L"stripped Unity Mono restore left the font patcher ownership snapshot behind");
    join(path, unity_stripped, L"BepInEx\\unstripped_corlib");
    if (!exists_path(path) || !file_contains(corlib_user_file, "user-owned-corlib-extension")) {
        return fail(L"stripped Unity Mono restore removed a user-added corlib file");
    }
    if (!DeleteFileW(corlib_user_file) || !RemoveDirectoryW(path)) {
        return fail(L"could not clean the preserved corlib fixture before redeploy");
    }
    join(path, unity_stripped, L"doorstop_config.ini");
    if (file_contains(path, "BepInEx\\unstripped_corlib")) {
        return fail(L"stripped Unity Mono restore left the owned Doorstop override behind");
    }
    WCHAR stripped_owned[MAX_PATH * 4], stripped_backup[MAX_PATH * 4];
    _snwprintf(stripped_owned, MAX_PATH * 4, L"%s.dst-stripped-owned", path);
    stripped_owned[MAX_PATH * 4 - 1] = 0;
    _snwprintf(stripped_backup, MAX_PATH * 4, L"%s.dst-stripped-backup", path);
    stripped_backup[MAX_PATH * 4 - 1] = 0;
    if (exists_path(stripped_owned) || exists_path(stripped_backup)) {
        return fail(L"stripped Unity Mono restore left Doorstop recovery metadata behind");
    }
    join(path, unity_stripped, L"BepInEx\\config\\BepInEx.cfg");
    if (exists_path(path)) return fail(L"stripped Unity Mono restore left its launcher-created BepInEx config behind");

    if (!deploy_unity(unity_stripped)) return fail(L"could not redeploy stripped Unity Mono ownership fixture");
    join(path, unity_stripped, L"BepInEx\\config\\BepInEx.cfg");
    if (!write_text_file_utf8(path, "[Logging]\nUnityLogListening = true\n[User]\nChanged=True\n")) {
        return fail(L"could not modify the stripped Unity Mono config fixture");
    }
    WCHAR modified_font_patcher[MAX_PATH * 4], font_patcher_owned[MAX_PATH * 4];
    join(modified_font_patcher, unity_stripped, L"BepInEx\\patchers\\DeepSeekUnityFontPatcher.dll");
    if (!write_text_file_utf8(modified_font_patcher, "user-modified-font-patcher")) {
        return fail(L"could not modify the stripped Unity Mono font patcher fixture");
    }
    if (restore_game(unity_stripped, ENGINE_UNITY)) {
        return fail(L"stripped Unity Mono restore should report user-modified config and patcher files as preserved");
    }
    if (!file_contains(path, "Changed=True")) {
        return fail(L"stripped Unity Mono restore damaged a user-modified BepInEx config");
    }
    _snwprintf(stripped_owned, MAX_PATH * 4, L"%s.dst-stripped-owned", path);
    stripped_owned[MAX_PATH * 4 - 1] = 0;
    if (!exists_path(stripped_owned)) {
        return fail(L"stripped Unity Mono restore discarded recovery metadata for a user-modified config");
    }
    if (!file_contains(modified_font_patcher, "user-modified-font-patcher")) {
        return fail(L"stripped Unity Mono restore damaged a user-modified font patcher");
    }
    _snwprintf(font_patcher_owned, MAX_PATH * 4, L"%s.dst-owned", modified_font_patcher);
    font_patcher_owned[MAX_PATH * 4 - 1] = 0;
    if (!exists_path(font_patcher_owned)) {
        return fail(L"stripped Unity Mono restore discarded ownership metadata for a user-modified font patcher");
    }

    if (!restore_game(unity6_mono, ENGINE_UNITY)) return fail(L"Unity 6 Mono restore should succeed");
    join(path, unity6_mono, L"BepInEx\\plugins\\UnityTranslator.dll");
    if (exists_path(path)) return fail(L"Unity 6 Mono restore left the owned plugin behind");
    join(path, unity6_mono, L"BepInEx\\core\\BepInEx.Unity.Mono.dll");
    if (!exists_path(path)) return fail(L"Unity 6 Mono restore removed the BepInEx runtime");

    join(path, unity_mono, L"BepInEx\\plugins\\UnityTranslator.dll");
    if (!write_text_file_utf8(path, "user-plugin")) return fail(L"could not create custom Unity plugin fixture");
    if (restore_game(unity_mono, ENGINE_UNITY)) {
        return fail(L"Unity Mono restore should report a mismatched plugin as preserved");
    }
    if (!file_contains(path, "user-plugin")) return fail(L"Unity Mono restore removed a custom plugin");

    WCHAR tmp_user_file[MAX_PATH * 4];
    join(tmp_user_file, unity_il2cpp,
         L"BepInEx\\plugins\\DeepSeekTMPFontFallback\\user-addon.txt");
    if (!write_text_file_utf8(tmp_user_file, "user-owned-tmp-extension")) {
        return fail(L"could not create user-owned TMP fallback restore fixture");
    }
    if (restore_game(unity_il2cpp, ENGINE_UNITY_IL2CPP)) {
        return fail(L"Unity IL2CPP restore should report a user-added TMP fallback file as preserved");
    }
    join(path, unity_il2cpp, L"BepInEx\\plugins\\XUnity.AutoTranslator\\Translators\\DeepSeekTranslate.dll");
    if (exists_path(path)) return fail(L"Unity IL2CPP restore left the owned endpoint behind");
    join(path, unity_il2cpp, L"BepInEx\\plugins\\XUnity.AutoTranslator");
    if (exists_path(path)) return fail(L"Unity IL2CPP restore left its owned XUnity plugin behind");
    join(path, unity_il2cpp, L"BepInEx\\plugins\\DeepSeekTMPFontFallback");
    if (!exists_path(path) || !file_contains(tmp_user_file, "user-owned-tmp-extension")) {
        return fail(L"Unity IL2CPP restore removed a user-added TMP fallback file");
    }
    join(path, unity_il2cpp, L"BepInEx\\plugins\\UnityTranslator.dll.disabled");
    if (exists_path(path)) return fail(L"Unity IL2CPP restore left the disabled owned Mono plugin behind");
    join(path, unity_il2cpp, L"BepInEx\\core\\Il2Cppmscorlib.dll");
    if (!exists_path(path)) return fail(L"Unity IL2CPP restore did not restore Il2Cppmscorlib.dll");
    join(path, unity_il2cpp, L"BepInEx\\config\\AutoTranslatorConfig.ini");
    if (!file_contains(path, "Keep=True")) return fail(L"Unity IL2CPP restore did not restore the user's config");
    WCHAR marker[MAX_PATH * 4];
    _snwprintf(marker, MAX_PATH * 4, L"%s.dst-owned", path);
    marker[MAX_PATH * 4 - 1] = 0;
    if (exists_path(marker)) return fail(L"Unity IL2CPP restore left its config ownership marker behind");
    join(path, unity_il2cpp, L"BepInEx\\core\\XUnity.Common.dll");
    if (!exists_path(path)) return fail(L"Unity IL2CPP restore removed the third-party XUnity runtime");

    WCHAR custom_cfg[MAX_PATH * 4], custom_owned[MAX_PATH * 4];
    join(custom_cfg, unity_custom, L"BepInEx\\config\\AutoTranslatorConfig.ini");
    _snwprintf(custom_owned, MAX_PATH * 4, L"%s.dst-owned", custom_cfg);
    custom_owned[MAX_PATH * 4 - 1] = 0;
    if (!write_text_file_utf8(custom_cfg, "[User]\nChanged=True\n")) {
        return fail(L"could not modify the XUnity config fixture");
    }
    if (restore_game(unity_custom, ENGINE_UNITY_IL2CPP)) {
        return fail(L"Unity IL2CPP restore should report a user-modified config as preserved");
    }
    if (!file_contains(custom_cfg, "Changed=True") || !exists_path(custom_owned)) {
        return fail(L"Unity IL2CPP restore damaged a user-modified config or its recovery marker");
    }
    join(path, unity_custom, L"BepInEx\\plugins\\UnityTranslator.dll");
    if (!file_contains(path, "custom-il2cpp-plugin")) {
        return fail(L"Unity IL2CPP restore removed an unrelated custom Mono plugin");
    }

    wprintf(L"restore probe passed\n");
    return 0;
}
