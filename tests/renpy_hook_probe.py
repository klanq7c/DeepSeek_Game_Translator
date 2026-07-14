import pathlib
import sys
import tempfile
import time
import types


def load_python_block(path):
    lines = pathlib.Path(path).read_text(encoding="utf-8-sig").splitlines()
    if not lines or lines[0].strip() != "init 999 python:":
        raise AssertionError("unexpected Ren'Py hook header")
    body = []
    for line in lines[1:]:
        if line and not line.startswith("    "):
            raise AssertionError("Ren'Py hook line escaped its python block")
        body.append(line[4:] if line else "")
    source = "\n".join(body) + "\n"
    return compile(source, str(path), "exec")


def fake_renpy(gamedir):
    renpy = types.ModuleType("renpy")
    renpy.exports = types.SimpleNamespace(say=lambda who, what, *args, **kwargs: what)
    renpy.config = types.SimpleNamespace(
        old_substitutions=False,
        say_menu_text_filter=None,
        gamedir=gamedir,
        font_replacement_map={},
    )
    renpy.style = types.SimpleNamespace(styles={})
    renpy.restart_interaction = lambda: None

    character = types.ModuleType("renpy.character")

    class ADVCharacter:
        def __call__(self, what, *args, **kwargs):
            return what

    character.ADVCharacter = ADVCharacter
    renpy.character = character

    ast = types.ModuleType("renpy.ast")

    class Menu:
        def __init__(self, items):
            self.items = items
            self.executed = False

        def execute(self):
            self.executed = True
            return "menu-executed"

    ast.Menu = Menu
    renpy.ast = ast
    sys.modules["renpy"] = renpy
    sys.modules["renpy.character"] = character
    sys.modules["renpy.ast"] = ast
    return renpy


def main():
    if len(sys.argv) != 2:
        raise SystemExit("usage: renpy_hook_probe.py <iron_deepseek.rpy>")

    code = load_python_block(sys.argv[1])
    with tempfile.TemporaryDirectory() as gamedir:
        renpy = fake_renpy(gamedir)
        namespace = {"renpy": renpy}
        exec(code, namespace)

    if namespace["_ds_next_poll_delay"]() is not None:
        raise AssertionError("empty Ren'Py queue should sleep until an event")

    http_calls = []
    namespace["_ds_http"] = lambda *args, **kwargs: http_calls.append((args, kwargs))
    namespace["_ds_ensure_poller"] = lambda: None

    if namespace["_ds_fetch"]("Probe dialogue") is not None:
        raise AssertionError("first miss should preserve source while background work is queued")
    if namespace["_ds_fetch"]("Probe dialogue") != "Probe dialogue":
        raise AssertionError("repeat miss should use the in-process negative memo")
    if list(namespace["_ds_pending"].keys()) != ["Probe dialogue"]:
        raise AssertionError("repeat render should not duplicate pending work")
    namespace["_ds_retry_after"]["Probe dialogue"] = time.time() + 0.1
    retry_delay = namespace["_ds_next_poll_delay"]()
    if retry_delay is None or retry_delay <= 0.0 or retry_delay > 0.2:
        raise AssertionError("Ren'Py retry worker did not sleep until the next deadline")
    namespace["_ds_retry_after"]["Probe dialogue"] = 0.0
    if http_calls:
        raise AssertionError("Ren'Py render path performed HTTP")

    started = time.perf_counter()
    for unused in range(50000):
        namespace["_ds_fetch"]("Probe dialogue")
    hot_path_seconds = time.perf_counter() - started
    if hot_path_seconds > 0.75:
        raise AssertionError("Ren'Py memo hot path exceeded its latency budget")

    namespace["_ds_memo_put"]("Cached dialogue", "Translated dialogue", time.time())
    if namespace["_ds_fetch"]("Cached dialogue") != "Translated dialogue":
        raise AssertionError("in-process translation was not returned immediately")
    if "Cached dialogue" in namespace["_ds_pending"]:
        raise AssertionError("memo hit should not queue background work")

    for index in range(8002):
        key = "memo-%d" % index
        namespace["_ds_memo_put"](key, "value-%d" % index, float(index))
    memo_size = len(namespace["_ds_memo"])
    if memo_size > 8000 or memo_size < 7000:
        raise AssertionError("bounded memo eviction removed too much or too little")

    for index in range(60):
        namespace["_ds_note_pending_many"](("Background UI text %d" % index,), False)

    choices = [
        "Normal difficulty - A balanced challenge.",
        "Easy difficulty - A slight ability scores boost for a more forgiving adventure.",
        "Story difficulty - A large ability scores boost for a focus on narrative.",
    ]
    menu = renpy.ast.Menu([(choice, "True", []) for choice in choices])
    if menu.execute() != "menu-executed" or not menu.executed:
        raise AssertionError("Ren'Py Menu.execute wrapper did not preserve the engine call")

    selected = namespace["_ds_select_poll_batch"](time.time(), 8)
    if selected[: len(choices)] != choices:
        raise AssertionError("visible Ren'Py menu labels were not promoted ahead of background UI")

    namespace["_ds_ensure_fast_live_worker"] = lambda: None
    namespace["_ds_queue_live"](choices, True)
    if namespace["_ds_fast_live_queue"] != choices:
        raise AssertionError("visible Ren'Py menu labels did not enter the independent fast queue")
    if namespace["_ds_live_queue"]:
        raise AssertionError("visible Ren'Py menu labels leaked into the normal live queue")

    print("renpy hook probe passed (50000 memo hits in %.3fs)" % hot_path_seconds)


if __name__ == "__main__":
    main()
