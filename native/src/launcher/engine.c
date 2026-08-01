/* ================================================================
 * engine.c — 游戏引擎自动检测实现
 * ----------------------------------------------------------------
 * 通过扫描游戏目录中的特征文件/目录来识别引擎类型。
 * 检测顺序：Ren'Py → RPG Maker MV/MZ → RPG Maker Legacy → Unity → Godot
 * ================================================================ */

#include "engine.h"
#include "fsutil.h"

#include <limits.h>
#include <string.h>
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
 *   - 本工具自身 (ds游戏翻译器.exe / DeepSeekTranslator.exe / dst_server.exe)
 * 找到后将完整路径写入 out。
 * ---------------------------------------------------------------- */
static int ignored_exe_name(const WCHAR *name) {
    return wcsstr(name, L"CrashHandler") ||
           wcsstr(name, L"UnityCrashHandler") ||
           !_wcsicmp(name, L"DeepSeekTranslator.exe") ||
           !_wcsicmp(name, L"dst_godot_patch.exe") ||
           !_wcsicmp(name, L"dst_server.exe");
}

static int godot_embedded_pck_exe(const WCHAR *path);

static void exe_stem(const WCHAR *name, WCHAR *out, size_t cap) {
    size_t n = wcslen(name);
    if (n >= 4 && !_wcsicmp(name + n - 4, L".exe")) n -= 4;
    if (n >= cap) n = cap - 1;
    wmemcpy(out, name, n);
    out[n] = 0;
}

static const WCHAR *dir_leaf(const WCHAR *dir, WCHAR *scratch, size_t cap) {
    size_t n = wcslen(dir);
    while (n > 0 && (dir[n - 1] == L'\\' || dir[n - 1] == L'/')) n--;
    if (n >= cap) n = cap - 1;
    wmemcpy(scratch, dir, n);
    scratch[n] = 0;
    const WCHAR *slash = wcsrchr(scratch, L'\\');
    const WCHAR *alt = wcsrchr(scratch, L'/');
    if (!slash || (alt && alt > slash)) slash = alt;
    return slash ? slash + 1 : scratch;
}

static int exe_candidate_score(const WCHAR *dir, const WCHAR *name, const WCHAR *path) {
    WCHAR stem[MAX_PATH * 4], marker[MAX_PATH * 4], leaf_buf[MAX_PATH * 4];
    exe_stem(name, stem, MAX_PATH * 4);
    int score = 0;

    if (!path_append_suffix(marker, MAX_PATH * 4, stem, L"_Data")) return 0;
    WCHAR candidate[MAX_PATH * 4];
    path_join(candidate, MAX_PATH * 4, dir, marker);
    if (is_dir(candidate)) score += 1000;

    if (!path_append_suffix(marker, MAX_PATH * 4, stem, L".pck")) return score;
    path_join(candidate, MAX_PATH * 4, dir, marker);
    if (exists_path(candidate) && !is_dir(candidate)) score += 900;

    if (godot_embedded_pck_exe(path)) score += 800;
    if (!_wcsicmp(stem, dir_leaf(dir, leaf_buf, MAX_PATH * 4))) score += 200;
    if (!_wcsicmp(stem, L"Game") || !_wcsicmp(stem, L"RPG_RT")) score += 100;

    if (!_wcsicmp(stem, L"Config") || !_wcsicmp(stem, L"Configuration") ||
        !_wcsicmp(stem, L"Setup") || !_wcsicmp(stem, L"Installer") ||
        !_wcsicmp(stem, L"Install") || !_wcsicmp(stem, L"Uninstall") ||
        !_wcsnicmp(stem, L"unins", 5)) {
        score -= 500;
    }
    return score;
}

int find_exe(const WCHAR *dir, WCHAR *out, size_t cap) {
    WCHAR pat[MAX_PATH * 4];
    path_join(pat, MAX_PATH * 4, dir, L"*.exe");
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) return 0;
    int ok = 0;
    int best_score = INT_MIN;
    WCHAR best_name[MAX_PATH * 4] = {0};
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        if (ignored_exe_name(fd.cFileName)) continue;
        if (!_wcsicmp(fd.cFileName, L"ds游戏翻译器.exe") ||
            !_wcsicmp(fd.cFileName, L"DeepSeekTranslator.exe") ||
            !_wcsicmp(fd.cFileName, L"dst_server.exe")) continue;
        WCHAR candidate[MAX_PATH * 4];
        path_join(candidate, MAX_PATH * 4, dir, fd.cFileName);
        int score = exe_candidate_score(dir, fd.cFileName, candidate);
        if (!ok || score > best_score ||
            (score == best_score && _wcsicmp(fd.cFileName, best_name) < 0)) {
            wcsncpy(best_name, fd.cFileName, MAX_PATH * 4 - 1);
            best_name[MAX_PATH * 4 - 1] = 0;
            best_score = score;
            ok = 1;
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    if (ok) path_join(out, cap, dir, best_name);
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

    /* Win32 通配符只在最后一个路径分量生效，"dir\*_Data\il2cpp_data" 永不匹配。
       先枚举子目录定位 *_Data，再拼固定路径检查其中的 il2cpp_data。 */
    WCHAR pat[MAX_PATH * 4];
    path_join(pat, MAX_PATH * 4, dir, L"*");
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) return 0;
    int ok = 0;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
        if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..")) continue;
        size_t nl = wcslen(fd.cFileName);
        if (nl < 5 || _wcsicmp(fd.cFileName + nl - 5, L"_Data")) continue;
        path_join(p, MAX_PATH * 4, dir, fd.cFileName);
        path_join(p, MAX_PATH * 4, p, L"il2cpp_data");
        if (is_dir(p)) {
            ok = 1;
            break;
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return ok;
}

