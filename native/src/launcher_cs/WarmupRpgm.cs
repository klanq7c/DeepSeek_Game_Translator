using System;
using System.Text;

namespace DstLauncher
{
    /*
     * Warmup —— RPG Maker MV/MZ 扫描（warmup.c 的 rpgm_* / scan_rpgm_* 部分）。
     *
     *   - data/*.json：事件命令 101/102/401/405 的 parameters、数据库文本字段
     *     （name/description/message1..4 等）；MapNNN.json 的 "name" 是编辑器标签，跳过；
     *     连续 401 行合并为多行消息块，与运行时显示形态一致。
     *   - 插件外部 TXT/CSV（Recipes.txt、quest/Quests.txt、game_messages.csv 等）：
     *     只读扫描，8 MiB 上限，跳过素材目录。
     *   - RPG Maker 控制码（\C[n] \N[n] \I[n] \FS[24] \\ 等）保留在缓存键里，只用可见
     *     文本做过滤；带前缀码的正文另采集一份去前缀版本。
     */
    public static partial class Warmup
    {
        /* should_warm_rpgm_text：验证并移除合法控制码后，用可见文本做通用判断。 */
        private static bool ShouldWarmRpgmText(string s)
        {
            int len = s.Length;
            if (len < 2 || len > MaxTextBytes) return false;
            var visible = new StringBuilder(len);
            for (int i = 0; i < len;) {
                if (s[i] != '\\') {
                    visible.Append(s[i++]);
                    continue;
                }

                int code = i + 1;
                if (code >= len) return false;
                if ("{}$.|!><^\\".IndexOf(s[code]) >= 0) {
                    visible.Append(' ');
                    i = code + 1;
                    continue;
                }

                int end = code;
                while (end < len && IsAlpha(s[end])) end++;
                int letters = end - code;
                bool singleKnown = letters == 1 && "CcNnVvPpIiGg".IndexOf(s[code]) >= 0;
                bool uppercaseCode = letters >= 2 && letters <= 8;
                for (int j = code; uppercaseCode && j < end; j++) {
                    if (s[j] < 'A' || s[j] > 'Z') uppercaseCode = false;
                }
                if (!singleKnown && !uppercaseCode) return false;

                if (end < len && s[end] == '[') {
                    int close = s.IndexOf(']', end + 1);
                    if (close < 0 || close - end > 64) return false;
                    end = close + 1;
                } else if (!singleKnown || "Gg".IndexOf(s[code]) < 0) {
                    return false;
                }

                visible.Append(' ');
                i = end;
            }
            return ShouldWarmText(visible.ToString());
        }

        /* rpgm_prefix_code：pop / n nc nr n1..n5 / nd ndc ndr nd1..5 / nt ntc ntr nt1..5（不区分大小写）。 */
        private static bool RpgmPrefixCode(string s, int start, int n)
        {
            if (n == 0 || n >= 8) return false;
            var buf = new char[n];
            for (int i = 0; i < n; i++) buf[i] = ByteStr.AsciiLower(s[start + i]);
            string code = new string(buf);
            if (code == "pop") return true;
            if (code == "n" || code == "nc" || code == "nr") return true;
            if (n == 2 && buf[0] == 'n' && buf[1] >= '1' && buf[1] <= '5') return true;
            if (code == "nd" || code == "ndc" || code == "ndr") return true;
            if (n == 3 && buf[0] == 'n' && buf[1] == 'd' && buf[2] >= '1' && buf[2] <= '5') return true;
            if (code == "nt" || code == "ntc" || code == "ntr") return true;
            if (n == 3 && buf[0] == 'n' && buf[1] == 't' && buf[2] >= '1' && buf[2] <= '5') return true;
            return false;
        }

