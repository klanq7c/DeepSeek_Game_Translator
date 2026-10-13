extends SceneTree

const DST_URL = "http://127.0.0.1:19999/batch"
const DST_MAX_TEXT = 1200
const DST_MAX_QUEUE = 1536
const DST_PER_SCAN = 900
const DST_NODE_SLICE = 256
const DST_ITEMS_PER_NODE_SLICE = 64
const DST_BATCH_SIZE = 48
const DST_HTTP_TIMEOUT_SECONDS = 12.0
const DST_MAX_INFLIGHT = 4
const DST_MAX_CACHE = 4096
const DST_MAX_SCENE_CONTROLS = 4096
const DST_MAX_SCENE_ITEMS = 4096
const DST_MAX_MISS = 4096
const DST_MISS_BACKOFF_MS = 8000
const DST_FONT_PATHS = ["C:/Windows/Fonts/simhei.ttf", "C:/Windows/Fonts/msyh.ttf", "C:/Windows/Fonts/simsun.ttc"]

var _dst_cache = {}
var _dst_cache_order = []
var _dst_cache_head = 0
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

# 场景生命周期只恢复已有译文；收尾剔除已离树弱引用，不携带旧场景对象。
var _dst_scene_controls = {}
var _dst_scene_refresh_pending = 0
var _dst_scene_draw_attempts = {}

func _dst_scene_added(node):
	_dst_scene_watch(node)
	# 重用已 ready 的节点不会再次发出 ready，仍需在本次入树后恢复。
	if node.is_node_ready():
		_dst_scene_ready()

# ready 在游戏自身初始化之后发出；父节点也要订阅，因为它可以重写子控件文本。
func _dst_scene_watch(node):
	if not node.is_node_ready() and not node.is_connected("ready", self, "_dst_scene_ready"):
		var err = node.connect("ready", self, "_dst_scene_ready", [], CONNECT_ONESHOT)
		if err != OK:
			_dst_report_error("scene-ready-connect", "error=%d node=%s" % [err, node.name])
	var has_items = node.has_method("get_item_count") and node.has_method("get_item_text") and node.has_method("set_item_text")
	if not (node is Control) and not has_items:
		return
	if typeof(node.get("text")) != TYPE_STRING and typeof(node.get("bbcode_text")) != TYPE_STRING and not has_items:
		return
	var id = node.get_instance_id()
	if _dst_scene_controls.has(id):
		return
	if _dst_scene_controls.size() >= DST_MAX_SCENE_CONTROLS:
		_dst_scene_prune()
	if _dst_scene_controls.size() >= DST_MAX_SCENE_CONTROLS:
		# 外部场景可能创建过量控件；游戏节点数无法由翻译器上游限制。
		# 超限仍走原有分片扫描，可能延后恢复，绝不伪装命中；限频报告容量。
		_dst_report_error("scene-cache-capacity", "controls limit=%d" % DST_MAX_SCENE_CONTROLS)
		return
	_dst_scene_controls[id] = weakref(node)
	# 动画会逐帧重写文本；draw 已提交本次画布命令，须在其收尾后恢复并重绘。
	# 延迟信号属于同一帧的引擎队列，不等待扫描计时器，不从绘制回调请求 HTTP。
	if node.has_signal("draw") and not node.is_connected("draw", self, "_dst_scene_draw"):
		var err = node.connect("draw", self, "_dst_scene_draw", [id], CONNECT_DEFERRED)
		if err != OK:
			_dst_report_error("scene-draw-connect", "error=%d node=%s" % [err, node.name])
	if node.has_signal("visibility_changed") and not node.is_connected("visibility_changed", self, "_dst_scene_visible"):
		var err = node.connect("visibility_changed", self, "_dst_scene_visible", [id])
		if err != OK:
			_dst_report_error("scene-visible-connect", "error=%d node=%s" % [err, node.name])
	_dst_scene_restore(node)

func _dst_scene_prune():
	for id in _dst_scene_controls.keys():
		var node = _dst_scene_controls[id].get_ref()
		if node == null or not node.is_inside_tree():
			_dst_scene_controls.erase(id)
			_dst_scene_draw_attempts.erase(id)

func _dst_scene_draw(id):
	var holder = _dst_scene_controls.get(id)
	var node = holder.get_ref() if holder != null else null
	if node == null or not node.is_inside_tree():
		_dst_scene_controls.erase(id)
		_dst_scene_draw_attempts.erase(id)
		return
	var frame = Engine.get_idle_frames()
	var attempt = _dst_scene_draw_attempts.get(id, [frame, 0])
	if attempt[0] != frame:
		attempt = [frame, 0]
	# 游戏若在 _draw 内反复写回原文，会与恢复重绘形成递归队列；上游不归
	# 翻译器所有。每帧最多恢复两次，超限保留游戏文本并限频记录节点信息。
	var apply = attempt[1] < 2
	attempt[1] += 1
	_dst_scene_draw_attempts[id] = attempt
	_dst_scene_restore(node, apply)

