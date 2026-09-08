/* ================================================================
 * deploy.c — 翻译 hook 与插件 payload 部署实现
 * ----------------------------------------------------------------
 * 本文件负责将翻译器 hook 注入到不同引擎的游戏目录中。
 * 包含：
 *   - Ren'Py Python hook（RCDATA 资源 payloads/RenPy/iron_deepseek.rpy，运行时写入 .rpy 文件）
 *   - RPG Maker MV/MZ JavaScript hook（RCDATA 资源 payloads/RPGMaker/hook_rpgm_mv.js，运行时写入 .js 文件）
 *   - Unity Mono BepInEx 插件部署（DLL 复制 + BepInEx 运行时安装）
 *   - Unity IL2CPP BepInEx be.755 + XUnity AutoTranslator 全套部署
 *   - CJK 字体部署（从系统 Fonts 复制到游戏目录供 hook 使用）
 * ================================================================ */

#include "deploy.h"
#include "embedded.h"
#include "fsutil.h"
#include "resource.h"
#include "ui.h"

#include <stdlib.h>
#include <string.h>
#include <wchar.h>

/* ----------------------------------------------------------------
 * Ren'Py 翻译 Python 脚本：payloads/RenPy/iron_deepseek.rpy（RCDATA 301）
 *
 * 脚本源码是真实文件，可直接做语法检查与测试；构建时原样嵌入启动器，
 * 部署时按字节写出，与历史上的 C 字符串常量字节级一致。
 *
 * 功能概述：
 *   1. Hook renpy.exports.say，将对话文本发送到本地 C 服务器翻译
 *   2. 渲染路径只查进程内缓存，不执行 HTTP
 *   3. 本地缓存查询与实时 API 使用独立后台线程，命中后立即刷新 interaction
 *   4. 部署 CJK 字体（ds_font.ttf/otf/ttc）替换所有 style 的 font 属性
 *   5. replace_text hook 翻译 UI 界面文本（菜单、按钮等）
 *
 * 关键设计：
 *   - _ds_memo: 内存缓存，避免重复调用本地服务器
 *   - _ds_pending: 待翻译队列，后台线程先查缓存再批量提交实时翻译
 *   - _ds_state['down_until']: 短暂熔断，服务器不可用时避免紧密重试
 *   - font_replacement_map: 将原字体映射到 CJK 字体
 * ---------------------------------------------------------------- */

/* ----------------------------------------------------------------
 * RPG Maker MV/MZ 翻译 JavaScript 脚本：payloads/RPGMaker/hook_rpgm_mv.js（RCDATA 302）
 *
 * 功能概述：
 *   1. Hook Window_Base.drawTextEx 和 drawText，将显示文本发送到本地服务器翻译
 *   2. 使用 cache_only 模式，仅缓存命中时替换（不阻塞游戏）
 *   3. 注入 CJK @font-face（ds_font.ttf/ttc），确保中文能正确渲染
 *   4. 覆盖 Window_Base.standardFontFace 和 Game_System.mainFontFace，
 *      将 CJK 字体设为首选
 *   5. 统一插件外部 CRLF 文本的缓存键，并为重复 miss 设置短冷却
 * ---------------------------------------------------------------- */

/* ----------------------------------------------------------------
 * deploy_renpy_font — 为 Ren'Py 游戏部署 CJK 字体
 *
 * Ren'Py 默认字体不含 CJK 字形，翻译后的中文会显示为方块。
 * 从 Windows 系统 Fonts 目录复制一个 CJK 字体（优先 simhei.ttf 黑体，
 * 回退 msyh.ttc 微软雅黑）到游戏的 game/ 目录，hook 脚本的
 * style 覆盖会引用此字体。
 * ---------------------------------------------------------------- */
static void deploy_renpy_font(const WCHAR *game) {
    WCHAR ttc_dst[MAX_PATH * 4], ttf_dst[MAX_PATH * 4];
    path_join(ttc_dst, MAX_PATH * 4, game, L"ds_font.ttc");
    path_join(ttf_dst, MAX_PATH * 4, game, L"ds_font.ttf");
    if (exists_path(ttc_dst) || exists_path(ttf_dst)) return;

    WCHAR windir[MAX_PATH];
    if (!GetWindowsDirectoryW(windir, MAX_PATH)) return;

    /* 优先 TTF 格式："0@file.ttc" 集合索引语法并非所有 Ren'Py 版本都支持，
       而普通 TTF 文件名在任何版本都可以工作。 */
    WCHAR src[MAX_PATH * 4];
    path_join(src, MAX_PATH * 4, windir, L"Fonts\\simhei.ttf");
    if (exists_path(src) && copy_file_safe(src, ttf_dst)) {
        append_log(L"Ren'Py：已部署中文字体（黑体）：%s", ttf_dst);
        return;
    }
    path_join(src, MAX_PATH * 4, windir, L"Fonts\\msyh.ttc");
    if (exists_path(src) && copy_file_safe(src, ttc_dst)) {
        append_log(L"Ren'Py：已部署中文字体（微软雅黑）：%s", ttc_dst);
        return;
    }
    append_log(L"Ren'Py：未找到系统中文字体（simhei.ttf/msyh.ttc），翻译文本可能显示为方块。");
}

/* ----------------------------------------------------------------
 * deploy_rpgm_font — 为 RPG Maker MV/MZ 部署 CJK 字体
 *
 * RPG Maker 在 canvas 上渲染文本，需要 @font-face 声明。
 * 将系统 CJK 字体复制到已解析内容根的 fonts/ 目录供 hook 脚本引用。
 * ---------------------------------------------------------------- */
static void deploy_rpgm_font(const WCHAR *content_root) {
    WCHAR font_dir[MAX_PATH * 4], ttf_dst[MAX_PATH * 4], ttc_dst[MAX_PATH * 4];
    path_join(font_dir, MAX_PATH * 4, content_root, L"fonts");
    ensure_dir(font_dir);
    path_join(ttf_dst, MAX_PATH * 4, font_dir, L"ds_font.ttf");
    path_join(ttc_dst, MAX_PATH * 4, font_dir, L"ds_font.ttc");
    if (exists_path(ttf_dst) || exists_path(ttc_dst)) return;

    WCHAR windir[MAX_PATH];
    if (!GetWindowsDirectoryW(windir, MAX_PATH)) return;

    WCHAR src[MAX_PATH * 4];
    path_join(src, MAX_PATH * 4, windir, L"Fonts\\simhei.ttf");
    if (exists_path(src) && copy_file_safe(src, ttf_dst)) {
        append_log(L"RPGM MV/MZ: deployed CJK font: %s", ttf_dst);
        return;
    }

    path_join(src, MAX_PATH * 4, windir, L"Fonts\\msyh.ttc");
    if (exists_path(src) && copy_file_safe(src, ttc_dst)) {
        append_log(L"RPGM MV/MZ: deployed CJK font: %s", ttc_dst);
        return;
    }

    append_log(L"RPGM MV/MZ: no system CJK font found (simhei.ttf/msyh.ttc); translated text may render as boxes.");
}

/* ----------------------------------------------------------------
 * deploy_renpy — 部署 Ren'Py 翻译 hook
 *
 * 将内嵌的 Ren'Py hook 脚本（payloads/RenPy/iron_deepseek.rpy）写入 game/iron_deepseek.rpy，
 * 并调用 deploy_renpy_font 部署 CJK 字体。
 * init 999 保证 hook 在所有其他游戏脚本之后加载。
 * ---------------------------------------------------------------- */
static int remove_stale_renpy_hook_bytecode(const WCHAR *path) {
    if (path_has_reparse_point(path, 0)) {
        append_log(L"Ren'Py: refused to remove stale hook bytecode through a reparse point: %s", path);
        SetLastError(ERROR_ACCESS_DENIED);
        return 0;
    }
    DWORD attr = GetFileAttributesW(path);
    if (attr == INVALID_FILE_ATTRIBUTES) {
        DWORD error = GetLastError();
        if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND) return 1;
        append_log(L"Ren'Py: could not inspect stale launcher hook bytecode %s (Windows error %lu).",
                   path, error);
        return 0;
    }
    if (attr & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) {
        append_log(L"Ren'Py: expected launcher hook bytecode to be a file, but found a directory: %s", path);
        return 0;
    }

    DWORD original_attr = attr;
    if ((attr & FILE_ATTRIBUTE_READONLY) &&
        !SetFileAttributesW(path, attr & ~(DWORD)FILE_ATTRIBUTE_READONLY)) {
        append_log(L"Ren'Py: could not make stale launcher hook bytecode writable %s (Windows error %lu).",
                   path, GetLastError());
        return 0;
    }
    if (delete_file_safe(path)) {
        append_log(L"Ren'Py: removed stale launcher hook bytecode: %s", path);
        return 1;
    }

    DWORD error = GetLastError();
    if (original_attr & FILE_ATTRIBUTE_READONLY) SetFileAttributesW(path, original_attr);
    append_log(L"Ren'Py: could not remove stale launcher hook bytecode %s (Windows error %lu).",
               path, error);
    return 0;
}

int deploy_renpy(const WCHAR *dir) {
    WCHAR game[MAX_PATH * 4], hook[MAX_PATH * 4], compiled_hook[MAX_PATH * 4];
    path_join(game, MAX_PATH * 4, dir, L"game");
    if (!is_dir(game)) {
        append_log(L"Ren'Py：找不到 game 目录，无法部署 hook：%s", game);
        return 0;
    }
    path_join(hook, MAX_PATH * 4, game, L"iron_deepseek.rpy");
    const char *renpy_hook = embedded_script_text(IDR_SCRIPT_RENPY_HOOK);
    if (!renpy_hook) {
        /* 资源缺失已由 embedded_script_text 记录；这里说明对用户的后果。 */
        append_log(L"Ren'Py：启动器缺少内嵌 hook 脚本，未部署 %s。请使用完整构建的启动器。", hook);
        return 0;
    }
    if (!write_text_file_utf8(hook, renpy_hook)) {
        append_log(L"Ren'Py：无法写入 hook 文件 %s（Windows 错误 %lu）。", hook, GetLastError());
        return 0;
    }
    path_join(compiled_hook, MAX_PATH * 4, game, L"iron_deepseek.rpyc");
    if (!remove_stale_renpy_hook_bytecode(compiled_hook)) return 0;
    deploy_renpy_font(game);
    append_log(L"已部署 Ren'Py hook：%s", hook);
    return 1;
}

static int backup_file_once(const WCHAR *path, const WCHAR *suffix) {
    WCHAR backup[MAX_PATH * 4];
    if (!path_append_suffix(backup, MAX_PATH * 4, path, suffix)) return 0;
    DWORD attr = GetFileAttributesW(backup);
    if (attr != INVALID_FILE_ATTRIBUTES) return !(attr & FILE_ATTRIBUTE_DIRECTORY);
    if (copy_file_if_absent_safe(path, backup)) return 1;
    return GetLastError() == ERROR_FILE_EXISTS;
}

