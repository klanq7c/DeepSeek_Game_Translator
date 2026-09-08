/* ================================================================
 * godot_preflight_cache.h — Godot headless 预检结论缓存
 * ----------------------------------------------------------------
 * Godot 启动流程会在每次拉起游戏前运行 headless 预检（sidecar 脚本、
 * autoload、--main-pack 兼容性）。同一游戏反复启动时这些预检的结论
 * 不变，却每次要花最多数秒的进程等待。
 *
 * 本模块按"通用文件特征"缓存确定性结论：
 *   键 = 翻译器版本 + 预检类别 + 引擎 exe 特征 + 游戏目录 + 相关文件特征；
 *   任一组件变化（引擎升级、补丁/脚本更新、翻译器升级）即视为未命中。
 *
 * 只缓存确定性结论：
 *   - 预检成功（ok）可以缓存；
 *   - Godot 对参数的"明确拒绝"（命令行解析器亲口拒绝）是确定性的，
 *     也可以缓存；
 *   - 超时、无法启动等瞬态失败不入缓存，下次启动会重新预检。
 *
 * 不影响任何实时翻译路径：缓存只在启动器的启动准备阶段被查询。
 * ================================================================ */

#pragma once

#include <windows.h>

/* 重置并初始化签名串（调用方随后用 sig_add_* 追加组件）。 */
void godot_preflight_sig_init(WCHAR *out, size_t cap);

/* 追加一个文本特征段。 */
void godot_preflight_sig_add_text(WCHAR *out, size_t cap, const WCHAR *text);

/* 追加一个文件特征段：路径 + 最后修改时间 + 大小。文件不存在时特征为 0。 */
void godot_preflight_sig_add_file(WCHAR *out, size_t cap, const WCHAR *path);

/* 查询缓存结论。
   返回 1 = 命中且结论为"支持"；0 = 命中且结论为"明确拒绝"；
   -1 = 未命中（键不匹配或无记录），调用方应实际运行预检。 */
int godot_preflight_cache_get(const WCHAR *kind, const WCHAR *sig);

/* 写入一条确定性结论：result 非 0 = 支持，0 = 明确拒绝。
   瞬态失败（超时/无法启动）不得调用本函数，避免把偶发故障变成永久结论。 */
void godot_preflight_cache_put(const WCHAR *kind, const WCHAR *sig, int result);
