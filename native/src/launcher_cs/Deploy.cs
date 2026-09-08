using System;
using System.Text;

namespace DstLauncher
{
    /*
     * Deploy —— deploy.c 的逐函数移植（阶段 3 进行中）。
     *
     * 已移植：Ren'Py（hook + 字体 + 过期 .rpyc 清理）、RPG Maker MV/MZ（hook + 字体 +
     * index.html 注入/剥离 + 一次性备份 + 原子写入）、Godot（仅日志，实际由资源扫描/
     * 补丁包流程处理）、RPG Maker XP/VX（无部署）、Unity Mono / IL2CPP（见 DeployUnity.cs：
     * BepInEx 运行时安装/修补、精简 mscorlib 支持、XUnity 配置与所有权迁移、TMP 字体回退），
     * 以及所有引擎的 restore_game。
     *
     * NotPorted 保留给尚未移植的调用路径（预热扫描、Godot 补丁包、自更新、UI 尚在 C 版）；
     * deploy/restore 本身已无 NotPorted 分支。C# 版遇到未移植路径时必须明确报告，不伪装成功。
     *
     * 日志文本与 C 版逐字一致：tests/launcher_parity 用 --deploy-and-exit /
     * --restore-and-exit 比较两版的日志与产物目录树。
     */
    public static partial class Deploy
    {
        public const int NotPorted = -1;

        /* ---------------- 字体 ---------------- */

        private static void DeployRenpyFont(string game)
        {
            string ttcDst = PathUtil.Join(game, "ds_font.ttc");
            string ttfDst = PathUtil.Join(game, "ds_font.ttf");
            if (SafeFs.Exists(ttcDst) || SafeFs.Exists(ttfDst)) return;

            string windir = SafeFs.WindowsDirectory();
            if (windir == null) return;

            /* 优先 TTF："0@file.ttc" 集合索引语法并非所有 Ren'Py 版本都支持。 */
            string src = PathUtil.Join(windir, "Fonts\\simhei.ttf");
            if (SafeFs.Exists(src) && SafeFs.CopyFileSafe(src, ttfDst)) {
                Log.Append("Ren'Py：已部署中文字体（黑体）：" + ttfDst);
                return;
            }
            src = PathUtil.Join(windir, "Fonts\\msyh.ttc");
            if (SafeFs.Exists(src) && SafeFs.CopyFileSafe(src, ttcDst)) {
                Log.Append("Ren'Py：已部署中文字体（微软雅黑）：" + ttcDst);
                return;
            }
            Log.Append("Ren'Py：未找到系统中文字体（simhei.ttf/msyh.ttc），翻译文本可能显示为方块。");
        }

        private static void DeployRpgmFont(string contentRoot)
        {
            string fontDir = PathUtil.Join(contentRoot, "fonts");
            SafeFs.EnsureDir(fontDir);
            string ttfDst = PathUtil.Join(fontDir, "ds_font.ttf");
            string ttcDst = PathUtil.Join(fontDir, "ds_font.ttc");
            if (SafeFs.Exists(ttfDst) || SafeFs.Exists(ttcDst)) return;

            string windir = SafeFs.WindowsDirectory();
            if (windir == null) return;

            string src = PathUtil.Join(windir, "Fonts\\simhei.ttf");
            if (SafeFs.Exists(src) && SafeFs.CopyFileSafe(src, ttfDst)) {
                Log.Append("RPGM MV/MZ: deployed CJK font: " + ttfDst);
                return;
            }
            src = PathUtil.Join(windir, "Fonts\\msyh.ttc");
            if (SafeFs.Exists(src) && SafeFs.CopyFileSafe(src, ttcDst)) {
                Log.Append("RPGM MV/MZ: deployed CJK font: " + ttcDst);
                return;
            }
            Log.Append("RPGM MV/MZ: no system CJK font found (simhei.ttf/msyh.ttc); translated text may render as boxes.");
        }

        /* ---------------- Ren'Py ---------------- */

