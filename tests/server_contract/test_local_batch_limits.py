"""真实 C/C# 服务：本机小批量、UTF-8 预算及保留大批量配置的实际提供商请求。"""

import argparse
from collections import Counter
from concurrent.futures import ThreadPoolExecutor
from contextlib import contextmanager
import hashlib
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
import os
from pathlib import Path
import subprocess
import tempfile
import threading
import time

from test_provider_body_timeout import ROOT, free_port, request


def translated(text):
    return '批量译文 ' + hashlib.sha256(text.encode('utf-8')).hexdigest()[:12]


@contextmanager
def provider():
    batches, errors, lock = [], [], threading.Lock()

    class Handler(BaseHTTPRequestHandler):
        def log_message(self, *_):
            return

        def do_POST(self):
            try:
                payload = json.loads(self.rfile.read(int(self.headers['Content-Length'])))
                user = payload['messages'][-1]['content']
                if '\nTexts to translate:\n' in user:
                    user = user.split('\nTexts to translate:\n', 1)[1]
                if user.startswith('['):
                    texts = json.loads(user)
                    answer = json.dumps([translated(text) for text in texts], ensure_ascii=False)
                else:
                    texts = [user.split('\n', 1)[1]]
                    answer = translated(texts[0])
                with lock:
                    batches.append(texts)
                body = json.dumps({'choices': [{'message': {'content': answer}}]},
                                  ensure_ascii=False).encode('utf-8')
                self.send_response(200)
                self.send_header('Content-Type', 'application/json')
                self.send_header('Content-Length', str(len(body)))
                self.end_headers()
                self.wfile.write(body)
            except (KeyError, IndexError, ValueError, OSError) as error:
                with lock:
                    errors.append(repr(error))
                self.close_connection = True

    http = ThreadingHTTPServer(('127.0.0.1', 0), Handler)
    thread = threading.Thread(target=http.serve_forever, daemon=True)
    thread.start()
    try:
        yield f'http://127.0.0.1:{http.server_port}/v1/chat/completions', batches, errors
    finally:
        http.shutdown()
        http.server_close()
        thread.join(timeout=3)


@contextmanager
def server(binary, directory, endpoint, overrides):
    ini, cache, log = (directory / name for name in ('api.ini', 'cache.tsv', 'server.log'))
    ini.write_text(f'[api]\nendpoint={endpoint}\nmodel=fixture\nkey=fixture\n'
                   'timeout_ms=6000\nconcurrency=1\n' + overrides, encoding='ascii')
    port = free_port()
    with log.open('wb') as output:
        process = subprocess.Popen([str(binary), '--port', str(port), '--cache', str(cache),
                                    '--api-config', str(ini)], stdout=output, stderr=output,
                                   creationflags=subprocess.CREATE_NO_WINDOW if os.name == 'nt' else 0)
        try:
            deadline = time.monotonic() + 8
            while True:
                try:
                    assert request(port, '/health', timeout=1)[0] == 200
                    break
                except OSError:
                    assert process.poll() is None and time.monotonic() < deadline, 'server failed to start'
                    time.sleep(.02)
            yield port
        except Exception:
            output.flush()
            print(log.read_text(encoding='utf-8', errors='replace'), flush=True)
            raise
        finally:
            if process.poll() is None:
                process.terminate()
            process.wait(timeout=8)


def call(port, path, body):
    status, response = request(port, path, body)
    assert status == 200, (status, response)
    return json.loads(response)


def dispatch(port, route, texts):
    if route == 'batch':
        # 同步分块不能改变结果次序或去重后的回填行为。
        inputs = texts + [texts[0], texts[-1]]
        result = call(port, '/batch', {'texts': inputs})
        assert result['results'] == [translated(text) for text in inputs], result
        assert result['sources'] == ['api_batch'] * len(inputs), result
    elif route == 'prefetch':
        result = call(port, '/prefetch', {'texts': texts, 'prevs': ['Previous game line.'] * len(texts)})
        assert result['queued'] == len(texts), result
    else:
        barrier = threading.Barrier(len(texts))

        def single(text):
            barrier.wait(timeout=8)
            result = call(port, '/translate', {'text': text})
            assert result['translation'] == translated(text) and result['source'] == 'api_batch', result

        with ThreadPoolExecutor(max_workers=len(texts)) as pool:
            list(pool.map(single, texts))

    deadline = time.monotonic() + 8
    while True:
        hits = call(port, '/cache/lookup', {'texts': texts})['hits']
        if len(hits) == len(texts):
            assert all(hits[text] == translated(text) for text in texts), hits
            break
        assert time.monotonic() < deadline, 'dispatched translations were not cached'
        time.sleep(.02)
    # 再次读取必须命中缓存，不触发额外提供商请求。
    result = call(port, '/batch', {'texts': texts, 'cache_only': True})
    assert result['sources'] == ['cache'] * len(texts), result


