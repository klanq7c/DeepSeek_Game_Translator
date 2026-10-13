using System;
using System.IO;
using Microsoft.Win32.SafeHandles;
using System.Runtime.InteropServices;
using System.Text;

namespace DstLauncher
{
    public static partial class ApiConfig
    {
        [DllImport("kernel32.dll")] private static extern uint GetACP();
        [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
        private static extern uint GetFileAttributesW(string path);
        [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
        private static extern uint GetTempFileNameW(string path, string prefix, uint unique, StringBuilder output);
        [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
        private static extern bool ReplaceFileW(string replaced, string replacement, string backup, uint flags, IntPtr exclude, IntPtr reserved);
        [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
        private static extern bool MoveFileExW(string source, string target, uint flags);
        [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
        private static extern bool DeleteFileW(string path);

        [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
        private static extern SafeFileHandle CreateFileW(string path, uint access, uint share, IntPtr sa, uint creation, uint flags, IntPtr template);
        [DllImport("kernel32.dll", SetLastError = true)]
        private static extern bool FlushFileBuffers(SafeFileHandle file);

        private static bool NeedsUnicodeIni(string value)
        {
            var encoding = Encoding.GetEncoding((int)GetACP());
            return encoding.GetString(encoding.GetBytes(value)) != value;
        }

        /* 与 C 版一致：用户保存时才升级无法表达新值的 INI，保留其他键和注释。
           临时副本完全写入后替换原文件；失败保留原配置并记录阶段和错误。 */
        private static bool SaveUnicodeIni(string directory, string cfg, string endpoint, string model, string key)
        {
            string stage = "读取原配置", tmp = null;
            byte[] bytes = null;
            bool ok = false;
            int error = 0;
            try {
                uint attrs = GetFileAttributesW(cfg);
                if (attrs == uint.MaxValue) {
                    error = Marshal.GetLastWin32Error();
                    if (error != 2 && error != 3) return false;
                    bytes = new byte[0];
                } else {
                    if ((attrs & (1u | 16u | 1024u)) != 0) { error = 5; return false; }
                    bytes = SafeFs.ReadBytes(cfg);
                    if (bytes == null) { error = SafeFs.LastError; return false; }
                }
                stage = "转换配置编码";
                if (bytes.Length > 1024 * 1024) { error = 223; return false; }
                byte[] output = bytes;
                if (bytes.Length < 2 || bytes[0] != 255 || bytes[1] != 254) {
                    var encoding = Encoding.GetEncoding((int)GetACP(), EncoderFallback.ExceptionFallback, DecoderFallback.ExceptionFallback);
                    output = Encoding.Unicode.GetBytes("\ufeff" + encoding.GetString(bytes));
                }
                stage = "写入临时配置";
                var name = new StringBuilder(260);
                if (GetTempFileNameW(directory, "dsa", 0, name) == 0) { error = Marshal.GetLastWin32Error(); return false; }
                tmp = name.ToString();
                if (!SafeFs.WriteBytes(tmp, output, output.Length)) { error = SafeFs.LastError; return false; }
                if (!WritePrivateProfileStringW("api", "endpoint", endpoint, tmp) ||
                    !WritePrivateProfileStringW("api", "model", model, tmp) ||
                    !WritePrivateProfileStringW("api", "key", key, tmp)) { error = Marshal.GetLastWin32Error(); return false; }
                // 全空参数的 Profile 缓存刷新按 Windows 契约返回 0。
                WritePrivateProfileStringW(null, null, null, tmp);
                stage = "刷新临时配置";
                using (var flush = CreateFileW(tmp, 0x40000000, 1, IntPtr.Zero, 3, 128, IntPtr.Zero)) {
                    if (flush.IsInvalid || !FlushFileBuffers(flush)) { error = Marshal.GetLastWin32Error(); return false; }
                }
                stage = "提交配置";
                ok = attrs == uint.MaxValue ? MoveFileExW(tmp, cfg, 8) : ReplaceFileW(cfg, tmp, null, 0, IntPtr.Zero, IntPtr.Zero);
                if (!ok) error = Marshal.GetLastWin32Error();
                return ok;
            } catch (DecoderFallbackException) { error = 1113; return false;
            } catch (OutOfMemoryException) { error = 8; return false;
            } finally {
                if (!ok) Log.Append("API Unicode 配置保存失败：" + stage + "（Windows 错误：" + error + "）");
                if (tmp != null && !ok && !DeleteFileW(tmp) && Marshal.GetLastWin32Error() != 2)
                    Log.Append("API 临时配置清理失败（Windows 错误：" + Marshal.GetLastWin32Error() + "）");
                if (bytes != null) Array.Clear(bytes, 0, bytes.Length);
            }
        }
    }
}
