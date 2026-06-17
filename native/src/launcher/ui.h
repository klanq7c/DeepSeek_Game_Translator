#pragma once

/* ================================================================
 * ui.h — 启动器 UI 辅助函数与绘制接口声明
 * ----------------------------------------------------------------
 * 提供日志追加、状态栏更新、控件重绘、引擎检测刷新，
 * 以及暗色主题绘制（圆角卡片、按钮、背景）、文件夹选择、
 * 游戏启动和翻译流程入口。
 * ================================================================ */

#include "globals.h"

/* ---- 日志与状态 ---- */

/* 向活动日志列表追加一行（带时间戳），自动裁剪超出行数上限 */
void append_log(const WCHAR *fmt, ...);

/* 更新顶部状态栏文本 */
void set_status(const WCHAR *text);

/* 使指定控件的区域无效化（触发重绘），pad 为外扩像素 */
void invalidate_control_area(HWND ctl, int pad);

/* 刷新缓存卡片显示的文件大小 */
void update_cache_card(void);

/* 根据当前路径重新检测引擎并更新引擎卡片显示 */
void refresh_engine(void);

/* ---- 字体与绘制 ---- */

/* 为所有控件应用全局字体（标题/正文/等宽） */
void apply_fonts(void);

/* 绘制主窗口背景（左侧导航栏 + 主区域卡片） */
void paint_background(HWND hwnd, HDC dc);

/* 根据窗口大小重新布局所有子控件 */
void layout(HWND hwnd);

/* 自绘按钮的 WM_DRAWITEM 处理（主按钮/服务器按钮/普通按钮） */
void draw_button(const DRAWITEMSTRUCT *di);

/* ---- 用户操作 ---- */

/* 打开文件夹选择对话框，选择游戏根目录 */
void browse_folder(void);

/* 启动指定目录下的游戏 exe */
void launch_game(const WCHAR *dir);

/* 主翻译流程入口：部署 hook + 启动服务器 + 预热 + 启动游戏 */
void start_translation(void);
