"""真实 C/C# 服务：非法端点、跨批实时并发和慢速本地模型的截止时间。"""
import argparse
from concurrent.futures import ThreadPoolExecutor
from contextlib import contextmanager
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
import hashlib
import os
import socket
from pathlib import Path
import subprocess
import sys
import tempfile
import threading
import time

from test_provider_body_timeout import ROOT, free_port, request

sys.stdout.reconfigure(errors='backslashreplace')


@contextmanager
def server(binary, directory, endpoint, timeout=6000):
    ini, cache, log = (directory / name for name in ('api.ini', 'cache.tsv', 'server.log'))
    ini.write_text(f'[api]\nendpoint={endpoint}\nmodel=fixture\nkey=fixture\n'
                   f'concurrency=4\ntimeout_ms={timeout}\n', encoding='ascii')
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
                    if process.poll() is not None or time.monotonic() >= deadline:
                        raise AssertionError('server failed to start')
                    time.sleep(.05)
            yield port, process
        except Exception:
            output.flush()
            print(log.read_text(encoding='utf-8', errors='replace'))
            raise
        finally:
            if process.poll() is None:
                process.terminate()
            process.wait(timeout=8)


@contextmanager
def provider(batch=False):
    arrived, overlapping, release = threading.Event(), threading.Event(), threading.Event()
    lock = threading.Lock()
    state = {'active': 0, 'calls': 0}

    class Handler(BaseHTTPRequestHandler):
        def log_message(self, *_):
            return

        def do_POST(self):
            payload = json.loads(self.rfile.read(int(self.headers['Content-Length'])))
            user = payload['messages'][-1]['content']
            if '\nTexts to translate:\n' in user:
                user = user.split('\nTexts to translate:\n', 1)[1]
            texts = json.loads(user) if user.startswith('[') else None
            with lock:
                state['active'] += 1
                state['calls'] += 1
                if state['active'] >= 2:
                    overlapping.set()
            arrived.set()
            try:
                if not release.wait(130):
                    raise AssertionError('provider fixture exceeded its deadline')
                content = json.dumps([batch_translation(text) for text in texts]) if batch and texts is not None else '边界检查译文'
                answer = json.dumps({'choices': [{'message': {'content': content}}]}).encode()
                self.send_response(200)
                self.send_header('Content-Length', str(len(answer)))
                self.end_headers()
                self.wfile.write(answer)
            except (BrokenPipeError, ConnectionResetError) as error:
                print('[fixture] provider peer closed:', type(error).__name__, flush=True)
            finally:
                with lock:
                    state['active'] -= 1

    http = ThreadingHTTPServer(('127.0.0.1', 0), Handler)
    thread = threading.Thread(target=http.serve_forever, daemon=True)
    thread.start()
    try:
        yield f'http://127.0.0.1:{http.server_port}/v1/chat/completions', arrived, overlapping, release
    finally:
        release.set()
        http.shutdown()
        http.server_close()
        thread.join(timeout=3)


def invalid_endpoint(binary, directory):
    with server(binary, directory, 'http://localhost:bad/v1/chat/completions') as (port, process):
        status, _ = request(port, '/prefetch', {'texts': ['Invalid endpoint must not kill the server.']})
        assert status == 200
        # 等待队列消费，避免仅测到入队后的瞬间健康状态。
        deadline = time.monotonic() + 4
        while True:
            assert process.poll() is None, 'invalid endpoint terminated the server'
            status, body = request(port, '/health')
            health = json.loads(body)
            if health['async_memory_bytes'] == 0:
                break
            assert time.monotonic() < deadline, 'invalid endpoint did not drain'
            time.sleep(.05)
        status, body = request(port, '/translate', {'text': 'Invalid live endpoint.', 'cache_only': False})
        assert status == 200 and json.loads(body)['source'] == 'miss', body
        assert request(port, '/health')[0] == 200
        status, body = request(port, '/cache/lookup', {'texts': ['Invalid live endpoint.']})
        assert status == 200 and not json.loads(body)['hits'], body


