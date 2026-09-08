using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.Net;
using System.Text;
using System.Threading;
using DstCore;

namespace DstLauncher
{
    /*
     * Warmup —— warmup.c 的逐函数移植（翻译缓存预热）。
     *
     * 职责与 C 版相同：按引擎扫描游戏目录中的文本数据（Ren'Py .rpy、RPG Maker
     * MV/MZ 的 data/*.json 与插件外部 TXT/CSV、Unity 的 *_Data 资源与 XUnity 翻译
     * 文件、Godot 的工程/导出资源与 .pck 目录表），经引擎专用过滤与去重后分批 POST
     * 到本地服务端 /prefetch（后台排队）或 /cache/import（导入已有译文）。
     *
     * 字节串约定：本模块所有"文本"都是 C 版 char* 的逐字节映射——string 的每个
     * char 是一个 0..255 的字节。这样 Length 即 strlen，IndexOf/Contains 即
     * strchr/strstr，1200 字节上限、手工 UTF-8 解码、非法序列透传都与 C 版逐字节
     * 一致；只在写 stdout 或 HTTP 请求体时才还原为 byte[]。C 字符串在首个 NUL 处
     * 截断的语义由 ByteStr.CStr 保证（JSON 的 \u0000 转义会产生 NUL）。
     *
     * 分文件：Warmup.cs（列表、通用过滤、JSON、批次序列化、本地 HTTP、入口）、
     * WarmupRenPy.cs、WarmupRpgm.cs、WarmupUnity.cs、WarmupGodot.cs（各引擎扫描器）。
     */
    public static partial class Warmup
    {
        /* ---- 预热容量与扫描限制（与 warmup.c / warmup_internal.h 同值） ---- */
        public const int MaxItems = 1200;                 /* WARMUP_MAX_ITEMS：TextList 默认上限，也是 PairList 上限 */
        public const int MaxTextBytes = 1200;             /* WARMUP_MAX_TEXT_BYTES */
        public const int GodotMaxItems = 12000;
        public const int RpgmMaxItems = 40000;
        public const int UnityMaxItems = 8000;
        public const int RenpyMaxItems = 100000;
        public const int BatchItems = 512;
        public const uint UnityAssetScanMaxBytes = 64u * 1024u * 1024u;
        public const ulong UnityBundleScanMaxBytes = 256ul * 1024ul * 1024ul;
        public const int UnityBundleScanChunkBytes = 1024 * 1024;
        public const ulong RenpyScriptScanMaxBytes = 8ul * 1024ul * 1024ul;
        public const int RenpyScanMaxDepth = 12;
        public const uint RpgmJsonScanMaxBytes = 64u * 1024u * 1024u;
        public const uint RpgmTextScanMaxBytes = 8u * 1024u * 1024u;

        /* 诊断转储（对应 C 版 g_warmup_dump_stdout）：非 null 时 LocalHttp 不联网、不查
           缓存、不回写翻译文件，每个本应 POST 的批次交给它 (path, body)。
           tests/launcher_parity 用 --warmup 与 C 版 --warmup-and-exit 逐字节对比。 */
        public static Action<string, byte[]> DumpSink;

        /* warmup_translations：按引擎分派。返回 0 表示该引擎的预热路径已执行（包括
           "无事可做"）；五种引擎的扫描器均已移植，未知引擎与 C 版一样不做任何事。 */
        public static int Run(string dir, Engine engine)
        {
            if (string.IsNullOrEmpty(dir)) return 0;
            switch (engine) {
                case Engine.RenPy: WarmupRenPy(dir); return 0;
                case Engine.Unity:
                case Engine.UnityIl2cpp: WarmupXunity(dir); return 0;
                case Engine.RpgmMv: WarmupRpgm(dir); return 0;
                case Engine.Godot: WarmupGodot(dir); return 0;
                default: return 0;
            }
        }

        /* ======================== 通用字符串工具 ======================== */

