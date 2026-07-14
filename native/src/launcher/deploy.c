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
"    import json, os, time, threading, sys, traceback\n"
"    try:\n"
"        from urllib.request import Request, urlopen\n"
"    except ImportError:\n"
"        from urllib2 import Request, urlopen\n"
"    _ds_old_say = renpy.exports.say\n"
"    try:\n"
"        _ds_string_types = (basestring,)\n"
"    except NameError:\n"
"        _ds_string_types = (str,)\n"
"    _ds_memo = {}\n"
"    _ds_pending = {}\n"
"    _ds_retry_after = {}\n"
"    _ds_live_queue = []\n"
"    _ds_fast_live_queue = []\n"
"    _ds_priority_pending = []\n"
"    _ds_priority_set = set()\n"
"    _ds_inflight = set()\n"
"    _ds_lock = threading.Lock()\n"
"    _ds_wake = threading.Event()\n"
"    _ds_live_wake = threading.Event()\n"
"    _ds_fast_live_wake = threading.Event()\n"
"    _ds_state = {'down_until': 0.0, 'poller': False, 'live_worker': False, 'fast_live_worker': False}\n"
"    _ds_error_counts = {}\n"
"    _ds_diag_counts = {}\n"
"    # Engine-boundary failures are counted and logged; repeated hot-path failures log at powers of two.\n"
"    def _ds_report_exception(exc, where=None):\n"
"        where = where or sys._getframe(1).f_code.co_name\n"
"        key = where + '|' + exc.__class__.__name__\n"
"        with _ds_lock:\n"
"            count = _ds_error_counts.get(key, 0) + 1\n"
"            _ds_error_counts[key] = count\n"
"        if count <= 3 or (count & (count - 1)) == 0:\n"
"            detail = traceback.format_exc() if count == 1 else repr(exc)\n"
"            message = '[DeepSeek][EXCEPTION-BOUNDARY] operation=%s occurrence=%d error=%s' % (where, count, detail)\n"
"            if hasattr(renpy, 'log'):\n"
"                renpy.log(message)\n"
"            else:\n"
"                sys.stderr.write(message + '\\n')\n"
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
"                sys.stderr.write(message + '\\n')\n"
"    def _ds_has_cjk(s):\n"
"        try:\n"
"            return any(u'\\u4e00' <= ch <= u'\\u9fff' for ch in s)\n"
"        except Exception as exc:\n"
"            _ds_report_exception(exc)\n"
"            return False\n"
"    def _ds_http(path, payload, timeout):\n"
"        data = json.dumps(payload).encode('utf-8')\n"
"        req = Request('http://127.0.0.1:19999' + path, data=data, headers={'Content-Type':'application/json'})\n"
"        raw = urlopen(req, timeout=timeout).read()\n"
"        if not isinstance(raw, str):\n"
"            raw = raw.decode('utf-8')\n"
"        return json.loads(raw)\n"
"    def _ds_memo_get(s):\n"
"        try:\n"
"            hit = _ds_memo.get(s)\n"
"            if hit is None:\n"
"                return None\n"
"            val, ts = hit\n"
"            if val != s or time.time() - ts < 5.0:\n"
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
"    def _ds_note_pending_many(texts, priority=False):\n"
"        try:\n"
"            now = time.time()\n"
"            queued = False\n"
"            dropped = 0\n"
"            with _ds_lock:\n"
"                for s in texts:\n"
"                    if not isinstance(s, _ds_string_types) or not s or _ds_has_cjk(s):\n"
"                        continue\n"
"                    if s not in _ds_pending and len(_ds_pending) >= 1200:\n"
"                        dropped += 1\n"
"                        continue\n"
"                    if s not in _ds_pending:\n"
"                        _ds_pending[s] = 0\n"
"                        _ds_retry_after[s] = 0.0\n"
"                        queued = True\n"
"                    if priority and s not in _ds_priority_set:\n"
"                        _ds_priority_set.add(s)\n"
"                        _ds_priority_pending.append(s)\n"
"                        queued = True\n"
"                    _ds_memo[s] = (s, now)\n"
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
"        if out is not None:\n"
"            if priority and out == s:\n"
"                _ds_note_pending(s, True)\n"
"            return out\n"
"        _ds_note_pending(s, priority)\n"
"        return None\n"
"    def _ds_refresh_interaction():\n"
"        try:\n"
"            renpy.restart_interaction()\n"
"        except Exception as exc:\n"
"            _ds_report_exception(exc)\n"
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
"    def _ds_select_poll_batch(now, limit=96):\n"
"        with _ds_lock:\n"
"            batch = [key for key in _ds_priority_pending if key in _ds_pending and key not in _ds_inflight and _ds_retry_after.get(key, 0.0) <= now][:limit]\n"
"            if len(batch) < limit:\n"
"                batch.extend(key for key in list(_ds_pending.keys()) if key not in _ds_priority_set and key not in _ds_inflight and _ds_retry_after.get(key, 0.0) <= now and key not in batch)\n"
"                del batch[limit:]\n"
"        return batch\n"
"    def _ds_next_poll_delay():\n"
"        now = time.time()\n"
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
"            now = time.time()\n"
"            if now < _ds_state['down_until']:\n"
"                _ds_wake.wait(max(0.05, _ds_state['down_until'] - now))\n"
"                _ds_wake.clear()\n"
"                continue\n"
"            batch = _ds_select_poll_batch(now, 96)\n"
"            if not batch:\n"
"                continue\n"
"            try:\n"
"                hits = _ds_http('/cache/lookup', {'texts': batch}, 0.5).get('hits') or {}\n"
"            except Exception as exc:\n"
"                _ds_report_exception(exc, 'poll-cache-lookup')\n"
"                _ds_state['down_until'] = time.time() + 1.0\n"
"                continue\n"
"            healed = 0\n"
"            now = time.time()\n"
"            misses = []\n"
"            for k in batch:\n"
"                v = hits.get(k)\n"
"                if v and v != k:\n"
"                    _ds_memo_put(k, _ds_restore_renpy_tokens(k, v), now)\n"
"                    with _ds_lock:\n"
"                        _ds_forget_pending_locked(k)\n"
"                    healed += 1\n"
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
"                while time.time() < _ds_state['down_until']:\n"
"                    wake.wait(max(0.05, _ds_state['down_until'] - time.time()))\n"
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
"                except Exception as exc:\n"
"                    _ds_report_exception(exc, 'live-batch-request')\n"
"                    now = time.time()\n"
"                    _ds_state['down_until'] = now + 1.0\n"
"                    with _ds_lock:\n"
"                        for k in batch:\n"
"                            _ds_inflight.discard(k)\n"
"                            if k in _ds_pending:\n"
"                                _ds_retry_after[k] = now + 1.0\n"
"                    _ds_wake.set()\n"
"                    continue\n"
"                healed = 0\n"
"                now = time.time()\n"
"                for i, k in enumerate(batch):\n"
"                    v = got.get(k)\n"
"                    source = sources[i] if i < len(sources) else 'miss'\n"
"                    if v and v != k:\n"
"                        _ds_memo_put(k, _ds_restore_renpy_tokens(k, v), now)\n"
"                        with _ds_lock:\n"
"                            _ds_forget_pending_locked(k)\n"
"                        healed += 1\n"
"                    elif source == 'pass':\n"
"                        _ds_memo_put(k, k, now)\n"
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
"            _ds_t = threading.Thread(target=_ds_poll_loop)\n"
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
"            _ds_live_t = threading.Thread(target=_ds_live_loop, args=(_ds_live_queue, _ds_live_wake, 16))\n"
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
"            _ds_fast_live_t = threading.Thread(target=_ds_live_loop, args=(_ds_fast_live_queue, _ds_fast_live_wake, 8))\n"
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
"    # Shared entry for say/menu/UI text; renderer-specific repair happens after lookup.\n"
"    def _ds_translate(s, hide_miss=False):\n"
"        try:\n"
"            if not s or _ds_has_cjk(s):\n"
"                return s\n"
"            out = _ds_fetch(s)\n"
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
"                if src_spans and len(_ds_collect_renpy_spans(out, open_ch, close_ch)) != len(src_spans):\n"
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
"    # Prefer the deployed CJK font for all dialogue/UI styles and known game fonts.\n"
"    _ds_font = None\n"
"    for _ds_cand, _ds_spec in (('ds_font.ttf', u'ds_font.ttf'), ('ds_font.otf', u'ds_font.otf'), ('ds_font.ttc', u'0@ds_font.ttc')):\n"
"        if os.path.exists(os.path.join(renpy.config.gamedir, _ds_cand)):\n"
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
"                for _ds_fn in os.listdir(renpy.config.gamedir):\n"
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
"                return _ds_restore_renpy_tokens(s, out)\n"
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
"            _ds_report_exception(exc)\n";

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
"  var MAX_QUEST_PRIME=128;\n"
"  var QUEST_PRIME_BATCH=16;\n"
"  var syncLookupTried=Object.create(null);\n"
"  var syncLookupOrder=[];\n"
"  var rpgmErrorCounts=Object.create(null);\n"
"  // Compatibility fallbacks cover optional plugin/renderer APIs that cannot be fixed upstream.\n"
"  // They never make a failed translation successful: each failure is counted and logged,\n"
"  // and a missing diagnostic console rethrows instead of hiding the original exception.\n"
"  function reportRpgmError(context,error){var e=error instanceof Error?error:new Error(String(error)); var key=String(context||'unknown')+':'+(e.name||'Error'); var count=(rpgmErrorCounts[key]||0)+1; rpgmErrorCounts[key]=count; if(count<=3||(count&(count-1))===0){var detail=count===1&&(e.stack||e.message)?(e.stack||e.message):e.message; if(window.console&&typeof console.error==='function'){console.error('[DeepSeek RPGM] '+context+' failed #'+count+': '+detail);}else{throw e;}}}\n"
"  function installCjkFont(){\n"
"    try{var st=document.createElement('style'); st.type='text/css'; st.textContent=\"@font-face{font-family:'DeepSeekCJK';src:url('fonts/ds_font.ttf') format('truetype'),url('fonts/ds_font.ttc') format('truetype');font-weight:normal;font-style:normal;} body,canvas{font-family:'DeepSeekCJK',sans-serif;}\"; (document.head||document.documentElement).appendChild(st); if(document.fonts&&document.fonts.load){document.fonts.load('16px '+CJK_FONT);}}\n"
"    catch(e){reportRpgmError('install-css-font',e);}\n"
"    try{if(window.Window_Base&&Window_Base.prototype.standardFontFace&&!Window_Base.prototype._dsStandardFontFace){var oldFont=Window_Base.prototype.standardFontFace; Window_Base.prototype._dsStandardFontFace=oldFont; Window_Base.prototype.standardFontFace=function(){var base=oldFont.call(this)||''; return base.indexOf(CJK_FONT)>=0?base:(base?CJK_FONT+', '+base:CJK_FONT);};}}\n"
"    catch(e){reportRpgmError('install-mv-font-face',e);}\n"
"    try{if(window.Game_System&&Game_System.prototype.mainFontFace&&!Game_System.prototype._dsMainFontFace){var oldMain=Game_System.prototype.mainFontFace; Game_System.prototype._dsMainFontFace=oldMain; Game_System.prototype.mainFontFace=function(){var base=oldMain.call(this)||''; return base.indexOf(CJK_FONT)>=0?base:(base?CJK_FONT+', '+base:CJK_FONT);};}}\n"
"    catch(e){reportRpgmError('install-mz-font-face',e);}\n"
"  }\n"
"  installCjkFont();\n"
"  function hasCjk(s){return /[\\u4e00-\\u9fff]/.test(String(s||''));}\n"
"  function keyOf(s){return String(s==null?'':s).replace(/^[ \\t\\r\\n]+|[ \\t\\r\\n]+$/g,'');}\n"
"  function isLayoutOnly(s){var t=String(s==null?'':s); t=t.replace(/(?:<WordWrap>|<br\\s*\\/?>)/gi,''); t=t.replace(/\\\\(?:c|i|fs|fr|fb|fi|oc|ow|v|n[1-5]?|nc|nr|nd[1-5]?|ndc|ndr|nt[1-5]?|ntc|ntr)(?:\\[[^\\]]{0,64}\\]|<[^>\\n]{0,64}>)/gi,''); t=t.replace(/\\\\[{}$.|!><^]/g,''); return !/[A-Za-z\\u0080-\\uffff]/.test(t);}\n"
"  function rpgmControlTokens(s){return String(s==null?'':s).match(/(?:<WordWrap>|<br\\s*\\/?>|\\\\(?:c|i|fs|fr|fb|fi|oc|ow|v|n[1-5]?|nc|nr|nd[1-5]?|ndc|ndr|nt[1-5]?|ntc|ntr)(?:\\[[^\\]]{0,64}\\]|<[^>\\n]{0,64}>))/gi)||[];}\n"
"  function safeRpgmTranslation(src,out){src=String(src==null?'':src); out=String(out==null?'':out); if(!out||out===src) return out||src; var a=rpgmControlTokens(src),b=rpgmControlTokens(out); if(a.length!==b.length) return src; for(var i=0;i<a.length;i++){if(String(a[i]).toLowerCase()!==String(b[i]).toLowerCase()) return src;} return out;}\n"
"  function markRpgmDisplayTarget(target,text){try{var key=keyOf(text); if(!target||!key||hasCjk(key)||!/[A-Za-z]/.test(key)) return; target._dsRpgmTranslationTarget=true; if(target.contents) target.contents._dsRpgmTranslationTarget=true;}catch(e){reportRpgmError('mark-display-target',e);}}\n"
"  function canAutoRefreshWindow(root){if(!root||root.visible===false||root._destroyed||root._closing) return false; if(typeof root.openness==='number'&&root.openness<=0&&!root._opening) return false; if(root.contents&&root.contents._destroyed) return false; return !!(root._dsRpgmTranslationTarget||(root.contents&&root.contents._dsRpgmTranslationTarget));}\n"
"  function isPersistenceScene(scene){try{if(!scene) return false; if(window.Scene_File&&scene instanceof Scene_File) return true; var mode=typeof scene.mode==='function'?scene.mode():scene._mode; return mode==='save'||mode==='load';}catch(e){reportRpgmError('detect-persistence-scene',e); return false;}}\n"
"  var refreshQueued=false;\n"
"  function walkSceneWindows(root,seen,state){if(!root||state.count>=2048||seen.indexOf(root)>=0) return; seen.push(root); state.count++; try{if(canAutoRefreshWindow(root)&&typeof root.refresh==='function'&&!(window.Window_Message&&root instanceof Window_Message)) root.refresh();}catch(e){reportRpgmError('refresh-rendered-window',e);} var children=root.children; if(children&&children.length){for(var i=0;i<children.length;i++) walkSceneWindows(children[i],seen,state);}}\n"
"  function requestWindowRefresh(){if(refreshQueued) return; refreshQueued=true; try{var scene=window.SceneManager&&SceneManager._scene; if(scene&&!isPersistenceScene(scene)) walkSceneWindows(scene,[],{count:0});}catch(e){reportRpgmError('refresh-scene-windows',e);}finally{refreshQueued=false;}}\n"
"  function requestLookup(key){\n"
"    if(pending[key]||pendingCount>=MAX_PENDING) return; pending[key]=true; pendingCount++; var x=null; var done=false;\n"
"    function finish(eventName){if(done) return; done=true; if(pending[key]){delete pending[key]; pendingCount--;} var now=Date.now(); if(x&&x.status===200){try{var r=JSON.parse(x.responseText); var v=r.translated_text||r.translation||key; if(v&&v!==key&&r.source!=='miss'&&r.source!=='queued'&&r.source!=='pass'){cache[key]=v; delete retryAfter[key]; requestWindowRefresh(); return;}}catch(e){reportRpgmError('parse-translate-response',e);}}else{reportRpgmError('translate-request-'+String(eventName||'http'),new Error('HTTP status '+(x?x.status:'unavailable')));} retryAfter[key]=now+MISS_RETRY_MS;}\n"
"    try{x=new XMLHttpRequest(); x.open('POST',URL,true); x.timeout=12000; x.setRequestHeader('Content-Type','application/json'); x.onreadystatechange=function(){if(x.readyState===4) finish('readystatechange');}; x.onload=function(){finish('load');}; x.onerror=function(){finish('error');}; x.ontimeout=function(){finish('timeout');}; x.send(JSON.stringify({text:key}));}\n"
"    catch(e){reportRpgmError('start-translate-request',e); if(pending[key]){delete pending[key]; pendingCount--;} retryAfter[key]=Date.now()+MISS_RETRY_MS;}\n"
"  }\n"
"  function lookup(s){\n"
"    s=String(s==null?'':s); if(!s||hasCjk(s)||isLayoutOnly(s)) return {text:s,hit:true}; var key=keyOf(s); if(!key) return {text:s,hit:true}; if(cache[key]) return {text:safeRpgmTranslation(s,cache[key]),hit:true}; var now=Date.now(); if(retryAfter[key]&&retryAfter[key]>now) return {text:s,hit:false};\n"
"    requestLookup(key); if(cache[key]) return {text:cache[key],hit:true};\n"
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
"    for(var i=0;i<list.length&&keys.length<MAX_QUEST_PRIME;i++){var q=list[i]; if(!q) continue; try{var name=typeof q.name==='function'?q.name():q.name; add(name); var split=String(name||'').lastIndexOf(' - '); if(split>=0) add(String(name).slice(split+3));}catch(e){reportRpgmError('read-quest-name',e);} try{addArray(typeof q.objectives==='function'?q.objectives():q.objectives);}catch(e){reportRpgmError('read-quest-objectives',e);} try{addArray(typeof q.desc==='function'?q.desc():q.desc);}catch(e){reportRpgmError('read-quest-description',e);} try{addArray(typeof q.resoTxtArray==='function'?q.resoTxtArray():null);}catch(e){reportRpgmError('read-quest-resolution',e);}}\n"
"    if(!keys.length) return; var signature=keys.join('\\n'); if(signature===questPrimeSignature) return; questPrimePending=true; var remaining=Math.ceil(keys.length/QUEST_PRIME_BATCH),allSucceeded=true;\n"
"    function settle(x,batch,eventName){var succeeded=false,changed=false; if(x&&x.status===200){try{var r=JSON.parse(x.responseText),results=r.results||[],sources=r.sources||[]; succeeded=true; for(var i=0;i<batch.length;i++){var value=results[i],source=sources[i]; if(value&&value!==batch[i]&&source!=='miss'&&source!=='queued'&&source!=='pass'){cache[batch[i]]=value; changed=true;}}}catch(e){reportRpgmError('parse-quest-prime-response',e);}}else{reportRpgmError('quest-prime-'+String(eventName||'http'),new Error('HTTP status '+(x?x.status:'unavailable')));} if(!succeeded) allSucceeded=false; if(changed) requestWindowRefresh(); remaining--; if(remaining<=0){questPrimePending=false; if(allSucceeded) questPrimeSignature=signature;}}\n"
"    for(var start=0;start<keys.length;start+=QUEST_PRIME_BATCH){(function(batch){var x=null,done=false; function finish(eventName){if(done) return; done=true; settle(x,batch,eventName);} try{x=new XMLHttpRequest(); x.open('POST',BATCH_URL,true); x.timeout=15000; x.setRequestHeader('Content-Type','application/json'); x.onreadystatechange=function(){if(x.readyState===4) finish('readystatechange');}; x.onload=function(){finish('load');}; x.onerror=function(){finish('error');}; x.ontimeout=function(){finish('timeout');}; x.send(JSON.stringify({texts:batch}));}catch(e){reportRpgmError('start-quest-prime-request',e); finish('constructor');}})(keys.slice(start,start+QUEST_PRIME_BATCH));}\n"
"  }\n"
"  function installQuestPrimeHooks(){try{if(window.DataManager&&DataManager.extractSaveContents&&!DataManager.extractSaveContents._dsQuestPrimeHook){var oldExtract=DataManager.extractSaveContents; var wrap=function(){var r=oldExtract.apply(this,arguments); primeGalvQuestCache(); return r;}; wrap._dsQuestPrimeHook=true; DataManager.extractSaveContents=wrap;} if(window.DataManager&&DataManager.setupNewGame&&!DataManager.setupNewGame._dsQuestPrimeHook){var oldSetup=DataManager.setupNewGame; var setupWrap=function(){var r=oldSetup.apply(this,arguments); primeGalvQuestCache(); return r;}; setupWrap._dsQuestPrimeHook=true; DataManager.setupNewGame=setupWrap;} primeGalvQuestCache();}catch(e){reportRpgmError('install-quest-prime-hooks',e);}}\n"
"  installQuestPrimeHooks();\n"
"  function lookupVisible(orig,prefix,body){var key=visibleLookupKey(body); if(!key||key===keyOf(body)) return null; var r=lookup(key); if(r.hit&&r.text!==key&&r.text!==keyOf(key)) return {text:String(prefix||'')+r.text,hit:true}; if(r.hit) return {text:orig,hit:true}; return {text:orig,hit:false};}\n"
"  function lookupDecorated(s){\n"
"    var orig=String(s==null?'':s); var raw=lookup(orig); if(raw.hit&&raw.text!==orig&&raw.text!==keyOf(orig)) return raw; var ww=/^(?:<WordWrap>)+/i.exec(orig); if(ww&&ww[0].length<orig.length){var inner=orig.slice(ww[0].length),wrapped=lookup(inner); if(wrapped.hit&&wrapped.text!==inner&&wrapped.text!==keyOf(inner)) return {text:ww[0]+wrapped.text,hit:true}; if(!wrapped.hit) raw.hit=false;} var parts=splitRpgmDecor(orig); if(parts){var body=lookup(parts.body); if(body.hit&&body.text!==parts.body&&body.text!==keyOf(parts.body)) return {text:parts.prefix+body.text,hit:true}; var vis=lookupVisible(orig,parts.prefix,parts.body); if(vis) return vis; if(body.hit&&raw.hit) return {text:orig,hit:true}; return {text:orig,hit:false};} var vis2=lookupVisible(orig,'',orig); if(vis2) return vis2; return raw;\n"
"  }\n"
"  function tr(s){return lookupDecorated(s).text;}\n"
"  var suppressDisplayTranslate=0;\n"
"  function syncLookupConvertedKeys(values,maxKeys){var keys=[],seen=Object.create(null),limit=Number(maxKeys)||64; if(limit<1) limit=64; if(limit>512) limit=512; for(var i=0;i<values.length&&keys.length<limit;i++){var key=keyOf(values[i]); if(!key||cache[key]||syncLookupTried[key]||seen[key]||hasCjk(key)||isLayoutOnly(key)||!/[A-Za-z]/.test(key)) continue; seen[key]=true; syncLookupTried[key]=true; syncLookupOrder.push(key); keys.push(key);} while(syncLookupOrder.length>4096){var old=syncLookupOrder.shift(); delete syncLookupTried[old];} for(var start=0;start<keys.length;start+=64){var batch=keys.slice(start,start+64); try{var x=new XMLHttpRequest(); x.open('POST',CACHE_LOOKUP_URL,false); x.setRequestHeader('Content-Type','application/json'); x.send(JSON.stringify({texts:batch})); if(x.status===200){var r=JSON.parse(x.responseText),hits=r.hits||{}; for(var k in hits){if(hits[k]&&hits[k]!==k) cache[k]=hits[k];}}else{reportRpgmError('sync-cache-lookup-http',new Error('HTTP status '+x.status));}}catch(e){reportRpgmError('sync-cache-lookup',e);}}}\n"
"  function translateConvertedRpgmText(s,syncFirst){var orig=String(s==null?'':s); if(orig.indexOf(String.fromCharCode(27))<0) return tr(orig); var parts=orig.split(/(\\x1b[A-Za-z]+(?:\\[[^\\]]{0,64}\\])?)/g),cores=[],allHit=true,changed=false; for(var i=0;i<parts.length;i++){var part=parts[i]; if(!part||part.charCodeAt(0)===27||hasCjk(part)||!/[A-Za-z]/.test(part)) continue; var lead=(part.match(/^\\s*/)||[''])[0],tail=(part.match(/\\s*$/)||[''])[0],core=part.slice(lead.length,part.length-tail.length); if(core&&/[A-Za-z]/.test(core)) cores.push(core);} if(syncFirst) syncLookupConvertedKeys(cores); for(var j=0;j<parts.length;j++){var value=parts[j]; if(!value||value.charCodeAt(0)===27||hasCjk(value)||!/[A-Za-z]/.test(value)) continue; var prefix=(value.match(/^\\s*/)||[''])[0],suffix=(value.match(/\\s*$/)||[''])[0],body=value.slice(prefix.length,value.length-suffix.length); if(!body||!/[A-Za-z]/.test(body)) continue; var result=lookupDecorated(body); if(!result.hit) allHit=false; if(result.text!==body){parts[j]=prefix+result.text+suffix; changed=true;}} return allHit&&changed?parts.join(''):orig;}\n"
"  function trDisplay(s){return suppressDisplayTranslate>0?String(s==null?'':s):translateConvertedRpgmText(s);}\n"
"  function withDisplayTranslationSuppressed(fn){suppressDisplayTranslate++; try{return fn();}finally{suppressDisplayTranslate--;}}\n"
"  function shouldBitmapTranslate(s){var key=keyOf(s); if(!key||key.length<3||hasCjk(key)) return false; if(!/[A-Za-z]/.test(key)) return false; return true;}\n"
"  function trBitmap(s){var orig=String(s==null?'':s); return suppressDisplayTranslate>0?orig:(shouldBitmapTranslate(orig)?tr(orig):orig);}\n"
"  function finiteDrawWidth(w){w=Number(w); return isFinite(w)&&w>0&&w<1000000;}\n"
"  function drawBitmapFit(oldDraw,self,text,x,y,maxWidth,lineHeight,align){var out; try{out=trBitmap(text);}catch(e){reportRpgmError('translate-bitmap-text',e); out=String(text==null?'':text);} if(!finiteDrawWidth(maxWidth)||!hasCjk(out)||!self||typeof self.measureTextWidth!=='function'||typeof self.fontSize!=='number') return oldDraw.call(self,out,x,y,maxWidth,lineHeight,align); var oldSize=self.fontSize; try{var min=Math.max(12,Math.floor(oldSize*0.62)); while(self.fontSize>min&&self.measureTextWidth(out)>maxWidth){self.fontSize--;} return oldDraw.call(self,out,x,y,maxWidth,lineHeight,align);}finally{self.fontSize=oldSize;}}\n"
"  function dsTextWidth(win,t){try{if(win&&win.textWidth) return win.textWidth(t); if(win&&win.contents&&win.contents.measureTextWidth) return win.contents.measureTextWidth(t);}catch(e){reportRpgmError('measure-renderer-text',e);} return String(t==null?'':t).length*16;}\n"
"  function dsLineHeight(win){try{if(win&&win.techTreeLineHeight) return win.techTreeLineHeight(); if(win&&win.lineHeight) return win.lineHeight();}catch(e){reportRpgmError('read-renderer-line-height',e);} return 36;}\n"
"  var rpgmTextExWrapStack=[];\n"
"  var rpgmLayoutFallbackCounts=Object.create(null);\n"
"  function reportRpgmLayoutFallback(context,detail){var key=String(context||'unknown'),count=(rpgmLayoutFallbackCounts[key]||0)+1; rpgmLayoutFallbackCounts[key]=count; if(count<=3||(count&(count-1))===0){var record={context:key,count:count,detail:String(detail||'')}; var records=window.__deepSeekRpgmDiagnostics||(window.__deepSeekRpgmDiagnostics=[]); records.push(record); while(records.length>32) records.shift(); if(window.console&&typeof console.info==='function') console.info('[DeepSeek RPGM] '+key+' #'+count+': '+record.detail);}}\n"
"  function rpgmTextExWidth(win,x,w){var left=Number(x); if(!isFinite(left)) left=0; if(finiteDrawWidth(w)) return Number(w); if(win&&win.contents&&finiteDrawWidth(win.contents.width-left)) return Number(win.contents.width)-left; if(win&&finiteDrawWidth(Number(win.innerWidth)-left)) return Number(win.innerWidth)-left; return 0;}\n"
"  function withRpgmTextExWrap(win,text,x,w,translated,draw){var width=rpgmTextExWidth(win,x,w); if(!translated||!hasCjk(text)||!finiteDrawWidth(width)) return draw(); var left=Number(x); if(!isFinite(left)) left=0; var state={win:win,left:left,right:left+width}; rpgmTextExWrapStack.push(state); reportRpgmLayoutFallback('draw-text-ex-auto-wrap','width='+width+', chars='+String(text==null?'':text).length); try{return draw();}finally{rpgmTextExWrapStack.pop();}}\n"
"  function currentRpgmTextExWrap(win){for(var i=rpgmTextExWrapStack.length-1;i>=0;i--){if(rpgmTextExWrapStack[i].win===win) return rpgmTextExWrapStack[i];} return null;}\n"
"  function rpgmNextWrapUnit(text,index){text=String(text==null?'':text); var unit=text.charAt(index); if(!unit) return ''; var i=index+1; if(/[A-Za-z0-9_'-]/.test(unit)){while(i<text.length&&/[A-Za-z0-9_'-]/.test(text.charAt(i))) unit+=text.charAt(i++);} while(i<text.length&&/[\\u3001\\u3002\\uff0c\\uff0e\\uff01\\uff1f\\uff1a\\uff1b\\uff09\\u3011\\u300b\\u300d\\u300f\\u2019\\u201d\\u2026]/.test(text.charAt(i))) unit+=text.charAt(i++); return unit;}\n"
"  function installTextExAutoWrapHook(){try{var proto=window.Window_Base&&Window_Base.prototype; if(!proto||typeof proto.processNormalCharacter!=='function'||typeof proto.processNewLine!=='function'||proto.processNormalCharacter._dsRpgmTextExWrapHook) return; var old=proto.processNormalCharacter; var wrap=function(textState){var layout=currentRpgmTextExWrap(this); if(layout&&textState&&typeof textState.index==='number'){var x=Number(textState.x),unit=rpgmNextWrapUnit(textState.text,textState.index); if(unit&&!/\\s/.test(unit.charAt(0))&&isFinite(x)&&x>layout.left&&x+dsTextWidth(this,unit)>layout.right){var index=textState.index; this.processNewLine(textState); textState.index=index;}} return old.apply(this,arguments);}; wrap._dsRpgmTextExWrapHook=true; wrap._dsRpgmOriginal=old; proto.processNormalCharacter=wrap;}catch(e){reportRpgmError('install-draw-text-ex-auto-wrap-hook',e);}}\n"
"  function drawCjkAutoWrap(win,text,x,y,maxWidth){text=String(text==null?'':text); try{if(win.convertEscapeCharacters) text=win.convertEscapeCharacters(text);}catch(e){reportRpgmError('convert-renderer-escapes',e);} if(!text) return 0; var esc=String.fromCharCode(27); var max=finiteDrawWidth(maxWidth)?Number(maxWidth):1000000; var lh=dsLineHeight(win); var lines=1,x2=0,y2=y; function nl(){lines++; y2+=lh; x2=0;} function drawTok(tok,w){if(x2>0&&x2+w>max) nl(); if(tok===' '&&x2===0) return; win.drawText(tok,x+x2,y2,w,'left'); x2+=w;} for(var i=0;i<text.length;){var rest=text.slice(i); if(text.charAt(i)===esc){var mi=/^\\x1bI\\[(\\d+)\\]/.exec(rest); if(mi){var iw=(window.Window_Base&&Window_Base._iconWidth)||32; if(x2>0&&x2+iw>max) nl(); if(win.drawIcon) win.drawIcon(Number(mi[1]),x+x2,y2); x2+=iw; i+=mi[0].length; continue;} var mc=/^\\x1bC\\[(\\d+)\\]/.exec(rest); if(mc){if(win.changeTextColor&&win.textColor) win.changeTextColor(win.textColor(Number(mc[1]))); i+=mc[0].length; continue;} if(rest.indexOf(esc+'n')===0){nl(); i+=2; continue;}} var ch=text.charAt(i); if(ch==='\\r'||ch==='\\n'){if(ch==='\\r'&&text.charAt(i+1)==='\\n') i++; nl(); i++; continue;} if(/\\s/.test(ch)){drawTok(' ',dsTextWidth(win,' ')); i++; continue;} var tok=ch; if(!hasCjk(ch)){var j=i+1; while(j<text.length&&text.charAt(j)!==esc&&!/\\s/.test(text.charAt(j))&&!hasCjk(text.charAt(j))) j++; tok=text.slice(i,j); i=j;}else{i++;} drawTok(tok,dsTextWidth(win,tok));} return lines;}\n"
"  function translateTextArray(values,allowPartial){\n"
"    if(!values||!values.length) return values; var out=new Array(values.length); var allHit=true; var changed=false;\n"
"    for(var i=0;i<values.length;i++){var orig=String(values[i]==null?'':values[i]); var r=lookupDecorated(orig); out[i]=r.text; if(!r.hit) allHit=false; if(r.text!==orig) changed=true;}\n"
"    if(!allHit&&changed&&!allowPartial) return values; return out;\n"
"  }\n"
"  function planMessagePage(original){var candidates=[]; for(var ci=0;ci<original.length;ci++){var line=original[ci]; candidates.push(line); var unwrapped=line.replace(/^(?:<WordWrap>)+/i,''); if(unwrapped!==line) candidates.push(unwrapped); var decor=splitRpgmDecor(line),body=decor?decor.body:unwrapped; if(body&&body!==line&&body!==unwrapped) candidates.push(body); var visible=visibleLookupKey(body); if(visible) candidates.push(visible);} var joined=original.join('\\n'); return {candidates:candidates,joined:joined,large:original.length>24||joined.length>4000};}\n"
"  function translateMessageLines(lines){\n"
"    if(!lines||!Array.isArray(lines)||!lines.length) return lines; var original=new Array(lines.length); for(var i=0;i<lines.length;i++) original[i]=String(lines[i]==null?'':lines[i]);\n"
"    var plan=planMessagePage(original); syncLookupConvertedKeys(plan.candidates,512);\n"
"    if(!plan.large&&original.length>1){var whole=lookup(plan.joined); if(whole.hit&&whole.text!==plan.joined&&whole.text!==keyOf(plan.joined)) return [whole.text];}\n"
"    if(!plan.large&&original.length>1){var first=splitRpgmDecor(original[0]); if(first){var visibleLines=new Array(original.length); visibleLines[0]=visibleLookupKey(first.body)||first.body; for(var vi=1;vi<original.length;vi++){var p=splitRpgmDecor(original[vi]); var b=p?p.body:original[vi]; visibleLines[vi]=visibleLookupKey(b)||b;} var vjoined=visibleLines.join('\\n'); var vwhole=lookup(vjoined); if(vwhole.hit&&vwhole.text!==vjoined&&vwhole.text!==keyOf(vjoined)) return [first.prefix+vwhole.text];}}\n"
"    return translateTextArray(original,plan.large);\n"
"  }\n"
"  function installMessageRuntimeHooks(){\n"
"    try{if(window.Game_Message&&Game_Message.prototype.allText&&!Game_Message.prototype.allText._dsRpgmMessageHook){var currentAllText=Game_Message.prototype.allText; var allTextWrap=(function(old){var fn=function(){var original=this&&this._texts; if(original&&Array.isArray(original)&&original.length){try{var translated=translateMessageLines(original); if(translated&&translated!==original) return translated.join('\\n');}catch(e){reportRpgmError('translate-message-all-text',e);}} return old.apply(this,arguments);}; fn._dsRpgmMessageHook=true; fn._dsRpgmOriginal=old; return fn;})(currentAllText); Game_Message.prototype.allText=allTextWrap;}}catch(e){reportRpgmError('install-message-all-text-hook',e);}\n"
"    try{if(window.Window_Message&&Window_Message.prototype.startMessage&&!Window_Message.prototype.startMessage._dsRpgmMessageHook){var currentStartMessage=Window_Message.prototype.startMessage; var startMessageWrap=(function(old){var fn=function(){var gm=window.$gameMessage; var original=gm&&gm._texts; if(!original||!Array.isArray(original)||!original.length) return old.apply(this,arguments); var translated; try{translated=translateMessageLines(original);}catch(e){reportRpgmError('translate-message-start',e); return old.apply(this,arguments);} if(translated===original) return old.apply(this,arguments); gm._texts=translated; try{return old.apply(this,arguments);} finally{gm._texts=original;}}; fn._dsRpgmMessageHook=true; fn._dsRpgmOriginal=old; return fn;})(currentStartMessage); Window_Message.prototype.startMessage=startMessageWrap;}}catch(e){reportRpgmError('install-message-start-hook',e);}\n"
"  }\n"
"  if(window.Window_Help&&Window_Help.prototype.setText&&!Window_Help.prototype._dsSetText){var oldHelpSet=Window_Help.prototype.setText; Window_Help.prototype._dsSetText=oldHelpSet; Window_Help.prototype.setText=function(text){markRpgmDisplayTarget(this,text); var out=text; try{out=tr(text);}catch(e){reportRpgmError('translate-help-text',e);} var rendered=String(out==null?'':out),source=String(text==null?'':text); this._dsRpgmTranslatedCjkText=rendered!==source&&hasCjk(rendered)?rendered:''; return oldHelpSet.call(this,out);};}\n"
"  if(window.Game_Message&&Game_Message.prototype.choices&&!Game_Message.prototype._dsChoices){var oldChoices=Game_Message.prototype.choices; Game_Message.prototype._dsChoices=oldChoices; Game_Message.prototype.choices=function(){var choices=oldChoices.apply(this,arguments); if(choices&&choices.length){try{var translated=translateTextArray(choices); if(translated&&translated!==choices) return translated;}catch(e){reportRpgmError('translate-message-choices',e);}} return choices;};}\n"
"  function applyTranslatedChoiceCommands(win){\n"
"    try{var list=win&&win._list; if(!list||!list.length) return; var names=new Array(list.length); for(var i=0;i<list.length;i++) names[i]=list[i]&&list[i].name; var translated=translateTextArray(names); for(var j=0;j<list.length;j++){if(list[j]&&typeof list[j].name==='string') list[j].name=translated[j];}}\n"
"    catch(e){reportRpgmError('translate-choice-commands',e);}\n"
"  }\n"
"  if(window.Window_ChoiceList&&Window_ChoiceList.prototype.makeCommandList&&!Window_ChoiceList.prototype._dsMakeCommandList){var oldChoiceMake=Window_ChoiceList.prototype.makeCommandList; Window_ChoiceList.prototype._dsMakeCommandList=oldChoiceMake; Window_ChoiceList.prototype.makeCommandList=function(){var r=oldChoiceMake.apply(this,arguments); applyTranslatedChoiceCommands(this); return r;};}\n"
"  if(window.Window_ChoiceList&&Window_ChoiceList.prototype.maxChoiceWidth&&!Window_ChoiceList.prototype._dsMaxChoiceWidth){var oldChoiceWidth=Window_ChoiceList.prototype.maxChoiceWidth; Window_ChoiceList.prototype._dsMaxChoiceWidth=oldChoiceWidth; Window_ChoiceList.prototype.maxChoiceWidth=function(){var w=oldChoiceWidth.apply(this,arguments); try{var gm=window.$gameMessage; var choices=gm&&gm.choices?gm.choices():null; if(choices&&choices.length){var translated=translateTextArray(choices); for(var i=0;i<translated.length;i++){var t=translated[i]; var extra=0; if(this.textWidthEx) extra=this.textWidthEx(t); else if(this.textWidth) extra=this.textWidth(t); else extra=String(t==null?'':t).length; if(this.textPadding) extra+=this.textPadding()*2; if(w<extra) w=extra;}}}catch(e){reportRpgmError('measure-choice-width',e);} return w;};}\n"
"  function installCopiedDrawTextExHooks(){try{if(!window.Window_Base||!Window_Base.prototype._dsDrawTextEx) return; var original=Window_Base.prototype._dsDrawTextEx,count=0; function patch(proto){if(count>=64||!proto||proto===Window_Base.prototype||proto.drawTextEx!==original) return; var wrap=(function(old){var fn=function(text,x,y,w){var self=this; markRpgmDisplayTarget(self,text); var out=text; try{out=translateConvertedRpgmText(text,true);}catch(e){reportRpgmError('translate-copied-draw-text-ex',e);} var changed=String(out==null?'':out)!==String(text==null?'':text); return withRpgmTextExWrap(self,out,x,w,changed,function(){return old.call(self,out,x,y,w);});}; fn._dsRpgmDisplayHook=true; return fn;})(proto.drawTextEx); proto._dsDrawTextEx=proto.drawTextEx; proto.drawTextEx=wrap; count++;} for(var name in window){if(count>=64) break; if(!/^(?:Sprite|Window)_/.test(name)) continue; var ctor=window[name]; patch(ctor&&ctor.prototype);} var types=window.HUDManager&&HUDManager.types; if(types){for(var typeName in types){if(count>=64) break; var entry=types[typeName],registered=entry&&entry.class; patch(registered&&registered.prototype);}}}catch(e){reportRpgmError('install-copied-draw-text-ex-hooks',e);}}\n"
"  function installDisplayTextHooks(){\n"
"    try{if(window.Bitmap&&Bitmap.prototype.drawText&&!Bitmap.prototype.drawText._dsRpgmDisplayHook){var oldBitmapDraw=Bitmap.prototype.drawText; var bitmapWrap=function(text,x,y,maxWidth,lineHeight,align){markRpgmDisplayTarget(this,text); return drawBitmapFit(oldBitmapDraw,this,text,x,y,maxWidth,lineHeight,align);}; bitmapWrap._dsRpgmDisplayHook=true; Bitmap.prototype._dsBitmapDrawText=oldBitmapDraw; Bitmap.prototype.drawText=bitmapWrap;}}catch(e){reportRpgmError('install-bitmap-draw-hook',e);}\n"
"    try{if(window.Window_Base&&Window_Base.prototype.drawTextAutoWrap&&!Window_Base.prototype.drawTextAutoWrap._dsRpgmDisplayHook){var oldAutoWrap=Window_Base.prototype.drawTextAutoWrap; var autoWrap=function(baseText,x,y,maxWidth){var self=this,args=arguments; markRpgmDisplayTarget(self,baseText); var orig,translated; try{orig=String(baseText==null?'':baseText); translated=tr(orig);}catch(e){reportRpgmError('translate-auto-wrap-text',e); return oldAutoWrap.apply(self,args);} if(hasCjk(translated)){try{return drawCjkAutoWrap(self,translated,x,y,maxWidth);}catch(e){reportRpgmError('draw-cjk-auto-wrap',e); return withDisplayTranslationSuppressed(function(){return oldAutoWrap.apply(self,args);});}} if(translated!==orig) return withDisplayTranslationSuppressed(function(){return oldAutoWrap.call(self,translated,x,y,maxWidth);}); return withDisplayTranslationSuppressed(function(){return oldAutoWrap.apply(self,args);});}; autoWrap._dsRpgmDisplayHook=true; Window_Base.prototype._dsDrawTextAutoWrap=oldAutoWrap; Window_Base.prototype.drawTextAutoWrap=autoWrap;}}catch(e){reportRpgmError('install-auto-wrap-hook',e);}\n"
"    try{if(window.Window_Base&&Window_Base.prototype.drawTextEx&&!Window_Base.prototype.drawTextEx._dsRpgmDisplayHook){var old=Window_Base.prototype.drawTextEx; var drawTextExWrap=function(text,x,y,w){var self=this; markRpgmDisplayTarget(self,text); var out=text; try{out=trDisplay(text);}catch(e){reportRpgmError('translate-draw-text-ex',e);} var rendered=String(out==null?'':out),changed=rendered!==String(text==null?'':text)||self._dsRpgmTranslatedCjkText===rendered; return withRpgmTextExWrap(self,out,x,w,changed,function(){return old.call(self,out,x,y,w);});}; drawTextExWrap._dsRpgmDisplayHook=true; Window_Base.prototype._dsDrawTextEx=old; Window_Base.prototype.drawTextEx=drawTextExWrap;}}catch(e){reportRpgmError('install-draw-text-ex-hook',e);}\n"
"    try{if(window.Window_Base&&Window_Base.prototype.drawText&&!Window_Base.prototype.drawText._dsRpgmDisplayHook){var old2=Window_Base.prototype.drawText; var drawTextWrap=function(text,x,y,w,a){markRpgmDisplayTarget(this,text); var out=text; try{out=trDisplay(text);}catch(e){reportRpgmError('translate-draw-text',e);} return old2.call(this,out,x,y,w,a);}; drawTextWrap._dsRpgmDisplayHook=true; Window_Base.prototype._dsDrawText=old2; Window_Base.prototype.drawText=drawTextWrap;}}catch(e){reportRpgmError('install-draw-text-hook',e);}\n"
"    installTextExAutoWrapHook();\n"
"    installCopiedDrawTextExHooks();\n"
"  }\n"
"  function installAllRpgmHooks(){installQuestPrimeHooks(); installMessageRuntimeHooks(); installDisplayTextHooks();}\n"
"  function installPluginLoadHook(){try{if(!window.PluginManager||!PluginManager.loadScript||PluginManager.loadScript._dsRpgmPluginLoadHook) return; var oldLoadScript=PluginManager.loadScript; var loadScriptWrap=function(){var r=oldLoadScript.apply(this,arguments); try{var scripts=document.getElementsByTagName('script'),script=scripts&&scripts[scripts.length-1]; if(script&&script.addEventListener) script.addEventListener('load',installAllRpgmHooks,false);}catch(e){reportRpgmError('attach-plugin-load-listener',e);} return r;}; loadScriptWrap._dsRpgmPluginLoadHook=true; loadScriptWrap._dsRpgmOriginal=oldLoadScript; PluginManager.loadScript=loadScriptWrap;}catch(e){reportRpgmError('install-plugin-load-hook',e);}}\n"
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
    _snwprintf(src, MAX_PATH * 4, L"%s\\Fonts\\simhei.ttf", windir);
    src[MAX_PATH * 4 - 1] = 0;
    if (exists_path(src) && copy_file_safe(src, ttf_dst)) {
        append_log(L"Ren'Py：已部署中文字体（黑体）：%s", ttf_dst);
        return;
    }
    _snwprintf(src, MAX_PATH * 4, L"%s\\Fonts\\msyh.ttc", windir);
    src[MAX_PATH * 4 - 1] = 0;
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
 * 将系统 CJK 字体复制到 www/fonts/ 目录供 hook 脚本引用。
 * ---------------------------------------------------------------- */
