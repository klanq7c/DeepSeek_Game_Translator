using System;
using System.ComponentModel;
using System.Diagnostics;
using System.IO;
using System.Reflection;
using System.Runtime.InteropServices;
using System.Text;
using Microsoft.Win32.SafeHandles;

namespace DstLauncher
{
    /*
     * GodotLaunch —— ui.c 中 Godot 的启动决策链：
     *
     *   godot_runtime_sidecar_preflight / godot_runtime_autoload_preflight /
     *   godot_main_pack_supported          三个无头预检探测（含结论缓存）
     *   launch_godot_with_pack / *_export_with_runtime_sidecar /
     *   *_with_runtime_sidecar             三条翻译启动路径
     *   launch_game_for_engine             引擎分派（非 Godot 直接启动游戏）
     *   start_godot_patch_worker           派出独立的补丁刷新进程
     *
     * 预检探测在演练模式下依然真的启动被测 exe：它的输出与退出码正是判定输入，
     * 用假结果替代就等于没测。真正"启动游戏"的三处才改为打印计划命令行。
     */
    public static class GodotLaunch
    {
        private const string RuntimeScriptName = "dst_godot_runtime.gd";
        private const string RuntimeScriptRes = "res://dst_godot_runtime.gd";

        #region P/Invoke

        [StructLayout(LayoutKind.Sequential)]
        private struct STARTUPINFOW
        {
            public int cb;
            public IntPtr lpReserved, lpDesktop, lpTitle;
            public int dwX, dwY, dwXSize, dwYSize, dwXCountChars, dwYCountChars, dwFillAttribute;
            public int dwFlags;
            public short wShowWindow, cbReserved2;
            public IntPtr lpReserved2, hStdInput, hStdOutput, hStdError;
        }

        [StructLayout(LayoutKind.Sequential)]
        private struct PROCESS_INFORMATION
        {
            public IntPtr hProcess, hThread;
            public int dwProcessId, dwThreadId;
        }

        [StructLayout(LayoutKind.Sequential)]
        private struct SECURITY_ATTRIBUTES
        {
            public int nLength;
            public IntPtr lpSecurityDescriptor;
            [MarshalAs(UnmanagedType.Bool)] public bool bInheritHandle;
        }

