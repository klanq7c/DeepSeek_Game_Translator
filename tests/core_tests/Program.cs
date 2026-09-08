using System;
using System.Collections.Generic;
using System.Text;
using DstCore;

namespace DstCoreTests
{
    /*
     * 共享核心 golden 测试。期望值按 native/src/server/util.c、json.c、cache.c 的
     * 语义书写：这里每一条断言同时是"C 服务器会怎么做"的书面记录。
     * 任何一条失败都意味着 C# 端（服务器 / Unity payload / 启动器）与 C 端分叉。
     */
    internal static class Program
    {
        private static int passed;
        private static readonly List<string> failures = new List<string>();

        private static void Check(string name, bool ok, string detail = null)
        {
            if (ok) { passed++; return; }
            failures.Add(name + (detail != null ? " -- " + detail : ""));
        }

        private static void Eq(string name, string expected, string actual)
        {
            Check(name, string.CompareOrdinal(expected, actual) == 0,
                  "expected " + Show(expected) + " got " + Show(actual));
        }

        private static string Show(string s)
        {
            if (s == null) return "<null>";
            var sb = new StringBuilder("\"");
            foreach (char c in s) {
                if (c < 0x20 || c > 0x7e) sb.Append("\\u").Append(((int)c).ToString("x4"));
                else sb.Append(c);
            }
            return sb.Append('"').ToString();
        }

        private static void Throws<T>(string name, Action act) where T : Exception
        {
            try { act(); Check(name, false, "no exception"); }
            catch (T) { Check(name, true); }
            catch (Exception ex) { Check(name, false, "wrong exception " + ex.GetType().Name); }
        }

        private static int Main()
        {
            TextRulesTests();
            JsonTests();
            CacheCodecTests();
            ContractTests();

            if (failures.Count > 0) {
                Console.Error.WriteLine("core tests FAILED: " + failures.Count + " failure(s), " + passed + " passed");
                foreach (var f in failures) Console.Error.WriteLine("  - " + f);
                return 1;
            }
            Console.WriteLine("core tests PASS: " + passed + " checks");
            return 0;
        }

