using System;
using System.IO;
using System.Text;

namespace DstLauncher
{
    /* engine.h 的 Engine 枚举：值与顺序保持一致，日志与 launcher.ini 里可能出现数值。 */
    public enum Engine
    {
        Unknown,      /* 未识别 */
        RenPy,        /* Ren'Py（.rpy / .rpyc / .rpa） */
        RpgmMv,       /* RPG Maker MV/MZ（www/ 或扁平网页内容） */
        Unity,        /* Unity Mono（BepInEx） */
        UnityIl2cpp,  /* Unity IL2CPP（XUnity） */
        RpgmLegacy,   /* RPG Maker XP/VX/VXAce（.rxdata / .rvdata） */
        Godot         /* Godot 导出物或工程（.pck / project.godot） */
    }

    /*
     * EngineDetector —— engine.c 的逐函数移植。
     *
     * 检测顺序与判定条件必须与 C 版完全一致（tests/launcher_parity 用同一批
     * 目录对比两版输出）：Ren'Py → RPG Maker MV/MZ → RPG Maker Legacy → Unity
     * （再分 Mono/IL2CPP）→ Godot → 未知。
     */
    public static class EngineDetector
    {
        /* has_file_pattern：dir 下存在匹配 pattern 的任意目录项。 */
        public static bool HasFilePattern(string dir, string pattern)
        {
            return PathUtil.ListEntries(dir, pattern).Length > 0;
        }

        /* find_subdir_suffix：dir 的直接子目录里有以 suffix（不区分大小写）结尾的。 */
        public static bool FindSubdirSuffix(string dir, string suffix)
        {
            foreach (string name in PathUtil.ListEntries(dir, "*")) {
                if (!PathUtil.IsDir(PathUtil.Join(dir, name))) continue;
                if (PathUtil.EndsWithNoCase(name, suffix)) return true;
            }
            return false;
        }

        /* ---------------- find_exe ---------------- */

        private static bool IgnoredExeName(string name)
        {
            return name.IndexOf("CrashHandler", StringComparison.Ordinal) >= 0
                || name.IndexOf("UnityCrashHandler", StringComparison.Ordinal) >= 0
                || PathUtil.EqualsNoCase(name, "DeepSeekTranslator.exe")
                || PathUtil.EqualsNoCase(name, "dst_godot_patch.exe")
                || PathUtil.EqualsNoCase(name, "dst_server.exe");
        }

        private static string ExeStem(string name)
        {
            if (PathUtil.EndsWithNoCase(name, ".exe")) return name.Substring(0, name.Length - 4);
            return name;
        }

        private static string DirLeaf(string dir)
        {
            int n = dir.Length;
            while (n > 0 && (dir[n - 1] == '\\' || dir[n - 1] == '/')) n--;
            string trimmed = dir.Substring(0, n);
            int slash = trimmed.LastIndexOf('\\');
            int alt = trimmed.LastIndexOf('/');
            if (slash < 0 || alt > slash) slash = alt;
            return slash >= 0 ? trimmed.Substring(slash + 1) : trimmed;
        }

        private static int ExeCandidateScore(string dir, string name, string path)
        {
            string stem = ExeStem(name);
            int score = 0;

            if (PathUtil.IsDir(PathUtil.Join(dir, stem + "_Data"))) score += 1000;
            if (PathUtil.IsFile(PathUtil.Join(dir, stem + ".pck"))) score += 900;
            if (GodotEmbeddedPckExe(path)) score += 800;
            if (PathUtil.EqualsNoCase(stem, DirLeaf(dir))) score += 200;
            if (PathUtil.EqualsNoCase(stem, "Game") || PathUtil.EqualsNoCase(stem, "RPG_RT")) score += 100;

            if (PathUtil.EqualsNoCase(stem, "Config") || PathUtil.EqualsNoCase(stem, "Configuration")
                || PathUtil.EqualsNoCase(stem, "Setup") || PathUtil.EqualsNoCase(stem, "Installer")
                || PathUtil.EqualsNoCase(stem, "Install") || PathUtil.EqualsNoCase(stem, "Uninstall")
                || (stem.Length >= 5 && string.Compare(stem, 0, "unins", 0, 5, StringComparison.OrdinalIgnoreCase) == 0)) {
                score -= 500;
            }
            return score;
        }

