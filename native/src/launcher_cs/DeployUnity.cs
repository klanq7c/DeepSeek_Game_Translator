using System;
using System.Text;

namespace DstLauncher
{
    /*
     * Deploy（Unity 部分）—— deploy.c 中 Unity Mono / IL2CPP 的部署与还原逐函数移植。
     *
     * 与 C 版一致的边界：
     *   - 所有第三方运行时（BepInEx、XUnity、Newtonsoft、TMP 字体包、完整 corlib）只从
     *     Launcher.Root\payloads 复制，缺失时记录可复制执行的安装命令，不联网下载；
     *   - 覆盖用户文件前留 .dst-backup 一次性备份；启动器自己写的文件用 .dst-owned /
     *     .dst-installed-by-ds 记录所有权字节快照，还原时只移除与快照/载荷逐字节一致的文件，
     *     其余全部保留并计入 preserved；
     *   - 每个失败分支都带 Windows 错误码写日志，然后返回 false；不会把"未部署"报告为成功。
     *
     * 日志文本与 deploy.c 逐字一致，tests/launcher_parity 的 unity_* 场景比较两版日志与目录树哈希。
     */
    public static partial class Deploy
    {
        private const string XunityOwnerMarkerText = "ds-game-translator:xunity-auto-translator:v1\n";
        private const int PeMachineX86 = 0x014c;
        private const int PeMachineX64 = 0x8664;

        /* ---------------- payload 定位 ---------------- */

        private static string UnityPayloadFile(string leaf)
        {
            return PathUtil.Join(PathUtil.Join(Launcher.Root, "payloads\\UnityTranslator"), leaf);
        }

        /* find_unity_payload_file：存在（文件或目录）即返回路径，否则 null。 */
        private static string FindUnityPayloadFile(string leaf)
        {
            string p = UnityPayloadFile(leaf);
            return SafeFs.Exists(p) ? p : null;
        }

        internal static string FindUnityTemplate() { return FindUnityPayloadFile("UnityTranslator.dll"); }
        private static string FindUnityBepInEx6Template() { return FindUnityPayloadFile("UnityTranslator.BepInEx6.dll"); }

        private static bool IsBundledUnityMonoPlugin(string path)
        {
            string src = FindUnityTemplate();
            if (src != null && SafeFs.FilesEqual(path, src)) return true;
            src = FindUnityBepInEx6Template();
            if (src != null && SafeFs.FilesEqual(path, src)) return true;
            return false;
        }

        /* disable_existing_file：改名为 .disabled（保留，不删除）。 */
        private static bool DisableExistingFile(string path)
        {
            if (!SafeFs.Exists(path)) return false;
            string disabled = path + ".disabled";
            if (SafeFs.Exists(disabled) && !SafeFs.DeleteFileSafe(disabled)) return false;
            return SafeFs.MoveFileSafe(path, disabled, SafeFs.MOVEFILE_REPLACE_EXISTING | SafeFs.MOVEFILE_COPY_ALLOWED);
        }

        /* find_il2cpp_payload：payloads\UnityIL2CPP\<leaf> 必须是目录。 */
        private static string FindIl2cppPayload(string leaf)
        {
            string p = PathUtil.Join(PathUtil.Join(Launcher.Root, "payloads\\UnityIL2CPP"), leaf);
            return SafeFs.IsDir(p) ? p : null;
        }

        private static bool CopyPayloadFile(string payloadRoot, string rel, string gameDir)
        {
            string src = PathUtil.Join(payloadRoot, rel);
            string dst = PathUtil.Join(gameDir, rel);
            if (!SafeFs.Exists(src)) return false;
            return SafeFs.CopyFileSafe(src, dst);
        }

        private static bool CopyPayloadTree(string payloadRoot, string rel, string gameDir)
        {
            string src = PathUtil.Join(payloadRoot, rel);
            string dst = PathUtil.Join(gameDir, rel);
            if (!SafeFs.IsDir(src)) return false;
            return SafeFs.CopyTreeSafe(src, dst);
        }

        /* ---------------- PE / Unity 版本探测 ---------------- */

        /* pe_machine：读取 IMAGE_FILE_HEADER.Machine；pe 偏移来自文件本身，做边界检查。 */
        private static int PeMachine(string path)
        {
            byte[] buf = SafeFs.ReadBytes(path);
            if (buf == null) return 0;
            int size = buf.Length;
            int machine = 0;
            if (size >= 0x40 && buf[0] == (byte)'M' && buf[1] == (byte)'Z') {
                uint pe = BitConverter.ToUInt32(buf, 0x3c);
                if (pe < (uint)size && (uint)size - pe > 6
                    && buf[pe] == (byte)'P' && buf[pe + 1] == (byte)'E' && buf[pe + 2] == 0 && buf[pe + 3] == 0) {
                    machine = BitConverter.ToUInt16(buf, (int)pe + 4);
                }
            }
            return machine;
        }

        private static int UnityPlayerMachine(string dir)
        {
            int machine = PeMachine(PathUtil.Join(dir, "UnityPlayer.dll"));
            if (machine != 0) return machine;
            string exe = EngineDetector.FindExe(dir);
            if (exe != null) return PeMachine(exe);
            return 0;
        }

        /* detect_unity_major：扫描 *_Data\globalgamemanagers 前 4096 字节里形如 "2021." / "6000." 的版本前缀。 */
        private static int DetectUnityMajor(string dir)
        {
            var entries = Win32Find.EnumerateOrNull(dir, "*_Data");
            if (entries == null) return 0;
            int major = 0;
            foreach (var fd in entries) {
                if ((fd.Attributes & SafeFs.FILE_ATTRIBUTE_DIRECTORY) == 0) continue;
                string ggm = PathUtil.Join(PathUtil.Join(dir, fd.Name), "globalgamemanagers");
                byte[] buf = SafeFs.ReadBytes(ggm);
                if (buf != null) {
                    int lim = buf.Length < 4096 ? buf.Length : 4096;
                    for (int i = 0; i + 5 < lim; i++) {
                        if (buf[i] < (byte)'0' || buf[i] > (byte)'9') continue;
                        int v = 0, d = 0;
                        int j = i;
                        while (j < lim && buf[j] >= (byte)'0' && buf[j] <= (byte)'9' && d < 5) { v = v * 10 + (buf[j] - '0'); j++; d++; }
                        if (d >= 1 && j < lim && buf[j] == (byte)'.' && (v == 5 || (v >= 2017 && v <= 2023) || v >= 6000)) { major = v; break; }
                    }
                }
                if (major != 0) break;
            }
            return major;
        }

        private static bool UnityHasBepInEx6Mono(string dir)
        {
            return SafeFs.Exists(PathUtil.Join(dir, "BepInEx\\core\\BepInEx.Unity.Mono.dll"));
        }

        /* payload 缺失时只记录可复制执行的修复命令，不在部署路径中自动联网下载。 */
        private static void LogPayloadInstallCommand(string flag)
        {
            Log.Append("Install runtime payloads with:");
            Log.Append("  powershell -ExecutionPolicy Bypass -File scripts\\install_runtime_payloads.ps1 " + flag);
        }

        /* ---------------- BepInEx Mono 运行时 ---------------- */

        private static string FindBepInExMonoRuntimePayload(bool useBepInEx6, int machine, out string runtimeRel)
        {
            bool useX86 = machine == PeMachineX86;
            runtimeRel = useBepInEx6
                ? (useX86 ? "payloads\\UnityMonoRuntime6X86" : "payloads\\UnityMonoRuntime6")
                : (useX86 ? "payloads\\UnityMonoRuntimeX86" : "payloads\\UnityMonoRuntime");
            string monoRt = PathUtil.Join(Launcher.Root, runtimeRel);
            if (!SafeFs.IsDir(monoRt)) {
                Log.Append("Unity: missing BepInEx " + (useBepInEx6 ? 6 : 5) + " Mono " + (useX86 ? "x86" : "x64")
                           + " runtime payload (" + runtimeRel + ").");
                LogPayloadInstallCommand(useBepInEx6 ? "-UnityMono6" : "-UnityMono5");
                return null;
            }
            return monoRt;
        }