static int write_file_bytes_atomic(const WCHAR *path, const char *data, DWORD size) {
    WCHAR temp[MAX_PATH * 4];
    if (!path_append_suffix(temp, MAX_PATH * 4, path, L".dst-tmp")) return 0;
    if (exists_path(temp) && !delete_file_safe(temp)) return 0;
    if (!write_file_bytes(temp, data, size)) return 0;
    if (move_file_safe(temp, path, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) return 1;
    append_log(L"无法将临时文件重命名为 %s（Windows 错误 %lu）。", path, GetLastError());
    delete_file_safe(temp);
    return 0;
}

/* 在 [start, end) 内做 ASCII 大小写不敏感子串查找；不依赖 NUL 结尾。 */
static const char *find_ascii_substr_nocase(const char *start, const char *end,
                                            const char *needle) {
    size_t needle_len = strlen(needle);
    if (!needle_len) return start;
    while ((size_t)(end - start) >= needle_len) {
        if (!_strnicmp(start, needle, needle_len)) return start;
        start++;
    }
    return NULL;
}

static void strip_owned_rpgm_hook_tags(const char *html, DWORD size, ByteBuf *out) {
    const char *hook_name = "hook_rpgm_mv.js";
    const char *end = html + size;
    const char *copy = html;
    const char *scan = html;
    const char *hit;
    out->data[0] = 0;

    while ((hit = strstr(scan, hook_name)) != NULL && hit < end) {
        const char *tag_start = NULL;
        const char *p = copy;
        while ((p = find_ascii_substr_nocase(p, end, "<script")) != NULL && p < hit) {
            tag_start = p;
            p += strlen("<script");
        }
        if (!tag_start) {
            scan = hit + strlen(hook_name);
            continue;
        }
        const char *open_end = strchr(tag_start, '>');
        const char *src_attr = find_ascii_substr_nocase(tag_start, end, "src");
        if (!open_end || open_end >= end || hit > open_end ||
            !src_attr || src_attr > hit || src_attr > open_end) {
            scan = hit + strlen(hook_name);
            continue;
        }
        const char *tag_end;
        const char *tag_close = open_end;
        while (tag_close > tag_start && (tag_close[-1] == ' ' || tag_close[-1] == '\t')) tag_close--;
        if (tag_close > tag_start && tag_close[-1] == '/') {
            /* 自闭合 <script .../> 没有 </script>，删除范围只到本标签末尾 */
            tag_end = open_end + 1;
        } else {
            tag_end = find_ascii_substr_nocase(open_end, end, "</script>");
            if (!tag_end) {
                scan = hit + strlen(hook_name);
                continue;
            }
            tag_end += strlen("</script>");
        }
        const char *trim_start = tag_start;
        while (trim_start > copy && (trim_start[-1] == ' ' || trim_start[-1] == '\t')) trim_start--;
        if (trim_start > copy && (trim_start[-1] == '\r' || trim_start[-1] == '\n')) {
            while (trim_start > copy && (trim_start[-1] == '\r' || trim_start[-1] == '\n')) trim_start--;
        }
        bb_add(out, copy, (size_t)(trim_start - copy));
        copy = tag_end;
        while (copy < end && (*copy == '\r' || *copy == '\n')) copy++;
        scan = copy;
    }
    bb_add(out, copy, (size_t)(end - copy));
}

/* ----------------------------------------------------------------
 * deploy_rpgm — 部署 RPG Maker MV/MZ 翻译 hook
 *
 * 1. 解析标准 www/ 或根目录扁平内容布局
 * 2. 将内嵌的 JS hook 脚本（payloads/RPGMaker/hook_rpgm_mv.js）写入内容根的 js/hook_rpgm_mv.js
 * 3. 部署 CJK 字体并修改内容根的 index.html
 * ---------------------------------------------------------------- */
int deploy_rpgm(const WCHAR *dir) {
    WCHAR content_root[MAX_PATH * 4], jsdir[MAX_PATH * 4];
    WCHAR hook[MAX_PATH * 4], index[MAX_PATH * 4];
    if (!rpgm_content_root(dir, content_root, MAX_PATH * 4)) {
        append_log(L"RPGM MV/MZ：无法解析游戏内容目录：%s", dir);
        return 0;
    }
    path_join(jsdir, MAX_PATH * 4, content_root, L"js");
    if (!is_dir(jsdir)) {
        append_log(L"RPGM MV/MZ：找不到 js 目录，无法部署 hook：%s", jsdir);
        return 0;
    }
    path_join(hook, MAX_PATH * 4, jsdir, L"hook_rpgm_mv.js");
    const char *rpgm_hook = embedded_script_text(IDR_SCRIPT_RPGM_HOOK);
    if (!rpgm_hook) {
        append_log(L"RPGM MV/MZ：启动器缺少内嵌 hook 脚本，未部署 %s。请使用完整构建的启动器。", hook);
        return 0;
    }
    if (!write_text_file_utf8(hook, rpgm_hook)) {
        append_log(L"RPGM MV/MZ：无法写入 hook 文件 %s（Windows 错误 %lu）。", hook, GetLastError());
        return 0;
    }
    deploy_rpgm_font(content_root);

    path_join(index, MAX_PATH * 4, content_root, L"index.html");
    char *html = NULL;
    DWORD sz = 0;
    if (!read_file_bytes(index, &html, &sz)) {
        append_log(L"RPGM MV/MZ：无法读取 index.html %s（Windows 错误 %lu）。", index, GetLastError());
        return 0;
    }

    const char *script = "\n<script type=\"text/javascript\" src=\"js/hook_rpgm_mv.js\"></script>\n";
    ByteBuf stripped = {0}, out = {0};
    int ok = 0;
    stripped.cap = (size_t)sz + 1;
    stripped.data = (char *)malloc(stripped.cap);
    if (!stripped.data) {
        append_log(L"RPGM MV/MZ：内存不足，无法处理 index.html。");
        goto done;
    }
    strip_owned_rpgm_hook_tags(html, sz, &stripped);

    const char *base = stripped.data;
    const char *main_ref = strstr(base, "js/main.js");
    const char *insert = NULL;
    if (main_ref) {
        insert = main_ref;
        while (insert > base && *insert != '<') insert--;
        if (*insert != '<') insert = main_ref;
    } else {
        insert = strstr(base, "</body>");
        if (!insert) insert = base + stripped.len;
    }

    out.cap = stripped.len + strlen(script) + 1;
    out.data = (char *)malloc(out.cap);
    if (!out.data) {
        append_log(L"RPGM MV/MZ：内存不足，无法生成新的 index.html。");
        goto done;
    }
    out.data[0] = 0;
    bb_add(&out, base, (size_t)(insert - base));
    bb_add(&out, script, strlen(script));
    bb_add(&out, insert, (size_t)((base + stripped.len) - insert));
    if (out.len != stripped.len + strlen(script)) {
        append_log(L"RPGM MV/MZ：生成 index.html 时内部长度不一致，已放弃写入。");
        goto done;
    }
    if (!backup_file_once(index, L".dst-backup")) goto done;
    if (!write_file_bytes_atomic(index, out.data, (DWORD)out.len)) goto done;
    ok = 1;

done:
    free(out.data);
    free(stripped.data);
    free(html);
    if (!ok) return 0;
    append_log(L"已部署 RPGM MV/MZ hook：%s", hook);
    return 1;
}

/* ======================== Unity Mono 部署辅助 ======================== */

/* 在 payloads/UnityTranslator 中查找指定文件 */
static int find_unity_payload_file(WCHAR *out, size_t cap, const WCHAR *leaf) {
    WCHAR base[MAX_PATH * 4];
    path_join(base, MAX_PATH * 4, g_root, L"payloads\\UnityTranslator");
    path_join(out, cap, base, leaf);
    return exists_path(out);
}

static int files_equal(const WCHAR *a, const WCHAR *b);

/* 查找 UnityTranslator.dll 模板（BepInEx 5 版） */
int find_unity_template(WCHAR *out, size_t cap) {
    WCHAR p[MAX_PATH * 4];
    if (!find_unity_payload_file(p, MAX_PATH * 4, L"UnityTranslator.dll")) return 0;
    size_t need = wcslen(p) + 1;
    if (need > cap) return 0;
    memcpy(out, p, need * sizeof(WCHAR));
    return 1;
}

/* 查找 UnityTranslator.BepInEx6.dll 模板（Unity 6+ BepInEx 6 版） */
static int find_unity_bepinex6_template(WCHAR *out, size_t cap) {
    WCHAR p[MAX_PATH * 4];
    if (!find_unity_payload_file(p, MAX_PATH * 4, L"UnityTranslator.BepInEx6.dll")) return 0;
    size_t need = wcslen(p) + 1;
    if (need > cap) return 0;
    memcpy(out, p, need * sizeof(WCHAR));
    return 1;
}

/* 检查文件是否为本工具内置的 Unity Mono 插件（用于 IL2CPP 部署时禁用旧文件） */
static int is_bundled_unity_mono_plugin(const WCHAR *path) {
    WCHAR src[MAX_PATH * 4];
    if (find_unity_template(src, MAX_PATH * 4) && files_equal(path, src)) return 1;
    if (find_unity_bepinex6_template(src, MAX_PATH * 4) && files_equal(path, src)) return 1;
    return 0;
}

/* 将文件重命名为 .disabled 后缀（不删除，保留备份） */
static int disable_existing_file(const WCHAR *path) {
    if (!exists_path(path)) return 0;
    WCHAR disabled[MAX_PATH * 4];
    if (!path_append_suffix(disabled, MAX_PATH * 4, path, L".disabled")) return 0;
    if (exists_path(disabled) && !delete_file_safe(disabled)) return 0;
    return move_file_safe(path, disabled, MOVEFILE_REPLACE_EXISTING | MOVEFILE_COPY_ALLOWED);
}

/* 二进制比较两个文件是否完全相同 */
static int files_equal(const WCHAR *a, const WCHAR *b) {
    char *ab = NULL, *bb = NULL;
    DWORD asz = 0, bsz = 0;
    int ok = 0;
    if (read_file_bytes(a, &ab, &asz) && read_file_bytes(b, &bb, &bsz)) {
        ok = asz == bsz && memcmp(ab, bb, asz) == 0;
    }
    free(ab);
    free(bb);
    return ok;
}

/* ======================== Unity IL2CPP 部署辅助 ======================== */

/* 在 payloads/UnityIL2CPP 中查找指定子目录 */
static int find_il2cpp_payload(WCHAR *out, size_t cap, const WCHAR *leaf) {
    WCHAR base[MAX_PATH * 4];
    path_join(base, MAX_PATH * 4, g_root, L"payloads\\UnityIL2CPP");
    path_join(out, cap, base, leaf);
    return is_dir(out);
}

/* 从 payload 目录复制单个文件到游戏目录 */
static int copy_payload_file(const WCHAR *payload_root, const WCHAR *rel, const WCHAR *game_dir) {
    WCHAR src[MAX_PATH * 4], dst[MAX_PATH * 4];
    path_join(src, MAX_PATH * 4, payload_root, rel);
    path_join(dst, MAX_PATH * 4, game_dir, rel);
    if (!exists_path(src)) return 0;
    return copy_file_safe(src, dst);
}

/* 从 payload 目录复制整个子目录树到游戏目录 */
static int copy_payload_tree(const WCHAR *payload_root, const WCHAR *rel, const WCHAR *game_dir) {
    WCHAR src[MAX_PATH * 4], dst[MAX_PATH * 4];
    path_join(src, MAX_PATH * 4, payload_root, rel);
    path_join(dst, MAX_PATH * 4, game_dir, rel);
    if (!is_dir(src)) return 0;
    return copy_tree_safe(src, dst);
}

static const char XUNITY_OWNER_MARKER_TEXT[] =
    "ds-game-translator:xunity-auto-translator:v1\n";

/* ----------------------------------------------------------------
 * pe_machine — 读取 PE 文件的机器类型
 *
 * 解析 PE 头获取 IMAGE_FILE_HEADER.Machine 字段。
 * 用于判断 GameAssembly.dll 是 x64 (0x8664) 还是其他架构。
 * pe 偏移量来自文件自身，做边界检查防止溢出。
 * ---------------------------------------------------------------- */
static int pe_machine(const WCHAR *path) {
    char *buf = NULL;
    DWORD size = 0;
    int machine = 0;
    if (!read_file_bytes(path, &buf, &size)) return 0;
    if (size >= 0x40 && buf[0] == 'M' && buf[1] == 'Z') {
        DWORD pe = *(DWORD *)(buf + 0x3c);
        /* pe 偏移量是文件控制的；做边界检查防止 (pe + 6) 溢出 */
        if (pe < size && size - pe > 6 && !memcmp(buf + pe, "PE\0\0", 4)) {
            machine = *(unsigned short *)(buf + pe + 4);
        }
    }
    free(buf);
    return machine;
}

static int unity_player_machine(const WCHAR *dir) {
    WCHAR player[MAX_PATH * 4], exe[MAX_PATH * 4];
    path_join(player, MAX_PATH * 4, dir, L"UnityPlayer.dll");
    int machine = pe_machine(player);
    if (machine) return machine;
    if (find_exe(dir, exe, MAX_PATH * 4)) return pe_machine(exe);
    return 0;
}

/* ----------------------------------------------------------------
 * write_xunity_config — 生成 XUnity.AutoTranslator 配置文件
 *
 * 写入 BepInEx/config/AutoTranslatorConfig.ini，
 * 配置 XUnity 使用本地 DeepSeek 端点 (http://127.0.0.1:19999)，
 * 语言方向 auto→zh-CN，并启用所有 UI 文本框架（UGUI/TMP/NGUI 等）。
 * ---------------------------------------------------------------- */
static int migrate_xunity_owned_ini_setting(const WCHAR *cfg, const WCHAR *owned,
                                            const char *section, const char *key,
                                            const char *value);

static int write_xunity_config(const WCHAR *dir) {
    WCHAR cfgdir[MAX_PATH * 4], cfg[MAX_PATH * 4], owned[MAX_PATH * 4];
    path_join(cfgdir, MAX_PATH * 4, dir, L"BepInEx\\config");
    if (!ensure_dir(cfgdir)) return 0;
    path_join(cfg, MAX_PATH * 4, cfgdir, L"AutoTranslatorConfig.ini");
    if (!path_append_suffix(owned, MAX_PATH * 4, cfg, L".dst-owned")) return 0;

    if (exists_path(owned) && exists_path(cfg) && !files_equal(cfg, owned)) {
        if (migrate_xunity_owned_ini_setting(
                cfg, owned, "Behaviour",
                "MaxCharactersPerTranslation", "2500")) {
            return 1;
        }
        append_log(L"Unity IL2CPP: preserved user-modified AutoTranslatorConfig.ini; deploy it again after reviewing the file.");
        return 0;
    }
    if (!exists_path(owned) && exists_path(cfg) && !backup_file_once(cfg, L".dst-backup")) {
        append_log(L"Unity IL2CPP: could not back up AutoTranslatorConfig.ini. Windows error: %lu", GetLastError());
        return 0;
    }

    /* 快照本次覆盖前的内容；ownership 记录失败时优先回滚到它，
       而不是首次部署前备份的用户原始文件。 */
    char *previous = NULL;
    DWORD previous_size = 0;
    if (exists_path(cfg) && !read_file_bytes(cfg, &previous, &previous_size)) {
        append_log(L"Unity IL2CPP: could not read the existing AutoTranslatorConfig.ini before updating it. Windows error: %lu", GetLastError());
        return 0;
    }

    static const char XUNITY_CONFIG_FMT[] =
        "[Service]\n"
        "Endpoint=DeepSeekTranslate\n"
        "FallbackEndpoint=\n"
        "\n"
        "[General]\n"
        "Language=zh-CN\n"
        "FromLanguage=auto\n"
        "\n"
        "[Files]\n"
        "Directory=Translation\\{Lang}\\Text\n"
        "OutputFile=Translation\\{Lang}\\Text\\_AutoGeneratedTranslations.txt\n"
        "SubstitutionFile=Translation\\{Lang}\\Text\\_Substitutions.txt\n"
        "PreprocessorsFile=Translation\\{Lang}\\Text\\_Preprocessors.txt\n"
        "PostprocessorsFile=Translation\\{Lang}\\Text\\_Postprocessors.txt\n"
        "\n"
        "[TextFrameworks]\n"
        "EnableIMGUI=False\n"
        "EnableUGUI=True\n"
        "EnableUIElements=True\n"
        "EnableNGUI=True\n"
        "EnableTextMeshPro=True\n"
        "EnableTextMesh=False\n"
        "EnableFairyGUI=True\n"
        "\n"
        "[Behaviour]\n"
        "MaxCharactersPerTranslation=2500\n"
        "IgnoreWhitespaceInDialogue=True\n"
        "MinDialogueChars=20\n"
        "ForceSplitTextAfterCharacters=0\n"
        "CopyToClipboard=False\n"
        "MaxClipboardCopyCharacters=2500\n"
        "ClipboardDebounceTime=1.25\n"
        "EnableUIResizing=False\n"
        "EnableBatching=True\n"
        "UseStaticTranslations=True\n"
        "OverrideFont=\n"
        "OverrideFontSize=\n"
        "OverrideFontTextMeshPro=\n"
        "FallbackFontTextMeshPro=\n"
        "ResizeUILineSpacingScale=\n"
        "ForceUIResizing=False\n"
        "IgnoreTextStartingWith=\\u180e;Confidence increased;Confidence decreased;Confidence lowered;Confidence reduced;Confidence changed;\n"
        "TextGetterCompatibilityMode=False\n"
        "GameLogTextPaths=\n"
        "RomajiPostProcessing=ReplaceMacronWithCircumflex;RemoveApostrophes;ReplaceHtmlEntities\n"
        "TranslationPostProcessing=ReplaceMacronWithCircumflex;ReplaceHtmlEntities\n"
        "RegexPostProcessing=\n"
        "CacheRegexPatternResults=False\n"
        "PersistRichTextMode=Final\n"
        "CacheRegexLookups=False\n"
        "CacheWhitespaceDifferences=False\n"
        "GenerateStaticSubstitutionTranslations=False\n"
        "GeneratePartialTranslations=False\n"
        "EnableTranslationScoping=True\n"
        "EnableSilentMode=True\n"
        "BlacklistedIMGUIPlugins=\n"
        "EnableTextPathLogging=False\n"
        "OutputUntranslatableText=False\n"
        "IgnoreVirtualTextSetterCallingRules=False\n"
        "MaxTextParserRecursion=1\n"
        "HtmlEntityPreprocessing=True\n"
        "HandleRichText=True\n"
        "EnableTranslationHelper=False\n"
        "ForceMonoModHooks=False\n"
        "InitializeHarmonyDetourBridge=False\n"
        "RedirectedResourceDetectionStrategy=AppendMongolianVowelSeparatorAndRemoveAll\n"
        "OutputTooLongText=False\n"
        "TemplateAllNumberAway=True\n"
        "ReloadTranslationsOnFileChange=True\n"
        "DisableTextMeshProScrollInEffects=False\n"
        "CacheParsedTranslations=False\n"
        "\n"
        "[Texture]\n"
        "TextureDirectory=Translation\\{Lang}\\Texture\n"
        "EnableTextureTranslation=False\n"
        "EnableTextureDumping=False\n"
        "EnableTextureToggling=False\n"
        "EnableTextureScanOnSceneLoad=False\n"
        "EnableSpriteRendererHooking=False\n"
        "LoadUnmodifiedTextures=False\n"
        "DetectDuplicateTextureNames=False\n"
        "DuplicateTextureNames=\n"
        "EnableLegacyTextureLoading=False\n"
        "TextureHashGenerationStrategy=FromImageName\n"
        "CacheTexturesInMemory=True\n"
        "EnableSpriteHooking=False\n"
        "\n"
        "[ResourceRedirector]\n"
        "PreferredStoragePath=Translation\\{Lang}\\RedirectedResources\n"
        "EnableTextAssetRedirector=False\n"
        "LogAllLoadedResources=False\n"
        "EnableDumping=False\n"
        "CacheMetadataForAllFiles=True\n"
        "\n"
        "[Http]\n"
        "UserAgent=\n"
        "DisableCertificateValidation=True\n"
        "\n"
        "[TranslationAggregator]\n"
        "Width=400\n"
        "Height=100\n"
        "EnabledTranslators=\n"
        "\n"
        "[Debug]\n"
        "EnableConsole=False\n"
        "\n"
        "[Migrations]\n"
        "Enable=True\n"
        "Tag=5.6.1\n"
        "\n"
        "[Custom]\n"
        "Url=http://127.0.0.1:19999/translate\n"
        "EnableShortDelay=False\n"
        "DisableSpamChecks=False\n"
        "\n"
        "[DeepSeek]\n"
        "Url=http://127.0.0.1:19999\n"
        "MaxBatchSize=16\n"
        "MaxConcurrency=8\n"
        "QueueWaitSeconds=30\n"
        "QueuePollIntervalSeconds=0.2\n"
        "TranslationDelay=0.1\n"
        "DisplaySafePunctuation=True\n";
    if (!write_text_file_utf8(cfg, XUNITY_CONFIG_FMT)) {
        append_log(L"Unity IL2CPP: could not write AutoTranslatorConfig.ini. Windows error: %lu", GetLastError());
        free(previous);
        return 0;
    }
    if (!copy_file_safe(cfg, owned)) {
        DWORD error = GetLastError();
        if (previous) {
            if (!write_file_bytes_atomic(cfg, previous, previous_size)) {
                append_log(L"Unity IL2CPP: could not roll back AutoTranslatorConfig.ini. Windows error: %lu", GetLastError());
            }
        } else {
            delete_file_safe(cfg);
        }
        free(previous);
        append_log(L"Unity IL2CPP: could not record config ownership; restored the previous config. Windows error: %lu", error);
        return 0;
    }
    free(previous);
    return 1;
}

/* ----------------------------------------------------------------
 * detect_unity_major — 从 globalgamemanagers 文件读取 Unity 大版本号
 *
 * <game>\*_Data\globalgamemanagers 文件头部包含版本字符串
 * （如 "6000.4.0f1" 或 "2022.3.62f3"）。扫描前 4096 字节中
 * 符合 Unity 版本模式的数字序列（5、2017-2023 或 6000+），
 * 提取大版本号。用于决定部署 BepInEx 5 还是 BepInEx 6 运行时。
 * ---------------------------------------------------------------- */
static int detect_unity_major(const WCHAR *dir) {
    WCHAR pat[MAX_PATH * 4];
    path_join(pat, MAX_PATH * 4, dir, L"*_Data");
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) return 0;
    int major = 0;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
        WCHAR ggm[MAX_PATH * 4], datadir[MAX_PATH * 4];
        path_join(datadir, MAX_PATH * 4, dir, fd.cFileName);
        path_join(ggm, MAX_PATH * 4, datadir, L"globalgamemanagers");
        char *buf = NULL;
        DWORD sz = 0;
        if (read_file_bytes(ggm, &buf, &sz)) {
            DWORD lim = sz < 4096 ? sz : 4096;
            for (DWORD i = 0; i + 5 < lim; i++) {
                if (buf[i] < '0' || buf[i] > '9') continue;
                int v = 0, d = 0;
                DWORD j = i;
                while (j < lim && buf[j] >= '0' && buf[j] <= '9' && d < 5) { v = v * 10 + (buf[j] - '0'); j++; d++; }
                if (d >= 1 && j < lim && buf[j] == '.' && (v == 5 || (v >= 2017 && v <= 2023) || v >= 6000)) { major = v; break; }
            }
            free(buf);
        }
        if (major) break;
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return major;
}

