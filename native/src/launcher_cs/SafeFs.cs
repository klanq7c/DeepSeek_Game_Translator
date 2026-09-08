using System;
using System.IO;
using System.Runtime.InteropServices;
using System.Text;
using Microsoft.Win32.SafeHandles;

namespace DstLauncher
{
    /*
     * SafeFs —— fsutil.c 的 C# 对等实现（路径安全检查、目录创建、读写/复制/移动/删除）。
     *
     * 直接 P/Invoke Win32，而不是走 System.IO：
     *   1. 部署目标位于用户选择、不可信的游戏目录，C 版逐级拒绝重解析点
     *      （junction / 目录符号链接）把写入重定向到所选目录之外，System.IO 没有这层检查；
     *   2. 日志里带 Windows 错误码，两版必须相同，System.IO 的异常会丢掉/改写原始码；
     *   3. 行为（CREATE_ALWAYS、MOVEFILE_* 标志、只读属性处理）要与 C 版逐条一致。
     *
     * 所有函数用 LastError 属性暴露最近一次失败的 Win32 错误码，对应 C 版 GetLastError()。
     */
    public static class SafeFs
    {
        public const int ERROR_SUCCESS = 0;
        public const int ERROR_FILE_NOT_FOUND = 2;
        public const int ERROR_PATH_NOT_FOUND = 3;
        public const int ERROR_ACCESS_DENIED = 5;
        public const int ERROR_INVALID_PARAMETER = 87;
        public const int ERROR_INSUFFICIENT_BUFFER = 122;
        public const int ERROR_DIR_NOT_EMPTY = 145;
        public const int ERROR_ALREADY_EXISTS = 183;
        public const int ERROR_FILE_EXISTS = 80;
        public const int ERROR_NO_MORE_FILES = 18;
        public const int ERROR_GEN_FAILURE = 31;

        public const uint INVALID_FILE_ATTRIBUTES = 0xFFFFFFFF;
        public const uint FILE_ATTRIBUTE_READONLY = 0x1;
        public const uint FILE_ATTRIBUTE_DIRECTORY = 0x10;
        public const uint FILE_ATTRIBUTE_NORMAL = 0x80;
        public const uint FILE_ATTRIBUTE_REPARSE_POINT = 0x400;

        public const uint MOVEFILE_REPLACE_EXISTING = 0x1;
        public const uint MOVEFILE_COPY_ALLOWED = 0x2;
        public const uint MOVEFILE_WRITE_THROUGH = 0x8;

        private const uint GENERIC_READ = 0x80000000;
        private const uint GENERIC_WRITE = 0x40000000;
        private const uint FILE_SHARE_READ = 0x1;
        private const uint FILE_SHARE_WRITE = 0x2;
        private const uint FILE_SHARE_DELETE = 0x4;
        private const uint CREATE_ALWAYS = 2;
        private const uint OPEN_EXISTING = 3;

        [ThreadStatic] private static int _lastError;

        /* 对应 GetLastError()：最近一次 SafeFs 调用失败留下的 Win32 错误码。 */
        public static int LastError
        {
            get { return _lastError; }
            set { _lastError = value; }
        }

        #region P/Invoke

