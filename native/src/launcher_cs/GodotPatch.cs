using System;
using System.Collections.Generic;
using System.IO;
using System.Net;
using System.Text;
using DstCore;

namespace DstLauncher
{
    /*
     * GodotPatch —— godot_patch.c 的逐函数移植（Godot 外部资源补丁包构建）。
     *
     * 与 C 版职责相同：在游戏目录旁构建 dst_godot_patch.pck。原始 .pck 或 EXE 内嵌包
     * 先整段复制，再对复制品内的可翻译资源（.tscn/.tres/.json/对话 .md、英文
     * .translation、选定的 .gdc、CJK 字体条目）逐条改写；格式 3 的包还会追加启动器
     * 自有的运行时脚本/autoload/override.cfg 并重建目录表。原始游戏文件只读。
     *
     * 字节串约定与 Warmup 相同：文本扫描用 ByteStr（一个 char 一个字节，越界读为
     * NUL），二进制结构直接用 byte[]；只有写盘和 HTTP 请求体才还原为字节。
     *
     * 分文件：GodotPatch.cs（常量、PCK 头、文件 IO、路径分类、文本过滤、JSON、
     * /batch 会话），GodotPatchResources.cs（文本/GDScript/OptimizedTranslation 修补），
     * GodotPatchPack.cs（包发现、包内修补、运行时脚本嵌入、对外入口）。
     */
    public static partial class GodotPatch
    {
        public const uint PckMagic = 0x43504447u;               /* "GDPC" 小端 */
        public const uint PckV1HeaderSize = 88u;
        public const uint PckV2HeaderSize = 100u;
        public const uint PckV3HeaderSize = 112u;
        public const uint MaxFiles = 200000u;
        public const uint MaxPath = 4096u;
        public const uint MaxEntryBytes = 2u * 1024u * 1024u;
        public const uint MaxFontBytes = 32u * 1024u * 1024u;
        public const uint MaxFontReplacements = 16u;
        public const int LooseFontMinPriority = 20;
        public const uint LooseMaxDepth = 32u;
        public const int MaxStringsPerResource = 1800;
        public const int MaxSourceTextBytes = 2400;
        public const int MaxTranslationTextBytes = 4096;
        /* 限制首次运行补丁刷新的工作量；其余文本由预热排队，供后续仅缓存重建使用。 */
        public const int LiveLimit = 1024;
        public const int Batch = 128;
        public const ulong SmallDialogicBytes = 16u * 1024u;

        public const string PackName = "dst_godot_patch.pck";
        public const string NextName = "dst_godot_patch.next.pck";
        public const string BuildingName = "dst_godot_patch.building";
        public const string RuntimeScriptName = "dst_godot_runtime.gd";
        public const string RuntimeScriptPath = "dst_godot_runtime.gd";
        public const string AutoloadScriptPath = "dst_godot_autoload.gd";
        public const string AutoloadOverridePath = "override.cfg";
        public const string AutoloadOverrideSection = "[autoload_prepend]";
        public const string AutoloadOverrideKey = "DeepSeekTranslator";
        public const string AutoloadOverrideValue = "*res://dst_godot_autoload.gd";
        public const string PatchLauncherName = "dst_godot_patch.exe";
        public const string PatchLauncherMarker = "dst_godot_patch.exe.dst-owned";
        public const string PatchLauncherMarkerText = "DS_GODOT_PATCH_LAUNCHER_V1\n";

        internal const int EntryTranslation = 1;
        internal const int EntryTextResource = 2;
        internal const int EntryFont = 3;
        internal const int EntryGdscriptBytecode = 4;

        internal struct PckSource
        {
            public string Path;
            public ulong Offset;
            public ulong Size;
        }

        internal sealed class PckInfo
        {
            public uint Format;
            public uint EngineMajor;
            public uint EngineMinor;
            public uint HeaderSize;
            public uint EntryMetaSize;
            public ulong FileBase;
            public ulong DirectoryOffset;
            public ulong EntriesOffset;
            public uint FileCount;
            public bool OffsetsAreAbsolute;
        }

        internal sealed class PckEntry
        {
            public string Path;      /* 字节串（UTF-8 原样） */
            public ulong Rel;
            public ulong Size;
            public ulong MetaPos;
            public int Kind;
            public int FontPriority;
        }

        /* ======================== 小端读写 ======================== */

        internal static uint ReadU32Le(byte[] p, int off)
        {
            return (uint)p[off] | ((uint)p[off + 1] << 8) | ((uint)p[off + 2] << 16) | ((uint)p[off + 3] << 24);
        }

        internal static void WriteU32Le(byte[] p, int off, uint v)
        {
            p[off] = (byte)(v & 0xff);
            p[off + 1] = (byte)((v >> 8) & 0xff);
            p[off + 2] = (byte)((v >> 16) & 0xff);
            p[off + 3] = (byte)((v >> 24) & 0xff);
        }

        internal static void WriteU64Le(byte[] p, int off, ulong v)
        {
            WriteU32Le(p, off, (uint)(v & 0xffffffffu));
            WriteU32Le(p, off + 4, (uint)(v >> 32));
        }

        internal static ulong ReadU64Le(byte[] p, int off)
        {
            return ReadU32Le(p, off) | ((ulong)ReadU32Le(p, off + 4) << 32);
        }

        /* godot_variant_pad4：补齐到 4 字节的填充数（本身对齐时为 0）。 */
        internal static uint VariantPad4(uint n)
        {
            uint extra = 4u - (n % 4u);
            return extra < 4u ? extra : 0u;
        }

