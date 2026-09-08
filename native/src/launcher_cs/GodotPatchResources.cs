using System;
using System.Collections.Generic;
using System.IO;
using System.Text;

namespace DstLauncher
{
    /*
     * GodotPatchResources —— godot_patch.c 里"单个资源如何被改写"的部分：
     * .tscn/.tres/.json/对话 .md 的引号跨度替换、GDScript 字节码（GDSC v13）常量池
     * 重写、OptimizedTranslation（.translation）字符串堆重建。
     *
     * 三者的共同约定：先收集原文与其在原缓冲中的字节跨度，一次性批量翻译，再按
     * 跨度逐段拼出新资源；任何一段译文不合格就原样保留该段字节，整份资源没有一处
     * 被替换时返回 null（调用方据此不写回）。
     */
    public static partial class GodotPatch
    {
        /* ======================== 文本资源（.tscn/.tres/.json/.md） ======================== */

        private static int KeyNormChar(char c)
        {
            if (c >= 'A' && c <= 'Z') return c - 'A' + 'a';
            if (c >= 'a' && c <= 'z') return c;
            if (c >= '0' && c <= '9') return c;
            return 0;
        }

        /* godot_key_equals_normalized：忽略大小写与所有非字母数字字符后比较。 */
        private static bool KeyEqualsNormalized(string key, string want)
        {
            int i = 0, j = 0;
            while (ByteStr.At(key, i) != 0 || ByteStr.At(want, j) != 0) {
                int a = 0, b = 0;
                while (ByteStr.At(key, i) != 0 && (a = KeyNormChar(key[i])) == 0) i++;
                while (ByteStr.At(want, j) != 0 && (b = KeyNormChar(want[j])) == 0) j++;
                if (a == 0 || b == 0) return a == b;
                if (a != b) return false;
                i++;
                j++;
            }
            return true;
        }

        private static readonly string[] DisplayTextKeys = {
            "text", "disabledtext", "displaytext", "bbcodetext", "placeholdertext",
            "tooltiptext", "hinttooltip", "dialogtext", "title", "windowtitle",
            "message", "caption", "description", "desc", "shortdescription",
            "longdescription", "body", "content", "prompt", "choice", "buttontext",
            "label", "name", "editorname", "displayname", "flavortext", "summary",
            "kin", "traittype", "groupname", "category"
        };

        private static bool DisplayTextKey(string key)
        {
            if (string.IsNullOrEmpty(key)) return false;
            foreach (string k in DisplayTextKeys) {
                if (KeyEqualsNormalized(key, k)) return true;
            }
            return false;
        }

        /* dup_trimmed_key：去掉两端空格/制表；空或超过 128 字节返回 null。 */
        private static string DupTrimmedKey(string buf, int start, int end)
        {
            while (start < end && (buf[start] == ' ' || buf[start] == '\t')) start++;
            while (end > start && (buf[end - 1] == ' ' || buf[end - 1] == '\t')) end--;
            if (end <= start || end - start > 128) return null;
            return buf.Substring(start, end - start);
        }

        /* json_parse_string_range：解析并回报字面量在缓冲中的起止（含两端引号）。 */
        private static string JsonParseStringRange(string buf, ref int pp, out int start, out int end)
        {
            start = pp;
            end = pp;
            if (ByteStr.At(buf, pp) != '"') return null;
            int p = pp;
            string s = JsonParseString(buf, ref p);
            if (s == null) return null;
            end = p;
            pp = p;
            return s;
        }

        private static bool IsJsonKeyToken(string buf, int end)
        {
            return ByteStr.At(buf, SkipWs(buf, end)) == ':';
        }

        /* json_key_before_value：从值的左引号回溯到 "key": 的键名。 */
        private static string JsonKeyBeforeValue(string buf, int quote)
        {
            int p = quote;
            while (p > 0 && (buf[p - 1] == ' ' || buf[p - 1] == '\t' || buf[p - 1] == '\r' || buf[p - 1] == '\n')) p--;
            if (p <= 0 || buf[p - 1] != ':') return null;
            p--;
            while (p > 0 && (buf[p - 1] == ' ' || buf[p - 1] == '\t' || buf[p - 1] == '\r' || buf[p - 1] == '\n')) p--;
            if (p <= 0 || buf[p - 1] != '"') return null;

            int keyEndQuote = p - 1;
            int keyStart = keyEndQuote;
            while (keyStart > 0) {
                keyStart--;
                if (buf[keyStart] != '"') continue;
                int slashes = 0;
                int s = keyStart;
                while (s > 0 && buf[s - 1] == '\\') {
                    slashes++;
                    s--;
                }
                if ((slashes % 2) == 0) break;
            }
            if (ByteStr.At(buf, keyStart) != '"') return null;
            int cursor = keyStart;
            string key = JsonParseString(buf, ref cursor);
            if (key == null || cursor != keyEndQuote + 1) return null;
            return key;
        }

