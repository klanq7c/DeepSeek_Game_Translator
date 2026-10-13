"""使用隔离的本地故障服务测试正式 Godot HTTP 连接池。"""

import argparse
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import threading

from test_godot_scene_refresh import ROOT, pack


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, *_args):
        return

    def do_POST(self):
        batch = json.loads(self.rfile.read(int(self.headers["Content-Length"])))['texts']
        self.server.batches.append(batch)
        if batch[0].startswith("Stall response "):
            self.send_response(200)
            self.send_header("Content-Length", "100")
            self.end_headers()
            self.wfile.write(b"{")
            self.wfile.flush()
            self.server.release.wait(4)
            self.close_connection = True
            return
        query = batch[0]
        values = {
            "Valid response": "有效译文",
            "Recovery response": "恢复译文",
            "Paused menu": "暂停菜单",
            "Identity response": query,
            "Empty response": "",
            "Whitespace response": " \t\n ",
            "Null response": None,
            "Numeric response": 123,
            "Array response": ["invalid"],
            "Object response": {"invalid": "value"},
            "Truncated response": "不完整译文",
        }
        translations = {key: values.get(key) for key in batch}
        if query == "Invalid map":
            translations = "not a dictionary"
        elif query == "Missing map":
            translations = None
        payload = json.dumps({"translations": translations}, ensure_ascii=False).encode()
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(payload) + (64 if query == "Truncated response" else 0)))
        self.send_header("Connection", "close")
        self.end_headers()
        self.wfile.write(payload)
        self.wfile.flush()
        self.close_connection = True


def fixture(major, mode, port):
    source = (ROOT / f"payloads/Godot/dst_godot_runtime_g{major}.gd").read_text(encoding="utf-8")
    source = source.replace("http://127.0.0.1:19999/batch", f"http://127.0.0.1:{port}/batch")
    # 缩短生产代码的截止时间以快速触发真实超时，不替换请求或回调逻辑；
    # 未修复的组件没有超时设置，因此该测试会失败。
    source = re.sub(r"(?m)^const DST_HTTP_TIMEOUT_SECONDS = .+$",
                    "const DST_HTTP_TIMEOUT_SECONDS = 0.25", source)
    change = "change_scene" if major == 3 else "change_scene_to_file"
    scene_block = '\tvar scene = str(ProjectSettings.get_setting("application/run/main_scene"))\n\tif scene != "":\n\t\t' + change + '(scene)\n'
    if mode == "autoload":
        source = source.replace("extends SceneTree\n", "extends Node\n").replace("func _initialize():\n", "func _ready():\n")
        source = source.replace("\t\tquit(73)\n", "\t\tget_tree().quit(73)\n").replace("get_root()", "get_tree().root").replace(scene_block, "")
    runtime = 'get_node("/root/DeepSeekRuntime")' if mode == "autoload" else "get_tree()"
    now = "OS.get_ticks_msec()" if major == 3 else "Time.get_ticks_msec()"
    seed = '''extends Node
var runtime
var cases = ["Valid response", "Identity response", "Empty response", "Whitespace response", "Null response", "Numeric response", "Array response", "Object response", "Invalid map", "Missing map", "Truncated response"]
var index = -1
var started = 0
var failures = []
var awaiting = false
var diagnostic_before = 0
var paused_label
func _ready():
	PROCESS_MODE
	call_deferred("_begin")
func _begin():
	runtime = RUNTIME
	_next()
func _next():
	index += 1
	started = NOW
	if index < cases.size():
		var context = "batch-schema" if cases[index] in ["Invalid map", "Missing map"] else "batch-value"
		diagnostic_before = int(runtime._dst_error_counts.get(context, 0))
		runtime._dst_queue_text(cases[index])
	else:
		get_tree().paused = true
		paused_label = Label.new()
		paused_label.text = "Paused menu"
		add_child(paused_label)
		# 填满四个各容纳 48 条文本的槽，让恢复任务排在其后。
		for i in range(192):
			runtime._dst_queue_text("Stall response " + str(i))
		runtime._dst_queue_text("Recovery response")
	runtime._dst_pump()
	awaiting = true
func _process(_delta):
	if not awaiting:
		return
	var elapsed = NOW - started
	if runtime._dst_busy.size() > 0 or runtime._dst_pending.size() > 0 or (index >= cases.size() and paused_label.text != "暂停菜单"):
		if elapsed < 1800:
			return
		failures.append("HTTP pool stalled: busy=" + str(runtime._dst_busy.size()) + " pending=" + str(runtime._dst_pending.size()))
		_finish()
		return
	awaiting = false
	if index < cases.size():
		var query = cases[index]
		if query == "Valid response":
			if runtime._dst_cache.get(query, "") != "有效译文":
				failures.append("valid response lost")
			if runtime.RENDER("[b] Valid response [/b]") != "[b] 有效译文 [/b]":
				failures.append("adjacent BBCode or whitespace changed")
		else:
			if runtime._dst_cache.has(query):
				failures.append("invalid response cached: " + query + "=" + str(runtime._dst_cache[query]))
			if not runtime._dst_recent_miss(query):
				failures.append("invalid response has no retry backoff: " + query)
			if query in ["Null response", "Numeric response", "Array response", "Object response", "Invalid map", "Missing map"]:
				var context = "batch-schema" if query in ["Invalid map", "Missing map"] else "batch-value"
				if int(runtime._dst_error_counts.get(context, 0)) <= diagnostic_before:
					failures.append("invalid schema lacks diagnostic: " + query)
		_next()
	else:
		if runtime._dst_cache.get("Recovery response", "") != "恢复译文":
			failures.append("HTTP pool did not accept recovery work")
		for i in range(192):
			if runtime._dst_cache.has("Stall response " + str(i)):
				failures.append("stalled response cached")
		if not runtime._dst_error_counts.has("batch-http"):
			failures.append("transport timeout lacks diagnostic")
		_finish()
func _finish():
	awaiting = false
	if failures.size() == 0:
		print("HTTP_CONTRACT_PASS: strings, transport, schema, markup, four-slot timeout recovery")
	else:
		for failure in failures:
			print("HTTP_CONTRACT_FAIL: ", failure)
	get_tree().quit(0 if failures.size() == 0 else 1)
'''.replace("RUNTIME", runtime).replace("NOW", now).replace("RENDER", "_dst_render_cached" if major == 3 else "_dst_render").replace("PROCESS_MODE", "pause_mode = Node.PAUSE_MODE_PROCESS" if major == 3 else "process_mode = Node.PROCESS_MODE_ALWAYS")
    project = 'config_version=4\n[application]\nrun/main_scene="res://seed.tscn"\n[debug]\nfile_logging/enable_file_logging=false\n[rendering]\nrenderer/rendering_method="gl_compatibility"\nquality/driver/driver_name="GLES2"\n'
    if mode == "autoload":
        project += '[autoload]\nDeepSeekRuntime="*res://runtime.gd"\n'
    return {"runtime.gd": source, "project.godot": project, "seed.gd": seed,
            "seed.tscn": '[gd_scene load_steps=2 format=2]\n[ext_resource path="res://seed.gd" type="Script" id=1]\n[node name="Scene" type="Node"]\nscript = ExtResource( 1 )\n',
            ".godot/global_script_class_cache.cfg": "list=[]\n"}