/* 检查游戏是否已有 BepInEx 6 Unity.Mono 运行时 */
static int unity_has_bepinex6_mono(const WCHAR *dir) {
    WCHAR p[MAX_PATH * 4];
    path_join(p, MAX_PATH * 4, dir, L"BepInEx\\core\\BepInEx.Unity.Mono.dll");
    return exists_path(p);
}

/* payload 缺失时只记录可复制执行的修复命令，不在部署路径中自动联网下载。 */
static void log_payload_install_command(const WCHAR *flag) {
    append_log(L"Install runtime payloads with:");
    append_log(L"  powershell -ExecutionPolicy Bypass -File scripts\\install_runtime_payloads.ps1 %s", flag);
}

/* ----------------------------------------------------------------
 * install_bepinex_mono_runtime — 安装 BepInEx Mono 运行时到游戏目录
 *
 * 复制 winhttp.dll（doorstop 加载器）、doorstop_config.ini、
 * .doorstop_version 以及 BepInEx/core 整个目录树。
 * use_bepinex6 为真时使用 payloads/UnityMonoRuntime6（Unity 6+），
 * 否则使用 payloads/UnityMonoRuntime（BepInEx 5）。
 * ---------------------------------------------------------------- */
static int find_bepinex_mono_runtime_payload(WCHAR *mono_rt, size_t cap,
                                              int use_bepinex6, int machine,
                                              const WCHAR **runtime_rel_out) {
    int use_x86 = machine == 0x014c;
    const WCHAR *runtime_rel = use_bepinex6
        ? (use_x86 ? L"payloads\\UnityMonoRuntime6X86" : L"payloads\\UnityMonoRuntime6")
        : (use_x86 ? L"payloads\\UnityMonoRuntimeX86" : L"payloads\\UnityMonoRuntime");
    path_join(mono_rt, cap, g_root, runtime_rel);
    if (!is_dir(mono_rt)) {
        append_log(L"Unity: missing BepInEx %d Mono %s runtime payload (%s).",
                   use_bepinex6 ? 6 : 5, use_x86 ? L"x86" : L"x64", runtime_rel);
        log_payload_install_command(use_bepinex6 ? L"-UnityMono6" : L"-UnityMono5");
        return 0;
    }
    if (runtime_rel_out) *runtime_rel_out = runtime_rel;
    return 1;
}

static int install_bepinex_mono_runtime(const WCHAR *dir, int use_bepinex6, int machine) {
    WCHAR mono_rt[MAX_PATH * 4];
    const WCHAR *runtime_rel = NULL;
    int use_x86 = machine == 0x014c;
    if (!find_bepinex_mono_runtime_payload(mono_rt, MAX_PATH * 4,
                                           use_bepinex6, machine, &runtime_rel)) {
        return 0;
    }

    int ok = 1;
    ok &= copy_payload_file(mono_rt, L"winhttp.dll", dir);
    ok &= copy_payload_file(mono_rt, L"doorstop_config.ini", dir);
    ok &= copy_payload_file(mono_rt, L".doorstop_version", dir);
    ok &= copy_payload_tree(mono_rt, L"BepInEx\\core", dir);
    if (!ok) {
        append_log(L"Unity: BepInEx %d Mono %s runtime deployment is incomplete; check %s.",
                   use_bepinex6 ? 6 : 5, use_x86 ? L"x86" : L"x64", runtime_rel);
        log_payload_install_command(use_bepinex6 ? L"-UnityMono6 -Force" : L"-UnityMono5 -Force");
        return 0;
    }
    append_log(L"Unity: deployed BepInEx %d (Mono) %s runtime%s.",
               use_bepinex6 ? 6 : 5, use_x86 ? L"x86" : L"x64",
               use_bepinex6 ? L" for Unity 6+" : L"");
    return 1;
}

/* 现有模组环境可能已经包含 BepInEx 的 plugins/core，却缺少 Doorstop
 * 根目录中的某个引导文件。保留所有现有文件，只补齐缺失的引导组件；
 * 仅当必需的入口程序集不完整时才复制 core 目录树。 */