        /* scene_property_key_before_quote：.tscn/.tres 的 key = "..." 属性名。 */
        private static bool ScenePropertyKeyBeforeQuote(string buf, int quote, out string key)
        {
            key = null;
            int line = quote;
            while (line > 0 && buf[line - 1] != '\n' && buf[line - 1] != '\r') line--;
            if (ByteStr.At(buf, line) == '[') return false;

            int eq = -1;
            for (int p = line; p < quote; p++) {
                if (buf[p] == '=') eq = p;
            }
            if (eq < 0) return false;
            string candidate = DupTrimmedKey(buf, line, eq);
            if (candidate == null) return false;
            if (!DisplayTextKey(candidate)) return false;
            key = candidate;
            return true;
        }

        /* collect_json_text_patches：只修补显示类键的字符串值（键本身不动）。 */
        private static void CollectJsonTextPatches(string buf, int size, StrList texts, TextPatchList patches)
        {
            int p = 0;
            while (p < size && buf[p] != 0) {
                if (buf[p] != '"') {
                    p++;
                    continue;
                }
                int cursor = p;
                int start, end;
                string value = JsonParseStringRange(buf, ref cursor, out start, out end);
                if (value == null) {
                    p++;
                    continue;
                }
                if (!IsJsonKeyToken(buf, end)) {
                    string key = JsonKeyBeforeValue(buf, start);
                    if (key != null && DisplayTextKey(key) && WantedSourceText(value)) {
                        int idx = texts.PushUniqueIndex(value);
                        patches.Push((uint)start, (uint)end, idx);
                    }
                }
                p = cursor;
            }
        }

        private static void CollectSceneTextPatches(string buf, int size, StrList texts, TextPatchList patches)
        {
            int p = 0;
            while (p < size && buf[p] != 0) {
                if (buf[p] != '"') {
                    p++;
                    continue;
                }
                int cursor = p;
                int start, end;
                string value = JsonParseStringRange(buf, ref cursor, out start, out end);
                if (value == null) {
                    p++;
                    continue;
                }
                string key;
                if (ScenePropertyKeyBeforeQuote(buf, start, out key) && WantedSourceText(value)) {
                    int idx = texts.PushUniqueIndex(value);
                    patches.Push((uint)start, (uint)end, idx);
                }
                p = cursor;
            }
        }

        /* collect_markdown_text_patches：面向对话的 Markdown 只替换普通正文行，
           围栏代码、引用、标题、分隔线、含反引号或方括号的行逐字节保留。 */
        private static void CollectMarkdownTextPatches(string buf, int size, StrList texts, TextPatchList patches)
        {
            int line = 0;
            bool fenced = false;
            if (size >= 3 && (byte)buf[0] == 0xef && (byte)buf[1] == 0xbb && (byte)buf[2] == 0xbf) line = 3;
            while (line < size) {
                int lineEnd = line;
                while (lineEnd < size && buf[lineEnd] != '\r' && buf[lineEnd] != '\n') lineEnd++;
                int start = line;
                int end = lineEnd;
                while (start < end && (buf[start] == ' ' || buf[start] == '\t')) start++;
                while (end > start && (buf[end - 1] == ' ' || buf[end - 1] == '\t')) end--;
                int len = end - start;
                bool fenceLine = len >= 3 &&
                    (string.CompareOrdinal(buf, start, "```", 0, 3) == 0 ||
                     string.CompareOrdinal(buf, start, "~~~", 0, 3) == 0);
                if (fenceLine) {
                    fenced = !fenced;
                } else if (!fenced && len > 0 && buf[start] != '#' && buf[start] != '>' &&
                           !(buf[start] == '[' && len > 1 && buf[end - 1] == ']') &&
                           !(len == 3 && (string.CompareOrdinal(buf, start, "---", 0, 3) == 0 ||
                                          string.CompareOrdinal(buf, start, "***", 0, 3) == 0 ||
                                          string.CompareOrdinal(buf, start, "___", 0, 3) == 0)) &&
                           buf.IndexOf('`', start, len) < 0 && buf.IndexOf('[', start, len) < 0) {
                    string value = buf.Substring(start, len);
                    if (WantedSourceText(value)) {
                        int idx = texts.PushUniqueIndex(value);
                        patches.PushRaw((uint)start, (uint)end, idx);
                    }
                }
                if (lineEnd >= size) break;
                line = lineEnd + 1;
                if (buf[lineEnd] == '\r' && line < size && buf[line] == '\n') line++;
            }
        }

