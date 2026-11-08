"""在隔离项目中运行正式钩子及真实 Ren'Py Text 词法分析器。"""
import argparse
import os
from pathlib import Path
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
DRIVER = r'''
init 1000 python:
    def dst_engine_guard():
        import threading
        import json
        import io
        guard_count = [0]
        def require(condition, message):
            if not condition:
                raise AssertionError(message)
            guard_count[0] += 1
        main_thread = threading.current_thread().ident
        literal_screen = renpy.display.screen.get_screen_variant('dst_literal_probe').function
        literal_values = [renpy.python.py_eval(child.positional[0]) for child in literal_screen.children]
        require(literal_values == [u'Open  the   door.', u'Keep \\[literal\\] and \\% literal.', u'Hex ! and 中 and 😀.', u'Octal A and\t tab.', u'Raw \\n and  spaces.'], 'real screen Python literal rules diverged from warmup cases')
        store.dst_pronoun = u'she'
        inventory = _ds_collect_script_prefetch_texts()
        require(u'Original compiled dialogue.' in inventory, 'compiled Say was not discovered')
        require(u'Static Speaker' in inventory and u'Old Man' in inventory, 'static speaker names were not discovered')
        require(u'Explicit speaker dialogue.' in inventory, 'explicit speaker dialogue was missed')
        require(u'First compiled paragraph.' in inventory and u'Second compiled paragraph.' in inventory, 'compiled monologue was missed')
        require(inventory.count(u'Repeated compiled line.') == 1, 'compiled inventory used occurrence counts instead of unique keys')
        require(u'Technical Python value.' not in inventory, 'Python expression was treated as dialogue')
        require(store.dst_pronoun == u'she', 'inventory changed a story variable')
        renpy.translation.init_translation()
        renpy.change_language('dst_native', force=True)
        translated_inventory = _ds_collect_script_prefetch_texts()
        require(u'Native original dialogue.' not in translated_inventory, 'existing native translation was ignored')
        require(u'Static Speaker' not in translated_inventory and u'Old Man' not in translated_inventory and u'Compiled menu option' not in translated_inventory, 'native speaker/menu string translation was ignored by prefetch')
        table = renpy.game.script.translator.strings['dst_native']
        old_strings, old_unknown = dict(table.translations), list(table.unknown)
        update_translations = renpy.translation.update_translations
        try:
            renpy.translation.update_translations = True
            require(_ds_native_string(u'Missing native key{#context}') == u'Missing native key{#context}', 'native lookup changed a missing key')
            _ds_collect_script_prefetch_texts()
            require(table.translations == old_strings and table.unknown == old_unknown, 'inventory registered missing strings in native update mode')
        finally:
            renpy.translation.update_translations = update_translations
        require(renpy.game.preferences.language == 'dst_native', 'translator forced a language preference')
        def chinese_font(font):
            font_layout = renpy.text.text.Layout.__new__(renpy.text.text.Layout)
            font_layout.oversample = 1.0
            segment = renpy.text.text.TextSegment()
            segment.cps = 0
            segment.take_style(style.default, font_layout)
            segment.font = font
            return [(part.font, text) for part, text in segment.subsegment(u'中')]
        require(chinese_font(style.default.font) == [(u'ds_font.otf', u'中')], 'language rebuild removed rendered CJK coverage')
        require(chinese_font('fonts/InlineOriginal.ttf') == [(u'ds_font.otf', u'中')], 'archived/nested inline font lost rendered CJK coverage')
        require(store.dst_pronoun == u'she', 'language callback rewrote a story variable')
        renpy.change_language(None, force=True)
        pending = []
        # 单独模拟网络入队边界，不接触用户正在运行的 localhost 服务。
        def record_pending(text, priority=False):
            pending.append(text)
        store._ds_note_pending = record_pending
        def visible_tokens(displayable):
            displayable.update()
            return u''.join(value for kind, value in displayable.tokens if kind == renpy.text.text.TEXT)
        guarded_source = u'Guarded original sentence.'
        for unsafe in (u'译文{' + u'x' * 130 + u'}', u'译文{}', u'译文{必须'):
            require(renpy.check_text_tags(unsafe) is not None, 'fixture did not reach a real invalid RenPy tag')
            _ds_memo_put(guarded_source, unsafe, 1.0)
            displayed = _ds_translate(guarded_source)
            require(displayed == guarded_source, 'unsafe markup reached early dialogue output')
            require(visible_tokens(renpy.text.text.Text(displayed, substitute=False)) == guarded_source, 'rejected response damaged real Text')
        many_source = u'{b}Original{/b}' * 40
        many_output = u'{b}译文{/b}' * 40 + u'{必须}'
        require(renpy.check_text_tags(many_output) is not None, 'tail tag fixture was not invalid')
        require(_ds_restore_renpy_tokens(many_source, many_output) == many_source, 'tail tag bypassed real hook validation')
        class NestedFieldPlayer(object):
            name = u'Alice'
            score = 7
        nested_scope = {'stats': {'player': NestedFieldPlayer()}, 'player': 'player'}
        for source, output, expected_source, expected_output in (
            (u'Name [stats[player].name].', u'名字[stats[玩家].名字]。', u'Name Alice.', u'名字Alice。'),
            (u'Name [stats[player].name!q].', u'名字[stats[玩家].名字!r]。', u'Name Alice.', u'名字Alice。'),
            (u'Score [stats[player].score:03d].', u'分数[stats[玩家].分数:04d]。', u'Score 007.', u'分数007。')):
            require(renpy.substitutions.substitute(source, scope=nested_scope, force=True, translate=False)[0] == expected_source, 'nested-field fixture is not supported by the native formatter')
            restored = _ds_restore_renpy_tokens(source, output)
            require(renpy.substitutions.substitute(restored, scope=nested_scope, force=True, translate=False)[0] == expected_output, 'restored nested field damaged native interpolation')
            require(_ds_restore_renpy_tokens(source, restored) == restored, 'second restoration changed a valid nested field')
            broken = output[:-2]
            require(_ds_restore_renpy_tokens(source, broken) == source, 'unclosed outer interpolation bypassed validation')
        escaped_source = u'Use [[display] and [stats[player].name].'
        escaped = _ds_restore_renpy_tokens(escaped_source, u'使用[[显示]和[stats[玩家].名字]。')
        require(renpy.substitutions.substitute(escaped, scope=nested_scope, force=True, translate=False)[0] == u'使用[显示]和Alice。', 'escaped opening bracket changed nested interpolation ownership')
        if callable(getattr(renpy.substitutions, 'parse', None)):
            nested_scope['stats']['player]'] = NestedFieldPlayer()
            for source, expected in ((u'Name [stats["player]"].name].', u'值Alice。'),
                                     (u'Count [len([1, 2, 3][1:])].', u'值2。')):
                restored = _ds_restore_renpy_tokens(source, u'值[changed]。')
                require(renpy.substitutions.substitute(restored, scope=nested_scope, force=True, translate=False)[0] == expected, 'native quoted/sliced expression boundary was truncated')
            for broken in (u'名字[stats["player].name]。', u'名字[(stats[player].name]。'):
                require(_ds_restore_renpy_tokens(escaped_source, broken) == escaped_source, 'malformed native expression bypassed validation')
        old_substitutions = renpy.config.old_substitutions
        try:
            renpy.config.old_substitutions = True
            old_source = u'Welcome %(player)s, score %(score)03d.'
            restored = _ds_restore_renpy_tokens(old_source, u'欢迎%(玩家)s，分数%(分数)03d。')
            require((_ds_protect_old_percent(restored) % {'player': u'Archer', 'score': 7}) == u'欢迎Archer，分数007。', 'legacy mapping fields were not restored before Python formatting')
            nested_source = u'Welcome %(player(name))s, %()s.'
            nested = _ds_restore_renpy_tokens(nested_source, u'欢迎%(玩家(名字))s，%()s。')
            require(nested == u'欢迎%(player(name))s，%()s。', 'valid nested or empty mapping key was rejected')
            require((_ds_protect_old_percent(nested) % {'player(name)': u'Archer', '': u'朋友'}) == u'欢迎Archer，朋友。', 'real Python rejected restored nested or empty mapping key')
            require(_ds_restore_renpy_tokens(guarded_source, u'译文%(attacker)s') == guarded_source, 'legacy mapping injection was accepted')
        finally:
            renpy.config.old_substitutions = old_substitutions
        _ds_memo_put(u'Start adventure', u'开始冒险', 1.0)
        cached = renpy.text.text.Text(u'Start adventure', substitute=False)
        require(visible_tokens(cached) == u'开始冒险', 'real Text cache replacement failed')
        late = renpy.text.text.Text(u'Welcome back', substitute=False)
        require(visible_tokens(late) == u'Welcome back', 'cold Text changed source')
        require(u'Welcome back' in pending, 'cold Text did not queue')
        unrelated = [cached] + [renpy.text.text.Text(u'无关文字 %d' % i, substitute=False) for i in range(100)]
        for displayable in unrelated:
            visible_tokens(displayable)
        queue = []
        invoke = getattr(renpy, 'invoke_in_main_thread', None)
        if invoke is not None:
            renpy.invoke_in_main_thread = queue.append
        try:
            _ds_memo_put(u'Welcome back', u'欢迎回来', 2.0)
            worker = threading.Thread(target=_ds_refresh_interaction, args=([u'Welcome back'],))
            worker.start()
            worker.join(2.0)
            require(not worker.is_alive(), 'worker hung')
            require(not late.dirty, 'HTTP worker dirtied real engine Text')
            if invoke is not None:
                require(len(queue) == 1, 'modern refresh was not coalesced')
                queue.pop()()
            else:
                require(_ds_flush_legacy_refresh in renpy.config.periodic_callbacks, 'legacy drain was not installed')
                _ds_flush_legacy_refresh()
            require(late.dirty, 'main-thread refresh did not invalidate real Text')
            unrelated_invalidations = sum(bool(displayable.dirty) for displayable in unrelated)
            print('refresh_probe unrelated_invalidations=%d objects=%d' % (unrelated_invalidations, len(unrelated)))
            require(unrelated_invalidations == 0, 'translation completion invalidated unrelated real Text objects: %d' % unrelated_invalidations)
            require(visible_tokens(late) == u'欢迎回来', 'late cache hit did not update real tokens')
            _ds_memo_put(u'Welcome back', u'Welcome back', 3.0)
            require(_ds_memo_get(u'Welcome back') == u'欢迎回来', 'late queued result erased real cache hit')
            tagged = renpy.text.text.Text(u'{b}Start adventure{/b}', substitute=False)
            require(visible_tokens(tagged) == u'开始冒险', 'real tagged Text lost visible translation')
            require(any(kind == renpy.text.text.TAG and value == u'b' for kind, value in tagged.tokens), 'real Text lost bold tag')
            count = len(pending)
            for frame in range(60):
                color = u'#ff0000' if frame % 2 == 0 else u'#0000ff'
                raw = u'{color=' + color + u'}{size=' + str(20 + frame % 4) + u'}Start adventure{/size}{/color}'
                tagged.set_text(raw, substitute=False)
                require(visible_tokens(tagged) == u'开始冒险', 'animated Text flashed source')
                tags = [value for kind, value in tagged.tokens if kind == renpy.text.text.TAG]
                require(u'color=' + color in tags and u'size=' + str(20 + frame % 4) in tags, 'animated Text lost current style')
            require(len(pending) == count, 'style animation queued translations')
            tagged.set_text(u'Untranslated new choice', substitute=False)
            require(visible_tokens(tagged) == u'Untranslated new choice', 'animated Text froze old translation')
            def complete(keys):
                worker = threading.Thread(target=_ds_refresh_interaction, args=(keys,))
                worker.start()
                worker.join(2.0)
                require(not worker.is_alive(), 'completion worker hung')
                if invoke is not None:
                    require(len(queue) == 1, 'completion did not schedule exactly one main-thread drain')
                    queue.pop()()
                else:
                    _ds_flush_legacy_refresh()
            complete([u'Start adventure'])
            require(not tagged.dirty, 'set_text retained the previous source dependency')
            require(cached.dirty, 'hot-cache Text stopped tracking its original source key')
            visible_tokens(cached)
            dynamic = renpy.text.text.Text(u'Hello [dst_name]', scope={'dst_name': u'Archer'}, substitute=True)
            require(visible_tokens(dynamic) == u'Hello Archer', 'real substitution fixture failed')
            history = dynamic._in_current_store()
            require(history is not dynamic, 'real history snapshot did not copy scoped Text')
            dynamic._scope({'dst_name': u'Robin'})
            require(visible_tokens(dynamic) == u'Hello Robin', 'scope update did not change real Text')
            _ds_memo_put(u'Hello Archer', u'你好Archer', 4.0)
            complete([u'Hello Archer'])
            require(not dynamic.dirty, 'old interpolated key invalidated new scope')
            require(history.dirty, 'history copy lost the original key dependency')
            require(visible_tokens(history) == u'你好Archer', 'history snapshot did not accept late translation')
            _ds_memo_put(u'Hello Robin', u'你好Robin', 4.0)
            complete([u'Hello Robin'])
            require(dynamic.dirty and not history.dirty, 'scope dependency crossed history snapshot')
            require(visible_tokens(dynamic) == u'你好Robin', 'current scope lost late translation')
            # 使用引擎自己的状态恢复边界；依赖登记不写入显示对象或存档数据。
            state = dynamic.__getstate__()
            require(not any(key.startswith('_ds_') for key in state), 'dependency metadata leaked into serialized Text')
            restored = renpy.text.text.Text.__new__(renpy.text.text.Text)
            restored.__setstate__(dict(state))
            require(not restored.dirty, 'restored Text fixture was not clean')
            restored.per_interact()
            require(restored.dirty, 'restored Text skipped dependency reconstruction')
            require(visible_tokens(restored) == u'你好Robin', 'restored Text lost translated tokens')
            _ds_memo_put(u'Hello Robin', u'欢迎Robin', 5.0)
            complete([u'Hello Robin'])
            require(restored.dirty, 'restored Text was not registered after reconstruction')
            require(visible_tokens(restored) == u'欢迎Robin', 'restored Text did not accept corrected translation')
            nested = renpy.text.text.Text(u'Nested custom sentence', substitute=False)
            def custom_tag(tag, argument, contents):
                visible_tokens(nested)
                return [(renpy.text.text.TEXT, u'Generated custom sentence')]
            renpy.config.custom_text_tags['dst_dependency_probe'] = custom_tag
            try:
                outer = renpy.text.text.Text(u'{dst_dependency_probe}Ignored source{/dst_dependency_probe}', substitute=False)
                require(visible_tokens(outer) == u'Generated custom sentence', 'custom tag fixture failed')
                _ds_memo_put(u'Nested custom sentence', u'嵌套文字', 5.0)
                complete([u'Nested custom sentence'])
                require(nested.dirty and not outer.dirty, 'nested Text lookup contaminated the outer dependency')
                require(visible_tokens(nested) == u'嵌套文字', 'nested Text did not accept late result')
                _ds_memo_put(u'Generated custom sentence', u'标签生成文字', 5.0)
                complete([u'Generated custom sentence'])
                require(outer.dirty, 'custom tag output dependency was not captured')
                require(visible_tokens(outer) == u'标签生成文字', 'custom tag output lost late translation')
            finally:
                del renpy.config.custom_text_tags['dst_dependency_probe']
            class PluginText(renpy.text.text.Text):
                # 插件自行分词，绕过基础 update，仍使用引擎标签/替换入口。
                def update(self):
                    self.dirty = False
                    self.kill_layout()
                    self.tokens = self.apply_custom_tags([(renpy.text.text.TEXT, self.text[0])])
                    self.displayables = set()
            plugin_text = PluginText(u'Plugin generated sentence', substitute=False)
            require(visible_tokens(plugin_text) == u'Plugin generated sentence', 'plugin Text fixture failed')
            _ds_memo_put(u'Plugin generated sentence', u'插件文字', 6.0)
            complete([u'Plugin generated sentence'])
            require(plugin_text.dirty, 'uncaptured plugin Text lost conservative late refresh')
            require(visible_tokens(plugin_text) == u'插件文字', 'plugin Text did not display late translation')
            require(threading.current_thread().ident == main_thread, 'drain ran outside main thread')
        finally:
            if invoke is not None:
                renpy.invoke_in_main_thread = invoke
        with io.open(renpy.config.basedir + '/result.json', 'w', encoding='utf-8') as output:
            version = renpy.version() if callable(renpy.version) else renpy.version
            output.write(u'%s' % json.dumps({'version': version, 'passed': guard_count[0], 'animated_frames': 60, 'compiled_inventory': len(inventory), 'unrelated_invalidations': unrelated_invalidations, 'unrelated_objects': len(unrelated)}, ensure_ascii=True))
        return False
    renpy.arguments.register_command('dst-engine-guard', dst_engine_guard)
'''

