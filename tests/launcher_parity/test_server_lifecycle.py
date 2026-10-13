"""通过夹具拥有的本地服务器验证真实启动器 HTTP 生命周期。"""
import argparse
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
from pathlib import Path
import re
import subprocess
import threading
import time

parser = argparse.ArgumentParser()
parser.add_argument('--c', type=Path, required=True)
parser.add_argument('--cs', type=Path, required=True)
parser.add_argument('--root', type=Path, required=True)
args = parser.parse_args()
args.root.mkdir(parents=True, exist_ok=True)
failures = []
passed = 0

for mode in ('refuse', 'reject', 'stop', 'delay', 'slow-health'):
    for binary in (args.c, args.cs):
        state = {'shutdown_at': None, 'shutdowns': 0}

        class Handler(BaseHTTPRequestHandler):
            def log_message(self, *unused):
                pass

            def do_GET(self):
                if mode == 'slow-health':
                    time.sleep(.1)
                stopped = state['shutdown_at'] is not None and time.monotonic() >= state['shutdown_at']
                body = json.dumps({'status': 'ok', 'server': 'dst_server_c', 'cache_size': 1}).encode()
                self.send_response(503 if stopped else 200)
                self.send_header('Content-Length', str(len(body)))
                self.end_headers()
                self.wfile.write(body)

            def do_POST(self):
                assert self.path == '/shutdown'
                state['shutdowns'] += 1
                if mode in ('stop', 'delay'):
                    state['shutdown_at'] = time.monotonic() + (0 if mode == 'stop' else .5)
                self.send_response(503 if mode == 'reject' else 200)
                self.send_header('Content-Length', '2')
                self.end_headers()
                self.wfile.write(b'{}')

        with ThreadingHTTPServer(('127.0.0.1', 19999), Handler) as server:
            thread = threading.Thread(target=server.serve_forever, daemon=True)
            thread.start()
            try:
                result = subprocess.run([str(binary), str(args.root)], capture_output=True, timeout=15,
                                        creationflags=subprocess.CREATE_NO_WINDOW)
            finally:
                server.shutdown()
                thread.join(timeout=5)
        output = result.stdout.decode('utf-8', 'replace').replace('\r\n', '\n')
        fields = dict(re.findall(r'^(before|after|alive|refreshed|stop_ms)=(\d+)$', output, re.M))
        still_running = mode not in ('stop', 'delay')
        errors = []
        if result.returncode != 0 or fields.get('before') != '1':
            errors.append('could not adopt fixture service')
        for field in ('after', 'alive', 'refreshed'):
            if fields.get(field) != str(int(still_running)):
                errors.append(f'{field} must reflect actual service state ({int(still_running)})')
        if state['shutdowns'] != 1:
            errors.append('shutdown request count changed')
        if mode == 'slow-health' and int(fields.get('stop_ms', '99999')) > 2100:
            errors.append('shutdown deadline excludes HTTP duration')
        if still_running and 'still owns' not in output and '发送停止请求失败' not in output:
            errors.append('missing shutdown failure diagnostic')
        name = f'{binary.stem}/{mode}'
        if errors:
            failures.append(name)
            print('[FAIL] ' + name + ': ' + '; '.join(errors) + '\n' + output)
        else:
            passed += 1
            print('[PASS] ' + name)
print(f'Launcher lifecycle: PASS={passed} FAIL={len(failures)}')
raise SystemExit(1 if failures else 0)
