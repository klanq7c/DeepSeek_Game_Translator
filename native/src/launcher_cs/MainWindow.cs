using System;
using System.Runtime.InteropServices;
using System.Text;
using System.Threading;

namespace DstLauncher
{
    /*
     * MainWindow —— native/src/launcher/main.c 的 wndproc / wWinMain 与 ui.c 中
     * 与窗口绑定的部分（append_log 写列表框、set_status、update_cache_card、
     * refresh_engine、apply_fonts、apply_window_chrome、browse_folder、
     * start_translation、restore_selected_game、clear_translation_cache）。
     *
     * 控件、样式、创建顺序、消息处理顺序都与 C 版一一对应：tests\launcher_parity
     * 的 --ui-probe 会把客户区渲染成位图逐字节比对，控件的创建顺序会影响 z 序，
     * 布局字段则逐行比对，所以这里不做结构性重排。
     */
    internal static class MainWindow
    {
        private const string WindowClass = "DSTNativeLauncherCs";

        /* ---- 控件句柄（对应 globals.c 的 g_*） ---- */
        internal static IntPtr Instance;
        internal static IntPtr Hwnd;
        internal static IntPtr Title, Subtitle, PathLabel, Path, EngineLabel, ServerLabel, CacheLabel, Status, LogList;
        internal static IntPtr BtnServer, BtnApi, BtnRestore, BtnClearCache;

        /* 一键翻译流程进行中标志（g_start_flow_running）：期间拒绝重复的开始/还原/
           清缓存/服务器切换，避免重复部署或并发 patch worker 争抢同一个 pck。 */
        private static volatile bool _startFlowRunning;

        internal static bool TranslationFlowRunning { get { return _startFlowRunning; } }

        /* 日志水平滚动的最大像素宽度（随最长行动态增长） */
        private static int _logExtentPx;

        private const int LogMaxLines = 2000;

        /* 侧边栏底部的运行时标签与副标题。两个实现故意不同——它们就是用来说明
           "这个窗口由哪一版启动器画出来"的。--ui-probe 会把真实值单独打印出来
           供 parity 断言，同时用中性文案渲染，使其余像素仍可逐字节比对。 */
        internal const string RealRuntimeTag = "C# managed runtime";
        internal const string RealSubtitle = "C# managed - local cache + live batch API - tags / vars / color safe";
        internal const string NeutralRuntimeTag = "runtime";
        internal const string NeutralSubtitle = "local cache + live batch API - tags / vars / color safe";

        /* --ui-probe 置 1：WM_CREATE 走确定性精简路径，渲染用中性文案，
           动画时钟冻结（见 Theme.AnimTickOverride）。 */
        internal static bool ProbeMode;

        internal static string RuntimeTag { get { return ProbeMode ? NeutralRuntimeTag : RealRuntimeTag; } }
        internal static string SubtitleText { get { return ProbeMode ? NeutralSubtitle : RealSubtitle; } }

        /* 委托由静态字段持有：交给 RegisterClassExW 后必须比窗口活得久。 */
        private static readonly Win32.WndProcDelegate WndProcHolder = WndProc;

        /* 探针用同一个窗口过程注册自己的窗口类，保证被渲染的这一帧走的就是
           真实的消息处理路径（对应 C 版的 ui_probe_wndproc）。 */
        internal static IntPtr WndProcPointer { get { return Marshal.GetFunctionPointerForDelegate(WndProcHolder); } }

        /* ---------------- 日志与状态（ui.c 的 append_log / set_status） ---------------- */

        /* 使指定控件的区域无效化（触发重绘），pad 为外扩像素。 */
        internal static void InvalidateControlArea(IntPtr ctl, int pad)
        {
            if (ctl == IntPtr.Zero || !Win32.IsWindow(ctl)) return;
            IntPtr parent = Win32.GetParent(ctl);
            if (parent != IntPtr.Zero) {
                Win32.RECT rc;
                Win32.GetWindowRect(ctl, out rc);
                Win32.MapWindowPoints(IntPtr.Zero, parent, ref rc, 2);
                Win32.InflateRect(ref rc, pad, pad);
                Win32.InvalidateRect(parent, ref rc, false);
            }
            Win32.RedrawWindow(ctl, IntPtr.Zero, IntPtr.Zero, Win32.RDW_INVALIDATE | Win32.RDW_NOERASE);
        }