def independent_live_requests(binary, directory):
    with provider() as (endpoint, arrived, overlapping, release):
        with server(binary, directory, endpoint) as (port, process):
            with ThreadPoolExecutor(max_workers=2) as pool:
                first = pool.submit(request, port, '/translate', {'text': 'First live text.', 'cache_only': False})
                try:
                    assert arrived.wait(3), 'first request did not reach provider'
                    second = pool.submit(request, port, '/translate', {'text': 'Second live text.', 'cache_only': False})
                    assert overlapping.wait(2), 'second live request waited for the first batch to finish'
                finally:
                    release.set()
                for future in (first, second):
                    status, body = future.result(timeout=8)
                    assert status == 200 and json.loads(body)['translated_text'] == '边界检查译文', body
            assert process.poll() is None


def batch_translation(text):
    return '批次译文' + hashlib.sha256(text.encode()).hexdigest()[:12]


def batch_live_accounting(binary, directory):
    texts = [f'Batch queue ownership text number {i}.' for i in range(80)]
    with provider(batch=True) as (endpoint, arrived, overlapping, release):
        with server(binary, directory, endpoint) as (port, _):
            with ThreadPoolExecutor(max_workers=1) as pool:
                future = pool.submit(request, port, '/batch', {'texts': texts + texts[:2]}, 20)
                try:
                    assert arrived.wait(4), 'batch did not reach provider'
                    health = json.loads(request(port, '/health')[1])
                    assert health['live_memory_bytes'] > 0, 'batch bypassed live memory accounting'
                    assert health['live_memory_bytes'] <= health['live_memory_limit']
                    assert overlapping.wait(2), 'batch lost multi-channel dispatch'
                finally:
                    release.set()
                status, body = future.result(timeout=15)
                result = json.loads(body)
                assert status == 200 and result['results'] == [batch_translation(t) for t in texts + texts[:2]], 'batch result order changed'
                assert all(s == 'api_batch' for s in result['sources']), body
            deadline = time.monotonic() + 3
            while json.loads(request(port, '/health')[1])['live_memory_bytes'] != 0:
                assert time.monotonic() < deadline, 'batch did not release live budget'
                time.sleep(.02)


def batch_budget_exhaustion(binary, directory):
    # 足够填满两版的 32 MiB 实时预算，单个 HTTP 请求仍小于 2 MiB。
    # 提供商持有响应直到明确观察到准入拒绝，不靠固定延时猜队列状态。
    with provider(batch=True) as (endpoint, arrived, overlapping, release):
        with server(binary, directory, endpoint, timeout=20000) as (port, _):
            with ThreadPoolExecutor(max_workers=36) as pool:
                futures = [pool.submit(request, port, '/batch', {'texts': [
                    f'Budget text {i} first ' + 'x' * 524200,
                    f'Budget text {i} second ' + 'y' * 524200]}, 40) for i in range(36)]
                try:
                    assert arrived.wait(5), 'budget fixture never reached provider'
                    deadline = time.monotonic() + 12
                    while not any(f.done() for f in futures):
                        assert time.monotonic() < deadline, 'no batch was rejected at the live budget'
                        time.sleep(.02)
                    health = json.loads(request(port, '/health')[1])
                    assert 0 < health['live_memory_bytes'] <= health['live_memory_limit'], health
                    rejected = next(f for f in futures if f.done())
                    status, body = rejected.result()
                    assert status == 200 and json.loads(body)['sources'] == ['miss', 'miss'], 'budget rejection must remain unresolved'
                finally:
                    release.set()
                for future in futures:
                    assert future.result(timeout=30)[0] == 200
            assert json.loads(request(port, '/health')[1])['live_memory_bytes'] == 0, 'budget remained reserved after all responses'