        private static bool InstallBepInExMonoRuntime(string dir, bool useBepInEx6, int machine)
        {
            string runtimeRel;
            bool useX86 = machine == PeMachineX86;
            string monoRt = FindBepInExMonoRuntimePayload(useBepInEx6, machine, out runtimeRel);
            if (monoRt == null) return false;

            bool ok = true;
            ok &= CopyPayloadFile(monoRt, "winhttp.dll", dir);
            ok &= CopyPayloadFile(monoRt, "doorstop_config.ini", dir);
            ok &= CopyPayloadFile(monoRt, ".doorstop_version", dir);
            ok &= CopyPayloadTree(monoRt, "BepInEx\\core", dir);
            if (!ok) {
                Log.Append("Unity: BepInEx " + (useBepInEx6 ? 6 : 5) + " Mono " + (useX86 ? "x86" : "x64")
                           + " runtime deployment is incomplete; check " + runtimeRel + ".");
                LogPayloadInstallCommand(useBepInEx6 ? "-UnityMono6 -Force" : "-UnityMono5 -Force");
                return false;
            }
            Log.Append("Unity: deployed BepInEx " + (useBepInEx6 ? 6 : 5) + " (Mono) " + (useX86 ? "x86" : "x64")
                       + " runtime" + (useBepInEx6 ? " for Unity 6+" : "") + ".");
            return true;
        }

        /* 现有模组环境可能已经包含 BepInEx 的 plugins/core，却缺少 Doorstop 根目录中的某个引导文件。
           保留所有现有文件，只补齐缺失的引导组件；仅当必需的入口程序集不完整时才复制 core 目录树。 */
        private static bool RepairExistingBepInExMonoRuntime(string dir, bool useBepInEx6, int machine)
        {
            string target = PathUtil.Join(dir, "winhttp.dll");
            int loaderMachine = PeMachine(target);
            bool needLoader = !SafeFs.Exists(target) || (machine != 0 && loaderMachine != 0 && loaderMachine != machine);

            bool needConfig = !SafeFs.Exists(PathUtil.Join(dir, "doorstop_config.ini"));
            bool needVersion = !SafeFs.Exists(PathUtil.Join(dir, ".doorstop_version"));

            string coreA = PathUtil.Join(dir, useBepInEx6 ? "BepInEx\\core\\BepInEx.Unity.Mono.dll" : "BepInEx\\core\\BepInEx.dll");
            string coreB = PathUtil.Join(dir, useBepInEx6 ? "BepInEx\\core\\BepInEx.Unity.Mono.Preloader.dll" : "BepInEx\\core\\BepInEx.Preloader.dll");
            bool needCore = !SafeFs.Exists(coreA) || !SafeFs.Exists(coreB);

            if (!needLoader && !needConfig && !needVersion && !needCore) return true;

            string runtimeRel;
            bool useX86 = machine == PeMachineX86;
            string monoRt = FindBepInExMonoRuntimePayload(useBepInEx6, machine, out runtimeRel);
            if (monoRt == null) return false;

            bool ok = true;
            int repaired = 0;
            if (needLoader) { ok &= CopyPayloadFile(monoRt, "winhttp.dll", dir); repaired++; }
            if (needConfig) { ok &= CopyPayloadFile(monoRt, "doorstop_config.ini", dir); repaired++; }
            if (needVersion) { ok &= CopyPayloadFile(monoRt, ".doorstop_version", dir); repaired++; }
            if (needCore) { ok &= CopyPayloadTree(monoRt, "BepInEx\\core", dir); repaired++; }

            if (!ok) {
                Log.Append("Unity: existing BepInEx " + (useBepInEx6 ? 6 : 5) + " Mono " + (useX86 ? "x86" : "x64")
                           + " runtime repair is incomplete; check " + runtimeRel + ".");
                LogPayloadInstallCommand(useBepInEx6 ? "-UnityMono6 -Force" : "-UnityMono5 -Force");
                return false;
            }
            if (repaired != 0) {
                Log.Append("Unity: repaired " + repaired + " missing BepInEx " + (useBepInEx6 ? 6 : 5)
                           + " Mono bootstrap component(s) without replacing existing user files.");
            }
            return true;
        }

        /* ---------------- 精简 mscorlib 探测与修复 ---------------- */

        private static bool BytesContainAscii(byte[] bytes, int size, string needle)
        {
            int n = needle == null ? 0 : needle.Length;
            if (bytes == null || n == 0 || n > size) return false;
            for (int i = 0; i <= size - n; i++) {
                int k = 0;
                while (k < n && bytes[i + k] == (byte)needle[k]) k++;
                if (k == n) return true;
            }
            return false;
        }

        private static string FindUnityMscorlib(string dir)
        {
            var entries = Win32Find.EnumerateOrNull(dir, "*_Data");
            if (entries == null) return null;
            foreach (var fd in entries) {
                if ((fd.Attributes & SafeFs.FILE_ATTRIBUTE_DIRECTORY) == 0) continue;
                string candidate = PathUtil.Join(PathUtil.Join(dir, fd.Name), "Managed\\mscorlib.dll");
                if (SafeFs.Exists(candidate)) return candidate;
            }
            return null;
        }

        private static bool MscorlibHasBepInExFileWriter(string path)
        {
            byte[] bytes = SafeFs.ReadBytes(path);
            if (bytes == null) return false;
            return bytes.Length >= 2 && bytes[0] == (byte)'M' && bytes[1] == (byte)'Z'
                   && BytesContainAscii(bytes, bytes.Length, "WriteAllText");
        }

        private static bool UnityMscorlibIsClearlyStripped(string dir)
        {
            string mscorlib = FindUnityMscorlib(dir);
            if (mscorlib == null) return false;
            byte[] bytes = SafeFs.ReadBytes(mscorlib);
            bool stripped = false;
            if (bytes != null) {
                stripped = bytes.Length >= 2 && bytes[0] == (byte)'M' && bytes[1] == (byte)'Z'
                           && BytesContainAscii(bytes, bytes.Length, "mscorlib")
                           && BytesContainAscii(bytes, bytes.Length, "System.IO")
                           && !BytesContainAscii(bytes, bytes.Length, "WriteAllText");
            }
            if (stripped) {
                Log.Append("Unity Mono: detected a clearly stripped mscorlib without System.IO.File.WriteAllText: " + mscorlib);
            }
            return stripped;
        }

        private static bool AsciiSpanEqualsIgnoreCase(byte[] data, int start, int len, string text)
        {
            if (len != text.Length) return false;
            for (int i = 0; i < len; i++) {
                if (AsciiLower(data[start + i]) != AsciiLower((byte)text[i])) return false;
            }
            return true;
        }

        private static byte AsciiLower(byte b)
        {
            return b >= (byte)'A' && b <= (byte)'Z' ? (byte)(b + 32) : b;
        }

        private static bool IsSpaceOrTab(byte b) { return b == (byte)' ' || b == (byte)'\t'; }
        private static bool IsCrLf(byte b) { return b == (byte)'\r' || b == (byte)'\n'; }

        /* find_active_ini_setting：只在目标 [section] 内匹配 key（大小写不敏感），跳过注释行；
           sectionBody 为目标段头之后第一行的起点（找到段头时设置，即使未找到键）。 */
        private static bool FindActiveIniSetting(byte[] data, int size, string section, string key,
                                                 out int lineStart, out int lineEnd,
                                                 out int valueStart, out int valueEnd,
                                                 ref int sectionBody)
        {
            lineStart = lineEnd = valueStart = valueEnd = 0;
            bool inTargetSection = false;
            int pos = 0;
            while (pos < size) {
                int ls = pos;
                while (pos < size && !IsCrLf(data[pos])) pos++;
                int le = pos;
                while (pos < size && IsCrLf(data[pos])) pos++;

                int start = ls;
                while (start < le && IsSpaceOrTab(data[start])) start++;
                if (start >= le || data[start] == (byte)'#' || data[start] == (byte)';') continue;
                if (data[start] == (byte)'[') {
                    int nameStart = start + 1;
                    int close = nameStart;
                    while (close < le && data[close] != (byte)']') close++;
                    int nameEnd = close;
                    while (nameEnd > nameStart && IsSpaceOrTab(data[nameEnd - 1])) nameEnd--;
                    while (nameStart < nameEnd && IsSpaceOrTab(data[nameStart])) nameStart++;
                    inTargetSection = close < le && AsciiSpanEqualsIgnoreCase(data, nameStart, nameEnd - nameStart, section);
                    if (inTargetSection) sectionBody = pos;
                    continue;
                }
                if (!inTargetSection) continue;
                int equals = start;
                while (equals < le && data[equals] != (byte)'=') equals++;
                if (equals >= le) continue;
                int keyEnd = equals;
                while (keyEnd > start && IsSpaceOrTab(data[keyEnd - 1])) keyEnd--;
                if (!AsciiSpanEqualsIgnoreCase(data, start, keyEnd - start, key)) continue;

                int vs = equals + 1;
                while (vs < le && IsSpaceOrTab(data[vs])) vs++;
                int ve = le;
                while (ve > vs && IsSpaceOrTab(data[ve - 1])) ve--;
                lineStart = ls; lineEnd = le; valueStart = vs; valueEnd = ve;
                return true;
            }
            return false;
        }

