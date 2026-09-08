using System;
using System.Runtime.InteropServices;
using System.Text;

namespace DstLauncher
{
    /*
     * FolderPicker —— ui.c 的 browse_folder 用的 SHBrowseForFolderW。
     *
     * 与 C 版同一个 shell 对话框、同一组标志：换成 IFileOpenDialog 会改变用户看到的
     * 选择器（不同的根节点、不同的"新建文件夹"行为），属于行为变更而不是移植。
     */
    internal static class FolderPicker
    {
        private const uint BIF_RETURNONLYFSDIRS = 0x00000001;
        private const uint BIF_NEWDIALOGSTYLE = 0x00000040;

        [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
        private struct BROWSEINFOW
        {
            internal IntPtr hwndOwner;
            internal IntPtr pidlRoot;
            internal IntPtr pszDisplayName;
            internal string lpszTitle;
            internal uint ulFlags;
            internal IntPtr lpfn;
            internal IntPtr lParam;
            internal int iImage;
        }

        [DllImport("shell32.dll", CharSet = CharSet.Unicode)]
        private static extern IntPtr SHBrowseForFolderW(ref BROWSEINFOW bi);

        [DllImport("shell32.dll", CharSet = CharSet.Unicode)]
        [return: MarshalAs(UnmanagedType.Bool)]
        private static extern bool SHGetPathFromIDListW(IntPtr pidl, [Out] StringBuilder path);

        [DllImport("ole32.dll")]
        private static extern void CoTaskMemFree(IntPtr ptr);

        /* 返回用户选择的目录；取消或路径无法转换时返回 null。 */
        internal static string Pick(IntPtr owner, string title)
        {
            var bi = new BROWSEINFOW();
            bi.hwndOwner = owner;
            bi.lpszTitle = title;
            bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
            IntPtr pid = SHBrowseForFolderW(ref bi);
            if (pid == IntPtr.Zero) return null;
            try {
                var sb = new StringBuilder(Win32.MAX_PATH * 4);
                /* 虚拟文件夹（"此电脑"、库等）没有文件系统路径，SHGetPathFromIDListW
                   会失败。这不是错误，用户只是选了不能部署的节点：与 C 版一样保持
                   路径框不变。 */
                if (!SHGetPathFromIDListW(pid, sb)) return null;
                return sb.ToString();
            } finally {
                CoTaskMemFree(pid);
            }
        }
    }
}
