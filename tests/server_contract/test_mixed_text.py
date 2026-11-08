"""真实 C/C# 服务：半汉化补译、中文保护及坏结果不得入缓存。"""
from contextlib import contextmanager
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
from pathlib import Path
import re
import tempfile
import threading
import time

from test_api_boundaries import ROOT, server
from test_provider_body_timeout import request


@contextmanager
def provider():
    state = {'texts': [], 'contexts': 0, 'errors': [], 'format_failures': 0}
    release, arrived = threading.Event(), threading.Event()

    def translate(text):
        state['texts'].append(text)
        if re.search('[\u4e00-\u9fff]', text):
            state['errors'].append('Chinese was sent as mutable provider input: ' + text)
        if 'Wait for response' in text:
            arrived.set()
            if not release.wait(5):
                state['errors'].append('queued request did not release provider')
        result = text.replace('Open the door', '打开门').replace('Continue', '继续').replace('Hello', '你好').replace('SAVE', '保存').replace('Wait for response', '等待响应')
        tokens = re.findall(r'__DST_HAN\d+_\d+__', result)
        if 'DROP' in text and tokens:
            result = result.replace(tokens[0], '')
        if 'DUP' in text and tokens:
            result += tokens[0]
        if 'REORDER' in text and len(tokens) >= 2:
            result = result.replace(tokens[0], '\x01').replace(tokens[1], tokens[0]).replace('\x01', tokens[1])
        return result

    class Handler(BaseHTTPRequestHandler):
        def log_message(self, *_):
            return

        def do_POST(self):
            payload = json.loads(self.rfile.read(int(self.headers['Content-Length'])))
            user = payload['messages'][-1]['content']
            if 'FORMAT_RETRY' in user and state['format_failures'] < 1:
                state['format_failures'] += 1
                self.send_response(503)
                self.send_header('Content-Length', '0')
                self.end_headers()
                return
            if '\nTexts to translate:\n' in user:
                state['contexts'] += 1
                user = user.split('\nTexts to translate:\n', 1)[1]
            if user.startswith('['):
                answer = json.dumps([translate(text) for text in json.loads(user)], ensure_ascii=False)
            else:
                answer = translate(user.split('\n', 1)[1])
            body = json.dumps({'choices': [{'message': {'content': answer}}]}, ensure_ascii=False).encode('utf-8')
            self.send_response(200)
            self.send_header('Content-Length', str(len(body)))
            self.end_headers()
            self.wfile.write(body)

    http = ThreadingHTTPServer(('127.0.0.1', 0), Handler)
    thread = threading.Thread(target=http.serve_forever, daemon=True)
    thread.start()
    try:
        yield f'http://127.0.0.1:{http.server_port}/v1/chat/completions', state, arrived, release
    finally:
        release.set()
        http.shutdown()
        http.server_close()
        thread.join(timeout=3)


def call(port, path, body):
    code, response = request(port, path, body)
    assert code == 200, (code, response)
    return json.loads(response)