        /* parse_pck_info：识别格式 1（Godot 3，绝对偏移）、2（file_base 相对）、
           3（目录表在 directory_offset）。 */
        internal static bool ParsePckInfo(byte[] header, uint headerBytes, ulong pckSize, PckInfo info)
        {
            info.Format = 0;
            info.EngineMajor = 0;
            info.EngineMinor = 0;
            info.HeaderSize = 0;
            info.EntryMetaSize = 0;
            info.FileBase = 0;
            info.DirectoryOffset = 0;
            info.EntriesOffset = 0;
            info.FileCount = 0;
            info.OffsetsAreAbsolute = false;
            if (headerBytes < PckV1HeaderSize || pckSize < PckV1HeaderSize) return false;
            if (ReadU32Le(header, 0) != PckMagic) return false;
            uint format = ReadU32Le(header, 4);
            info.EngineMajor = ReadU32Le(header, 8);
            info.EngineMinor = ReadU32Le(header, 12);
            if (format == 1) {
                info.Format = format;
                info.HeaderSize = PckV1HeaderSize;
                info.EntryMetaSize = 8u + 8u + 16u;
                info.FileBase = 0;
                info.DirectoryOffset = info.HeaderSize;
                info.EntriesOffset = info.DirectoryOffset;
                info.FileCount = ReadU32Le(header, 84);
                info.OffsetsAreAbsolute = true;
            } else if (format == 2) {
                if (headerBytes < PckV2HeaderSize || pckSize < PckV2HeaderSize) return false;
                info.Format = format;
                info.HeaderSize = PckV2HeaderSize;
                info.EntryMetaSize = 8u + 8u + 16u + 4u;
                info.FileBase = ReadU64Le(header, 24);
                info.DirectoryOffset = info.HeaderSize;
                info.EntriesOffset = info.DirectoryOffset;
                info.FileCount = ReadU32Le(header, 96);
                info.OffsetsAreAbsolute = false;
            } else if (format == 3) {
                if (headerBytes < PckV3HeaderSize || pckSize < PckV3HeaderSize) return false;
                info.Format = format;
                info.HeaderSize = PckV3HeaderSize;
                info.EntryMetaSize = 8u + 8u + 16u + 4u;
                info.FileBase = ReadU64Le(header, 24);
                info.DirectoryOffset = ReadU64Le(header, 32);
                if (info.DirectoryOffset > pckSize - 4u) return false;
                info.EntriesOffset = info.DirectoryOffset + 4u;
                info.OffsetsAreAbsolute = false;
            } else {
                return false;
            }
            return info.FileBase <= pckSize;
        }

        /* ======================== 文件句柄读写 ======================== */

        /* write_at_exact：定位后写满 size 字节。 */
        internal static bool WriteAtExact(FileStream h, ulong offset, byte[] buf, int size)
        {
            try {
                h.Seek((long)offset, SeekOrigin.Begin);
                h.Write(buf, 0, size);
                return true;
            } catch (IOException) {
                return false; /* SetFilePointerEx/WriteFile 失败 */
            }
        }

        /* read_at_exact：定位后读满 size 字节；提前 EOF 视为失败。 */
        internal static bool ReadAtExact(FileStream h, ulong offset, byte[] buf, int size)
        {
            if (offset > long.MaxValue) return false;
            try {
                h.Seek((long)offset, SeekOrigin.Begin);
                int done = 0;
                while (done < size) {
                    int got = h.Read(buf, done, size - done);
                    if (got <= 0) return false;
                    done += got;
                }
                return true;
            } catch (IOException) {
                return false;
            }
        }

        /* read_pck_info：读头（格式 3 还要到 directory_offset 处取条目数）。 */
        internal static bool ReadPckInfo(FileStream h, ulong baseOffset, ulong pckSize, PckInfo info)
        {
            if (h == null || pckSize < PckV1HeaderSize || info == null) return false;
            var header = new byte[PckV3HeaderSize];
            uint headerBytes = pckSize >= PckV3HeaderSize
                ? PckV3HeaderSize
                : (pckSize >= PckV2HeaderSize ? PckV2HeaderSize : PckV1HeaderSize);
            if (!ReadAtExact(h, baseOffset, header, (int)headerBytes) ||
                !ParsePckInfo(header, headerBytes, pckSize, info)) {
                return false;
            }
            if (info.Format == 3) {
                var count = new byte[4];
                if (baseOffset > ulong.MaxValue - info.DirectoryOffset ||
                    !ReadAtExact(h, baseOffset + info.DirectoryOffset, count, 4)) {
                    return false;
                }
                info.FileCount = ReadU32Le(count, 0);
            }
            return info.FileCount > 0 && info.FileCount <= MaxFiles;
        }

        /* append_file_bytes：追加到文件末尾，返回写入起点。 */
        internal static bool AppendFileBytes(FileStream h, byte[] buf, int size, out ulong offset)
        {
            offset = 0;
            try {
                long end = h.Seek(0, SeekOrigin.End);
                offset = (ulong)end;
                h.Write(buf, 0, size);
                return true;
            } catch (IOException) {
                return false;
            }
        }

        /* append_file_range：把同一文件内的一段复制到末尾。 */
        internal static bool AppendFileRange(FileStream h, ulong sourceOffset, ulong size, out ulong destOffset)
        {
            destOffset = 0;
            long end;
            try {
                end = h.Seek(0, SeekOrigin.End);
            } catch (IOException) {
                return false;
            }
            destOffset = (ulong)end;
            var buf = new byte[64 * 1024];
            ulong copied = 0;
            while (copied < size) {
                int chunk = size - copied > (ulong)buf.Length ? buf.Length : (int)(size - copied);
                if (!ReadAtExact(h, sourceOffset + copied, buf, chunk) ||
                    !WriteAtExact(h, (ulong)end + copied, buf, chunk)) {
                    return false;
                }
                copied += (ulong)chunk;
            }
            return true;
        }

        /* CreateFileW(GENERIC_READ, FILE_SHARE_READ|WRITE|DELETE)：打不开返回 null，不记日志。 */
        internal static FileStream OpenSharedRead(string path)
        {
            try {
                return new FileStream(path, FileMode.Open, FileAccess.Read, FileShare.ReadWrite | FileShare.Delete);
            } catch (IOException) {
                return null;
            } catch (UnauthorizedAccessException) {
                return null;
            } catch (ArgumentException) {
                return null; /* 非法路径字符：C 版 CreateFileW 同样失败 */
            }
        }

