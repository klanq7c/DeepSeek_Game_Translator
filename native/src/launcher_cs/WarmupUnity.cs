using System;
using System.IO;
using System.Text;
using DstCore;

namespace DstLauncher
{
    /*
     * Warmup —— Unity (Mono/IL2CPP) 预热（warmup.c 的 xunity_* / unity_* / warmup_xunity 部分）。
     *
     *   1. XUnity 翻译目录（BepInEx\Translation\zh-CN\Text 等）：已有译文进入 imports
     *      （/cache/import），空值或恒等条目查本地缓存——命中则回写文件（先备份一次），
     *      未命中只排队预热，原行原样保留；
     *   2. *_Data\ 下 resources.assets / sharedassets*.assets / level<N> /
     *      globalgamemanagers（u32 长度前缀 + ASCII）与 *.unity3d / *.bundle /
     *      *.assetbundle（流式扫描可打印片段），经严格的 Unity 文本过滤后排队；
     *   3. 等服务端就绪后先导入再预热。
     */
    public static partial class Warmup
    {
        /* should_warm_unity_asset_text：只接受含标点/空格的纯 ASCII，排除 Unity 内部命名。 */
        private static bool ShouldWarmUnityAssetText(string s)
        {
            if (!ShouldWarmText(s) || !AsciiOnly(s)) return false;
            if (!ByteStr.ContainsAny(s, " \t.?!,:;<>")) return false;
            if (s.Contains("Base Layer") || s.Contains(" -> ") ||
                s.Contains(".assets") || s.Contains(".resource") ||
                s.Contains("Atlas") || s.Contains("Material") ||
                s.Contains("SDF") ||
                s.Contains("DebugUI") || s.Contains(" Track") ||
                s.Contains("Scrollbar") || s.Contains("Sliding Area") ||
                s.Contains("Signal ") || s.Contains("Activation ") ||
                s.Contains("Animation ") || s.Contains("Cinemachine") ||
                s.Contains("Audio Track") || s.Contains("Override ") ||
                s.Contains(" Button") || s.Contains("Font") ||
                s.Contains("Texture") || s.Contains("Shader") ||
                s.Contains("Lightmap") || s.Contains("Sprite")) {
                return false;
            }
            int len = s.Length;
            if (len > 3 && s[len - 1] == ')' && s.Contains(" (")) return false;
            if (s.StartsWith("_", StringComparison.Ordinal) || s.StartsWith("{", StringComparison.Ordinal) ||
                s.StartsWith("<noninit>", StringComparison.Ordinal) ||
                s.StartsWith("Unity", StringComparison.Ordinal) || s.StartsWith("Render", StringComparison.Ordinal) ||
                s.StartsWith("Recorded", StringComparison.Ordinal) || s.StartsWith("LineBreaking", StringComparison.Ordinal)) {
                return false;
            }
            return true;
        }

        /* ---------------- XUnity 翻译文件 ---------------- */

        /* xunity_find_separator：第一个未转义的 '='。 */
        private static int XunityFindSeparator(string line)
        {
            bool escaped = false;
            for (int i = 0; i < line.Length; i++) {
                if (escaped) {
                    escaped = false;
                    continue;
                }
                if (line[i] == '\\') {
                    escaped = true;
                    continue;
                }
                if (line[i] == '=') return i;
            }
            return -1;
        }

        /* xunity_unescape：\n \r \t \= \\；未知转义保留反斜杠。 */
        private static string XunityUnescape(string s)
        {
            var b = new StringBuilder(s.Length + 1);
            for (int i = 0; i < s.Length; i++) {
                char c = s[i];
                if (c != '\\' || ByteStr.At(s, i + 1) == 0) {
                    b.Append(c);
                    continue;
                }
                i++;
                char e = s[i];
                if (e == 'n') b.Append('\n');
                else if (e == 'r') b.Append('\r');
                else if (e == 't') b.Append('\t');
                else if (e == '=' || e == '\\') b.Append(e);
                else {
                    b.Append('\\');
                    b.Append(e);
                }
            }
            return ByteStr.CStr(b);
        }

