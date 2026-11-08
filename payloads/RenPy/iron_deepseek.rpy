init 999 python:
    import ast as _ds_python_ast, re as _ds_re, json as _ds_json, os as _ds_os, time as _ds_time, threading as _ds_threading, sys as _ds_sys, traceback as _ds_traceback, weakref as _ds_weakref, collections as _ds_collections, unicodedata as _ds_unicode
    try:
        from urllib.request import Request as _ds_Request, urlopen as _ds_urlopen
    except ImportError:
        from urllib2 import Request as _ds_Request, urlopen as _ds_urlopen
    _ds_old_say = renpy.exports.say
    try:
        _ds_string_types = (basestring,)
    except NameError:
        _ds_string_types = (str,)
    try:
        _ds_text_type = unicode
        _ds_codepoint_char = unichr
    except NameError:
        _ds_text_type = str
        _ds_codepoint_char = chr
    _ds_terminal_negative = {}
    _ds_terminal_negative_order = _ds_collections.deque()
    _ds_terminal_negative_ttl = 30.0
    _ds_completed_outputs = set()
    _ds_completed_output_order = _ds_collections.deque()
    _ds_memo = {}
    _ds_memo_order = _ds_collections.deque()
    _ds_pending = {}
    _ds_retry_after = {}
    _ds_live_queue = []
    _ds_fast_live_queue = []
    _ds_priority_pending = []
    _ds_priority_set = set()
    _ds_inflight = set()
    _ds_lock = _ds_threading.RLock()
    _ds_wake = _ds_threading.Event()
    _ds_live_wake = _ds_threading.Event()
    _ds_fast_live_wake = _ds_threading.Event()
    _ds_state = {'down_until': 0.0, 'poller': False, 'live_worker': False, 'fast_live_worker': False, 'menu_prefetch': False, 'refresh_scheduled': False}
    _ds_error_counts = {}
    _ds_diag_counts = {}
    # 对象由引擎拥有；依赖只保留原文键，不让刷新登记延长 Text 的寿命。
    _ds_text_dependencies = _ds_weakref.WeakKeyDictionary()
    _ds_uncaptured_texts = _ds_weakref.WeakSet()
    _ds_text_capture = _ds_threading.local()
    _ds_refresh_keys = set()
    # 引擎边界故障会计数并记录；热路径重复故障只在二次幂次数时输出。
    def _ds_report_exception(exc, where=None):
        where = where or _ds_sys._getframe(1).f_code.co_name
        key = where + '|' + exc.__class__.__name__
        with _ds_lock:
            count = _ds_error_counts.get(key, 0) + 1
            _ds_error_counts[key] = count
        if count <= 3 or (count & (count - 1)) == 0:
            detail = _ds_traceback.format_exc() if count == 1 else repr(exc)
            message = '[DeepSeek][EXCEPTION-BOUNDARY] operation=%s occurrence=%d error=%s' % (where, count, detail)
            if hasattr(renpy, 'log'):
                renpy.log(message)
            else:
                _ds_sys.stderr.write(message + '\n')
    def _ds_report_diagnostic(where, detail):
        key = where + '|' + detail
        with _ds_lock:
            count = _ds_diag_counts.get(key, 0) + 1
            _ds_diag_counts[key] = count
        if count <= 3 or (count & (count - 1)) == 0:
            message = '[DeepSeek][DIAGNOSTIC] operation=%s occurrence=%d detail=%s' % (where, count, detail)
            if hasattr(renpy, 'log'):
                renpy.log(message)
            else:
                _ds_sys.stderr.write(message + '\n')
    def _ds_has_cjk(s):
        try:
            if not isinstance(s, _ds_text_type):
                s = s.decode('utf-8', 'replace')
            return any(u'\u4e00' <= ch <= u'\u9fff' for ch in s)
        except Exception as exc:
            _ds_report_exception(exc)
            return False
    # 只把可见英文作为半汉化残留；变量、标签和短大写缩写不重新翻译。
    def _ds_is_localized(s):
        completed_key = _ds_completed_output_key(s)
        with _ds_lock:
            if completed_key in _ds_completed_outputs:
                return True
        if not _ds_has_cjk(s):
            return False
        if not isinstance(s, _ds_text_type):
            s = s.decode('utf-8', 'replace')
        # 双开括号是 Ren'Py 的可见字面量；只遮住恢复器识别的真实标签/插值。
        # 使用位置而非 replace，避免同名的字面 [name] 被实际变量一起删除。
        visible = list(s)
        for open_ch, close_ch in (('[', ']'), ('{', '}')):
            for start, end in _ds_collect_renpy_spans(s, open_ch, close_ch, True):
                visible[start:end] = [' '] * (end - start)
        visible = _ds_re.sub(_ds_text_type(r"%\([^)]+\)[A-Za-z]|[A-Za-z][A-Za-z0-9+.-]*:[\\/][!-~]*"), ' ', ''.join(visible))
        return not any(len(word) >= 4 or any('a' <= ch <= 'z' for ch in word) for word in _ds_re.findall('[A-Za-z]{3,}', visible))
    # 颜色动画会重写标签；只规范真实格式标签，保留字面括号和插值标识。
    def _ds_completed_output_key(s):
        if not isinstance(s, _ds_text_type):
            s = s.decode('utf-8', 'replace')
        parts = []
        cursor = 0
        for start, end in _ds_collect_renpy_spans(s, '{', '}', True):
            parts.append(s[cursor:start])
            cursor = end
        parts.append(s[cursor:])
        return ''.join(parts)
    # 输出里可保留英文人名；格式变动后的终态也不能作为新的混合原文排队。
    def _ds_completed_output(source, output):
        completed_key = _ds_completed_output_key(output)
        if completed_key and completed_key != _ds_completed_output_key(source):
            with _ds_lock:
                if completed_key not in _ds_completed_outputs:
                    _ds_completed_outputs.add(completed_key)
                    _ds_completed_output_order.append(completed_key)
                while len(_ds_completed_outputs) > 8000:
                    _ds_completed_outputs.discard(_ds_completed_output_order.popleft())
        return output
    def _ds_http(path, payload, timeout):
        data = _ds_json.dumps(payload).encode('utf-8')
        req = _ds_Request('http://127.0.0.1:19999' + path, data=data, headers={'Content-Type':'application/json'})
        raw = _ds_urlopen(req, timeout=timeout).read()
        if not isinstance(raw, str):
            raw = raw.decode('utf-8')
        return _ds_json.loads(raw)
    def _ds_memo_get(s):
        try:
            hit = _ds_memo.get(s)
            if hit is None:
                return None
            val, ts = hit
            if val != s or _ds_time.time() - ts < 5.0:
                return val
        except Exception as exc:
            _ds_report_exception(exc)
        return None
    def _ds_memo_put(s, val, now):
        try:
            # deque 与字典共同构成有界 FIFO：淘汰为摊还 O(1)，避免后台线程持 GIL 排序。
            with _ds_lock:
                # 缓存查询与实时请求可以乱序完成；迟到 queued 原文不能覆盖已完成的译文。
                previous = _ds_memo.get(s)
                if val == s and previous is not None and previous[0] != s:
                    return
                if s not in _ds_memo:
                    _ds_memo_order.append(s)
                _ds_memo[s] = (val, now)
                while len(_ds_memo) > 8000:
                    _ds_memo.pop(_ds_memo_order.popleft(), None)
        except Exception as exc:
            _ds_report_exception(exc)
    # 提供方透传结果和令牌校验拒绝结果，在有限缓存代内视为终态。
    # 低频复查可发现共享缓存中的修正条目，同时避免恢复每 5 秒一次的热重试循环。
    def _ds_mark_terminal_negative(s):
        try:
            with _ds_lock:
                if s not in _ds_terminal_negative:
                    _ds_terminal_negative_order.append(s)
                _ds_terminal_negative[s] = _ds_time.time()
                while len(_ds_terminal_negative) > 4096:
                    _ds_terminal_negative.pop(_ds_terminal_negative_order.popleft(), None)
        except Exception as exc:
            _ds_report_exception(exc)
    def _ds_is_terminal_negative(s, now=None):
        try:
            now = _ds_time.time() if now is None else now
            with _ds_lock:
                marked = _ds_terminal_negative.get(s)
                if marked is None:
                    return False
                if now - marked < _ds_terminal_negative_ttl:
                    return True
            _ds_report_diagnostic('terminal-negative-recheck', 'cooldown-expired')
            return False
        except Exception as exc:
            _ds_report_exception(exc)
            return True
    def _ds_note_pending_many(texts, priority=False):
        try:
            now = _ds_time.time()
            queued = False
            dropped = 0
            with _ds_lock:
                for s in texts:
                    if not isinstance(s, _ds_string_types) or not s or _ds_is_localized(s):
                        continue
                    if _ds_is_terminal_negative(s, now):
                        continue
                    if s not in _ds_pending and len(_ds_pending) >= 1200:
                        if priority and _ds_evict_nonpriority_pending_locked():
                            pass
                        else:
                            dropped += 1
                            continue
                    if s not in _ds_pending:
                        _ds_pending[s] = 0
                        _ds_retry_after[s] = 0.0
                        queued = True
                    if priority and s not in _ds_priority_set:
                        _ds_priority_set.add(s)
                        _ds_priority_pending.append(s)
                        queued = True
                    _ds_prev_memo = _ds_memo.get(s)
                    if _ds_prev_memo is None or _ds_prev_memo[0] == s:
                        _ds_memo_put(s, s, now)
            if queued:
                _ds_ensure_poller()
                _ds_wake.set()
            if dropped:
                _ds_report_diagnostic('pending-capacity', 'dropped=%d limit=1200' % dropped)
        except Exception as exc:
            _ds_report_exception(exc)
    def _ds_note_pending(s, priority=False):
        _ds_note_pending_many((s,), priority)
    # 渲染回调只访问进程内存；所有 HTTP 请求都留在守护工作线程。
    def _ds_fetch(s, priority=False):
        _ds_capture_text_key(s)
        out = _ds_memo_get(s)
        if out is not None and out != s:
            return out
        if _ds_is_terminal_negative(s):
            return s
        if out is not None:
            if priority:
                _ds_note_pending(s, True)
            return out
        _ds_note_pending(s, priority)
        return None
    def _ds_capture_text_key(s):
        stack = getattr(_ds_text_capture, 'stack', None)
        if stack:
            stack[-1].add(s)
    def _ds_refresh_visible_text():
        with _ds_lock:
            _ds_state['refresh_scheduled'] = False
            completed = set(_ds_refresh_keys)
            _ds_refresh_keys.clear()
        if not completed:
            return
        affected = False
        targets = [displayable for displayable, dependencies in list(_ds_text_dependencies.items()) if not completed.isdisjoint(dependencies)]
        for displayable in list(_ds_uncaptured_texts):
            # 插件可绕过基础 update 自行分词；未捕获的干净对象保留原有保守刷新。
            # 尚未分词的脏对象会在正常绘制时读最新缓存，不必为它重启交互。
            if displayable not in _ds_text_dependencies and not displayable.dirty:
                _ds_report_diagnostic('text-dependency-uncaptured', 'custom Text retained conservative refresh')
                targets.append(displayable)
        for _ds_text_displayable in targets:
            affected = True
            try:
                _ds_text_displayable.kill_layout()
                _ds_text_displayable.dirty = True
                renpy.display.render.redraw(_ds_text_displayable, 0)
            except Exception as exc:
                _ds_report_exception(exc, 'refresh-text-displayable')
        if not affected:
            return
        try:
            renpy.restart_interaction()
        except Exception as exc:
            _ds_report_exception(exc, 'restart-interaction')
    def _ds_refresh_interaction(keys):
        try:
            # 多个 worker 同帧完成时只排一个主线程刷新；回调开始后重新开放下一轮。
            with _ds_lock:
                _ds_refresh_keys.update(keys)
                if not _ds_refresh_keys:
                    return
                if _ds_state['refresh_scheduled']:
                    return
                _ds_state['refresh_scheduled'] = True
            _ds_invoke_main = getattr(renpy, 'invoke_in_main_thread', None)
            if _ds_invoke_main is not None:
                _ds_invoke_main(_ds_refresh_visible_text)
            else:
                # Ren'Py 7.2 等旧版本没有主线程派发接口；不能让 HTTP worker 操作布局。
                # 主线程拥有 periodic/interact 生命周期，在那里消费已标记刷新。
                # 上游引擎版本不可更改；若回调注册失败，只保留原文并记录诊断，不伪装刷新成功。
                _ds_report_diagnostic('refresh-main-thread-fallback', 'deferred to engine lifecycle callbacks')
        except Exception as exc:
            with _ds_lock:
                _ds_state['refresh_scheduled'] = False
            _ds_report_exception(exc, 'refresh-interaction')
    def _ds_flush_legacy_refresh():
        with _ds_lock:
            scheduled = _ds_state['refresh_scheduled']
        if scheduled:
            _ds_refresh_visible_text()
    def _ds_install_legacy_refresh_hook():
        if getattr(renpy, 'invoke_in_main_thread', None) is not None:
            return
        installed = 0
        for name in ('periodic_callbacks', 'interact_callbacks'):
            try:
                callbacks = getattr(renpy.config, name, None)
                if callbacks is None:
                    continue
                if _ds_flush_legacy_refresh not in callbacks:
                    callbacks.append(_ds_flush_legacy_refresh)
                installed += 1
            except Exception as exc:
                _ds_report_exception(exc, 'register-legacy-refresh-' + name)
        if not installed:
            _ds_report_diagnostic('legacy-refresh-unavailable', 'no main-thread lifecycle callback list')
    _ds_install_legacy_refresh_hook()
    def _ds_queue_live(keys, priority=False):
        queued = False
        target = _ds_fast_live_queue if priority else _ds_live_queue
        with _ds_lock:
            for key in keys:
                if key in _ds_pending and key not in _ds_inflight:
                    _ds_inflight.add(key)
                    target.append(key)
                    queued = True
        if queued:
            if priority:
                _ds_ensure_fast_live_worker()
                _ds_fast_live_wake.set()
            else:
                _ds_ensure_live_worker()
                _ds_live_wake.set()
    def _ds_forget_pending_locked(key):
        _ds_pending.pop(key, None)
        _ds_retry_after.pop(key, None)
        _ds_inflight.discard(key)
        if key in _ds_priority_set:
            _ds_priority_set.discard(key)
            _ds_priority_pending[:] = [item for item in _ds_priority_pending if item != key]
    def _ds_evict_nonpriority_pending_locked():
        for key in list(_ds_pending.keys()):
            if key in _ds_priority_set or key in _ds_inflight:
                continue
            _ds_pending.pop(key, None)
            _ds_retry_after.pop(key, None)
            return True
        return False
    def _ds_select_poll_batch(now, limit=96):
        batch = []
        if limit <= 0:
            return batch
        with _ds_lock:
            # 取满即停止；锁内没有队列写入，可直接遍历字典，兼容 Ren'Py 7 的 Python 2。
            for key in _ds_priority_pending:
                if key in _ds_pending and key not in _ds_inflight and _ds_retry_after.get(key, 0.0) <= now:
                    batch.append(key)
                    if len(batch) >= limit:
                        return batch
            # 优先集合与列表在同一把锁下维护，剩余字典键不会与已选优先项重复。
            for key in _ds_pending:
                if key not in _ds_priority_set and key not in _ds_inflight and _ds_retry_after.get(key, 0.0) <= now:
                    batch.append(key)
                    if len(batch) >= limit:
                        break
        return batch
    def _ds_next_poll_delay():
        now = _ds_time.time()
        with _ds_lock:
            due = [_ds_retry_after.get(key, 0.0) for key in _ds_pending if key not in _ds_inflight]
        if not due:
            return None
        delay = min(due) - now
        return delay if delay > 0.0 else 0.0
    # 即使实时 API 批次较慢，缓存命中也会独立完成修复。
    def _ds_poll_loop():
        while True:
            delay = _ds_next_poll_delay()
            if delay is None:
                _ds_wake.wait()
            elif delay > 0.0:
                _ds_wake.wait(delay)
            _ds_wake.clear()
            now = _ds_time.time()
            if now < _ds_state['down_until']:
                _ds_wake.wait(max(0.05, _ds_state['down_until'] - now))
                _ds_wake.clear()
                continue
            batch = _ds_select_poll_batch(now, 96)
            if not batch:
                continue
            try:
                hits = _ds_http('/cache/lookup', {'texts': batch}, 0.5).get('hits') or {}
                if not isinstance(hits, dict):
                    raise ValueError('unexpected cache lookup response shape')
            except Exception as exc:
                _ds_report_exception(exc, 'poll-cache-lookup')
                _ds_state['down_until'] = _ds_time.time() + 1.0
                continue
            healed = []
            now = _ds_time.time()
            misses = []
            for k in batch:
                v = hits.get(k)
                if v and v != k:
                    restored = _ds_restore_renpy_tokens(k, v)
                    if restored != k:
                        _ds_memo_put(k, restored, now)
                        healed.append(k)
                    else:
                        _ds_mark_terminal_negative(k)
                    with _ds_lock:
                        _ds_forget_pending_locked(k)
                else:
                    misses.append(k)
            if healed:
                _ds_refresh_interaction(healed)
            with _ds_lock:
                fast_misses = [key for key in misses if key in _ds_priority_set]
                normal_misses = [key for key in misses if key not in _ds_priority_set]
            _ds_queue_live(fast_misses, True)
            _ds_queue_live(normal_misses, False)
    def _ds_live_loop(queue, wake, batch_limit):
        while True:
            wake.wait()
            wake.clear()
            while True:
                while _ds_time.time() < _ds_state['down_until']:
                    wake.wait(max(0.05, _ds_state['down_until'] - _ds_time.time()))
                    wake.clear()
                with _ds_lock:
                    batch = queue[:batch_limit]
                    del queue[:len(batch)]
                if not batch:
                    break
                try:
                    reply = _ds_http('/batch', {'texts': batch, 'engine': 'renpy'}, 12.0)
                    got = reply.get('translations') or {}
                    sources = reply.get('sources') or []
                    if not isinstance(got, dict) or not isinstance(sources, list):
                        raise ValueError('unexpected batch response shape')
                except Exception as exc:
                    _ds_report_exception(exc, 'live-batch-request')
                    now = _ds_time.time()
                    _ds_state['down_until'] = now + 1.0
                    with _ds_lock:
                        for k in batch:
                            _ds_inflight.discard(k)
                            if k in _ds_pending:
                                _ds_retry_after[k] = now + 1.0
                    _ds_wake.set()
                    continue
                healed = []
                now = _ds_time.time()
                for i, k in enumerate(batch):
                    v = got.get(k)
                    source = sources[i] if i < len(sources) else 'miss'
                    if v and v != k:
                        restored = _ds_restore_renpy_tokens(k, v)
                        if restored != k:
                            _ds_memo_put(k, restored, now)
                            healed.append(k)
                        else:
                            _ds_mark_terminal_negative(k)
                        with _ds_lock:
                            _ds_forget_pending_locked(k)
                    elif source == 'pass':
                        _ds_mark_terminal_negative(k)
                        with _ds_lock:
                            _ds_forget_pending_locked(k)
                    else:
                        _ds_memo_put(k, k, now)
                        abandoned = False
                        with _ds_lock:
                            _ds_inflight.discard(k)
                            if k in _ds_pending:
                                _ds_pending[k] += 1
                                if _ds_pending[k] > 120:
                                    _ds_forget_pending_locked(k)
                                    abandoned = True
                                else:
                                    _ds_retry_after[k] = now + min(5.0, 0.25 * (_ds_pending[k] + 1))
                        if abandoned:
                            _ds_report_diagnostic('translation-abandoned', 'retry_limit=120')
                if healed:
                    _ds_refresh_interaction(healed)
                _ds_wake.set()
    def _ds_ensure_poller():
        with _ds_lock:
            if _ds_state['poller']:
                return
            _ds_state['poller'] = True
        try:
            _ds_t = _ds_threading.Thread(target=_ds_poll_loop)
            _ds_t.daemon = True
            _ds_t.start()
        except Exception as exc:
            _ds_report_exception(exc)
            _ds_state['poller'] = False
    def _ds_ensure_live_worker():
        with _ds_lock:
            if _ds_state['live_worker']:
                return
            _ds_state['live_worker'] = True
        try:
            _ds_live_t = _ds_threading.Thread(target=_ds_live_loop, args=(_ds_live_queue, _ds_live_wake, 16))
            _ds_live_t.daemon = True
            _ds_live_t.start()
        except Exception as exc:
            _ds_report_exception(exc, 'start-live-worker')
            with _ds_lock:
                _ds_state['live_worker'] = False
                for key in _ds_live_queue:
                    _ds_inflight.discard(key)
                del _ds_live_queue[:]
            _ds_wake.set()
    def _ds_ensure_fast_live_worker():
        with _ds_lock:
            if _ds_state['fast_live_worker']:
                return
            _ds_state['fast_live_worker'] = True
        try:
            _ds_fast_live_t = _ds_threading.Thread(target=_ds_live_loop, args=(_ds_fast_live_queue, _ds_fast_live_wake, 16))
            _ds_fast_live_t.daemon = True
            _ds_fast_live_t.start()
        except Exception as exc:
            _ds_report_exception(exc, 'start-fast-live-worker')
            with _ds_lock:
                _ds_state['fast_live_worker'] = False
                for key in _ds_fast_live_queue:
                    _ds_inflight.discard(key)
                del _ds_fast_live_queue[:]
            _ds_wake.set()
    # 可见对白和菜单文本通过优先查询路径绕过后台 UI 工作。
    def _ds_translate(s, priority=False):
        try:
            if not s or _ds_is_localized(s):
                return s
            out = _ds_fetch(s, priority)
            if out:
                return _ds_completed_output(s, _ds_restore_renpy_tokens(s, out))
            return s
        except Exception as exc:
            _ds_report_exception(exc)
            return s
    # 保住整个插值表达式，不能让内层方括号后的属性名落入可译正文。
    # 老 Formatter 与新 parse 的表达式语法不同，按引擎实际入口选择边界。
    def _ds_renpy_span_end(s, start, open_ch, close_ch):
        if open_ch != '[':
            return s.find(close_ch, start + 1)
        # 空括号仍交给既有字面量规则；本层不改变它的显示行为。
        if start + 1 < len(s) and s[start + 1] == close_ch:
            return start + 1
        modern = callable(getattr(getattr(renpy, 'substitutions', None), 'parse', None))
        depth = 0
        parens = 0
        field = True
        # 老解析器把首字符直接收入字段；双开括号由调用方处理。
        cursor = start + (1 if modern else 2)
        while cursor < len(s):
            ch = s[cursor]
            if modern and field and ch in ('"', "'"):
                quote = ch * (3 if s[cursor:cursor + 3] == ch * 3 else 1)
                cursor += len(quote)
                while cursor < len(s) and s[cursor:cursor + len(quote)] != quote:
                    cursor += 2 if s[cursor] == '\\' else 1
                if cursor >= len(s):
                    return -1
                cursor += len(quote)
                continue
            if modern and field and ch == '(':
                parens += 1
            elif modern and field and ch == ')':
                if not parens:
                    return -1
                parens -= 1
            elif modern and field and parens:
                pass
            elif field and ch == '[':
                depth += 1
            elif ch == close_ch:
                if field and depth:
                    depth -= 1
                else:
                    return cursor
            elif field and ch in ':!' and (not modern or not depth):
                if modern and ch == '!' and s[cursor + 1:cursor + 2] == '=':
                    cursor += 1
                else:
                    field = False
            cursor += 1
        return -1
    # 收集 Ren'Py 插值和文本标签片段，例如 [ruler] 与 {i}。
    def _ds_collect_renpy_spans(s, open_ch, close_ch, with_ranges=False):
        spans = []
        try:
            i = 0
            while i < len(s):
                start = s.find(open_ch, i)
                if start < 0:
                    break
                if start + 1 < len(s) and s[start + 1] == open_ch:
                    i = start + 2
                    continue
                end = _ds_renpy_span_end(s, start, open_ch, close_ch)
                if end < 0:
                    break
                inner = s[start + 1:end]
                if inner or open_ch == '{':
                    spans.append((start, end + 1) if with_ranges else s[start:end + 1])
                i = end + 1
        except Exception as exc:
            _ds_report_exception(exc)
        return spans
    # 按位置把译文中的括号或标签片段还原为安全原始片段。
    def _ds_restore_span_sequence(src, out, open_ch, close_ch):
        src_spans = _ds_collect_renpy_spans(src, open_ch, close_ch)
        if not src_spans or open_ch not in out:
            return out
        try:
            parts = []
            i = 0
            scan = 0
            n = 0
            while n < len(src_spans):
                start = out.find(open_ch, scan)
                if start < 0:
                    break
                # 扫描位置和输出位置分开，跳过转义或字面括号时保留原有文本。
                if start + 1 < len(out) and out[start + 1] == open_ch:
                    scan = start + 2
                    continue
                end = _ds_renpy_span_end(out, start, open_ch, close_ch)
                if end < 0:
                    break
                inner = out[start + 1:end]
                if not inner and open_ch != '{':
                    scan = end + 1
                    continue
                parts.append(out[i:start])
                parts.append(src_spans[n])
                i = end + 1
                scan = i
                n += 1
            if not parts:
                return out
            parts.append(out[i:])
            return ''.join(parts)
        except Exception as exc:
            _ds_report_exception(exc)
            return out
    def _ds_renpy_delimiters_complete(text, open_ch, close_ch):
        cursor = 0
        while cursor < len(text):
            start = text.find(open_ch, cursor)
            if start < 0:
                return True
            if start + 1 < len(text) and text[start + 1] == open_ch:
                cursor = start + 2
                continue
            end = _ds_renpy_span_end(text, start, open_ch, close_ch)
            if end < 0:
                return False
            cursor = end + 1
        return True
    def _ds_collect_old_percent_spans(text):
        # 只保护映射型旧占位符；普通百分号仍由显示入口转为 %% 字面量。
        pattern = _ds_re.compile(r'[#0 +\-]*(?:\d+|\*)?(?:\.(?:\d+|\*))?[hlL]?[diouxXeEfFgGcrsa]')
        spans = []
        cursor = 0
        while cursor < len(text):
            start = text.find('%', cursor)
            if start < 0:
                break
            cursor = start + 1
            if text[start:start + 2] == '%%':
                cursor += 1
                continue
            if text[start:start + 2] != '%(':
                continue
            # Python 的映射键允许空串及平衡括号，不能在第一个右括号处截断。
            cursor = start + 2
            depth = 1
            while cursor < len(text) and depth:
                if text[cursor] == '(':
                    depth += 1
                elif text[cursor] == ')':
                    depth -= 1
                cursor += 1
            match = pattern.match(text, cursor) if not depth else None
            if match is None:
                raise ValueError('Malformed RenPy old-substitution mapping placeholder')
            spans.append((start, match.end()))
            cursor = match.end()
        return spans
    # 机器翻译可能改写 [变量]、{文本标签} 或旧式占位符；必须在引擎求值前还原。
    def _ds_restore_renpy_tokens(src, out):
        try:
            if not out or out == src:
                return out
            for open_ch, close_ch in (('[', ']'), ('{', '}')):
                if not _ds_renpy_delimiters_complete(out, open_ch, close_ch):
                    _ds_report_diagnostic('renpy-token-unclosed', '%s%s unclosed span rejected; source preserved' % (open_ch, close_ch))
                    return src
                src_spans = _ds_collect_renpy_spans(src, open_ch, close_ch)
                out_spans = _ds_collect_renpy_spans(out, open_ch, close_ch)
                if not src_spans and out_spans:
                    _ds_report_diagnostic('renpy-token-injection', 'mt-introduced %s%s spans rejected; source preserved' % (open_ch, close_ch))
                    return src
                if src_spans and len(out_spans) != len(src_spans):
                    _ds_report_diagnostic('renpy-token-mismatch', '%s%s span count %d != %d; source preserved' % (open_ch, close_ch, len(out_spans), len(src_spans)))
                    return src
            out = _ds_restore_span_sequence(src, out, '[', ']')
            out = _ds_restore_span_sequence(src, out, '{', '}')
            if renpy.config.old_substitutions:
                src_percent = _ds_collect_old_percent_spans(src)
                out_percent = _ds_collect_old_percent_spans(out)
                if len(src_percent) != len(out_percent):
                    _ds_report_diagnostic('renpy-percent-mismatch', 'old-substitution count %d != %d; source preserved' % (len(out_percent), len(src_percent)))
                    return src
                for index in range(len(src_percent) - 1, -1, -1):
                    start, end = out_percent[index]
                    source_start, source_end = src_percent[index]
                    out = out[:start] + src[source_start:source_end] + out[end:]
            return out
        except Exception as exc:
            _ds_report_exception(exc, 'restore-renpy-tokens')
            return src
    # Ren'Py 的 old_substitutions 会把 '%' 当作格式符；译文中的字面百分号需要转义。
    def _ds_protect_old_percent(s):
        try:
            if not renpy.config.old_substitutions or '%' not in s:
                return s
            out = []
            i = 0
            while i < len(s):
                ch = s[i]
                if ch == '%':
                    nxt = s[i + 1] if i + 1 < len(s) else ''
                    if nxt == '%':
                        out.append('%%')
                        i += 2
                        continue
                    if nxt == '(':
                        out.append('%')
                        i += 1
                        continue
                    out.append('%%')
                    i += 1
                    continue
                out.append(ch)
                i += 1
            return ''.join(out)
        except Exception as exc:
            _ds_report_exception(exc)
            return s
    # 直接 say 调用仍然经过 Ren'Py 原始 say 实现。
    def _ds_say(who, what, *args, **kwargs):
        return _ds_old_say(who, _ds_protect_old_percent(_ds_translate(what, True)), *args, **kwargs)
    renpy.exports.say = _ds_say
    try:
        renpy.say = _ds_say
    except Exception as exc:
        _ds_report_exception(exc)
    # Python 脚本片段可以直接调用 Character 对象，从而绕过 renpy.exports.say。
    def _ds_install_character_call_hook():
        try:
            import renpy.character as _ds_character
            _ds_cls = getattr(_ds_character, 'ADVCharacter', None)
            if _ds_cls is None or getattr(_ds_cls, '_ds_deepseek_call_hooked', False):
                return
            _ds_old_character_call = _ds_cls.__call__
            def _ds_character_call(self, what, *args, **kwargs):
                try:
                    if isinstance(what, _ds_string_types):
                        what = _ds_protect_old_percent(_ds_translate(what, True))
                except Exception as exc:
                    _ds_report_exception(exc)
                return _ds_old_character_call(self, what, *args, **kwargs)
            _ds_cls.__call__ = _ds_character_call
            _ds_cls._ds_deepseek_call_hooked = True
            _ds_cls._ds_deepseek_old_call = _ds_old_character_call
        except Exception as exc:
            _ds_report_exception(exc)
    _ds_install_character_call_hook()
    # 已编译的 Say/Menu AST 节点会在渲染标签和文本前经过这个官方过滤器。
    try:
        _ds_prev_say_menu_text_filter = renpy.config.say_menu_text_filter
        def _ds_say_menu_text_filter(s):
            if _ds_prev_say_menu_text_filter is not None:
                try:
                    s = _ds_prev_say_menu_text_filter(s)
                except Exception as exc:
                    _ds_report_exception(exc)
            return _ds_protect_old_percent(_ds_translate(s, True))
        renpy.config.say_menu_text_filter = _ds_say_menu_text_filter
    except Exception as exc:
        _ds_report_exception(exc)
    # 在 Ren'Py 分别过滤标签前预热完整 AST 菜单。这个生命周期边界会把可见选项
    # 保持在同一批中，并允许它们绕过无关 UI 批次。
    def _ds_native_string(text):
        # translate_string 在开发者更新模式会登记缺失项并最终写文件；预取只能只读。
        try:
            translator = renpy.game.script.translator
            strings = getattr(translator, 'strings', None)
            if strings is None:
                raise RuntimeError('RenPy native string table is unavailable; retaining live discovery')
            language = renpy.game.preferences.language
            table = strings.get(language)
            if table is None:
                return text
            translations = table.translations
            translated = translations.get(text)
            if translated is None:
                translated = translations.get(_ds_re.sub(r'\{\#.*?\}', '', text))
            return text if translated is None else translated
        except Exception as exc:
            _ds_report_exception(exc, 'native-string-inventory')
            return text
    def _ds_install_menu_execute_hook():
        try:
            import renpy.ast as _ds_ast
            _ds_menu_cls = getattr(_ds_ast, 'Menu', None)
            if _ds_menu_cls is None:
                raise RuntimeError('renpy.ast.Menu is unavailable')
            if getattr(_ds_menu_cls, '_ds_deepseek_menu_hooked', False):
                return
            _ds_old_menu_execute = _ds_menu_cls.execute
            def _ds_menu_execute(self, *args, **kwargs):
                try:
                    labels = [_ds_native_string(item[0]) for item in self.items if item and isinstance(item[0], _ds_string_types)]
                    _ds_note_pending_many(labels, True)
                except Exception as exc:
                    _ds_report_exception(exc, 'prime-menu-labels')
                return _ds_old_menu_execute(self, *args, **kwargs)
            _ds_menu_cls.execute = _ds_menu_execute
            _ds_menu_cls._ds_deepseek_menu_hooked = True
            _ds_menu_cls._ds_deepseek_old_execute = _ds_old_menu_execute
        except Exception as exc:
            _ds_report_exception(exc, 'install-menu-prime-hook')
    _ds_install_menu_execute_hook()
    # 仅含编译脚本的游戏仍会通过 Script.namemap 暴露每个 AST 菜单。应在 Ren'Py
    # 启动生命周期预取这些精确标签，使其在玩家看到前进入缓存。
    def _ds_collect_script_menu_labels(limit=4096):
        try:
            import renpy.ast as _ds_ast
            _ds_menu_cls = getattr(_ds_ast, 'Menu', None)
            _ds_script = getattr(getattr(renpy, 'game', None), 'script', None)
            _ds_namemap = getattr(_ds_script, 'namemap', None)
            if _ds_menu_cls is None or _ds_namemap is None:
                raise RuntimeError('RenPy compiled script menu inventory is unavailable')
            _ds_nodes = _ds_namemap.values()
        except Exception as exc:
            _ds_report_exception(exc, 'collect-script-menu-inventory')
            return []
        labels = []
        seen = set()
        for _ds_node in _ds_nodes:
            try:
                if not isinstance(_ds_node, _ds_menu_cls):
                    continue
                for _ds_item in getattr(_ds_node, 'items', ()):
                    _ds_label = _ds_item[0] if _ds_item else None
                    if not isinstance(_ds_label, _ds_string_types):
                        continue
                    _ds_label = _ds_native_string(_ds_label)
                    if not _ds_label or _ds_is_localized(_ds_label) or _ds_label in seen:
                        continue
                    seen.add(_ds_label)
                    labels.append(_ds_label)
                    if len(labels) >= limit:
                        _ds_report_diagnostic('menu-prefetch-capacity', 'limit=%d' % limit)
                        return labels
            except Exception as exc:
                _ds_report_exception(exc, 'collect-script-menu-node')
        return labels
    # 编译后的 Say 已由对应引擎解码；只读正文和静态名字，不求值游戏表达式。
    def _ds_script_say_strings(node):
        values = []
        what = getattr(node, 'what', None)
        if isinstance(what, _ds_string_types):
            values.append(what)
        who = getattr(node, 'who', None)
        if not isinstance(who, _ds_string_types) or not who:
            return values
        if _ds_re.match(r'''^(?:[uUrR]{0,2})?(?:"(?:[^"\\]|\\.)*"|'(?:[^'\\]|\\.)*')$''', who):
            try:
                name = _ds_python_ast.literal_eval(who)
                if isinstance(name, _ds_string_types):
                    values.append(_ds_native_string(name))
            except (ValueError, SyntaxError) as exc:
                _ds_report_exception(exc, 'compiled-speaker-literal')
        elif _ds_re.match(r'^[A-Za-z_][A-Za-z0-9_]*$', who):
            import renpy.character as _ds_character
            _ds_store_dict = getattr(getattr(renpy, 'store', None), '__dict__', {})
            character = _ds_store_dict.get(who)
            if isinstance(character, _ds_character.ADVCharacter):
                fields = character.__dict__
                if not fields.get('dynamic', False) and isinstance(fields.get('name'), _ds_string_types):
                    values.append(_ds_native_string(fields['name']))
        return values
    def _ds_collect_script_prefetch_texts(limit=8192):
        labels = _ds_collect_script_menu_labels(min(limit, 4096))
        seen = set(labels)
        try:
            import renpy.ast as _ds_ast
            script = renpy.game.script
            translator = getattr(script, 'translator', None)
            originals = getattr(translator, 'default_translates', None)
            if originals is None:
                raise RuntimeError('RenPy compiled dialogue inventory is unavailable; keeping menu/live discovery')
            language = getattr(getattr(renpy.game, 'preferences', None), 'language', None)
            translated = getattr(translator, 'language_translates', {})
            say_types = tuple(cls for cls in (getattr(_ds_ast, 'Say', None), getattr(_ds_ast, 'TranslateSay', None)) if cls is not None)
        except Exception as exc:
            # 已导出脚本的版本/结构属于外部边界，无法从源码扫描修复。
            # 保留菜单和实时发现，不伪造对白命中；错误在此限频记录。
            _ds_report_exception(exc, 'collect-compiled-dialogue-inventory')
            return labels
        visited = 0
        for original in originals.values():
            visited += 1
            if visited > 65536:
                _ds_report_diagnostic('compiled-prefetch-scan-capacity', 'limit=65536')
                break
            identifier = getattr(original, 'identifier', None)
            current = None
            if language is not None and isinstance(identifier, _ds_string_types):
                current = translated.get((identifier.replace('.', '_'), language))
                if current is None:
                    current = translated.get((getattr(original, 'alternate', None), language))
            current = original if current is None else current
            nodes = [current] if isinstance(current, say_types) else getattr(current, 'block', ())
            for node in nodes:
                if not isinstance(node, say_types):
                    continue
                try:
                    for text in _ds_script_say_strings(node):
                        if not text or len(text) > 1200 or text in seen or _ds_is_localized(text):
                            continue
                        if len(labels) >= limit:
                            _ds_report_diagnostic('compiled-prefetch-capacity', 'limit=%d' % limit)
                            return labels
                        seen.add(text)
                        labels.append(text)
                except Exception as exc:
                    _ds_report_exception(exc, 'collect-compiled-say')
        return labels
    def _ds_prefetch_script_menus(labels=None):
        if labels is None:
            labels = _ds_collect_script_menu_labels()
        for _ds_start in range(0, len(labels), 128):
            try:
                _ds_http('/prefetch', {'texts': labels[_ds_start:_ds_start + 128], 'engine': 'renpy'}, 2.0)
            except Exception as exc:
                _ds_report_exception(exc, 'prefetch-script-menu-batch')
                break
    def _ds_start_script_menu_prefetch():
        with _ds_lock:
            if _ds_state['menu_prefetch']:
                return
            _ds_state['menu_prefetch'] = True
        try:
            # 生命周期回调在主线程读取引擎数据；工作线程只持有不可变字符串快照。
            labels = tuple(_ds_collect_script_prefetch_texts())
            _ds_menu_prefetch_t = _ds_threading.Thread(target=_ds_prefetch_script_menus, args=(labels,))
            _ds_menu_prefetch_t.daemon = True
            _ds_menu_prefetch_t.start()
        except Exception as exc:
            _ds_report_exception(exc, 'start-script-menu-prefetch')
            with _ds_lock:
                _ds_state['menu_prefetch'] = False
    try:
        _ds_start_callbacks = getattr(renpy.config, 'start_callbacks', None)
        if _ds_start_callbacks is None:
            raise RuntimeError('renpy.config.start_callbacks is unavailable')
        _ds_start_callbacks.append(_ds_start_script_menu_prefetch)
    except Exception as exc:
        _ds_report_exception(exc, 'register-script-menu-prefetch')
    # 中文补字仅在本次排版分段中生效，保留游戏样式、字体组和替换表的所有权。
    _ds_font = None
    for _ds_cand, _ds_spec in (('ds_font.ttf', u'ds_font.ttf'), ('ds_font.otf', u'ds_font.otf'), ('ds_font.ttc', u'0@ds_font.ttc')):
        if _ds_os.path.exists(_ds_os.path.join(renpy.config.gamedir, _ds_cand)):
            _ds_font = _ds_spec
            break
    _ds_font_segment_context = _ds_threading.local()
    def _ds_cjk_font_codepoint(n):
        # 仅补中文及其标点；私用区、emoji 和其他文字继续使用游戏字体。
        return (0x2e80 <= n <= 0x303f or 0x3100 <= n <= 0x312f or
                0x31a0 <= n <= 0x31bf or 0x3400 <= n <= 0x4dbf or
                0x4e00 <= n <= 0x9fff or 0xf900 <= n <= 0xfaff or
                0xfe10 <= n <= 0xfe1f or 0xfe30 <= n <= 0xfe4f or
                0xff01 <= n <= 0xff60 or 0xffe0 <= n <= 0xffe6 or
                0x20000 <= n <= 0x323af)
    def _ds_font_characters(text):
        # Windows 的 Python 2 使用 UTF-16；不把补充平面的汉字/选择符拆成代理项。
        pos = 0
        while pos < len(text):
            end = pos + 1
            n = ord(text[pos])
            if 0xd800 <= n <= 0xdbff and end < len(text):
                low = ord(text[end])
                if 0xdc00 <= low <= 0xdfff:
                    n = 0x10000 + ((n - 0xd800) << 10) + low - 0xdc00
                    end += 1
            yield pos, end, n
            pos = end
    def _ds_font_mark(n):
        if 0xe0100 <= n <= 0xe01ef:
            return True
        if n > _ds_sys.maxunicode:
            return False
        ch = _ds_codepoint_char(n)
        return len(ch) == 1 and _ds_unicode.category(ch).startswith('M')
    def _ds_plain_font_segments(segment, text):
        start = 0
        previous = None
        fallback = None
        for pos, end, n in _ds_font_characters(text):
            use_cjk = _ds_cjk_font_codepoint(n) or (previous and _ds_font_mark(n))
            if use_cjk != previous:
                if pos:
                    yield (fallback if previous else segment), text[start:pos]
                if use_cjk and fallback is None:
                    fallback = renpy.text.text.TextSegment(segment)
                    fallback.font = _ds_font
                start, previous = pos, use_cjk
        if text:
            yield (fallback if previous else segment), text[start:]
        else:
            yield segment, text
    def _ds_apply_font():
        if not _ds_font:
            return
        try:
            segment_type = renpy.text.text.TextSegment
            group_type = renpy.text.font.FontGroup
            original_subsegment = segment_type.subsegment
            original_group_segment = group_type.segment
            if getattr(original_subsegment, '_ds_cjk_segments', False):
                return
            if not callable(original_subsegment) or not callable(original_group_segment):
                raise RuntimeError('RenPy font segmentation API is unavailable')
        except (AttributeError, ImportError, RuntimeError) as exc:
            # 不支持此边界的引擎保留原字体；不能回退为覆盖全局样式/映射。
            _ds_report_exception(exc, 'install-font-segmentation')
            return
        def group_segment(group, text):
            stack = getattr(_ds_font_segment_context, 'stack', ())
            if not stack:
                for part in original_group_segment(group, text):
                    yield part
                return
            stack[-1]['group'] = True
            start, previous, replacement = 0, None, []
            for pos, end, n in _ds_font_characters(text):
                # 使用引擎的逐字符 remap 语义，再决定是否为目标补中文。
                mapped = u''.join(_ds_codepoint_char(group.char_map.get(ord(ch), ord(ch))) for ch in text[pos:end])
                targets = list(_ds_font_characters(mapped))
                use_cjk = False
                if len(targets) == 1:
                    target = targets[0][2]
                    use_cjk = not group.map.get(target) and (_ds_cjk_font_codepoint(target) or (previous and _ds_font_mark(target)))
                if use_cjk != previous:
                    if pos:
                        if previous:
                            yield _ds_font, u''.join(replacement)
                        else:
                            for part in original_group_segment(group, text[start:pos]):
                                yield part
                    start, previous, replacement = pos, use_cjk, []
                if use_cjk:
                    replacement.append(mapped)
            if previous:
                yield _ds_font, u''.join(replacement)
            else:
                # 显式范围及非中文继续经过原组，保留缺字错误和成员字体选择。
                for part in original_group_segment(group, text[start:]):
                    yield part
        def subsegment(segment, text):
            stack = getattr(_ds_font_segment_context, 'stack', None)
            if stack is None:
                stack = []
                _ds_font_segment_context.stack = stack
            context = {'group': False}
            # 只在推进原生成器时拥有上下文，避免暂停的生成器污染别的排版。
            stack.append(context)
            try:
                iterator = iter(original_subsegment(segment, text))
            finally:
                stack.pop()
            try:
                while True:
                    stack.append(context)
                    try:
                        try:
                            current, value = next(iterator)
                        except StopIteration:
                            break
                    finally:
                        stack.pop()
                    if context['group']:
                        yield current, value
                    else:
                        for part in _ds_plain_font_segments(current, value):
                            yield part
            finally:
                close = getattr(iterator, 'close', None)
                if close is not None:
                    close()
        subsegment._ds_cjk_segments = True
        group_type.segment = group_segment
        segment_type.subsegment = subsegment
    _ds_apply_font()
    # UI 显示对象经过 replace_text 并使用记忆缓存，避免逐帧发出 HTTP 请求。
    def _ds_wants_ui_text(s):
        if not s or len(s) < 2 or len(s) > 1200 or _ds_is_localized(s):
            return False
        alpha = 0
        digits = 0
        for ch in s:
            if ('a' <= ch <= 'z') or ('A' <= ch <= 'Z'):
                alpha += 1
            elif '0' <= ch <= '9':
                digits += 1
        return alpha >= 2 and digits < 4
    def _ds_replace_text(s):
        try:
            if not _ds_wants_ui_text(s):
                return s
            out = _ds_fetch(s)
            if out:
                return _ds_completed_output(s, _ds_protect_old_percent(_ds_restore_renpy_tokens(s, out)))
            return s
        except Exception as exc:
            _ds_report_exception(exc)
            return s
    # 与游戏提供的 replace_text 钩子串联，而不是直接覆盖。
    if _ds_font:
        try:
            if hasattr(renpy.config, 'replace_text'):
                _ds_prev_replace = renpy.config.replace_text
                def _ds_chain_replace(s):
                    if _ds_prev_replace is not None:
                        try:
                            s = _ds_prev_replace(s)
                        except Exception as exc:
                            _ds_report_exception(exc)
                    return _ds_replace_text(s)
                renpy.config.replace_text = _ds_chain_replace
        except Exception as exc:
            _ds_report_exception(exc)
    # 在引擎真实分词期间收集查询键，包含插值及自定义标签生成的正文。
    # 每次成功更新替换旧依赖，嵌套 Text 各有独立栈帧；不重新解析游戏表达式。
    def _ds_install_text_displayable_hook():
        try:
            _ds_text_cls = renpy.text.text.Text
            if getattr(_ds_text_cls, '_ds_deepseek_update_hooked', False):
                return
            _ds_old_text_update = _ds_text_cls.update
            _ds_old_text_init = _ds_text_cls.__init__
            _ds_old_text_copy = _ds_text_cls._copy
            _ds_old_text_per_interact = _ds_text_cls.per_interact
            def _ds_text_init(self, *args, **kwargs):
                _ds_old_text_init(self, *args, **kwargs)
                try:
                    if self not in _ds_text_dependencies:
                        _ds_uncaptured_texts.add(self)
                except Exception as exc:
                    _ds_report_exception(exc, 'track-uncaptured-text')
            def _ds_text_update(self, *args, **kwargs):
                stack = getattr(_ds_text_capture, 'stack', None)
                if stack is None:
                    stack = []
                    _ds_text_capture.stack = stack
                keys = set()
                stack.append(keys)
                try:
                    result = _ds_old_text_update(self, *args, **kwargs)
                finally:
                    stack.pop()
                # 引擎更新异常继续由引擎报告，不用半次分词的依赖覆盖上一次成功结果。
                try:
                    _ds_text_dependencies[self] = frozenset(keys)
                    _ds_uncaptured_texts.discard(self)
                except Exception as exc:
                    _ds_report_exception(exc, 'track-text-dependencies')
                return result
            def _ds_text_copy(self, *args, **kwargs):
                result = _ds_old_text_copy(self, *args, **kwargs)
                try:
                    if self in _ds_text_dependencies:
                        _ds_text_dependencies[result] = _ds_text_dependencies[self]
                        _ds_uncaptured_texts.discard(result)
                    else:
                        _ds_uncaptured_texts.add(result)
                except Exception as exc:
                    _ds_report_exception(exc, 'copy-text-dependencies')
                return result
            def _ds_text_per_interact(self, *args, **kwargs):
                result = _ds_old_text_per_interact(self, *args, **kwargs)
                # 读档或第三方复制可绕过构造/分词；首次进入交互时重新建立依赖。
                # 只重建这个对象，元数据不写入对象或存档；已有 _copy 的依赖直接继承。
                try:
                    if self not in _ds_text_dependencies:
                        _ds_uncaptured_texts.add(self)
                        if not self.dirty:
                            _ds_report_diagnostic('text-dependency-rebuild', 'untracked clean Text entered interaction')
                            self.dirty = True
                            renpy.display.render.redraw(self, 0)
                except Exception as exc:
                    _ds_report_exception(exc, 'rebuild-text-dependencies')
                return result
            _ds_text_cls.update = _ds_text_update
            _ds_text_cls.__init__ = _ds_text_init
            _ds_text_cls._copy = _ds_text_copy
            _ds_text_cls.per_interact = _ds_text_per_interact
            _ds_text_cls._ds_deepseek_update_hooked = True
        except Exception as exc:
            _ds_report_exception(exc, 'install-text-displayable-hook')
    _ds_install_text_displayable_hook()
