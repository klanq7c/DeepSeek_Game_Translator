"""使用本地提供商、真实 HTTP 和生产实现测试模型发现。"""
import argparse
import http.server
import json
import subprocess
import threading
from urllib.parse import parse_qs, urlsplit

parser = argparse.ArgumentParser()
parser.add_argument('--c', required=True)
parser.add_argument('--cs', required=True)
args = parser.parse_args()
requests = []
response = (200, b'{"data":[]}')
pages = None
page_requests = []
framing = 'length'

class Provider(http.server.BaseHTTPRequestHandler):
    def log_message(self, *args):
        pass

    def do_GET(self):
        requests.append((self.path, self.headers.get('Authorization')))
        page_requests.append((self.path, self.headers.get('x-api-key'), self.headers.get('anthropic-version')))
        status, body = pages.pop(0) if pages else response
        self.send_response(status)
        if status == 302:
            self.send_header('Location', '/stolen-key')
        if framing == 'chunked':
            self.send_header('Transfer-Encoding', 'chunked')
            body = ('%x\r\n' % len(body)).encode() + body + b'\r\n0\r\n\r\n'
        elif framing != 'close':
            self.send_header('Content-Length', str(len(body) + (20 if framing == 'short' else 0)))
        self.send_header('Connection', 'close')
        self.end_headers()
        try:
            self.wfile.write(body)
        except (BrokenPipeError, ConnectionResetError, ConnectionAbortedError) as exc:
            print('provider: bounded client closed:', type(exc).__name__)

server = http.server.ThreadingHTTPServer(('127.0.0.1', 0), Provider)
thread = threading.Thread(target=server.serve_forever, daemon=True)
thread.start()
base = 'http://127.0.0.1:' + str(server.server_port)
checks = 0

def run(exe, *argv):
    return subprocess.run([exe, *argv], capture_output=True, encoding='utf-8', timeout=25, check=True).stdout.splitlines()

def check_case(name, data, expected, status=200, key='fake-secret', prefix='/v1', frame='length'):
    global response, checks, framing
    framing = frame
    response = (status, data if isinstance(data, bytes) else json.dumps(data, ensure_ascii=True).encode())
    before = len(requests)
    outputs = [run(exe, 'fetch', base + prefix + '/chat/completions', key) for exe in (args.c, args.cs)]
    for output in outputs:
        assert output[0] == 'count=' + str(len(expected)), (name, output)
        assert output[2:] == expected, (name, output)
        assert 'fake-secret' not in '\n'.join(output), name
    assert outputs[0] == outputs[1], (name, outputs)
    assert requests[before:] == [(prefix + '/models', 'Bearer fake-secret' if key else None)] * 2, (name, requests[before:])
    checks += 1
    print('[PASS]', name)


def check_pages(name, kind, responses, expected, cursors=None):
    global pages, checks, framing
    framing = 'length'
    outputs = []
    for exe in (args.c, args.cs):
        pages = [(status, body if isinstance(body, bytes) else json.dumps(body).encode()) for status, body in responses]
        before = len(requests)
        query = '?limit=1000' if kind == 1 else '?page_size=100'
        output = run(exe, 'catalog', base + '/models' + query, 'fake-secret', str(kind))
        assert output[0] == 'count=' + str(len(expected)) and output[2:] == expected, (name, output)
        assert not pages, (name, 'not all expected pages requested')
        assert 'fake-secret' not in '\n'.join(output), (name, output)
        outputs.append(output)
        for index, (path, auth) in enumerate(requests[before:]):
            assert urlsplit(path).path == '/models', (name, path)
            args_query = parse_qs(urlsplit(path).query)
            if kind == 1:
                assert auth is None and page_requests[before + index][1:] == ('fake-secret', '2023-06-01')
                assert args_query == ({'limit': ['1000']} if index == 0 else {'limit': ['1000'], 'after_id': [cursors[index - 1]]}), (name, path)
            else:
                assert auth == 'Bearer fake-secret' and page_requests[before + index][1:] == (None, None)
                assert args_query == {'page_size': ['100'], 'page_no': [str(index + 1)]}, (name, path)
    assert outputs[0] == outputs[1], (name, outputs)
    pages = None
    checks += 1
    print('[PASS]', name)


def claude(ids, more=False, cursor=None):
    return {'data': [{'id': id} for id in ids], 'has_more': more, 'last_id': cursor}


def dashscope(ids, page, total):
    return {'success': True, 'output': {'models': [{'model': id} for id in ids], 'page_no': page, 'total': total}}

