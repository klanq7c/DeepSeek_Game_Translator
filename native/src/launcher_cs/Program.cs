using System;
using System.IO;
using System.Text;

namespace DstLauncher
{
    /*
     * 程序入口。阶段 3 进行中：目前只提供诊断子命令，供 tests/launcher_parity
     * 把 C# 移植与 C 启动器（--detect-and-exit / --deploy-and-exit /
     * --restore-and-exit）的输出逐字节对比。
     *
     *   --detect <dir>            引擎检测报告
     *   --deploy <dir>            检测引擎后执行 Deploy.ForEngine，日志镜像为 log= 行，末行 result=
     *   --restore <dir>           检测引擎后执行 Deploy.Restore，同上
     *   --warmup <dir>            检测引擎后执行 Warmup.Run 的扫描部分（对应 C 版
     *                             --warmup-and-exit）：不联网、不查缓存、不回写文件，每个
     *                             待提交批次输出 post=<path> <body>，末行 result=
     *   --sync-payloads           SelfUpdate.SyncEmbeddedPayloads（对应 C 版
     *                             --sync-payloads-and-exit）：把内嵌 payload 同步到 --root，
     *                             日志镜像为 log= 行，末行 result=，退出码 0 / 5 与 C 版相同
     *   --godot-patch <dir>       GodotPatch.PreparePatchPack（对应 C 版 --godot-patch-and-exit）：
     *                             成功时先输出 pack=<路径>，末行 result=
     *   --godot-promote <dir>     GodotPatch.PromoteStagedPatchPack（对应 --godot-promote-and-exit）
     *   --godot-launcher <dir>    GodotPatch.PreparePatchLauncher（对应 --godot-launcher-and-exit）：
     *                             成功时先输出 launcher=<路径>，末行 result=
     *   --launch-flow <dir>       LaunchFlow.RunEngineLaunchFlow（对应 --launch-flow-and-exit）：
     *                             预热按 --warmup 的方式转储 post= 行，真正拉起进程的三处
     *                             改为 spawn=<kind>|<exe>|<cmd>|<cwd>，状态推进为 status= 行
     *   --clear-cache             缓存卡片文本 cache=，删除共享缓存后再打印 cache2=，
     *                             末行 result=（对应 --clear-cache-and-exit）
     *   --godot-patch-worker <dir>  重建 Godot 补丁包的隐藏辅助进程（非诊断，退出码
     *                             2/3/4 与 C 版一致）
     *   --api-config              ApiConfig 预设表 + 当前 api.ini 三个键 + 预选下标
     *                             （对应 C 版 --api-config-and-exit）
     *   --api-config-set <e> <m> <k>  ApiConfig.Save（对应 --api-config-set-and-exit）
     *   --godot-probe <text>...   GodotProbe.OutputExplicitlyRejectsMainPack，每个参数
     *                             一行 reject<i>=（对应 --godot-probe-and-exit）
     *   --godot-preflight <kind> <file> <text> <put>
     *                             GodotPreflightCache 签名/查询/写入（对应
     *                             --godot-preflight-and-exit）
     *   --ui-probe <w> <h> <alive> <out.bmp>
     *                             把主窗口客户区渲染成确定的一帧（对应
     *                             --ui-probe-and-exit）：布局报告走 stdout，
     *                             像素写成 32bpp BMP，两者都逐字节比对
     *   --ui-identity             本二进制真实的运行时标签与副标题（探针会中性化
     *                             这两处，真实取值由此单独暴露，对应
     *                             --ui-identity-and-exit）
     *   --server-smoke            ServerProcess 生命周期冒烟（见 ServerSmoke）
     *   --root <launcher-dir>     覆盖启动器根目录（payloads/、config/ 由此派生）
     *
     * 不带任何标志时就是正常的图形启动（MainWindow.Run）。
     *
     * 诊断输出全部走 stdout、UTF-8 无 BOM、LF 换行，字段顺序固定；两边任何差异都
     * 视为移植缺陷而不是"实现细节"。尚未移植的路径返回 result=-1 并退出码 3，
     * 绝不伪装成 C 版的结果。
     */
    internal static class Program
    {
        private static readonly UTF8Encoding Utf8 = new UTF8Encoding(false);
        private static Stream _stdout;

        /* ATTACH_PARENT_PROCESS。已经有控制台（或父进程没有）时返回 false，
           那正是 stdout 已被重定向或无处可写的情况，无需处理。 */
        [System.Runtime.InteropServices.DllImport("kernel32.dll")]
        private static extern bool AttachConsole(uint processId);

