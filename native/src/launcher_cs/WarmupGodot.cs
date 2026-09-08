using System;
using System.IO;
using System.Text;

namespace DstLauncher
{
    /*
     * WarmupGodot —— godot_warmup.c 的逐函数移植（Godot 资源缓存预热扫描器）。
     *
     * 只负责只读文本发现：工程/导出目录中的 .tscn/.tres/.gd/.json/.csv/.po/.md/.txt、
     * project.godot、.translation、godot_project.binary、独立 .pck（格式 1/2/3）以及
     * EXE 内嵌 "pck" 区段。运行时修补与 sidecar 属于 godot_patch.c（尚未移植）。
     * 原始 .pck 与编译资源永远不会被改写。
     *
     * 字节串约定见 Warmup.cs 头部：文本扫描器的 string 每个 char 是一个字节，文件
     * 内容在首个 NUL 处截断（C 的 char* 语义）；二进制扫描器直接处理 byte[]。
     */
    public static partial class Warmup
    {
        public const uint GodotResourceScanMaxBytes = 64u * 1024u * 1024u;
        public const int GodotScanMaxDepth = 10;
        private const uint GodotPckMagic = 0x43504447u; /* "GDPC" 小端 */
        private const uint GodotPckV1HeaderSize = 88u;
        private const uint GodotPckV2HeaderSize = 100u;
        private const uint GodotPckV3HeaderSize = 112u;
        private const uint GodotPckMaxFiles = 200000u;
        private const uint GodotPckMaxPathBytes = 4096u;

        private struct GodotPckInfo
        {
            public uint Format;
            public uint HeaderSize;
            public uint EntryMetaSize;
            public ulong FileBase;
            public ulong EntriesOffset;
            public uint FileCount;
            public bool OffsetsAreAbsolute;
        }

        private const string GodotPunct = " \t.?!,:;'-\"";

        /* should_warm_godot_text：Godot 资源混有玩家文本、路径、节点元数据、资源 ID 与
           工程设置；保留常见的玩家可见文本，拒绝只会污染共享缓存的字符串。 */
        private static bool ShouldWarmGodotText(string s)
        {
            if (!ShouldWarmText(s)) return false;
            int len = s.Length;
            if (!ByteStr.ContainsAny(s, GodotPunct)) {
                if (len < 3 || len > 32) return false;
                for (int i = 0; i < len; i++) {
                    char c = s[i];
                    if (c < 0x80 && !(IsAlpha(c) || IsDigit(c))) return false;
                }
            }
            if (s.Contains("res://") || s.Contains("user://") || s.Contains("uid://")) return false;
            if (s.Contains(".tscn") || s.Contains(".tres") || s.Contains(".gd") ||
                s.Contains(".import") || s.Contains(".shader") || s.Contains(".material") ||
                s.Contains(".png") || s.Contains(".jpg") || s.Contains(".webp") ||
                s.Contains(".svg") || s.Contains(".ogg") || s.Contains(".wav") ||
                s.Contains(".mp3") || s.Contains(".pck")) {
                return false;
            }
            if (s.Contains("ExtResource") || s.Contains("SubResource") ||
                s.Contains("NodePath") || s.Contains("PackedScene") ||
                s.Contains("ResourceUID") || s.Contains("ProjectSettings")) {
                return false;
            }
            if (s == "RSRC" || s == "OptimizedTranslation" ||
                s == "messages" || s == "locale" ||
                s == "strings" || s == "resource_name") {
                return false;
            }
            char first = ByteStr.At(s, 0);
            if (first == '_' || first == '@' || first == '{' ||
                StartsWithWordI(s, "script") || StartsWithWordI(s, "resource") ||
                StartsWithWordI(s, "node") || StartsWithWordI(s, "signal")) {
                return false;
            }
            return true;
        }

        /* collect_godot_string：定向字符串（明确承载翻译的属性或调用），允许 "Start" 这类短标签。 */
        private static void CollectGodotString(string s, TextList prefetch)
        {
            string t = ByteStr.Trim(s);
            if (ShouldWarmGodotText(t)) prefetch.Add(t);
        }

        /* collect_godot_free_string：松散字符串需要更强的文本信号（标点或 ≥ 18 字节）。 */
        private static void CollectGodotFreeString(string s, TextList prefetch)
        {
            string t = ByteStr.Trim(s);
            if (!ShouldWarmGodotText(t)) return;
            if (ByteStr.ContainsAny(t, GodotPunct) || t.Length >= 18) prefetch.Add(t);
        }

        private static bool AlphaWordLenAtLeast(string s, int minLen)
        {
            int n = 0;
            for (int i = 0; i < s.Length; i++) {
                if (!IsAlpha(s[i])) return false;
                n++;
            }
            return n >= minLen;
        }

        /* collect_godot_binary_string：NUL 分隔的二进制载荷；允许 "Start" 这类紧凑菜单词，
           拒绝 "bin" 之类的微小噪声与含控制字符的片段。 */
        private static void CollectGodotBinaryString(string s, TextList prefetch)
        {
            string t = ByteStr.Trim(s);
            if (!ShouldWarmGodotText(t)) return;
            if (t.IndexOf('\r') >= 0 || t.IndexOf('\n') >= 0 || t.IndexOf('\t') >= 0) return;
            if (t.Length >= 5 || AlphaWordLenAtLeast(t, 4)) prefetch.Add(t);
        }

        private static bool IsIdentChar(char c) { return IsAlpha(c) || IsDigit(c) || c == '_'; }

        /* godot_key_equals：去掉两端空格/制表后与 name 做 ASCII 不区分大小写的全等比较。 */
        private static bool GodotKeyEquals(string s, int start, int end, string name)
        {
            while (start < end && (s[start] == ' ' || s[start] == '\t')) start++;
            while (end > start && (s[end - 1] == ' ' || s[end - 1] == '\t')) end--;
            int len = end - start;
            return name.Length == len && ByteStr.RegionEqualsNoCase(s, start, name, len);
        }

