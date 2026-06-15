#include "engine.h"
#include "fsutil.h"

#include <wchar.h>

int has_file_pattern(const WCHAR *dir, const WCHAR *pattern) {
    WCHAR pat[MAX_PATH * 4];
    path_join(pat, MAX_PATH * 4, dir, pattern);
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) return 0;
    FindClose(h);
    return 1;
}

int find_subdir_suffix(const WCHAR *dir, const WCHAR *suffix) {
    WCHAR pat[MAX_PATH * 4];
    path_join(pat, MAX_PATH * 4, dir, L"*");
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) return 0;
    int found = 0;
    size_t sl = wcslen(suffix);
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
        if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..")) continue;
        size_t nl = wcslen(fd.cFileName);
        if (nl >= sl && !_wcsicmp(fd.cFileName + nl - sl, suffix)) {
            found = 1;
            break;
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return found;
}

int find_exe(const WCHAR *dir, WCHAR *out, size_t cap) {
    WCHAR pat[MAX_PATH * 4];
    path_join(pat, MAX_PATH * 4, dir, L"*.exe");
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) return 0;
    int ok = 0;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        if (wcsstr(fd.cFileName, L"CrashHandler") || wcsstr(fd.cFileName, L"UnityCrashHandler")) continue;
        if (!_wcsicmp(fd.cFileName, L"DeepSeekTranslator.exe") || !_wcsicmp(fd.cFileName, L"dst_server.exe")) continue;
        path_join(out, cap, dir, fd.cFileName);
        ok = 1;
        break;
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return ok;
}

int unity_is_il2cpp(const WCHAR *dir) {
    WCHAR p[MAX_PATH * 4];
    path_join(p, MAX_PATH * 4, dir, L"GameAssembly.dll");
    if (exists_path(p)) return 1;

    path_join(p, MAX_PATH * 4, dir, L"*_Data\\il2cpp_data");
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(p, &fd);
    if (h == INVALID_HANDLE_VALUE) return 0;
    int ok = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
    FindClose(h);
    return ok;
}

Engine detect_engine(const WCHAR *dir) {
    WCHAR p[MAX_PATH * 4];
    path_join(p, MAX_PATH * 4, dir, L"game");
    if (is_dir(p) && (has_file_pattern(p, L"*.rpy") || has_file_pattern(p, L"*.rpyc") || has_file_pattern(p, L"*.rpa"))) return ENGINE_RENPY;

    path_join(p, MAX_PATH * 4, dir, L"www\\index.html");
    if (exists_path(p)) {
        WCHAR js[MAX_PATH * 4];
        path_join(js, MAX_PATH * 4, dir, L"www\\js");
        if (is_dir(js)) return ENGINE_RPGM_MV;
    }

    path_join(p, MAX_PATH * 4, dir, L"Data");
    if (is_dir(p) && (has_file_pattern(p, L"*.rxdata") || has_file_pattern(p, L"*.rvdata") || has_file_pattern(p, L"*.rvdata2"))) return ENGINE_RPGM_LEGACY;

    if (find_subdir_suffix(dir, L"_Data")) return unity_is_il2cpp(dir) ? ENGINE_UNITY_IL2CPP : ENGINE_UNITY;
    return ENGINE_UNKNOWN;
}

const WCHAR *engine_name(Engine e) {
    switch (e) {
    case ENGINE_RENPY: return L"Ren'Py";
    case ENGINE_RPGM_MV: return L"RPG Maker MV/MZ";
    case ENGINE_UNITY: return L"Unity";
    case ENGINE_UNITY_IL2CPP: return L"Unity (IL2CPP)";
    case ENGINE_RPGM_LEGACY: return L"RPG Maker XP/VX/VXAce";
    default: return L"未知";
    }
}
