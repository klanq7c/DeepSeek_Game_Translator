"""真实两版启动器：RPG Maker 命令按对象取字段，字段顺序不影响预热。"""
import argparse
import hashlib
import itertools
import json
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
BINARIES = [(ROOT / "ds游戏翻译器.exe", "--warmup-and-exit"),
            (ROOT / "build/launcher_cs/dst_launcher_cs.exe", "--warmup")]
COMMANDS = [
    (101, ["Face.png", 0, 0, 2, "Narrator Name"]),
    (401, ["First dialogue line."]),
    (401, ["Second dialogue line."]),
    (102, [["Accept the quest.", "Leave the village."], 1, 0, 2, 0]),
    (405, ["Scrolling epilogue line."]),
    (108, ["A comment must never be queued."]),
    (0, []),
]
EXPECTED = {"Narrator Name", "First dialogue line.", "Second dialogue line.",
            "First dialogue line.\nSecond dialogue line.",
            "Accept the quest.", "Leave the village.", "Scrolling epilogue line."}


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    output = args.output or Path(tempfile.mkdtemp(prefix="dst_rpgm_fields_"))
    output.mkdir(parents=True, exist_ok=True)
    cases = {}
    for order in itertools.permutations(("code", "indent", "parameters")):
        commands = []
        for code, parameters in COMMANDS:
            values = {"code": code, "parameters": parameters, "indent": 0}
            commands.append({key: values[key] for key in order})
        cases["-".join(order)] = commands
    cases["object-isolation"] = [
        {"code": 401},
        {"parameters": ["Do not borrow the preceding object's code."]},
        {"code": 401, "metadata": {"code": 108}, "parameters": ["Owned dialogue line."]},
        {"code": 0, "parameters": []},
        {"parameters": ["A comment after dialogue is not shown."], "code": 108},
    ]
    cases["object-depth-limit"] = []
    cases["incomplete-parameters"] = []
    cases["parameter-depth-limit"] = []
    rows = []
    for name, commands in cases.items():
        game = output / name
        for relative, value in {
            "Game.exe": "",
            "www/index.html": "<script src='js/main.js'></script>",
            "www/js/main.js": "",
            "www/js/rpg_core.js": "",
            "www/data/System.json": '{"gameTitle":"x"}',
            "www/data/Map001.json": json.dumps({"events": [None, {"name": "EditorInternal",
                 "pages": [{"list": commands}]}]}, ensure_ascii=False),
        }.items():
            path = game / relative
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(value, encoding="utf-8")
        diagnostic = None
        if name == "object-depth-limit":
            text = '{"nested":' * 129 + '{}' + '}' * 129
            diagnostic = "对象嵌套超过 128 层"
        elif name == "incomplete-parameters":
            text = '{"code":401,"parameters":["Do not queue incomplete dialogue."'
            diagnostic = "parameters 格式不完整或嵌套过深"
        elif name == "parameter-depth-limit":
            text = '{"code":401,"parameters":' + '[' * 33 + '"Deep dialogue."' + ']' * 33 + '}'
            diagnostic = "parameters 格式不完整或嵌套过深"
        if diagnostic:
            (game / "www/data/Map001.json").write_text(text, encoding="utf-8")
        before = {str(p.relative_to(game)): hashlib.sha256(p.read_bytes()).hexdigest()
                  for p in game.rglob("*") if p.is_file()}
        expected = {"Owned dialogue line."} if name == "object-isolation" else EXPECTED
        if diagnostic:
            expected = set()
        all_texts = []
        for binary, mode in BINARIES:
            result = subprocess.run([str(binary), mode, str(game.resolve())],
                                    capture_output=True, timeout=20)
            log = result.stdout.decode("utf-8-sig")
            (output / (name + "-" + binary.stem + ".log")).write_text(log, encoding="utf-8")
            texts = [text for line in log.splitlines() if line.startswith("post=/prefetch ")
                     for text in json.loads(line.split(" ", 1)[1])["texts"]]
            ok = result.returncode == 0 and set(texts) == expected
            if diagnostic:
                ok = ok and diagnostic in log
            rows.append({"case": name, "binary": binary.name, "pass": ok,
                         "missing": sorted(expected - set(texts)), "extra": sorted(set(texts) - expected)})
            all_texts.append(texts)
        rows.append({"case": name + " exact C/C# parity", "pass": all_texts[0] == all_texts[1]})
        after = {str(p.relative_to(game)): hashlib.sha256(p.read_bytes()).hexdigest()
                 for p in game.rglob("*") if p.is_file()}
        rows.append({"case": name + " read-only", "pass": before == after})
    (output / "results.json").write_text(json.dumps(rows, indent=2), encoding="utf-8")
    failed = [row for row in rows if not row["pass"]]
    for row in failed:
        print("FAIL " + json.dumps(row))
    print(f"RPGM command fields: {len(rows)-len(failed)} passed, {len(failed)} failed; output={output}")
    if failed:
        raise SystemExit(1)


if __name__ == "__main__":
    main()