        /* append_file_from_path：把外部文件（字体，≤ 32 MiB）追加到包末尾。 */
        internal static bool AppendFileFromPath(FileStream dst, string src, out ulong offset, out ulong size)
        {
            offset = 0;
            size = 0;
            FileStream inFs = OpenSharedRead(src);
            if (inFs == null) return false;
            using (inFs) {
                long srcSize;
                try {
                    srcSize = inFs.Length;
                } catch (IOException) {
                    return false;
                }
                if (srcSize <= 0 || (ulong)srcSize > MaxFontBytes) return false;
                long end;
                try {
                    end = dst.Seek(0, SeekOrigin.End);
                } catch (IOException) {
                    return false;
                }
                offset = (ulong)end;
                size = (ulong)srcSize;
                var buf = new byte[64 * 1024];
                long copied = 0;
                while (copied < srcSize) {
                    int want = (int)Math.Min(buf.Length, srcSize - copied);
                    int got;
                    try {
                        got = inFs.Read(buf, 0, want);
                        if (got <= 0) return false;
                        dst.Write(buf, 0, got);
                    } catch (IOException) {
                        return false;
                    }
                    copied += got;
                }
                return true;
            }
        }

        /* ======================== ASCII 路径分类 ======================== */

        internal static bool AsciiEndsWithI(string s, string suffix)
        {
            if (s == null) return false;
            return s.Length >= suffix.Length &&
                   ByteStr.RegionEqualsNoCase(s, s.Length - suffix.Length, suffix, suffix.Length);
        }

        internal static bool AsciiContainsI(string s, string needle)
        {
            if (needle.Length == 0) return true;
            if (s == null) return false;
            for (int i = 0; i + needle.Length <= s.Length; i++) {
                if (ByteStr.RegionEqualsNoCase(s, i, needle, needle.Length)) return true;
            }
            return false;
        }

        private static bool EnglishTranslationPath(string path)
        {
            return AsciiEndsWithI(path, ".en.translation") ||
                   AsciiEndsWithI(path, ".en_us.translation") ||
                   AsciiEndsWithI(path, ".en-us.translation");
        }

        internal static bool MarkdownResourcePath(string path)
        {
            if (path == null || !AsciiEndsWithI(path, ".md")) return false;
            return AsciiContainsI(path, "lang/") ||
                   AsciiContainsI(path, "locale/") ||
                   AsciiContainsI(path, "localization/") ||
                   AsciiContainsI(path, "dialog") ||
                   AsciiContainsI(path, "story") ||
                   AsciiContainsI(path, "scenario") ||
                   AsciiContainsI(path, "conversation") ||
                   AsciiContainsI(path, "narrative");
        }

        private static bool TextResourcePath(string path)
        {
            if (path == null) return false;
            if (AsciiContainsI(path, "/.import/") || AsciiContainsI(path, "res://.import/")) return false;
            return AsciiEndsWithI(path, ".tscn") ||
                   AsciiEndsWithI(path, ".tres") ||
                   AsciiEndsWithI(path, ".json") ||
                   MarkdownResourcePath(path);
        }

        private static bool GdscriptBytecodePath(string path)
        {
            if (path == null || !AsciiEndsWithI(path, ".gdc")) return false;
            if (AsciiContainsI(path, "/.import/") || AsciiContainsI(path, "res://.import/")) return false;
            if (AsciiContainsI(path, "/scripts/menus/") || AsciiContainsI(path, "/scripts/scenario/")) return true;
            return AsciiEndsWithI(path, "/in_game_editor/scripts/resource/scenario_action_data.gdc") ||
                   AsciiEndsWithI(path, "/in_game_editor/scripts/static/target_type.gdc") ||
                   AsciiEndsWithI(path, "/in_game_editor/scripts/static/unit_stat.gdc");
        }

        private static string PatchBasename(string path)
        {
            if (path == null) return "";
            int start = 0;
            for (int i = 0; i < path.Length; i++) {
                if (path[i] == '/' || path[i] == '\\') start = i + 1;
            }
            return path.Substring(start);
        }

        private static readonly string[] FontSkipTokens = {
            "display", "decor", "title", "logo", "icon", "symbol", "emoji",
            "material", "awesome", "rocker", "script", "dingbat", "glyph",
            "cursor"
        };

        /* godot_patch_font_resource_priority：给 .ttf/.otf 打分，决定哪些字体条目值得
           换成本机 CJK 字体（0 表示不换）。 */
        internal static int FontResourcePriority(string path)
        {
            if (path == null) return 0;
            if (AsciiContainsI(path, "/.import/") || AsciiContainsI(path, "res://.import/")) return 0;
            if (!AsciiEndsWithI(path, ".ttf") && !AsciiEndsWithI(path, ".otf")) return 0;

            string name = PatchBasename(path);
            foreach (string skip in FontSkipTokens) {
                if (AsciiContainsI(name, skip)) return 0;
            }

            int score = 10;
            if (AsciiContainsI(path, "font")) score += 5;
            if (AsciiContainsI(name, "regular")) score += 80;
            if (AsciiContainsI(name, "normal") || AsciiContainsI(name, "book")) score += 70;
            if (AsciiContainsI(name, "default") || AsciiContainsI(name, "text")) score += 40;
            if (AsciiContainsI(name, "medium") ||
                AsciiContainsI(name, "bold") || AsciiContainsI(name, "italic") ||
                AsciiContainsI(name, "light") || AsciiContainsI(name, "semi") ||
                AsciiContainsI(name, "extra")) {
                score += 25;
            }
            if (AsciiContainsI(name, "sans") || AsciiContainsI(name, "arial") ||
                AsciiContainsI(name, "roboto") || AsciiContainsI(name, "noto")) {
                score += 20;
            }
            return score;
        }

