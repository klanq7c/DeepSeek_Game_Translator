extends SceneTree

const DST_URL = "http://127.0.0.1:19999/batch"
const DST_MAX_TEXT = 1200
const DST_QUEUE_LIMIT = 1536
const DST_SCAN_LIMIT = 900
const DST_BATCH_SIZE = 48
const DST_MAX_CACHE = 4096
const DST_MAX_MISS = 4096
const DST_MAX_TRACKED_CONTROLS = 4096
const DST_TRACKED_CONTROL_SLICE = 256
const DST_FALLBACK_NODE_SLICE = 256
const DST_ITEMS_PER_NODE_SLICE = 64
const DST_FULL_SCAN_EVERY = 25
const DST_MISS_BACKOFF_MS = 8000
const DST_FONT_PATHS = ["C:/Windows/Fonts/simhei.ttf", "C:/Windows/Fonts/msyh.ttf", "C:/Windows/Fonts/simsun.ttc"]

var _dst_cache = {}
var _dst_cache_order = []
var _dst_miss_until = {}
var _dst_miss_order = []
var _dst_pending = {}
var _dst_queue = []
var _dst_queue_head = 0
var _dst_busy = {}
var _dst_requests = []
var _dst_font = null
var _dst_font_warned = false
var _dst_seen = 0
var _dst_controls = {}
var _dst_control_order = []
var _dst_control_cursor = -1
var _dst_scan_frontier = []
var _dst_full_scan_tick = 0
var _dst_error_counts = {}

func _dst_report_error(context, detail):
	var count = int(_dst_error_counts.get(context, 0)) + 1
	_dst_error_counts[context] = count
	if count <= 3 or (count & (count - 1)) == 0:
		push_warning("[DeepSeek Godot] %s failed #%d: %s" % [context, count, str(detail)])

func _initialize():
	if OS.get_cmdline_args().has("--dst-preflight") or OS.get_cmdline_user_args().has("--dst-preflight"):
		quit(0)
		return
	call_deferred("_dst_start")

func _dst_start():
	get_root().get_tree().node_added.connect(Callable(self, "_dst_track_control"))
	# 初始场景也进入节点预算队列，避免启动时递归遍历整棵树。
	_dst_scan_frontier.append(get_root())
	for i in range(4):
		var req = HTTPRequest.new()
		req.process_mode = Node.PROCESS_MODE_ALWAYS
		get_root().add_child(req)
		_dst_requests.append(req)
		req.request_completed.connect(Callable(self, "_dst_done").bind(req.get_instance_id()))
	var timer = Timer.new()
	timer.process_mode = Node.PROCESS_MODE_ALWAYS
	timer.wait_time = 0.08
	timer.one_shot = false
	get_root().add_child(timer)
	timer.timeout.connect(_dst_scan)
	timer.start()
	var scene = str(ProjectSettings.get_setting("application/run/main_scene"))
	if scene != "":
		change_scene_to_file(scene)

func _dst_track_control(node):
	if not (node is Control):
		return
	var id = node.get_instance_id()
	if _dst_controls.has(id):
		var current = _dst_controls[id].get_ref()
		if current != null:
			return
		_dst_controls.erase(id)
		_dst_control_order.erase(id)
	while _dst_control_order.size() >= DST_MAX_TRACKED_CONTROLS:
		_dst_controls.erase(_dst_control_order.pop_front())
	_dst_controls[id] = weakref(node)
	_dst_control_order.append(id)
	# 游标只在首次注册时初始化；持续创建控件不能反复重置游标，
	# 否则旧控件可能长期得不到轮转。
	if _dst_control_cursor < 0:
		_dst_control_cursor = _dst_control_order.size() - 1

func _dst_scan_controls():
	var stale = []
	var count = _dst_control_order.size()
	if count == 0:
		_dst_control_cursor = -1
		return
	if _dst_control_cursor < 0 or _dst_control_cursor >= count:
		_dst_control_cursor = count - 1
	var index = _dst_control_cursor
	var scanned = 0
	while scanned < count and scanned < DST_TRACKED_CONTROL_SLICE and _dst_seen < DST_SCAN_LIMIT:
		var id = _dst_control_order[index]
		index -= 1
		if index < 0:
			index = count - 1
		scanned += 1
		var holder = _dst_controls.get(id)
		var node = holder.get_ref() if holder != null else null
		if node == null:
			stale.append(id)
			continue
		if not node.is_visible_in_tree():
			continue
		_dst_apply(node, "text")
		_dst_apply_items(node)
	for id in stale:
		_dst_controls.erase(id)
		_dst_control_order.erase(id)
	_dst_control_cursor = min(index, _dst_control_order.size() - 1)

