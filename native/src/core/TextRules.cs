using System;
using System.Text;

namespace DstCore
{
    /*
     * TextRules —— 与 http.c/util.c 对齐的文本判定与清洗规则。
     *
     * 共享核心（native/src/core）：纯逻辑、无状态、只依赖 BCL，同时被
     *   - dst_server_cs（net472）
     *   - Unity IL2CPP XUnity 端点 payload（net6.0）
     *   - 后续的 C# 启动器
     * 以源文件链接方式编译，保证各端对"能不能译 / 译文算不算成功 / 提示词回显
     * 怎么剥"得出相同结论。修改这里等于修改跨引擎契约：必须同步 util.c 并跑
     * tests/core_tests。
     */

    public static class TextRules
    {
        /* has_translation_signal：含 ASCII 字母或任何非 ASCII 字符即视为可译。 */
        public static bool HasTranslationSignal(string s)
        {
            if (string.IsNullOrEmpty(s)) return false;
            foreach (char c in s) {
                if (c < 0x80) {
                    if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')) return true;
                } else {
                    return true;
                }
            }
            return false;
        }

        /* utf8_has_cjk：字符串含 U+4E00..U+9FFF 汉字（已是中文则不再翻译）。 */
        public static bool HasCjk(string s)
        {
            if (string.IsNullOrEmpty(s)) return false;
            foreach (char c in s) {
                if (c >= 0x4e00 && c <= 0x9fff) return true;
                // UTF-16 代理区存放的增补平面字符不可能是 CJK 统一表意文字本部。
            }
            return false;
        }

        /* should_translate_text：非空 + 有翻译信号 + 不含 CJK。 */
        public static bool ShouldTranslate(string text)
        {
            return !string.IsNullOrEmpty(text) && HasTranslationSignal(text) && !HasCjk(text);
        }

        /* is_resolved_translation：译文非空且与原文不同——绝不把原文回显当成功。 */
        public static bool IsResolvedTranslation(string original, string translated)
        {
            return !string.IsNullOrEmpty(original)
                && !string.IsNullOrEmpty(translated)
                && string.CompareOrdinal(original, translated) != 0;
        }

        private static readonly string[] EchoPrefixes = {
            "翻译成简体中文",
            "翻译为简体中文",
            "译成简体中文",
            "简体中文翻译",
            "简体中文译文",
            "Simplified Chinese translation",
            "Translation to Simplified Chinese",
            "Translate to Simplified Chinese",
            "Translated into Simplified Chinese",
            "Translate this exact game text to Simplified Chinese. Return only the translation."
        };

        /* 与 C 的 isspace 一致：只把 ASCII 空白视为空白。char.IsWhiteSpace 还会
           吞掉全角空格 U+3000 等，那会让两端对同一条缓存值得出不同结果。 */
        private static bool IsAsciiSpace(char c)
        {
            return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\v' || c == '\f';
        }

        private static readonly char[] AsciiSpaces = { ' ', '\t', '\n', '\r', '\v', '\f' };

        /* translation_prefix_equal：ASCII 字节不区分大小写，非 ASCII 逐字精确比较。
           不用文化相关的 string.Compare——它对某些码位做语言学等价处理，与 C 的
           逐字节语义不一致。 */
        private static bool PrefixEqualAt(string s, int offset, string prefix)
        {
            if (offset + prefix.Length > s.Length) return false;
            for (int i = 0; i < prefix.Length; i++) {
                char a = s[offset + i];
                char b = prefix[i];
                if (a == b) continue;
                if (a < 0x80 && b < 0x80
                    && char.ToLowerInvariant(a) == char.ToLowerInvariant(b)) continue;
                return false;
            }
            return true;
        }

        /* strip_translation_prompt_echo：剥掉模型把提示词带进译文的常见前缀。
           与 util.c 的 translation_after_prompt_prefix 逐条对齐：前缀之后必须有
           真实分隔符（可选空格/制表符 + 半角或全角冒号，或直接换行），且余文非空；
           否则不是回显，原样保留——避免误改仅以相似短语开头的普通译文，
           也避免把恰好等于前缀的译文剥成空串后被缓存拒收。 */
        private static bool StripPromptEcho(ref string s)
        {
            int start = 0;
            while (start < s.Length && IsAsciiSpace(s[start])) start++;
            for (int i = 0; i < EchoPrefixes.Length; i++) {
                if (!PrefixEqualAt(s, start, EchoPrefixes[i])) continue;
                int rest = start + EchoPrefixes[i].Length;
                while (rest < s.Length && (s[rest] == ' ' || s[rest] == '\t')) rest++;
                if (rest < s.Length && (s[rest] == ':' || s[rest] == '\uFF1A')) {
                    rest++;
                } else if (rest >= s.Length || (s[rest] != '\r' && s[rest] != '\n')) {
                    continue; // 无分隔符：不是提示词回显
                }
                while (rest < s.Length && IsAsciiSpace(s[rest])) rest++;
                if (rest >= s.Length) continue; // 余文为空：不剥离
                s = s.Substring(rest).Trim(AsciiSpaces);
                return true;
            }
            return false;
        }

        /* normalize_translation_result：最多 3 轮前缀剥离。所有缓存入口与
           API 响应共用，与 util.c 的 normalize_translation_result 逐条对齐。 */
        public static string NormalizeTranslationResult(string s)
        {
            if (string.IsNullOrEmpty(s)) return s;
            for (int pass = 0; pass < 3 && s.Length > 0; pass++) {
                if (!StripPromptEcho(ref s)) break;
            }
            return s;
        }

        /* api.c 的 normalize_translation：去首尾空白 + <<<>>> 与 ``` 围栏。 */
        public static string NormalizeTranslation(string s)
        {
            s = s.Trim();
            if (s.StartsWith("<<<")) {
                int p = 3;
                while (p < s.Length && char.IsWhiteSpace(s[p])) p++;
                int end = s.IndexOf(">>>", p, StringComparison.Ordinal);
                if (end >= 0) s = s.Substring(p, end - p).Trim();
            }
            if (s.StartsWith("```")) {
                int nl = s.IndexOf('\n', 3);
                int end = s.IndexOf("```", 3, StringComparison.Ordinal);
                if (nl >= 0 && end > nl) s = s.Substring(nl + 1, end - nl - 1).Trim();
            }
            return s;
        }
    }
}