        /* dup_rpgm_prefixed_body：跳过开头的说话人/弹窗前缀码与 <tag>，返回其后的正文（trim 后），
           没有前缀或正文为空时返回 null。 */
        private static string DupRpgmPrefixedBody(string s)
        {
            if (s == null) return null;
            int len = s.Length;
            int i = 0;
            bool moved = false;
            for (;;) {
                if (ByteStr.At(s, i) == '\\') {
                    int code = i + 1;
                    int end = code;
                    while (end < len && IsAlpha(s[end])) end++;
                    if (end < len && (ByteStr.At(s, code) == 'n' || ByteStr.At(s, code) == 'N') &&
                        s[end] >= '1' && s[end] <= '5') {
                        end++;
                    }
                    if (!RpgmPrefixCode(s, code, end - code)) break;
                    if (end < len && s[end] == '[') {
                        int close = s.IndexOf(']', end + 1);
                        if (close < 0 || close - end > 64) break;
                        i = close + 1;
                        moved = true;
                        continue;
                    }
                    if (end < len && s[end] == '<') {
                        int close = s.IndexOf('>', end + 1);
                        if (close < 0 || close - end > 64) break;
                        i = close + 1;
                        moved = true;
                        continue;
                    }
                    break;
                }
                if (ByteStr.At(s, i) == '<') {
                    int close = s.IndexOf('>', i + 1);
                    if (close < 0 || close - i > 64) break;
                    i = close + 1;
                    moved = true;
                    continue;
                }
                break;
            }
            if (!moved || i >= len) return null;
            string body = ByteStr.Trim(s.Substring(i));
            return body.Length == 0 ? null : body;
        }

        /* rpgm_text_key：含可译文本的数据库字段名。 */
        private static bool RpgmTextKey(string key)
        {
            switch (key) {
                case "name": case "nickname": case "description": case "profile":
                case "displayName": case "header": case "tech_description": case "basic":
                case "commands": case "params": case "elements": case "equipTypes":
                case "weaponTypes": case "armorTypes": case "skillTypes":
                case "message1": case "message2": case "message3": case "message4":
                    return true;
                default:
                    return false;
            }
        }

        /* rpgm_text_command：101 Show Text / 102 Show Choices / 401 续行 / 405 滚动文本。 */
        private static bool RpgmTextCommand(int code)
        {
            return code == 101 || code == 102 || code == 401 || code == 405;
        }

        /* collect_string：保留原始控制码作为缓存键；带前缀码时另采集去前缀正文。 */
        private static void CollectRpgmString(string s, TextList prefetch)
        {
            string t = ByteStr.Trim(s);
            if (ShouldWarmRpgmText(t)) prefetch.Add(t);
            string body = DupRpgmPrefixedBody(t);
            if (body != null && ShouldWarmRpgmText(body)) prefetch.Add(body);
        }

        /* collect_array_strings：采集 [...] 内所有字符串元素；后随冒号的对象键名跳过。 */
        private static void CollectArrayStrings(byte[] buf, int end, ref int pp, TextList prefetch)
        {
            int p = JsonWs(buf, end, pp);
            if (ByteStr.At(buf, end, p) != '[') return;
            int depth = 0;
            do {
                char c = ByteStr.At(buf, end, p);
                if (c == '[') {
                    depth++;
                    p++;
                } else if (c == ']') {
                    depth--;
                    p++;
                } else if (c == '"') {
                    string s = JsonStringAt(buf, end, ref p);
                    if (s != null) {
                        if (ByteStr.At(buf, end, JsonWs(buf, end, p)) != ':') CollectRpgmString(s, prefetch);
                    }
                } else {
                    p++;
                }
            } while (ByteStr.At(buf, end, p) != 0 && depth > 0);
            pp = p;
        }

        /* RpgmMessageBlock：连续 401 行拼成的多行消息（仅 ≥ 2 行时作为整体采集）。 */
        private sealed class RpgmMessageBlock
        {
            public StringBuilder Text;
            public bool Active;
            public int Lines;

            public void Clear()
            {
                Text = null;
                Active = false;
                Lines = 0;
            }

            public void Flush(TextList prefetch)
            {
                if (!Active) return;
                if (Lines > 1 && Text != null) CollectRpgmString(ByteStr.CStr(Text), prefetch);
                Clear();
            }

            public void Begin()
            {
                Clear();
                Text = new StringBuilder(128);
                Active = true;
            }

