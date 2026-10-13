#include "godot_encrypted_pack.h"
#include "fsutil.h"
#include "ui.h"
#include <bcrypt.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define DIR_LIMIT (64u * 1024u * 1024u)
#define SCAN_LIMIT (32u * 1024u * 1024u)
#define KEY_LIMIT 1000000u

static uint32_t u32(const unsigned char *p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
static uint64_t u64(const unsigned char *p) { return (uint64_t)u32(p) | (uint64_t)u32(p + 4) << 32; }
static void put64(unsigned char *p, uint64_t n) { for (int i = 0; i < 8; i++) p[i] = (unsigned char)(n >> (i * 8)); }
static int at(HANDLE h, uint64_t offset, void *p, DWORD n, int write) {
    LARGE_INTEGER pos; pos.QuadPart = (LONGLONG)offset;
    DWORD got = 0;
    return offset <= INT64_MAX && SetFilePointerEx(h, pos, NULL, FILE_BEGIN) &&
        (write ? WriteFile(h, p, n, &got, NULL) : ReadFile(h, p, n, &got, NULL)) && got == n;
}
static int cfb(BCRYPT_KEY_HANDLE key, const unsigned char *iv, const unsigned char *cipher,
               unsigned char *out, DWORD n) {
    unsigned char feedback[16], block[16];
    memcpy(feedback, iv, 16);
    for (DWORD off = 0; off < n; off += 16) {
        ULONG got = 0;
        if (BCryptEncrypt(key, feedback, 16, NULL, NULL, 0, block, 16, &got, 0) < 0 || got != 16) return 0;
        DWORD size = n - off < 16 ? n - off : 16;
        for (DWORD j = 0; j < size; j++) out[off + j] = cipher[off + j] ^ block[j];
        memcpy(feedback, cipher + off, size);
    }
    return 1;
}
static int md5_ok(BCRYPT_ALG_HANDLE md5, unsigned char *data, DWORD n, const unsigned char *expected) {
    BCRYPT_HASH_HANDLE hash = NULL;
    unsigned char digest[16];
    int ok = BCryptCreateHash(md5, &hash, NULL, 0, NULL, 0, 0) >= 0 &&
        BCryptHashData(hash, data, n, 0) >= 0 && BCryptFinishHash(hash, digest, 16, 0) >= 0 &&
        memcmp(digest, expected, 16) == 0;
    if (hash) BCryptDestroyHash(hash);
    return ok;
}
static int valid_directory(const unsigned char *p, uint32_t n, uint32_t count, uint64_t base, uint64_t size) {
    uint32_t pos = 0;
    if (!count || count > 200000u || base > size) return 0;
    for (uint32_t i = 0; i < count; i++) {
        if (pos > n || n - pos < 4) return 0;
        uint32_t len = u32(p + pos); pos += 4;
        if (!len || len > 4096 || (len & 3) || len > n - pos) return 0;
        pos += len;
        if (n - pos < 36) return 0;
        uint64_t off = u64(p + pos), bytes = u64(p + pos + 8);
        /* 已加密的资源仍由游戏读取，补丁层不解密/替换这些条目。 */
        if ((u32(p + pos + 32) & ~1u) || off > size - base || bytes > size - base - off) return 0;
        pos += 36;
    }
    return pos == n;
}

/* Godot 4.6.1 的 FileAccessEncrypted：MD5 + 长度 + IV + AES-256-CFB128。
   script_encryption_key 是导出程序 .data 的 32 字节数组，候选只存在于内存。
   加密目录不能直接追加条目；只有完整 MD5 和全部条目边界通过后才改写副本。
   找不到编译期数组、损坏或超界均明确失败，日志不记录密钥，见 FAILURE_TRANSPARENCY。 */
int godot_normalize_encrypted_directory(const WCHAR *copy, const WCHAR *game_exe) {
    HANDLE pack = INVALID_HANDLE_VALUE, exe = INVALID_HANDLE_VALUE;
    BCRYPT_ALG_HANDLE aes = NULL, md5 = NULL;
    unsigned char *cipher = NULL, *plain = NULL, *section = NULL;
    unsigned char header[112], envelope[44], dos[64], pe[24], sh[40];
    LARGE_INTEGER size;
    int ok = 0, verified = 0;
    if (path_has_reparse_point(copy, 1)) goto done;
    pack = CreateFileW(copy, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (pack == INVALID_HANDLE_VALUE || !GetFileSizeEx(pack, &size) || size.QuadPart < 24 ||
        !at(pack, 0, header, 24, 0)) goto done;
    uint32_t format = u32(header + 4), flags = u32(header + 20);
    if (format == 1 || !(flags & 1u)) { ok = 1; goto done; }
    if (format != 3 || (flags & ~3u) || size.QuadPart < 112 || !at(pack, 0, header, 112, 0)) {
        append_log(L"Godot: encrypted directory requires a supported PCK v3 export; patch discarded."); goto done;
    }
    uint64_t pack_size = (uint64_t)size.QuadPart, dir = u64(header + 32), base = u64(header + 24);
    if (dir > pack_size || pack_size - dir < 44 || !at(pack, dir, envelope, 44, 0)) goto done;
    uint32_t count = u32(envelope);
    uint64_t length = u64(envelope + 20), padded = (length + 15u) & ~15ULL;
    if (length < 40 || length > DIR_LIMIT || padded > pack_size - dir - 44 || count > 200000u) goto done;
    cipher = (unsigned char *)malloc((size_t)padded);
    plain = (unsigned char *)malloc((size_t)padded);
    if (!cipher || !plain || !at(pack, dir + 44, cipher, (DWORD)padded, 0)) goto done;
    if (!game_exe || path_has_reparse_point(game_exe, 1)) goto done;
    exe = CreateFileW(game_exe, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                      NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (exe == INVALID_HANDLE_VALUE || !GetFileSizeEx(exe, &size) || size.QuadPart < 64 ||
        !at(exe, 0, dos, 64, 0) || dos[0] != 'M' || dos[1] != 'Z') goto done;
    uint64_t exe_size = (uint64_t)size.QuadPart, pe_offset = u32(dos + 60);
    if (pe_offset > exe_size || exe_size - pe_offset < 24 || !at(exe, pe_offset, pe, 24, 0) || u32(pe) != 0x4550) goto done;
    uint32_t sections = pe[6] | (uint32_t)pe[7] << 8;
    uint64_t table = pe_offset + 24 + (pe[20] | (uint32_t)pe[21] << 8);
    if (!sections || sections > 128 || table > exe_size || sections * 40u > exe_size - table) goto done;
    if (BCryptOpenAlgorithmProvider(&aes, BCRYPT_AES_ALGORITHM, NULL, 0) < 0 ||
        BCryptSetProperty(aes, BCRYPT_CHAINING_MODE, (PUCHAR)BCRYPT_CHAIN_MODE_ECB, sizeof(BCRYPT_CHAIN_MODE_ECB), 0) < 0 ||
        BCryptOpenAlgorithmProvider(&md5, BCRYPT_MD5_ALGORITHM, NULL, 0) < 0) goto done;
    uint32_t scanned = 0, candidates = 0;
    int zero_tried = 0;
    for (uint32_t i = 0; i < sections && !verified; i++) {
        if (!at(exe, table + i * 40u, sh, 40, 0)) goto done;
        /* 编译期非 const 密钥只在可写的已初始化数据区；扫描预算有上限。 */
        if ((u32(sh + 36) & 0x80000040u) != 0x80000040u) continue;
        uint32_t bytes = u32(sh + 16), offset = u32(sh + 20);
        if (bytes < 32 || offset > exe_size || bytes > exe_size - offset || bytes > SCAN_LIMIT - scanned) goto done;
        scanned += bytes;
        section = (unsigned char *)malloc(bytes);
        if (!section || !at(exe, offset, section, bytes, 0)) goto done;
        for (uint32_t pos = 0; pos <= bytes - 32 && !verified; pos += 8) {
            unsigned char zeros[32] = {0}, sample[32];
            if (!memcmp(section + pos, zeros, 32)) { if (zero_tried) continue; zero_tried = 1; }
            if (++candidates > KEY_LIMIT) goto done;
            BCRYPT_KEY_HANDLE key = NULL;
            if (BCryptGenerateSymmetricKey(aes, &key, NULL, 0, section + pos, 32, 0) < 0) goto done;
            int decrypted = cfb(key, envelope + 28, cipher, sample, 32);
            uint32_t path_len = decrypted ? u32(sample) : 0;
            if (path_len >= 4 && path_len <= 4096 && !(path_len & 3) &&
                cfb(key, envelope + 28, cipher, plain, (DWORD)padded) &&
                md5_ok(md5, plain, (DWORD)length, envelope + 4) &&
                valid_directory(plain, (DWORD)length, count, base, pack_size)) verified = 1;
            BCryptDestroyKey(key);
            if (!decrypted) goto done;
        }
        SecureZeroMemory(section, bytes); free(section); section = NULL;
    }
    if (!verified) { append_log(L"Godot: no checksum-verified encrypted directory found in export data; patch discarded."); goto done; }
    unsigned char count_bytes[4], position[8], flag_bytes[4];
    memcpy(count_bytes, envelope, 4);
    put64(position, pack_size);
    memcpy(flag_bytes, header + 20, 4); flag_bytes[0] &= (unsigned char)~1u;
    ok = at(pack, pack_size, count_bytes, 4, 1) && at(pack, pack_size + 4, plain, (DWORD)length, 1) &&
        at(pack, 32, position, 8, 1) && at(pack, 20, flag_bytes, 4, 1) && FlushFileBuffers(pack);
    if (ok) append_log(L"Godot: checksum-verified encrypted directory normalized in the owned patch copy.");
done:
    if (!ok) append_log(L"Godot: encrypted-directory preparation failed; original game files remain unchanged.");
    if (section) free(section);
    if (plain) free(plain);
    if (cipher) free(cipher);
    if (md5) BCryptCloseAlgorithmProvider(md5, 0);
    if (aes) BCryptCloseAlgorithmProvider(aes, 0);
    if (exe != INVALID_HANDLE_VALUE) CloseHandle(exe);
    if (pack != INVALID_HANDLE_VALUE) CloseHandle(pack);
    return ok;
}
