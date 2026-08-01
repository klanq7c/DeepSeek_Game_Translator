/* ================================================================
 * deploy.c — 翻译 hook 与插件 payload 部署实现
 * ----------------------------------------------------------------
 * 本文件负责将翻译器 hook 注入到不同引擎的游戏目录中。
 * 包含：
 *   - Ren'Py Python hook（嵌入字符串，运行时写入 .rpy 文件）
 *   - RPG Maker MV/MZ JavaScript hook（嵌入字符串，运行时写入 .js 文件）
 *   - Unity Mono BepInEx 插件部署（DLL 复制 + BepInEx 运行时安装）
 *   - Unity IL2CPP BepInEx be.755 + XUnity AutoTranslator 全套部署
 *   - CJK 字体部署（从系统 Fonts 复制到游戏目录供 hook 使用）
 * ================================================================ */

#include "deploy.h"
#include "fsutil.h"
#include "ui.h"

#include <stdlib.h>
#include <string.h>
#include <wchar.h>

/* ----------------------------------------------------------------
 * RENPY_HOOK — Ren'Py 翻译 Python 脚本（嵌入源码）
 *
 * 功能概述：
 *   1. Hook renpy.exports.say，将对话文本发送到本地 C 服务器翻译
 *   2. 渲染路径只查进程内缓存，不执行 HTTP
 *   3. 本地缓存查询与实时 API 使用独立后台线程，命中后立即刷新 interaction
 *   4. 部署 CJK 字体（ds_font.ttf/otf/ttc）替换所有 style 的 font 属性
 *   5. replace_text hook 翻译 UI 界面文本（菜单、按钮等）
 *
 * 关键设计：
 *   - _ds_memo: 内存缓存，避免重复调用本地服务器
 *   - _ds_pending: 待翻译队列，后台线程先查缓存再批量提交实时翻译
 *   - _ds_state['down_until']: 短暂熔断，服务器不可用时避免紧密重试
 *   - font_replacement_map: 将原字体映射到 CJK 字体
 * ---------------------------------------------------------------- */
static const char RENPY_HOOK[] =
"init 999 python:\n"
"    import json as _ds_json, os as _ds_os, time as _ds_time, threading as _ds_threading, sys as _ds_sys, traceback as _ds_traceback, weakref as _ds_weakref\n"
"    try:\n"
"        from urllib.request import Request as _ds_Request, urlopen as _ds_urlopen\n"
"    except ImportError:\n"
"        from urllib2 import Request as _ds_Request, urlopen as _ds_urlopen\n"
"    _ds_old_say = renpy.exports.say\n"
"    try:\n"
"        _ds_string_types = (basestring,)\n"
"    except NameError:\n"
"        _ds_string_types = (str,)\n"
"    try:\n"
"        _ds_text_type = unicode\n"
"    except NameError:\n"
"        _ds_text_type = str\n"
"    _ds_terminal_negative = {}\n"
"    _ds_terminal_negative_ttl = 30.0\n"
"    _ds_memo = {}\n"
"    _ds_pending = {}\n"
"    _ds_retry_after = {}\n"
"    _ds_live_queue = []\n"
"    _ds_fast_live_queue = []\n"
"    _ds_priority_pending = []\n"
"    _ds_priority_set = set()\n"
"    _ds_inflight = set()\n"
"    _ds_lock = _ds_threading.RLock()\n"
"    _ds_wake = _ds_threading.Event()\n"
"    _ds_live_wake = _ds_threading.Event()\n"
"    _ds_fast_live_wake = _ds_threading.Event()\n"
"    _ds_state = {'down_until': 0.0, 'poller': False, 'live_worker': False, 'fast_live_worker': False, 'menu_prefetch': False}\n"
"    _ds_error_counts = {}\n"
"    _ds_diag_counts = {}\n"
"    _ds_text_displayables = _ds_weakref.WeakSet()\n"
"    # Engine-boundary failures are counted and logged; repeated hot-path failures log at powers of two.\n"
"    def _ds_report_exception(exc, where=None):\n"
"        where = where or _ds_sys._getframe(1).f_code.co_name\n"
"        key = where + '|' + exc.__class__.__name__\n"
"        with _ds_lock:\n"
"            count = _ds_error_counts.get(key, 0) + 1\n"
"            _ds_error_counts[key] = count\n"
"        if count <= 3 or (count & (count - 1)) == 0:\n"
"            detail = _ds_traceback.format_exc() if count == 1 else repr(exc)\n"
"            message = '[DeepSeek][EXCEPTION-BOUNDARY] operation=%s occurrence=%d error=%s' % (where, count, detail)\n"
"            if hasattr(renpy, 'log'):\n"
"                renpy.log(message)\n"
"            else:\n"
"                _ds_sys.stderr.write(message + '\\n')\n"
"    def _ds_report_diagnostic(where, detail):\n"
"        key = where + '|' + detail\n"
"        with _ds_lock:\n"
"            count = _ds_diag_counts.get(key, 0) + 1\n"
"            _ds_diag_counts[key] = count\n"
"        if count <= 3 or (count & (count - 1)) == 0:\n"
"            message = '[DeepSeek][DIAGNOSTIC] operation=%s occurrence=%d detail=%s' % (where, count, detail)\n"
"            if hasattr(renpy, 'log'):\n"
"                renpy.log(message)\n"
"            else:\n"
"                _ds_sys.stderr.write(message + '\\n')\n"
"    def _ds_has_cjk(s):\n"
"        try:\n"
"            if not isinstance(s, _ds_text_type):\n"
"                s = s.decode('utf-8', 'replace')\n"
"            return any(u'\\u4e00' <= ch <= u'\\u9fff' for ch in s)\n"
"        except Exception as exc:\n"
"            _ds_report_exception(exc)\n"
"            return False\n"
"    def _ds_http(path, payload, timeout):\n"
"        data = _ds_json.dumps(payload).encode('utf-8')\n"
"        req = _ds_Request('http://127.0.0.1:19999' + path, data=data, headers={'Content-Type':'application/json'})\n"
"        raw = _ds_urlopen(req, timeout=timeout).read()\n"
"        if not isinstance(raw, str):\n"
"            raw = raw.decode('utf-8')\n"
"        return _ds_json.loads(raw)\n"
"    def _ds_memo_get(s):\n"
"        try:\n"
"            hit = _ds_memo.get(s)\n"
"            if hit is None:\n"
"                return None\n"
"            val, ts = hit\n"
"            if val != s or _ds_time.time() - ts < 5.0:\n"
"                return val\n"
"        except Exception as exc:\n"
"            _ds_report_exception(exc)\n"
"        return None\n"
"    def _ds_memo_put(s, val, now):\n"
"        try:\n"
"            _ds_memo[s] = (val, now)\n"
"            if len(_ds_memo) > 8000:\n"
"                oldest = sorted(_ds_memo.items(), key=lambda item: item[1][1])[:512]\n"
"                for key, unused in oldest:\n"
"                    _ds_memo.pop(key, None)\n"
"        except Exception as exc:\n"
"            _ds_report_exception(exc)\n"
"    # Provider pass results and token-rejected hits are terminal for a bounded cache generation.\n"
"    # A low-frequency recheck observes a corrected shared-cache entry without restoring the hot 5-second retry loop.\n"
"    def _ds_mark_terminal_negative(s):\n"
"        try:\n"
"            with _ds_lock:\n"
"                _ds_terminal_negative[s] = _ds_time.time()\n"
"                if len(_ds_terminal_negative) > 4096:\n"
"                    oldest = sorted(_ds_terminal_negative.items(), key=lambda item: item[1])[:512]\n"
"                    for key, unused in oldest:\n"
"                        _ds_terminal_negative.pop(key, None)\n"
"        except Exception as exc:\n"
"            _ds_report_exception(exc)\n"
"    def _ds_is_terminal_negative(s, now=None):\n"
"        try:\n"
"            now = _ds_time.time() if now is None else now\n"
"            with _ds_lock:\n"
"                marked = _ds_terminal_negative.get(s)\n"
"                if marked is None:\n"
"                    return False\n"
"                if now - marked < _ds_terminal_negative_ttl:\n"
"                    return True\n"
"                _ds_terminal_negative.pop(s, None)\n"
"            _ds_report_diagnostic('terminal-negative-recheck', 'cooldown-expired')\n"
"            return False\n"
"        except Exception as exc:\n"
"            _ds_report_exception(exc)\n"
"            return True\n"
"    def _ds_note_pending_many(texts, priority=False):\n"
"        try:\n"
"            now = _ds_time.time()\n"
"            queued = False\n"
"            dropped = 0\n"
"            with _ds_lock:\n"
"                for s in texts:\n"
"                    if not isinstance(s, _ds_string_types) or not s or _ds_has_cjk(s):\n"
"                        continue\n"
"                    if _ds_is_terminal_negative(s, now):\n"
"                        continue\n"
"                    if s not in _ds_pending and len(_ds_pending) >= 1200:\n"
"                        if priority and _ds_evict_nonpriority_pending_locked():\n"
"                            pass\n"
"                        else:\n"
"                            dropped += 1\n"
"                            continue\n"
"                    if s not in _ds_pending:\n"
"                        _ds_pending[s] = 0\n"
"                        _ds_retry_after[s] = 0.0\n"
"                        queued = True\n"
"                    if priority and s not in _ds_priority_set:\n"
"                        _ds_priority_set.add(s)\n"
"                        _ds_priority_pending.append(s)\n"
"                        queued = True\n"
"                    _ds_prev_memo = _ds_memo.get(s)\n"
"                    if _ds_prev_memo is None or _ds_prev_memo[0] == s:\n"
"                        _ds_memo[s] = (s, now)\n"
"            if queued:\n"
"                _ds_ensure_poller()\n"
"                _ds_wake.set()\n"
"            if dropped:\n"
"                _ds_report_diagnostic('pending-capacity', 'dropped=%d limit=1200' % dropped)\n"
"        except Exception as exc:\n"
"            _ds_report_exception(exc)\n"
"    def _ds_note_pending(s, priority=False):\n"
"        _ds_note_pending_many((s,), priority)\n"
"    # Render callbacks only touch process memory; all HTTP stays on daemon workers.\n"
"    def _ds_fetch(s, priority=False):\n"
"        out = _ds_memo_get(s)\n"
"        if out is not None and out != s:\n"
"            return out\n"
"        if _ds_is_terminal_negative(s):\n"
"            return s\n"
"        if out is not None:\n"
"            if priority:\n"
"                _ds_note_pending(s, True)\n"
"            return out\n"
"        _ds_note_pending(s, priority)\n"
"        return None\n"
"    def _ds_refresh_visible_text():\n"
"        for _ds_text_displayable in list(_ds_text_displayables):\n"
"            try:\n"
"                _ds_text_displayable.kill_layout()\n"
"                _ds_text_displayable.dirty = True\n"
"                renpy.display.render.redraw(_ds_text_displayable, 0)\n"
"            except Exception as exc:\n"
"                _ds_report_exception(exc, 'refresh-text-displayable')\n"
"        renpy.restart_interaction()\n"
"    def _ds_refresh_interaction():\n"
"        try:\n"
"            _ds_invoke_main = getattr(renpy, 'invoke_in_main_thread', None)\n"
"            if _ds_invoke_main is not None:\n"
"                _ds_invoke_main(_ds_refresh_visible_text)\n"
"            else:\n"
"                _ds_report_diagnostic('refresh-main-thread-fallback', 'invoke_in_main_thread unavailable')\n"
"                _ds_refresh_visible_text()\n"
"        except Exception as exc:\n"
"            _ds_report_exception(exc, 'refresh-interaction')\n"
"    def _ds_queue_live(keys, priority=False):\n"
"        queued = False\n"
"        target = _ds_fast_live_queue if priority else _ds_live_queue\n"
"        with _ds_lock:\n"
"            for key in keys:\n"
"                if key in _ds_pending and key not in _ds_inflight:\n"
"                    _ds_inflight.add(key)\n"
"                    target.append(key)\n"
"                    queued = True\n"
"        if queued:\n"
"            if priority:\n"
"                _ds_ensure_fast_live_worker()\n"
"                _ds_fast_live_wake.set()\n"
"            else:\n"
"                _ds_ensure_live_worker()\n"
"                _ds_live_wake.set()\n"
"    def _ds_forget_pending_locked(key):\n"
"        _ds_pending.pop(key, None)\n"
"        _ds_retry_after.pop(key, None)\n"
"        _ds_inflight.discard(key)\n"
"        if key in _ds_priority_set:\n"
"            _ds_priority_set.discard(key)\n"
"            _ds_priority_pending[:] = [item for item in _ds_priority_pending if item != key]\n"
"    def _ds_evict_nonpriority_pending_locked():\n"
"        for key in list(_ds_pending.keys()):\n"
"            if key in _ds_priority_set or key in _ds_inflight:\n"
"                continue\n"
"            _ds_pending.pop(key, None)\n"
"            _ds_retry_after.pop(key, None)\n"
"            return True\n"
"        return False\n"
"    def _ds_select_poll_batch(now, limit=96):\n"
"        with _ds_lock:\n"
"            batch = [key for key in _ds_priority_pending if key in _ds_pending and key not in _ds_inflight and _ds_retry_after.get(key, 0.0) <= now][:limit]\n"
"            if len(batch) < limit:\n"
"                batch.extend(key for key in list(_ds_pending.keys()) if key not in _ds_priority_set and key not in _ds_inflight and _ds_retry_after.get(key, 0.0) <= now and key not in batch)\n"
"                del batch[limit:]\n"
"        return batch\n"
"    def _ds_next_poll_delay():\n"
"        now = _ds_time.time()\n"
"        with _ds_lock:\n"
"            due = [_ds_retry_after.get(key, 0.0) for key in _ds_pending if key not in _ds_inflight]\n"
"        if not due:\n"
"            return None\n"
"        delay = min(due) - now\n"
"        return delay if delay > 0.0 else 0.0\n"
"    # Cache hits are healed independently, even while a live API batch is slow.\n"
"    def _ds_poll_loop():\n"
"        while True:\n"
"            delay = _ds_next_poll_delay()\n"
"            if delay is None:\n"
"                _ds_wake.wait()\n"
"            elif delay > 0.0:\n"
"                _ds_wake.wait(delay)\n"
"            _ds_wake.clear()\n"
"            now = _ds_time.time()\n"
"            if now < _ds_state['down_until']:\n"
"                _ds_wake.wait(max(0.05, _ds_state['down_until'] - now))\n"
"                _ds_wake.clear()\n"
"                continue\n"
"            batch = _ds_select_poll_batch(now, 96)\n"
"            if not batch:\n"
"                continue\n"
"            try:\n"
"                hits = _ds_http('/cache/lookup', {'texts': batch}, 0.5).get('hits') or {}\n"
"                if not isinstance(hits, dict):\n"
"                    raise ValueError('unexpected cache lookup response shape')\n"
"            except Exception as exc:\n"
"                _ds_report_exception(exc, 'poll-cache-lookup')\n"
"                _ds_state['down_until'] = _ds_time.time() + 1.0\n"
"                continue\n"
"            healed = 0\n"
"            now = _ds_time.time()\n"
"            misses = []\n"
"            for k in batch:\n"
"                v = hits.get(k)\n"
"                if v and v != k:\n"
"                    restored = _ds_restore_renpy_tokens(k, v)\n"
"                    if restored != k:\n"
"                        _ds_memo_put(k, restored, now)\n"
"                        healed += 1\n"
"                    else:\n"
"                        _ds_mark_terminal_negative(k)\n"
"                    with _ds_lock:\n"
"                        _ds_forget_pending_locked(k)\n"
"                else:\n"
"                    misses.append(k)\n"
"            if healed:\n"
"                _ds_refresh_interaction()\n"
"            with _ds_lock:\n"
"                fast_misses = [key for key in misses if key in _ds_priority_set]\n"
"                normal_misses = [key for key in misses if key not in _ds_priority_set]\n"
"            _ds_queue_live(fast_misses, True)\n"
"            _ds_queue_live(normal_misses, False)\n"
"    def _ds_live_loop(queue, wake, batch_limit):\n"
"        while True:\n"
"            wake.wait()\n"
"            wake.clear()\n"
"            while True:\n"
"                while _ds_time.time() < _ds_state['down_until']:\n"
"                    wake.wait(max(0.05, _ds_state['down_until'] - _ds_time.time()))\n"
"                    wake.clear()\n"
"                with _ds_lock:\n"
"                    batch = queue[:batch_limit]\n"
"                    del queue[:len(batch)]\n"
"                if not batch:\n"
"                    break\n"
"                try:\n"
"                    reply = _ds_http('/batch', {'texts': batch}, 12.0)\n"
"                    got = reply.get('translations') or {}\n"
"                    sources = reply.get('sources') or []\n"
"                    if not isinstance(got, dict) or not isinstance(sources, list):\n"
"                        raise ValueError('unexpected batch response shape')\n"
"                except Exception as exc:\n"
"                    _ds_report_exception(exc, 'live-batch-request')\n"
"                    now = _ds_time.time()\n"
"                    _ds_state['down_until'] = now + 1.0\n"
"                    with _ds_lock:\n"
"                        for k in batch:\n"
"                            _ds_inflight.discard(k)\n"
"                            if k in _ds_pending:\n"
"                                _ds_retry_after[k] = now + 1.0\n"
"                    _ds_wake.set()\n"
"                    continue\n"
"                healed = 0\n"
"                now = _ds_time.time()\n"
"                for i, k in enumerate(batch):\n"
"                    v = got.get(k)\n"
"                    source = sources[i] if i < len(sources) else 'miss'\n"
"                    if v and v != k:\n"
"                        restored = _ds_restore_renpy_tokens(k, v)\n"
"                        if restored != k:\n"
"                            _ds_memo_put(k, restored, now)\n"
"                            healed += 1\n"
"                        else:\n"
"                            _ds_mark_terminal_negative(k)\n"
"                        with _ds_lock:\n"
"                            _ds_forget_pending_locked(k)\n"
"                    elif source == 'pass':\n"
"                        _ds_mark_terminal_negative(k)\n"
"                        with _ds_lock:\n"
"                            _ds_forget_pending_locked(k)\n"
"                    else:\n"
"                        _ds_memo_put(k, k, now)\n"
"                        abandoned = False\n"
"                        with _ds_lock:\n"
"                            _ds_inflight.discard(k)\n"
"                            if k in _ds_pending:\n"
"                                _ds_pending[k] += 1\n"
"                                if _ds_pending[k] > 120:\n"
"                                    _ds_forget_pending_locked(k)\n"
"                                    abandoned = True\n"
"                                else:\n"
"                                    _ds_retry_after[k] = now + min(5.0, 0.25 * (_ds_pending[k] + 1))\n"
"                        if abandoned:\n"
"                            _ds_report_diagnostic('translation-abandoned', 'retry_limit=120')\n"
"                if healed:\n"
"                    _ds_refresh_interaction()\n"
"                _ds_wake.set()\n"
"    def _ds_ensure_poller():\n"
"        with _ds_lock:\n"
"            if _ds_state['poller']:\n"
"                return\n"
"            _ds_state['poller'] = True\n"
"        try:\n"
"            _ds_t = _ds_threading.Thread(target=_ds_poll_loop)\n"
"            _ds_t.daemon = True\n"
"            _ds_t.start()\n"
"        except Exception as exc:\n"
"            _ds_report_exception(exc)\n"
"            _ds_state['poller'] = False\n"
"    def _ds_ensure_live_worker():\n"
"        with _ds_lock:\n"
"            if _ds_state['live_worker']:\n"
"                return\n"
"            _ds_state['live_worker'] = True\n"
"        try:\n"
"            _ds_live_t = _ds_threading.Thread(target=_ds_live_loop, args=(_ds_live_queue, _ds_live_wake, 16))\n"
"            _ds_live_t.daemon = True\n"
"            _ds_live_t.start()\n"
"        except Exception as exc:\n"
"            _ds_report_exception(exc, 'start-live-worker')\n"
"            with _ds_lock:\n"
"                _ds_state['live_worker'] = False\n"
"                for key in _ds_live_queue:\n"
"                    _ds_inflight.discard(key)\n"
"                del _ds_live_queue[:]\n"
"            _ds_wake.set()\n"
"    def _ds_ensure_fast_live_worker():\n"
"        with _ds_lock:\n"
"            if _ds_state['fast_live_worker']:\n"
"                return\n"
"            _ds_state['fast_live_worker'] = True\n"
"        try:\n"
"            _ds_fast_live_t = _ds_threading.Thread(target=_ds_live_loop, args=(_ds_fast_live_queue, _ds_fast_live_wake, 8))\n"
"            _ds_fast_live_t.daemon = True\n"
"            _ds_fast_live_t.start()\n"
"        except Exception as exc:\n"
"            _ds_report_exception(exc, 'start-fast-live-worker')\n"
"            with _ds_lock:\n"
"                _ds_state['fast_live_worker'] = False\n"
"                for key in _ds_fast_live_queue:\n"
"                    _ds_inflight.discard(key)\n"
"                del _ds_fast_live_queue[:]\n"
"            _ds_wake.set()\n"
"    # Visible say/menu text bypasses background UI work via the priority lookup path.\n"
"    def _ds_translate(s, priority=False):\n"
"        try:\n"
"            if not s or _ds_has_cjk(s):\n"
"                return s\n"
"            out = _ds_fetch(s, priority)\n"
"            if out:\n"
"                return _ds_restore_renpy_tokens(s, out)\n"
"            return s\n"
"        except Exception as exc:\n"
"            _ds_report_exception(exc)\n"
"            return s\n"
"    # Collect Ren'Py interpolation and text-tag spans such as [ruler] and {i}.\n"
"    def _ds_collect_renpy_spans(s, open_ch, close_ch):\n"
"        spans = []\n"
"        try:\n"
"            i = 0\n"
"            while len(spans) < 64:\n"
"                start = s.find(open_ch, i)\n"
"                if start < 0:\n"
"                    break\n"
"                if open_ch == '[' and start + 1 < len(s) and s[start + 1] == '[':\n"
"                    i = start + 2\n"
"                    continue\n"
"                end = s.find(close_ch, start + 1)\n"
"                if end < 0:\n"
"                    break\n"
"                inner = s[start + 1:end]\n"
"                if inner and len(inner) <= 96 and '\\n' not in inner and '\\r' not in inner:\n"
"                    spans.append(s[start:end + 1])\n"
"                i = end + 1\n"
"        except Exception as exc:\n"
"            _ds_report_exception(exc)\n"
"        return spans\n"
"    # Replace translated bracket/tag spans positionally with the original safe spans.\n"
"    def _ds_restore_span_sequence(src, out, open_ch, close_ch):\n"
"        src_spans = _ds_collect_renpy_spans(src, open_ch, close_ch)\n"
"        if not src_spans or open_ch not in out:\n"
"            return out\n"
"        try:\n"
"            parts = []\n"
"            i = 0\n"
"            n = 0\n"
"            while n < len(src_spans):\n"
"                start = out.find(open_ch, i)\n"
"                if start < 0:\n"
"                    break\n"
"                end = out.find(close_ch, start + 1)\n"
"                if end < 0:\n"
"                    break\n"
"                inner = out[start + 1:end]\n"
"                if not inner or len(inner) > 96 or '\\n' in inner or '\\r' in inner:\n"
"                    i = end + 1\n"
"                    continue\n"
"                parts.append(out[i:start])\n"
"                parts.append(src_spans[n])\n"
"                i = end + 1\n"
"                n += 1\n"
"            if not parts:\n"
"                return out\n"
"            parts.append(out[i:])\n"
"            return ''.join(parts)\n"
"        except Exception as exc:\n"
"            _ds_report_exception(exc)\n"
"            return out\n"
"    # MT can localize [variables] or {text tags}; restore them before Ren'Py evaluates text.\n"
"    def _ds_restore_renpy_tokens(src, out):\n"
"        try:\n"
"            if not out or out == src:\n"
"                return out\n"
"            for open_ch, close_ch in (('[', ']'), ('{', '}')):\n"
"                src_spans = _ds_collect_renpy_spans(src, open_ch, close_ch)\n"
"                out_spans = _ds_collect_renpy_spans(out, open_ch, close_ch)\n"
"                if not src_spans and out_spans:\n"
"                    _ds_report_diagnostic('renpy-token-injection', 'mt-introduced %s%s spans rejected; source preserved' % (open_ch, close_ch))\n"
"                    return src\n"
"                if src_spans and len(out_spans) != len(src_spans):\n"
"                    _ds_report_diagnostic('renpy-token-mismatch', '%s%s span count %d != %d; source preserved' % (open_ch, close_ch, len(out_spans), len(src_spans)))\n"
"                    return src\n"
"            out = _ds_restore_span_sequence(src, out, '[', ']')\n"
"            out = _ds_restore_span_sequence(src, out, '{', '}')\n"
"            return out\n"
"        except Exception as exc:\n"
"            _ds_report_exception(exc)\n"
"            return out\n"
"    # Ren'Py old_substitutions treats '%' as formatting; escape literal translated percent signs.\n"
"    def _ds_protect_old_percent(s):\n"
"        try:\n"
"            if not renpy.config.old_substitutions or '%' not in s:\n"
"                return s\n"
"            out = []\n"
"            i = 0\n"
"            while i < len(s):\n"
"                ch = s[i]\n"
"                if ch == '%':\n"
"                    nxt = s[i + 1] if i + 1 < len(s) else ''\n"
"                    if nxt == '%':\n"
"                        out.append('%%')\n"
"                        i += 2\n"
"                        continue\n"
"                    if nxt == '(':\n"
"                        out.append('%')\n"
"                        i += 1\n"
"                        continue\n"
"                    out.append('%%')\n"
"                    i += 1\n"
"                    continue\n"
"                out.append(ch)\n"
"                i += 1\n"
"            return ''.join(out)\n"
"        except Exception as exc:\n"
"            _ds_report_exception(exc)\n"
"            return s\n"
"    # Direct say calls still pass through the original Ren'Py say implementation.\n"
"    def _ds_say(who, what, *args, **kwargs):\n"
"        return _ds_old_say(who, _ds_protect_old_percent(_ds_translate(what, True)), *args, **kwargs)\n"
"    renpy.exports.say = _ds_say\n"
"    try:\n"
"        renpy.say = _ds_say\n"
"    except Exception as exc:\n"
"        _ds_report_exception(exc)\n"
"    # Python script snippets can call Character objects directly, bypassing renpy.exports.say.\n"
"    def _ds_install_character_call_hook():\n"
"        try:\n"
"            import renpy.character as _ds_character\n"
"            _ds_cls = getattr(_ds_character, 'ADVCharacter', None)\n"
"            if _ds_cls is None or getattr(_ds_cls, '_ds_deepseek_call_hooked', False):\n"
"                return\n"
"            _ds_old_character_call = _ds_cls.__call__\n"
"            def _ds_character_call(self, what, *args, **kwargs):\n"
"                try:\n"
"                    if isinstance(what, _ds_string_types):\n"
"                        what = _ds_protect_old_percent(_ds_translate(what, True))\n"
"                except Exception as exc:\n"
"                    _ds_report_exception(exc)\n"
"                return _ds_old_character_call(self, what, *args, **kwargs)\n"
"            _ds_cls.__call__ = _ds_character_call\n"
"            _ds_cls._ds_deepseek_call_hooked = True\n"
"            _ds_cls._ds_deepseek_old_call = _ds_old_character_call\n"
"        except Exception as exc:\n"
"            _ds_report_exception(exc)\n"
"    _ds_install_character_call_hook()\n"
"    # Compiled Say/Menu AST nodes use this official filter before rendering labels/text.\n"
"    try:\n"
"        _ds_prev_say_menu_text_filter = renpy.config.say_menu_text_filter\n"
"        def _ds_say_menu_text_filter(s):\n"
"            if _ds_prev_say_menu_text_filter is not None:\n"
"                try:\n"
"                    s = _ds_prev_say_menu_text_filter(s)\n"
"                except Exception as exc:\n"
"                    _ds_report_exception(exc)\n"
"            return _ds_protect_old_percent(_ds_translate(s, True))\n"
"        renpy.config.say_menu_text_filter = _ds_say_menu_text_filter\n"
"    except Exception as exc:\n"
"        _ds_report_exception(exc)\n"
"    # Prime a complete AST menu before Ren'Py filters individual labels. This lifecycle seam\n"
"    # keeps visible choices together and lets them bypass unrelated UI batches.\n"
"    def _ds_install_menu_execute_hook():\n"
"        try:\n"
"            import renpy.ast as _ds_ast\n"
"            _ds_menu_cls = getattr(_ds_ast, 'Menu', None)\n"
"            if _ds_menu_cls is None:\n"
"                raise RuntimeError('renpy.ast.Menu is unavailable')\n"
"            if getattr(_ds_menu_cls, '_ds_deepseek_menu_hooked', False):\n"
"                return\n"
"            _ds_old_menu_execute = _ds_menu_cls.execute\n"
"            def _ds_menu_execute(self, *args, **kwargs):\n"
"                try:\n"
"                    labels = [item[0] for item in self.items if item and isinstance(item[0], _ds_string_types)]\n"
"                    _ds_note_pending_many(labels, True)\n"
"                except Exception as exc:\n"
"                    _ds_report_exception(exc, 'prime-menu-labels')\n"
"                return _ds_old_menu_execute(self, *args, **kwargs)\n"
"            _ds_menu_cls.execute = _ds_menu_execute\n"
"            _ds_menu_cls._ds_deepseek_menu_hooked = True\n"
"            _ds_menu_cls._ds_deepseek_old_execute = _ds_old_menu_execute\n"
"        except Exception as exc:\n"
"            _ds_report_exception(exc, 'install-menu-prime-hook')\n"
"    _ds_install_menu_execute_hook()\n"
"    # Compiled-only games still expose every AST menu through Script.namemap. Prefetch\n"
"    # those exact labels at the Ren'Py start lifecycle before the player reaches them.\n"
"    def _ds_collect_script_menu_labels(limit=4096):\n"
"        try:\n"
"            import renpy.ast as _ds_ast\n"
"            _ds_menu_cls = getattr(_ds_ast, 'Menu', None)\n"
"            _ds_script = getattr(getattr(renpy, 'game', None), 'script', None)\n"
"            _ds_namemap = getattr(_ds_script, 'namemap', None)\n"
"            if _ds_menu_cls is None or _ds_namemap is None:\n"
"                raise RuntimeError('RenPy compiled script menu inventory is unavailable')\n"
"            _ds_nodes = _ds_namemap.values()\n"
"        except Exception as exc:\n"
"            _ds_report_exception(exc, 'collect-script-menu-inventory')\n"
"            return []\n"
"        labels = []\n"
"        seen = set()\n"
"        for _ds_node in _ds_nodes:\n"
"            try:\n"
"                if not isinstance(_ds_node, _ds_menu_cls):\n"
"                    continue\n"
"                for _ds_item in getattr(_ds_node, 'items', ()):\n"
"                    _ds_label = _ds_item[0] if _ds_item else None\n"
"                    if not isinstance(_ds_label, _ds_string_types) or not _ds_label or _ds_has_cjk(_ds_label) or _ds_label in seen:\n"
"                        continue\n"
"                    seen.add(_ds_label)\n"
"                    labels.append(_ds_label)\n"
"                    if len(labels) >= limit:\n"
"                        _ds_report_diagnostic('menu-prefetch-capacity', 'limit=%d' % limit)\n"
"                        return labels\n"
"            except Exception as exc:\n"
"                _ds_report_exception(exc, 'collect-script-menu-node')\n"
"        return labels\n"
"    def _ds_prefetch_script_menus():\n"
"        labels = _ds_collect_script_menu_labels()\n"
"        for _ds_start in range(0, len(labels), 128):\n"
"            try:\n"
"                _ds_http('/prefetch', {'texts': labels[_ds_start:_ds_start + 128]}, 2.0)\n"
"            except Exception as exc:\n"
"                _ds_report_exception(exc, 'prefetch-script-menu-batch')\n"
"                break\n"
"    def _ds_start_script_menu_prefetch():\n"
"        with _ds_lock:\n"
"            if _ds_state['menu_prefetch']:\n"
"                return\n"
"            _ds_state['menu_prefetch'] = True\n"
"        try:\n"
"            _ds_menu_prefetch_t = _ds_threading.Thread(target=_ds_prefetch_script_menus)\n"
"            _ds_menu_prefetch_t.daemon = True\n"
"            _ds_menu_prefetch_t.start()\n"
"        except Exception as exc:\n"
"            _ds_report_exception(exc, 'start-script-menu-prefetch')\n"
"            with _ds_lock:\n"
"                _ds_state['menu_prefetch'] = False\n"
"    try:\n"
"        _ds_start_callbacks = getattr(renpy.config, 'start_callbacks', None)\n"
"        if _ds_start_callbacks is None:\n"
"            raise RuntimeError('renpy.config.start_callbacks is unavailable')\n"
"        _ds_start_callbacks.append(_ds_start_script_menu_prefetch)\n"
"    except Exception as exc:\n"
"        _ds_report_exception(exc, 'register-script-menu-prefetch')\n"
"    # Prefer the deployed CJK font for all dialogue/UI styles and known game fonts.\n"
"    _ds_font = None\n"
"    for _ds_cand, _ds_spec in (('ds_font.ttf', u'ds_font.ttf'), ('ds_font.otf', u'ds_font.otf'), ('ds_font.ttc', u'0@ds_font.ttc')):\n"
"        if _ds_os.path.exists(_ds_os.path.join(renpy.config.gamedir, _ds_cand)):\n"
"            _ds_font = _ds_spec\n"
"            break\n"
"    if _ds_font:\n"
"        try:\n"
"            _ds_all_styles = list(renpy.style.styles.values())\n"
"        except Exception as exc:\n"
"            _ds_report_exception(exc)\n"
"            _ds_all_styles = []\n"
"        for _ds_st in _ds_all_styles:\n"
"            try:\n"
"                _ds_st.font = _ds_font\n"
"            except Exception as exc:\n"
"                _ds_report_exception(exc)\n"
"        for _ds_style_name in ('default', 'say_dialogue', 'say_label', 'say_thought', 'centered_text', 'nvl_dialogue', 'nvl_label', 'nvl_thought'):\n"
"            try:\n"
"                getattr(style, _ds_style_name).font = _ds_font\n"
"            except Exception as exc:\n"
"                _ds_report_exception(exc)\n"
"        try:\n"
"            _ds_fonts = set([u'DejaVuSans.ttf'])\n"
"            for _ds_gv in ('text_font', 'name_text_font', 'interface_text_font', 'button_text_font', 'choice_button_text_font', 'label_text_font', 'prompt_text_font'):\n"
"                try:\n"
"                    _ds_val = getattr(gui, _ds_gv, None)\n"
"                    if _ds_val:\n"
"                        _ds_fonts.add(_ds_val)\n"
"                except Exception as exc:\n"
"                    _ds_report_exception(exc)\n"
"            try:\n"
"                for _ds_fn in _ds_os.listdir(renpy.config.gamedir):\n"
"                    _ds_low = _ds_fn.lower()\n"
"                    if (_ds_low.endswith('.ttf') or _ds_low.endswith('.otf')) and not _ds_low.startswith('ds_font'):\n"
"                        _ds_fonts.add(_ds_fn)\n"
"            except Exception as exc:\n"
"                _ds_report_exception(exc)\n"
"            for _ds_fn in _ds_fonts:\n"
"                for _ds_b in (False, True):\n"
"                    for _ds_i in (False, True):\n"
"                        renpy.config.font_replacement_map[(_ds_fn, _ds_b, _ds_i)] = (_ds_font, _ds_b, _ds_i)\n"
"        except Exception as exc:\n"
"            _ds_report_exception(exc)\n"
"    # UI displayables go through replace_text and are memoized to avoid per-frame HTTP.\n"
"    def _ds_wants_ui_text(s):\n"
"        if not s or len(s) < 2 or len(s) > 1200 or _ds_has_cjk(s):\n"
"            return False\n"
"        alpha = 0\n"
"        digits = 0\n"
"        for ch in s:\n"
"            if ('a' <= ch <= 'z') or ('A' <= ch <= 'Z'):\n"
"                alpha += 1\n"
"            elif '0' <= ch <= '9':\n"
"                digits += 1\n"
"        return alpha >= 2 and digits < 4\n"
"    def _ds_replace_text(s):\n"
"        try:\n"
"            if not _ds_wants_ui_text(s):\n"
"                return s\n"
"            out = _ds_fetch(s)\n"
"            if out:\n"
"                return _ds_protect_old_percent(_ds_restore_renpy_tokens(s, out))\n"
"            return s\n"
"        except Exception as exc:\n"
"            _ds_report_exception(exc)\n"
"            return s\n"
"    # Chain with any game-provided replace_text hook instead of replacing it outright.\n"
"    if _ds_font:\n"
"        try:\n"
"            if hasattr(renpy.config, 'replace_text'):\n"
"                _ds_prev_replace = renpy.config.replace_text\n"
"                def _ds_chain_replace(s):\n"
"                    if _ds_prev_replace is not None:\n"
"                        try:\n"
"                            s = _ds_prev_replace(s)\n"
"                        except Exception as exc:\n"
"                            _ds_report_exception(exc)\n"
"                    return _ds_replace_text(s)\n"
"                renpy.config.replace_text = _ds_chain_replace\n"
"        except Exception as exc:\n"
"            _ds_report_exception(exc)\n"
"    # Existing Text displayables cache tokenized layouts; retain weak references so a\n"
"    # later async cache hit can invalidate only live Ren'Py text on the main thread.\n"
"    def _ds_install_text_displayable_hook():\n"
"        try:\n"
"            _ds_text_cls = renpy.text.text.Text\n"
"            if getattr(_ds_text_cls, '_ds_deepseek_init_hooked', False):\n"
"                return\n"
"            _ds_old_text_init = _ds_text_cls.__init__\n"
"            def _ds_text_init(self, *args, **kwargs):\n"
"                _ds_old_text_init(self, *args, **kwargs)\n"
"                try:\n"
"                    _ds_text_displayables.add(self)\n"
"                except Exception as exc:\n"
"                    _ds_report_exception(exc, 'track-text-displayable')\n"
"            _ds_text_cls.__init__ = _ds_text_init\n"
"            _ds_text_cls._ds_deepseek_init_hooked = True\n"
"            _ds_text_cls._ds_deepseek_old_init = _ds_old_text_init\n"
"        except Exception as exc:\n"
"            _ds_report_exception(exc, 'install-text-displayable-hook')\n"
"    _ds_install_text_displayable_hook()\n";

