/*
 * globals.h —— 启动器全局变量声明。
 *
 * 包含：UI 控件 ID 常量、控件窗口句柄、路径、服务器进程信息、字体/画刷/颜色。
 * 这些 extern 在 globals.c 中统一定义，被 ui.c、deploy.c、server_proc.c 等模块引用。
 */
#pragma once

#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <windows.h>

/* 控件 ID——必须全局唯一，主窗口与设置页分配不同范围。 */
#define IDC_PATH 101             /* 游戏路径编辑框 */
#define IDC_BROWSE 102           /* 浏览按钮 */
#define IDC_START 103            /* 启动/停止按钮 */
#define IDC_OPEN 104             /* 打开游戏目录按钮 */
#define IDC_STATUS 105           /* 状态栏 */
#define IDC_LOG 106              /* 日志列表框 */
#define IDC_ENGINE 107           /* 引擎类型下拉框 */
#define IDC_TITLE 108            /* 窗口标题静态控件 */
#define IDC_SUBTITLE 109         /* 副标题/描述 */
#define IDC_SERVER 110           /* 服务器状态指示 */
#define IDC_CACHE 111            /* 缓存状态 */
#define IDC_PATH_LABEL 112       /* "游戏路径"标签 */
#define IDC_API_CONFIG 114       /* API 配置按钮 */
#define IDC_SERVER_TOGGLE 115    /* 服务器开关 */

#define IDC_API_ENDPOINT 201     /* API 设置页：端点 URL */
#define IDC_API_MODEL 202        /* API 设置页：模型名 */
#define IDC_API_KEY 203          /* API 设置页：API Key */
#define IDC_API_SAVE 204         /* API 设置页：保存 */
#define IDC_API_CANCEL 205       /* API 设置页：取消 */

#define RAIL_W 256               /* 侧边导航栏宽度（像素） */

/* 控件窗口句柄——在 CreateWindow 时赋值，各模块按需读取。 */
extern HINSTANCE g_inst;         /* 当前模块实例（WinMain 参数） */
extern HWND g_main;             /* 主窗口 */
extern HWND g_title;
extern HWND g_subtitle;
extern HWND g_path_label;
extern HWND g_path;             /* 游戏路径编辑框 */
extern HWND g_engine;           /* 引擎选择下拉框 */
extern HWND g_server;           /* 服务器状态区 */
extern HWND g_cache;            /* 缓存状态区 */
extern HWND g_status;           /* 底部状态栏 */
extern HWND g_log;              /* 日志列表框 */
extern HWND g_btn_server;
extern HWND g_btn_api;

/* 路径：g_root = 本工具所在目录，g_root + config\ = 配置目录，g_game = 用户选择的游戏根目录 */
extern WCHAR g_root[MAX_PATH * 4];
extern WCHAR g_game[MAX_PATH * 4];

/* 服务器子进程——由 server_proc.c 管理，launcher 启动/监控/停止 dst_server.exe。 */
extern PROCESS_INFORMATION g_server_pi;
extern int g_server_started;

/* 字体——启动时由 ui.c 创建，统一风格。 */
extern HFONT g_font_title;
extern HFONT g_font_heading;
extern HFONT g_font_body;
extern HFONT g_font_small;
extern HFONT g_font_mono;       /* 等宽字体，用于日志 */
extern HFONT g_font_mono_small;

/* 预创建的画刷——控件背景色，避免每次 WM_ERASEBKGND 临时创建。 */
extern HBRUSH g_brush_page;
extern HBRUSH g_brush_edit;
extern HBRUSH g_brush_log;
extern HBRUSH g_brush_transparent;

/* 颜色常量——暗色主题色板，与 launcher UI 视觉一致。 */
extern const COLORREF C_PAGE;
extern const COLORREF C_RAIL;
extern const COLORREF C_CARD;
extern const COLORREF C_CARD_ELEV;
extern const COLORREF C_LINE;
extern const COLORREF C_LINE_BRIGHT;
extern const COLORREF C_TEXT;
extern const COLORREF C_TEXT_DIM;
extern const COLORREF C_MUTED;
extern const COLORREF C_ACCENT;
extern const COLORREF C_ACCENT_DARK;
extern const COLORREF C_ACCENT_DEEP;
extern const COLORREF C_GREEN;
extern const COLORREF C_DANGER;
extern const COLORREF C_LOG;
extern const COLORREF C_LOG_TEXT;