def check(binary, directory):
    with provider() as (endpoint, state, arrived, release), server(binary, directory, endpoint) as (port, process):
        for source in ('已翻译完成。', '生命 HP 100 FPS', '<color=red>已完成</color> [player]', '按 [Esc] 退出',
                       '<link="quest>original">中文</link>', '文件 git+ssh://host/path',
                       '角色 {actor.name} 已完成', '效果 \\C[red]中文\\C[0]',
                       '已完成 __DS_TOKEN_0__', '你好 %(player)s',
                       '网址 https://example.com/Continue', '已完成 __DS_TOKEN_0____DS_TOKEN_1__'):
            result = call(port, '/translate', {'text': source, 'cache_only': False})
            assert result['source'] == 'pass' and result['translation'] == source, result
        assert not state['texts'], 'already localized text called the provider'

        source = '请 <color=red>Open the door</color>，玩家 [player] %d。'
        expected = source.replace('Open the door', '打开门')
        result = call(port, '/translate', {'text': source, 'cache_only': False})
        assert result['translation'] == expected and result['source'] == 'api_batch', result
        before = len(state['texts'])
        assert call(port, '/translate', {'text': source})['translation'] == expected
        assert len(state['texts']) == before, 'resolved original did not use local cache'
        assert call(port, '/translate', {'text': '点击 SAVE', 'cache_only': False})['translation'] == '点击 保存'

        # 实际 Unity 保护器会让可见英文紧邻两个标签占位符，不能整块吞掉。
        for source in ('请 __DS_TOKEN_0__Continue__DS_TOKEN_1__',
                       '请 __DS_TOKEN_0____DS_TOKEN_1__Continue__DS_TOKEN_2__',
                       '网址 https://example.com/path，Continue',
                       '网址 https://example.com/path。Continue',
                       '文件 C:\\game\\data；Continue',
                       '网址 git+ssh://host/path！Continue'):
            expected = source.replace('Continue', '继续')
            result = call(port, '/translate', {'text': source, 'cache_only': False})
            assert result['source'] == 'api_batch' and result['translation'] == expected, result
            assert call(port, '/cache/lookup', {'texts': [source]})['hits'].get(source) == expected

        # Ren'Py 的字面括号不能套用其他引擎的标签规则；原文键保持原样。
        literals = ('请按 [[Continue] 继续。', '请按 {{Continue} 继续。',
                    '请按 <Continue> 继续。', '请按 __Continue__ 继续。')
        before = len(state['texts'])
        for source in literals:
            for engine in (None, 'unknown', 'RenPy'):
                body = {'text': source, 'cache_only': False}
                if engine is not None:
                    body['engine'] = engine
                result = call(port, '/translate', body)
                assert result['source'] == 'pass' and result['translation'] == source, result
        for source in ('请按 [Continue] 继续。', '请按 {Continue} 继续。', '已完成。'):
            result = call(port, '/translate', {'text': source, 'engine': 'renpy'})
            assert result['source'] == 'pass', result
        assert len(state['texts']) == before, 'format hint changed unrelated token visibility'
        for source in literals:
            result = call(port, '/translate', {'text': source, 'engine': 'renpy', 'cache_only': False})
            assert result['source'] == 'api_batch' and result['translation'] == source.replace('Continue', '继续'), result
        batch_literals = ['菜单 ' + source for source in literals]
        result = call(port, '/batch', {'texts': batch_literals, 'engine': 'renpy'})
        assert result['results'] == [source.replace('Continue', '继续') for source in batch_literals], result
        assert all(source == 'api_batch' for source in result['sources']), result
        queued_literal = '排队 请按 [[Continue]。'
        result = call(port, '/translate', {'text': queued_literal, 'engine': 'renpy', 'cache_only': True})
        assert result['source'] == 'queued' and result['translation'] == queued_literal, result
        prefetch_literal = '预取 请按 {{Continue}。'
        result = call(port, '/prefetch', {'texts': [prefetch_literal], 'prevs': ['Previous line.'], 'engine': 'renpy'})
        assert result['queued'] == 1, result
        for source in (queued_literal, prefetch_literal):
            deadline = time.monotonic() + 5
            while True:
                hit = call(port, '/cache/lookup', {'texts': [source]})['hits'].get(source)
                if hit is not None:
                    assert hit == source.replace('Continue', '继续'), hit
                    break
                assert time.monotonic() < deadline, 'RenPy formatted queue did not resolve'
                time.sleep(.02)
        retry_literal = '失败回队 请按 [[Continue] FORMAT_RETRY'
        result = call(port, '/translate', {'text': retry_literal, 'engine': 'renpy', 'cache_only': False})
        assert result['source'] in ('miss', 'api_batch'), result
        deadline = time.monotonic() + 5
        while True:
            hit = call(port, '/cache/lookup', {'texts': [retry_literal]})['hits'].get(retry_literal)
            if hit is not None:
                assert hit == retry_literal.replace('Continue', '继续'), hit
                break
            assert time.monotonic() < deadline, 'RenPy format was lost when failed live work returned to queue'
            time.sleep(.02)
        assert state['format_failures'] == 1

        sources = ['点击 Continue', '你好，Hello [name]', '已经是中文。']
        result = call(port, '/batch', {'texts': sources, 'cache_only': False})
        assert result['results'] == ['点击 继续', '你好，你好 [name]', sources[2]], result
        assert result['sources'][2] == 'pass', result

        queued = '保留中文 Wait for response.'
        start = time.monotonic()
        result = call(port, '/translate', {'text': queued, 'cache_only': True})
        assert time.monotonic() - start < 1.5 and result['source'] == 'queued' and result['translation'] == queued, result
        assert arrived.wait(3), 'queued provider work was never dispatched'
        release.set()
        deadline = time.monotonic() + 5
        while queued not in call(port, '/cache/lookup', {'texts': [queued]})['hits']:
            assert time.monotonic() < deadline, 'queued translation never became a hit'
            time.sleep(.02)

        context_source = '上下文 Continue the story.'
        call(port, '/prefetch', {'texts': [context_source], 'prevs': ['Previous line.']})
        deadline = time.monotonic() + 5
        while context_source not in call(port, '/cache/lookup', {'texts': [context_source]})['hits']:
            assert time.monotonic() < deadline, 'context translation never became a hit'
            time.sleep(.02)
        assert state['contexts'] > 0

        for marker in ('DROP', 'DUP', 'REORDER'):
            bad = '原中文 ' + marker + ' Open the door 后中文'
            result = call(port, '/translate', {'text': bad, 'cache_only': False})
            assert result['source'] == 'miss' and result['translation'] == bad, result
            assert bad not in call(port, '/cache/lookup', {'texts': [bad]})['hits'], 'broken Chinese entered successful cache'
        assert not state['errors'], state['errors']
        assert process.poll() is None
    assert 'mixed-cjk' in (directory / 'server.log').read_text(encoding='utf-8', errors='replace'), 'missing protection diagnostic'
    print('[PASS] ' + binary.name + ': mixed single/batch/context, adjacent tokens/URL boundaries, RenPy format/queue, cache, nonblocking queue, preserved Chinese/tags, token loss/duplicate/order rejection', flush=True)


def main():
    with tempfile.TemporaryDirectory(prefix='dst_mixed_contract_') as temp:
        for name in ('dst_server.exe', 'dst_server_cs.exe'):
            directory = Path(temp) / name
            directory.mkdir()
            check(ROOT / 'native' / name, directory)


if __name__ == '__main__':
    main()