        /* build_text_resource_override：按跨度拼出新资源；没有任何一处被替换返回 null。 */
        private static byte[] BuildTextResourceOverride(string buf, int size, TextPatchList patches,
                                                        string[] translations, StrList texts,
                                                        out int patchedStrings)
        {
            patchedStrings = 0;
            var outBuf = new StringBuilder(size + 64);
            uint last = 0;
            for (int i = 0; i < patches.Count; i++) {
                TextPatch p = patches.V[i];
                if (p.Start < last || p.End > (uint)size) return null;
                outBuf.Append(buf, (int)last, (int)(p.Start - last));
                string source = p.SourceIndex >= 0 ? texts.V[p.SourceIndex] : null;
                string translated = p.SourceIndex >= 0 ? translations[p.SourceIndex] : null;
                if (translated != null && WantedTranslationText(translated) &&
                    TextTranslationPreservesFormat(source, translated) &&
                    (!p.RawText || (translated.IndexOf('\r') < 0 && translated.IndexOf('\n') < 0))) {
                    string wrapped = WrapCjkTranslation(translated);
                    string finalText = wrapped ?? translated;
                    if (p.RawText) outBuf.Append(finalText);
                    else AppendJsonString(outBuf, finalText);
                    patchedStrings++;
                } else {
                    outBuf.Append(buf, (int)p.Start, (int)(p.End - p.Start));
                }
                last = p.End;
            }
            outBuf.Append(buf, (int)last, size - (int)last);
            if (patchedStrings == 0) return null;
            return ByteStr.ToBytes(outBuf.ToString());
        }

        /* build_text_resource_patch：收集 → 批量翻译 → 重建。返回 null 表示不写回。 */
        internal static byte[] BuildTextResourcePatch(string path, byte[] raw, int size, PatchHttp http,
                                                      ref int liveUsed, out int patchedStrings)
        {
            patchedStrings = 0;
            if (path == null || raw == null || size == 0 || !ValidUtf8Payload(raw, 0, size)) return null;
            string buf = ByteStr.FromBytes(raw, 0, size);
            var texts = new StrList();
            var patches = new TextPatchList();
            if (AsciiEndsWithI(path, ".json")) CollectJsonTextPatches(buf, size, texts, patches);
            else if (AsciiEndsWithI(path, ".md")) CollectMarkdownTextPatches(buf, size, texts, patches);
            else CollectSceneTextPatches(buf, size, texts, patches);
            if (texts.Count == 0 || patches.Count == 0) return null;

            var translations = new string[texts.Count];
            TranslateStrings(http, texts, translations, ref liveUsed);
            return BuildTextResourceOverride(buf, size, patches, translations, texts, out patchedStrings);
        }

        /* ======================== GDScript 字节码（GDSC v13） ======================== */

        private static bool GdscriptExact(string s, string[] values)
        {
            if (s == null) return false;
            foreach (string v in values) {
                if (string.Equals(s, v, StringComparison.Ordinal)) return true;
            }
            return false;
        }

        private static readonly string[] ScenarioActionDataAllowed = {
            "Sub Actions: %s", "Special Effects: %s\n", "SP: %d", "HP: %d",
            "Maximum Uses: %d", "Cooldown: %d", "Type: ", "Weapon", "Passive %s",
            "Target Type: %s\t", "Target Type: Empty Space\t", "Target Range: ",
            "Hit Range: -", "Hit Range: ", "Teleport Range: -", "Teleport Range: ",
            "Element: %s", "Resist: %s", "Hits: %d", "Hits: %d-%d", "Apply: "
        };

