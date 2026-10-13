"""真实 Godot 场景生命周期回归：使用隔离包，不调用远程 API。"""

import argparse
import hashlib
import os
from pathlib import Path
import re
import shutil
import struct
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]


def check_sources():
    sources = [(ROOT / f"payloads/Godot/dst_godot_runtime_g{major}.gd").read_text(encoding="utf-8") for major in (3, 4)]
    for name in ("_dst_scene_prune", "_dst_scene_ready", "_dst_scene_flush",
                 "_dst_scene_frame_restore", "_dst_scene_restore_all", "_dst_scene_visible"):
        functions = [re.search(r"(?m)^func " + name + r"\([^\n]*\):\n(?:^[\t].*\n|^\n)*", source) for source in sources]
        assert all(functions), f"missing lifecycle function: {name}"
        assert functions[0].group() == functions[1].group(), f"Godot 3/4 lifecycle diverged: {name}"
    for source in sources:
        lifecycle = source[source.index("func _dst_scene_added"):source.index("func _dst_report_error")]
        for remote_work in ("_dst_queue_text(", "_dst_pump(", ".request(", "_dst_cache_put("):
            assert remote_work not in lifecycle, f"cache-only lifecycle unexpectedly performs {remote_work}"
        assert 'Engine.get_main_loop()' in source and ('"node_added"' in source or '.node_added.connect' in source)
        assert 'func _dst_scene_request_frame():' in lifecycle
        assert 'scene-frame-connect' in lifecycle
    assert '"idle_frame"' in sources[0] and 'CONNECT_ONESHOT' in sources[0]
    assert '.process_frame.connect' in sources[1] and 'CONNECT_ONE_SHOT' in sources[1]
    print("[PASS] Godot scene lifecycle: shared ordering/pruning, cache-only restoration")


