"""测试真实 C/C# 运行时包处理；Windows AES 夹具生成仅使用标准库。"""
import argparse
import ctypes
import hashlib
import pathlib
import shutil
import struct
import subprocess
import tempfile


def cfb_encrypt(data, key, iv):
    bcrypt = ctypes.WinDLL('bcrypt.dll')
    aes = ctypes.c_void_p()
    handle = ctypes.c_void_p()
    check = lambda code: code >= 0 or (_ for _ in ()).throw(RuntimeError('CNG fixture error'))
    check(bcrypt.BCryptOpenAlgorithmProvider(ctypes.byref(aes), 'AES', None, 0))
    mode = ctypes.create_unicode_buffer('ChainingModeECB')
    try:
        check(bcrypt.BCryptSetProperty(aes, 'ChainingMode', mode, ctypes.sizeof(mode), 0))
        keybuf = ctypes.create_string_buffer(key)
        check(bcrypt.BCryptGenerateSymmetricKey(aes, ctypes.byref(handle), None, 0, keybuf, len(key), 0))
        feedback = iv
        result = bytearray()
        for offset in range(0, len(data), 16):
            block = ctypes.create_string_buffer(16)
            written = ctypes.c_ulong()
            check(bcrypt.BCryptEncrypt(handle, ctypes.create_string_buffer(feedback), 16, None, None, 0,
                                       block, 16, ctypes.byref(written), 0))
            feedback = bytes(a ^ b for a, b in zip(data[offset:offset + 16], block.raw))
            result.extend(feedback)
        return bytes(result)
    finally:
        if handle.value:
            bcrypt.BCryptDestroyKey(handle)
        bcrypt.BCryptCloseAlgorithmProvider(aes, 0)


KEY = hashlib.sha256(b'Godot encrypted directory regression fixture').digest()
IV = bytes(range(16))


def pe_file(key=KEY, broken=False):
    pe = bytearray(1024)
    pe[:2] = b'MZ'
    struct.pack_into('<I', pe, 60, 128)
    pe[128:132] = b'PE\0\0'
    struct.pack_into('<HH', pe, 132, 0x8664, 1)
    pe[152:160] = b'.data\0\0\0'
    struct.pack_into('<II', pe, 168, 512, 512)
    struct.pack_into('<I', pe, 188, 0xc0000040)
    pe[544:576] = key
    if broken:
        struct.pack_into('<I', pe, 172, 0xfffffff0)
    return bytes(pe)


def pack_file(encrypted=True, corrupt=None, key=KEY):
    resources = [('ui/menu.tscn', b'[gd_scene format=3]\n[node name="Title" type="Label"]\ntext="Start Game"\n', 0),
                 ('fonts/other.ttf', b'unchanged encrypted resource', 1 if corrupt == 'file-encrypted' else 0)]
    content = bytearray()
    directory = bytearray()
    for name, data, flags in resources:
        path = name.encode() + b'\0'
        path += bytes((-len(path)) % 4)
        offset = len(content)
        content.extend(data)
        directory.extend(struct.pack('<I', len(path)) + path + struct.pack('<QQ', offset, len(data)) + hashlib.md5(data).digest() + struct.pack('<I', flags))
    if corrupt == 'entry-bounds':
        struct.pack_into('<Q', directory, 4 + struct.unpack_from('<I', directory)[0], 0xffffffffffffffff)
    if corrupt == 'entry-flags':
        struct.pack_into('<I', directory, len(directory) - 4, 2)
    header = bytearray(112)
    struct.pack_into('<IIIIIIQQ', header, 0, 0x43504447, 3, 4, 6, 1, 3 if encrypted else 2, 112, 112 + len(content))
    count = 3 if corrupt == 'entry-count' else len(resources)
    if encrypted:
        digest = hashlib.md5(directory).digest()
        if corrupt == 'md5':
            digest = bytes(16)
        length = 0xffffffffffffffff if corrupt == 'length' else len(directory)
        padded = directory + bytes((-len(directory)) % 16)
        wrapper = digest + struct.pack('<Q', length) + IV + cfb_encrypt(padded, key, IV)
    else:
        wrapper = directory
    return bytes(header + content + struct.pack('<I', count) + wrapper)


