using System;
using System.Text;
using System.Text.RegularExpressions;

namespace DstCore
{
    // 混合文本的语言判定与中文保护只影响新请求，不改写已有翻译记忆。
    public static class MixedText
    {
        // 占位符到第一个闭合边界为止；URL 到可打印 ASCII 末尾为止，
        // 防止吞掉相邻标签之间的正文或中文标点之后的英文，与 C 判定保持一致。
        private static readonly Regex ProtectedSpan = new Regex(
            @"__[A-Za-z0-9_]+?__|%\([^)]+\)[A-Za-z]|<(?:(?:""[^""]*"")|(?:'[^']*')|[^'"">])*>|\[[^\]\r\n]*\]|\{[^}\r\n]*\}|(?:\\|\x1b)[A-Za-z]+(?:\[[^\]]*\]|<[^>]*>)?|(?:[A-Za-z][A-Za-z0-9+.-]*:[\\/])[!-~]*",
            RegexOptions.CultureInvariant);
        private static readonly Regex LatinWord = new Regex(@"[A-Za-z]{3,}", RegexOptions.CultureInvariant);
        private static readonly Regex RenPyProtectedSpan = new Regex(
            @"%\([^)]+\)[A-Za-z]|(?:[A-Za-z][A-Za-z0-9+.-]*:[\\/])[!-~]*",
            RegexOptions.CultureInvariant);

        public static bool HasUntranslatedLatin(string text)
        {
            if (string.IsNullOrEmpty(text)) return false;
            if (!LatinWord.IsMatch(text)) return false;
            return HasLatinWord(ProtectedSpan.Replace(text, " "));
        }

        private static bool HasLatinWord(string visible)
        {
            foreach (Match word in LatinWord.Matches(visible)) {
                if (word.Length >= 4) return true;
                foreach (char ch in word.Value) if (ch >= 'a' && ch <= 'z') return true;
            }
            return false;
        }

        // Ren'Py 的双开括号是可见字面内容，尖括号也不是其文本标签。
        // 只在请求显式声明引擎时使用这条规则，避免改变其他引擎的自定义标签语义。
        public static bool HasUntranslatedLatinRenPy(string text)
        {
            if (string.IsNullOrEmpty(text) || !LatinWord.IsMatch(text)) return false;
            var visible = new StringBuilder(text.Length);
            int square = 0, curly = 0;
            for (int i = 0; i < text.Length;) {
                char ch = text[i];
                if (ch == '[' || ch == '{') {
                    if (i + 1 < text.Length && text[i + 1] == ch) {
                        visible.Append(ch).Append(ch); i += 2; continue;
                    }
                    int end = text.IndexOf(ch == '[' ? ']' : '}', i + 1);
                    if (end > i && (ch == '[' ? square : curly) < 64) {
                        int count = 0; bool singleLine = true;
                        for (int j = i + 1; j < end; j++) {
                            if (text[j] == '\n' || text[j] == '\r') singleLine = false;
                            if (!char.IsLowSurrogate(text[j]) || j == i + 1 || !char.IsHighSurrogate(text[j - 1])) count++;
                        }
                        if (count > 0 && count <= 96 && singleLine) {
                            if (ch == '[') square++; else curly++;
                            visible.Append(' '); i = end + 1; continue;
                        }
                    }
                }
                visible.Append(ch); i++;
            }
            return HasLatinWord(RenPyProtectedSpan.Replace(visible.ToString(), " "));
        }

        private static string Prefix(string source)
        {
            int salt = 0;
            string prefix;
            do { prefix = "__DST_HAN" + salt++ + "_"; }
            while (source.IndexOf(prefix, StringComparison.Ordinal) >= 0);
            return prefix;
        }

        private static bool IsHan(char ch) { return ch >= '\u4e00' && ch <= '\u9fff'; }

        public static string Protect(string source)
        {
            if (!TextRules.HasCjk(source)) return source;
            string prefix = Prefix(source);
            var result = new StringBuilder();
            int token = 0;
            for (int i = 0; i < source.Length;) {
                if (!IsHan(source[i])) { result.Append(source[i++]); continue; }
                while (i < source.Length && IsHan(source[i])) i++;
                result.Append(prefix).Append(token++).Append("__");
            }
            return result.ToString();
        }

        // 模型丢失、重复或重排保护标记时拒绝结果；调用方记录故障，不能写成功缓存。
        public static bool TryRestore(string source, string reply, out string restored)
        {
            restored = reply;
            if (!TextRules.HasCjk(source)) return true;
            if (string.IsNullOrEmpty(reply)) return false;
            string prefix = Prefix(source);
            var result = new StringBuilder();
            int token = 0, copied = 0;
            for (int i = 0; i < source.Length;) {
                if (!IsHan(source[i])) { i++; continue; }
                int start = i;
                while (i < source.Length && IsHan(source[i])) i++;
                string key = prefix + token++ + "__";
                int pos = reply.IndexOf(key, copied, StringComparison.Ordinal);
                if (pos < 0 || reply.IndexOf(key, pos + key.Length, StringComparison.Ordinal) >= 0) return false;
                result.Append(reply, copied, pos - copied).Append(source, start, i - start);
                copied = pos + key.Length;
            }
            result.Append(reply, copied, reply.Length - copied);
            restored = result.ToString();
            return restored.IndexOf(prefix, StringComparison.Ordinal) < 0;
        }
    }
}
