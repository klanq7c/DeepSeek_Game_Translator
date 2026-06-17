#pragma once

/* ================================================================
 * deploy.h — 翻译 hook 与插件 payload 部署声明
 * ----------------------------------------------------------------
 * 针对每种引擎类型，将对应的翻译 hook 注入到游戏目录中：
 *   - Ren'Py：注入 Python 脚本 + 中文字体
 *   - RPG Maker MV/MZ：注入 JS 脚本 + CJK 字体 + 修改 index.html
 *   - Unity Mono：部署 BepInEx 插件 + 依赖 DLL
 *   - Unity IL2CPP：部署 BepInEx be.755 + XUnity AutoTranslator + 字体回退
 * ================================================================ */

#include "globals.h"

/* 部署 Ren'Py 翻译 hook（game/iron_deepseek.rpy + CJK 字体） */
int deploy_renpy(const WCHAR *dir);

/* 部署 RPG Maker MV/MZ 翻译 hook（www/js/hook_rpgm_mv.js + CJK 字体 + index.html 注入） */
int deploy_rpgm(const WCHAR *dir);

/* 部署 Unity Mono 翻译插件（BepInEx 5/6 + UnityTranslator.dll + Newtonsoft.Json） */
int deploy_unity(const WCHAR *dir);

/* 部署 Unity IL2CPP 翻译插件（BepInEx be.755 + XUnity AutoTranslator + TMP 字体回退） */
int deploy_unity_il2cpp(const WCHAR *dir);

/* 在 payloads 目录中查找 UnityTranslator.dll 模板文件，成功返回 1 并写入 out */
int find_unity_template(WCHAR *out, size_t cap);