        /* find_system_cjk_font：%WINDIR%\Fonts 下第一个存在的 CJK 字体。 */
        internal static string FindSystemCjkFont()
        {
            string windir = SafeFs.WindowsDirectory();
            if (string.IsNullOrEmpty(windir)) return null;
            string fontDir = PathUtil.Join(windir, "Fonts");
            if (string.IsNullOrEmpty(fontDir)) return null;
            foreach (string name in new[] { "simhei.ttf", "msyh.ttf", "msyh.ttc", "simsun.ttc" }) {
                string candidate = PathUtil.Join(fontDir, name);
                if (PathUtil.Exists(candidate)) return candidate;
            }
            return null;
        }

        private static bool StoryTextPath(string path)
        {
            return AsciiContainsI(path, "dialog") ||
                   AsciiContainsI(path, "cutscene") ||
                   AsciiContainsI(path, "scenario") ||
                   AsciiContainsI(path, "story") ||
                   AsciiContainsI(path, "quest") ||
                   AsciiContainsI(path, "mission") ||
                   AsciiContainsI(path, "tutorial") ||
                   AsciiContainsI(path, "conversation") ||
                   AsciiContainsI(path, "event");
        }

        private static bool StartupTextPath(string path)
        {
            return AsciiContainsI(path, "/prologue/") ||
                   AsciiContainsI(path, "/intro/") ||
                   AsciiContainsI(path, "tutorial");
        }

        private static bool LowValueToolingPath(string path)
        {
            return AsciiContainsI(path, "/editor/") ||
                   AsciiContainsI(path, "/editors/") ||
                   AsciiContainsI(path, "/debug/") ||
                   AsciiContainsI(path, "/test/") ||
                   AsciiContainsI(path, "/tests/");
        }

        private static bool UiDataPath(string path)
        {
            if (path == null || !AsciiContainsI(path, "/data/")) return false;
            return AsciiEndsWithI(path, "/skills.json") ||
                   AsciiEndsWithI(path, "/heroes.json") ||
                   AsciiEndsWithI(path, "/weapons.json") ||
                   AsciiEndsWithI(path, "/items.json") ||
                   AsciiEndsWithI(path, "/status_effects.json") ||
                   AsciiEndsWithI(path, "/date_traits.json") ||
                   AsciiEndsWithI(path, "/date_actions.json") ||
                   AsciiEndsWithI(path, "/conquest.json") ||
                   AsciiEndsWithI(path, "/general.json");
        }

        /* godot_patch_entry_priority：修补顺序（数字小的先做，先用完 live 额度）。 */
        internal static int EntryPriority(string path, ulong size)
        {
            bool dialogic = AsciiContainsI(path, "localization_dialogic/");
            bool smallDialogic = dialogic && size <= SmallDialogicBytes;
            bool json = AsciiEndsWithI(path, ".json");
            bool markdown = MarkdownResourcePath(path);
            bool sceneText = AsciiEndsWithI(path, ".tscn") || AsciiEndsWithI(path, ".tres");
            bool gdscriptBytecode = AsciiEndsWithI(path, ".gdc");
            int p = 50;
            if (markdown && (AsciiContainsI(path, "lang/en/") ||
                             AsciiContainsI(path, "locale/en/") ||
                             AsciiContainsI(path, "localization/en/"))) p = 3;
            else if (gdscriptBytecode && AsciiContainsI(path, "/scripts/menus/")) p = 4;
            else if (gdscriptBytecode && AsciiContainsI(path, "/scripts/scenario/")) p = 4;
            else if (gdscriptBytecode && AsciiContainsI(path, "scenario_action_data.gdc")) p = 4;
            else if (gdscriptBytecode && AsciiContainsI(path, "target_type.gdc")) p = 4;
            else if (gdscriptBytecode && AsciiContainsI(path, "unit_stat.gdc")) p = 4;
            else if (json && UiDataPath(path)) p = 5;
            else if ((json || sceneText) && StartupTextPath(path)) p = json ? 6 : 16;
            else if (dialogic) p = smallDialogic ? 8 : 10;
            else if (AsciiContainsI(path, "localization/")) p = 20;
            else if (markdown) p = StoryTextPath(path) ? 22 : 32;
            else if (json) p = StoryTextPath(path) ? 24 : 34;
            else if (sceneText) p = StoryTextPath(path) ? 38 : 62;

            /* 小型 Dialogic 场景或战斗时间线应优先于大型可重复内容包。 */
            if (!smallDialogic) {
                if (AsciiContainsI(path, "bond")) p += 80;
                if (AsciiContainsI(path, "negotation") || AsciiContainsI(path, "negotiation")) p += 70;
                if (AsciiContainsI(path, "waizatsuhi")) p += 70;
            }
            if ((json || sceneText) && AsciiContainsI(path, "/data/")) p -= 4;
            if ((json || sceneText) && LowValueToolingPath(path)) p += 40;
            return p;
        }

        /* compare_pck_entries_for_patch：优先级 → 大小 → 路径 strcmp。路径唯一，
           因此是全序，qsort 是否稳定都不影响结果。 */
        internal static int CompareEntriesForPatch(PckEntry a, PckEntry b)
        {
            int pa = EntryPriority(a.Path, a.Size);
            int pb = EntryPriority(b.Path, b.Size);
            if (pa != pb) return pa - pb;
            if (a.Size < b.Size) return -1;
            if (a.Size > b.Size) return 1;
            return Strcmp(a.Path, b.Path);
        }

        /* strcmp：按无符号字节比较（字节串里 char 就是 0..255 的字节）。 */
        internal static int Strcmp(string a, string b)
        {
            int n = Math.Min(a.Length, b.Length);
            for (int i = 0; i < n; i++) {
                if (a[i] != b[i]) return a[i] < b[i] ? -1 : 1;
            }
            if (a.Length == b.Length) return 0;
            return a.Length < b.Length ? -1 : 1;
        }

        /* ======================== 文本列表与补丁跨度 ======================== */