        [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
        [return: MarshalAs(UnmanagedType.Bool)]
        private static extern bool CreateProcessW(string applicationName, StringBuilder commandLine,
                                                  IntPtr processAttributes, IntPtr threadAttributes,
                                                  [MarshalAs(UnmanagedType.Bool)] bool inheritHandles,
                                                  uint creationFlags, IntPtr environment,
                                                  string currentDirectory,
                                                  ref STARTUPINFOW startupInfo,
                                                  out PROCESS_INFORMATION processInformation);

        [DllImport("kernel32.dll", SetLastError = true)]
        private static extern uint WaitForSingleObject(IntPtr handle, uint milliseconds);

        [DllImport("kernel32.dll", SetLastError = true)]
        [return: MarshalAs(UnmanagedType.Bool)]
        private static extern bool GetExitCodeProcess(IntPtr handle, out uint exitCode);

        [DllImport("kernel32.dll", SetLastError = true)]
        [return: MarshalAs(UnmanagedType.Bool)]
        private static extern bool TerminateProcess(IntPtr handle, uint exitCode);

        [DllImport("kernel32.dll", SetLastError = true)]
        [return: MarshalAs(UnmanagedType.Bool)]
        private static extern bool CloseHandle(IntPtr handle);

        [DllImport("kernel32.dll", SetLastError = true)]
        private static extern IntPtr GetStdHandle(int which);

        [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
        private static extern uint GetTempPathW(uint cap, StringBuilder buffer);

        [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
        private static extern uint GetTempFileNameW(string dir, string prefix, uint unique, StringBuilder buffer);

        [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
        private static extern SafeFileHandle CreateFileW(string path, uint access, uint share,
                                                         ref SECURITY_ATTRIBUTES sa, uint disposition,
                                                         uint flags, IntPtr template);

        [DllImport("kernel32.dll", SetLastError = true)]
        [return: MarshalAs(UnmanagedType.Bool)]
        private static extern bool SetFilePointerEx(SafeFileHandle handle, long distance, IntPtr newPointer, uint method);

        [DllImport("kernel32.dll", SetLastError = true)]
        [return: MarshalAs(UnmanagedType.Bool)]
        private static extern bool ReadFile(SafeFileHandle handle, byte[] buffer, uint toRead,
                                            out uint read, IntPtr overlapped);

        private const uint WAIT_OBJECT_0 = 0;
        private const uint WAIT_TIMEOUT = 258;
        private const uint CREATE_NO_WINDOW = 0x08000000;
        private const int STARTF_USESHOWWINDOW = 0x00000001;
        private const int STARTF_USESTDHANDLES = 0x00000100;
        private const short SW_HIDE = 0;

        #endregion

        /* 预检/启动进程的统一创建入口，保持 C 版的 CreateProcessW 参数形状：
           lpApplicationName = exe，lpCommandLine = 完整命令行（含 argv[0]）。 */
        private static bool CreateProcess(string exe, string cmd, string cwd, uint flags,
                                          bool hideWindow, bool inherit, IntPtr stdOut,
                                          out PROCESS_INFORMATION pi)
        {
            var si = new STARTUPINFOW();
            si.cb = Marshal.SizeOf(typeof(STARTUPINFOW));
            if (hideWindow) {
                si.dwFlags |= STARTF_USESHOWWINDOW;
                si.wShowWindow = SW_HIDE;
            }
            if (stdOut != IntPtr.Zero) {
                si.dwFlags |= STARTF_USESTDHANDLES;
                si.hStdInput = GetStdHandle(-10 /* STD_INPUT_HANDLE */);
                si.hStdOutput = stdOut;
                si.hStdError = stdOut;
            }
            /* CreateProcessW 会就地改写 lpCommandLine，托管 string 不可写，必须用缓冲。 */
            var buffer = new StringBuilder(cmd, LaunchFlow.CmdCap);
            bool ok = CreateProcessW(exe, buffer, IntPtr.Zero, IntPtr.Zero, inherit, flags,
                                     IntPtr.Zero, cwd, ref si, out pi);
            if (!ok) SafeFs.LastError = Marshal.GetLastWin32Error();
            return ok;
        }

        /* ---------------- 预检探测 ---------------- */

        /* godot_runtime_sidecar_preflight：桥接脚本带一个私有参数，脚本真正加载后
           立即退出，因此一个短暂的隐藏进程即可验证 --script 支持是否可用。 */
        public static bool RuntimeSidecarPreflight(string dir, string runtimeExe, string pack,
                                                   string script, bool looseProject)
        {
            if (dir == null || string.IsNullOrEmpty(script)) return false;
            string exe;
            if (!string.IsNullOrEmpty(runtimeExe)) {
                exe = runtimeExe;
            } else {
                exe = EngineDetector.FindExe(dir);
                if (exe == null) return false;
            }

            /* 预检结论缓存：引擎/游戏/脚本/补丁任一变化即未命中。
               只有成功结论入缓存——超时等瞬态失败下次启动会重新预检。 */
            var sig = new GodotPreflightCache.Sig();
            sig.AddFile(exe);
            sig.AddText(dir);
            if (!string.IsNullOrEmpty(pack)) sig.AddFile(pack);
            else sig.AddText("nopack");
            if (script.StartsWith("res://", StringComparison.Ordinal)) sig.AddText(script);
            else sig.AddFile(script);
            sig.AddText(looseProject ? "loose" : "export");
            string signature = sig.ToString();
            int cached = GodotPreflightCache.Get("sidecar", signature);
            if (cached >= 0) {
                Log.Append(cached != 0
                    ? "Godot: sidecar preflight cache hit (supported)."
                    : "Godot: sidecar preflight cache hit (not supported).");
                return cached != 0;
            }

            string cmd;
            if (looseProject) {
                cmd = LaunchFlow.FormatChecked(
                    "\"" + exe + "\" --headless --path \"" + dir +
                    "\" --script \"res://dst_godot_runtime.gd\" -- --dst-preflight");
            } else if (!string.IsNullOrEmpty(pack)) {
                cmd = LaunchFlow.FormatChecked(
                    "\"" + exe + "\" --headless --main-pack \"" + pack +
                    "\" --script \"" + script + "\" -- --dst-preflight");
            } else {
                cmd = LaunchFlow.FormatChecked(
                    "\"" + exe + "\" --headless --script \"" + script + "\" -- --dst-preflight");
            }
            if (cmd == null) {
                Log.Append("Godot: runtime sidecar preflight command is too long.");
                return false;
            }

            PROCESS_INFORMATION pi;
            if (!CreateProcess(exe, cmd, dir, CREATE_NO_WINDOW, true, false, IntPtr.Zero, out pi)) {
                Log.Append("Godot: runtime sidecar preflight could not start. Windows error: " + SafeFs.LastError);
                return false;
            }
            CloseHandle(pi.hThread);

            uint waited = WaitForSingleObject(pi.hProcess, 5000);
            uint exitCode = 1;
            bool ok = waited == WAIT_OBJECT_0 && GetExitCodeProcess(pi.hProcess, out exitCode) && exitCode == 0;
            if (waited == WAIT_TIMEOUT) {
                TerminateProcess(pi.hProcess, 1);
                WaitForSingleObject(pi.hProcess, 1000);
                Log.Append("Godot: runtime sidecar preflight timed out.");
            } else if (!ok) {
                Log.Append("Godot: runtime sidecar preflight exited with code " + exitCode + ".");
            }
            CloseHandle(pi.hProcess);
            /* 只有成功结论入缓存；超时/非零退出可能是瞬态或游戏侧问题，不缓存。 */
            if (ok) GodotPreflightCache.Put("sidecar", signature, 1);
            return ok;
        }

        /* godot_runtime_autoload_preflight：format 3 补丁包把翻译器注册为 autoload，
           匹配的启动器自有可执行文件只需一个私有用户参数即可验证该路径可用。 */
        public static bool RuntimeAutoloadPreflight(string dir, string runtimeExe)
        {
            if (dir == null || string.IsNullOrEmpty(runtimeExe)) return false;

            var sig = new GodotPreflightCache.Sig();
            sig.AddFile(runtimeExe);
            sig.AddText(dir);
            string signature = sig.ToString();
            int cached = GodotPreflightCache.Get("autoload", signature);
            if (cached >= 0) {
                Log.Append(cached != 0
                    ? "Godot: runtime autoload preflight cache hit (supported)."
                    : "Godot: runtime autoload preflight cache hit (not supported).");
                return cached != 0;
            }

            string cmd = LaunchFlow.FormatChecked("\"" + runtimeExe + "\" --headless -- --dst-preflight");
            if (cmd == null) {
                Log.Append("Godot: runtime autoload preflight command is too long.");
                return false;
            }

            PROCESS_INFORMATION pi;
            if (!CreateProcess(runtimeExe, cmd, dir, CREATE_NO_WINDOW, true, false, IntPtr.Zero, out pi)) {
                Log.Append("Godot: runtime autoload preflight could not start. Windows error: " + SafeFs.LastError);
                return false;
            }
            CloseHandle(pi.hThread);

            uint waited = WaitForSingleObject(pi.hProcess, 5000);
            uint exitCode = 1;
            bool ok = waited == WAIT_OBJECT_0 && GetExitCodeProcess(pi.hProcess, out exitCode) && exitCode == 0;
            if (waited == WAIT_TIMEOUT) {
                TerminateProcess(pi.hProcess, 1);
                WaitForSingleObject(pi.hProcess, 1000);
                Log.Append("Godot: runtime autoload preflight timed out.");
            } else if (!ok) {
                Log.Append("Godot: runtime autoload preflight exited with code " + exitCode + ".");
            }
            CloseHandle(pi.hProcess);
            if (ok) GodotPreflightCache.Put("autoload", signature, 1);
            return ok;
        }

        /* godot_main_pack_supported：进程以非零码退出并不足以证明 --main-pack 不受支持
           （导出的游戏可能在自身 autoload/DRM/音频初始化里失败）。只有明确提及该选项的
           命令行解析诊断才归类为"拒绝"，其余情况保持"结论不确定"并继续尝试真实启动。 */
        public static bool MainPackSupported(string dir, string runtimeExe, string pack)
        {
            string cmd = LaunchFlow.FormatChecked(
                "\"" + runtimeExe + "\" --headless --main-pack \"" + pack + "\" --quit");
            if (cmd == null) {
                Log.Append("Godot: --main-pack probe command is too long.");
                return false;
            }

            var sig = new GodotPreflightCache.Sig();
            sig.AddFile(runtimeExe);
            sig.AddFile(pack);
            sig.AddText(dir);
            string signature = sig.ToString();
            int cached = GodotPreflightCache.Get("mainpack", signature);
            if (cached >= 0) {
                Log.Append(cached != 0
                    ? "Godot: --main-pack probe cache hit (supported)."
                    : "Godot: --main-pack probe cache hit (explicitly rejected).");
                return cached != 0;
            }

            var inherit = new SECURITY_ATTRIBUTES();
            inherit.nLength = Marshal.SizeOf(typeof(SECURITY_ATTRIBUTES));
            inherit.bInheritHandle = true;
            SafeFileHandle capture = null;
            var tempDir = new StringBuilder(260 * 4);
            uint tempLen = GetTempPathW(260 * 4, tempDir);
            if (tempLen > 0 && tempLen < 260 * 4) {
                var capturePath = new StringBuilder(260 * 4);
                if (GetTempFileNameW(tempDir.ToString(), "dsg", 0, capturePath) != 0) {
                    capture = CreateFileW(capturePath.ToString(),
                                          0x80000000u | 0x40000000u /* GENERIC_READ|GENERIC_WRITE */,
                                          0x1u | 0x2u | 0x4u /* FILE_SHARE_READ|WRITE|DELETE */,
                                          ref inherit, 2 /* CREATE_ALWAYS */,
                                          0x100u | 0x04000000u /* TEMPORARY | DELETE_ON_CLOSE */,
                                          IntPtr.Zero);
                    if (capture.IsInvalid) {
                        SafeFs.LastError = Marshal.GetLastWin32Error();
                        capture.Dispose();
                        capture = null;
                    }
                }
            }
            if (capture == null) {
                Log.Append("Godot: --main-pack probe output capture could not be created; treating support as inconclusive (Windows error " +
                           SafeFs.LastError + ").");
                return true;
            }

            PROCESS_INFORMATION pi;
            if (!CreateProcess(runtimeExe, cmd, dir, CREATE_NO_WINDOW, true, true,
                               capture.DangerousGetHandle(), out pi)) {
                /* 结论不确定：让真实启动路径去尝试并记录它自己的错误。 */
                Log.Append("Godot: --main-pack probe could not start. Windows error: " + SafeFs.LastError);
                capture.Dispose();
                return true;
            }
            CloseHandle(pi.hThread);

            uint waited = WaitForSingleObject(pi.hProcess, 5000);
            uint exitCode = 1;
            if (waited == WAIT_TIMEOUT) {
                TerminateProcess(pi.hProcess, 1);
                WaitForSingleObject(pi.hProcess, 1000);
                Log.Append("Godot: --main-pack probe kept running past the deadline; treating --main-pack as supported.");
            }

            /* C 版读进 char output[16385] 并按 C 字符串扫描，这里保持同样的字节视角。 */
            var buffer = new byte[16384];
            uint got = 0;
            string output = "";
            if (SetFilePointerEx(capture, 0, IntPtr.Zero, 0 /* FILE_BEGIN */) &&
                ReadFile(capture, buffer, (uint)buffer.Length, out got, IntPtr.Zero)) {
                output = ByteStr.FromBytes(buffer, 0, (int)got);
            }
            capture.Dispose();

            bool exitedNonzero = waited == WAIT_OBJECT_0 &&
                                 GetExitCodeProcess(pi.hProcess, out exitCode) && exitCode != 0;
            bool rejected = exitedNonzero && GodotProbe.OutputExplicitlyRejectsMainPack(ByteStr.CStr(output));
            if (rejected) {
                Log.Append("Godot: --main-pack probe received an explicit option-rejection diagnostic (exit " + exitCode + ").");
            } else if (exitedNonzero) {
                Log.Append("Godot: --main-pack probe exited with code " + exitCode +
                           " for a non-option startup error; support remains inconclusive and translation launch will still be attempted.");
            } else if (waited != WAIT_OBJECT_0 && waited != WAIT_TIMEOUT) {
                Log.Append("Godot: --main-pack probe wait failed; support remains inconclusive (Windows error " +
                           Marshal.GetLastWin32Error() + ").");
            }
            CloseHandle(pi.hProcess);
            /* 确定性结论入缓存：明确拒绝（不支持）与确认支持（成功/超时保活）。
               非选项性启动错误 = 结论不确定，不入缓存，下次仍会重新探测。 */
            if (rejected) GodotPreflightCache.Put("mainpack", signature, 0);
            else if (!exitedNonzero) GodotPreflightCache.Put("mainpack", signature, 1);
            return !rejected;
        }

        /* ---------------- 翻译启动路径 ---------------- */

        private static bool StartLaunch(string exe, string cmd, string dir)
        {
            if (LaunchFlow.DryRun) {
                LaunchFlow.SpawnSink("proc", exe, cmd, dir);
                return true;
            }
            PROCESS_INFORMATION pi;
            if (!CreateProcess(exe, cmd, dir, 0, false, false, IntPtr.Zero, out pi)) return false;
            CloseHandle(pi.hThread);
            CloseHandle(pi.hProcess);
            return true;
        }

        public static bool LaunchGodotWithPack(string dir, string pack)
        {
            string exe = EngineDetector.FindExe(dir);
            if (exe == null) {
                Log.Append("Game exe not found.");
                return false;
            }
            string patchLauncher = GodotPatch.PreparePatchLauncher(dir);
            bool hasPatchLauncher = patchLauncher != null;
            string runtimeExe = hasPatchLauncher ? patchLauncher : exe;

            string script = PathUtil.Join(dir, RuntimeScriptName);
            string runtimeScript = script;
            bool embeddedRuntimeAutoload = hasPatchLauncher && GodotPatch.PatchPackHasRuntimeAutoload(pack);
            bool hasRuntimeAutoload = embeddedRuntimeAutoload && RuntimeAutoloadPreflight(dir, runtimeExe);
            if (embeddedRuntimeAutoload && !hasRuntimeAutoload) {
                Log.Append("Godot: runtime autoload preflight failed; trying script-launch compatibility.");
            }
            bool embeddedRuntimeBridge = GodotPatch.PatchPackHasRuntimeSidecar(pack);
            bool preparedRuntimeBridge = embeddedRuntimeBridge ||
                                         (GodotPatch.PrepareRuntimeSidecar(dir) && PathUtil.Exists(script));
            if (embeddedRuntimeBridge) runtimeScript = RuntimeScriptRes;
            bool hasRuntimeBridge = !hasRuntimeAutoload && preparedRuntimeBridge &&
                                    RuntimeSidecarPreflight(dir, runtimeExe,
                                                            hasPatchLauncher ? null : pack,
                                                            runtimeScript, false);
            if (preparedRuntimeBridge && !hasRuntimeBridge) {
                Log.Append("Godot: runtime sidecar preflight failed; falling back to the static launch path.");
            }

            string cmd;
            if (hasRuntimeAutoload) {
                cmd = LaunchFlow.FormatChecked("\"" + runtimeExe + "\" --language en");
            } else if (hasPatchLauncher && hasRuntimeBridge) {
                cmd = LaunchFlow.FormatChecked("\"" + runtimeExe + "\" --script \"" + runtimeScript + "\" --language en");
            } else if (hasPatchLauncher) {
                cmd = LaunchFlow.FormatChecked("\"" + runtimeExe + "\" --language en");
            } else if (hasRuntimeBridge) {
                cmd = LaunchFlow.FormatChecked("\"" + runtimeExe + "\" --main-pack \"" + pack +
                                               "\" --script \"" + runtimeScript + "\" --language en");
            } else {
                /* 静态回退只能经 --main-pack 加载补丁包。sidecar preflight（同样带
                   --main-pack）已失败时，单独探测该参数；模板拒绝则退回普通启动，
                   而不是拉起一个会立即退出的进程。sidecar 本地准备失败（未执行
                   preflight）时没有模板信息，保留原有静态尝试。 */
                if (preparedRuntimeBridge && !MainPackSupported(dir, runtimeExe, pack)) {
                    Log.Append("Godot: template rejects --main-pack; falling back to a normal launch without the translation patch pack.");
                    return false;
                }
                cmd = LaunchFlow.FormatChecked("\"" + runtimeExe + "\" --main-pack \"" + pack + "\" --language en");
            }
            if (cmd == null) {
                Log.Append("Godot: translated launch command is too long; no process was started.");
                return false;
            }

            Log.Append((hasRuntimeAutoload || hasRuntimeBridge)
                ? "Launching Godot export with patch pack and runtime translator: " + runtimeExe
                : "Launching Godot export with static patch pack: " + runtimeExe);
            if (!StartLaunch(runtimeExe, cmd, dir)) {
                Log.Append("Godot: failed to launch with patch pack. Windows error: " + SafeFs.LastError);
                return false;
            }
            return true;
        }

        public static bool LaunchGodotExportWithRuntimeSidecar(string dir)
        {
            string exe = EngineDetector.FindExe(dir);
            if (exe == null) {
                Log.Append("Game exe not found.");
                return false;
            }
            if (!GodotPatch.PrepareRuntimeSidecar(dir)) return false;
            string script = PathUtil.Join(dir, RuntimeScriptName);
            if (!PathUtil.Exists(script) || !RuntimeSidecarPreflight(dir, exe, null, script, false)) {
                Log.Append("Godot: runtime sidecar preflight failed; falling back to the static launch path.");
                return false;
            }

            string cmd = LaunchFlow.FormatChecked("\"" + exe + "\" --script \"" + script + "\" --language en");
            if (cmd == null) {
                Log.Append("Godot: export sidecar launch command is too long.");
                return false;
            }

            Log.Append("Launching Godot export with runtime translator before static patch is ready: " + exe);
            if (!StartLaunch(exe, cmd, dir)) {
                Log.Append("Godot: failed to launch export runtime sidecar. Windows error: " + SafeFs.LastError);
                return false;
            }
            return true;
        }

        public static bool LaunchGodotWithRuntimeSidecar(string dir)
        {
            string exe = EngineDetector.FindExe(dir);
            if (exe == null) {
                Log.Append("Game exe not found.");
                return false;
            }
            string script = PathUtil.Join(dir, RuntimeScriptName);
            if (!PathUtil.Exists(script) || !RuntimeSidecarPreflight(dir, exe, null, script, true)) {
                Log.Append("Godot: runtime sidecar preflight failed; falling back to the normal launch path.");
                return false;
            }

            string cmd = LaunchFlow.FormatChecked(
                "\"" + exe + "\" --path \"" + dir + "\" --script \"res://dst_godot_runtime.gd\" --language en");
            if (cmd == null) {
                Log.Append("Godot: loose-project sidecar launch command is too long.");
                return false;
            }

            Log.Append("Launching Godot loose project with runtime translator: " + exe);
            if (!StartLaunch(exe, cmd, dir)) {
                Log.Append("Godot: failed to launch with runtime sidecar. Windows error: " + SafeFs.LastError);
                return false;
            }
            return true;
        }

        /* launch_game_for_engine：Godot 走补丁包/侧车路径，其余引擎直接启动游戏 exe。 */
        public static void LaunchGameForEngine(string dir, Engine engine)
        {
            if (engine == Engine.Godot) {
                if (GodotPatch.IsLooseProject(dir)) {
                    if (GodotPatch.PrepareRuntimeSidecar(dir)) {
                        Log.Append("Godot: loose project detected; launching with runtime translation sidecar.");
                        if (LaunchGodotWithRuntimeSidecar(dir)) return;
                        Log.Append("Godot: runtime-sidecar launch failed; falling back to normal game launch.");
                    } else {
                        Log.Append("Godot: runtime sidecar could not be prepared; falling back to normal game launch.");
                    }
                    LaunchFlow.LaunchGame(dir);
                    return;
                }
                GodotPatch.PromoteStagedPatchPack(dir);
                string pack = PathUtil.Join(dir, "dst_godot_patch.pck");
                if (PathUtil.Exists(pack)) {
                    Log.Append("Godot: launching with external translation patch pack.");
                    if (LaunchGodotWithPack(dir, pack)) return;
                    Log.Append("Godot: patch-pack launch failed; falling back to normal game launch.");
                } else {
                    Log.Append("Godot: no patch pack yet; using the generic runtime translator for this launch.");
                    if (LaunchGodotExportWithRuntimeSidecar(dir)) return;
                    Log.Append("Godot: export runtime-sidecar launch failed; falling back to normal game launch.");
                }
            }
            LaunchFlow.LaunchGame(dir);
        }

        /* start_godot_patch_worker：以隐藏窗口启动 --godot-patch-worker 子进程重建补丁包。
           工作进程没有窗口，其日志只到 OutputDebugString，因此父进程用一个监视线程把
           退出码记进启动器日志（2=参数错误，3=服务器未就绪，4=补丁包构建失败）。 */
        public static bool StartGodotPatchWorker(string dir)
        {
            if (string.IsNullOrEmpty(dir)) return false;

            string exe = Assembly.GetEntryAssembly().Location;
            if (string.IsNullOrEmpty(exe)) return false;

            string cmd = LaunchFlow.FormatChecked("\"" + exe + "\" --godot-patch-worker \"" + dir + "\"");
            if (cmd == null) {
                Log.Append("Godot: patch worker command is too long.");
                return false;
            }

            if (LaunchFlow.DryRun) {
                LaunchFlow.SpawnSink("worker", exe, cmd, Launcher.Root);
                Log.Append("Godot: patch refresh worker started.");
                return true;
            }

            PROCESS_INFORMATION pi;
            string cwd = string.IsNullOrEmpty(Launcher.Root) ? null : Launcher.Root;
            var si = new STARTUPINFOW();
            si.cb = Marshal.SizeOf(typeof(STARTUPINFOW));
            si.dwFlags = STARTF_USESHOWWINDOW;
            si.wShowWindow = SW_HIDE;
            var buffer = new StringBuilder(cmd, LaunchFlow.CmdCap);
            if (!CreateProcessW(null, buffer, IntPtr.Zero, IntPtr.Zero, false, CREATE_NO_WINDOW,
                                IntPtr.Zero, cwd, ref si, out pi)) {
                Log.Append("Godot: failed to start patch refresh worker. Windows error: " + Marshal.GetLastWin32Error());
                return false;
            }

            CloseHandle(pi.hThread);
            IntPtr process = pi.hProcess;
            try {
                var watch = new System.Threading.Thread(() => WatchPatchWorker(process));
                watch.IsBackground = true;
                watch.Start();
            } catch (Exception ex) {
                Log.Append("Godot: could not monitor the patch refresh worker (" + ex.GetType().Name +
                           ": " + ex.Message + "); its exit code will not be recorded.");
                CloseHandle(process);
            }
            Log.Append("Godot: patch refresh worker started.");
            return true;
        }

        private static void WatchPatchWorker(IntPtr process)
        {
            WaitForSingleObject(process, 0xFFFFFFFFu /* INFINITE */);
            uint code;
            if (!GetExitCodeProcess(process, out code)) {
                Log.Append("Godot: patch refresh worker exit code unavailable. Windows error: " + Marshal.GetLastWin32Error());
            } else if (code == 0) {
                Log.Append("Godot: patch refresh worker finished successfully.");
            } else {
                Log.Append("Godot: patch refresh worker exited with code " + code + ".");
            }
            CloseHandle(process);
        }
    }
}
