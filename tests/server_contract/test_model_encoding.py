"""从真实启动器的保存/加载连接到两版生产服务器的请求构造器，不使用网络。"""
import ctypes
import json
import pathlib
import shutil
import subprocess
import sys
import tempfile

repo = pathlib.Path(__file__).resolve().parents[2]
failed = 0
passed = 0
endpoint = 'http://127.0.0.1:11434/v1/chat/completions'

def run(argv):
    return subprocess.run([str(x) for x in argv], capture_output=True, timeout=20)

with tempfile.TemporaryDirectory(prefix='dst_model_encoding_') as temp:
    root = pathlib.Path(temp).resolve()
    assert root.parent == pathlib.Path(tempfile.gettempdir()).resolve() and root.name.startswith('dst_model_encoding_')
    croot = root / 'c'; croot.mkdir()
    csroot = root / 'cs'; csroot.mkdir()
    native = croot / 'launcher.exe'
    shutil.copy2(repo / 'ds游戏翻译器.exe', native)
    cs = repo / 'build/launcher_cs/dst_launcher_cs.exe'
    builders = [repo / 'build/model_defaults/models_c.exe', repo / 'build/model_defaults/models_cs.exe']
    legacy = '; keep comment\r\n[api]\r\nmodel=before\r\ntimeout_ms=22000\r\nconcurrency=2\r\n[other]\r\nuntouched=value\r\n'
    cases = [
        ('legacy Chinese', '模型/最新', legacy.encode('ascii'), True, False),
        ('legacy emoji', 'local-😀', legacy.encode('ascii'), True, False),
        ('new emoji', 'local-😀', None, True, False),
        ('existing Unicode', '模型/😀', legacy.encode('utf-16'), True, False),
        ('UTF8 limit', '模' * 85, legacy.encode('ascii'), True, False),
        ('UTF8 overflow rejected', '模' * 86, legacy.encode('ascii'), False, False),
        ('read-only Unicode upgrade rejected', 'local-😀', legacy.encode('ascii'), False, True),
    ]
    for name, model, initial, expected, readonly in cases:
        outputs = []
        try:
            for folder, exe, flag, root_args in ((croot, native, '--api-config-set-and-exit', []), (csroot, cs, '--api-config-set', ['--root', csroot])):
                cfgdir = folder / 'config'; cfgdir.mkdir(exist_ok=True)
                ini = cfgdir / 'api.ini'
                if ini.exists(): ini.unlink()
                if initial is not None: ini.write_bytes(initial)
                if readonly: assert ctypes.windll.kernel32.SetFileAttributesW(str(ini), 1)
                saved = run([exe, flag, endpoint, model, 'fake-key', *root_args])
                if readonly: assert ctypes.windll.kernel32.SetFileAttributesW(str(ini), 128)
                assert saved.returncode == 0, saved.stderr
                assert ('result=' + str(int(expected))) in saved.stdout.decode('utf-8'), saved.stdout
                result_bytes = ini.read_bytes() if ini.exists() else None
                if not expected:
                    assert result_bytes == initial, 'failed save modified original'
                else:
                    loaded = run([exe, '--api-config-and-exit' if folder == croot else '--api-config', *root_args])
                    assert ('model=' + model + '\n') in loaded.stdout.decode('utf-8').replace('\r\n', '\n'), 'launcher changed model'
                    if initial is not None:
                        text = result_bytes.decode('utf-16' if result_bytes.startswith(b'\xff\xfe') else 'mbcs')
                        for keep in ('; keep comment', 'timeout_ms=22000', 'concurrency=2', '[other]', 'untouched=value'):
                            assert keep in text, 'lost ' + keep
                    before = ini.read_bytes()
                    for builder in builders:
                        request = run([builder, ini])
                        assert request.returncode == 0, (builder.name, request.stderr)
                        for line in request.stdout.decode('utf-8').splitlines()[1:]:
                            assert json.loads(line)['model'] == model, (builder.name, 'wrong model')
                        assert ini.read_bytes() == before, 'server load rewrote config'
                assert not list(cfgdir.glob('*.tmp')), 'temporary config leaked'
                outputs.append(result_bytes)
            assert outputs[0] == outputs[1], 'C/C# config bytes differ'
            passed += 1
            print('[PASS]', name)
        except (AssertionError, UnicodeError) as ex:
            failed += 1
            print('[FAIL]', name, str(ex))
    print('Model encoding:', passed, 'passed,', failed, 'failed')
sys.exit(1 if failed else 0)