        /* xunity_escape_to */
        private static void XunityEscapeTo(StringBuilder b, string s)
        {
            for (int i = 0; s != null && i < s.Length; i++) {
                char c = s[i];
                if (c == '\n') b.Append("\\n");
                else if (c == '\r') b.Append("\\r");
                else if (c == '\t') b.Append("\\t");
                else if (c == '=' || c == '\\') {
                    b.Append('\\');
                    b.Append(c);
                } else {
                    b.Append(c);
                }
            }
        }

        /* backup_once：首次修改翻译文件前保留一份 .deepseek.bak；已有备份不覆盖。 */
        private static bool BackupOnce(string path)
        {
            const string suffix = ".deepseek.bak";
            if (path.Length + suffix.Length >= 260 * 4) {
                Log.Append("Unity warmup: backup path is too long; translation file was not modified.");
                return false;
            }
            string bak = path + suffix;
            if (SafeFs.CopyFileIfAbsentSafe(path, bak) || SafeFs.LastError == SafeFs.ERROR_FILE_EXISTS) {
                return true;
            }
            Log.Append("Unity warmup: could not create translation-file backup (Windows error " + SafeFs.LastError + ").");
            return false;
        }

        /* parse_translation_file：已有译文进 imports；空值/恒等条目查缓存回写或排队，原行保留。 */
        private static void ParseTranslationFile(string path, PairList imports, TextList prefetch)
        {
            byte[] buf = SafeFs.ReadBytes(path);
            if (buf == null) return;
            int size = buf.Length;
            int end = ByteStr.CStrLen(buf);

            var outText = new StringBuilder(size + 256);
            bool changed = false;
            int p = 0;
            if (size >= 3 && buf[0] == 0xef && buf[1] == 0xbb && buf[2] == 0xbf) {
                outText.Append((char)0xef).Append((char)0xbb).Append((char)0xbf);
                p += 3;
            }

            while (p < end) {
                int lineStart = p;
                while (p < end && buf[p] != '\r' && buf[p] != '\n') p++;
                int lineLen = p - lineStart;
                bool hadCr = ByteStr.At(buf, end, p) == '\r';
                if (hadCr) p++;
                bool hadLf = ByteStr.At(buf, end, p) == '\n';
                if (hadLf) p++;

                string work = ByteStr.FromBytes(buf, lineStart, lineLen);
                string line = ByteStr.Trim(work);
                bool keepOriginal = true;

                if (line.Length > 0 && !line.StartsWith("//", StringComparison.Ordinal) && line[0] != '#' && line[0] != ';') {
                    int eq = XunityFindSeparator(line);
                    if (eq >= 0) {
                        string key = XunityUnescape(line.Substring(0, eq));
                        string val = XunityUnescape(line.Substring(eq + 1));
                        if (key.Length > 0 && ShouldWarmText(key)) {
                            if (val.Length > 0 && key != val) {
                                imports.Add(key, val);
                            } else {
                                string cached;
                                if (LocalHttp.GetCachedTranslate(key, out cached) &&
                                    !string.IsNullOrEmpty(cached) && cached != key && ShouldWarmText(key)) {
                                    XunityEscapeTo(outText, key);
                                    outText.Append('=');
                                    XunityEscapeTo(outText, cached);
                                    if (hadCr) outText.Append('\r');
                                    if (hadLf) outText.Append('\n');
                                    imports.Add(key, cached);
                                    keepOriginal = false;
                                    changed = true;
                                } else {
                                    /* 缓存未命中：只排队待译。空值条目（key=）与恒等标记（key=key）
                                       的原行必须原样写回，不能从翻译文件中删除。 */
                                    prefetch.Add(key);
                                }
                            }
                        }
                    }
                }

                if (keepOriginal) {
                    outText.Append(work);
                    if (hadCr) outText.Append('\r');
                    if (hadLf) outText.Append('\n');
                }
            }

            if (changed) {
                byte[] bytes = ByteStr.ToBytes(outText.ToString());
                if (BackupOnce(path) && !SafeFs.WriteBytes(path, bytes, bytes.Length)) {
                    Log.Append("Unity warmup: could not update translation file (Windows error " + SafeFs.LastError + ").");
                }
            }
        }

