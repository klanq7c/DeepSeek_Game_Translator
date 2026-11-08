"""真实 C/C# 服务：原文提示词形状的前缀不能被响应或缓存归一化删除。"""
import argparse
from contextlib import contextmanager
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
from pathlib import Path
import tempfile
import threading
import time

from test_api_boundaries import ROOT, server
from test_provider_body_timeout import request


def call(port, path, body):
    status, response = request(port, path, body)
    assert status == 200, (status, response)
    return json.loads(response)


@contextmanager
def provider():
    state = {'calls': 0, 'echo': 0}

    def translate(text):
        state['calls'] += 1
        translated = text.replace('Open the door', '打开门').replace('Continue', '继续')
        translated = translated.replace('Simplified Chinese translation:', '简体中文翻译：')
        return '简体中文译文：' * state['echo'] + translated

    class Handler(BaseHTTPRequestHandler):
        def log_message(self, *_):
            return

        def do_POST(self):
            payload = json.loads(self.rfile.read(int(self.headers['Content-Length'])))
            user = payload['messages'][-1]['content']
            if '\nTexts to translate:\n' in user:
                user = user.split('\nTexts to translate:\n', 1)[1]
            if user.startswith('['):
                answer = json.dumps([translate(text) for text in json.loads(user)], ensure_ascii=False)
            else:
                answer = translate(user.split('\n', 1)[1])
            body = json.dumps({'choices': [{'message': {'content': answer}}]}, ensure_ascii=False).encode()
            self.send_response(200)
            self.send_header('Content-Length', str(len(body)))
            self.end_headers()
            self.wfile.write(body)

    http = ThreadingHTTPServer(('127.0.0.1', 0), Handler)
    thread = threading.Thread(target=http.serve_forever, daemon=True)
    thread.start()
    try:
        yield f'http://127.0.0.1:{http.server_port}/v1/chat/completions', state
    finally:
        http.shutdown()
        http.server_close()
        thread.join(timeout=3)


def check(binary, directory):
    expected = {}
    with provider() as (endpoint, state):
        with server(binary, directory, endpoint) as (port, _):
            for echo in (0, 1, 2):
                state['echo'] = echo
                for source in ('翻译成简体中文：Open the door', '简体中文译文：Continue',
                               '翻译成简体中文：简体中文译文：Continue',
                               'Simplified Chinese translation: 请 Continue'):
                    source += ' ' + str(echo)
                    value = source.replace('Open the door', '打开门').replace('Continue', '继续')
                    value = value.replace('Simplified Chinese translation:', '简体中文翻译：')
                    result = call(port, '/translate', {'text': source, 'cache_only': False})
                    assert result['translation'] == value and result['source'] == 'api_batch', (source, result)
                    again = call(port, '/translate', {'text': source})
                    assert again['translation'] == value and again['source'] == 'cache', (source, again)
                    expected[source] = value

            sources = ['翻译成简体中文：Open the door batch', 'Simplified Chinese translation: 请 Continue batch']
            values = ['翻译成简体中文：打开门 batch', '简体中文翻译： 请 继续 batch']
            result = call(port, '/batch', {'texts': sources, 'cache_only': False})
            assert result['results'] == values, result
            expected.update(zip(sources, values))

            source, value = '简体中文译文：Continue context', '简体中文译文：继续 context'
            call(port, '/prefetch', {'texts': [source], 'prevs': ['Previous line.']})
            deadline = time.monotonic() + 5
            while source not in call(port, '/cache/lookup', {'texts': [source]})['hits']:
                assert time.monotonic() < deadline, 'context result never entered cache'
                time.sleep(.02)
            expected[source] = value

            imported = {'翻译成简体中文：Continue import': '翻译成简体中文：继续 import',
                        'RenPy [player] {b}line{/b}': '译 [player] {b}文{/b}',
                        'RPG \\c[2]line\\v[1]': '译 \\c[2]文\\v[1]',
                        'Unity <color=red>line</color>': '译 <color=red>文</color>',
                        'Godot [color=red]line[/color]': '译 [color=red]文[/color]'}
            entries = [{'key': key, 'value': '简体中文译文：' + value} for key, value in imported.items()]
            identity = '翻译成简体中文：Continue identity'
            entries += [{'key': identity, 'value': identity},
                        {'key': identity, 'value': '简体中文译文：' + identity}]
            result = call(port, '/cache/import', {'entries': entries})
            assert result['accepted'] == len(imported) and result['rejected'] == 2, result
            expected.update(imported)
            assert call(port, '/cache/lookup', {'texts': list(expected)})['hits'] == expected

        # 同一夹具的重启只能读取测试自己创建的缓存，不修改用户翻译记忆。
        with server(binary, directory, endpoint) as (port, _):
            before = state['calls']
            assert call(port, '/cache/lookup', {'texts': list(expected)})['hits'] == expected
            for source, value in expected.items():
                result = call(port, '/translate', {'text': source})
                assert result['source'] == 'cache' and result['translation'] == value, result
            assert before == state['calls'], 'cache reload unexpectedly called provider'
    print('[PASS] ' + binary.name + ': original Chinese/English prefixes, genuine echo, batch/context, import, identity rejection and restart', flush=True)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--exe', action='append', type=Path)
    args = parser.parse_args()
    binaries = args.exe or [ROOT / 'native' / name for name in ('dst_server.exe', 'dst_server_cs.exe')]
    with tempfile.TemporaryDirectory(prefix='dst_mixed_prefix_') as temp:
        for index, binary in enumerate(binaries):
            directory = Path(temp) / str(index)
            directory.mkdir()
            check(binary.resolve(), directory)


if __name__ == '__main__':
    main()
