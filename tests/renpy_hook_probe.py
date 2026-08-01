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
    renpy._probe_main_thread_calls = 0
    renpy._probe_redraws = []
    renpy.exports = types.SimpleNamespace(say=lambda who, what, *args, **kwargs: what)
    renpy.config = types.SimpleNamespace(
        old_substitutions=False,
        say_menu_text_filter=None,
        gamedir=gamedir,
        font_replacement_map={},
        start_callbacks=[],
    )
    renpy.style = types.SimpleNamespace(styles={})
    renpy.restart_interaction = lambda: None

    text_module = types.ModuleType("renpy.text.text")

    class Text:
        def __init__(self, text):
            self.text = text
            self.dirty = False
            self.killed_layouts = 0

        def kill_layout(self):
            self.killed_layouts += 1

    text_module.Text = Text
    renpy.text = types.SimpleNamespace(text=text_module)
    renpy.display = types.SimpleNamespace(
        render=types.SimpleNamespace(
            redraw=lambda displayable, delay: renpy._probe_redraws.append((displayable, delay))
        )
    )

    def invoke_in_main_thread(callback, *args, **kwargs):
        renpy._probe_main_thread_calls += 1
        return callback(*args, **kwargs)

    renpy.invoke_in_main_thread = invoke_in_main_thread

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
    renpy.game = types.SimpleNamespace(
        script=types.SimpleNamespace(namemap={})
    )
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

    startup_choices = [
        "Reassure her {color=#FF0090}lovingly{/color}.",
        "Assuage her doubts in a {color=#B22222}controlling{/color} manner.",
        "(Path Explanation)",
    ]
    renpy.game.script.namemap = {
        "menu-one": renpy.ast.Menu([(choice, "True", []) for choice in startup_choices]),
        "menu-two": renpy.ast.Menu([(startup_choices[0], "True", [])]),
        "not-a-menu": types.SimpleNamespace(items=[("Implementation detail", "True", [])]),
    }
    collected_choices = namespace["_ds_collect_script_menu_labels"]()
    if collected_choices != startup_choices:
        raise AssertionError("Ren'Py startup did not collect exact deduplicated compiled menu labels")
    startup_prefetch_calls = []
    namespace["_ds_http"] = lambda *args, **kwargs: startup_prefetch_calls.append((args, kwargs)) or {}
    namespace["_ds_prefetch_script_menus"]()
    if len(startup_prefetch_calls) != 1:
        raise AssertionError("Ren'Py startup menu labels were not submitted in one prefetch batch")
    if startup_prefetch_calls[0][0][0] != "/prefetch" or startup_prefetch_calls[0][0][1] != {"texts": startup_choices}:
        raise AssertionError("Ren'Py startup menu prefetch changed labels or used the wrong endpoint")
    if not renpy.config.start_callbacks:
        raise AssertionError("Ren'Py menu prefetch was not attached to the engine start lifecycle")
    namespace["_ds_http"] = lambda *args, **kwargs: http_calls.append((args, kwargs))

    visible_text = renpy.text.text.Text("Start")
    namespace["_ds_refresh_interaction"]()
    if renpy._probe_main_thread_calls != 1:
        raise AssertionError("Ren'Py interaction refresh was not dispatched to the main thread")
    if visible_text.killed_layouts != 1 or not visible_text.dirty:
        raise AssertionError("Ren'Py cache hit did not invalidate an existing Text layout")
    if renpy._probe_redraws != [(visible_text, 0)]:
        raise AssertionError("Ren'Py cache hit did not redraw the existing Text displayable")

    for store_name in ("time", "json", "threading", "os", "sys", "traceback"):
        namespace[store_name] = 7
    collision_text = "Store module name collision"
    if namespace["_ds_fetch"](collision_text) is not None:
        raise AssertionError("first store-collision miss should preserve source")
    if collision_text not in namespace["_ds_pending"]:
        raise AssertionError("Ren'Py store variables shadowed a hook standard-library module")
    namespace["_ds_pending"].pop(collision_text, None)
    namespace["_ds_retry_after"].pop(collision_text, None)
    namespace["_ds_memo"].pop(collision_text, None)

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

    rejected_dialogue = "Token-rejected dialogue"
    namespace["_ds_mark_terminal_negative"](rejected_dialogue)
    if namespace["_ds_fetch"](rejected_dialogue) != rejected_dialogue:
        raise AssertionError("fresh terminal-negative text should preserve source")
    if rejected_dialogue in namespace["_ds_pending"]:
        raise AssertionError("fresh terminal-negative text should not immediately requeue")
    namespace["_ds_terminal_negative"][rejected_dialogue] = (
        time.time() - namespace["_ds_terminal_negative_ttl"] - 1.0
    )
    if namespace["_ds_fetch"](rejected_dialogue) is not None:
        raise AssertionError("expired terminal-negative text should re-enter background lookup")
    if rejected_dialogue not in namespace["_ds_pending"]:
        raise AssertionError("expired terminal-negative text was not queued to observe a corrected cache entry")
    with namespace["_ds_lock"]:
        namespace["_ds_forget_pending_locked"](rejected_dialogue)

    token_source = "Welcome [player]! {color=#f00}Danger{/color}"
    token_valid = "欢迎 [player]！{color=#f00}危险{/color}"
    if namespace["_ds_restore_renpy_tokens"](token_source, token_valid) != token_valid:
        raise AssertionError("Ren'Py token guard rejected a valid protected-token translation")
    token_invalid = "欢迎！{color=#f00}危险{/color}"
    if namespace["_ds_restore_renpy_tokens"](token_source, token_invalid) != token_source:
        raise AssertionError("Ren'Py token guard accepted a translation that dropped interpolation")

    for index in range(8002):
        key = "memo-%d" % index
        namespace["_ds_memo_put"](key, "value-%d" % index, float(index))
    memo_size = len(namespace["_ds_memo"])
    if memo_size > 8000 or memo_size < 7000:
        raise AssertionError("bounded memo eviction removed too much or too little")

    for index in range(60):
        namespace["_ds_note_pending_many"](("Background UI text %d" % index,), False)

    visible_dialogue = "Visible dialogue must bypass background UI"
    if namespace["_ds_translate"](visible_dialogue, True) != visible_dialogue:
        raise AssertionError("first visible dialogue miss should preserve source")
    selected = namespace["_ds_select_poll_batch"](time.time(), 1)
    if selected != [visible_dialogue]:
        raise AssertionError("visible Ren'Py dialogue was not promoted ahead of background UI")
    with namespace["_ds_lock"]:
        namespace["_ds_forget_pending_locked"](visible_dialogue)

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

    with namespace["_ds_lock"]:
        namespace["_ds_pending"].clear()
        namespace["_ds_retry_after"].clear()
        namespace["_ds_priority_pending"][:] = []
        namespace["_ds_priority_set"].clear()
        namespace["_ds_inflight"].clear()
        namespace["_ds_live_queue"][:] = []
        namespace["_ds_fast_live_queue"][:] = []
    for index in range(1200):
        namespace["_ds_note_pending_many"](("Background saturation %04d" % index,), False)
    saturated_visible = "Visible dialogue survives a saturated Ren'Py background queue"
    if namespace["_ds_translate"](saturated_visible, True) != saturated_visible:
        raise AssertionError("first saturated visible miss should preserve source")
    if saturated_visible not in namespace["_ds_pending"] or saturated_visible not in namespace["_ds_priority_set"]:
        raise AssertionError("Ren'Py visible text was dropped behind a saturated background queue")
    if len(namespace["_ds_pending"]) > 1200:
        raise AssertionError("Ren'Py priority admission must preserve the bounded pending queue")
    selected = namespace["_ds_select_poll_batch"](time.time(), 1)
    if selected != [saturated_visible]:
        raise AssertionError("Ren'Py saturated visible text did not keep poll priority")

    print("renpy hook probe passed (50000 memo hits in %.3fs)" % hot_path_seconds)


if __name__ == "__main__":
    main()
