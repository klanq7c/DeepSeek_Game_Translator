"""使用真实 C/C# 服务器测试提供商发送响应头后停滞的情况。"""

import concurrent.futures
import http.client
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
import os
from pathlib import Path
import socket
import subprocess
import tempfile
import threading
import time

ROOT = Path(__file__).resolve().parents[2]
SOURCE = "Open the treasure chest"
TRANSLATED = "打开宝箱"


def free_port():
    with socket.socket() as sock:
        sock.bind(("127.0.0.1", 0))
        return sock.getsockname()[1]


def request(port, path, body=None, timeout=15):
    connection = http.client.HTTPConnection("127.0.0.1", port, timeout=timeout)
    try:
        data = json.dumps(body).encode() if body is not None else None
        connection.request("POST" if data else "GET", path, data,
                           {"Content-Type": "application/json"})
        response = connection.getresponse()
        return response.status, response.read().decode("utf-8")
    finally:
        connection.close()


def check_server(binary, root):
    arrived = threading.Event()
    release = threading.Event()
    stalled_calls = []
    normal = threading.Event()

    class Provider(BaseHTTPRequestHandler):
        def log_message(self, *_):
            return

        def do_POST(self):
            try:
                self.rfile.read(int(self.headers.get("Content-Length", 0)))
            except ConnectionResetError as error:
                # 夹具退出时，由测试拥有的服务器可能关闭已排队的套接字。
                print(f"[provider-test] request body peer closed ({type(error).__name__})", flush=True)
                return
            if normal.is_set():
                answer = json.dumps({"choices": [{"message": {"content": TRANSLATED}}]}).encode()
                self.send_response(200)
                self.send_header("Content-Type", "application/json")
                self.send_header("Content-Length", str(len(answer)))
                self.end_headers()
                self.wfile.write(answer)
                return
            stalled_calls.append(time.monotonic())
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", "1000")
            self.end_headers()
            self.wfile.write(b"{")
            self.wfile.flush()
            arrived.set()
            release.wait(20)

    provider = ThreadingHTTPServer(("127.0.0.1", 0), Provider)
    provider.daemon_threads = True
    threading.Thread(target=provider.serve_forever, daemon=True).start()
    prefix = binary.stem
    ini = root / (prefix + ".ini")
    cache = root / (prefix + ".tsv")
    log_path = root / (prefix + ".log")
    ini.write_text("[api]\nendpoint=http://127.0.0.1:%d/v1/chat/completions\n"
                   "model=fake\nkey=fake\ntimeout_ms=3000\nconcurrency=1\n" % provider.server_port,
                   encoding="ascii")
    port = free_port()
    log = log_path.open("wb")
    process = subprocess.Popen([str(binary), "--port", str(port), "--cache", str(cache),
                                "--api-config", str(ini)], stdout=log, stderr=log,
                               creationflags=subprocess.CREATE_NO_WINDOW if os.name == "nt" else 0)
    try:
        deadline = time.monotonic() + 5
        while True:
            try:
                request(port, "/health", timeout=1)
                break
            except (OSError, http.client.HTTPException):
                if time.monotonic() >= deadline:
                    raise AssertionError(prefix + " failed to start")
                time.sleep(.05)

        with concurrent.futures.ThreadPoolExecutor(max_workers=1) as pool:
            start = time.monotonic()
            future = pool.submit(request, port, "/translate",
                                 {"text": SOURCE, "cache_only": False})
            assert arrived.wait(4), prefix + " did not call provider"
            try:
                status, body = future.result(timeout=11)
            finally:
                release.set()
            elapsed = time.monotonic() - start
        result = json.loads(body)
        assert status == 200 and result["translated_text"] == SOURCE and result["source"] == "miss", (
            prefix, status, body)
        # 原生 WinHTTP 可能尝试两次，每次 3 秒；C# 单次请求的响应体读取
        # 必须在自身截止时间后尽快结束，不能一直等待夹具解除阻塞。
        limit = 5.5 if prefix.endswith("_cs") else 11
        assert elapsed < limit, f"{prefix} held the provider stream for {elapsed:.2f}s (> {limit}s)"
        assert stalled_calls, prefix + " did not exercise stalled body"

        normal.set()
        status, body = request(port, "/translate", {"text": SOURCE, "cache_only": False})
        translated = json.loads(body)
        assert status == 200 and translated["translated_text"] == TRANSLATED, (
            prefix, status, body)
        status, body = request(port, "/cache/lookup", {"texts": [SOURCE]})
        lookup = json.loads(body)
        assert status == 200 and lookup["hits"].get(SOURCE) == TRANSLATED, (
            prefix, status, body)
        print(f"[PASS] {prefix}: stalled body ended in {elapsed:.2f}s; next request translated and cached")
    except Exception as error:
        log.flush()
        details = log_path.read_text(encoding="utf-8", errors="replace")
        raise AssertionError(f"{prefix}: {error}\n{details}") from error
    finally:
        release.set()
        if process.poll() is None:
            process.terminate()
        process.wait(timeout=5)
        log.close()
        provider.shutdown()
        provider.server_close()


def main():
    root = Path(tempfile.mkdtemp(prefix="dst_provider_body_"))
    try:
        for name in ("dst_server_cs.exe", "dst_server.exe"):
            check_server(ROOT / "native" / name, root)
    finally:
        # 所有文件均由本测试在同一个平面目录中创建。
        assert root.resolve().parent == Path(tempfile.gettempdir()).resolve()
        assert root.name.startswith("dst_provider_body_")
        for entry in root.iterdir():
            if entry.is_symlink() or not entry.is_file():
                raise RuntimeError(f"Unexpected fixture entry, retained: {entry}")
            entry.unlink()
        root.rmdir()


if __name__ == "__main__":
    main()