func _dst_scene_ready():
	# 同一批 ready/显示事件合并为最后一个延迟回调，排在游戏的 set_deferred 后面。
	# 这是生命周期队列的收尾，不等待计时器，也不请求 HTTP。
	_dst_scene_refresh_pending += 1
	call_deferred("_dst_scene_flush")

func _dst_scene_flush():
	_dst_scene_refresh_pending -= 1
	if _dst_scene_refresh_pending > 0:
		return
	_dst_scene_restore_all()
	_dst_scene_request_frame()

func _dst_scene_request_frame():
	var tree = Engine.get_main_loop()
	if tree.is_connected("idle_frame", self, "_dst_scene_frame_restore"):
		return
	var err = tree.connect("idle_frame", self, "_dst_scene_frame_restore", [], CONNECT_ONESHOT)
	if err != OK:
		_dst_report_error("scene-frame-connect", "error=%d" % err)

func _dst_scene_frame_restore():
	# 游戏在 ready 的延迟方法中还能再 set_deferred；下一帧处理开始时再恢复
	# 一次命中的译文，排在绘制前。不能从翻译器上游约束游戏脚本的延迟链；
	# 只处理缓存命中，不将原文写入缓存，超限和信号故障由限频诊断记录。
	_dst_scene_restore_all()

func _dst_scene_restore_all():
	_dst_scene_prune()
	for holder in _dst_scene_controls.values():
		var node = holder.get_ref()
		if node != null and node.is_inside_tree():
			_dst_scene_restore(node)

func _dst_scene_visible(id):
	var holder = _dst_scene_controls.get(id)
	var node = holder.get_ref() if holder != null else null
	if node != null and node.is_inside_tree():
		_dst_scene_restore(node)
		_dst_scene_ready()

func _dst_scene_restore(node, apply = true):
	if node is CanvasItem and not node.is_visible_in_tree():
		return
	if node.get("bbcode_enabled") == true:
		_dst_scene_property(node, "bbcode_text", apply)
	else:
		_dst_scene_property(node, "text", apply)
		_dst_scene_property(node, "bbcode_text", apply)
	if not node.has_method("get_item_count") or not node.has_method("get_item_text") or not node.has_method("set_item_text"):
		return
	var count = node.get_item_count()
	if count > DST_MAX_SCENE_ITEMS:
		# 过长菜单保留原有轮转扫描；同步命中恢复也受容量上限保护，并明示延后。
		_dst_report_error("scene-cache-capacity", "items=%d limit=%d" % [count, DST_MAX_SCENE_ITEMS])
	for index in range(min(count, DST_MAX_SCENE_ITEMS)):
		var source = str(node.get_item_text(index))
		var translated = _dst_render_cached(source)
		if translated != "" and translated != source:
			if apply:
				_dst_apply_cjk_font(node)
				node.set_item_text(index, translated)
			else:
				_dst_report_error("scene-draw-repeat", "node=%s item=%d" % [node.name, index])

func _dst_scene_property(node, prop, apply = true):
	var source = node.get(prop)
	if typeof(source) != TYPE_STRING:
		return
	var translated = _dst_render_cached(source)
	if translated != "" and translated != source:
		if apply:
			_dst_apply_cjk_font(node)
			node.set(prop, translated)
		else:
			_dst_report_error("scene-draw-repeat", "node=%s property=%s" % [node.name, prop])

func _dst_report_error(context, detail):
	var count = int(_dst_error_counts.get(context, 0)) + 1
	_dst_error_counts[context] = count
	if count <= 3 or (count & (count - 1)) == 0:
		push_warning("[DeepSeek Godot] %s failed #%d: %s" % [context, count, str(detail)])

func _initialize():
	if OS.get_cmdline_args().has("--dst-preflight"):
		quit(73)
		return
	var err = Engine.get_main_loop().connect("node_added", self, "_dst_scene_added")
	if err != OK:
		_dst_report_error("scene-added-connect", "error=%d" % err)
	call_deferred("_dst_start")