        /* scan_text_dir：目录下的 *.txt，跳过 XUnity 自身的 _Substitutions/_Preprocessors/_Postprocessors。 */
        private static void ScanTextDir(string dir, PairList imports, TextList prefetch)
        {
            if (!PathUtil.IsDir(dir)) return;
            var files = Win32Find.EnumerateOrNull(dir, "*.txt");
            if (files == null) return;
            foreach (var fd in files) {
                if ((fd.Attributes & SafeFs.FILE_ATTRIBUTE_DIRECTORY) != 0) continue;
                if (PathUtil.EqualsNoCase(fd.Name, "_Substitutions.txt") ||
                    PathUtil.EqualsNoCase(fd.Name, "_Preprocessors.txt") ||
                    PathUtil.EqualsNoCase(fd.Name, "_Postprocessors.txt")) {
                    continue;
                }
                ParseTranslationFile(PathUtil.Join(dir, fd.Name), imports, prefetch);
            }
        }

        /* ---------------- Unity 资源文件 ---------------- */

        /* unity_asset_file_name：resources.assets / globalgamemanagers / sharedassets*.assets / level<N>。 */
        private static bool UnityAssetFileName(string name)
        {
            if (PathUtil.EqualsNoCase(name, "resources.assets") || PathUtil.EqualsNoCase(name, "globalgamemanagers")) {
                return true;
            }
            if (name.StartsWith("sharedassets", StringComparison.OrdinalIgnoreCase) && name.Contains(".assets")) return true;
            if (name.StartsWith("level", StringComparison.OrdinalIgnoreCase)) {
                if (name.Length == 5) return false;
                for (int i = 5; i < name.Length; i++) {
                    if (name[i] < '0' || name[i] > '9') return false;
                }
                return true;
            }
            return false;
        }

        private static bool UnityBundleFileName(string name)
        {
            return PathUtil.EndsWithNoCase(name, ".unity3d") ||
                   PathUtil.EndsWithNoCase(name, ".bundle") ||
                   PathUtil.EndsWithNoCase(name, ".assetbundle");
        }

        /* valid_ascii_payload：仅可打印 ASCII 与 \t \n \r，且至少一个字母。 */
        private static bool ValidAsciiPayload(byte[] b, int off, int n)
        {
            bool alpha = false;
            for (int i = 0; i < n; i++) {
                byte c = b[off + i];
                if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')) alpha = true;
                if (c == 9 || c == 10 || c == 13) continue;
                if (c < 32 || c > 126) return false;
            }
            return alpha;
        }

        /* keep_rich_tag：color/size/sprite/b/i（含闭合形式）保留，其余视为玩法标记。 */
        private static bool KeepRichTag(string s, int start, int n)
        {
            while (n > 0 && (s[start] == '/' || s[start] == ' ' || s[start] == '\t')) {
                start++;
                n--;
            }
            return (n >= 5 && ByteStr.RegionEqualsNoCase(s, start, "color", 5)) ||
                   (n >= 4 && ByteStr.RegionEqualsNoCase(s, start, "size", 4)) ||
                   (n >= 6 && ByteStr.RegionEqualsNoCase(s, start, "sprite", 6)) ||
                   (n == 1 && (s[start] == 'b' || s[start] == 'i'));
        }

        /* strip_gameplay_tags：移除非富文本 <tag>，保留富文本标签原样。 */
        private static string StripGameplayTags(string s)
        {
            var b = new StringBuilder(s.Length + 1);
            for (int p = 0; p < s.Length; p++) {
                if (s[p] != '<') {
                    b.Append(s[p]);
                    continue;
                }
                int end = s.IndexOf('>', p);
                if (end < 0 || end - p > 96) {
                    b.Append(s[p]);
                    continue;
                }
                if (KeepRichTag(s, p + 1, end - p - 1)) {
                    b.Append(s, p, end - p + 1);
                }
                p = end;
            }
            return b.ToString();
        }