        private static int Main(string[] args)
        {
            string mode = null;
            string dir = null;
            string[] rest = null;
            /* --root 先单独摘出去：变长参数模式会吞掉标志之后的全部参数，若留在原地
               就会被当成待判定的文本。 */
            var kept = new System.Collections.Generic.List<string>(args.Length);
            for (int i = 0; i < args.Length; i++) {
                if (args[i] == "--root" && i + 1 < args.Length) {
                    Launcher.Root = args[++i];
                    continue;
                }
                kept.Add(args[i]);
            }
            args = kept.ToArray();
            for (int i = 0; i < args.Length; i++) {
                if (args[i] == "--server-smoke" || args[i] == "--sync-payloads" ||
                    args[i] == "--api-config" || args[i] == "--clear-cache" ||
                    args[i] == "--ui-identity") {
                    mode = args[i];
                    dir = ".";
                    continue;
                }
                if (args[i] == "--ui-probe") {
                    mode = args[i];
                    dir = ".";
                    var tail = new string[args.Length - i - 1];
                    Array.Copy(args, i + 1, tail, 0, tail.Length);
                    rest = tail;
                    i = args.Length;
                    continue;
                }
                if (args[i] == "--api-config-set" || args[i] == "--godot-probe" || args[i] == "--godot-preflight") {
                    /* 变长参数模式：其余参数全部归本模式（C 版同样取该标志之后的参数）。 */
                    mode = args[i];
                    dir = ".";
                    var tail = new string[args.Length - i - 1];
                    Array.Copy(args, i + 1, tail, 0, tail.Length);
                    rest = tail;
                    i = args.Length;
                    continue;
                }
                if (args[i] == "--detect" || args[i] == "--deploy" || args[i] == "--restore" ||
                    args[i] == "--warmup" || args[i] == "--godot-patch" ||
                    args[i] == "--godot-promote" || args[i] == "--godot-launcher" ||
                    args[i] == "--launch-flow" || args[i] == "--godot-patch-worker") {
                    mode = args[i];
                    dir = i + 1 < args.Length ? args[i + 1] : null;
                    i++;
                    continue;
                }
            }

            if (mode == null) {
                /* 没有任何诊断标志：这就是正常的图形启动（对应 C 版 wWinMain 的
                   后半段）。窗口层已移植完毕，不再是"未移植"路径。 */
                return MainWindow.Run();
            }
            if (mode == "--usage") {
                WriteErr("dst_launcher_cs: 阶段 3 移植进行中，支持 --detect|--deploy|--restore|--warmup|" +
                         "--godot-patch|--godot-promote|--godot-launcher|--launch-flow|--godot-patch-worker <game-dir> | " +
                         "--sync-payloads | --clear-cache | --api-config | " +
                         "--api-config-set <e> <m> <k> | --godot-probe <text>... | " +
                         "--godot-preflight <kind> <file> <text> <put> | " +
                         "--ui-probe <w> <h> <alive> <out.bmp> | --ui-identity | " +
                         "--server-smoke [--root <dir>]\n");
                return 2;
            }
            if (string.IsNullOrEmpty(dir)) {
                WriteErr("usage: dst_launcher_cs " + mode + " <game-dir>\n");
                return 2;
            }

            /* GUI 子系统的进程默认没有控制台。父进程重定向 stdout 时句柄照常继承
               （parity 脚本走的就是这条路），只有从交互式控制台直接运行时才需要接管
               调用者的控制台，否则诊断输出会静默丢失。 */
            AttachConsole(unchecked((uint)-1));
            _stdout = Console.OpenStandardOutput();
            try {
                if (mode == "--detect") {
                    WriteOut(DetectReport(dir));
                    return 0;
                }
                if (mode == "--ui-identity") {
                    /* 本二进制真实的运行时标签与副标题。探针会把它们中性化以便
                       逐像素比对，真实取值由这里单独暴露给 parity 断言。 */
                    WriteOut("runtime_tag=" + MainWindow.RealRuntimeTag + "\n");
                    WriteOut("subtitle=" + MainWindow.RealSubtitle + "\n");
                    return 0;
                }
                if (mode == "--ui-probe") {
                    /* --ui-probe <w> <h> <alive> <out.bmp> */
                    string[] p = rest ?? new string[0];
                    if (p.Length < 4) return 2;
                    int w, h, alive;
                    if (!int.TryParse(p[0], out w) || !int.TryParse(p[1], out h) ||
                        !int.TryParse(p[2], out alive)) {
                        return 2;
                    }
                    return UiProbe.Run(w, h, alive != 0, p[3], WriteOut);
                }
                Log.SetSink(line => WriteOut("log=" + line + "\n"));
                Log.SetStatusSink(text => WriteOut("status=" + text + "\n"));
                if (mode == "--server-smoke") return ServerSmoke();
                if (mode == "--godot-patch-worker") return GodotPatchWorker(dir);
                if (mode == "--clear-cache") {
                    WriteOut("cache=" + LaunchFlow.CacheSizeText() + "\n");
                    bool cleared = LaunchFlow.ClearCacheFile();
                    WriteOut("cache2=" + LaunchFlow.CacheSizeText() + "\n");
                    WriteOut("result=" + (cleared ? 1 : 0) + "\n");
                    return 0;
                }
                if (mode == "--api-config" || mode == "--api-config-set" ||
                    mode == "--godot-probe" || mode == "--godot-preflight") {
                    return ConfigDiagnostic(mode, rest ?? new string[0]);
                }
                if (mode == "--sync-payloads") {
                    /* C 版 wWinMain：return sync_embedded_payloads() ? 0 : 5 */
                    bool ok = SelfUpdate.SyncEmbeddedPayloads();
                    WriteOut("result=" + (ok ? 0 : 5) + "\n");
                    return ok ? 0 : 5;
                }
                if (mode == "--godot-patch" || mode == "--godot-promote" || mode == "--godot-launcher") {
                    int godotResult;
                    if (mode == "--godot-patch") {
                        string pack = GodotPatch.PreparePatchPack(dir);
                        godotResult = pack != null ? 1 : 0;
                        if (pack != null) WriteOut("pack=" + pack + "\n");
                    } else if (mode == "--godot-launcher") {
                        string launcher = GodotPatch.PreparePatchLauncher(dir);
                        godotResult = launcher != null ? 1 : 0;
                        if (launcher != null) WriteOut("launcher=" + launcher + "\n");
                    } else {
                        godotResult = GodotPatch.PromoteStagedPatchPack(dir) ? 1 : 0;
                    }
                    WriteOut("result=" + godotResult + "\n");
                    return 0;
                }
                Engine engine = EngineDetector.Detect(dir);
                int result;
                if (mode == "--warmup" || mode == "--launch-flow") {
                    /* 与 C 版 warmup_dump_post 同格式：post=<path> <body>\n，body 原始字节直写。 */
                    Warmup.DumpSink = (path, body) => {
                        WriteOut("post=" + path + " ");
                        _stdout.Write(body, 0, body.Length);
                        WriteOut("\n");
                    };
                }
                if (mode == "--launch-flow") {
                    /* 一键流程中"服务器已就绪之后"的部分。真正拉起进程的三处改为
                       spawn= 计划行；预检探测仍然真的启动被测 exe。 */
                    LaunchFlow.SpawnSink = (kind, exe, cmd, cwd) =>
                        WriteOut("spawn=" + kind + "|" + exe + "|" + cmd + "|" + cwd + "\n");
                    LaunchFlow.RunEngineLaunchFlow(dir, engine);
                    WriteOut("result=1\n");
                    return 0;
                }
                if (mode == "--warmup") {
                    result = Warmup.Run(dir, engine);
                } else {
                    result = mode == "--deploy" ? Deploy.ForEngine(dir, engine) : Deploy.Restore(dir, engine);
                }
                WriteOut("result=" + result + "\n");
                return result == Deploy.NotPorted ? 3 : 0;
            } finally {
                _stdout.Flush();
            }
        }