        /* 引号所在行的起点（跳过前导空格/制表）。 */
        private static int GodotLineStart(string s, int quote)
        {
            int line = quote;
            while (line > 0 && s[line - 1] != '\n' && s[line - 1] != '\r') line--;
            while (ByteStr.At(s, line) == ' ' || ByteStr.At(s, line) == '\t') line++;
            return line;
        }

        /* 引号前最近的 '='（同一行内）左侧的标识符区间；没有 '=' 返回 false。 */
        private static bool GodotKeyBeforeEquals(string s, int line, int quote, out int keyStart, out int keyEnd)
        {
            keyStart = keyEnd = 0;
            int eq = quote;
            while (eq > line && s[eq - 1] != '\n' && s[eq - 1] != '\r' && s[eq - 1] != '=') eq--;
            if (eq <= line || s[eq - 1] != '=') return false;
            keyEnd = eq - 1;
            while (keyEnd > line && (s[keyEnd - 1] == ' ' || s[keyEnd - 1] == '\t')) keyEnd--;
            keyStart = keyEnd;
            while (keyStart > line && IsIdentChar(s[keyStart - 1])) keyStart--;
            return true;
        }

        /* godot_metadata_quote：[node name="..."] 之类的场景/资源头与 name/type/path 等
           元数据键的引号内容不含玩家正文，跳过。 */
        private static bool GodotMetadataQuote(string s, int quote)
        {
            int line = GodotLineStart(s, quote);
            if (ByteStr.At(s, line) == '[') return true;
            int keyStart, keyEnd;
            if (!GodotKeyBeforeEquals(s, line, quote, out keyStart, out keyEnd)) return false;
            return GodotKeyEquals(s, keyStart, keyEnd, "name") ||
                   GodotKeyEquals(s, keyStart, keyEnd, "type") ||
                   GodotKeyEquals(s, keyStart, keyEnd, "parent") ||
                   GodotKeyEquals(s, keyStart, keyEnd, "instance") ||
                   GodotKeyEquals(s, keyStart, keyEnd, "script") ||
                   GodotKeyEquals(s, keyStart, keyEnd, "path") ||
                   GodotKeyEquals(s, keyStart, keyEnd, "resource_path") ||
                   GodotKeyEquals(s, keyStart, keyEnd, "uid") ||
                   GodotKeyEquals(s, keyStart, keyEnd, "id");
        }

        private static bool GodotTranslatablePropertyKey(string s, int start, int end)
        {
            return GodotKeyEquals(s, start, end, "text") ||
                   GodotKeyEquals(s, start, end, "bbcode_text") ||
                   GodotKeyEquals(s, start, end, "placeholder_text") ||
                   GodotKeyEquals(s, start, end, "tooltip_text") ||
                   GodotKeyEquals(s, start, end, "hint_tooltip") ||
                   GodotKeyEquals(s, start, end, "dialog_text") ||
                   GodotKeyEquals(s, start, end, "title") ||
                   GodotKeyEquals(s, start, end, "window_title") ||
                   GodotKeyEquals(s, start, end, "message") ||
                   GodotKeyEquals(s, start, end, "caption") ||
                   GodotKeyEquals(s, start, end, "description") ||
                   GodotKeyEquals(s, start, end, "display_text");
        }

        /* godot_call_name_before_quote：引号紧跟在 name( 或 x.name( 之后。 */
        private static bool GodotCallNameBeforeQuote(string s, int line, int quote, string name)
        {
            int p = quote;
            while (p > line && (s[p - 1] == ' ' || s[p - 1] == '\t')) p--;
            if (p <= line || s[p - 1] != '(') return false;
            p--;
            while (p > line && (s[p - 1] == ' ' || s[p - 1] == '\t')) p--;
            int end = p;
            while (p > line) {
                char c = s[p - 1];
                if (!(IsIdentChar(c) || c == '.')) break;
                p--;
            }
            int len = end - p;
            int want = name.Length;
            return len >= want && ByteStr.RegionEqualsNoCase(s, end - want, name, want) &&
                   (len == want || s[end - want - 1] == '.');
        }

        /* godot_translatable_quote：定向上下文（可翻译属性键或 tr()/translate()/N_()/RTR() 调用）。 */
        private static bool GodotTranslatableQuote(string s, int quote)
        {
            int line = GodotLineStart(s, quote);
            if (ByteStr.At(s, line) == '[') return false;
            int keyStart, keyEnd;
            if (GodotKeyBeforeEquals(s, line, quote, out keyStart, out keyEnd) &&
                GodotTranslatablePropertyKey(s, keyStart, keyEnd)) {
                return true;
            }
            return GodotCallNameBeforeQuote(s, line, quote, "tr") ||
                   GodotCallNameBeforeQuote(s, line, quote, "translate") ||
                   GodotCallNameBeforeQuote(s, line, quote, "N_") ||
                   GodotCallNameBeforeQuote(s, line, quote, "RTR");
        }

        /* valid_utf8_text_payload：只接受制表/换行/回车、可打印 ASCII 与合法（非过长编码、
           非代理区、≤ U+10FFFF）的多字节 UTF-8；至少含一个字母或一个多字节字符。 */
        private static bool ValidUtf8TextPayload(byte[] p, int offset, int n)
        {
            bool signal = false;
            for (int i = 0; i < n;) {
                byte c = p[offset + i];
                if (c == 9 || c == 10 || c == 13) {
                    i++;
                    continue;
                }
                if (c >= 32 && c <= 126) {
                    if (IsAlpha((char)c)) signal = true;
                    i++;
                    continue;
                }
                uint cp;
                int need;
                if ((c & 0xe0) == 0xc0) {
                    cp = (uint)(c & 0x1f);
                    need = 2;
                } else if ((c & 0xf0) == 0xe0) {
                    cp = (uint)(c & 0x0f);
                    need = 3;
                } else if ((c & 0xf8) == 0xf0) {
                    cp = (uint)(c & 0x07);
                    need = 4;
                } else {
                    return false;
                }
                if (i + need > n) return false;
                for (int j = 1; j < need; j++) {
                    byte cc = p[offset + i + j];
                    if ((cc & 0xc0) != 0x80) return false;
                    cp = (cp << 6) | (uint)(cc & 0x3f);
                }
                if ((need == 2 && cp < 0x80) ||
                    (need == 3 && cp < 0x800) ||
                    (need == 4 && (cp < 0x10000 || cp > 0x10ffff)) ||
                    (cp >= 0xd800 && cp <= 0xdfff)) {
                    return false;
                }
                signal = true;
                i += need;
            }
            return signal;
        }