        /* collect_unity_asset_text：原文与去玩法标签后的纯文本（不同时）都入队。 */
        private static void CollectUnityAssetText(string s, TextList prefetch)
        {
            string t = ByteStr.Trim(s);
            if (!ShouldWarmUnityAssetText(t)) return;
            prefetch.Add(t);

            if (t.IndexOf('<') >= 0) {
                string plain = ByteStr.Trim(StripGameplayTags(t));
                if (plain != t && ShouldWarmUnityAssetText(plain)) prefetch.Add(plain);
            }
        }

        private static bool UnityBundleTextByte(byte c)
        {
            return (c >= 32 && c <= 126) || c == '\t' || c == '\r' || c == '\n';
        }

        /* looks_like_unity_bundle_text：句子形态启发式（字母占比、首字母大写、单词大小写形态）。 */
        private static bool LooksLikeUnityBundleText(string text)
        {
            int len = text.Length;
            if (len < 4 || len > 500) return false;

            int letters = 0, lowercase = 0, visible = 0;
            bool hasAnglePair = text.IndexOf('<') >= 0 && text.IndexOf('>') >= 0;
            for (int i = 0; i < len; i++) {
                char c = text[i];
                if (IsAlpha(c)) {
                    letters++;
                    if (IsLower(c)) lowercase++;
                    visible++;
                    continue;
                }
                if (IsDigit(c)) {
                    visible++;
                    continue;
                }
                if (c == ' ' || c == '\t') continue;
                if (".,?!:;'\"-+&()[]*".IndexOf(c) >= 0) {
                    visible++;
                    continue;
                }
                if (hasAnglePair && "<>=/#".IndexOf(c) >= 0) {
                    visible++;
                    continue;
                }
                return false;
            }
            if (letters < 3 || lowercase < 2 || visible == 0 || letters * 2 < visible) return false;

            int start = 0;
            if (ByteStr.At(text, 0) == '-' && ByteStr.At(text, 1) == ' ') start += 2;
            while (ByteStr.At(text, start) != 0 && "\"'([".IndexOf(text[start]) >= 0) start++;
            char first = ByteStr.At(text, start);
            if (first < 'A' || first > 'Z') return false;

            for (int p = 0; p < len;) {
                if (!IsAlpha(text[p])) {
                    p++;
                    continue;
                }
                int wordStart = p;
                int wordLetters = 0, wordUpper = 0, wordLower = 0;
                while (p < len && (IsAlpha(text[p]) || text[p] == '\'')) {
                    if (IsUpper(text[p])) {
                        wordUpper++;
                        wordLetters++;
                    } else if (IsLower(text[p])) {
                        wordLower++;
                        wordLetters++;
                    }
                    p++;
                }
                if (wordLetters == 1 && text[wordStart] != 'A' && text[wordStart] != 'I' && text[wordStart] != 'a') return false;
                if (wordUpper > 0 && wordLower > 0 &&
                    (!IsUpper(text[wordStart]) || wordUpper != 1)) {
                    return false;
                }
            }
            return true;
        }