        /* Log.Append 的窗口接收端：加时间戳、裁剪行数、扩展水平滚动范围。 */
        private static void AppendLogToList(string body)
        {
            DateTime now = DateTime.Now;
            string line = string.Format("[{0:00}:{1:00}:{2:00}] {3}", now.Hour, now.Minute, now.Second, body);

            if (LogList == IntPtr.Zero || !Win32.IsWindow(LogList)) {
                Win32.OutputDebugStringW(line);
                Win32.OutputDebugStringW("\n");
                return;
            }

            long count = (long)Win32.SendMessageW(LogList, Win32.LB_GETCOUNT, IntPtr.Zero, IntPtr.Zero);
            while (count >= LogMaxLines) {
                Win32.SendMessageW(LogList, Win32.LB_DELETESTRING, IntPtr.Zero, IntPtr.Zero);
                count--;
            }

            int idx = (int)Win32.SendMessageW(LogList, Win32.LB_ADDSTRING, IntPtr.Zero, line);
            if (idx >= 0) {
                IntPtr dc = Win32.GetDC(LogList);
                if (dc != IntPtr.Zero) {
                    IntPtr old = Win32.SelectObject(dc, Theme.FontMono);
                    Win32.SIZE sz;
                    if (Win32.GetTextExtentPoint32W(dc, line, line.Length, out sz)) {
                        int extent = sz.cx + Theme.Sc(32);
                        if (extent > _logExtentPx) {
                            _logExtentPx = extent;
                            Win32.SendMessageW(LogList, Win32.LB_SETHORIZONTALEXTENT, (IntPtr)_logExtentPx, IntPtr.Zero);
                        }
                    }
                    Win32.SelectObject(dc, old);
                    Win32.ReleaseDC(LogList, dc);
                }
                Win32.SendMessageW(LogList, Win32.LB_SETTOPINDEX, (IntPtr)idx, IntPtr.Zero);
            }
        }

        /* 更新顶部状态栏（带 "STATUS · " 前缀）。 */
        private static void SetStatusText(string text)
        {
            if (Status == IntPtr.Zero || !Win32.IsWindow(Status)) return;
            string buf = "STATUS  ·  " + (text ?? "");
            InvalidateControlArea(Status, 6);
            Win32.SetWindowTextW(Status, buf);
            InvalidateControlArea(Status, 6);
        }

        /* 刷新缓存卡片：读取 translation_memory_c.tsv 文件大小并显示。 */
        internal static void UpdateCacheCard()
        {
            if (CacheLabel == IntPtr.Zero || !Win32.IsWindow(CacheLabel)) return;
            InvalidateControlArea(CacheLabel, 4);
            string text = LaunchFlow.CacheSizeText();
            if (LaunchFlow.CacheFileReadable()) {
                InvalidateControlArea(CacheLabel, 4);
                Win32.SetWindowTextW(CacheLabel, text);
                InvalidateControlArea(CacheLabel, 4);
            } else {
                Win32.SetWindowTextW(CacheLabel, text);
            }
            InvalidateControlArea(CacheLabel, 4);
        }

        /* 刷新引擎卡片：从路径框读取目录，重新检测引擎类型并更新显示。 */
        internal static void RefreshEngine()
        {
            if (EngineLabel == IntPtr.Zero || !Win32.IsWindow(EngineLabel)) return;
            string dir = GetPathText();
            Engine e = EngineDetector.Detect(dir);
            InvalidateControlArea(EngineLabel, 4);
            Win32.SetWindowTextW(EngineLabel, EngineDetector.Name(e));
            InvalidateControlArea(EngineLabel, 4);
        }

        private static string GetPathText()
        {
            if (Path == IntPtr.Zero || !Win32.IsWindow(Path)) return "";
            var sb = new StringBuilder(Win32.MAX_PATH * 4);
            Win32.GetWindowTextW(Path, sb, Win32.MAX_PATH * 4);
            return sb.ToString();
        }

        /* ---------------- 字体与窗口铬框 ---------------- */

        internal static void ApplyFonts()
        {
            IntPtr[] controls = { Title, Subtitle, PathLabel, Path, EngineLabel, ServerLabel, CacheLabel,
                                  Status, LogList, BtnServer, BtnApi, BtnRestore, BtnClearCache };
            foreach (IntPtr c in controls) {
                if (c != IntPtr.Zero && Win32.IsWindow(c)) {
                    Win32.SendMessageW(c, Win32.WM_SETFONT, Theme.FontBody, (IntPtr)1);
                }
            }
            SetFont(Title, Theme.FontTitle);
            SetFont(Subtitle, Theme.FontSmall);
            SetFont(PathLabel, Theme.FontHeading);
            SetFont(EngineLabel, Theme.FontHeading);
            SetFont(ServerLabel, Theme.FontHeading);
            SetFont(CacheLabel, Theme.FontHeading);
            SetFont(BtnClearCache, Theme.FontSmall);
            int[] buttonIds = { Theme.IDC_BROWSE, Theme.IDC_OPEN, Theme.IDC_START };
            foreach (int id in buttonIds) {
                IntPtr button = Win32.GetDlgItem(Hwnd, id);
                if (button != IntPtr.Zero && Win32.IsWindow(button)) {
                    Win32.SendMessageW(button, Win32.WM_SETFONT, Theme.FontBody, (IntPtr)1);
                }
            }
            SetFont(Status, Theme.FontMonoSmall);
            if (LogList != IntPtr.Zero && Win32.IsWindow(LogList)) {
                Win32.SendMessageW(LogList, Win32.WM_SETFONT, Theme.FontMono, (IntPtr)1);
                IntPtr dc = Win32.GetDC(LogList);
                if (dc != IntPtr.Zero) {
                    IntPtr old = Win32.SelectObject(dc, Theme.FontMono);
                    Win32.TEXTMETRICW tm;
                    if (Win32.GetTextMetricsW(dc, out tm)) {
                        Win32.SendMessageW(LogList, Win32.LB_SETITEMHEIGHT, IntPtr.Zero,
                                           (IntPtr)(tm.tmHeight + Theme.Sc(6)));
                    }
                    Win32.SelectObject(dc, old);
                    Win32.ReleaseDC(LogList, dc);
                }
            }
        }