        private static bool RemoveStaleRenpyHookBytecode(string path)
        {
            if (SafeFs.PathHasReparsePoint(path, false)) {
                Log.Append("Ren'Py: refused to remove stale hook bytecode through a reparse point: " + path);
                SafeFs.LastError = SafeFs.ERROR_ACCESS_DENIED;
                return false;
            }
            uint attr = SafeFs.Attributes(path);
            if (attr == SafeFs.INVALID_FILE_ATTRIBUTES) {
                int error = SafeFs.LastError;
                if (error == SafeFs.ERROR_FILE_NOT_FOUND || error == SafeFs.ERROR_PATH_NOT_FOUND) return true;
                Log.Append("Ren'Py: could not inspect stale launcher hook bytecode " + path + " (Windows error " + error + ").");
                return false;
            }
            if ((attr & (SafeFs.FILE_ATTRIBUTE_DIRECTORY | SafeFs.FILE_ATTRIBUTE_REPARSE_POINT)) != 0) {
                Log.Append("Ren'Py: expected launcher hook bytecode to be a file, but found a directory: " + path);
                return false;
            }

            uint originalAttr = attr;
            if ((attr & SafeFs.FILE_ATTRIBUTE_READONLY) != 0
                && !SafeFs.SetAttributes(path, attr & ~SafeFs.FILE_ATTRIBUTE_READONLY)) {
                Log.Append("Ren'Py: could not make stale launcher hook bytecode writable " + path
                           + " (Windows error " + SafeFs.LastError + ").");
                return false;
            }
            if (SafeFs.DeleteFileSafe(path)) {
                Log.Append("Ren'Py: removed stale launcher hook bytecode: " + path);
                return true;
            }

            int err = SafeFs.LastError;
            if ((originalAttr & SafeFs.FILE_ATTRIBUTE_READONLY) != 0) SafeFs.SetAttributes(path, originalAttr);
            Log.Append("Ren'Py: could not remove stale launcher hook bytecode " + path + " (Windows error " + err + ").");
            return false;
        }

        public static bool RenPy(string dir)
        {
            string game = PathUtil.Join(dir, "game");
            if (!SafeFs.IsDir(game)) {
                Log.Append("Ren'Py：找不到 game 目录，无法部署 hook：" + game);
                return false;
            }
            string hook = PathUtil.Join(game, "iron_deepseek.rpy");
            byte[] script = EmbeddedScripts.Bytes(EmbeddedScripts.RenPyHook);
            if (script == null) {
                Log.Append("Ren'Py：启动器缺少内嵌 hook 脚本，未部署 " + hook + "。请使用完整构建的启动器。");
                return false;
            }
            if (!SafeFs.WriteBytes(hook, script, script.Length)) {
                Log.Append("Ren'Py：无法写入 hook 文件 " + hook + "（Windows 错误 " + SafeFs.LastError + "）。");
                return false;
            }
            string compiledHook = PathUtil.Join(game, "iron_deepseek.rpyc");
            if (!RemoveStaleRenpyHookBytecode(compiledHook)) return false;
            DeployRenpyFont(game);
            Log.Append("已部署 Ren'Py hook：" + hook);
            return true;
        }

        /* ---------------- 通用：一次性备份 / 原子写 ---------------- */

        private static bool BackupFileOnce(string path, string suffix)
        {
            string backup = path + suffix;
            uint attr = SafeFs.Attributes(backup);
            if (attr != SafeFs.INVALID_FILE_ATTRIBUTES) return (attr & SafeFs.FILE_ATTRIBUTE_DIRECTORY) == 0;
            if (SafeFs.CopyFileIfAbsentSafe(path, backup)) return true;
            return SafeFs.LastError == SafeFs.ERROR_FILE_EXISTS;
        }

        private static bool WriteFileBytesAtomic(string path, byte[] data, int size)
        {
            string temp = path + ".dst-tmp";
            if (SafeFs.Exists(temp) && !SafeFs.DeleteFileSafe(temp)) return false;
            if (!SafeFs.WriteBytes(temp, data, size)) return false;
            if (SafeFs.MoveFileSafe(temp, path, SafeFs.MOVEFILE_REPLACE_EXISTING | SafeFs.MOVEFILE_WRITE_THROUGH)) return true;
            Log.Append("无法将临时文件重命名为 " + path + "（Windows 错误 " + SafeFs.LastError + "）。");
            SafeFs.DeleteFileSafe(temp);
            return false;
        }

        /* ---------------- RPG Maker MV/MZ：index.html 字节级处理 ---------------- */

