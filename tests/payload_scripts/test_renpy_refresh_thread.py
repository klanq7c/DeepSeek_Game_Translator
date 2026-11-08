"""在工作线程与主线程边界执行正式刷新函数。"""
import ast
import gc
import sys
import threading
import unittest
import weakref
from pathlib import Path
from types import SimpleNamespace

HOOK = Path(__file__).resolve().parents[2] / 'payloads/RenPy/iron_deepseek.rpy'
body = '\n'.join(line[4:] if line else '' for line in HOOK.read_text(encoding='utf-8').splitlines()[1:])
tree = ast.parse(body, str(HOOK))
names = {'_ds_refresh_interaction', '_ds_refresh_visible_text',
         '_ds_flush_legacy_refresh', '_ds_install_legacy_refresh_hook', '_ds_memo_put', '_ds_memo_get',
         '_ds_capture_text_key', '_ds_install_text_displayable_hook'}
nodes = [n for n in tree.body if isinstance(n, ast.FunctionDef) and n.name in names]
code = compile(ast.Module(body=nodes, type_ignores=[]), str(HOOK), 'exec')


class RefreshTests(unittest.TestCase):
    def memo_fixture(self, now=10.0):
        import collections
        errors = []
        clock = SimpleNamespace(now=now)
        env = {'_ds_lock': threading.RLock(), '_ds_memo': {},
               '_ds_memo_order': collections.deque(),
               '_ds_time': SimpleNamespace(time=lambda: clock.now),
               '_ds_report_exception': lambda *a: errors.append(a)}
        exec(code, env)
        return env, clock, errors

    def test_new_translation_can_correct_an_existing_positive_entry(self):
        env, clock, errors = self.memo_fixture()
        env['_ds_memo_put']('Continue.', '继续。', 1.0)
        env['_ds_memo_put']('Continue.', '接着走。', 2.0)
        env['_ds_memo_put']('Unrelated miss.', 'Unrelated miss.', 9.0)
        self.assertEqual(env['_ds_memo_get']('Continue.'), '接着走。')
        self.assertEqual(env['_ds_memo_get']('Unrelated miss.'), 'Unrelated miss.')
        self.assertEqual(list(env['_ds_memo_order']), ['Continue.', 'Unrelated miss.'])
        self.assertEqual(errors, [])

    def test_negative_ttl_and_positive_lifetime_are_preserved(self):
        env, clock, errors = self.memo_fixture(5.9)
        env['_ds_memo_put']('Pending.', 'Pending.', 1.0)
        env['_ds_memo_put']('Ready.', '准备好了。', 1.0)
        self.assertEqual(env['_ds_memo_get']('Pending.'), 'Pending.')
        clock.now = 6.1
        self.assertIsNone(env['_ds_memo_get']('Pending.'))
        clock.now = 10000.0
        self.assertEqual(env['_ds_memo_get']('Ready.'), '准备好了。')
        self.assertEqual(errors, [])

    def test_late_queued_callback_cannot_erase_a_cache_hit(self):
        import collections
        errors = []
        env = {'_ds_lock': threading.RLock(), '_ds_memo': {},
               '_ds_memo_order': collections.deque(), '_ds_report_exception': lambda *a: errors.append(a)}
        exec(code, env)
        cache_hit = threading.Event()
        def late_callback():
            self.assertTrue(cache_hit.wait(2))
            env['_ds_memo_put']('Welcome back.', 'Welcome back.', 2.0)
        thread = threading.Thread(target=late_callback)
        thread.start()
        env['_ds_memo_put']('Welcome back.', '欢迎回来。', 1.0)
        cache_hit.set(); thread.join(2)
        self.assertFalse(thread.is_alive())
        self.assertEqual(env['_ds_memo']['Welcome back.'][0], '欢迎回来。',
                         'late queued worker replaced an already resolved translation with source text')
        self.assertEqual(errors, [])
    def fixture(self, modern=False):
        events, diagnostics, errors, queue = [], [], [], []
        class Text:
            def kill_layout(self): events.append(('layout', threading.get_ident()))
        displayable = Text()
        engine = SimpleNamespace(
            current_text=displayable,
            config=SimpleNamespace(periodic_callbacks=[], interact_callbacks=[]),
            display=SimpleNamespace(render=SimpleNamespace(redraw=lambda *a: events.append(('redraw', threading.get_ident())))),
            restart_interaction=lambda: events.append(('restart', threading.get_ident())))
        if modern:
            engine.invoke_in_main_thread = lambda callback: queue.append(callback)
        env = {'renpy': engine, '_ds_lock': threading.RLock(),
               '_ds_state': {'refresh_scheduled': False}, '_ds_refresh_keys': set(),
               '_ds_uncaptured_texts': weakref.WeakSet(),
               '_ds_text_dependencies': weakref.WeakKeyDictionary({displayable: frozenset(['Ready.'])}),
               '_ds_report_exception': lambda *a: errors.append(a),
               '_ds_report_diagnostic': lambda *a: diagnostics.append(a)}
        exec(code, env)
        if '_ds_install_legacy_refresh_hook' in env:
            env['_ds_install_legacy_refresh_hook']()
        return env, engine, events, diagnostics, errors, queue

    def worker(self, env, keys=('Ready.',)):
        thread = threading.Thread(target=env['_ds_refresh_interaction'], args=(keys,))
        thread.start(); thread.join(2)
        self.assertFalse(thread.is_alive())

    def test_legacy_worker_never_calls_engine_ui(self):
        env, engine, events, diagnostics, errors, queue = self.fixture()
        self.worker(env)
        self.assertEqual(events, [], 'legacy refresh called engine UI from a worker thread')
        self.assertTrue(env['_ds_state']['refresh_scheduled'])
        self.assertTrue(engine.config.periodic_callbacks, 'no engine-owned main-thread lifecycle drain')
        for callback in engine.config.periodic_callbacks: callback()
        self.assertEqual([e[0] for e in events], ['layout', 'redraw', 'restart'])
        self.assertTrue(all(e[1] == threading.get_ident() for e in events))
        self.assertFalse(env['_ds_state']['refresh_scheduled'])
        self.assertTrue(diagnostics)
        self.assertEqual(errors, [])

    def test_modern_coalesces_worker_completions(self):
        env, engine, events, diagnostics, errors, queue = self.fixture(True)
        self.worker(env); self.worker(env)
        self.assertEqual(events, [])
        self.assertEqual(len(queue), 1)
        queue.pop()()
        self.assertTrue(all(e[1] == threading.get_ident() for e in events))
        self.worker(env)
        self.assertEqual(len(queue), 1)
        self.assertEqual(errors, [])

    def test_unrelated_completion_does_not_restart_or_invalidate(self):
        env, engine, events, diagnostics, errors, queue = self.fixture(True)
        self.worker(env, ['Other.'])
        queue.pop()()
        self.assertEqual(events, [])
        self.assertEqual(env['_ds_refresh_keys'], set())
        self.assertEqual(errors, [])

    def test_dispatch_failure_preserves_keys_for_the_next_notification(self):
        env, engine, events, diagnostics, errors, queue = self.fixture(True)
        def fail_dispatch(callback): raise RuntimeError('engine dispatch failed')
        engine.invoke_in_main_thread = fail_dispatch
        self.worker(env)
        self.assertEqual(events, [])
        self.assertFalse(env['_ds_state']['refresh_scheduled'])
        self.assertEqual(env['_ds_refresh_keys'], {'Ready.'})
        self.assertEqual(len(errors), 1)
        self.assertEqual(errors[0][1], 'refresh-interaction')
        engine.invoke_in_main_thread = queue.append
        self.worker(env, ['Other.'])
        self.assertEqual(len(queue), 1)
        queue.pop()()
        self.assertEqual([event[0] for event in events], ['layout', 'redraw', 'restart'])
        self.assertEqual(env['_ds_refresh_keys'], set())

    def test_coalesced_keys_and_completion_during_drain_are_not_lost(self):
        env, engine, events, diagnostics, errors, queue = self.fixture(True)
        first = engine.current_text
        class Text:
            def kill_layout(self): events.append(('second-layout', threading.get_ident()))
        second = Text()
        env['_ds_text_dependencies'][second] = frozenset(['Next.'])
        self.worker(env, ['Absent.'])
        self.worker(env, ['Ready.'])
        self.assertEqual(len(queue), 1)
        def redraw(displayable, delay):
            events.append(('redraw', threading.get_ident()))
            if displayable is first:
                env['_ds_refresh_interaction'](['Next.'])
        engine.display.render.redraw = redraw
        queue.pop()()
        self.assertEqual([event[0] for event in events], ['layout', 'redraw', 'restart'])
        self.assertEqual(len(queue), 1)
        queue.pop()()
        self.assertEqual([event[0] for event in events][3:], ['second-layout', 'redraw', 'restart'])
        self.assertEqual(env['_ds_refresh_keys'], set())
        self.assertFalse(env['_ds_state']['refresh_scheduled'])
        self.assertEqual(errors, [])

    def test_dependency_registration_does_not_retain_text(self):
        env, engine, events, diagnostics, errors, queue = self.fixture(True)
        reference = weakref.ref(engine.current_text)
        del engine.current_text
        gc.collect()
        self.assertIsNone(reference())
        self.assertEqual(len(env['_ds_text_dependencies']), 0)
        self.worker(env)
        queue.pop()()
        self.assertEqual(events, [])

    def test_real_update_wrapper_replaces_keys_and_cleans_up_on_engine_error(self):
        env, engine, events, diagnostics, errors, queue = self.fixture(True)
        class Text:
            dirty = False
            def __init__(self, key): self.key, self.nested, self.fail = key, None, False
            def update(self):
                env['_ds_capture_text_key'](self.key)
                if self.nested is not None: self.nested.update()
                if self.fail: raise ValueError('engine update failure')
            def _copy(self): return Text(self.key)
            def per_interact(self): events.append(('interact', threading.get_ident()))
        engine.text = SimpleNamespace(text=SimpleNamespace(Text=Text))
        env['_ds_text_capture'] = threading.local()
        env['_ds_install_text_displayable_hook']()
        first, nested = Text('First.'), Text('Nested.')
        first.nested = nested
        first.update()
        self.assertEqual(env['_ds_text_dependencies'][first], frozenset(['First.']))
        self.assertEqual(env['_ds_text_dependencies'][nested], frozenset(['Nested.']))
        first.key = 'Updated.'
        first.update()
        self.assertEqual(env['_ds_text_dependencies'][first], frozenset(['Updated.']))
        clone = first._copy()
        self.assertEqual(env['_ds_text_dependencies'][clone], frozenset(['Updated.']))
        first.key = 'Failed partial update.'
        first.fail = True
        with self.assertRaisesRegex(ValueError, 'engine update failure'): first.update()
        self.assertEqual(env['_ds_text_capture'].stack, [])
        env['_ds_capture_text_key']('Outside update.')
        self.assertEqual(env['_ds_text_dependencies'][first], frozenset(['Updated.']))
        untracked = Text('Restored.')
        untracked.per_interact()
        self.assertTrue(untracked.dirty)
        self.assertEqual(events[-2:][0][0], 'interact')
        self.assertEqual(events[-1][0], 'redraw')
        self.assertTrue(any(d[0] == 'text-dependency-rebuild' for d in diagnostics))
        self.assertEqual(errors, [])
        reference = weakref.ref(untracked)
        del untracked
        gc.collect()
        self.assertIsNone(reference(), 'uncaptured fallback retained a Text object')

    def test_legacy_drain_is_idempotent_and_preserves_game_callbacks(self):
        env, engine, events, diagnostics, errors, queue = self.fixture()
        game_callback = lambda: None
        engine.config.periodic_callbacks.append(game_callback)
        self.assertIn('_ds_install_legacy_refresh_hook', env)
        env['_ds_install_legacy_refresh_hook']()
        self.assertEqual(engine.config.periodic_callbacks.count(game_callback), 1)
        self.assertEqual(engine.config.periodic_callbacks.count(env['_ds_flush_legacy_refresh']), 1)
        self.worker(env); self.worker(env)
        for callback in engine.config.interact_callbacks: callback()
        for callback in engine.config.periodic_callbacks: callback()
        self.assertEqual(len(events), 3, 'two lifecycle events refreshed the same completion twice')


if __name__ == '__main__':
    unittest.main(testRunner=unittest.TextTestRunner(stream=sys.stdout, verbosity=2))
