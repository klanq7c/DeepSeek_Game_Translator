using System;
using System.Diagnostics;
using System.IO;
using System.Net;
using System.Runtime.InteropServices;
using System.Text;
using System.Threading;
using DstCore;

namespace DstLauncher
{
    /*
     * ServerProcess —— server_proc.c 的移植：本地翻译服务器子进程的启动、健康检查、
     * 接管与关停。
     *
     * 与 C 版一致的契约：
     *   - 端口固定 19999（Contract.DefaultPort），先探测 /health，已有健康服务则"接管"而不再拉起；
     *   - launcher.ini [server] binary=native|cs 选择 native\dst_server.exe / native\dst_server_cs.exe；
     *   - 启动后轮询 /health，按真实时钟计 15 s 预算，进程提前退出立即失败；
     *   - 关停先 POST /shutdown，1500 ms 内未退出则 TerminateProcess；
     *   - 所有失败路径都写日志（含 Win32/HTTP 错误），不吞异常。
     *
     * UI 标签/按钮文本通过 StateChanged 事件交给宿主（将来的窗口），本类不依赖任何控件。
     */
    public static class ServerProcess
    {
        private const int ReadyTimeoutMs = 15000;
        private const int ReadyPollMs = 200;
        private const int Port = Contract.DefaultPort;

        private static Process _proc;          /* 启动器自己创建的进程；接管外部服务时为 null */
        private static bool _started;          /* g_server_started */
        private static bool _owned;            /* g_server_owned */

        public sealed class State
        {
            public string Label;   /* 服务器卡片文字，如 "运行中 · 19999" */
            public string Button;  /* 服务器按钮文字，如 "停止服务器" */
        }

        public static event Action<State> StateChanged;

        public static bool Started { get { return _started; } }
        public static bool Owned { get { return _owned; } }

        /* 只给 --ui-probe 用：状态药丸、SERVER 卡片配色和服务器按钮的样式都取决于
           这个标志，探针必须能把它固定成给定值才能渲染出确定的一帧。C 版直接写
           全局 g_server_started。这里不碰进程句柄，也不影响 Alive()/Start()/Stop()
           的真实判定。 */
        internal static void SetStartedForProbe(bool started)
        {
            _started = started;
        }

        private static void SetState(string label, string button)
        {
            var h = StateChanged;
            if (h != null) h(new State { Label = label, Button = button });
        }

        /* ---------------- HTTP 探测 ---------------- */

        /* server_http_alive：GET /health 在 timeout 内返回 200。 */
        private static bool HttpAlive(int timeoutMs)
        {
            try {
                var req = (HttpWebRequest)WebRequest.Create(Contract.DefaultBaseUrl + Contract.PathHealth);
                req.Method = "GET";
                req.Timeout = timeoutMs;
                req.ReadWriteTimeout = timeoutMs;
                req.Proxy = null;
                req.KeepAlive = false;
                using (var resp = (HttpWebResponse)req.GetResponse()) {
                    return resp.StatusCode == HttpStatusCode.OK;
                }
            } catch (WebException) {
                return false;
            }
        }

        /* request_server_shutdown：POST /shutdown，500 ms 快速失败；每条失败路径都记录。 */
        private static bool RequestShutdown()
        {
            try {
                var req = (HttpWebRequest)WebRequest.Create(Contract.DefaultBaseUrl + Contract.PathShutdown);
                req.Method = "POST";
                req.ContentLength = 0;
                req.Timeout = 500;
                req.ReadWriteTimeout = 500;
                req.Proxy = null;
                req.KeepAlive = false;
                using (var resp = (HttpWebResponse)req.GetResponse()) {
                    int status = (int)resp.StatusCode;
                    if (status >= 200 && status < 300) return true;
                    Log.Append("服务端关闭请求返回异常状态：" + status);
                    return false;
                }
            } catch (WebException ex) {
                var resp = ex.Response as HttpWebResponse;
                if (resp != null) {
                    Log.Append("服务端关闭请求返回异常状态：" + (int)resp.StatusCode + "（" + ex.Status + "）");
                    resp.Dispose();
                } else {
                    Log.Append("发送服务端关闭请求失败。错误：" + ex.Status + " " + ex.Message);
                }
                return false;
            }
        }

