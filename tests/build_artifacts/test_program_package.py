"""检查实际发布包的下载文件名、说明链接、许可和程序字节。"""

import argparse
import hashlib
import re
import sys
import zipfile
from pathlib import Path, PurePosixPath


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--archive", required=True, type=Path)
    parser.add_argument("--exe", required=True, type=Path)
    args = parser.parse_args()
    repo = Path(__file__).resolve().parents[2]
    version = (repo / "VERSION").read_text(encoding="utf-8-sig").strip()
    failures = []
    passed = 0

    def check(name, ok):
        nonlocal passed
        if ok:
            passed += 1
            print("PASS " + name)
        else:
            failures.append(name)
            print("FAIL " + name)

    check("stable archive filename", args.archive.name == f"ds-translator_{version}.zip")
    check("stable executable filename", args.exe.name == f"ds-translator_{version}.exe")
    with zipfile.ZipFile(args.archive) as archive:
        entries = [entry for entry in archive.namelist() if not entry.endswith("/")]
        roots = {PurePosixPath(entry).parts[0] for entry in entries}
        check("single package directory", len(roots) == 1)
        if len(roots) != 1:
            return 1
        root = next(iter(roots)) + "/"
        files = {entry[len(root):]: entry for entry in entries}
        check("no unsafe archive paths", all(
            not PurePosixPath(entry).is_absolute() and ".." not in PurePosixPath(entry).parts
            for entry in entries))
        for name in ("README.md", "LICENSE", "THIRD_PARTY_NOTICES.md",
                     "docs/USER_GUIDE.md", "assets/fonts/OFL.txt", "assets/fonts/manifest.json"):
            check("included " + name, name in files)
        if "README.md" in files:
            readme = archive.read(files["README.md"]).decode("utf-8-sig")
            check("current release link", f"/releases/tag/v{version}" in readme)
            check("documented download name", f"ds-translator_{version}.exe" in readme)
            for link in re.findall(r"\[[^\]]+\]\(([^)]+)\)", readme):
                if "://" in link or link.startswith("#"):
                    continue
                target = link.split("#", 1)[0]
                check("README link " + target, target in files)
        exe_name = "ds游戏翻译器.exe"
        check("launcher included", exe_name in files)
        if exe_name in files:
            digest = hashlib.sha256(archive.read(files[exe_name])).digest()
            check("archive equals standalone", digest == hashlib.sha256(args.exe.read_bytes()).digest())
            check("archive equals current build", digest == hashlib.sha256((repo / exe_name).read_bytes()).digest())
        for relative in ("assets/fonts/OFL.txt", "assets/fonts/manifest.json"):
            if relative in files:
                check("reviewed bytes " + relative, archive.read(files[relative]) == (repo / relative).read_bytes())
        check("no user data or external runtime", all(
            not re.search(r"(^|/)(config|logs|payloads|native|bin|obj)/|translation_memory|\.(ini|tsv|dll|pdb|log)$", path, re.I)
            for path in files))
    print(f"Program package: PASS={passed} FAIL={len(failures)}")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