        private static readonly string[] TargetTypeAllowed = {
            "Unit", "Empty Space", "Any", "Self", "Ally", "Other Ally",
            "Enemy", "Anyone", "Anyone Else"
        };

        private static readonly string[] UnitStatAllowed = {
            "HP", "SP", "ATK", "DEF", "RES", "SKL", "SPD", "MOV", "JH",
            "Health Points", "Special Points", "Attack", "Defense", "Resistance",
            "Speed", "Skill", "Movement", "Jump Height"
        };

        /* wanted_gdscript_constant_text：字节码常量池混有信号名与资源键，按脚本路径
           使用白名单或对话式启发规则。 */
        private static bool WantedGdscriptConstantText(string path, string s)
        {
            if (path == null || s == null || !WantedSourceText(s)) return false;
            if (s[0] == '_' || s.IndexOf('_') >= 0) return false;
            if (s == "pressed" || s == "cancel_pressed" ||
                s == "confirm_pressed" || s == "ui_cancel" ||
                s == "stackable" || s == "normal_font" ||
                s == "RichTextLabel") {
                return false;
            }
            if (s.StartsWith("ERROR:", StringComparison.Ordinal)) return false;

            if (AsciiEndsWithI(path, "/in_game_editor/scripts/resource/scenario_action_data.gdc")) {
                return GdscriptExact(s, ScenarioActionDataAllowed);
            }
            if (AsciiEndsWithI(path, "/in_game_editor/scripts/static/target_type.gdc")) {
                return GdscriptExact(s, TargetTypeAllowed);
            }
            if (AsciiEndsWithI(path, "/in_game_editor/scripts/static/unit_stat.gdc")) {
                return GdscriptExact(s, UnitStatAllowed);
            }
            if (AsciiContainsI(path, "/scripts/menus/") || AsciiContainsI(path, "/scripts/scenario/")) {
                if (s.IndexOf('\n') >= 0 || s.IndexOf('?') >= 0 || s.IndexOf('!') >= 0) return true;
                if (s.IndexOf(' ') >= 0 && s.IndexOf('%') < 0) return true;
                if (AsciiEndsWithI(path, "/confirm_popup.gdc") && s == "Title") return true;
            }
            return false;
        }

        /* gdscript_variant_payload_size：非字符串常量的定长载荷。未知类型放弃整个文件。 */
        private static bool GdscriptVariantPayloadSize(uint type, out uint payloadSize)
        {
            switch (type) {
                case 0: payloadSize = 0; return true;
                case 1:
                case 2:
                case 3:
                case 16:
                case 17: payloadSize = 4; return true;
                case 5: payloadSize = 8; return true;
                case 6:
                case 9:
                case 10:
                case 14: payloadSize = 16; return true;
                case 7: payloadSize = 12; return true;
                case 8:
                case 11: payloadSize = 24; return true;
                case 12: payloadSize = 36; return true;
                case 13: payloadSize = 48; return true;
                default: payloadSize = 0; return false;
            }
        }

        private static bool CollectGdscriptBytecodePatches(string path, byte[] buf, uint size,
                                                           StrList texts, TextPatchList patches)
        {
            if (path == null || buf == null || size < 24 ||
                buf[0] != 'G' || buf[1] != 'D' || buf[2] != 'S' || buf[3] != 'C') return false;
            uint version = ReadU32Le(buf, 4);
            uint identifierCount = ReadU32Le(buf, 8);
            uint constantCount = ReadU32Le(buf, 12);
            if (version != 13 || identifierCount > 100000u || constantCount > 100000u) return false;

            uint pos = 24;
            for (uint i = 0; i < identifierCount; i++) {
                if (pos > size || size - pos < 4) return false;
                uint n = ReadU32Le(buf, (int)pos);
                pos += 4;
                if (n > size - pos) return false;
                pos += n;
            }

            for (uint i = 0; i < constantCount; i++) {
                if (pos > size || size - pos < 4) return false;
                uint typePos = pos;
                uint type = ReadU32Le(buf, (int)pos);
                pos += 4;
                if (type == 4) {
                    if (pos > size || size - pos < 4) return false;
                    uint n = ReadU32Le(buf, (int)pos);
                    pos += 4;
                    uint padded = n + VariantPad4(n);
                    if (padded < n || padded > size - pos) return false;
                    if (n > 0 && n <= MaxSourceTextBytes && ValidUtf8Payload(buf, (int)pos, (int)n)) {
                        string s = ByteStr.FromBytes(buf, (int)pos, (int)n);
                        if (WantedGdscriptConstantText(path, s)) {
                            int idx = texts.PushUniqueIndex(s);
                            patches.Push(typePos, pos + padded, idx);
                        }
                    }
                    pos += padded;
                } else {
                    uint payloadSize;
                    if (!GdscriptVariantPayloadSize(type, out payloadSize)) return false;
                    if (payloadSize > size - pos) return false;
                    pos += payloadSize;
                }
            }
            return true;
        }