/* ----------------------------------------------------------------
 * RPGM_HOOK — RPG Maker MV/MZ 翻译 JavaScript 脚本（嵌入源码）
 *
 * 功能概述：
 *   1. Hook Window_Base.drawTextEx 和 drawText，将显示文本发送到本地服务器翻译
 *   2. 使用 cache_only 模式，仅缓存命中时替换（不阻塞游戏）
 *   3. 注入 CJK @font-face（ds_font.ttf/ttc），确保中文能正确渲染
 *   4. 覆盖 Window_Base.standardFontFace 和 Game_System.mainFontFace，
 *      将 CJK 字体设为首选
 *   5. 统一插件外部 CRLF 文本的缓存键，并为重复 miss 设置短冷却
 * ---------------------------------------------------------------- */
static const char RPGM_HOOK[] =
"(function(){\n"
"  'use strict';\n"
"  var URL='http://127.0.0.1:19999/translate';\n"
"  var BATCH_URL='http://127.0.0.1:19999/batch';\n"
"  var CACHE_LOOKUP_URL='http://127.0.0.1:19999/cache/lookup';\n"
"  var CJK_FONT='DeepSeekCJK';\n"
"  var cache=Object.create(null);\n"
"  var pending=Object.create(null);\n"
"  var pendingCount=0;\n"
"  var retryAfter=Object.create(null);\n"
"  var MISS_RETRY_MS=1000;\n"
"  var MAX_PENDING=64;\n"
"  var MESSAGE_PAGE_BATCH_MAX=32;\n"
"  var MESSAGE_PAGE_CHAR_BUDGET=6000;\n"
"  var MAX_MESSAGE_PAGE_PRIME=192;\n"
"  var MAX_QUEST_PRIME=128;\n"
"  var QUEST_PRIME_BATCH=16;\n"
"  var messagePagePending=Object.create(null);\n"
"  var suppressLookupRequests=0;\n"
"  var localLookupTried=Object.create(null);\n"
"  var localLookupPending=Object.create(null);\n"
"  var localLookupOrder=[];\n"
"  var ACTIVE_MESSAGE_POLL_INITIAL_MS=120;\n"
"  var ACTIVE_MESSAGE_POLL_MAX_MS=1000;\n"
"  var ACTIVE_MESSAGE_POLL_WINDOW_MS=15000;\n"
"  var activeMessagePoll={win:null,signature:'',deadline:0,delay:0,timer:false,generation:0};\n"
"  var rpgmErrorCounts=Object.create(null);\n"
"  // Compatibility fallbacks cover optional plugin/renderer APIs that cannot be fixed upstream.\n"
"  // They never make a failed translation successful: each failure is counted and logged,\n"
"  // and a missing diagnostic console rethrows instead of hiding the original exception.\n"
"  function reportRpgmError(context,error){var e=error instanceof Error?error:new Error(String(error)); var key=String(context||'unknown')+':'+(e.name||'Error'); var count=(rpgmErrorCounts[key]||0)+1; rpgmErrorCounts[key]=count; if(count<=3||(count&(count-1))===0){var detail=count===1&&(e.stack||e.message)?(e.stack||e.message):e.message; if(window.console&&typeof console.error==='function'){console.error('[DeepSeek RPGM] '+context+' failed #'+count+': '+detail);}else{throw e;}}}\n"
"  // NW.js exposes the game as a chrome-extension page. Recent Chromium builds can leave\n"
"  // localhost XHR behind their private-network/CORS boundary without a renderer callback,\n"
"  // while the embedded Node transport remains available in the same game process. This cannot\n"
"  // be repaired in game/plugin code upstream. A Node failure falls back to async XHR, is reported\n"
"  // through reportRpgmError, and returns status 0/source text; it never creates a cache success.\n"
"  function rpgmEndpointPath(endpoint){var match=/^https?:\\/\\/[^/]+(\\/.*)$/.exec(String(endpoint||'')); return match?match[1]:'/';}\n"
"  function sendRpgmJson(endpoint,payload,timeoutMs,context,callback){var body=JSON.stringify(payload),nodeRuntime=false; try{nodeRuntime=typeof require==='function'&&typeof process==='object'&&process.versions&&!!process.versions.nw; if(nodeRuntime){var http=require('http'),completed=false; function complete(response,eventName){if(completed) return; completed=true; callback(response,eventName);} var req=http.request({hostname:'127.0.0.1',port:19999,path:rpgmEndpointPath(endpoint),method:'POST',headers:{'Content-Type':'application/json','Content-Length':Buffer.byteLength(body,'utf8')}},function(res){var responseText='',oversized=false; res.setEncoding('utf8'); res.on('data',function(chunk){if(oversized) return; responseText+=chunk; if(responseText.length>8388608){oversized=true; req.destroy(new Error('response exceeds 8 MiB'));}}); res.on('end',function(){if(!oversized) complete({status:Number(res.statusCode)||0,responseText:responseText},'node-http');}); res.on('error',function(error){reportRpgmError('node-http-response-'+String(context||'request'),error); complete({status:0,responseText:''},'node-response-error');});}); req.setTimeout(Number(timeoutMs)||12000,function(){req.destroy(new Error('timeout'));}); req.on('error',function(error){reportRpgmError('node-http-'+String(context||'request'),error); complete({status:0,responseText:''},error&&error.message==='timeout'?'timeout':'node-error');}); req.end(body); return;}}catch(e){reportRpgmError('start-node-http-'+String(context||'request'),e);} var x=null,done=false; function finish(eventName){if(done) return; done=true; callback(x,eventName);} try{x=new XMLHttpRequest(); x.open('POST',endpoint,true); x.timeout=Number(timeoutMs)||12000; x.setRequestHeader('Content-Type','application/json'); x.onreadystatechange=function(){if(x.readyState===4) finish('readystatechange');}; x.onload=function(){finish('load');}; x.onerror=function(){finish('error');}; x.ontimeout=function(){finish('timeout');}; x.send(body);}catch(e){reportRpgmError('start-xhr-'+String(context||'request'),e); finish('constructor');}}\n"
"  function installCjkFont(){\n"
"    try{if(!installCjkFont._dsCssInjected){var st=document.createElement('style'); st.type='text/css'; st.textContent=\"@font-face{font-family:'DeepSeekCJK';src:url('fonts/ds_font.ttf') format('truetype'),url('fonts/ds_font.ttc') format('truetype');font-weight:normal;font-style:normal;} body,canvas{font-family:'DeepSeekCJK',sans-serif;}\"; (document.head||document.documentElement).appendChild(st); installCjkFont._dsCssInjected=true; if(document.fonts&&document.fonts.load){document.fonts.load('16px '+CJK_FONT);}}}\n"
"    catch(e){reportRpgmError('install-css-font',e);}\n"
"    try{if(window.Window_Base&&Window_Base.prototype.standardFontFace&&!Window_Base.prototype._dsStandardFontFace){var oldFont=Window_Base.prototype.standardFontFace; Window_Base.prototype._dsStandardFontFace=oldFont; Window_Base.prototype.standardFontFace=function(){var base=oldFont.call(this)||''; return base.indexOf(CJK_FONT)>=0?base:(base?CJK_FONT+', '+base:CJK_FONT);};}}\n"
"    catch(e){reportRpgmError('install-mv-font-face',e);}\n"
"    try{if(window.Game_System&&Game_System.prototype.mainFontFace&&!Game_System.prototype._dsMainFontFace){var oldMain=Game_System.prototype.mainFontFace; Game_System.prototype._dsMainFontFace=oldMain; Game_System.prototype.mainFontFace=function(){var base=oldMain.call(this)||''; return base.indexOf(CJK_FONT)>=0?base:(base?CJK_FONT+', '+base:CJK_FONT);};}}\n"
"    catch(e){reportRpgmError('install-mz-font-face',e);}\n"
"  }\n"
"  installCjkFont();\n"
"  function hasCjk(s){return /[\\u4e00-\\u9fff\\u3000-\\u303f\\uff00-\\uffef]/.test(String(s||''));}\n"
"  function keyOf(s){return String(s==null?'':s).replace(/^[ \\t\\r\\n]+|[ \\t\\r\\n]+$/g,'');}\n"
"  function isLayoutOnly(s){var t=String(s==null?'':s); t=t.replace(/(?:<WordWrap>|<br\\s*\\/?>)/gi,''); t=t.replace(/\\\\(?:c|i|fs|fr|fb|fi|oc|ow|v|n[1-5]?|nc|nr|nd[1-5]?|ndc|ndr|nt[1-5]?|ntc|ntr)(?:\\[[^\\]]{0,64}\\]|<[^>\\n]{0,64}>)/gi,''); t=t.replace(/\\\\[{}$.|!><^]/g,''); return !/[A-Za-z\\u0080-\\uffff]/.test(t);}\n"
"  function rpgmControlTokens(s){return String(s==null?'':s).match(/(?:<WordWrap>|<br\\s*\\/?>|\\\\(?:c|i|fs|fr|fb|fi|oc|ow|v|n[1-5]?|nc|nr|nd[1-5]?|ndc|ndr|nt[1-5]?|ntc|ntr)(?:\\[[^\\]]{0,64}\\]|<[^>\\n]{0,64}>))/gi)||[];}\n"
"  function rpgmLatinWords(s){var t=String(s==null?'':s); t=t.replace(/(?:<WordWrap>|<br\\s*\\/?>|\\\\(?:c|i|fs|fr|fb|fi|oc|ow|v|n[1-5]?|nc|nr|nd[1-5]?|ndc|ndr|nt[1-5]?|ntc|ntr)(?:\\[[^\\]]{0,64}\\]|<[^>\\n]{0,64}>))/gi,' '); return t.toLowerCase().match(/[a-z][a-z'-]{2,}/g)||[];}\n"
"  function hasSuspiciousRpgmSourceResidue(src,out){if(!hasCjk(out)) return false; var a=rpgmLatinWords(src),b=rpgmLatinWords(out); if(a.length<4||b.length<3) return false; for(var i=0;i+2<a.length;i++){for(var j=0;j+2<b.length;j++){if(a[i]===b[j]&&a[i+1]===b[j+1]&&a[i+2]===b[j+2]) return true;}} return false;}\n"
"  function safeRpgmTranslation(src,out){src=String(src==null?'':src); out=String(out==null?'':out); if(!out||out===src) return out||src; var a=rpgmControlTokens(src),b=rpgmControlTokens(out); if(a.length!==b.length) return src; for(var i=0;i<a.length;i++){if(String(a[i]).toLowerCase()!==String(b[i]).toLowerCase()) return src;} if(hasSuspiciousRpgmSourceResidue(src,out)) return src; return out;}\n"
"  function cacheSafeRpgmTranslation(key,value,context){var residue=hasSuspiciousRpgmSourceResidue(key,value),safe=safeRpgmTranslation(key,value); if(safe&&safe!==key){cache[key]=safe; delete retryAfter[key]; return true;} if(value&&value!==key) reportRpgmLayoutFallback(residue?'mixed-language-guard':(context||'cache-control-guard'),'cache result rejected'); return false;}\n"
"  function markRpgmDisplayTarget(target,text){try{var key=keyOf(text); if(!target||!key||hasCjk(key)||!/[A-Za-z]/.test(key)) return; target._dsRpgmTranslationTarget=true; if(target.contents) target.contents._dsRpgmTranslationTarget=true;}catch(e){reportRpgmError('mark-display-target',e);}}\n"
"  function canAutoRefreshWindow(root){if(!root||root.visible===false||root._destroyed||root._closing) return false; if(typeof root.openness==='number'&&root.openness<=0&&!root._opening) return false; if(root.contents&&root.contents._destroyed) return false; return !!(root._dsRpgmTranslationTarget||(root.contents&&root.contents._dsRpgmTranslationTarget));}\n"
"  function isPersistenceScene(scene){try{if(!scene) return false; if(window.Scene_File&&scene instanceof Scene_File) return true; var mode=typeof scene.mode==='function'?scene.mode():scene._mode; return mode==='save'||mode==='load';}catch(e){reportRpgmError('detect-persistence-scene',e); return false;}}\n"
"  var refreshQueued=false;\n"
"  function messageRenderSignature(source,translated){return source.join('\\u001e')+'\\u001f'+translated.join('\\u001e');}\n"
"  function activeMessageSourceSignature(lines){var out=[]; for(var i=0;i<lines.length;i++) out.push(String(lines[i]==null?'':lines[i])); return out.join('\\u001e');}\n"
"  function stopActiveMessageCachePoll(win){if(win&&activeMessagePoll.win!==win) return; activeMessagePoll.win=null; activeMessagePoll.signature=''; activeMessagePoll.deadline=0; activeMessagePoll.delay=0; activeMessagePoll.timer=false; activeMessagePoll.generation++;}\n"
"  // The local server intentionally returns queued source text immediately and completes remote\n"
"  // translation later, so the renderer has no completion callback to subscribe to upstream.\n"
"  // While the same source page remains visible, bounded backoff polls only /cache/lookup. A miss\n"
"  // never becomes a cache success, page changes cancel the generation, and timeout is diagnosed.\n"
"  function scheduleActiveMessageCachePoll(win,lines){try{if(!win||!win._textState||win.visible===false||win._destroyed||win._closing||!lines||!lines.length) return; var signature=activeMessageSourceSignature(lines),now=Date.now(); if(activeMessagePoll.win!==win||activeMessagePoll.signature!==signature){activeMessagePoll.win=win; activeMessagePoll.signature=signature; activeMessagePoll.deadline=now+ACTIVE_MESSAGE_POLL_WINDOW_MS; activeMessagePoll.delay=ACTIVE_MESSAGE_POLL_INITIAL_MS; activeMessagePoll.timer=false; activeMessagePoll.generation++;} if(now>=activeMessagePoll.deadline){reportRpgmError('active-message-cache-poll-timeout',new Error('translation stayed unresolved for visible message page')); stopActiveMessageCachePoll(win); return;} if(activeMessagePoll.timer) return; activeMessagePoll.timer=true; var generation=activeMessagePoll.generation,delay=activeMessagePoll.delay; setTimeout(function(){try{if(generation!==activeMessagePoll.generation) return; activeMessagePoll.timer=false; var gm=window.$gameMessage,current=gm&&gm._texts; if(activeMessagePoll.win!==win||!current||!Array.isArray(current)||activeMessageSourceSignature(current)!==signature||!win._textState||win.visible===false||win._destroyed||win._closing){stopActiveMessageCachePoll(win); return;} var plan=planMessagePage(current),keys=[plan.joined].concat(plan.candidates,plan.liveCandidates),seen=Object.create(null),lookupKeys=[]; for(var i=0;i<keys.length;i++){var key=keyOf(keys[i]); if(!key||seen[key]||hasCjk(key)||isLayoutOnly(key)) continue; seen[key]=true; if(!localLookupPending[key]) delete localLookupTried[key]; lookupKeys.push(key);} activeMessagePoll.delay=Math.min(ACTIVE_MESSAGE_POLL_MAX_MS,Math.max(ACTIVE_MESSAGE_POLL_INITIAL_MS,delay*2)); if(!requestLocalCacheKeys(lookupKeys,512)) scheduleActiveMessageCachePoll(win,current);}catch(e){reportRpgmError('run-active-message-cache-poll',e); stopActiveMessageCachePoll(win);}},delay);}catch(e){reportRpgmError('schedule-active-message-cache-poll',e); stopActiveMessageCachePoll(win);}}\n"
"  function refreshActiveRpgmMessage(win){try{var gm=window.$gameMessage,original=gm&&gm._texts; if(!win||win.visible===false||win._destroyed||win._closing) return; if(!original||!Array.isArray(original)||!original.length){win._dsRpgmDeferredMessageStart=false; stopActiveMessageCachePoll(win); return;} if(typeof win.openness==='number'&&win.openness<=0&&!win._opening) return; if(!win._textState){if(win._dsRpgmDeferredMessageStart&&typeof win.startMessage==='function'){win._dsRpgmDeferredMessageStart=false; win.startMessage();} return;} var translated=translateMessageLines(original,true); if(!translated||sameRpgmTextArray(original,translated)){scheduleActiveMessageCachePoll(win,original); return;} stopActiveMessageCachePoll(win); var signature=messageRenderSignature(original,translated); if(win._dsRpgmRenderedMessageSignature===signature) return; win._dsRpgmRenderedMessageSignature=signature; if(typeof win.startMessage==='function') win.startMessage();}catch(e){reportRpgmError('refresh-active-message',e);}}\n"
"  function walkSceneWindows(root,seen,state){if(!root||state.count>=2048||seen.indexOf(root)>=0) return; seen.push(root); state.count++; try{if(window.Window_Message&&root instanceof Window_Message) refreshActiveRpgmMessage(root); else if(canAutoRefreshWindow(root)&&typeof root.refresh==='function') root.refresh();}catch(e){reportRpgmError('refresh-rendered-window',e);} var children=root.children; if(children&&children.length){for(var i=0;i<children.length;i++) walkSceneWindows(children[i],seen,state);}}\n"
"  function walkSceneOwnedWindows(scene,seen,state){var names; try{names=Object.keys(scene||{});}catch(e){reportRpgmError('list-scene-owned-windows',e); return;} for(var i=0;i<names.length&&state.count<2048;i++){var name=names[i]; if(!/window/i.test(name)) continue; try{var owned=scene[name]; if(owned&&typeof owned==='object') walkSceneWindows(owned,seen,state);}catch(e){reportRpgmError('read-scene-owned-window',e);}}}\n"
"  function runWindowRefresh(){try{var scene=window.SceneManager&&SceneManager._scene; if(scene&&!isPersistenceScene(scene)){var seen=[],state={count:0}; walkSceneWindows(scene,seen,state); walkSceneOwnedWindows(scene,seen,state);}}catch(e){reportRpgmError('refresh-scene-windows',e);}finally{refreshQueued=false;}}\n"
"  function requestWindowRefresh(){if(refreshQueued) return; refreshQueued=true; try{if(window.setTimeout){setTimeout(runWindowRefresh,0); return;}}catch(e){reportRpgmError('schedule-window-refresh',e);} runWindowRefresh();}\n"
"  // A local cache reply can arrive while a freshly created title/menu window is still closed.\n"
"  // Window_Base.updateOpen is the engine-owned lifecycle boundary that makes it renderable;\n"
"  // refreshing exactly on that transition avoids a fixed delay and never touches save/load scenes.\n"
"  function installWindowOpenRefreshHook(){try{if(!window.Window_Base||!Window_Base.prototype.updateOpen||Window_Base.prototype.updateOpen._dsRpgmOpenRefreshHook) return; var oldUpdateOpen=Window_Base.prototype.updateOpen; var updateOpenWrap=function(){var wasOpen=typeof this.isOpen==='function'?this.isOpen():Number(this.openness)>=255; var result=oldUpdateOpen.apply(this,arguments); try{var nowOpen=typeof this.isOpen==='function'?this.isOpen():Number(this.openness)>=255; if(!wasOpen&&nowOpen&&canAutoRefreshWindow(this)&&typeof this.refresh==='function'){var scene=window.SceneManager&&SceneManager._scene; if(!isPersistenceScene(scene)) this.refresh();}}catch(e){reportRpgmError('refresh-window-open-transition',e);} return result;}; updateOpenWrap._dsRpgmOpenRefreshHook=true; updateOpenWrap._dsRpgmOriginal=oldUpdateOpen; Window_Base.prototype.updateOpen=updateOpenWrap;}catch(e){reportRpgmError('install-window-open-refresh-hook',e);}}\n"
"  function requestLookup(key){\n"
"    if(localLookupPending[key]||pending[key]||messagePagePending[key]||pendingCount>=MAX_PENDING) return; pending[key]=true; pendingCount++; var x=null; var done=false;\n"
"    function finish(response,eventName){if(done) return; done=true; x=response; if(pending[key]){delete pending[key]; pendingCount--;} var now=Date.now(); if(x&&x.status===200){try{var r=JSON.parse(x.responseText); var v=r.translated_text||r.translation||key; if(v&&v!==key&&r.source!=='miss'&&r.source!=='queued'&&r.source!=='pass'&&cacheSafeRpgmTranslation(key,v,'single-control-guard')){requestWindowRefresh(); return;}}catch(e){reportRpgmError('parse-translate-response',e);}}else{reportRpgmError('translate-request-'+String(eventName||'http'),new Error('HTTP status '+(x?x.status:'unavailable')));} retryAfter[key]=now+MISS_RETRY_MS;}\n"
"    sendRpgmJson(URL,{text:key},12000,'translate',finish);\n"
"  }\n"
"  function lookup(s){\n"
"    s=String(s==null?'':s); if(!s||hasCjk(s)||isLayoutOnly(s)) return {text:s,hit:true}; var key=keyOf(s); if(!key) return {text:s,hit:true}; if(cache[key]) return {text:safeRpgmTranslation(s,cache[key]),hit:true}; var now=Date.now(); if(retryAfter[key]&&retryAfter[key]>now) return {text:s,hit:false};\n"
"    if(!suppressLookupRequests) requestLookup(key); if(cache[key]) return {text:safeRpgmTranslation(s,cache[key]),hit:true};\n"
"    return {text:s,hit:false};\n"
"  }\n"
"  function splitRpgmDecor(s){\n"
"    s=String(s==null?'':s); var i=0,m; for(;;){var rest=s.slice(i); if((m=/^\\\\(?:pop|n[1-5]?|nc|nr|nd[1-5]?|ndc|ndr|nt[1-5]?|ntc|ntr)(?:\\[[^\\]]{0,64}\\]|<[^>\\n]{0,64}>)/i.exec(rest))){i+=m[0].length; continue;} if((m=/^<[^>\\n]{1,64}>/.exec(rest))){i+=m[0].length; continue;} break;}\n"
"    if(i<=0||i>=s.length) return null; var body=s.slice(i); if(!/[A-Za-z\\u0080-\\uffff]/.test(body)) return null; return {prefix:s.slice(0,i),body:body};\n"
"  }\n"
"  function dataName(kind,id){var t=null,k=String(kind||'').toLowerCase(); if(k==='ii'||k==='ni') t=window.$dataItems; else if(k==='iw'||k==='nw') t=window.$dataWeapons; else if(k==='ia'||k==='na') t=window.$dataArmors; else if(k==='is'||k==='ns') t=window.$dataSkills; else if(k==='it'||k==='nt') t=window.$dataStates; var o=t&&t[parseInt(id,10)]; return o&&o.name?String(o.name):'';}\n"
"  function visibleLookupKey(s){\n"
"    var changed=false; var text=String(s==null?'':s); text=text.replace(/\\\\(ii|iw|ia|is|it|ni|nw|na|ns|nt)\\[(\\d+)\\]/gi,function(m,c,id){var n=dataName(c,id); if(n){changed=true; return n;} return m;});\n"
"    text=text.replace(/\\\\v\\[(\\d+)\\]/gi,function(m,id){try{if(window.$gameVariables&&typeof $gameVariables.value==='function'){changed=true; return String($gameVariables.value(Number(id)));}}catch(e){reportRpgmError('resolve-game-variable',e);} return m;});\n"
"    text=text.replace(/\\\\(?:c|i|fs|fr|fb|fi|oc|ow)\\[[^\\]]{0,64}\\]/gi,function(){changed=true; return '';}); text=text.replace(/\\\\[{}$.|!><^]/g,function(){changed=true; return ' ';}); text=keyOf(text).replace(/[ \\t]{2,}/g,' ');\n"
"    return changed&&text?text:null;\n"
"  }\n"
"  var questPrimePending=false;\n"
"  var questPrimeSignature='';\n"
"  function primeGalvQuestCache(){\n"
"    if(questPrimePending) return; var qs=window.$gameSystem&&$gameSystem._quests; var list=qs&&qs.quest; if(!list) return; var keys=[],seen=Object.create(null);\n"
"    function add(value){if(keys.length>=MAX_QUEST_PRIME||value==null) return; var raw=keyOf(value); if(!raw||raw.length<3||hasCjk(raw)) return; if(!seen[raw]){seen[raw]=true; keys.push(raw);} var visible=visibleLookupKey(raw); if(visible&&!seen[visible]&&keys.length<MAX_QUEST_PRIME){seen[visible]=true; keys.push(visible);}}\n"
"    function addArray(values){if(!values||!values.length) return; for(var i=0;i<values.length&&keys.length<MAX_QUEST_PRIME;i++) add(values[i]);}\n"
"    for(var i=0;i<list.length&&keys.length<MAX_QUEST_PRIME;i++){var q=list[i]; if(!q) continue; try{var name=typeof q.name==='function'?q.name():q.name; add(name); var split=String(name||'').lastIndexOf(' - '); if(split>=0) add(String(name).slice(split+3));}catch(e){reportRpgmError('read-quest-name',e);} try{addArray(typeof q.objectives==='function'?q.objectives():q.objectives);}catch(e){reportRpgmError('read-quest-objectives',e);} try{addArray(typeof q.desc==='function'?q.desc():q.desc);}catch(e){reportRpgmError('read-quest-description',e);} try{addArray(typeof q.resoTxtArray==='function'?q.resoTxtArray():q.resoTxtArray);}catch(e){reportRpgmError('read-quest-resolution',e);}}\n"
"    if(!keys.length) return; var signature=keys.join('\\n'); if(signature===questPrimeSignature) return; questPrimePending=true; var remaining=Math.ceil(keys.length/QUEST_PRIME_BATCH),allSucceeded=true;\n"
"    function settle(x,batch,eventName){var succeeded=false,changed=false; if(x&&x.status===200){try{var r=JSON.parse(x.responseText),results=r.results||[],sources=r.sources||[]; succeeded=true; for(var i=0;i<batch.length;i++){var value=results[i],source=sources[i]; if(value&&value!==batch[i]&&source!=='miss'&&source!=='queued'&&source!=='pass'&&cacheSafeRpgmTranslation(batch[i],value,'quest-prime-control-guard')) changed=true;}}catch(e){reportRpgmError('parse-quest-prime-response',e);}}else{reportRpgmError('quest-prime-'+String(eventName||'http'),new Error('HTTP status '+(x?x.status:'unavailable')));} if(!succeeded) allSucceeded=false; if(changed) requestWindowRefresh(); remaining--; if(remaining<=0){questPrimePending=false; if(allSucceeded) questPrimeSignature=signature;}}\n"
"    for(var start=0;start<keys.length;start+=QUEST_PRIME_BATCH){(function(batch){var done=false; function finish(response,eventName){if(done) return; done=true; settle(response,batch,eventName);} sendRpgmJson(BATCH_URL,{texts:batch},15000,'quest-prime',finish);})(keys.slice(start,start+QUEST_PRIME_BATCH));}\n"
"  }\n"
"  function installQuestPrimeHooks(){try{if(window.DataManager&&DataManager.extractSaveContents&&!DataManager.extractSaveContents._dsQuestPrimeHook){var oldExtract=DataManager.extractSaveContents; var wrap=function(){var r=oldExtract.apply(this,arguments); primeGalvQuestCache(); return r;}; wrap._dsQuestPrimeHook=true; DataManager.extractSaveContents=wrap;} if(window.DataManager&&DataManager.setupNewGame&&!DataManager.setupNewGame._dsQuestPrimeHook){var oldSetup=DataManager.setupNewGame; var setupWrap=function(){var r=oldSetup.apply(this,arguments); primeGalvQuestCache(); return r;}; setupWrap._dsQuestPrimeHook=true; DataManager.setupNewGame=setupWrap;} primeGalvQuestCache();}catch(e){reportRpgmError('install-quest-prime-hooks',e);}}\n"
"  installQuestPrimeHooks();\n"
"  function lookupVisible(orig,prefix,body){var key=visibleLookupKey(body); if(!key||key===keyOf(body)) return null; var r=lookup(key); if(r.hit&&r.text!==key&&r.text!==keyOf(key)) return {text:String(prefix||'')+r.text,hit:true}; if(r.hit) return {text:orig,hit:true}; return {text:orig,hit:false};}\n"
"  function lookupDecorated(s){\n"
"    var orig=String(s==null?'':s); var raw=lookup(orig); if(raw.hit&&raw.text!==orig&&raw.text!==keyOf(orig)) return raw; var ww=/^(?:<WordWrap>)+/i.exec(orig); if(ww&&ww[0].length<orig.length){var inner=orig.slice(ww[0].length),wrapped=lookup(inner); if(wrapped.hit&&wrapped.text!==inner&&wrapped.text!==keyOf(inner)) return {text:ww[0]+wrapped.text,hit:true}; if(!wrapped.hit) raw.hit=false;} var parts=splitRpgmDecor(orig); if(parts){var body=lookup(parts.body); if(body.hit&&body.text!==parts.body&&body.text!==keyOf(parts.body)) return {text:parts.prefix+body.text,hit:true}; var vis=lookupVisible(orig,parts.prefix,parts.body); if(vis) return vis; if(body.hit&&raw.hit) return {text:orig,hit:true}; return {text:orig,hit:false};} var vis2=lookupVisible(orig,'',orig); if(vis2) return vis2; return raw;\n"
"  }\n"
"  function tr(s){return lookupDecorated(s).text;}\n"
"  var suppressDisplayTranslate=0;\n"
"  // Local-cache lookup is asynchronous so renderer hooks never block the RPG Maker frame.\n"
"  // Its bounded transport timeout clears only the short local gate; misses and failures remain\n"
"  // source text and may continue through the existing foreground translation path.\n"
"  function requestLocalCacheKeys(values,maxKeys){var keys=[],seen=Object.create(null),limit=Number(maxKeys)||64; if(limit<1) limit=64; if(limit>512) limit=512; for(var i=0;i<values.length&&keys.length<limit;i++){var key=keyOf(values[i]); if(!key||cache[key]||localLookupTried[key]||localLookupPending[key]||seen[key]||hasCjk(key)||isLayoutOnly(key)||!/[A-Za-z]/.test(key)) continue; seen[key]=true; localLookupTried[key]=true; localLookupPending[key]=true; localLookupOrder.push(key); keys.push(key);} while(localLookupOrder.length>4096){var old=localLookupOrder.shift(); if(!localLookupPending[old]) delete localLookupTried[old];} for(var start=0;start<keys.length;start+=64){(function(batch){var done=false; function finish(response,eventName){if(done) return; done=true; for(var i=0;i<batch.length;i++) delete localLookupPending[batch[i]]; if(response&&response.status===200){try{var r=JSON.parse(response.responseText),hits=r.hits||{}; for(var j=0;j<batch.length;j++){var key=batch[j],value=hits[key]; if(value&&value!==key) cacheSafeRpgmTranslation(key,value,'async-cache-control-guard');}}catch(e){reportRpgmError('parse-async-cache-lookup',e);}}else{reportRpgmError('async-cache-lookup-'+String(eventName||'http'),new Error('HTTP status '+(response?response.status:'unavailable')));} requestWindowRefresh();} sendRpgmJson(CACHE_LOOKUP_URL,{texts:batch},1500,'cache-lookup',finish);})(keys.slice(start,start+64));} return keys.length;}\n"
"  function isConvertedRpgmControlToken(part){return part.charCodeAt(0)===27||/^<(?:WordWrap|br\\s*\\/?)>$/i.test(part);}\n"
"  function translateConvertedRpgmText(s,lookupFirst){var orig=String(s==null?'':s); if(orig.indexOf(String.fromCharCode(27))<0){if(lookupFirst) requestLocalCacheKeys([orig],1); return tr(orig);} var parts=orig.split(/(\\x1b[A-Za-z]+(?:\\[[^\\]]{0,64}\\]|<[^>\\n]{0,64}>)?|\\x1b[{}$.|!><^]|<WordWrap>|<br\\s*\\/?>)/gi),cores=[],allHit=true,changed=false; for(var i=0;i<parts.length;i++){var part=parts[i]; if(!part||isConvertedRpgmControlToken(part)||hasCjk(part)||!/[A-Za-z]/.test(part)) continue; var lead=(part.match(/^\\s*/)||[''])[0],tail=(part.match(/\\s*$/)||[''])[0],core=part.slice(lead.length,part.length-tail.length); if(core&&/[A-Za-z]/.test(core)) cores.push(core);} if(lookupFirst) requestLocalCacheKeys(cores); for(var j=0;j<parts.length;j++){var value=parts[j]; if(!value||isConvertedRpgmControlToken(value)||hasCjk(value)||!/[A-Za-z]/.test(value)) continue; var prefix=(value.match(/^\\s*/)||[''])[0],suffix=(value.match(/\\s*$/)||[''])[0],body=value.slice(prefix.length,value.length-suffix.length); if(!body||!/[A-Za-z]/.test(body)) continue; var result=lookupDecorated(body); if(!result.hit) allHit=false; if(result.text!==body){parts[j]=prefix+result.text+suffix; changed=true;}} return allHit&&changed?parts.join(''):orig;}\n"
"  function trDisplay(s){return suppressDisplayTranslate>0?String(s==null?'':s):translateConvertedRpgmText(s,true);}\n"
"  function withDisplayTranslationSuppressed(fn){suppressDisplayTranslate++; try{return fn();}finally{suppressDisplayTranslate--;}}\n"
"  function shouldBitmapTranslate(s){var key=keyOf(s); if(!key||key.length<3||hasCjk(key)) return false; if(!/[A-Za-z]/.test(key)) return false; return true;}\n"
"  function trBitmap(s){var orig=String(s==null?'':s); if(suppressDisplayTranslate>0||!shouldBitmapTranslate(orig)) return orig; requestLocalCacheKeys([orig],1); return tr(orig);}\n"
"  function finiteDrawWidth(w){w=Number(w); return isFinite(w)&&w>0&&w<1000000;}\n"
"  function drawBitmapFit(oldDraw,self,text,x,y,maxWidth,lineHeight,align){var out; try{out=trBitmap(text);}catch(e){reportRpgmError('translate-bitmap-text',e); out=String(text==null?'':text);} if(!finiteDrawWidth(maxWidth)||!hasCjk(out)||!self||typeof self.measureTextWidth!=='function'||typeof self.fontSize!=='number') return oldDraw.call(self,out,x,y,maxWidth,lineHeight,align); var oldSize=self.fontSize; try{var min=Math.max(12,Math.floor(oldSize*0.62)); while(self.fontSize>min&&self.measureTextWidth(out)>maxWidth){self.fontSize--;} return oldDraw.call(self,out,x,y,maxWidth,lineHeight,align);}finally{self.fontSize=oldSize;}}\n"
"  function dsTextWidth(win,t){try{if(win&&win.textWidth) return win.textWidth(t); if(win&&win.contents&&win.contents.measureTextWidth) return win.contents.measureTextWidth(t);}catch(e){reportRpgmError('measure-renderer-text',e);} return String(t==null?'':t).length*16;}\n"
"  function dsLineHeight(win){try{if(win&&win.techTreeLineHeight) return win.techTreeLineHeight(); if(win&&win.lineHeight) return win.lineHeight();}catch(e){reportRpgmError('read-renderer-line-height',e);} return 36;}\n"
"  var rpgmTextExWrapStack=[];\n"
"  var rpgmLayoutFallbackCounts=Object.create(null);\n"
"  function reportRpgmLayoutFallback(context,detail){if(!rpgmLayoutFallbackCounts) rpgmLayoutFallbackCounts=Object.create(null); var key=String(context||'unknown'),count=(rpgmLayoutFallbackCounts[key]||0)+1; rpgmLayoutFallbackCounts[key]=count; if(count<=3||(count&(count-1))===0){var record={context:key,count:count,detail:String(detail||'')}; var records=window.__deepSeekRpgmDiagnostics||(window.__deepSeekRpgmDiagnostics=[]); records.push(record); while(records.length>32) records.shift(); if(window.console&&typeof console.info==='function') console.info('[DeepSeek RPGM] '+key+' #'+count+': '+record.detail);}}\n"
"  function rpgmTextExWidth(win,x,w){var left=Number(x); if(!isFinite(left)) left=0; if(finiteDrawWidth(w)) return Number(w); if(win&&win.contents&&finiteDrawWidth(win.contents.width-left)) return Number(win.contents.width)-left; if(win&&finiteDrawWidth(Number(win.innerWidth)-left)) return Number(win.innerWidth)-left; return 0;}\n"
"  function withRpgmTextExWrap(win,text,x,w,translated,draw){var width=rpgmTextExWidth(win,x,w); if(!translated||!hasCjk(text)||!finiteDrawWidth(width)) return draw(); var left=Number(x); if(!isFinite(left)) left=0; var state={win:win,left:left,right:left+width}; rpgmTextExWrapStack.push(state); reportRpgmLayoutFallback('draw-text-ex-auto-wrap','width='+width+', chars='+String(text==null?'':text).length); try{return draw();}finally{rpgmTextExWrapStack.pop();}}\n"
"  function currentRpgmTextExWrap(win){for(var i=rpgmTextExWrapStack.length-1;i>=0;i--){if(rpgmTextExWrapStack[i].win===win) return rpgmTextExWrapStack[i];} return null;}\n"
"  function rpgmNextWrapUnit(text,index){text=String(text==null?'':text); var unit=text.charAt(index); if(!unit) return ''; var i=index+1; if(/[A-Za-z0-9_'-]/.test(unit)){while(i<text.length&&/[A-Za-z0-9_'-]/.test(text.charAt(i))) unit+=text.charAt(i++);} while(i<text.length&&/[\\u3001\\u3002\\uff0c\\uff0e\\uff01\\uff1f\\uff1a\\uff1b\\uff09\\u3011\\u300b\\u300d\\u300f\\u2019\\u201d\\u2026]/.test(text.charAt(i))) unit+=text.charAt(i++); return unit;}\n"
"  function installTextExAutoWrapHook(){try{var proto=window.Window_Base&&Window_Base.prototype; if(!proto||typeof proto.processNormalCharacter!=='function'||typeof proto.processNewLine!=='function'||proto.processNormalCharacter._dsRpgmTextExWrapHook) return; var old=proto.processNormalCharacter; var wrap=function(textState){var layout=currentRpgmTextExWrap(this); if(layout&&textState&&typeof textState.index==='number'){var x=Number(textState.x),unit=rpgmNextWrapUnit(textState.text,textState.index); if(unit&&!/\\s/.test(unit.charAt(0))&&isFinite(x)&&x>layout.left&&x+dsTextWidth(this,unit)>layout.right){var index=textState.index; this.processNewLine(textState); textState.index=index;}} return old.apply(this,arguments);}; wrap._dsRpgmTextExWrapHook=true; wrap._dsRpgmOriginal=old; proto.processNormalCharacter=wrap;}catch(e){reportRpgmError('install-draw-text-ex-auto-wrap-hook',e);}}\n"
"  function drawCjkAutoWrap(win,text,x,y,maxWidth){text=String(text==null?'':text); try{if(win.convertEscapeCharacters) text=win.convertEscapeCharacters(text);}catch(e){reportRpgmError('convert-renderer-escapes',e);} if(!text) return 0; var esc=String.fromCharCode(27); var max=finiteDrawWidth(maxWidth)?Number(maxWidth):1000000; var lh=dsLineHeight(win); var lines=1,x2=0,y2=y; function nl(){lines++; y2+=lh; x2=0;} function drawTok(tok,w){if(x2>0&&x2+w>max) nl(); if(tok===' '&&x2===0) return; win.drawText(tok,x+x2,y2,w,'left'); x2+=w;} for(var i=0;i<text.length;){var rest=text.slice(i); if(text.charAt(i)===esc){var mi=/^\\x1bI\\[(\\d+)\\]/.exec(rest); if(mi){var iw=(window.Window_Base&&Window_Base._iconWidth)||32; if(x2>0&&x2+iw>max) nl(); if(win.drawIcon) win.drawIcon(Number(mi[1]),x+x2,y2); x2+=iw; i+=mi[0].length; continue;} var mc=/^\\x1bC\\[(\\d+)\\]/.exec(rest); if(mc){if(win.changeTextColor&&win.textColor) win.changeTextColor(win.textColor(Number(mc[1]))); i+=mc[0].length; continue;} if(rest.indexOf(esc+'n')===0){nl(); i+=2; continue;}} var ch=text.charAt(i); if(ch==='\\r'||ch==='\\n'){if(ch==='\\r'&&text.charAt(i+1)==='\\n') i++; nl(); i++; continue;} if(/\\s/.test(ch)){drawTok(' ',dsTextWidth(win,' ')); i++; continue;} var tok=ch; if(!hasCjk(ch)){var j=i+1; while(j<text.length&&text.charAt(j)!==esc&&!/\\s/.test(text.charAt(j))&&!hasCjk(text.charAt(j))) j++; tok=text.slice(i,j); i=j;}else{i++;} drawTok(tok,dsTextWidth(win,tok));} return lines;}\n"
"  // Visible message-page batching owns local XHR/status/JSON/control-token failures because\n"
"  // only the live RPG Maker page planner knows the safe per-line keys. Failures remain source\n"
"  // text, are never cached as success, enter retryAfter, and emit rate-limited diagnostics.\n"
"  function finishMessagePageBatch(x,batch,eventName){var response=null,results=null,sources=null,translations=null,parsed=false,changed=false,now=Date.now(); if(x&&x.status===200){try{response=JSON.parse(x.responseText); results=response&&response.results; sources=response&&response.sources; translations=response&&response.translations; if(!Array.isArray(results)&&(!translations||typeof translations!=='object')) throw new Error('missing batch result fields'); if(!Array.isArray(results)) results=[]; if(!Array.isArray(sources)) sources=[]; parsed=true;}catch(e){reportRpgmError('parse-message-page-batch-response',e);}}else{reportRpgmError('message-page-batch-'+String(eventName||'http'),new Error('HTTP status '+(x?x.status:'unavailable')));} for(var i=0;i<batch.length;i++){var key=batch[i],value=results&&results[i],source=sources&&sources[i]; delete messagePagePending[key]; if((value==null||value==='')&&translations) value=translations[key]; if(parsed&&value&&value!==key&&source!=='miss'&&source!=='queued'&&source!=='pass'){var residue=hasSuspiciousRpgmSourceResidue(key,value),safe=safeRpgmTranslation(key,value); if(safe&&safe!==key){cache[key]=safe; delete retryAfter[key]; changed=true; continue;} if(value!==key) reportRpgmLayoutFallback(residue?'mixed-language-guard':'message-page-control-guard','batch result rejected');} retryAfter[key]=now+MISS_RETRY_MS;} if(changed) requestWindowRefresh();}\n"
"  function requestMessagePageBatch(batch){var done=false; function finish(response,eventName){if(done) return; done=true; finishMessagePageBatch(response,batch,eventName);} sendRpgmJson(BATCH_URL,{texts:batch},15000,'message-page-batch',finish);}\n"
"  function primeMessagePage(values){var keys=[],seen=Object.create(null),now=Date.now(); for(var i=0;i<values.length&&keys.length<MAX_MESSAGE_PAGE_PRIME;i++){var key=keyOf(values[i]); if(!key||seen[key]||cache[key]||pending[key]||messagePagePending[key]||hasCjk(key)||isLayoutOnly(key)||!/[A-Za-z]/.test(key)) continue; if(retryAfter[key]&&retryAfter[key]>now) continue; seen[key]=true; messagePagePending[key]=true; keys.push(key);} for(var start=0;start<keys.length;){var batch=[],chars=0; while(start<keys.length&&batch.length<MESSAGE_PAGE_BATCH_MAX){var next=keys[start]; if(batch.length&&chars+next.length>MESSAGE_PAGE_CHAR_BUDGET) break; batch.push(next); chars+=next.length; start++;} requestMessagePageBatch(batch);}}\n"
"  function withLookupRequestsSuppressed(fn){suppressLookupRequests++; try{return fn();}finally{suppressLookupRequests--;}}\n"
"  function translateTextArray(values,allowPartial,skipLocalLookup){\n"
"    if(!values||!values.length) return values; if(!skipLocalLookup) requestLocalCacheKeys(values,512); var out=new Array(values.length); var allHit=true; var changed=false;\n"
"    for(var i=0;i<values.length;i++){var orig=String(values[i]==null?'':values[i]); var r=lookupDecorated(orig); out[i]=r.text; if(!r.hit) allHit=false; if(r.text!==orig) changed=true;}\n"
"    if(!allHit&&changed&&!allowPartial) return values; return out;\n"
"  }\n"
"  function planMessagePage(original){var candidates=[],liveCandidates=[]; for(var ci=0;ci<original.length;ci++){var line=original[ci]; candidates.push(line); var unwrapped=line.replace(/^(?:<WordWrap>)+/i,''); if(unwrapped!==line) candidates.push(unwrapped); var decor=splitRpgmDecor(line),body=decor?decor.body:unwrapped; if(body){liveCandidates.push(body); if(body!==line&&body!==unwrapped) candidates.push(body);} var visible=visibleLookupKey(body); if(visible){candidates.push(visible); if(visible!==body) liveCandidates.push(visible);}} var joined=original.join('\\n'); return {candidates:candidates,liveCandidates:liveCandidates,joined:joined,large:original.length>24||joined.length>4000};}\n"
"  function translateMessageLines(lines,cacheOnly){\n"
"    if(!lines||!Array.isArray(lines)||!lines.length) return lines; var original=new Array(lines.length); for(var i=0;i<lines.length;i++) original[i]=String(lines[i]==null?'':lines[i]);\n"
"    var plan=planMessagePage(original); if(!cacheOnly) requestLocalCacheKeys(original.length>1?[plan.joined].concat(plan.candidates):plan.candidates,512);\n"
"    function resolve(){if(plan.large){if(!cacheOnly) primeMessagePage(plan.liveCandidates); return withLookupRequestsSuppressed(function(){return translateTextArray(original,false,true);});}\n"
"      if(!plan.large&&original.length>1){var whole=lookup(plan.joined); if(whole.hit&&whole.text!==plan.joined&&whole.text!==keyOf(plan.joined)) return [whole.text];}\n"
"      if(!plan.large&&original.length>1){var first=splitRpgmDecor(original[0]); if(first){var visibleLines=new Array(original.length); visibleLines[0]=visibleLookupKey(first.body)||first.body; for(var vi=1;vi<original.length;vi++){var p=splitRpgmDecor(original[vi]); var b=p?p.body:original[vi]; visibleLines[vi]=visibleLookupKey(b)||b;} var vjoined=visibleLines.join('\\n'); var vwhole=lookup(vjoined); if(vwhole.hit&&vwhole.text!==vjoined&&vwhole.text!==keyOf(vjoined)) return [first.prefix+vwhole.text];}}\n"
"      return translateTextArray(original,false,true);}\n"
"    return cacheOnly?withLookupRequestsSuppressed(resolve):resolve();\n"
"  }\n"
"  function sameRpgmTextArray(a,b){if(!a||!b||a.length!==b.length) return false; for(var i=0;i<a.length;i++){if(String(a[i]==null?'':a[i])!==String(b[i]==null?'':b[i])) return false;} return true;}\n"
"  function messagePageHasLocalLookupPending(lines){var values=new Array(lines.length); for(var i=0;i<lines.length;i++) values[i]=String(lines[i]==null?'':lines[i]); var plan=planMessagePage(values),keys=[plan.joined],seen=Object.create(null); keys=keys.concat(plan.candidates,plan.liveCandidates); for(var j=0;j<keys.length;j++){var key=keyOf(keys[j]); if(!key||seen[key]) continue; seen[key]=true; if(localLookupPending[key]) return true;} return false;}\n"
"  // Only the nonblocking localhost cache read may briefly defer page construction. MZ calls\n"
"  // startMessage from a synchronous while loop, so a deferred start must set the engine-owned\n"
"  // wait counter for one frame; otherwise the same start spins forever and freezes the renderer.\n"
"  // Remote provider work never gates the message lifecycle. Cache failure/timeout is recorded by\n"
"  // requestLocalCacheKeys, resumes this marked start, preserves source text, and cannot create a\n"
"  // successful cache entry, so this renderer fallback does not hide the transport error.\n"
"  function deferRpgmMessageForLocalCache(original,translated){return sameRpgmTextArray(original,translated)&&messagePageHasLocalLookupPending(original);}\n"
"  function installMessageRuntimeHooks(){\n"
"    try{if(window.Game_Message&&Game_Message.prototype.allText&&!Game_Message.prototype.allText._dsRpgmMessageHook){var currentAllText=Game_Message.prototype.allText; var allTextWrap=(function(old){var fn=function(){var original=this&&this._texts; if(original&&Array.isArray(original)&&original.length){try{var translated=translateMessageLines(original); if(translated&&translated!==original) return translated.join('\\n');}catch(e){reportRpgmError('translate-message-all-text',e);}} return old.apply(this,arguments);}; fn._dsRpgmMessageHook=true; fn._dsRpgmOriginal=old; return fn;})(currentAllText); Game_Message.prototype.allText=allTextWrap;}}catch(e){reportRpgmError('install-message-all-text-hook',e);}\n"
"    try{if(window.Window_Message&&Window_Message.prototype.startMessage&&!Window_Message.prototype.startMessage._dsRpgmMessageHook){var currentStartMessage=Window_Message.prototype.startMessage; var startMessageWrap=(function(old){var fn=function(){this._dsRpgmDeferredMessageStart=false; var gm=window.$gameMessage; var original=gm&&gm._texts; if(!original||!Array.isArray(original)||!original.length) return old.apply(this,arguments); var translated; try{translated=translateMessageLines(original); if(deferRpgmMessageForLocalCache(original,translated)){this._dsRpgmDeferredMessageStart=true; this._waitCount=Math.max(Number(this._waitCount)||0,1); return;}}catch(e){reportRpgmError('translate-message-start',e); return old.apply(this,arguments);} this._dsRpgmRenderedMessageSignature=messageRenderSignature(original,translated); if(sameRpgmTextArray(translated,original)){var sourceResult=old.apply(this,arguments); if(this._textState) scheduleActiveMessageCachePoll(this,original); return sourceResult;} stopActiveMessageCachePoll(this); gm._texts=translated; try{return old.apply(this,arguments);} finally{gm._texts=original;}}; fn._dsRpgmMessageHook=true; fn._dsRpgmOriginal=old; return fn;})(currentStartMessage); Window_Message.prototype.startMessage=startMessageWrap;}}catch(e){reportRpgmError('install-message-start-hook',e);}\n"
"  }\n"
"  function installTopLevelWindowHooks(){\n"
"    try{if(window.Window_Help&&Window_Help.prototype.setText&&!Window_Help.prototype.setText._dsRpgmDisplayHook){var oldHelpSet=Window_Help.prototype.setText; var helpSetTextWrap=function(text){markRpgmDisplayTarget(this,text); var out=text; try{out=trDisplay(text);}catch(e){reportRpgmError('translate-help-text',e);} var rendered=String(out==null?'':out),source=String(text==null?'':text); this._dsRpgmTranslatedCjkText=rendered!==source&&hasCjk(rendered)?rendered:''; return oldHelpSet.call(this,out);}; helpSetTextWrap._dsRpgmDisplayHook=true; helpSetTextWrap._dsRpgmOriginal=oldHelpSet; Window_Help.prototype.setText=helpSetTextWrap;}}catch(e){reportRpgmError('install-help-set-text-hook',e);}\n"
"    try{if(window.Game_Message&&Game_Message.prototype.choices&&!Game_Message.prototype.choices._dsRpgmDisplayHook){var oldChoices=Game_Message.prototype.choices; var choicesWrap=function(){var choices=oldChoices.apply(this,arguments); if(choices&&choices.length){try{var translated=translateTextArray(choices); if(translated&&translated!==choices) return translated;}catch(e){reportRpgmError('translate-message-choices',e);}} return choices;}; choicesWrap._dsRpgmDisplayHook=true; choicesWrap._dsRpgmOriginal=oldChoices; Game_Message.prototype.choices=choicesWrap;}}catch(e){reportRpgmError('install-message-choices-hook',e);}\n"
"  function applyTranslatedChoiceCommands(win){\n"
"    try{var list=win&&win._list; if(!list||!list.length) return; var names=new Array(list.length); for(var i=0;i<list.length;i++) names[i]=list[i]&&list[i].name; var translated=translateTextArray(names); for(var j=0;j<list.length;j++){if(list[j]&&typeof list[j].name==='string') list[j].name=translated[j];}}\n"
"    catch(e){reportRpgmError('translate-choice-commands',e);}\n"
"  }\n"
"    try{if(window.Window_ChoiceList&&Window_ChoiceList.prototype.makeCommandList&&!Window_ChoiceList.prototype.makeCommandList._dsRpgmDisplayHook){var oldChoiceMake=Window_ChoiceList.prototype.makeCommandList; var makeCommandListWrap=function(){var r=oldChoiceMake.apply(this,arguments); applyTranslatedChoiceCommands(this); return r;}; makeCommandListWrap._dsRpgmDisplayHook=true; makeCommandListWrap._dsRpgmOriginal=oldChoiceMake; Window_ChoiceList.prototype.makeCommandList=makeCommandListWrap;}}catch(e){reportRpgmError('install-choice-command-list-hook',e);}\n"
"    try{if(window.Window_ChoiceList&&Window_ChoiceList.prototype.maxChoiceWidth&&!Window_ChoiceList.prototype.maxChoiceWidth._dsRpgmDisplayHook){var oldChoiceWidth=Window_ChoiceList.prototype.maxChoiceWidth; var maxChoiceWidthWrap=function(){var w=oldChoiceWidth.apply(this,arguments); try{var gm=window.$gameMessage; var choices=gm&&gm.choices?gm.choices():null; if(choices&&choices.length){var translated=translateTextArray(choices); for(var i=0;i<translated.length;i++){var t=translated[i]; var extra=0; if(this.textWidthEx) extra=this.textWidthEx(t); else if(this.textWidth) extra=this.textWidth(t); else extra=String(t==null?'':t).length; if(this.textPadding) extra+=this.textPadding()*2; if(w<extra) w=extra;}}}catch(e){reportRpgmError('measure-choice-width',e);} return w;}; maxChoiceWidthWrap._dsRpgmDisplayHook=true; maxChoiceWidthWrap._dsRpgmOriginal=oldChoiceWidth; Window_ChoiceList.prototype.maxChoiceWidth=maxChoiceWidthWrap;}}catch(e){reportRpgmError('install-choice-width-hook',e);}\n"
"  }\n"
"  function installCopiedDrawTextExHooks(){try{if(!window.Window_Base||!Window_Base.prototype._dsDrawTextEx) return; var original=Window_Base.prototype._dsDrawTextEx,count=0; function patch(proto){if(count>=64||!proto||proto===Window_Base.prototype||proto.drawTextEx!==original) return; var wrap=(function(old){var fn=function(text,x,y,w){var self=this; markRpgmDisplayTarget(self,text); var out=text; try{out=translateConvertedRpgmText(text,true);}catch(e){reportRpgmError('translate-copied-draw-text-ex',e);} var changed=String(out==null?'':out)!==String(text==null?'':text); return withRpgmTextExWrap(self,out,x,w,changed,function(){return old.call(self,out,x,y,w);});}; fn._dsRpgmDisplayHook=true; return fn;})(proto.drawTextEx); proto._dsDrawTextEx=proto.drawTextEx; proto.drawTextEx=wrap; count++;} for(var name in window){if(count>=64) break; if(!/^(?:Sprite|Window)_/.test(name)) continue; var ctor=window[name]; patch(ctor&&ctor.prototype);} var types=window.HUDManager&&HUDManager.types; if(types){for(var typeName in types){if(count>=64) break; var entry=types[typeName],registered=entry&&entry.class; patch(registered&&registered.prototype);}}}catch(e){reportRpgmError('install-copied-draw-text-ex-hooks',e);}}\n"
"  function installDisplayTextHooks(){\n"
"    try{if(window.Bitmap&&Bitmap.prototype.drawText&&!Bitmap.prototype.drawText._dsRpgmDisplayHook){var oldBitmapDraw=Bitmap.prototype.drawText; var bitmapWrap=function(text,x,y,maxWidth,lineHeight,align){markRpgmDisplayTarget(this,text); return drawBitmapFit(oldBitmapDraw,this,text,x,y,maxWidth,lineHeight,align);}; bitmapWrap._dsRpgmDisplayHook=true; Bitmap.prototype._dsBitmapDrawText=oldBitmapDraw; Bitmap.prototype.drawText=bitmapWrap;}}catch(e){reportRpgmError('install-bitmap-draw-hook',e);}\n"
"    try{if(window.Window_Base&&Window_Base.prototype.drawTextAutoWrap&&!Window_Base.prototype.drawTextAutoWrap._dsRpgmDisplayHook){var oldAutoWrap=Window_Base.prototype.drawTextAutoWrap; var autoWrap=function(baseText,x,y,maxWidth){var self=this,args=arguments; markRpgmDisplayTarget(self,baseText); var orig,translated; try{orig=String(baseText==null?'':baseText); translated=trDisplay(orig);}catch(e){reportRpgmError('translate-auto-wrap-text',e); return oldAutoWrap.apply(self,args);} if(hasCjk(translated)){try{return withDisplayTranslationSuppressed(function(){return drawCjkAutoWrap(self,translated,x,y,maxWidth);});}catch(e){reportRpgmError('draw-cjk-auto-wrap',e); return withDisplayTranslationSuppressed(function(){return oldAutoWrap.apply(self,args);});}} if(translated!==orig) return withDisplayTranslationSuppressed(function(){return oldAutoWrap.call(self,translated,x,y,maxWidth);}); return withDisplayTranslationSuppressed(function(){return oldAutoWrap.apply(self,args);});}; autoWrap._dsRpgmDisplayHook=true; Window_Base.prototype._dsDrawTextAutoWrap=oldAutoWrap; Window_Base.prototype.drawTextAutoWrap=autoWrap;}}catch(e){reportRpgmError('install-auto-wrap-hook',e);}\n"
"    try{if(window.Window_Base&&Window_Base.prototype.drawTextEx&&!Window_Base.prototype.drawTextEx._dsRpgmDisplayHook){var old=Window_Base.prototype.drawTextEx; var drawTextExWrap=function(text,x,y,w){var self=this; markRpgmDisplayTarget(self,text); var out=text; try{out=trDisplay(text);}catch(e){reportRpgmError('translate-draw-text-ex',e);} var rendered=String(out==null?'':out),changed=rendered!==String(text==null?'':text)||self._dsRpgmTranslatedCjkText===rendered; return withRpgmTextExWrap(self,out,x,w,changed,function(){return old.call(self,out,x,y,w);});}; drawTextExWrap._dsRpgmDisplayHook=true; Window_Base.prototype._dsDrawTextEx=old; Window_Base.prototype.drawTextEx=drawTextExWrap;}}catch(e){reportRpgmError('install-draw-text-ex-hook',e);}\n"
"    try{if(window.Window_Base&&Window_Base.prototype.drawText&&!Window_Base.prototype.drawText._dsRpgmDisplayHook){var old2=Window_Base.prototype.drawText; var drawTextWrap=function(text,x,y,w,a){markRpgmDisplayTarget(this,text); var out=text; try{out=trDisplay(text);}catch(e){reportRpgmError('translate-draw-text',e);} return old2.call(this,out,x,y,w,a);}; drawTextWrap._dsRpgmDisplayHook=true; Window_Base.prototype._dsDrawText=old2; Window_Base.prototype.drawText=drawTextWrap;}}catch(e){reportRpgmError('install-draw-text-hook',e);}\n"
"    installTextExAutoWrapHook();\n"
"    installCopiedDrawTextExHooks();\n"
"  }\n"
"  function installAllRpgmHooks(){installCjkFont(); installQuestPrimeHooks(); installMessageRuntimeHooks(); installTopLevelWindowHooks(); installDisplayTextHooks(); installWindowOpenRefreshHook();}\n"
"  // MZ main.js may be the only static script in index.html and create every rmmz_*/plugins.js\n"
"  // script after this hook has already run. The script load event is the engine-owned lifecycle\n"
"  // boundary where those classes become available. Listener/install failures preserve source text\n"
"  // and are recorded through reportRpgmError; they never create a translation/cache success.\n"
"  function isRpgmLifecycleScript(script){var src=String(script&&script.src||'').replace(/\\\\/g,'/').toLowerCase(); return /(?:^|\\/)js\\/(?:rmmz_|rpg_|plugins(?:\\.js|\\/))/.test(src);}\n"
"  function attachRpgmLifecycleScriptLoadHook(script){try{if(!script||!script.addEventListener||script._dsRpgmLifecycleLoadHook) return; script.addEventListener('load',function(){try{if(!isRpgmLifecycleScript(script)) return; installPluginLoadHook(); installAllRpgmHooks();}catch(e){reportRpgmError('run-bootstrap-script-load-listener',e);}},false); script._dsRpgmLifecycleLoadHook=true;}catch(e){reportRpgmError('attach-bootstrap-script-load-listener',e);}}\n"
"  function installDynamicScriptLoadHook(){try{if(!document||!document.createElement||document.createElement._dsRpgmCreateElementHook) return; var oldCreate=document.createElement; var createWrap=function(){var element=oldCreate.apply(this,arguments); try{if(arguments.length&&String(arguments[0]).toLowerCase()==='script') attachRpgmLifecycleScriptLoadHook(element);}catch(e){reportRpgmError('observe-created-script',e);} return element;}; createWrap._dsRpgmCreateElementHook=true; createWrap._dsRpgmOriginal=oldCreate; document.createElement=createWrap;}catch(e){reportRpgmError('install-dynamic-script-load-hook',e);}}\n"
"  // plugins.js may create every plugin tag before this hook runs. Only the DOM load lifecycle\n"
"  // can identify when those external scripts finish replacing engine methods; attachment failures\n"
"  // leave source text visible, never create cache success, and are recorded by reportRpgmError.\n"
"  function attachPluginScriptLoadHook(script){try{if(!script||!script.addEventListener||script._dsRpgmLoadHook) return; var src=String(script.src||'').replace(/\\\\/g,'/').toLowerCase(); if(src&&src.indexOf('js/plugins/')<0) return; script.addEventListener('load',installAllRpgmHooks,false); script._dsRpgmLoadHook=true;}catch(e){reportRpgmError('attach-plugin-load-listener',e);}}\n"
"  function installPluginLoadHook(){try{var scripts=document&&document.getElementsByTagName?document.getElementsByTagName('script'):null; if(scripts){for(var i=0;i<scripts.length;i++) attachPluginScriptLoadHook(scripts[i]);} if(!window.PluginManager||!PluginManager.loadScript||PluginManager.loadScript._dsRpgmPluginLoadHook) return; var oldLoadScript=PluginManager.loadScript; var loadScriptWrap=function(){var r=oldLoadScript.apply(this,arguments); try{var current=document.getElementsByTagName('script'),script=current&&current[current.length-1]; attachPluginScriptLoadHook(script);}catch(e){reportRpgmError('observe-plugin-load-script',e);} return r;}; loadScriptWrap._dsRpgmPluginLoadHook=true; loadScriptWrap._dsRpgmOriginal=oldLoadScript; PluginManager.loadScript=loadScriptWrap;}catch(e){reportRpgmError('install-plugin-load-hook',e);}}\n"
"  installDynamicScriptLoadHook();\n"
"  installPluginLoadHook();\n"
"  installAllRpgmHooks();\n"
"})();\n";

