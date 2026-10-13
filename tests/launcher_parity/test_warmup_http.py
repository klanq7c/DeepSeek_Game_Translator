"""验证 C/C# 生产预热流程中的真实 HTTP 与文件写入行为。"""
import argparse
import json
import subprocess
import threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path


def encode_json(value):
    return json.dumps(value, ensure_ascii=True, separators=(",", ":")).encode("utf-8")


class Server(ThreadingHTTPServer):
    daemon_threads = True
    allow_reuse_address = False

    def __init__(self):
        super().__init__(("127.0.0.1", 19999), Handler)
        self.requests = []
        self.hits = {}
        self.mode = "normal"
        self.disconnects = 0


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, *_args):
        # 状态和请求体记录在 server.requests 中，供断言使用。
        return

    def handle_one_request(self):
        try:
            super().handle_one_request()
        except (ConnectionResetError, ConnectionAbortedError):
            # WinHTTP 会关闭尚未读完的 POST 确认响应和过大响应。
            self.server.disconnects += 1
            self.close_connection = True

    def do_GET(self):
        self.server.requests.append(("GET", self.path, b"", None))
        self.reply(200, b'{"status":"ok"}')

    def do_POST(self):
        body = self.rfile.read(int(self.headers.get("Content-Length", "0")))
        data = json.loads(body)
        self.server.requests.append(("POST", self.path, body, data))
        if self.path != "/cache/lookup":
            self.reply(200, b'{"accepted":true}')
            return
        mode = self.server.mode
        if mode == "reject":
            self.reply(503, b"Never write this error page.")
        elif mode == "truncated":
            self.send_response(200)
            self.send_header("Content-Length", "100000")
            self.send_header("Connection", "close")
            self.end_headers()
            self.wfile.write(b'{"hits":{"Fault dialogue.":"Bad partial translation."}')
            self.close_connection = True
        elif mode == "malformed":
            self.reply(200, b'{"hits":{"Fault dialogue.":"Never write this."},"hit_count":1,"miss_count":0}garbage')
        elif mode == "invalid_utf8":
            self.reply(200, b'{"hits":{"Fault dialogue.":"Bad \xff"},"hit_count":1,"miss_count":0}')
        elif mode == "invalid_escape":
            self.reply(200, b'{"hits":{"Fault dialogue.":"Bad \\q"},"hit_count":1,"miss_count":0}')
        elif mode == "nul":
            self.reply(200, b'{"hits":{"Fault dialogue.":"Bad\\u0000translation"},"hit_count":1,"miss_count":0}')
        elif mode == "numeric":
            self.reply(200, b'{"hits":{"Numeric dialogue.":"Valid numeric response."},"hit_count":1.0,"miss_count":-0e0}')
        elif mode == "duplicate":
            self.reply(200, b'{"hits":{"Repeated dialogue.":"Earlier value."},"hit_count":1,"hits":{"Repeated dialogue.":"Later value.","Repeated dialogue.":"Last value."},"miss_count":0}')
        else:
            lookup_number = sum(r[1] == "/cache/lookup" for r in self.server.requests)
            suffix = str(lookup_number) if mode == "changing" else ""
            # 真实服务器为每个请求文本输出一个 hits 属性，包含重复键。
            hits = [(key, self.server.hits[key] + suffix) for key in data["texts"] if key in self.server.hits]
            body = b'{"hits":{' + b','.join(encode_json(k) + b':' + encode_json(v) for k, v in hits)
            body += b'},"hit_count":' + str(len(hits)).encode() + b',"miss_count":' + str(len(data["texts"]) - len(hits)).encode() + b'}'
            self.reply(200, body)

    def reply(self, status, body):
        self.send_response(status)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        try:
            self.wfile.write(body)
        except (BrokenPipeError, ConnectionResetError, ConnectionAbortedError):
            # 过大响应测试会在 EOF 之前主动停止读取。
            self.server.disconnects += 1


def xescape(value):
    return value.replace("\\", "\\\\").replace("=", "\\=").replace("\n", "\\n").replace("\r", "\\r").replace("\t", "\\t")