        private static void SetFont(IntPtr ctl, IntPtr font)
        {
            if (ctl != IntPtr.Zero && Win32.IsWindow(ctl)) {
                Win32.SendMessageW(ctl, Win32.WM_SETFONT, font, (IntPtr)1);
            }
        }

        [DllImport("dwmapi.dll", EntryPoint = "DwmSetWindowAttribute")]
        private static extern int DwmSetWindowAttribute(IntPtr hwnd, uint attr, ref int value, uint size);

        [DllImport("uxtheme.dll", EntryPoint = "SetWindowTheme", CharSet = CharSet.Unicode)]
        private static extern int SetWindowTheme(IntPtr hwnd, string appName, string subIdList);

        internal static void ApplyWindowChrome(IntPtr hwnd)
        {
            /* dwmapi / uxtheme 在旧版 Windows 上可能缺失或缺少这些属性。DllNotFound /
               EntryPointNotFound 是外部平台边界，无法在上游修复；这里只影响标题栏配色，
               窗口本身照常工作，因此记录到调试输出而不是让启动器崩溃。 */
            try {
                int dark = 1;
                if (DwmSetWindowAttribute(hwnd, 20, ref dark, sizeof(int)) < 0) {
                    DwmSetWindowAttribute(hwnd, 19, ref dark, sizeof(int));
                }
                int caption = (int)Theme.C_PAGE;
                int border = (int)Theme.C_LINE;
                int text = (int)Theme.C_TEXT;
                DwmSetWindowAttribute(hwnd, 35, ref caption, sizeof(int));
                DwmSetWindowAttribute(hwnd, 34, ref border, sizeof(int));
                DwmSetWindowAttribute(hwnd, 36, ref text, sizeof(int));
            } catch (DllNotFoundException ex) {
                Win32.OutputDebugStringW("ds launcher: dwmapi unavailable: " + ex.Message + "\n");
            } catch (EntryPointNotFoundException ex) {
                Win32.OutputDebugStringW("ds launcher: DwmSetWindowAttribute unavailable: " + ex.Message + "\n");
            }

            try {
                if (LogList != IntPtr.Zero && Win32.IsWindow(LogList)) SetWindowTheme(LogList, "DarkMode_Explorer", null);
                if (Path != IntPtr.Zero && Win32.IsWindow(Path)) SetWindowTheme(Path, "DarkMode_CFD", null);
            } catch (DllNotFoundException ex) {
                Win32.OutputDebugStringW("ds launcher: uxtheme unavailable: " + ex.Message + "\n");
            } catch (EntryPointNotFoundException ex) {
                Win32.OutputDebugStringW("ds launcher: SetWindowTheme unavailable: " + ex.Message + "\n");
            }
        }

        /* ---------------- 用户操作 ---------------- */

        /* 弹出文件夹浏览对话框（browse_folder）。 */
        internal static void BrowseFolder()
        {
            string picked = FolderPicker.Pick(Hwnd, "选择游戏根目录");
            if (picked == null) return;
            Win32.SetWindowTextW(Path, picked);
            LauncherConfig.SaveLastGameDir(picked);
            RefreshEngine();
        }

