extends SceneTree

const DST_URL = "http://127.0.0.1:19999/batch"
const DST_MAX_TEXT = 1200
const DST_MAX_QUEUE = 1536
const DST_PER_SCAN = 900
const DST_NODE_SLICE = 256
const DST_ITEMS_PER_NODE_SLICE = 64
const DST_BATCH_SIZE = 48
const DST_MAX_INFLIGHT = 4
const DST_MAX_CACHE = 4096
const DST_MAX_MISS = 4096
const DST_MISS_BACKOFF_MS = 8000
const DST_FONT_PATHS = ["C:/Windows/Fonts/simhei.ttf", "C:/Windows/Fonts/msyh.ttf", "C:/Windows/Fonts/simsun.ttc"]

var _dst_cache = {}
var _dst_cache_order = []
var _dst_pending = {}
var _dst_queue = []
var _dst_queue_head = 0
var _dst_http_pool = []
var _dst_busy = {}
var _dst_miss_until = {}
var _dst_miss_order = []
var _dst_timer = null
var _dst_seen_this_scan = 0
var _dst_scan_frontier = []
var _dst_font = null
var _dst_font_warned = false
var _dst_error_counts = {}

func _dst_report_error(context, detail):
	var count = int(_dst_error_counts.get(context, 0)) + 1
	_dst_error_counts[context] = count
	if count <= 3 or (count & (count - 1)) == 0:
		push_warning("[DeepSeek Godot] %s failed #%d: %s" % [context, count, str(detail)])

func _initialize():
	if OS.get_cmdline_args().has("--dst-preflight"):
		quit(0)
		return
	call_deferred("_dst_start")

func _dst_start():
	for i in range(DST_MAX_INFLIGHT):
		var req = HTTPRequest.new()
		get_root().add_child(req)
		_dst_http_pool.append(req)
		req.connect("request_completed", self, "_dst_http_done", [req.get_instance_id()])
	_dst_timer = Timer.new()
	_dst_timer.wait_time = 0.15
	_dst_timer.one_shot = false
	get_root().add_child(_dst_timer)
	_dst_timer.connect("timeout", self, "_dst_scan")
	_dst_timer.start()
	var scene = str(ProjectSettings.get_setting("application/run/main_scene"))
	if scene != "":
		change_scene(scene)

func _dst_scan():
	_dst_seen_this_scan = 0
	if _dst_scan_frontier.empty():
		_dst_scan_frontier.append(get_root())
	_dst_scan_fallback_slice()
	_dst_pump()

func _dst_scan_fallback_slice():
	var visited = 0
	while not _dst_scan_frontier.empty() and visited < DST_NODE_SLICE and _dst_seen_this_scan < DST_PER_SCAN:
		var node = _dst_scan_frontier.pop_back()
		visited += 1
		if node == null or not is_instance_valid(node):
			continue
		if node is CanvasItem and not node.is_visible_in_tree():
			continue
		if node.get("bbcode_enabled") == true:
			_dst_apply_property(node, "bbcode_text")
		else:
			_dst_apply_property(node, "text")
			_dst_apply_property(node, "bbcode_text")
		_dst_apply_items(node)
		for child in node.get_children():
			_dst_scan_frontier.append(child)

func _dst_apply_property(node, prop):
	var value = node.get(prop)
	if typeof(value) != TYPE_STRING:
		return
	var source = str(value)
	if not _dst_wanted(source):
		return
	_dst_seen_this_scan += 1
	var translated = _dst_render_cached(source)
	if translated != "" and translated != source:
		_dst_apply_cjk_font(node)
		node.set(prop, translated)
	else:
		_dst_queue_text(source)

func _dst_apply_items(node):
	if not node.has_method("get_item_count") or not node.has_method("get_item_text") or not node.has_method("set_item_text"):
		return
	var count = node.get_item_count()
	if count <= 0:
		return
	var cursor = int(node.get_meta("__deepseek_item_cursor")) if node.has_meta("__deepseek_item_cursor") else 0
	if cursor < 0 or cursor >= count:
		cursor = 0
	var scanned = 0
	while scanned < count and scanned < DST_ITEMS_PER_NODE_SLICE and _dst_seen_this_scan < DST_PER_SCAN:
		var source = str(node.get_item_text(cursor))
		if _dst_wanted(source):
			_dst_seen_this_scan += 1
			var translated = _dst_render_cached(source)
			if translated != "" and translated != source:
				_dst_apply_cjk_font(node)
				node.set_item_text(cursor, translated)
			else:
				_dst_queue_text(source)
		cursor = (cursor + 1) % count
		scanned += 1
	node.set_meta("__deepseek_item_cursor", cursor)

func _dst_queue_text(source):
	for part in _dst_split_bbcode(str(source)):
		if part.tag:
			continue
		var query = str(part.text).strip_edges()
		if not _dst_plain_wanted(query):
			continue
		if _dst_cache.has(query) or _dst_pending.has(query) or _dst_recent_miss(query) or _dst_queue_count() >= DST_MAX_QUEUE:
			continue
		_dst_pending[query] = true
		_dst_queue.append(query)

func _dst_pump():
	if _dst_queue_count() <= 0:
		return
	for req in _dst_http_pool:
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
		if batch.empty():
			continue
		_dst_busy[id] = batch
		var body = to_json({"texts": batch})
		var err = req.request(DST_URL, ["Content-Type: application/json"], false, HTTPClient.METHOD_POST, body)
		if err != OK:
			_dst_report_error("batch-request", "error=%d request_id=%d" % [err, id])
			for source in batch:
				_dst_pending.erase(source)
			_dst_busy.erase(id)
			_dst_backoff_batch(batch)

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