            public void Append(string line, TextList prefetch)
            {
                if (string.IsNullOrEmpty(line)) return;
                if (!Active) Begin();
                int n = line.Length;
                int extra = n + (Lines > 0 ? 1 : 0);
                if (Text.Length + extra > MaxTextBytes) {
                    Flush(prefetch);
                    if (n > MaxTextBytes) return;
                    Begin();
                }
                if (Lines > 0) Text.Append('\n');
                Text.Append(line);
                Lines++;
            }
        }

        /* collect_rpgm_command_parameters：101 只采集 MZ 的说话人名（顶层下标 4），401 的首个
           字符串进入消息块，其余文本命令采集全部字符串。 */
        private static void CollectRpgmCommandParameters(int code, byte[] buf, int end, ref int pp,
                                                         TextList prefetch, RpgmMessageBlock block)
        {
            int p = JsonWs(buf, end, pp);
            if (ByteStr.At(buf, end, p) != '[') return;
            int depth = 0;
            int stringIndex = 0;
            int elemIndex = 0; /* 顶层数组元素下标（含数字等非字符串元素） */
            do {
                char c = ByteStr.At(buf, end, p);
                if (c == '[') {
                    depth++;
                    p++;
                } else if (c == ']') {
                    depth--;
                    p++;
                } else if (c == ',' && depth == 1) {
                    elemIndex++;
                    p++;
                } else if (c == '"') {
                    string s = JsonStringAt(buf, end, ref p);
                    if (s != null) {
                        if (code == 401 && depth == 1 && stringIndex == 0) {
                            block.Append(s, prefetch);
                        }
                        if (code != 101 || (depth == 1 && elemIndex == 4)) {
                            CollectRpgmString(s, prefetch);
                        }
                        stringIndex++;
                    }
                } else {
                    p++;
                }
            } while (ByteStr.At(buf, end, p) != 0 && depth > 0);
            pp = p;
        }

        /* MapNNN.json（Map 后随纯数字编号）：其 "name" 是事件/地图的编辑器内部标签。 */
        private static bool RpgmMapDataFileName(string name)
        {
            if (name == null || name.Length < 3 || !name.StartsWith("Map", StringComparison.OrdinalIgnoreCase)) return false;
            int p = 3;
            if (p >= name.Length || name[p] < '0' || name[p] > '9') return false;
            while (p < name.Length && name[p] >= '0' && name[p] <= '9') p++;
            return PathUtil.EqualsNoCase(name.Substring(p), ".json");
        }

        /* parse_rpgm_json_file：线性扫描 key/value，不建完整 JSON 树。 */
        private static void ParseRpgmJsonFile(string path, TextList prefetch)
        {
            if (!SafeFs.FileSizeAtMost(path, RpgmJsonScanMaxBytes)) return;
            byte[] buf = SafeFs.ReadBytes(path);
            if (buf == null) return;
            int slash = Math.Max(path.LastIndexOf('\\'), path.LastIndexOf('/'));
            string baseName = slash >= 0 ? path.Substring(slash + 1) : path;
            bool isMapData = RpgmMapDataFileName(baseName);

            int end = ByteStr.CStrLen(buf);
            int p = 0;
            if (buf.Length >= 3 && buf[0] == 0xef && buf[1] == 0xbb && buf[2] == 0xbf) p = 3;
            int lastCode = -1;
            var block = new RpgmMessageBlock();

            while (p < end) {
                p = JsonWs(buf, end, p);
                if (ByteStr.At(buf, end, p) != '"') {
                    p++;
                    continue;
                }
                string key = JsonStringAt(buf, end, ref p);
                if (key == null) break;
                int v = JsonWs(buf, end, p);
                if (ByteStr.At(buf, end, v) != ':') {
                    p = v;
                    continue;
                }
                v = JsonWs(buf, end, v + 1);

                if (key == "code") {
                    lastCode = Atoi(buf, end, v);
                } else if (key == "parameters") {
                    if (lastCode == 101) {
                        block.Flush(prefetch);
                        block.Begin();
                    } else if (lastCode != 401) {
                        block.Flush(prefetch);
                    }
                    if (RpgmTextCommand(lastCode)) {
                        CollectRpgmCommandParameters(lastCode, buf, end, ref v, prefetch, block);
                    }
                    lastCode = -1;
                } else if (RpgmTextKey(key)) {
                    block.Flush(prefetch);
                    char vc = ByteStr.At(buf, end, v);
                    if (isMapData && key == "name") {
                        /* 地图事件名是编辑器内部标签：消费掉字符串值但不采集。 */
                        if (vc == '"') JsonStringAt(buf, end, ref v);
                    } else if (vc == '"') {
                        string s = JsonStringAt(buf, end, ref v);
                        if (s != null) CollectRpgmString(s, prefetch);
                    } else if (vc == '[') {
                        CollectArrayStrings(buf, end, ref v, prefetch);
                    }
                }
                p = v;
                if (prefetch.Full) break;
            }
            block.Flush(prefetch);
            block.Clear();
        }

