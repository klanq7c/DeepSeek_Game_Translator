"""使用 FIFO 参考实现验证真实 Godot 3/4 缓存写入函数体。"""

from collections import OrderedDict
from pathlib import Path
import re


ROOT = Path(__file__).resolve().parents[2]
LIMIT = 4096


class GodotDictionary(dict):
    def has(self, key):
        return key in self

    def erase(self, key):
        self.pop(key, None)


class GodotArray(list):
    def size(self):
        return len(self)


def load_put(path):
    source = path.read_text(encoding="utf-8")
    match = re.search(
        r"(?m)^func _dst_cache_put\(query, translated\):\n(?:^[\t].*\n|^\n)*",
        source,
    )
    if match is None:
        raise AssertionError(f"cache function missing in {path}")
    body = match.group(0)
    if "pop_front" in body:
        raise AssertionError(f"cache eviction still shifts the array in {path}")
    # 这个简短的 GDScript 函数使用 Python 兼容表达式；通过适配
    # Dictionary.has/erase 和 Array.size，执行其真实函数体。
    python = body.replace("func _dst_cache_put", "def _dst_cache_put", 1).expandtabs(4)
    python = re.sub(r"\bvar ", "", python)
    python = python.replace("def _dst_cache_put(query, translated):\n",
                            "def _dst_cache_put(query, translated):\n    global _dst_cache_head\n", 1)
    namespace = {
        "DST_MAX_CACHE": LIMIT,
        "_dst_cache": GodotDictionary(),
        "_dst_cache_order": GodotArray(),
        "_dst_cache_head": 0,
    }
    exec(python, namespace)
    return namespace, namespace["_dst_cache_put"], body


def check_state(namespace, expected, label):
    cache = namespace["_dst_cache"]
    slots = namespace["_dst_cache_order"]
    head = namespace["_dst_cache_head"]
    assert len(cache) == len(expected) <= LIMIT, (label, "size", len(cache), len(expected))
    assert dict(cache) == dict(expected), (label, "values")
    assert len(slots) == len(expected), (label, "slots")
    if len(slots) < LIMIT:
        assert head == 0, (label, "early head", head)
        order = list(slots)
    else:
        assert 0 <= head < LIMIT, (label, "head", head)
        order = list(slots[head:]) + list(slots[:head])
    assert order == list(expected), (label, "FIFO order")


def exercise(path):
    namespace, put, body = load_put(path)
    expected = OrderedDict()
    checks = 0
    for index in range(LIMIT * 3 + 77):
        key = f"entry-{index}"
        value = f"译文-{index}"
        put(key, value)
        if key not in expected and len(expected) == LIMIT:
            expected.popitem(last=False)
        expected[key] = value
        if index % 17 == 0:
            # 更新已有条目不得将 FIFO 行为变成 LRU。
            key = f"entry-{index}"
            put(key, "updated")
            expected[key] = "updated"
        if index % 137 == 0 or index in (LIMIT - 1, LIMIT, LIMIT * 2 - 1):
            check_state(namespace, expected, f"{path.name} step {index}")
            checks += 1
    check_state(namespace, expected, f"{path.name} final")
    return body, checks + 1


def main():
    bodies = []
    total = 0
    for name in ("dst_godot_runtime_g3.gd", "dst_godot_runtime_g4.gd"):
        body, checks = exercise(ROOT / "payloads" / "Godot" / name)
        bodies.append(body)
        total += checks
    assert bodies[0] == bodies[1], "Godot 3/4 FIFO implementations diverged"
    print(f"Godot cache ring PASS: {total} source-linked FIFO checks")


if __name__ == "__main__":
    main()