/* ----------------------------------------------------------------
 * deploy_renpy_font — 为 Ren'Py 游戏部署 CJK 字体
 *
 * Ren'Py 默认字体不含 CJK 字形，翻译后的中文会显示为方块。
 * 从 Windows 系统 Fonts 目录复制一个 CJK 字体（优先 simhei.ttf 黑体，
 * 回退 msyh.ttc 微软雅黑）到游戏的 game/ 目录，hook 脚本的
 * style 覆盖会引用此字体。
 * ---------------------------------------------------------------- */
static void deploy_renpy_font(const WCHAR *game) {
    WCHAR ttc_dst[MAX_PATH * 4], ttf_dst[MAX_PATH * 4];
    path_join(ttc_dst, MAX_PATH * 4, game, L"ds_font.ttc");
    path_join(ttf_dst, MAX_PATH * 4, game, L"ds_font.ttf");
    if (exists_path(ttc_dst) || exists_path(ttf_dst)) return;

    WCHAR windir[MAX_PATH];
    if (!GetWindowsDirectoryW(windir, MAX_PATH)) return;

    /* 优先 TTF 格式："0@file.ttc" 集合索引语法并非所有 Ren'Py 版本都支持，
       而普通 TTF 文件名在任何版本都可以工作。 */
    WCHAR src[MAX_PATH * 4];
    path_join(src, MAX_PATH * 4, windir, L"Fonts\\simhei.ttf");
    if (exists_path(src) && copy_file_safe(src, ttf_dst)) {
        append_log(L"Ren'Py：已部署中文字体（黑体）：%s", ttf_dst);
        return;
    }
    path_join(src, MAX_PATH * 4, windir, L"Fonts\\msyh.ttc");
    if (exists_path(src) && copy_file_safe(src, ttc_dst)) {
        append_log(L"Ren'Py：已部署中文字体（微软雅黑）：%s", ttc_dst);
        return;
    }
    append_log(L"Ren'Py：未找到系统中文字体（simhei.ttf/msyh.ttc），翻译文本可能显示为方块。");
}