        private static bool GodotTextFileName(string name)
        {
            return PathUtil.EndsWithNoCase(name, ".tscn") ||
                   PathUtil.EndsWithNoCase(name, ".tres") ||
                   PathUtil.EndsWithNoCase(name, ".gd") ||
                   PathUtil.EndsWithNoCase(name, ".json") ||
                   PathUtil.EndsWithNoCase(name, ".csv") ||
                   PathUtil.EndsWithNoCase(name, ".po") ||
                   PathUtil.EndsWithNoCase(name, ".md") ||
                   PathUtil.EndsWithNoCase(name, ".txt") ||
                   PathUtil.EqualsNoCase(name, "project.godot");
        }

        private static bool GodotBinaryFileName(string name)
        {
            return PathUtil.EndsWithNoCase(name, ".pck") ||
                   PathUtil.EndsWithNoCase(name, ".translation") ||
                   PathUtil.EqualsNoCase(name, "godot_project.binary");
        }

        /* ascii_ends_with_i：字节串路径的 ASCII 不区分大小写后缀比较。 */
        private static bool AsciiEndsWithI(string s, string suffix)
        {
            return s.Length >= suffix.Length && ByteStr.RegionEqualsNoCase(s, s.Length - suffix.Length, suffix, suffix.Length);
        }

        private static bool GodotPackTextPath(string path)
        {
            return AsciiEndsWithI(path, ".tscn") ||
                   AsciiEndsWithI(path, ".tres") ||
                   AsciiEndsWithI(path, ".gd") ||
                   AsciiEndsWithI(path, ".json") ||
                   AsciiEndsWithI(path, ".csv") ||
                   AsciiEndsWithI(path, ".po") ||
                   AsciiEndsWithI(path, ".md") ||
                   AsciiEndsWithI(path, ".txt") ||
                   ByteStr.EqualsNoCase(path, "project.godot");
        }

        private static bool AsciiLocaleChar(char c) { return IsAlpha(c) || IsDigit(c) || c == '_' || c == '-'; }

        /* godot_locale_token：2..8 字节，前两个为字母，其余为字母/数字/_/-。 */
        private static bool GodotLocaleToken(string s, int start, int n)
        {
            if (n < 2 || n > 8) return false;
            if (!IsAlpha(s[start]) || !IsAlpha(s[start + 1])) return false;
            for (int i = 2; i < n; i++) {
                if (!AsciiLocaleChar(s[start + i])) return false;
            }
            return true;
        }

        /* godot_pack_translation_path：*.translation；带地域后缀（name.<locale>.translation）
           时只接受 en / en_* / en-*，其余地域视为已有译文跳过。 */
        private static bool GodotPackTranslationPath(string path)
        {
            const string suffix = ".translation";
            int len = path.Length;
            int suffixLen = suffix.Length;
            if (len < suffixLen || !ByteStr.RegionEqualsNoCase(path, len - suffixLen, suffix, suffixLen)) return false;

            int end = len - suffixLen;
            int token = end;
            while (token > 0 && path[token - 1] != '.' && path[token - 1] != '/' && path[token - 1] != '\\') token--;
            if (token > 0 && path[token - 1] == '.') {
                int tokenLen = end - token;
                if (GodotLocaleToken(path, token, tokenLen)) {
                    return ByteStr.RegionEqualsNoCase(path, token, "en", 2) &&
                           (tokenLen == 2 || path[token + 2] == '_' || path[token + 2] == '-');
                }
            }
            return true;
        }

        private static bool GodotPackBinaryPath(string path)
        {
            return GodotPackTranslationPath(path) ||
                   AsciiEndsWithI(path, ".scn") ||
                   AsciiEndsWithI(path, ".res") ||
                   AsciiEndsWithI(path, ".gdc") ||
                   AsciiEndsWithI(path, ".gde") ||
                   ByteStr.EqualsNoCase(path, "godot_project.binary");
        }

        /* godot_pack_deferred_binary_path：编译资源放到第二遍，避免庞大的 .godot/exported 树先耗尽上限。 */
        private static bool GodotPackDeferredBinaryPath(string path)
        {
            return AsciiEndsWithI(path, ".scn") ||
                   AsciiEndsWithI(path, ".res") ||
                   AsciiEndsWithI(path, ".gdc") ||
                   AsciiEndsWithI(path, ".gde");
        }

        /* skip_godot_scan_directory：生成物/缓存/插件/存档目录。 */
        private static bool SkipGodotScanDirectory(string name)
        {
            return PathUtil.EqualsNoCase(name, ".godot") ||
                   PathUtil.EqualsNoCase(name, ".import") ||
                   PathUtil.EqualsNoCase(name, "addons") ||
                   PathUtil.EqualsNoCase(name, "export_presets") ||
                   PathUtil.EqualsNoCase(name, "saves") ||
                   PathUtil.EqualsNoCase(name, "save");
        }

        private static bool SkipGodotGeneratedFile(string name)
        {
            return PathUtil.EqualsNoCase(name, "dst_godot_runtime.gd") ||
                   PathUtil.EqualsNoCase(name, "dst_godot_patch.pck") ||
                   PathUtil.EqualsNoCase(name, "dst_godot_patch.next.pck") ||
                   PathUtil.EqualsNoCase(name, "dst_godot_patch.building");
        }