        internal static bool IsAlpha(char c) { return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z'); }
        internal static bool IsUpper(char c) { return c >= 'A' && c <= 'Z'; }
        internal static bool IsLower(char c) { return c >= 'a' && c <= 'z'; }
        internal static bool IsDigit(char c) { return c >= '0' && c <= '9'; }

        /* has_cjk_utf8：手工解码 2/3 字节序列，命中 U+4E00..U+9FFF 即为含汉字。 */
        internal static bool HasCjkUtf8(string s)
        {
            int i = 0;
            while (i < s.Length) {
                int c = s[i];
                uint cp = 0;
                if (c < 0x80) {
                    cp = (uint)c;
                    i++;
                } else if ((c & 0xe0) == 0xc0 && ByteStr.At(s, i + 1) != 0) {
                    cp = (uint)(((c & 0x1f) << 6) | (s[i + 1] & 0x3f));
                    i += 2;
                } else if ((c & 0xf0) == 0xe0 && ByteStr.At(s, i + 1) != 0 && ByteStr.At(s, i + 2) != 0) {
                    cp = (uint)(((c & 0x0f) << 12) | ((s[i + 1] & 0x3f) << 6) | (s[i + 2] & 0x3f));
                    i += 3;
                } else {
                    i++;
                }
                if (cp >= 0x4e00 && cp <= 0x9fff) return true;
            }
            return false;
        }

        /* should_warm_text：通用过滤——长度 2..1200、无反斜杠/URL/媒体扩展名、含字母或
           非 ASCII、且不含汉字。 */
        internal static bool ShouldWarmText(string s)
        {
            int len = s.Length;
            if (len < 2 || len > MaxTextBytes) return false;
            if (s.IndexOf('\\') >= 0 || s.Contains("://") || s.Contains(".png") || s.Contains(".ogg") || s.Contains(".m4a")) return false;
            bool signal = false;
            for (int i = 0; i < len; i++) {
                char c = s[i];
                if (c < 0x80) {
                    if (IsAlpha(c)) signal = true;
                } else {
                    signal = true;
                }
            }
            return signal && !HasCjkUtf8(s);
        }

        internal static bool AsciiOnly(string s)
        {
            for (int i = 0; i < s.Length; i++) if (s[i] >= 0x80) return false;
            return true;
        }

        /* starts_with_word_i：不区分大小写以 word 开头，且后随分隔符或行尾。 */
        internal static bool StartsWithWordI(string s, string word)
        {
            if (!ByteStr.StartsWithNoCase(s, word)) return false;
            char c = ByteStr.At(s, word.Length);
            return c == 0 || c == ' ' || c == '\t' || c == ':' || c == '(';
        }

        /* ======================== JSON 解析辅助 ======================== */

        private static int HexValue(char c)
        {
            if (c >= '0' && c <= '9') return c - '0';
            if (c >= 'a' && c <= 'f') return c - 'a' + 10;
            if (c >= 'A' && c <= 'F') return c - 'A' + 10;
            return -1;
        }

        /* bb_utf8：码点编码为 UTF-8 字节（> 0x10FFFF 丢弃）。 */
        internal static void AppendUtf8(StringBuilder b, uint cp)
        {
            if (cp < 0x80) {
                b.Append((char)cp);
            } else if (cp < 0x800) {
                b.Append((char)(0xc0 | (cp >> 6)));
                b.Append((char)(0x80 | (cp & 63)));
            } else if (cp < 0x10000) {
                b.Append((char)(0xe0 | (cp >> 12)));
                b.Append((char)(0x80 | ((cp >> 6) & 63)));
                b.Append((char)(0x80 | (cp & 63)));
            } else if (cp <= 0x10ffff) {
                b.Append((char)(0xf0 | (cp >> 18)));
                b.Append((char)(0x80 | ((cp >> 12) & 63)));
                b.Append((char)(0x80 | ((cp >> 6) & 63)));
                b.Append((char)(0x80 | (cp & 63)));
            }
        }

        /* json_u4：4 位十六进制（\uXXXX），任一位缺失或非法即失败。 */
        internal static bool JsonU4(string s, int p, out uint cp)
        {
            cp = 0;
            if (ByteStr.At(s, p) == 0 || ByteStr.At(s, p + 1) == 0 || ByteStr.At(s, p + 2) == 0 || ByteStr.At(s, p + 3) == 0) return false;
            int a = HexValue(s[p]), b = HexValue(s[p + 1]), c = HexValue(s[p + 2]), d = HexValue(s[p + 3]);
            if (a < 0 || b < 0 || c < 0 || d < 0) return false;
            cp = (uint)((a << 12) | (b << 8) | (c << 4) | d);
            return true;
        }

        internal static bool JsonU4(byte[] buf, int end, int p, out uint cp)
        {
            cp = 0;
            if (ByteStr.At(buf, end, p) == 0 || ByteStr.At(buf, end, p + 1) == 0 ||
                ByteStr.At(buf, end, p + 2) == 0 || ByteStr.At(buf, end, p + 3) == 0) return false;
            int a = HexValue((char)buf[p]), b = HexValue((char)buf[p + 1]), c = HexValue((char)buf[p + 2]), d = HexValue((char)buf[p + 3]);
            if (a < 0 || b < 0 || c < 0 || d < 0) return false;
            cp = (uint)((a << 12) | (b << 8) | (c << 4) | d);
            return true;
        }

        /* json_ws：跳过 JSON 空白（空格/制表/回车/换行）。 */
        internal static int JsonWs(byte[] buf, int end, int p)
        {
            while (p < end && (buf[p] == ' ' || buf[p] == '\t' || buf[p] == '\r' || buf[p] == '\n')) p++;
            return p;
        }

        /* json_string_at：解析 JSON 字符串字面量（\n \r \t \uXXXX 及代理对），返回字节串。
           前导空白后不是引号时返回 null 且不移动游标；未闭合的字符串返回已解析部分。 */
        internal static string JsonStringAt(byte[] buf, int end, ref int pp)
        {
            int p = JsonWs(buf, end, pp);
            if (ByteStr.At(buf, end, p) != '"') return null;
            p++;
            var b = new StringBuilder(64);
            while (p < end && buf[p] != '"') {
                char c = (char)buf[p++];
                if (c == '\\') {
                    char e = ByteStr.At(buf, end, p);
                    if (e == 0) break;
                    p++;
                    if (e == 'n') b.Append('\n');
                    else if (e == 'r') b.Append('\r');
                    else if (e == 't') b.Append('\t');
                    else if (e == 'u') {
                        uint cp;
                        if (!JsonU4(buf, end, p, out cp)) break;
                        p += 4;
                        /* UTF-16 代理对：高位代理后跟 \uDCxx 合并为完整码点，孤立代理记为 U+FFFD。 */
                        if (cp >= 0xd800 && cp <= 0xdbff) {
                            uint lo;
                            if (ByteStr.At(buf, end, p) == '\\' && ByteStr.At(buf, end, p + 1) == 'u' &&
                                JsonU4(buf, end, p + 2, out lo) && lo >= 0xdc00 && lo <= 0xdfff) {
                                cp = 0x10000 + ((cp - 0xd800) << 10) + (lo - 0xdc00);
                                p += 6;
                            } else {
                                cp = 0xfffd;
                            }
                        } else if (cp >= 0xdc00 && cp <= 0xdfff) {
                            cp = 0xfffd;
                        }
                        AppendUtf8(b, cp);
                    } else {
                        b.Append(e);
                    }
                } else {
                    b.Append(c);
                }
            }
            if (ByteStr.At(buf, end, p) == '"') p++;
            pp = p;
            return ByteStr.CStr(b);
        }

        /* atoi：跳过 C isspace 空白、可选符号、十进制数字；无数字为 0，溢出饱和。 */
        internal static int Atoi(byte[] buf, int end, int p)
        {
            while (p < end && (buf[p] == ' ' || buf[p] == '\t' || buf[p] == '\n' || buf[p] == '\v' || buf[p] == '\f' || buf[p] == '\r')) p++;
            bool neg = false;
            if (p < end && (buf[p] == '+' || buf[p] == '-')) {
                neg = buf[p] == '-';
                p++;
            }
            long v = 0;
            while (p < end && buf[p] >= '0' && buf[p] <= '9') {
                v = v * 10 + (buf[p] - '0');
                if (v > int.MaxValue + 1L) break;
                p++;
            }
            if (neg) v = -v;
            if (v > int.MaxValue) return int.MaxValue;
            if (v < int.MinValue) return int.MinValue;
            return (int)v;
        }

        /* bb_json：JSON 转义（" \ \n \r \t，其余控制字符 \u00xx），输出字节串。 */
        internal static void AppendJson(StringBuilder b, string s)
        {
            b.Append('"');
            for (int i = 0; s != null && i < s.Length; i++) {
                char c = s[i];
                if (c == '"' || c == '\\') {
                    b.Append('\\').Append(c);
                } else if (c == '\n') b.Append("\\n");
                else if (c == '\r') b.Append("\\r");
                else if (c == '\t') b.Append("\\t");
                else if (c < 32) {
                    b.Append("\\u").Append(((int)c).ToString("x4"));
                } else {
                    b.Append(c);
                }
            }
            b.Append('"');
        }

        /* ======================== 批次序列化 ======================== */

        /* post_prefetch_batch 的请求体：{"texts":[...]}；当该批次内至少一条 prev 非空时
           追加 "prevs" 平行数组（缺失处为空串），否则与旧协议字节级相同。 */
        internal static byte[] BuildPrefetchBody(TextList l, int start, int count)
        {
            bool anyPrev = false;
            if (l.Prev != null) {
                for (int i = 0; i < count; i++) {
                    if (!string.IsNullOrEmpty(l.PrevAt(start + i))) { anyPrev = true; break; }
                }
            }
            var b = new StringBuilder(2048);
            b.Append("{\"texts\":[");
            for (int i = 0; i < count; i++) {
                if (i > 0) b.Append(',');
                AppendJson(b, l.Items[start + i]);
            }
            b.Append(']');
            if (anyPrev) {
                b.Append(",\"prevs\":[");
                for (int i = 0; i < count; i++) {
                    if (i > 0) b.Append(',');
                    string p = l.PrevAt(start + i);
                    if (!string.IsNullOrEmpty(p)) AppendJson(b, p);
                    else b.Append("\"\"");
                }
                b.Append(']');
            }
            b.Append('}');
            return ByteStr.ToBytes(b.ToString());
        }

        /* post_import_batch 的请求体：{"entries":[{"key":..,"value":..},...]}。 */
        internal static byte[] BuildImportBody(PairList l, int start, int count)
        {
            var b = new StringBuilder(4096);
            b.Append("{\"entries\":[");
            for (int i = 0; i < count; i++) {
                if (i > 0) b.Append(',');
                b.Append("{\"key\":");
                AppendJson(b, l.Keys[start + i]);
                b.Append(",\"value\":");
                AppendJson(b, l.Vals[start + i]);
                b.Append('}');
            }
            b.Append("]}");
            return ByteStr.ToBytes(b.ToString());
        }

        /* post_prefetch_all：等服务端就绪后分批提交 /prefetch，返回被接受的条数。 */
        private static int PostPrefetchAll(TextList prefetch)
        {
            if (prefetch.Count == 0) return 0;
            var http = new LocalHttp();
            if (!http.Open()) return 0;
            if (!http.WaitReady(8000)) {
                Log.Append("预热跳过：本地服务端未及时就绪。");
                http.Close();
                return 0;
            }
            int queued = 0;
            for (int i = 0; i < prefetch.Count; i += BatchItems) {
                int n = Math.Min(BatchItems, prefetch.Count - i);
                if (http.Post(Contract.PathPrefetch, BuildPrefetchBody(prefetch, i, n), 1500)) queued += n;
            }
            http.Close();
            return queued;
        }
    }