        private static bool BytesEqual(byte[] a, int aStart, byte[] b, int bStart, int len)
        {
            for (int i = 0; i < len; i++) if (a[aStart + i] != b[bStart + i]) return false;
            return true;
        }

        private static byte[] Ascii(string s) { return Encoding.ASCII.GetBytes(s); }

        /*
         * XUnity 首次启动时会向 INI 写入提供方默认值和端点生成的设置。因此，即使用户没有改动待迁移设置，
         * 实时文件也可能与启动器的所有权快照不同。只有实时值仍与上一份所有权快照完全一致时，才更新受管值。
         * 用户改过的值因而会以失败关闭方式由 WriteXunityConfig 保留。两个文件都以原子方式更新；若所有权快照
         * 写入失败，则回滚实时配置。
         */
        private static bool MigrateXunityOwnedIniSetting(string cfg, string owned, string section, string key, string value)
        {
            byte[] cfgData = SafeFs.ReadBytes(cfg);
            byte[] ownedData = cfgData == null ? null : SafeFs.ReadBytes(owned);
            if (cfgData == null || ownedData == null) {
                Log.Append("Unity IL2CPP: could not inspect the owned XUnity config migration (Windows error " + SafeFs.LastError + ").");
                return false;
            }

            int ls, le, cfgVs, cfgVe, ownedVs, ownedVe;
            int unusedBody = 0;
            bool cfgFound = FindActiveIniSetting(cfgData, cfgData.Length, section, key, out ls, out le, out cfgVs, out cfgVe, ref unusedBody);
            bool ownedFound = FindActiveIniSetting(ownedData, ownedData.Length, section, key, out ls, out le, out ownedVs, out ownedVe, ref unusedBody);
            int cfgValueLen = cfgVe - cfgVs;
            int ownedValueLen = ownedVe - ownedVs;
            if (!cfgFound || !ownedFound || cfgValueLen != ownedValueLen
                || !BytesEqual(cfgData, cfgVs, ownedData, ownedVs, cfgValueLen)) {
                return false;
            }

            byte[] newValue = Ascii(value);
            if (cfgValueLen == newValue.Length && BytesEqual(cfgData, cfgVs, newValue, 0, newValue.Length)) return true;

            var cfgOut = new ByteBuf(cfgData.Length + newValue.Length);
            cfgOut.Add(cfgData, 0, cfgVs);
            cfgOut.Add(newValue, 0, newValue.Length);
            cfgOut.Add(cfgData, cfgVe, cfgData.Length - cfgVe);
            var ownedOut = new ByteBuf(ownedData.Length + newValue.Length);
            ownedOut.Add(ownedData, 0, ownedVs);
            ownedOut.Add(newValue, 0, newValue.Length);
            ownedOut.Add(ownedData, ownedVe, ownedData.Length - ownedVe);

            byte[] cfgBytes = cfgOut.ToArray();
            byte[] ownedBytes = ownedOut.ToArray();
            if (!WriteFileBytesAtomic(cfg, cfgBytes, cfgBytes.Length)) {
                Log.Append("Unity IL2CPP: could not migrate the owned XUnity config (Windows error " + SafeFs.LastError + ").");
                return false;
            }
            if (!WriteFileBytesAtomic(owned, ownedBytes, ownedBytes.Length)) {
                int error = SafeFs.LastError;
                if (!WriteFileBytesAtomic(cfg, cfgData, cfgData.Length)) {
                    Log.Append("Unity IL2CPP: could not roll back the XUnity config migration (Windows error " + SafeFs.LastError + ").");
                }
                Log.Append("Unity IL2CPP: could not update XUnity config ownership; restored the live config (Windows error " + error + ").");
                return false;
            }

            Log.Append("Unity IL2CPP: migrated " + key + " while preserving XUnity-added config sections.");
            return true;
        }

        /* 高度裁剪的 Unity Player 可能同时移除 BepInEx 所需的 corlib 写入器和 Unity 日志回调 API。
           下列配置修改可能让 Unity 消息不再写入 BepInEx 的辅助 LogOutput.log，但 Player.log 保持不变；
           修改失败时终止部署，不会虚报翻译器可用。 */
        private static bool UpdateStrippedIniSetting(string path, string section, string key, string value, string label)
        {
            bool hadOriginal = SafeFs.Exists(path);
            byte[] data = null;
            if (hadOriginal) {
                data = SafeFs.ReadBytes(path);
                if (data == null) {
                    Log.Append("Unity Mono: could not read " + label + " while applying stripped-runtime support (Windows error " + SafeFs.LastError + ").");
                    return false;
                }
            }
            int size = data == null ? 0 : data.Length;
            if (data == null) data = new byte[0];

            int lineStart, lineEnd, valueStart, valueEnd;
            int sectionBody = 0;
            bool found = FindActiveIniSetting(data, size, section, key, out lineStart, out lineEnd, out valueStart, out valueEnd, ref sectionBody);
            if (found && AsciiSpanEqualsIgnoreCase(data, valueStart, valueEnd - valueStart, value)) return true;

            string backup = path + ".dst-stripped-backup";
            string owned = path + ".dst-stripped-owned";

            if (SafeFs.Exists(owned) && (!hadOriginal || !SafeFs.FilesEqual(path, owned))) {
                Log.Append("Unity Mono: preserved user-modified " + label + "; stripped-runtime setting was not overwritten.");
                return false;
            }
            if (!SafeFs.Exists(owned) && SafeFs.Exists(backup)) {
                Log.Append("Unity Mono: preserved " + label + " because an unowned stripped-runtime backup already exists.");
                return false;
            }

            byte[] keyBytes = Ascii(key), valueBytes = Ascii(value), sectionBytes = Ascii(section);
            byte[] eq = Ascii(" = "), crlf = Ascii("\r\n");
            var out_ = new ByteBuf(size + 64 + keyBytes.Length + valueBytes.Length + sectionBytes.Length);
            if (found) {
                out_.Add(data, 0, lineStart);
                out_.Add(keyBytes, 0, keyBytes.Length);
                out_.Add(eq, 0, 3);
                out_.Add(valueBytes, 0, valueBytes.Length);
                out_.Add(data, lineEnd, size - lineEnd);
            } else if (sectionBody != 0) {
                /* 目标 section 已存在但缺少该键：插到段首，避免在文件尾追加重复 section */
                out_.Add(data, 0, sectionBody);
                if (data[sectionBody - 1] != (byte)'\n') out_.Add(crlf, 0, 2);
                out_.Add(keyBytes, 0, keyBytes.Length);
                out_.Add(eq, 0, 3);
                out_.Add(valueBytes, 0, valueBytes.Length);
                out_.Add(crlf, 0, 2);
                out_.Add(data, sectionBody, size - sectionBody);
            } else {
                if (size != 0) out_.Add(data, 0, size);
                if (size != 0 && data[size - 1] != (byte)'\n' && data[size - 1] != (byte)'\r') out_.Add(crlf, 0, 2);
                if (size != 0) out_.Add(crlf, 0, 2);
                out_.Add(Ascii("["), 0, 1);
                out_.Add(sectionBytes, 0, sectionBytes.Length);
                out_.Add(Ascii("]\r\n"), 0, 3);
                out_.Add(keyBytes, 0, keyBytes.Length);
                out_.Add(eq, 0, 3);
                out_.Add(valueBytes, 0, valueBytes.Length);
                out_.Add(crlf, 0, 2);
            }
            byte[] result = out_.ToArray();

            if (hadOriginal && !BackupFileOnce(path, ".dst-stripped-backup")) {
                Log.Append("Unity Mono: could not back up " + label + " before applying stripped-runtime support (Windows error " + SafeFs.LastError + ").");
                return false;
            }
            int slash = path.LastIndexOf('\\');
            if (slash >= 0) {
                if (!SafeFs.EnsureDir(path.Substring(0, slash))) {
                    Log.Append("Unity Mono: could not create the directory for " + label + " (Windows error " + SafeFs.LastError + ").");
                    return false;
                }
            }
            if (!WriteFileBytesAtomic(path, result, result.Length)) {
                Log.Append("Unity Mono: could not update " + label + " (Windows error " + SafeFs.LastError + ").");
                return false;
            }

            if (!SafeFs.CopyFileSafe(path, owned)) {
                int error = SafeFs.LastError;
                /* 回滚到本次调用前的内容，而不是首次部署前备份的用户原始文件 */
                if (hadOriginal) {
                    if (!WriteFileBytesAtomic(path, data, size)) {
                        Log.Append("Unity Mono: could not roll back " + label + " (Windows error " + SafeFs.LastError + ").");
                    }
                } else {
                    SafeFs.DeleteFileSafe(path);
                }
                Log.Append("Unity Mono: could not record ownership for " + label + "; restored its prior state (Windows error " + error + ").");
                return false;
            }
            Log.Append("Unity Mono: updated " + label + " for the detected stripped runtime.");
            return true;
        }