        /* collect_unity_bundle_segment：剥离高置信度的序列化残留（[k:v] 元数据前缀、句末紧跟的
           单个字段字节、项目符号行尾的单字节字段），再交给严格过滤。 */
        private static void CollectUnityBundleSegment(string segment, TextList prefetch)
        {
            string text = ByteStr.Trim(segment);
            if (text.Length == 0) return;

            for (;;) {
                if (ByteStr.At(text, 0) != '[') break;
                int close = text.IndexOf(']', 1);
                if (close < 0 || close > 96) break;
                bool metadata = false;
                for (int i = 1; i < close; i++) {
                    if (text[i] == ':') {
                        metadata = true;
                        break;
                    }
                }
                if (!metadata) break;
                text = ByteStr.Trim(text.Substring(close + 1));
            }
            if (text.Length == 0) return;

            int len = text.Length;
            int boundary = -1;
            for (int i = 0; i < len; i++) {
                if (text[i] == '.' || text[i] == '?' || text[i] == '!') boundary = i;
            }
            /* 句末标点后紧跟的单个可打印字节通常是下一个序列化字段，例如"Hi honey.0"或"What?(" */
            if (boundary >= 0 && boundary + 2 == len) {
                text = text.Substring(0, boundary + 1);
                len = boundary + 1;
            }
            /* 项目符号文本后直接追加的无分隔单字节字段（数字或反斜杠）。 */
            if (len > 4 && text[0] == '-' && text[1] == ' ' &&
                (IsDigit(text[len - 1]) || text[len - 1] == '\\') &&
                IsAlpha(text[len - 2])) {
                text = text.Substring(0, len - 1);
                len--;
            }
            if (!LooksLikeUnityBundleText(text)) return;
            CollectUnityAssetText(text, prefetch);
        }

        /* collect_unity_bundle_run：一段可打印连续片段按行拆分后逐段处理。 */
        private static void CollectUnityBundleRun(string run, TextList prefetch)
        {
            int line = 0;
            while (line < run.Length && !prefetch.Full) {
                int end = run.IndexOfAny(new[] { '\r', '\n' }, line);
                string seg = end < 0 ? run.Substring(line) : run.Substring(line, end - line);
                CollectUnityBundleSegment(seg, prefetch);
                if (end < 0) break;
                line = end + 1;
                while (line < run.Length && (run[line] == '\r' || run[line] == '\n')) line++;
            }
        }

        /* scan_unity_bundle_file：流式扫描前 256 MiB，只保留 ≤ 1200 字节的可打印连续片段。 */
        private static void ScanUnityBundleFile(string path, TextList prefetch)
        {
            FileStream file;
            try {
                file = new FileStream(path, FileMode.Open, FileAccess.Read, FileShare.Read, 1, FileOptions.None);
            } catch (IOException) {
                return;
            } catch (UnauthorizedAccessException) {
                return;
            }
            using (file) {
                long size;
                try {
                    size = file.Length;
                } catch (IOException) {
                    return;
                }
                if (size <= 0) return;
                ulong remaining = (ulong)size;
                if (remaining > UnityBundleScanMaxBytes) remaining = UnityBundleScanMaxBytes;

                var chunk = new byte[UnityBundleScanChunkBytes];
                var run = new StringBuilder(MaxTextBytes + 1);
                bool overflow = false;
                while (remaining > 0 && !prefetch.Full) {
                    int request = remaining > (ulong)UnityBundleScanChunkBytes ? UnityBundleScanChunkBytes : (int)remaining;
                    int read;
                    try {
                        read = file.Read(chunk, 0, request);
                    } catch (IOException) {
                        break;
                    }
                    if (read <= 0) break;
                    remaining -= (ulong)read;
                    for (int i = 0; i < read; i++) {
                        byte c = chunk[i];
                        if (UnityBundleTextByte(c)) {
                            if (!overflow && run.Length < MaxTextBytes) {
                                run.Append((char)c);
                            } else {
                                overflow = true;
                            }
                            continue;
                        }
                        if (!overflow && run.Length >= 2) {
                            CollectUnityBundleRun(run.ToString(), prefetch);
                        }
                        run.Length = 0;
                        overflow = false;
                        if (prefetch.Full) break;
                    }
                }
                if (!overflow && run.Length >= 2 && !prefetch.Full) {
                    CollectUnityBundleRun(run.ToString(), prefetch);
                }
            }
        }