    /* 字节串工具：见 Warmup 类头部的约定。 */
    internal static class ByteStr
    {
        public static string FromBytes(byte[] b, int start, int count)
        {
            var chars = new char[count];
            for (int i = 0; i < count; i++) chars[i] = (char)b[start + i];
            return new string(chars);
        }

        public static byte[] ToBytes(string s)
        {
            var b = new byte[s.Length];
            for (int i = 0; i < s.Length; i++) b[i] = (byte)s[i];
            return b;
        }

        /* 首个 NUL 之后的内容对 C 字符串不可见。 */
        public static string CStr(StringBuilder sb)
        {
            return CStr(sb.ToString());
        }

        public static string CStr(string s)
        {
            int nul = s.IndexOf('\0');
            return nul >= 0 ? s.Substring(0, nul) : s;
        }

        /* strlen 语义下缓冲的可见长度：首个 NUL 的下标，没有则为全长。 */
        public static int CStrLen(byte[] buf)
        {
            int i = Array.IndexOf(buf, (byte)0);
            return i < 0 ? buf.Length : i;
        }

        /* C 里越过字符串末尾读到的是 NUL 终止符。 */
        public static char At(string s, int i) { return i >= 0 && i < s.Length ? s[i] : '\0'; }
        public static char At(byte[] b, int end, int i) { return i >= 0 && i < end ? (char)b[i] : '\0'; }