        /* scan_godot_quoted_strings：逐个引号分类——定向上下文宽松采集，元数据上下文跳过，
           其余交给松散字符串收集器；字面量解析复用 renpy_string_at。 */
        private static void ScanGodotQuotedStrings(string s, TextList prefetch)
        {
            int p = 0;
            while (ByteStr.At(s, p) != 0 && !prefetch.Full) {
                char c = s[p];
                if (c != '"' && c != '\'') {
                    p++;
                    continue;
                }
                bool targeted = GodotTranslatableQuote(s, p);
                if (!targeted && GodotMetadataQuote(s, p)) {
                    p++;
                    continue;
                }
                int cursor = p;
                string text = RenpyStringAt(s, ref cursor);
                if (text != null) {
                    if (targeted) CollectGodotString(text, prefetch);
                    else CollectGodotFreeString(text, prefetch);
                    p = cursor;
                } else {
                    p++;
                }
            }
        }

        /* C 版各行扫描器共用的循环：跳过 BOM，按 \r、\n、\r\n 或 NUL 切行，每行去首尾空白后
           交给 onLine；NUL 或列表满时停止。 */
        private static void ForEachGodotLine(string s, TextList prefetch, Action<string> onLine)
        {
            int line = 0;
            if (ByteStr.At(s, 0) == 0xef && ByteStr.At(s, 1) == 0xbb && ByteStr.At(s, 2) == 0xbf) line = 3;
            for (int p = line; ; p++) {
                char c = ByteStr.At(s, p);
                if (c != '\r' && c != '\n' && c != 0) continue;
                onLine(ByteStr.Trim(s.Substring(line, p - line)));
                if (c == 0 || prefetch.Full) break;
                if (c == '\r' && ByteStr.At(s, p + 1) == '\n') p++;
                line = p + 1;
            }
        }

        /* scan_godot_lines：手写文本资源的纯文本行回退路径——忽略赋值、注释、PO 指令、
           节头以及带引号（上面已处理）的行。 */
        private static void ScanGodotLines(string s, TextList prefetch)
        {
            ForEachGodotLine(s, prefetch, text => {
                if (text.Length != 0 && text[0] != '#' && text[0] != ';' && text[0] != '[' &&
                    text.IndexOf('=') < 0 && text.IndexOf('"') < 0 && text.IndexOf('\'') < 0 &&
                    !text.StartsWith("msgid", StringComparison.Ordinal) &&
                    !text.StartsWith("msgstr", StringComparison.Ordinal) &&
                    !text.StartsWith("msgctx", StringComparison.Ordinal) /* C 版 strncmp(…, "msgctxt", 6) */) {
                    CollectGodotFreeString(text, prefetch);
                }
            });
        }

        /* godot_bbcode_tag_span：s[i0] 起的 [tag] / [/tag] 长度（≤ 160 字节，不跨行、不嵌套），否则 0。 */
        private static int GodotBbcodeTagSpan(string s, int i0)
        {
            if (ByteStr.At(s, i0) != '[') return 0;
            int i = 1;
            if (ByteStr.At(s, i0 + i) == '/') i++;
            if (!IsAlpha(ByteStr.At(s, i0 + i))) return 0;
            for (; ByteStr.At(s, i0 + i) != 0 && i < 160; i++) {
                char c = s[i0 + i];
                if (c == '\r' || c == '\n' || c == '[') return 0;
                if (c == ']') return i + 1;
            }
            return 0;
        }

        /* godot_strip_bbcode：剥离行内 BBCode 标签，返回可见文本。 */
        private static string GodotStripBbcode(string s)
        {
            var b = new StringBuilder(s.Length + 1);
            for (int p = 0; p < s.Length;) {
                int tag = GodotBbcodeTagSpan(s, p);
                if (tag > 0) {
                    p += tag;
                    continue;
                }
                b.Append(s[p++]);
            }
            return b.ToString();
        }

        /* scan_godot_markdown_strings：Markdown 剧本——跳过围栏代码、> 引用行与分隔线；
           # 标题与 BBCode 包裹行按可见文本采集，其余行整行采集。 */
        private static void ScanGodotMarkdownStrings(string s, TextList prefetch)
        {
            bool fenced = false;
            ForEachGodotLine(s, prefetch, text => {
                int len = text.Length;
                if (text.StartsWith("```", StringComparison.Ordinal) || text.StartsWith("~~~", StringComparison.Ordinal)) {
                    fenced = !fenced;
                } else if (!fenced && len != 0 && text[0] != '>' &&
                           text != "---" && text != "***" && text != "___") {
                    if (text[0] == '#') {
                        int h = 0;
                        while (ByteStr.At(text, h) == '#') h++;
                        string heading = ByteStr.Trim(text.Substring(h));
                        if (heading.Length != 0) CollectGodotString(heading, prefetch);
                    } else if (text[0] == '[' && len > 1 && text[len - 1] == ']') {
                        string v = ByteStr.Trim(GodotStripBbcode(text));
                        if (v.Length != 0) CollectGodotString(v, prefetch);
                    } else {
                        CollectGodotString(text, prefetch);
                    }
                }
            });
        }

        /* godot_po_flush：current 非 null（至少成功追加过一次，可为空串）且处于 msgid 时采集。 */
        private static void GodotPoFlush(ref StringBuilder current, ref bool active, TextList prefetch)
        {
            if (active && current != null) CollectGodotString(current.ToString(), prefetch);
            current = null;
            active = false;
        }

        private static void GodotPoAppendQuoted(string line, ref StringBuilder current)
        {
            int q = line.IndexOf('"');
            if (q < 0) return;
            string s = RenpyStringAt(line, ref q);
            if (s == null) return;
            if (current == null) current = new StringBuilder(s.Length + 1);
            current.Append(s);
        }

