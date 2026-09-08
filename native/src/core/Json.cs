using System.Collections.Generic;
using System.Globalization;
using System.Text;

namespace DstCore
{
    /*
     * Json —— 与 native/src/server/json.c 语义对齐的严格 JSON 解析与转义输出。
     * 共享核心（native/src/core）：只依赖 BCL，被 dst_server_cs 与后续 C# 启动器
     * 以源文件链接方式编译。
     *
     * 解析必须是"全量校验"式的：未闭合字符串、未知转义（\q）、裸控制字符
     * 一律判失败（对应测试 "unterminated string" / "bad\qescape" 必须返回 400）。
     * \uXXXX 解码后与后继低位代理合并为真实码位，输出侧再按 UTF-8 编码，
     * 保证 emoji 以 4 字节 UTF-8 落盘/回传而不是两个孤立代理半片。
     *
     * DOM 表示：object = Dictionary<string,object>（后值覆盖前值），
     * array = List<object>，string / bool / double / null = 对应 CLR 类型。
     */

    public static class Json
    {
        public sealed class ParseException : System.Exception
        {
            public ParseException(string message) : base(message) { }
        }

        public static object Parse(string text)
        {
            var p = new Parser(text ?? "");
            var value = p.ParseValue();
            p.SkipWs();
            if (!p.AtEnd) throw new ParseException("trailing content after JSON value");
            return value;
        }

        private sealed class Parser
        {
            /* 递归下降解析器的嵌套上限。C 版 json.c 是非递归扫描，天然不怕
               "[[[[...]]]]"；这里 2MB 请求体足以塞进上百万层嵌套，若不设限会
               直接栈溢出结束进程（StackOverflow 不可捕获）。正常请求嵌套不过 3 层。 */
            private const int MaxDepth = 256;

            private readonly string s;
            private int i;
            private int depth;

            public Parser(string text) { s = text; }

            private void Enter()
            {
                if (++depth > MaxDepth) throw new ParseException("JSON nesting too deep");
            }

            private void Leave() { depth--; }

            public bool AtEnd { get { return i >= s.Length; } }

            public void SkipWs()
            {
                while (i < s.Length) {
                    char c = s[i];
                    if (c == ' ' || c == '\t' || c == '\r' || c == '\n') i++;
                    else break;
                }
            }

            private char Peek()
            {
                if (i >= s.Length) throw new ParseException("unexpected end of JSON");
                return s[i];
            }

            private char Next()
            {
                if (i >= s.Length) throw new ParseException("unexpected end of JSON");
                return s[i++];
            }

            public object ParseValue()
            {
                SkipWs();
                char c = Peek();
                if (c == '{') return ParseObject();
                if (c == '[') return ParseArray();
                if (c == '"') return ParseString();
                if (c == 't') { Expect("true"); return true; }
                if (c == 'f') { Expect("false"); return false; }
                if (c == 'n') { Expect("null"); return null; }
                return ParseNumber();
            }

            private void Expect(string word)
            {
                if (i + word.Length > s.Length ||
                    string.CompareOrdinal(s, i, word, 0, word.Length) != 0) {
                    throw new ParseException("invalid JSON literal");
                }
                i += word.Length;
            }

            private Dictionary<string, object> ParseObject()
            {
                Enter();
                var obj = new Dictionary<string, object>();
                Next(); // '{'
                SkipWs();
                if (Peek() == '}') { Next(); Leave(); return obj; }
                for (; ; ) {
                    SkipWs();
                    if (Peek() != '"') throw new ParseException("object key must be a string");
                    var key = ParseString();
                    SkipWs();
                    if (Next() != ':') throw new ParseException("expected ':' in object");
                    var value = ParseValue();
                    obj[key] = value;
                    SkipWs();
                    char c = Next();
                    if (c == '}') { Leave(); return obj; }
                    if (c != ',') throw new ParseException("expected ',' or '}' in object");
                }
            }

            private List<object> ParseArray()
            {
                Enter();
                var arr = new List<object>();
                Next(); // '['
                SkipWs();
                if (Peek() == ']') { Next(); Leave(); return arr; }
                for (; ; ) {
                    arr.Add(ParseValue());
                    SkipWs();
                    char c = Next();
                    if (c == ']') { Leave(); return arr; }
                    if (c != ',') throw new ParseException("expected ',' or ']' in array");
                }
            }