STORY = r'''
define dst_actor = Character("Static Speaker")
default dst_pronoun = "she"
init python:
    dst_technical = "Technical Python value."
label dst_inventory:
    dst_actor "Original compiled dialogue."
    "Old Man" "Explicit speaker dialogue."
    dst_actor "Repeated compiled line."
    dst_actor "Repeated compiled line."
    dst_actor """First compiled paragraph.

    Second compiled paragraph."""
    dst_actor "Native original dialogue." id dst_native_original
    menu:
        "Compiled menu option":
            return
translate dst_native dst_native_original:
    dst_actor "已经翻译。"
translate dst_native style default:
    font "DejaVuSans.ttf"
translate dst_native strings:
    old "Static Speaker"
    new "静态角色"
    old "Old Man"
    new "老人"
    old "Compiled menu option"
    new "已汉化选项"
screen dst_literal_probe():
    text "Open  the   door."
    text "Keep \[literal\] and \% literal."
    text _("Hex \x21 and \u4e2d and \U0001f600.")
    text "Octal \101 and\t tab."
    text R"Raw \n and  spaces."
'''


def run(runtime, driver=DRIVER, story=STORY, prepare_game=None, artifacts=None):
    runtime = runtime.resolve()
    entries = list(runtime.glob('*.py'))
    if len(entries) != 1:
        raise AssertionError('expected one bundled RenPy launcher .py: ' + str(runtime))
    executables = list((runtime / 'lib').glob('*windows*/python.exe'))
    if not executables:
        raise AssertionError('bundled Python runtime missing')
    executable = sorted(executables, key=lambda p: ('py3' not in str(p), 'x86_64' not in str(p)))[0]
    with tempfile.TemporaryDirectory(prefix='dst_renpy_engine_') as name:
        fixture = Path(name)
        game = fixture / 'game'
        game.mkdir()
        shutil.copyfile(ROOT / 'payloads/RenPy/iron_deepseek.rpy', game / 'iron_deepseek.rpy')
        shutil.copyfile(ROOT / 'assets/fonts/NotoSansCJKsc-Regular.otf', game / 'ds_font.otf')
        (game / 'fonts').mkdir()
        shutil.copyfile(runtime / 'renpy/common/DejaVuSans.ttf', game / 'fonts/InlineOriginal.ttf')
        if prepare_game is not None:
            prepare_game(game, runtime)
        (game / 'guard.rpy').write_text(driver, encoding='utf-8')
        (game / 'story.rpy').write_text(story, encoding='utf-8')
        environment = dict(os.environ, PYTHONDONTWRITEBYTECODE='1', SDL_AUDIODRIVER='dummy',
                           SDL_VIDEODRIVER='dummy', RENPY_DISABLE_JOYSTICK='1')
        # Python 2 游戏包的解释器需要其随附标准库；不修改系统或游戏配置。
        libraries = runtime / 'lib'
        if any(libraries.glob('python?.?')) or any(libraries.glob('python?.??')):
            environment['PYTHONHOME'] = str(runtime)
        elif 'py3' not in str(executable):
            environment['PYTHONHOME'] = str(executable.parent)
        environment.pop('RENPY_LANGUAGE', None)
        environment.pop('RENPY_UPDATE_STRINGS', None)
        command = [str(executable)]
        if 'py3' not in str(executable): command.append('-O')  # 随附的 Python 2 标准库包含 .pyo 文件。
        command += [str(entries[0]), str(fixture), 'dst-engine-guard', '--savedir', str(fixture / 'saves')]
        result = subprocess.run(command, env=environment, cwd=fixture, stdout=subprocess.PIPE,
                                stderr=subprocess.STDOUT, timeout=40)
        report = fixture / 'result.json'
        if result.returncode or not report.exists():
            diagnostics = result.stdout.decode('utf-8', errors='replace')
            for log in ('traceback.txt', 'errors.txt'):
                if (fixture / log).exists(): diagnostics += '\n' + (fixture / log).read_text(encoding='utf-8', errors='replace')
            raise AssertionError('RenPy engine guard failed: ' + diagnostics[-9000:])
        first = report.read_text(encoding='utf-8')
        compiled = (game / 'story.rpyc').read_bytes()
        (game / 'story.rpy').rename(fixture / 'original-story.rpy')
        report.rename(fixture / 'source-result.json')
        second = subprocess.run(command, env=environment, cwd=fixture, stdout=subprocess.PIPE,
                                stderr=subprocess.STDOUT, timeout=40)
        assert second.returncode == 0, second.stdout.decode('utf-8', errors='replace')[-9000:]
        assert report.exists() and report.read_text(encoding='utf-8') == first, 'compiled-only run did not produce the same engine guard results'
        assert (game / 'story.rpyc').read_bytes() == compiled, 'original compiled script was rewritten'
        if artifacts is not None:
            artifacts.mkdir(parents=True, exist_ok=True)
            for image in fixture.glob('font-*.png'):
                shutil.copyfile(image, artifacts / image.name)
            shutil.copyfile(report, artifacts / 'result.json')
        print('[PASS] real RenPy engine, source and original compiled-only script ' + first)


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--runtime', type=Path, required=True)
    run(parser.parse_args().runtime)