        private static bool ProcessExited()
        {
            return _proc != null && _proc.HasExited;
        }

        /* wait_for_server_ready：真实时钟记账，进程退出即失败。 */
        private static bool WaitForReady(int timeoutMs)
        {
            var sw = Stopwatch.StartNew();
            for (;;) {
                if (HttpAlive(200)) return true;
                if (ProcessExited()) return false;
                if (sw.ElapsedMilliseconds >= timeoutMs) return false;
                Thread.Sleep(ReadyPollMs);
            }
        }

        private static bool WaitForStopped(int timeoutMs)
        {
            int waited = 0;
            for (;;) {
                if (!HttpAlive(120)) return true;
                if (waited >= timeoutMs) return false;
                Thread.Sleep(ReadyPollMs);
                waited += ReadyPollMs;
            }
        }

        /* server_alive：先看自有进程句柄，再回退 HTTP（覆盖接管的外部服务）。 */
        public static bool Alive()
        {
            if (!_started) return false;
            if (_proc != null && !_proc.HasExited) return true;
            return HttpAlive(120);
        }

        private static void ResetHandle()
        {
            if (_proc != null) _proc.Dispose();
            _proc = null;
            _started = false;
            _owned = false;
        }

        /* refresh_server_status：不启动新进程，只对齐状态。 */
        public static void RefreshStatus()
        {
            if (_started && Alive()) {
                SetState("运行中 · 19999", "停止服务器");
                return;
            }
            if (_started) ResetHandle();
            if (HttpAlive(200)) {
                _started = true;
                _owned = false;
                SetState("运行中 · 19999", "停止服务器");
                Log.Append("检测到已运行的 C 服务端：127.0.0.1:19999");
            } else {
                SetState("未启动", "启动服务器");
            }
        }

        /* toggle_server：服务器按钮的回调，运行中则停止，否则启动。 */
        public static void Toggle()
        {
            /* 一键翻译流程的工作线程正在执行 Start/Deploy 期间，拒绝并发切换，
               避免与后台线程争抢进程句柄与 _started。 */
            if (MainWindow.TranslationFlowRunning) {
                Log.Status("状态：翻译流程正在进行，请稍候再操作服务器");
                return;
            }
            if (_started && Alive()) {
                Stop();
                if (_started && Alive()) {
                    Log.Status("状态：服务器仍由外部进程运行");
                } else {
                    Log.Status("服务器已停止");
                }
                return;
            }
            Log.Status("正在启动服务器...");
            if (Start()) Log.Status("状态：服务器运行中");
            else Log.Status("状态：服务器启动失败");
        }

        /* ---------------- 二进制选择 ---------------- */