        /* scan_godot_po_strings：源串在 msgid/msgid_plural（含续行）；msgstr 是已有译文不排队。 */
        private static void ScanGodotPoStrings(string s, TextList prefetch)
        {
            StringBuilder current = null;
            bool inMsgid = false;
            ForEachGodotLine(s, prefetch, text => {
                if (text.StartsWith("msgid", StringComparison.Ordinal) &&
                    (ByteStr.At(text, 5) == ' ' || ByteStr.At(text, 5) == '\t' || ByteStr.At(text, 5) == '_' || ByteStr.At(text, 5) == 0)) {
                    GodotPoFlush(ref current, ref inMsgid, prefetch);
                    inMsgid = true;
                    GodotPoAppendQuoted(text, ref current);
                } else if (text.StartsWith("msgstr", StringComparison.Ordinal) || text.StartsWith("msgctx", StringComparison.Ordinal)) {
                    GodotPoFlush(ref current, ref inMsgid, prefetch);
                } else if (inMsgid && ByteStr.At(text, 0) == '"') {
                    GodotPoAppendQuoted(text, ref current);
                } else if (text.Length != 0 && text[0] != '#') {
                    GodotPoFlush(ref current, ref inMsgid, prefetch);
                }
            });
            GodotPoFlush(ref current, ref inMsgid, prefetch);
        }

        /* godot_csv_next_cell：带引号单元格（"" 转义）或到逗号/行尾的裸单元格；消费其后的逗号。 */
        private static string GodotCsvNextCell(string s, ref int pp)
        {
            int p = pp;
            var b = new StringBuilder(64);
            if (ByteStr.At(s, p) == '"') {
                p++;
                while (ByteStr.At(s, p) != 0) {
                    if (s[p] == '"' && ByteStr.At(s, p + 1) == '"') {
                        b.Append('"');
                        p += 2;
                        continue;
                    }
                    if (s[p] == '"') {
                        p++;
                        break;
                    }
                    b.Append(s[p++]);
                }
                while (ByteStr.At(s, p) == ' ' || ByteStr.At(s, p) == '\t') p++;
                if (ByteStr.At(s, p) == ',') p++;
            } else {
                while (ByteStr.At(s, p) != 0 && s[p] != ',' && s[p] != '\r' && s[p] != '\n') b.Append(s[p++]);
                if (ByteStr.At(s, p) == ',') p++;
            }
            pp = p;
            return b.ToString();
        }

        private static int GodotCsvSourceColumnScore(string cell)
        {
            string t = ByteStr.Trim(cell);
            if (t.Length == 0) return 0;
            if (ByteStr.EqualsNoCase(t, "en") ||
                ByteStr.EqualsNoCase(t, "en_us") ||
                ByteStr.EqualsNoCase(t, "en-us") ||
                (ByteStr.StartsWithNoCase(t, "en_") && GodotLocaleToken(t, 0, t.Length)) ||
                (ByteStr.StartsWithNoCase(t, "en-") && GodotLocaleToken(t, 0, t.Length))) {
                return 100;
            }
            if (ByteStr.EqualsNoCase(t, "source") ||
                ByteStr.EqualsNoCase(t, "source_text") ||
                ByteStr.EqualsNoCase(t, "original") ||
                ByteStr.EqualsNoCase(t, "english") ||
                ByteStr.EqualsNoCase(t, "msgid")) {
                return 90;
            }
            if (ByteStr.EqualsNoCase(t, "text") ||
                ByteStr.EqualsNoCase(t, "display_text") ||
                ByteStr.EqualsNoCase(t, "body")) {
                return 80;
            }
            return 0;
        }

        private static int GodotCsvSourceColumn(string header)
        {
            int cellp = 0;
            int bestCol = -1;
            int bestScore = 0;
            for (int col = 0; ByteStr.At(header, cellp) != 0; col++) {
                string cell = GodotCsvNextCell(header, ref cellp);
                int score = GodotCsvSourceColumnScore(cell);
                if (score > bestScore) {
                    bestScore = score;
                    bestCol = col;
                }
                while (ByteStr.At(header, cellp) == ' ' || ByteStr.At(header, cellp) == '\t') cellp++;
            }
            return bestCol;
        }

        /* collect_godot_csv_source_cell：已识别源列时只采该列（行列不足则什么都不采），返回 true；
           未识别源列返回 false 交给回退路径。 */
        private static bool CollectGodotCsvSourceCell(string line, int sourceCol, TextList prefetch)
        {
            if (sourceCol < 0) return false;
            int cellp = 0;
            for (int col = 0; ByteStr.At(line, cellp) != 0; col++) {
                string cell = GodotCsvNextCell(line, ref cellp);
                if (col == sourceCol) {
                    CollectGodotString(cell, prefetch);
                    return true;
                }
                while (ByteStr.At(line, cellp) == ' ' || ByteStr.At(line, cellp) == '\t') cellp++;
            }
            return true;
        }

        /* godot_csv_short_id：无空格无标点的短标识符（key/ID 列形态）。 */
        private static bool GodotCsvShortId(string s)
        {
            int len = s.Length;
            if (len == 0 || len > 32) return false;
            for (int i = 0; i < len; i++) {
                if (!IsIdentChar(s[i])) return false;
            }
            return true;
        }

        /* scan_godot_csv_strings：表头选源语言/英语列；未知布局回退为首个非短 ID 的自然文本单元格。 */
        private static void ScanGodotCsvStrings(string s, TextList prefetch)
        {
            int row = 0;
            int sourceCol = -1;
            ForEachGodotLine(s, prefetch, text => {
                if (row == 0) {
                    sourceCol = GodotCsvSourceColumn(text);
                } else if (text.Length != 0 && text[0] != '#') {
                    int before = prefetch.Count;
                    if (!CollectGodotCsvSourceCell(text, sourceCol, prefetch)) {
                        int cellp = 0;
                        while (ByteStr.At(text, cellp) != 0 && prefetch.Count == before) {
                            string cell = GodotCsvNextCell(text, ref cellp);
                            if (!GodotCsvShortId(ByteStr.Trim(cell))) CollectGodotString(cell, prefetch);
                            while (ByteStr.At(text, cellp) == ' ' || ByteStr.At(text, cellp) == '\t') cellp++;
                        }
                    }
                }
                row++;
            });
        }

