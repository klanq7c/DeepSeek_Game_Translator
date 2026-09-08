using System;
using System.Runtime.InteropServices;
using System.Text;

namespace DstLauncher
{
    /*
     * GodotProbe —— godot_probe.c 的移植：判断 headless 预检输出是不是"命令行解析器
     * 明确拒绝了该选项"。进程非零退出本身不能作为结论（导出的游戏可能在自身
     * autoload/DRM/音频初始化里失败），只有输出同时提到该选项和拒绝性诊断才算。
     */
    public static class GodotProbe
    {
        private static char AsciiLower(char c)
        {
            return c >= 'A' && c <= 'Z' ? (char)(c - 'A' + 'a') : c;
        }

        /* ascii_contains_case_insensitive：C 版对 char* 逐字节比较，locale 为 "C" 时
           只折叠 ASCII，这里同样只折叠 ASCII。 */
        private static bool ContainsNoCase(string text, string needle)
        {
            if (text == null || string.IsNullOrEmpty(needle)) return false;
            for (int p = 0; p < text.Length; p++) {
                int i = 0;
                while (i < needle.Length && p + i < text.Length &&
                       AsciiLower(text[p + i]) == AsciiLower(needle[i])) {
                    i++;
                }
                if (i == needle.Length) return true;
            }
            return false;
        }

        private static readonly string[] Markers = {
            "unknown option",
            "unknown command line",
            "unrecognized option",
            "unrecognized command line",
            "invalid option",
            "invalid command line",
            "unsupported option",
            "not supported",
            "not available"
        };

        public static bool OutputExplicitlyRejectsMainPack(string output)
        {
            if (output == null || !ContainsNoCase(output, "main-pack")) return false;
            foreach (string marker in Markers) {
                if (ContainsNoCase(output, marker)) return true;
            }
            return false;
        }
    }