def run_case(server, exe, root, name, original, expected=None, hits=None, mode="normal", engine=3, backup=None, dump=False, fault=0):
    game = root / name
    path = game / "BepInEx/Translation/zh-CN/Text/dialogue.txt"
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(original)
    bak = path.with_name(path.name + ".deepseek.bak")
    if backup is not None:
        bak.write_bytes(backup)
    server.requests.clear()
    server.hits = hits or {}
    server.mode = mode
    extra = ["dump"] if dump else ([f"fault={fault}"] if fault else [])
    proc = subprocess.run([str(exe), str(game), str(engine)] + extra, capture_output=True, timeout=100)
    assert proc.returncode == 0, (name, proc.returncode, proc.stdout, proc.stderr)
    expected = original if expected is None else expected
    assert path.read_bytes() == expected, (name, "translation-file bytes")
    if expected != original:
        assert bak.read_bytes() == (original if backup is None else backup), (name, "backup bytes")
    elif backup is None:
        assert not bak.exists(), (name, "unexpected backup")
    requests = list(server.requests)
    if fault:
        assert b"allocation_faults=1" in proc.stdout, (name, "fault was not injected")
        assert b"cache lookup failed" in proc.stdout, (name, "missing allocation diagnostic")
        assert not any(r[1] == "/cache/import" for r in requests), (name, "allocation fault imported damaged text")
    lookups = [r for r in requests if r[1] == "/cache/lookup"]
    for method, route, body, data in requests:
        assert not route.startswith("/translate"), (name, "individual GET lookup returned")
        if route == "/cache/lookup":
            assert method == "POST" and len(data["texts"]) <= 128 and len(body) <= 65536, (name, "lookup budget")
    if mode in ("reject", "truncated", "malformed", "invalid_utf8", "invalid_escape", "nul"):
        assert len(lookups) == 1, (name, "HTTP/JSON fault was retried")
        assert b"cache lookup failed" in proc.stdout, (name, "missing failure diagnostic")
        assert all(r[1] != "/cache/import" for r in requests), (name, "fault imported as translation")
    return requests, proc.stdout


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--c", required=True, type=Path)
    parser.add_argument("--cs", required=True, type=Path)
    parser.add_argument("--root", required=True, type=Path)
    args = parser.parse_args()
    try:
        server = Server()
    except OSError as exc:
        if exc.errno in (10048, 10013, 98) or getattr(exc, "winerror", None) in (10048, 10013):
            print("[skip] Unity warmup HTTP guards: port 19999 is occupied; existing service left running.")
            return
        raise
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    snapshots = []
    checks = 0
    try:
        for label, exe in (("c", args.c), ("cs", args.cs)):
            root = args.root / label
            result = []
            original = b'\xef\xbb\xbf# Keep comment\r\n  Welcome, traveler.=  \r\nMissing dialogue.=\nIdentity dialogue.=Identity dialogue.\rAlready done.=Existing translation.\n<color\\=red>Hello</color>\\nNext\\tline\\=yes=\r\n'
            hits = {"Welcome, traveler.": "你好，旅人。", "Identity dialogue.": "Identity dialogue.", "<color=red>Hello</color>\nNext\tline=yes": "<color=red>你好</color>\n下一行\t=是\\"}
            expected = original.replace(b"  Welcome, traveler.=  ", "Welcome, traveler.=你好，旅人。".encode()).replace(b"<color\\=red>Hello</color>\\nNext\\tline\\=yes=", (xescape("<color=red>Hello</color>\nNext\tline=yes") + "=" + xescape(hits["<color=red>Hello</color>\nNext\tline=yes"])).encode())
            requests, _ = run_case(server, exe, root, "hits", original, expected, hits)
            assert len([r for r in requests if r[1] == "/cache/lookup"]) == 1
            imported = [e for r in requests if r[1] == "/cache/import" for e in r[3]["entries"]]
            assert [e["key"] for e in imported] == ["Welcome, traveler.", "Already done.", "<color=red>Hello</color>\nNext\tline=yes"]
            assert all(e["key"] != e["value"] for e in imported)
            result.append(requests)
            checks += 1
            requests, _ = run_case(server, exe, root, "backup", b"Welcome, traveler.=", b"Welcome, traveler.=New translation.", {"Welcome, traveler.": "New translation."}, backup=b"Original backup stays.")
            result.append(requests)
            checks += 1
            for mode in ("reject", "truncated", "malformed", "invalid_utf8", "invalid_escape", "nul"):
                requests, _ = run_case(server, exe, root, "fault_" + mode, b"Fault dialogue.=\r\n", mode=mode)
                result.append(requests)
                checks += 1
            requests, _ = run_case(server, exe, root, "numeric", b"Numeric dialogue.=", b"Numeric dialogue.=Valid numeric response.", mode="numeric")
            result.append(requests)
            checks += 1
            requests, _ = run_case(server, exe, root, "duplicate", b"Repeated dialogue.=\nRepeated dialogue.=", b"Repeated dialogue.=Last value.\nRepeated dialogue.=Last value.", mode="duplicate")
            result.append(requests)
            checks += 1
            for count in (128, 129, 8001):
                keys = [f"Dialogue number {i:05d}." for i in range(count)]
                original = "".join(key + "=\n" for key in keys).encode()
                requests, _ = run_case(server, exe, root, f"count{count}", original)
                lookups = [r for r in requests if r[1] == "/cache/lookup"]
                assert len(lookups) == (count + 127) // 128
                assert [key for r in lookups for key in r[3]["texts"]] == keys
                prefetched = [key for r in requests if r[1] == "/prefetch" for key in r[3]["texts"]]
                assert set(prefetched) == set(keys), "misses above 8000 must retain GET's enqueue semantics"
                assert len(prefetched) == count + min(count, 8000)
                result.append(requests)
                checks += 1
            keys = [f"Long dialogue {i:03d} " + "word " * 233 for i in range(140)]
            assert all(len(key) <= 1200 for key in keys)
            requests, _ = run_case(server, exe, root, "byte_budget", "".join(key + "=\n" for key in keys).encode())
            lookups = [r for r in requests if r[1] == "/cache/lookup"]
            assert len(lookups) > 2 and all(len(r[3]["texts"]) < 128 for r in lookups)
            assert [key for r in lookups for key in r[3]["texts"]] == keys
            result.append(requests)
            checks += 1
            # 即使用每字符六字节的 JSON 转义，最长键仍应符合首条数据的字节预算。
            max_key = "A" + "\x01" * 1198 + "Z"
            requests, _ = run_case(server, exe, root, "single_key_budget", (max_key + "=\n").encode())
            lookups = [r for r in requests if r[1] == "/cache/lookup"]
            assert len(lookups) == 1 and len(lookups[0][2]) < 7216 and lookups[0][3]["texts"] == [max_key]
            result.append(requests)
            checks += 1
            large = "Large translation " + "x" * (1100 * 1024)
            original = b"Large first dialogue.=\r\nLarge second dialogue.=\n"
            expected = ("Large first dialogue.=" + large + "\r\nLarge second dialogue.=" + large + "\n").encode()
            requests, output = run_case(server, exe, root, "large_split", original, expected, {"Large first dialogue.": large, "Large second dialogue.": large})
            assert len([r for r in requests if r[1] == "/cache/lookup"]) == 3
            assert b"splitting lookup batch" in output and b"cache lookup failed" not in output
            result.append(requests)
            checks += 1
            original = b"Repeated large dialogue.=\nOther large dialogue.=\nRepeated large dialogue.=\n"
            expected = ("Repeated large dialogue.=" + large + "2\nOther large dialogue.=" + large + "4\nRepeated large dialogue.=" + large + "5\n").encode()
            requests, _ = run_case(server, exe, root, "changing_split", original, expected, {"Repeated large dialogue.": large, "Other large dialogue.": large}, mode="changing")
            assert len([r for r in requests if r[1] == "/cache/lookup"]) == 5
            result.append(requests)
            checks += 1
            large = "Single large translation " + "y" * (3 * 1024 * 1024)
            requests, _ = run_case(server, exe, root, "large_single", b"Single large dialogue.=", ("Single large dialogue.=" + large).encode(), {"Single large dialogue.": large})
            assert len([r for r in requests if r[1] == "/cache/lookup"]) == 1
            result.append(requests)
            checks += 1
            requests, output = run_case(server, exe, root, "dump", b"Welcome, traveler.=\r\n", hits={"Welcome, traveler.": "Never write during dump."}, dump=True)
            assert not requests and b"post=/prefetch" in output
            checks += 1
            # 相邻引擎守卫：RPG Maker 不得修改看起来像 XUnity 翻译文件的同名文件。
            requests, _ = run_case(server, exe, root, "adjacent_rpgm", b"Welcome, traveler.=\r\n", hits={"Welcome, traveler.": "Never write in RPG Maker."}, engine=2)
            assert not requests
            checks += 1
            snapshots.append(result)
        assert snapshots[0] == snapshots[1], "C/C# HTTP request sequence or raw body differs"
        original = b"Fault dialogue.=\r\n"
        for name, size, value in (("parse", 128, "x" * 200), ("parse_unicode", 128, "译" * 100), ("receive", 8192, "x" * 9000),
                                  ("output", 2 * (len(original) + 256), "x" * 500)):
            run_case(server, args.c, args.root / "allocation", name, original,
                     hits={"Fault dialogue.": value}, fault=size)
            checks += 1
        run_case(server, args.c, args.root / "allocation", "import", b"Existing dialogue.=" + b"x" * 9000 + b"\n", fault=8192)
        long_keys = [f"Long dialogue {i:03d} " + "word " * 200 for i in range(10)]
        run_case(server, args.c, args.root / "allocation", "request", "".join(k + "=\n" for k in long_keys).encode(), fault=8192)
        checks += 2
        print(f"[pass] Unity warmup HTTP: {checks} scenarios plus exact C/C# HTTP body parity; bounded lookup, enqueue >8000, writeback, backup, faults, large values, dump, adjacent engine.")
    finally:
        server.shutdown()
        server.server_close()
        thread.join(timeout=5)


if __name__ == "__main__":
    main()
