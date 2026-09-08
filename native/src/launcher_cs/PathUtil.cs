using System;
using System.Runtime.InteropServices;

namespace DstLauncher
{
    /*
     * PathUtil —— fsutil.c 中路径/属性辅助函数的对等实现。
     *
     * 刻意不用 Path.Combine：C 版 path_join 只在 a 不以反斜杠结尾时插入一个 '\'，
     * 不会因为 b 是绝对路径而丢弃 a，也不做规范化；检测结果（尤其是拼出来的
     * 路径字符串）要与 C 版逐字节一致。属性查询直接走 GetFileAttributesW，与
     * C 版对不存在/无权限路径的判定一致（File.Exists 会把无权限当不存在，且
     * 对目录返回 false）。
     */
    internal static class PathUtil
    {
        private const uint INVALID_FILE_ATTRIBUTES = 0xFFFFFFFF;
        private const uint FILE_ATTRIBUTE_DIRECTORY = 0x10;

        [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
        private static extern uint GetFileAttributesW(string path);

        /* path_join：a + ('\' 若 a 非空且不以 '\' 结尾) + b。 */
        public static string Join(string a, string b)
        {
            if (a == null || b == null) return "";
            if (a.Length > 0 && a[a.Length - 1] != '\\') return a + "\\" + b;
            return a + b;
        }

        /* exists_path：GetFileAttributesW 成功即存在（文件或目录）。 */
        public static bool Exists(string path)
        {
            if (string.IsNullOrEmpty(path)) return false;
            return GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES;
        }

        /* is_dir：存在且带目录属性。 */
        public static bool IsDir(string path)
        {
            if (string.IsNullOrEmpty(path)) return false;
            uint a = GetFileAttributesW(path);
            return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY) != 0;
        }

        /* 存在且不是目录（C 版多处写作 exists_path(p) && !is_dir(p)）。 */
        public static bool IsFile(string path)
        {
            if (string.IsNullOrEmpty(path)) return false;
            uint a = GetFileAttributesW(path);
            return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY) == 0;
        }

        /* _wcsicmp 语义：不区分大小写的序数比较（不做文化相关折叠）。 */
        public static bool EqualsNoCase(string a, string b)
        {
            return string.Equals(a, b, StringComparison.OrdinalIgnoreCase);
        }

        public static bool EndsWithNoCase(string s, string suffix)
        {
            return s != null && suffix != null && s.Length >= suffix.Length
                && string.Compare(s, s.Length - suffix.Length, suffix, 0, suffix.Length,
                                  StringComparison.OrdinalIgnoreCase) == 0;
        }

        /* FindFirstFileW 通配符枚举（只匹配最后一个路径分量），返回目录项名（不含
           路径、不含 . 与 ..）。直接走 Win32 而不是 System.IO，保证匹配规则
           （含 8.3 短名命中）与 C 版一致。目录不存在或无匹配时返回空数组。 */
        public static string[] ListEntries(string dir, string pattern)
        {
            var entries = Win32Find.Enumerate(dir, pattern);
            var names = new string[entries.Count];
            for (int i = 0; i < entries.Count; i++) names[i] = entries[i].Name;
            return names;
        }
    }
}
