"""真实启动器：Ren'Py 子目录跨过传统路径上限后仍收集原文，且不改游戏文件。"""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]


def extended(path):
    return Path('\\\\?\\' + str(path.resolve()))


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--c', type=Path, default=ROOT / 'ds游戏翻译器.exe')
    parser.add_argument('--cs', type=Path, default=ROOT / 'build/launcher_cs/dst_launcher_cs.exe')
    parser.add_argument('--output', type=Path)
    args = parser.parse_args()
    output = (args.output or Path(tempfile.mkdtemp(prefix='dst_renpy_paths_'))).resolve()
    output.mkdir(parents=True, exist_ok=True)
    rows = []
    for engine, length in [('renpy', 160), ('renpy', 240), ('rpgm', 160)]:
        parent = output / (engine + str(length))
        game = parent / ('p' * (length - len(str(parent)) - 1))
        assert len(str(game)) == length
        if engine == 'renpy':
            files = {
                'game/script.rpyc': b'fixture compiled marker',
                'game/chapter_with_long_name/script.rpy': b'label first:\n    "The village has a quiet evening."\n    return\n',
                'game/chapter_with_long_name/another_chapter/script.rpy': b'label second:\n    "Tomorrow we will cross the river."\n    return\n',
                'game/chapter_with_long_name/iron_deepseek.rpy': b'label internal:\n    "Do not collect the translator hook."\n',
            }
            expected = {'The village has a quiet evening.', 'Tomorrow we will cross the river.'}
        else:
            files = {
                'www/index.html': b'<script src="js/main.js"></script>',
                'www/js/main.js': b'', 'www/js/rpg_core.js': b'',
                'www/data/System.json': b'{"gameTitle":"Village Journey"}',
                'www/data/Actors.json': b'[null,{"name":"Village Guide","description":"A helpful traveler."}]',
            }
            expected = {'Village Guide', 'A helpful traveler.'}
        for relative, data in files.items():
            path = extended(game / relative)
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(data)

        def snapshot():
            base = extended(game)
            return {str(p.relative_to(base)): hashlib.sha256(p.read_bytes()).hexdigest()
                    for p in base.rglob('*') if p.is_file()}

        before = snapshot()
        all_posts = []
        for side, binary, mode in [('c', args.c, '--warmup-and-exit'), ('cs', args.cs, '--warmup')]:
            run = subprocess.run([str(binary), mode, str(game)], capture_output=True, timeout=20)
            log = run.stdout.decode('utf-8-sig')
            (output / f'{engine}{length}-{side}.log').write_bytes(run.stdout + run.stderr)
            posts = [json.loads(line.split(' ', 1)[1]) for line in log.splitlines() if line.startswith('post=/prefetch ')]
            texts = [text for post in posts for text in post['texts']]
            rows.append({'case': f'{engine}{length}/{side} exact original keys',
                         'pass': run.returncode == 0 and set(texts) == expected and len(texts) == len(expected),
                         'missing': sorted(expected - set(texts)), 'extra': sorted(set(texts) - expected)})
            rows.append({'case': f'{engine}{length}/{side} read only', 'pass': snapshot() == before})
            if engine == 'renpy':
                rows.append({'case': f'{engine}{length}/{side} engine hint',
                             'pass': bool(posts) and all(p.get('engine') == 'renpy' for p in posts)})
            all_posts.append(posts)
        rows.append({'case': f'{engine}{length} C/C# request parity', 'pass': all_posts[0] == all_posts[1]})
    (output / 'results.json').write_text(json.dumps(rows, indent=2), encoding='utf-8')
    failed = [row for row in rows if not row['pass']]
    for row in failed:
        print('FAIL ' + json.dumps(row))
    print(f'RenPy long scan: {len(rows)-len(failed)} passed, {len(failed)} failed; output={output}')
    raise SystemExit(bool(failed))


if __name__ == '__main__':
    main()
