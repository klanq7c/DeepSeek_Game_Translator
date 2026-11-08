"""真实 C/C# 服务：上下文去重必须比较原文，中文保护不能合并不同前文。"""

import argparse
from contextlib import contextmanager
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
from pathlib import Path
import tempfile
import threading
import time
import sys

from test_local_batch_limits import ROOT, call, server

sys.stdout.reconfigure(encoding='utf-8', errors='backslashreplace')


@contextmanager
def provider():
    requests, errors = [], []

    class Handler(BaseHTTPRequestHandler):
        def log_message(self, *_):
            return

        def do_POST(self):
            try:
                payload = json.loads(self.rfile.read(int(self.headers['Content-Length'])))
                user = payload['messages'][-1]['content']
                context, raw = user.split('\nTexts to translate:\n', 1)
                texts = json.loads(raw)
                requests.append((context, texts))
                answer = json.dumps([text.replace('Continue', '继续').replace('Open', '打开')
                                     for text in texts], ensure_ascii=False)
                body = json.dumps({'choices': [{'message': {'content': answer}}]},
                                  ensure_ascii=False).encode('utf-8')
                self.send_response(200)
                self.send_header('Content-Length', str(len(body)))
                self.end_headers()
                self.wfile.write(body)
            except (KeyError, ValueError, OSError) as error:
                errors.append(repr(error))
                self.close_connection = True

    http = ThreadingHTTPServer(('127.0.0.1', 0), Handler)
    thread = threading.Thread(target=http.serve_forever, daemon=True)
    thread.start()
    try:
        yield f'http://127.0.0.1:{http.server_port}/v1/chat/completions', requests, errors
    finally:
        http.shutdown()
        http.server_close()
        thread.join(timeout=3)


def check(binary, directory, name, texts, prevs, expected_indices):
    with provider() as (endpoint, requests, errors), server(binary, directory, endpoint, '') as port:
        result = call(port, '/prefetch', {'texts': texts, 'prevs': prevs})
        assert result['queued'] == len(texts), result
        deadline = time.monotonic() + 8
        while True:
            hits = call(port, '/cache/lookup', {'texts': texts})['hits']
            if len(hits) == len(texts):
                break
            assert time.monotonic() < deadline, ('context request did not resolve', requests, errors)
            time.sleep(.02)
        assert not errors, errors
        assert len(requests) == 1 and len(requests[0][1]) == len(texts), requests
        context, _ = requests[0]
        actual = [int(line.split(': ', 1)[0]) for line in context.splitlines()[1:] if line]
        assert actual == expected_indices, (name, 'expected context indices', expected_indices,
                                            'actual', actual, 'provider context', context)
        assert all(hits[text] == text.replace('Continue', '继续').replace('Open', '打开')
                   for text in texts), hits
    print(f'[PASS] {binary.name}: {name}', flush=True)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--exe', type=Path, action='append')
    parser.add_argument('--output', type=Path)
    args = parser.parse_args()
    binaries = args.exe or [ROOT / 'native' / name for name in ('dst_server.exe', 'dst_server_cs.exe')]
    cases = [
        ('plain-identical', ['Continue first.', 'Open the door second.'],
         ['Earlier line.', 'Continue first.'], [0]),
        ('mixed-identical', ['她 Continue first.', 'Open the door second.'],
         ['Earlier line.', '她 Continue first.'], [0]),
        ('mixed-distinct-collision', ['她 Continue first.', 'Open the door second.'],
         ['Earlier line.', '他 Continue first.'], [0, 1]),
        ('missing-first-context', ['Continue first.', 'Open the door second.'],
         ['', 'Continue first.'], []),
        ('mixed-missing-first-context', ['她 Continue first.', 'Open the door second.'],
         ['', '她 Continue first.'], []),
        ('markup-distinct-context', ['<b>她</b> Continue first.', 'Open the door [player].'],
         ['Earlier line.', '<b>他</b> Continue first.'], [0, 1]),
    ]
    failures = []
    with tempfile.TemporaryDirectory(prefix='dst_context_dedup_') as temp:
        output = args.output or Path(temp)
        for index, binary in enumerate(binaries):
            for name, texts, prevs, expected in cases:
                directory = output / f'{index}_{name}'
                directory.mkdir(parents=True, exist_ok=True)
                try:
                    check(binary.resolve(), directory, name, texts, prevs, expected)
                except AssertionError as error:
                    failures.append((binary.name, name, repr(error)))
                    print('[FAIL]', binary.name, name, repr(error), flush=True)
    print(f'Context dedup: {len(cases) * len(binaries) - len(failures)} passed, '
          f'{len(failures)} failed', flush=True)
    raise SystemExit(bool(failures))


if __name__ == '__main__':
    main()