func _dst_scan():
	_dst_seen = 0
	_dst_scan_controls()
	# node_added 负责跟踪普通 UI；自定义文本节点按固定节点预算分片兜底，
	# 即使场景含大量非文本节点，单次 80 毫秒回调也不会遍历整棵树。
	if _dst_scan_frontier.is_empty() and _dst_full_scan_tick == 0:
		_dst_scan_frontier.append(get_root())
	_dst_scan_fallback_slice()
	_dst_full_scan_tick = (_dst_full_scan_tick + 1) % DST_FULL_SCAN_EVERY
	_dst_pump()

func _dst_scan_fallback_slice():
	var visited = 0
	while not _dst_scan_frontier.is_empty() and visited < DST_FALLBACK_NODE_SLICE and _dst_seen < DST_SCAN_LIMIT:
		var node = _dst_scan_frontier.pop_back()
		visited += 1
		if node == null or not is_instance_valid(node):
			continue
		_dst_track_control(node)
		if node is CanvasItem and not node.is_visible_in_tree():
			continue
		# Godot 4 的 RichTextLabel 把 BBCode 原文保存在 text 属性中。
		_dst_apply(node, "text")
		_dst_apply_items(node)
		for child in node.get_children():
			_dst_scan_frontier.append(child)

func _dst_apply(node, prop):
	var source = node.get(prop)
	if typeof(source) != TYPE_STRING or not _dst_wanted(source):
		return
	_dst_seen += 1
	var translated = _dst_render(source)
	if translated != "":
		_dst_font_for(node)
		node.set(prop, translated)
	else:
		_dst_queue_text(source)

func _dst_apply_item(node, index):
	if _dst_seen >= DST_SCAN_LIMIT:
		return
	var source = str(node.get_item_text(index))
	if not _dst_wanted(source):
		return
	_dst_seen += 1
	var translated = _dst_render(source)
	if translated != "":
		_dst_font_for(node)
		node.set_item_text(index, translated)
	else:
		_dst_queue_text(source)

func _dst_apply_items(node):
	if not node.has_method("get_item_count") or not node.has_method("get_item_text") or not node.has_method("set_item_text"):
		return
	var count = int(node.get_item_count())
	if count <= 0:
		return
	var cursor = int(node.get_meta("__deepseek_item_cursor", 0))
	if cursor < 0 or cursor >= count:
		cursor = 0
	var scanned = 0
	while scanned < count and scanned < DST_ITEMS_PER_NODE_SLICE and _dst_seen < DST_SCAN_LIMIT:
		_dst_apply_item(node, cursor)
		cursor = (cursor + 1) % count
		scanned += 1
	node.set_meta("__deepseek_item_cursor", cursor)

func _dst_queue_text(source):
	for part in _dst_split(str(source)):
		if part.tag:
			continue
		var query = str(part.text).strip_edges()
		if not _dst_plain_wanted(query) or _dst_cache.has(query) or _dst_pending.has(query) or _dst_recent_miss(query) or _dst_queue_count() >= DST_QUEUE_LIMIT:
			continue
		_dst_pending[query] = true
		_dst_queue.append(query)

func _dst_pump():
	for req in _dst_requests:
		if _dst_queue_count() <= 0:
			return
		var id = req.get_instance_id()
		if _dst_busy.has(id):
			continue
		var batch = []
		while _dst_queue_head < _dst_queue.size() and batch.size() < DST_BATCH_SIZE:
			batch.append(_dst_queue[_dst_queue_head])
			_dst_queue_head += 1
		_dst_compact_queue()
		_dst_busy[id] = batch
		var err = req.request(DST_URL, ["Content-Type: application/json"], HTTPClient.METHOD_POST, JSON.stringify({"texts": batch}))
		if err != OK:
			_dst_report_error("batch-request", "error=%d request_id=%d" % [err, id])
			_dst_busy.erase(id)
			for query in batch:
				_dst_pending.erase(query)
				_dst_mark_miss(query)

func _dst_queue_count():
	return _dst_queue.size() - _dst_queue_head

func _dst_compact_queue():
	if _dst_queue_head <= 0:
		return
	if _dst_queue_head >= _dst_queue.size():
		_dst_queue.clear()
		_dst_queue_head = 0
		return
	if _dst_queue_head < 256:
		return
	var remaining = []
	for i in range(_dst_queue_head, _dst_queue.size()):
		remaining.append(_dst_queue[i])
	_dst_queue = remaining
	_dst_queue_head = 0

