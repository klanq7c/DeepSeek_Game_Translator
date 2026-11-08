"""真实启动器检测：发行文件、自身改名与相邻引擎主程序选择。"""
from pathlib import Path
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
BINARIES = [(ROOT / 'ds游戏翻译器.exe', '--detect-and-exit'),
            (ROOT / 'build/launcher_cs/dst_launcher_cs.exe', '--detect')]


def detect(binary, mode, directory):
    result = subprocess.run([str(binary), mode, str(directory)], capture_output=True,
                            encoding='utf-8', timeout=20)
    assert result.returncode == 0, result.stderr
    fields = dict(line.split('=', 1) for line in result.stdout.splitlines() if '=' in line)
    return fields


def main():
    for binary, mode in BINARIES:
        with tempfile.TemporaryDirectory(prefix='dst_exe_selection_') as folder:
            root = Path(folder)
            for name in ['ds-translator_0.4.1.0.exe', 'DS-TRANSLATOR_12.3.4.exe', 'dst_server_cs.exe']:
                (root / name).write_bytes(b'fixture')
            (root / 'Novel.exe').write_bytes(b'fixture')
            (root / 'game').mkdir()
            (root / 'game/script.rpy').write_text('label start:\n    return\n')
            report = detect(binary, mode, root)
            assert Path(report['exe']).name == 'Novel.exe', report
            print('[PASS]', binary.name, 'release names excluded')
            own = root / '000-renamed-translator.exe'
            shutil.copy2(binary, own)
            report = detect(own, mode, root)
            assert Path(report['exe']).name == 'Novel.exe', report
            own.unlink()
            # 保留其他引擎更强的主程序标志评分，不能以固定选第一个候选替代。
            (root / 'UnityAdventure.exe').write_bytes(b'fixture')
            (root / 'UnityAdventure_Data').mkdir()
            report = detect(binary, mode, root)
            assert Path(report['exe']).name == 'UnityAdventure.exe', report
            print('[PASS]', binary.name, 'renamed self and adjacent Unity score')


if __name__ == '__main__':
    main()
