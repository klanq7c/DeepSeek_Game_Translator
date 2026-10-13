"""独立验证生成的 Godot 包中的源码/组件指纹。"""

from pathlib import Path
import sys


def mix(value: int, chunk: bytes) -> int:
    for byte in chunk:
        value = ((value ^ byte) * 1099511628211) & 0xFFFFFFFFFFFFFFFF
    return value


def main() -> int:
    source, marker, runtime_g3, runtime_g4, version_file, font_manifest = map(Path, sys.argv[1:])
    value = 14695981039346656037
    with source.open("rb") as stream:
        while chunk := stream.read(65536):
            value = mix(value, chunk)
    for delimiter, script in ((0xFF, runtime_g3), (0xFE, runtime_g4)):
        value = mix(value, bytes((delimiter,)))
        value = mix(value, script.read_bytes())
    value = mix(value, b"\xfc")
    value = mix(value, font_manifest.read_bytes())
    value = mix(value, b"\xfd")
    version = version_file.read_text(encoding="ascii").splitlines()[0]
    value = mix(value, version.encode("utf-8"))
    expected = f"DST_GODOT_SOURCE_V4:{value:016x}\n".encode("ascii")
    actual = marker.read_bytes()
    if actual != expected:
        print(f"Godot source/payload stamp mismatch: expected {expected!r}, got {actual!r}")
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