def expected_chunks(texts, max_items, byte_budget):
    sizes, current, used = [], 0, 0
    for text in texts:
        size = len(text.encode('utf-8'))
        if current and (current >= max_items or used + size > byte_budget):
            sizes.append(current)
            current, used = 0, 0
        current += 1
        used += size
    if current:
        sizes.append(current)
    return sorted(sizes)


def check(binary, directory, route, texts, max_items=8, byte_budget=2048,
          overrides=''):
    with provider() as (endpoint, batches, errors), server(binary, directory, endpoint, overrides) as port:
        dispatch(port, route, texts)
        assert not errors, errors
        assert Counter(text for batch in batches for text in batch) == Counter(texts), batches
        for batch in batches:
            assert len(batch) <= max_items, [len(part) for part in batches]
            size = sum(len(text.encode('utf-8')) for text in batch)
            assert size <= byte_budget or len(batch) == 1, (size, byte_budget, batch)
        if route == 'batch':
            actual = sorted(len(batch) for batch in batches)
            assert actual == expected_chunks(texts, max_items, byte_budget), actual
        print(f'[PASS] {binary.name} {route}: {len(batches)} actual requests, '
              f'items={[len(batch) for batch in batches]}, budget={max_items}/{byte_budget}', flush=True)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--exe', type=Path, action='append', help='server binary; default: both native binaries')
    args = parser.parse_args()
    binaries = args.exe or [ROOT / 'native' / name for name in ('dst_server.exe', 'dst_server_cs.exe')]
    short = [f'Translate game line number {i:02d}.' for i in range(19)]
    unicode = [f'Translate game line {i}: ' + 'é😊' * 100 for i in range(10)]
    medium = [f'Translate game line {i}: ' + 'c' * 100 for i in range(7)]
    oversized = ['Translate this long line: ' + 'a' * 3000, 'Translate the next line.']
    cases = []
    for route in ('batch', 'prefetch', 'live'):
        cases.extend([
            (route + '-default-count', dict(route=route, texts=short)),
            (route + '-default-utf8', dict(route=route, texts=unicode)),
            (route + '-overrides', dict(route=route, texts=short, max_items=3, byte_budget=256,
                                        overrides='batch_max_items=3\nbatch_char_budget=256\n')),
            (route + '-byte-override', dict(route=route, texts=medium, max_items=8, byte_budget=256,
                                            overrides='batch_char_budget=256\n')),
            (route + '-oversize', dict(route=route, texts=oversized)),
        ])
    cases.extend([
        # 远程配置默认值另由配置探针验证；这里通过显式设置旧预算，只连接回环假提供商。
        ('legacy-bulk-count', dict(route='batch', texts=[f'Translate remote game line {i:02d}.' for i in range(49)],
                                  max_items=48, byte_budget=9600,
                                  overrides='batch_max_items=48\nbatch_char_budget=9600\n')),
        ('legacy-bulk-bytes', dict(route='batch', texts=[f'Translate remote item {i:02d}: ' + 'b' * 574 for i in range(19)],
                                  max_items=48, byte_budget=9600,
                                  overrides='batch_max_items=48\nbatch_char_budget=9600\n')),
    ])
    with tempfile.TemporaryDirectory(prefix='dst_local_batch_') as folder:
        for binary_index, binary in enumerate(binaries):
            for name, options in cases:
                directory = Path(folder) / f'{binary_index}_{name}'
                directory.mkdir()
                check(binary.resolve(), directory, **options)


if __name__ == '__main__':
    main()