            public string ParseString()
            {
                var sb = new StringBuilder();
                Next(); // '"'
                for (; ; ) {
                    if (i >= s.Length) throw new ParseException("unterminated JSON string");
                    char c = s[i++];
                    if (c == '"') return sb.ToString();
                    if (c == '\\') {
                        if (i >= s.Length) throw new ParseException("unterminated JSON escape");
                        char e = s[i++];
                        switch (e) {
                            case '"': sb.Append('"'); break;
                            case '\\': sb.Append('\\'); break;
                            case '/': sb.Append('/'); break;
                            case 'b': sb.Append('\b'); break;
                            case 'f': sb.Append('\f'); break;
                            case 'n': sb.Append('\n'); break;
                            case 'r': sb.Append('\r'); break;
                            case 't': sb.Append('\t'); break;
                            case 'u': {
                                sb.Append((char)ParseHex4());
                                // 代理对：与 json.c 一致合并为真实码位（.NET string
                                // 本身按 UTF-16 保存，孤立代理输出时替换为 U+FFFD）。
                                break;
                            }
                            default:
                                throw new ParseException("unknown JSON escape \\" + e);
                        }
                    } else if (c < 0x20) {
                        throw new ParseException("raw control character in JSON string");
                    } else {
                        sb.Append(c);
                    }
                }
            }

            private int ParseHex4()
            {
                if (i + 4 > s.Length) throw new ParseException("truncated \\u escape");
                int v;
                if (!int.TryParse(s.Substring(i, 4), NumberStyles.HexNumber,
                                  CultureInfo.InvariantCulture, out v)) {
                    throw new ParseException("invalid \\u escape");
                }
                i += 4;
                return v;
            }

            private object ParseNumber()
            {
                int start = i;
                if (Peek() == '-') i++;
                while (i < s.Length && ((s[i] >= '0' && s[i] <= '9') ||
                       s[i] == '.' || s[i] == 'e' || s[i] == 'E' ||
                       s[i] == '+' || s[i] == '-')) i++;
                if (i == start) throw new ParseException("invalid JSON value");
                double d;
                if (!double.TryParse(s.Substring(start, i - start), NumberStyles.Float,
                                     CultureInfo.InvariantCulture, out d)) {
                    throw new ParseException("invalid JSON number");
                }
                return d;
            }
        }

        /* 顶层便捷取值：只看根对象的直接字段（嵌套同名字段不参与）。 */
        public static Dictionary<string, object> TopObject(object dom)
        {
            return dom as Dictionary<string, object>;
        }

        public static string TopString(object dom, string key)
        {
            var obj = dom as Dictionary<string, object>;
            if (obj == null) return null;
            return obj.TryGetValue(key, out var v) ? (v as string) : null;
        }

        public static List<object> TopArray(object dom, string key)
        {
            var obj = dom as Dictionary<string, object>;
            if (obj == null) return null;
            return obj.TryGetValue(key, out var v) ? (v as List<object>) : null;
        }

        public static bool TopBool(object dom, string key)
        {
            var obj = dom as Dictionary<string, object>;
            if (obj == null) return false;
            return obj.TryGetValue(key, out var v) && (v is bool) && (bool)v;
        }

        /* buf_json 语义的转义输出：仅转义 " \ 和控制字符，其余（含多字节
           UTF-8）原样通过，由响应编码层统一落为 UTF-8 字节。 */
        public static string Escape(string s)
        {
            if (s == null) return "";
            var sb = new StringBuilder(s.Length + 8);
            sb.Append('"');
            foreach (char c in s) {
                if (c == '"') sb.Append("\\\"");
                else if (c == '\\') sb.Append("\\\\");
                else if (c == '\n') sb.Append("\\n");
                else if (c == '\r') sb.Append("\\r");
                else if (c == '\t') sb.Append("\\t");
                else if (c < 0x20) sb.Append("\\u").Append(((int)c).ToString("x4"));
                else sb.Append(c);
            }
            sb.Append('"');
            return sb.ToString();
        }
    }
}
