using System;
using System.Text;

namespace DstLauncher
{
    /*
     * Warmup —— Ren'Py 脚本扫描（warmup.c 的 renpy_* / scan_renpy_script_dir 部分）。
     *
     * 逐行解析 game/ 下的 .rpy：用 renpy_skip_statement 过滤非对话语句，收集同一
     * 已接受行内的每个字符串字面量；同时维护 TextList.Prev 平行数组（同一脚本内
     * 上一条采集文本），让 /prefetch 请求携带对话语境。语境不跨文件延续。
     */
    public static partial class Warmup
    {
        /* should_warm_renpy_text：通用过滤之上再排除过短、无标点的短标识符和资源名。 */
        private static bool ShouldWarmRenpyText(string s)
        {
            if (!ShouldWarmText(s)) return false;
            int len = s.Length;
            if (len < 3) return false;
            if (!ByteStr.ContainsAny(s, " \t.?!,:;'-\"") && len < 18) return false;
            if (s.Contains("://") || s.Contains(".rpy") || s.Contains(".png") ||
                s.Contains(".jpg") || s.Contains(".webp") || s.Contains(".ogg") ||
                s.Contains(".mp3") || s.Contains(".wav")) {
                return false;
            }
            return true;
        }

        private static void CollectRenpyString(string s, TextList prefetch)
        {
            string t = ByteStr.Trim(s);
            if (ShouldWarmRenpyText(t)) prefetch.Add(t);
        }