        /* build_gdscript_bytecode_override：把常量整条（类型 + 长度 + 载荷 + 对齐）换成译文。 */
        private static byte[] BuildGdscriptBytecodeOverride(byte[] buf, uint size, TextPatchList patches,
                                                            string[] translations, StrList texts,
                                                            out int patchedStrings)
        {
            patchedStrings = 0;
            var outBuf = new MemoryStream((int)size + 64);
            uint last = 0;
            for (int i = 0; i < patches.Count; i++) {
                TextPatch p = patches.V[i];
                if (p.Start < last || p.End > size) return null;
                outBuf.Write(buf, (int)last, (int)(p.Start - last));
                string source = p.SourceIndex >= 0 ? texts.V[p.SourceIndex] : null;
                string translated = p.SourceIndex >= 0 ? translations[p.SourceIndex] : null;
                if (translated != null && WantedTranslationText(translated) &&
                    GdscriptTranslationPreservesFormatTokens(source, translated)) {
                    byte[] bytes = ByteStr.ToBytes(translated);
                    uint pad = VariantPad4((uint)bytes.Length);
                    var header = new byte[8];
                    WriteU32Le(header, 0, 4u);
                    WriteU32Le(header, 4, (uint)bytes.Length);
                    outBuf.Write(header, 0, 8);
                    outBuf.Write(bytes, 0, bytes.Length);
                    for (uint j = 0; j < pad; j++) outBuf.WriteByte(0);
                    patchedStrings++;
                } else {
                    outBuf.Write(buf, (int)p.Start, (int)(p.End - p.Start));
                }
                last = p.End;
            }
            outBuf.Write(buf, (int)last, (int)(size - last));
            if (patchedStrings == 0) return null;
            return outBuf.ToArray();
        }

        internal static byte[] BuildGdscriptBytecodePatch(string path, byte[] buf, uint size, PatchHttp http,
                                                          ref int liveUsed, out int patchedStrings)
        {
            patchedStrings = 0;
            var texts = new StrList();
            var patches = new TextPatchList();
            if (!CollectGdscriptBytecodePatches(path, buf, size, texts, patches) ||
                texts.Count == 0 || patches.Count == 0) {
                return null;
            }
            var translations = new string[texts.Count];
            TranslateStrings(http, texts, translations, ref liveUsed);
            return BuildGdscriptBytecodeOverride(buf, size, patches, translations, texts, out patchedStrings);
        }

        /* ======================== OptimizedTranslation（.translation） ======================== */

        private struct OptItem
        {
            public uint RecOff;
            public uint OldOffset;
            public uint CompSize;
            public uint UncompSize;
            public int SourceIndex;
        }

        /* find_packed_i32_property：属性索引 + 类型 0x20（PackedInt32Array）。 */
        private static bool FindPackedI32Property(byte[] buf, uint size, uint propIndex,
                                                  out uint dataOff, out uint count)
        {
            dataOff = 0;
            count = 0;
            for (uint i = 0; i + 12 <= size; i++) {
                if (ReadU32Le(buf, (int)i) != propIndex || ReadU32Le(buf, (int)i + 4) != 0x20u) continue;
                uint n = ReadU32Le(buf, (int)i + 8);
                ulong bytes = (ulong)n * 4u;
                if (n > 1000000u || i + 12u > size || bytes > size - (i + 12u)) continue;
                dataOff = i + 12u;
                count = n;
                return true;
            }
            return false;
        }