        public static char AsciiLower(char c) { return c >= 'A' && c <= 'Z' ? (char)(c - 'A' + 'a') : c; }

        /* _strnicmp(s, ascii, strlen(ascii)) == 0：只折叠 ASCII 大小写（C 运行时 "C" 区域语义）。 */
        public static bool StartsWithNoCase(string s, string ascii)
        {
            if (s.Length < ascii.Length) return false;
            for (int i = 0; i < ascii.Length; i++) {
                if (AsciiLower(s[i]) != AsciiLower(ascii[i])) return false;
            }
            return true;
        }

        /* _stricmp(s, ascii) == 0 */
        public static bool EqualsNoCase(string s, string ascii)
        {
            return s.Length == ascii.Length && StartsWithNoCase(s, ascii);
        }

        /* _strnicmp(s + start, ascii, n) == 0，n ≤ ascii.Length。 */
        public static bool RegionEqualsNoCase(string s, int start, string ascii, int n)
        {
            if (start + n > s.Length) return false;
            for (int i = 0; i < n; i++) {
                if (AsciiLower(s[start + i]) != AsciiLower(ascii[i])) return false;
            }
            return true;
        }

        private static bool IsWs(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }

        /* trim_ascii：去掉首尾的空格/制表/回车/换行。 */
        public static string Trim(string s)
        {
            int a = 0, b = s.Length;
            while (a < b && IsWs(s[a])) a++;
            while (b > a && IsWs(s[b - 1])) b--;
            return a == 0 && b == s.Length ? s : s.Substring(a, b - a);
        }

