using System;
using System.Diagnostics;
using System.Runtime.InteropServices;
using System.Text;

namespace DstLauncher
{
    /*
     * LaunchFlow —— ui.c 中与窗口无关的一键流程部分：
     *
     *   launch_game / launch_game_with_params   启动游戏 exe
     *   run_engine_launch_flow                  按引擎决定启动/预热/补丁刷新的顺序
     *   cache_size_text / clear_cache_file      缓存卡片文本与共享缓存删除
     *
     * 绘制、控件、消息循环仍留在 C 版 ui.c；这里只搬"决定做什么"的那一层，
     * 使 tests/launcher_parity 能用 --launch-flow-and-exit / --launch-flow 比对
     * 两版的日志、状态推进、预热批次与将要拉起的进程命令行。
     */
    public static class LaunchFlow
    {
        /* C 版命令行缓冲 WCHAR cmd[MAX_PATH * 12]（含结尾 NUL），
           wide_format_checked 在放不下时清空并失败，绝不把截断的命令交给 Win32。 */
        public const int CmdCap = 260 * 12;

        /* 演练模式（--launch-flow）：不真的创建进程，只把计划好的命令行交给这个回调。
           kind 与 C 版 write_spawn_plan 一致：shell / proc / worker。 */
        public static Action<string, string, string, string> SpawnSink;

        public static bool DryRun { get { return SpawnSink != null; } }

        public static string FormatChecked(string text)
        {
            if (text == null || text.Length >= CmdCap) return null;
            return text;
        }

        private static void Spawn(string kind, string exe, string cmd, string cwd)
        {
            Action<string, string, string, string> sink = SpawnSink;
            if (sink != null) sink(kind, exe ?? "", cmd ?? "", cwd ?? "");
        }

        /* ---------------- 启动游戏 ---------------- */