        /* scan_rpgm_data_dir：<content_root>\data\*.json */
        private static void ScanRpgmDataDir(string contentRoot, TextList prefetch)
        {
            string data = PathUtil.Join(contentRoot, "data");
            if (!PathUtil.IsDir(data)) return;
            var files = Win32Find.EnumerateOrNull(data, "*.json");
            if (files == null) return;
            foreach (var fd in files) {
                if ((fd.Attributes & SafeFs.FILE_ATTRIBUTE_DIRECTORY) != 0) continue;
                ParseRpgmJsonFile(PathUtil.Join(data, fd.Name), prefetch);
                if (prefetch.Full) break;
            }
        }

        /* collect_rpgm_text_line：插件外部文本的一行。Galv Quest Log 的 <quest id:标题|难度|分类>
           只显示"标题"，因此单独提取标题（以及最后一个 " - " 之后的子标题）。 */
        private static void CollectRpgmTextLine(string line, TextList prefetch)
        {
            string text = ByteStr.Trim(line);
            if (text.Length == 0 || text[0] == '#') return;
            if (ByteStr.StartsWithNoCase(text, "<quest ")) {
                int colon = text.IndexOf(':', 7);
                int end = text.IndexOf('>', 7);
                if (colon < 0 || end < 0 || colon >= end) return;
                int pipe = text.IndexOf('|', colon + 1);
                if (pipe >= 0 && pipe < end) end = pipe;
                string title = text.Substring(colon + 1, end - colon - 1);
                CollectRpgmString(title, prefetch);
                /* C 版的 trim_ascii 原地把尾部空白置 NUL，后续 strstr 看到的是去尾后的缓冲。 */
                string search = ByteStr.TrimEnd(title);
                int suffix = -1;
                for (int dash = search.IndexOf(" - ", StringComparison.Ordinal); dash >= 0;
                     dash = search.IndexOf(" - ", dash + 3, StringComparison.Ordinal)) {
                    suffix = dash + 3;
                }
                if (suffix >= 0 && suffix < search.Length) CollectRpgmString(search.Substring(suffix), prefetch);
                return;
            }
            if (text[0] == '<') return; /* 结束标签和其他插件控制标签不是显示文本 */
            CollectRpgmString(text, prefetch);
        }

        /* scan_rpgm_text_file：逐行采集（\r、\n、\r\n 或 NUL 为分隔），8 MiB 上限。 */
        private static void ScanRpgmTextFile(string path, TextList prefetch)
        {
            if (!SafeFs.FileSizeAtMost(path, RpgmTextScanMaxBytes)) return;
            byte[] buf = SafeFs.ReadBytes(path);
            if (buf == null) return;
            int size = buf.Length;
            int line = 0;
            if (size >= 3 && buf[0] == 0xef && buf[1] == 0xbb && buf[2] == 0xbf) line = 3;
            for (int p = line; ; p++) {
                char c = ByteStr.At(buf, size, p);
                if (c != '\r' && c != '\n' && c != 0) continue;
                CollectRpgmTextLine(ByteStr.FromBytes(buf, line, p - line), prefetch);
                if (c == 0 || prefetch.Full) break;
                if (c == '\r' && ByteStr.At(buf, size, p + 1) == '\n') p++;
                line = p + 1;
            }
        }