        /* collect_godot_binary_payload：运行时按可见片段逐个查询以保留 BBCode，预热必须用同样
           的缓存键——含标签的载荷拆成标签之间的片段分别采集。 */
        private static void CollectGodotBinaryPayload(string s, TextList prefetch)
        {
            bool hasTag = false;
            for (int p = 0; p < s.Length; p++) {
                if (GodotBbcodeTagSpan(s, p) > 0) {
                    hasTag = true;
                    break;
                }
            }
            if (!hasTag) {
                CollectGodotBinaryString(s, prefetch);
                return;
            }

            int plain = 0;
            int q = 0;
            while (ByteStr.At(s, q) != 0 && !prefetch.Full) {
                int tagLen = GodotBbcodeTagSpan(s, q);
                if (tagLen == 0) {
                    q++;
                    continue;
                }
                if (q > plain) CollectGodotBinaryString(s.Substring(plain, q - plain), prefetch);
                q += tagLen;
                plain = q;
            }
            if (q > plain || ByteStr.At(s, plain) != 0) CollectGodotBinaryString(s.Substring(plain), prefetch);
        }

        /* scan_godot_binary_buffer：以非文本字节为界切片，3..1200 字节且通过 UTF-8 校验的片段进入采集。 */
        private static void ScanGodotBinaryBuffer(byte[] bytes, int size, TextList prefetch)
        {
            int start = 0;
            for (int i = 0; i <= size && !prefetch.Full; i++) {
                bool textual = false;
                if (i < size) {
                    byte c = bytes[i];
                    textual = (c >= 32 && c <= 126) || c == 9 || c == 10 || c == 13 || c >= 0x80;
                }
                if (textual) continue;
                int n = i - start;
                if (n >= 3 && n <= MaxTextBytes && ValidUtf8TextPayload(bytes, start, n)) {
                    CollectGodotBinaryPayload(ByteStr.FromBytes(bytes, start, n), prefetch);
                }
                start = i + 1;
            }
        }

        private static uint ReadU32Le(byte[] p, int off)
        {
            return (uint)p[off] | ((uint)p[off + 1] << 8) | ((uint)p[off + 2] << 16) | ((uint)p[off + 3] << 24);
        }

        private static ulong ReadU64Le(byte[] p, int off)
        {
            ulong lo = ReadU32Le(p, off);
            ulong hi = ReadU32Le(p, off + 4);
            return lo | (hi << 32);
        }

        /* CreateFileW(GENERIC_READ, FILE_SHARE_READ|WRITE|DELETE)：C 版打不开即返回 0，不记日志。 */
        private static FileStream OpenGodotSharedRead(string path)
        {
            try {
                return new FileStream(path, FileMode.Open, FileAccess.Read, FileShare.ReadWrite | FileShare.Delete);
            } catch (IOException) {
                return null;
            } catch (UnauthorizedAccessException) {
                return null;
            }
        }

        /* read_at_exact：定位后读满 size 字节；定位失败、提前到 EOF 或 I/O 错误都为失败。 */
        private static bool ReadAtExact(FileStream fs, long offset, byte[] buf, int size)
        {
            if (offset < 0) return false;
            try {
                fs.Seek(offset, SeekOrigin.Begin);
                int done = 0;
                while (done < size) {
                    int got = fs.Read(buf, done, size - done);
                    if (got <= 0) return false;
                    done += got;
                }
                return true;
            } catch (IOException) {
                return false; /* ReadFile/SetFilePointerEx 失败 */
            }
        }

        /* read_godot_pck_info：识别 PCK 格式 1（Godot 3，绝对偏移）、2（Godot 4，file_base 相对）、
           3（目录表在 directory_offset）；文件数 > 200000 或 file_base 越界视为无效。 */
        private static bool ReadGodotPckInfo(FileStream fs, long pckBase, ulong pckSize, out GodotPckInfo info)
        {
            info = new GodotPckInfo();
            if (pckSize < GodotPckV1HeaderSize) return false;
            uint want = pckSize >= GodotPckV3HeaderSize
                ? GodotPckV3HeaderSize
                : (pckSize >= GodotPckV2HeaderSize ? GodotPckV2HeaderSize : GodotPckV1HeaderSize);
            var header = new byte[GodotPckV3HeaderSize];
            if (!ReadAtExact(fs, pckBase, header, (int)want)) return false;
            if (ReadU32Le(header, 0) != GodotPckMagic) return false;

            uint format = ReadU32Le(header, 4);
            if (format == 1) {
                info.Format = format;
                info.HeaderSize = GodotPckV1HeaderSize;
                info.EntryMetaSize = 8u + 8u + 16u;
                info.FileBase = 0;
                info.EntriesOffset = info.HeaderSize;
                info.FileCount = ReadU32Le(header, 84);
                info.OffsetsAreAbsolute = true;
            } else if (format == 2) {
                if (pckSize < GodotPckV2HeaderSize || want < GodotPckV2HeaderSize) return false;
                info.Format = format;
                info.HeaderSize = GodotPckV2HeaderSize;
                info.EntryMetaSize = 8u + 8u + 16u + 4u;
                info.FileBase = ReadU64Le(header, 24);
                info.EntriesOffset = info.HeaderSize;
                info.FileCount = ReadU32Le(header, 96);
                info.OffsetsAreAbsolute = false;
            } else if (format == 3) {
                if (pckSize < GodotPckV3HeaderSize || want < GodotPckV3HeaderSize) return false;
                ulong directoryOffset = ReadU64Le(header, 32);
                if (directoryOffset > pckSize - 4u || directoryOffset > (ulong)long.MaxValue) return false;
                var count = new byte[4];
                if (pckBase > long.MaxValue - (long)directoryOffset ||
                    !ReadAtExact(fs, pckBase + (long)directoryOffset, count, 4)) {
                    return false;
                }
                info.Format = format;
                info.HeaderSize = GodotPckV3HeaderSize;
                info.EntryMetaSize = 8u + 8u + 16u + 4u;
                info.FileBase = ReadU64Le(header, 24);
                info.EntriesOffset = directoryOffset + 4u;
                info.FileCount = ReadU32Le(count, 0);
                info.OffsetsAreAbsolute = false;
            } else {
                return false;
            }
            if (info.FileCount > GodotPckMaxFiles || info.FileBase > pckSize) return false;
            return true;
        }