        private static bool InstallStrippedUnityCorlib(string dir)
        {
            string payload = PathUtil.Join(Launcher.Root, "payloads\\UnityMonoCorlib");
            string payloadMscorlib = PathUtil.Join(payload, "mscorlib.dll");
            string payloadMarker = PathUtil.Join(payload, ".dst-installed-by-ds");
            if (!SafeFs.IsDir(payload) || !SafeFs.Exists(payloadMarker) || !MscorlibHasBepInExFileWriter(payloadMscorlib)) {
                Log.Append("Unity Mono: official complete corlib payload is missing or invalid.");
                LogPayloadInstallCommand("-UnityMonoCorlib");
                return false;
            }

            string target = PathUtil.Join(dir, "BepInEx\\unstripped_corlib");
            string targetMscorlib = PathUtil.Join(target, "mscorlib.dll");
            string targetMarker = PathUtil.Join(target, ".dst-installed-by-ds");
            if (SafeFs.IsDir(target) && !SafeFs.Exists(targetMarker)) {
                if (MscorlibHasBepInExFileWriter(targetMscorlib)) {
                    Log.Append("Unity Mono: using an existing unstripped_corlib directory without replacing user files.");
                    return true;
                }
                Log.Append("Unity Mono: existing unstripped_corlib is incomplete and has no launcher ownership marker; preserved it.");
                return false;
            }
            if (SafeFs.Exists(targetMarker) && !SafeFs.FilesEqual(targetMarker, payloadMarker)) {
                Log.Append("Unity Mono: existing unstripped_corlib ownership marker differs from the installed payload; preserved it.");
                return false;
            }
            if (!SafeFs.CopyTreeSafe(payload, target) || !MscorlibHasBepInExFileWriter(targetMscorlib)) {
                Log.Append("Unity Mono: failed to deploy the official complete corlib payload (Windows error " + SafeFs.LastError + ").");
                return false;
            }
            Log.Append("Unity Mono: deployed official Mono corlib support for the detected stripped runtime.");
            return true;
        }

        /* 某些受保护的 Unity Mono 构建只移除了 Font.Internal_CreateDynamicFont 的托管声明，却保留原生内部调用。
           BepInEx 5 预加载补丁器是恢复该声明的最早本地边界。所有权以精确字节快照记录；冲突时保留用户文件并失败。 */
        private static bool InstallStrippedUnityFontPatcher(string dir)
        {
            string payload = FindUnityPayloadFile("DeepSeekUnityFontPatcher.dll");
            if (payload == null) {
                Log.Append("Unity Mono: stripped-runtime font metadata patcher payload is missing.");
                return false;
            }
            string target = PathUtil.Join(dir, "BepInEx\\patchers\\DeepSeekUnityFontPatcher.dll");
            string owned = PathUtil.Join(dir, "BepInEx\\patchers\\DeepSeekUnityFontPatcher.dll.dst-owned");

            bool hadTarget = SafeFs.Exists(target);
            bool hadOwned = SafeFs.Exists(owned);
            if (hadOwned && hadTarget && !SafeFs.FilesEqual(target, owned)) {
                Log.Append("Unity Mono: preserved user-modified DeepSeekUnityFontPatcher.dll; its ownership snapshot no longer matches.");
                return false;
            }
            if (!hadOwned && hadTarget) {
                if (SafeFs.FilesEqual(target, payload)) {
                    Log.Append("Unity Mono: using an existing unowned DeepSeekUnityFontPatcher.dll without claiming ownership.");
                    return true;
                }
                Log.Append("Unity Mono: preserved an existing unowned DeepSeekUnityFontPatcher.dll; stripped-runtime patcher was not overwritten.");
                return false;
            }

            if (!SafeFs.CopyFileSafe(payload, target)) {
                Log.Append("Unity Mono: could not deploy DeepSeekUnityFontPatcher.dll (Windows error " + SafeFs.LastError + ").");
                return false;
            }
            if (!SafeFs.CopyFileSafe(target, owned)) {
                int error = SafeFs.LastError;
                if (hadOwned) {
                    if (hadTarget) SafeFs.CopyFileSafe(owned, target);
                    else SafeFs.DeleteFileSafe(target);
                } else {
                    SafeFs.DeleteFileSafe(target);
                }
                Log.Append("Unity Mono: could not record font patcher ownership; restored its prior state (Windows error " + error + ").");
                return false;
            }
            Log.Append("Unity Mono: deployed the BepInEx 5 font metadata patcher for the detected stripped runtime.");
            return true;
        }

        private static bool EnsureStrippedUnityMonoSupport(string dir, bool stripped)
        {
            if (!stripped) return true;
            if (!InstallStrippedUnityCorlib(dir)) return false;

            string doorstop = PathUtil.Join(dir, "doorstop_config.ini");
            string bepCfg = PathUtil.Join(dir, "BepInEx\\config\\BepInEx.cfg");
            if (!UpdateStrippedIniSetting(doorstop, "UnityMono", "dll_search_path_override",
                                          "BepInEx\\unstripped_corlib;BepInEx\\core", "doorstop_config.ini")) return false;
            if (!UpdateStrippedIniSetting(bepCfg, "Logging", "UnityLogListening", "false", "BepInEx.cfg")) return false;
            return true;
        }

        /* ensure_bepinex_mono：Unity 6+ 需要 BepInEx 6 Unity.Mono；旧版保持 BepInEx 5；用户自带 BepInEx 只修补不覆盖。 */
        private static bool EnsureBepInExMono(string dir)
        {
            int major = DetectUnityMajor(dir);
            bool useBepInEx6 = major >= 6000;
            int machine = UnityPlayerMachine(dir);
            if (machine != 0 && machine != PeMachineX86 && machine != PeMachineX64) {
                Log.Append("Unity Mono: unsupported player PE machine 0x" + machine.ToString("X4") + "; only x86 and x64 runtimes are available.");
                return false;
            }
            string bep = PathUtil.Join(dir, "BepInEx");
            if (SafeFs.IsDir(bep)) {
                bool hasBepInEx6 = UnityHasBepInEx6Mono(dir);
                if (useBepInEx6 && !hasBepInEx6) {
                    Log.Append("Unity " + major + " (Unity 6+): existing BepInEx is not Unity.Mono 6; updating runtime files.");
                    return InstallBepInExMonoRuntime(dir, true, machine);
                }
                return RepairExistingBepInExMonoRuntime(dir, useBepInEx6 || hasBepInEx6, machine);
            }
            return InstallBepInExMonoRuntime(dir, useBepInEx6, machine);
        }

        /* ---------------- deploy_unity（Mono） ---------------- */

