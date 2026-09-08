#pragma once

/* ================================================================
 * ui.h — 启动器 UI 辅助函数与绘制接口声明
 * ----------------------------------------------------------------
 * 提供日志追加、状态栏更新、控件重绘、引擎检测刷新，
 * 以及暗色主题绘制（圆角卡片、按钮、背景）、文件夹选择、
 * 游戏启动和翻译流程入口。
 * ================================================================ */

#include "globals.h"
#include "engine.h"

/* ---- 日志与状态 ---- */

/* 向活动日志列表追加一行（带时间戳），自动裁剪超出行数上限 */
void append_log(const WCHAR *fmt, ...);

/* 诊断子命令（--detect-and-exit / --deploy-and-exit / --restore-and-exit）置 1：
   append_log 额外把不带时间戳的正文以 "log=<正文>\n" 写到父进程重定向的 stdout，
   供 tests/launcher_parity 与 C# 移植逐行对比。set_status 同样以 "status=<文本>"
   镜像，使一键流程里的状态推进顺序也可比对。 */
extern int g_log_to_stdout;

/* --launch-flow-and-exit 置 1：一键流程里"真正拉起进程"的三处（游戏 exe 的
   ShellExecuteW、Godot 的 CreateProcessW、独立补丁工作进程）改为把将要执行的
   命令以 "spawn=<kind>|<exe>|<cmd>|<cwd>" 打印到 stdout 并按成功返回，其余
   决策（引擎检测、预检探测、sidecar/补丁包准备、预热扫描）全部照常真跑。
   预检探测仍然真的启动被测 exe——它的输出正是要比对的判定输入。 */
extern int g_launch_dry_run;

/* --ui-probe-and-exit 置 1：WM_CREATE 只做"画一帧所必需"的初始化（DPI、字体、
   画刷、控件、字体应用），跳过 payload 同步、日志、上次目录恢复、服务器探测与
   动画定时器。这些副作用会让同一台机器上两次渲染不同，而它们各自都已有对应的
   parity 场景（--sync-payloads / --launch-flow / --server-smoke）覆盖。 */
extern int g_ui_probe;

/* --ui-probe-and-exit 置 1：侧边栏底部的运行时标签与副标题改用中性文案。
   两个实现在这两处故意不同（它们就是用来标识"这一帧由哪版启动器画出"的），
   探针把真实值单独打印成 runtime_tag= / subtitle= 供断言，其余像素仍逐字节比对。 */
extern int g_ui_probe_neutral;

/* >= 0 时冻结动画时钟（毫秒）。呼吸灯与英雄区光束都按它取相位，
   取 0 可让 sinf/cosf 落在精确可表示的 -1/1 上，两版结果逐位一致。 */
extern int g_anim_tick_override;

/* 侧边栏底部的运行时标签与英雄区副标题（探针中性化后的取值也在这里决定）。 */
const WCHAR *ui_runtime_tag(void);
const WCHAR *ui_subtitle_text(void);

/* 主窗口过程（main.c 定义）。探针用同一个过程注册自己的窗口类，
   保证被渲染的这一帧走的就是真实的消息处理路径。 */
WNDPROC ui_probe_wndproc(void);

/* --ui-probe-and-exit <w> <h> <alive> <out.bmp>：见 ui_probe.c 顶部说明。
   返回进程退出码（0 成功、1 渲染失败、2 参数错误）。 */
int run_ui_probe(int width, int height, int alive, const WCHAR *bmp_path);

/* 运行一键流程中"服务器已就绪之后"的部分：按引擎决定启动/预热/补丁刷新的
   先后顺序。start_translation 的工作线程与 --launch-flow-and-exit 共用。 */
void run_engine_launch_flow(const WCHAR *dir, Engine engine);

/* 缓存卡片显示的文本（"%.1f MB"，文件不存在时 "0.0 MB"）。
   返回是否读到了文件属性。 */
int cache_size_text(WCHAR *out, int cap);

/* 删除共享翻译缓存文件本身，返回是否已确认清除（文件不存在也算已清除）。
   调用方负责先停掉本地服务：服务还在跑时它的内存缓存会再写回磁盘。 */
int clear_cache_file(void);

/* 把 UTF-16 文本以 UTF-8（无 BOM）写到 stdout；句柄未重定向/不可用时静默返回。 */
void write_stdout_utf8(const WCHAR *text);

/* 按引擎部署翻译钩子（ui.c 的一键流程与诊断子命令共用），返回 deploy_* 的结果。 */
int deploy_for_engine(const WCHAR *dir, Engine e);

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

/* 通过脏矩形双缓冲绘制父窗口，避免动画和调整窗口时出现撕裂。 */
void paint_background_buffered(HWND hwnd, HDC dc, const RECT *dirty);

/* 根据窗口大小重新布局所有子控件 */
void layout(HWND hwnd);

/* 应用原生深色窗口铬框（DWM 暗色标题栏），并刷新小幅动画的失效区域。 */
void apply_window_chrome(HWND hwnd);
void tick_ui_animation(HWND hwnd);

/* 为所有自绘按钮安装鼠标悬停跟踪（配合 tick_ui_animation 做渐变过渡） */
void install_button_hover_tracking(HWND hwnd);

/* 卡片内静态文字的不透明底色画刷（与所在高度的面板渐变一致） */
HBRUSH card_text_brush(int picker_label, COLORREF *out_color);
void free_card_text_brushes(void);

/* 自绘按钮的 WM_DRAWITEM 处理（主按钮/服务器按钮/普通按钮） */
void draw_button(const DRAWITEMSTRUCT *di);

/* ---- 用户操作 ---- */

/* 打开文件夹选择对话框，选择游戏根目录 */
void browse_folder(void);

/* 启动指定目录下的游戏 exe */
void launch_game(const WCHAR *dir);

/* 主翻译流程入口：部署 hook + 启动服务器 + 预热 + 启动游戏 */
void start_translation(void);

/* 一键翻译流程是否正在进行（后台线程执行服务启动/部署/预热期间为 1），
   用于拒绝并发的开始/还原/清缓存/服务器切换操作 */
int translation_flow_running(void);

/* 只移除由本启动器部署的翻译文件。 */
void restore_selected_game(void);

/* 确认后删除共享翻译缓存。 */
void clear_translation_cache(void);
