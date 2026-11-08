using System;
using System.Text;

namespace DstLauncher
{
    /*
     * Warmup —— Ren'Py 脚本扫描（warmup.c 的 renpy_* / scan_renpy_script_dir 部分）。
     *
     * 按逻辑语句解析 game/ 下的 .rpy：词法层处理转义、独白及正文分类。
     * 同时维护 TextList.Prev 平行数组（同一脚本内
     * 上一条采集文本），让 /prefetch 请求携带对话语境。语境不跨文件延续。
     */
    public static partial class Warmup
    {
        /* 语句分类已排除字体/样式字段；保留短菜单与带字体标签的正文原键。 */
        private static bool ShouldWarmRenpyText(string s)
        {
            int len = s.Length;
            if (len < 2 || len > MaxTextBytes) return false;
            if (s.Contains("://") || (len >= 3 && s[1] == ':' && (s[2] == '\\' || s[2] == '/'))) return false;
            foreach (string extension in new[] { ".rpy", ".png", ".jpg", ".webp", ".ogg", ".mp3", ".wav" })
                if (s.Contains(extension)) return false;
            if (HasCjkUtf8(s)) return DstCore.MixedText.HasUntranslatedLatinRenPy(Encoding.UTF8.GetString(ByteStr.ToBytes(s)));
            int latin = 0;
            foreach (char ch in s) {
                if (ch >= 128) return true;
                if (IsAlpha(ch)) latin++;
            }
            return latin >= 2;
        }

        private static void CollectRenpyString(string s, TextList prefetch)
        {
            // 字面量已经过引擎词法处理；首尾显式空格仍属于缓存键。
            if (ShouldWarmRenpyText(s)) prefetch.Add(s);
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

        private static void ScanRenpyScript(byte[] buf, TextList prefetch)
        {
            ScanRenpySource(ByteStr.FromBytes(buf, 0, ByteStr.CStrLen(buf)), prefetch);
        }

        /* scan_renpy_script_dir：先扫本目录的 *.rpy，再递归子目录（深度上限 12）。
           本目录没有 .rpy 时仍进入子目录；跳过自带钩子和超过 8 MiB 的脚本。 */
        private static void ScanRenpyScriptDir(string dir, TextList prefetch, int depth)
        {
            if (depth > RenpyScanMaxDepth) return;
            if (!PathUtil.IsDir(dir)) return;
            // 根目录可以很短，章节子目录仍可能超过传统路径上限；枚举与读取同用文件边界。
            var files = Win32Find.EnumerateOrNull(dir, "*.rpy", true);
            if (files != null) foreach (var fd in files) {
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
            var entries = Win32Find.EnumerateOrNull(dir, "*", true);
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
            var prefetch = new TextList { MaxItems = RenpyMaxItems, RenPy = true };
            ScanRenpyScriptDir(PathUtil.Join(dir, "game"), prefetch, 0);
            if (prefetch.Full)
                Log.Append("预热扫描达到 " + prefetch.Limit + " 条上限，部分资源文本可能未预热。");
            int queued = PostPrefetchAll(prefetch);
            if (queued > 0) Log.Append("Ren'Py preheated translation cache: queued " + queued + " texts.");
        }
    }
}