        /* find_exe：分数最高者胜出，同分取名字 _wcsicmp 更小者；返回 null 表示没有候选。 */
        public static string FindExe(string dir)
        {
            bool ok = false;
            int bestScore = int.MinValue;
            string bestName = "";
            foreach (string name in PathUtil.ListEntries(dir, "*.exe")) {
                string candidate = PathUtil.Join(dir, name);
                if (PathUtil.IsDir(candidate)) continue;
                if (IgnoredExeName(name)) continue;
                if (PathUtil.EqualsNoCase(name, "ds游戏翻译器.exe")
                    || PathUtil.EqualsNoCase(name, "DeepSeekTranslator.exe")
                    || PathUtil.EqualsNoCase(name, "dst_server.exe")) continue;
                int score = ExeCandidateScore(dir, name, candidate);
                if (!ok || score > bestScore
                    || (score == bestScore && string.Compare(name, bestName, StringComparison.OrdinalIgnoreCase) < 0)) {
                    bestName = name;
                    bestScore = score;
                    ok = true;
                }
            }
            return ok ? PathUtil.Join(dir, bestName) : null;
        }

        /* ---------------- Unity ---------------- */

        /* unity_is_il2cpp：GameAssembly.dll 或任一 *_Data\il2cpp_data 目录。 */
        public static bool UnityIsIl2cpp(string dir)
        {
            if (PathUtil.Exists(PathUtil.Join(dir, "GameAssembly.dll"))) return true;
            foreach (string name in PathUtil.ListEntries(dir, "*")) {
                if (!PathUtil.IsDir(PathUtil.Join(dir, name))) continue;
                if (name.Length < 5 || !PathUtil.EndsWithNoCase(name, "_Data")) continue;
                if (PathUtil.IsDir(PathUtil.Join(PathUtil.Join(dir, name), "il2cpp_data"))) return true;
            }
            return false;
        }

        /* ---------------- Godot ---------------- */

        private static bool GodotProjectMarker(string dir)
        {
            if (PathUtil.Exists(PathUtil.Join(dir, "project.godot"))) return true;
            if (PathUtil.Exists(PathUtil.Join(dir, "godot_project.binary"))) return true;
            return PathUtil.IsDir(PathUtil.Join(dir, ".godot"));
        }

        private static bool ReadAt(FileStream fs, long offset, byte[] buf)
        {
            if (offset < 0) return false;
            fs.Seek(offset, SeekOrigin.Begin);
            int got = 0;
            while (got < buf.Length) {
                int n = fs.Read(buf, got, buf.Length - got);
                if (n <= 0) return false;
                got += n;
            }
            return true;
        }

        private static bool FileTailIsGdpc(FileStream fs, long size)
        {
            if (size < 4) return false;
            var magic = new byte[4];
            return ReadAt(fs, size - 4, magic)
                && magic[0] == (byte)'G' && magic[1] == (byte)'D' && magic[2] == (byte)'P' && magic[3] == (byte)'C';
        }

        /* pe_has_pck_section：PE 区段表里有名为 "pck" 的区段（Godot 4 内嵌包）。
           偏移与上限检查与 C 版一致：区段数 1..128，任何越界读取都判为否。 */
        private static bool PeHasPckSection(FileStream fs, long size)
        {
            const int DosHeaderSize = 64;
            const int FileHeaderSize = 20;
            const int SectionHeaderSize = 40;
            if (size < DosHeaderSize) return false;
            var dos = new byte[DosHeaderSize];
            if (!ReadAt(fs, 0, dos)) return false;
            if (dos[0] != (byte)'M' || dos[1] != (byte)'Z') return false;
            int lfanew = BitConverter.ToInt32(dos, 60);
            if (lfanew <= 0) return false;

            long pe = lfanew;
            if (pe > size - (4 + FileHeaderSize)) return false;
            var sig = new byte[4];
            if (!ReadAt(fs, pe, sig) || BitConverter.ToUInt32(sig, 0) != 0x00004550u) return false; // "PE\0\0"
            var fh = new byte[FileHeaderSize];
            if (!ReadAt(fs, pe + 4, fh)) return false;
            ushort numberOfSections = BitConverter.ToUInt16(fh, 2);
            ushort sizeOfOptionalHeader = BitConverter.ToUInt16(fh, 16);
            if (numberOfSections == 0 || numberOfSections > 128) return false;

            long sections = pe + 4 + FileHeaderSize + sizeOfOptionalHeader;
            if (sections < pe || sections > size - SectionHeaderSize) return false;

            var sh = new byte[SectionHeaderSize];
            for (int i = 0; i < numberOfSections; i++) {
                long off = sections + (long)i * SectionHeaderSize;
                if (off > size - SectionHeaderSize) return false;
                if (!ReadAt(fs, off, sh)) return false;
                if (sh[0] == (byte)'p' && sh[1] == (byte)'c' && sh[2] == (byte)'k' && sh[3] == 0) return true;
            }
            return false;
        }