static int repair_existing_bepinex_mono_runtime(const WCHAR *dir,
                                                 int use_bepinex6,
                                                 int machine) {
    WCHAR target[MAX_PATH * 4];

    /* 先扫描缺失项：用户自带的完整 BepInEx 不依赖本地 payload，
       只有确有文件需要复制时才解析并校验 payload 目录。 */
    path_join(target, MAX_PATH * 4, dir, L"winhttp.dll");
    int loader_machine = pe_machine(target);
    int need_loader = !exists_path(target) ||
                      (machine && loader_machine && loader_machine != machine);

    path_join(target, MAX_PATH * 4, dir, L"doorstop_config.ini");
    int need_config = !exists_path(target);

    path_join(target, MAX_PATH * 4, dir, L".doorstop_version");
    int need_version = !exists_path(target);

    WCHAR core_a[MAX_PATH * 4], core_b[MAX_PATH * 4];
    path_join(core_a, MAX_PATH * 4, dir, use_bepinex6
        ? L"BepInEx\\core\\BepInEx.Unity.Mono.dll"
        : L"BepInEx\\core\\BepInEx.dll");
    path_join(core_b, MAX_PATH * 4, dir, use_bepinex6
        ? L"BepInEx\\core\\BepInEx.Unity.Mono.Preloader.dll"
        : L"BepInEx\\core\\BepInEx.Preloader.dll");
    int need_core = !exists_path(core_a) || !exists_path(core_b);

    if (!need_loader && !need_config && !need_version && !need_core) return 1;

    WCHAR mono_rt[MAX_PATH * 4];
    const WCHAR *runtime_rel = NULL;
    int use_x86 = machine == 0x014c;
    if (!find_bepinex_mono_runtime_payload(mono_rt, MAX_PATH * 4,
                                           use_bepinex6, machine, &runtime_rel)) {
        return 0;
    }

    int ok = 1;
    int repaired = 0;
    if (need_loader) {
        ok &= copy_payload_file(mono_rt, L"winhttp.dll", dir);
        repaired++;
    }
    if (need_config) {
        ok &= copy_payload_file(mono_rt, L"doorstop_config.ini", dir);
        repaired++;
    }
    if (need_version) {
        ok &= copy_payload_file(mono_rt, L".doorstop_version", dir);
        repaired++;
    }
    if (need_core) {
        ok &= copy_payload_tree(mono_rt, L"BepInEx\\core", dir);
        repaired++;
    }

    if (!ok) {
        append_log(L"Unity: existing BepInEx %d Mono %s runtime repair is incomplete; check %s.",
                   use_bepinex6 ? 6 : 5, use_x86 ? L"x86" : L"x64", runtime_rel);
        log_payload_install_command(use_bepinex6 ? L"-UnityMono6 -Force" : L"-UnityMono5 -Force");
        return 0;
    }
    if (repaired) {
        append_log(L"Unity: repaired %d missing BepInEx %d Mono bootstrap component(s) without replacing existing user files.",
                   repaired, use_bepinex6 ? 6 : 5);
    }
    return 1;
}

/* ----------------------------------------------------------------
 * ensure_bepinex_mono — 确保游戏有 BepInEx Mono 运行时
 *
 * 自动安装策略：
 *   - Unity 6+ (major >= 6000) 需要 BepInEx 6 Unity.Mono 运行时
 *   - 旧版 Unity 保持使用 BepInEx 5（兼容已有安装）
 *   - 如果用户已自行安装 BepInEx，保留不覆盖（除非版本不匹配）
 * ---------------------------------------------------------------- */
static int bytes_contain_ascii(const char *bytes, DWORD size, const char *needle) {
    size_t needle_len = needle ? strlen(needle) : 0;
    if (!bytes || !needle_len || needle_len > size) return 0;
    for (DWORD i = 0; i <= size - (DWORD)needle_len; i++) {
        if (!memcmp(bytes + i, needle, needle_len)) return 1;
    }
    return 0;
}

static int find_unity_mscorlib(const WCHAR *dir, WCHAR *out, size_t cap) {
    WCHAR pattern[MAX_PATH * 4];
    path_join(pattern, MAX_PATH * 4, dir, L"*_Data");
    WIN32_FIND_DATAW fd;
    HANDLE find = FindFirstFileW(pattern, &fd);
    if (find == INVALID_HANDLE_VALUE) return 0;
    int found = 0;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
        WCHAR data_dir[MAX_PATH * 4];
        path_join(data_dir, MAX_PATH * 4, dir, fd.cFileName);
        path_join(out, cap, data_dir, L"Managed\\mscorlib.dll");
        if (exists_path(out)) {
            found = 1;
            break;
        }
    } while (FindNextFileW(find, &fd));
    FindClose(find);
    return found;
}

static int mscorlib_has_bepinex_file_writer(const WCHAR *path) {
    char *bytes = NULL;
    DWORD size = 0;
    int complete = 0;
    if (read_file_bytes(path, &bytes, &size)) {
        complete = size >= 2 && bytes[0] == 'M' && bytes[1] == 'Z' &&
                   bytes_contain_ascii(bytes, size, "WriteAllText");
    }
    free(bytes);
    return complete;
}

static int unity_mscorlib_is_clearly_stripped(const WCHAR *dir) {
    WCHAR mscorlib[MAX_PATH * 4];
    if (!find_unity_mscorlib(dir, mscorlib, MAX_PATH * 4)) return 0;
    char *bytes = NULL;
    DWORD size = 0;
    int stripped = 0;
    if (read_file_bytes(mscorlib, &bytes, &size)) {
        stripped = size >= 2 && bytes[0] == 'M' && bytes[1] == 'Z' &&
                   bytes_contain_ascii(bytes, size, "mscorlib") &&
                   bytes_contain_ascii(bytes, size, "System.IO") &&
                   !bytes_contain_ascii(bytes, size, "WriteAllText");
    }
    free(bytes);
    if (stripped) {
        append_log(L"Unity Mono: detected a clearly stripped mscorlib without System.IO.File.WriteAllText: %s", mscorlib);
    }
    return stripped;
}

static int ascii_span_equals_ignore_case(const char *span, size_t len, const char *text) {
    size_t text_len = strlen(text);
    return len == text_len && !_strnicmp(span, text, len);
}

static int find_active_ini_setting(const char *data, DWORD size, const char *section,
                                   const char *key,
                                   DWORD *line_start_out, DWORD *line_end_out,
                                   DWORD *value_start_out, DWORD *value_end_out,
                                   DWORD *section_body_out) {
    int in_target_section = 0;
    DWORD pos = 0;
    while (pos < size) {
        DWORD line_start = pos;
        while (pos < size && data[pos] != '\r' && data[pos] != '\n') pos++;
        DWORD line_end = pos;
        while (pos < size && (data[pos] == '\r' || data[pos] == '\n')) pos++;

        DWORD start = line_start;
        while (start < line_end && (data[start] == ' ' || data[start] == '\t')) start++;
        if (start >= line_end || data[start] == '#' || data[start] == ';') continue;
        if (data[start] == '[') {
            /* 跟踪当前 [section]，只在目标段内匹配键，避免改到其他段的同名键 */
            DWORD name_start = start + 1;
            DWORD close = name_start;
            while (close < line_end && data[close] != ']') close++;
            DWORD name_end = close;
            while (name_end > name_start && (data[name_end - 1] == ' ' || data[name_end - 1] == '\t')) name_end--;
            while (name_start < name_end && (data[name_start] == ' ' || data[name_start] == '\t')) name_start++;
            in_target_section = close < line_end &&
                                ascii_span_equals_ignore_case(data + name_start,
                                                              name_end - name_start, section);
            if (in_target_section && section_body_out) *section_body_out = pos;
            continue;
        }
        if (!in_target_section) continue;
        DWORD equals = start;
        while (equals < line_end && data[equals] != '=') equals++;
        if (equals >= line_end) continue;
        DWORD key_end = equals;
        while (key_end > start && (data[key_end - 1] == ' ' || data[key_end - 1] == '\t')) key_end--;
        if (!ascii_span_equals_ignore_case(data + start, key_end - start, key)) continue;

        DWORD value_start = equals + 1;
        while (value_start < line_end && (data[value_start] == ' ' || data[value_start] == '\t')) value_start++;
        DWORD value_end = line_end;
        while (value_end > value_start && (data[value_end - 1] == ' ' || data[value_end - 1] == '\t')) value_end--;
        if (line_start_out) *line_start_out = line_start;
        if (line_end_out) *line_end_out = line_end;
        if (value_start_out) *value_start_out = value_start;
        if (value_end_out) *value_end_out = value_end;
        return 1;
    }
    return 0;
}

/*
 * XUnity 首次启动时会向 INI 写入提供方默认值和端点生成的设置。因此，即使
 * 用户没有改动待迁移设置，实时文件也可能与启动器的所有权快照不同。
 *
 * 只有实时值仍与上一份所有权快照完全一致时，才更新受管值。用户改过的值
 * 因而会以失败关闭方式由 write_xunity_config 保留。两个文件都以原子方式
 * 更新；若所有权快照写入失败，则回滚实时配置。XUnity 追加的其他节保持
 * 逐字节不变，诊断日志会记录这一外部所有权边界上的每次文件系统失败。
 */
static int migrate_xunity_owned_ini_setting(const WCHAR *cfg, const WCHAR *owned,
                                            const char *section, const char *key,
                                            const char *value) {
    char *cfg_data = NULL, *owned_data = NULL;
    DWORD cfg_size = 0, owned_size = 0;
    if (!read_file_bytes(cfg, &cfg_data, &cfg_size) ||
        !read_file_bytes(owned, &owned_data, &owned_size)) {
        append_log(L"Unity IL2CPP: could not inspect the owned XUnity config migration (Windows error %lu).",
                   GetLastError());
        free(cfg_data);
        free(owned_data);
        return 0;
    }

    DWORD cfg_value_start = 0, cfg_value_end = 0;
    DWORD owned_value_start = 0, owned_value_end = 0;
    int cfg_found = find_active_ini_setting(
        cfg_data, cfg_size, section, key,
        NULL, NULL, &cfg_value_start, &cfg_value_end, NULL);
    int owned_found = find_active_ini_setting(
        owned_data, owned_size, section, key,
        NULL, NULL, &owned_value_start, &owned_value_end, NULL);
    size_t cfg_value_len = cfg_value_end - cfg_value_start;
    size_t owned_value_len = owned_value_end - owned_value_start;
    if (!cfg_found || !owned_found ||
        cfg_value_len != owned_value_len ||
        memcmp(cfg_data + cfg_value_start,
               owned_data + owned_value_start,
               cfg_value_len) != 0) {
        free(cfg_data);
        free(owned_data);
        return 0;
    }

    size_t new_value_len = strlen(value);
    if (cfg_value_len == new_value_len &&
        !memcmp(cfg_data + cfg_value_start, value, new_value_len)) {
        free(cfg_data);
        free(owned_data);
        return 1;
    }

    ByteBuf cfg_out = {0}, owned_out = {0};
    bb_add(&cfg_out, cfg_data, cfg_value_start);
    bb_add(&cfg_out, value, new_value_len);
    bb_add(&cfg_out, cfg_data + cfg_value_end, cfg_size - cfg_value_end);
    bb_add(&owned_out, owned_data, owned_value_start);
    bb_add(&owned_out, value, new_value_len);
    bb_add(&owned_out, owned_data + owned_value_end, owned_size - owned_value_end);
    if (!cfg_out.data || !owned_out.data ||
        cfg_out.len > MAXDWORD || owned_out.len > MAXDWORD) {
        append_log(L"Unity IL2CPP: could not allocate the owned XUnity config migration.");
        free(cfg_out.data);
        free(owned_out.data);
        free(cfg_data);
        free(owned_data);
        return 0;
    }

    if (!write_file_bytes_atomic(cfg, cfg_out.data, (DWORD)cfg_out.len)) {
        append_log(L"Unity IL2CPP: could not migrate the owned XUnity config (Windows error %lu).",
                   GetLastError());
        free(cfg_out.data);
        free(owned_out.data);
        free(cfg_data);
        free(owned_data);
        return 0;
    }
    if (!write_file_bytes_atomic(owned, owned_out.data, (DWORD)owned_out.len)) {
        DWORD error = GetLastError();
        if (!write_file_bytes_atomic(cfg, cfg_data, cfg_size)) {
            append_log(L"Unity IL2CPP: could not roll back the XUnity config migration (Windows error %lu).",
                       GetLastError());
        }
        append_log(L"Unity IL2CPP: could not update XUnity config ownership; restored the live config (Windows error %lu).",
                   error);
        free(cfg_out.data);
        free(owned_out.data);
        free(cfg_data);
        free(owned_data);
        return 0;
    }

    append_log(L"Unity IL2CPP: migrated %S while preserving XUnity-added config sections.", key);
    free(cfg_out.data);
    free(owned_out.data);
    free(cfg_data);
    free(owned_data);
    return 1;
}