        /* 只移除由本启动器部署的翻译文件（restore_selected_game）。 */
        internal static void RestoreSelectedGame()
        {
            if (_startFlowRunning) {
                Win32.MessageBoxW(Hwnd, "翻译流程正在进行中，请完成后再还原游戏。", "ds游戏翻译器", MbIconWarning);
                return;
            }
            string game = GetPathText();
            if (!SafeFs.IsDir(game)) {
                Win32.MessageBoxW(Hwnd, "请先选择游戏目录。", "ds游戏翻译器", MbIconWarning);
                return;
            }

            Engine engine = EngineDetector.Detect(game);
            RefreshEngine();
            if (engine == Engine.Unknown) {
                Win32.MessageBoxW(Hwnd, "无法识别游戏引擎，未执行还原。", "ds游戏翻译器", MbIconWarning);
                return;
            }

            const string message =
                "请先完全退出游戏，然后再执行还原。\n\n" +
                "此操作只移除启动器部署的翻译文件，不会删除翻译缓存、用户模组或已有的 BepInEx。\n\n" +
                "确定要还原当前游戏吗？";
            if (Win32.MessageBoxW(Hwnd, message, "还原游戏", MbIconWarning | MbYesNo | MbDefButton2) != IdYes) return;

            LauncherConfig.SaveLastGameDir(game);
            Log.Append("开始还原：" + game + "（" + EngineDetector.Name(engine) + "）");
            Log.Status("正在还原游戏...");
            bool ok = Deploy.Restore(game, engine) != 0;
            Log.Status(ok ? "游戏还原完成。" : "游戏还原未完成，请查看日志。");
            Win32.MessageBoxW(Hwnd,
                ok ? "还原完成。翻译缓存、用户模组和已有运行时均已保留。"
                   : "还原未完全完成，请查看启动器日志。未确认归属的文件已保留。",
                "还原游戏", ok ? MbIconInformation : MbIconWarning);
        }

        /* 确认后删除共享翻译缓存（clear_translation_cache）。 */
        internal static void ClearTranslationCache()
        {
            if (_startFlowRunning) {
                Win32.MessageBoxW(Hwnd, "翻译流程正在进行中，请完成后再清除缓存。", "ds游戏翻译器", MbIconWarning);
                return;
            }
            const string message =
                "这会永久删除本机共享翻译缓存 translation_memory_c.tsv。\n\n" +
                "清除后，已有译文需要重新请求 API。正在运行的游戏可能仍保留进程内存缓存，重启游戏后才会完全生效。\n\n" +
                "API 配置、日志和游戏目录不会被删除。确定继续吗？";
            if (Win32.MessageBoxW(Hwnd, message, "清除缓存", MbIconWarning | MbYesNo | MbDefButton2) != IdYes) return;

            ServerProcess.RefreshStatus();
            bool wasRunning = ServerProcess.Started && ServerProcess.Alive();
            bool serverStopped = true;
            if (wasRunning) {
                Log.Status("正在停止服务并清除缓存...");
                Log.Append("清除缓存：正在停止本地服务，避免内存缓存再次写回磁盘。");
                ServerProcess.Stop();
                ServerProcess.RefreshStatus();
                serverStopped = !(ServerProcess.Started && ServerProcess.Alive());
                if (!serverStopped) {
                    Log.Append("清除缓存：本地服务仍在运行，已取消删除以避免旧内存缓存继续生效。");
                }
            } else {
                Log.Status("正在清除缓存...");
            }

            string cachePath = PathUtil.Join(Launcher.Root, "translation_memory_c.tsv");
            bool cleared = serverStopped && LaunchFlow.ClearCacheFile();

            bool restarted = true;
            if (wasRunning && serverStopped) {
                Log.Status("正在重新启动本地服务...");
                restarted = ServerProcess.Start();
            }
            UpdateCacheCard();

            if (cleared) Log.Append("共享翻译缓存已清除：" + cachePath);
            if (cleared && restarted) {
                Log.Status("缓存已清除。");
                Win32.MessageBoxW(Hwnd, "共享翻译缓存已清除。正在运行的游戏请重启后再使用。",
                                  "清除缓存", MbIconInformation);
            } else if (cleared) {
                Log.Status("缓存已清除，但本地服务重启失败。");
                Win32.MessageBoxW(Hwnd, "缓存已清除，但本地服务重启失败，请查看日志。",
                                  "清除缓存", MbIconWarning);
            } else {
                Log.Status("缓存清除失败，请查看日志。");
                Win32.MessageBoxW(Hwnd, "缓存清除失败，原缓存文件已保留，请查看日志。",
                                  "清除缓存", MbIconWarning);
            }
        }

        /* 本地翻译服务未就绪时取消部署与游戏启动（report_server_start_failure）。 */
        private static void ReportServerStartFailure()
        {
            Log.Append("本地翻译服务未就绪，已取消部署和游戏启动。");
            Log.Status("状态：服务器启动失败，未启动游戏");
        }

        /* start_server 的就绪轮询最长 15 秒、deploy 是同步文件 I/O、warmup 扫描
           数十 MB 资源并做同步 HTTP，全部在工作线程执行以保持 UI 响应。 */
        private static void WarmupLaunchThread(object state)
        {
            var a = (Tuple<string, Engine>)state;
            try {
                if (!ServerProcess.Start()) {
                    ReportServerStartFailure();
                } else {
                    Deploy.ForEngine(a.Item1, a.Item2);
                    Log.Status("正在预热缓存并启动游戏...");
                    LaunchFlow.RunEngineLaunchFlow(a.Item1, a.Item2);
                }
            } finally {
                _startFlowRunning = false;
            }
        }