/* ----------------------------------------------------------------
 * deploy_rpgm_font — 为 RPG Maker MV/MZ 部署 CJK 字体
 *
 * RPG Maker 在 canvas 上渲染文本，需要 @font-face 声明。
 * 将系统 CJK 字体复制到已解析内容根的 fonts/ 目录供 hook 脚本引用。
 * ---------------------------------------------------------------- */
static void deploy_rpgm_font(const WCHAR *content_root) {
    WCHAR font_dir[MAX_PATH * 4], ttf_dst[MAX_PATH * 4], ttc_dst[MAX_PATH * 4];
    path_join(font_dir, MAX_PATH * 4, content_root, L"fonts");
    ensure_dir(font_dir);
    path_join(ttf_dst, MAX_PATH * 4, font_dir, L"ds_font.ttf");
    path_join(ttc_dst, MAX_PATH * 4, font_dir, L"ds_font.ttc");
    if (exists_path(ttf_dst) || exists_path(ttc_dst)) return;

    WCHAR windir[MAX_PATH];
    if (!GetWindowsDirectoryW(windir, MAX_PATH)) return;

    WCHAR src[MAX_PATH * 4];
    path_join(src, MAX_PATH * 4, windir, L"Fonts\\simhei.ttf");
    if (exists_path(src) && copy_file_safe(src, ttf_dst)) {
        append_log(L"RPGM MV/MZ: deployed CJK font: %s", ttf_dst);
        return;
    }

    path_join(src, MAX_PATH * 4, windir, L"Fonts\\msyh.ttc");
    if (exists_path(src) && copy_file_safe(src, ttc_dst)) {
        append_log(L"RPGM MV/MZ: deployed CJK font: %s", ttc_dst);
        return;
    }

    append_log(L"RPGM MV/MZ: no system CJK font found (simhei.ttf/msyh.ttc); translated text may render as boxes.");
}