        /* StrList：单个资源内的待翻译原文（去重，上限 1800）。 */
        internal sealed class StrList
        {
            public readonly List<string> V = new List<string>();
            private readonly Dictionary<string, int> _index = new Dictionary<string, int>(StringComparer.Ordinal);

            public int Count { get { return V.Count; } }

            /* strlist_push_unique_index：已存在返回原下标；列表已满返回 -1（不能返回
               末尾下标，否则会套用无关字符串的译文）。 */
            public int PushUniqueIndex(string s)
            {
                if (string.IsNullOrEmpty(s)) return -1;
                int found;
                if (_index.TryGetValue(s, out found)) return found;
                if (V.Count >= MaxStringsPerResource) return -1;
                V.Add(s);
                _index[s] = V.Count - 1;
                return V.Count - 1;
            }
        }

        internal struct TextPatch
        {
            public uint Start;
            public uint End;
            public int SourceIndex;
            public bool RawText;
        }

        internal sealed class TextPatchList
        {
            public readonly List<TextPatch> V = new List<TextPatch>();
            public int Count { get { return V.Count; } }

            /* textpatch_push：source_index < 0（列表已满）或空跨度时静默跳过。 */
            public void Push(uint start, uint end, int sourceIndex)
            {
                if (sourceIndex < 0 || start >= end) return;
                V.Add(new TextPatch { Start = start, End = end, SourceIndex = sourceIndex, RawText = false });
            }

            public void PushRaw(uint start, uint end, int sourceIndex)
            {
                int before = V.Count;
                Push(start, end, sourceIndex);
                if (V.Count > before) {
                    TextPatch p = V[V.Count - 1];
                    p.RawText = true;
                    V[V.Count - 1] = p;
                }
            }
        }

        /* ======================== 文本过滤 ======================== */

        private static readonly UTF8Encoding StrictUtf8 = new UTF8Encoding(false, true);

        /* valid_utf8_payload：MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS) 的对等物
           ——过长编码、代理区、> U+10FFFF、截断序列都判为非法；长度 0 同样非法
           （C 版 need <= 0）。内嵌 NUL 字节是合法的。 */
        internal static bool ValidUtf8Payload(byte[] b, int start, int n)
        {
            if (n <= 0) return false;
            try {
                StrictUtf8.GetCharCount(b, start, n);
                return true;
            } catch (DecoderFallbackException) {
                return false;
            }
        }

        internal static bool ValidUtf8Payload(string byteStr)
        {
            byte[] b = ByteStr.ToBytes(byteStr);
            return ValidUtf8Payload(b, 0, b.Length);
        }

        /* has_cjk_utf8：解码后命中 U+4E00..U+9FFF。非法 UTF-8 时 C 版 MultiByteToWideChar
           不带 MB_ERR_INVALID_CHARS，会把坏字节替换为 U+FFFD，因此这里也用宽松解码。 */
        internal static bool HasCjkUtf8(string s)
        {
            string decoded;
            try {
                decoded = Encoding.UTF8.GetString(ByteStr.ToBytes(s));
            } catch (ArgumentException) {
                return false;
            }
            foreach (char c in decoded) {
                if (c >= 0x4e00 && c <= 0x9fff) return true;
            }
            return false;
        }

        /* Godot Translation 替换属于渲染器本地行为，此处不插入人为硬换行（C 版同样返回 NULL）。 */
        private static string WrapCjkTranslation(string s)
        {
            return null;
        }

        private static readonly string[] SourceFileSuffixes = {
            ".png", ".jpg", ".jpeg", ".webp", ".bmp", ".gif", ".svg", ".ogg", ".wav",
            ".mp3", ".webm", ".mp4", ".tscn", ".tres", ".res", ".stex", ".json",
            ".gd", ".gdc", ".pck", ".ttf", ".otf", ".fnt", ".import"
        };

        private static bool SourceLooksLikePathOrUrl(string s)
        {
            if (s == null) return false;
            if (s.StartsWith("res://", StringComparison.Ordinal) || s.Contains("://")) return true;
            if (s.Length > 0 && (s[0] == '/' || s[0] == '\\')) return true;
            if (s.IndexOf('/') < 0 && s.IndexOf('\\') < 0) return false;
            foreach (string suffix in SourceFileSuffixes) {
                if (AsciiEndsWithI(s, suffix)) return true;
            }
            return false;
        }

        /* wanted_source_text：值得送去翻译的原文。 */
        internal static bool WantedSourceText(string s)
        {
            if (s == null) return false;
            int n = s.Length;
            if (n < 2 || n > MaxSourceTextBytes) return false;
            if (HasCjkUtf8(s)) return false;
            if (s == "RSRC" || s == "OptimizedTranslation" ||
                s == "Translation" || s == "messages" ||
                s == "locale" || s == "strings" ||
                s == "resource_name") return false;
            if (s.StartsWith("res://", StringComparison.Ordinal) ||
                s.Contains(".translation") || s.Contains(".import")) return false;
            if (SourceLooksLikePathOrUrl(s)) return false;
            foreach (string suffix in SourceFileSuffixes) {
                if (AsciiEndsWithI(s, suffix)) return false;
            }
            int alpha = 0;
            for (int i = 0; i < n; i++) {
                char c = s[i];
                if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')) alpha++;
                if (c < 9 || (c > 13 && c < 32)) return false;
            }
            return alpha >= 2;
        }

        /* wanted_translation_text：可写回包内的译文。 */
        internal static bool WantedTranslationText(string s)
        {
            if (s == null) return false;
            int n = s.Length;
            if (n < 1 || n > MaxTranslationTextBytes) return false;
            if (s == "translation_unavailable" || s == "missing_text") return false;
            for (int i = 0; i < n; i++) {
                char c = s[i];
                if (c < 9 || (c > 13 && c < 32)) return false;
            }
            return true;
        }