        /* 开始翻译主流程入口（start_translation）。 */
        internal static void StartTranslation()
        {
            if (_startFlowRunning) {
                Log.Append("翻译流程仍在进行中，已忽略重复的开始请求。");
                Log.Status("翻译流程仍在进行中，请稍候。");
                return;
            }
            string game = GetPathText();
            if (!SafeFs.IsDir(game)) {
                Win32.MessageBoxW(Hwnd, "请先选择游戏目录。", "ds游戏翻译器", MbIconWarning);
                return;
            }
            LauncherConfig.SaveLastGameDir(game);
            Engine e = EngineDetector.Detect(game);
            RefreshEngine();
            Log.Append("选择目录：" + game);
            Log.Append("识别引擎：" + EngineDetector.Name(e));
            Log.Status("正在启动服务并部署...");
            _startFlowRunning = true;

            /* 把服务器启动、部署、预热（重 I/O + 同步 HTTP）和游戏启动全部放到
               工作线程，避免 UI 冻结。传入路径的私有副本，防止并发的路径框编辑
               在中途改变它。 */
            var th = new Thread(WarmupLaunchThread);
            th.IsBackground = true;
            th.SetApartmentState(ApartmentState.STA);   /* LaunchFlow 用 ShellExecuteW（COM） */
            th.Start(Tuple.Create(game, e));
        }

        /* ---------------- 窗口过程 ---------------- */

        private const uint MbIconWarning = 0x00000030;
        private const uint MbIconInformation = 0x00000040;
        private const uint MbYesNo = 0x00000004;
        private const uint MbDefButton2 = 0x00000100;
        private const int IdYes = 6;

