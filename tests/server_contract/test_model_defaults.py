"""验证生产请求构造器和配置读取的 C/C# 一致性；不启动服务或发网络请求。"""
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
ollama = 'http://127.0.0.1:11434/v1/chat/completions'
cases = []


def case(name, endpoint=official, model='user-model', expected=None,
         thinking=False, reasoning=False, local=False, settings=None, limits=None,
         timeout=None, concurrency=8):
    cases.append(dict(name=name, endpoint=endpoint, model=model,
                      expected=expected if expected is not None else model,
                      thinking=thinking, reasoning=reasoning,
                      settings=settings or {},
                      limits=limits or ((8, 2048) if local else (48, 9600)),
                      timeout=timeout if timeout is not None else (180000 if local else 15000),
                      concurrency=concurrency))


case('missing model', model=None, expected='deepseek-flash', thinking=True)
case('V4.1 Flash current id', model='deepseek-flash', thinking=True)
case('retired chat alias', model='deepseek-chat', expected='deepseek-flash', thinking=True)
case('explicit legacy flash preserved', model='deepseek-v4-flash', thinking=True)
case('explicit V4 Pro preserved', model='deepseek-v4-pro', thinking=True)
case('custom model preserved')
case('custom endpoint owns flash id', 'https://gateway.example/v1/chat/completions', 'deepseek-flash')
case('custom endpoint owns chat id', 'https://gateway.example/v1/chat/completions', 'deepseek-chat')
case('host suffix is not official', 'https://api.deepseek.com.example/v1/chat/completions', 'deepseek-flash')
case('local provider owns flash id', ollama, 'deepseek-flash', local=True, reasoning=True)

# 专用选项由端点识别，不能依赖模型别名；本地自定义名和 Unicode 原样保留。
for name, endpoint in (
    ('Ollama IPv4', ollama),
    ('Ollama localhost', 'http://localhost:11434/v1/chat/completions'),
    ('Ollama case insensitive host', 'http://LOCALHOST:11434/v1/chat/completions'),
    ('Ollama IPv6', 'http://[::1]:11434/v1/chat/completions'),
    ('Ollama HTTPS', 'https://localhost:11434/v1/chat/completions'),
    ('Ollama terminal slash', ollama + '/'),
    ('Ollama query', ollama + '?test=1'),
    ('Ollama fragment', ollama + '#test'),
):
    case(name, endpoint, 'index-translate:2b', local=True, reasoning=True)
case('Ollama user model preserved', ollama, 'my-custom-model:latest', local=True, reasoning=True)
case('Ollama Unicode model preserved', ollama, '模型/翻译-😀', local=True, reasoning=True)
case('Ollama legacy alias remains user owned', ollama, 'deepseek-chat', local=True, reasoning=True)

# 错误主机、端口或路径均不能注入 Ollama 参数；LM Studio 保持通用请求协议。
for name, endpoint, local in (
    ('LM Studio localhost', 'http://localhost:1234/v1/chat/completions', True),
    ('LM Studio IPv6', 'http://[::1]:1234/v1/chat/completions', True),
    ('local custom default port', 'http://127.0.0.1/v1/chat/completions', True),
    ('Ollama host suffix', 'https://localhost.example:11434/v1/chat/completions', False),
    ('Ollama IPv4 host suffix', 'https://127.0.0.1.example:11434/v1/chat/completions', False),
    ('remote port 11434 is generic', 'https://gateway.example:11434/v1/chat/completions', False),
    ('Ollama port prefix lookalike', 'https://localhost:114340/v1/chat/completions', True),
    ('Ollama different port', 'https://localhost:11435/v1/chat/completions', True),
    ('Ollama path suffix lookalike', ollama + '-other', True),
    ('Ollama path child lookalike', ollama + '/other', True),
    ('Ollama path prefix lookalike', 'http://localhost:11434/proxy/v1/chat/completions', True),
    ('Ollama route case matters', 'http://localhost:11434/v1/Chat/Completions', True),
    ('Ollama non OpenAI route', 'http://localhost:11434/api/chat', True),
    ('Ollama missing v1 route', 'http://localhost:11434/chat/completions', True),
):
    case(name, endpoint, 'index-translate:2b', local=local)

