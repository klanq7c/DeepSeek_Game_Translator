"""真实提供商响应边界：完整 UTF-8 JSON 与 choices[0].message.content 的字段所有权。"""

import argparse
from contextlib import contextmanager
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
from pathlib import Path
import tempfile
import threading
import sys

from test_local_batch_limits import ROOT, call, server

sys.stdout.reconfigure(encoding='utf-8', errors='backslashreplace')


def response(mode):
    message = {'choices': [{'message': {'content': '正确译文'}}]}
    if mode == 'decoy-content':
        message = {'metadata': {'content': '错误字段'}, **message}
    elif mode == 'wrong-root':
        message = {'content': '错误字段'}
    elif mode == 'wrong-first-choice':
        message = {'choices': [None, {'message': {'content': '错误字段'}}]}
    elif mode == 'valid-metadata':
        message = {'metadata': {'nul': '\0', 'lone': '\ud800', 'control': '\t\n',
                                'list': [None, True, False, -3, .125, 2e12], '😀': '保留'}, **message}
        return json.dumps(message, ensure_ascii=True).encode('ascii')
    raw = json.dumps(message, ensure_ascii=False).encode('utf-8')
    if mode == 'truncated-json':
        raw = raw[:-4]
    elif mode == 'invalid-utf8':
        raw = raw.replace('正确译文'.encode('utf-8'), b'\xffbroken')
    elif mode == 'trailing-bytes':
        raw += b'garbage'
    elif mode == 'raw-nul':
        raw += b'\0'
    elif mode == 'number-overflow':
        raw = b'{"metadata":1e999,' + raw[1:]
    elif mode == 'duplicate-metadata':
        raw = b'{"metadata":{"same":1,"s\\u0061me":2},' + raw[1:]
    elif mode == 'duplicate-nul-key':
        raw = b'{"metadata":{"\\u0000":1,"\\u0000":2},' + raw[1:]
    elif mode == 'duplicate-emoji-key':
        raw = '{"metadata":{"😀":1,"\\ud83d\\ude00":2},'.encode('utf-8') + raw[1:]
    elif mode in ('isolated-high-surrogate', 'isolated-low-surrogate', 'escaped-nul',
                   'invalid-escape-leading-space', 'invalid-escape-trailing-space'):
        bad = {'isolated-high-surrogate': b'\\ud800', 'isolated-low-surrogate': b'\\udc00',
               'escaped-nul': b'\\u0000', 'invalid-escape-leading-space': b'\\u 041',
               'invalid-escape-trailing-space': b'\\u004 '}[mode]
        raw = raw.replace('正确译文'.encode('utf-8'), bad)
    elif mode == 'invalid-key-escape':
        raw = raw.replace(b'"content"', b'"\\u 063ontent"')
    return raw


@contextmanager
def provider(mode):
    class Handler(BaseHTTPRequestHandler):
        def log_message(self, *_):
            return

        def do_POST(self):
            self.rfile.read(int(self.headers['Content-Length']))
            body = response(mode)
            self.send_response(200)
            self.send_header('Content-Length', str(len(body)))
            self.end_headers()
            self.wfile.write(body)

    http = ThreadingHTTPServer(('127.0.0.1', 0), Handler)
    thread = threading.Thread(target=http.serve_forever, daemon=True)
    thread.start()
    try:
        yield f'http://127.0.0.1:{http.server_port}/v1/chat/completions'
    finally:
        http.shutdown()
        http.server_close()
        thread.join(timeout=3)


def check(binary, directory, mode):
    source = 'Read a complete provider translation.'
    with provider(mode) as endpoint, server(binary, directory, endpoint, '') as port:
        result = call(port, '/translate', {'text': source})
        hits = call(port, '/cache/lookup', {'texts': [source]})['hits']
        if mode in ('valid', 'decoy-content', 'valid-metadata'):
            assert result['translation'] == '正确译文', result
            assert hits.get(source) == '正确译文', hits
        else:
            assert result['translation'] == source and result['source'] == 'miss', result
            assert source not in hits, hits
    print(f'[PASS] {binary.name}: {mode}', flush=True)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--exe', type=Path, action='append')
    parser.add_argument('--output', type=Path)
    args = parser.parse_args()
    binaries = args.exe or [ROOT / 'native' / name for name in ('dst_server.exe', 'dst_server_cs.exe')]
    cases = ('valid', 'decoy-content', 'valid-metadata', 'wrong-root', 'wrong-first-choice',
             'truncated-json', 'invalid-utf8', 'trailing-bytes', 'raw-nul', 'number-overflow',
             'duplicate-metadata', 'duplicate-nul-key', 'duplicate-emoji-key',
             'isolated-high-surrogate', 'isolated-low-surrogate', 'escaped-nul',
             'invalid-escape-leading-space', 'invalid-escape-trailing-space', 'invalid-key-escape')
    failures = []
    with tempfile.TemporaryDirectory(prefix='dst_provider_envelope_') as temp:
        output = args.output or Path(temp)
        for index, binary in enumerate(binaries):
            for mode in cases:
                directory = output / f'{index}_{mode}'
                directory.mkdir(parents=True, exist_ok=True)
                try:
                    check(binary.resolve(), directory, mode)
                except AssertionError as error:
                    failures.append((binary.name, mode, repr(error)))
                    print('[FAIL]', binary.name, mode, repr(error), flush=True)
    print(f'Provider envelope: {len(cases) * len(binaries) - len(failures)} passed, '
          f'{len(failures)} failed', flush=True)
    raise SystemExit(bool(failures))


if __name__ == '__main__':
    main()