        /* trim_ascii 原地修改时只把尾部空白置 NUL，首部空白仍留在缓冲里；需要在
           "被 trim 过的缓冲"上继续 strstr 时用这个只去尾的版本。 */
        public static string TrimEnd(string s)
        {
            int b = s.Length;
            while (b > 0 && IsWs(s[b - 1])) b--;
            return b == s.Length ? s : s.Substring(0, b);
        }

        /* contains_any：s 含 chars 中任一字符。 */
        public static bool ContainsAny(string s, string chars)
        {
            for (int i = 0; i < s.Length; i++) if (chars.IndexOf(s[i]) >= 0) return true;
            return false;
        }

        public static int IndexOf(byte[] buf, byte value, int start, int end)
        {
            for (int i = start; i < end; i++) if (buf[i] == value) return i;
            return -1;
        }
    }

    /* TextList（warmup_internal.h）：待预热文本的有序去重列表。 */
    internal sealed class TextList
    {
        public readonly List<string> Items = new List<string>();
        /* 可选平行数组（仅 Ren'Py 扫描器维护）：Prev[i] 是 Items[i] 在同一脚本内的上一条采集文本。 */
        public List<string> Prev;
        private readonly HashSet<string> _seen = new HashSet<string>(StringComparer.Ordinal);
        public int MaxItems;

