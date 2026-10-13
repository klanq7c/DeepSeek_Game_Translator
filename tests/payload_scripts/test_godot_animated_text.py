"""真实 CanvasItem 重绘回归：缓存预热后的动画原文回写。"""
import argparse
import os
from pathlib import Path
import shutil
import subprocess
import tempfile

from test_godot_scene_refresh import ROOT, pack


def files_for(major, mode):
    source = (ROOT / f"payloads/Godot/dst_godot_runtime_g{major}.gd").read_text(encoding="utf-8")
    change = "change_scene" if major == 3 else "change_scene_to_file"
    scene_block = '\tvar scene = str(ProjectSettings.get_setting("application/run/main_scene"))\n\tif scene != "":\n\t\t' + change + '(scene)\n'
    if mode == "autoload":
        source = source.replace("extends SceneTree\n", "extends Node\n").replace("func _initialize():\n", "func _ready():\n")
        source = source.replace("\t\tquit(73)\n", "\t\tget_tree().quit(73)\n").replace("get_root()", "get_tree().root").replace(scene_block, "")
    # HTTP 请求不会到达用户服务；缓存未命中时仍保留真实队列状态。
    source = source.replace('var err = req.request(', 'var err = ERR_CANT_CONNECT # req.request(')
    runtime = "get_tree()" if mode == "sidecar" else 'get_node("/root/DeepSeekRuntime")'
    rich = "bbcode_text" if major == 3 else "text"
    server = "VisualServer" if major == 3 else "RenderingServer"
    connect_draw = 'node.connect("draw", self, "_draw_observe", [node])' if major == 3 else 'node.draw.connect(_draw_observe.bind(node))'
    connect_post = 'VisualServer.connect("frame_post_draw", self, "_observe")' if major == 3 else 'RenderingServer.frame_post_draw.connect(_observe)'
    driver = '''extends Node
var runtime
var frames = 0
var warm = false
var english_frames = 0
var drawings = {}
var failures = []
var expected = ""
func _ready():
	runtime = RUNTIME
	runtime._dst_cache_put("New Game", "新游戏")
	runtime._dst_cache_put("Settings", "设置")
	runtime._dst_cache_put("Exit Game", "退出游戏")
	$Rich.bbcode_enabled = true
	for node in [$Rich, $Text, $Items]:
		CONNECT_DRAW
	CONNECT_POST
	$Items.add_item("New Game")
	$Items.add_item("Settings")
	call_deferred("_begin")
func _begin():
	runtime._dst_scan()
	for node in get_tree().root.get_children():
		if node is Timer:
			node.stop()
	warm = true
func _process(_delta):
	if not warm:
		return
	var color = "red" if frames % 2 == 0 else "blue"
	var source = "[bgcolor=" + color + "][url=0] New Game[/url][/bgcolor]\\n[url=1] Settings[/url]"
	expected = source.replace("New Game", "新游戏").replace("Settings", "设置")
	$Rich.set("RICH", source)
	$Text.text = "Exit Game" if frames % 3 != 0 else "New Game"
	$Items.set_item_text(0, "New Game")
	$Items.set_item_text(1, "Settings")
	$Unknown.text = "Untranslated quest " + str(frames)
	if frames == 16:
		$Rich.hide()
	if frames == 18:
		$Rich.show()
	if frames == 20:
		get_tree().paused = true
	if frames == 22:
		get_tree().paused = false
func _draw_observe(node):
	if node == $Items:
		drawings[node.name] = node.get_item_text(0) + node.get_item_text(1)
	else:
		drawings[node.name] = str(node.get("RICH")) if node == $Rich else node.text
func _observe():
	if not warm:
		return
	frames += 1
	if frames > 3:
		var english = false
		for name in drawings:
			if drawings[name].find("New Game") >= 0 or drawings[name].find("Settings") >= 0 or drawings[name].find("Exit Game") >= 0:
				english = true
		if english:
			english_frames += 1
		if $Rich.is_visible_in_tree() and str($Rich.get("RICH")) != expected:
			failures.append("animation tags or live text changed")
		if not $Unknown.text.begins_with("Untranslated quest "):
			failures.append("unknown text frozen to old translation")
		for query in ["New Game", "Settings", "Exit Game"]:
			if runtime._dst_pending.has(query):
				failures.append("known animated text queued HTTP")
	if frames >= 60:
		if english_frames > 0:
			failures.append("cached English rendered in " + str(english_frames) + " frames")
		if drawings.size() != 3:
			failures.append("missing real CanvasItem draws")
		print("ANIMATION_RESULT frames=", frames, " english_frames=", english_frames, " failures=", failures)
		get_tree().quit(0 if failures.EMPTY() else 1)
'''.replace("RUNTIME", runtime).replace("RICH", rich).replace("CONNECT_DRAW", connect_draw).replace("CONNECT_POST", connect_post).replace("EMPTY", "empty" if major == 3 else "is_empty")
    driver = driver.replace("func _ready():\n", 'func _ready():\n\t' + ('pause_mode = Node.PAUSE_MODE_PROCESS' if major == 3 else 'process_mode = Node.PROCESS_MODE_ALWAYS') + '\n', 1)
    # Godot 4 的无头显示模式不会自动提交帧。
    driver += '\nfunc _force_frame():\n\t' + server + '.force_draw()\n'
    driver = driver.replace('warm = true\n', 'warm = true\n\t' + ('get_tree().connect("idle_frame", self, "_schedule_draw")' if major == 3 else 'get_tree().process_frame.connect(_schedule_draw)') + '\n')
    driver += '\nfunc _schedule_draw():\n\tcall_deferred("_force_frame")\n'
    nodes = '[gd_scene load_steps=2 format=2]\n[ext_resource path="res://driver.gd" type="Script" id=1]\n[node name="Scene" type="Node"]\nscript = ExtResource( 1 )\n'
    for name, kind in (("Rich", "RichTextLabel"), ("Text", "Label"), ("Items", "ItemList"), ("Unknown", "Label")):
        nodes += f'[node name="{name}" type="{kind}" parent="."]\n'
    project = 'config_version=4\n[application]\nrun/main_scene="res://main.tscn"\n[debug]\nfile_logging/enable_file_logging=false\n[rendering]\nrenderer/rendering_method="gl_compatibility"\nquality/driver/driver_name="GLES2"\n'
    if mode == "autoload":
        project += '[autoload]\nDeepSeekRuntime="*res://runtime.gd"\n'
    return {"runtime.gd": source, "project.godot": project, "driver.gd": driver,
            "main.tscn": nodes, ".godot/global_script_class_cache.cfg": "list=[]\n"}