        /* 配置与 Godot 预检层的诊断输出，逐字对应 C 版 run_config_diagnostic_from_cmd。 */
        private static int ConfigDiagnostic(string mode, string[] rest)
        {
            if (mode == "--api-config") {
                WriteOut("presets=" + ApiConfig.Providers.Length + "\n");
                for (int p = 0; p < ApiConfig.Providers.Length; p++) {
                    ApiConfig.Preset preset = ApiConfig.Providers[p];
                    WriteOut("preset" + p + "=" + preset.Name + "|" + (preset.Endpoint ?? "-") +
                             "|" + (preset.Model ?? "-") + "\n");
                }
                ApiConfig.Values values = ApiConfig.Load();
                WriteOut("endpoint=" + values.Endpoint + "\nmodel=" + values.Model + "\nkey=" + values.Key +
                         "\nselected=" + ApiConfig.PresetIndexFor(values.Endpoint) + "\nresult=1\n");
                return 0;
            }
            if (mode == "--api-config-set") {
                if (rest.Length < 3) return 2;
                bool saved = ApiConfig.Save(rest[0], rest[1], rest[2]);
                WriteOut("result=" + (saved ? 1 : 0) + "\n");
                return 0;
            }
            if (mode == "--godot-probe") {
                for (int a = 0; a < rest.Length; a++) {
                    /* C 版先把参数转成 UTF-8 再逐字节判断，这里用同样的字节串。 */
                    byte[] utf8 = Encoding.UTF8.GetBytes(rest[a]);
                    string text = ByteStr.FromBytes(utf8, 0, utf8.Length);
                    WriteOut("reject" + a + "=" + (GodotProbe.OutputExplicitlyRejectsMainPack(text) ? 1 : 0) + "\n");
                }
                WriteOut("result=1\n");
                return 0;
            }
            /* --godot-preflight <kind> <file> <text> <put> */
            if (rest.Length < 4) return 2;
            var sig = new GodotPreflightCache.Sig();
            sig.AddFile(rest[1]);
            sig.AddText(rest[2]);
            string signature = sig.ToString();
            WriteOut("sig=" + signature + "\n");
            WriteOut("get=" + GodotPreflightCache.Get(rest[0], signature) + "\n");
            int put;
            int.TryParse(rest[3], out put);
            if (put >= 0) {
                GodotPreflightCache.Put(rest[0], signature, put);
                WriteOut("get2=" + GodotPreflightCache.Get(rest[0], signature) + "\n");
            }
            WriteOut("result=1\n");
            return 0;
        }