try:
    urls = [
        ('https://api.deepseek.com/v1/chat/completions', 'https://api.deepseek.com/v1/models'),
        ('https://api.deepseek.com/chat/completions', 'https://api.deepseek.com/models'),
        ('https://host/compatible-mode/v1/chat/completions/', 'https://host/compatible-mode/v1/models'),
        ('https://host/v1beta/openai/chat/completions', 'https://host/v1beta/openai/models'),
        ('https://host/api/paas/v4/chat/completions', 'https://host/api/paas/v4/models'),
        ('https://host/custom/chat', 'INVALID'),
        ('https://host/v1/chat/completions?key=secret', 'INVALID'),
        ('https://api.anthropic.com/v1/chat/completions', 'https://api.anthropic.com/v1/models?limit=1000'),
        ('https://api.siliconflow.cn/v1/chat/completions', 'https://api.siliconflow.cn/v1/models?sub_type=chat'),
        ('https://api.siliconflow.com/v1/chat/completions', 'https://api.siliconflow.com/v1/models?sub_type=chat'),
        ('https://api.moonshot.cn/v1/chat/completions', 'https://api.moonshot.cn/v1/models'),
        ('https://open.bigmodel.cn/api/paas/v4/chat/completions', 'https://open.bigmodel.cn/api/paas/v4/models'),
        ('https://api.openai.com/v1/chat/completions', 'https://api.openai.com/v1/models'),
        ('https://generativelanguage.googleapis.com/v1beta/openai/chat/completions', 'https://generativelanguage.googleapis.com/v1beta/openai/models'),
        ('https://dashscope.aliyuncs.com/compatible-mode/v1/chat/completions///', 'https://dashscope.aliyuncs.com/api/v1/models?capabilities=TG&capabilities=Reasoning&page_size=100'),
    ]
    for host in ('dashscope.aliyuncs.com', 'dashscope-intl.aliyuncs.com', 'cn-hongkong.dashscope.aliyuncs.com', 'workspace.cn-beijing.maas.aliyuncs.com', 'workspace.ap-northeast-1.maas.aliyuncs.com', 'workspace.eu-central-1.maas.aliyuncs.com', 'workspace.us-east-1.maas.aliyuncs.com'):
        urls.append(('https://' + host + '/compatible-mode/v1/chat/completions',
                     'https://' + host + '/api/v1/models?capabilities=TG&capabilities=Reasoning&page_size=100'))
    for host in ('api.anthropic.com.evil.invalid', 'api.siliconflow.cn.evil.invalid', 'dashscope.aliyuncs.com.evil.invalid', 'cn-beijing.maas.aliyuncs.com', 'nested.workspace.cn-beijing.maas.aliyuncs.com'):
        urls.append(('https://' + host + '/compatible-mode/v1/chat/completions', 'https://' + host + '/compatible-mode/v1/models'))
    for endpoint, expected in urls:
        for exe in (args.c, args.cs):
            assert run(exe, 'url', endpoint) == [expected]
        checks += 1
    check_case('latest ids, dedup, nested id ignored', {'data': [{'id': 'new-model-2099', 'nested': {'id': 'wrong'}}, {'id': 'org/model:latest'}, {'id': 'new-model-2099'}]}, ['new-model-2099', 'org/model:latest'])
    check_case('refresh replaces outdated catalog', {'data': [{'id': 'newer-release'}]}, ['newer-release'])
    check_case('DeepSeek V4.1 Flash uses stable API id', {'data': [{'id': 'deepseek-flash'}, {'id': 'deepseek-v4-pro'}]}, ['deepseek-flash', 'deepseek-v4-pro'])
    check_case('local keyless and custom prefix', {'data': [{'id': 'local:model'}]}, ['local:model'], key='', prefix='/proxy/compatible-mode/v1')
    check_case('unicode model ids', {'data': [{'id': '模型/最新'}]}, ['模型/最新'])
    check_case('empty list', {'data': []}, [])
    check_case('nested data rejected', {'error': {'data': [{'id': 'bad'}]}}, [])
    check_case('invalid item rejects partial list', {'data': [{'id': 'valid'}, {'id': 5}]}, [])
    check_case('incomplete pagination rejected', {'data': [{'id': 'first'}], 'has_more': True}, [])
    check_case('control character rejected', {'data': [{'id': 'bad\nmodel'}]}, [])
    check_case('too long id rejected', {'data': [{'id': 'x' * 256}]}, [])
    check_case('UTF8 storage bound', {'data': [{'id': '模' * 86}]}, [])
    check_case('model count bounded', {'data': [{'id': 'model-' + str(i)} for i in range(4097)]}, [])
    check_case('pagination flag cannot be null', {'data': [{'id': 'one'}], 'has_more': None}, [])
    check_case('short Content-Length rejects otherwise valid JSON', {'data': [{'id': 'model'}]}, [], frame='short')
    check_case('close-delimited response remains supported', {'data': [{'id': 'model'}]}, ['model'], frame='close')
    check_case('chunked response remains supported', {'data': [{'id': 'model'}]}, ['model'], frame='chunked')
    for field, body in (
        ('has_more', b'{"data":[{"id":"one"}],"has_more":true,"has_more":true}'),
        ('data', b'{"data":[],"data":[{"id":"one"}]}'),
        ('id', b'{"data":[{"id":"one","id":"two"}]}'),
    ):
        check_case('duplicate ' + field + ' rejected', body, [])
    for number in ('01', '+1', '.1', '1.', '1e', '--1'):
        check_case('invalid numeric metadata ' + number, ('{"data":[{"id":"one"}],"created":' + number + '}').encode(), [])
    for index, body in enumerate([b'{"data":[{"id":"x"}', b'{"data":[]} trailing', b'{"data":[{"id":"\\u', b'{"data":[{"id":"x\x00z"}]}', b'\xff', b'{"data":[{"id":"\\q"}]}']):
        check_case('malformed JSON ' + str(index), body, [])
    for status in (401, 403, 404, 429, 500, 302):
        check_case('HTTP ' + str(status), {'data': [{'id': 'not-success'}]}, [], status)
    check_case('response size bound', b' ' * (2 * 1024 * 1024 + 1), [])
    check_pages('Claude paginates, preserves API order and deduplicates', 1,
                [(200, claude(['new', 'cursor'], True, 'cursor')), (200, claude(['cursor', 'old']))], ['new', 'cursor', 'old'], ['cursor'])
    check_pages('Claude cursor escaped on same origin', 1,
                [(200, claude(['one'], True, 'm/模?&=#%')), (200, claude(['two']))], ['one', 'two'], ['m/模?&=#%'])
    check_pages('Claude missing cursor rejects partial results', 1, [(200, claude(['one'], True))], [])
    check_pages('Claude duplicate pagination cannot hide later page', 1,
                [(200, b'{"data":[{"id":"one"}],"has_more":true,"has_more":true,"last_id":"one"}')], [])
    check_pages('Claude repeated cursor rejected', 1,
                [(200, claude(['one'], True, 'same')), (200, claude(['two'], True, 'same'))], [], ['same'])
    check_pages('Claude empty intermediate page rejected', 1,
                [(200, claude(['one'], True, 'one')), (200, claude([], True, 'two'))], [], ['one'])
    check_pages('Claude later HTTP error discards earlier page', 1,
                [(200, claude(['one'], True, 'one')), (401, {'error': 'denied'})], [], ['one'])
    check_pages('Claude later malformed JSON discards earlier page', 1,
                [(200, claude(['one'], True, 'one')), (200, b'{')], [], ['one'])
    check_pages('Claude redirects never followed', 1,
                [(200, claude(['one'], True, 'one')), (302, {})], [], ['one'])
    check_pages('Claude page count bounded', 1,
                [(200, claude(['model-' + str(i)], True, 'cursor-' + str(i))) for i in range(64)], [], ['cursor-' + str(i) for i in range(63)])
    check_pages('Claude model count bounded across pages', 1,
                [(200, claude(['model-' + str(i) for i in range(4096)], True, 'end')), (200, claude(['overflow']))], [], ['end'])
    check_pages('DashScope new IDs from output models across pages', 2,
                [(200, dashscope(['new-qwen', 'other'], 1, 4)), (200, dashscope(['other', 'new-partner'], 2, 4))], ['new-qwen', 'other', 'new-partner'])
    check_pages('DashScope wrong page rejected', 2, [(200, dashscope(['one'], 2, 1))], [])
    check_pages('DashScope total changing rejects partial catalog', 2,
                [(200, dashscope(['one'], 1, 2)), (200, dashscope(['two'], 2, 3))], [])
    check_pages('DashScope duplicate-only page rejected', 2,
                [(200, dashscope(['one'], 1, 3)), (200, dashscope(['one'], 2, 3))], [])
    check_pages('DashScope empty intermediate page rejected', 2,
                [(200, dashscope(['one'], 1, 2)), (200, dashscope([], 2, 2))], [])
    check_pages('DashScope total overflow rejected', 2, [(200, dashscope(['one'], 1, 5000))], [])
    check_pages('DashScope missing metadata rejected', 2, [(200, {'output': {'models': [{'model': 'one'}]}})], [])
    check_pages('DashScope reported failure rejects model payload', 2,
                [(200, dict(dashscope(['one'], 1, 1), success=False))], [])
    check_pages('DashScope null success rejected', 2,
                [(200, dict(dashscope(['one'], 1, 1), success=None))], [])
    check_pages('DashScope duplicate success cannot hide failure', 2,
                [(200, b'{"success":false,"success":true,"output":{"models":[{"model":"one"}],"page_no":1,"total":1}}')], [])
    check_pages('DashScope later HTTP error discards catalog', 2,
                [(200, dashscope(['one'], 1, 2)), (429, {})], [])
    before = len(requests)
    for exe in (args.c, args.cs):
        assert run(exe, 'fetch', 'http://example.com/v1/chat/completions', 'fake-secret')[0] == 'count=0'
        assert run(exe, 'fetch', base + '/v1/chat/completions', 'bad\r\nkey')[0] == 'count=0'
        assert run(exe, 'fetch', base + '/v1/chat/completions', 'x' * 1024)[0] == 'count=0'
    assert len(requests) == before
    checks += 3
    print('[PASS] API models:', checks, 'scenarios, C/C# parity, no config/cache writes')
finally:
    server.shutdown()
    server.server_close()
    thread.join()
