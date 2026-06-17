#pragma once

/* ================================================================
 * engine.h — 游戏引擎类型枚举与自动检测声明
 * ----------------------------------------------------------------
 * 提供基于目录特征的引擎识别，支持 Ren'Py、RPG Maker (MV/MZ 与
 * 旧版 XP/VX/VXAce)、Unity (Mono 与 IL2CPP)。
 * ================================================================ */

#include "globals.h"

/* 支持的引擎类型枚举 */
typedef enum {
    ENGINE_UNKNOWN,       /* 未识别 */
    ENGINE_RENPY,         /* Ren'Py (.rpy / .rpyc / .rpa) */
    ENGINE_RPGM_MV,       /* RPG Maker MV / MZ (www/index.html) */
    ENGINE_UNITY,         /* Unity Mono (BepInEx) */
    ENGINE_UNITY_IL2CPP,  /* Unity IL2CPP (XUnity) */
    ENGINE_RPGM_LEGACY    /* RPG Maker XP / VX / VXAce (.rxdata / .rvdata) */
} Engine;

/* ---------- 目录探测辅助函数 ---------- */

/* 在 dir 下查找匹配 pattern（通配符）的文件，找到返回 1 */
int has_file_pattern(const WCHAR *dir, const WCHAR *pattern);

/* 在 dir 的子目录中查找以 suffix 结尾的目录名，找到返回 1 */
int find_subdir_suffix(const WCHAR *dir, const WCHAR *suffix);

/* 在 dir 下查找第一个合理的游戏可执行文件（排除崩溃报告和本工具），
 * 成功时将完整路径写入 out，返回 1 */
int find_exe(const WCHAR *dir, WCHAR *out, size_t cap);

/* 判断 Unity 游戏是否使用 IL2CPP 后端（检查 GameAssembly.dll 或 il2cpp_data 目录） */
int unity_is_il2cpp(const WCHAR *dir);

/* ---------- 主检测入口 ---------- */

/* 根据目录特征自动检测引擎类型 */
Engine detect_engine(const WCHAR *dir);

/* 返回引擎的可读中文名称 */
const WCHAR *engine_name(Engine e);