        /* --godot-patch-worker <dir>：游戏已启动后重建 Godot 补丁包的隐藏辅助进程，
           退出码与 C 版 run_godot_patch_worker_from_cmd 一致（2=参数错误、3=服务器
           未就绪、4=补丁包构建失败）。 */
        private static int GodotPatchWorker(string dir)
        {
            if (string.IsNullOrEmpty(dir)) return 2;
            if (!ServerProcess.Start()) return 3;
            Warmup.Run(dir, Engine.Godot);
            return GodotPatch.PreparePatchPack(dir) != null ? 0 : 4;
        }

        /* --server-smoke：用 --root 指向的 native\ 服务器二进制走一遍 启动→/health→停止，
           验证 ServerProcess 与 C 版 server_proc.c 的生命周期契约（含 launcher.ini 的 binary 选择）。
           输出 state= 行（宿主 UI 将来显示的标签/按钮文字）与 result=。 */
        private static int ServerSmoke()
        {
            ServerProcess.StateChanged += s => WriteOut("state=" + s.Label + " | " + s.Button + "\n");
            WriteOut("binary=" + ServerProcess.SelectServerBinary() + "\n");
            bool started = ServerProcess.Start();
            WriteOut("started=" + (started ? 1 : 0) + "\n");
            WriteOut("alive=" + (ServerProcess.Alive() ? 1 : 0) + "\n");
            WriteOut("owned=" + (ServerProcess.Owned ? 1 : 0) + "\n");
            ServerProcess.Stop();
            WriteOut("alive_after_stop=" + (ServerProcess.Alive() ? 1 : 0) + "\n");
            WriteOut("result=" + (started ? 1 : 0) + "\n");
            return started ? 0 : 1;
        }

        /* 与 native/src/launcher/main.c 的 write_detect_report 保持字段一致。 */
        internal static string DetectReport(string dir)
        {
            Engine engine = EngineDetector.Detect(dir);
            string exe = EngineDetector.FindExe(dir);
            string rpgmRoot = EngineDetector.RpgmContentRoot(dir);
            bool il2cpp = EngineDetector.UnityIsIl2cpp(dir);
            bool unityData = EngineDetector.FindSubdirSuffix(dir, "_Data");

            var sb = new StringBuilder();
            sb.Append("engine=").Append(EngineDetector.Name(engine)).Append('\n');
            sb.Append("engine_id=").Append((int)engine).Append('\n');
            sb.Append("exe=").Append(exe ?? "-").Append('\n');
            sb.Append("rpgm_root=").Append(rpgmRoot ?? "-").Append('\n');
            sb.Append("unity_data=").Append(unityData ? 1 : 0).Append('\n');
            sb.Append("il2cpp=").Append(il2cpp ? 1 : 0).Append('\n');
            return sb.ToString();
        }

        private static void WriteOut(string text)
        {
            byte[] bytes = Utf8.GetBytes(text);
            _stdout.Write(bytes, 0, bytes.Length);
        }

        private static void WriteErr(string text)
        {
            using (Stream stderr = Console.OpenStandardError()) {
                byte[] bytes = Utf8.GetBytes(text);
                stderr.Write(bytes, 0, bytes.Length);
            }
        }
    }
}