/* 高度裁剪的 Unity Player 可能同时移除 BepInEx 所需的 corlib 写入器和
 * Unity 日志回调 API。这些外部不兼容由游戏构建产物决定，启动器无法在
 * 上游修复。下列配置修改可能让 Unity 消息不再写入 BepInEx 的辅助
 * LogOutput.log，但 Player.log 保持不变；启动器日志会记录检测结果和每次
 * 所有权冲突。修改失败时终止部署，不会虚报翻译器可用，也不会生成成功的
 * 翻译或缓存记录。 */
static int update_stripped_ini_setting(const WCHAR *path, const char *section,
                                       const char *key, const char *value,
                                       const WCHAR *label) {
    char *data = NULL;
    DWORD size = 0;
    int had_original = exists_path(path);
    if (had_original && !read_file_bytes(path, &data, &size)) {
        append_log(L"Unity Mono: could not read %s while applying stripped-runtime support (Windows error %lu).", label, GetLastError());
        return 0;
    }

    DWORD line_start = 0, line_end = 0, value_start = 0, value_end = 0, section_body = 0;
    int found = find_active_ini_setting(data, size, section, key,
                                        &line_start, &line_end,
                                        &value_start, &value_end,
                                        &section_body);
    if (found && ascii_span_equals_ignore_case(data + value_start,
                                                value_end - value_start, value)) {
        free(data);
        return 1;
    }

    WCHAR backup[MAX_PATH * 4], owned[MAX_PATH * 4];
    if (!path_append_suffix(backup, MAX_PATH * 4, path,
                            L".dst-stripped-backup") ||
        !path_append_suffix(owned, MAX_PATH * 4, path,
                            L".dst-stripped-owned")) {
        free(data);
        append_log(L"Unity Mono: recovery path for %s is too long.", label);
        return 0;
    }

    if (exists_path(owned) && (!had_original || !files_equal(path, owned))) {
        append_log(L"Unity Mono: preserved user-modified %s; stripped-runtime setting was not overwritten.", label);
        free(data);
        return 0;
    }
    if (!exists_path(owned) && exists_path(backup)) {
        append_log(L"Unity Mono: preserved %s because an unowned stripped-runtime backup already exists.", label);
        free(data);
        return 0;
    }

    ByteBuf out = {0};
    if (found) {
        bb_add(&out, data, line_start);
        bb_add(&out, key, strlen(key));
        bb_add(&out, " = ", 3);
        bb_add(&out, value, strlen(value));
        bb_add(&out, data + line_end, size - line_end);
    } else if (section_body) {
        /* 目标 section 已存在但缺少该键：插到段首，避免在文件尾追加重复 section */
        bb_add(&out, data, section_body);
        if (data[section_body - 1] != '\n') bb_add(&out, "\r\n", 2);
        bb_add(&out, key, strlen(key));
        bb_add(&out, " = ", 3);
        bb_add(&out, value, strlen(value));
        bb_add(&out, "\r\n", 2);
        bb_add(&out, data + section_body, size - section_body);
    } else {
        if (size) bb_add(&out, data, size);
        if (size && data[size - 1] != '\n' && data[size - 1] != '\r') bb_add(&out, "\r\n", 2);
        if (size) bb_add(&out, "\r\n", 2);
        bb_add(&out, "[", 1);
        bb_add(&out, section, strlen(section));
        bb_add(&out, "]\r\n", 3);
        bb_add(&out, key, strlen(key));
        bb_add(&out, " = ", 3);
        bb_add(&out, value, strlen(value));
        bb_add(&out, "\r\n", 2);
    }
    if (!out.data || out.len > MAXDWORD) {
        free(out.data);
        free(data);
        append_log(L"Unity Mono: could not allocate the updated %s.", label);
        return 0;
    }

    if (had_original && !backup_file_once(path, L".dst-stripped-backup")) {
        free(out.data);
        free(data);
        append_log(L"Unity Mono: could not back up %s before applying stripped-runtime support (Windows error %lu).", label, GetLastError());
        return 0;
    }
    WCHAR parent[MAX_PATH * 4];
    wcsncpy(parent, path, MAX_PATH * 4 - 1);
    parent[MAX_PATH * 4 - 1] = 0;
    WCHAR *slash = wcsrchr(parent, L'\\');
    if (slash) {
        *slash = 0;
        if (!ensure_dir(parent)) {
            free(out.data);
            free(data);
            append_log(L"Unity Mono: could not create the directory for %s (Windows error %lu).", label, GetLastError());
            return 0;
        }
    }
    if (!write_file_bytes_atomic(path, out.data, (DWORD)out.len)) {
        free(out.data);
        free(data);
        append_log(L"Unity Mono: could not update %s (Windows error %lu).", label, GetLastError());
        return 0;
    }
    free(out.data);

    if (!copy_file_safe(path, owned)) {
        DWORD error = GetLastError();
        /* 回滚到本次调用前的内容，而不是首次部署前备份的用户原始文件 */
        if (had_original) {
            if (!write_file_bytes_atomic(path, data, size)) {
                append_log(L"Unity Mono: could not roll back %s (Windows error %lu).", label, GetLastError());
            }
        } else {
            delete_file_safe(path);
        }
        free(data);
        append_log(L"Unity Mono: could not record ownership for %s; restored its prior state (Windows error %lu).", label, error);
        return 0;
    }
    free(data);
    append_log(L"Unity Mono: updated %s for the detected stripped runtime.", label);
    return 1;
}

static int install_stripped_unity_corlib(const WCHAR *dir) {
    WCHAR payload[MAX_PATH * 4], payload_mscorlib[MAX_PATH * 4], payload_marker[MAX_PATH * 4];
    path_join(payload, MAX_PATH * 4, g_root, L"payloads\\UnityMonoCorlib");
    path_join(payload_mscorlib, MAX_PATH * 4, payload, L"mscorlib.dll");
    path_join(payload_marker, MAX_PATH * 4, payload, L".dst-installed-by-ds");
    if (!is_dir(payload) || !exists_path(payload_marker) ||
        !mscorlib_has_bepinex_file_writer(payload_mscorlib)) {
        append_log(L"Unity Mono: official complete corlib payload is missing or invalid.");
        log_payload_install_command(L"-UnityMonoCorlib");
        return 0;
    }

    WCHAR target[MAX_PATH * 4], target_mscorlib[MAX_PATH * 4], target_marker[MAX_PATH * 4];
    path_join(target, MAX_PATH * 4, dir, L"BepInEx\\unstripped_corlib");
    path_join(target_mscorlib, MAX_PATH * 4, target, L"mscorlib.dll");
    path_join(target_marker, MAX_PATH * 4, target, L".dst-installed-by-ds");
    if (is_dir(target) && !exists_path(target_marker)) {
        if (mscorlib_has_bepinex_file_writer(target_mscorlib)) {
            append_log(L"Unity Mono: using an existing unstripped_corlib directory without replacing user files.");
            return 1;
        }
        append_log(L"Unity Mono: existing unstripped_corlib is incomplete and has no launcher ownership marker; preserved it.");
        return 0;
    }
    if (exists_path(target_marker) && !files_equal(target_marker, payload_marker)) {
        append_log(L"Unity Mono: existing unstripped_corlib ownership marker differs from the installed payload; preserved it.");
        return 0;
    }
    if (!copy_tree_safe(payload, target) || !mscorlib_has_bepinex_file_writer(target_mscorlib)) {
        append_log(L"Unity Mono: failed to deploy the official complete corlib payload (Windows error %lu).", GetLastError());
        return 0;
    }
    append_log(L"Unity Mono: deployed official Mono corlib support for the detected stripped runtime.");
    return 1;
}

/* 某些受保护的 Unity Mono 构建只移除了 Font.Internal_CreateDynamicFont 的
 * 托管声明，却保留 UnityPlayer 中的原生内部调用。BepInEx 5 预加载补丁器
 * 是在 UnityEngine.TextRenderingModule 加载前恢复该声明的最早本地边界。
 * 这个自带补丁器不会替换任何 Unity DLL。所有权以精确字节快照记录；若与
 * 用户文件冲突，则保留用户文件并让部署失败，而不是静默覆盖。 */
static int install_stripped_unity_font_patcher(const WCHAR *dir) {
    WCHAR payload[MAX_PATH * 4], target[MAX_PATH * 4], owned[MAX_PATH * 4];
    if (!find_unity_payload_file(payload, MAX_PATH * 4,
                                 L"DeepSeekUnityFontPatcher.dll")) {
        append_log(L"Unity Mono: stripped-runtime font metadata patcher payload is missing.");
        return 0;
    }
    path_join(target, MAX_PATH * 4, dir,
              L"BepInEx\\patchers\\DeepSeekUnityFontPatcher.dll");
    path_join(owned, MAX_PATH * 4, dir,
              L"BepInEx\\patchers\\DeepSeekUnityFontPatcher.dll.dst-owned");

    int had_target = exists_path(target);
    int had_owned = exists_path(owned);
    if (had_owned && had_target && !files_equal(target, owned)) {
        append_log(L"Unity Mono: preserved user-modified DeepSeekUnityFontPatcher.dll; its ownership snapshot no longer matches.");
        return 0;
    }
    if (!had_owned && had_target) {
        if (files_equal(target, payload)) {
            append_log(L"Unity Mono: using an existing unowned DeepSeekUnityFontPatcher.dll without claiming ownership.");
            return 1;
        }
        append_log(L"Unity Mono: preserved an existing unowned DeepSeekUnityFontPatcher.dll; stripped-runtime patcher was not overwritten.");
        return 0;
    }

    if (!copy_file_safe(payload, target)) {
        append_log(L"Unity Mono: could not deploy DeepSeekUnityFontPatcher.dll (Windows error %lu).", GetLastError());
        return 0;
    }
    if (!copy_file_safe(target, owned)) {
        DWORD error = GetLastError();
        if (had_owned) {
            if (had_target) copy_file_safe(owned, target);
            else delete_file_safe(target);
        } else {
            delete_file_safe(target);
        }
        append_log(L"Unity Mono: could not record font patcher ownership; restored its prior state (Windows error %lu).", error);
        return 0;
    }
    append_log(L"Unity Mono: deployed the BepInEx 5 font metadata patcher for the detected stripped runtime.");
    return 1;
}

static int ensure_stripped_unity_mono_support(const WCHAR *dir, int stripped) {
    if (!stripped) return 1;
    if (!install_stripped_unity_corlib(dir)) return 0;

    WCHAR doorstop[MAX_PATH * 4], bep_cfg[MAX_PATH * 4];
    path_join(doorstop, MAX_PATH * 4, dir, L"doorstop_config.ini");
    path_join(bep_cfg, MAX_PATH * 4, dir, L"BepInEx\\config\\BepInEx.cfg");
    if (!update_stripped_ini_setting(doorstop, "UnityMono", "dll_search_path_override",
                                     "BepInEx\\unstripped_corlib;BepInEx\\core",
                                     L"doorstop_config.ini")) return 0;
    if (!update_stripped_ini_setting(bep_cfg, "Logging", "UnityLogListening", "false",
                                     L"BepInEx.cfg")) return 0;
    return 1;
}

static int ensure_bepinex_mono(const WCHAR *dir) {
    int major = detect_unity_major(dir);
    int use_bepinex6 = major >= 6000;
    int machine = unity_player_machine(dir);
    if (machine && machine != 0x014c && machine != 0x8664) {
        append_log(L"Unity Mono: unsupported player PE machine 0x%04X; only x86 and x64 runtimes are available.", machine);
        return 0;
    }
    WCHAR bep[MAX_PATH * 4];
    path_join(bep, MAX_PATH * 4, dir, L"BepInEx");
    if (is_dir(bep)) {
        int has_bepinex6 = unity_has_bepinex6_mono(dir);
        if (use_bepinex6 && !has_bepinex6) {
            append_log(L"Unity %d (Unity 6+): existing BepInEx is not Unity.Mono 6; updating runtime files.", major);
            return install_bepinex_mono_runtime(dir, 1, machine);
        }
        return repair_existing_bepinex_mono_runtime(dir,
                                                     use_bepinex6 || has_bepinex6,
                                                     machine);
    }
    return install_bepinex_mono_runtime(dir, use_bepinex6, machine);
}

/* ----------------------------------------------------------------
 * deploy_unity — 部署 Unity Mono 翻译插件
 *
 * 流程：
 *   1. ensure_bepinex_mono — 安装/检查 BepInEx 运行时
 *   2. 根据运行时版本选择 BepInEx 5 或 6 的插件 DLL
 *   3. 复制 UnityTranslator.dll + Newtonsoft.Json.dll 到 BepInEx/plugins/
 *   4. 复制 TMP 字体资源包（如有）到 BepInEx/font/
 * ---------------------------------------------------------------- */