        [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
        private static extern uint GetFileAttributesW(string path);

        [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
        [return: MarshalAs(UnmanagedType.Bool)]
        private static extern bool SetFileAttributesW(string path, uint attrs);

        [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
        [return: MarshalAs(UnmanagedType.Bool)]
        private static extern bool CreateDirectoryW(string path, IntPtr securityAttributes);

        [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
        [return: MarshalAs(UnmanagedType.Bool)]
        private static extern bool RemoveDirectoryW(string path);

        [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
        [return: MarshalAs(UnmanagedType.Bool)]
        private static extern bool DeleteFileW(string path);

        [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
        [return: MarshalAs(UnmanagedType.Bool)]
        private static extern bool CopyFileW(string from, string to, [MarshalAs(UnmanagedType.Bool)] bool failIfExists);

        [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
        [return: MarshalAs(UnmanagedType.Bool)]
        private static extern bool MoveFileExW(string from, string to, uint flags);

        [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
        private static extern uint GetFullPathNameW(string path, uint cap, StringBuilder buffer, IntPtr filePart);

        [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
        private static extern uint GetWindowsDirectoryW(StringBuilder buffer, uint cap);

        [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
        private static extern SafeFileHandle CreateFileW(string path, uint access, uint share, IntPtr sa,
                                                         uint disposition, uint flags, IntPtr template);

        [StructLayout(LayoutKind.Sequential)]
        private struct WIN32_FILE_ATTRIBUTE_DATA
        {
            public uint dwFileAttributes;
            public System.Runtime.InteropServices.ComTypes.FILETIME ftCreationTime;
            public System.Runtime.InteropServices.ComTypes.FILETIME ftLastAccessTime;
            public System.Runtime.InteropServices.ComTypes.FILETIME ftLastWriteTime;
            public uint nFileSizeHigh;
            public uint nFileSizeLow;
        }

        [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
        [return: MarshalAs(UnmanagedType.Bool)]
        private static extern bool GetFileAttributesExW(string path, int infoLevel, out WIN32_FILE_ATTRIBUTE_DATA data);

        #endregion

        /* file_size_at_most（warmup.c）：普通文件且大小 ≤ maxBytes；查不到属性、是目录、
           或高 32 位非零（≥ 4 GB）都视为不满足，扫描器据此跳过异常大的文件。 */
        public static bool FileSizeAtMost(string path, uint maxBytes)
        {
            WIN32_FILE_ATTRIBUTE_DATA d;
            if (!GetFileAttributesExW(path, 0 /* GetFileExInfoStandard */, out d)) {
                _lastError = Win32Error();
                return false;
            }
            if ((d.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) return false;
            if (d.nFileSizeHigh != 0) return false;
            return d.nFileSizeLow <= maxBytes;
        }

        private static int Win32Error()
        {
            return Marshal.GetLastWin32Error();
        }

        private static bool Fail(int error)
        {
            _lastError = error;
            return false;
        }

        /* ---------------- 属性查询 ---------------- */

        public static uint Attributes(string path)
        {
            uint a = GetFileAttributesW(path);
            if (a == INVALID_FILE_ATTRIBUTES) _lastError = Win32Error();
            return a;
        }

        public static bool Exists(string path) { return PathUtil.Exists(path); }
        public static bool IsDir(string path) { return PathUtil.IsDir(path); }

        public static bool SetAttributes(string path, uint attrs)
        {
            if (SetFileAttributesW(path, attrs)) return true;
            return Fail(Win32Error());
        }

        public static string WindowsDirectory()
        {
            var sb = new StringBuilder(260);
            uint n = GetWindowsDirectoryW(sb, 260);
            if (n == 0 || n >= 260) return null;
            return sb.ToString();
        }

        /* ---------------- 重解析点安全检查（full_path_for_safety_check / component_is_unsafe） ---------------- */

        private static bool FullPathForSafetyCheck(string path, out string full, out int rootLen)
        {
            full = null;
            rootLen = 0;
            if (string.IsNullOrEmpty(path)) return false;
            const int cap = 260 * 4;
            var sb = new StringBuilder(cap);
            uint count = GetFullPathNameW(path, cap, sb, IntPtr.Zero);
            if (count == 0 || count >= cap) return false;
            string f = sb.ToString();
            int len = f.Length;
            while (len > 3 && f[len - 1] == '\\') len--;
            f = f.Substring(0, len);

            int root;
            if (len >= 3 && f[1] == ':' && f[2] == '\\') {
                root = 3;
            } else if (len >= 5 && f[0] == '\\' && f[1] == '\\') {
                /* 设备命名空间可以绕过常规的 Win32 路径假设。 */
                if ((f[2] == '?' || f[2] == '.') && f[3] == '\\') return false;
                int serverEnd = f.IndexOf('\\', 2);
                if (serverEnd < 0 || serverEnd + 1 >= len) return false;
                int shareEnd = f.IndexOf('\\', serverEnd + 1);
                root = shareEnd >= 0 ? shareEnd + 1 : len;
            } else {
                return false;
            }
            full = f;
            rootLen = root;
            return true;
        }

        private static bool ComponentIsUnsafe(string path, bool requireDirectory)
        {
            uint attrs = GetFileAttributesW(path);
            if (attrs == INVALID_FILE_ATTRIBUTES) {
                int error = Win32Error();
                return error != ERROR_FILE_NOT_FOUND && error != ERROR_PATH_NOT_FOUND;
            }
            if ((attrs & FILE_ATTRIBUTE_REPARSE_POINT) != 0) return true;
            return requireDirectory && (attrs & FILE_ATTRIBUTE_DIRECTORY) == 0;
        }

        /* path_has_reparse_point：任一已存在组件是重解析点、或路径无法检查时返回 true（失败关闭）。 */
        public static bool PathHasReparsePoint(string path, bool includeLeaf)
        {
            string full;
            int rootLen;
            if (!FullPathForSafetyCheck(path, out full, out rootLen)) return true;
            int len = full.Length;
            for (int i = rootLen; i <= len; i++) {
                if (i < len && full[i] != '\\') continue;
                if (i == len && !includeLeaf) break;
                string component = full.Substring(0, i);
                if (ComponentIsUnsafe(component, i != len || !includeLeaf)) return true;
            }
            return false;
        }

        private static bool EnsureCheckedDirectory(string path)
        {
            uint attrs = GetFileAttributesW(path);
            if (attrs != INVALID_FILE_ATTRIBUTES) {
                return (attrs & FILE_ATTRIBUTE_DIRECTORY) != 0 && (attrs & FILE_ATTRIBUTE_REPARSE_POINT) == 0;
            }
            int error = Win32Error();
            if (error != ERROR_FILE_NOT_FOUND && error != ERROR_PATH_NOT_FOUND) return false;
            if (!CreateDirectoryW(path, IntPtr.Zero)) return false;
            attrs = GetFileAttributesW(path);
            return attrs != INVALID_FILE_ATTRIBUTES
                && (attrs & FILE_ATTRIBUTE_DIRECTORY) != 0
                && (attrs & FILE_ATTRIBUTE_REPARSE_POINT) == 0;
        }

        /* ensure_dir：逐级创建；任何一级不可检查/是重解析点/不是目录都失败（ERROR_ACCESS_DENIED）。 */
        public static bool EnsureDir(string path)
        {
            string full;
            int rootLen;
            if (!FullPathForSafetyCheck(path, out full, out rootLen)) return Fail(ERROR_ACCESS_DENIED);
            if (ComponentIsUnsafe(full, true) && Exists(full)) return Fail(ERROR_ACCESS_DENIED);
            int len = full.Length;
            for (int i = rootLen; i <= len; i++) {
                if (i < len && full[i] != '\\') continue;
                if (!EnsureCheckedDirectory(full.Substring(0, i))) return Fail(ERROR_ACCESS_DENIED);
            }
            return true;
        }

        private static bool WriteTargetIsSafe(string path)
        {
            if (PathHasReparsePoint(path, false)) return Fail(ERROR_ACCESS_DENIED);
            uint attrs = GetFileAttributesW(path);
            if (attrs == INVALID_FILE_ATTRIBUTES) {
                int error = Win32Error();
                _lastError = error;
                return error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND;
            }
            if ((attrs & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) != 0) return Fail(ERROR_ACCESS_DENIED);
            return true;
        }

        /* ---------------- 读写 ---------------- */

        private static bool WriteAll(SafeFileHandle h, byte[] data, int size)
        {
            try {
                using (var fs = new FileStream(h, FileAccess.Write, 1, false)) {
                    fs.Write(data, 0, size);
                    fs.Flush();
                }
                return true;
            } catch (IOException ex) {
                return Fail(HResultToWin32(ex.HResult));
            }
        }

        private static int HResultToWin32(int hresult)
        {
            if ((hresult & 0xFFFF0000) == unchecked((int)0x80070000)) return hresult & 0xFFFF;
            return ERROR_GEN_FAILURE;
        }

        /* write_text_file_utf8：CREATE_ALWAYS 写入 UTF-8 字节（不加 BOM，与 C 版 strlen 语义一致）。 */
        public static bool WriteTextUtf8(string path, string text)
        {
            if (path == null || text == null) return false;
            byte[] bytes = new UTF8Encoding(false).GetBytes(text);
            return WriteBytes(path, bytes, bytes.Length);
        }

        /* write_file_bytes：CREATE_ALWAYS 覆盖写入原始字节。 */
        public static bool WriteBytes(string path, byte[] data, int size)
        {
            if (path == null || (data == null && size != 0) || !WriteTargetIsSafe(path)) return false;
            SafeFileHandle h = CreateFileW(path, GENERIC_WRITE, 0, IntPtr.Zero, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, IntPtr.Zero);
            if (h.IsInvalid) { h.Dispose(); return Fail(Win32Error()); }
            return WriteAll(h, data ?? new byte[0], size);
        }

        /* read_file_bytes：读取全部字节；> 4GB-2 的文件拒绝。 */
        public static byte[] ReadBytes(string path)
        {
            if (path == null) return null;
            SafeFileHandle h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, IntPtr.Zero, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, IntPtr.Zero);
            if (h.IsInvalid) { _lastError = Win32Error(); h.Dispose(); return null; }
            try {
                using (var fs = new FileStream(h, FileAccess.Read, 1, false)) {
                    long len = fs.Length;
                    if (len < 0 || len > uint.MaxValue - 1) { _lastError = ERROR_GEN_FAILURE; return null; }
                    var buf = new byte[len];
                    int got = 0;
                    while (got < buf.Length) {
                        int n = fs.Read(buf, got, buf.Length - got);
                        if (n <= 0) { _lastError = ERROR_GEN_FAILURE; return null; }
                        got += n;
                    }
                    return buf;
                }
            } catch (IOException ex) {
                _lastError = HResultToWin32(ex.HResult);
                return null;
            }
        }

        /* files_equal_streaming：固定缓冲比较两个普通文件。 */
        public static bool FilesEqualStreaming(string left, string right)
        {
            SafeFileHandle lh = CreateFileW(left, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                            IntPtr.Zero, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, IntPtr.Zero);
            if (lh.IsInvalid) { lh.Dispose(); return false; }
            SafeFileHandle rh = CreateFileW(right, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                            IntPtr.Zero, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, IntPtr.Zero);
            if (rh.IsInvalid) { rh.Dispose(); lh.Dispose(); return false; }
            try {
                using (var l = new FileStream(lh, FileAccess.Read, 1, false))
                using (var r = new FileStream(rh, FileAccess.Read, 1, false)) {
                    if (l.Length != r.Length) return false;
                    var lb = new byte[32 * 1024];
                    var rb = new byte[32 * 1024];
                    for (;;) {
                        int ln = l.Read(lb, 0, lb.Length);
                        int rn = r.Read(rb, 0, rb.Length);
                        if (ln != rn) return false;
                        if (ln == 0) return true;
                        for (int i = 0; i < ln; i++) if (lb[i] != rb[i]) return false;
                    }
                }
            } catch (IOException) {
                return false;
            }
        }

        /* files_equal（deploy.c）：整文件读入后比较；任一读不出即视为不同。 */
        public static bool FilesEqual(string a, string b)
        {
            byte[] ab = ReadBytes(a);
            if (ab == null) return false;
            byte[] bb = ReadBytes(b);
            if (bb == null) return false;
            if (ab.Length != bb.Length) return false;
            for (int i = 0; i < ab.Length; i++) if (ab[i] != bb[i]) return false;
            return true;
        }

        /* ---------------- 复制 / 移动 / 删除 ---------------- */

        private static bool CopyFileChecked(string from, string to, bool failIfExists)
        {
            if (from == null || to == null || PathHasReparsePoint(from, true)) return Fail(ERROR_ACCESS_DENIED);
            uint sourceAttrs = GetFileAttributesW(from);
            if (sourceAttrs == INVALID_FILE_ATTRIBUTES) return Fail(Win32Error());
            if ((sourceAttrs & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) != 0) return false;
            int slash = to.LastIndexOf('\\');
            if (slash >= 0) {
                if (!EnsureDir(to.Substring(0, slash))) return false;
            }
            if (!WriteTargetIsSafe(to)) return false;
            if (!failIfExists && FilesEqualStreaming(from, to)) return true;
            if (CopyFileW(from, to, failIfExists)) return true;
            return Fail(Win32Error());
        }

        public static bool CopyFileSafe(string from, string to) { return CopyFileChecked(from, to, false); }
        public static bool CopyFileIfAbsentSafe(string from, string to) { return CopyFileChecked(from, to, true); }

        public static bool MoveFileSafe(string from, string to, uint flags)
        {
            if (from == null || to == null || PathHasReparsePoint(from, true) || !WriteTargetIsSafe(to)) {
                return Fail(ERROR_ACCESS_DENIED);
            }
            uint attrs = GetFileAttributesW(from);
            if (attrs == INVALID_FILE_ATTRIBUTES || (attrs & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) != 0) {
                return Fail(ERROR_ACCESS_DENIED);
            }
            if (MoveFileExW(from, to, flags)) return true;
            return Fail(Win32Error());
        }

        public static bool DeleteFileSafe(string path)
        {
            if (path == null || PathHasReparsePoint(path, false)) return Fail(ERROR_ACCESS_DENIED);
            uint attrs = GetFileAttributesW(path);
            if (attrs == INVALID_FILE_ATTRIBUTES || (attrs & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) != 0) {
                return Fail(ERROR_ACCESS_DENIED);
            }
            if (DeleteFileW(path)) return true;
            return Fail(Win32Error());
        }

        public static bool RemoveDirectory(string path)
        {
            if (RemoveDirectoryW(path)) return true;
            return Fail(Win32Error());
        }

        /* copy_tree_safe：递归复制；遇到重解析点整体失败但继续处理其余项，记录首个错误。 */
        public static bool CopyTreeSafe(string from, string to)
        {
            if (!IsDir(from) || PathHasReparsePoint(from, true)) return false;
            if (!EnsureDir(to)) return false;
            bool ok = true;
            int firstError = ERROR_SUCCESS;
            foreach (var entry in Win32Find.Enumerate(from, "*")) {
                string src = PathUtil.Join(from, entry.Name);
                string dst = PathUtil.Join(to, entry.Name);
                if ((entry.Attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
                    ok = false;
                    if (firstError == ERROR_SUCCESS) firstError = ERROR_ACCESS_DENIED;
                    continue;
                }
                if ((entry.Attributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
                    if (!CopyTreeSafe(src, dst)) {
                        if (firstError == ERROR_SUCCESS) firstError = _lastError;
                        ok = false;
                    }
                } else if (!CopyFileSafe(src, dst)) {
                    if (firstError == ERROR_SUCCESS) firstError = _lastError;
                    ok = false;
                }
            }
            if (!ok) _lastError = firstError != ERROR_SUCCESS ? firstError : ERROR_GEN_FAILURE;
            return ok;
        }

        /* ---------------- 配置路径（全部基于 Launcher.Root） ---------------- */

        public static string ConfigDir() { return PathUtil.Join(Launcher.Root, "config"); }
        public static string ApiConfigPath() { return PathUtil.Join(Launcher.Root, "config\\api.ini"); }
        public static string LauncherConfigPath() { return PathUtil.Join(Launcher.Root, "config\\launcher.ini"); }
    }

    /* FindFirstFileW/FindNextFileW 的最小封装：与 C 版枚举顺序、属性一致，跳过 . 和 ..。 */
    public static class Win32Find
    {
        public struct Entry
        {
            public string Name;
            public uint Attributes;
            public ulong Size; /* nFileSizeHigh:nFileSizeLow，供扫描器按目录项大小跳过大文件 */
        }

        [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
        private struct WIN32_FIND_DATAW
        {
            public uint dwFileAttributes;
            public System.Runtime.InteropServices.ComTypes.FILETIME ftCreationTime;
            public System.Runtime.InteropServices.ComTypes.FILETIME ftLastAccessTime;
            public System.Runtime.InteropServices.ComTypes.FILETIME ftLastWriteTime;
            public uint nFileSizeHigh;
            public uint nFileSizeLow;
            public uint dwReserved0;
            public uint dwReserved1;
            [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 260)] public string cFileName;
            [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 14)] public string cAlternateFileName;
        }

        [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
        private static extern IntPtr FindFirstFileW(string pattern, out WIN32_FIND_DATAW data);

        [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
        [return: MarshalAs(UnmanagedType.Bool)]
        private static extern bool FindNextFileW(IntPtr h, out WIN32_FIND_DATAW data);

        [DllImport("kernel32.dll", SetLastError = true)]
        [return: MarshalAs(UnmanagedType.Bool)]
        private static extern bool FindClose(IntPtr h);

        private static readonly IntPtr InvalidHandle = new IntPtr(-1);

        /* 返回 null 表示 FindFirstFileW 失败（目录不存在/无匹配），LastError 已设置。 */
        public static System.Collections.Generic.List<Entry> EnumerateOrNull(string dir, string pattern)
        {
            WIN32_FIND_DATAW fd;
            IntPtr h = FindFirstFileW(PathUtil.Join(dir, pattern), out fd);
            if (h == InvalidHandle) {
                SafeFs.LastError = Marshal.GetLastWin32Error();
                return null;
            }
            var list = new System.Collections.Generic.List<Entry>();
            try {
                do {
                    if (fd.cFileName == "." || fd.cFileName == "..") continue;
                    list.Add(new Entry {
                        Name = fd.cFileName,
                        Attributes = fd.dwFileAttributes,
                        Size = ((ulong)fd.nFileSizeHigh << 32) | fd.nFileSizeLow
                    });
                } while (FindNextFileW(h, out fd));
                SafeFs.LastError = Marshal.GetLastWin32Error();
            } finally {
                FindClose(h);
            }
            return list;
        }

        public static System.Collections.Generic.List<Entry> Enumerate(string dir, string pattern)
        {
            return EnumerateOrNull(dir, pattern) ?? new System.Collections.Generic.List<Entry>();
        }
    }
}
