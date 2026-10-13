using System;
using System.IO;
using System.Security.Cryptography;

namespace DstLauncher
{
    public static partial class GodotPatch
    {
        // FileAccessEncrypted 的 v3 目录：MD5、长度、IV、AES-256-CFB128。
        // 编译期密钥仅在内存中使用，完整校验及条目边界通过后才改写自有副本。
        // 无匹配/超界/损坏均拒绝补丁并记录诊断，原始游戏文件保持只读。
        private static bool NormalizeEncryptedDirectory(string copy, string gameExe)
        {
            const int dirLimit = 64 * 1024 * 1024, scanLimit = 32 * 1024 * 1024, keyLimit = 1000000;
            if (SafeFs.PathHasReparsePoint(copy, true)) return EncryptedPackFailure();
            try {
                using (var pack = new FileStream(copy, FileMode.Open, FileAccess.ReadWrite, FileShare.None)) {
                    var header = new byte[112];
                    if (pack.Length < 24 || !ReadAtExact(pack, 0, header, 24)) return EncryptedPackFailure();
                    uint format = ReadU32Le(header, 4), flags = ReadU32Le(header, 20);
                    if (format == 1 || (flags & 1u) == 0) return true;
                    if (format != 3 || (flags & ~3u) != 0 || pack.Length < 112 || !ReadAtExact(pack, 0, header, 112)) {
                        Log.Append("Godot: encrypted directory requires a supported PCK v3 export; patch discarded.");
                        return EncryptedPackFailure();
                    }
                    ulong size = (ulong)pack.Length, dir = ReadU64Le(header, 32), fileBase = ReadU64Le(header, 24);
                    var envelope = new byte[44];
                    if (dir > size || size - dir < 44 || !ReadAtExact(pack, dir, envelope, 44)) return EncryptedPackFailure();
                    uint count = ReadU32Le(envelope, 0);
                    ulong length = ReadU64Le(envelope, 20);
                    if (length < 40 || length > dirLimit || count > 200000u) return EncryptedPackFailure();
                    int padded = ((int)length + 15) & ~15;
                    if ((ulong)padded > size - dir - 44) return EncryptedPackFailure();
                    var cipher = new byte[padded];
                    var plain = new byte[padded];
                    var iv = new byte[16];
                    Buffer.BlockCopy(envelope, 28, iv, 0, 16);
                    if (!ReadAtExact(pack, dir + 44, cipher, padded) || gameExe == null ||
                        SafeFs.PathHasReparsePoint(gameExe, true)) return EncryptedPackFailure();
                    bool verified = false;
                    using (var exe = new FileStream(gameExe, FileMode.Open, FileAccess.Read, FileShare.ReadWrite | FileShare.Delete))
                    using (var aes = Aes.Create())
                    using (var md5 = MD5.Create()) {
                        var dos = new byte[64]; var pe = new byte[24]; var sh = new byte[40];
                        if (exe.Length < 64 || !ReadAtExact(exe, 0, dos, 64) || dos[0] != 'M' || dos[1] != 'Z') return EncryptedPackFailure();
                        ulong exeSize = (ulong)exe.Length, peOffset = ReadU32Le(dos, 60);
                        if (peOffset > exeSize || exeSize - peOffset < 24 || !ReadAtExact(exe, peOffset, pe, 24) ||
                            ReadU32Le(pe, 0) != 0x4550u) return EncryptedPackFailure();
                        int sections = pe[6] | pe[7] << 8;
                        ulong table = peOffset + 24u + (uint)(pe[20] | pe[21] << 8);
                        if (sections < 1 || sections > 128 || table > exeSize || (ulong)(sections * 40) > exeSize - table) return EncryptedPackFailure();
                        aes.Mode = CipherMode.ECB; aes.Padding = PaddingMode.None; aes.KeySize = 256;
                        int scanned = 0, candidates = 0;
                        bool zeroTried = false;
                        var key = new byte[32]; var sample = new byte[32];
                        for (int i = 0; i < sections && !verified; i++) {
                            if (!ReadAtExact(exe, table + (ulong)(i * 40), sh, 40)) return EncryptedPackFailure();
                            if ((ReadU32Le(sh, 36) & 0x80000040u) != 0x80000040u) continue;
                            uint bytes = ReadU32Le(sh, 16), offset = ReadU32Le(sh, 20);
                            if (bytes < 32 || offset > exeSize || bytes > exeSize - offset || bytes > scanLimit - scanned) return EncryptedPackFailure();
                            scanned += (int)bytes;
                            var section = new byte[bytes];
                            if (!ReadAtExact(exe, offset, section, section.Length)) return EncryptedPackFailure();
                            for (int pos = 0; pos <= section.Length - 32 && !verified; pos += 8) {
                                Buffer.BlockCopy(section, pos, key, 0, 32);
                                bool zero = true;
                                foreach (byte b in key) if (b != 0) { zero = false; break; }
                                if (zero) { if (zeroTried) continue; zeroTried = true; }
                                if (++candidates > keyLimit) return EncryptedPackFailure();
                                aes.Key = key;
                                using (var encrypt = aes.CreateEncryptor()) {
                                    DecryptCfb(encrypt, iv, cipher, sample, 32);
                                    uint pathLength = ReadU32Le(sample, 0);
                                    if (pathLength < 4 || pathLength > 4096 || (pathLength & 3) != 0) continue;
                                    DecryptCfb(encrypt, iv, cipher, plain, padded);
                                    byte[] digest = md5.ComputeHash(plain, 0, (int)length);
                                    bool match = true;
                                    for (int j = 0; j < 16; j++) if (digest[j] != envelope[4 + j]) match = false;
                                    verified = match && ValidEncryptedDirectory(plain, (int)length, count, fileBase, size);
                                }
                            }
                            Array.Clear(section, 0, section.Length);
                        }
                        Array.Clear(key, 0, key.Length);
                    }
                    if (!verified) {
                        Log.Append("Godot: no checksum-verified encrypted directory found in export data; patch discarded.");
                        return EncryptedPackFailure();
                    }
                    var position = new byte[8]; var flagBytes = new byte[4];
                    WriteU64Le(position, 0, size); WriteU32Le(flagBytes, 0, flags & ~1u);
                    if (!WriteAtExact(pack, size, envelope, 4) || !WriteAtExact(pack, size + 4, plain, (int)length) ||
                        !WriteAtExact(pack, 32, position, 8) || !WriteAtExact(pack, 20, flagBytes, 4)) return EncryptedPackFailure();
                    pack.Flush(true);
                    Log.Append("Godot: checksum-verified encrypted directory normalized in the owned patch copy.");
                    return true;
                }
            } catch (IOException ex) {
                Log.Append("Godot: encrypted-directory I/O failed (Windows error " + (ex.HResult & 0xffff) + ").");
                return EncryptedPackFailure();
            } catch (UnauthorizedAccessException ex) {
                Log.Append("Godot: encrypted-directory access failed (Windows error " + (ex.HResult & 0xffff) + ").");
                return EncryptedPackFailure();
            } catch (CryptographicException ex) {
                Log.Append("Godot: encrypted-directory cryptography failed (" + ex.Message + ").");
                return EncryptedPackFailure();
            }
        }
        private static bool EncryptedPackFailure()
        {
            Log.Append("Godot: encrypted-directory preparation failed; original game files remain unchanged.");
            return false;
        }
        private static void DecryptCfb(ICryptoTransform encrypt, byte[] iv, byte[] cipher, byte[] output, int length)
        {
            var feedback = (byte[])iv.Clone(); var block = new byte[16];
            for (int off = 0; off < length; off += 16) {
                encrypt.TransformBlock(feedback, 0, 16, block, 0);
                int size = Math.Min(16, length - off);
                for (int j = 0; j < size; j++) output[off + j] = (byte)(cipher[off + j] ^ block[j]);
                Buffer.BlockCopy(cipher, off, feedback, 0, size);
            }
        }
        private static bool ValidEncryptedDirectory(byte[] p, int n, uint count, ulong fileBase, ulong size)
        {
            int pos = 0;
            if (count == 0 || count > 200000 || fileBase > size) return false;
            for (uint i = 0; i < count; i++) {
                if (n - pos < 4) return false;
                uint len = ReadU32Le(p, pos); pos += 4;
                if (len == 0 || len > 4096 || (len & 3) != 0 || len > n - pos) return false;
                pos += (int)len;
                if (n - pos < 36) return false;
                ulong offset = ReadU64Le(p, pos), bytes = ReadU64Le(p, pos + 8);
                if ((ReadU32Le(p, pos + 32) & ~1u) != 0 || offset > size - fileBase || bytes > size - fileBase - offset) return false;
                pos += 36;
            }
            return pos == n;
        }
    }
}
