/* ================================================================
 * engine.c — 游戏引擎自动检测实现
 * ----------------------------------------------------------------
 * 通过扫描游戏目录中的特征文件/目录来识别引擎类型。
 * 检测顺序：Ren'Py → RPG Maker MV/MZ → RPG Maker Legacy → Unity
 * ================================================================ */

#include "engine.h"
#include "fsutil.h"

#include <wchar.h>

/* ----------------------------------------------------------------
 * has_file_pattern — 在指定目录下用通配符匹配文件
 *
 * 拼接 dir + pattern 后调用 FindFirstFileW，只要找到至少一个
 * 匹配项即返回 1。用于快速检测特征文件是否存在。
 * ---------------------------------------------------------------- */
int has_file_pattern(const WCHAR *dir, const WCHAR *pattern) {
    WCHAR pat[MAX_PATH * 4];
    path_join(pat, MAX_PATH * 4, dir, pattern);
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) return 0;
    FindClose(h);
    return 1;
}

/* ----------------------------------------------------------------
 * find_subdir_suffix — 在子目录中查找以指定后缀结尾的目录名
 *
 * 遍历 dir 下的所有子目录，检查目录名是否以 suffix（不区分大小写）
 * 结尾。Unity 游戏的 Data 目录通常命名为 <GameName>_Data，
 * 因此用 "_Data" 后缀来匹配。
 * ---------------------------------------------------------------- */
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

/* ----------------------------------------------------------------
 * find_exe — 定位游戏主可执行文件
 *
 * 在 dir 下查找第一个 .exe 文件，但排除：
 *   - 崩溃报告程序 (CrashHandler / UnityCrashHandler)
 *   - 本工具自身 (DeepSeekTranslator.exe / dst_server.exe)
 * 找到后将完整路径写入 out。
 * ---------------------------------------------------------------- */
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

/* ----------------------------------------------------------------
 * unity_is_il2cpp — 判断 Unity 是否使用 IL2CPP 后端
 *
 * IL2CPP 构建的特征：游戏根目录存在 GameAssembly.dll，
 * 或 *_Data/il2cpp_data 目录存在。
 * 如果都不存在，则认为是 Mono 构建。
 * ---------------------------------------------------------------- */
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

/* ----------------------------------------------------------------
 * detect_engine — 主引擎检测入口
 *
 * 按优先级顺序依次检查特征文件：
 *   1. Ren'Py：存在 game/ 目录且含 .rpy / .rpyc / .rpa 文件
 *   2. RPG Maker MV/MZ：存在 www/index.html 且 www/js/ 为目录
 *   3. RPG Maker Legacy：Data/ 目录含 .rxdata / .rvdata / .rvdata2
 *   4. Unity：子目录名以 _Data 结尾，再细分 Mono/IL2CPP
 * 都不匹配则返回 ENGINE_UNKNOWN。
 * ---------------------------------------------------------------- */
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

/* ----------------------------------------------------------------
 * engine_name — 返回引擎类型的可读中文名称
 * 用于界面显示和日志输出。
 * ---------------------------------------------------------------- */
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