int deploy_unity(const WCHAR *dir) {
    WCHAR plugins[MAX_PATH * 4], dll[MAX_PATH * 4], src[MAX_PATH * 4], json_src[MAX_PATH * 4], json_dst[MAX_PATH * 4], fontfix[MAX_PATH * 4];
    path_join(plugins, MAX_PATH * 4, dir, L"BepInEx\\plugins");

    if (!ensure_bepinex_mono(dir)) return 0;
    int stripped = unity_mscorlib_is_clearly_stripped(dir);
    if (!ensure_stripped_unity_mono_support(dir, stripped)) return 0;
    int use_bepinex6 = unity_has_bepinex6_mono(dir);
    if (stripped && !use_bepinex6 &&
        !install_stripped_unity_font_patcher(dir)) return 0;
    int found_template = use_bepinex6
        ? find_unity_bepinex6_template(src, MAX_PATH * 4)
        : find_unity_template(src, MAX_PATH * 4);
    if (!found_template) {
        append_log(use_bepinex6
            ? L"Unity: missing UnityTranslator.BepInEx6.dll template."
            : L"Unity: missing UnityTranslator.dll template.");
        return 0;
    }
    path_join(dll, MAX_PATH * 4, plugins, L"UnityTranslator.dll");
    if (exists_path(dll) && !is_bundled_unity_mono_plugin(dll) &&
        !backup_file_once(dll, L".dst-backup")) {
        append_log(L"Unity: could not back up the existing UnityTranslator.dll before overwriting it (Windows error %lu).", GetLastError());
        return 0;
    }
    if (!copy_file_safe(src, dll)) {
        append_log(L"Unity：无法部署 UnityTranslator.dll（Windows 错误 %lu）。", GetLastError());
        return 0;
    }
    if (!find_unity_payload_file(json_src, MAX_PATH * 4, L"Newtonsoft.Json.dll")) {
        append_log(L"Unity: missing Newtonsoft.Json.dll dependency; UnityTranslator cannot start.");
        log_payload_install_command(L"-Newtonsoft");
        return 0;
    }
    path_join(json_dst, MAX_PATH * 4, plugins, L"Newtonsoft.Json.dll");
    if (exists_path(json_dst) && !files_equal(json_dst, json_src) &&
        !backup_file_once(json_dst, L".dst-backup")) {
        append_log(L"Unity: could not back up the existing Newtonsoft.Json.dll before overwriting it (Windows error %lu).", GetLastError());
        return 0;
    }
    if (!copy_file_safe(json_src, json_dst)) {
        append_log(L"Unity：无法部署 Newtonsoft.Json.dll（Windows 错误 %lu）。", GetLastError());
        return 0;
    }
    if (find_il2cpp_payload(fontfix, MAX_PATH * 4, L"TMPFontAssetBundles")) {
        if (!copy_payload_tree(fontfix, L"BepInEx\\font", dir)) {
            append_log(L"Unity：无法部署 TMP 字体资源目录（Windows 错误 %lu）。", GetLastError());
            return 0;
        }
    } else {
        append_log(L"Unity Mono: TMP font asset bundle payload missing; Chinese TMP glyphs may use overlay fallback.");
    }
    append_log(use_bepinex6
        ? L"Unity: deployed BepInEx 6 compatible Unity plugin: %s"
        : L"Unity: deployed BepInEx 5 compatible Unity plugin: %s", dll);
    return 1;
}

/* ----------------------------------------------------------------
 * deploy_unity_il2cpp — 部署 Unity IL2CPP 翻译插件
 *
 * 完整部署流程：
 *   1. PE 机器类型检查：仅支持 x64（0x8664）
 *   2. 禁用旧的 Mono 版 UnityTranslator.dll（如有）
 *   3. 从 payloads/UnityIL2CPP/BepInExRuntime 复制运行时：
 *      - doorstop_config.ini, winhttp.dll, .doorstop_version
 *      - dotnet/ 目录（自包含 .NET 运行时）
 *      - BepInEx/core/ 和 BepInEx/patchers/
 *   4. 从 payloads/UnityIL2CPP/XUnityAutoTranslator 复制：
 *      - XUnity.Common.dll → BepInEx/core/
 *      - XUnity.AutoTranslator/ → BepInEx/plugins/
 *      - XUnity.ResourceRedirector/ → BepInEx/plugins/
 *   5. 复制 TMP 字体资源包和字体回退插件
 *   6. 禁用旧的 Il2Cppmscorlib.dll（避免遮挡新 interop）
 *   7. 写入 XUnity 配置（AutoTranslatorConfig.ini）
 * ---------------------------------------------------------------- */
int deploy_godot(const WCHAR *dir) {
    (void)dir;
    append_log(L"Godot: enabled PO/CSV/GDScript/resource scan and cache warmup mode.");
    append_log(L"Godot: will build an external translation patch pack after warmup; original .pck files are left unchanged.");
    return 1;
}

int deploy_unity_il2cpp(const WCHAR *dir) {
    WCHAR dll[MAX_PATH * 4], pdb[MAX_PATH * 4], il2cpp_mscorlib[MAX_PATH * 4], runtime[MAX_PATH * 4], xunity[MAX_PATH * 4], fontfix[MAX_PATH * 4], fontplugin[MAX_PATH * 4], gameasm[MAX_PATH * 4], endpoint_src[MAX_PATH * 4], endpoint_dst[MAX_PATH * 4], xunity_plugin_dir[MAX_PATH * 4], xunity_owner_marker[MAX_PATH * 4];
    path_join(dll, MAX_PATH * 4, dir, L"BepInEx\\plugins\\UnityTranslator.dll");
    path_join(pdb, MAX_PATH * 4, dir, L"BepInEx\\plugins\\UnityTranslator.pdb");
    path_join(il2cpp_mscorlib, MAX_PATH * 4, dir, L"BepInEx\\core\\Il2Cppmscorlib.dll");
    path_join(gameasm, MAX_PATH * 4, dir, L"GameAssembly.dll");

    /* 只支持 x64 IL2CPP 构建 */
    int machine = pe_machine(gameasm);
    if (!machine) {
        append_log(L"Unity IL2CPP：无法确认 GameAssembly.dll 的架构，按 x64 处理：%s", gameasm);
    }
    if (machine && machine != 0x8664) {
        append_log(L"Unity IL2CPP：当前只内置 x64 插件运行时，已跳过非 x64 游戏。");
        return 0;
    }

    /* 如果存在旧的 Mono 版插件，禁用它避免冲突 */
    if (exists_path(dll)) {
        if (!is_bundled_unity_mono_plugin(dll)) {
            append_log(L"Unity IL2CPP：保留现有 UnityTranslator.dll（不是内置 Mono 模板）。");
        } else if (disable_existing_file(dll)) {
            append_log(L"Unity IL2CPP：已禁用旧的 Mono UnityTranslator.dll：%s.disabled", dll);
            disable_existing_file(pdb);
        } else {
            append_log(L"Unity IL2CPP：警告：无法禁用内置 Mono UnityTranslator.dll（Windows 错误 %lu），它可能与 IL2CPP 插件冲突：%s", GetLastError(), dll);
        }
    }

    /* 查找 IL2CPP payload 目录 */
    if (!find_il2cpp_payload(runtime, MAX_PATH * 4, L"BepInExRuntime")) {
        append_log(L"Unity IL2CPP：找不到 BepInEx IL2CPP payload。");
        log_payload_install_command(L"-UnityIL2CPP");
        return 0;
    }
    if (!find_il2cpp_payload(xunity, MAX_PATH * 4, L"XUnityAutoTranslator")) {
        append_log(L"Unity IL2CPP：找不到 XUnity AutoTranslator payload。");
        log_payload_install_command(L"-UnityIL2CPP");
        return 0;
    }

    /* 复制运行时和插件文件 */
    int ok = 1;
    ok &= copy_payload_file(runtime, L"doorstop_config.ini", dir);
    ok &= copy_payload_file(runtime, L"winhttp.dll", dir);
    ok &= copy_payload_file(runtime, L".doorstop_version", dir);
    ok &= copy_payload_tree(runtime, L"dotnet", dir);
    ok &= copy_payload_tree(runtime, L"BepInEx\\core", dir);
    ok &= copy_payload_tree(runtime, L"BepInEx\\patchers", dir);
    ok &= copy_payload_file(xunity, L"BepInEx\\core\\XUnity.Common.dll", dir);
    path_join(xunity_plugin_dir, MAX_PATH * 4, dir, L"BepInEx\\plugins\\XUnity.AutoTranslator");
    path_join(xunity_owner_marker, MAX_PATH * 4, xunity_plugin_dir, L".dst-installed-by-ds");
    int xunity_preexisting = is_dir(xunity_plugin_dir);
    if (!xunity_preexisting) {
        if (!ensure_dir(xunity_plugin_dir) ||
            !write_text_file_utf8(xunity_owner_marker, XUNITY_OWNER_MARKER_TEXT)) {
            append_log(L"Unity IL2CPP: could not record ownership for the XUnity plugin directory.");
            ok = 0;
        }
    }
    if (ok || xunity_preexisting) {
        ok &= copy_payload_tree(xunity, L"BepInEx\\plugins\\XUnity.AutoTranslator", dir);
    }
    ok &= copy_payload_tree(xunity, L"BepInEx\\plugins\\XUnity.ResourceRedirector", dir);
    path_join(endpoint_src, MAX_PATH * 4, g_root, L"payloads\\UnityIL2CPP\\DeepSeekXUnityTranslator\\DeepSeekTranslate.dll");
    path_join(endpoint_dst, MAX_PATH * 4, dir, L"BepInEx\\plugins\\XUnity.AutoTranslator\\Translators\\DeepSeekTranslate.dll");
    if (exists_path(endpoint_src)) {
        ok &= copy_file_safe(endpoint_src, endpoint_dst);
    } else {
        append_log(L"Unity IL2CPP: missing DeepSeek XUnity endpoint payload (payloads\\UnityIL2CPP\\DeepSeekXUnityTranslator\\DeepSeekTranslate.dll).");
        append_log(L"Download the full program package or run build_native.bat before deploying Unity IL2CPP.");
        ok = 0;
    }
    if (find_il2cpp_payload(fontfix, MAX_PATH * 4, L"TMPFontAssetBundles")) {
        ok &= copy_payload_tree(fontfix, L"BepInEx\\font", dir);
    } else {
        append_log(L"Unity IL2CPP: TMP font asset bundle payload missing; Chinese TMP glyphs may show as boxes.");
    }
    if (find_il2cpp_payload(fontplugin, MAX_PATH * 4, L"DeepSeekTMPFontFallback")) {
        ok &= copy_payload_tree(fontplugin, L"BepInEx\\plugins\\DeepSeekTMPFontFallback", dir);
    } else {
        append_log(L"Unity IL2CPP: TMP font fallback plugin payload missing; Chinese TMP glyphs may show as boxes.");
    }

    if (!ok) {
        append_log(L"Unity IL2CPP：插件运行时部署不完整，请检查 payloads\\UnityIL2CPP。");
        log_payload_install_command(L"-UnityIL2CPP -Force");
        return 0;
    }

    /* 禁用旧的 Il2Cppmscorlib.dll，避免遮挡新 interop 层 */
    if (exists_path(il2cpp_mscorlib)) {
        if (disable_existing_file(il2cpp_mscorlib)) {
            append_log(L"Unity IL2CPP：已禁用旧的 core\\Il2Cppmscorlib.dll，避免遮挡新 interop。");
        } else {
            append_log(L"Unity IL2CPP：警告：无法禁用旧的 core\\Il2Cppmscorlib.dll（Windows 错误 %lu），它可能遮挡新 interop 层。", GetLastError());
        }
    }

    /* 生成 XUnity 配置文件 */
    if (!write_xunity_config(dir)) return 0;
    append_log(L"Unity IL2CPP: deployed TMP Chinese system font fallback.");
    append_log(L"Unity IL2CPP：已部署 BepInEx be.755 + XUnity AutoTranslator。");
    append_log(L"Unity IL2CPP：XUnity 已配置为使用本地 DeepSeek 批量端点 http://127.0.0.1:19999。");
    return 1;
}

typedef struct {
    int removed;
    int restored;
    int preserved;
    int failed;
} RestoreStats;

static int path_missing_error(DWORD error) {
    return error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND;
}

static int restore_delete_file(const WCHAR *path, RestoreStats *stats) {
    if (path_has_reparse_point(path, 0)) {
        stats->failed++;
        append_log(L"Restore: refused to delete through a directory reparse point: %s", path);
        return 0;
    }
    DWORD attr = GetFileAttributesW(path);
    if (attr == INVALID_FILE_ATTRIBUTES) {
        DWORD error = GetLastError();
        if (path_missing_error(error)) return 1;
        stats->failed++;
        append_log(L"还原：无法检查文件 %s（Windows 错误 %lu）。", path, error);
        return 0;
    }
    if (attr & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) {
        stats->failed++;
        append_log(L"还原：应为普通文件但检测到目录或重解析点，已保留：%s", path);
        return 0;
    }

    DWORD original_attr = attr;
    if (attr & FILE_ATTRIBUTE_READONLY) {
        SetFileAttributesW(path, attr & ~(DWORD)FILE_ATTRIBUTE_READONLY);
    }
    if (delete_file_safe(path)) {
        stats->removed++;
        append_log(L"还原：已移除 %s", path);
        return 1;
    }

    DWORD error = GetLastError();
    if (original_attr & FILE_ATTRIBUTE_READONLY) SetFileAttributesW(path, original_attr);
    stats->failed++;
    append_log(L"还原：无法删除 %s（Windows 错误 %lu）。", path, error);
    return 0;
}