        [DllImport("shell32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
        private static extern IntPtr ShellExecuteW(IntPtr hwnd, string op, string file,
                                                   string param, string dir, int show);

        /* launch_game_with_params：找到游戏 exe 并用 ShellExecuteW 打开。 */
        public static void LaunchGameWithParams(string dir, string parameters)
        {
            string exe = EngineDetector.FindExe(dir);
            if (exe == null) {
                Log.Append("未找到游戏 exe。");
                return;
            }
            Log.Append("启动游戏：" + exe);
            if (DryRun) {
                Spawn("shell", exe, parameters, dir);
                return;
            }
            IntPtr result = ShellExecuteW(IntPtr.Zero, "open", exe, parameters, dir, 1 /* SW_SHOWNORMAL */);
            /* ShellExecuteW 返回值 <=32 表示失败（2=文件未找到、5=拒绝访问等） */
            if (result.ToInt64() <= 32) {
                Log.Append("启动游戏失败：" + exe + "（ShellExecuteW 错误码：" + result.ToInt64() + "）");
            }
        }

        public static void LaunchGame(string dir)
        {
            LaunchGameWithParams(dir, null);
        }

        /* ---------------- 一键流程（服务器就绪之后） ---------------- */

        /* run_engine_launch_flow：Ren'Py 先启动再后台预热（渲染回调从不等 HTTP）；
           Unity/XUnity 先导入后启动，让插件的实时查询命中已导入的行；Godot 先启动、
           再预热、最后派出独立的补丁刷新进程。 */
        public static void RunEngineLaunchFlow(string dir, Engine engine)
        {
            if (engine == Engine.RenPy) {
                GodotLaunch.LaunchGameForEngine(dir, engine);
                Log.Status("已启动 · 正在后台预热剧本...");
                Warmup.Run(dir, engine);
            } else if (engine == Engine.Godot) {
                string patch = PathUtil.Join(dir, "dst_godot_patch.pck");
                bool hadPatch = PathUtil.Exists(patch);
                if (hadPatch) {
                    Log.Append("Godot: existing patch pack found; launching before cache warmup and patch refresh.");
                    GodotLaunch.LaunchGameForEngine(dir, engine);
                    Log.Status("Godot: 已启动，正在后台预热缓存...");
                } else {
                    Log.Append("Godot: no patch pack yet; launching first, then warming resources for patch preparation.");
                    GodotLaunch.LaunchGameForEngine(dir, engine);
                    Log.Status("Godot: 已启动，正在后台准备翻译补丁...");
                }
                Warmup.Run(dir, engine);
                if (!GodotLaunch.StartGodotPatchWorker(dir)) {
                    Log.Append(hadPatch
                        ? "Godot: detached patch refresh did not start; current game continues with the existing pack."
                        : "Godot: detached patch preparation did not start; game was already started normally.");
                }
            } else {
                Warmup.Run(dir, engine);
                GodotLaunch.LaunchGameForEngine(dir, engine);
            }
            Log.Status("已启动 · 本地缓存 + 实时批量 API");
        }

        /* ---------------- 缓存卡片 ---------------- */

        [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
        private static extern bool GetFileAttributesExW(string name, int level,
                                                        out WIN32_FILE_ATTRIBUTE_DATA data);

        [StructLayout(LayoutKind.Sequential)]
        private struct WIN32_FILE_ATTRIBUTE_DATA
        {
            public uint dwFileAttributes;
            public uint ftCreationLow, ftCreationHigh;
            public uint ftAccessLow, ftAccessHigh;
            public uint ftWriteLow, ftWriteHigh;
            public uint nFileSizeHigh, nFileSizeLow;
        }

        public static string CachePath()
        {
            return PathUtil.Join(Launcher.Root, "translation_memory_c.tsv");
        }

        /* cache_size_text："%.1f MB"。C 的 %.1f 用 round-half-away-from-zero，
           .NET 的 "F1" 在 net472 上同样如此，不能换成默认的 ToString()。 */
        public static string CacheSizeText()
        {
            WIN32_FILE_ATTRIBUTE_DATA data;
            if (!GetFileAttributesExW(CachePath(), 0 /* GetFileExInfoStandard */, out data)) return "0.0 MB";
            ulong bytes = ((ulong)data.nFileSizeHigh << 32) | data.nFileSizeLow;
            double mb = bytes / 1024.0 / 1024.0;
            return mb.ToString("F1", System.Globalization.CultureInfo.InvariantCulture) + " MB";
        }

        /* cache_size_text 的返回值：是否读到了文件属性。update_cache_card 用它
           决定重绘次数（C 版在读到属性时多失效一次，缺失时少一次）。 */
        public static bool CacheFileReadable()
        {
            WIN32_FILE_ATTRIBUTE_DATA data;
            return GetFileAttributesExW(CachePath(), 0 /* GetFileExInfoStandard */, out data);
        }

        /* clear_cache_file：删除共享缓存文件本身。文件不存在视为已清除；目标是目录
           或删除失败时保留原文件并记录 Windows 错误码，绝不把失败当成已清除。
           调用方负责先停掉本地服务。 */
        public static bool ClearCacheFile()
        {
            string cachePath = CachePath();
            uint attr = SafeFs.Attributes(cachePath);
            if (attr == 0xFFFFFFFFu) {
                int error = SafeFs.LastError;
                if (error == 2 /* ERROR_FILE_NOT_FOUND */ || error == 3 /* ERROR_PATH_NOT_FOUND */) return true;
                Log.Append("清除缓存：无法检查 " + cachePath + "（Windows 错误 " + error + "）。");
                return false;
            }
            if ((attr & 0x10u) != 0 /* FILE_ATTRIBUTE_DIRECTORY */) {
                Log.Append("清除缓存：目标路径是目录，已保留：" + cachePath);
                return false;
            }
            if (SafeFs.DeleteFileSafe(cachePath)) return true;
            Log.Append("清除缓存：无法删除 " + cachePath + "（Windows 错误 " + SafeFs.LastError + "）。");
            return false;
        }
    }
}