/* ----------------------------------------------------------------
 * deploy_renpy — 部署 Ren'Py 翻译 hook
 *
 * 将 RENPY_HOOK Python 脚本写入 game/iron_deepseek.rpy，
 * 并调用 deploy_renpy_font 部署 CJK 字体。
 * init 999 保证 hook 在所有其他游戏脚本之后加载。
 * ---------------------------------------------------------------- */
static int remove_stale_renpy_hook_bytecode(const WCHAR *path) {
    if (path_has_reparse_point(path, 0)) {
        append_log(L"Ren'Py: refused to remove stale hook bytecode through a reparse point: %s", path);
        SetLastError(ERROR_ACCESS_DENIED);
        return 0;
    }
    DWORD attr = GetFileAttributesW(path);
    if (attr == INVALID_FILE_ATTRIBUTES) {
        DWORD error = GetLastError();
        if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND) return 1;
        append_log(L"Ren'Py: could not inspect stale launcher hook bytecode %s (Windows error %lu).",
                   path, error);
        return 0;
    }
    if (attr & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) {
        append_log(L"Ren'Py: expected launcher hook bytecode to be a file, but found a directory: %s", path);
        return 0;
    }

    DWORD original_attr = attr;
    if ((attr & FILE_ATTRIBUTE_READONLY) &&
        !SetFileAttributesW(path, attr & ~(DWORD)FILE_ATTRIBUTE_READONLY)) {
        append_log(L"Ren'Py: could not make stale launcher hook bytecode writable %s (Windows error %lu).",
                   path, GetLastError());
        return 0;
    }
    if (delete_file_safe(path)) {
        append_log(L"Ren'Py: removed stale launcher hook bytecode: %s", path);
        return 1;
    }

    DWORD error = GetLastError();
    if (original_attr & FILE_ATTRIBUTE_READONLY) SetFileAttributesW(path, original_attr);
    append_log(L"Ren'Py: could not remove stale launcher hook bytecode %s (Windows error %lu).",
               path, error);
    return 0;
}

int deploy_renpy(const WCHAR *dir) {
    WCHAR game[MAX_PATH * 4], hook[MAX_PATH * 4], compiled_hook[MAX_PATH * 4];
    path_join(game, MAX_PATH * 4, dir, L"game");
    if (!is_dir(game)) {
        append_log(L"Ren'Py：找不到 game 目录，无法部署 hook：%s", game);
        return 0;
    }
    path_join(hook, MAX_PATH * 4, game, L"iron_deepseek.rpy");
    if (!write_text_file_utf8(hook, RENPY_HOOK)) {
        append_log(L"Ren'Py：无法写入 hook 文件 %s（Windows 错误 %lu）。", hook, GetLastError());
        return 0;
    }
    path_join(compiled_hook, MAX_PATH * 4, game, L"iron_deepseek.rpyc");
    if (!remove_stale_renpy_hook_bytecode(compiled_hook)) return 0;
    deploy_renpy_font(game);
    append_log(L"已部署 Ren'Py hook：%s", hook);
    return 1;
}

static int backup_file_once(const WCHAR *path, const WCHAR *suffix) {
    WCHAR backup[MAX_PATH * 4];
    if (!path_append_suffix(backup, MAX_PATH * 4, path, suffix)) return 0;
    DWORD attr = GetFileAttributesW(backup);
    if (attr != INVALID_FILE_ATTRIBUTES) return !(attr & FILE_ATTRIBUTE_DIRECTORY);
    if (copy_file_if_absent_safe(path, backup)) return 1;
    return GetLastError() == ERROR_FILE_EXISTS;
}

static int write_file_bytes_atomic(const WCHAR *path, const char *data, DWORD size) {
    WCHAR temp[MAX_PATH * 4];
    if (!path_append_suffix(temp, MAX_PATH * 4, path, L".dst-tmp")) return 0;
    if (exists_path(temp) && !delete_file_safe(temp)) return 0;
    if (!write_file_bytes(temp, data, size)) return 0;
    if (move_file_safe(temp, path, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) return 1;
    append_log(L"无法将临时文件重命名为 %s（Windows 错误 %lu）。", path, GetLastError());
    delete_file_safe(temp);
    return 0;
}

/* 在 [start, end) 内做 ASCII 大小写不敏感子串查找；不依赖 NUL 结尾。 */
static const char *find_ascii_substr_nocase(const char *start, const char *end,
                                            const char *needle) {
    size_t needle_len = strlen(needle);
    if (!needle_len) return start;
    while ((size_t)(end - start) >= needle_len) {
        if (!_strnicmp(start, needle, needle_len)) return start;
        start++;
    }
    return NULL;
}

static void strip_owned_rpgm_hook_tags(const char *html, DWORD size, ByteBuf *out) {
    const char *hook_name = "hook_rpgm_mv.js";
    const char *end = html + size;
    const char *copy = html;
    const char *scan = html;
    const char *hit;
    out->data[0] = 0;

    while ((hit = strstr(scan, hook_name)) != NULL && hit < end) {
        const char *tag_start = NULL;
        const char *p = copy;
        while ((p = find_ascii_substr_nocase(p, end, "<script")) != NULL && p < hit) {
            tag_start = p;
            p += strlen("<script");
        }
        if (!tag_start) {
            scan = hit + strlen(hook_name);
            continue;
        }
        const char *open_end = strchr(tag_start, '>');
        const char *src_attr = find_ascii_substr_nocase(tag_start, end, "src");
        if (!open_end || open_end >= end || hit > open_end ||
            !src_attr || src_attr > hit || src_attr > open_end) {
            scan = hit + strlen(hook_name);
            continue;
        }
        const char *tag_end;
        const char *tag_close = open_end;
        while (tag_close > tag_start && (tag_close[-1] == ' ' || tag_close[-1] == '\t')) tag_close--;
        if (tag_close > tag_start && tag_close[-1] == '/') {
            /* 自闭合 <script .../> 没有 </script>，删除范围只到本标签末尾 */
            tag_end = open_end + 1;
        } else {
            tag_end = find_ascii_substr_nocase(open_end, end, "</script>");
            if (!tag_end) {
                scan = hit + strlen(hook_name);
                continue;
            }
            tag_end += strlen("</script>");
        }
        const char *trim_start = tag_start;
        while (trim_start > copy && (trim_start[-1] == ' ' || trim_start[-1] == '\t')) trim_start--;
        if (trim_start > copy && (trim_start[-1] == '\r' || trim_start[-1] == '\n')) {
            while (trim_start > copy && (trim_start[-1] == '\r' || trim_start[-1] == '\n')) trim_start--;
        }
        bb_add(out, copy, (size_t)(trim_start - copy));
        copy = tag_end;
        while (copy < end && (*copy == '\r' || *copy == '\n')) copy++;
        scan = copy;
    }
    bb_add(out, copy, (size_t)(end - copy));
}

/* ----------------------------------------------------------------
 * deploy_rpgm — 部署 RPG Maker MV/MZ 翻译 hook
 *
 * 1. 解析标准 www/ 或根目录扁平内容布局
 * 2. 将 RPGM_HOOK JS 脚本写入内容根的 js/hook_rpgm_mv.js
 * 3. 部署 CJK 字体并修改内容根的 index.html
 * ---------------------------------------------------------------- */
int deploy_rpgm(const WCHAR *dir) {
    WCHAR content_root[MAX_PATH * 4], jsdir[MAX_PATH * 4];
    WCHAR hook[MAX_PATH * 4], index[MAX_PATH * 4];
    if (!rpgm_content_root(dir, content_root, MAX_PATH * 4)) {
        append_log(L"RPGM MV/MZ：无法解析游戏内容目录：%s", dir);
        return 0;
    }
    path_join(jsdir, MAX_PATH * 4, content_root, L"js");
    if (!is_dir(jsdir)) {
        append_log(L"RPGM MV/MZ：找不到 js 目录，无法部署 hook：%s", jsdir);
        return 0;
    }
    path_join(hook, MAX_PATH * 4, jsdir, L"hook_rpgm_mv.js");
    if (!write_text_file_utf8(hook, RPGM_HOOK)) {
        append_log(L"RPGM MV/MZ：无法写入 hook 文件 %s（Windows 错误 %lu）。", hook, GetLastError());
        return 0;
    }
    deploy_rpgm_font(content_root);

    path_join(index, MAX_PATH * 4, content_root, L"index.html");
    char *html = NULL;
    DWORD sz = 0;
    if (!read_file_bytes(index, &html, &sz)) {
        append_log(L"RPGM MV/MZ：无法读取 index.html %s（Windows 错误 %lu）。", index, GetLastError());
        return 0;
    }

    const char *script = "\n<script type=\"text/javascript\" src=\"js/hook_rpgm_mv.js\"></script>\n";
    ByteBuf stripped = {0}, out = {0};
    int ok = 0;
    stripped.cap = (size_t)sz + 1;
    stripped.data = (char *)malloc(stripped.cap);
    if (!stripped.data) {
        append_log(L"RPGM MV/MZ：内存不足，无法处理 index.html。");
        goto done;
    }
    strip_owned_rpgm_hook_tags(html, sz, &stripped);

    const char *base = stripped.data;
    const char *main_ref = strstr(base, "js/main.js");
    const char *insert = NULL;
    if (main_ref) {
        insert = main_ref;
        while (insert > base && *insert != '<') insert--;
        if (*insert != '<') insert = main_ref;
    } else {
        insert = strstr(base, "</body>");
        if (!insert) insert = base + stripped.len;
    }

    out.cap = stripped.len + strlen(script) + 1;
    out.data = (char *)malloc(out.cap);
    if (!out.data) {
        append_log(L"RPGM MV/MZ：内存不足，无法生成新的 index.html。");
        goto done;
    }
    out.data[0] = 0;
    bb_add(&out, base, (size_t)(insert - base));
    bb_add(&out, script, strlen(script));
    bb_add(&out, insert, (size_t)((base + stripped.len) - insert));
    if (out.len != stripped.len + strlen(script)) {
        append_log(L"RPGM MV/MZ：生成 index.html 时内部长度不一致，已放弃写入。");
        goto done;
    }
    if (!backup_file_once(index, L".dst-backup")) goto done;
    if (!write_file_bytes_atomic(index, out.data, (DWORD)out.len)) goto done;
    ok = 1;

done:
    free(out.data);
    free(stripped.data);
    free(html);
    if (!ok) return 0;
    append_log(L"已部署 RPGM MV/MZ hook：%s", hook);
    return 1;
}

/* ======================== Unity Mono 部署辅助 ======================== */

/* 在 payloads/UnityTranslator 中查找指定文件 */
static int find_unity_payload_file(WCHAR *out, size_t cap, const WCHAR *leaf) {
    WCHAR base[MAX_PATH * 4];
    path_join(base, MAX_PATH * 4, g_root, L"payloads\\UnityTranslator");
    path_join(out, cap, base, leaf);
    return exists_path(out);
}

static int files_equal(const WCHAR *a, const WCHAR *b);

/* 查找 UnityTranslator.dll 模板（BepInEx 5 版） */
int find_unity_template(WCHAR *out, size_t cap) {
    WCHAR p[MAX_PATH * 4];
    if (!find_unity_payload_file(p, MAX_PATH * 4, L"UnityTranslator.dll")) return 0;
    size_t need = wcslen(p) + 1;
    if (need > cap) return 0;
    memcpy(out, p, need * sizeof(WCHAR));
    return 1;
}

/* 查找 UnityTranslator.BepInEx6.dll 模板（Unity 6+ BepInEx 6 版） */
static int find_unity_bepinex6_template(WCHAR *out, size_t cap) {
    WCHAR p[MAX_PATH * 4];
    if (!find_unity_payload_file(p, MAX_PATH * 4, L"UnityTranslator.BepInEx6.dll")) return 0;
    size_t need = wcslen(p) + 1;
    if (need > cap) return 0;
    memcpy(out, p, need * sizeof(WCHAR));
    return 1;
}

/* 检查文件是否为本工具内置的 Unity Mono 插件（用于 IL2CPP 部署时禁用旧文件） */
static int is_bundled_unity_mono_plugin(const WCHAR *path) {
    WCHAR src[MAX_PATH * 4];
    if (find_unity_template(src, MAX_PATH * 4) && files_equal(path, src)) return 1;
    if (find_unity_bepinex6_template(src, MAX_PATH * 4) && files_equal(path, src)) return 1;
    return 0;
}

/* 将文件重命名为 .disabled 后缀（不删除，保留备份） */
static int disable_existing_file(const WCHAR *path) {
    if (!exists_path(path)) return 0;
    WCHAR disabled[MAX_PATH * 4];
    if (!path_append_suffix(disabled, MAX_PATH * 4, path, L".disabled")) return 0;
    if (exists_path(disabled) && !delete_file_safe(disabled)) return 0;
    return move_file_safe(path, disabled, MOVEFILE_REPLACE_EXISTING | MOVEFILE_COPY_ALLOWED);
}

/* 二进制比较两个文件是否完全相同 */
static int files_equal(const WCHAR *a, const WCHAR *b) {
    char *ab = NULL, *bb = NULL;
    DWORD asz = 0, bsz = 0;
    int ok = 0;
    if (read_file_bytes(a, &ab, &asz) && read_file_bytes(b, &bb, &bsz)) {
        ok = asz == bsz && memcmp(ab, bb, asz) == 0;
    }
    free(ab);
    free(bb);
    return ok;
}

/* ======================== Unity IL2CPP 部署辅助 ======================== */

/* 在 payloads/UnityIL2CPP 中查找指定子目录 */
static int find_il2cpp_payload(WCHAR *out, size_t cap, const WCHAR *leaf) {
    WCHAR base[MAX_PATH * 4];
    path_join(base, MAX_PATH * 4, g_root, L"payloads\\UnityIL2CPP");
    path_join(out, cap, base, leaf);
    return is_dir(out);
}

/* 从 payload 目录复制单个文件到游戏目录 */
static int copy_payload_file(const WCHAR *payload_root, const WCHAR *rel, const WCHAR *game_dir) {
    WCHAR src[MAX_PATH * 4], dst[MAX_PATH * 4];
    path_join(src, MAX_PATH * 4, payload_root, rel);
    path_join(dst, MAX_PATH * 4, game_dir, rel);
    if (!exists_path(src)) return 0;
    return copy_file_safe(src, dst);
}

/* 从 payload 目录复制整个子目录树到游戏目录 */
static int copy_payload_tree(const WCHAR *payload_root, const WCHAR *rel, const WCHAR *game_dir) {
    WCHAR src[MAX_PATH * 4], dst[MAX_PATH * 4];
    path_join(src, MAX_PATH * 4, payload_root, rel);
    path_join(dst, MAX_PATH * 4, game_dir, rel);
    if (!is_dir(src)) return 0;
    return copy_tree_safe(src, dst);
}

static const char XUNITY_OWNER_MARKER_TEXT[] =
    "ds-game-translator:xunity-auto-translator:v1\n";

/* ----------------------------------------------------------------
 * pe_machine — 读取 PE 文件的机器类型
 *
 * 解析 PE 头获取 IMAGE_FILE_HEADER.Machine 字段。
 * 用于判断 GameAssembly.dll 是 x64 (0x8664) 还是其他架构。
 * pe 偏移量来自文件自身，做边界检查防止溢出。
 * ---------------------------------------------------------------- */
static int pe_machine(const WCHAR *path) {
    char *buf = NULL;
    DWORD size = 0;
    int machine = 0;
    if (!read_file_bytes(path, &buf, &size)) return 0;
    if (size >= 0x40 && buf[0] == 'M' && buf[1] == 'Z') {
        DWORD pe = *(DWORD *)(buf + 0x3c);
        /* pe 偏移量是文件控制的；做边界检查防止 (pe + 6) 溢出 */
        if (pe < size && size - pe > 6 && !memcmp(buf + pe, "PE\0\0", 4)) {
            machine = *(unsigned short *)(buf + pe + 4);
        }
    }
    free(buf);
    return machine;
}

static int unity_player_machine(const WCHAR *dir) {
    WCHAR player[MAX_PATH * 4], exe[MAX_PATH * 4];
    path_join(player, MAX_PATH * 4, dir, L"UnityPlayer.dll");
    int machine = pe_machine(player);
    if (machine) return machine;
    if (find_exe(dir, exe, MAX_PATH * 4)) return pe_machine(exe);
    return 0;
}

/* ----------------------------------------------------------------
 * write_xunity_config — 生成 XUnity.AutoTranslator 配置文件
 *
 * 写入 BepInEx/config/AutoTranslatorConfig.ini，
 * 配置 XUnity 使用本地 DeepSeek 端点 (http://127.0.0.1:19999)，
 * 语言方向 auto→zh-CN，并启用所有 UI 文本框架（UGUI/TMP/NGUI 等）。
 * ---------------------------------------------------------------- */
static int migrate_xunity_owned_ini_setting(const WCHAR *cfg, const WCHAR *owned,
                                            const char *section, const char *key,
                                            const char *value);

static int write_xunity_config(const WCHAR *dir) {
    WCHAR cfgdir[MAX_PATH * 4], cfg[MAX_PATH * 4], owned[MAX_PATH * 4];
    path_join(cfgdir, MAX_PATH * 4, dir, L"BepInEx\\config");
    if (!ensure_dir(cfgdir)) return 0;
    path_join(cfg, MAX_PATH * 4, cfgdir, L"AutoTranslatorConfig.ini");
    if (!path_append_suffix(owned, MAX_PATH * 4, cfg, L".dst-owned")) return 0;

    if (exists_path(owned) && exists_path(cfg) && !files_equal(cfg, owned)) {
        if (migrate_xunity_owned_ini_setting(
                cfg, owned, "Behaviour",
                "MaxCharactersPerTranslation", "2500")) {
            return 1;
        }
        append_log(L"Unity IL2CPP: preserved user-modified AutoTranslatorConfig.ini; deploy it again after reviewing the file.");
        return 0;
    }
    if (!exists_path(owned) && exists_path(cfg) && !backup_file_once(cfg, L".dst-backup")) {
        append_log(L"Unity IL2CPP: could not back up AutoTranslatorConfig.ini. Windows error: %lu", GetLastError());
        return 0;
    }

    /* 快照本次覆盖前的内容；ownership 记录失败时优先回滚到它，
       而不是首次部署前备份的用户原始文件。 */
    char *previous = NULL;
    DWORD previous_size = 0;
    if (exists_path(cfg) && !read_file_bytes(cfg, &previous, &previous_size)) {
        append_log(L"Unity IL2CPP: could not read the existing AutoTranslatorConfig.ini before updating it. Windows error: %lu", GetLastError());
        return 0;
    }

    static const char XUNITY_CONFIG_FMT[] =
        "[Service]\n"
        "Endpoint=DeepSeekTranslate\n"
        "FallbackEndpoint=\n"
        "\n"
        "[General]\n"
        "Language=zh-CN\n"
        "FromLanguage=auto\n"
        "\n"
        "[Files]\n"
        "Directory=Translation\\{Lang}\\Text\n"
        "OutputFile=Translation\\{Lang}\\Text\\_AutoGeneratedTranslations.txt\n"
        "SubstitutionFile=Translation\\{Lang}\\Text\\_Substitutions.txt\n"
        "PreprocessorsFile=Translation\\{Lang}\\Text\\_Preprocessors.txt\n"
        "PostprocessorsFile=Translation\\{Lang}\\Text\\_Postprocessors.txt\n"
        "\n"
        "[TextFrameworks]\n"
        "EnableIMGUI=False\n"
        "EnableUGUI=True\n"
        "EnableUIElements=True\n"
        "EnableNGUI=True\n"
        "EnableTextMeshPro=True\n"
        "EnableTextMesh=False\n"
        "EnableFairyGUI=True\n"
        "\n"
        "[Behaviour]\n"
        "MaxCharactersPerTranslation=2500\n"
        "IgnoreWhitespaceInDialogue=True\n"
        "MinDialogueChars=20\n"
        "ForceSplitTextAfterCharacters=0\n"
        "CopyToClipboard=False\n"
        "MaxClipboardCopyCharacters=2500\n"
        "ClipboardDebounceTime=1.25\n"
        "EnableUIResizing=False\n"
        "EnableBatching=True\n"
        "UseStaticTranslations=True\n"
        "OverrideFont=\n"
        "OverrideFontSize=\n"
        "OverrideFontTextMeshPro=\n"
        "FallbackFontTextMeshPro=\n"
        "ResizeUILineSpacingScale=\n"
        "ForceUIResizing=False\n"
        "IgnoreTextStartingWith=\\u180e;Confidence increased;Confidence decreased;Confidence lowered;Confidence reduced;Confidence changed;\n"
        "TextGetterCompatibilityMode=False\n"
        "GameLogTextPaths=\n"
        "RomajiPostProcessing=ReplaceMacronWithCircumflex;RemoveApostrophes;ReplaceHtmlEntities\n"
        "TranslationPostProcessing=ReplaceMacronWithCircumflex;ReplaceHtmlEntities\n"
        "RegexPostProcessing=\n"
        "CacheRegexPatternResults=False\n"
        "PersistRichTextMode=Final\n"
        "CacheRegexLookups=False\n"
        "CacheWhitespaceDifferences=False\n"
        "GenerateStaticSubstitutionTranslations=False\n"
        "GeneratePartialTranslations=False\n"
        "EnableTranslationScoping=True\n"
        "EnableSilentMode=True\n"
        "BlacklistedIMGUIPlugins=\n"
        "EnableTextPathLogging=False\n"
        "OutputUntranslatableText=False\n"
        "IgnoreVirtualTextSetterCallingRules=False\n"
        "MaxTextParserRecursion=1\n"
        "HtmlEntityPreprocessing=True\n"
        "HandleRichText=True\n"
        "EnableTranslationHelper=False\n"
        "ForceMonoModHooks=False\n"
        "InitializeHarmonyDetourBridge=False\n"
        "RedirectedResourceDetectionStrategy=AppendMongolianVowelSeparatorAndRemoveAll\n"
        "OutputTooLongText=False\n"
        "TemplateAllNumberAway=True\n"
        "ReloadTranslationsOnFileChange=True\n"
        "DisableTextMeshProScrollInEffects=False\n"
        "CacheParsedTranslations=False\n"
        "\n"
        "[Texture]\n"
        "TextureDirectory=Translation\\{Lang}\\Texture\n"
        "EnableTextureTranslation=False\n"
        "EnableTextureDumping=False\n"
        "EnableTextureToggling=False\n"
        "EnableTextureScanOnSceneLoad=False\n"
        "EnableSpriteRendererHooking=False\n"
        "LoadUnmodifiedTextures=False\n"
        "DetectDuplicateTextureNames=False\n"
        "DuplicateTextureNames=\n"
        "EnableLegacyTextureLoading=False\n"
        "TextureHashGenerationStrategy=FromImageName\n"
        "CacheTexturesInMemory=True\n"
        "EnableSpriteHooking=False\n"
        "\n"
        "[ResourceRedirector]\n"
        "PreferredStoragePath=Translation\\{Lang}\\RedirectedResources\n"
        "EnableTextAssetRedirector=False\n"
        "LogAllLoadedResources=False\n"
        "EnableDumping=False\n"
        "CacheMetadataForAllFiles=True\n"
        "\n"
        "[Http]\n"
        "UserAgent=\n"
        "DisableCertificateValidation=True\n"
        "\n"
        "[TranslationAggregator]\n"
        "Width=400\n"
        "Height=100\n"
        "EnabledTranslators=\n"
        "\n"
        "[Debug]\n"
        "EnableConsole=False\n"
        "\n"
        "[Migrations]\n"
        "Enable=True\n"
        "Tag=5.6.1\n"
        "\n"
        "[Custom]\n"
        "Url=http://127.0.0.1:19999/translate\n"
        "EnableShortDelay=False\n"
        "DisableSpamChecks=False\n"
        "\n"
        "[DeepSeek]\n"
        "Url=http://127.0.0.1:19999\n"
        "MaxBatchSize=16\n"
        "MaxConcurrency=8\n"
        "QueueWaitSeconds=30\n"
        "QueuePollIntervalSeconds=0.2\n"
        "TranslationDelay=0.1\n"
        "DisplaySafePunctuation=True\n";
    if (!write_text_file_utf8(cfg, XUNITY_CONFIG_FMT)) {
        append_log(L"Unity IL2CPP: could not write AutoTranslatorConfig.ini. Windows error: %lu", GetLastError());
        free(previous);
        return 0;
    }
    if (!copy_file_safe(cfg, owned)) {
        DWORD error = GetLastError();
        if (previous) {
            if (!write_file_bytes_atomic(cfg, previous, previous_size)) {
                append_log(L"Unity IL2CPP: could not roll back AutoTranslatorConfig.ini. Windows error: %lu", GetLastError());
            }
        } else {
            delete_file_safe(cfg);
        }
        free(previous);
        append_log(L"Unity IL2CPP: could not record config ownership; restored the previous config. Windows error: %lu", error);
        return 0;
    }
    free(previous);
    return 1;
}

/* ----------------------------------------------------------------
 * detect_unity_major — 从 globalgamemanagers 文件读取 Unity 大版本号
 *
 * <game>\*_Data\globalgamemanagers 文件头部包含版本字符串
 * （如 "6000.4.0f1" 或 "2022.3.62f3"）。扫描前 4096 字节中
 * 符合 Unity 版本模式的数字序列（5、2017-2023 或 6000+），
 * 提取大版本号。用于决定部署 BepInEx 5 还是 BepInEx 6 运行时。
 * ---------------------------------------------------------------- */
static int detect_unity_major(const WCHAR *dir) {
    WCHAR pat[MAX_PATH * 4];
    path_join(pat, MAX_PATH * 4, dir, L"*_Data");
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) return 0;
    int major = 0;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
        WCHAR ggm[MAX_PATH * 4], datadir[MAX_PATH * 4];
        path_join(datadir, MAX_PATH * 4, dir, fd.cFileName);
        path_join(ggm, MAX_PATH * 4, datadir, L"globalgamemanagers");
        char *buf = NULL;
        DWORD sz = 0;
        if (read_file_bytes(ggm, &buf, &sz)) {
            DWORD lim = sz < 4096 ? sz : 4096;
            for (DWORD i = 0; i + 5 < lim; i++) {
                if (buf[i] < '0' || buf[i] > '9') continue;
                int v = 0, d = 0;
                DWORD j = i;
                while (j < lim && buf[j] >= '0' && buf[j] <= '9' && d < 5) { v = v * 10 + (buf[j] - '0'); j++; d++; }
                if (d >= 1 && j < lim && buf[j] == '.' && (v == 5 || (v >= 2017 && v <= 2023) || v >= 6000)) { major = v; break; }
            }
            free(buf);
        }
        if (major) break;
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return major;
}