    /*
     * GodotPreflightCache —— godot_preflight_cache.c 的移植：把确定性的 headless 预检
     * 结论缓存到 config\godot_preflight.ini。
     *
     * 键 = 翻译器版本 + 预检类别 + 相关文件特征（路径 + 最后修改时间 + 大小）+
     * 文本特征，做 FNV-1a 后取十六进制作节名，节内再存完整签名串防碰撞。
     * 翻译器版本参与哈希，所以升级后所有条目自然失效——C 版由
     * -DDS_TRANSLATOR_VERSION 注入，C# 侧由 csproj 从同一个 VERSION 文件生成
     * BuildInfo.TranslatorVersion，两者必须相同，否则两版算出的节名不一致。
     */
    public static class GodotPreflightCache
    {
        [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
        private static extern uint GetPrivateProfileStringW(string section, string key, string def,
                                                            StringBuilder outBuf, uint size, string path);

        [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
        [return: MarshalAs(UnmanagedType.Bool)]
        private static extern bool WritePrivateProfileStringW(string section, string key, string value, string path);

        [StructLayout(LayoutKind.Sequential)]
        private struct WIN32_FILE_ATTRIBUTE_DATA
        {
            public uint dwFileAttributes;
            public uint ftCreationLow, ftCreationHigh;
            public uint ftAccessLow, ftAccessHigh;
            public uint ftWriteLow, ftWriteHigh;
            public uint nFileSizeHigh, nFileSizeLow;
        }

        [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
        [return: MarshalAs(UnmanagedType.Bool)]
        private static extern bool GetFileAttributesExW(string path, int level, out WIN32_FILE_ATTRIBUTE_DATA data);

        /* C 调用方统一用 WCHAR sig[MAX_PATH * 8]（含结尾 NUL）。 */
        public const int SigCap = 260 * 4 * 8;

        /*
         * Sig —— godot_preflight_sig_init/append 的等价物。C 版在固定缓冲上工作并按
         * cap 截断，这里保留同样的截断语义，否则超长游戏路径会让两版算出不同的键。
         */
        public sealed class Sig
        {
            private readonly StringBuilder _sb = new StringBuilder();
            private readonly int _cap;

            public Sig() : this(SigCap) { }
            public Sig(int cap) { _cap = cap; }

            private void Append(string text)
            {
                if (_cap == 0 || text == null) return;
                int len = _sb.Length;
                if (len + 1 >= _cap) return;
                int avail = _cap - len - 1;
                int tl = text.Length;
                if (tl > avail) tl = avail;
                _sb.Append(text, 0, tl);
            }

            public void AddText(string text)
            {
                Append("|");
                Append(text ?? "");
            }

            /* 文件特征 = 最后修改时间 + 大小；不存在时特征为 0。 */
            public void AddFile(string path)
            {
                Append("|");
                Append(path ?? "");
                WIN32_FILE_ATTRIBUTE_DATA fad;
                if (!GetFileAttributesExW(path ?? "", 0 /* GetFileExInfoStandard */, out fad)) {
                    Append("|0|0|");
                    return;
                }
                ulong mtime = ((ulong)fad.ftWriteHigh << 32) | fad.ftWriteLow;
                ulong size = ((ulong)fad.nFileSizeHigh << 32) | fad.nFileSizeLow;
                Append("|" + mtime.ToString("x16") + "|" + size + "|");
            }

            public override string ToString() { return _sb.ToString(); }
        }

        /* FNV-1a：先哈希翻译器版本（ANSI），再哈希类别与签名串（UTF-16 码元）。 */
        private static ulong Hash(string kind, string sig)
        {
            ulong h = 1469598103934665603UL;
            foreach (byte b in Encoding.UTF8.GetBytes(BuildInfo.TranslatorVersion)) {
                h ^= b;
                h *= 1099511628211UL;
            }
            h ^= 0x1f;
            h *= 1099511628211UL;
            foreach (char c in kind) {
                h ^= c;
                h *= 1099511628211UL;
            }
            h ^= 0x2f;
            h *= 1099511628211UL;
            foreach (char c in sig) {
                h ^= c;
                h *= 1099511628211UL;
            }
            return h;
        }

        private static string CachePath()
        {
            return PathUtil.Join(PathUtil.Join(Launcher.Root, "config"), "godot_preflight.ini");
        }

        private static string Section(string kind, string sig)
        {
            return "p_" + Hash(kind, sig).ToString("x16");
        }

        /* 1 = 命中且结论为"支持"，0 = 命中且结论为"明确拒绝"，-1 = 未命中。 */
        public static int Get(string kind, string sig)
        {
            if (kind == null || sig == null) return -1;
            string path = CachePath();
            string section = Section(kind, sig);

            var storedSig = new StringBuilder(260 * 8);
            GetPrivateProfileStringW(section, "sig", "", storedSig, 260 * 8, path);
            if (storedSig.Length == 0 || !string.Equals(storedSig.ToString(), sig, StringComparison.Ordinal)) return -1;

            var storedResult = new StringBuilder(16);
            GetPrivateProfileStringW(section, "result", "", storedResult, 16, path);
            string result = storedResult.ToString();
            if (string.Equals(result, "ok", StringComparison.OrdinalIgnoreCase)) return 1;
            if (string.Equals(result, "reject", StringComparison.OrdinalIgnoreCase)) return 0;
            return -1;
        }

        /* 只允许写入确定性结论：result 非 0 = 支持，0 = 明确拒绝。
           瞬态失败（超时/无法启动）不得调用，避免把偶发故障变成永久结论。 */
        public static void Put(string kind, string sig, int result)
        {
            if (kind == null || sig == null) return;
            string cfgdir = PathUtil.Join(Launcher.Root, "config");
            SafeFs.EnsureDir(cfgdir);
            string path = CachePath();
            string section = Section(kind, sig);
            WritePrivateProfileStringW(section, "sig", sig, path);
            WritePrivateProfileStringW(section, "result", result != 0 ? "ok" : "reject", path);
        }
    }
}
