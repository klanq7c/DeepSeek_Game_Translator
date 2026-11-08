"""源链接正式编译脚本采集与主线程快照，禁止求值游戏表达式或后台读 AST。"""
import ast
from collections import deque
from pathlib import Path
import re
import sys
import threading
from types import ModuleType, SimpleNamespace
import unittest
from unittest.mock import patch

HOOK = Path(__file__).resolve().parents[2] / 'payloads/RenPy/iron_deepseek.rpy'
body = '\n'.join(line[4:] if line else '' for line in HOOK.read_text(encoding='utf-8').splitlines()[1:])
NAMES = {'_ds_native_string', '_ds_collect_script_menu_labels', '_ds_script_say_strings', '_ds_collect_script_prefetch_texts',
         '_ds_prefetch_script_menus', '_ds_start_script_menu_prefetch'}
nodes = [n for n in ast.parse(body).body if isinstance(n, ast.FunctionDef) and n.name in NAMES]
CODE = compile(ast.Module(body=nodes, type_ignores=[]), str(HOOK), 'exec')


class Say:
    def __init__(self, what, who=None): self.what, self.who = what, who


class Menu:
    def __init__(self, *labels): self.items = [(label, None, []) for label in labels]


class Character:
    def __init__(self, name, dynamic=False): self.name, self.dynamic = name, dynamic


class InventoryTests(unittest.TestCase):
    def setUp(self):
        self.errors, self.diagnostics, self.requests, self.workers = [], [], [], []
        self.store = ModuleType('store')
        self.store.e = Character('Okin')
        self.store.dynamic = Character('player_name', True)
        self.source = Say('Original dialogue.', 'e')
        self.block = SimpleNamespace(identifier='original_12ab34cd', block=[self.source])
        self.script = SimpleNamespace(namemap={0: Menu('Choose a route', '已经是中文。')},
            translator=SimpleNamespace(default_translates={'original_12ab34cd': self.block}, language_translates={}))
        self.renpy = ModuleType('renpy')
        self.renpy.store = self.store
        self.native_strings = {}
        self.script.translator.strings = {'native_zh': SimpleNamespace(translations=self.native_strings)}
        self.renpy.game = SimpleNamespace(script=self.script, preferences=SimpleNamespace(language=None))
        ast_module = ModuleType('renpy.ast'); ast_module.Say, ast_module.Menu = Say, Menu
        character_module = ModuleType('renpy.character'); character_module.ADVCharacter = Character
        self.patch = patch.dict(sys.modules, {'renpy': self.renpy, 'renpy.ast': ast_module, 'renpy.character': character_module})
        self.patch.start(); self.addCleanup(self.patch.stop)
        class Thread:
            def __init__(thread, target, args=()): thread.target, thread.args = target, args
            def start(thread): self.workers.append(thread)
        self.ns = {'renpy': self.renpy, '_ds_re': re, '_ds_python_ast': ast,
                   '_ds_text_type': str, '_ds_string_types': (str,), '_ds_lock': threading.RLock(),
                   '_ds_state': {'menu_prefetch': False}, '_ds_threading': SimpleNamespace(Thread=Thread),
                   '_ds_is_localized': lambda text: bool(text) and not re.search('[A-Za-z]', text),
                   '_ds_report_exception': lambda *args: self.errors.append(args),
                   '_ds_report_diagnostic': lambda *args: self.diagnostics.append(args),
                   '_ds_http': lambda path, payload, timeout: self.requests.append((path, payload))}
        exec(CODE, self.ns)

    def collect(self, limit=8192):
        self.assertIn('_ds_collect_script_prefetch_texts', self.ns, 'compiled Say prefetch missing')
        return self.ns['_ds_collect_script_prefetch_texts'](limit)

    def test_original_compiled_dialogue_and_static_names_without_evaluation(self):
        self.block.block += [Say('Repeated line.'), Say('Repeated line.'), Say('Quoted speaker line.', '"Old Man"'),
                             Say('Dynamic character line.', 'dynamic'), Say('Expression speaker line.', 'side_effect()'), Say('已经汉化。')]
        result = self.collect()
        self.assertEqual(set(result), {'Choose a route', 'Original dialogue.', 'Okin', 'Repeated line.',
                         'Quoted speaker line.', 'Old Man', 'Dynamic character line.', 'Expression speaker line.'})
        self.assertEqual(len(result), len(set(result)))
        self.assertEqual(self.block.identifier, 'original_12ab34cd')
        self.assertEqual(self.store.dynamic.name, 'player_name')
        self.assertEqual(self.errors, [])

    def test_active_native_translation_is_used_and_original_not_queued(self):
        self.renpy.game.preferences.language = 'native_zh'
        self.script.translator.language_translates[('original_12ab34cd', 'native_zh')] = SimpleNamespace(block=[Say('已经汉化。', 'e')])
        result = self.collect()
        self.assertNotIn('Original dialogue.', result)
        self.assertNotIn('已经汉化。', result)
        self.assertEqual(self.renpy.game.preferences.language, 'native_zh')

    def test_start_callback_captures_ast_before_worker_and_bounds_unique_keys(self):
        for i in range(20): self.block.block.append(Say('Dialogue number ' + str(i)))
        self.assertEqual(len(self.collect(3)), 3)
        self.assertTrue(self.diagnostics, 'inventory cap was silent')
        self.ns['_ds_start_script_menu_prefetch']()
        self.assertEqual(len(self.workers), 1)
        self.script.translator = None
        self.script.namemap = None
        worker = self.workers[0]
        worker.target(*worker.args)
        self.assertIn('Original dialogue.', self.requests[0][1]['texts'])
        self.assertEqual(self.requests[0][1]['engine'], 'renpy')
        self.ns['_ds_start_script_menu_prefetch']()
        self.assertEqual(len(self.workers), 1)
        self.assertEqual(self.errors, [])

    def test_missing_inventory_is_diagnosed_and_menu_fallback_kept(self):
        self.script.translator = None
        self.assertEqual(self.collect(), ['Choose a route'])
        self.assertTrue(self.errors or self.diagnostics, 'missing translator was hidden')

    def test_native_string_translation_covers_menu_and_speaker_names(self):
        self.renpy.game.preferences.language = 'native_zh'
        self.native_strings.update({'Choose a route': '选择路线', 'Okin': '御琴', 'Old Man': '老人'})
        self.block.block.append(Say('Explicit speaker line.', '"Old Man"'))
        result = self.collect()
        self.assertEqual(set(result), {'Original dialogue.', 'Explicit speaker line.'})

    def test_native_lookup_keeps_missing_keys_and_context_matching_readonly(self):
        self.renpy.game.preferences.language = 'native_zh'
        self.native_strings['Choose a route'] = '选择路线'
        native = self.ns['_ds_native_string']
        self.assertEqual(native('Choose a route{#menu}'), '选择路线')
        self.assertEqual(native('No native entry'), 'No native entry')
        self.assertEqual(self.native_strings, {'Choose a route': '选择路线'})


if __name__ == '__main__':
    unittest.main(testRunner=unittest.TextTestRunner(stream=sys.stdout, verbosity=2))
