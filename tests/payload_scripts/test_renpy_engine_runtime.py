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
        def require(condition, message):
            if not condition:
                raise AssertionError(message)
        main_thread = threading.current_thread().ident
        pending = []
        # 单独模拟网络入队边界，不接触用户正在运行的 localhost 服务。
        def record_pending(text, priority=False):
            pending.append(text)
        store._ds_note_pending = record_pending
        def visible_tokens(displayable):
            displayable.update()
            return u''.join(value for kind, value in displayable.tokens if kind == renpy.text.text.TEXT)
        _ds_memo_put(u'Start adventure', u'开始冒险', 1.0)
        cached = renpy.text.text.Text(u'Start adventure', substitute=False)
        require(visible_tokens(cached) == u'开始冒险', 'real Text cache replacement failed')
        late = renpy.text.text.Text(u'Welcome back', substitute=False)
        require(visible_tokens(late) == u'Welcome back', 'cold Text changed source')
        require(u'Welcome back' in pending, 'cold Text did not queue')
        queue = []
        invoke = getattr(renpy, 'invoke_in_main_thread', None)
        if invoke is not None:
            renpy.invoke_in_main_thread = queue.append
        try:
            _ds_memo_put(u'Welcome back', u'欢迎回来', 2.0)
            worker = threading.Thread(target=_ds_refresh_interaction)
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
            require(threading.current_thread().ident == main_thread, 'drain ran outside main thread')
        finally:
            if invoke is not None:
                renpy.invoke_in_main_thread = invoke
        with io.open(renpy.config.basedir + '/result.json', 'w', encoding='utf-8') as output:
            version = renpy.version() if callable(renpy.version) else renpy.version
            output.write(u'%s' % json.dumps({'version': version, 'passed': 7, 'animated_frames': 60}, ensure_ascii=True))
        return False
    renpy.arguments.register_command('dst-engine-guard', dst_engine_guard)
'''


def run(runtime):
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
        (game / 'guard.rpy').write_text(DRIVER, encoding='utf-8')
        environment = dict(os.environ, PYTHONDONTWRITEBYTECODE='1', SDL_AUDIODRIVER='dummy',
                           SDL_VIDEODRIVER='dummy', RENPY_DISABLE_JOYSTICK='1')
        # Python 2 游戏包的解释器需要其随附标准库；不修改系统或游戏配置。
        if 'py3' not in str(executable):
            environment['PYTHONHOME'] = str(executable.parent)
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
        print('[PASS] real RenPy engine ' + report.read_text(encoding='utf-8'))


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--runtime', type=Path, required=True)
    run(parser.parse_args().runtime)