def run(engine, major, mode):
    server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
    server.daemon_threads = True
    server.batches = []
    server.release = threading.Event()
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    try:
        with tempfile.TemporaryDirectory(prefix="dst_godot_http_") as folder:
            root = Path(folder)
            local_engine = root / "engine.exe"
            shutil.copy2(engine, local_engine)
            (root / "engine.pck").write_bytes(pack(fixture(major, mode, server.server_port), major))
            args = [str(local_engine), "--headless" if major == 4 else "--no-window", "--audio-driver", "Dummy"]
            if major == 4:
                args += ["--max-fps", "60"]
            if mode == "sidecar":
                args += ["--script", "res://runtime.gd"]
            try:
                result = subprocess.run(args, cwd=root, capture_output=True, timeout=15,
                                        creationflags=subprocess.CREATE_NO_WINDOW if os.name == "nt" else 0)
            except subprocess.TimeoutExpired as error:
                output = ((error.stdout or b"") + (error.stderr or b"")).decode("utf-8", "replace")
                raise AssertionError(f"Godot {major} {mode} timed out:\n{output}") from error
            output = (result.stdout + result.stderr).decode("utf-8", "replace")
            assert result.returncode == 0 and "HTTP_CONTRACT_PASS" in output and "SCRIPT ERROR" not in output, output
            stalled = [batch for batch in server.batches if batch[0].startswith("Stall response ")]
            assert len(stalled) == 4 and all(len(batch) == 48 for batch in stalled), stalled
            assert any("Recovery response" in batch for batch in server.batches), "recovery request never dispatched"
            assert any("Paused menu" in batch for batch in server.batches), "paused menu never scanned"
            print(f"[PASS] Godot {major} {mode}: valid/identity/empty, nonstring/map rejection, BBCode, truncated transfer, four stalled slots recover while paused")
    finally:
        server.release.set()
        server.shutdown()
        server.server_close()
        thread.join(timeout=2)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--godot3", default=os.environ.get("GODOT3_EXE"))
    parser.add_argument("--godot4", default=os.environ.get("GODOT4_EXE"))
    parser.add_argument("--mode", choices=("both", "autoload", "sidecar"), default="both")
    args = parser.parse_args()
    if not args.godot3 and not args.godot4:
        print("[SKIP] Godot HTTP runtime checks require GODOT3_EXE or GODOT4_EXE")
        return
    failures = []
    for major, engine in ((3, args.godot3), (4, args.godot4)):
        if not engine:
            continue
        for mode in (("autoload", "sidecar") if args.mode == "both" else (args.mode,)):
            try:
                run(Path(engine).resolve(strict=True), major, mode)
            except AssertionError as error:
                failures.append(f"Godot {major} {mode}: {error}")
    assert not failures, "\n".join(failures)


if __name__ == "__main__":
    main()