        /* trim_pack_path：Godot 用 NUL 把路径填充到 4 字节对齐，首个 NUL 处截断。 */
        private static string TrimPackPath(byte[] path, int len)
        {
            int nul = Array.IndexOf(path, (byte)0, 0, len);
            return ByteStr.FromBytes(path, 0, nul < 0 ? len : nul);
        }

        /* scan_godot_pack_entry：按包内路径分派——.po/.csv/.md/其余文本走文本扫描器，
           .translation/.scn/.res/.gdc/.gde/godot_project.binary 走二进制切片。 */
        private static void ScanGodotPackEntry(FileStream fs, long dataAbs, ulong size, string path, TextList prefetch)
        {
            if (path.Length == 0 || size > GodotResourceScanMaxBytes || size > uint.MaxValue) return;
            if (!GodotPackTextPath(path) && !GodotPackBinaryPath(path)) return;

            var buf = new byte[(int)size];
            if (!ReadAtExact(fs, dataAbs, buf, (int)size)) return;

            if (AsciiEndsWithI(path, ".po")) {
                ScanGodotPoStrings(CStringOf(buf), prefetch);
            } else if (AsciiEndsWithI(path, ".csv")) {
                ScanGodotCsvStrings(CStringOf(buf), prefetch);
            } else if (AsciiEndsWithI(path, ".md")) {
                ScanGodotMarkdownStrings(CStringOf(buf), prefetch);
            } else if (GodotPackTextPath(path)) {
                string s = CStringOf(buf);
                ScanGodotQuotedStrings(s, prefetch);
                ScanGodotLines(s, prefetch);
            } else {
                ScanGodotBinaryBuffer(buf, (int)size, prefetch);
            }
        }

        /* read_file_bytes 之后当 C 字符串用：首个 NUL 处截断的字节串。 */
        private static string CStringOf(byte[] buf)
        {
            return ByteStr.FromBytes(buf, 0, ByteStr.CStrLen(buf));
        }

        /* scan_godot_pck_at：两遍遍历目录表（先文本/翻译，后编译资源）。目录表结构损坏时返回
           false，调用方据此回退到整包二进制切片；越界的数据偏移只跳过该条目。 */
        private static bool ScanGodotPckAt(FileStream fs, long pckBase, ulong pckSize, TextList prefetch)
        {
            if (prefetch.Full) return true;

            GodotPckInfo info;
            if (!ReadGodotPckInfo(fs, pckBase, pckSize, out info)) return false;

            long pckEnd = pckBase + (long)pckSize;
            var lenbuf = new byte[4];
            var entry = new byte[8 + 8 + 16 + 4];
            for (int pass = 0; pass < 2 && !prefetch.Full; pass++) {
                long pos = pckBase + (long)info.EntriesOffset;
                for (uint i = 0; i < info.FileCount && !prefetch.Full; i++) {
                    if (pos > pckEnd - 4 || !ReadAtExact(fs, pos, lenbuf, 4)) return false;
                    pos += 4;

                    uint pathLen = ReadU32Le(lenbuf, 0);
                    if (pathLen == 0 || pathLen > GodotPckMaxPathBytes || pos > pckEnd - pathLen) return false;
                    var pathBytes = new byte[pathLen];
                    if (!ReadAtExact(fs, pos, pathBytes, (int)pathLen)) return false;
                    string path = TrimPackPath(pathBytes, (int)pathLen);
                    pos += pathLen;

                    if (pos > pckEnd - (long)info.EntryMetaSize ||
                        !ReadAtExact(fs, pos, entry, (int)info.EntryMetaSize)) {
                        return false;
                    }
                    pos += info.EntryMetaSize;

                    ulong rel = ReadU64Le(entry, 0);
                    ulong size = ReadU64Le(entry, 8);
                    ulong dataInPack;
                    long dataAbs;
                    if (info.OffsetsAreAbsolute) {
                        if (rel < (ulong)pckBase || rel - (ulong)pckBase > pckSize) continue;
                        dataInPack = rel - (ulong)pckBase;
                        dataAbs = (long)rel;
                    } else if (rel <= ulong.MaxValue - info.FileBase) {
                        dataInPack = info.FileBase + rel;
                        dataAbs = unchecked(pckBase + (long)dataInPack);
                    } else {
                        continue;
                    }
                    int deferred = GodotPackDeferredBinaryPath(path) ? 1 : 0;
                    if (deferred == pass && dataInPack <= pckSize && size <= pckSize - dataInPack) {
                        ScanGodotPackEntry(fs, dataAbs, size, path, prefetch);
                    }
                }
            }
            return true;
        }

