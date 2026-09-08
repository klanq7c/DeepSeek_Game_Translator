using System;
using System.Text;

namespace DstCore
{
    /*
     * CacheCodec —— 翻译记忆 TSV 的行编解码与入库准入规则（与 cache.c 对齐）。
     *
     * 行格式：<base64(utf8(key))>\t<base64(utf8(value))>\n，标准 Base64（含填充）。
     * 准入规则（cache_set_* 与 cache_load 共用）：
     *   - 空键 / 空值拒绝；
     *   - 值先经 TextRules.NormalizeTranslationResult 剥提示词回显；
     *   - 剥离后为空、或与键同文（原文回显）拒绝——绝不把"没翻译"伪装成成功。
     * 共享核心：dst_server_cs 的 CacheStore 与后续 C# 启动器的缓存导入/导出都
     * 必须走这里，保证与 C 版 cache.c 的文件逐字节兼容。
     */
    public static class CacheCodec
    {
        /* 单条记录允许的最大原始行长；超过视为损坏记录跳过（防止畸形文件拖垮启动）。 */
        public const int MaxLineChars = 8 * 1024 * 1024;

        /* 返回可入库的规范化译文；不可入库返回 null。 */
        public static string NormalizeForStore(string key, string value)
        {
            if (string.IsNullOrEmpty(key) || string.IsNullOrEmpty(value)) return null;
            string clean = TextRules.NormalizeTranslationResult(value);
            if (string.IsNullOrEmpty(clean) || string.CompareOrdinal(clean, key) == 0) return null;
            return clean;
        }

        /* 编码一行（含结尾 \n）。调用方保证 key/value 已通过 NormalizeForStore。 */
        public static string EncodeLine(string key, string value)
        {
            var sb = new StringBuilder(((key.Length + value.Length) * 4) / 3 + 8);
            AppendLine(sb, key, value);
            return sb.ToString();
        }

        public static void AppendLine(StringBuilder sb, string key, string value)
        {
            sb.Append(Convert.ToBase64String(Encoding.UTF8.GetBytes(key)));
            sb.Append('\t');
            sb.Append(Convert.ToBase64String(Encoding.UTF8.GetBytes(value)));
            sb.Append('\n');
        }

        /* 解码一行（不含结尾换行）。返回 false 表示该行不是可用记录：
           无制表符、Base64 非法、超长、键/值为空或值规范化后不可入库。
           解码得到的 value 已经过 NormalizeForStore。 */
        public static bool TryDecodeLine(string line, out string key, out string value)
        {
            key = null;
            value = null;
            if (line == null || line.Length > MaxLineChars) return false;
            int tab = line.IndexOf('\t');
            if (tab < 0) return false;
            string k = DecodeB64(line.Substring(0, tab));
            string v = DecodeB64(line.Substring(tab + 1));
            string clean = NormalizeForStore(k, v);
            if (clean == null) return false;
            key = k;
            value = clean;
            return true;
        }

        private static string DecodeB64(string s)
        {
            try {
                return Encoding.UTF8.GetString(Convert.FromBase64String(s));
            } catch (FormatException) {
                /* 非法 Base64 视为空串，随后被准入规则拒绝；与 C 版 b64_decode 失败即跳过一致。 */
                return "";
            }
        }
    }
}