        [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
        private static extern uint GetPrivateProfileStringW(string section, string key, string def,
                                                            StringBuilder ret, uint size, string file);

        /* 与 C 版一样走 GetPrivateProfileStringW，保证对同一份 launcher.ini 的解释一致。 */
        public static string ReadLauncherIni(string section, string key, string def, int cap)
        {
            var sb = new StringBuilder(cap);
            GetPrivateProfileStringW(section, key, def, sb, (uint)cap, SafeFs.LauncherConfigPath());
            return sb.ToString();
        }

        /* select_server_binary：缺省/native → dst_server.exe；cs → dst_server_cs.exe。 */
        public static string SelectServerBinary()
        {
            string choice = ReadLauncherIni("server", "binary", "native", 64);
            if (string.Equals(choice, "cs", StringComparison.OrdinalIgnoreCase)) {
                Log.Append("已按配置选择 C# 服务器二进制（server binary=cs）。");
                return PathUtil.Join(Launcher.Root, "native\\dst_server_cs.exe");
            }
            return PathUtil.Join(Launcher.Root, "native\\dst_server.exe");
        }

        /* ---------------- 启动 / 停止 ---------------- */

        public static bool Start()
        {
            if (_started && Alive()) return true;
            if (_started) ResetHandle();

            if (HttpAlive(200)) {
                _started = true;
                _owned = false;
                SetState("运行中 · 19999", "停止服务器");
                Log.Append("检测到已运行的 C 服务端：127.0.0.1:19999");
                return true;
            }

            string exe = SelectServerBinary();
            string cache = PathUtil.Join(Launcher.Root, "translation_memory_c.tsv");
            string apiCfg = SafeFs.ApiConfigPath();
            if (!SafeFs.Exists(exe)) {
                Log.Append("找不到 C 服务端：" + exe);
                return false;
            }

            var psi = new ProcessStartInfo {
                FileName = exe,
                Arguments = "--port " + Port + " --cache \"" + cache + "\" --api-config \"" + apiCfg + "\"",
                WorkingDirectory = Launcher.Root,
                UseShellExecute = false,
                CreateNoWindow = true,
                WindowStyle = ProcessWindowStyle.Hidden
            };
            try {
                _proc = Process.Start(psi);
            } catch (System.ComponentModel.Win32Exception ex) {
                Log.Append("服务端启动失败，可能 19999 已被占用。错误：" + ex.NativeErrorCode);
                _proc = null;
                return false;
            }
            if (_proc == null) {
                Log.Append("服务端启动失败：Process.Start 未返回进程。");
                return false;
            }

            _started = true;
            _owned = true;
            SetState("正在启动 · 19999", "停止服务器");
            Log.Append("C 服务端已启动，正在等待 /health：127.0.0.1:19999");
            if (!WaitForReady(ReadyTimeoutMs)) {
                if (ProcessExited()) {
                    Log.Append("C 服务端在健康检查前退出，退出码：" + _proc.ExitCode + "。");
                } else {
                    Log.Append("C 服务端在 " + ReadyTimeoutMs + " ms 内未通过 /health 健康检查。");
                }
                if (_proc != null && !_proc.HasExited) {
                    RequestShutdown();
                    if (!_proc.WaitForExit(1000)) {
                        Log.Append("服务端未在优雅关闭期限内退出，正在终止启动器创建的进程。");
                        try { _proc.Kill(); } catch (InvalidOperationException) { /* 已退出：Kill 与 HasExited 之间的竞争，无需处理 */ }
                        _proc.WaitForExit(500);
                    }
                }
                ResetHandle();
                SetState("未启动", "启动服务器");
                return false;
            }
            SetState("运行中 · 19999", "停止服务器");
            Log.Append("C 服务端健康检查通过：127.0.0.1:19999");
            return true;
        }

        public static void Stop()
        {
            bool owned = _owned;
            if (!_started && !HttpAlive(200)) {
                SetState("未启动", "启动服务器");
                return;
            }
            if (Alive() || HttpAlive(200)) {
                bool shutdownRequested = RequestShutdown();
                if (_proc != null && !_proc.WaitForExit(1500)) {
                    Log.Append("服务端未在优雅关闭期限内退出，正在终止启动器创建的进程。");
                    try { _proc.Kill(); } catch (InvalidOperationException) { /* 已退出 */ }
                    _proc.WaitForExit(500);
                } else if (_proc == null && shutdownRequested && !WaitForStopped(1500)) {
                    Log.Append("External C server still owns 127.0.0.1:19999 after the shutdown deadline; cache maintenance may need a retry.");
                }
                if (_proc == null && !shutdownRequested) {
                    /* 关停请求未送达外部服务端：保留接管状态，避免 UI 误报"已停止"。 */
                    SetState("运行中 · 19999", "停止服务器");
                    Log.Append("向外部 C 服务端发送停止请求失败，服务端可能仍在运行。");
                    return;
                }
            }
            ResetHandle();
            SetState("已停止", "启动服务器");
            Log.Append(owned ? "C 服务端已停止。" : "已向外部 C 服务端发送停止请求。");
        }
    }
}