        /* find_pe_pck_section：PE 区段表里名为 "pck" 且起始 4 字节为 GDPC 的区段。 */
        private static bool FindPePckSection(string path, out long pckBase, out ulong pckSize)
        {
            pckBase = 0;
            pckSize = 0;
            FileStream fs = OpenGodotSharedRead(path);
            if (fs == null) return false;
            using (fs) {
                const int DosHeaderSize = 64;
                const int FileHeaderSize = 20;
                const int SectionHeaderSize = 40;
                long fileSize = fs.Length;
                var dos = new byte[DosHeaderSize];
                if (fileSize < DosHeaderSize || !ReadAtExact(fs, 0, dos, DosHeaderSize)) return false;
                if (dos[0] != (byte)'M' || dos[1] != (byte)'Z') return false;
                int lfanew = (int)ReadU32Le(dos, 60);
                if (lfanew <= 0) return false;
                long pe = lfanew;
                var sig = new byte[4];
                var fh = new byte[FileHeaderSize];
                if (pe > fileSize - (4 + FileHeaderSize) ||
                    !ReadAtExact(fs, pe, sig, 4) ||
                    ReadU32Le(sig, 0) != 0x00004550u /* "PE\0\0" */ ||
                    !ReadAtExact(fs, pe + 4, fh, FileHeaderSize)) {
                    return false;
                }
                int numberOfSections = fh[2] | (fh[3] << 8);
                int sizeOfOptionalHeader = fh[16] | (fh[17] << 8);
                if (numberOfSections <= 0 || numberOfSections > 128) return false;
                long sections = pe + 4 + FileHeaderSize + sizeOfOptionalHeader;
                var sh = new byte[SectionHeaderSize];
                for (int i = 0; i < numberOfSections; i++) {
                    long off = sections + (long)i * SectionHeaderSize;
                    if (off > fileSize - SectionHeaderSize || !ReadAtExact(fs, off, sh, SectionHeaderSize)) break;
                    if (sh[0] == (byte)'p' && sh[1] == (byte)'c' && sh[2] == (byte)'k' && sh[3] == 0) {
                        uint sizeOfRawData = ReadU32Le(sh, 16);
                        uint pointerToRawData = ReadU32Le(sh, 20);
                        if (pointerToRawData > 0 && sizeOfRawData >= GodotPckV1HeaderSize &&
                            (long)pointerToRawData <= fileSize - (long)sizeOfRawData) {
                            var magic = new byte[4];
                            if (ReadAtExact(fs, pointerToRawData, magic, 4) && ReadU32Le(magic, 0) == GodotPckMagic) {
                                pckBase = pointerToRawData;
                                pckSize = sizeOfRawData;
                                return true;
                            }
                        }
                    }
                }
                return false;
            }
        }

        /* scan_godot_pck_file：独立 .pck；返回目录表是否被成功解析。 */
        private static bool ScanGodotPckFile(string path, TextList prefetch)
        {
            FileStream fs = OpenGodotSharedRead(path);
            if (fs == null) return false;
            using (fs) {
                long size = fs.Length;
                if (size > 0) return ScanGodotPckAt(fs, 0, (ulong)size, prefetch);
                return false;
            }
        }

        private static void ScanGodotEmbeddedPckExe(string path, TextList prefetch)
        {
            long pckBase;
            ulong pckSize;
            if (!FindPePckSection(path, out pckBase, out pckSize)) return;
            FileStream fs = OpenGodotSharedRead(path);
            if (fs == null) return;
            using (fs) {
                ScanGodotPckAt(fs, pckBase, pckSize, prefetch);
            }
        }

        private static void ScanGodotTextFile(string path, TextList prefetch)
        {
            if (!SafeFs.FileSizeAtMost(path, GodotResourceScanMaxBytes)) return;
            byte[] buf = SafeFs.ReadBytes(path);
            if (buf == null) return;
            string s = CStringOf(buf);
            if (PathUtil.EndsWithNoCase(path, ".po")) {
                ScanGodotPoStrings(s, prefetch);
            } else if (PathUtil.EndsWithNoCase(path, ".csv")) {
                ScanGodotCsvStrings(s, prefetch);
            } else if (PathUtil.EndsWithNoCase(path, ".md")) {
                ScanGodotMarkdownStrings(s, prefetch);
            } else {
                ScanGodotQuotedStrings(s, prefetch);
                ScanGodotLines(s, prefetch);
            }
        }

        /* scan_godot_binary_file：.pck 先按目录表解析；解析失败（且未满）时整文件二进制切片。 */
        private static void ScanGodotBinaryFile(string path, TextList prefetch)
        {
            if (PathUtil.EndsWithNoCase(path, ".pck")) {
                if (ScanGodotPckFile(path, prefetch) || prefetch.Full) return;
            }
            if (!SafeFs.FileSizeAtMost(path, GodotResourceScanMaxBytes)) return;
            byte[] buf = SafeFs.ReadBytes(path);
            if (buf == null) return;
            ScanGodotBinaryBuffer(buf, buf.Length, prefetch);
        }

        /* scan_godot_resource_dir：目录顺序即 FindFirstFileW 顺序；深度上限 10；跳过启动器
           生成物与缓存/插件/存档目录；.exe 只检查内嵌 pck 区段。 */
        private static void ScanGodotResourceDir(string dir, TextList prefetch, int depth)
        {
            if (depth > GodotScanMaxDepth || !PathUtil.IsDir(dir)) return;
            var entries = Win32Find.EnumerateOrNull(dir, "*");
            if (entries == null) return;
            foreach (var fd in entries) {
                if (SkipGodotGeneratedFile(fd.Name)) continue;
                string path = PathUtil.Join(dir, fd.Name);
                if ((fd.Attributes & SafeFs.FILE_ATTRIBUTE_DIRECTORY) != 0) {
                    if (!SkipGodotScanDirectory(fd.Name)) ScanGodotResourceDir(path, prefetch, depth + 1);
                } else if (GodotTextFileName(fd.Name)) {
                    ScanGodotTextFile(path, prefetch);
                } else if (GodotBinaryFileName(fd.Name)) {
                    ScanGodotBinaryFile(path, prefetch);
                } else if (PathUtil.EndsWithNoCase(fd.Name, ".exe")) {
                    ScanGodotEmbeddedPckExe(path, prefetch);
                }
                if (prefetch.Full) break;
            }
        }

        /* warmup_scan_godot_resources */
        internal static void WarmupScanGodotResources(string dir, TextList prefetch)
        {
            ScanGodotResourceDir(dir, prefetch, 0);
        }

        /* warmup_godot：扫描资源并分批提交；原始 .pck 与编译资源只作为只读输入。 */
        private static void WarmupGodot(string dir)
        {
            var prefetch = new TextList { MaxItems = GodotMaxItems };
            WarmupScanGodotResources(dir, prefetch);
            int queued = PostPrefetchAll(prefetch);
            if (queued > 0) Log.Append("Godot preheated translation cache: queued " + queued + " texts.");
        }
    }
}
