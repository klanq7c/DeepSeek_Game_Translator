using System.Runtime.InteropServices;
using System.Text;

namespace DstLauncher
{
    /*
     * LauncherConfig —— fsutil.c 的 save_last_game_dir / load_last_game_dir。
     *
     * 与 ApiConfig 一样走 Windows Profile API：launcher.ini 同时被 C 版启动器
     * 读写（[server] binary 也在同一个文件里），自己解析必然与 C 版分叉。
     */
    internal static class LauncherConfig
    {
        [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
        private static extern uint GetPrivateProfileStringW(string section, string key, string def,
                                                            StringBuilder outBuf, uint size, string path);

        [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
        [return: MarshalAs(UnmanagedType.Bool)]
        private static extern bool WritePrivateProfileStringW(string section, string key, string value, string path);

        /* 把上次用户选择的游戏目录写入 launcher.ini，下次启动自动填充。 */
        internal static bool SaveLastGameDir(string dir)
        {
            if (string.IsNullOrEmpty(dir) || !SafeFs.IsDir(dir)) return false;
            SafeFs.EnsureDir(SafeFs.ConfigDir());
            string cfg = SafeFs.LauncherConfigPath();
            if (WritePrivateProfileStringW("launcher", "last_game_dir", dir, cfg)) return true;
            Log.Append("保存上次游戏目录失败：" + cfg + "（Windows 错误 " + Marshal.GetLastWin32Error() + "）。");
            return false;
        }

        /* 从 launcher.ini 读取上次目录。校验路径确实存在（可能被用户删了），
           不存在时返回 null（对应 C 版返回 0）。 */
        internal static string LoadLastGameDir()
        {
            var buf = new StringBuilder(Win32.MAX_PATH * 4);
            GetPrivateProfileStringW("launcher", "last_game_dir", "", buf,
                                     (uint)(Win32.MAX_PATH * 4), SafeFs.LauncherConfigPath());
            string dir = buf.ToString();
            return SafeFs.IsDir(dir) ? dir : null;
        }
    }
}