        /* strip_owned_rpgm_hook_tags：删掉所有 src 指向 hook_rpgm_mv.js 的 <script> 标签
           （含自闭合），连同其前导空白/换行与后随换行；其余字节原样保留。 */
        internal static byte[] StripOwnedRpgmHookTags(byte[] html, int size)
        {
            const string hookName = "hook_rpgm_mv.js";
            var out_ = new ByteBuf(size + 1);
            int end = size;
            int copy = 0;
            int scan = 0;
            int hit;

            while ((hit = CStr.StrStr(html, scan, end, hookName)) >= 0) {
                int tagStart = -1;
                int p = copy;
                while ((p = CStr.FindAsciiNoCase(html, p, end, "<script")) >= 0 && p < hit) {
                    tagStart = p;
                    p += "<script".Length;
                }
                if (tagStart < 0) {
                    scan = hit + hookName.Length;
                    continue;
                }
                int openEnd = CStr.StrChr(html, tagStart, end, (byte)'>');
                int srcAttr = CStr.FindAsciiNoCase(html, tagStart, end, "src");
                if (openEnd < 0 || openEnd >= end || hit > openEnd
                    || srcAttr < 0 || srcAttr > hit || srcAttr > openEnd) {
                    scan = hit + hookName.Length;
                    continue;
                }
                int tagEnd;
                int tagClose = openEnd;
                while (tagClose > tagStart && (html[tagClose - 1] == (byte)' ' || html[tagClose - 1] == (byte)'\t')) tagClose--;
                if (tagClose > tagStart && html[tagClose - 1] == (byte)'/') {
                    /* 自闭合 <script .../> 没有 </script>，删除范围只到本标签末尾 */
                    tagEnd = openEnd + 1;
                } else {
                    tagEnd = CStr.FindAsciiNoCase(html, openEnd, end, "</script>");
                    if (tagEnd < 0) {
                        scan = hit + hookName.Length;
                        continue;
                    }
                    tagEnd += "</script>".Length;
                }
                int trimStart = tagStart;
                while (trimStart > copy && (html[trimStart - 1] == (byte)' ' || html[trimStart - 1] == (byte)'\t')) trimStart--;
                if (trimStart > copy && (html[trimStart - 1] == (byte)'\r' || html[trimStart - 1] == (byte)'\n')) {
                    while (trimStart > copy && (html[trimStart - 1] == (byte)'\r' || html[trimStart - 1] == (byte)'\n')) trimStart--;
                }
                out_.Add(html, copy, trimStart - copy);
                copy = tagEnd;
                while (copy < end && (html[copy] == (byte)'\r' || html[copy] == (byte)'\n')) copy++;
                scan = copy;
            }
            out_.Add(html, copy, end - copy);
            return out_.ToArray();
        }