        /* find_packed_byte_property：属性索引 + 类型 0x1f（PackedByteArray，4 字节对齐）。 */
        private static bool FindPackedByteProperty(byte[] buf, uint size, uint propIndex,
                                                   out uint lenOff, out uint dataOff, out uint len)
        {
            lenOff = 0;
            dataOff = 0;
            len = 0;
            for (uint i = 0; i + 12 <= size; i++) {
                if (ReadU32Le(buf, (int)i) != propIndex || ReadU32Le(buf, (int)i + 4) != 0x1fu) continue;
                uint n = ReadU32Le(buf, (int)i + 8);
                uint data = i + 12u;
                ulong alignedEnd = (ulong)data + n + VariantPad4(n);
                if (n > MaxEntryBytes || alignedEnd > size) continue;
                lenOff = i + 8u;
                dataOff = data;
                len = n;
                return true;
            }
            return false;
        }

        /* dup_blob_string：去掉结尾的 NUL 后作为字节串。 */
        private static string DupBlobString(byte[] blob, uint blobStart, uint off, uint n)
        {
            if (n == 0) return null;
            if (blob[blobStart + off + n - 1] == 0) n--;
            return ByteStr.FromBytes(blob, (int)(blobStart + off), (int)n);
        }

        private static string DupOptimizedTranslationString(byte[] blob, uint blobStart, uint blobLen,
                                                            uint off, uint compSize, uint uncompSize)
        {
            if (compSize == 0 || off > blobLen || compSize > blobLen - off) return null;
            if (compSize == uncompSize) return DupBlobString(blob, blobStart, off, compSize);
            if (uncompSize == 0 || uncompSize > MaxEntryBytes) return null;
            var sb = new StringBuilder((int)uncompSize);
            int got = SmazDecompress(blob, (int)(blobStart + off), (int)compSize, sb, (int)uncompSize);
            if (got != (int)uncompSize) return null;
            return sb.ToString();
        }

        /* collect_optimized_translation_items：遍历哈希桶表，登记每条记录并收集原文。 */
        private static bool CollectOptimizedTranslationItems(byte[] buf, uint size, StrList texts,
                                                             List<OptItem> items,
                                                             out uint stringsLenOff, out uint stringsDataOff,
                                                             out uint stringsLen)
        {
            stringsLenOff = 0;
            stringsDataOff = 0;
            stringsLen = 0;
            uint bucketOff, bucketCount;
            if (!FindPackedI32Property(buf, size, 5u, out bucketOff, out bucketCount)) return false;
            if (!FindPackedByteProperty(buf, size, 6u, out stringsLenOff, out stringsDataOff, out stringsLen)) return false;

            uint pos = 0;
            while (pos + 2u <= bucketCount) {
                uint bucketSize = ReadU32Le(buf, (int)(bucketOff + (ulong)pos * 4u));
                if (bucketSize == 0 || bucketSize > 100000u) return false;
                ulong next = (ulong)pos + 2u + (ulong)bucketSize * 4u;
                if (next > bucketCount) return false;
                for (uint j = 0; j < bucketSize; j++) {
                    uint rec = bucketOff + (pos + 2u + j * 4u) * 4u;
                    uint strOff = ReadU32Le(buf, (int)rec + 4);
                    uint compSize = ReadU32Le(buf, (int)rec + 8);
                    uint uncompSize = ReadU32Le(buf, (int)rec + 12);
                    /* 跳过的记录会继续指向旧字符串堆，因此应放弃整个资源。 */
                    if (compSize > stringsLen || strOff > stringsLen - compSize) return false;
                    var item = new OptItem {
                        RecOff = rec,
                        OldOffset = strOff,
                        CompSize = compSize,
                        UncompSize = uncompSize,
                        SourceIndex = -1
                    };
                    if (compSize > 0) {
                        string s = DupOptimizedTranslationString(buf, stringsDataOff, stringsLen, strOff, compSize, uncompSize);
                        if (s != null) {
                            /* C 版之后只用 strlen 语义（valid_utf8_payload(s, strlen(s))、
                               wanted_source_text），内嵌 NUL 之后的字节不可见。 */
                            int nul = s.IndexOf('\0');
                            if (nul >= 0) s = s.Substring(0, nul);
                            if (ValidUtf8Payload(s) && WantedSourceText(s)) {
                                item.SourceIndex = texts.PushUniqueIndex(s);
                            }
                        }
                    }
                    items.Add(item);
                }
                pos = (uint)next;
            }
            return items.Count > 0;
        }