        private static IntPtr WndProc(IntPtr hwnd, uint msg, IntPtr wp, IntPtr lp)
        {
            switch (msg) {
                case Win32.WM_CREATE: {
                    /* 与 C 版一致：g_main / Hwnd 由 CreateWindowExW 的返回值赋值，
                       WM_CREATE 期间仍是空，ApplyFonts 里对三个自绘按钮的 GetDlgItem
                       因此取不到句柄（它们的字体由 DrawButton 自己选，不影响绘制）。 */
                    Theme.SetDpiForWindow(hwnd);
                    Theme.CreateFonts();
                    Theme.CreateBrushes();
                    CreateControls(hwnd);

                    ApplyFonts();
                    UiButtons.InstallHoverTracking(hwnd);
                    ApplyWindowChrome(hwnd);
                    /* --ui-probe：到此为止就够画一帧了。后面的 payload 同步、缓存卡片、
                       日志、上次目录恢复、服务器探测和动画定时器都会随机器状态变化，
                       各自另有 parity 场景覆盖，探针跳过它们以获得确定的一帧。 */
                    if (ProbeMode) {
                        UiLayout.Apply(hwnd);
                        return IntPtr.Zero;
                    }
                    Log.SetSink(AppendLogToList);
                    Log.SetStatusSink(SetStatusText);
                    ServerProcess.StateChanged += OnServerState;
                    SelfUpdate.SyncEmbeddedPayloads();
                    UpdateCacheCard();
                    Log.Append("C# 启动器已就绪。");
                    string lastGame = LauncherConfig.LoadLastGameDir();
                    if (lastGame != null) {
                        Win32.SetWindowTextW(Path, lastGame);
                        RefreshEngine();
                        Log.Append("已恢复上次游戏目录：" + lastGame);
                        Log.Append("直接点击开始汉化即可。");
                    } else {
                        Log.Append("选择游戏目录后点击开始汉化。");
                    }
                    ServerProcess.RefreshStatus();
                    UiLayout.Apply(hwnd);
                    /* 约 60fps 的动画心跳：驱动数据线光束、呼吸灯与按钮悬停渐变 */
                    Win32.SetTimer(hwnd, (UIntPtr)2, 16, IntPtr.Zero);
                    return IntPtr.Zero;
                }
                case Win32.WM_PAINT: {
                    Win32.PAINTSTRUCT ps;
                    IntPtr dc = Win32.BeginPaint(hwnd, out ps);
                    UiPaint.PaintBackgroundBuffered(hwnd, dc, ps.rcPaint);
                    Win32.EndPaint(hwnd, ref ps);
                    return IntPtr.Zero;
                }
                /* PrintWindow 截屏、远程桌面捕获与辅助功能工具需要完整的客户区渲染。 */
                case Win32.WM_PRINTCLIENT:
                    UiPaint.PaintBackground(hwnd, wp);
                    return IntPtr.Zero;
                case Win32.WM_SIZE:
                    UiLayout.Apply(hwnd);
                    return IntPtr.Zero;
                case Win32.WM_GETMINMAXINFO: {
                    var mmi = (Win32.MINMAXINFO)Marshal.PtrToStructure(lp, typeof(Win32.MINMAXINFO));
                    mmi.ptMinTrackSize.x = Theme.Sc(1080);
                    mmi.ptMinTrackSize.y = Theme.Sc(700);
                    Marshal.StructureToPtr(mmi, lp, false);
                    return IntPtr.Zero;
                }
                case Win32.WM_TIMER:
                    if ((long)wp == 1) {
                        Win32.KillTimer(hwnd, (UIntPtr)1);
                        RefreshEngine();
                    } else if ((long)wp == 2) {
                        UiLayout.TickAnimation(hwnd);
                    }
                    return IntPtr.Zero;
                case Win32.WM_DPICHANGED: {
                    Theme.ScaleDpi = Win32.HIWORD(wp);
                    var prc = (Win32.RECT)Marshal.PtrToStructure(lp, typeof(Win32.RECT));
                    Win32.SetWindowPos(hwnd, IntPtr.Zero, prc.left, prc.top,
                                       prc.right - prc.left, prc.bottom - prc.top,
                                       Win32.SWP_NOZORDER | Win32.SWP_NOACTIVATE);
                    IntPtr[] old = { Theme.FontTitle, Theme.FontHeading, Theme.FontBody,
                                     Theme.FontSmall, Theme.FontMono, Theme.FontMonoSmall };
                    Theme.CreateFonts();
                    ApplyFonts();
                    foreach (IntPtr f in old) Win32.DeleteObject(f);
                    Win32.RedrawWindow(hwnd, IntPtr.Zero, IntPtr.Zero,
                                       Win32.RDW_INVALIDATE | Win32.RDW_ALLCHILDREN | Win32.RDW_NOERASE);
                    return IntPtr.Zero;
                }
                case Win32.WM_ERASEBKGND:
                    return (IntPtr)1;
                case Win32.WM_DRAWITEM: {
                    var di = (Win32.DRAWITEMSTRUCT)Marshal.PtrToStructure(lp, typeof(Win32.DRAWITEMSTRUCT));
                    UiButtons.DrawButton(ref di);
                    return (IntPtr)1;
                }
                case Win32.WM_CTLCOLORSTATIC: {
                    IntPtr dc = wp;
                    IntPtr ctl = lp;
                    if (ctl == Title || ctl == Subtitle || ctl == Status) {
                        Win32.SetBkMode(dc, Win32.OPAQUE);
                        Win32.SetBkColor(dc, Theme.C_PAGE);
                        if (ctl == Title) Win32.SetTextColor(dc, Theme.C_TEXT);
                        else if (ctl == Subtitle) Win32.SetTextColor(dc, Theme.C_TEXT_DIM);
                        else Win32.SetTextColor(dc, Theme.C_MUTED);
                        return Theme.BrushPage;
                    }
                    if (ctl == PathLabel || ctl == EngineLabel || ctl == ServerLabel || ctl == CacheLabel) {
                        /* 卡片内文字底色匹配所在高度的面板渐变，文本框无缝衔接 */
                        uint cardBg;
                        IntPtr cardBrush = UiPaint.CardTextBrush(ctl == PathLabel, out cardBg);
                        Win32.SetBkMode(dc, Win32.OPAQUE);
                        Win32.SetBkColor(dc, cardBg);
                        Win32.SetTextColor(dc, ctl == PathLabel ? Theme.C_MUTED : Theme.C_TEXT);
                        return cardBrush;
                    }
                    Win32.SetBkMode(dc, Win32.TRANSPARENT);
                    Win32.SetTextColor(dc, Theme.C_TEXT);
                    return Theme.BrushTransparent;
                }
                case Win32.WM_CTLCOLOREDIT: {
                    IntPtr dc = wp;
                    Win32.SetTextColor(dc, Theme.C_TEXT);
                    Win32.SetBkColor(dc, Theme.C_LOG);
                    return Theme.BrushEdit;
                }
                case Win32.WM_CTLCOLORLISTBOX: {
                    IntPtr dc = wp;
                    if (lp == LogList) {
                        Win32.SetTextColor(dc, Theme.C_LOG_TEXT);
                        Win32.SetBkColor(dc, Theme.C_LOG);
                        return Theme.BrushLog;
                    }
                    return Win32.DefWindowProcW(hwnd, msg, wp, lp);
                }
                case Win32.WM_COMMAND: {
                    int id = Win32.LOWORD(wp);
                    int notify = Win32.HIWORD(wp);
                    if (id == Theme.IDC_BROWSE) BrowseFolder();
                    else if (id == Theme.IDC_START) StartTranslation();
                    else if (id == Theme.IDC_RESTORE) RestoreSelectedGame();
                    else if (id == Theme.IDC_CLEAR_CACHE) ClearTranslationCache();
                    else if (id == Theme.IDC_SERVER_TOGGLE) {
                        ServerProcess.Toggle();
                        Win32.InvalidateRect(hwnd, IntPtr.Zero, false);
                    } else if (id == Theme.IDC_API_CONFIG) {
                        ApiConfigDialog.Show(hwnd);
                    } else if (id == Theme.IDC_OPEN) {
                        string game = GetPathText();
                        if (SafeFs.IsDir(game)) {
                            Win32.ShellExecuteW(hwnd, "open", game, null, null, Win32.SW_SHOWNORMAL);
                        }
                    } else if (id == Theme.IDC_PATH && notify == Win32.EN_CHANGE) {
                        /* 防抖：Detect 会扫描目录树，需要合并短时间内的连续触发。 */
                        Win32.SetTimer(hwnd, (UIntPtr)1, 200, IntPtr.Zero);
                    }
                    return IntPtr.Zero;
                }
                case Win32.WM_CLOSE:
                    Win32.DestroyWindow(hwnd);
                    return IntPtr.Zero;
                case Win32.WM_DESTROY:
                    /* 启动器窗口关闭后仍保持本地翻译服务器运行。 */
                    Win32.KillTimer(hwnd, (UIntPtr)2);
                    Theme.DeleteFontsAndBrushes();
                    Win32.PostQuitMessage(0);
                    return IntPtr.Zero;
            }
            return Win32.DefWindowProcW(hwnd, msg, wp, lp);
        }