/* 检查游戏是否已有 BepInEx 6 Unity.Mono 运行时 */
static int unity_has_bepinex6_mono(const WCHAR *dir) {
    WCHAR p[MAX_PATH * 4];
    path_join(p, MAX_PATH * 4, dir, L"BepInEx\\core\\BepInEx.Unity.Mono.dll");
    return exists_path(p);
}

/* payload 缺失时只记录可复制执行的修复命令，不在部署路径中自动联网下载。 */
static void log_payload_install_command(const WCHAR *flag) {
    append_log(L"Install runtime payloads with:");
    append_log(L"  powershell -ExecutionPolicy Bypass -File scripts\\install_runtime_payloads.ps1 %s", flag);
}

/* ----------------------------------------------------------------
 * install_bepinex_mono_runtime — 安装 BepInEx Mono 运行时到游戏目录
 *
 * 复制 winhttp.dll（doorstop 加载器）、doorstop_config.ini、
 * .doorstop_version 以及 BepInEx/core 整个目录树。
 * use_bepinex6 为真时使用 payloads/UnityMonoRuntime6（Unity 6+），
 * 否则使用 payloads/UnityMonoRuntime（BepInEx 5）。
 * ---------------------------------------------------------------- */
static int find_bepinex_mono_runtime_payload(WCHAR *mono_rt, size_t cap,
                                              int use_bepinex6, int machine,
                                              const WCHAR **runtime_rel_out) {
    int use_x86 = machine == 0x014c;
    const WCHAR *runtime_rel = use_bepinex6
        ? (use_x86 ? L"payloads\\UnityMonoRuntime6X86" : L"payloads\\UnityMonoRuntime6")
        : (use_x86 ? L"payloads\\UnityMonoRuntimeX86" : L"payloads\\UnityMonoRuntime");
    path_join(mono_rt, cap, g_root, runtime_rel);
    if (!is_dir(mono_rt)) {
        append_log(L"Unity: missing BepInEx %d Mono %s runtime payload (%s).",
                   use_bepinex6 ? 6 : 5, use_x86 ? L"x86" : L"x64", runtime_rel);
        log_payload_install_command(use_bepinex6 ? L"-UnityMono6" : L"-UnityMono5");
        return 0;
    }
    if (runtime_rel_out) *runtime_rel_out = runtime_rel;
    return 1;
}

static int install_bepinex_mono_runtime(const WCHAR *dir, int use_bepinex6, int machine) {
    WCHAR mono_rt[MAX_PATH * 4];
    const WCHAR *runtime_rel = NULL;
    int use_x86 = machine == 0x014c;
    if (!find_bepinex_mono_runtime_payload(mono_rt, MAX_PATH * 4,
                                           use_bepinex6, machine, &runtime_rel)) {
        return 0;
    }

    int ok = 1;
    ok &= copy_payload_file(mono_rt, L"winhttp.dll", dir);
    ok &= copy_payload_file(mono_rt, L"doorstop_config.ini", dir);
    ok &= copy_payload_file(mono_rt, L".doorstop_version", dir);
    ok &= copy_payload_tree(mono_rt, L"BepInEx\\core", dir);
    if (!ok) {
        append_log(L"Unity: BepInEx %d Mono %s runtime deployment is incomplete; check %s.",
                   use_bepinex6 ? 6 : 5, use_x86 ? L"x86" : L"x64", runtime_rel);
        log_payload_install_command(use_bepinex6 ? L"-UnityMono6 -Force" : L"-UnityMono5 -Force");
        return 0;
    }
    append_log(L"Unity: deployed BepInEx %d (Mono) %s runtime%s.",
               use_bepinex6 ? 6 : 5, use_x86 ? L"x86" : L"x64",
               use_bepinex6 ? L" for Unity 6+" : L"");
    return 1;
}

/* Existing mod installations may contain BepInEx plugins/core but be missing
 * one of Doorstop's root bootstrap files. Preserve every existing file and
 * copy only missing bootstrap pieces; copy the core tree only when its
 * required entry assemblies are incomplete. */
static int repair_existing_bepinex_mono_runtime(const WCHAR *dir,
                                                 int use_bepinex6,
                                                 int machine) {
    WCHAR target[MAX_PATH * 4];

    /* 先扫描缺失项：用户自带的完整 BepInEx 不依赖本地 payload，
       只有确有文件需要复制时才解析并校验 payload 目录。 */
    path_join(target, MAX_PATH * 4, dir, L"winhttp.dll");
    int loader_machine = pe_machine(target);
    int need_loader = !exists_path(target) ||
                      (machine && loader_machine && loader_machine != machine);

    path_join(target, MAX_PATH * 4, dir, L"doorstop_config.ini");
    int need_config = !exists_path(target);

    path_join(target, MAX_PATH * 4, dir, L".doorstop_version");
    int need_version = !exists_path(target);

    WCHAR core_a[MAX_PATH * 4], core_b[MAX_PATH * 4];
    path_join(core_a, MAX_PATH * 4, dir, use_bepinex6
        ? L"BepInEx\\core\\BepInEx.Unity.Mono.dll"
        : L"BepInEx\\core\\BepInEx.dll");
    path_join(core_b, MAX_PATH * 4, dir, use_bepinex6
        ? L"BepInEx\\core\\BepInEx.Unity.Mono.Preloader.dll"
        : L"BepInEx\\core\\BepInEx.Preloader.dll");
    int need_core = !exists_path(core_a) || !exists_path(core_b);

    if (!need_loader && !need_config && !need_version && !need_core) return 1;

    WCHAR mono_rt[MAX_PATH * 4];
    const WCHAR *runtime_rel = NULL;
    int use_x86 = machine == 0x014c;
    if (!find_bepinex_mono_runtime_payload(mono_rt, MAX_PATH * 4,
                                           use_bepinex6, machine, &runtime_rel)) {
        return 0;
    }

    int ok = 1;
    int repaired = 0;
    if (need_loader) {
        ok &= copy_payload_file(mono_rt, L"winhttp.dll", dir);
        repaired++;
    }
    if (need_config) {
        ok &= copy_payload_file(mono_rt, L"doorstop_config.ini", dir);
        repaired++;
    }
    if (need_version) {
        ok &= copy_payload_file(mono_rt, L".doorstop_version", dir);
        repaired++;
    }
    if (need_core) {
        ok &= copy_payload_tree(mono_rt, L"BepInEx\\core", dir);
        repaired++;
    }

    if (!ok) {
        append_log(L"Unity: existing BepInEx %d Mono %s runtime repair is incomplete; check %s.",
                   use_bepinex6 ? 6 : 5, use_x86 ? L"x86" : L"x64", runtime_rel);
        log_payload_install_command(use_bepinex6 ? L"-UnityMono6 -Force" : L"-UnityMono5 -Force");
        return 0;
    }
    if (repaired) {
        append_log(L"Unity: repaired %d missing BepInEx %d Mono bootstrap component(s) without replacing existing user files.",
                   repaired, use_bepinex6 ? 6 : 5);
    }
    return 1;
}

/* ----------------------------------------------------------------
 * ensure_bepinex_mono — 确保游戏有 BepInEx Mono 运行时
 *
 * 自动安装策略：
 *   - Unity 6+ (major >= 6000) 需要 BepInEx 6 Unity.Mono 运行时
 *   - 旧版 Unity 保持使用 BepInEx 5（兼容已有安装）
 *   - 如果用户已自行安装 BepInEx，保留不覆盖（除非版本不匹配）
 * ---------------------------------------------------------------- */
static int bytes_contain_ascii(const char *bytes, DWORD size, const char *needle) {
    size_t needle_len = needle ? strlen(needle) : 0;
    if (!bytes || !needle_len || needle_len > size) return 0;
    for (DWORD i = 0; i <= size - (DWORD)needle_len; i++) {
        if (!memcmp(bytes + i, needle, needle_len)) return 1;
    }
    return 0;
}

static int find_unity_mscorlib(const WCHAR *dir, WCHAR *out, size_t cap) {
    WCHAR pattern[MAX_PATH * 4];
    path_join(pattern, MAX_PATH * 4, dir, L"*_Data");
    WIN32_FIND_DATAW fd;
    HANDLE find = FindFirstFileW(pattern, &fd);
    if (find == INVALID_HANDLE_VALUE) return 0;
    int found = 0;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
        WCHAR data_dir[MAX_PATH * 4];
        path_join(data_dir, MAX_PATH * 4, dir, fd.cFileName);
        path_join(out, cap, data_dir, L"Managed\\mscorlib.dll");
        if (exists_path(out)) {
            found = 1;
            break;
        }
    } while (FindNextFileW(find, &fd));
    FindClose(find);
    return found;
}

static int mscorlib_has_bepinex_file_writer(const WCHAR *path) {
    char *bytes = NULL;
    DWORD size = 0;
    int complete = 0;
    if (read_file_bytes(path, &bytes, &size)) {
        complete = size >= 2 && bytes[0] == 'M' && bytes[1] == 'Z' &&
                   bytes_contain_ascii(bytes, size, "WriteAllText");
    }
    free(bytes);
    return complete;
}

static int unity_mscorlib_is_clearly_stripped(const WCHAR *dir) {
    WCHAR mscorlib[MAX_PATH * 4];
    if (!find_unity_mscorlib(dir, mscorlib, MAX_PATH * 4)) return 0;
    char *bytes = NULL;
    DWORD size = 0;
    int stripped = 0;
    if (read_file_bytes(mscorlib, &bytes, &size)) {
        stripped = size >= 2 && bytes[0] == 'M' && bytes[1] == 'Z' &&
                   bytes_contain_ascii(bytes, size, "mscorlib") &&
                   bytes_contain_ascii(bytes, size, "System.IO") &&
                   !bytes_contain_ascii(bytes, size, "WriteAllText");
    }
    free(bytes);
    if (stripped) {
        append_log(L"Unity Mono: detected a clearly stripped mscorlib without System.IO.File.WriteAllText: %s", mscorlib);
    }
    return stripped;
}

static int ascii_span_equals_ignore_case(const char *span, size_t len, const char *text) {
    size_t text_len = strlen(text);
    return len == text_len && !_strnicmp(span, text, len);
}

static int find_active_ini_setting(const char *data, DWORD size, const char *section,
                                   const char *key,
                                   DWORD *line_start_out, DWORD *line_end_out,
                                   DWORD *value_start_out, DWORD *value_end_out,
                                   DWORD *section_body_out) {
    int in_target_section = 0;
    DWORD pos = 0;
    while (pos < size) {
        DWORD line_start = pos;
        while (pos < size && data[pos] != '\r' && data[pos] != '\n') pos++;
        DWORD line_end = pos;
        while (pos < size && (data[pos] == '\r' || data[pos] == '\n')) pos++;

        DWORD start = line_start;
        while (start < line_end && (data[start] == ' ' || data[start] == '\t')) start++;
        if (start >= line_end || data[start] == '#' || data[start] == ';') continue;
        if (data[start] == '[') {
            /* 跟踪当前 [section]，只在目标段内匹配键，避免改到其他段的同名键 */
            DWORD name_start = start + 1;
            DWORD close = name_start;
            while (close < line_end && data[close] != ']') close++;
            DWORD name_end = close;
            while (name_end > name_start && (data[name_end - 1] == ' ' || data[name_end - 1] == '\t')) name_end--;
            while (name_start < name_end && (data[name_start] == ' ' || data[name_start] == '\t')) name_start++;
            in_target_section = close < line_end &&
                                ascii_span_equals_ignore_case(data + name_start,
                                                              name_end - name_start, section);
            if (in_target_section && section_body_out) *section_body_out = pos;
            continue;
        }
        if (!in_target_section) continue;
        DWORD equals = start;
        while (equals < line_end && data[equals] != '=') equals++;
        if (equals >= line_end) continue;
        DWORD key_end = equals;
        while (key_end > start && (data[key_end - 1] == ' ' || data[key_end - 1] == '\t')) key_end--;
        if (!ascii_span_equals_ignore_case(data + start, key_end - start, key)) continue;

        DWORD value_start = equals + 1;
        while (value_start < line_end && (data[value_start] == ' ' || data[value_start] == '\t')) value_start++;
        DWORD value_end = line_end;
        while (value_end > value_start && (data[value_end - 1] == ' ' || data[value_end - 1] == '\t')) value_end--;
        if (line_start_out) *line_start_out = line_start;
        if (line_end_out) *line_end_out = line_end;
        if (value_start_out) *value_start_out = value_start;
        if (value_end_out) *value_end_out = value_end;
        return 1;
    }
    return 0;
}

/*
 * XUnity adds provider defaults and endpoint-created settings to its INI on
 * first launch. That makes the live file differ from the launcher's ownership
 * snapshot even when the user did not change the setting we need to migrate.
 *
 * Only update a managed value when the live value still exactly matches the
 * previous ownership snapshot. Any user-edited value therefore fails closed
 * and is preserved by write_xunity_config. Both files are updated atomically;
 * if the ownership snapshot write fails, the live config is rolled back.
 * Additive XUnity sections remain byte-for-byte intact and diagnostics record
 * every filesystem failure at this external ownership boundary.
 */
static int migrate_xunity_owned_ini_setting(const WCHAR *cfg, const WCHAR *owned,
                                            const char *section, const char *key,
                                            const char *value) {
    char *cfg_data = NULL, *owned_data = NULL;
    DWORD cfg_size = 0, owned_size = 0;
    if (!read_file_bytes(cfg, &cfg_data, &cfg_size) ||
        !read_file_bytes(owned, &owned_data, &owned_size)) {
        append_log(L"Unity IL2CPP: could not inspect the owned XUnity config migration (Windows error %lu).",
                   GetLastError());
        free(cfg_data);
        free(owned_data);
        return 0;
    }

    DWORD cfg_value_start = 0, cfg_value_end = 0;
    DWORD owned_value_start = 0, owned_value_end = 0;
    int cfg_found = find_active_ini_setting(
        cfg_data, cfg_size, section, key,
        NULL, NULL, &cfg_value_start, &cfg_value_end, NULL);
    int owned_found = find_active_ini_setting(
        owned_data, owned_size, section, key,
        NULL, NULL, &owned_value_start, &owned_value_end, NULL);
    size_t cfg_value_len = cfg_value_end - cfg_value_start;
    size_t owned_value_len = owned_value_end - owned_value_start;
    if (!cfg_found || !owned_found ||
        cfg_value_len != owned_value_len ||
        memcmp(cfg_data + cfg_value_start,
               owned_data + owned_value_start,
               cfg_value_len) != 0) {
        free(cfg_data);
        free(owned_data);
        return 0;
    }

    size_t new_value_len = strlen(value);
    if (cfg_value_len == new_value_len &&
        !memcmp(cfg_data + cfg_value_start, value, new_value_len)) {
        free(cfg_data);
        free(owned_data);
        return 1;
    }

    ByteBuf cfg_out = {0}, owned_out = {0};
    bb_add(&cfg_out, cfg_data, cfg_value_start);
    bb_add(&cfg_out, value, new_value_len);
    bb_add(&cfg_out, cfg_data + cfg_value_end, cfg_size - cfg_value_end);
    bb_add(&owned_out, owned_data, owned_value_start);
    bb_add(&owned_out, value, new_value_len);
    bb_add(&owned_out, owned_data + owned_value_end, owned_size - owned_value_end);
    if (!cfg_out.data || !owned_out.data ||
        cfg_out.len > MAXDWORD || owned_out.len > MAXDWORD) {
        append_log(L"Unity IL2CPP: could not allocate the owned XUnity config migration.");
        free(cfg_out.data);
        free(owned_out.data);
        free(cfg_data);
        free(owned_data);
        return 0;
    }

    if (!write_file_bytes_atomic(cfg, cfg_out.data, (DWORD)cfg_out.len)) {
        append_log(L"Unity IL2CPP: could not migrate the owned XUnity config (Windows error %lu).",
                   GetLastError());
        free(cfg_out.data);
        free(owned_out.data);
        free(cfg_data);
        free(owned_data);
        return 0;
    }
    if (!write_file_bytes_atomic(owned, owned_out.data, (DWORD)owned_out.len)) {
        DWORD error = GetLastError();
        if (!write_file_bytes_atomic(cfg, cfg_data, cfg_size)) {
            append_log(L"Unity IL2CPP: could not roll back the XUnity config migration (Windows error %lu).",
                       GetLastError());
        }
        append_log(L"Unity IL2CPP: could not update XUnity config ownership; restored the live config (Windows error %lu).",
                   error);
        free(cfg_out.data);
        free(owned_out.data);
        free(cfg_data);
        free(owned_data);
        return 0;
    }

    append_log(L"Unity IL2CPP: migrated %S while preserving XUnity-added config sections.", key);
    free(cfg_out.data);
    free(owned_out.data);
    free(cfg_data);
    free(owned_data);
    return 1;
}

/* Highly stripped Unity players can remove both the corlib writer required by
 * BepInEx and Unity's log callback API. The game build owns those external
 * incompatibilities, so the launcher cannot repair them upstream. These
 * config edits can hide Unity messages from BepInEx's secondary LogOutput.log,
 * but Player.log remains intact; launcher logs record detection and every
 * ownership conflict. A failed edit stops deployment instead of reporting a
 * working translator or creating a translation/cache success. */