func _dst_done(result, response_code, headers, body, request_id):
	if not _dst_busy.has(request_id):
		_dst_report_error("unknown-request-callback", "request_id=%d result=%d status=%d" % [request_id, result, response_code])
		return
	var batch = _dst_busy[request_id]
	_dst_busy.erase(request_id)
	for query in batch:
		_dst_pending.erase(query)
	if response_code != 200:
		_dst_report_error("batch-http", "request_id=%d result=%d status=%d" % [request_id, result, response_code])
		for query in batch:
			_dst_mark_miss(query)
		_dst_pump()
		return
	var json = JSON.new()
	if json.parse(body.get_string_from_utf8()) != OK or typeof(json.data) != TYPE_DICTIONARY or not json.data.has("translations"):
		_dst_report_error("batch-json-schema", "request_id=%d parse_error=%s" % [request_id, json.get_error_message()])
		for query in batch:
			_dst_mark_miss(query)
		_dst_pump()
		return
	var translations = json.data["translations"]
	for query in batch:
		if translations.has(query) and str(translations[query]) != query and str(translations[query]) != "":
			_dst_cache_put(query, str(translations[query]))
		else:
			_dst_mark_miss(query)
	_dst_pump()

func _dst_cache_put(query, translated):
	if not _dst_cache.has(query):
		_dst_cache_order.append(query)
	_dst_cache[query] = translated
	while _dst_cache_order.size() > DST_MAX_CACHE:
		_dst_cache.erase(_dst_cache_order.pop_front())

func _dst_mark_miss(query):
	if not _dst_miss_until.has(query):
		_dst_miss_order.append(query)
	_dst_miss_until[query] = Time.get_ticks_msec() + DST_MISS_BACKOFF_MS
	while _dst_miss_order.size() > DST_MAX_MISS:
		_dst_miss_until.erase(_dst_miss_order.pop_front())

func _dst_recent_miss(query):
	if not _dst_miss_until.has(query):
		return false
	if Time.get_ticks_msec() < int(_dst_miss_until[query]):
		return true
	_dst_miss_until.erase(query)
	_dst_miss_order.erase(query)
	return false

func _dst_render(source):
	var out = ""
	var changed = false
	for part in _dst_split(str(source)):
		if part.tag:
			out += str(part.text)
			continue
		var raw = str(part.text)
		var query = raw.strip_edges()
		if not _dst_plain_wanted(query):
			out += raw
		elif not _dst_cache.has(query):
			return ""
		else:
			out += raw.replace(query, str(_dst_cache[query]))
			changed = true
	return out if changed else ""

func _dst_split(text):
	var parts = []
	var rest = str(text)
	while rest != "":
		var start = rest.find("[")
		if start < 0:
			parts.append({"tag": false, "text": rest})
			break
		if start > 0:
			parts.append({"tag": false, "text": rest.substr(0, start)})
			rest = rest.substr(start)
		var end = rest.find("]")
		if end < 0:
			parts.append({"tag": false, "text": rest})
			break
		parts.append({"tag": true, "text": rest.substr(0, end + 1)})
		rest = rest.substr(end + 1)
	return parts

func _dst_font_for(node):
	if _dst_font == null:
		for path in DST_FONT_PATHS:
			if FileAccess.file_exists(path):
				var font = FontFile.new()
				if font.load_dynamic_font(path) == OK:
					_dst_font = font
					break
		if _dst_font == null and not _dst_font_warned:
			_dst_font_warned = true
			push_warning("[DeepSeek Godot] no usable CJK font found in DST_FONT_PATHS; Chinese glyphs may render as boxes.")
	if _dst_font != null and node.has_method("add_theme_font_override"):
		node.add_theme_font_override("font", _dst_font)
		node.add_theme_font_override("normal_font", _dst_font)

func _dst_wanted(text):
	for part in _dst_split(str(text)):
		if not part.tag and _dst_plain_wanted(str(part.text).strip_edges()):
			return true
	return false

func _dst_plain_wanted(s):
	if s.length() < 2 or s.length() > DST_MAX_TEXT or s.find("res://") >= 0 or s.find("user://") >= 0 or s.find("/") >= 0 or s.find("\\") >= 0:
		return false
	var latin = false
	for i in range(s.length()):
		var c = s.unicode_at(i)
		if c >= 0x4e00 and c <= 0x9fff:
			return false
		if (c >= 65 and c <= 90) or (c >= 97 and c <= 122):
			latin = true
	return latin