        /* 服务器状态卡片与按钮文字（C 版由 server_proc.c 直接 SetWindowTextW）。 */
        private static void OnServerState(ServerProcess.State s)
        {
            if (ServerLabel != IntPtr.Zero && Win32.IsWindow(ServerLabel)) {
                InvalidateControlArea(ServerLabel, 4);
                Win32.SetWindowTextW(ServerLabel, s.Label);
                InvalidateControlArea(ServerLabel, 4);
            }
            if (BtnServer != IntPtr.Zero && Win32.IsWindow(BtnServer)) {
                Win32.SetWindowTextW(BtnServer, s.Button);
                Win32.InvalidateRect(BtnServer, IntPtr.Zero, false);
            }
            /* 状态药丸与指标卡片的配色取决于服务器状态，需要整窗重绘（C 版
               refresh_server_status 结尾同样 InvalidateRect(g_main, NULL, FALSE)）。 */
            if (Hwnd != IntPtr.Zero && Win32.IsWindow(Hwnd)) Win32.InvalidateRect(Hwnd, IntPtr.Zero, false);
        }

        private static void CreateControls(IntPtr hwnd)
        {
            const uint staticStyle = Win32.WS_CHILD | Win32.WS_VISIBLE | Win32.SS_NOPREFIX | Win32.SS_ENDELLIPSIS;

            Title = Child("STATIC", "无感翻译控制台", staticStyle, hwnd, Theme.IDC_TITLE);
            Subtitle = Child("STATIC", SubtitleText, staticStyle, hwnd, Theme.IDC_SUBTITLE);
            Status = Child("STATIC", "STATUS  ·  READY", staticStyle, hwnd, Theme.IDC_STATUS);

            PathLabel = Child("STATIC", "游戏目录", staticStyle, hwnd, Theme.IDC_PATH_LABEL);
            Path = Child("EDIT", "", Win32.WS_CHILD | Win32.WS_VISIBLE | Win32.ES_AUTOHSCROLL, hwnd, Theme.IDC_PATH);

            Child("BUTTON", "浏览", OwnerDrawButton, hwnd, Theme.IDC_BROWSE);
            Child("BUTTON", "打开目录", OwnerDrawButton, hwnd, Theme.IDC_OPEN);
            Child("BUTTON", "开始汉化", OwnerDrawButton, hwnd, Theme.IDC_START);
            BtnRestore = Child("BUTTON", "还原游戏", OwnerDrawButton, hwnd, Theme.IDC_RESTORE);
            BtnServer = Child("BUTTON", "启动服务器", OwnerDrawButton, hwnd, Theme.IDC_SERVER_TOGGLE);
            BtnApi = Child("BUTTON", "配置 API", OwnerDrawButton, hwnd, Theme.IDC_API_CONFIG);

            EngineLabel = Child("STATIC", "未选择", staticStyle, hwnd, Theme.IDC_ENGINE);
            ServerLabel = Child("STATIC", "未启动", staticStyle, hwnd, Theme.IDC_SERVER);
            CacheLabel = Child("STATIC", "检查中", staticStyle, hwnd, Theme.IDC_CACHE);
            BtnClearCache = Child("BUTTON", "清除缓存", OwnerDrawButton, hwnd, Theme.IDC_CLEAR_CACHE);

            LogList = Child("LISTBOX", "",
                            Win32.WS_CHILD | Win32.WS_VISIBLE | Win32.WS_VSCROLL | Win32.WS_HSCROLL |
                            Win32.LBS_NOINTEGRALHEIGHT | Win32.LBS_DISABLENOSCROLL,
                            hwnd, Theme.IDC_LOG);
        }

        private const uint OwnerDrawButton = Win32.WS_CHILD | Win32.WS_VISIBLE | Win32.BS_OWNERDRAW;