        internal static int CountSubstr(string s, string needle)
        {
            if (s == null || needle == null || needle.Length == 0) return 0;
            int n = 0;
            for (int i = 0; i + needle.Length <= s.Length; ) {
                if (string.CompareOrdinal(s, i, needle, 0, needle.Length) == 0) {
                    n++;
                    i += needle.Length;
                } else {
                    i++;
                }
            }
            return n;
        }

        /* 静态文本资源与 GDScript 常量进入同一套 BBCode/格式化渲染器：括号必须配对，
           printf 风格令牌必须与原文一致，否则保留原字符串。 */
        internal static bool TextTranslationPreservesFormat(string source, string translated)
        {
            if (source == null || translated == null) return false;
            int brackets = 0;
            for (int i = 0; i < translated.Length; i++) {
                if (translated[i] == '[') brackets++;
                else if (translated[i] == ']') brackets--;
                if (brackets < 0) return false;
            }
            if (brackets != 0) return false;
            if (CountSubstr(source, "%s") != CountSubstr(translated, "%s")) return false;
            if (CountSubstr(source, "%d") != CountSubstr(translated, "%d")) return false;
            return true;
        }

        internal static bool GdscriptTranslationPreservesFormatTokens(string source, string translated)
        {
            if (source == null || translated == null) return false;
            if (source.IndexOf('%') >= 0 && translated.IndexOf('%') < 0) return false;
            foreach (string token in new[] { "%s", "%d", "%1.2f", "%%" }) {
                if (CountSubstr(source, token) != CountSubstr(translated, token)) return false;
            }
            return true;
        }

        /* ======================== JSON ======================== */

        /* bb_json_string：JSON 转义（字节串进，字节串出）。 */
        internal static void AppendJsonString(StringBuilder b, string s)
        {
            b.Append('"');
            for (int i = 0; s != null && i < s.Length; i++) {
                char c = s[i];
                if (c == '\\' || c == '"') {
                    b.Append('\\').Append(c);
                } else if (c == '\n') b.Append("\\n");
                else if (c == '\r') b.Append("\\r");
                else if (c == '\t') b.Append("\\t");
                else if (c < 32) b.Append("\\u").Append(((int)c).ToString("x4"));
                else b.Append(c);
            }
            b.Append('"');
        }

        private static int HexVal(char c)
        {
            if (c >= '0' && c <= '9') return c - '0';
            if (c >= 'a' && c <= 'f') return c - 'a' + 10;
            if (c >= 'A' && c <= 'F') return c - 'A' + 10;
            return -1;
        }

        private static void AppendUtf8Codepoint(StringBuilder b, uint cp)
        {
            if (cp < 0x80) {
                b.Append((char)cp);
            } else if (cp < 0x800) {
                b.Append((char)(0xc0 | (cp >> 6)));
                b.Append((char)(0x80 | (cp & 0x3f)));
            } else if (cp < 0x10000) {
                b.Append((char)(0xe0 | (cp >> 12)));
                b.Append((char)(0x80 | ((cp >> 6) & 0x3f)));
                b.Append((char)(0x80 | (cp & 0x3f)));
            } else {
                b.Append((char)(0xf0 | (cp >> 18)));
                b.Append((char)(0x80 | ((cp >> 12) & 0x3f)));
                b.Append((char)(0x80 | ((cp >> 6) & 0x3f)));
                b.Append((char)(0x80 | (cp & 0x3f)));
            }
        }

        /* json_parse_string：解析 JSON 字符串字面量；未闭合返回 null 且不移动游标。
           与 C 版一样保留结果中的 NUL 之后内容不可见的语义（调用方按字节串使用）。 */
        internal static string JsonParseString(string buf, ref int pp)
        {
            int p = pp;
            if (ByteStr.At(buf, p) != '"') return null;
            p++;
            var b = new StringBuilder(64);
            while (ByteStr.At(buf, p) != 0 && buf[p] != '"') {
                if (buf[p] == '\\') {
                    p++;
                    char e = ByteStr.At(buf, p);
                    if (e == 'n') { b.Append('\n'); p++; }
                    else if (e == 'r') { b.Append('\r'); p++; }
                    else if (e == 't') { b.Append('\t'); p++; }
                    else if (e == '"' || e == '\\' || e == '/') { b.Append(e); p++; }
                    else if (e == 'u' &&
                             HexVal(ByteStr.At(buf, p + 1)) >= 0 && HexVal(ByteStr.At(buf, p + 2)) >= 0 &&
                             HexVal(ByteStr.At(buf, p + 3)) >= 0 && HexVal(ByteStr.At(buf, p + 4)) >= 0) {
                        uint cp = (uint)((HexVal(buf[p + 1]) << 12) | (HexVal(buf[p + 2]) << 8) |
                                         (HexVal(buf[p + 3]) << 4) | HexVal(buf[p + 4]));
                        p += 5;
                        if (cp >= 0xd800 && cp <= 0xdbff) {
                            /* 高代理项只能与紧随其后的低代理项组合；孤立代理项编码为 U+FFFD。 */
                            uint lo = 0;
                            if (ByteStr.At(buf, p) == '\\' && ByteStr.At(buf, p + 1) == 'u' &&
                                HexVal(ByteStr.At(buf, p + 2)) >= 0 && HexVal(ByteStr.At(buf, p + 3)) >= 0 &&
                                HexVal(ByteStr.At(buf, p + 4)) >= 0 && HexVal(ByteStr.At(buf, p + 5)) >= 0) {
                                lo = (uint)((HexVal(buf[p + 2]) << 12) | (HexVal(buf[p + 3]) << 8) |
                                            (HexVal(buf[p + 4]) << 4) | HexVal(buf[p + 5]));
                            }
                            if (lo >= 0xdc00 && lo <= 0xdfff) {
                                cp = 0x10000u + ((cp - 0xd800u) << 10) + (lo - 0xdc00u);
                                p += 6;
                                AppendUtf8Codepoint(b, cp);
                            } else {
                                AppendUtf8Codepoint(b, 0xfffdu);
                            }
                        } else if (cp >= 0xdc00 && cp <= 0xdfff) {
                            AppendUtf8Codepoint(b, 0xfffdu);
                        } else {
                            AppendUtf8Codepoint(b, cp);
                        }
                    } else {
                        p++;
                    }
                } else {
                    b.Append(buf[p]);
                    p++;
                }
            }
            if (ByteStr.At(buf, p) != '"') return null;
            p++;
            pp = p;
            return ByteStr.CStr(b);
        }