static int godot_project_marker(const WCHAR *dir) {
    WCHAR p[MAX_PATH * 4];
    path_join(p, MAX_PATH * 4, dir, L"project.godot");
    if (exists_path(p)) return 1;
    path_join(p, MAX_PATH * 4, dir, L"godot_project.binary");
    if (exists_path(p)) return 1;
    path_join(p, MAX_PATH * 4, dir, L".godot");
    if (is_dir(p)) return 1;
    return 0;
}

static int read_file_at(HANDLE h, LONGLONG offset, void *buf, DWORD size) {
    LARGE_INTEGER pos;
    pos.QuadPart = offset;
    if (!SetFilePointerEx(h, pos, NULL, FILE_BEGIN)) return 0;
    DWORD got = 0;
    return ReadFile(h, buf, size, &got, NULL) && got == size;
}

static int file_tail_is_gdpc(HANDLE h, const LARGE_INTEGER *size) {
    char magic[4];
    if (!size || size->QuadPart < 4) return 0;
    return read_file_at(h, size->QuadPart - 4, magic, sizeof(magic)) &&
           !memcmp(magic, "GDPC", sizeof(magic));
}

static int pe_has_pck_section(HANDLE h, const LARGE_INTEGER *size) {
    IMAGE_DOS_HEADER dos;
    if (!size || size->QuadPart < (LONGLONG)sizeof(dos)) return 0;
    if (!read_file_at(h, 0, &dos, sizeof(dos))) return 0;
    if (dos.e_magic != IMAGE_DOS_SIGNATURE || dos.e_lfanew <= 0) return 0;

    LONGLONG pe = (LONGLONG)dos.e_lfanew;
    if (pe > size->QuadPart - (LONGLONG)(sizeof(DWORD) + sizeof(IMAGE_FILE_HEADER))) return 0;

    DWORD sig = 0;
    IMAGE_FILE_HEADER fh;
    if (!read_file_at(h, pe, &sig, sizeof(sig)) || sig != IMAGE_NT_SIGNATURE) return 0;
    if (!read_file_at(h, pe + (LONGLONG)sizeof(sig), &fh, sizeof(fh))) return 0;
    if (fh.NumberOfSections == 0 || fh.NumberOfSections > 128) return 0;

    LONGLONG sections = pe + (LONGLONG)sizeof(sig) +
                        (LONGLONG)sizeof(fh) +
                        (LONGLONG)fh.SizeOfOptionalHeader;
    if (sections < pe || sections > size->QuadPart - (LONGLONG)sizeof(IMAGE_SECTION_HEADER)) return 0;

    for (WORD i = 0; i < fh.NumberOfSections; i++) {
        IMAGE_SECTION_HEADER sh;
        LONGLONG off = sections + (LONGLONG)i * (LONGLONG)sizeof(sh);
        if (off > size->QuadPart - (LONGLONG)sizeof(sh)) return 0;
        if (!read_file_at(h, off, &sh, sizeof(sh))) return 0;
        if (sh.Name[0] == 'p' && sh.Name[1] == 'c' && sh.Name[2] == 'k' && sh.Name[3] == 0) {
            return 1;
        }
    }
    return 0;
}

static int godot_embedded_pck_exe(const WCHAR *path) {
    HANDLE h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return 0;
    LARGE_INTEGER size;
    int ok = GetFileSizeEx(h, &size) &&
             file_tail_is_gdpc(h, &size) &&
             pe_has_pck_section(h, &size);
    CloseHandle(h);
    return ok;
}

static int has_godot_embedded_exe(const WCHAR *dir) {
    WCHAR pat[MAX_PATH * 4];
    path_join(pat, MAX_PATH * 4, dir, L"*.exe");
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) return 0;
    int found = 0;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        if (ignored_exe_name(fd.cFileName)) continue;
        WCHAR p[MAX_PATH * 4];
        path_join(p, MAX_PATH * 4, dir, fd.cFileName);
        if (godot_embedded_pck_exe(p)) {
            found = 1;
            break;
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return found;
}

/* Godot Windows exports commonly ship a sidecar .pck next to the .exe. Source
   projects use project.godot/.godot instead. Some Godot 4 Windows exports embed
   the pack in the exe as a pck PE section ending in the GDPC package marker.
   Godot detection intentionally stays behind Unity: some Unity games/mods may
   also contain unrelated .pck files. */
