"""真实 Godot 运行时：关联的选择器文本片段、缓存未命中及延迟完成。"""
import argparse
import os
from pathlib import Path
import re
from test_godot_scene_refresh import ROOT
from test_godot_animated_text import files_for as animation_files, run as animation_run


def files_for(major, mode):
    files = animation_files(major, mode)
    # 计数只注入隔离测试包，验证真实控件调用链；发布脚本不增加运行时计数。
    anchor = 'func _dst_text_plan(source):\n'
    assert files['runtime.gd'].count(anchor) == 1
    files['runtime.gd'] = files['runtime.gd'].replace(
        anchor, 'var plan_parse_count = 0\n' + anchor + '\tplan_parse_count += 1\n')
    runtime = "get_tree()" if mode == "sidecar" else 'get_node("/root/DeepSeekRuntime")'
    rich = "bbcode_text" if major == 3 else "text"
    connect_draw = '$Rich.connect("draw", self, "_draw_observe")' if major == 3 else '$Rich.draw.connect(_draw_observe)'
    connect_post = 'VisualServer.connect("frame_post_draw", self, "_observe")' if major == 3 else 'RenderingServer.frame_post_draw.connect(_observe)'
    frame = 'get_tree().connect("idle_frame", self, "_schedule_draw")' if major == 3 else 'get_tree().process_frame.connect(_schedule_draw)'
    server = 'VisualServer' if major == 3 else 'RenderingServer'
    driver = '''extends Node
var runtime
var frames = 0
var warm = false
var failures = []
var last_draw = ""
var original = ""
var expected = ""
func _ready():
	PROCESS
	runtime = RUNTIME
	var hits = {"Category":"类别", "General":"通用", "Language":"语言", "English":"英语", "Smooth Animations":"流畅动画", "Off":"关闭", "Save":"保存", "Return":"返回"}
	for key in hits:
		runtime._dst_cache_put(key, hits[key])
	# 旧半词命中不能遮挡完整词；模拟真实缓存里的错误半词译文，不改用户缓存。
	runtime._dst_cache_put("< Gen", "< 将军 >")
	runtime._dst_cache_put("eral >", "< 工程")
	runtime._dst_cache_put("Very Low Quality", "极低质量")
	runtime._dst_cache_put("Left: Interact/Select", "左键：交互/选择")
	runtime._dst_cache_put("Right: Move/Attack", "右键：移动/攻击")
	runtime._dst_cache_put("0x0", "错误数字译文")
	$Rich.bbcode_enabled = true
	CONNECT_DRAW
	CONNECT_POST
	call_deferred("_begin")
func _begin():
	for node in get_tree().root.get_children():
		if node is Timer:
			node.stop()
	_check_single_plan()
	warm = true
	FRAME
func _check_single_plan():
	runtime._dst_cache_put("Plan cache hit", "计划命中")
	$Text.text = "Plan cache hit"
	runtime.SCAN_COUNT = 0
	runtime.plan_parse_count = 0
	runtime.APPLY_PROPERTY($Text, "text")
	if runtime.plan_parse_count != 1 or $Text.text != "计划命中":
		failures.append("label plan count=" + str(runtime.plan_parse_count))
	var source = "[url=l] < Gen[/url][url=r]eral >[/url] [url=miss]Plan queued rich option[/url]"
	$Rich.set("RICH", source)
	runtime.SCAN_COUNT = 0
	runtime.plan_parse_count = 0
	runtime.APPLY_PROPERTY($Rich, "RICH")
	if runtime.plan_parse_count != 1:
		failures.append("rich plan count=" + str(runtime.plan_parse_count))
	if $Rich.get("RICH") != source.replace(" < Gen", " < 通").replace("eral >", "用 >"):
		failures.append("shared plan damaged partial-hit BBCode")
	if not runtime._dst_pending.has("Plan queued rich option") or runtime._dst_pending.has("通用"):
		failures.append("shared plan queued rendered text instead of original")
	$Items.clear()
	$Items.add_item("Plan cache hit")
	$Items.add_item("Plan queued item option")
	runtime.SCAN_COUNT = 0
	runtime.plan_parse_count = 0
	runtime._dst_apply_items($Items)
	if runtime.plan_parse_count != 2 or $Items.get_item_text(0) != "计划命中":
		failures.append("item plan count=" + str(runtime.plan_parse_count))
	if not runtime._dst_pending.has("Plan queued item option") or runtime._dst_cache.has("Plan queued item option"):
		failures.append("shared item plan lost miss or published untranslated cache")
func _process(_delta):
	if not warm:
		return
	var color = "black" if frames % 2 == 0 else "white"
	original = "[color=" + color + "][url=0]Category[/url][url=0-l] < Gen[/url][url=0-r]eral >[/url]\\n[url=2]Language[/url][url=2-l] < Eng[/url][url=2-r]lish >[/url]\\n[url=3]Smooth Animations[/url][url=3-l] < O[/url][url=3-r]ff >[/url]\\n[url=5]Save[/url] [url=6]Return[/url]\\n[url=7]Untranslated new option[/url][/color]"
	expected = original.replace("Category", "类别").replace(" < Gen", " < 通").replace("eral >", "用 >").replace("Language", "语言").replace(" < Eng", " < 英").replace("lish >", "语 >").replace("Smooth Animations", "流畅动画").replace(" < O", " < 关").replace("ff >", "闭 >").replace("Save", "保存").replace("Return", "返回")
	if frames >= 30:
		expected = expected.replace("Untranslated new option", "新选项")
	$Rich.set("RICH", original)
	# 真正调用发现路径：部分命中之后，未命中的余项也必须进入原有异步队列。
	runtime._dst_scan()
func _draw_observe():
	last_draw = str($Rich.get("RICH"))
func _observe():
	if not warm:
		return
	if frames >= 3 and last_draw != expected:
		failures.append("frame " + str(frames) + " settings mismatched: " + last_draw)
	if frames >= 3:
		for key in ["< Gen", "eral >", "< Eng", "lish >", "< O", "ff >"]:
			if runtime._dst_pending.has(key):
				failures.append("fragment queued: " + key)
		if frames < 30 and not runtime._dst_miss_until.has("Untranslated new option"):
			failures.append("partial-hit miss was not discovered")
		if runtime._dst_cache.has("Untranslated new option") and runtime._dst_cache["Untranslated new option"] == "Untranslated new option":
			failures.append("miss became successful cache entry")
	frames += 1
	if frames == 30:
		runtime._dst_cache_put("Untranslated new option", "新选项")
	if frames >= 60:
		# 相邻链接具有不同语义，空格和标签必须原样保留；路径不应入队。
		var adjacent = "[url=a]Save[/url] [url=b]Return[/url]"
		if runtime.RENDER(adjacent) != "[url=a]保存[/url] [url=b]返回[/url]":
			failures.append("adjacent independent links merged")
		if runtime.RENDER("[url=a]Save[/url][url=b]Return[/url]") != "[url=a]保存[/url][url=b]返回[/url]":
			failures.append("adjacent links without whitespace merged")
		if runtime.RENDER("Gen[color=red]eral[/color]") != "通[color=red]用[/color]":
			failures.append("inline word style lost")
		if runtime.RENDER("[url=l] < [color=red]Gen[/color][/url][url=r]eral >[/url]") != "[url=l] < [color=red]通[/color][/url][url=r]用 >[/url]":
			failures.append("styled picker arrows or link targets changed")
		if runtime.RENDER("Gen[img]res://icon.png[/img]eral") != "":
			failures.append("image barrier merged independent words")
		if runtime.RENDER("[url=l] < Very Low[/url][url=r] Quality >[/url]") != "[url=l] < 极低[/url][url=r]质量 >[/url]":
			failures.append("multiword picker split at whitespace")
		var numeric = "[url=l] < 0[/url][url=r]x0 >[/url]"
		if runtime._dst_wanted(numeric) or runtime.RENDER(numeric) != "":
			failures.append("numeric picker translated")
		for label in ["Left: Interact/Select", "Right: Move/Attack"]:
			if runtime.RENDER(label) != runtime._dst_cache[label]:
				failures.append("slash label treated as path: " + label)
		for path in ["res://folder/file.tres", "user://save.json", "C:/Games/config.ini", "folder/file.tres", "folder with spaces/file.tres", "https://example.com/game", "/folder/path", "../folder/path"]:
			if runtime._dst_wanted(path):
				failures.append("path accepted")
		print("SETTINGS_RESULT frames=", frames, " failures=", failures)
		if failures.EMPTY():
			print("SETTINGS_OK")
		get_tree().quit(0 if failures.EMPTY() else 1)
func _schedule_draw():
	call_deferred("_force_frame")
func _force_frame():
	SERVER.force_draw()
'''.replace('PROCESS', 'pause_mode = Node.PAUSE_MODE_PROCESS' if major == 3 else 'process_mode = Node.PROCESS_MODE_ALWAYS').replace('RUNTIME', runtime).replace('RICH', rich).replace('CONNECT_DRAW', connect_draw).replace('CONNECT_POST', connect_post).replace('FRAME', frame).replace('SERVER', server).replace('EMPTY', 'empty' if major == 3 else 'is_empty').replace('RENDER', '_dst_render_cached' if major == 3 else '_dst_render')
    driver = driver.replace('SCAN_COUNT', '_dst_seen_this_scan' if major == 3 else '_dst_seen')
    driver = driver.replace('APPLY_PROPERTY', '_dst_apply_property' if major == 3 else '_dst_apply')
    files['driver.gd'] = driver
    return files


