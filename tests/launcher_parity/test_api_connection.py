"""本机假提供商验证实际 POST；不读真实 Key，不连接外网，不启动翻译服务。"""
import argparse
import concurrent.futures
import http.server
import json
import subprocess
import threading
import time

parser = argparse.ArgumentParser()
parser.add_argument('--c', required=True)
parser.add_argument('--cs', required=True)
parser.add_argument('--cold-start-only', action='store_true')
args = parser.parse_args()
requests = []
response = (200, b'')
framing = 'length'


class Provider(http.server.BaseHTTPRequestHandler):
    def log_message(self, *_):
        return  # 测试夹具按断言输出，不记录伪造的认证头。

    def do_POST(self):
        payload = self.rfile.read(int(self.headers['Content-Length']))
        requests.append((self.path, self.headers.get('Authorization'), json.loads(payload)))
        status, body = response
        frame = framing
        if frame == 'cold-start':
            time.sleep(18)
        if frame == 'timeout':
            time.sleep(125)
        self.send_response(status)
        if 300 <= status < 400:
            self.send_header('Location', '/unexpected-redirect')
        if frame == 'chunked':
            self.send_header('Transfer-Encoding', 'chunked')
            body = ('%x\r\n' % len(body)).encode() + body + b'\r\n0\r\n\r\n'
        elif frame != 'close':
            self.send_header('Content-Length', str(len(body) + (20 if frame == 'short' else 0)))
        self.send_header('Connection', 'close')
        try:
            self.end_headers()
            if frame == 'body-timeout':
                self.wfile.flush()
                time.sleep(19)
            self.wfile.write(body)
        except (BrokenPipeError, ConnectionResetError, ConnectionAbortedError) as exc:
            print('provider bounded client closed:', type(exc).__name__)


server = http.server.ThreadingHTTPServer(('127.0.0.1', 0), Provider)
threading.Thread(target=server.serve_forever, daemon=True).start()
base = 'http://127.0.0.1:' + str(server.server_port)
good = {'choices': [{'message': {'role': 'assistant', 'content': 'OK'}, 'finish_reason': 'stop'}]}
checks = 0


def run(exe, endpoint, model, key):
    return subprocess.run([exe, endpoint, model, key], capture_output=True, encoding='utf-8', check=True, timeout=150).stdout.splitlines()


def case(name, body=good, success=False, status=200, frame='length', model='fixture-model', key='fake-secret', endpoint=None, sends=True, message=None):
    global response, framing, checks
    response = (status, body if isinstance(body, bytes) else json.dumps(body, ensure_ascii=True).encode())
    framing = frame
    before = len(requests)
    with concurrent.futures.ThreadPoolExecutor(max_workers=2) as pool:
        outputs = list(pool.map(lambda exe: run(exe, endpoint or base + '/v1/chat/completions?fixture=1', model, key), (args.c, args.cs)))
    for output in outputs:
        assert output[0] == 'success=' + str(int(success)), (name, output)
        assert 'fake-secret' not in '\n'.join(output) and 'private-body' not in '\n'.join(output), (name, output)
        if message:
            assert message in output[1], (name, output)
    if frame not in ('short', 'timeout', 'body-timeout'):
        assert outputs[0] == outputs[1], (name, outputs)
    assert len(requests) - before == (2 if sends else 0), (name, requests[before:])
    for path, auth, payload in requests[before:]:
        assert path == '/v1/chat/completions?fixture=1', (name, path)
        assert auth == ('Bearer ' + key if key else None), name
        assert payload == {'model': model, 'messages': [{'role': 'user', 'content': 'Reply with OK.'}],
                           'temperature': 0, 'max_tokens': 128, 'stream': False}, (name, payload)
    checks += 1
    print('ok', name, flush=True)