static int update_stripped_ini_setting(const WCHAR *path, const char *section,
                                       const char *key, const char *value,
                                       const WCHAR *label) {
    char *data = NULL;
    DWORD size = 0;
    int had_original = exists_path(path);
    if (had_original && !read_file_bytes(path, &data, &size)) {
        append_log(L"Unity Mono: could not read %s while applying stripped-runtime support (Windows error %lu).", label, GetLastError());
        return 0;
    }

    DWORD line_start = 0, line_end = 0, value_start = 0, value_end = 0, section_body = 0;
    int found = find_active_ini_setting(data, size, section, key,
                                        &line_start, &line_end,
                                        &value_start, &value_end,
                                        &section_body);
    if (found && ascii_span_equals_ignore_case(data + value_start,
                                                value_end - value_start, value)) {
        free(data);
        return 1;
    }

    WCHAR backup[MAX_PATH * 4], owned[MAX_PATH * 4];
    if (!path_append_suffix(backup, MAX_PATH * 4, path,
                            L".dst-stripped-backup") ||
        !path_append_suffix(owned, MAX_PATH * 4, path,
                            L".dst-stripped-owned")) {
        free(data);
        append_log(L"Unity Mono: recovery path for %s is too long.", label);
        return 0;
    }

    if (exists_path(owned) && (!had_original || !files_equal(path, owned))) {
        append_log(L"Unity Mono: preserved user-modified %s; stripped-runtime setting was not overwritten.", label);
        free(data);
        return 0;
    }
    if (!exists_path(owned) && exists_path(backup)) {
        append_log(L"Unity Mono: preserved %s because an unowned stripped-runtime backup already exists.", label);
        free(data);
        return 0;
    }

    ByteBuf out = {0};
    if (found) {
        bb_add(&out, data, line_start);
        bb_add(&out, key, strlen(key));
        bb_add(&out, " = ", 3);
        bb_add(&out, value, strlen(value));
        bb_add(&out, data + line_end, size - line_end);
    } else if (section_body) {
        /* 目标 section 已存在但缺少该键：插到段首，避免在文件尾追加重复 section */
        bb_add(&out, data, section_body);
        if (data[section_body - 1] != '\n') bb_add(&out, "\r\n", 2);
        bb_add(&out, key, strlen(key));
        bb_add(&out, " = ", 3);
        bb_add(&out, value, strlen(value));
        bb_add(&out, "\r\n", 2);
        bb_add(&out, data + section_body, size - section_body);
    } else {
        if (size) bb_add(&out, data, size);
        if (size && data[size - 1] != '\n' && data[size - 1] != '\r') bb_add(&out, "\r\n", 2);
        if (size) bb_add(&out, "\r\n", 2);
        bb_add(&out, "[", 1);
        bb_add(&out, section, strlen(section));
        bb_add(&out, "]\r\n", 3);
        bb_add(&out, key, strlen(key));
        bb_add(&out, " = ", 3);
        bb_add(&out, value, strlen(value));
        bb_add(&out, "\r\n", 2);
    }
    if (!out.data || out.len > MAXDWORD) {
        free(out.data);
        free(data);
        append_log(L"Unity Mono: could not allocate the updated %s.", label);
        return 0;
    }

    if (had_original && !backup_file_once(path, L".dst-stripped-backup")) {
        free(out.data);
        free(data);
        append_log(L"Unity Mono: could not back up %s before applying stripped-runtime support (Windows error %lu).", label, GetLastError());
        return 0;
    }
    WCHAR parent[MAX_PATH * 4];
    wcsncpy(parent, path, MAX_PATH * 4 - 1);
    parent[MAX_PATH * 4 - 1] = 0;
    WCHAR *slash = wcsrchr(parent, L'\\');
    if (slash) {
        *slash = 0;
        if (!ensure_dir(parent)) {
            free(out.data);
            free(data);
            append_log(L"Unity Mono: could not create the directory for %s (Windows error %lu).", label, GetLastError());
            return 0;
        }
    }
    if (!write_file_bytes_atomic(path, out.data, (DWORD)out.len)) {
        free(out.data);
        free(data);
        append_log(L"Unity Mono: could not update %s (Windows error %lu).", label, GetLastError());
        return 0;
    }
    free(out.data);

    if (!copy_file_safe(path, owned)) {
        DWORD error = GetLastError();
        /* 回滚到本次调用前的内容，而不是首次部署前备份的用户原始文件 */
        if (had_original) {
            if (!write_file_bytes_atomic(path, data, size)) {
                append_log(L"Unity Mono: could not roll back %s (Windows error %lu).", label, GetLastError());
            }
        } else {
            delete_file_safe(path);
        }
        free(data);
        append_log(L"Unity Mono: could not record ownership for %s; restored its prior state (Windows error %lu).", label, error);
        return 0;
    }
    free(data);
    append_log(L"Unity Mono: updated %s for the detected stripped runtime.", label);
    return 1;
}

static int install_stripped_unity_corlib(const WCHAR *dir) {
    WCHAR payload[MAX_PATH * 4], payload_mscorlib[MAX_PATH * 4], payload_marker[MAX_PATH * 4];
    path_join(payload, MAX_PATH * 4, g_root, L"payloads\\UnityMonoCorlib");
    path_join(payload_mscorlib, MAX_PATH * 4, payload, L"mscorlib.dll");
    path_join(payload_marker, MAX_PATH * 4, payload, L".dst-installed-by-ds");
    if (!is_dir(payload) || !exists_path(payload_marker) ||
        !mscorlib_has_bepinex_file_writer(payload_mscorlib)) {
        append_log(L"Unity Mono: official complete corlib payload is missing or invalid.");
        log_payload_install_command(L"-UnityMonoCorlib");
        return 0;
    }

    WCHAR target[MAX_PATH * 4], target_mscorlib[MAX_PATH * 4], target_marker[MAX_PATH * 4];
    path_join(target, MAX_PATH * 4, dir, L"BepInEx\\unstripped_corlib");
    path_join(target_mscorlib, MAX_PATH * 4, target, L"mscorlib.dll");
    path_join(target_marker, MAX_PATH * 4, target, L".dst-installed-by-ds");
    if (is_dir(target) && !exists_path(target_marker)) {
        if (mscorlib_has_bepinex_file_writer(target_mscorlib)) {
            append_log(L"Unity Mono: using an existing unstripped_corlib directory without replacing user files.");
            return 1;
        }
        append_log(L"Unity Mono: existing unstripped_corlib is incomplete and has no launcher ownership marker; preserved it.");
        return 0;
    }
    if (exists_path(target_marker) && !files_equal(target_marker, payload_marker)) {
        append_log(L"Unity Mono: existing unstripped_corlib ownership marker differs from the installed payload; preserved it.");
        return 0;
    }
    if (!copy_tree_safe(payload, target) || !mscorlib_has_bepinex_file_writer(target_mscorlib)) {
        append_log(L"Unity Mono: failed to deploy the official complete corlib payload (Windows error %lu).", GetLastError());
        return 0;
    }
    append_log(L"Unity Mono: deployed official Mono corlib support for the detected stripped runtime.");
    return 1;
}

/* Some protected Unity Mono builds remove only the managed declaration for
 * Font.Internal_CreateDynamicFont while leaving the native internal call in
 * UnityPlayer. BepInEx 5 preloader patchers are the earliest local boundary
 * that can restore that declaration before UnityEngine.TextRenderingModule is
 * loaded. This first-party patcher never replaces a Unity DLL. Ownership is
 * recorded as an exact byte snapshot; conflicting user files are preserved and
 * fail deployment instead of being silently overwritten. */
static int install_stripped_unity_font_patcher(const WCHAR *dir) {
    WCHAR payload[MAX_PATH * 4], target[MAX_PATH * 4], owned[MAX_PATH * 4];
    if (!find_unity_payload_file(payload, MAX_PATH * 4,
                                 L"DeepSeekUnityFontPatcher.dll")) {
        append_log(L"Unity Mono: stripped-runtime font metadata patcher payload is missing.");
        return 0;
    }
    path_join(target, MAX_PATH * 4, dir,
              L"BepInEx\\patchers\\DeepSeekUnityFontPatcher.dll");
    path_join(owned, MAX_PATH * 4, dir,
              L"BepInEx\\patchers\\DeepSeekUnityFontPatcher.dll.dst-owned");

    int had_target = exists_path(target);
    int had_owned = exists_path(owned);
    if (had_owned && had_target && !files_equal(target, owned)) {
        append_log(L"Unity Mono: preserved user-modified DeepSeekUnityFontPatcher.dll; its ownership snapshot no longer matches.");
        return 0;
    }
    if (!had_owned && had_target) {
        if (files_equal(target, payload)) {
            append_log(L"Unity Mono: using an existing unowned DeepSeekUnityFontPatcher.dll without claiming ownership.");
            return 1;
        }
        append_log(L"Unity Mono: preserved an existing unowned DeepSeekUnityFontPatcher.dll; stripped-runtime patcher was not overwritten.");
        return 0;
    }

    if (!copy_file_safe(payload, target)) {
        append_log(L"Unity Mono: could not deploy DeepSeekUnityFontPatcher.dll (Windows error %lu).", GetLastError());
        return 0;
    }
    if (!copy_file_safe(target, owned)) {
        DWORD error = GetLastError();
        if (had_owned) {
            if (had_target) copy_file_safe(owned, target);
            else delete_file_safe(target);
        } else {
            delete_file_safe(target);
        }
        append_log(L"Unity Mono: could not record font patcher ownership; restored its prior state (Windows error %lu).", error);
        return 0;
    }
    append_log(L"Unity Mono: deployed the BepInEx 5 font metadata patcher for the detected stripped runtime.");
    return 1;
}

static int ensure_stripped_unity_mono_support(const WCHAR *dir, int stripped) {
    if (!stripped) return 1;
    if (!install_stripped_unity_corlib(dir)) return 0;

    WCHAR doorstop[MAX_PATH * 4], bep_cfg[MAX_PATH * 4];
    path_join(doorstop, MAX_PATH * 4, dir, L"doorstop_config.ini");
    path_join(bep_cfg, MAX_PATH * 4, dir, L"BepInEx\\config\\BepInEx.cfg");
    if (!update_stripped_ini_setting(doorstop, "UnityMono", "dll_search_path_override",
                                     "BepInEx\\unstripped_corlib;BepInEx\\core",
                                     L"doorstop_config.ini")) return 0;
    if (!update_stripped_ini_setting(bep_cfg, "Logging", "UnityLogListening", "false",
                                     L"BepInEx.cfg")) return 0;
    return 1;
}

static int ensure_bepinex_mono(const WCHAR *dir) {
    int major = detect_unity_major(dir);
    int use_bepinex6 = major >= 6000;
    int machine = unity_player_machine(dir);
    if (machine && machine != 0x014c && machine != 0x8664) {
        append_log(L"Unity Mono: unsupported player PE machine 0x%04X; only x86 and x64 runtimes are available.", machine);
        return 0;
    }
    WCHAR bep[MAX_PATH * 4];
    path_join(bep, MAX_PATH * 4, dir, L"BepInEx");
    if (is_dir(bep)) {
        int has_bepinex6 = unity_has_bepinex6_mono(dir);
        if (use_bepinex6 && !has_bepinex6) {
            append_log(L"Unity %d (Unity 6+): existing BepInEx is not Unity.Mono 6; updating runtime files.", major);
            return install_bepinex_mono_runtime(dir, 1, machine);
        }
        return repair_existing_bepinex_mono_runtime(dir,
                                                     use_bepinex6 || has_bepinex6,
                                                     machine);
    }
    return install_bepinex_mono_runtime(dir, use_bepinex6, machine);
}

/* ----------------------------------------------------------------
 * deploy_unity — 部署 Unity Mono 翻译插件
 *
 * 流程：
 *   1. ensure_bepinex_mono — 安装/检查 BepInEx 运行时
 *   2. 根据运行时版本选择 BepInEx 5 或 6 的插件 DLL
 *   3. 复制 UnityTranslator.dll + Newtonsoft.Json.dll 到 BepInEx/plugins/
 *   4. 复制 TMP 字体资源包（如有）到 BepInEx/font/
 * ---------------------------------------------------------------- */
int deploy_unity(const WCHAR *dir) {
    WCHAR plugins[MAX_PATH * 4], dll[MAX_PATH * 4], src[MAX_PATH * 4], json_src[MAX_PATH * 4], json_dst[MAX_PATH * 4], fontfix[MAX_PATH * 4];
    path_join(plugins, MAX_PATH * 4, dir, L"BepInEx\\plugins");

    if (!ensure_bepinex_mono(dir)) return 0;
    int stripped = unity_mscorlib_is_clearly_stripped(dir);
    if (!ensure_stripped_unity_mono_support(dir, stripped)) return 0;
    int use_bepinex6 = unity_has_bepinex6_mono(dir);
    if (stripped && !use_bepinex6 &&
        !install_stripped_unity_font_patcher(dir)) return 0;
    int found_template = use_bepinex6
        ? find_unity_bepinex6_template(src, MAX_PATH * 4)
        : find_unity_template(src, MAX_PATH * 4);
    if (!found_template) {
        append_log(use_bepinex6
            ? L"Unity: missing UnityTranslator.BepInEx6.dll template."
            : L"Unity: missing UnityTranslator.dll template.");
        return 0;
    }
    path_join(dll, MAX_PATH * 4, plugins, L"UnityTranslator.dll");
    if (exists_path(dll) && !is_bundled_unity_mono_plugin(dll) &&
        !backup_file_once(dll, L".dst-backup")) {
        append_log(L"Unity: could not back up the existing UnityTranslator.dll before overwriting it (Windows error %lu).", GetLastError());
        return 0;
    }
    if (!copy_file_safe(src, dll)) return 0;
    if (!find_unity_payload_file(json_src, MAX_PATH * 4, L"Newtonsoft.Json.dll")) {
        append_log(L"Unity: missing Newtonsoft.Json.dll dependency; UnityTranslator cannot start.");
        log_payload_install_command(L"-Newtonsoft");
        return 0;
    }
    path_join(json_dst, MAX_PATH * 4, plugins, L"Newtonsoft.Json.dll");
    if (exists_path(json_dst) && !files_equal(json_dst, json_src) &&
        !backup_file_once(json_dst, L".dst-backup")) {
        append_log(L"Unity: could not back up the existing Newtonsoft.Json.dll before overwriting it (Windows error %lu).", GetLastError());
        return 0;
    }
    if (!copy_file_safe(json_src, json_dst)) return 0;
    if (find_il2cpp_payload(fontfix, MAX_PATH * 4, L"TMPFontAssetBundles")) {
        if (!copy_payload_tree(fontfix, L"BepInEx\\font", dir)) return 0;
    } else {
        append_log(L"Unity Mono: TMP font asset bundle payload missing; Chinese TMP glyphs may use overlay fallback.");
    }
    append_log(use_bepinex6
        ? L"Unity: deployed BepInEx 6 compatible Unity plugin: %s"
        : L"Unity: deployed BepInEx 5 compatible Unity plugin: %s", dll);
    return 1;
}

/* ----------------------------------------------------------------
 * deploy_unity_il2cpp — 部署 Unity IL2CPP 翻译插件
 *
 * 完整部署流程：
 *   1. PE 机器类型检查：仅支持 x64（0x8664）
 *   2. 禁用旧的 Mono 版 UnityTranslator.dll（如有）
 *   3. 从 payloads/UnityIL2CPP/BepInExRuntime 复制运行时：
 *      - doorstop_config.ini, winhttp.dll, .doorstop_version
 *      - dotnet/ 目录（自包含 .NET 运行时）
 *      - BepInEx/core/ 和 BepInEx/patchers/
 *   4. 从 payloads/UnityIL2CPP/XUnityAutoTranslator 复制：
 *      - XUnity.Common.dll → BepInEx/core/
 *      - XUnity.AutoTranslator/ → BepInEx/plugins/
 *      - XUnity.ResourceRedirector/ → BepInEx/plugins/
 *   5. 复制 TMP 字体资源包和字体回退插件
 *   6. 禁用旧的 Il2Cppmscorlib.dll（避免遮挡新 interop）
 *   7. 写入 XUnity 配置（AutoTranslatorConfig.ini）
 * ---------------------------------------------------------------- */
int deploy_godot(const WCHAR *dir) {
    (void)dir;
    append_log(L"Godot: enabled PO/CSV/GDScript/resource scan and cache warmup mode.");
    append_log(L"Godot: will build an external translation patch pack after warmup; original .pck files are left unchanged.");
    return 1;
}

int deploy_unity_il2cpp(const WCHAR *dir) {
    WCHAR dll[MAX_PATH * 4], pdb[MAX_PATH * 4], il2cpp_mscorlib[MAX_PATH * 4], runtime[MAX_PATH * 4], xunity[MAX_PATH * 4], fontfix[MAX_PATH * 4], fontplugin[MAX_PATH * 4], gameasm[MAX_PATH * 4], endpoint_src[MAX_PATH * 4], endpoint_dst[MAX_PATH * 4], xunity_plugin_dir[MAX_PATH * 4], xunity_owner_marker[MAX_PATH * 4];
    path_join(dll, MAX_PATH * 4, dir, L"BepInEx\\plugins\\UnityTranslator.dll");
    path_join(pdb, MAX_PATH * 4, dir, L"BepInEx\\plugins\\UnityTranslator.pdb");
    path_join(il2cpp_mscorlib, MAX_PATH * 4, dir, L"BepInEx\\core\\Il2Cppmscorlib.dll");
    path_join(gameasm, MAX_PATH * 4, dir, L"GameAssembly.dll");

    /* 只支持 x64 IL2CPP 构建 */
    int machine = pe_machine(gameasm);
    if (!machine) {
        append_log(L"Unity IL2CPP：无法确认 GameAssembly.dll 的架构，按 x64 处理：%s", gameasm);
    }
    if (machine && machine != 0x8664) {
        append_log(L"Unity IL2CPP：当前只内置 x64 插件运行时，已跳过非 x64 游戏。");
        return 0;
    }

    /* 如果存在旧的 Mono 版插件，禁用它避免冲突 */
    if (exists_path(dll)) {
        if (!is_bundled_unity_mono_plugin(dll)) {
            append_log(L"Unity IL2CPP：保留现有 UnityTranslator.dll（不是内置 Mono 模板）。");
        } else if (disable_existing_file(dll)) {
            append_log(L"Unity IL2CPP：已禁用旧的 Mono UnityTranslator.dll：%s.disabled", dll);
            disable_existing_file(pdb);
        } else {
            append_log(L"Unity IL2CPP：警告：无法禁用内置 Mono UnityTranslator.dll（Windows 错误 %lu），它可能与 IL2CPP 插件冲突：%s", GetLastError(), dll);
        }
    }

    /* 查找 IL2CPP payload 目录 */
    if (!find_il2cpp_payload(runtime, MAX_PATH * 4, L"BepInExRuntime")) {
        append_log(L"Unity IL2CPP：找不到 BepInEx IL2CPP payload。");
        log_payload_install_command(L"-UnityIL2CPP");
        return 0;
    }
    if (!find_il2cpp_payload(xunity, MAX_PATH * 4, L"XUnityAutoTranslator")) {
        append_log(L"Unity IL2CPP：找不到 XUnity AutoTranslator payload。");
        log_payload_install_command(L"-UnityIL2CPP");
        return 0;
    }

    /* 复制运行时和插件文件 */
    int ok = 1;
    ok &= copy_payload_file(runtime, L"doorstop_config.ini", dir);
    ok &= copy_payload_file(runtime, L"winhttp.dll", dir);
    ok &= copy_payload_file(runtime, L".doorstop_version", dir);
    ok &= copy_payload_tree(runtime, L"dotnet", dir);
    ok &= copy_payload_tree(runtime, L"BepInEx\\core", dir);
    ok &= copy_payload_tree(runtime, L"BepInEx\\patchers", dir);
    ok &= copy_payload_file(xunity, L"BepInEx\\core\\XUnity.Common.dll", dir);
    path_join(xunity_plugin_dir, MAX_PATH * 4, dir, L"BepInEx\\plugins\\XUnity.AutoTranslator");
    path_join(xunity_owner_marker, MAX_PATH * 4, xunity_plugin_dir, L".dst-installed-by-ds");
    int xunity_preexisting = is_dir(xunity_plugin_dir);
    if (!xunity_preexisting) {
        if (!ensure_dir(xunity_plugin_dir) ||
            !write_text_file_utf8(xunity_owner_marker, XUNITY_OWNER_MARKER_TEXT)) {
            append_log(L"Unity IL2CPP: could not record ownership for the XUnity plugin directory.");
            ok = 0;
        }
    }
    if (ok || xunity_preexisting) {
        ok &= copy_payload_tree(xunity, L"BepInEx\\plugins\\XUnity.AutoTranslator", dir);
    }
    ok &= copy_payload_tree(xunity, L"BepInEx\\plugins\\XUnity.ResourceRedirector", dir);
    path_join(endpoint_src, MAX_PATH * 4, g_root, L"payloads\\UnityIL2CPP\\DeepSeekXUnityTranslator\\DeepSeekTranslate.dll");
    path_join(endpoint_dst, MAX_PATH * 4, dir, L"BepInEx\\plugins\\XUnity.AutoTranslator\\Translators\\DeepSeekTranslate.dll");
    if (exists_path(endpoint_src)) {
        ok &= copy_file_safe(endpoint_src, endpoint_dst);
    } else {
        append_log(L"Unity IL2CPP: missing DeepSeek XUnity endpoint payload (payloads\\UnityIL2CPP\\DeepSeekXUnityTranslator\\DeepSeekTranslate.dll).");
        append_log(L"Download the full program package or run build_native.bat before deploying Unity IL2CPP.");
        ok = 0;
    }
    if (find_il2cpp_payload(fontfix, MAX_PATH * 4, L"TMPFontAssetBundles")) {
        ok &= copy_payload_tree(fontfix, L"BepInEx\\font", dir);
    } else {
        append_log(L"Unity IL2CPP: TMP font asset bundle payload missing; Chinese TMP glyphs may show as boxes.");
    }
    if (find_il2cpp_payload(fontplugin, MAX_PATH * 4, L"DeepSeekTMPFontFallback")) {
        ok &= copy_payload_tree(fontplugin, L"BepInEx\\plugins\\DeepSeekTMPFontFallback", dir);
    } else {
        append_log(L"Unity IL2CPP: TMP font fallback plugin payload missing; Chinese TMP glyphs may show as boxes.");
    }

    if (!ok) {
        append_log(L"Unity IL2CPP：插件运行时部署不完整，请检查 payloads\\UnityIL2CPP。");
        log_payload_install_command(L"-UnityIL2CPP -Force");
        return 0;
    }

    /* 禁用旧的 Il2Cppmscorlib.dll，避免遮挡新 interop 层 */
    if (exists_path(il2cpp_mscorlib)) {
        if (disable_existing_file(il2cpp_mscorlib)) {
            append_log(L"Unity IL2CPP：已禁用旧的 core\\Il2Cppmscorlib.dll，避免遮挡新 interop。");
        } else {
            append_log(L"Unity IL2CPP：警告：无法禁用旧的 core\\Il2Cppmscorlib.dll（Windows 错误 %lu），它可能遮挡新 interop 层。", GetLastError());
        }
    }

    /* 生成 XUnity 配置文件 */
    if (!write_xunity_config(dir)) return 0;
    append_log(L"Unity IL2CPP: deployed TMP Chinese system font fallback.");
    append_log(L"Unity IL2CPP：已部署 BepInEx be.755 + XUnity AutoTranslator。");
    append_log(L"Unity IL2CPP：XUnity 已配置为使用本地 DeepSeek 批量端点 http://127.0.0.1:19999。");
    return 1;
}

typedef struct {
    int removed;
    int restored;
    int preserved;
    int failed;
} RestoreStats;

static int path_missing_error(DWORD error) {
    return error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND;
}

static int restore_delete_file(const WCHAR *path, RestoreStats *stats) {
    if (path_has_reparse_point(path, 0)) {
        stats->failed++;
        append_log(L"Restore: refused to delete through a directory reparse point: %s", path);
        return 0;
    }
    DWORD attr = GetFileAttributesW(path);
    if (attr == INVALID_FILE_ATTRIBUTES) {
        DWORD error = GetLastError();
        if (path_missing_error(error)) return 1;
        stats->failed++;
        append_log(L"还原：无法检查文件 %s（Windows 错误 %lu）。", path, error);
        return 0;
    }
    if (attr & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) {
        stats->failed++;
        append_log(L"还原：应为普通文件但检测到目录或重解析点，已保留：%s", path);
        return 0;
    }

    DWORD original_attr = attr;
    if (attr & FILE_ATTRIBUTE_READONLY) {
        SetFileAttributesW(path, attr & ~(DWORD)FILE_ATTRIBUTE_READONLY);
    }
    if (delete_file_safe(path)) {
        stats->removed++;
        append_log(L"还原：已移除 %s", path);
        return 1;
    }

    DWORD error = GetLastError();
    if (original_attr & FILE_ATTRIBUTE_READONLY) SetFileAttributesW(path, original_attr);
    stats->failed++;
    append_log(L"还原：无法删除 %s（Windows 错误 %lu）。", path, error);
    return 0;
}

static int restore_remove_matching_file(const WCHAR *installed, const WCHAR *payload,
                                        const WCHAR *label, RestoreStats *stats) {
    if (!exists_path(installed)) return 1;
    if (!exists_path(payload) || !files_equal(installed, payload)) {
        stats->preserved++;
        append_log(L"还原：%s 与当前内置版本不一致，已保留：%s", label, installed);
        return 0;
    }
    return restore_delete_file(installed, stats);
}