        /* scan_unity_asset_file：遍历小端 u32 长度前缀 + ASCII 载荷（≤ 64 MiB 文件）。 */
        private static void ScanUnityAssetFile(string path, TextList prefetch)
        {
            if (!SafeFs.FileSizeAtMost(path, UnityAssetScanMaxBytes)) return;
            byte[] bytes = SafeFs.ReadBytes(path);
            if (bytes == null) return;
            long size = bytes.Length;
            for (long i = 0; i + 8 < size && !prefetch.Full; i++) {
                uint n = (uint)bytes[i] | ((uint)bytes[i + 1] << 8) | ((uint)bytes[i + 2] << 16) | ((uint)bytes[i + 3] << 24);
                if (n < 2 || n > MaxTextBytes || i + 4 + n > size) continue;
                if (!ValidAsciiPayload(bytes, (int)(i + 4), (int)n)) continue;
                CollectUnityAssetText(ByteStr.FromBytes(bytes, (int)(i + 4), (int)n), prefetch);
            }
        }

        /* scan_unity_data_dir：单个 *_Data 目录下的资源文件。 */
        private static void ScanUnityDataDir(string dataDir, TextList prefetch)
        {
            if (!PathUtil.IsDir(dataDir)) return;
            var entries = Win32Find.EnumerateOrNull(dataDir, "*");
            if (entries == null) return;
            foreach (var fd in entries) {
                if ((fd.Attributes & SafeFs.FILE_ATTRIBUTE_DIRECTORY) != 0) continue;
                string p = PathUtil.Join(dataDir, fd.Name);
                if (UnityAssetFileName(fd.Name)) {
                    ScanUnityAssetFile(p, prefetch);
                } else if (UnityBundleFileName(fd.Name)) {
                    ScanUnityBundleFile(p, prefetch);
                } else {
                    continue;
                }
                if (prefetch.Full) break;
            }
        }

        /* scan_unity_assets：所有 *_Data 目录。 */
        private static void ScanUnityAssets(string dir, TextList prefetch)
        {
            var entries = Win32Find.EnumerateOrNull(dir, "*_Data");
            if (entries == null) return;
            foreach (var fd in entries) {
                if ((fd.Attributes & SafeFs.FILE_ATTRIBUTE_DIRECTORY) == 0) continue;
                ScanUnityDataDir(PathUtil.Join(dir, fd.Name), prefetch);
                if (prefetch.Full) break;
            }
        }

        /* warmup_xunity：翻译目录 + 资源扫描，就绪后先导入再预热。 */
        private static void WarmupXunity(string dir)
        {
            var imports = new PairList();
            var prefetch = new TextList { MaxItems = UnityMaxItems };
            string[] rels = {
                "BepInEx\\Translation\\zh-CN\\Text",
                "Translation\\zh-CN\\Text",
                "AutoTranslator\\Translation\\zh-CN\\Text"
            };
            foreach (string rel in rels) {
                ScanTextDir(PathUtil.Join(dir, rel), imports, prefetch);
            }

            ScanUnityAssets(dir, prefetch);

            int imported = 0;
            int queued = 0;
            var http = new LocalHttp();
            if ((imports.Count > 0 || prefetch.Count > 0) && http.Open()) {
                if (http.WaitReady(8000)) {
                    for (int i = 0; i < imports.Count; i += BatchItems) {
                        int n = Math.Min(BatchItems, imports.Count - i);
                        if (http.Post(Contract.PathCacheImport, BuildImportBody(imports, i, n), 1500)) imported += n;
                    }
                    for (int i = 0; i < prefetch.Count; i += BatchItems) {
                        int n = Math.Min(BatchItems, prefetch.Count - i);
                        if (http.Post(Contract.PathPrefetch, BuildPrefetchBody(prefetch, i, n), 1500)) queued += n;
                    }
                } else {
                    Log.Append("预热跳过：本地服务端未及时就绪。");
                }
                http.Close();
            }

            if (imported > 0 || queued > 0) {
                Log.Append("预热翻译缓存：导入 " + imported + " 条，后台排队 " + queued + " 条。");
            }
        }
    }
}