        private static IntPtr Child(string cls, string text, uint style, IntPtr parent, int id)
        {
            return Win32.CreateWindowExW(0, cls, text, style, 0, 0, 0, 0, parent, (IntPtr)id, Instance, IntPtr.Zero);
        }

        /* ---------------- 入口 ---------------- */

        internal static ushort RegisterClass(IntPtr inst)
        {
            var wc = new Win32.WNDCLASSEXW();
            wc.cbSize = (uint)Marshal.SizeOf(typeof(Win32.WNDCLASSEXW));
            wc.lpfnWndProc = WndProcPointer;
            wc.hInstance = inst;
            wc.hIcon = Win32.LoadImageW(inst, (IntPtr)Theme.IDI_APP_ICON, Win32.IMAGE_ICON,
                                        Win32.GetSystemMetrics(Win32.SM_CXICON),
                                        Win32.GetSystemMetrics(Win32.SM_CYICON), Win32.LR_SHARED);
            wc.hIconSm = Win32.LoadImageW(inst, (IntPtr)Theme.IDI_APP_ICON, Win32.IMAGE_ICON,
                                          Win32.GetSystemMetrics(Win32.SM_CXSMICON),
                                          Win32.GetSystemMetrics(Win32.SM_CYSMICON), Win32.LR_SHARED);
            wc.hCursor = Win32.LoadCursorW(IntPtr.Zero, (IntPtr)32512 /* IDC_ARROW */);
            wc.hbrBackground = IntPtr.Zero;
            wc.lpszClassName = Marshal.StringToHGlobalUni(WindowClass);
            return Win32.RegisterClassExW(ref wc);
        }

        /* 正常 GUI 启动：注册窗口类 → 创建主窗口 → 消息循环（wWinMain 的后半段）。 */
        internal static int Run()
        {
            Theme.EnableDpiAwareness();

            var ic = new Win32.INITCOMMONCONTROLSEX();
            ic.dwSize = (uint)Marshal.SizeOf(typeof(Win32.INITCOMMONCONTROLSEX));
            ic.dwICC = Win32.ICC_STANDARD_CLASSES;
            Win32.InitCommonControlsEx(ref ic);
            Win32.CoInitialize(IntPtr.Zero);

            Instance = Win32.GetModuleHandleW(null);

            /* 用主显示器的 DPI 决定初始窗口尺寸（窗口之后可能被移动到其他显示器，
               由 WM_DPICHANGED 处理）。 */
            int primaryDpi = 96;
            IntPtr sdc = Win32.GetDC(IntPtr.Zero);
            if (sdc != IntPtr.Zero) {
                primaryDpi = Win32.GetDeviceCaps(sdc, Win32.LOGPIXELSY);
                Win32.ReleaseDC(IntPtr.Zero, sdc);
            }
            if (primaryDpi <= 0) primaryDpi = 96;

            /* 注册/创建失败会让整个启动器没有界面。C 版忽略了返回值；这里如实记录到
               调试输出和 stderr 后以非 0 退出，而不是静默进入一个永远收不到消息的
               循环——没有窗口时 Log 的接收端还没装上，写日志列表框会丢。 */
            if (RegisterClass(Instance) == 0) {
                FatalStartupError("无法注册启动器窗口类", Marshal.GetLastWin32Error());
                return 1;
            }

            int initW = Win32.MulDiv(1200, primaryDpi, 96);
            int initH = Win32.MulDiv(780, primaryDpi, 96);
            Hwnd = Win32.CreateWindowExW(0, WindowClass, "ds游戏翻译器",
                                         Win32.WS_OVERLAPPEDWINDOW | Win32.WS_VISIBLE | Win32.WS_CLIPCHILDREN,
                                         Win32.CW_USEDEFAULT, Win32.CW_USEDEFAULT, initW, initH,
                                         IntPtr.Zero, IntPtr.Zero, Instance, IntPtr.Zero);
            if (Hwnd == IntPtr.Zero) {
                FatalStartupError("无法创建启动器窗口", Marshal.GetLastWin32Error());
                return 1;
            }

            Win32.ShowWindow(Hwnd, Win32.SW_SHOW);
            Win32.UpdateWindow(Hwnd);

            var msg = new Win32.MSG();
            for (;;) {
                /* GetMessageW 返回 -1 表示出错（如传入无效句柄），0 为 WM_QUIT，两者都退出循环 */
                int got = Win32.GetMessageW(out msg, IntPtr.Zero, 0, 0);
                if (got <= 0) break;
                Win32.TranslateMessage(ref msg);
                Win32.DispatchMessageW(ref msg);
            }
            return (int)msg.wParam;
        }

        private static void FatalStartupError(string what, int error)
        {
            string line = "ds launcher: " + what + "（Windows 错误：" + error + "）\n";
            Win32.OutputDebugStringW(line);
            Console.Error.Write(line);
        }
    }
}