static void deploy_rpgm_font(const WCHAR *dir) {
    WCHAR font_dir[MAX_PATH * 4], ttf_dst[MAX_PATH * 4], ttc_dst[MAX_PATH * 4];
    path_join(font_dir, MAX_PATH * 4, dir, L"www\\fonts");
    ensure_dir(font_dir);
    path_join(ttf_dst, MAX_PATH * 4, font_dir, L"ds_font.ttf");
    path_join(ttc_dst, MAX_PATH * 4, font_dir, L"ds_font.ttc");
    if (exists_path(ttf_dst) || exists_path(ttc_dst)) return;

    WCHAR windir[MAX_PATH];
    if (!GetWindowsDirectoryW(windir, MAX_PATH)) return;

    WCHAR src[MAX_PATH * 4];
    _snwprintf(src, MAX_PATH * 4, L"%s\\Fonts\\simhei.ttf", windir);
    src[MAX_PATH * 4 - 1] = 0;
    if (exists_path(src) && copy_file_safe(src, ttf_dst)) {
        append_log(L"RPGM MV/MZ: deployed CJK font: %s", ttf_dst);
        return;
    }

    _snwprintf(src, MAX_PATH * 4, L"%s\\Fonts\\msyh.ttc", windir);
    src[MAX_PATH * 4 - 1] = 0;
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
int deploy_renpy(const WCHAR *dir) {
    WCHAR game[MAX_PATH * 4], hook[MAX_PATH * 4];
    path_join(game, MAX_PATH * 4, dir, L"game");
    if (!is_dir(game)) return 0;
    path_join(hook, MAX_PATH * 4, game, L"iron_deepseek.rpy");
    if (!write_text_file_utf8(hook, RENPY_HOOK)) return 0;
    deploy_renpy_font(game);
    append_log(L"已部署 Ren'Py hook：%s", hook);
    return 1;
}

static int backup_file_once(const WCHAR *path, const WCHAR *suffix) {
    WCHAR backup[MAX_PATH * 4];
    _snwprintf(backup, MAX_PATH * 4, L"%s%s", path, suffix);
    backup[MAX_PATH * 4 - 1] = 0;
    DWORD attr = GetFileAttributesW(backup);
    if (attr != INVALID_FILE_ATTRIBUTES) return !(attr & FILE_ATTRIBUTE_DIRECTORY);
    if (CopyFileW(path, backup, TRUE)) return 1;
    return GetLastError() == ERROR_FILE_EXISTS;
}

static int write_file_bytes_atomic(const WCHAR *path, const char *data, DWORD size) {
    WCHAR temp[MAX_PATH * 4];
    _snwprintf(temp, MAX_PATH * 4, L"%s.dst-tmp", path);
    temp[MAX_PATH * 4 - 1] = 0;
    DeleteFileW(temp);
    if (!write_file_bytes(temp, data, size)) return 0;
    if (MoveFileExW(temp, path, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) return 1;
    DeleteFileW(temp);
    return 0;
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
        while ((p = strstr(p, "<script")) != NULL && p < hit) {
            tag_start = p;
            p += strlen("<script");
        }
        if (!tag_start) {
            scan = hit + strlen(hook_name);
            continue;
        }
        const char *open_end = strchr(tag_start, '>');
        const char *src_attr = strstr(tag_start, "src");
        if (!open_end || open_end >= end || hit > open_end ||
            !src_attr || src_attr > hit || src_attr > open_end) {
            scan = hit + strlen(hook_name);
            continue;
        }
        const char *tag_end = strstr(open_end, "</script>");
        if (!tag_end || tag_end >= end) {
            scan = hit + strlen(hook_name);
            continue;
        }
        tag_end += strlen("</script>");
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
 * 1. 将 RPGM_HOOK JS 脚本写入 www/js/hook_rpgm_mv.js
 * 2. 调用 deploy_rpgm_font 部署 CJK 字体到 www/fonts/
 * 3. 修改 www/index.html，在 </body> 前插入 <script> 标签引用 hook
 * ---------------------------------------------------------------- */
int deploy_rpgm(const WCHAR *dir) {
    WCHAR jsdir[MAX_PATH * 4], hook[MAX_PATH * 4], index[MAX_PATH * 4];
    path_join(jsdir, MAX_PATH * 4, dir, L"www\\js");
    if (!is_dir(jsdir)) return 0;
    path_join(hook, MAX_PATH * 4, jsdir, L"hook_rpgm_mv.js");
    if (!write_text_file_utf8(hook, RPGM_HOOK)) return 0;
    deploy_rpgm_font(dir);

    path_join(index, MAX_PATH * 4, dir, L"www\\index.html");
    char *html = NULL;
    DWORD sz = 0;
    if (!read_file_bytes(index, &html, &sz)) return 0;

    const char *script = "\n<script type=\"text/javascript\" src=\"js/hook_rpgm_mv.js\"></script>\n";
    ByteBuf stripped = {0}, out = {0};
    int ok = 0;
    stripped.cap = (size_t)sz + 1;
    stripped.data = (char *)malloc(stripped.cap);
    if (!stripped.data) goto done;
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
    if (!out.data) goto done;
    out.data[0] = 0;
    bb_add(&out, base, (size_t)(insert - base));
    bb_add(&out, script, strlen(script));
    bb_add(&out, insert, (size_t)((base + stripped.len) - insert));
    if (out.len != stripped.len + strlen(script)) goto done;
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
    _snwprintf(disabled, MAX_PATH * 4, L"%s.disabled", path);
    disabled[MAX_PATH * 4 - 1] = 0;
    DeleteFileW(disabled);
    return MoveFileExW(path, disabled, MOVEFILE_REPLACE_EXISTING | MOVEFILE_COPY_ALLOWED);
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

/* ----------------------------------------------------------------
 * write_xunity_config — 生成 XUnity.AutoTranslator 配置文件
 *
 * 写入 BepInEx/config/AutoTranslatorConfig.ini，
 * 配置 XUnity 使用本地 DeepSeek 端点 (http://127.0.0.1:19999)，
 * 语言方向 auto→zh-CN，并启用所有 UI 文本框架（UGUI/TMP/NGUI 等）。
 * ---------------------------------------------------------------- */
static void write_xunity_config(const WCHAR *dir) {
    WCHAR cfgdir[MAX_PATH * 4], cfg[MAX_PATH * 4];
    path_join(cfgdir, MAX_PATH * 4, dir, L"BepInEx\\config");
    ensure_dir(cfgdir);
    path_join(cfg, MAX_PATH * 4, cfgdir, L"AutoTranslatorConfig.ini");

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
        "MaxCharactersPerTranslation=400\n"
        "IgnoreWhitespaceInDialogue=True\n"
        "MinDialogueChars=20\n"
        "ForceSplitTextAfterCharacters=0\n"
        "CopyToClipboard=False\n"
        "MaxClipboardCopyCharacters=2500\n"
        "ClipboardDebounceTime=1.25\n"
        "EnableUIResizing=False\n"
        "EnableBatching=True\n"
        "UseStaticTranslations=True\n"
        "OverrideFont=Microsoft YaHei\n"
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
        "TranslationDelay=0.1\n"
        "DisplaySafePunctuation=True\n";
    write_text_file_utf8(cfg, XUNITY_CONFIG_FMT);
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
static int install_bepinex_mono_runtime(const WCHAR *dir, int use_bepinex6) {
    WCHAR mono_rt[MAX_PATH * 4];
    path_join(mono_rt, MAX_PATH * 4, g_root, use_bepinex6 ? L"payloads\\UnityMonoRuntime6" : L"payloads\\UnityMonoRuntime");
    if (!is_dir(mono_rt)) {
        append_log(use_bepinex6
            ? L"Unity: missing BepInEx 6 Mono runtime payload (payloads\\UnityMonoRuntime6)."
            : L"Unity: missing BepInEx 5 Mono runtime payload (payloads\\UnityMonoRuntime).");
        log_payload_install_command(use_bepinex6 ? L"-UnityMono6" : L"-UnityMono5");
        return 0;
    }

    int ok = 1;
    ok &= copy_payload_file(mono_rt, L"winhttp.dll", dir);
    ok &= copy_payload_file(mono_rt, L"doorstop_config.ini", dir);
    ok &= copy_payload_file(mono_rt, L".doorstop_version", dir);
    ok &= copy_payload_tree(mono_rt, L"BepInEx\\core", dir);
    if (!ok) {
        append_log(use_bepinex6
            ? L"Unity: BepInEx 6 Mono runtime deployment is incomplete; check payloads\\UnityMonoRuntime6."
            : L"Unity: BepInEx 5 Mono runtime deployment is incomplete; check payloads\\UnityMonoRuntime.");
        log_payload_install_command(use_bepinex6 ? L"-UnityMono6 -Force" : L"-UnityMono5 -Force");
        return 0;
    }
    append_log(use_bepinex6
        ? L"Unity: deployed BepInEx 6 (Mono) runtime for Unity 6+."
        : L"Unity: deployed BepInEx 5 (Mono) runtime.");
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
static int ensure_bepinex_mono(const WCHAR *dir) {
    int major = detect_unity_major(dir);
    int use_bepinex6 = major >= 6000;
    WCHAR bep[MAX_PATH * 4];
    path_join(bep, MAX_PATH * 4, dir, L"BepInEx");
    if (is_dir(bep)) {
        if (use_bepinex6 && !unity_has_bepinex6_mono(dir)) {
            append_log(L"Unity %d (Unity 6+): existing BepInEx is not Unity.Mono 6; updating runtime files.", major);
            return install_bepinex_mono_runtime(dir, 1);
        }
        return 1; /* 非Unity6路径保留用户已有的BepInEx */
    }
    return install_bepinex_mono_runtime(dir, use_bepinex6);
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

    int use_bepinex6 = unity_has_bepinex6_mono(dir);
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
    if (!copy_file_safe(src, dll)) return 0;
    if (!find_unity_payload_file(json_src, MAX_PATH * 4, L"Newtonsoft.Json.dll")) {
        append_log(L"Unity: missing Newtonsoft.Json.dll dependency; UnityTranslator cannot start.");
        log_payload_install_command(L"-Newtonsoft");
        return 0;
    }
    path_join(json_dst, MAX_PATH * 4, plugins, L"Newtonsoft.Json.dll");
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
    WCHAR dll[MAX_PATH * 4], pdb[MAX_PATH * 4], il2cpp_mscorlib[MAX_PATH * 4], runtime[MAX_PATH * 4], xunity[MAX_PATH * 4], fontfix[MAX_PATH * 4], fontplugin[MAX_PATH * 4], gameasm[MAX_PATH * 4], endpoint_src[MAX_PATH * 4], endpoint_dst[MAX_PATH * 4];
    path_join(dll, MAX_PATH * 4, dir, L"BepInEx\\plugins\\UnityTranslator.dll");
    path_join(pdb, MAX_PATH * 4, dir, L"BepInEx\\plugins\\UnityTranslator.pdb");
    path_join(il2cpp_mscorlib, MAX_PATH * 4, dir, L"BepInEx\\core\\Il2Cppmscorlib.dll");
    path_join(gameasm, MAX_PATH * 4, dir, L"GameAssembly.dll");

    /* 只支持 x64 IL2CPP 构建 */
    int machine = pe_machine(gameasm);
    if (machine && machine != 0x8664) {
        append_log(L"Unity IL2CPP：当前只内置 x64 插件运行时，已跳过非 x64 游戏。");
        return 0;
    }

    /* 如果存在旧的 Mono 版插件，禁用它避免冲突 */
    if (is_bundled_unity_mono_plugin(dll) && disable_existing_file(dll)) {
        append_log(L"Unity IL2CPP：已禁用旧的 Mono UnityTranslator.dll：%s.disabled", dll);
        disable_existing_file(pdb);
    } else if (exists_path(dll)) {
        append_log(L"Unity IL2CPP：保留现有 UnityTranslator.dll（不是内置 Mono 模板）。");
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
    ok &= copy_payload_tree(xunity, L"BepInEx\\plugins\\XUnity.AutoTranslator", dir);
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
    if (exists_path(il2cpp_mscorlib) && disable_existing_file(il2cpp_mscorlib)) {
        append_log(L"Unity IL2CPP：已禁用旧的 core\\Il2Cppmscorlib.dll，避免遮挡新 interop。");
    }

    /* 生成 XUnity 配置文件 */
    write_xunity_config(dir);
    append_log(L"Unity IL2CPP: deployed TMP Chinese system font fallback.");
    append_log(L"Unity IL2CPP：已部署 BepInEx be.755 + XUnity AutoTranslator。");
    append_log(L"Unity IL2CPP：XUnity 已配置为使用本地 DeepSeek 批量端点 http://127.0.0.1:19999。");
    return 1;
}