        /* rpgm_csv_delimiter：按首条逻辑记录（尊重引号）统计 ; , \t，取最多者；并列取先者；都为 0 返回 0。 */
        private static char RpgmCsvDelimiter(byte[] buf, int start, int size)
        {
            var counts = new int[3];
            bool quoted = false;
            for (int p = start; p < size && buf[p] != 0; p++) {
                byte c = buf[p];
                if (c == '"') {
                    if (quoted && ByteStr.At(buf, size, p + 1) == '"') {
                        p++;
                    } else {
                        quoted = !quoted;
                    }
                    continue;
                }
                if (quoted) continue;
                if (c == '\r' || c == '\n') break;
                if (c == ';') counts[0]++;
                else if (c == ',') counts[1]++;
                else if (c == '\t') counts[2]++;
            }
            int best = 0;
            for (int i = 1; i < 3; i++) {
                if (counts[i] > counts[best]) best = i;
            }
            if (counts[best] == 0) return '\0';
            return best == 0 ? ';' : (best == 1 ? ',' : '\t');
        }

        /* 无空格无标点的短标识符（ID/key 列形态）。 */
        private static bool RpgmCsvShortId(string s)
        {
            int len = s.Length;
            if (len == 0 || len > 32) return false;
            for (int i = 0; i < len; i++) {
                char c = s[i];
                if (!(IsAlpha(c) || IsDigit(c) || c == '_')) return false;
            }
            return true;
        }

        /* en / en_US / en-US 等英语地域标记。 */
        private static bool RpgmCsvEnLocale(string t)
        {
            int n = t.Length;
            if (n < 2 || n > 8 || !ByteStr.StartsWithNoCase(t, "en")) return false;
            if (n == 2) return true;
            if (t[2] != '_' && t[2] != '-') return false;
            for (int i = 3; i < n; i++) {
                char c = t[i];
                if (!(IsAlpha(c) || IsDigit(c) || c == '_' || c == '-')) return false;
            }
            return true;
        }

        /* 表头列打分：English/en 地域 100，source/original/msgid 90，text/body 80。 */
        private static int RpgmCsvSourceColumnScore(string cell)
        {
            string t = ByteStr.Trim(cell);
            if (t.Length == 0) return 0;
            if (RpgmCsvEnLocale(t) || ByteStr.EqualsNoCase(t, "english")) return 100;
            if (ByteStr.EqualsNoCase(t, "source") || ByteStr.EqualsNoCase(t, "source_text") ||
                ByteStr.EqualsNoCase(t, "original") || ByteStr.EqualsNoCase(t, "msgid")) return 90;
            if (ByteStr.EqualsNoCase(t, "text") || ByteStr.EqualsNoCase(t, "display_text") ||
                ByteStr.EqualsNoCase(t, "body")) return 80;
            return 0;
        }

        private static void RpgmCsvCollectField(string field, int row, int col, ref int sourceCol, ref int bestScore,
                                                TextList prefetch)
        {
            if (row == 0) {
                int score = RpgmCsvSourceColumnScore(field);
                if (score > bestScore) {
                    bestScore = score;
                    sourceCol = col;
                }
                return;
            }
            if (sourceCol >= 0) {
                if (col == sourceCol) CollectRpgmString(field, prefetch);
            } else if (!RpgmCsvShortId(ByteStr.Trim(field))) {
                CollectRpgmString(field, prefetch);
            }
        }