        /* ---------------- TextRules（util.c / http.c） ---------------- */
        private static void TextRulesTests()
        {
            // has_translation_signal：ASCII 字母或任何非 ASCII
            Check("signal: latin", TextRules.HasTranslationSignal("Hello"));
            Check("signal: non-ascii", TextRules.HasTranslationSignal("日本語"));
            Check("signal: digits/punct only -> none", !TextRules.HasTranslationSignal("123 ... !?"));
            Check("signal: empty -> none", !TextRules.HasTranslationSignal(""));
            Check("signal: null -> none", !TextRules.HasTranslationSignal(null));

            // utf8_has_cjk：仅 U+4E00..U+9FFF
            Check("cjk: han", TextRules.HasCjk("你好"));
            Check("cjk: hiragana is not han", !TextRules.HasCjk("こんにちは"));
            Check("cjk: hangul is not han", !TextRules.HasCjk("안녕"));
            Check("cjk: fullwidth punct is not han", !TextRules.HasCjk("！？"));
            Check("cjk: boundary U+4E00", TextRules.HasCjk("\u4e00"));
            Check("cjk: boundary U+9FFF", TextRules.HasCjk("\u9fff"));
            Check("cjk: U+9FA6.. still in range", TextRules.HasCjk("\u9fa6"));
            Check("cjk: U+A000 out of range", !TextRules.HasCjk("\ua000"));

            // should_translate_text
            Check("should: english yes", TextRules.ShouldTranslate("Start Game"));
            Check("should: japanese yes (non-ascii signal, no han)", TextRules.ShouldTranslate("はじめる"));
            Check("should: chinese no", !TextRules.ShouldTranslate("开始游戏"));
            Check("should: mixed with han no", !TextRules.ShouldTranslate("HP 值"));
            Check("should: numbers no", !TextRules.ShouldTranslate("100%"));
            Check("should: empty no", !TextRules.ShouldTranslate(""));

            // is_resolved_translation
            Check("resolved: different", TextRules.IsResolvedTranslation("Hello", "你好"));
            Check("resolved: identity is not success", !TextRules.IsResolvedTranslation("Hello", "Hello"));
            Check("resolved: empty translated", !TextRules.IsResolvedTranslation("Hello", ""));
            Check("resolved: case differs counts as different (ordinal)", TextRules.IsResolvedTranslation("Hello", "hello"));

            // normalize_translation_result：提示词回显剥离
            Eq("echo: colon", "你好", TextRules.NormalizeTranslationResult("翻译成简体中文：你好"));
            Eq("echo: ascii colon + space", "你好", TextRules.NormalizeTranslationResult("翻译成简体中文: 你好"));
            Eq("echo: tab before colon", "你好", TextRules.NormalizeTranslationResult("翻译成简体中文\t:\t你好"));
            Eq("echo: newline separator", "你好", TextRules.NormalizeTranslationResult("Simplified Chinese translation\n你好"));
            Eq("echo: crlf separator", "你好", TextRules.NormalizeTranslationResult("Simplified Chinese translation\r\n你好"));
            Eq("echo: leading ascii whitespace skipped", "你好", TextRules.NormalizeTranslationResult("  \n翻译为简体中文：你好"));
            Eq("echo: ascii prefix case-insensitive", "你好", TextRules.NormalizeTranslationResult("simplified chinese TRANSLATION: 你好"));
            Eq("echo: trailing ascii whitespace trimmed after strip", "你好", TextRules.NormalizeTranslationResult("译成简体中文：你好 \n"));
            Eq("echo: long prompt sentence", "你好", TextRules.NormalizeTranslationResult(
                "Translate this exact game text to Simplified Chinese. Return only the translation.: 你好"));
            Eq("echo: two nested echoes stripped (pass 2)", "你好",
               TextRules.NormalizeTranslationResult("翻译成简体中文：简体中文译文：你好"));
            Eq("echo: three nested echoes stripped (pass 3)", "你好",
               TextRules.NormalizeTranslationResult("翻译成简体中文：简体中文译文：译成简体中文：你好"));
            Eq("echo: fourth echo survives (3-pass limit, parity with C)", "简体中文翻译：你好",
               TextRules.NormalizeTranslationResult("翻译成简体中文：简体中文译文：译成简体中文：简体中文翻译：你好"));

            // 不剥离的情形——必须逐字节原样返回
            Eq("echo: no separator -> untouched", "翻译成简体中文你好", TextRules.NormalizeTranslationResult("翻译成简体中文你好"));
            Eq("echo: similar phrase in a sentence -> untouched", "简体中文翻译很难",
               TextRules.NormalizeTranslationResult("简体中文翻译很难"));
            Eq("echo: empty remainder -> untouched", "翻译成简体中文：", TextRules.NormalizeTranslationResult("翻译成简体中文："));
            Eq("echo: whitespace-only remainder -> untouched", "翻译成简体中文： \n", TextRules.NormalizeTranslationResult("翻译成简体中文： \n"));
            Eq("echo: normal value untouched (no trimming)", "  你好  ", TextRules.NormalizeTranslationResult("  你好  "));
            Eq("echo: null passthrough", null, TextRules.NormalizeTranslationResult(null));
            Eq("echo: empty passthrough", "", TextRules.NormalizeTranslationResult(""));
            // C 的 isspace 不认识全角空格 U+3000：它既不能跳过前缀前的空白，也不算分隔符后的空白
            Eq("echo: fullwidth space before prefix is not whitespace (C isspace parity)",
               "\u3000翻译成简体中文：你好", TextRules.NormalizeTranslationResult("\u3000翻译成简体中文：你好"));
            Eq("echo: fullwidth space after colon is kept (C isspace parity)",
               "\u3000你好", TextRules.NormalizeTranslationResult("翻译成简体中文：\u3000你好"));
            // 非 ASCII 部分精确比较：繁体"翻譯"不是回显前缀
            Eq("echo: traditional variant is not the prefix", "翻譯成簡體中文：你好",
               TextRules.NormalizeTranslationResult("翻譯成簡體中文：你好"));

            // api.c normalize_translation：围栏剥离
            Eq("fence: <<< >>>", "你好", TextRules.NormalizeTranslation("<<< 你好 >>>"));
            Eq("fence: ``` block", "你好", TextRules.NormalizeTranslation("```text\n你好\n```"));
            Eq("fence: plain trimmed", "你好", TextRules.NormalizeTranslation("  你好\n"));
            Eq("fence: unclosed <<< kept", "<<< 你好", TextRules.NormalizeTranslation("<<< 你好"));
        }

