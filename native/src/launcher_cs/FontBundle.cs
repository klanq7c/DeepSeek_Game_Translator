using System;
using System.Linq;

namespace DstLauncher
{
    /* font_bundle.c 的对等实现；保留授权文件并拒绝覆盖不匹配的磁盘资源。 */
    internal static class FontBundle
    {
        internal const string GodotLicenseOwner = "DST_FONT_OWNED_V1:NotoSansCJKsc-Regular.otf:Godot-OFL\n";
        private static bool EnsureAsset(string path, string name)
        {
            byte[] data = EmbeddedScripts.BytesOrNull(name);
            if (data == null) {
                Log.Append("CJK font bundle resource " + name + " is missing or empty.");
                return false;
            }
            if (SafeFs.Exists(path)) {
                if (SafeFs.PathHasReparsePoint(path, true)) {
                    Log.Append("CJK font bundle asset is a reparse point: " + path);
                    return false;
                }
                byte[] existing = SafeFs.ReadBytes(path);
                bool matches = existing != null && existing.SequenceEqual(data);
                if (!matches) Log.Append("CJK font bundle asset differs from embedded bytes: " + path);
                return matches;
            }
            if (!SafeFs.WriteBytesIfAbsentSafe(path, data)) {
                Log.Append("CJK font bundle asset could not be created (Windows error " + SafeFs.LastError + "): " + path);
                return false;
            }
            return true;
        }

        internal static bool Source(out string font, out string license)
        {
            font = PathUtil.Join(Launcher.Root, "assets\\fonts\\NotoSansCJKsc-Regular.otf");
            license = PathUtil.Join(Launcher.Root, "assets\\fonts\\OFL.txt");
            return EnsureAsset(license, "IDR_PAYLOAD_CJK_FONT_LICENSE") &&
                   EnsureAsset(font, "IDR_PAYLOAD_CJK_FONT");
        }

        internal static bool EnsureGodotLicense(string gameDir)
        {
            string font, license;
            if (gameDir == null || !Source(out font, out license)) return false;
            string dst = PathUtil.Join(gameDir, "dst_godot_patch.OFL.txt");
            string marker = dst + ".dst-owned";
            if (SafeFs.Exists(dst)) {
                if (SafeFs.FilesEqualStreaming(dst, license)) return true;
                Log.Append("Godot: existing font license differs; patch build cancelled: " + dst);
                return false;
            }
            if (SafeFs.Exists(marker)) {
                Log.Append("Godot: font license ownership marker already exists; preserved: " + marker);
                return false;
            }
            if (!SafeFs.CopyFileIfAbsentSafe(license, dst)) {
                Log.Append("Godot: could not create bundled font license (Windows error " + SafeFs.LastError + "): " + dst);
                return false;
            }
            if (!SafeFs.WriteTextUtf8(marker, GodotLicenseOwner)) {
                Log.Append("Godot: font license was copied but ownership could not be recorded (Windows error " + SafeFs.LastError + "): " + dst);
            }
            return true;
        }
    }
}