        /* build_optimized_translation_resource：重建字符串堆（译文以未压缩 UTF-8 写入，
           未翻译条目按原字节复制），并同步每条记录的偏移/长度与堆长度字段。 */
        private static byte[] BuildOptimizedTranslationResource(byte[] buf, uint size, List<OptItem> items,
                                                                string[] translations,
                                                                uint stringsLenOff, uint stringsDataOff,
                                                                uint oldStringsLen, out bool hasTranslated)
        {
            hasTranslated = false;
            var work = new byte[size];
            Buffer.BlockCopy(buf, 0, work, 0, (int)size);

            var strings = new MemoryStream();
            foreach (OptItem it in items) {
                uint newOff = (uint)strings.Length;
                string translated = it.SourceIndex >= 0 ? translations[it.SourceIndex] : null;
                if (translated != null && WantedTranslationText(translated)) {
                    string wrapped = WrapCjkTranslation(translated);
                    string finalText = wrapped ?? translated;
                    byte[] bytes = ByteStr.ToBytes(finalText);
                    strings.Write(bytes, 0, bytes.Length);
                    strings.WriteByte(0);
                    uint n = (uint)(bytes.Length + 1);
                    WriteU32Le(work, (int)it.RecOff + 4, newOff);
                    WriteU32Le(work, (int)it.RecOff + 8, n);
                    WriteU32Le(work, (int)it.RecOff + 12, n);
                    hasTranslated = true;
                } else {
                    if (it.OldOffset > oldStringsLen || it.CompSize > oldStringsLen - it.OldOffset) return null;
                    strings.Write(buf, (int)(stringsDataOff + it.OldOffset), (int)it.CompSize);
                    WriteU32Le(work, (int)it.RecOff + 4, newOff);
                    WriteU32Le(work, (int)it.RecOff + 8, it.CompSize);
                    WriteU32Le(work, (int)it.RecOff + 12, it.UncompSize);
                }
            }

            if (!hasTranslated || strings.Length > uint.MaxValue) return null;
            WriteU32Le(work, (int)stringsLenOff, (uint)strings.Length);

            uint oldTail = stringsDataOff + oldStringsLen + VariantPad4(oldStringsLen);
            if (oldTail > size) return null;
            uint newPad = VariantPad4((uint)strings.Length);
            ulong total = (ulong)stringsDataOff + (ulong)strings.Length + newPad + (size - oldTail);
            if (total > uint.MaxValue) return null;

            var outBuf = new byte[total];
            Buffer.BlockCopy(work, 0, outBuf, 0, (int)stringsDataOff);
            byte[] heap = strings.ToArray();
            Buffer.BlockCopy(heap, 0, outBuf, (int)stringsDataOff, heap.Length);
            /* newPad 处保持 0（new byte[] 已清零，与 C 版 memset 一致）。 */
            Buffer.BlockCopy(work, (int)oldTail, outBuf, (int)(stringsDataOff + heap.Length + newPad), (int)(size - oldTail));
            return outBuf;
        }

        /* patch_pack_file 里 .translation 分支的整段流程：解析 → 批量翻译 → 重建。 */
        internal static byte[] BuildOptimizedTranslationPatch(byte[] buf, uint size, PatchHttp http,
                                                              ref int liveUsed, out int patchedStrings)
        {
            patchedStrings = 0;
            var texts = new StrList();
            var items = new List<OptItem>();
            uint stringsLenOff, stringsDataOff, stringsLen;
            bool parsed = CollectOptimizedTranslationItems(buf, size, texts, items,
                                                           out stringsLenOff, out stringsDataOff, out stringsLen);
            if (!parsed || texts.Count == 0) return null;

            var translations = new string[texts.Count];
            TranslateStrings(http, texts, translations, ref liveUsed);
            bool hasTranslated;
            byte[] resource = BuildOptimizedTranslationResource(buf, size, items, translations,
                                                                stringsLenOff, stringsDataOff, stringsLen,
                                                                out hasTranslated);
            /* C 版按"拿到译文的原文条数"计数，而不是被替换的记录数。 */
            for (int j = 0; j < translations.Length; j++) {
                if (translations[j] != null) patchedStrings++;
            }
            return hasTranslated ? resource : null;
        }
    }
}