def pack(files, major):
    paths = []
    for name, data in files.items():
        path = ("res://" + name).encode()
        path += bytes((len(path) + 4) // 4 * 4 - len(path))
        paths.append((path, data.encode()))
    header_size = 88 if major == 3 else 100
    header = bytearray(header_size)
    struct.pack_into("<5I", header, 0, 0x43504447, 1 if major == 3 else 2, major, 0, 0)
    data_start = header_size + sum(4 + len(path) + (32 if major == 3 else 36) for path, data in paths)
    struct.pack_into("<I", header, header_size - 4, len(paths))
    if major == 4:
        struct.pack_into("<Q", header, 24, data_start)
    directory = bytearray()
    offset = 0
    for path, data in paths:
        directory += struct.pack("<I", len(path)) + path
        directory += struct.pack("<QQ", offset + (data_start if major == 3 else 0), len(data))
        directory += hashlib.md5(data).digest() + (bytes(4) if major == 4 else b"")
        offset += len(data)
    return header + directory + b"".join(data for path, data in paths)


def files_for(major, mode):
    source = (ROOT / f"payloads/Godot/dst_godot_runtime_g{major}.gd").read_text(encoding="utf-8")
    change = "change_scene" if major == 3 else "change_scene_to_file"
    scene_block = '\tvar scene = str(ProjectSettings.get_setting("application/run/main_scene"))\n\tif scene != "":\n\t\t' + change + '(scene)\n'
    assert source.count(scene_block) == 1
    if mode == "autoload":
        source = source.replace("extends SceneTree\n", "extends Node\n").replace("func _initialize():\n", "func _ready():\n")
        source = source.replace("\t\tquit(73)\n", "\t\tget_tree().quit(73)\n").replace("get_root()", "get_tree().root").replace(scene_block, "")
    # 仅在外部 HTTP 边界禁用出站请求，保留真实队列状态，
    # 使测试能断言生命周期内的缓存命中不会将原文加入队列。
    source = source.replace('var err = req.request(', 'var err = ERR_CANT_CONNECT # req.request(')
    tree = "get_tree()" if mode == "sidecar" else 'get_node("/root/DeepSeekRuntime")'
    server = "VisualServer" if major == 3 else "RenderingServer"
    rich_prop = "bbcode_text" if major == 3 else "text"
    connect_post = 'VisualServer.connect("frame_post_draw", self, "_observe")' if major == 3 else 'RenderingServer.frame_post_draw.connect(_observe)'
    connect_draw = 'node.connect("draw", self, "_draw_observe", [node])' if major == 3 else 'node.draw.connect(_draw_observe.bind(node))'
    seed = '''extends Node
var runtime
var frames = 0
var failures = []
var draws = 0
var known = []
func _ready():
	call_deferred("_begin")
func _begin():
	runtime = RUNTIME
	if not get_tree().has_meta("reloaded"):
		runtime._dst_cache_put("Return to town", "返回城镇")
		runtime._dst_scan()
		if $Text.text != "返回城镇":
			print("FAIL: initial translation did not render; inside=", is_inside_tree(), " visible=", $Text.is_visible_in_tree(), " value=", $Text.text, " cache=", runtime._dst_cache.size())
			get_tree().quit(2)
			return
		for child in get_tree().root.get_children():
			if child is Timer:
				child.start()
		get_tree().set_meta("reloaded", true)
		get_tree().call_deferred("CHANGE", "res://next.tscn")
func _process(_delta):
	SERVER.force_draw()
'''.replace("RUNTIME", tree).replace("CHANGE", change).replace("SERVER", server)
    scene = '''extends Node
var runtime
var frames = 0
var failures = []
var draws = 0
var drawn_values = {}
var known = []
func _ready():
	runtime = RUNTIME
	known = [$Text, $Nested/ReadyText, $DeferredText, $Button, $Rich, $DeepDeferredText]
	$Nested/ReadyText.text = "Return to town"
	$DeferredText.set_deferred("text", "Return to town")
	call_deferred("_late_set")
	$Rich.bbcode_enabled = true
	$Rich.set("RICH_PROP", "[b]Return to town[/b]")
	for i in range(80):
		$Items.add_item("Return to town")
	for node in known:
		CONNECT_DRAW
	CONNECT_POST
func _late_set():
	$DeepDeferredText.set_deferred("text", "Return to town")
func _process(_delta):
	SERVER.force_draw()
func _draw_observe(node):
	draws += 1
	var value = str(node.get("RICH_PROP")) if node == $Rich else str(node.get("text"))
	# Godot 在提交一帧之前可能多次重建 CanvasItem。
	# 记录最终绘制命令，排除同帧中已被后续重建覆盖的中间结果。
	drawn_values[node.get_instance_id()] = value
func _observe():
	if frames >= 12:
		return
	frames += 1
	for node in known:
		var value = str(node.get("RICH_PROP")) if node == $Rich else str(node.get("text"))
		if value.find("返回城镇") < 0:
			failures.append("frame " + str(frames) + " original: " + str(node.name))
		if drawn_values.has(node.get_instance_id()) and str(drawn_values[node.get_instance_id()]).find("Return to town") >= 0:
			failures.append("frame " + str(frames) + " drew original: " + str(node.name))
	for i in range($Items.get_item_count()):
		if $Items.get_item_text(i) != "返回城镇":
			failures.append("frame " + str(frames) + " original item " + str(i))
	if $Unknown.text != "Untranslated quest":
		failures.append("unknown text changed")
	if runtime._dst_cache.has("Untranslated quest"):
		failures.append("unknown original entered cache")
	if runtime._dst_pending.has("Return to town"):
		failures.append("cache hit was queued")
	if str($Rich.get("RICH_PROP")) != "[b]返回城镇[/b]":
		failures.append("BBCode changed")
	if frames == 4:
		$Text.hide()
		$Text.text = "Return to town"
		$Text.show()
	if frames == 6:
		var old = $Button
		remove_child(old)
		old.free()
		var replacement = Button.new()
		replacement.name = "Button"
		replacement.text = "Return to town"
		add_child(replacement)
		known[3] = replacement
	if frames == 8:
		var reused = $Text
		remove_child(reused)
		reused.text = "Return to town"
		add_child(reused)
	if frames >= 12:
		if draws == 0:
			failures.append("no CanvasItem draw observed")
		if failures.empty():
			print("SCENE_REFRESH_PASS frames=", frames, " draws=", draws)
		else:
			print("SCENE_REFRESH_FAIL count=", failures.size(), " first=", failures[0])
		get_tree().quit(0 if failures.empty() else 1)
'''.replace("RUNTIME", tree).replace("RICH_PROP", rich_prop).replace("CONNECT_DRAW", connect_draw).replace("CONNECT_POST", connect_post).replace("SERVER", server)
    if major == 4:
        scene = scene.replace("failures.empty()", "failures.is_empty()")
    tscn_header = '[gd_scene load_steps=2 format=2]\n[ext_resource path="res://%s.gd" type="Script" id=1]\n[node name="Scene" type="Node"]\nscript = ExtResource( 1 )\n'
    nodes = '[node name="Text" type="Label" parent="."]\ntext = "Return to town"\n'
    next_nodes = nodes + '[node name="Nested" type="Control" parent="."]\n[node name="ReadyText" type="Label" parent="Nested"]\n[node name="DeferredText" type="Label" parent="."]\n[node name="DeepDeferredText" type="Label" parent="."]\ntext = "Return to town"\n[node name="Button" type="Button" parent="."]\ntext = "Return to town"\n[node name="Rich" type="RichTextLabel" parent="."]\n[node name="Items" type="ItemList" parent="."]\n[node name="Unknown" type="Label" parent="."]\ntext = "Untranslated quest"\n'
    project = 'config_version=4\n[application]\nrun/main_scene="res://seed.tscn"\n[debug]\nfile_logging/enable_file_logging=false\n[rendering]\nrenderer/rendering_method="gl_compatibility"\nquality/driver/driver_name="GLES2"\n'
    if mode == "autoload":
        project += '[autoload]\nDeepSeekRuntime="*res://runtime.gd"\n'
    return {"runtime.gd": source, "project.godot": project, "seed.gd": seed, "next.gd": scene,
            "seed.tscn": tscn_header % "seed" + nodes, "next.tscn": tscn_header % "next" + next_nodes,
            ".godot/global_script_class_cache.cfg": "list=[]\n"}


def run(engine, major, mode):
    root = Path(tempfile.mkdtemp(prefix="dst_godot_scene_"))
    try:
        local_engine = root / "engine.exe"
        shutil.copy2(engine, local_engine)
        for mode in (("autoload", "sidecar") if mode == "both" else (mode,)):
            (root / "engine.pck").write_bytes(pack(files_for(major, mode), major))
            args = [str(local_engine), "--headless" if major == 4 else "--no-window", "--audio-driver", "Dummy"]
            if major == 4:
                args += ["--max-fps", "60"]
            if mode == "sidecar":
                args += ["--script", "res://runtime.gd"]
            try:
                result = subprocess.run(args, cwd=root, capture_output=True, timeout=20,
                                        creationflags=subprocess.CREATE_NO_WINDOW if os.name == "nt" else 0)
            except subprocess.TimeoutExpired as error:
                output = ((error.stdout or b"") + (error.stderr or b"")).decode("utf-8", "replace")
                raise AssertionError(f"Godot {major} {mode} timed out:\n{output}") from error
            output = (result.stdout + result.stderr).decode("utf-8", "replace")
            if result.returncode != 0 or "SCENE_REFRESH_PASS" not in output or "SCRIPT ERROR" in output or "ERROR:" in output:
                raise AssertionError(f"Godot {major} {mode} failed ({result.returncode}):\n{output}")
            print(f"[PASS] Godot {major} {mode}: known text before first draw, nested deferred assignments, 80 items, reuse, freed nodes, pending isolation")
    finally:
        # 此平面夹具由测试拥有；出现非预期子目录或链接时拒绝清理。
        assert root.resolve().parent == Path(tempfile.gettempdir()).resolve()
        assert root.name.startswith("dst_godot_scene_")
        for entry in root.iterdir():
            if entry.is_symlink() or not entry.is_file():
                raise RuntimeError(f"Unexpected fixture entry, retained: {entry}")
            entry.unlink()
        root.rmdir()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--godot3", default=os.environ.get("GODOT3_EXE"))
    parser.add_argument("--godot4", default=os.environ.get("GODOT4_EXE"))
    parser.add_argument("--mode", choices=("both", "autoload", "sidecar"), default="both")
    parser.add_argument("--check-source", action="store_true")
    args = parser.parse_args()
    if args.check_source:
        check_sources()
    if not args.godot3 and not args.godot4 and not args.check_source:
        parser.error("provide --godot3/--godot4 or GODOT3_EXE/GODOT4_EXE")
    for major, engine in ((3, args.godot3), (4, args.godot4)):
        if engine:
            run(Path(engine).resolve(strict=True), major, args.mode)


if __name__ == "__main__":
    main()
