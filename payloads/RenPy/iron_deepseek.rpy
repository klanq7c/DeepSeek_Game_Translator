init 999 python:
    import json as _ds_json, os as _ds_os, time as _ds_time, threading as _ds_threading, sys as _ds_sys, traceback as _ds_traceback, weakref as _ds_weakref, collections as _ds_collections
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
    except NameError:
        _ds_text_type = str
    _ds_terminal_negative = {}
    _ds_terminal_negative_order = _ds_collections.deque()
    _ds_terminal_negative_ttl = 30.0
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
    _ds_text_displayables = _ds_weakref.WeakSet()
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
                    if not isinstance(s, _ds_string_types) or not s or _ds_has_cjk(s):
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
    def _ds_refresh_visible_text():
        with _ds_lock:
            _ds_state['refresh_scheduled'] = False
        for _ds_text_displayable in list(_ds_text_displayables):
            try:
                _ds_text_displayable.kill_layout()
                _ds_text_displayable.dirty = True
                renpy.display.render.redraw(_ds_text_displayable, 0)
            except Exception as exc:
                _ds_report_exception(exc, 'refresh-text-displayable')
        try:
            renpy.restart_interaction()
        except Exception as exc:
            _ds_report_exception(exc, 'restart-interaction')
    def _ds_refresh_interaction():
        try:
            # 多个 worker 同帧完成时只排一个主线程刷新；回调开始后重新开放下一轮。
            with _ds_lock:
                if _ds_state['refresh_scheduled']:
                    return
                _ds_state['refresh_scheduled'] = True
            _ds_invoke_main = getattr(renpy, 'invoke_in_main_thread', None)
            if _ds_invoke_main is not None:
                _ds_invoke_main(_ds_refresh_visible_text)
            else:
                _ds_report_diagnostic('refresh-main-thread-fallback', 'invoke_in_main_thread unavailable')
                _ds_refresh_visible_text()
        except Exception as exc:
            with _ds_lock:
                _ds_state['refresh_scheduled'] = False
            _ds_report_exception(exc, 'refresh-interaction')
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
        with _ds_lock:
            batch = [key for key in _ds_priority_pending if key in _ds_pending and key not in _ds_inflight and _ds_retry_after.get(key, 0.0) <= now][:limit]
            if len(batch) < limit:
                batch.extend(key for key in list(_ds_pending.keys()) if key not in _ds_priority_set and key not in _ds_inflight and _ds_retry_after.get(key, 0.0) <= now and key not in batch)
                del batch[limit:]
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
            healed = 0
            now = _ds_time.time()
            misses = []
            for k in batch:
                v = hits.get(k)
                if v and v != k:
                    restored = _ds_restore_renpy_tokens(k, v)
                    if restored != k:
                        _ds_memo_put(k, restored, now)
                        healed += 1
                    else:
                        _ds_mark_terminal_negative(k)
                    with _ds_lock:
                        _ds_forget_pending_locked(k)
                else:
                    misses.append(k)
            if healed:
                _ds_refresh_interaction()
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
                    reply = _ds_http('/batch', {'texts': batch}, 12.0)
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
                healed = 0
                now = _ds_time.time()
                for i, k in enumerate(batch):
                    v = got.get(k)
                    source = sources[i] if i < len(sources) else 'miss'
                    if v and v != k:
                        restored = _ds_restore_renpy_tokens(k, v)
                        if restored != k:
                            _ds_memo_put(k, restored, now)
                            healed += 1
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
                    _ds_refresh_interaction()
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
            if not s or _ds_has_cjk(s):
                return s
            out = _ds_fetch(s, priority)
            if out:
                return _ds_restore_renpy_tokens(s, out)
            return s
        except Exception as exc:
            _ds_report_exception(exc)
            return s
    # 收集 Ren'Py 插值和文本标签片段，例如 [ruler] 与 {i}。
    def _ds_collect_renpy_spans(s, open_ch, close_ch):
        spans = []
        try:
            i = 0
            while len(spans) < 64:
                start = s.find(open_ch, i)
                if start < 0:
                    break
                if open_ch == '[' and start + 1 < len(s) and s[start + 1] == '[':
                    i = start + 2
                    continue
                end = s.find(close_ch, start + 1)
                if end < 0:
                    break
                inner = s[start + 1:end]
                if inner and len(inner) <= 96 and '\n' not in inner and '\r' not in inner:
                    spans.append(s[start:end + 1])
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
            n = 0
            while n < len(src_spans):
                start = out.find(open_ch, i)
                if start < 0:
                    break
                end = out.find(close_ch, start + 1)
                if end < 0:
                    break
                inner = out[start + 1:end]
                if not inner or len(inner) > 96 or '\n' in inner or '\r' in inner:
                    i = end + 1
                    continue
                parts.append(out[i:start])
                parts.append(src_spans[n])
                i = end + 1
                n += 1
            if not parts:
                return out
            parts.append(out[i:])
            return ''.join(parts)
        except Exception as exc:
            _ds_report_exception(exc)
            return out
    # 机器翻译可能改写 [变量] 或 {文本标签}；必须在 Ren'Py 求值前还原。
    def _ds_restore_renpy_tokens(src, out):
        try:
            if not out or out == src:
                return out
            for open_ch, close_ch in (('[', ']'), ('{', '}')):
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
            return out
        except Exception as exc:
            _ds_report_exception(exc)
            return out
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
                    labels = [item[0] for item in self.items if item and isinstance(item[0], _ds_string_types)]
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
                    if not isinstance(_ds_label, _ds_string_types) or not _ds_label or _ds_has_cjk(_ds_label) or _ds_label in seen:
                        continue
                    seen.add(_ds_label)
                    labels.append(_ds_label)
                    if len(labels) >= limit:
                        _ds_report_diagnostic('menu-prefetch-capacity', 'limit=%d' % limit)
                        return labels
            except Exception as exc:
                _ds_report_exception(exc, 'collect-script-menu-node')
        return labels
    def _ds_prefetch_script_menus():
        labels = _ds_collect_script_menu_labels()
        for _ds_start in range(0, len(labels), 128):
            try:
                _ds_http('/prefetch', {'texts': labels[_ds_start:_ds_start + 128]}, 2.0)
            except Exception as exc:
                _ds_report_exception(exc, 'prefetch-script-menu-batch')
                break
    def _ds_start_script_menu_prefetch():
        with _ds_lock:
            if _ds_state['menu_prefetch']:
                return
            _ds_state['menu_prefetch'] = True
        try:
            _ds_menu_prefetch_t = _ds_threading.Thread(target=_ds_prefetch_script_menus)
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
    # 所有对白/UI 样式及已知游戏字体优先使用已部署的 CJK 字体。
    _ds_font = None
    for _ds_cand, _ds_spec in (('ds_font.ttf', u'ds_font.ttf'), ('ds_font.otf', u'ds_font.otf'), ('ds_font.ttc', u'0@ds_font.ttc')):
        if _ds_os.path.exists(_ds_os.path.join(renpy.config.gamedir, _ds_cand)):
            _ds_font = _ds_spec
            break
    if _ds_font:
        try:
            _ds_all_styles = list(renpy.style.styles.values())
        except Exception as exc:
            _ds_report_exception(exc)
            _ds_all_styles = []
        for _ds_st in _ds_all_styles:
            try:
                _ds_st.font = _ds_font
            except Exception as exc:
                _ds_report_exception(exc)
        for _ds_style_name in ('default', 'say_dialogue', 'say_label', 'say_thought', 'centered_text', 'nvl_dialogue', 'nvl_label', 'nvl_thought'):
            try:
                getattr(style, _ds_style_name).font = _ds_font
            except Exception as exc:
                _ds_report_exception(exc)
        try:
            _ds_fonts = set([u'DejaVuSans.ttf'])
            for _ds_gv in ('text_font', 'name_text_font', 'interface_text_font', 'button_text_font', 'choice_button_text_font', 'label_text_font', 'prompt_text_font'):
                try:
                    _ds_val = getattr(gui, _ds_gv, None)
                    if _ds_val:
                        _ds_fonts.add(_ds_val)
                except Exception as exc:
                    _ds_report_exception(exc)
            try:
                for _ds_fn in _ds_os.listdir(renpy.config.gamedir):
                    _ds_low = _ds_fn.lower()
                    if (_ds_low.endswith('.ttf') or _ds_low.endswith('.otf')) and not _ds_low.startswith('ds_font'):
                        _ds_fonts.add(_ds_fn)
            except Exception as exc:
                _ds_report_exception(exc)
            for _ds_fn in _ds_fonts:
                for _ds_b in (False, True):
                    for _ds_i in (False, True):
                        renpy.config.font_replacement_map[(_ds_fn, _ds_b, _ds_i)] = (_ds_font, _ds_b, _ds_i)
        except Exception as exc:
            _ds_report_exception(exc)
    # UI 显示对象经过 replace_text 并使用记忆缓存，避免逐帧发出 HTTP 请求。
    def _ds_wants_ui_text(s):
        if not s or len(s) < 2 or len(s) > 1200 or _ds_has_cjk(s):
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
                return _ds_protect_old_percent(_ds_restore_renpy_tokens(s, out))
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
    # 现有 Text 显示对象会缓存分词后的布局；保留弱引用，使后续异步缓存命中
    # 只能在主线程使仍存活的 Ren'Py 文本失效。
    def _ds_install_text_displayable_hook():
        try:
            _ds_text_cls = renpy.text.text.Text
            if getattr(_ds_text_cls, '_ds_deepseek_init_hooked', False):
                return
            _ds_old_text_init = _ds_text_cls.__init__
            def _ds_text_init(self, *args, **kwargs):
                _ds_old_text_init(self, *args, **kwargs)
                try:
                    _ds_text_displayables.add(self)
                except Exception as exc:
                    _ds_report_exception(exc, 'track-text-displayable')
            _ds_text_cls.__init__ = _ds_text_init
            _ds_text_cls._ds_deepseek_init_hooked = True
            _ds_text_cls._ds_deepseek_old_init = _ds_old_text_init
        except Exception as exc:
            _ds_report_exception(exc, 'install-text-displayable-hook')
    _ds_install_text_displayable_hook()