try:
    if args.cold_start_only:
        case('local-cold-start', success=True, frame='cold-start')
        raise SystemExit(0)
    for endpoint, expected in [
        ('http://127.0.0.1:11434/v1/chat/completions', 120000),
        ('http://localhost:1234/v1/chat/completions', 120000),
        ('https://LOCALHOST:1234/v1/chat/completions', 120000),
        ('http://[::1]:11434/v1/chat/completions', 120000),
        ('https://provider.invalid/v1/chat/completions', 15000),
        ('https://localhost.evil.invalid/v1/chat/completions', 15000),
        ('https://127.0.0.1.evil.invalid/v1/chat/completions', 15000),
        ('https://localhost@provider.invalid/v1/chat/completions', 15000),
        ('https://user@localhost/v1/chat/completions', 15000),
        ('http://127.1:11434/v1/chat/completions', 15000),
        ('http://2130706433:11434/v1/chat/completions', 15000),
        ('http://0x7f000001:11434/v1/chat/completions', 15000),
        ('http://0177.0.0.1:11434/v1/chat/completions', 15000),
        ('invalid', 15000),
    ]:
        for exe in (args.c, args.cs):
            result = subprocess.run([exe, 'wait', endpoint], capture_output=True, encoding='utf-8', check=True, timeout=5)
            assert result.stdout.strip() == str(expected), (endpoint, result.stdout)
        checks += 1
    print('ok local/remote wait policy: 14 cases', flush=True)
    case('success', success=True)
    case('chunked', success=True, frame='chunked')
    case('close-delimited', success=True, frame='close')
    case('local-no-key', success=True, key='')
    case('model-exact-and-escaped', success=True, model='自选"\\\n模型')
    case('legacy-id-not-upgraded', success=True, model='deepseek-chat')
    case('unused-surrogate-metadata-remains-ignored', {**good, '\ud800': '\ud800'}, success=True)
    case('unused-finite-number-remains-supported', {**good, 'unused': 1e308}, success=True)
    for status, message in [(400, '请求被拒绝'), (401, '认证失败'), (403, '访问被拒绝'), (404, '接口或模型不存在'),
                            (407, '代理需要认证'), (429, '请求受限'), (500, '提供商暂时不可用'), (503, '提供商暂时不可用'),
                            (302, '重定向'), (307, '重定向'), (204, '请求被拒绝')]:
        case('http-' + str(status), {'error': 'private-body fake-secret'}, status=status, message=message)
    invalid = [b'', b'<html>OK</html>', b'{"choices":', b'{} trailing', b'\xff',
               {'data': [{'id': 'model'}]}, {'error': None, **good}, {'choices': []},
               {'choices': [{'message': {'content': None, 'reasoning_content': 'OK'}}]},
               {'choices': [{'message': {'content': ' \t\n\u3000'}}]},
               {'choices': [{'message': {'content': ['OK']}}]}, {'choices': [{'content': 'OK'}]},
               {'nested': good}, {'choices': [{'message': {'content': '\0OK'}}]},
               b'{"choices":[{"message":{"content":"OK","content":""}}]}',
               b'{"choices":[{"message":{"content":"OK"}}],"unused":1,"unused":2}',
               b'{"choices":[{"message":{"content":"OK"}}],"unused":{"x":1,"\\u0078":2}}',
               b'{"choices":[{"message":{"content":"OK"}}],"unused":{"\\u0000":1,"\\u0000":2}}',
               b'{"choices":[{"message":{"content":"OK"}}],"\\ud800":1,"\\ud800":2}',
               ('{"choices":[{"message":{"content":"OK"}}],"' + 'k' * 80 + '":1,"' + 'k' * 80 + '":2}').encode(),
               b'{"choices":[{"message":{"content":"OK"}}],"unused":1e999}',
               b'{"choices":[{"message":{"content":"\\ud800"}}]}',
               json.dumps(good).encode() + b'\0']
    for i, body in enumerate(invalid):
        case('invalid-reply-' + str(i), body, message='未获得有效模型回复')
    case('short-response', frame='short')
    case('oversized', b' ' * 65536, message='检测响应过大')
    case('empty-model', model='', sends=False, message='请填写模型 ID')
    case('whitespace-model', model='\u3000 \t', sends=False, message='请填写模型 ID')
    case('oversized-model', model='中' * 86, sends=False, message='请填写模型 ID')
    case('invalid-key', key='fake-secret\r\nInjected: yes', sends=False, message='API Key 含无效字符')
    case('plaintext-remote', endpoint='http://example.invalid/v1/chat/completions', sends=False, message='远程服务需要 HTTPS')
    for host in ('127.1', '2130706433', '0x7f000001', '0177.0.0.1'):
        case('noncanonical-loopback-' + host, endpoint='http://' + host + ':' + str(server.server_port) + '/v1/chat/completions?fixture=1', sends=False, message='远程服务需要 HTTPS')
    case('url-userinfo', endpoint=base.replace('://', '://user:password@') + '/v1/chat/completions', sends=False, message='有效的完整 API 地址')
    case('url-fragment', endpoint=base + '/v1/chat/completions#fragment', sends=False, message='有效的完整 API 地址')
    case('local-cold-start', success=True, frame='cold-start')
    case('body-timeout-still-bounded', frame='body-timeout')
    case('timeout', frame='timeout', message='检测超时')
    print('API connectivity: %d passed (C/C# actual HTTP)' % checks)
finally:
    server.shutdown()
    server.server_close()