        /* renpy_string_at：解析单引号或双引号字面量（\n \r \t \uXXXX 及代理对），跳过
           三引号。未闭合（行尾/回车/换行前没有配对引号）时返回 null 并把游标停在
           断点；成功时游标停在闭合引号之后。也供将来的 Godot 扫描器复用。 */
        internal static string RenpyStringAt(string line, ref int pos)
        {
            int p = pos;
            char quote = ByteStr.At(line, p);
            if (quote != '"' && quote != '\'') return null;
            if (ByteStr.At(line, p + 1) == quote && ByteStr.At(line, p + 2) == quote) return null;
            p++;
            var b = new StringBuilder(64);
            for (;;) {
                char c = ByteStr.At(line, p);
                if (c == 0 || c == quote || c == '\r' || c == '\n') break;
                p++;
                if (c == '\\') {
                    char e = ByteStr.At(line, p);
                    if (e == 0) break;
                    p++;
                    if (e == 'n') b.Append('\n');
                    else if (e == 'r') b.Append('\r');
                    else if (e == 't') b.Append('\t');
                    else if (e == 'u') {
                        uint cp;
                        if (JsonU4(line, p, out cp)) {
                            p += 4;
                            /* 与 JsonStringAt 一致合并代理对，孤立代理记为 U+FFFD，避免 .rpy 中的
                               emoji 以 CESU-8 形式进入缓存键。 */
                            if (cp >= 0xd800 && cp <= 0xdbff) {
                                uint lo;
                                if (ByteStr.At(line, p) == '\\' && ByteStr.At(line, p + 1) == 'u' &&
                                    JsonU4(line, p + 2, out lo) && lo >= 0xdc00 && lo <= 0xdfff) {
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
                            b.Append('u');
                        }
                    } else {
                        b.Append(e);
                    }
                } else {
                    b.Append(c);
                }
            }
            if (ByteStr.At(line, p) != quote) {
                pos = p;
                return null;
            }
            p++;
            pos = p;
            return ByteStr.CStr(b);
        }

        /* renpy_first_quote：from 起最先出现的单引号或双引号下标，没有则 -1。 */
        private static int RenpyFirstQuote(string line, int from)
        {
            int single = line.IndexOf('\'', from);
            int dbl = line.IndexOf('"', from);
            if (single < 0) return dbl;
            if (dbl < 0) return single;
            return single < dbl ? single : dbl;
        }

        private static readonly string[] RenpySkipWords = {
            "image", "scene", "show", "hide", "play", "queue", "stop", "with", "jump", "call",
            "return", "label", "define", "default", "style", "transform", "screen", "init",
            "python", "if", "elif", "else", "for", "while"
        };

        /* renpy_skip_statement：注释、三引号、赋值（= 在首个引号之前）以及 Ren'Py 指令关键字
           开头的行都不是对话。注意 C 版不对行做 trim：缩进的对话行不会被关键字规则误判。 */
        private static bool RenpySkipStatement(string line, int firstQuote)
        {
            if (line.Length == 0 || line[0] == '#') return true;
            if (line.Contains("\"\"\"") || line.Contains("'''")) return true;
            int eq = line.IndexOf('=');
            if (eq >= 0 && firstQuote >= 0 && eq < firstQuote) return true;
            foreach (string w in RenpySkipWords) {
                if (StartsWithWordI(line, w)) return true;
            }
            return line[0] == '#';
        }

        /* collect_renpy_line_strings：收集一个已接受行内的全部字面量（最多 16 个）。 */
        private static void CollectRenpyLineStrings(string line, TextList prefetch)
        {
            if (line == null || prefetch == null || prefetch.Full) return;
            int cursor = RenpyFirstQuote(line, 0);
            if (cursor < 0 || RenpySkipStatement(line, cursor)) return;

            int seen = 0;
            while (ByteStr.At(line, cursor) != 0 && seen < 16 && !prefetch.Full) {
                string text = RenpyStringAt(line, ref cursor);
                if (text == null) break;
                CollectRenpyString(text, prefetch);
                seen++;

                int next = RenpyFirstQuote(line, cursor);
                if (next < 0) break;
                cursor = next;
            }
        }

        /* 单个脚本：按 '\n' 分行（首个 NUL 处结束，与 C 的 strchr/strlen 一致），维护 Prev。 */
        private static void ScanRenpyScript(byte[] buf, TextList prefetch)
        {
            int end = ByteStr.CStrLen(buf);
            string prevLast = null; /* 上一条采集文本（本文件内有效） */
            int pos = 0;
            while (pos < end && !prefetch.Full) {
                int nl = ByteStr.IndexOf(buf, (byte)'\n', pos, end);
                int lineEnd = nl < 0 ? end : nl;
                string line = ByteStr.FromBytes(buf, pos, lineEnd - pos);
                pos = nl < 0 ? end : nl + 1;

                int before = prefetch.Count;
                CollectRenpyLineStrings(line, prefetch);
                if (prefetch.Count > before) {
                    prefetch.EnsurePrev();
                    for (int k = before; k < prefetch.Count; k++) prefetch.SetPrev(k, prevLast);
                    prevLast = prefetch.Items[prefetch.Count - 1];
                }
            }
        }

        /* scan_renpy_script_dir：先扫本目录的 *.rpy，再递归子目录（深度上限 12）。
           与 C 版一致：FindFirstFileW(*.rpy) 失败（本目录没有 .rpy）时直接返回，不再
           进入子目录；启动器自带钩子 iron_deepseek.rpy 与超过 8 MiB 的脚本跳过。 */
        private static void ScanRenpyScriptDir(string dir, TextList prefetch, int depth)
        {
            if (depth > RenpyScanMaxDepth) return;
            if (!PathUtil.IsDir(dir)) return;
            var files = Win32Find.EnumerateOrNull(dir, "*.rpy");
            if (files == null) return;
            foreach (var fd in files) {
                if (prefetch.Full) return;
                if ((fd.Attributes & SafeFs.FILE_ATTRIBUTE_DIRECTORY) != 0) continue;
                if (!PathUtil.EndsWithNoCase(fd.Name, ".rpy")) continue;
                if (PathUtil.EqualsNoCase(fd.Name, "iron_deepseek.rpy")) continue;
                if (fd.Size > RenpyScriptScanMaxBytes) continue;
                string full = PathUtil.Join(dir, fd.Name);
                byte[] buf = SafeFs.ReadBytes(full);
                if (buf == null) continue;
                ScanRenpyScript(buf, prefetch);
            }
            var entries = Win32Find.EnumerateOrNull(dir, "*");
            if (entries == null) return;
            foreach (var fd in entries) {
                if (prefetch.Full) return;
                if ((fd.Attributes & SafeFs.FILE_ATTRIBUTE_DIRECTORY) != 0) {
                    ScanRenpyScriptDir(PathUtil.Join(dir, fd.Name), prefetch, depth + 1);
                }
            }
        }

        /* warmup_renpy：递归扫描 game/ 并批量提交。 */
        private static void WarmupRenPy(string dir)
        {
            var prefetch = new TextList { MaxItems = RenpyMaxItems };
            ScanRenpyScriptDir(PathUtil.Join(dir, "game"), prefetch, 0);
            int queued = PostPrefetchAll(prefetch);
            if (queued > 0) Log.Append("Ren'Py preheated translation cache: queued " + queued + " texts.");
        }
    }
}