def batch_disconnect(binary, directory):
    # 客户端关闭连接不能遗留整组任务的预算，也不能终止工作线程。
    with provider(batch=True) as (endpoint, arrived, _, release):
        with server(binary, directory, endpoint) as (port, process):
            texts = [f'Disconnected batch item {i}.' for i in range(80)]
            body = json.dumps({'texts': texts, 'cache_only': False}).encode()
            with socket.create_connection(('127.0.0.1', port), timeout=3) as client:
                client.sendall(f'POST /batch HTTP/1.1\r\nHost: localhost\r\nContent-Length: {len(body)}\r\n\r\n'.encode() + body)
                assert arrived.wait(4), 'disconnected batch did not enter provider'
                assert json.loads(request(port, '/health')[1])['live_memory_bytes'] > 0
            release.set()
            deadline = time.monotonic() + 8
            while json.loads(request(port, '/health')[1])['live_memory_bytes'] != 0:
                assert time.monotonic() < deadline, 'client disconnect leaked live budget'
                time.sleep(.02)
            assert process.poll() is None, 'client disconnect killed server'
            status, data = request(port, '/batch', {'texts': texts, 'cache_only': True})
            assert status == 200 and json.loads(data)['sources'] == ['cache'] * len(texts), data


def batch_shutdown(binary, directory):
    # 远程响应由本测试持有，确认队列中存在候选后才关停；不依赖固定睡眠制造竞争。
    with provider(batch=True) as (endpoint, arrived, _, release):
        with server(binary, directory, endpoint) as (port, process):
            with ThreadPoolExecutor(max_workers=12) as pool:
                groups = [[f'Shutdown group {g} item {i}: ' + 'q' * 80 for i in range(16)] for g in range(12)]
                futures = [pool.submit(request, port, '/batch', {'texts': texts, 'cache_only': False}, 12)
                           for texts in groups]
                try:
                    assert arrived.wait(4), 'shutdown batch did not enter provider'
                    deadline = time.monotonic() + 5
                    while json.loads(request(port, '/health')[1])['live_memory_bytes'] < 16000:
                        assert time.monotonic() < deadline, 'shutdown fixture did not reserve queued groups'
                        time.sleep(.02)
                    assert request(port, '/shutdown', {})[0] == 200
                finally:
                    release.set()
                for texts, future in zip(groups, futures):
                    status, data = future.result(timeout=8)
                    reply = json.loads(data)
                    assert status == 200 and len(reply['results']) == len(reply['sources']) == len(texts), data
                    for original, value, source in zip(texts, reply['results'], reply['sources']):
                        assert (source == 'miss' and value == original) or (source == 'api_batch' and value == batch_translation(original)), (source, value)
                assert process.wait(timeout=8) == 0, 'shutdown did not finish outstanding groups'


def slow_local_model(binary, directory):
    with provider() as (endpoint, arrived, overlapping, release):
        with server(binary, directory, endpoint, timeout=120000) as (port, _):
            with ThreadPoolExecutor(max_workers=1) as pool:
                future = pool.submit(request, port, '/translate',
                                     {'text': 'A slow local model generation.', 'cache_only': False}, 125)
                try:
                    assert arrived.wait(4)
                    # 等待的所有者是本地假提供商；实际响应晚于旧的隐式 100 秒限制。
                    release.wait(104)
                finally:
                    release.set()
                status, body = future.result(timeout=15)
                assert status == 200 and json.loads(body)['translated_text'] == '边界检查译文', body


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--slow', action='store_true', help='also run the 104-second local response case')
    parser.add_argument('--case', choices=['invalid', 'concurrent', 'batch', 'budget', 'disconnect', 'shutdown', 'slow'])
    args = parser.parse_args()
    cases = [('invalid', invalid_endpoint), ('concurrent', independent_live_requests), ('batch', batch_live_accounting), ('budget', batch_budget_exhaustion), ('disconnect', batch_disconnect), ('shutdown', batch_shutdown)]
    if args.slow or args.case == 'slow':
        cases.append(('slow', slow_local_model))
    failures = []
    for name in ('dst_server.exe', 'dst_server_cs.exe'):
        for case, check in cases:
            if args.case and args.case != case:
                continue
            # 每个夹具目录均由本测试创建，关闭拥有的进程和日志后再自动回收。
            with tempfile.TemporaryDirectory(prefix='dst_api_boundary_') as folder:
                try:
                    check(ROOT / 'native' / name, Path(folder))
                    print('[PASS]', name, case, flush=True)
                except Exception as error:
                    failures.append(f'{name}/{case}: {error}')
                    print('[FAIL]', failures[-1], flush=True)
    assert not failures, '\n'.join(failures)


if __name__ == '__main__':
    main()