def run(engine, major, mode, *, file_builder=None, success_marker="english_frames=0", description=None):
    root = Path(tempfile.mkdtemp(prefix="dst_godot_animation_"))
    try:
        local_engine = root / "engine.exe"
        shutil.copy2(engine, local_engine)
        (root / "engine.pck").write_bytes(pack((file_builder or files_for)(major, mode), major))
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
            raise AssertionError(f"Godot {major} {mode} runtime timed out:\n{output}") from error
        output = (result.stdout + result.stderr).decode("utf-8", "replace")
        if result.returncode != 0 or success_marker not in output or "SCRIPT ERROR" in output or "ERROR:" in output:
            raise AssertionError(f"Godot {major} {mode} runtime failed ({result.returncode}):\n{output}")
        description = description or "60 drawn frames, animated BBCode/options, pause, visibility, misses, no cache-hit HTTP"
        print(f"[PASS] Godot {major} {mode}: {description}")
    finally:
        assert root.resolve().parent == Path(tempfile.gettempdir()).resolve() and root.name.startswith("dst_godot_animation_")
        for entry in root.iterdir():
            if entry.is_symlink() or not entry.is_file():
                raise RuntimeError(f"Unexpected fixture entry retained: {entry}")
            entry.unlink()
        root.rmdir()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--godot3", default=os.environ.get("GODOT3_EXE"))
    parser.add_argument("--godot4", default=os.environ.get("GODOT4_EXE"))
    args = parser.parse_args()
    for major, engine in ((3, args.godot3), (4, args.godot4)):
        if engine:
            for mode in ("autoload", "sidecar"):
                run(Path(engine).resolve(strict=True), major, mode)
    if not args.godot3 and not args.godot4:
        print("[NOTE] animation draw tests require GODOT3_EXE/GODOT4_EXE")


if __name__ == "__main__":
    main()
