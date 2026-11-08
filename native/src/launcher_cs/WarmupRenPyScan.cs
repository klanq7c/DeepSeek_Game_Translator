using System;
using System.Text;

namespace DstLauncher
{
    public static partial class Warmup
    {
        // 此层只服务 Ren'Py。Godot 使用的 RenpyStringAt 保持原有规则。
        private static bool RenpyQuote(char ch) { return ch == '\'' || ch == '"' || ch == '`'; }
        private static bool RenpyIdent(char ch) { return IsAlpha(ch) || (ch >= '0' && ch <= '9') || ch == '_'; }

        // 屏幕/输入只读取首个静态正文参数，后续 style/action 的字符串不是正文。
        private static int RenpyExpressionQuote(string text, int p)
        {
            while (ByteStr.At(text, p) == ' ' || ByteStr.At(text, p) == '\t') p++;
            if (ByteStr.At(text, p) == '_') {
                p++; while (ByteStr.At(text, p) == ' ' || ByteStr.At(text, p) == '\t') p++;
                if (ByteStr.At(text, p) != '(') return -1;
                p++; while (ByteStr.At(text, p) == ' ' || ByteStr.At(text, p) == '\t') p++;
            }
            while ("rRuU".IndexOf(ByteStr.At(text, p)) >= 0) p++;
            return RenpyQuote(ByteStr.At(text, p)) ? p : -1;
        }

        private static string RenpyUnsupportedExpression()
        {
            Log.Append("RenPy 预热：Python 字面量包含不可提交的转义，交由运行时发现。");
            return null;
        }

        private static int RenpyLiteralEnd(string text, int p, out bool triple)
        {
            char quote = text[p];
            triple = ByteStr.At(text, p + 1) == quote && ByteStr.At(text, p + 2) == quote;
            p += triple ? 3 : 1;
            while (p < text.Length) {
                if (text[p] == '\\' && p + 1 < text.Length) { p += 2; continue; }
                if (text[p] == quote && (!triple || (ByteStr.At(text, p + 1) == quote && ByteStr.At(text, p + 2) == quote))) return p;
                p++;
            }
            return -1;
        }

        private static string RenpyDecodeValue(string text, bool raw, bool expression)
        {
            var result = new StringBuilder(text.Length * 2 + 1);
            // Python 2/3 的 raw Unicode 规则不同，保留给对应引擎解析。
            if (raw && expression) for (int i = 0; i + 1 < text.Length; i++)
                if (text[i] == '\\' && "uUN".IndexOf(text[i + 1]) >= 0) return RenpyUnsupportedExpression();
            for (int i = 0; i < text.Length;) {
                char ch = text[i++];
                if (!raw && !expression && (ch == ' ' || ch == '\n')) {
                    while (i < text.Length && (text[i] == ' ' || text[i] == '\n')) i++;
                    result.Append(' '); continue;
                }
                if (!raw && ch == '\\' && i < text.Length) {
                    char escaped = text[i++];
                    if (expression) {
                        if (escaped == '\n') continue;
                        if (escaped == 'x' || escaped == 'u' || escaped == 'U') {
                            uint cp = 0; int width = escaped == 'x' ? 2 : escaped == 'u' ? 4 : 8;
                            for (int k = 0; k < width; k++) {
                                if (i >= text.Length || HexValue(text[i]) < 0) return RenpyUnsupportedExpression();
                                cp = (cp << 4) | (uint)HexValue(text[i++]);
                            }
                            if (cp == 0 || cp > 0x10ffff || (cp >= 0xd800 && cp <= 0xdfff)) return RenpyUnsupportedExpression();
                            AppendUtf8(result, cp); continue;
                        }
                        if (escaped >= '0' && escaped <= '7') {
                            uint cp = (uint)(escaped - '0'); int count = 1;
                            while (i < text.Length && count < 3 && text[i] >= '0' && text[i] <= '7') { cp = cp * 8 + (uint)(text[i++] - '0'); count++; }
                            if (cp == 0) return RenpyUnsupportedExpression();
                            AppendUtf8(result, cp); continue;
                        }
                        if (escaped == 'N') return RenpyUnsupportedExpression();
                        if (escaped == 'a') result.Append('\a');
                        else if (escaped == 'b') result.Append('\b');
                        else if (escaped == 'f') result.Append('\f');
                        else if (escaped == 'n') result.Append('\n');
                        else if (escaped == 'r') result.Append('\r');
                        else if (escaped == 't') result.Append('\t');
                        else if (escaped == 'v') result.Append('\v');
                        else { if (escaped != '\\' && escaped != '\'' && escaped != '"') result.Append('\\'); result.Append(escaped); }
                        continue;
                    }
                    if (!expression && (escaped == ' ' || escaped == '\n')) {
                        // 引擎先折叠源码空白，再解码反斜杠；不能保留第二段空白。
                        while (i < text.Length && (text[i] == ' ' || text[i] == '\n')) i++;
                        result.Append(' '); continue;
                    }
                    if (escaped == 'u') {
                        uint cp = 0; int count = 0;
                        while (i < text.Length && count < 4 && HexValue(text[i]) >= 0) { cp = cp * 16 + (uint)HexValue(text[i++]); count++; }
                        if (count > 0) {
                            uint lo;
                            if (cp >= 0xd800 && cp <= 0xdbff && i + 6 <= text.Length && text[i] == '\\' && text[i + 1] == 'u' &&
                                JsonU4(text, i + 2, out lo) && lo >= 0xdc00 && lo <= 0xdfff) {
                                cp = 0x10000 + ((cp - 0xd800) << 10) + lo - 0xdc00; i += 6;
                            } else if (cp >= 0xd800 && cp <= 0xdfff) cp = 0xfffd;
                            AppendUtf8(result, cp); continue;
                        }
                    }
                    if (!expression && (escaped == '[' || escaped == '{' || escaped == '%')) result.Append(escaped);
                    if (escaped == 'n') result.Append('\n');
                    else if (expression && escaped == 'r') result.Append('\r');
                    else if (expression && escaped == 't') result.Append('\t');
                    else result.Append(escaped);
                    continue;
                }
                result.Append(ch);
            }
            return result.ToString();
        }