for name, settings, limits in (
    ('explicit small batch', {'batch_max_items': 3, 'batch_char_budget': 1024}, (3, 1024)),
    ('minimum batch boundaries', {'batch_max_items': 1, 'batch_char_budget': 256}, (1, 256)),
    ('maximum batch boundaries', {'batch_max_items': 48, 'batch_char_budget': 9600}, (48, 9600)),
    ('zero batch values clamped', {'batch_max_items': 0, 'batch_char_budget': 0}, (1, 256)),
    ('negative batch values clamped', {'batch_max_items': -1, 'batch_char_budget': -1}, (1, 256)),
    ('oversized batch values clamped', {'batch_max_items': 49, 'batch_char_budget': 9601}, (48, 9600)),
):
    for local in (False, True):
        case(('local ' if local else 'remote ') + name,
             ollama if local else official, 'index-translate:2b',
             local=local, reasoning=local, settings=settings, limits=limits)
case('local explicit resource settings', ollama, 'index-translate:2b', local=True, reasoning=True,
     settings={'concurrency': 1, 'timeout_ms': 22000}, timeout=22000, concurrency=1)
case('local timeout lower bound', ollama, local=True, reasoning=True,
     settings={'timeout_ms': 1}, timeout=3000)
case('local timeout upper bound', ollama, local=True, reasoning=True,
     settings={'timeout_ms': 300001}, timeout=300000)
case('remote timeout upper bound retained', settings={'timeout_ms': 300001}, timeout=60000)

failed = 0
for index, item in enumerate(cases):
    ini = root / ('model-' + str(index) + '.ini')
    content = '[api]\nendpoint=' + item['endpoint'] + '\nkey=fake-test-key\n'
    if item['model'] is not None:
        content += 'model=' + item['model'] + '\n'
    content += ''.join(key + '=' + str(value) + '\n' for key, value in item['settings'].items())
    ini.write_text(content, encoding='utf-16' if not content.isascii() else 'ascii')
    before = ini.read_bytes()
    configs = []
    for exe in (a.c, a.cs):
        try:
            result = subprocess.run([exe, str(ini)], capture_output=True, text=True,
                                    encoding='utf-8', check=True, timeout=20)
            lines = result.stdout.splitlines()
            assert lines[0] == 'deepseek-flash', ('default', lines[0])
            assert len(lines) == 4, ('expected single, batch and contextual requests', lines)
            config_lines = [line[7:] for line in result.stderr.splitlines() if line.startswith('CONFIG ')]
            assert len(config_lines) == 1, ('config metadata', result.stderr)
            config = json.loads(config_lines[0])
            assert (config['batch_max_items'], config['batch_char_budget']) == item['limits'], config
            assert config['timeout_ms'] == item['timeout'], config
            assert config['concurrency'] == item['concurrency'], config
            configs.append(config)
            for mode, line in enumerate(lines[1:]):
                body = json.loads(line)
                assert body['model'] == item['expected'], body
                assert body.get('thinking') == ({'type': 'disabled'} if item['thinking'] else None), body
                assert ('thinking' in body) == item['thinking'], body
                assert body.get('reasoning_effort') == ('none' if item['reasoning'] else None), body
                assert ('reasoning_effort' in body) == item['reasoning'], body
                assert body['temperature'] == 0, body
                assert body['max_tokens'] >= 512, body
                assert [message['role'] for message in body['messages']] == ['system', 'user'], body
                user = body['messages'][1]['content']
                assert 'Hello {name}.' in user, user
                if mode == 0:
                    assert '\\C[1]Open menu.' not in user, user
                else:
                    array_text = user if mode == 1 else user.split('Texts to translate:\n', 1)[1]
                    assert json.loads(array_text) == ['Hello {name}.', '\\C[1]Open menu.'], user
                    if mode == 2:
                        assert 'Previous line.' in user, user
                assert 'fake-test-key' not in line, body
            assert ini.read_bytes() == before, 'user config rewritten'
            assert 'fake-test-key' not in result.stderr
            print('[PASS]', pathlib.Path(exe).name, item['name'])
        except (AssertionError, IndexError, KeyError, ValueError,
                subprocess.CalledProcessError, subprocess.TimeoutExpired) as exc:
            failed += 1
            print('[FAIL]', pathlib.Path(exe).name, item['name'], exc)
    if len(configs) == 2 and configs[0] != configs[1]:
        failed += 1
        print('[FAIL] C/C# config parity', item['name'], configs)
print('Model defaults:', len(cases) * 2 - failed, 'passed,', failed, 'failed')
raise SystemExit(1 if failed else 0)