        /* scan_rpgm_csv_file：RFC 4180 风格引号字段；表头识别源列，否则回退采集全部非短 ID 字段。 */
        private static void ScanRpgmCsvFile(string path, TextList prefetch)
        {
            if (!SafeFs.FileSizeAtMost(path, RpgmTextScanMaxBytes)) return;
            byte[] buf = SafeFs.ReadBytes(path);
            if (buf == null) return;
            int size = buf.Length;
            int start = 0;
            if (size >= 3 && buf[0] == 0xef && buf[1] == 0xbb && buf[2] == 0xbf) start = 3;
            char delimiter = RpgmCsvDelimiter(buf, start, size);
            if (delimiter == '\0') return;

            var field = new StringBuilder();
            bool quoted = false;
            int row = 0, col = 0;
            int sourceCol = -1;
            int bestScore = 0;
            for (int p = start; ; p++) {
                char ch = ByteStr.At(buf, size, p);
                if (quoted) {
                    if (ch == '"' && ByteStr.At(buf, size, p + 1) == '"') {
                        field.Append('"');
                        p++;
                    } else if (ch == '"') {
                        quoted = false;
                    } else if (ch == 0) {
                        RpgmCsvCollectField(field.ToString(), row, col, ref sourceCol, ref bestScore, prefetch);
                        break;
                    } else {
                        field.Append(ch);
                    }
                    continue;
                }
                if (ch == '"' && field.Length == 0) {
                    quoted = true;
                    continue;
                }
                if (ch == delimiter || ch == '\r' || ch == '\n' || ch == 0) {
                    RpgmCsvCollectField(field.ToString(), row, col, ref sourceCol, ref bestScore, prefetch);
                    field.Length = 0;
                    if (ch == delimiter) {
                        col++;
                    } else {
                        row++;
                        col = 0;
                    }
                    if (ch == '\r' && ByteStr.At(buf, size, p + 1) == '\n') p++;
                    if (ch == 0 || prefetch.Full) break;
                    continue;
                }
                field.Append(ch);
            }
        }

        private static bool SkipRpgmTextDirectory(string name)
        {
            return PathUtil.EqualsNoCase(name, "img") ||
                   PathUtil.EqualsNoCase(name, "audio") ||
                   PathUtil.EqualsNoCase(name, "fonts") ||
                   PathUtil.EqualsNoCase(name, "movies") ||
                   PathUtil.EqualsNoCase(name, "js") ||
                   PathUtil.EqualsNoCase(name, "save") ||
                   PathUtil.EqualsNoCase(name, "icon");
        }

        /* scan_rpgm_external_text_dir：递归（深度 ≤ 6）扫描 .txt / .csv，跳过素材目录。 */
        private static void ScanRpgmExternalTextDir(string dir, TextList prefetch, int depth)
        {
            if (depth > 6 || !PathUtil.IsDir(dir)) return;
            var entries = Win32Find.EnumerateOrNull(dir, "*");
            if (entries == null) return;
            foreach (var fd in entries) {
                string path = PathUtil.Join(dir, fd.Name);
                if ((fd.Attributes & SafeFs.FILE_ATTRIBUTE_DIRECTORY) != 0) {
                    if (!SkipRpgmTextDirectory(fd.Name)) {
                        ScanRpgmExternalTextDir(path, prefetch, depth + 1);
                    }
                } else if (PathUtil.EndsWithNoCase(fd.Name, ".txt")) {
                    ScanRpgmTextFile(path, prefetch);
                } else if (PathUtil.EndsWithNoCase(fd.Name, ".csv")) {
                    ScanRpgmCsvFile(path, prefetch);
                }
                if (prefetch.Full) break;
            }
        }

        /* warmup_scan_rpgm_resources：从 www/ 或扁平内容根扫描外部文本与 data/*.json。 */
        internal static void ScanRpgmResources(string dir, TextList prefetch)
        {
            if (string.IsNullOrEmpty(dir) || prefetch == null) return;
            string contentRoot = EngineDetector.RpgmContentRoot(dir);
            if (contentRoot == null) return;
            ScanRpgmExternalTextDir(contentRoot, prefetch, 0);
            ScanRpgmDataDir(contentRoot, prefetch);
        }

        /* warmup_rpgm */
        private static void WarmupRpgm(string dir)
        {
            var prefetch = new TextList { MaxItems = RpgmMaxItems };
            ScanRpgmResources(dir, prefetch);
            int queued = PostPrefetchAll(prefetch);
            if (queued > 0) Log.Append("RPGM 预热翻译缓存：后台排队 " + queued + " 条。");
        }
    }
}