func _dst_http_done(result, response_code, headers, body, request_id):
	if not _dst_busy.has(request_id):
		_dst_report_error("unknown-request-callback", "request_id=%d result=%d status=%d" % [request_id, result, response_code])
		return
	var batch = _dst_busy[request_id]
	_dst_busy.erase(request_id)
	for source in batch:
		_dst_pending.erase(source)
	if response_code != 200:
		_dst_report_error("batch-http", "request_id=%d result=%d status=%d" % [request_id, result, response_code])
		_dst_backoff_batch(batch)
		_dst_pump()
		return
	var parsed = JSON.parse(body.get_string_from_utf8())
	if parsed.error != OK or typeof(parsed.result) != TYPE_DICTIONARY:
		_dst_report_error("batch-json", "request_id=%d parse_error=%d" % [request_id, parsed.error])
		_dst_backoff_batch(batch)
		_dst_pump()
		return
	var data = parsed.result
	if not data.has("translations") or typeof(data["translations"]) != TYPE_DICTIONARY:
		_dst_report_error("batch-schema", "request_id=%d missing translations dictionary" % request_id)
		_dst_backoff_batch(batch)
		_dst_pump()
		return
	var translations = data["translations"]
	for query in batch:
		if translations.has(query):
			var translated = str(translations[query])
			if translated != "" and translated != query:
				_dst_cache_put(query, translated)
			else:
				_dst_mark_miss(query)
		else:
			_dst_mark_miss(query)
	_dst_pump()

func _dst_backoff_batch(batch):
	for query in batch:
		_dst_mark_miss(query)

func _dst_cache_put(query, translated):
	if not _dst_cache.has(query):
		_dst_cache_order.append(query)
	_dst_cache[query] = translated
	while _dst_cache_order.size() > DST_MAX_CACHE:
		var old = _dst_cache_order.pop_front()
		_dst_cache.erase(old)

func _dst_mark_miss(query):
	if not _dst_miss_until.has(query):
		_dst_miss_order.append(query)
	_dst_miss_until[query] = OS.get_ticks_msec() + DST_MISS_BACKOFF_MS
	_dst_trim_miss()

func _dst_recent_miss(query):
	if not _dst_miss_until.has(query):
		return false
	if OS.get_ticks_msec() < int(_dst_miss_until[query]):
		return true
	_dst_miss_until.erase(query)
	_dst_miss_order.erase(query)
	return false

func _dst_trim_miss():
	while _dst_miss_order.size() > DST_MAX_MISS:
		var old = _dst_miss_order.pop_front()
		_dst_miss_until.erase(old)

func _dst_render_cached(source):
	var out = ""
	var changed = false
	for part in _dst_split_bbcode(str(source)):
		if part.tag:
			out += str(part.text)
			continue
		var raw = str(part.text)
		var query = raw.strip_edges()
		if not _dst_plain_wanted(query):
			out += raw
			continue
		if not _dst_cache.has(query):
			return ""
		var translated = str(_dst_cache[query])
		out += raw.replace(query, translated)
		changed = true
	return out if changed else ""

func _dst_split_bbcode(text):
	var parts = []
	var rest = str(text)
	while rest != "":
		var start = rest.find("[")
		if start < 0:
			parts.append({"tag": false, "text": rest})
			break
		if start > 0:
			parts.append({"tag": false, "text": rest.substr(0, start)})
			rest = rest.substr(start, rest.length() - start)
		var end = rest.find("]")
		if end < 0:
			parts.append({"tag": false, "text": rest})
			break
		parts.append({"tag": true, "text": rest.substr(0, end + 1)})
		rest = rest.substr(end + 1, rest.length() - end - 1)
	return parts

func _dst_apply_cjk_font(node):
	var font = _dst_get_font()
	if font == null or node == null or not node.has_method("add_font_override"):
		return
	node.add_font_override("font", font)
	node.add_font_override("normal_font", font)
	node.add_font_override("bold_font", font)
	node.add_font_override("italics_font", font)
	node.add_font_override("bold_italics_font", font)
	node.add_font_override("mono_font", font)

func _dst_get_font():
	if _dst_font != null:
		return _dst_font
	var file = File.new()
	for path in DST_FONT_PATHS:
		if file.file_exists(path):
			var data = DynamicFontData.new()
			data.font_path = path
			var font = DynamicFont.new()
			font.font_data = data
			font.size = 22
			_dst_font = font
			return _dst_font
	if not _dst_font_warned:
		_dst_font_warned = true
		push_warning("[DeepSeek Godot] no usable CJK font found in DST_FONT_PATHS; Chinese glyphs may render as boxes.")
	return null

func _dst_wanted(text):
	if text == null:
		return false
	for part in _dst_split_bbcode(str(text)):
		if not part.tag and _dst_plain_wanted(str(part.text).strip_edges()):
			return true
	return false

func _dst_plain_wanted(s):
	if s.length() < 2 or s.length() > DST_MAX_TEXT:
		return false
	if s.find("res://") >= 0 or s.find("user://") >= 0 or s.find("/") >= 0 or s.find("\\") >= 0:
		return false
	var has_latin = false
	for i in range(s.length()):
		var c = s.ord_at(i)
		if c >= 0x4e00 and c <= 0x9fff:
			return false
		if (c >= 65 and c <= 90) or (c >= 97 and c <= 122):
			has_latin = true
	return has_latin