        public int Limit { get { return MaxItems != 0 ? MaxItems : Warmup.MaxItems; } }
        public int Count { get { return Items.Count; } }
        public bool Full { get { return Items.Count >= Limit; } }

        /* textlist_add：空串、超长、重复或已达上限时忽略。 */
        public void Add(string s)
        {
            if (string.IsNullOrEmpty(s) || s.Length > Warmup.MaxTextBytes) return;
            if (_seen.Contains(s)) return;
            if (Items.Count >= Limit) return;
            Items.Add(s);
            _seen.Add(s);
        }

        /* renpy_prevs_ensure：惰性建立平行数组；此前已采集的条目 prev 为 null。 */
        public void EnsurePrev()
        {
            if (Prev == null) Prev = new List<string>();
        }

        public void SetPrev(int index, string prev)
        {
            while (Prev.Count <= index) Prev.Add(null);
            Prev[index] = prev;
        }

        public string PrevAt(int index)
        {
            return Prev != null && index < Prev.Count ? Prev[index] : null;
        }
    }

    /* PairList：XUnity 已有译文（key=原文, value=译文），按 key 去重，上限 WARMUP_MAX_ITEMS。 */
    internal sealed class PairList
    {
        public readonly List<string> Keys = new List<string>();
        public readonly List<string> Vals = new List<string>();
        private readonly HashSet<string> _seen = new HashSet<string>(StringComparer.Ordinal);

        public int Count { get { return Keys.Count; } }

        public void Add(string k, string v)
        {
            if (string.IsNullOrEmpty(k) || string.IsNullOrEmpty(v) || k.Length > Warmup.MaxTextBytes) return;
            if (_seen.Contains(k)) return;
            if (Keys.Count >= Warmup.MaxItems) return;
            Keys.Add(k);
            Vals.Add(v);
            _seen.Add(k);
        }
    }

    /*
     * LocalHttp —— warmup.c 的 LocalHttp/WinHTTP 会话对等实现。
     *
     * C 版复用一个 WinHTTP session/connection 提交数百批；这里用 HttpWebRequest
     * （与 ServerProcess 一致，KeepAlive 由 ServicePoint 连接池复用）。语义保持：
     *   - 只有 /health 明确返回 200 才允许提交；
     *   - 只有 2xx 视为批次被接受；4xx/5xx 记一次日志并返回失败，调用方不得计为已排队；
     *   - 超时/连接失败静默返回失败（与 WinHttpSendRequest 失败一致）。
     * Warmup.DumpSink 非 null 时不联网：Open/WaitReady 直接成功，Post 交给 sink，
     * GetCachedTranslate 一律未命中。
     */
    internal sealed class LocalHttp
    {
        private bool _open;
        private bool _rejectLogged;

        public bool Open()
        {
            _open = true;
            return true;
        }

        public void Close()
        {
            _open = false;
        }

        private static bool GetStatus(string path, int timeoutMs, out int status)
        {
            status = 0;
            try {
                var req = (HttpWebRequest)WebRequest.Create(Contract.DefaultBaseUrl + path);
                req.Method = "GET";
                req.Timeout = timeoutMs;
                req.ReadWriteTimeout = timeoutMs;
                req.Proxy = null;
                req.UserAgent = "ds-game-translator Launcher/3.1";
                using (var resp = (HttpWebResponse)req.GetResponse()) {
                    status = (int)resp.StatusCode;
                    return true;
                }
            } catch (WebException ex) {
                var resp = ex.Response as HttpWebResponse;
                if (resp != null) {
                    status = (int)resp.StatusCode;
                    resp.Dispose();
                    return true;
                }
                return false;
            }
        }

