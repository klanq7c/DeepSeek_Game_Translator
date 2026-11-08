"""源链接真实预检凭据解析：单个 ready 行不能证明已完成探针握手。"""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
READY = 'DST_GODOT_AUTOLOAD_READY_V1'
ACK = 'DST_GODOT_AUTOLOAD_ACK_V2'


def execute(args):
    result = subprocess.run([str(arg) for arg in args], capture_output=True, timeout=30,
                            creationflags=subprocess.CREATE_NO_WINDOW)
    output = (result.stdout + result.stderr).decode('utf-8', 'replace').replace('\r\n', '\n')
    assert result.returncode == 0, output
    return output


def main():
    c = (ROOT / 'native/src/launcher/ui.c').read_text(encoding='utf-8-sig')
    cs = (ROOT / 'native/src/launcher_cs/GodotLaunch.cs').read_text(encoding='utf-8-sig')
    c_helper = 'typedef struct GodotAutoloadProof {' + c.split('typedef struct GodotAutoloadProof {', 1)[1].split('\n/* 某些导出的', 1)[0]
    cs_helper = 'private sealed class AutoloadProofState' + cs.split('private sealed class AutoloadProofState', 1)[1].split('\n        /* godot_runtime_autoload_preflight', 1)[0]
    assert 'waited == WAIT_TIMEOUT && godot_autoload_proof_consume(&proof, output, got)' in c
    assert 'waited == WAIT_TIMEOUT && ConsumeAutoloadProof(proof, output, (int)got)' in cs
    c_runtime = c.split('static int godot_runtime_autoload_preflight(', 1)[1].split('static int godot_main_pack_supported(', 1)[0]
    cs_runtime = cs.split('public static bool RuntimeAutoloadPreflight(', 1)[1].split('public static bool MainPackSupported(', 1)[0]
    assert 'SetFilePointerEx(reader' not in c_runtime and 'SetFilePointerEx(reader' not in cs_runtime, 'capture must advance across chunks'
    root = Path(tempfile.mkdtemp(prefix='dst_preflight_handshake_')).resolve()
    try:
        (root / 'probe.c').write_text('#include <stdio.h>\n#include <stdlib.h>\n#include <string.h>\n' + c_helper + r'''
int main(int argc, char **argv) {
    if (argc != 3) return 2;
    FILE *input = fopen(argv[1], "rb");
    int chunk = atoi(argv[2]);
    if (!input || chunk <= 0 || chunk > 16384) return 3;
    char output[16384]; size_t got;
    GodotAutoloadProof state = {0}; int ok = 0;
    while ((got = fread(output, 1, (size_t)chunk, input)) != 0) {
        if (godot_autoload_proof_consume(&state, output, got)) { ok = 1; break; }
    }
    fclose(input); printf("%d\n", ok); return 0;
}
''', encoding='utf-8')
        (root / 'probe.cs').write_text('using System; using System.IO; class Probe {\n' + cs_helper + r'''
static int Main(string[] args) {
    int chunk = int.Parse(args[1]);
    var state = new AutoloadProofState(); var output = new byte[chunk]; bool ok = false;
    using (var input = File.OpenRead(args[0])) {
        int got;
        while ((got = input.Read(output, 0, output.Length)) != 0) {
            if (ConsumeAutoloadProof(state, output, got)) { ok = true; break; }
        }
    }
    Console.WriteLine(ok ? 1 : 0); return 0;
} }
''', encoding='utf-8')
        gcc = ROOT / 'native/toolchain/w64devkit/bin/gcc.exe'
        if not gcc.is_file():
            gcc = shutil.which('gcc')
            assert gcc, 'gcc not found'
        os.environ['PATH'] = str(Path(gcc).parent) + os.pathsep + os.environ['PATH']
        execute([gcc, '-std=c17', '-O2', '-Wall', '-Wextra', '-Werror', root / 'probe.c', '-o', root / 'c.exe'])
        csc = Path(os.environ['WINDIR']) / 'Microsoft.NET/Framework64/v4.0.30319/csc.exe'
        execute([csc, '/nologo', '/target:exe', '/out:' + str(root / 'cs.exe'), root / 'probe.cs'])
        cases = (
            ('ready without acknowledgement', READY + '\n', False),
            ('acknowledgement without ready', ACK + '\n', False),
            ('complete handshake', READY + '\n' + ACK + '\n', True),
            ('CRLF handshake', READY + '\r\n' + ACK + '\r\n', True),
            ('quoted markers in error', 'ERROR: "' + READY + '"\nERROR: "' + ACK + '"\n', False),
            ('partial acknowledgement', READY + '\n' + ACK, False),
            ('invalid first occurrence', 'ERROR: ' + READY + '\n' + READY + '\n' + ACK + '\n', True),
            ('out of order', ACK + '\n' + READY + '\n', False),
            ('long startup log crosses the old capture window', 'L' * 16349 + '\n' + READY + '\n' + ACK + '\n', True),
            ('oversized lines between proof stages', 'L' * 70000 + '\n' + READY + '\n' + 'L' * 70000 + '\n' + ACK + '\n', True),
            ('binary prefix is not a proof line', '\0' + READY + '\n' + ACK + '\n', False),
            ('binary line cannot hide a later real handshake', '\0' + READY + '\n' + READY + '\n' + ACK + '\n', True),
        )
        failures = []
        for name, text, expected in cases:
            (root / 'capture.bin').write_bytes(text.encode('ascii'))
            for chunk in (1, 7, 16384):
                outputs = [execute([root / binary, root / 'capture.bin', chunk]) for binary in ('c.exe', 'cs.exe')]
                if outputs != [str(int(expected)) + '\n'] * 2:
                    failures.append(name + f' chunk={chunk}: ' + repr(outputs))
            print('[CHECK] Godot handshake C/C# chunks=1/7/16384: ' + name)
        assert not failures, '\n'.join(failures)
        print('[PASS] Godot handshake: 36 chunk scenarios per binary, C/C# parity')
    finally:
        assert root.parent == Path(tempfile.gettempdir()).resolve() and root.name.startswith('dst_preflight_handshake_')
        for entry in root.iterdir():
            if not entry.is_file() or entry.is_symlink():
                raise RuntimeError('Unexpected fixture entry retained: ' + str(entry))
            entry.unlink()
        root.rmdir()


if __name__ == '__main__':
    main()