        public static bool Unity(string dir)
        {
            string plugins = PathUtil.Join(dir, "BepInEx\\plugins");

            if (!EnsureBepInExMono(dir)) return false;
            bool stripped = UnityMscorlibIsClearlyStripped(dir);
            if (!EnsureStrippedUnityMonoSupport(dir, stripped)) return false;
            bool useBepInEx6 = UnityHasBepInEx6Mono(dir);
            if (stripped && !useBepInEx6 && !InstallStrippedUnityFontPatcher(dir)) return false;
            string src = useBepInEx6 ? FindUnityBepInEx6Template() : FindUnityTemplate();
            if (src == null) {
                Log.Append(useBepInEx6
                    ? "Unity: missing UnityTranslator.BepInEx6.dll template."
                    : "Unity: missing UnityTranslator.dll template.");
                return false;
            }
            string dll = PathUtil.Join(plugins, "UnityTranslator.dll");
            if (SafeFs.Exists(dll) && !IsBundledUnityMonoPlugin(dll) && !BackupFileOnce(dll, ".dst-backup")) {
                Log.Append("Unity: could not back up the existing UnityTranslator.dll before overwriting it (Windows error " + SafeFs.LastError + ").");
                return false;
            }
            if (!SafeFs.CopyFileSafe(src, dll)) {
                Log.Append("Unity：无法部署 UnityTranslator.dll（Windows 错误 " + SafeFs.LastError + "）。");
                return false;
            }
            string jsonSrc = FindUnityPayloadFile("Newtonsoft.Json.dll");
            if (jsonSrc == null) {
                Log.Append("Unity: missing Newtonsoft.Json.dll dependency; UnityTranslator cannot start.");
                LogPayloadInstallCommand("-Newtonsoft");
                return false;
            }
            string jsonDst = PathUtil.Join(plugins, "Newtonsoft.Json.dll");
            if (SafeFs.Exists(jsonDst) && !SafeFs.FilesEqual(jsonDst, jsonSrc) && !BackupFileOnce(jsonDst, ".dst-backup")) {
                Log.Append("Unity: could not back up the existing Newtonsoft.Json.dll before overwriting it (Windows error " + SafeFs.LastError + ").");
                return false;
            }
            if (!SafeFs.CopyFileSafe(jsonSrc, jsonDst)) {
                Log.Append("Unity：无法部署 Newtonsoft.Json.dll（Windows 错误 " + SafeFs.LastError + "）。");
                return false;
            }
            string fontfix = FindIl2cppPayload("TMPFontAssetBundles");
            if (fontfix != null) {
                if (!CopyPayloadTree(fontfix, "BepInEx\\font", dir)) {
                    Log.Append("Unity：无法部署 TMP 字体资源目录（Windows 错误 " + SafeFs.LastError + "）。");
                    return false;
                }
            } else {
                Log.Append("Unity Mono: TMP font asset bundle payload missing; Chinese TMP glyphs may use overlay fallback.");
            }
            Log.Append((useBepInEx6
                ? "Unity: deployed BepInEx 6 compatible Unity plugin: "
                : "Unity: deployed BepInEx 5 compatible Unity plugin: ") + dll);
            return true;
        }

        /* ---------------- write_xunity_config ---------------- */

        private const string XunityConfigText =
            "[Service]\n" +
            "Endpoint=DeepSeekTranslate\n" +
            "FallbackEndpoint=\n" +
            "\n" +
            "[General]\n" +
            "Language=zh-CN\n" +
            "FromLanguage=auto\n" +
            "\n" +
            "[Files]\n" +
            "Directory=Translation\\{Lang}\\Text\n" +
            "OutputFile=Translation\\{Lang}\\Text\\_AutoGeneratedTranslations.txt\n" +
            "SubstitutionFile=Translation\\{Lang}\\Text\\_Substitutions.txt\n" +
            "PreprocessorsFile=Translation\\{Lang}\\Text\\_Preprocessors.txt\n" +
            "PostprocessorsFile=Translation\\{Lang}\\Text\\_Postprocessors.txt\n" +
            "\n" +
            "[TextFrameworks]\n" +
            "EnableIMGUI=False\n" +
            "EnableUGUI=True\n" +
            "EnableUIElements=True\n" +
            "EnableNGUI=True\n" +
            "EnableTextMeshPro=True\n" +
            "EnableTextMesh=False\n" +
            "EnableFairyGUI=True\n" +
            "\n" +
            "[Behaviour]\n" +
            "MaxCharactersPerTranslation=2500\n" +
            "IgnoreWhitespaceInDialogue=True\n" +
            "MinDialogueChars=20\n" +
            "ForceSplitTextAfterCharacters=0\n" +
            "CopyToClipboard=False\n" +
            "MaxClipboardCopyCharacters=2500\n" +
            "ClipboardDebounceTime=1.25\n" +
            "EnableUIResizing=False\n" +
            "EnableBatching=True\n" +
            "UseStaticTranslations=True\n" +
            "OverrideFont=\n" +
            "OverrideFontSize=\n" +
            "OverrideFontTextMeshPro=\n" +
            "FallbackFontTextMeshPro=\n" +
            "ResizeUILineSpacingScale=\n" +
            "ForceUIResizing=False\n" +
            "IgnoreTextStartingWith=\\u180e;Confidence increased;Confidence decreased;Confidence lowered;Confidence reduced;Confidence changed;\n" +
            "TextGetterCompatibilityMode=False\n" +
            "GameLogTextPaths=\n" +
            "RomajiPostProcessing=ReplaceMacronWithCircumflex;RemoveApostrophes;ReplaceHtmlEntities\n" +
            "TranslationPostProcessing=ReplaceMacronWithCircumflex;ReplaceHtmlEntities\n" +
            "RegexPostProcessing=\n" +
            "CacheRegexPatternResults=False\n" +
            "PersistRichTextMode=Final\n" +
            "CacheRegexLookups=False\n" +
            "CacheWhitespaceDifferences=False\n" +
            "GenerateStaticSubstitutionTranslations=False\n" +
            "GeneratePartialTranslations=False\n" +
            "EnableTranslationScoping=True\n" +
            "EnableSilentMode=True\n" +
            "BlacklistedIMGUIPlugins=\n" +
            "EnableTextPathLogging=False\n" +
            "OutputUntranslatableText=False\n" +
            "IgnoreVirtualTextSetterCallingRules=False\n" +
            "MaxTextParserRecursion=1\n" +
            "HtmlEntityPreprocessing=True\n" +
            "HandleRichText=True\n" +
            "EnableTranslationHelper=False\n" +
            "ForceMonoModHooks=False\n" +
            "InitializeHarmonyDetourBridge=False\n" +
            "RedirectedResourceDetectionStrategy=AppendMongolianVowelSeparatorAndRemoveAll\n" +
            "OutputTooLongText=False\n" +
            "TemplateAllNumberAway=True\n" +
            "ReloadTranslationsOnFileChange=True\n" +
            "DisableTextMeshProScrollInEffects=False\n" +
            "CacheParsedTranslations=False\n" +
            "\n" +
            "[Texture]\n" +
            "TextureDirectory=Translation\\{Lang}\\Texture\n" +
            "EnableTextureTranslation=False\n" +
            "EnableTextureDumping=False\n" +
            "EnableTextureToggling=False\n" +
            "EnableTextureScanOnSceneLoad=False\n" +
            "EnableSpriteRendererHooking=False\n" +
            "LoadUnmodifiedTextures=False\n" +
            "DetectDuplicateTextureNames=False\n" +
            "DuplicateTextureNames=\n" +
            "EnableLegacyTextureLoading=False\n" +
            "TextureHashGenerationStrategy=FromImageName\n" +
            "CacheTexturesInMemory=True\n" +
            "EnableSpriteHooking=False\n" +
            "\n" +
            "[ResourceRedirector]\n" +
            "PreferredStoragePath=Translation\\{Lang}\\RedirectedResources\n" +
            "EnableTextAssetRedirector=False\n" +
            "LogAllLoadedResources=False\n" +
            "EnableDumping=False\n" +
            "CacheMetadataForAllFiles=True\n" +
            "\n" +
            "[Http]\n" +
            "UserAgent=\n" +
            "DisableCertificateValidation=True\n" +
            "\n" +
            "[TranslationAggregator]\n" +
            "Width=400\n" +
            "Height=100\n" +
            "EnabledTranslators=\n" +
            "\n" +
            "[Debug]\n" +
            "EnableConsole=False\n" +
            "\n" +
            "[Migrations]\n" +
            "Enable=True\n" +
            "Tag=5.6.1\n" +
            "\n" +
            "[Custom]\n" +
            "Url=http://127.0.0.1:19999/translate\n" +
            "EnableShortDelay=False\n" +
            "DisableSpamChecks=False\n" +
            "\n" +
            "[DeepSeek]\n" +
            "Url=http://127.0.0.1:19999\n" +
            "MaxBatchSize=16\n" +
            "MaxConcurrency=8\n" +
            "QueueWaitSeconds=30\n" +
            "QueuePollIntervalSeconds=0.2\n" +
            "TranslationDelay=0.1\n" +
            "DisplaySafePunctuation=True\n";

