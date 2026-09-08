using System;
using System.Collections.Generic;
using System.IO;
using System.Runtime.InteropServices;
using System.Text;

namespace DstLauncher
{
    /*
     * GodotPatchPack —— godot_patch.c 的包层：源包发现（同级 .pck / EXE 内嵌 pck 区段）、
     * 复制品内的条目改写、格式 3 运行时脚本嵌入与目录表重建、松散工程覆盖包、
     * 运行时侧车脚本，以及对外的四个入口。
     *
     * 原始 .pck、EXE 与工程资源全程只读；所有写操作都发生在 dst_godot_patch.building
     * 上，成功后才原子替换 dst_godot_patch.pck（活动包被占用时降级为 .next.pck）。
     */
    public static partial class GodotPatch
    {
        [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
        [return: MarshalAs(UnmanagedType.Bool)]
        private static extern bool CreateHardLinkW(string newFile, string existingFile, IntPtr securityAttributes);

        /* ======================== 源包发现 ======================== */

        /* find_sidecar_pck：同级目录里可解析的 .pck，优先与主 EXE 同名的那个。 */
        internal static bool FindSidecarPck(string dir, out PckSource src)
        {
            src = new PckSource();
            string preferred = null;
            string exe = EngineDetector.FindExe(dir);
            if (exe != null) {
                int slash = exe.LastIndexOf('\\');
                string name = slash >= 0 ? exe.Substring(slash + 1) : exe;
                int dot = name.LastIndexOf('.');
                if (dot >= 0) name = name.Substring(0, dot);
                preferred = name + ".pck";
            }
            var entries = Win32Find.EnumerateOrNull(dir, "*.pck");
            if (entries == null) return false;

            bool ok = false;
            int bestScore = -1;
            string bestPath = "";
            ulong bestSize = 0;
            foreach (var fd in entries) {
                if ((fd.Attributes & SafeFs.FILE_ATTRIBUTE_DIRECTORY) != 0) continue;
                if (PathUtil.EqualsNoCase(fd.Name, PackName) ||
                    PathUtil.EqualsNoCase(fd.Name, NextName) ||
                    PathUtil.EqualsNoCase(fd.Name, BuildingName)) continue;
                string path = PathUtil.Join(dir, fd.Name);
                FileStream f = OpenSharedRead(path);
                if (f == null) continue;
                using (f) {
                    long size;
                    try {
                        size = f.Length;
                    } catch (IOException) {
                        continue;
                    }
                    if (size < PckV1HeaderSize) continue;
                    var info = new PckInfo();
                    if (!ReadPckInfo(f, 0, (ulong)size, info)) continue;
                    int score = !string.IsNullOrEmpty(preferred) && PathUtil.EqualsNoCase(fd.Name, preferred) ? 100 : 0;
                    if (!ok || score > bestScore ||
                        (score == bestScore && string.Compare(path, bestPath, StringComparison.OrdinalIgnoreCase) < 0)) {
                        bestPath = path;
                        bestSize = (ulong)size;
                        bestScore = score;
                        ok = true;
                    }
                }
            }
            if (ok) {
                src.Path = bestPath;
                src.Offset = 0;
                src.Size = bestSize;
            }
            return ok;
        }

        /* find_embedded_pck：主 EXE 的 PE 区段表里名为 "pck" 且以 GDPC 开头的区段。 */
        internal static bool FindEmbeddedPck(string dir, out PckSource src)
        {
            src = new PckSource();
            string exe = EngineDetector.FindExe(dir);
            if (exe == null) return false;
            FileStream h = OpenSharedRead(exe);
            if (h == null) return false;
            using (h) {
                const int DosHeaderSize = 64;
                const int FileHeaderSize = 20;
                const int SectionHeaderSize = 40;
                long fileSize;
                try {
                    fileSize = h.Length;
                } catch (IOException) {
                    return false;
                }
                var dos = new byte[DosHeaderSize];
                if (fileSize < DosHeaderSize || !ReadAtExact(h, 0, dos, DosHeaderSize)) return false;
                if (dos[0] != (byte)'M' || dos[1] != (byte)'Z') return false;
                uint lfanew = ReadU32Le(dos, 60);
                if (lfanew == 0 || lfanew > int.MaxValue) return false;
                ulong pe = lfanew;
                var sig = new byte[4];
                var fh = new byte[FileHeaderSize];
                if (pe > (ulong)fileSize - 4u - FileHeaderSize ||
                    !ReadAtExact(h, pe, sig, 4) || ReadU32Le(sig, 0) != 0x00004550u /* "PE\0\0" */ ||
                    !ReadAtExact(h, pe + 4, fh, FileHeaderSize)) {
                    return false;
                }
                int numberOfSections = fh[2] | (fh[3] << 8);
                int sizeOfOptionalHeader = fh[16] | (fh[17] << 8);
                if (numberOfSections <= 0 || numberOfSections > 128) return false;
                ulong sections = pe + 4u + FileHeaderSize + (ulong)sizeOfOptionalHeader;
                var sh = new byte[SectionHeaderSize];
                for (int i = 0; i < numberOfSections; i++) {
                    ulong off = sections + (ulong)i * SectionHeaderSize;
                    if (off > (ulong)fileSize - SectionHeaderSize || !ReadAtExact(h, off, sh, SectionHeaderSize)) break;
                    if (sh[0] == (byte)'p' && sh[1] == (byte)'c' && sh[2] == (byte)'k' && sh[3] == 0) {
                        uint sizeOfRawData = ReadU32Le(sh, 16);
                        uint pointerToRawData = ReadU32Le(sh, 20);
                        if (pointerToRawData > 0 && sizeOfRawData >= PckV1HeaderSize) {
                            var magic = new byte[4];
                            if (ReadAtExact(h, pointerToRawData, magic, 4) && ReadU32Le(magic, 0) == PckMagic) {
                                src.Path = exe;
                                src.Offset = pointerToRawData;
                                src.Size = sizeOfRawData;
                                return true;
                            }
                        }
                    }
                }
                return false;
            }
        }

        /* ======================== 复制与偏移归一 ======================== */

        /* copy_range_to_file：把源文件的一段整体复制成独立文件。 */
        private static bool CopyRangeToFile(string srcPath, ulong offset, ulong size, string dstPath)
        {
            if (SafeFs.PathHasReparsePoint(srcPath, true) || SafeFs.PathHasReparsePoint(dstPath, true)) return false;
            FileStream src = OpenSharedRead(srcPath);
            if (src == null) return false;
            using (src) {
                FileStream dst;
                try {
                    dst = new FileStream(dstPath, FileMode.Create, FileAccess.Write, FileShare.None);
                } catch (IOException) {
                    return false;
                } catch (UnauthorizedAccessException) {
                    return false;
                }
                using (dst) {
                    try {
                        src.Seek((long)offset, SeekOrigin.Begin);
                    } catch (IOException) {
                        return false;
                    }
                    var buf = new byte[1024 * 1024];
                    ulong left = size;
                    while (left > 0) {
                        int want = left > (ulong)buf.Length ? buf.Length : (int)left;
                        int got;
                        try {
                            got = src.Read(buf, 0, want);
                            if (got <= 0) return false;
                            dst.Write(buf, 0, got);
                        } catch (IOException) {
                            return false;
                        }
                        left -= (ulong)got;
                    }
                    return true;
                }
            }
        }

        /* normalize_embedded_pck_v1_offsets：从 EXE 里抠出来的包，其绝对偏移（格式 1）或
           file_base（格式 2）仍相对 EXE 起点，需要减去原始基址。 */
        private static bool NormalizeEmbeddedPckV1Offsets(string packPath, ulong originalBase)
        {
            if (originalBase == 0) return true;
            if (SafeFs.PathHasReparsePoint(packPath, true)) return false;
            FileStream h;
            try {
                h = new FileStream(packPath, FileMode.Open, FileAccess.ReadWrite, FileShare.None);
            } catch (IOException) {
                return false;
            } catch (UnauthorizedAccessException) {
                return false;
            }
            using (h) {
                ulong packSize;
                try {
                    packSize = (ulong)h.Length;
                } catch (IOException) {
                    return false;
                }

                /* 内嵌格式 2 包（Godot 4.0–4.5 单 EXE 导出）的 file_base 相对可执行文件起点，
                   read_pck_info 会判为越界，因此先直接重定位头字段。 */
                var v2Header = new byte[PckV2HeaderSize];
                if (packSize >= PckV2HeaderSize && ReadAtExact(h, 0, v2Header, (int)PckV2HeaderSize) &&
                    ReadU32Le(v2Header, 0) == PckMagic && ReadU32Le(v2Header, 4) == 2u) {
                    ulong fileBase = ReadU64Le(v2Header, 24);
                    if (fileBase >= originalBase) {
                        WriteU64Le(v2Header, 24, fileBase - originalBase);
                        var slice = new byte[8];
                        Buffer.BlockCopy(v2Header, 24, slice, 0, 8);
                        if (!WriteAtExact(h, 24, slice, 8)) return false;
                    }
                    return true;
                }

                var info = new PckInfo();
                if (!ReadPckInfo(h, 0, packSize, info)) return false;
                if (info.Format != 1 || !info.OffsetsAreAbsolute) return true;

                ulong pos = info.EntriesOffset;
                var lenbuf = new byte[4];
                var offbuf = new byte[8];
                for (uint i = 0; i < info.FileCount; i++) {
                    if (!ReadAtExact(h, pos, lenbuf, 4)) return false;
                    pos += 4;
                    uint pathLen = ReadU32Le(lenbuf, 0);
                    if (pathLen == 0 || pathLen > MaxPath) return false;
                    if (pos > packSize || pathLen > packSize - pos) return false;
                    pos += pathLen;

                    if (pos > packSize || info.EntryMetaSize > packSize - pos ||
                        !ReadAtExact(h, pos, offbuf, 8)) {
                        return false;
                    }
                    ulong oldOff = ReadU64Le(offbuf, 0);
                    if (oldOff >= originalBase) {
                        WriteU64Le(offbuf, 0, oldOff - originalBase);
                        if (!WriteAtExact(h, pos, offbuf, 8)) return false;
                    }
                    pos += info.EntryMetaSize;
                }
                return true;
            }
        }

        /* ======================== 松散工程覆盖包 ======================== */

        private sealed class LoosePatchItem
        {
            public string Path;   /* res:// 前缀的包内路径（字节串） */
            public byte[] Data;
        }

        /* wide_rel_to_pack_path：相对路径 → res:// 包内路径（反斜杠转正斜杠，UTF-8）。 */
        private static string WideRelToPackPath(string rel)
        {
            if (string.IsNullOrEmpty(rel)) return null;
            byte[] utf8 = Encoding.UTF8.GetBytes(rel);
            if (utf8.Length + 1 > MaxPath) return null; /* WideCharToMultiByte 的 need 含结尾 NUL */
            var sb = new StringBuilder(utf8.Length + 6);
            sb.Append("res://");
            foreach (byte b in utf8) sb.Append(b == (byte)'\\' ? '/' : (char)b);
            return sb.ToString();
        }

        private static bool LooseSkipDirName(string name)
        {
            return name == null || name == "." || name == ".." ||
                   PathUtil.EqualsNoCase(name, "backup") || PathUtil.EqualsNoCase(name, ".import");
        }

        private static bool LooseSkipFileName(string name)
        {
            return name == null || PathUtil.EqualsNoCase(name, PackName) ||
                   PathUtil.EqualsNoCase(name, NextName) ||
                   PathUtil.EqualsNoCase(name, BuildingName);
        }

        private static bool LooseProjectTextPath(string path)
        {
            if (path == null) return false;
            if (AsciiContainsI(path, "/.import/") || AsciiContainsI(path, "res://.import/")) return false;
            return AsciiEndsWithI(path, ".tscn") ||
                   AsciiEndsWithI(path, ".tres") ||
                   AsciiEndsWithI(path, ".json") ||
                   MarkdownResourcePath(path);
        }

        /* write_loose_patch_pack：写出格式 1 的最小覆盖包（目录表紧跟 88 字节头，
           路径长度含结尾 NUL，条目元数据 32 字节，MD5 全零）。 */
        private static bool WriteLoosePatchPack(string packPath, List<LoosePatchItem> items)
        {
            if (packPath == null || items == null || items.Count == 0) return false;
            if (SafeFs.PathHasReparsePoint(packPath, true)) return false;
            ulong tableSize = 0;
            foreach (LoosePatchItem it in items) {
                ulong pathLen = (ulong)it.Path.Length + 1u;
                if (pathLen > MaxPath) return false;
                tableSize += 4u + pathLen + 32u;
            }
            ulong dataOff = PckV1HeaderSize + tableSize;
            if (dataOff > uint.MaxValue) return false;

            FileStream h;
            try {
                h = new FileStream(packPath, FileMode.Create, FileAccess.Write, FileShare.None);
            } catch (IOException) {
                return false;
            } catch (UnauthorizedAccessException) {
                return false;
            }
            using (h) {
                ulong ignored;
                var header = new byte[PckV1HeaderSize];
                WriteU32Le(header, 0, PckMagic);
                WriteU32Le(header, 4, 1u);
                WriteU32Le(header, 84, (uint)items.Count);
                if (!AppendFileBytes(h, header, header.Length, out ignored)) return false;

                ulong off = dataOff;
                foreach (LoosePatchItem it in items) {
                    byte[] pathBytes = ByteStr.ToBytes(it.Path + "\0");
                    var lenbuf = new byte[4];
                    var meta = new byte[32];
                    WriteU32Le(lenbuf, 0, (uint)pathBytes.Length);
                    WriteU64Le(meta, 0, off);
                    WriteU64Le(meta, 8, (ulong)it.Data.Length);
                    if (!AppendFileBytes(h, lenbuf, 4, out ignored) ||
                        !AppendFileBytes(h, pathBytes, pathBytes.Length, out ignored) ||
                        !AppendFileBytes(h, meta, 32, out ignored)) {
                        return false;
                    }
                    off += (ulong)it.Data.Length;
                }
                foreach (LoosePatchItem it in items) {
                    if (!AppendFileBytes(h, it.Data, it.Data.Length, out ignored)) return false;
                }
                return true;
            }
        }

        private sealed class LooseScanState
        {
            public List<LoosePatchItem> Items;
            public PatchHttp Http;
            public int LiveUsed;
            public int PatchedResources;
            public int PatchedStrings;
            public string CjkFont;
            public int FontReplacements;
        }

        /* collect_loose_project_overrides：遍历工程目录，修补文本资源并按优先级替换字体。 */
        private static bool CollectLooseProjectOverrides(string root, string relDir, LooseScanState st, uint depth)
        {
            if (depth > LooseMaxDepth) return true;
            string dir = !string.IsNullOrEmpty(relDir) ? PathUtil.Join(root, relDir) : root;
            var entries = Win32Find.EnumerateOrNull(dir, "*");
            if (entries == null) return true;

            foreach (var fd in entries) {
                string childRel = !string.IsNullOrEmpty(relDir) ? PathUtil.Join(relDir, fd.Name) : fd.Name;
                if ((fd.Attributes & SafeFs.FILE_ATTRIBUTE_DIRECTORY) != 0) {
                    if (LooseSkipDirName(fd.Name)) continue;
                    /* 联接和符号链接可能让遍历循环回目录树。 */
                    if ((fd.Attributes & SafeFs.FILE_ATTRIBUTE_REPARSE_POINT) != 0) continue;
                    if (!CollectLooseProjectOverrides(root, childRel, st, depth + 1u)) return false;
                    continue;
                }
                if (LooseSkipFileName(fd.Name)) continue;
                if (PathUtil.EqualsNoCase(childRel, "project.godot")) continue;

                string packPath = WideRelToPackPath(childRel);
                if (packPath == null) continue;
                string absPath = PathUtil.Join(root, childRel);

                if (LooseProjectTextPath(packPath)) {
                    byte[] buf = SafeFs.ReadBytes(absPath);
                    if (buf != null && buf.Length <= MaxEntryBytes) {
                        int resourcePatchedStrings;
                        byte[] resource = BuildTextResourcePatch(packPath, buf, buf.Length, st.Http,
                                                                 ref st.LiveUsed, out resourcePatchedStrings);
                        if (resource != null && resourcePatchedStrings > 0) {
                            st.Items.Add(new LoosePatchItem { Path = packPath, Data = resource });
                            st.PatchedResources++;
                            st.PatchedStrings += resourcePatchedStrings;
                        }
                    }
                } else if (st.CjkFont != null && st.FontReplacements < MaxFontReplacements) {
                    int fontPriority = FontResourcePriority(packPath);
                    if (fontPriority >= LooseFontMinPriority) {
                        byte[] fontBuf = SafeFs.ReadBytes(st.CjkFont);
                        if (fontBuf != null && fontBuf.Length > 0 && (ulong)fontBuf.Length <= MaxFontBytes) {
                            st.Items.Add(new LoosePatchItem { Path = packPath, Data = fontBuf });
                            st.PatchedResources++;
                            st.FontReplacements++;
                        }
                    }
                }
            }
            return true;
        }

        /* build_loose_project_patch_pack：完整松散工程的覆盖包（原样带上 project.godot）。 */
        private static bool BuildLooseProjectPatchPack(string dir, string buildPack,
                                                       ref int patchedResources, ref int patchedStrings)
        {
            string project = PathUtil.Join(dir, "project.godot");
            if (!PathUtil.Exists(project)) return false;

            var http = new PatchHttp();
            if (!http.Open()) {
                Log.Append("Godot: local server was not reachable while building loose project patch pack.");
                return false;
            }

            try {
                byte[] projectBuf = SafeFs.ReadBytes(project);
                if (projectBuf == null) return false;
                var st = new LooseScanState {
                    Items = new List<LoosePatchItem>(),
                    Http = http,
                    CjkFont = FindSystemCjkFont()
                };
                st.Items.Add(new LoosePatchItem { Path = "res://project.godot", Data = projectBuf });
                if (!CollectLooseProjectOverrides(dir, "", st, 0u)) return false;
                patchedResources += st.PatchedResources;
                patchedStrings += st.PatchedStrings;
                if (st.PatchedResources == 0 || st.PatchedStrings == 0) return false;
                return WriteLoosePatchPack(buildPack, st.Items);
            } finally {
                http.Close();
            }
        }

        /* ======================== 补丁启动器 EXE ======================== */

        /* godot_prepare_patch_launcher：某些导出模板禁用 --main-pack，这时需要一个与游戏
           EXE 同内容、位于同目录的启动器副本（配合同名 .pck 覆盖）。返回副本路径或 null。 */
        public static string PreparePatchLauncher(string dir)
        {
            if (string.IsNullOrEmpty(dir) || SafeFs.PathHasReparsePoint(dir, true)) return null;

            PckSource sidecar, embedded;
            if (!FindSidecarPck(dir, out sidecar) || FindEmbeddedPck(dir, out embedded)) return null;

            string sourceExe = EngineDetector.FindExe(dir);
            if (sourceExe == null) return null;
            string launcher = PathUtil.Join(dir, PatchLauncherName);
            string marker = PathUtil.Join(dir, PatchLauncherMarker);

            if (PathUtil.Exists(launcher)) {
                if (!PathUtil.Exists(marker)) {
                    Log.Append("Godot: preserved an existing dst_godot_patch.exe without a launcher ownership marker.");
                    return null;
                }
                return launcher;
            }
            if (PathUtil.Exists(marker) && !SafeFs.DeleteFileSafe(marker)) return null;

            bool pathsSafe = !SafeFs.PathHasReparsePoint(sourceExe, true) &&
                             !SafeFs.PathHasReparsePoint(launcher, true);
            bool linked = false;
            int linkError = SafeFs.ERROR_ACCESS_DENIED;
            if (pathsSafe) {
                linked = CreateHardLinkW(launcher, sourceExe, IntPtr.Zero);
                if (!linked) {
                    linkError = Marshal.GetLastWin32Error();
                    linked = SafeFs.CopyFileIfAbsentSafe(sourceExe, launcher);
                    if (!linked) linkError = SafeFs.LastError;
                }
            }
            if (!linked) {
                Log.Append("Godot: could not create the patch-pack launcher copy. Windows error: " + linkError);
                return null;
            }
            byte[] markerBytes = ByteStr.ToBytes(PatchLauncherMarkerText);
            if (!SafeFs.WriteBytes(marker, markerBytes, markerBytes.Length)) {
                int markerErr = SafeFs.LastError;
                SafeFs.DeleteFileSafe(launcher);
                Log.Append("Godot: could not record patch-launcher ownership. Windows error: " + markerErr);
                return null;
            }

            Log.Append("Godot: prepared a launcher-owned executable for exports that disable --main-pack.");
            return launcher;
        }

        /* ======================== 包内条目查询 ======================== */

        /* trim_pack_path：Godot 用 NUL 把路径填充到 4 字节对齐，首个 NUL 处截断。 */
        private static string TrimPackPath(byte[] path, int len)
        {
            int nul = Array.IndexOf(path, (byte)0, 0, len);
            return ByteStr.FromBytes(path, 0, nul < 0 ? len : nul);
        }

        /* godot_pack_entry_name：比较时忽略 res:// 前缀。 */
        private static string PackEntryName(string path)
        {
            if (path == null) return "";
            return path.StartsWith("res://", StringComparison.Ordinal) ? path.Substring(6) : path;
        }

        private static bool PatchPackHasEntry(string packPath, string entryPath)
        {
            if (string.IsNullOrEmpty(packPath) || string.IsNullOrEmpty(entryPath)) return false;
            FileStream h = OpenSharedRead(packPath);
            if (h == null) return false;
            using (h) {
                ulong packSize;
                try {
                    packSize = (ulong)h.Length;
                } catch (IOException) {
                    return false;
                }
                var info = new PckInfo();
                if (!ReadPckInfo(h, 0, packSize, info)) return false;

                bool found = false;
                ulong pos = info.EntriesOffset;
                var lenbuf = new byte[4];
                for (uint i = 0; i < info.FileCount; i++) {
                    if (!ReadAtExact(h, pos, lenbuf, 4)) break;
                    pos += 4;
                    uint pathLen = ReadU32Le(lenbuf, 0);
                    if (pathLen == 0 || pathLen > MaxPath || pos > packSize || pathLen > packSize - pos) break;
                    var pathBytes = new byte[pathLen];
                    if (!ReadAtExact(h, pos, pathBytes, (int)pathLen)) break;
                    string path = TrimPackPath(pathBytes, (int)pathLen);
                    found = ByteStr.EqualsNoCase(PackEntryName(path), entryPath);
                    pos += pathLen;
                    if (found) break;
                    if (pos > packSize || info.EntryMetaSize > packSize - pos) break;
                    pos += info.EntryMetaSize;
                }
                return found;
            }
        }

        public static bool PatchPackHasRuntimeSidecar(string packPath)
        {
            return PatchPackHasEntry(packPath, RuntimeScriptPath);
        }

        public static bool PatchPackHasRuntimeAutoload(string packPath)
        {
            return PatchPackHasEntry(packPath, AutoloadScriptPath) &&
                   PatchPackHasEntry(packPath, AutoloadOverridePath);
        }

        /* godot_is_loose_project：有 project.godot 且既没有同级 .pck 也没有内嵌包。 */
        public static bool IsLooseProject(string dir)
        {
            if (string.IsNullOrEmpty(dir)) return false;
            if (!PathUtil.Exists(PathUtil.Join(dir, "project.godot"))) return false;
            PckSource src;
            if (FindSidecarPck(dir, out src)) return false;
            if (FindEmbeddedPck(dir, out src)) return false;
            return true;
        }

        /* ======================== 运行时脚本 ======================== */

        /*
         * Godot 3 与 4 的 JSON、信号和字体 API 不兼容，侧车脚本按版本分为
         * payloads/Godot/dst_godot_runtime_g3.gd 与 _g4.gd（以 EmbeddedResource 嵌入）。
         * 下方的文本替换依赖脚本里的锚点："extends SceneTree\n"、"func _initialize():\n"、
         * "\t\tquit(0)\n"、"get_root()"、main_scene 切换块与 "C:/Windows/Fonts/simhei.ttf"；
         * 改脚本时必须同步。
         */
        private static string RuntimeSidecarSource(bool useGodot3)
        {
            string name = useGodot3 ? EmbeddedScripts.GodotRuntimeG3 : EmbeddedScripts.GodotRuntimeG4;
            byte[] bytes = EmbeddedScripts.Bytes(name);
            if (bytes == null) return null;
            if (Array.IndexOf(bytes, (byte)0) >= 0) {
                Log.Append("内嵌脚本资源含 NUL 字节，已拒绝使用：" + name);
                return null;
            }
            return ByteStr.FromBytes(bytes, 0, bytes.Length);
        }

        /* 运行时模板里的默认 Windows 字体路径换成本机实际解析到的 CJK 字体；
           没有 CJK 字体时保留默认列表，由脚本在运行时警告一次。 */
        private static string RuntimeScriptWithFont(string source)
        {
            if (source == null) return null;
            string fontPath = FindSystemCjkFont();
            if (fontPath == null) return source;
            byte[] utf8 = Encoding.UTF8.GetBytes(fontPath);
            string fontUtf8 = ByteStr.FromBytes(utf8, 0, utf8.Length).Replace('\\', '/');
            return source.Replace("C:/Windows/Fonts/simhei.ttf", fontUtf8);
        }

        /* build_godot_runtime_autoload：把 SceneTree 侧车改写成可作为 autoload 的 Node 脚本。 */
        private static string BuildRuntimeAutoload(uint engineMajor)
        {
            string source = RuntimeSidecarSource(engineMajor == 3);
            if (source == null) return null;
            string sceneStart = engineMajor == 3
                ? "\tvar scene = str(ProjectSettings.get_setting(\"application/run/main_scene\"))\n" +
                  "\tif scene != \"\":\n" +
                  "\t\tchange_scene(scene)\n"
                : "\tvar scene = str(ProjectSettings.get_setting(\"application/run/main_scene\"))\n" +
                  "\tif scene != \"\":\n" +
                  "\t\tchange_scene_to_file(scene)\n";
            string step = source.Replace("extends SceneTree\n", "extends Node\n");
            step = step.Replace("func _initialize():\n", "func _ready():\n");
            step = step.Replace("\t\tquit(0)\n", "\t\tget_tree().quit(0)\n");
            step = step.Replace("get_root()", "get_tree().root");
            step = step.Replace(sceneStart, "");
            if (step.Contains("extends SceneTree") || step.Contains("change_scene")) return null;
            return RuntimeScriptWithFont(step);
        }

        /* runtime_override_has_key：行首出现的真实键才算数（注释里的子串不算）。 */
        private static bool RuntimeOverrideHasKey(string source)
        {
            int sourceSize = source.Length;
            int keyLen = AutoloadOverrideKey.Length;
            int pos = 0;
            while (pos + keyLen <= sourceSize) {
                if ((pos == 0 || source[pos - 1] == '\n') &&
                    string.CompareOrdinal(source, pos, AutoloadOverrideKey, 0, keyLen) == 0) {
                    char next = pos + keyLen < sourceSize ? source[pos + keyLen] : '\n';
                    if (next == '=' || next == ' ' || next == '\t') return true;
                }
                int nl = source.IndexOf('\n', pos);
                if (nl < 0) break;
                pos = nl + 1;
            }
            return false;
        }

        /* build_runtime_override：在 [autoload_prepend] 节里插入本启动器的 autoload 条目。 */
        private static bool BuildRuntimeOverride(string source, out string result)
        {
            const string entry = AutoloadOverrideKey + "=\"" + AutoloadOverrideValue + "\"\n";
            result = null;
            if (source == null) source = "";
            int sourceSize = source.Length;

            if (RuntimeOverrideHasKey(source)) {
                result = source;
                return true;
            }

            int sectionPos = source.IndexOf(AutoloadOverrideSection, StringComparison.Ordinal);
            int insert = sourceSize;
            string prefix = null;
            if (sectionPos >= 0) {
                insert = sectionPos + AutoloadOverrideSection.Length;
                while (insert < sourceSize && source[insert] != '\n') insert++;
                if (insert < sourceSize) insert++;
            } else {
                prefix = sourceSize != 0 && source[sourceSize - 1] != '\n'
                    ? "\n\n" + AutoloadOverrideSection + "\n"
                    : "\n" + AutoloadOverrideSection + "\n";
            }

            /* 节头可能是最后一行且没有尾随换行；插入的条目仍必须从独立新行开始。 */
            string entryLead = "";
            if (prefix == null && insert > 0 && source[insert - 1] != '\n') entryLead = "\n";

            result = source.Substring(0, insert) + (prefix ?? "") + entryLead + entry + source.Substring(insert);
            return true;
        }

        /* append_pck_directory_entry：路径按 4 字节对齐补 NUL 后写入目录表。 */
        private static bool AppendPckDirectoryEntry(FileStream h, string path, byte[] meta, uint metaSize)
        {
            int rawPathSize = path.Length + 1;
            int pathSize = (rawPathSize + 3) & ~3;
            if (pathSize > 64) return false;
            var lenbuf = new byte[4];
            var pathBuf = new byte[pathSize];
            WriteU32Le(lenbuf, 0, (uint)pathSize);
            byte[] raw = ByteStr.ToBytes(path);
            Buffer.BlockCopy(raw, 0, pathBuf, 0, raw.Length);
            ulong ignored;
            return AppendFileBytes(h, lenbuf, 4, out ignored) &&
                   AppendFileBytes(h, pathBuf, pathSize, out ignored) &&
                   AppendFileBytes(h, meta, (int)metaSize, out ignored);
        }

        /* append_runtime_script：脚本追加到包末尾，元数据写回既有条目（若存在）。 */
        private static bool AppendRuntimeScript(FileStream h, PckInfo info, string script,
                                                ulong existingMetaPos, byte[] meta)
        {
            Array.Clear(meta, 0, meta.Length);
            byte[] bytes = ByteStr.ToBytes(script);
            if (bytes.Length == 0) return false;
            ulong scriptAbs;
            if (!AppendFileBytes(h, bytes, bytes.Length, out scriptAbs) || scriptAbs < info.FileBase) return false;
            WriteU64Le(meta, 0, info.OffsetsAreAbsolute ? scriptAbs : scriptAbs - info.FileBase);
            WriteU64Le(meta, 8, (ulong)bytes.Length);
            return existingMetaPos == 0 || WriteAtExact(h, existingMetaPos, meta, (int)info.EntryMetaSize);
        }

        /* embed_format3_runtime_scripts：追加运行时脚本/autoload/override.cfg，并在包末尾
           重建目录表（条目数 + 原目录 + 新增条目），最后把 header@32 指向新目录。 */
        private static bool EmbedFormat3RuntimeScripts(FileStream h, PckInfo info, ulong directoryEnd,
                                                       ulong runtimeMetaPos, ulong autoloadMetaPos,
                                                       ulong overrideMetaPos, string autoloadScript,
                                                       string overrideConfig)
        {
            uint additions = (runtimeMetaPos != 0 ? 0u : 1u) +
                             (autoloadScript != null && autoloadMetaPos == 0 ? 1u : 0u) +
                             (overrideConfig != null && overrideMetaPos == 0 ? 1u : 0u);
            if (h == null || info == null || info.Format != 3 ||
                info.FileCount > MaxFiles - additions ||
                directoryEnd < info.EntriesOffset) {
                return false;
            }
            string runtimeScript = RuntimeScriptWithFont(RuntimeSidecarSource(info.EngineMajor == 3));
            if (runtimeScript == null) return false;

            var runtimeMeta = new byte[8 + 8 + 16 + 4];
            var autoloadMeta = new byte[8 + 8 + 16 + 4];
            var overrideMeta = new byte[8 + 8 + 16 + 4];
            if (!AppendRuntimeScript(h, info, runtimeScript, runtimeMetaPos, runtimeMeta) ||
                (autoloadScript != null &&
                 !AppendRuntimeScript(h, info, autoloadScript, autoloadMetaPos, autoloadMeta)) ||
                (overrideConfig != null &&
                 !AppendRuntimeScript(h, info, overrideConfig, overrideMetaPos, overrideMeta))) {
                return false;
            }

            var count = new byte[4];
            WriteU32Le(count, 0, info.FileCount + additions);
            ulong newDirectory;
            ulong ignored;
            if (!AppendFileBytes(h, count, 4, out newDirectory) ||
                !AppendFileRange(h, info.EntriesOffset, directoryEnd - info.EntriesOffset, out ignored)) {
                return false;
            }
            if (runtimeMetaPos == 0 &&
                !AppendPckDirectoryEntry(h, "res://" + RuntimeScriptPath, runtimeMeta, info.EntryMetaSize)) {
                return false;
            }
            if (autoloadScript != null && autoloadMetaPos == 0 &&
                !AppendPckDirectoryEntry(h, "res://" + AutoloadScriptPath, autoloadMeta, info.EntryMetaSize)) {
                return false;
            }
            if (overrideConfig != null && overrideMetaPos == 0 &&
                !AppendPckDirectoryEntry(h, "res://" + AutoloadOverridePath, overrideMeta, info.EntryMetaSize)) {
                return false;
            }
            var directoryOffset = new byte[8];
            WriteU64Le(directoryOffset, 0, newDirectory);
            return WriteAtExact(h, 32, directoryOffset, 8);
        }

        private static int RuntimeMajorFromSource(PckSource src)
        {
            if (string.IsNullOrEmpty(src.Path) || src.Size < PckV1HeaderSize) return 0;
            FileStream h = OpenSharedRead(src.Path);
            if (h == null) return 0;
            var info = new PckInfo();
            bool ok;
            using (h) {
                ok = ReadPckInfo(h, src.Offset, src.Size, info);
            }
            if (!ok) return 0;
            if (info.EngineMajor == 3 || info.EngineMajor == 4) return (int)info.EngineMajor;
            return info.Format == 1 ? 3 : 4;
        }

        private static int RuntimeMajorFromProject(string dir)
        {
            byte[] buf = SafeFs.ReadBytes(PathUtil.Join(dir, "project.godot"));
            int major = 3;
            if (buf != null) {
                string text = ByteStr.FromBytes(buf, 0, ByteStr.CStrLen(buf)); /* C 版按 C 字符串处理 */
                if (AsciiContainsI(text, "config_version=5") || AsciiContainsI(text, "PackedStringArray(\"4")) {
                    major = 4;
                }
            }
            return major;
        }

        private static int RuntimeMajor(string dir)
        {
            PckSource src;
            if (FindSidecarPck(dir, out src) || FindEmbeddedPck(dir, out src)) {
                int major = RuntimeMajorFromSource(src);
                if (major != 0) return major;
            }
            return RuntimeMajorFromProject(dir);
        }

        private static bool WriteRuntimeSidecarIfChanged(string scriptPath, string sidecar)
        {
            byte[] bytes = ByteStr.ToBytes(sidecar);
            byte[] existing = SafeFs.ReadBytes(scriptPath);
            if (existing != null && existing.Length == bytes.Length) {
                bool same = true;
                for (int i = 0; i < bytes.Length; i++) {
                    if (existing[i] != bytes[i]) { same = false; break; }
                }
                if (same) return true;
            }
            return SafeFs.WriteBytes(scriptPath, bytes, bytes.Length);
        }

        /* godot_prepare_runtime_sidecar：把版本匹配的运行时脚本写到游戏目录旁。 */
        public static bool PrepareRuntimeSidecar(string dir)
        {
            if (string.IsNullOrEmpty(dir) || SafeFs.PathHasReparsePoint(dir, true)) return false;
            string script = PathUtil.Join(dir, RuntimeScriptName);
            int major = RuntimeMajor(dir);
            string sidecar = RuntimeScriptWithFont(RuntimeSidecarSource(major < 4));
            if (sidecar == null) {
                Log.Append("Godot: failed to build the runtime translation sidecar script.");
                return false;
            }
            if (!WriteRuntimeSidecarIfChanged(script, sidecar)) {
                Log.Append("Godot: failed to write runtime translation sidecar.");
                return false;
            }
            Log.Append("Godot: prepared Godot " + major + " runtime translation sidecar.");
            return true;
        }

        /* ======================== 包内条目改写 ======================== */

        /* patch_pack_file：解析目录表 → 按优先级排序 → 逐条改写（字体/文本/字节码/
           .translation）→ 格式 3 追加运行时脚本并重建目录。 */
        private static bool PatchPackFile(string packPath, ref int patchedResources, ref int patchedStrings)
        {
            patchedResources = 0;
            patchedStrings = 0;
            if (SafeFs.PathHasReparsePoint(packPath, true)) return false;
            FileStream h;
            try {
                h = new FileStream(packPath, FileMode.Open, FileAccess.ReadWrite, FileShare.None);
            } catch (IOException) {
                return false;
            } catch (UnauthorizedAccessException) {
                return false;
            }
            using (h) {
                ulong packSize;
                try {
                    packSize = (ulong)h.Length;
                } catch (IOException) {
                    return false;
                }
                var info = new PckInfo();
                if (!ReadPckInfo(h, 0, packSize, info)) return false;

                var entries = new List<PckEntry>();
                uint parsedEntries = 0;
                ulong runtimeMetaPos = 0, autoloadMetaPos = 0, overrideMetaPos = 0;
                ulong overrideRel = 0, overrideSize = 0;
                ulong pos = info.EntriesOffset;
                var lenbuf = new byte[4];
                var meta = new byte[8 + 8 + 16 + 4];
                for (uint i = 0; i < info.FileCount; i++) {
                    if (!ReadAtExact(h, pos, lenbuf, 4)) break;
                    pos += 4;
                    uint pathLen = ReadU32Le(lenbuf, 0);
                    if (pathLen == 0 || pathLen > MaxPath) break;
                    var pathBytes = new byte[pathLen];
                    if (!ReadAtExact(h, pos, pathBytes, (int)pathLen)) break;
                    string path = TrimPackPath(pathBytes, (int)pathLen);
                    pos += pathLen;
                    ulong metaPos = pos;
                    if (!ReadAtExact(h, pos, meta, (int)info.EntryMetaSize)) break;
                    pos += info.EntryMetaSize;
                    parsedEntries++;

                    string entryName = PackEntryName(path);
                    if (ByteStr.EqualsNoCase(entryName, RuntimeScriptPath)) runtimeMetaPos = metaPos;
                    if (ByteStr.EqualsNoCase(entryName, AutoloadScriptPath)) autoloadMetaPos = metaPos;
                    if (ByteStr.EqualsNoCase(entryName, AutoloadOverridePath)) {
                        overrideMetaPos = metaPos;
                        overrideRel = ReadU64Le(meta, 0);
                        overrideSize = ReadU64Le(meta, 8);
                    }
                    int kind = 0;
                    int fontPriority = 0;
                    if (EnglishTranslationPath(path)) {
                        kind = EntryTranslation;
                    } else if (TextResourcePath(path)) {
                        kind = EntryTextResource;
                    } else if ((fontPriority = FontResourcePriority(path)) > 0) {
                        kind = EntryFont;
                    } else if (GdscriptBytecodePath(path)) {
                        kind = EntryGdscriptBytecode;
                    }
                    if (kind != 0) {
                        entries.Add(new PckEntry {
                            Path = path,
                            MetaPos = metaPos,
                            Rel = ReadU64Le(meta, 0),
                            Size = ReadU64Le(meta, 8),
                            Kind = kind,
                            FontPriority = fontPriority
                        });
                    }
                }
                if (parsedEntries != info.FileCount) {
                    Log.Append("Godot: PCK directory ended before all declared entries were parsed.");
                    return false;
                }
                ulong directoryEnd = pos;

                if (entries.Count > 1) entries.Sort(CompareEntriesForPatch);

                var http = new PatchHttp();
                bool httpAvailable = http.Open();
                if (!httpAvailable && entries.Count != 0) {
                    Log.Append("Godot: local server was not reachable; building a runtime-only format 3 patch when possible.");
                }

                string cjkFont = FindSystemCjkFont();
                bool warnedMissingCjkFont = false;
                int fontReplacements = 0;
                int liveUsed = 0;
                foreach (PckEntry entry in entries) {
                    if (entry.Kind == EntryFont) {
                        if (fontReplacements >= MaxFontReplacements) continue;
                        if (cjkFont == null) {
                            if (!warnedMissingCjkFont) {
                                Log.Append("Godot: no system CJK font found (simhei.ttf/msyh.ttf/msyh.ttc); Chinese glyphs may render as boxes.");
                                warnedMissingCjkFont = true;
                            }
                            continue;
                        }
                        ulong dataAbs, fontSize;
                        if (AppendFileFromPath(h, cjkFont, out dataAbs, out fontSize) &&
                            (info.OffsetsAreAbsolute || dataAbs >= info.FileBase)) {
                            ulong newRel = info.OffsetsAreAbsolute ? dataAbs : dataAbs - info.FileBase;
                            var entryMeta = new byte[8 + 8 + 16 + 4];
                            WriteU64Le(entryMeta, 0, newRel);
                            WriteU64Le(entryMeta, 8, fontSize);
                            /* 未加密包的 MD5 可选，保持全零即可。 */
                            if (WriteAtExact(h, entry.MetaPos, entryMeta, (int)info.EntryMetaSize)) {
                                patchedResources++;
                                fontReplacements++;
                            }
                        }
                        continue;
                    }
                    if (!httpAvailable) continue;
                    if (entry.Size == 0 || entry.Size > MaxEntryBytes) continue;
                    ulong abs = info.OffsetsAreAbsolute ? entry.Rel : info.FileBase + entry.Rel;
                    if (entry.Size > uint.MaxValue || abs > packSize || entry.Size > packSize - abs) continue;
                    var buf = new byte[(int)entry.Size];
                    if (!ReadAtExact(h, abs, buf, (int)entry.Size)) continue;

                    int resourcePatchedStrings = 0;
                    byte[] resource = null;
                    if (entry.Kind == EntryTranslation) {
                        resource = BuildOptimizedTranslationPatch(buf, (uint)entry.Size, http,
                                                                  ref liveUsed, out resourcePatchedStrings);
                    } else if (entry.Kind == EntryTextResource) {
                        resource = BuildTextResourcePatch(entry.Path, buf, (int)entry.Size, http,
                                                          ref liveUsed, out resourcePatchedStrings);
                        if (resourcePatchedStrings == 0) resource = null;
                    } else if (entry.Kind == EntryGdscriptBytecode) {
                        resource = BuildGdscriptBytecodePatch(entry.Path, buf, (uint)entry.Size, http,
                                                              ref liveUsed, out resourcePatchedStrings);
                        if (resourcePatchedStrings == 0) resource = null;
                    }

                    if (resource != null) {
                        ulong dataAbs;
                        if (AppendFileBytes(h, resource, resource.Length, out dataAbs) && dataAbs >= info.FileBase) {
                            ulong newRel = info.OffsetsAreAbsolute ? dataAbs : dataAbs - info.FileBase;
                            var entryMeta = new byte[8 + 8 + 16 + 4];
                            WriteU64Le(entryMeta, 0, newRel);
                            WriteU64Le(entryMeta, 8, (ulong)resource.Length);
                            /* 未加密包的 MD5 可选，保持全零即可。 */
                            if (WriteAtExact(h, entry.MetaPos, entryMeta, (int)info.EntryMetaSize)) {
                                patchedResources++;
                                patchedStrings += resourcePatchedStrings;
                            }
                        }
                    }
                }
                http.Close();

                if (info.Format == 3) {
                    bool supportsAutoloadPrepend = info.EngineMajor > 4u ||
                                                   (info.EngineMajor == 4u && info.EngineMinor >= 6u);
                    string overrideSource = null;
                    string overrideConfig = null;
                    bool autoloadConfigured = false;
                    if (supportsAutoloadPrepend) {
                        if (overrideMetaPos != 0) {
                            ulong overrideAbs = info.OffsetsAreAbsolute ? overrideRel : info.FileBase + overrideRel;
                            if (overrideSize > MaxEntryBytes || overrideSize > uint.MaxValue ||
                                overrideAbs > packSize || overrideSize > packSize - overrideAbs) {
                                Log.Append("Godot: existing override.cfg is unreadable; keeping script-launch compatibility only.");
                            } else {
                                var overrideBuf = new byte[(int)overrideSize];
                                if (ReadAtExact(h, overrideAbs, overrideBuf, (int)overrideSize)) {
                                    overrideSource = ByteStr.FromBytes(overrideBuf, 0, overrideBuf.Length);
                                } else {
                                    Log.Append("Godot: existing override.cfg could not be read; keeping script-launch compatibility only.");
                                }
                            }
                        }
                        if ((overrideMetaPos == 0 || overrideSource != null) &&
                            BuildRuntimeOverride(overrideSource, out overrideConfig)) {
                            autoloadConfigured = true;
                        } else if (overrideConfig == null) {
                            Log.Append("Godot: override.cfg could not accept the runtime autoload; keeping script-launch compatibility only.");
                        }
                    }
                    string autoloadScript = autoloadConfigured ? BuildRuntimeAutoload(info.EngineMajor) : null;
                    if (autoloadConfigured && autoloadScript == null) {
                        Log.Append("Godot: failed to construct the engine-compatible runtime autoload script.");
                        overrideConfig = null;
                        autoloadConfigured = false;
                    }
                    if (!EmbedFormat3RuntimeScripts(h, info, directoryEnd, runtimeMetaPos, autoloadMetaPos,
                                                    overrideMetaPos, autoloadScript, overrideConfig)) {
                        Log.Append("Godot: failed to embed the runtime translator and rebuild the format 3 PCK directory.");
                        return false;
                    }
                    patchedResources += 1 + (autoloadConfigured ? 2 : 0);
                }
                return true;
            }
        }

        /* ======================== 对外入口 ======================== */

        /* godot_prepare_patch_pack：返回启动时应使用的覆盖包（或松散工程的侧车脚本）路径，
           失败返回 null。原始 .pck / EXE 不被修改。 */
        public static string PreparePatchPack(string dir)
        {
            if (string.IsNullOrEmpty(dir) || SafeFs.PathHasReparsePoint(dir, true)) return null;
            string finalPack = PathUtil.Join(dir, PackName);
            string buildPack = PathUtil.Join(dir, BuildingName);
            string nextPack = PathUtil.Join(dir, NextName);
            if (PathUtil.Exists(buildPack) && !SafeFs.DeleteFileSafe(buildPack)) return null;

            if (IsLooseProject(dir)) {
                if (PathUtil.Exists(finalPack) && !SafeFs.DeleteFileSafe(finalPack)) return null;
                if (PathUtil.Exists(nextPack) && !SafeFs.DeleteFileSafe(nextPack)) return null;
                if (!PrepareRuntimeSidecar(dir)) return null;
                Log.Append("Godot: loose project detected; prepared runtime sidecar instead of a --main-pack overlay.");
                return PathUtil.Join(dir, RuntimeScriptName);
            }

            PckSource src;
            bool hasPckSource = FindSidecarPck(dir, out src) || FindEmbeddedPck(dir, out src);
            Log.Append("Godot: building external translation patch pack...");
            int patchedResources = 0, patchedStrings = 0;
            string outPack = finalPack;
            if (hasPckSource) {
                bool shouldTryLooseProject = false;
                if (!CopyRangeToFile(src.Path, src.Offset, src.Size, buildPack)) {
                    SafeFs.DeleteFileSafe(buildPack);
                    shouldTryLooseProject = true;
                    Log.Append("Godot: failed to copy original pack into patch pack; trying loose project overrides.");
                } else if (!NormalizeEmbeddedPckV1Offsets(buildPack, src.Offset)) {
                    SafeFs.DeleteFileSafe(buildPack);
                    shouldTryLooseProject = true;
                    Log.Append("Godot: failed to normalize embedded PCK v1 offsets; trying loose project overrides.");
                } else if (!PatchPackFile(buildPack, ref patchedResources, ref patchedStrings) || patchedResources == 0) {
                    SafeFs.DeleteFileSafe(buildPack);
                    shouldTryLooseProject = true;
                    Log.Append("Godot: PCK source had no translated overrides; trying loose project overrides.");
                }
                if (shouldTryLooseProject) {
                    /* 最小松散覆盖包会替换整个游戏包，只适用于完整的松散工程；装到已打包
                       游戏上会导致无法启动（Godot 4 直接拒绝格式 1 的包）。 */
                    if (!IsLooseProject(dir)) {
                        Log.Append("Godot: no patch output for the packaged game; not installing a loose minimal pack.");
                        return null;
                    }
                    patchedResources = 0;
                    patchedStrings = 0;
                    if (!BuildLooseProjectPatchPack(dir, buildPack, ref patchedResources, ref patchedStrings)) {
                        Log.Append("Godot: PCK source and loose project overrides had no translated resource overrides yet.");
                        SafeFs.DeleteFileSafe(buildPack);
                        return null;
                    }
                    Log.Append("Godot: using loose project override pack.");
                }
            } else {
                if (!BuildLooseProjectPatchPack(dir, buildPack, ref patchedResources, ref patchedStrings)) {
                    SafeFs.DeleteFileSafe(buildPack);
                    Log.Append("Godot: no readable PCK source or loose project text overrides found for runtime patch pack.");
                    return null;
                }
            }

            if (SafeFs.MoveFileSafe(buildPack, finalPack, SafeFs.MOVEFILE_REPLACE_EXISTING | SafeFs.MOVEFILE_WRITE_THROUGH)) {
                if (PathUtil.Exists(nextPack) && !SafeFs.DeleteFileSafe(nextPack)) {
                    int cleanupErr = SafeFs.LastError;
                    if (cleanupErr != SafeFs.ERROR_FILE_NOT_FOUND) {
                        Log.Append("Godot: installed refreshed patch pack but could not remove stale staged pack. Windows error: " + cleanupErr);
                    }
                }
                Log.Append("Godot: patch pack ready: " + finalPack);
            } else {
                int replaceErr = SafeFs.LastError;
                if (!SafeFs.MoveFileSafe(buildPack, nextPack, SafeFs.MOVEFILE_REPLACE_EXISTING | SafeFs.MOVEFILE_WRITE_THROUGH)) {
                    int stageErr = SafeFs.LastError;
                    SafeFs.DeleteFileSafe(buildPack);
                    Log.Append("Godot: failed to install refreshed patch pack (replace error " + replaceErr +
                               ", stage error " + stageErr + ").");
                    return null;
                }
                outPack = nextPack;
                Log.Append("Godot: active patch pack is busy; staged refreshed pack for next launch: " + nextPack);
            }
            Log.Append("Godot: patched " + patchedResources + " resources with " + patchedStrings + " translated strings.");
            return outPack;
        }

        /* godot_promote_staged_patch_pack：把上次运行留下的 .next.pck 提升为活动包。 */
        public static bool PromoteStagedPatchPack(string dir)
        {
            if (string.IsNullOrEmpty(dir) || SafeFs.PathHasReparsePoint(dir, true)) return false;
            string finalPack = PathUtil.Join(dir, PackName);
            string nextPack = PathUtil.Join(dir, NextName);
            if (!PathUtil.Exists(nextPack)) return false;
            if (SafeFs.MoveFileSafe(nextPack, finalPack, SafeFs.MOVEFILE_REPLACE_EXISTING | SafeFs.MOVEFILE_WRITE_THROUGH)) {
                Log.Append("Godot: promoted refreshed patch pack from previous run.");
                return true;
            }
            Log.Append("Godot: refreshed patch pack exists but could not be promoted yet. Windows error: " + SafeFs.LastError);
            return false;
        }
    }
}