static int restore_remove_matching_file(const WCHAR *installed, const WCHAR *payload,
                                        const WCHAR *label, RestoreStats *stats) {
    if (!exists_path(installed)) return 1;
    if (!exists_path(payload) || !files_equal(installed, payload)) {
        stats->preserved++;
        append_log(L"还原：%s 与当前内置版本不一致，已保留：%s", label, installed);
        return 0;
    }
    return restore_delete_file(installed, stats);
}

static int restore_file_equals_text(const WCHAR *path, const char *text) {
    char *bytes = NULL;
    DWORD size = 0;
    int equal = 0;
    if (read_file_bytes(path, &bytes, &size)) {
        size_t expected = strlen(text);
        equal = expected == size && memcmp(bytes, text, expected) == 0;
    }
    free(bytes);
    return equal;
}

/* 只移除字节仍与对应载荷完全一致的文件。启动器标记只能证明初始所有权；
   用户或模组管理器编辑目录后，它无法证明每个后代文件仍归启动器所有。
   因此，逐个比较载荷文件是最接近目标的安全所有权边界。未知、新增和已修改
   的文件都会被保留并记录，不会被成功恢复的结果掩盖。 */
static void restore_remove_verified_payload_files(const WCHAR *payload_dir,
                                                  const WCHAR *installed_dir,
                                                  const WCHAR *label,
                                                  RestoreStats *stats) {
    if (path_has_reparse_point(payload_dir, 1) ||
        path_has_reparse_point(installed_dir, 1)) {
        stats->failed++;
        append_log(L"Restore: refused to enumerate %s through a reparse point: %s",
                   label, installed_dir);
        return;
    }
    WCHAR pattern[MAX_PATH * 4];
    path_join(pattern, MAX_PATH * 4, payload_dir, L"*");
    WIN32_FIND_DATAW fd;
    HANDLE find = FindFirstFileW(pattern, &fd);
    if (find == INVALID_HANDLE_VALUE) {
        DWORD error = GetLastError();
        stats->preserved++;
        append_log(L"还原：无法读取 %s payload，已保留安装目录 %s（Windows 错误 %lu）。",
                   label, installed_dir, error);
        return;
    }

    do {
        if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..")) continue;
        WCHAR payload_child[MAX_PATH * 4], installed_child[MAX_PATH * 4];
        path_join(payload_child, MAX_PATH * 4, payload_dir, fd.cFileName);
        path_join(installed_child, MAX_PATH * 4, installed_dir, fd.cFileName);
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            DWORD installed_attr = GetFileAttributesW(installed_child);
            if (installed_attr == INVALID_FILE_ATTRIBUTES) continue;
            if (!(installed_attr & FILE_ATTRIBUTE_DIRECTORY) ||
                (installed_attr & FILE_ATTRIBUTE_REPARSE_POINT)) {
                stats->preserved++;
                append_log(L"还原：%s 子目录类型已变化，已保留：%s", label, installed_child);
                continue;
            }
            restore_remove_verified_payload_files(payload_child, installed_child, label, stats);
        } else {
            restore_remove_matching_file(installed_child, payload_child, label, stats);
        }
    } while (FindNextFileW(find, &fd));
    DWORD error = GetLastError();
    FindClose(find);
    if (error != ERROR_NO_MORE_FILES) {
        stats->failed++;
        append_log(L"还原：枚举 %s payload 失败（Windows 错误 %lu）。", label, error);
    }
}

static void restore_prune_empty_dirs(const WCHAR *path, RestoreStats *stats) {
    if (path_has_reparse_point(path, 1)) {
        stats->failed++;
        append_log(L"Restore: refused to prune a directory through a reparse point: %s", path);
        return;
    }
    DWORD attr = GetFileAttributesW(path);
    if (attr == INVALID_FILE_ATTRIBUTES || !(attr & FILE_ATTRIBUTE_DIRECTORY) ||
        (attr & FILE_ATTRIBUTE_REPARSE_POINT)) return;

    WCHAR pattern[MAX_PATH * 4];
    path_join(pattern, MAX_PATH * 4, path, L"*");
    WIN32_FIND_DATAW fd;
    HANDLE find = FindFirstFileW(pattern, &fd);
    if (find != INVALID_HANDLE_VALUE) {
        do {
            if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..")) continue;
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                WCHAR child[MAX_PATH * 4];
                path_join(child, MAX_PATH * 4, path, fd.cFileName);
                restore_prune_empty_dirs(child, stats);
            }
        } while (FindNextFileW(find, &fd));
        FindClose(find);
    }
    if (RemoveDirectoryW(path)) {
        stats->removed++;
        append_log(L"还原：已移除空目录 %s", path);
    } else {
        DWORD error = GetLastError();
        if (error == ERROR_DIR_NOT_EMPTY) {
            stats->preserved++;
            append_log(L"还原：目录含有非启动器文件，已保留：%s", path);
        } else if (!path_missing_error(error)) {
            stats->failed++;
            append_log(L"还原：无法移除空目录 %s（Windows 错误 %lu）。", path, error);
        }
    }
}

static int restore_remove_owned_tree(const WCHAR *tree, const WCHAR *installed_marker,
                                     const WCHAR *payload_tree, const WCHAR *payload_marker,
                                     const WCHAR *label, RestoreStats *stats) {
    if (!exists_path(tree)) return 1;
    if (!exists_path(installed_marker) || !exists_path(payload_marker) ||
        !files_equal(installed_marker, payload_marker)) {
        stats->preserved++;
        append_log(L"还原：无法确认 %s 目录归属，已保留：%s", label, tree);
        return 0;
    }
    if (!is_dir(payload_tree)) {
        stats->preserved++;
        append_log(L"还原：缺少 %s payload，无法逐文件验证，已保留：%s", label, tree);
        return 0;
    }

    restore_remove_verified_payload_files(payload_tree, tree, label, stats);
    restore_prune_empty_dirs(tree, stats);
    return stats->failed == 0 && stats->preserved == 0;
}

static void restore_xunity_plugin(const WCHAR *dir, RestoreStats *stats) {
    WCHAR installed[MAX_PATH * 4], marker[MAX_PATH * 4], payload[MAX_PATH * 4];
    path_join(installed, MAX_PATH * 4, dir, L"BepInEx\\plugins\\XUnity.AutoTranslator");
    if (!is_dir(installed)) return;
    path_join(marker, MAX_PATH * 4, installed, L".dst-installed-by-ds");
    if (!restore_file_equals_text(marker, XUNITY_OWNER_MARKER_TEXT)) {
        stats->preserved++;
        append_log(L"还原 Unity IL2CPP：XUnity 目录不是本版本首次安装，已保留以避免删除用户运行时。");
        return;
    }
    path_join(payload, MAX_PATH * 4, g_root,
              L"payloads\\UnityIL2CPP\\XUnityAutoTranslator\\BepInEx\\plugins\\XUnity.AutoTranslator");
    if (!is_dir(payload)) {
        stats->preserved++;
        append_log(L"还原 Unity IL2CPP：缺少 XUnity payload，无法验证已安装文件，目录已保留。");
        return;
    }

    restore_remove_verified_payload_files(payload, installed, L"XUnity plugin file", stats);
    restore_delete_file(marker, stats);
    restore_prune_empty_dirs(installed, stats);
}

static void restore_renpy(const WCHAR *dir, RestoreStats *stats) {
    static const WCHAR *files[] = {
        L"game\\iron_deepseek.rpy",
        L"game\\iron_deepseek.rpyc",
        L"game\\ds_font.ttf",
        L"game\\ds_font.ttc",
        L"game\\ds_font.otf"
    };
    for (size_t i = 0; i < sizeof(files) / sizeof(files[0]); i++) {
        WCHAR path[MAX_PATH * 4];
        path_join(path, MAX_PATH * 4, dir, files[i]);
        restore_delete_file(path, stats);
    }
}

static void restore_rpgm(const WCHAR *dir, RestoreStats *stats) {
    WCHAR content_root[MAX_PATH * 4], index[MAX_PATH * 4], backup[MAX_PATH * 4];
    if (!rpgm_content_root(dir, content_root, MAX_PATH * 4)) {
        stats->failed++;
        append_log(L"还原 RPG Maker：无法解析游戏内容目录，未修改文件。");
        return;
    }
    int index_ok = 1;
    path_join(index, MAX_PATH * 4, content_root, L"index.html");
    if (!path_append_suffix(backup, MAX_PATH * 4, index, L".dst-backup")) {
        stats->failed++;
        append_log(L"Restore RPG Maker: recovery path is too long.");
        return;
    }

    if (exists_path(index) && !is_dir(index)) {
        char *html = NULL;
        DWORD size = 0;
        if (read_file_bytes(index, &html, &size)) {
            ByteBuf stripped = {0};
            stripped.cap = (size_t)size + 1;
            stripped.data = (char *)malloc(stripped.cap);
            if (!stripped.data) {
                index_ok = 0;
                stats->failed++;
                append_log(L"还原 RPG Maker：内存不足，未修改 index.html。");
            } else {
                stripped.data[0] = 0;
                strip_owned_rpgm_hook_tags(html, size, &stripped);
                if (stripped.len != size) {
                    if (write_file_bytes_atomic(index, stripped.data, (DWORD)stripped.len)) {
                        stats->removed++;
                        append_log(L"还原 RPG Maker：已从 index.html 移除启动器脚本标签。");
                    } else {
                        index_ok = 0;
                        stats->failed++;
                        append_log(L"还原 RPG Maker：无法更新 index.html（Windows 错误 %lu）。", GetLastError());
                    }
                }
                free(stripped.data);
            }
            free(html);
        } else {
            index_ok = 0;
            stats->failed++;
            append_log(L"还原 RPG Maker：无法读取 index.html（Windows 错误 %lu）。", GetLastError());
        }
    } else if (!exists_path(index) && exists_path(backup)) {
        if (move_file_safe(backup, index, MOVEFILE_WRITE_THROUGH)) {
            stats->restored++;
            append_log(L"还原 RPG Maker：已从备份恢复 index.html。");
        } else {
            index_ok = 0;
            stats->failed++;
            append_log(L"还原 RPG Maker：无法恢复 index.html（Windows 错误 %lu）。", GetLastError());
        }
    } else if (is_dir(index)) {
        index_ok = 0;
        stats->failed++;
        append_log(L"还原 RPG Maker：index.html 是目录，未修改。");
    }

    if (index_ok && exists_path(index)) restore_delete_file(backup, stats);

    static const WCHAR *files[] = {
        L"js\\hook_rpgm_mv.js",
        L"fonts\\ds_font.ttf",
        L"fonts\\ds_font.ttc",
        L"fonts\\ds_font.otf"
    };
    for (size_t i = 0; i < sizeof(files) / sizeof(files[0]); i++) {
        WCHAR path[MAX_PATH * 4];
        path_join(path, MAX_PATH * 4, content_root, files[i]);
        restore_delete_file(path, stats);
    }
}

static void restore_godot(const WCHAR *dir, RestoreStats *stats) {
    static const WCHAR *files[] = {
        L"dst_godot_runtime.gd",
        L"dst_godot_patch.pck",
        L"dst_godot_patch.next.pck",
        L"dst_godot_patch.building"
    };
    for (size_t i = 0; i < sizeof(files) / sizeof(files[0]); i++) {
        WCHAR path[MAX_PATH * 4];
        path_join(path, MAX_PATH * 4, dir, files[i]);
        restore_delete_file(path, stats);
    }

    WCHAR launcher[MAX_PATH * 4], marker[MAX_PATH * 4];
    path_join(launcher, MAX_PATH * 4, dir, L"dst_godot_patch.exe");
    path_join(marker, MAX_PATH * 4, dir, L"dst_godot_patch.exe.dst-owned");
    if (exists_path(marker)) {
        restore_delete_file(launcher, stats);
        restore_delete_file(marker, stats);
    } else if (exists_path(launcher)) {
        stats->preserved++;
        append_log(L"还原 Godot：dst_godot_patch.exe 没有启动器所有权标记，已保留。");
    }
}

/* deploy_unity 覆盖用户已有的插件文件前会留下 .dst-backup 一次性备份。
   仅当当前文件仍与内置版本一致时才移除它并恢复备份；已被改动的文件和备份都保留。 */