        /* ---------------- Json（json.c） ---------------- */
        private static void JsonTests()
        {
            var dom = Json.Parse("{\"texts\":[\"a\",\"b\"],\"cache_only\":true,\"n\":1.5,\"z\":null,\"s\":\"\\u4f60\\n\"}");
            var top = Json.TopObject(dom);
            Check("json: top object", top != null);
            var texts = Json.TopArray(dom, "texts");
            Check("json: array length", texts != null && texts.Count == 2 && (string)texts[1] == "b");
            Check("json: bool", Json.TopBool(dom, "cache_only"));
            Check("json: missing bool false", !Json.TopBool(dom, "nope"));
            Check("json: number", top["n"] is double && (double)top["n"] == 1.5);
            Check("json: null", top.ContainsKey("z") && top["z"] == null);
            Eq("json: \\u escape + \\n", "你\n", Json.TopString(dom, "s"));
            Eq("json: surrogate pair joins to one code point", "\U0001F600",
               Json.TopString(Json.Parse("{\"e\":\"\\ud83d\\ude00\"}"), "e"));
            Check("json: duplicate key -> last wins", Json.TopString(Json.Parse("{\"k\":\"1\",\"k\":\"2\"}"), "k") == "2");
            Check("json: whitespace tolerant", Json.TopBool(Json.Parse(" \r\n\t{ \"a\" : true } "), "a"));

            Throws<Json.ParseException>("json: unterminated string", () => Json.Parse("{\"a\":\"abc"));
            Throws<Json.ParseException>("json: bad escape \\q", () => Json.Parse("{\"a\":\"bad\\qescape\"}"));
            Throws<Json.ParseException>("json: raw control char", () => Json.Parse("{\"a\":\"x\u0001y\"}"));
            Throws<Json.ParseException>("json: trailing content", () => Json.Parse("{} x"));
            Throws<Json.ParseException>("json: trailing comma", () => Json.Parse("{\"a\":1,}"));
            Throws<Json.ParseException>("json: empty input", () => Json.Parse(""));
            Throws<Json.ParseException>("json: nesting too deep", () => Json.Parse(new string('[', 300) + new string(']', 300)));
            Check("json: nesting 200 ok", Json.Parse(new string('[', 200) + new string(']', 200)) != null);

            Eq("json escape: quotes/backslash", "\"a\\\"b\\\\c\"", Json.Escape("a\"b\\c"));
            Eq("json escape: control chars", "\"\\n\\r\\t\\u0001\"", Json.Escape("\n\r\t\u0001"));
            Eq("json escape: non-ascii passthrough", "\"你好😀\"", Json.Escape("你好😀"));
            Eq("json escape: null -> empty (buf_json parity)", "", Json.Escape(null));
            // 往返：Escape 的输出必须能被 Parse 还原
            string sample = "tab\t quote\" back\\ 你好 \u0007 😀";
            Eq("json roundtrip", sample, (string)Json.Parse(Json.Escape(sample)));
        }