        private static bool WriteXunityConfig(string dir)
        {
            string cfgdir = PathUtil.Join(dir, "BepInEx\\config");
            if (!SafeFs.EnsureDir(cfgdir)) return false;
            string cfg = PathUtil.Join(cfgdir, "AutoTranslatorConfig.ini");
            string owned = cfg + ".dst-owned";

            if (SafeFs.Exists(owned) && SafeFs.Exists(cfg) && !SafeFs.FilesEqual(cfg, owned)) {
                if (MigrateXunityOwnedIniSetting(cfg, owned, "Behaviour", "MaxCharactersPerTranslation", "2500")) return true;
                Log.Append("Unity IL2CPP: preserved user-modified AutoTranslatorConfig.ini; deploy it again after reviewing the file.");
                return false;
            }
            if (!SafeFs.Exists(owned) && SafeFs.Exists(cfg) && !BackupFileOnce(cfg, ".dst-backup")) {
                Log.Append("Unity IL2CPP: could not back up AutoTranslatorConfig.ini. Windows error: " + SafeFs.LastError);
                return false;
            }

            /* 快照本次覆盖前的内容；ownership 记录失败时优先回滚到它，而不是首次部署前备份的用户原始文件。 */
            byte[] previous = null;
            if (SafeFs.Exists(cfg)) {
                previous = SafeFs.ReadBytes(cfg);
                if (previous == null) {
                    Log.Append("Unity IL2CPP: could not read the existing AutoTranslatorConfig.ini before updating it. Windows error: " + SafeFs.LastError);
                    return false;
                }
            }

            if (!SafeFs.WriteTextUtf8(cfg, XunityConfigText)) {
                Log.Append("Unity IL2CPP: could not write AutoTranslatorConfig.ini. Windows error: " + SafeFs.LastError);
                return false;
            }
            if (!SafeFs.CopyFileSafe(cfg, owned)) {
                int error = SafeFs.LastError;
                if (previous != null) {
                    if (!WriteFileBytesAtomic(cfg, previous, previous.Length)) {
                        Log.Append("Unity IL2CPP: could not roll back AutoTranslatorConfig.ini. Windows error: " + SafeFs.LastError);
                    }
                } else {
                    SafeFs.DeleteFileSafe(cfg);
                }
                Log.Append("Unity IL2CPP: could not record config ownership; restored the previous config. Windows error: " + error);
                return false;
            }
            return true;
        }

        /* ---------------- deploy_unity_il2cpp ---------------- */

        public static bool UnityIl2cpp(string dir)
        {
            string dll = PathUtil.Join(dir, "BepInEx\\plugins\\UnityTranslator.dll");
            string pdb = PathUtil.Join(dir, "BepInEx\\plugins\\UnityTranslator.pdb");
            string il2cppMscorlib = PathUtil.Join(dir, "BepInEx\\core\\Il2Cppmscorlib.dll");
            string gameasm = PathUtil.Join(dir, "GameAssembly.dll");

            /* 只支持 x64 IL2CPP 构建 */
            int machine = PeMachine(gameasm);
            if (machine == 0) {
                Log.Append("Unity IL2CPP：无法确认 GameAssembly.dll 的架构，按 x64 处理：" + gameasm);
            }
            if (machine != 0 && machine != PeMachineX64) {
                Log.Append("Unity IL2CPP：当前只内置 x64 插件运行时，已跳过非 x64 游戏。");
                return false;
            }

            /* 如果存在旧的 Mono 版插件，禁用它避免冲突 */
            if (SafeFs.Exists(dll)) {
                if (!IsBundledUnityMonoPlugin(dll)) {
                    Log.Append("Unity IL2CPP：保留现有 UnityTranslator.dll（不是内置 Mono 模板）。");
                } else if (DisableExistingFile(dll)) {
                    Log.Append("Unity IL2CPP：已禁用旧的 Mono UnityTranslator.dll：" + dll + ".disabled");
                    DisableExistingFile(pdb);
                } else {
                    Log.Append("Unity IL2CPP：警告：无法禁用内置 Mono UnityTranslator.dll（Windows 错误 " + SafeFs.LastError
                               + "），它可能与 IL2CPP 插件冲突：" + dll);
                }
            }

            string runtime = FindIl2cppPayload("BepInExRuntime");
            if (runtime == null) {
                Log.Append("Unity IL2CPP：找不到 BepInEx IL2CPP payload。");
                LogPayloadInstallCommand("-UnityIL2CPP");
                return false;
            }
            string xunity = FindIl2cppPayload("XUnityAutoTranslator");
            if (xunity == null) {
                Log.Append("Unity IL2CPP：找不到 XUnity AutoTranslator payload。");
                LogPayloadInstallCommand("-UnityIL2CPP");
                return false;
            }

            bool ok = true;
            ok &= CopyPayloadFile(runtime, "doorstop_config.ini", dir);
            ok &= CopyPayloadFile(runtime, "winhttp.dll", dir);
            ok &= CopyPayloadFile(runtime, ".doorstop_version", dir);
            ok &= CopyPayloadTree(runtime, "dotnet", dir);
            ok &= CopyPayloadTree(runtime, "BepInEx\\core", dir);
            ok &= CopyPayloadTree(runtime, "BepInEx\\patchers", dir);
            ok &= CopyPayloadFile(xunity, "BepInEx\\core\\XUnity.Common.dll", dir);
            string xunityPluginDir = PathUtil.Join(dir, "BepInEx\\plugins\\XUnity.AutoTranslator");
            string xunityOwnerMarker = PathUtil.Join(xunityPluginDir, ".dst-installed-by-ds");
            bool xunityPreexisting = SafeFs.IsDir(xunityPluginDir);
            if (!xunityPreexisting) {
                if (!SafeFs.EnsureDir(xunityPluginDir) || !SafeFs.WriteTextUtf8(xunityOwnerMarker, XunityOwnerMarkerText)) {
                    Log.Append("Unity IL2CPP: could not record ownership for the XUnity plugin directory.");
                    ok = false;
                }
            }
            if (ok || xunityPreexisting) {
                ok &= CopyPayloadTree(xunity, "BepInEx\\plugins\\XUnity.AutoTranslator", dir);
            }
            ok &= CopyPayloadTree(xunity, "BepInEx\\plugins\\XUnity.ResourceRedirector", dir);
            string endpointSrc = PathUtil.Join(Launcher.Root, "payloads\\UnityIL2CPP\\DeepSeekXUnityTranslator\\DeepSeekTranslate.dll");
            string endpointDst = PathUtil.Join(dir, "BepInEx\\plugins\\XUnity.AutoTranslator\\Translators\\DeepSeekTranslate.dll");
            if (SafeFs.Exists(endpointSrc)) {
                ok &= SafeFs.CopyFileSafe(endpointSrc, endpointDst);
            } else {
                Log.Append("Unity IL2CPP: missing DeepSeek XUnity endpoint payload (payloads\\UnityIL2CPP\\DeepSeekXUnityTranslator\\DeepSeekTranslate.dll).");
                Log.Append("Download the full program package or run build_native.bat before deploying Unity IL2CPP.");
                ok = false;
            }
            string fontfix = FindIl2cppPayload("TMPFontAssetBundles");
            if (fontfix != null) {
                ok &= CopyPayloadTree(fontfix, "BepInEx\\font", dir);
            } else {
                Log.Append("Unity IL2CPP: TMP font asset bundle payload missing; Chinese TMP glyphs may show as boxes.");
            }
            string fontplugin = FindIl2cppPayload("DeepSeekTMPFontFallback");
            if (fontplugin != null) {
                ok &= CopyPayloadTree(fontplugin, "BepInEx\\plugins\\DeepSeekTMPFontFallback", dir);
            } else {
                Log.Append("Unity IL2CPP: TMP font fallback plugin payload missing; Chinese TMP glyphs may show as boxes.");
            }

            if (!ok) {
                Log.Append("Unity IL2CPP：插件运行时部署不完整，请检查 payloads\\UnityIL2CPP。");
                LogPayloadInstallCommand("-UnityIL2CPP -Force");
                return false;
            }

            /* 禁用旧的 Il2Cppmscorlib.dll，避免遮挡新 interop 层 */
            if (SafeFs.Exists(il2cppMscorlib)) {
                if (DisableExistingFile(il2cppMscorlib)) {
                    Log.Append("Unity IL2CPP：已禁用旧的 core\\Il2Cppmscorlib.dll，避免遮挡新 interop。");
                } else {
                    Log.Append("Unity IL2CPP：警告：无法禁用旧的 core\\Il2Cppmscorlib.dll（Windows 错误 " + SafeFs.LastError
                               + "），它可能遮挡新 interop 层。");
                }
            }

            if (!WriteXunityConfig(dir)) return false;
            Log.Append("Unity IL2CPP: deployed TMP Chinese system font fallback.");
            Log.Append("Unity IL2CPP：已部署 BepInEx be.755 + XUnity AutoTranslator。");
            Log.Append("Unity IL2CPP：XUnity 已配置为使用本地 DeepSeek 批量端点 http://127.0.0.1:19999。");
            return true;
        }

        /* ======================== Unity 还原 ======================== */

