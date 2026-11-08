"""执行正式恢复与显示方法，保护转义字面量和真实变量共存的文本。"""
import ast
import re
import threading
from collections import deque
import sys
import unittest
from pathlib import Path
from types import SimpleNamespace

HOOK = Path(__file__).resolve().parents[2] / 'payloads/RenPy/iron_deepseek.rpy'
body = '\n'.join(line[4:] if line else '' for line in HOOK.read_text(encoding='utf-8').splitlines()[1:])
names = {'_ds_renpy_span_end', '_ds_collect_renpy_spans', '_ds_restore_span_sequence', '_ds_renpy_delimiters_complete', '_ds_collect_old_percent_spans', '_ds_restore_renpy_tokens', '_ds_translate', '_ds_is_localized', '_ds_completed_output', '_ds_completed_output_key', '_ds_replace_text', '_ds_wants_ui_text', '_ds_protect_old_percent'}
nodes = [n for n in ast.parse(body).body if isinstance(n, ast.FunctionDef) and n.name in names]
assert {n.name for n in nodes} == names
code = compile(ast.Module(body=nodes, type_ignores=[]), str(HOOK), 'exec')


class TokenTests(unittest.TestCase):
    def setUp(self):
        self.memo, self.diagnostics, self.requests = {}, [], []
        self.env = {'_ds_lock': threading.RLock(), '_ds_completed_outputs': set(), '_ds_completed_output_order': deque(), '_ds_re': re, '_ds_text_type': str, '_ds_report_exception': lambda *args: self.diagnostics.append(args),
                    '_ds_report_diagnostic': lambda *args: self.diagnostics.append(args),
                    '_ds_has_cjk': lambda text: any('\u4e00' <= ch <= '\u9fff' for ch in text),
                    'renpy': SimpleNamespace(config=SimpleNamespace(old_substitutions=False)),
                    '_ds_fetch': lambda source, priority=False: self.requests.append(source) or self.memo.get(source)}
        exec(code, self.env)

    def render(self, source, translated):
        self.memo[source] = self.env['_ds_restore_renpy_tokens'](source, translated)
        return self.env['_ds_translate'](source)

    def test_escaped_literals_and_real_tokens(self):
        cases = [
            ('Press [[Continue] to proceed, [player].', '按 [[Continue] 继续，[player]。'),
            ('Welcome [player], press [[Go] now.', '欢迎[player]，按 [[Go]。'),
            ('Show {{literal} and {b}bold{/b}.', '显示 {{literal} 和 {b}粗体{/b}。'),
            ('[[A] [player] [[B] [score]', '[[A] [player] [[B] [score]分'),
            ('Welcome, [player].', '欢迎，[玩家]。'),
        ]
        for source, translated in cases:
            with self.subTest(source=source):
                expected = translated.replace('[玩家]', '[player]')
                self.assertEqual(self.render(source, translated), expected)
        self.assertEqual(self.diagnostics, [])

    def test_unsafe_tokens_still_rejected(self):
        for source, translated in [('Welcome [player].', '欢迎。'),
                                   ('Welcome.', '欢迎[attacker]。'),
                                   ('{b}Welcome{/b}', '{b}欢迎')]:
            with self.subTest(source=source):
                self.assertEqual(self.render(source, translated), source)
        self.assertEqual(len(self.diagnostics), 3)

    def test_nested_fields_preserve_attributes_conversions_and_formats(self):
        cases = [
            ('Your name is [stats[player].name].', '你的名字是[stats[玩家].名字]。', '你的名字是[stats[player].name]。'),
            ('Name [stats[player].name!q].', '名字[stats[玩家].名字!r]。', '名字[stats[player].name!q]。'),
            ('Score [stats[player].score:03d].', '分数[stats[玩家].分数:04d]。', '分数[stats[player].score:03d]。'),
        ]
        for source, translated, expected in cases:
            with self.subTest(source=source):
                self.assertEqual(self.render(source, translated), expected)
                self.assertEqual(self.env['_ds_restore_renpy_tokens'](source, expected), expected)
        self.assertTrue(self.env['_ds_is_localized']('名字[stats[player].english_name]。'))
        self.assertEqual(self.diagnostics, [])

    def test_unclosed_outer_interpolation_is_rejected(self):
        source = 'Your name is [stats[player].name].'
        for translated in ('你的名字是[stats[玩家].名字。', '你的名字是[stats[玩家]。'):
            with self.subTest(translated=translated):
                self.assertEqual(self.render(source, translated), source)
        self.assertEqual(len(self.diagnostics), 2)

    def test_modern_expression_fields_preserve_quotes_slices_and_parentheses(self):
        self.env['renpy'].substitutions = SimpleNamespace(parse=lambda value: None)
        for field in ('stats[1:3].name', 'stats["player]"].name', 'stats["""player]"""].name',
                      'len(stats[1:3])', '(1 != 2)', 'stats["a\\\"b]"].name'):
            with self.subTest(field=field):
                source = 'Value [' + field + '].'
                self.assertEqual(self.render(source, '值[changed]。'), '值[' + field + ']。')
        self.assertEqual(self.diagnostics, [])
        source = 'Name [stats[player].name].'
        for broken in ('名字[stats["player].name]。', '名字[(stats[player].name]。'):
            self.assertEqual(self.render(source, broken), source)
        self.assertEqual(len(self.diagnostics), 2)

    def test_long_and_tail_markup_cannot_bypass_validation(self):
        source = '{b}Welcome{/b}' * 32
        cases = [('Welcome.', '欢迎{' + 'x' * 130 + '}。'),
                 ('Welcome.', '欢迎{}。'), ('Welcome.', '欢迎{必须'),
                 (source, '{b}欢迎{/b}' * 32 + '{必须}'),
                 ('Welcome.', '欢迎[' + 'attacker' * 20 + ']。')]
        for original, translated in cases:
            with self.subTest(original=original[:30], translated=translated[-30:]):
                self.assertEqual(self.render(original, translated), original)
        self.assertEqual(len(self.diagnostics), len(cases))

    def test_complete_long_attributes_and_more_than_64_tokens_are_restored(self):
        link = 'https://example.invalid/' + 'a' * 150
        source = '{a=' + link + '}Welcome{/a}'
        self.assertEqual(self.render(source, '{a=changed-' + link + '}欢迎{/a}'), '{a=' + link + '}欢迎{/a}')
        source = ''.join('{color=#%06x}Line{/color}' % i for i in range(40))
        translated = ''.join('{color=red}行{/color}' for i in range(40))
        self.assertEqual(self.render(source, translated), source.replace('Line', '行'))
        self.assertEqual(self.diagnostics, [])

    def test_legacy_named_percent_slots_are_restored_and_cannot_be_injected(self):
        self.env['renpy'].config.old_substitutions = True
        source = 'Welcome %(player)s, score %(score)03d.'
        expected = '欢迎%(player)s，分数%(score)03d。'
        self.assertEqual(self.render(source, '欢迎%(玩家)s，分数%(分数)03d。'), expected)
        for original, translated in [('Welcome.', '欢迎%(attacker)s。'),
                                     ('Welcome.', '欢迎%(attacker。'),
                                     (source, '欢迎。')]:
            self.assertEqual(self.render(original, translated), original)

    def test_invalid_span_does_not_drop_literal_prefix(self):
        source, translated = 'Show [] then [player].', '显示 [] 然后[玩家]。'
        self.assertEqual(self.render(source, translated), '显示 [] 然后[player]。')

    def test_legacy_mapping_keys_allow_balanced_parentheses_and_empty_names(self):
        self.env['renpy'].config.old_substitutions = True
        cases = [
            ('Welcome %(player(name))s.', '欢迎%(玩家(名字))s。', '欢迎%(player(name))s。', {'player(name)': 'Archer'}, '欢迎Archer。'),
            ('Welcome %()s.', '欢迎%()s。', '欢迎%()s。', {'': 'Archer'}, '欢迎Archer。'),
            ('Score %(score(round(1)))03d.', '分数%(分数(回合(一)))03d。', '分数%(score(round(1)))03d。', {'score(round(1))': 7}, '分数007。'),
        ]
        for source, translated, expected, mapping, visible in cases:
            with self.subTest(source=source):
                self.assertIn('Archer' if 'Welcome' in source else '007', source % mapping)
                actual = self.render(source, translated)
                self.assertEqual(actual, expected)
                self.assertEqual(self.env['_ds_protect_old_percent'](actual) % mapping, visible)
        self.assertEqual(self.diagnostics, [])

    def test_half_localized_dialogue_keeps_chinese_and_tokens(self):
        source = '请 {b}Open the door{/b}, [player].'
        self.assertEqual(self.render(source, '请 {b}打开门{/b}, [player].'), '请 {b}打开门{/b}, [player].')
        completed = self.render('请 Hello John.', '请 你好 John。')
        self.assertTrue(self.env['_ds_is_localized'](completed), 'completed output was considered a new mixed source')

    def test_mixed_literals_remain_visible_to_translation(self):
        cases = [
            ('请按 [[Continue] 继续。', '请按 [[继续] 继续。'),
            ('请按 {{Continue} 继续。', '请按 {{继续} 继续。'),
            ('请按 <Continue> 继续。', '请按 <继续> 继续。'),
            ('请按 __Continue__ 继续。', '请按 __继续__ 继续。'),
            ('请按 [[Continue]，变量 [Continue] 保留。', '请按 [[继续]，变量 [Continue] 保留。'),
            ('请 __DS_TOKEN_0__Continue__DS_TOKEN_1__', '请 __DS_TOKEN_0__继续__DS_TOKEN_1__'),
            ('请访问 https://example.com，Continue', '请访问 https://example.com，继续'),
        ]
        for source, translated in cases:
            with self.subTest(source=source):
                self.assertEqual(self.render(source, translated), translated)
        for source in ('请 [Continue] 继续。', '请 {font=Continue.ttf}继续{/font}。',
                       '请访问 https://example.com/Continue，继续。'):
            with self.subTest(localized=source):
                self.assertTrue(self.env['_ds_is_localized'](source))

    def test_completed_dialogue_and_ui_survive_color_and_style_partition_changes(self):
        source, translated = '{color=red}请 Hello John.{/color}', '{color=red}请 你好 John。{/color}'
        for entry in ('_ds_translate', '_ds_replace_text'):
            with self.subTest(entry=entry):
                self.memo[source] = translated
                self.assertEqual(self.env[entry](source), translated)
                before = len(self.requests)
                for displayed in ('{color=blue}请 你好 John。{/color}',
                                  '请 你{b}好 John。{/b}', '{i}请 你好{/i} John。'):
                    self.assertEqual(self.env[entry](displayed), displayed)
                self.assertEqual(len(self.requests), before, 'format-only changes requeued a completed result')
        self.env['_ds_completed_output']('{b}请 Open the door{/b}', '请 Open the door')
        self.assertFalse(self.env['_ds_is_localized']('{color=blue}请 Open the door{/color}'),
                         'format-cleaned original was marked as a successful translation')
        self.assertFalse(self.env['_ds_is_localized']('请 [[Hello John]'),
                         'literal brackets were normalized as formatting')


if __name__ == '__main__':
    unittest.main(testRunner=unittest.TextTestRunner(stream=sys.stdout, verbosity=2))