        private static int SkipWs(string s, int p)
        {
            while (ByteStr.At(s, p) == ' ' || ByteStr.At(s, p) == '\r' ||
                   ByteStr.At(s, p) == '\n' || ByteStr.At(s, p) == '\t') p++;
            return p;
        }

        /* parse_results_array：取 "results" 数组里的字符串，最多 cap 条。 */
        internal static int ParseResultsArray(string json, string[] outResults, int cap)
        {
            int at = json.IndexOf("\"results\"", StringComparison.Ordinal);
            if (at < 0) return 0;
            int p = json.IndexOf('[', at);
            if (p < 0) return 0;
            p++;
            int n = 0;
            while (ByteStr.At(json, p) != 0 && n < cap) {
                p = SkipWs(json, p);
                if (ByteStr.At(json, p) == ']') break;
                if (ByteStr.At(json, p) == '"') {
                    outResults[n++] = JsonParseString(json, ref p);
                } else {
                    while (ByteStr.At(json, p) != 0 && json[p] != ',' && json[p] != ']') p++;
                }
                p = SkipWs(json, p);
                if (ByteStr.At(json, p) == ',') p++;
            }
            return n;
        }

        /* ======================== 本地服务端 /batch ======================== */

        /*
         * PatchHttp —— C 版 WinHttpOpen + WinHttpConnect(127.0.0.1, 19999) 的对等物。
         * WinHttpConnect 不建立 TCP 连接，因此 C 版 http_open 在服务端未运行时同样
         * 返回成功，失败只发生在单次 POST 上；这里保持一致（Open 永远成功）。
         */
        internal sealed class PatchHttp
        {
            private bool _open;

            public bool Open()
            {
                _open = true;
                return true;
            }

            public void Close()
            {
                _open = false;
            }

            /* http_post_batch：POST /batch，仅 200 且结果条数与请求相等才算成功。 */
            public bool PostBatch(string[] texts, int count, bool cacheOnly, string[] results)
            {
                if (!_open || count == 0) return false;
                var body = new StringBuilder(4096);
                body.Append("{\"texts\":[");
                for (int i = 0; i < count; i++) {
                    if (i > 0) body.Append(',');
                    AppendJsonString(body, texts[i]);
                }
                body.Append(cacheOnly ? "],\"cache_only\":true}" : "],\"cache_only\":false}");
                byte[] payload = ByteStr.ToBytes(body.ToString());

                int status;
                byte[] response;
                try {
                    var req = (HttpWebRequest)WebRequest.Create(Contract.DefaultBaseUrl + Contract.PathBatch);
                    req.Method = "POST";
                    req.ContentType = "application/json";
                    req.ContentLength = payload.Length;
                    /* C 版 WinHttpSetTimeouts(3000, 3000, 12000, 45000)：解析/连接/发送/接收。
                       .NET 只有请求超时与读写超时，取最长的接收超时。 */
                    req.Timeout = 45000;
                    req.ReadWriteTimeout = 45000;
                    req.Proxy = null;
                    using (Stream s = req.GetRequestStream()) s.Write(payload, 0, payload.Length);
                    using (var resp = (HttpWebResponse)req.GetResponse()) {
                        status = (int)resp.StatusCode;
                        response = ReadAll(resp);
                    }
                } catch (WebException ex) {
                    var resp = ex.Response as HttpWebResponse;
                    if (resp == null) return false; /* 连接失败/超时：与 WinHttpSendRequest 失败一致 */
                    using (resp) {
                        status = (int)resp.StatusCode;
                        response = ReadAll(resp);
                    }
                }
                if (status != 200 || response == null) return false;
                /* http_read_body 以 NUL 收尾后交给 strstr：正文里的 NUL 之后不可见。 */
                string json = ByteStr.FromBytes(response, 0, ByteStr.CStrLen(response));
                return ParseResultsArray(json, results, count) == count;
            }

            private static byte[] ReadAll(HttpWebResponse resp)
            {
                try {
                    using (Stream s = resp.GetResponseStream())
                    using (var ms = new MemoryStream()) {
                        if (s == null) return new byte[0];
                        var buf = new byte[8192];
                        int n;
                        while ((n = s.Read(buf, 0, buf.Length)) > 0) ms.Write(buf, 0, n);
                        return ms.ToArray();
                    }
                } catch (IOException) {
                    return null; /* 读取中途断开：C 版 http_read_body 会 break，正文不完整 */
                }
            }
        }

        /* godot_leading_rich_tag_span：行首 <f…> / <F…> 富文本标签的长度（不跨行，≤ 160）。 */
        private static int LeadingRichTagSpan(string s)
        {
            if (s == null || ByteStr.At(s, 0) != '<') return 0;
            char c1 = ByteStr.At(s, 1);
            if (c1 != 'f' && c1 != 'F') return 0;
            char c2 = ByteStr.At(s, 2);
            if (c2 != 0 && c2 != '>' && c2 != ' ' && c2 != '\t') return 0;
            for (int i = 3; ByteStr.At(s, i) != 0 && i < 160; i++) {
                if (s[i] == '\r' || s[i] == '\n') return 0;
                if (s[i] == '>') return i + 1;
            }
            return 0;
        }

        /* godot_dup_patch_query_text：查询时剥掉行首富文本标签，保证与运行时同一缓存键。 */
        private static string DupPatchQueryText(string source)
        {
            int prefix = LeadingRichTagSpan(source);
            if (prefix == 0 || ByteStr.At(source, prefix) == 0) return null;
            string body = source.Substring(prefix);
            return WantedSourceText(body) ? body : null;
        }