        /* ---------------- CacheCodec（cache.c） ---------------- */
        private static void CacheCodecTests()
        {
            Eq("codec: accept normal", "你好", CacheCodec.NormalizeForStore("Hello", "你好"));
            Eq("codec: strips echo before store", "你好", CacheCodec.NormalizeForStore("Hello", "翻译成简体中文：你好"));
            Check("codec: reject empty key", CacheCodec.NormalizeForStore("", "你好") == null);
            Check("codec: reject null key", CacheCodec.NormalizeForStore(null, "你好") == null);
            Check("codec: reject empty value", CacheCodec.NormalizeForStore("Hello", "") == null);
            Check("codec: reject identity (original echoed back)", CacheCodec.NormalizeForStore("Hello", "Hello") == null);
            Check("codec: reject echo that collapses to identity", CacheCodec.NormalizeForStore("Hello", "翻译成简体中文：Hello") == null);
            // 前缀后无余文时不算回显（util.c translation_after_prompt_prefix 返回 NULL），
            // 值原样保留并因非空、不等于键而入库——这是 C 版的既定行为，此处只记录，不改。
            Eq("codec: prefix with empty remainder is kept verbatim (C parity)", "翻译成简体中文：",
               CacheCodec.NormalizeForStore("Hello", "翻译成简体中文："));
            Check("codec: whitespace-only value is not rejected by codec (normalize leaves it; C parity)",
                  CacheCodec.NormalizeForStore("Hello", "   ") == "   ");

            string line = CacheCodec.EncodeLine("Hello", "你好");
            Eq("codec: line format base64\\tbase64\\n", "SGVsbG8=\t5L2g5aW9\n", line);
            string k, v;
            Check("codec: decode roundtrip", CacheCodec.TryDecodeLine(line.TrimEnd('\n'), out k, out v) && k == "Hello" && v == "你好");
            Check("codec: decode strips echo", CacheCodec.TryDecodeLine(CacheCodec.EncodeLine("Hello", "翻译成简体中文：你好").TrimEnd('\n'), out k, out v) && v == "你好");
            Check("codec: decode rejects identity", !CacheCodec.TryDecodeLine(CacheCodec.EncodeLine("Hello", "Hello").TrimEnd('\n'), out k, out v));
            Check("codec: decode rejects no tab", !CacheCodec.TryDecodeLine("SGVsbG8=", out k, out v));
            Check("codec: decode rejects bad base64 key", !CacheCodec.TryDecodeLine("!!!\t5L2g5aW9", out k, out v));
            Check("codec: decode rejects bad base64 value", !CacheCodec.TryDecodeLine("SGVsbG8=\t!!!", out k, out v));
            Check("codec: decode rejects empty value", !CacheCodec.TryDecodeLine("SGVsbG8=\t", out k, out v));
            Check("codec: decode rejects null", !CacheCodec.TryDecodeLine(null, out k, out v));
            Check("codec: decode rejects over-long line", !CacheCodec.TryDecodeLine(new string('A', CacheCodec.MaxLineChars + 1), out k, out v));
            // 多行文本与 tab 都经 Base64 安全落盘
            string multi = "line1\nline2\tcol";
            Check("codec: multiline/tab roundtrip", CacheCodec.TryDecodeLine(CacheCodec.EncodeLine(multi, "多行\t译文").TrimEnd('\n'), out k, out v)
                  && k == multi && v == "多行\t译文");
            // 与 C 的 b64 一致：标准字母表 + '=' 填充
            Check("codec: standard alphabet with padding", CacheCodec.EncodeLine("a", "b") == "YQ==\tYg==\n");
        }

        /* ---------------- Contract ---------------- */
        private static void ContractTests()
        {
            Check("contract: resolved cache", Contract.IsResolvedSource(Contract.SourceCache));
            Check("contract: resolved api_batch", Contract.IsResolvedSource(Contract.SourceApiBatch));
            Check("contract: pass is neither resolved nor pending", !Contract.IsResolvedSource(Contract.SourcePass) && !Contract.IsPendingSource(Contract.SourcePass));
            Check("contract: queued pending", Contract.IsPendingSource(Contract.SourceQueued));
            Check("contract: miss pending", Contract.IsPendingSource(Contract.SourceMiss));
            Check("contract: unknown source is nothing", !Contract.IsResolvedSource("weird") && !Contract.IsPendingSource("weird"));
            Check("contract: default url uses default port", Contract.DefaultBaseUrl.EndsWith(":" + Contract.DefaultPort));
        }
    }
}