        public static bool Rpgm(string dir)
        {
            string contentRoot = EngineDetector.RpgmContentRoot(dir);
            if (contentRoot == null) {
                Log.Append("RPGM MV/MZ：无法解析游戏内容目录：" + dir);
                return false;
            }
            string jsdir = PathUtil.Join(contentRoot, "js");
            if (!SafeFs.IsDir(jsdir)) {
                Log.Append("RPGM MV/MZ：找不到 js 目录，无法部署 hook：" + jsdir);
                return false;
            }
            string hook = PathUtil.Join(jsdir, "hook_rpgm_mv.js");
            byte[] script = EmbeddedScripts.Bytes(EmbeddedScripts.RpgmHook);
            if (script == null) {
                Log.Append("RPGM MV/MZ：启动器缺少内嵌 hook 脚本，未部署 " + hook + "。请使用完整构建的启动器。");
                return false;
            }
            if (!SafeFs.WriteBytes(hook, script, script.Length)) {
                Log.Append("RPGM MV/MZ：无法写入 hook 文件 " + hook + "（Windows 错误 " + SafeFs.LastError + "）。");
                return false;
            }
            DeployRpgmFont(contentRoot);

            string index = PathUtil.Join(contentRoot, "index.html");
            byte[] html = SafeFs.ReadBytes(index);
            if (html == null) {
                Log.Append("RPGM MV/MZ：无法读取 index.html " + index + "（Windows 错误 " + SafeFs.LastError + "）。");
                return false;
            }

            byte[] scriptTag = Encoding.ASCII.GetBytes("\n<script type=\"text/javascript\" src=\"js/hook_rpgm_mv.js\"></script>\n");
            byte[] stripped = StripOwnedRpgmHookTags(html, html.Length);

            int insert;
            int mainRef = CStr.StrStr(stripped, 0, stripped.Length, "js/main.js");
            if (mainRef >= 0) {
                insert = mainRef;
                while (insert > 0 && stripped[insert] != (byte)'<') insert--;
                if (stripped[insert] != (byte)'<') insert = mainRef;
            } else {
                insert = CStr.StrStr(stripped, 0, stripped.Length, "</body>");
                if (insert < 0) insert = stripped.Length;
            }

            var out_ = new ByteBuf(stripped.Length + scriptTag.Length + 1);
            out_.Add(stripped, 0, insert);
            out_.Add(scriptTag, 0, scriptTag.Length);
            out_.Add(stripped, insert, stripped.Length - insert);
            if (out_.Length != stripped.Length + scriptTag.Length) {
                Log.Append("RPGM MV/MZ：生成 index.html 时内部长度不一致，已放弃写入。");
                return false;
            }
            if (!BackupFileOnce(index, ".dst-backup")) return false;
            byte[] result = out_.ToArray();
            if (!WriteFileBytesAtomic(index, result, result.Length)) return false;
            Log.Append("已部署 RPGM MV/MZ hook：" + hook);
            return true;
        }

        /* ---------------- Godot / Unity ---------------- */

        public static bool Godot(string dir)
        {
            Log.Append("Godot: enabled PO/CSV/GDScript/resource scan and cache warmup mode.");
            Log.Append("Godot: will build an external translation patch pack after warmup; original .pck files are left unchanged.");
            return true;
        }

        /* deploy_for_engine（ui.c）：按引擎分派。返回 1/0 与 C 版一致。 */
        public static int ForEngine(string dir, Engine e)
        {
            int deployed = 0;
            if (e == Engine.RenPy) deployed = RenPy(dir) ? 1 : 0;
            else if (e == Engine.RpgmMv) deployed = Rpgm(dir) ? 1 : 0;
            else if (e == Engine.Unity) deployed = Unity(dir) ? 1 : 0;
            else if (e == Engine.UnityIl2cpp) deployed = UnityIl2cpp(dir) ? 1 : 0;
            else if (e == Engine.Godot) deployed = Godot(dir) ? 1 : 0;
            else if (e == Engine.RpgmLegacy) Log.Append("RPGM XP/VX：离线写入器仍待迁移，当前保留本地缓存服务。");
            else Log.Append("未知引擎：只启动服务端和游戏。");
            Log.Append(deployed != 0 ? "部署完成。" : "部署跳过或未完成。");
            return deployed;
        }

        /* ---------------- restore_game ---------------- */

        public sealed class RestoreStats
        {
            public int Removed, Restored, Preserved, Failed;
        }

        private static bool PathMissingError(int error)
        {
            return error == SafeFs.ERROR_FILE_NOT_FOUND || error == SafeFs.ERROR_PATH_NOT_FOUND;
        }

        private static bool RestoreDeleteFile(string path, RestoreStats stats)
        {
            if (SafeFs.PathHasReparsePoint(path, false)) {
                stats.Failed++;
                Log.Append("Restore: refused to delete through a directory reparse point: " + path);
                return false;
            }
            uint attr = SafeFs.Attributes(path);
            if (attr == SafeFs.INVALID_FILE_ATTRIBUTES) {
                int error = SafeFs.LastError;
                if (PathMissingError(error)) return true;
                stats.Failed++;
                Log.Append("还原：无法检查文件 " + path + "（Windows 错误 " + error + "）。");
                return false;
            }
            if ((attr & (SafeFs.FILE_ATTRIBUTE_DIRECTORY | SafeFs.FILE_ATTRIBUTE_REPARSE_POINT)) != 0) {
                stats.Failed++;
                Log.Append("还原：应为普通文件但检测到目录或重解析点，已保留：" + path);
                return false;
            }

            uint originalAttr = attr;
            if ((attr & SafeFs.FILE_ATTRIBUTE_READONLY) != 0) {
                SafeFs.SetAttributes(path, attr & ~SafeFs.FILE_ATTRIBUTE_READONLY);
            }
            if (SafeFs.DeleteFileSafe(path)) {
                stats.Removed++;
                Log.Append("还原：已移除 " + path);
                return true;
            }

            int err = SafeFs.LastError;
            if ((originalAttr & SafeFs.FILE_ATTRIBUTE_READONLY) != 0) SafeFs.SetAttributes(path, originalAttr);
            stats.Failed++;
            Log.Append("还原：无法删除 " + path + "（Windows 错误 " + err + "）。");
            return false;
        }