        private static bool RestoreRemoveMatchingFile(string installed, string payload, string label, RestoreStats stats)
        {
            if (!SafeFs.Exists(installed)) return true;
            if (!SafeFs.Exists(payload) || !SafeFs.FilesEqual(installed, payload)) {
                stats.Preserved++;
                Log.Append("还原：" + label + " 与当前内置版本不一致，已保留：" + installed);
                return false;
            }
            return RestoreDeleteFile(installed, stats);
        }

        private static bool RestoreFileEqualsText(string path, string text)
        {
            byte[] bytes = SafeFs.ReadBytes(path);
            if (bytes == null) return false;
            byte[] expected = Ascii(text);
            return expected.Length == bytes.Length && BytesEqual(bytes, 0, expected, 0, expected.Length);
        }

        /* 只移除字节仍与对应载荷完全一致的文件。启动器标记只能证明初始所有权；用户或模组管理器编辑目录后，
           它无法证明每个后代文件仍归启动器所有。未知、新增和已修改的文件都会被保留并记录。 */
        private static void RestoreRemoveVerifiedPayloadFiles(string payloadDir, string installedDir, string label, RestoreStats stats)
        {
            if (SafeFs.PathHasReparsePoint(payloadDir, true) || SafeFs.PathHasReparsePoint(installedDir, true)) {
                stats.Failed++;
                Log.Append("Restore: refused to enumerate " + label + " through a reparse point: " + installedDir);
                return;
            }
            var entries = Win32Find.EnumerateOrNull(payloadDir, "*");
            if (entries == null) {
                int error = SafeFs.LastError;
                stats.Preserved++;
                Log.Append("还原：无法读取 " + label + " payload，已保留安装目录 " + installedDir + "（Windows 错误 " + error + "）。");
                return;
            }
            int enumError = SafeFs.LastError;

            foreach (var fd in entries) {
                string payloadChild = PathUtil.Join(payloadDir, fd.Name);
                string installedChild = PathUtil.Join(installedDir, fd.Name);
                if ((fd.Attributes & SafeFs.FILE_ATTRIBUTE_DIRECTORY) != 0) {
                    uint installedAttr = SafeFs.Attributes(installedChild);
                    if (installedAttr == SafeFs.INVALID_FILE_ATTRIBUTES) continue;
                    if ((installedAttr & SafeFs.FILE_ATTRIBUTE_DIRECTORY) == 0 || (installedAttr & SafeFs.FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
                        stats.Preserved++;
                        Log.Append("还原：" + label + " 子目录类型已变化，已保留：" + installedChild);
                        continue;
                    }
                    RestoreRemoveVerifiedPayloadFiles(payloadChild, installedChild, label, stats);
                } else {
                    RestoreRemoveMatchingFile(installedChild, payloadChild, label, stats);
                }
            }
            if (enumError != SafeFs.ERROR_NO_MORE_FILES) {
                stats.Failed++;
                Log.Append("还原：枚举 " + label + " payload 失败（Windows 错误 " + enumError + "）。");
            }
        }

        private static void RestorePruneEmptyDirs(string path, RestoreStats stats)
        {
            if (SafeFs.PathHasReparsePoint(path, true)) {
                stats.Failed++;
                Log.Append("Restore: refused to prune a directory through a reparse point: " + path);
                return;
            }
            uint attr = SafeFs.Attributes(path);
            if (attr == SafeFs.INVALID_FILE_ATTRIBUTES || (attr & SafeFs.FILE_ATTRIBUTE_DIRECTORY) == 0
                || (attr & SafeFs.FILE_ATTRIBUTE_REPARSE_POINT) != 0) return;

            var entries = Win32Find.EnumerateOrNull(path, "*");
            if (entries != null) {
                foreach (var fd in entries) {
                    if ((fd.Attributes & SafeFs.FILE_ATTRIBUTE_DIRECTORY) != 0) {
                        RestorePruneEmptyDirs(PathUtil.Join(path, fd.Name), stats);
                    }
                }
            }
            if (SafeFs.RemoveDirectory(path)) {
                stats.Removed++;
                Log.Append("还原：已移除空目录 " + path);
            } else {
                int error = SafeFs.LastError;
                if (error == SafeFs.ERROR_DIR_NOT_EMPTY) {
                    stats.Preserved++;
                    Log.Append("还原：目录含有非启动器文件，已保留：" + path);
                } else if (!PathMissingError(error)) {
                    stats.Failed++;
                    Log.Append("还原：无法移除空目录 " + path + "（Windows 错误 " + error + "）。");
                }
            }
        }

        private static bool RestoreRemoveOwnedTree(string tree, string installedMarker, string payloadTree, string payloadMarker,
                                                   string label, RestoreStats stats)
        {
            if (!SafeFs.Exists(tree)) return true;
            if (!SafeFs.Exists(installedMarker) || !SafeFs.Exists(payloadMarker) || !SafeFs.FilesEqual(installedMarker, payloadMarker)) {
                stats.Preserved++;
                Log.Append("还原：无法确认 " + label + " 目录归属，已保留：" + tree);
                return false;
            }
            if (!SafeFs.IsDir(payloadTree)) {
                stats.Preserved++;
                Log.Append("还原：缺少 " + label + " payload，无法逐文件验证，已保留：" + tree);
                return false;
            }

            RestoreRemoveVerifiedPayloadFiles(payloadTree, tree, label, stats);
            RestorePruneEmptyDirs(tree, stats);
            return stats.Failed == 0 && stats.Preserved == 0;
        }

        private static void RestoreXunityPlugin(string dir, RestoreStats stats)
        {
            string installed = PathUtil.Join(dir, "BepInEx\\plugins\\XUnity.AutoTranslator");
            if (!SafeFs.IsDir(installed)) return;
            string marker = PathUtil.Join(installed, ".dst-installed-by-ds");
            if (!RestoreFileEqualsText(marker, XunityOwnerMarkerText)) {
                stats.Preserved++;
                Log.Append("还原 Unity IL2CPP：XUnity 目录不是本版本首次安装，已保留以避免删除用户运行时。");
                return;
            }
            string payload = PathUtil.Join(Launcher.Root, "payloads\\UnityIL2CPP\\XUnityAutoTranslator\\BepInEx\\plugins\\XUnity.AutoTranslator");
            if (!SafeFs.IsDir(payload)) {
                stats.Preserved++;
                Log.Append("还原 Unity IL2CPP：缺少 XUnity payload，无法验证已安装文件，目录已保留。");
                return;
            }

            RestoreRemoveVerifiedPayloadFiles(payload, installed, "XUnity plugin file", stats);
            RestoreDeleteFile(marker, stats);
            RestorePruneEmptyDirs(installed, stats);
        }

        /* deploy_unity 覆盖用户已有的插件文件前会留下 .dst-backup 一次性备份。
           仅当当前文件仍与内置版本一致时才移除它并恢复备份；已被改动的文件和备份都保留。 */
        private static void RestoreUnityPluginWithBackup(string installed, bool matchesBundled, RestoreStats stats)
        {
            string backup = installed + ".dst-backup";
            if (SafeFs.Exists(installed)) {
                if (!matchesBundled) {
                    stats.Preserved++;
                    Log.Append("还原 Unity：" + installed + " 与当前内置版本不一致，已作为用户文件保留。");
                    return;
                }
                if (!RestoreDeleteFile(installed, stats)) return;
            }
            if (!SafeFs.Exists(backup)) return;
            if (SafeFs.MoveFileSafe(backup, installed, SafeFs.MOVEFILE_WRITE_THROUGH)) {
                stats.Restored++;
                Log.Append("还原 Unity：已从备份恢复 " + installed + "。");
            } else {
                stats.Failed++;
                Log.Append("还原 Unity：无法从备份恢复 " + installed + "（Windows 错误 " + SafeFs.LastError + "）。");
            }
        }

        private static void RestoreUnityMonoPlugin(string dir, RestoreStats stats)
        {
            string installed = PathUtil.Join(dir, "BepInEx\\plugins\\UnityTranslator.dll");
            RestoreUnityPluginWithBackup(installed, IsBundledUnityMonoPlugin(installed), stats);
        }

        /* Newtonsoft.Json.dll 是共享依赖：没有备份说明部署时未覆盖用户文件，保持原样；
           有备份说明部署时覆盖了用户自带版本，还原时把它换回来。 */
        private static void RestoreUnityMonoNewtonsoft(string dir, RestoreStats stats)
        {
            string installed = PathUtil.Join(dir, "BepInEx\\plugins\\Newtonsoft.Json.dll");
            string backup = installed + ".dst-backup";
            if (!SafeFs.Exists(backup)) return;
            if (SafeFs.Exists(installed)) {
                string payload = FindUnityPayloadFile("Newtonsoft.Json.dll");
                if (payload == null || !SafeFs.FilesEqual(installed, payload)) {
                    stats.Preserved++;
                    Log.Append("还原 Unity：Newtonsoft.Json.dll 在部署后已被改动，文件和备份均已保留。");
                    return;
                }
            }
            uint flags = SafeFs.MOVEFILE_WRITE_THROUGH | (SafeFs.Exists(installed) ? SafeFs.MOVEFILE_REPLACE_EXISTING : 0);
            if (SafeFs.MoveFileSafe(backup, installed, flags)) {
                stats.Restored++;
                Log.Append("还原 Unity：已从备份恢复原 Newtonsoft.Json.dll。");
            } else {
                stats.Failed++;
                Log.Append("还原 Unity：无法恢复 Newtonsoft.Json.dll 备份（Windows 错误 " + SafeFs.LastError + "）。");
            }
        }

        private static void RestoreStrippedUnityFontPatcher(string dir, RestoreStats stats)
        {
            string installed = PathUtil.Join(dir, "BepInEx\\patchers\\DeepSeekUnityFontPatcher.dll");
            string owned = PathUtil.Join(dir, "BepInEx\\patchers\\DeepSeekUnityFontPatcher.dll.dst-owned");
            if (!SafeFs.Exists(owned)) {
                if (SafeFs.Exists(installed)) {
                    stats.Preserved++;
                    Log.Append("Restore Unity Mono: DeepSeekUnityFontPatcher.dll has no launcher ownership snapshot and was preserved.");
                }
                return;
            }
            if (SafeFs.Exists(installed) && !SafeFs.FilesEqual(installed, owned)) {
                stats.Preserved++;
                Log.Append("Restore Unity Mono: DeepSeekUnityFontPatcher.dll changed after deployment; the file and ownership snapshot were preserved.");
                return;
            }
            if (SafeFs.Exists(installed) && !RestoreDeleteFile(installed, stats)) return;
            RestoreDeleteFile(owned, stats);
        }

        private static void RestoreStrippedIniConfig(string path, string label, RestoreStats stats)
        {
            string backup = path + ".dst-stripped-backup";
            string owned = path + ".dst-stripped-owned";
            if (!SafeFs.Exists(owned)) return;

            if (SafeFs.Exists(path) && !SafeFs.FilesEqual(path, owned)) {
                stats.Preserved++;
                Log.Append("Restore Unity Mono: " + label + " changed after stripped-runtime deployment; config and recovery metadata were preserved.");
                return;
            }
            if (SafeFs.Exists(backup)) {
                if (!SafeFs.CopyFileSafe(backup, path)) {
                    stats.Failed++;
                    Log.Append("Restore Unity Mono: could not restore " + label + " (Windows error " + SafeFs.LastError + ").");
                    return;
                }
                stats.Restored++;
                Log.Append("Restore Unity Mono: restored the original " + label + ".");
                RestoreDeleteFile(backup, stats);
            } else if (SafeFs.Exists(path) && !RestoreDeleteFile(path, stats)) {
                return;
            }
            RestoreDeleteFile(owned, stats);
        }

        private static void RestoreUnityMono(string dir, RestoreStats stats)
        {
            RestoreUnityMonoPlugin(dir, stats);
            RestoreUnityMonoNewtonsoft(dir, stats);
            RestoreStrippedUnityFontPatcher(dir, stats);

            RestoreStrippedIniConfig(PathUtil.Join(dir, "doorstop_config.ini"), "doorstop_config.ini", stats);
            RestoreStrippedIniConfig(PathUtil.Join(dir, "BepInEx\\config\\BepInEx.cfg"), "BepInEx.cfg", stats);

            string corlib = PathUtil.Join(dir, "BepInEx\\unstripped_corlib");
            string installedMarker = PathUtil.Join(corlib, ".dst-installed-by-ds");
            string payloadTree = PathUtil.Join(Launcher.Root, "payloads\\UnityMonoCorlib");
            string payloadMarker = PathUtil.Join(payloadTree, ".dst-installed-by-ds");
            RestoreRemoveOwnedTree(corlib, installedMarker, payloadTree, payloadMarker, "Unity Mono complete corlib", stats);
        }

        private static void RestoreXunityConfig(string dir, RestoreStats stats)
        {
            string cfg = PathUtil.Join(dir, "BepInEx\\config\\AutoTranslatorConfig.ini");
            string backup = cfg + ".dst-backup";
            string owned = cfg + ".dst-owned";

            if (!SafeFs.Exists(owned)) return;
            if (SafeFs.Exists(cfg) && !SafeFs.FilesEqual(cfg, owned)) {
                stats.Preserved++;
                Log.Append("还原 Unity IL2CPP：AutoTranslatorConfig.ini 已被用户修改，配置和备份均已保留。");
                return;
            }

            if (SafeFs.Exists(backup)) {
                if (SafeFs.CopyFileSafe(backup, cfg)) {
                    stats.Restored++;
                    Log.Append("还原 Unity IL2CPP：已恢复原 AutoTranslatorConfig.ini。");
                    RestoreDeleteFile(backup, stats);
                } else {
                    stats.Failed++;
                    Log.Append("还原 Unity IL2CPP：无法恢复配置备份（Windows 错误 " + SafeFs.LastError + "）。");
                    return;
                }
            } else {
                RestoreDeleteFile(cfg, stats);
            }
            RestoreDeleteFile(owned, stats);
        }

        private static void RestoreUnityIl2cpp(string dir, RestoreStats stats)
        {
            string installed = PathUtil.Join(dir, "BepInEx\\plugins\\XUnity.AutoTranslator\\Translators\\DeepSeekTranslate.dll");
            string payload = PathUtil.Join(Launcher.Root, "payloads\\UnityIL2CPP\\DeepSeekXUnityTranslator\\DeepSeekTranslate.dll");
            RestoreRemoveMatchingFile(installed, payload, "DeepSeek XUnity endpoint", stats);
            RestoreXunityPlugin(dir, stats);

            string tree = PathUtil.Join(dir, "BepInEx\\plugins\\DeepSeekTMPFontFallback");
            string installedMarker = PathUtil.Join(tree, "DeepSeekTMPFontFallback.dll");
            string payloadTree = PathUtil.Join(Launcher.Root, "payloads\\UnityIL2CPP\\DeepSeekTMPFontFallback\\BepInEx\\plugins\\DeepSeekTMPFontFallback");
            string payloadMarker = PathUtil.Join(payloadTree, "DeepSeekTMPFontFallback.dll");
            RestoreRemoveOwnedTree(tree, installedMarker, payloadTree, payloadMarker, "DeepSeek TMP 字体回退", stats);

            string disabled = PathUtil.Join(dir, "BepInEx\\plugins\\UnityTranslator.dll.disabled");
            string disabledPdb = PathUtil.Join(dir, "BepInEx\\plugins\\UnityTranslator.pdb.disabled");
            if (SafeFs.Exists(disabled)) {
                if (IsBundledUnityMonoPlugin(disabled)) {
                    if (RestoreDeleteFile(disabled, stats)) RestoreDeleteFile(disabledPdb, stats);
                } else {
                    stats.Preserved++;
                    Log.Append("还原 Unity IL2CPP：禁用的 UnityTranslator.dll 无法确认归属，已保留。");
                }
            }

            string mscorlib = PathUtil.Join(dir, "BepInEx\\core\\Il2Cppmscorlib.dll");
            string mscorlibDisabled = mscorlib + ".disabled";
            if (SafeFs.Exists(mscorlibDisabled)) {
                if (SafeFs.Exists(mscorlib)) {
                    stats.Preserved++;
                    Log.Append("还原 Unity IL2CPP：Il2Cppmscorlib.dll 已存在，禁用备份已保留以避免覆盖。");
                } else if (SafeFs.MoveFileSafe(mscorlibDisabled, mscorlib, SafeFs.MOVEFILE_WRITE_THROUGH)) {
                    stats.Restored++;
                    Log.Append("还原 Unity IL2CPP：已恢复原 Il2Cppmscorlib.dll。");
                } else {
                    stats.Failed++;
                    Log.Append("还原 Unity IL2CPP：无法恢复 Il2Cppmscorlib.dll（Windows 错误 " + SafeFs.LastError + "）。");
                }
            }

            RestoreXunityConfig(dir, stats);
        }
    }
}
