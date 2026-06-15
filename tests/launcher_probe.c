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

int wmain(int argc, WCHAR **argv) {
    if (argc != 3) return fail(L"usage: launcher_probe <repo-root> <fixture-root>");
    wcsncpy(g_root, argv[1], MAX_PATH * 4 - 1);
    g_root[MAX_PATH * 4 - 1] = 0;

    WCHAR renpy[MAX_PATH * 4], rpgm[MAX_PATH * 4], unity_mono[MAX_PATH * 4], unity6_mono[MAX_PATH * 4], unity_il2cpp[MAX_PATH * 4], unity_custom[MAX_PATH * 4];
    join(renpy, argv[2], L"renpy");
    join(rpgm, argv[2], L"rpgm");
    join(unity_mono, argv[2], L"unity_mono");
    join(unity6_mono, argv[2], L"unity6_mono");
    join(unity_il2cpp, argv[2], L"unity_il2cpp");
    join(unity_custom, argv[2], L"unity_custom");

    if (expect_engine(renpy, ENGINE_RENPY, L"renpy")) return 1;
    if (expect_engine(rpgm, ENGINE_RPGM_MV, L"rpgm")) return 1;
    if (expect_engine(unity_mono, ENGINE_UNITY, L"unity_mono")) return 1;
    if (expect_engine(unity6_mono, ENGINE_UNITY, L"unity6_mono")) return 1;
    if (expect_engine(unity_il2cpp, ENGINE_UNITY_IL2CPP, L"unity_il2cpp")) return 1;

    if (!deploy_renpy(renpy)) return fail(L"renpy deploy should write the say hook");
    WCHAR renpy_hook[MAX_PATH * 4], renpy_font_ttc[MAX_PATH * 4], renpy_font_ttf[MAX_PATH * 4];
    join(renpy_hook, renpy, L"game\\iron_deepseek.rpy");
    join(renpy_font_ttc, renpy, L"game\\ds_font.ttc");
    join(renpy_font_ttf, renpy, L"game\\ds_font.ttf");
    if (!exists_path(renpy_hook)) return fail(L"renpy hook missing after deploy");
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

    if (!deploy_unity(unity_mono)) return fail(L"unity_mono deploy should copy the bundled plugin");
    WCHAR mono_dll[MAX_PATH * 4], mono_json[MAX_PATH * 4], mono_font[MAX_PATH * 4];
    join(mono_dll, unity_mono, L"BepInEx\\plugins\\UnityTranslator.dll");
    join(mono_json, unity_mono, L"BepInEx\\plugins\\Newtonsoft.Json.dll");
    join(mono_font, unity_mono, L"BepInEx\\font\\arialuni_sdf_u2019");
    if (!exists_path(mono_dll)) return fail(L"unity_mono plugin missing after deploy");
    if (!exists_path(mono_json)) return fail(L"unity_mono Newtonsoft.Json dependency missing after deploy");
    if (!exists_path(mono_font)) return fail(L"unity_mono TMP font bundle missing after deploy");

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

    WCHAR custom_dll[MAX_PATH * 4], custom_disabled[MAX_PATH * 4];
    join(custom_dll, unity_custom, L"BepInEx\\plugins\\UnityTranslator.dll");
    _snwprintf(custom_disabled, MAX_PATH * 4, L"%s.disabled", custom_dll);
    deploy_unity_il2cpp(unity_custom);
    if (!exists_path(custom_dll)) return fail(L"custom IL2CPP plugin should be preserved");
    if (exists_path(custom_disabled)) return fail(L"custom IL2CPP plugin should not be disabled");

    WCHAR bundled_dll[MAX_PATH * 4], bundled_disabled[MAX_PATH * 4];
    join(bundled_dll, unity_il2cpp, L"BepInEx\\plugins\\UnityTranslator.dll");
    _snwprintf(bundled_disabled, MAX_PATH * 4, L"%s.disabled", bundled_dll);
    deploy_unity_il2cpp(unity_il2cpp);
    if (exists_path(bundled_dll)) return fail(L"bundled Mono plugin should be disabled for IL2CPP");
    if (!exists_path(bundled_disabled)) return fail(L"disabled bundled plugin not found");

    wcsncpy(g_root, argv[2], MAX_PATH * 4 - 1);
    g_root[MAX_PATH * 4 - 1] = 0;
    WCHAR loaded[MAX_PATH * 4];
    if (!save_last_game_dir(unity_mono)) return fail(L"last game dir should be saved");
    if (!load_last_game_dir(loaded, MAX_PATH * 4)) return fail(L"last game dir should be loaded");
    if (wcscmp(loaded, unity_mono)) return fail(L"loaded last game dir mismatch");

    wprintf(L"launcher probe passed\n");
    return 0;
}