        private static void RestoreRenpy(string dir, RestoreStats stats)
        {
            string[] files = {
                "game\\iron_deepseek.rpy",
                "game\\iron_deepseek.rpyc",
                "game\\ds_font.ttf",
                "game\\ds_font.ttc",
                "game\\ds_font.otf"
            };
            foreach (string f in files) RestoreDeleteFile(PathUtil.Join(dir, f), stats);
        }

        private static void RestoreRpgm(string dir, RestoreStats stats)
        {
            string contentRoot = EngineDetector.RpgmContentRoot(dir);
            if (contentRoot == null) {
                stats.Failed++;
                Log.Append("还原 RPG Maker：无法解析游戏内容目录，未修改文件。");
                return;
            }
            bool indexOk = true;
            string index = PathUtil.Join(contentRoot, "index.html");
            string backup = index + ".dst-backup";

            if (SafeFs.Exists(index) && !SafeFs.IsDir(index)) {
                byte[] html = SafeFs.ReadBytes(index);
                if (html != null) {
                    byte[] stripped = StripOwnedRpgmHookTags(html, html.Length);
                    if (stripped.Length != html.Length) {
                        if (WriteFileBytesAtomic(index, stripped, stripped.Length)) {
                            stats.Removed++;
                            Log.Append("还原 RPG Maker：已从 index.html 移除启动器脚本标签。");
                        } else {
                            indexOk = false;
                            stats.Failed++;
                            Log.Append("还原 RPG Maker：无法更新 index.html（Windows 错误 " + SafeFs.LastError + "）。");
                        }
                    }
                } else {
                    indexOk = false;
                    stats.Failed++;
                    Log.Append("还原 RPG Maker：无法读取 index.html（Windows 错误 " + SafeFs.LastError + "）。");
                }
            } else if (!SafeFs.Exists(index) && SafeFs.Exists(backup)) {
                if (SafeFs.MoveFileSafe(backup, index, SafeFs.MOVEFILE_WRITE_THROUGH)) {
                    stats.Restored++;
                    Log.Append("还原 RPG Maker：已从备份恢复 index.html。");
                } else {
                    indexOk = false;
                    stats.Failed++;
                    Log.Append("还原 RPG Maker：无法恢复 index.html（Windows 错误 " + SafeFs.LastError + "）。");
                }
            } else if (SafeFs.IsDir(index)) {
                indexOk = false;
                stats.Failed++;
                Log.Append("还原 RPG Maker：index.html 是目录，未修改。");
            }

            if (indexOk && SafeFs.Exists(index)) RestoreDeleteFile(backup, stats);

            string[] files = {
                "js\\hook_rpgm_mv.js",
                "fonts\\ds_font.ttf",
                "fonts\\ds_font.ttc",
                "fonts\\ds_font.otf"
            };
            foreach (string f in files) RestoreDeleteFile(PathUtil.Join(contentRoot, f), stats);
        }

        private static void RestoreGodot(string dir, RestoreStats stats)
        {
            string[] files = {
                "dst_godot_runtime.gd",
                "dst_godot_patch.pck",
                "dst_godot_patch.next.pck",
                "dst_godot_patch.building"
            };
            foreach (string f in files) RestoreDeleteFile(PathUtil.Join(dir, f), stats);

            string launcher = PathUtil.Join(dir, "dst_godot_patch.exe");
            string marker = PathUtil.Join(dir, "dst_godot_patch.exe.dst-owned");
            if (SafeFs.Exists(marker)) {
                RestoreDeleteFile(launcher, stats);
                RestoreDeleteFile(marker, stats);
            } else if (SafeFs.Exists(launcher)) {
                stats.Preserved++;
                Log.Append("还原 Godot：dst_godot_patch.exe 没有启动器所有权标记，已保留。");
            }
        }