static int godot_export_or_project(const WCHAR *dir) {
    return has_file_pattern(dir, L"*.pck") ||
           godot_project_marker(dir) ||
           has_godot_embedded_exe(dir);
}

/* ----------------------------------------------------------------
 * detect_engine — 主引擎检测入口
 *
 * 按优先级顺序依次检查特征文件：
 *   1. Ren'Py：存在 game/ 目录且含 .rpy / .rpyc / .rpa 文件
 *   2. RPG Maker MV/MZ：支持 www/ 标准布局和根目录扁平布局
 *   3. RPG Maker Legacy：Data/ 目录含 .rxdata / .rvdata / .rvdata2
 *   4. Unity：子目录名以 _Data 结尾，再细分 Mono/IL2CPP
 *   5. Godot：存在 .pck 导出包或 project.godot/.godot 工程标记
 * 都不匹配则返回 ENGINE_UNKNOWN。
 * ---------------------------------------------------------------- */
static int copy_resolved_root(WCHAR *out, size_t cap, const WCHAR *root) {
    if (!out || cap == 0 || !root) return 0;
    size_t len = wcslen(root);
    if (len >= cap) return 0;
    wmemcpy(out, root, len + 1);
    return 1;
}

static int rpgm_index_or_owned_backup(const WCHAR *content_root) {
    WCHAR index[MAX_PATH * 4], backup[MAX_PATH * 4];
    path_join(index, MAX_PATH * 4, content_root, L"index.html");
    if (exists_path(index)) return 1;
    path_join(backup, MAX_PATH * 4, content_root, L"index.html.dst-backup");
    return exists_path(backup);
}

/* www/ 标准布局与根目录扁平布局共用同一组特征文件校验：
   index.html（或启动器部署留下的 index.html.dst-backup）、
   data\System.json、js\main.js，且 MV 的 js\rpg_core.js 与
   MZ 的 js\rmmz_core.js 至少其一。www 分支强度与扁平分支对齐，
   避免把只含 www\js + www\index.html 的普通 NW.js 应用误判为 RPG Maker。 */
static int rpgm_content_markers(const WCHAR *content_root) {
    if (!rpgm_index_or_owned_backup(content_root)) return 0;

    WCHAR system[MAX_PATH * 4], main_js[MAX_PATH * 4];
    WCHAR mv_core[MAX_PATH * 4], mz_core[MAX_PATH * 4];
    path_join(system, MAX_PATH * 4, content_root, L"data\\System.json");
    path_join(main_js, MAX_PATH * 4, content_root, L"js\\main.js");
    path_join(mv_core, MAX_PATH * 4, content_root, L"js\\rpg_core.js");
    path_join(mz_core, MAX_PATH * 4, content_root, L"js\\rmmz_core.js");
    if (!exists_path(system) || is_dir(system) ||
        !exists_path(main_js) || is_dir(main_js) ||
        ((!exists_path(mv_core) || is_dir(mv_core)) &&
         (!exists_path(mz_core) || is_dir(mz_core)))) {
        return 0;
    }
    return 1;
}

int rpgm_content_root(const WCHAR *dir, WCHAR *out, size_t cap) {
    if (!dir || !dir[0]) return 0;

    WCHAR content[MAX_PATH * 4], marker[MAX_PATH * 4];
    path_join(content, MAX_PATH * 4, dir, L"www");
    path_join(marker, MAX_PATH * 4, content, L"js");
    if (is_dir(marker) && rpgm_content_markers(content)) {
        return copy_resolved_root(out, cap, content);
    }

    path_join(marker, MAX_PATH * 4, dir, L"js");
    if (!is_dir(marker) || !rpgm_content_markers(dir)) return 0;
    return copy_resolved_root(out, cap, dir);
}

Engine detect_engine(const WCHAR *dir) {
    WCHAR p[MAX_PATH * 4];
    path_join(p, MAX_PATH * 4, dir, L"game");
    if (is_dir(p) && (has_file_pattern(p, L"*.rpy") || has_file_pattern(p, L"*.rpyc") || has_file_pattern(p, L"*.rpa"))) return ENGINE_RENPY;

    if (rpgm_content_root(dir, p, MAX_PATH * 4)) return ENGINE_RPGM_MV;

    path_join(p, MAX_PATH * 4, dir, L"Data");
    if (is_dir(p) && (has_file_pattern(p, L"*.rxdata") || has_file_pattern(p, L"*.rvdata") || has_file_pattern(p, L"*.rvdata2"))) return ENGINE_RPGM_LEGACY;

    if (find_subdir_suffix(dir, L"_Data")) return unity_is_il2cpp(dir) ? ENGINE_UNITY_IL2CPP : ENGINE_UNITY;
    if (godot_export_or_project(dir)) return ENGINE_GODOT;
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
    case ENGINE_GODOT: return L"Godot";
    default: return L"未知";
    }
}