static int restore_file_equals_text(const WCHAR *path, const char *text) {
    char *bytes = NULL;
    DWORD size = 0;
    int equal = 0;
    if (read_file_bytes(path, &bytes, &size)) {
        size_t expected = strlen(text);
        equal = expected == size && memcmp(bytes, text, expected) == 0;
    }
    free(bytes);
    return equal;
}

/* Remove only files whose bytes still match the corresponding payload.
   A launcher marker proves initial ownership, but it cannot prove that every
   descendant is still launcher-owned after a user or mod manager edits the
   directory.  Comparing each payload file is therefore the closest safe
   ownership boundary.  Unknown, added and modified files are preserved and
   recorded instead of being hidden behind a successful restore result. */
static void restore_remove_verified_payload_files(const WCHAR *payload_dir,
                                                  const WCHAR *installed_dir,
                                                  const WCHAR *label,
                                                  RestoreStats *stats) {
    if (path_has_reparse_point(payload_dir, 1) ||
        path_has_reparse_point(installed_dir, 1)) {
        stats->failed++;
        append_log(L"Restore: refused to enumerate %s through a reparse point: %s",
                   label, installed_dir);
        return;
    }
    WCHAR pattern[MAX_PATH * 4];
    path_join(pattern, MAX_PATH * 4, payload_dir, L"*");
    WIN32_FIND_DATAW fd;
    HANDLE find = FindFirstFileW(pattern, &fd);
    if (find == INVALID_HANDLE_VALUE) {
        DWORD error = GetLastError();
        stats->preserved++;
        append_log(L"还原：无法读取 %s payload，已保留安装目录 %s（Windows 错误 %lu）。",
                   label, installed_dir, error);
        return;
    }

    do {
        if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..")) continue;
        WCHAR payload_child[MAX_PATH * 4], installed_child[MAX_PATH * 4];
        path_join(payload_child, MAX_PATH * 4, payload_dir, fd.cFileName);
        path_join(installed_child, MAX_PATH * 4, installed_dir, fd.cFileName);
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            DWORD installed_attr = GetFileAttributesW(installed_child);
            if (installed_attr == INVALID_FILE_ATTRIBUTES) continue;
            if (!(installed_attr & FILE_ATTRIBUTE_DIRECTORY) ||
                (installed_attr & FILE_ATTRIBUTE_REPARSE_POINT)) {
                stats->preserved++;
                append_log(L"还原：%s 子目录类型已变化，已保留：%s", label, installed_child);
                continue;
            }
            restore_remove_verified_payload_files(payload_child, installed_child, label, stats);
        } else {
            restore_remove_matching_file(installed_child, payload_child, label, stats);
        }
    } while (FindNextFileW(find, &fd));
    DWORD error = GetLastError();
    FindClose(find);
    if (error != ERROR_NO_MORE_FILES) {
        stats->failed++;
        append_log(L"还原：枚举 %s payload 失败（Windows 错误 %lu）。", label, error);
    }
}

static void restore_prune_empty_dirs(const WCHAR *path, RestoreStats *stats) {
    if (path_has_reparse_point(path, 1)) {
        stats->failed++;
        append_log(L"Restore: refused to prune a directory through a reparse point: %s", path);
        return;
    }
    DWORD attr = GetFileAttributesW(path);
    if (attr == INVALID_FILE_ATTRIBUTES || !(attr & FILE_ATTRIBUTE_DIRECTORY) ||
        (attr & FILE_ATTRIBUTE_REPARSE_POINT)) return;

    WCHAR pattern[MAX_PATH * 4];
    path_join(pattern, MAX_PATH * 4, path, L"*");
    WIN32_FIND_DATAW fd;
    HANDLE find = FindFirstFileW(pattern, &fd);
    if (find != INVALID_HANDLE_VALUE) {
        do {
            if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..")) continue;
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                WCHAR child[MAX_PATH * 4];
                path_join(child, MAX_PATH * 4, path, fd.cFileName);
                restore_prune_empty_dirs(child, stats);
            }
        } while (FindNextFileW(find, &fd));
        FindClose(find);
    }
    if (RemoveDirectoryW(path)) {
        stats->removed++;
        append_log(L"还原：已移除空目录 %s", path);
    } else {
        DWORD error = GetLastError();
        if (error == ERROR_DIR_NOT_EMPTY) {
            stats->preserved++;
            append_log(L"还原：目录含有非启动器文件，已保留：%s", path);
        } else if (!path_missing_error(error)) {
            stats->failed++;
            append_log(L"还原：无法移除空目录 %s（Windows 错误 %lu）。", path, error);
        }
    }
}

static int restore_remove_owned_tree(const WCHAR *tree, const WCHAR *installed_marker,
                                     const WCHAR *payload_tree, const WCHAR *payload_marker,
                                     const WCHAR *label, RestoreStats *stats) {
    if (!exists_path(tree)) return 1;
    if (!exists_path(installed_marker) || !exists_path(payload_marker) ||
        !files_equal(installed_marker, payload_marker)) {
        stats->preserved++;
        append_log(L"还原：无法确认 %s 目录归属，已保留：%s", label, tree);
        return 0;
    }
    if (!is_dir(payload_tree)) {
        stats->preserved++;
        append_log(L"还原：缺少 %s payload，无法逐文件验证，已保留：%s", label, tree);
        return 0;
    }

    restore_remove_verified_payload_files(payload_tree, tree, label, stats);
    restore_prune_empty_dirs(tree, stats);
    return stats->failed == 0 && stats->preserved == 0;
}

static void restore_xunity_plugin(const WCHAR *dir, RestoreStats *stats) {
    WCHAR installed[MAX_PATH * 4], marker[MAX_PATH * 4], payload[MAX_PATH * 4];
    path_join(installed, MAX_PATH * 4, dir, L"BepInEx\\plugins\\XUnity.AutoTranslator");
    if (!is_dir(installed)) return;
    path_join(marker, MAX_PATH * 4, installed, L".dst-installed-by-ds");
    if (!restore_file_equals_text(marker, XUNITY_OWNER_MARKER_TEXT)) {
        stats->preserved++;
        append_log(L"还原 Unity IL2CPP：XUnity 目录不是本版本首次安装，已保留以避免删除用户运行时。");
        return;
    }
    path_join(payload, MAX_PATH * 4, g_root,
              L"payloads\\UnityIL2CPP\\XUnityAutoTranslator\\BepInEx\\plugins\\XUnity.AutoTranslator");
    if (!is_dir(payload)) {
        stats->preserved++;
        append_log(L"还原 Unity IL2CPP：缺少 XUnity payload，无法验证已安装文件，目录已保留。");
        return;
    }

    restore_remove_verified_payload_files(payload, installed, L"XUnity plugin file", stats);
    restore_delete_file(marker, stats);
    restore_prune_empty_dirs(installed, stats);
}

static void restore_renpy(const WCHAR *dir, RestoreStats *stats) {
    static const WCHAR *files[] = {
        L"game\\iron_deepseek.rpy",
        L"game\\iron_deepseek.rpyc",
        L"game\\ds_font.ttf",
        L"game\\ds_font.ttc",
        L"game\\ds_font.otf"
    };
    for (size_t i = 0; i < sizeof(files) / sizeof(files[0]); i++) {
        WCHAR path[MAX_PATH * 4];
        path_join(path, MAX_PATH * 4, dir, files[i]);
        restore_delete_file(path, stats);
    }
}

static void restore_rpgm(const WCHAR *dir, RestoreStats *stats) {
    WCHAR content_root[MAX_PATH * 4], index[MAX_PATH * 4], backup[MAX_PATH * 4];
    if (!rpgm_content_root(dir, content_root, MAX_PATH * 4)) {
        stats->failed++;
        append_log(L"还原 RPG Maker：无法解析游戏内容目录，未修改文件。");
        return;
    }
    int index_ok = 1;
    path_join(index, MAX_PATH * 4, content_root, L"index.html");
    if (!path_append_suffix(backup, MAX_PATH * 4, index, L".dst-backup")) {
        stats->failed++;
        append_log(L"Restore RPG Maker: recovery path is too long.");
        return;
    }

    if (exists_path(index) && !is_dir(index)) {
        char *html = NULL;
        DWORD size = 0;
        if (read_file_bytes(index, &html, &size)) {
            ByteBuf stripped = {0};
            stripped.cap = (size_t)size + 1;
            stripped.data = (char *)malloc(stripped.cap);
            if (!stripped.data) {
                index_ok = 0;
                stats->failed++;
                append_log(L"还原 RPG Maker：内存不足，未修改 index.html。");
            } else {
                stripped.data[0] = 0;
                strip_owned_rpgm_hook_tags(html, size, &stripped);
                if (stripped.len != size) {
                    if (write_file_bytes_atomic(index, stripped.data, (DWORD)stripped.len)) {
                        stats->removed++;
                        append_log(L"还原 RPG Maker：已从 index.html 移除启动器脚本标签。");
                    } else {
                        index_ok = 0;
                        stats->failed++;
                        append_log(L"还原 RPG Maker：无法更新 index.html（Windows 错误 %lu）。", GetLastError());
                    }
                }
                free(stripped.data);
            }
            free(html);
        } else {
            index_ok = 0;
            stats->failed++;
            append_log(L"还原 RPG Maker：无法读取 index.html（Windows 错误 %lu）。", GetLastError());
        }
    } else if (!exists_path(index) && exists_path(backup)) {
        if (move_file_safe(backup, index, MOVEFILE_WRITE_THROUGH)) {
            stats->restored++;
            append_log(L"还原 RPG Maker：已从备份恢复 index.html。");
        } else {
            index_ok = 0;
            stats->failed++;
            append_log(L"还原 RPG Maker：无法恢复 index.html（Windows 错误 %lu）。", GetLastError());
        }
    } else if (is_dir(index)) {
        index_ok = 0;
        stats->failed++;
        append_log(L"还原 RPG Maker：index.html 是目录，未修改。");
    }

    if (index_ok && exists_path(index)) restore_delete_file(backup, stats);

    static const WCHAR *files[] = {
        L"js\\hook_rpgm_mv.js",
        L"fonts\\ds_font.ttf",
        L"fonts\\ds_font.ttc",
        L"fonts\\ds_font.otf"
    };
    for (size_t i = 0; i < sizeof(files) / sizeof(files[0]); i++) {
        WCHAR path[MAX_PATH * 4];
        path_join(path, MAX_PATH * 4, content_root, files[i]);
        restore_delete_file(path, stats);
    }
}

static void restore_godot(const WCHAR *dir, RestoreStats *stats) {
    static const WCHAR *files[] = {
        L"dst_godot_runtime.gd",
        L"dst_godot_patch.pck",
        L"dst_godot_patch.next.pck",
        L"dst_godot_patch.building"
    };
    for (size_t i = 0; i < sizeof(files) / sizeof(files[0]); i++) {
        WCHAR path[MAX_PATH * 4];
        path_join(path, MAX_PATH * 4, dir, files[i]);
        restore_delete_file(path, stats);
    }

    WCHAR launcher[MAX_PATH * 4], marker[MAX_PATH * 4];
    path_join(launcher, MAX_PATH * 4, dir, L"dst_godot_patch.exe");
    path_join(marker, MAX_PATH * 4, dir, L"dst_godot_patch.exe.dst-owned");
    if (exists_path(marker)) {
        restore_delete_file(launcher, stats);
        restore_delete_file(marker, stats);
    } else if (exists_path(launcher)) {
        stats->preserved++;
        append_log(L"还原 Godot：dst_godot_patch.exe 没有启动器所有权标记，已保留。");
    }
}

/* deploy_unity 覆盖用户已有的插件文件前会留下 .dst-backup 一次性备份。
   仅当当前文件仍与内置版本一致时才移除它并恢复备份；已被改动的文件和备份都保留。 */
static void restore_unity_plugin_with_backup(const WCHAR *installed, int matches_bundled,
                                             RestoreStats *stats) {
    WCHAR backup[MAX_PATH * 4];
    if (!path_append_suffix(backup, MAX_PATH * 4, installed, L".dst-backup")) {
        stats->failed++;
        append_log(L"Restore Unity: plugin recovery path is too long.");
        return;
    }
    if (exists_path(installed)) {
        if (!matches_bundled) {
            stats->preserved++;
            append_log(L"还原 Unity：%s 与当前内置版本不一致，已作为用户文件保留。", installed);
            return;
        }
        if (!restore_delete_file(installed, stats)) return;
    }
    if (!exists_path(backup)) return;
    if (move_file_safe(backup, installed, MOVEFILE_WRITE_THROUGH)) {
        stats->restored++;
        append_log(L"还原 Unity：已从备份恢复 %s。", installed);
    } else {
        stats->failed++;
        append_log(L"还原 Unity：无法从备份恢复 %s（Windows 错误 %lu）。", installed, GetLastError());
    }
}

static void restore_unity_mono_plugin(const WCHAR *dir, RestoreStats *stats) {
    WCHAR installed[MAX_PATH * 4];
    path_join(installed, MAX_PATH * 4, dir, L"BepInEx\\plugins\\UnityTranslator.dll");
    restore_unity_plugin_with_backup(installed, is_bundled_unity_mono_plugin(installed), stats);
}

/* Newtonsoft.Json.dll 是共享依赖：没有备份说明部署时未覆盖用户文件，保持原样；
   有备份说明部署时覆盖了用户自带版本，还原时把它换回来。 */
static void restore_unity_mono_newtonsoft(const WCHAR *dir, RestoreStats *stats) {
    WCHAR installed[MAX_PATH * 4], backup[MAX_PATH * 4], payload[MAX_PATH * 4];
    path_join(installed, MAX_PATH * 4, dir, L"BepInEx\\plugins\\Newtonsoft.Json.dll");
    if (!path_append_suffix(backup, MAX_PATH * 4, installed, L".dst-backup")) {
        stats->failed++;
        append_log(L"Restore Unity: Newtonsoft.Json.dll recovery path is too long.");
        return;
    }
    if (!exists_path(backup)) return;
    if (exists_path(installed) &&
        (!find_unity_payload_file(payload, MAX_PATH * 4, L"Newtonsoft.Json.dll") ||
         !files_equal(installed, payload))) {
        stats->preserved++;
        append_log(L"还原 Unity：Newtonsoft.Json.dll 在部署后已被改动，文件和备份均已保留。");
        return;
    }
    DWORD flags = MOVEFILE_WRITE_THROUGH | (exists_path(installed) ? MOVEFILE_REPLACE_EXISTING : 0);
    if (move_file_safe(backup, installed, flags)) {
        stats->restored++;
        append_log(L"还原 Unity：已从备份恢复原 Newtonsoft.Json.dll。");
    } else {
        stats->failed++;
        append_log(L"还原 Unity：无法恢复 Newtonsoft.Json.dll 备份（Windows 错误 %lu）。", GetLastError());
    }
}

static void restore_stripped_unity_font_patcher(const WCHAR *dir,
                                                RestoreStats *stats) {
    WCHAR installed[MAX_PATH * 4], owned[MAX_PATH * 4];
    path_join(installed, MAX_PATH * 4, dir,
              L"BepInEx\\patchers\\DeepSeekUnityFontPatcher.dll");
    path_join(owned, MAX_PATH * 4, dir,
              L"BepInEx\\patchers\\DeepSeekUnityFontPatcher.dll.dst-owned");
    if (!exists_path(owned)) {
        if (exists_path(installed)) {
            stats->preserved++;
            append_log(L"Restore Unity Mono: DeepSeekUnityFontPatcher.dll has no launcher ownership snapshot and was preserved.");
        }
        return;
    }
    if (exists_path(installed) && !files_equal(installed, owned)) {
        stats->preserved++;
        append_log(L"Restore Unity Mono: DeepSeekUnityFontPatcher.dll changed after deployment; the file and ownership snapshot were preserved.");
        return;
    }
    if (exists_path(installed) && !restore_delete_file(installed, stats)) return;
    restore_delete_file(owned, stats);
}

static void restore_stripped_ini_config(const WCHAR *path, const WCHAR *label,
                                        RestoreStats *stats) {
    WCHAR backup[MAX_PATH * 4], owned[MAX_PATH * 4];
    if (!path_append_suffix(backup, MAX_PATH * 4, path,
                            L".dst-stripped-backup") ||
        !path_append_suffix(owned, MAX_PATH * 4, path,
                            L".dst-stripped-owned")) {
        stats->failed++;
        append_log(L"Restore Unity Mono: recovery path for %s is too long.", label);
        return;
    }
    if (!exists_path(owned)) return;

    if (exists_path(path) && !files_equal(path, owned)) {
        stats->preserved++;
        append_log(L"Restore Unity Mono: %s changed after stripped-runtime deployment; config and recovery metadata were preserved.", label);
        return;
    }
    if (exists_path(backup)) {
        if (!copy_file_safe(backup, path)) {
            stats->failed++;
            append_log(L"Restore Unity Mono: could not restore %s (Windows error %lu).", label, GetLastError());
            return;
        }
        stats->restored++;
        append_log(L"Restore Unity Mono: restored the original %s.", label);
        restore_delete_file(backup, stats);
    } else if (exists_path(path) && !restore_delete_file(path, stats)) {
        return;
    }
    restore_delete_file(owned, stats);
}

static void restore_unity_mono(const WCHAR *dir, RestoreStats *stats) {
    restore_unity_mono_plugin(dir, stats);
    restore_unity_mono_newtonsoft(dir, stats);
    restore_stripped_unity_font_patcher(dir, stats);

    WCHAR doorstop[MAX_PATH * 4], bep_cfg[MAX_PATH * 4];
    path_join(doorstop, MAX_PATH * 4, dir, L"doorstop_config.ini");
    path_join(bep_cfg, MAX_PATH * 4, dir, L"BepInEx\\config\\BepInEx.cfg");
    restore_stripped_ini_config(doorstop, L"doorstop_config.ini", stats);
    restore_stripped_ini_config(bep_cfg, L"BepInEx.cfg", stats);

    WCHAR corlib[MAX_PATH * 4], installed_marker[MAX_PATH * 4];
    WCHAR payload_tree[MAX_PATH * 4], payload_marker[MAX_PATH * 4];
    path_join(corlib, MAX_PATH * 4, dir, L"BepInEx\\unstripped_corlib");
    path_join(installed_marker, MAX_PATH * 4, corlib, L".dst-installed-by-ds");
    path_join(payload_tree, MAX_PATH * 4, g_root, L"payloads\\UnityMonoCorlib");
    path_join(payload_marker, MAX_PATH * 4, payload_tree, L".dst-installed-by-ds");
    restore_remove_owned_tree(corlib, installed_marker, payload_tree, payload_marker,
                              L"Unity Mono complete corlib", stats);
}

static void restore_xunity_config(const WCHAR *dir, RestoreStats *stats) {
    WCHAR cfg[MAX_PATH * 4], backup[MAX_PATH * 4], owned[MAX_PATH * 4];
    path_join(cfg, MAX_PATH * 4, dir, L"BepInEx\\config\\AutoTranslatorConfig.ini");
    if (!path_append_suffix(backup, MAX_PATH * 4, cfg, L".dst-backup") ||
        !path_append_suffix(owned, MAX_PATH * 4, cfg, L".dst-owned")) {
        stats->failed++;
        append_log(L"Restore Unity IL2CPP: XUnity recovery path is too long.");
        return;
    }

    if (!exists_path(owned)) return;
    if (exists_path(cfg) && !files_equal(cfg, owned)) {
        stats->preserved++;
        append_log(L"还原 Unity IL2CPP：AutoTranslatorConfig.ini 已被用户修改，配置和备份均已保留。");
        return;
    }

    if (exists_path(backup)) {
        if (copy_file_safe(backup, cfg)) {
            stats->restored++;
            append_log(L"还原 Unity IL2CPP：已恢复原 AutoTranslatorConfig.ini。");
            restore_delete_file(backup, stats);
        } else {
            stats->failed++;
            append_log(L"还原 Unity IL2CPP：无法恢复配置备份（Windows 错误 %lu）。", GetLastError());
            return;
        }
    } else {
        restore_delete_file(cfg, stats);
    }
    restore_delete_file(owned, stats);
}

static void restore_unity_il2cpp(const WCHAR *dir, RestoreStats *stats) {
    WCHAR installed[MAX_PATH * 4], payload[MAX_PATH * 4];

    path_join(installed, MAX_PATH * 4, dir, L"BepInEx\\plugins\\XUnity.AutoTranslator\\Translators\\DeepSeekTranslate.dll");
    path_join(payload, MAX_PATH * 4, g_root, L"payloads\\UnityIL2CPP\\DeepSeekXUnityTranslator\\DeepSeekTranslate.dll");
    restore_remove_matching_file(installed, payload, L"DeepSeek XUnity endpoint", stats);
    restore_xunity_plugin(dir, stats);

    WCHAR tree[MAX_PATH * 4], installed_marker[MAX_PATH * 4];
    WCHAR payload_tree[MAX_PATH * 4], payload_marker[MAX_PATH * 4];
    path_join(tree, MAX_PATH * 4, dir, L"BepInEx\\plugins\\DeepSeekTMPFontFallback");
    path_join(installed_marker, MAX_PATH * 4, tree, L"DeepSeekTMPFontFallback.dll");
    path_join(payload_tree, MAX_PATH * 4, g_root,
              L"payloads\\UnityIL2CPP\\DeepSeekTMPFontFallback\\BepInEx\\plugins\\DeepSeekTMPFontFallback");
    path_join(payload_marker, MAX_PATH * 4, payload_tree, L"DeepSeekTMPFontFallback.dll");
    restore_remove_owned_tree(tree, installed_marker, payload_tree, payload_marker,
                              L"DeepSeek TMP 字体回退", stats);

    WCHAR disabled[MAX_PATH * 4], disabled_pdb[MAX_PATH * 4];
    path_join(disabled, MAX_PATH * 4, dir, L"BepInEx\\plugins\\UnityTranslator.dll.disabled");
    path_join(disabled_pdb, MAX_PATH * 4, dir, L"BepInEx\\plugins\\UnityTranslator.pdb.disabled");
    if (exists_path(disabled)) {
        if (is_bundled_unity_mono_plugin(disabled)) {
            if (restore_delete_file(disabled, stats)) restore_delete_file(disabled_pdb, stats);
        } else {
            stats->preserved++;
            append_log(L"还原 Unity IL2CPP：禁用的 UnityTranslator.dll 无法确认归属，已保留。");
        }
    }

    WCHAR mscorlib[MAX_PATH * 4], mscorlib_disabled[MAX_PATH * 4];
    path_join(mscorlib, MAX_PATH * 4, dir, L"BepInEx\\core\\Il2Cppmscorlib.dll");
    if (!path_append_suffix(mscorlib_disabled, MAX_PATH * 4, mscorlib,
                            L".disabled")) {
        stats->failed++;
        append_log(L"Restore Unity IL2CPP: Il2Cppmscorlib recovery path is too long.");
        return;
    }
    if (exists_path(mscorlib_disabled)) {
        if (exists_path(mscorlib)) {
            stats->preserved++;
            append_log(L"还原 Unity IL2CPP：Il2Cppmscorlib.dll 已存在，禁用备份已保留以避免覆盖。");
        } else if (move_file_safe(mscorlib_disabled, mscorlib, MOVEFILE_WRITE_THROUGH)) {
            stats->restored++;
            append_log(L"还原 Unity IL2CPP：已恢复原 Il2Cppmscorlib.dll。");
        } else {
            stats->failed++;
            append_log(L"还原 Unity IL2CPP：无法恢复 Il2Cppmscorlib.dll（Windows 错误 %lu）。", GetLastError());
        }
    }

    restore_xunity_config(dir, stats);
}

int restore_game(const WCHAR *dir, Engine engine) {
    RestoreStats stats = {0};
    if (!dir || !is_dir(dir) || path_has_reparse_point(dir, 1)) {
        append_log(L"还原：游戏目录无效。");
        return 0;
    }

    if (engine == ENGINE_RENPY) restore_renpy(dir, &stats);
    else if (engine == ENGINE_RPGM_MV) restore_rpgm(dir, &stats);
    else if (engine == ENGINE_UNITY) restore_unity_mono(dir, &stats);
    else if (engine == ENGINE_UNITY_IL2CPP) restore_unity_il2cpp(dir, &stats);
    else if (engine == ENGINE_GODOT) restore_godot(dir, &stats);
    else if (engine == ENGINE_RPGM_LEGACY) {
        append_log(L"还原 RPG Maker XP/VX：启动器未向游戏目录部署文件，无需处理。");
    } else {
        stats.failed++;
        append_log(L"还原：未知引擎，未修改游戏目录。");
    }

    append_log(L"还原汇总：移除 %d，恢复 %d，保留 %d，失败 %d。",
               stats.removed, stats.restored, stats.preserved, stats.failed);
    return stats.failed == 0 && stats.preserved == 0;
}