def check_sources():
    functions = ('_dst_text_plan', '_dst_inline_tag', '_dst_picker_end', '_dst_latin_edge', '_dst_query', '_dst_render_plan', '_dst_render_groups', '_dst_plan_wanted', '_dst_path_text', '_dst_numeric_text')
    implementations = []
    for major in (3, 4):
        source = (ROOT / f'payloads/Godot/dst_godot_runtime_g{major}.gd').read_text(encoding='utf-8')
        bodies = []
        for name in functions:
            match = re.search(r'^func ' + name + r'\(.*?(?=^func |\Z)', source, re.M | re.S)
            assert match, f'Godot {major} missing {name}'
            body = match.group(0)
            assert not any(call in body for call in ('req.request(', '_dst_cache_put(', '_dst_queue_text(', '_dst_queue_plan(')), f'{name} must only plan or render existing cache'
            bodies.append(body)
        implementations.append(bodies)
    assert implementations[0] == implementations[1], 'Godot 3/4 logical text renderers diverged'
    print('[PASS] Godot 3/4 logical text grouping and renderer parity; no render HTTP or cache writes')


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--godot3', default=os.environ.get('GODOT3_EXE'))
    parser.add_argument('--godot4', default=os.environ.get('GODOT4_EXE'))
    args = parser.parse_args()
    check_sources()
    for major, engine in ((3, args.godot3), (4, args.godot4)):
        if engine:
            for mode in ('autoload', 'sidecar'):
                animation_run(Path(engine).resolve(strict=True), major, mode, file_builder=files_for,
                              success_marker='SETTINGS_OK',
                              description='60 settings draws, linked words, partial hits, late completion, markup and path guards')
    if not args.godot3 and not args.godot4:
        print('[NOTE] settings runtime tests require GODOT3_EXE/GODOT4_EXE')


if __name__ == '__main__':
    main()