        /* godot_restore_patch_query_text：把剥掉的标签接回译文。 */
        private static string RestorePatchQueryText(string source, string query, string translated)
        {
            if (query == null || translated == null) return translated;
            int prefix = LeadingRichTagSpan(source);
            if (prefix == 0) return translated;
            if (translated.Length >= prefix && string.CompareOrdinal(source, 0, translated, 0, prefix) == 0) return translated;
            return source.Substring(0, prefix) + translated;
        }

        /* translate_strings：128 条一批；前 1024 条走实时翻译，之后一律 cache_only。 */
        internal static void TranslateStrings(PatchHttp http, StrList texts, string[] translations, ref int liveUsed)
        {
            int i = 0;
            while (i < texts.Count) {
                int n = Math.Min(texts.Count - i, Batch);
                bool live = liveUsed < LiveLimit;
                if (live && liveUsed + n > LiveLimit) n = LiveLimit - liveUsed;
                if (n == 0) {
                    n = Math.Min(texts.Count - i, Batch);
                    live = false;
                }
                var queries = new string[n];
                var batchTexts = new string[n];
                for (int j = 0; j < n; j++) {
                    queries[j] = DupPatchQueryText(texts.V[i + j]);
                    batchTexts[j] = queries[j] ?? texts.V[i + j];
                }
                var results = new string[n];
                if (http.PostBatch(batchTexts, n, !live, results)) {
                    for (int j = 0; j < n; j++) {
                        if (results[j] != null && !string.Equals(results[j], batchTexts[j], StringComparison.Ordinal) &&
                            WantedTranslationText(results[j])) {
                            translations[i + j] = RestorePatchQueryText(texts.V[i + j], queries[j], results[j]);
                        }
                    }
                }
                if (live) liveUsed += n;
                i += n;
            }
        }

        /* ======================== Smaz 解压 ======================== */

        /*
         * Godot OptimizedTranslation 使用的 Smaz 解压表。
         * Copyright (c) 2006-2009, Salvatore Sanfilippo. BSD-3-Clause.
         * 只需要解压：已修补翻译以未压缩 UTF-8 写回，未修改的压缩条目按原样复制。
         */
        private static readonly string[] SmazRcb = {
            " ", "the", "e", "t", "a", "of", "o", "and", "i", "n", "s", "e ", "r", " th",
            " t", "in", "he", "th", "h", "he ", "to", "\r\n", "l", "s ", "d", " a", "an",
            "er", "c", " o", "d ", "on", " of", "re", "of ", "t ", ", ", "is", "u", "at",
            "   ", "n ", "or", "which", "f", "m", "as", "it", "that", "\n", "was", "en",
            "  ", " w", "es", " an", " i", "\r", "f ", "g", "p", "nd", " s", "nd ", "ed ",
            "w", "ed", "http://", "for", "te", "ing", "y ", "The", " c", "ti", "r ", "his",
            "st", " in", "ar", "nt", ",", " to", "y", "ng", " h", "with", "le", "al", "to ",
            "b", "ou", "be", "were", " b", "se", "o ", "ent", "ha", "ng ", "their", "\"",
            "hi", "from", " f", "in ", "de", "ion", "me", "v", ".", "ve", "all", "re ",
            "ri", "ro", "is ", "co", "f t", "are", "ea", ". ", "her", " m", "er ", " p",
            "es ", "by", "they", "di", "ra", "ic", "not", "s, ", "d t", "at ", "ce", "la",
            "h ", "ne", "as ", "tio", "on ", "n t", "io", "we", " a ", "om", ", a", "s o",
            "ur", "li", "ll", "ch", "had", "this", "e t", "g ", "e\r\n", " wh", "ere",
            " co", "e o", "a ", "us", " d", "ss", "\n\r\n", "\r\n\r", "=\"", " be", " e",
            "s a", "ma", "one", "t t", "or ", "but", "el", "so", "l ", "e s", "s,", "no",
            "ter", " wa", "iv", "ho", "e a", " r", "hat", "s t", "ns", "ch ", "wh", "tr",
            "ut", "/", "have", "ly ", "ta", " ha", " on", "tha", "-", " l", "ati", "en ",
            "pe", " re", "there", "ass", "si", " fo", "wa", "ec", "our", "who", "its", "z",
            "fo", "rs", ">", "ot", "un", "<", "im", "th ", "nc", "ate", "><", "ver", "ad",
            " we", "ly", "ee", " n", "id", " cl", "ac", "il", "</", "rt", " wi", "div",
            "e, ", " it", "whi", " ma", "ge", "x", "e c", "men", ".com"
        };

        /* godot_smaz_decompress：成功返回写入长度，失败返回 outlen + 1（与 C 版一致）。 */
        internal static int SmazDecompress(byte[] input, int inOffset, int inlen, StringBuilder outBuf, int outlen)
        {
            int originalOutlen = outlen;
            int written = 0;
            int c = inOffset;
            while (inlen > 0) {
                if (input[c] == 254) {
                    if (inlen < 2 || outlen < 1) return originalOutlen + 1;
                    outBuf.Append((char)input[c + 1]);
                    written++;
                    outlen--;
                    c += 2;
                    inlen -= 2;
                } else if (input[c] == 255) {
                    if (inlen < 2) return originalOutlen + 1;
                    int len = input[c + 1] + 1;
                    if (inlen < 2 + len || outlen < len) return originalOutlen + 1;
                    for (int i = 0; i < len; i++) outBuf.Append((char)input[c + 2 + i]);
                    written += len;
                    outlen -= len;
                    c += 2 + len;
                    inlen -= 2 + len;
                } else {
                    string s = SmazRcb[input[c]];
                    if (outlen < s.Length) return originalOutlen + 1;
                    outBuf.Append(s);
                    written += s.Length;
                    outlen -= s.Length;
                    c++;
                    inlen--;
                }
            }
            return written;
        }
    }
}