        private static bool GodotEmbeddedPckExe(string path)
        {
            try {
                using (var fs = new FileStream(path, FileMode.Open, FileAccess.Read,
                                               FileShare.ReadWrite | FileShare.Delete)) {
                    long size = fs.Length;
                    return FileTailIsGdpc(fs, size) && PeHasPckSection(fs, size);
                }
            } catch (IOException) {
                /* C 版 CreateFileW 失败即返回 0：打不开就不是可识别的内嵌包 EXE。 */
                return false;
            } catch (UnauthorizedAccessException) {
                return false;
            }
        }

        private static bool HasGodotEmbeddedExe(string dir)
        {
            foreach (string name in PathUtil.ListEntries(dir, "*.exe")) {
                string p = PathUtil.Join(dir, name);
                if (PathUtil.IsDir(p)) continue;
                if (IgnoredExeName(name)) continue;
                if (GodotEmbeddedPckExe(p)) return true;
            }
            return false;
        }

        /* Godot 检测刻意排在 Unity 之后：部分 Unity 游戏或模组也可能带无关的 .pck。 */
        private static bool GodotExportOrProject(string dir)
        {
            return HasFilePattern(dir, "*.pck") || GodotProjectMarker(dir) || HasGodotEmbeddedExe(dir);
        }

        /* ---------------- RPG Maker MV/MZ ---------------- */

        private static bool RpgmIndexOrOwnedBackup(string contentRoot)
        {
            if (PathUtil.Exists(PathUtil.Join(contentRoot, "index.html"))) return true;
            return PathUtil.Exists(PathUtil.Join(contentRoot, "index.html.dst-backup"));
        }

        /* www/ 标准布局与扁平布局共用同一组特征：index.html（或部署备份）、
           data\System.json、js\main.js，且 js\rpg_core.js（MV）与 js\rmmz_core.js（MZ）至少其一。 */
        private static bool RpgmContentMarkers(string contentRoot)
        {
            if (!RpgmIndexOrOwnedBackup(contentRoot)) return false;
            string system = PathUtil.Join(contentRoot, "data\\System.json");
            string mainJs = PathUtil.Join(contentRoot, "js\\main.js");
            string mvCore = PathUtil.Join(contentRoot, "js\\rpg_core.js");
            string mzCore = PathUtil.Join(contentRoot, "js\\rmmz_core.js");
            if (!PathUtil.IsFile(system) || !PathUtil.IsFile(mainJs)
                || (!PathUtil.IsFile(mvCore) && !PathUtil.IsFile(mzCore))) {
                return false;
            }
            return true;
        }

        /* rpgm_content_root：返回内容根（dir\www 或 dir），不是 RPG Maker MV/MZ 时返回 null。 */
        public static string RpgmContentRoot(string dir)
        {
            if (string.IsNullOrEmpty(dir)) return null;
            string content = PathUtil.Join(dir, "www");
            if (PathUtil.IsDir(PathUtil.Join(content, "js")) && RpgmContentMarkers(content)) return content;
            if (!PathUtil.IsDir(PathUtil.Join(dir, "js")) || !RpgmContentMarkers(dir)) return null;
            return dir;
        }

        /* ---------------- detect_engine ---------------- */

        public static Engine Detect(string dir)
        {
            string p = PathUtil.Join(dir, "game");
            if (PathUtil.IsDir(p) && (HasFilePattern(p, "*.rpy") || HasFilePattern(p, "*.rpyc") || HasFilePattern(p, "*.rpa"))) {
                return Engine.RenPy;
            }
            if (RpgmContentRoot(dir) != null) return Engine.RpgmMv;

            p = PathUtil.Join(dir, "Data");
            if (PathUtil.IsDir(p) && (HasFilePattern(p, "*.rxdata") || HasFilePattern(p, "*.rvdata") || HasFilePattern(p, "*.rvdata2"))) {
                return Engine.RpgmLegacy;
            }
            if (FindSubdirSuffix(dir, "_Data")) return UnityIsIl2cpp(dir) ? Engine.UnityIl2cpp : Engine.Unity;
            if (GodotExportOrProject(dir)) return Engine.Godot;
            return Engine.Unknown;
        }

        /* engine_name：界面与日志用的可读名称，与 C 版逐字一致。 */
        public static string Name(Engine e)
        {
            switch (e) {
            case Engine.RenPy: return "Ren'Py";
            case Engine.RpgmMv: return "RPG Maker MV/MZ";
            case Engine.Unity: return "Unity";
            case Engine.UnityIl2cpp: return "Unity (IL2CPP)";
            case Engine.RpgmLegacy: return "RPG Maker XP/VX/VXAce";
            case Engine.Godot: return "Godot";
            default: return "未知";
            }
        }
    }
}
