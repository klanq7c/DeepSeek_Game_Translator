import argparse
import json
import pathlib
import subprocess

p = argparse.ArgumentParser()
p.add_argument('--c', required=True)
p.add_argument('--cs', required=True)
p.add_argument('--root', required=True)
a = p.parse_args()
root = pathlib.Path(a.root)
official = 'https://api.deepseek.com/v1/chat/completions'
cases = [
    ('missing model', official, None, 'deepseek-flash', True),
    ('V4.1 Flash current id', official, 'deepseek-flash', 'deepseek-flash', True),
    ('retired chat alias', official, 'deepseek-chat', 'deepseek-flash', True),
    ('explicit legacy flash preserved', official, 'deepseek-v4-flash', 'deepseek-v4-flash', True),
    ('explicit V4 Pro preserved', official, 'deepseek-v4-pro', 'deepseek-v4-pro', True),
    ('custom model preserved', official, 'user-model', 'user-model', False),
    ('custom endpoint owns flash id', 'https://gateway.example/v1/chat/completions', 'deepseek-flash', 'deepseek-flash', False),
    ('custom endpoint owns chat id', 'https://gateway.example/v1/chat/completions', 'deepseek-chat', 'deepseek-chat', False),
    ('host suffix is not official', 'https://api.deepseek.com.example/v1/chat/completions', 'deepseek-flash', 'deepseek-flash', False),
    ('local provider owns flash id', 'http://127.0.0.1:11434/v1/chat/completions', 'deepseek-flash', 'deepseek-flash', False),
]
failed = 0
for index, (name, endpoint, model, expected, thinking) in enumerate(cases):
    ini = root / ('model-' + str(index) + '.ini')
    content = '[api]\nendpoint=' + endpoint + '\nkey=fake-test-key\n' + (('model=' + model + '\n') if model else '')
    ini.write_text(content, encoding='ascii')
    before = ini.read_bytes()
    for exe in (a.c, a.cs):
        result = subprocess.run([exe, str(ini)], capture_output=True, text=True, encoding='utf-8', check=True)
        lines = result.stdout.splitlines()
        try:
            assert lines[0] == 'deepseek-flash', ('default', lines[0])
            for line in lines[1:]:
                body = json.loads(line)
                assert body['model'] == expected, body
                assert body.get('thinking') == ({'type': 'disabled'} if thinking else None), body
            assert ini.read_bytes() == before, 'user config rewritten'
            assert 'fake-test-key' not in result.stderr
            print('[PASS]', pathlib.Path(exe).name, name)
        except AssertionError as exc:
            failed += 1
            print('[FAIL]', pathlib.Path(exe).name, name, exc)
print('Model defaults:', len(cases) * 2 - failed, 'passed,', failed, 'failed')
raise SystemExit(1 if failed else 0)