func _dst_start():
	for i in range(DST_MAX_INFLIGHT):
		var req = HTTPRequest.new()
		# 本地进程可能停顿或只返回半截正文；渲染层无法替它完成请求。
		# 超时走 batch-http 限频诊断和原有退避，释放槽位且不缓存原文。
		req.timeout = DST_HTTP_TIMEOUT_SECONDS
		# 暂停菜单仍需翻译，HTTP 处理与超时不能继承游戏的暂停状态。
		req.pause_mode = Node.PAUSE_MODE_PROCESS
		get_root().add_child(req)
		_dst_http_pool.append(req)
		req.connect("request_completed", self, "_dst_http_done", [req.get_instance_id()])
	_dst_timer = Timer.new()
	_dst_timer.pause_mode = Node.PAUSE_MODE_PROCESS
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
		_dst_scene_watch(node)
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
	# 本次处理只解析一次；先按原文排队，再由渲染消耗该计划。
	var plan = _dst_text_plan(source)
	if not _dst_plan_wanted(plan):
		return
	_dst_seen_this_scan += 1
	_dst_queue_plan(plan)
	var translated = _dst_render_groups(plan)
	if translated != "" and translated != source:
		_dst_apply_cjk_font(node)
		node.set(prop, translated)

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
		var plan = _dst_text_plan(source)
		if _dst_plan_wanted(plan):
			_dst_seen_this_scan += 1
			_dst_queue_plan(plan)
			var translated = _dst_render_groups(plan)
			if translated != "" and translated != source:
				_dst_apply_cjk_font(node)
				node.set_item_text(cursor, translated)
		cursor = (cursor + 1) % count
		scanned += 1
	node.set_meta("__deepseek_item_cursor", cursor)

func _dst_queue_text(source):
	_dst_queue_plan(_dst_text_plan(str(source)))

func _dst_queue_plan(plan):
	for group in plan.groups:
		var query = _dst_query(group.raw)
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
	if result != HTTPRequest.RESULT_SUCCESS or response_code != 200:
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
			var translated = translations[query]
			# 外部响应值不能强制转字符串，否则 null/数字会成为显示译文。
			# 此处只拒绝该项，batch-value 记录请求与类型；保留原文并退避。
			if typeof(translated) != TYPE_STRING:
				_dst_report_error("batch-value", "request_id=%d query_chars=%d type=%d" % [request_id, query.length(), typeof(translated)])
				_dst_mark_miss(query)
			elif translated.strip_edges() != "" and translated != query:
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
		# 缓存满后覆盖最旧槽位，保持 FIFO 而不移动整条数组。
		if _dst_cache_order.size() < DST_MAX_CACHE:
			_dst_cache_order.append(query)
		else:
			_dst_cache.erase(_dst_cache_order[_dst_cache_head])
			_dst_cache_order[_dst_cache_head] = query
			_dst_cache_head = (_dst_cache_head + 1) % DST_MAX_CACHE
	_dst_cache[query] = translated

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
	return _dst_render_plan(source)

func _dst_text_parts(source):
	return _dst_split_bbcode(source)

# 只组合被内联样式拆开的可见词；不同链接之间仅组合带左右箭头的选择器。
# 标签和链接标识原样保留，渲染与排队共用同一份逻辑文本，避免请求半词。
func _dst_text_plan(source):
	var parts = _dst_text_parts(source)
	var groups = []
	var last = null
	var link = ""
	var barrier = false
	for index in range(parts.size()):
		var part = parts[index]
		if part.tag:
			var tag = str(part.text)
			var body = tag.substr(1, tag.length() - 2).strip_edges()
			var closing = body.begins_with("/")
			if closing:
				body = body.substr(1, body.length() - 1)
			var name = body.split("=")[0].split(" ")[0].to_lower()
			if name == "url":
				link = "" if closing else body
			elif not _dst_inline_tag(name):
				barrier = true
			continue
		var raw = str(part.text)
		var join = last != null and not barrier and _dst_latin_edge(str(last.raw), false) and _dst_latin_edge(raw, true)
		if last != null and not barrier and int(last.picker_end) >= index:
			# 先确认同一行存在完整箭头边界，再组合数字、多词和内联样式片段。
			join = true
		if join and str(last.link) != link:
			join = int(last.picker_end) >= index
		if join:
			last.raw += raw
			last.indices.append(index)
			last.link = link
		else:
			last = {"raw": raw, "indices": [index], "link": link, "picker_end": _dst_picker_end(parts, index)}
			groups.append(last)
		barrier = false
	return {"parts": parts, "groups": groups}

func _dst_inline_tag(name):
	return name in ["b", "i", "u", "s", "color", "bgcolor", "fgcolor", "font", "font_size", "outline_color", "outline_size", "code", "wave", "shake", "tornado", "fade", "rainbow", "pulse"]