        /* restore_game：返回 1/0 与 C 版一致。 */
        public static int Restore(string dir, Engine engine)
        {
            var stats = new RestoreStats();
            if (dir == null || !SafeFs.IsDir(dir) || SafeFs.PathHasReparsePoint(dir, true)) {
                Log.Append("还原：游戏目录无效。");
                return 0;
            }

            if (engine == Engine.RenPy) RestoreRenpy(dir, stats);
            else if (engine == Engine.RpgmMv) RestoreRpgm(dir, stats);
            else if (engine == Engine.Unity) RestoreUnityMono(dir, stats);
            else if (engine == Engine.UnityIl2cpp) RestoreUnityIl2cpp(dir, stats);
            else if (engine == Engine.Godot) RestoreGodot(dir, stats);
            else if (engine == Engine.RpgmLegacy) {
                Log.Append("还原 RPG Maker XP/VX：启动器未向游戏目录部署文件，无需处理。");
            } else {
                stats.Failed++;
                Log.Append("还原：未知引擎，未修改游戏目录。");
            }

            Log.Append("还原汇总：移除 " + stats.Removed + "，恢复 " + stats.Restored
                       + "，保留 " + stats.Preserved + "，失败 " + stats.Failed + "。");
            return stats.Failed == 0 && stats.Preserved == 0 ? 1 : 0;
        }
    }

    /* ByteBuf（fsutil.h）：只增长的字节缓冲。 */
    internal sealed class ByteBuf
    {
        private byte[] _data;
        private int _len;

        public ByteBuf(int capacity) { _data = new byte[Math.Max(capacity, 16)]; }

        public int Length { get { return _len; } }

        public void Add(byte[] src, int offset, int count)
        {
            if (src == null || count <= 0) return;
            if (_len + count > _data.Length) {
                int newCap = _data.Length;
                while (_len + count > newCap) newCap *= 2;
                Array.Resize(ref _data, newCap);
            }
            Buffer.BlockCopy(src, offset, _data, _len, count);
            _len += count;
        }

        public byte[] ToArray()
        {
            var r = new byte[_len];
            Buffer.BlockCopy(_data, 0, r, 0, _len);
            return r;
        }
    }

    /* C 字符串语义的字节搜索：strstr/strchr 在遇到 NUL 时停止（read_file_bytes 在末尾
       补 NUL，文件内部的 NUL 也会截断搜索），find_ascii_substr_nocase 只受显式 end 约束。 */
    internal static class CStr
    {
        private static int NulEnd(byte[] buf, int start, int end)
        {
            for (int i = start; i < end; i++) if (buf[i] == 0) return i;
            return end;
        }

        /* strstr(buf + start, needle)，结果为绝对下标，找不到返回 -1。 */
        public static int StrStr(byte[] buf, int start, int end, string needle)
        {
            int stop = NulEnd(buf, start, end);
            int n = needle.Length;
            if (n == 0) return start;
            for (int i = start; i + n <= stop; i++) {
                int k = 0;
                while (k < n && buf[i + k] == (byte)needle[k]) k++;
                if (k == n) return i;
            }
            return -1;
        }

        /* strchr(buf + start, c) */
        public static int StrChr(byte[] buf, int start, int end, byte c)
        {
            int stop = NulEnd(buf, start, end);
            for (int i = start; i < stop; i++) if (buf[i] == c) return i;
            return -1;
        }

        /* find_ascii_substr_nocase：[start, end) 内 ASCII 大小写不敏感查找，不依赖 NUL。 */
        public static int FindAsciiNoCase(byte[] buf, int start, int end, string needle)
        {
            int n = needle.Length;
            if (n == 0) return start;
            for (int i = start; end - i >= n; i++) {
                int k = 0;
                while (k < n && ToLower(buf[i + k]) == ToLower((byte)needle[k])) k++;
                if (k == n) return i;
            }
            return -1;
        }

        private static byte ToLower(byte b)
        {
            return b >= (byte)'A' && b <= (byte)'Z' ? (byte)(b + 32) : b;
        }
    }
}