        private static void RenpyAddSourceValue(TextList prefetch, string text, ref string previous)
        {
            if (text == null) return;
            int before = prefetch.Count;
            CollectRenpyString(text, prefetch);
            if (prefetch.Count > before) {
                prefetch.EnsurePrev();
                prefetch.SetPrev(before, previous);
                previous = prefetch.Items[before];
            }
        }

        private static int RenpyInputArgument(string line)
        {
            for (int p = 0; p < line.Length; p++) {
                if (line[p] == '#') break;
                if (RenpyQuote(line[p])) {
                    bool triple; int end = RenpyLiteralEnd(line, p, out triple);
                    if (end < 0) return -1;
                    p = end + (triple ? 3 : 1) - 1; continue;
                }
                if (line.IndexOf("renpy.input", p, StringComparison.Ordinal) == p && (p == 0 || (!RenpyIdent(line[p - 1]) && line[p - 1] != '.'))) {
                    int arg = p + 11;
                    while (ByteStr.At(line, arg) == ' ' || ByteStr.At(line, arg) == '\t') arg++;
                    if (ByteStr.At(line, arg) == '(') return arg + 1;
                }
            }
            return -1;
        }

        private static void RenpyAddSourceLiteral(TextList prefetch, string text, int p, int end,
            bool triple, bool raw, bool expression, ref string previous)
        {
            int width = triple ? 3 : 1;
            string body = text.Substring(p + width, end - p - width);
            if (!triple || raw || expression) {
                RenpyAddSourceValue(prefetch, RenpyDecodeValue(body, raw, expression), ref previous); return;
            }
            // 默认独白按空行分段；清屏段属于语句，不是待译正文。
            var normalized = new StringBuilder(body.Length);
            for (int i = 0; i < body.Length;) {
                if (body[i] == ' ') {
                    int j = i; while (j < body.Length && body[j] == ' ') j++;
                    if (j < body.Length && body[j] == '\n') { i = j; continue; }
                }
                char ch = body[i++]; normalized.Append(ch);
                if (ch == '\n') while (i < body.Length && body[i] == ' ') i++;
            }
            foreach (string part in normalized.ToString().Split(new[] { "\n\n" }, StringSplitOptions.None)) {
                string trimmed = ByteStr.Trim(part);
                if (trimmed.Length > 0 && trimmed != "{clear}")
                    RenpyAddSourceValue(prefetch, RenpyDecodeValue(trimmed, false, false), ref previous);
            }
        }

