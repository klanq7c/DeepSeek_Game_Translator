"""执行真实 C/C# 生成器，核对两版 autoload 并在 Godot 3/4 验证 stdin 协议。"""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
READY = 'DST_GODOT_AUTOLOAD_READY_V1'
ACK = 'DST_GODOT_AUTOLOAD_ACK_V2'
CONTINUE = 'DST_GODOT_AUTOLOAD_CONTINUE_V2\n'


def function(source, signature, end):
    return signature + source.split(signature, 1)[1].split(end, 1)[0] + end


def execute(args, **kwargs):
    result = subprocess.run([str(arg) for arg in args], capture_output=True, timeout=40,
                            creationflags=subprocess.CREATE_NO_WINDOW, **kwargs)
    output = (result.stdout + result.stderr).decode('utf-8', 'replace').replace('\r\n', '\n')
    assert result.returncode == 0, output
    return output


def main():
    c = (ROOT / 'native/src/launcher/godot_patch.c').read_text(encoding='utf-8-sig')
    cs = (ROOT / 'native/src/launcher_cs/GodotPatchPack.cs').read_text(encoding='utf-8-sig')
    c_helper = function(c, 'static char *replace_all_text(', '\n}\n')
    c_builder = function(c, 'static char *build_godot_runtime_autoload(', '\n}\n')
    cs_builder = function(cs, 'private static string BuildRuntimeAutoload(', '\n        }\n')
    root = Path(tempfile.mkdtemp(prefix='dst_autoload_runtime_')).resolve()
    try:
        (root / 'generator.c').write_text(r'''
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
static char *payload;
static const char *godot_runtime_sidecar_source(int godot3) { (void)godot3; return payload; }
static char *godot_runtime_script_with_font(const char *source) { return _strdup(source); }
''' + c_helper + c_builder + r'''
int main(int argc, char **argv) {
    if (argc != 3) return 2;
    FILE *file = fopen(argv[2], "rb");
    if (!file || fseek(file, 0, SEEK_END)) return 3;
    long size = ftell(file);
    if (size < 0 || fseek(file, 0, SEEK_SET)) return 4;
    payload = malloc((size_t)size + 1);
    if (!payload || fread(payload, 1, (size_t)size, file) != (size_t)size) return 5;
    fclose(file); payload[size] = 0;
    char *result = build_godot_runtime_autoload((uint32_t)atoi(argv[1]));
    if (!result) return 6;
    fputs(result, stdout); free(result); free(payload); return 0;
}
''', encoding='utf-8')
        (root / 'generator.cs').write_text('''using System; using System.IO; using System.Text;
class Generator {
    static string payload;
    static string RuntimeSidecarSource(bool godot3) { return payload; }
    static string RuntimeScriptWithFont(string source) { return source; }
''' + cs_builder + '''
    static int Main(string[] args) {
        Console.OutputEncoding = new UTF8Encoding(false);
        payload = File.ReadAllText(args[1]);
        Console.Write(BuildRuntimeAutoload(uint.Parse(args[0]))); return 0;
    }
}
''', encoding='utf-8')
        gcc = ROOT / 'native/toolchain/w64devkit/bin/gcc.exe'
        if not gcc.is_file():
            gcc = shutil.which('gcc')
            assert gcc, 'gcc not found'
        os.environ['PATH'] = str(Path(gcc).parent) + os.pathsep + os.environ['PATH']
        execute([gcc, '-std=c17', '-O2', '-Wall', '-Wextra', '-Werror', root / 'generator.c', '-o', root / 'c.exe'])
        csc = Path(os.environ['WINDIR']) / 'Microsoft.NET/Framework64/v4.0.30319/csc.exe'
        execute([csc, '/nologo', '/target:exe', '/out:' + str(root / 'cs.exe'), root / 'generator.cs'])
        for major in (3, 4):
            payload = ROOT / f'payloads/Godot/dst_godot_runtime_g{major}.gd'
            scripts = [execute([root / binary, major, payload]) for binary in ('c.exe', 'cs.exe')]
            assert scripts[0] == scripts[1], f'Godot {major}: C/C# generated scripts differ'
            assert '__DST_PREFLIGHT_HANDSHAKE__' not in scripts[0]
            print(f'[PASS] Godot {major}: actual C/C# autoload builders agree')
            engine = os.environ.get(f'GODOT{major}_EXE')
            if not engine:
                print(f'[SKIP] Godot {major}: configure GODOT{major}_EXE for stdin runtime checks')
                continue
            fixture = root / f'project{major}'
            fixture.mkdir()
            (fixture / 'project.godot').write_text('''config_version=4
[application]
config/name="DeepSeek isolated autoload handshake"
run/main_scene="res://main.tscn"
[autoload]
DeepSeekRuntime="*res://runtime.gd"
[rendering]
renderer/rendering_method="gl_compatibility"
quality/driver/driver_name="GLES2"
''', encoding='utf-8')
            (fixture / 'runtime.gd').write_text(scripts[0], encoding='utf-8')
            (fixture / 'main.gd').write_text('''extends Node
func _ready():
    print("DST_FIXTURE_MAIN_READY")
    get_tree().quit(0)
''', encoding='utf-8')
            (fixture / 'main.tscn').write_text('''[gd_scene load_steps=2 format=2]
[ext_resource path="res://main.gd" type="Script" id=1]
[node name="Fixture" type="Node"]
script = ExtResource( 1 )
''', encoding='utf-8')
            base = [engine, '--no-window' if major == 3 else '--headless', '--path', fixture]
            for name, data, expected, preflight in (
                ('valid preloaded input', CONTINUE, True, True),
                ('wrong input', 'WRONG\n', False, True),
                ('closed input', '', False, True),
                ('ordinary launch', CONTINUE, False, False),
            ):
                output = execute(base + (['--', '--dst-autoload-preflight'] if preflight else []), input=data.encode())
                lines = output.splitlines()
                assert (READY in lines) == preflight, output
                assert (ACK in lines) == expected, output
                assert 'DST_FIXTURE_MAIN_READY' in lines, output
                assert 'SCRIPT ERROR:' not in output and 'Parse Error:' not in output, output
                if preflight and not expected:
                    assert 'invalid stdin handshake' in output, output
                else:
                    assert 'ERROR:' not in output, output
                print(f'[PASS] Godot {major}: {name}')
    finally:
        assert root.parent == Path(tempfile.gettempdir()).resolve() and root.name.startswith('dst_autoload_runtime_')
        assert not any(entry.is_symlink() or (hasattr(entry, 'is_junction') and entry.is_junction()) for entry in root.rglob('*'))
        shutil.rmtree(root)


if __name__ == '__main__':
    main()