def entries(data):
    base, directory = struct.unpack_from('<QQ', data, 24)
    count = struct.unpack_from('<I', data, directory)[0]
    cursor = directory + 4
    result = {}
    for _ in range(count):
        n = struct.unpack_from('<I', data, cursor)[0]
        cursor += 4
        name = data[cursor:cursor + n].rstrip(b'\0').decode().removeprefix('res://')
        cursor += n
        offset, size = struct.unpack_from('<QQ', data, cursor)
        flags = struct.unpack_from('<I', data, cursor + 32)[0]
        result[name] = (data[base + offset:base + offset + size], flags)
        cursor += 36
    return result


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--c', type=pathlib.Path)
    parser.add_argument('--c-root', type=pathlib.Path)
    parser.add_argument('--cs', type=pathlib.Path, required=True)
    args = parser.parse_args()
    if args.c_root:
        args.c = args.c_root / 'ds游戏翻译器.exe'
    programs = ([('C', args.c.resolve(), '--godot-runtime-pack-and-exit')] if args.c else []) + [('C#', args.cs.resolve(), '--godot-runtime-pack')]
    cases = [('plain', False, None, True), ('encrypted', True, None, True),
             ('ascii-key', True, None, True), ('zero-key', True, None, True),
             ('file-encrypted', True, 'file-encrypted', True),
             ('md5', True, 'md5', False), ('length', True, 'length', False),
             ('entry-bounds', True, 'entry-bounds', False), ('entry-count', True, 'entry-count', False),
             ('entry-flags', True, 'entry-flags', False), ('no-key', True, None, False),
             ('bad-pe', True, None, False)]
    checked = 0
    with tempfile.TemporaryDirectory(prefix='dst-godot-encrypted-') as temp:
        root = pathlib.Path(temp)
        for name, encrypted, corrupt, success in cases:
            key = b'01234567890123456789012345678901' if name == 'ascii-key' else bytes(32) if name == 'zero-key' else KEY
            outputs = []
            for label, program, command in programs:
                game = root / (name + '-' + label.replace('#', 's'))
                game.mkdir()
                source = pack_file(encrypted, corrupt, key)
                exe = pe_file(bytes([0xa5]) * 32 if name == 'no-key' else key, name == 'bad-pe')
                (game / 'Game.pck').write_bytes(source)
                (game / 'Game.exe').write_bytes(exe)
                proc = subprocess.run([str(program), command, str(game)], capture_output=True, timeout=20)
                log = proc.stdout.decode('utf-8', 'replace')
                assert f'result={int(success)}' in log, (name, label, log)
                assert (game / 'Game.pck').read_bytes() == source
                assert (game / 'Game.exe').read_bytes() == exe
                patch = game / 'dst_godot_patch.pck'
                assert patch.exists() == success
                assert not (game / 'dst_godot_patch.building').exists()
                if success:
                    output = patch.read_bytes()
                    assert struct.unpack_from('<I', output, 20)[0] == 2
                    output_entries = entries(output)
                    assert output_entries['ui/menu.tscn'][0] in source
                    assert output_entries['fonts/other.ttf'] == (b'unchanged encrypted resource', int(corrupt == 'file-encrypted'))
                    assert b'DST_GODOT_AUTOLOAD_READY_V1' in output_entries['dst_godot_autoload.gd'][0]
                    assert b'[autoload_prepend]' in output_entries['override.cfg'][0]
                    if encrypted:
                        assert 'checksum-verified encrypted directory normalized' in log
                    outputs.append(output)
                else:
                    assert 'encrypted-directory preparation failed' in log
                checked += 1
            assert len(outputs) < 2 or outputs[0] == outputs[1], (name, 'C/C# patch bytes differ')
            print('PASS', name, flush=True)
    print(f'PASS: {checked} runtime-pack cases; source preservation, bounds, checksums, encrypted resources, C/C# parity')


if __name__ == '__main__':
    main()