        private static void ScanRenpySource(string source, TextList prefetch)
        {
            // 引擎装载源码时先统一物理换行；这不会改变 raw 中显式的反斜杠序列。
            source = source.Replace("\r\n", "\n").Replace('\r', '\n');
            int p = source.StartsWith("\xef\xbb\xbf", StringComparison.Ordinal) ? 3 : 0;
            int excludedIndent = -1, screenIndent = -1;
            string previous = null;
            while (p < source.Length && !prefetch.Full) {
                int indent = 0;
                while (p < source.Length && (source[p] == ' ' || source[p] == '\t')) { indent++; p++; }
                int content = p, depth = 0;
                while (p < source.Length && (source[p] != '\n' || depth > 0)) {
                    if (RenpyQuote(source[p])) {
                        bool triple; int end = RenpyLiteralEnd(source, p, out triple);
                        if (end < 0) { Log.Append("RenPy 预热：未闭合字符串，余文交由运行时发现。"); return; }
                        p = end + (triple ? 3 : 1); continue;
                    }
                    if (source[p] == '#') { while (p < source.Length && source[p] != '\n') p++; if (depth == 0) break; continue; }
                    if (source[p] == '(' || source[p] == '[' || source[p] == '{') depth++;
                    if ((source[p] == ')' || source[p] == ']' || source[p] == '}') && depth > 0) depth--;
                    p++;
                }
                string line = source.Substring(content, p - content);
                if (p < source.Length && source[p] == '\n') p++;
                if (line.Length == 0 || line[0] == '#' || line[0] == '\r') continue;
                if (indent <= excludedIndent) excludedIndent = -1;
                if (indent <= screenIndent) screenIndent = -1;
                if (excludedIndent >= 0) continue;
                bool inScreen = screenIndent >= 0;
                if (StartsWithWordI(line, "screen")) { screenIndent = indent; continue; }
                if (StartsWithWordI(line, "python") || (StartsWithWordI(line, "init") && line.Contains("python")) ||
                    StartsWithWordI(line, "image") || StartsWithWordI(line, "transform") || StartsWithWordI(line, "style")) {
                    excludedIndent = indent; continue;
                }
                bool uiText = inScreen && (StartsWithWordI(line, "text") || StartsWithWordI(line, "textbutton") || StartsWithWordI(line, "label"));
                int cursor = 0; bool expression = uiText;
                if (line[0] == '$') {
                    int input = RenpyInputArgument(line);
                    if (input < 0) continue;
                    cursor = input; expression = true;
                } else if (uiText) {
                    while (cursor < line.Length && line[cursor] != ' ' && line[cursor] != '\t') cursor++;
                } else if (inScreen) continue;
                else if (!uiText) {
                    bool ignored = false;
                    foreach (string word in new[] { "scene", "show", "hide", "play", "queue", "stop", "with", "jump", "call", "return", "label", "define", "default", "init", "if", "elif", "else", "for", "while", "menu", "translate" })
                        if (StartsWithWordI(line, word)) { ignored = true; break; }
                    if (ignored) continue;
                }
                int seen = 0;
                if (expression) {
                    cursor = RenpyExpressionQuote(line, cursor);
                    if (cursor < 0) continue;
                } else while (cursor < line.Length && !RenpyQuote(line[cursor]) && line[cursor] != '#') cursor++;
                while (cursor < line.Length && RenpyQuote(line[cursor]) && seen++ < 16) {
                    bool triple; int end = RenpyLiteralEnd(line, cursor, out triple);
                    if (end < 0) break;
                    if (expression) {
                        int tail = end + (triple ? 3 : 1);
                        while (ByteStr.At(line, tail) == ' ' || ByteStr.At(line, tail) == '\t' || ByteStr.At(line, tail) == ')') tail++;
                        char next = ByteStr.At(line, tail);
                        if (RenpyQuote(next) || (next != 0 && "+-%*/.[(".IndexOf(next) >= 0) ||
                            StartsWithWordI(line.Substring(tail), "if") || StartsWithWordI(line.Substring(tail), "and") || StartsWithWordI(line.Substring(tail), "or") ||
                            ("rRuU".IndexOf(next) >= 0 && RenpyQuote(ByteStr.At(line, tail + 1)))) break;
                    }
                    int prefix = cursor; bool raw = false;
                    while (prefix > 0 && "rRuU".IndexOf(line[prefix - 1]) >= 0) { prefix--; if (line[prefix] == 'r' || line[prefix] == 'R') raw = true; }
                    raw = raw && (prefix == 0 || !RenpyIdent(line[prefix - 1]));
                    RenpyAddSourceLiteral(prefetch, line, cursor, end, triple, raw, expression, ref previous);
                    cursor = end + (triple ? 3 : 1);
                    while (cursor < line.Length && (line[cursor] == ' ' || line[cursor] == '\t')) cursor++;
                    if (ByteStr.At(line, cursor) == 'r' && RenpyQuote(ByteStr.At(line, cursor + 1))) cursor++;
                }
            }
        }
    }
}
