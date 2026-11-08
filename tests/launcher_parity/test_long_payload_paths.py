"""真实启动器长路径释放：字节一致、重用/修复、目录和 junction 所有权保护。"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import stat
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
PAYLOADS = [
    "native/dst_server.exe", "native/dst_server_cs.exe",
    "scripts/install_runtime_payloads.ps1", "config/api.ini.example",
    "config/launcher.ini.example", "config/glossary.example.tsv",
    "assets/fonts/NotoSansCJKsc-Regular.otf", "assets/fonts/OFL.txt",
    "assets/fonts/manifest.json", "payloads/UnityTranslator/UnityTranslator.dll",
    "payloads/UnityTranslator/UnityTranslator.BepInEx6.dll",
    "payloads/UnityTranslator/DeepSeekUnityFontPatcher.dll",
    "payloads/UnityIL2CPP/DeepSeekXUnityTranslator/DeepSeekTranslate.dll",
    "payloads/UnityIL2CPP/DeepSeekTMPFontFallback/BepInEx/plugins/DeepSeekTMPFontFallback/DeepSeekTMPFontFallback.dll",
]


def extended(path):
    return Path("\\\\?\\" + os.path.abspath(path))


def sha(path):
    return hashlib.sha256(extended(path).read_bytes()).hexdigest()


def write(path, data):
    path = extended(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(data)


def padded(parent, length):
    suffix = "中文 space "
    remaining = length - len(str(parent.resolve())) - 1
    if remaining < len(suffix):
        raise ValueError("fixture base too long for requested root length")
    result = parent / (suffix + "p" * (remaining - len(suffix)))
    extended(result).mkdir(parents=True)
    assert len(str(result.resolve())) == length
    return result


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    output = (args.output or Path(tempfile.mkdtemp(prefix="dst_long_paths_"))).resolve()
    output.mkdir(parents=True, exist_ok=True)
    rows = []
    binaries = [("c", ROOT / "ds游戏翻译器.exe"), ("cs", ROOT / "build/launcher_cs/dst_launcher_cs.exe")]

    def check(name, ok):
        rows.append({"case": name, "pass": bool(ok)})
        if not ok:
            print("FAIL " + name)

    for side, binary in binaries:
        for length in (150, 230):
            folder = padded(output / (side + str(length)), length)
            launcher = folder / binary.name
            shutil.copy2(binary, launcher)
            commands = [str(launcher), "--sync-payloads-and-exit"] if side == "c" else [
                str(launcher), "--sync-payloads", "--root", str(folder)]
            label = side + "/" + str(length)
            counter = 0

            def run():
                nonlocal counter
                counter += 1
                result = subprocess.run(commands, capture_output=True, timeout=30)
                text = (result.stdout + result.stderr).decode("utf-8-sig", errors="replace")
                (output / (side + str(length) + "-" + str(counter) + ".log")).write_text(text, encoding="utf-8")
                return result.returncode, text

            user_files = ["config/api.ini", "config/launcher.ini", "translation_memory_c.tsv"]
            for relative in user_files:
                path = extended(folder / relative)
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_bytes(b"owned fixture data; preserve bytes")
            initial = {name: sha(folder / name) for name in user_files}
            code, text = run()
            check(label + " sync succeeds", code == 0 and "result=0" in text)
            for relative in PAYLOADS:
                target = folder / relative
                check(label + " bytes " + relative, extended(target).is_file() and sha(target) == sha(ROOT / relative))
            check(label + " user files preserved", all(sha(folder / name) == digest for name, digest in initial.items()))
            existing = [folder / relative for relative in PAYLOADS if extended(folder / relative).is_file()]
            times = {str(path): extended(path).stat().st_mtime_ns for path in existing}
            code, text = run()
            check(label + " no-op succeeds", code == 0 and "components synced:" not in text)
            check(label + " no-op preserves timestamps", all(extended(path).stat().st_mtime_ns == times[str(path)] for path in existing))

            deepest = extended(folder / PAYLOADS[-1])
            deepest.parent.mkdir(parents=True, exist_ok=True)
            if deepest.exists():
                deepest.rename(Path(str(deepest) + ".test-original"))
            deepest.write_bytes(b"broken owned payload")
            code, _ = run()
            check(label + " repairs damaged long file", code == 0 and sha(folder / PAYLOADS[-1]) == sha(ROOT / PAYLOADS[-1]))
            check(label + " no temporary file remains", not Path(str(deepest) + ".dstmp").exists())

            if deepest.exists():
                deepest.rename(Path(str(deepest) + ".test-repaired"))
            deepest.mkdir()
            (deepest / "keep.txt").write_bytes(b"directory belongs to fixture user")
            code, text = run()
            check(label + " refuses directory collision", code == 5 and "component update failed" in text
                  and (deepest / "keep.txt").read_bytes() == b"directory belongs to fixture user")

            # 只在本测试拥有的树内创建 junction；目标的字节和目录必须保持不变。
            junction_root = padded(output / (side + str(length) + "j"), length)
            junction_launcher = junction_root / binary.name
            shutil.copy2(binary, junction_launcher)
            link = junction_root / Path(PAYLOADS[-1]).parent
            extended(link.parent).mkdir(parents=True, exist_ok=True)
            target = output / (side + str(length) + "-outside")
            target.mkdir()
            (target / "keep.txt").write_bytes(b"junction target must remain untouched")
            mklink = subprocess.run(["cmd.exe", "/d", "/c", "mklink", "/J",
                                     str(extended(link)), str(target)], capture_output=True)
            if mklink.returncode:
                raise RuntimeError("owned junction fixture failed: " + mklink.stdout.decode(errors="replace") + mklink.stderr.decode(errors="replace"))
            try:
                command = [str(junction_launcher), "--sync-payloads-and-exit"] if side == "c" else [
                    str(junction_launcher), "--sync-payloads", "--root", str(junction_root)]
                result = subprocess.run(command, capture_output=True, timeout=30)
                (output / (side + str(length) + "-junction.log")).write_bytes(result.stdout + result.stderr)
                check(label + " rejects long junction", result.returncode == 5)
                check(label + " junction target preserved", sorted(p.name for p in target.iterdir()) == ["keep.txt"]
                      and (target / "keep.txt").read_bytes() == b"junction target must remain untouched")
            finally:
                # rmdir 只移除本测试创建的 junction 自身，不递归删除它的目标。
                assert os.path.abspath(link).startswith(str(output) + os.sep) and os.path.isjunction(link)
                os.rmdir(extended(link))

    # 相邻引擎的真实部署/还原也经过共享文件层，核对长路径下的字体和原文件归属。
    for side, binary in binaries:
        for engine in ("renpy", "rpgm"):
            game = padded(output / (side + "-" + engine), 240)
            files = {"Game.exe": b"fixture executable, never launched"}
            if engine == "renpy":
                files["game/script.rpy"] = b'label start:\n    "Original dialogue."\n    return\n'
                hook = "game/iron_deepseek.rpy"
                font = "game/ds_font.ttf"
            else:
                files.update({"www/index.html": b'<html><body><script src="js/main.js"></script></body></html>',
                              "www/data/System.json": b'{"gameTitle":"Long path fixture"}',
                              "www/js/main.js": b"", "www/js/rpg_core.js": b""})
                hook = "www/js/hook_rpgm_mv.js"
                font = "www/fonts/ds_font.ttf"
            for relative, data in files.items():
                path = extended(game / relative)
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_bytes(data)

            def snapshot():
                base = extended(game)
                return {str(path.relative_to(base)): hashlib.sha256(path.read_bytes()).hexdigest()
                        for path in base.rglob("*") if path.is_file()}

            original = snapshot()
            stale = extended(game / "game/iron_deepseek.rpyc")
            if engine == "renpy":
                stale.write_bytes(b"old hook bytecode")
                stale.chmod(stat.S_IREAD)
            backup_hash = None
            for step, mode in enumerate(("deploy", "deploy", "restore")):
                flag = "--" + mode + "-and-exit" if side == "c" else "--" + mode
                command = [str(binary), flag, str(game)]
                if side == "cs":
                    command += ["--root", str(ROOT)]
                result = subprocess.run(command, capture_output=True, timeout=30)
                text = (result.stdout + result.stderr).decode("utf-8-sig", errors="replace")
                (output / (side + "-" + engine + "-" + str(step) + "-" + mode + ".log")).write_text(text, encoding="utf-8")
                check(side + "/" + engine + " " + str(step) + " " + mode + " succeeds", result.returncode == 0 and "result=1" in text)
                if mode == "deploy":
                    check(side + "/" + engine + " hook and font exist", extended(game / hook).is_file()
                          and extended(game / font).is_file())
                    if engine == "renpy":
                        check(side + "/renpy removes readonly stale bytecode", not stale.exists())
                    else:
                        backup = game / "www/index.html.dst-backup"
                        current_hash = sha(backup) if extended(backup).is_file() else None
                        check(side + "/rpgm retains original backup", current_hash == original["www\\index.html"]
                              and (backup_hash is None or current_hash == backup_hash))
                        backup_hash = current_hash
                    if step == 1:
                        extended(game / hook).chmod(stat.S_IREAD)
                else:
                    check(side + "/" + engine + " restores exact original files", snapshot() == original)

            # 合法所有权标记不能授权删除部署后被用户改过的字体。
            write(game / font, b"user edited font")
            write(game / (font + ".dst-owned"), b"DST_FONT_OWNED_V1:simhei.ttf\n")
            changed = snapshot()
            result = subprocess.run(command, capture_output=True, timeout=30)
            text = (result.stdout + result.stderr).decode("utf-8-sig", errors="replace")
            (output / (side + "-" + engine + "-modified-font.log")).write_text(text, encoding="utf-8")
            check(side + "/" + engine + " preserves modified owned font", snapshot() == changed and "result=0" in text)

    # 长 payload 与安装目录同时覆盖 Unity 递归还原、空目录清理、用户文件保护。
    for side, binary in binaries:
        payload_root = padded(output / (side + "-unity-root"), 230)
        launcher = payload_root / binary.name
        shutil.copy2(binary, launcher)
        installed = Path("BepInEx/plugins/XUnity.AutoTranslator")
        payload = payload_root / "payloads/UnityIL2CPP/XUnityAutoTranslator" / installed
        contents = {"XUnity.AutoTranslator.Plugin.Core.dll": b"synthetic core",
                    "Translators/FullNameSplitter.dll": b"synthetic translator"}
        for name, data in contents.items():
            write(payload / name, data)
        for changed in (False, True):
            label = side + "-unity-" + str(changed)
            game = padded(output / label, 240)
            originals = {"Game.exe": b"fixture, never launched", "UnityPlayer.dll": b"fixture",
                         "GameAssembly.dll": b"fixture", "Game_Data/globalgamemanagers": b"2022.3.1f1"}
            for name, data in originals.items():
                write(game / name, data)
            for name, data in contents.items():
                write(game / installed / name, data)
            write(game / installed / ".dst-installed-by-ds", b"ds-game-translator:xunity-auto-translator:v1\n")
            edited = game / installed / "Translators/FullNameSplitter.dll"
            extra = game / installed / "user.ini"
            if changed:
                write(edited, b"user modified translator")
                write(extra, b"user config")
            else:
                extended(edited).chmod(stat.S_IREAD)
            command = [str(launcher), "--restore-and-exit", str(game)] if side == "c" else [
                str(launcher), "--restore", str(game), "--root", str(payload_root)]
            result = subprocess.run(command, capture_output=True, timeout=30)
            text = (result.stdout + result.stderr).decode("utf-8-sig", errors="replace")
            (output / (label + ".log")).write_text(text, encoding="utf-8")
            check(label + " original game preserved", all(extended(game / name).read_bytes() == data for name, data in originals.items()))
            check(label + " unchanged payload removed", not extended(game / installed / "XUnity.AutoTranslator.Plugin.Core.dll").exists())
            if changed:
                check(label + " modified and extra files preserved", extended(edited).read_bytes() == b"user modified translator"
                      and extended(extra).read_bytes() == b"user config" and "result=0" in text)
            else:
                check(label + " owned tree fully removed", not extended(game / installed).exists()
                      and result.returncode == 0 and "result=1" in text)

        # Godot 相邻还原只移除自有补丁，原项目与包保持不变。
        game = padded(output / (side + "-godot"), 240)
        originals = {"project.godot": b"config_version=5\n", "game.pck": b"original pack"}
        for name, data in originals.items():
            write(game / name, data)
        owned = ["dst_godot_runtime.gd", "dst_godot_patch.pck", "dst_godot_patch.pck.dst-source",
                 "dst_godot_patch.exe", "dst_godot_patch.exe.dst-owned"]
        for name in owned:
            write(game / name, b"owned fixture")
        command = [str(binary), "--restore-and-exit", str(game)] if side == "c" else [
            str(binary), "--restore", str(game), "--root", str(ROOT)]
        result = subprocess.run(command, capture_output=True, timeout=30)
        text = (result.stdout + result.stderr).decode("utf-8-sig", errors="replace")
        (output / (side + "-godot-restore.log")).write_text(text, encoding="utf-8")
        check(side + "/godot removes owned long files", result.returncode == 0 and "result=1" in text
              and all(not extended(game / name).exists() for name in owned))
        check(side + "/godot preserves original files", all(extended(game / name).read_bytes() == data for name, data in originals.items()))

    (output / "results.json").write_text(json.dumps(rows, indent=2), encoding="utf-8")
    failed = sum(not row["pass"] for row in rows)
    print(f"Long payload paths: {len(rows)-failed} passed, {failed} failed; output={output}")
    if failed:
        raise SystemExit(1)


if __name__ == "__main__":
    main()