func _dst_picker_end(parts, start):
	var first = str(parts[start].text).strip_edges()
	if not first.begins_with("< ") and first != "<":
		return -1
	var raw = ""
	for index in range(start, parts.size()):
		var part = parts[index]
		if part.tag:
			var body = str(part.text).substr(1, str(part.text).length() - 2).strip_edges()
			if body.begins_with("/"):
				body = body.substr(1, body.length() - 1)
			var name = body.split("=")[0].split(" ")[0].to_lower()
			if name != "url" and not _dst_inline_tag(name):
				return -1
		else:
			raw += str(part.text)
			if raw.find("\n") >= 0 or raw.find("\r") >= 0:
				return -1
			if raw.strip_edges().ends_with(" >"):
				return index
	return -1

func _dst_latin_edge(text, first):
	if text == "":
		return false
	var letter = text.substr(0 if first else text.length() - 1, 1)
	return "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ".find(letter) >= 0

func _dst_query(raw):
	var query = str(raw).strip_edges()
	# 选择器箭头属于显示装饰；不将其当作 HTML 或翻译正文发送给服务器。
	if query.begins_with("< ") and query.ends_with(" >"):
		query = query.substr(1, query.length() - 2).strip_edges()
	return query

func _dst_render_plan(source):
	return _dst_render_groups(_dst_text_plan(str(source)))

# 渲染会修改 parts，计划只属于本次处理；排队必须先读取原文，不跨帧保留。
func _dst_render_groups(plan):
	var changed = false
	for group in plan.groups:
		var raw = str(group.raw)
		var query = _dst_query(raw)
		if not _dst_plain_wanted(query) or not _dst_cache.has(query):
			# 未命中的余项保留当前原文；已命中项仍可显示，扫描负责继续发现余项。
			continue
		var translated = str(_dst_cache[query])
		var begin = raw.find(query)
		var end = begin + query.length()
		var cursor = 0
		for index in group.indices:
			var slot = str(plan.parts[index].text)
			var slot_end = cursor + slot.length()
			var replacement = ""
			if cursor < begin:
				replacement += slot.substr(0, min(slot.length(), begin - cursor))
			var lo = max(cursor, begin)
			var hi = min(slot_end, end)
			if hi > lo:
				# 中文长度改变时，按可见字符比例分配到原有点击/颜色区间。
				# 前后边界共用映射，所有字符恰好出现一次，链接仍覆盖原区域。
				var mapped_lo = int(round(float(lo - begin) * translated.length() / query.length()))
				var mapped_hi = int(round(float(hi - begin) * translated.length() / query.length()))
				replacement += translated.substr(mapped_lo, mapped_hi - mapped_lo)
			if slot_end > end:
				var suffix = max(0, end - cursor)
				replacement += slot.substr(suffix, slot.length() - suffix)
			plan.parts[index].text = replacement
			cursor = slot_end
		changed = true
	var out = ""
	for part in plan.parts:
		out += str(part.text)
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
	return _dst_plan_wanted(_dst_text_plan(str(text)))

func _dst_plan_wanted(plan):
	for group in plan.groups:
		if _dst_plain_wanted(_dst_query(group.raw)):
			return true
	return false

func _dst_path_text(s):
	# 资源和文件路径不翻译；带空格的操作说明可包含 Interact/Select 等斜杠。
	if s.find("\\") >= 0 or s.find("://") >= 0 or s.find(":/") >= 0:
		return true
	if s.find("/") < 0:
		return false
	if s.begins_with("/") or s.begins_with("./") or s.begins_with("../"):
		return true
	var leaf = s.substr(s.rfind("/") + 1, s.length())
	if leaf.find(".") >= 0 and leaf.find(" ") < 0 and leaf.find("\t") < 0 and leaf.find("\n") < 0:
		return true
	return s.find(" ") < 0 and s.find("\t") < 0 and s.find("\n") < 0

func _dst_numeric_text(s):
	# 分辨率的 x 是数字分隔符，不是待翻译的英文；保留数值原样。
	var digit = false
	for index in range(s.length()):
		var c = s.substr(index, 1)
		if "0123456789".find(c) >= 0:
			digit = true
		elif " .,:+-xX×%".find(c) < 0:
			return false
	return digit

func _dst_plain_wanted(s):
	if s.length() < 2 or s.length() > DST_MAX_TEXT:
		return false
	if _dst_path_text(s) or _dst_numeric_text(s):
		return false
	var has_latin = false
	for i in range(s.length()):
		var c = s.ord_at(i)
		if c >= 0x4e00 and c <= 0x9fff:
			return false
		if (c >= 65 and c <= 90) or (c >= 97 and c <= 122):
			has_latin = true
	return has_latin
