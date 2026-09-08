using System;
using System.IO;

namespace DstLauncher
{
    /*
     * SelfUpdate —— self_update.c 的对等实现：把启动器内嵌的自有运行时 payload 同步到
     * Launcher.Root 下的固定位置。
     *
     * C 版由 build_native.bat 以 RCDATA 101..106 / 201..205 嵌入；C# 版在 launcher_cs.csproj
     * 里用同名 LogicalName 的 EmbeddedResource 嵌入同一批文件。边界约束与 C 版相同：
     *   - 只同步本项目自有文件，第三方运行时（BepInEx、XUnity 等）仍由安装脚本负责；
     *   - 不触碰 api.ini、翻译记忆、日志或游戏目录；
     *   - 目标内容相同就不重写（流式比较，不改时间戳）；
     *   - 更新走同目录临时文件 + 原子替换，失败时删除临时文件并保留原文件。
     */
    public static class SelfUpdate
    {
        private struct EmbeddedPayload
        {
            public string Name;         /* EmbeddedResource LogicalName（= C 版资源宏名） */
            public string RelativePath; /* 相对 Launcher.Root 的释放位置 */
            public string Label;        /* 仅用于日志 */
            public bool Important;      /* 缺失时是否计入"构建不完整" */

            public EmbeddedPayload(string name, string relativePath, string label, bool important)
            {
                Name = name; RelativePath = relativePath; Label = label; Important = important;
            }
        }

        /* 顺序与 self_update.c 的 EMBEDDED_PAYLOADS 一致（决定日志与写盘顺序）。 */
        private static readonly EmbeddedPayload[] Payloads = {
            new EmbeddedPayload("IDR_PAYLOAD_DST_SERVER",         @"native\dst_server.exe", "dst_server.exe", true),
            new EmbeddedPayload("IDR_PAYLOAD_INSTALLER",          @"scripts\install_runtime_payloads.ps1", "install_runtime_payloads.ps1", true),
            new EmbeddedPayload("IDR_PAYLOAD_API_EXAMPLE",        @"config\api.ini.example", "api.ini.example", true),
            new EmbeddedPayload("IDR_PAYLOAD_LAUNCHER_EXAMPLE",   @"config\launcher.ini.example", "launcher.ini.example", true),
            new EmbeddedPayload("IDR_PAYLOAD_DST_SERVER_CS",      @"native\dst_server_cs.exe", "dst_server_cs.exe", true),
            new EmbeddedPayload("IDR_PAYLOAD_GLOSSARY_EXAMPLE",   @"config\glossary.example.tsv", "glossary.example.tsv", true),
            new EmbeddedPayload("IDR_PAYLOAD_UNITY_MONO5",        @"payloads\UnityTranslator\UnityTranslator.dll", "UnityTranslator.dll", false),
            new EmbeddedPayload("IDR_PAYLOAD_UNITY_MONO6",        @"payloads\UnityTranslator\UnityTranslator.BepInEx6.dll", "UnityTranslator.BepInEx6.dll", false),
            new EmbeddedPayload("IDR_PAYLOAD_XUNITY_ENDPOINT",    @"payloads\UnityIL2CPP\DeepSeekXUnityTranslator\DeepSeekTranslate.dll", "DeepSeekTranslate.dll", false),
            new EmbeddedPayload("IDR_PAYLOAD_TMP_FALLBACK",       @"payloads\UnityIL2CPP\DeepSeekTMPFontFallback\BepInEx\plugins\DeepSeekTMPFontFallback\DeepSeekTMPFontFallback.dll", "DeepSeekTMPFontFallback.dll", false),
            new EmbeddedPayload("IDR_PAYLOAD_UNITY_FONT_PATCHER", @"payloads\UnityTranslator\DeepSeekUnityFontPatcher.dll", "DeepSeekUnityFontPatcher.dll", false),
        };

        /* ensure_parent_dir：只创建父目录；没有反斜杠时视为当前目录，直接成功。 */
        private static bool EnsureParentDir(string path)
        {
            int slash = path.LastIndexOf('\\');
            if (slash < 0) return true;
            return SafeFs.EnsureDir(path.Substring(0, slash));
        }

        /* file_matches_bytes：大小相同且逐块（64 KiB）比较相同。打不开/读不满/不同都为 false。 */
        private static bool FileMatchesBytes(string path, byte[] data)
        {
            FileStream fs;
            try {
                fs = new FileStream(path, FileMode.Open, FileAccess.Read, FileShare.ReadWrite | FileShare.Delete);
            } catch (IOException) {
                return false; /* CreateFileW 失败：不存在或被独占 */
            } catch (UnauthorizedAccessException) {
                return false;
            }
            using (fs) {
                if (fs.Length != data.Length) return false;
                var buf = new byte[64 * 1024];
                int offset = 0;
                try {
                    while (offset < data.Length) {
                        int want = Math.Min(buf.Length, data.Length - offset);
                        int got = fs.Read(buf, 0, want);
                        if (got != want) return false;
                        for (int i = 0; i < want; i++) {
                            if (buf[i] != data[offset + i]) return false;
                        }
                        offset += got;
                    }
                } catch (IOException) {
                    return false; /* ReadFile 失败 */
                }
                return true;
            }
        }

        /* write_bytes_atomic：同目录 .dstmp 临时文件 + MoveFileExW(REPLACE_EXISTING|COPY_ALLOWED|
           WRITE_THROUGH)；替换失败时删除临时文件，原文件保持不变。 */
        private static bool WriteBytesAtomic(string path, byte[] data)
        {
            if (!EnsureParentDir(path)) return false;
            string tmp = path + ".dstmp";
            if (!SafeFs.WriteBytes(tmp, data, data.Length)) return false;
            if (!SafeFs.MoveFileSafe(tmp, path,
                                     SafeFs.MOVEFILE_REPLACE_EXISTING | SafeFs.MOVEFILE_COPY_ALLOWED |
                                     SafeFs.MOVEFILE_WRITE_THROUGH)) {
                SafeFs.DeleteFileSafe(tmp);
                return false;
            }
            return true;
        }

        /* sync_embedded_payloads：返回 true 表示没有失败且必需组件齐全。日志措辞与 C 版一致。 */
        public static bool SyncEmbeddedPayloads()
        {
            int updated = 0;
            int failed = 0;
            int missingImportant = 0;

            foreach (var p in Payloads) {
                byte[] data = EmbeddedScripts.BytesOrNull(p.Name);
                if (data == null) {
                    if (p.Important) missingImportant++;
                    continue;
                }

                string dst = PathUtil.Join(Launcher.Root, p.RelativePath);
                if (FileMatchesBytes(dst, data)) continue;

                if (WriteBytesAtomic(dst, data)) {
                    updated++;
                } else {
                    failed++;
                    Log.Append("Built-in component update failed: " + p.Label);
                }
            }

            if (updated > 0) {
                Log.Append("Built-in components synced: " + updated + " file(s).");
            }
            if (missingImportant > 0) {
                Log.Append("Warning: launcher was built without " + missingImportant + " required embedded component(s).");
            }
            if (failed > 0) {
                Log.Append("Warning: " + failed + " built-in component(s) could not be updated. Close running games/server and restart the launcher.");
            }
            return failed == 0 && missingImportant == 0;
        }
    }
}