        /* local_http_wait_ready：总超时内轮询 /health，只有明确 200 才放行。 */
        public bool WaitReady(int totalTimeoutMs)
        {
            if (Warmup.DumpSink != null) return true;
            var sw = Stopwatch.StartNew();
            for (;;) {
                int status;
                if (GetStatus(Contract.PathHealth, 350, out status) && status == 200) return true;
                if (sw.ElapsedMilliseconds >= totalTimeoutMs) return false;
                Thread.Sleep(100);
            }
        }

        /* local_http_post：POST JSON，2xx 为接受；非 2xx 记录一次并返回失败。 */
        public bool Post(string path, byte[] body, int timeoutMs)
        {
            if (Warmup.DumpSink != null) {
                Warmup.DumpSink(path, body);
                return true;
            }
            if (!_open) return false;
            int status;
            try {
                var req = (HttpWebRequest)WebRequest.Create(Contract.DefaultBaseUrl + path);
                req.Method = "POST";
                req.ContentType = "application/json";
                req.ContentLength = body.Length;
                req.Timeout = timeoutMs;
                req.ReadWriteTimeout = timeoutMs;
                req.Proxy = null;
                req.UserAgent = "ds-game-translator Launcher/3.1";
                using (Stream s = req.GetRequestStream()) s.Write(body, 0, body.Length);
                using (var resp = (HttpWebResponse)req.GetResponse()) {
                    status = (int)resp.StatusCode;
                }
            } catch (WebException ex) {
                var resp = ex.Response as HttpWebResponse;
                if (resp == null) return false; /* 连接失败/超时：与 WinHttpSendRequest 失败一致，静默 */
                status = (int)resp.StatusCode;
                resp.Dispose();
            }
            if (status < 200 || status >= 300) {
                if (!_rejectLogged) {
                    _rejectLogged = true;
                    Log.Append("预热批次被本地服务端拒绝（HTTP " + status + "），该批次未入队。");
                }
                return false;
            }
            return true;
        }

        /* RFC 3986 非保留字节直接保留，其余按 UTF-8 原始字节百分号编码（大写十六进制）。 */
        private static string UrlEncodeUtf8(string byteStr)
        {
            const string hex = "0123456789ABCDEF";
            var b = new StringBuilder(byteStr.Length * 3);
            for (int i = 0; i < byteStr.Length; i++) {
                char c = byteStr[i];
                if (Warmup.IsAlpha(c) || Warmup.IsDigit(c) || c == '-' || c == '_' || c == '.' || c == '~') {
                    b.Append(c);
                } else {
                    b.Append('%').Append(hex[c >> 4]).Append(hex[c & 15]);
                }
            }
            return b.ToString();
        }

        /* localhost_get_cached_translate：GET /translate?cache_only=true&text=…，只接受 200 且
           完整读取的响应体（纯文本译文，字节串）；其余一律视为未命中。 */
        public static bool GetCachedTranslate(string text, out string body)
        {
            body = null;
            if (Warmup.DumpSink != null) return false;
            try {
                var req = (HttpWebRequest)WebRequest.Create(Contract.DefaultBaseUrl + Contract.PathTranslate +
                                                             "?cache_only=true&text=" + UrlEncodeUtf8(text));
                req.Method = "GET";
                req.Timeout = 350;
                req.ReadWriteTimeout = 350;
                req.Proxy = null;
                req.UserAgent = "ds-game-translator Launcher/3.1";
                using (var resp = (HttpWebResponse)req.GetResponse()) {
                    if ((int)resp.StatusCode != 200) return false;
                    using (Stream s = resp.GetResponseStream())
                    using (var ms = new MemoryStream()) {
                        var buf = new byte[4096];
                        int n;
                        while ((n = s.Read(buf, 0, buf.Length)) > 0) ms.Write(buf, 0, n);
                        byte[] bytes = ms.ToArray();
                        body = ByteStr.FromBytes(bytes, 0, ByteStr.CStrLen(bytes));
                        return true;
                    }
                }
            } catch (WebException) {
                return false;
            } catch (IOException) {
                return false; /* 读取中途断开：截断内容不可用 */
            }
        }
    }
}