static void restore_unity_plugin_with_backup(const WCHAR *installed, int matches_bundled,
                                             RestoreStats *stats) {
    WCHAR backup[MAX_PATH * 4];
    if (!path_append_suffix(backup, MAX_PATH * 4, installed, L".dst-backup")) {
        stats->failed++;
        append_log(L"Restore Unity: plugin recovery path is too long.");
        return;
    }
    if (exists_path(installed)) {
        if (!matches_bundled) {
            stats->preserved++;
            append_log(L"还原 Unity：%s 与当前内置版本不一致，已作为用户文件保留。", installed);
            return;
        }
        if (!restore_delete_file(installed, stats)) return;
    }
    if (!exists_path(backup)) return;
    if (move_file_safe(backup, installed, MOVEFILE_WRITE_THROUGH)) {
        stats->restored++;
        append_log(L"还原 Unity：已从备份恢复 %s。", installed);
    } else {
        stats->failed++;
        append_log(L"还原 Unity：无法从备份恢复 %s（Windows 错误 %lu）。", installed, GetLastError());
    }
}

static void restore_unity_mono_plugin(const WCHAR *dir, RestoreStats *stats) {
    WCHAR installed[MAX_PATH * 4];
    path_join(installed, MAX_PATH * 4, dir, L"BepInEx\\plugins\\UnityTranslator.dll");
    restore_unity_plugin_with_backup(installed, is_bundled_unity_mono_plugin(installed), stats);
}

/* Newtonsoft.Json.dll 是共享依赖：没有备份说明部署时未覆盖用户文件，保持原样；
   有备份说明部署时覆盖了用户自带版本，还原时把它换回来。 */
static void restore_unity_mono_newtonsoft(const WCHAR *dir, RestoreStats *stats) {
    WCHAR installed[MAX_PATH * 4], backup[MAX_PATH * 4], payload[MAX_PATH * 4];
    path_join(installed, MAX_PATH * 4, dir, L"BepInEx\\plugins\\Newtonsoft.Json.dll");
    if (!path_append_suffix(backup, MAX_PATH * 4, installed, L".dst-backup")) {
        stats->failed++;
        append_log(L"Restore Unity: Newtonsoft.Json.dll recovery path is too long.");
        return;
    }
    if (!exists_path(backup)) return;
    if (exists_path(installed) &&
        (!find_unity_payload_file(payload, MAX_PATH * 4, L"Newtonsoft.Json.dll") ||
         !files_equal(installed, payload))) {
        stats->preserved++;
        append_log(L"还原 Unity：Newtonsoft.Json.dll 在部署后已被改动，文件和备份均已保留。");
        return;
    }
    DWORD flags = MOVEFILE_WRITE_THROUGH | (exists_path(installed) ? MOVEFILE_REPLACE_EXISTING : 0);
    if (move_file_safe(backup, installed, flags)) {
        stats->restored++;
        append_log(L"还原 Unity：已从备份恢复原 Newtonsoft.Json.dll。");
    } else {
        stats->failed++;
        append_log(L"还原 Unity：无法恢复 Newtonsoft.Json.dll 备份（Windows 错误 %lu）。", GetLastError());
    }
}

static void restore_stripped_unity_font_patcher(const WCHAR *dir,
                                                RestoreStats *stats) {
    WCHAR installed[MAX_PATH * 4], owned[MAX_PATH * 4];
    path_join(installed, MAX_PATH * 4, dir,
              L"BepInEx\\patchers\\DeepSeekUnityFontPatcher.dll");
    path_join(owned, MAX_PATH * 4, dir,
              L"BepInEx\\patchers\\DeepSeekUnityFontPatcher.dll.dst-owned");
    if (!exists_path(owned)) {
        if (exists_path(installed)) {
            stats->preserved++;
            append_log(L"Restore Unity Mono: DeepSeekUnityFontPatcher.dll has no launcher ownership snapshot and was preserved.");
        }
        return;
    }
    if (exists_path(installed) && !files_equal(installed, owned)) {
        stats->preserved++;
        append_log(L"Restore Unity Mono: DeepSeekUnityFontPatcher.dll changed after deployment; the file and ownership snapshot were preserved.");
        return;
    }
    if (exists_path(installed) && !restore_delete_file(installed, stats)) return;
    restore_delete_file(owned, stats);
}

static void restore_stripped_ini_config(const WCHAR *path, const WCHAR *label,
                                        RestoreStats *stats) {
    WCHAR backup[MAX_PATH * 4], owned[MAX_PATH * 4];
    if (!path_append_suffix(backup, MAX_PATH * 4, path,
                            L".dst-stripped-backup") ||
        !path_append_suffix(owned, MAX_PATH * 4, path,
                            L".dst-stripped-owned")) {
        stats->failed++;
        append_log(L"Restore Unity Mono: recovery path for %s is too long.", label);
        return;
    }
    if (!exists_path(owned)) return;

    if (exists_path(path) && !files_equal(path, owned)) {
        stats->preserved++;
        append_log(L"Restore Unity Mono: %s changed after stripped-runtime deployment; config and recovery metadata were preserved.", label);
        return;
    }
    if (exists_path(backup)) {
        if (!copy_file_safe(backup, path)) {
            stats->failed++;
            append_log(L"Restore Unity Mono: could not restore %s (Windows error %lu).", label, GetLastError());
            return;
        }
        stats->restored++;
        append_log(L"Restore Unity Mono: restored the original %s.", label);
        restore_delete_file(backup, stats);
    } else if (exists_path(path) && !restore_delete_file(path, stats)) {
        return;
    }
    restore_delete_file(owned, stats);
}

static void restore_unity_mono(const WCHAR *dir, RestoreStats *stats) {
    restore_unity_mono_plugin(dir, stats);
    restore_unity_mono_newtonsoft(dir, stats);
    restore_stripped_unity_font_patcher(dir, stats);

    WCHAR doorstop[MAX_PATH * 4], bep_cfg[MAX_PATH * 4];
    path_join(doorstop, MAX_PATH * 4, dir, L"doorstop_config.ini");
    path_join(bep_cfg, MAX_PATH * 4, dir, L"BepInEx\\config\\BepInEx.cfg");
    restore_stripped_ini_config(doorstop, L"doorstop_config.ini", stats);
    restore_stripped_ini_config(bep_cfg, L"BepInEx.cfg", stats);

    WCHAR corlib[MAX_PATH * 4], installed_marker[MAX_PATH * 4];
    WCHAR payload_tree[MAX_PATH * 4], payload_marker[MAX_PATH * 4];
    path_join(corlib, MAX_PATH * 4, dir, L"BepInEx\\unstripped_corlib");
    path_join(installed_marker, MAX_PATH * 4, corlib, L".dst-installed-by-ds");
    path_join(payload_tree, MAX_PATH * 4, g_root, L"payloads\\UnityMonoCorlib");
    path_join(payload_marker, MAX_PATH * 4, payload_tree, L".dst-installed-by-ds");
    restore_remove_owned_tree(corlib, installed_marker, payload_tree, payload_marker,
                              L"Unity Mono complete corlib", stats);
}

static void restore_xunity_config(const WCHAR *dir, RestoreStats *stats) {
    WCHAR cfg[MAX_PATH * 4], backup[MAX_PATH * 4], owned[MAX_PATH * 4];
    path_join(cfg, MAX_PATH * 4, dir, L"BepInEx\\config\\AutoTranslatorConfig.ini");
    if (!path_append_suffix(backup, MAX_PATH * 4, cfg, L".dst-backup") ||
        !path_append_suffix(owned, MAX_PATH * 4, cfg, L".dst-owned")) {
        stats->failed++;
        append_log(L"Restore Unity IL2CPP: XUnity recovery path is too long.");
        return;
    }

    if (!exists_path(owned)) return;
    if (exists_path(cfg) && !files_equal(cfg, owned)) {
        stats->preserved++;
        append_log(L"还原 Unity IL2CPP：AutoTranslatorConfig.ini 已被用户修改，配置和备份均已保留。");
        return;
    }

    if (exists_path(backup)) {
        if (copy_file_safe(backup, cfg)) {
            stats->restored++;
            append_log(L"还原 Unity IL2CPP：已恢复原 AutoTranslatorConfig.ini。");
            restore_delete_file(backup, stats);
        } else {
            stats->failed++;
            append_log(L"还原 Unity IL2CPP：无法恢复配置备份（Windows 错误 %lu）。", GetLastError());
            return;
        }
    } else {
        restore_delete_file(cfg, stats);
    }
    restore_delete_file(owned, stats);
}

static void restore_unity_il2cpp(const WCHAR *dir, RestoreStats *stats) {
    WCHAR installed[MAX_PATH * 4], payload[MAX_PATH * 4];

    path_join(installed, MAX_PATH * 4, dir, L"BepInEx\\plugins\\XUnity.AutoTranslator\\Translators\\DeepSeekTranslate.dll");
    path_join(payload, MAX_PATH * 4, g_root, L"payloads\\UnityIL2CPP\\DeepSeekXUnityTranslator\\DeepSeekTranslate.dll");
    restore_remove_matching_file(installed, payload, L"DeepSeek XUnity endpoint", stats);
    restore_xunity_plugin(dir, stats);

    WCHAR tree[MAX_PATH * 4], installed_marker[MAX_PATH * 4];
    WCHAR payload_tree[MAX_PATH * 4], payload_marker[MAX_PATH * 4];
    path_join(tree, MAX_PATH * 4, dir, L"BepInEx\\plugins\\DeepSeekTMPFontFallback");
    path_join(installed_marker, MAX_PATH * 4, tree, L"DeepSeekTMPFontFallback.dll");
    path_join(payload_tree, MAX_PATH * 4, g_root,
              L"payloads\\UnityIL2CPP\\DeepSeekTMPFontFallback\\BepInEx\\plugins\\DeepSeekTMPFontFallback");
    path_join(payload_marker, MAX_PATH * 4, payload_tree, L"DeepSeekTMPFontFallback.dll");
    restore_remove_owned_tree(tree, installed_marker, payload_tree, payload_marker,
                              L"DeepSeek TMP 字体回退", stats);

    WCHAR disabled[MAX_PATH * 4], disabled_pdb[MAX_PATH * 4];
    path_join(disabled, MAX_PATH * 4, dir, L"BepInEx\\plugins\\UnityTranslator.dll.disabled");
    path_join(disabled_pdb, MAX_PATH * 4, dir, L"BepInEx\\plugins\\UnityTranslator.pdb.disabled");
    if (exists_path(disabled)) {
        if (is_bundled_unity_mono_plugin(disabled)) {
            if (restore_delete_file(disabled, stats)) restore_delete_file(disabled_pdb, stats);
        } else {
            stats->preserved++;
            append_log(L"还原 Unity IL2CPP：禁用的 UnityTranslator.dll 无法确认归属，已保留。");
        }
    }

    WCHAR mscorlib[MAX_PATH * 4], mscorlib_disabled[MAX_PATH * 4];
    path_join(mscorlib, MAX_PATH * 4, dir, L"BepInEx\\core\\Il2Cppmscorlib.dll");
    if (!path_append_suffix(mscorlib_disabled, MAX_PATH * 4, mscorlib,
                            L".disabled")) {
        stats->failed++;
        append_log(L"Restore Unity IL2CPP: Il2Cppmscorlib recovery path is too long.");
        return;
    }
    if (exists_path(mscorlib_disabled)) {
        if (exists_path(mscorlib)) {
            stats->preserved++;
            append_log(L"还原 Unity IL2CPP：Il2Cppmscorlib.dll 已存在，禁用备份已保留以避免覆盖。");
        } else if (move_file_safe(mscorlib_disabled, mscorlib, MOVEFILE_WRITE_THROUGH)) {
            stats->restored++;
            append_log(L"还原 Unity IL2CPP：已恢复原 Il2Cppmscorlib.dll。");
        } else {
            stats->failed++;
            append_log(L"还原 Unity IL2CPP：无法恢复 Il2Cppmscorlib.dll（Windows 错误 %lu）。", GetLastError());
        }
    }

    restore_xunity_config(dir, stats);
}

int restore_game(const WCHAR *dir, Engine engine) {
    RestoreStats stats = {0};
    if (!dir || !is_dir(dir) || path_has_reparse_point(dir, 1)) {
        append_log(L"还原：游戏目录无效。");
        return 0;
    }

    if (engine == ENGINE_RENPY) restore_renpy(dir, &stats);
    else if (engine == ENGINE_RPGM_MV) restore_rpgm(dir, &stats);
    else if (engine == ENGINE_UNITY) restore_unity_mono(dir, &stats);
    else if (engine == ENGINE_UNITY_IL2CPP) restore_unity_il2cpp(dir, &stats);
    else if (engine == ENGINE_GODOT) restore_godot(dir, &stats);
    else if (engine == ENGINE_RPGM_LEGACY) {
        append_log(L"还原 RPG Maker XP/VX：启动器未向游戏目录部署文件，无需处理。");
    } else {
        stats.failed++;
        append_log(L"还原：未知引擎，未修改游戏目录。");
    }

    append_log(L"还原汇总：移除 %d，恢复 %d，保留 %d，失败 %d。",
               stats.removed, stats.restored, stats.preserved, stats.failed);
    return stats.failed == 0 && stats.preserved == 0;
}
