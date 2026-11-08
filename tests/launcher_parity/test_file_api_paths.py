"""文件 API 长路径转换：生产 C 源码与当前 C# 启动器的边界一致性。"""
import argparse
import json
import os
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
C_PROBE = r'''
#include "fsutil.h"
#include <stdio.h>
#include <stdlib.h>
int wmain(int argc, WCHAR **argv) {
    if (argc != 2) return 2;
    WCHAR converted[FS_FILE_API_PATH_CAP];
    int ok = path_for_file_api(converted, FS_FILE_API_PATH_CAP, argv[1]);
    printf("%d\n", ok);
    if (ok) {
        char text[FS_FILE_API_PATH_CAP * 4];
        int n = WideCharToMultiByte(CP_UTF8, 0, converted, -1, text, sizeof text, NULL, NULL);
        if (!n) return 3;
        fwrite(text, 1, n - 1, stdout);
    }
    return 0;
}
'''
PS_PROBE = r'''
param([string]$Launcher, [string]$Cases)
$ErrorActionPreference = 'Stop'
[Console]::OutputEncoding = New-Object Text.UTF8Encoding($false)
$assembly = [Reflection.Assembly]::LoadFrom($Launcher)
$method = $assembly.GetType('DstLauncher.SafeFs').GetMethod('TryFileApiPath')
$rows = @(foreach ($item in (Get-Content -LiteralPath $Cases -Raw -Encoding UTF8 | ConvertFrom-Json)) {
    $callArgs = [object[]]@([string]$item.input, $null)
    $ok = [bool]$method.Invoke($null, $callArgs)
    @{name=$item.name; ok=$ok; value=$callArgs[1]}
})
[Console]::Write((ConvertTo-Json -InputObject $rows -Depth 4 -Compress))
'''


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--output", type=Path)
    parser.add_argument("--helper-shell", default="powershell.exe")
    args = parser.parse_args()
    output = (args.output or Path(tempfile.mkdtemp(prefix="dst_file_api_"))).resolve()
    output.mkdir(parents=True, exist_ok=True)
    drive = "C:\\" + "normal directory\\" * 18
    unc = "\\\\server\\share\\" + "normal directory\\" * 18
    cases = [
        {"name": "short unchanged", "input": "C:/folder/../file.txt", "expected": "C:/folder/../file.txt"},
        {"name": "long drive", "input": drive + "file.txt", "expected": "\\\\?\\" + drive + "file.txt"},
        {"name": "long slash normalization", "input": (drive + "file.txt").replace("\\", "/"), "expected": "\\\\?\\" + drive + "file.txt"},
        {"name": "parent normalization", "input": drive + "child\\..\\file.txt", "expected": "\\\\?\\" + drive + "file.txt"},
        {"name": "long UNC", "input": unc + "file.txt", "expected": "\\\\?\\UNC\\" + unc[2:] + "file.txt"},
        {"name": "internal trailing dot", "input": drive + "ambiguous.\\file.txt", "expected": None},
        {"name": "internal trailing space", "input": drive + "ambiguous \\file.txt", "expected": None},
        {"name": "reserved device name", "input": drive + "NUL.txt", "expected": None},
        {"name": "reserved COM superscript", "input": drive + "COM\u00b9.txt", "expected": None},
        {"name": "alternate stream", "input": drive + "file.txt:stream", "expected": None},
        {"name": "external extended namespace", "input": "\\\\?\\" + drive + "file.txt", "expected": None},
        {"name": "external device namespace", "input": "\\\\.\\" + drive + "file.txt", "expected": None},
        {"name": "buffer ceiling", "input": "C:\\" + "directory\\" * 120 + "file.txt", "expected": None},
    ]
    (output / "cases.json").write_text(json.dumps(cases, ensure_ascii=False), encoding="utf-8")
    (output / "probe.c").write_text(C_PROBE, encoding="utf-8")
    (output / "probe.ps1").write_text(PS_PROBE, encoding="utf-8-sig")
    gcc = ROOT / "native/toolchain/w64devkit/bin/gcc.exe"
    env = os.environ.copy()
    env["PATH"] = str(gcc.parent) + os.pathsep + env["PATH"]
    native = output / "probe.exe"
    command = [str(gcc), "-std=c17", "-O2", "-Wall", "-Wextra", "-Werror", "-municode",
               "-ffunction-sections", "-fdata-sections", "-Wl,--gc-sections",
               "-I" + str(ROOT / "native/src/launcher"), str(output / "probe.c"),
               str(ROOT / "native/src/launcher/fsutil.c"), str(ROOT / "native/src/launcher/globals.c"),
               "-lgdi32", "-o", str(native)]
    result = subprocess.run(command, cwd=ROOT, env=env, capture_output=True, timeout=60)
    (output / "compile.log").write_bytes(result.stdout + result.stderr)
    if result.returncode:
        raise RuntimeError("native path probe compile failed: " + str(output / "compile.log"))
    rows = []
    for case in cases:
        run = subprocess.run([str(native), case["input"]], capture_output=True, timeout=10)
        assert run.returncode == 0
        pieces = run.stdout.decode("utf-8").split("\n", 1)
        value = pieces[1] if pieces[0].strip() == "1" else None
        rows.append({"name": "C " + case["name"], "pass": value == case["expected"],
                     "actual": value, "expected": case["expected"]})
    run = subprocess.run([args.helper_shell, "-NoProfile", "-File", str(output / "probe.ps1"),
                          "-Launcher", str(ROOT / "build/launcher_cs/dst_launcher_cs.exe"),
                          "-Cases", str(output / "cases.json")], capture_output=True, timeout=30)
    (output / "managed.log").write_bytes(run.stdout + run.stderr)
    if run.returncode:
        raise RuntimeError("managed path probe failed: " + str(output / "managed.log"))
    actual = json.loads(run.stdout.decode("utf-8-sig"))
    assert len(actual) == len(cases)
    for case, value in zip(cases, actual):
        rows.append({"name": "C# " + case["name"],
                     "pass": value["ok"] == (case["expected"] is not None) and value["value"] == case["expected"],
                     "actual": value["value"], "expected": case["expected"]})
    (output / "results.json").write_text(json.dumps(rows, indent=2), encoding="utf-8")
    failed = [row for row in rows if not row["pass"]]
    for row in failed:
        print("FAIL " + json.dumps(row))
    print(f"File API paths: {len(rows)-len(failed)} passed, {len(failed)} failed; output={output}")
    if failed:
        raise SystemExit(1)


if __name__ == "__main__":
    main()
