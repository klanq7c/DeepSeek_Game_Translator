using System;
using System.IO;
using System.Reflection;
using System.Text;

namespace DstLauncher
{
    /*
     * EmbeddedScripts —— embedded.c 的对等实现。
     *
     * C 版把 payloads/RenPy、payloads/RPGMaker、payloads/Godot 下的脚本以 RCDATA
     * 301..304 嵌入；C# 版用 EmbeddedResource 嵌入同一批文件（见 launcher_cs.csproj，
     * LogicalName 与资源 ID 同名）。部署时按字节原样写出，两版产物逐字节一致。
     *
     * 缺失资源不是可恢复状态：记录日志并返回 null，调用方负责向用户说明后果
     * （与 embedded_script_text 一致）。
     */
    public static class EmbeddedScripts
    {
        public const string RenPyHook = "IDR_SCRIPT_RENPY_HOOK";
        public const string RpgmHook = "IDR_SCRIPT_RPGM_HOOK";
        public const string GodotRuntimeG3 = "IDR_SCRIPT_GODOT_RUNTIME_G3";
        public const string GodotRuntimeG4 = "IDR_SCRIPT_GODOT_RUNTIME_G4";

        public static byte[] Bytes(string logicalName)
        {
            byte[] bytes = BytesOrNull(logicalName);
            if (bytes == null) Log.Append("内嵌脚本资源缺失：" + logicalName + "（启动器构建不完整）。");
            return bytes;
        }

        /* embedded_resource_bytes：资源缺失或为空时静默返回 null，由调用方决定是否记录
           （self_update.c 对可选的 Unity 程序集缺失不记日志，只对必需组件计数）。 */
        public static byte[] BytesOrNull(string logicalName)
        {
            Assembly asm = typeof(EmbeddedScripts).Assembly;
            using (Stream s = asm.GetManifestResourceStream(logicalName)) {
                if (s == null) return null;
                var ms = new MemoryStream();
                s.CopyTo(ms);
                byte[] bytes = ms.ToArray();
                return bytes.Length == 0 ? null : bytes; /* SizeofResource == 0 视为缺失 */
            }
        }

        /* embedded_script_text：脚本按 UTF-8 解释；含 NUL 字节视为损坏（C 版以 NUL 结尾截断，
           这里直接拒绝并记录，避免写出半个脚本）。 */
        public static string Text(string logicalName)
        {
            byte[] bytes = Bytes(logicalName);
            if (bytes == null) return null;
            if (Array.IndexOf(bytes, (byte)0) >= 0) {
                Log.Append("内嵌脚本资源含 NUL 字节，已拒绝使用：" + logicalName);
                return null;
            }
            return new UTF8Encoding(false, true).GetString(bytes);
        }
    }
}
