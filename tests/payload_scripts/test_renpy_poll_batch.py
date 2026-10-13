"""执行正式 Ren'Py 队列函数，不启动游戏或工作线程。"""

import ast
import random
import sys
import threading
import unittest
from pathlib import Path
from types import SimpleNamespace


HOOK = Path(__file__).resolve().parents[2] / "payloads/RenPy/iron_deepseek.rpy"
FUNCTIONS = {
    "_ds_select_poll_batch", "_ds_queue_live", "_ds_forget_pending_locked",
    "_ds_live_loop", "_ds_note_pending_many", "_ds_evict_nonpriority_pending_locked",
}
body = "\n".join(line[4:] if line else "" for line in HOOK.read_text(encoding="utf-8").splitlines()[1:])
tree = ast.parse(body, str(HOOK))
nodes = [node for node in tree.body if isinstance(node, ast.FunctionDef) and node.name in FUNCTIONS]
if {node.name for node in nodes} != FUNCTIONS:
    raise AssertionError("Ren'Py queue function missing from shipped hook")
QUEUE_CODE = compile(ast.Module(body=nodes, type_ignores=[]), str(HOOK), "exec")


class CheckedLock:
    def __init__(self):
        self.lock = threading.RLock()
        self.depth = 0

    def __enter__(self):
        self.lock.acquire()
        self.depth += 1

    def __exit__(self, *args):
        self.depth -= 1
        self.lock.release()

    def require_held(self):
        if not self.depth:
            raise AssertionError("queue state read outside _ds_lock")


class CountedDict(dict):
    def __init__(self, lock, items=()):
        super().__init__(items)
        self.lock = lock
        self.visited = 0

    def __iter__(self):
        self.lock.require_held()
        for key in super().__iter__():
            self.visited += 1
            yield key

    def keys(self):
        return iter(self)

    def __contains__(self, key):
        self.lock.require_held()
        return super().__contains__(key)

    def get(self, key, default=None):
        self.lock.require_held()
        return super().get(key, default)


class CountedList(list):
    def __init__(self, lock, items=()):
        super().__init__(items)
        self.lock = lock
        self.visited = 0

    def __iter__(self):
        self.lock.require_held()
        for item in super().__iter__():
            self.visited += 1
            yield item


def make_state(pending=(), priority=(), inflight=(), retry=()):
    lock = CheckedLock()
    signals = []
    priority = list(priority)
    state = {
        "_ds_lock": lock,
        "_ds_pending": CountedDict(lock, ((key, 0) for key in pending)),
        "_ds_priority_pending": CountedList(lock, priority),
        "_ds_priority_set": set(priority),
        "_ds_inflight": set(inflight),
        "_ds_retry_after": CountedDict(lock, retry),
        "_ds_live_queue": [], "_ds_fast_live_queue": [],
        "_ds_ensure_live_worker": lambda: signals.append("normal-worker"),
        "_ds_ensure_fast_live_worker": lambda: signals.append("fast-worker"),
        "_ds_ensure_poller": lambda: signals.append("poller"),
        "_ds_live_wake": SimpleNamespace(set=lambda: signals.append("normal-wake")),
        "_ds_fast_live_wake": SimpleNamespace(set=lambda: signals.append("fast-wake")),
        "_ds_wake": SimpleNamespace(set=lambda: signals.append("poll-wake")),
        "_ds_time": SimpleNamespace(time=lambda: 10.0),
        "_ds_state": {"down_until": 0.0},
        "signals": signals,
    }
    exec(QUEUE_CODE, state)
    return state


class RenpyPollBatchTests(unittest.TestCase):
    def test_priority_order_and_retry_deadline_before_background(self):
        ns = make_state(
            pending=["background-first", "priority-late", "priority-first", "busy", "future", "background-last"],
            priority=["removed", "future", "priority-first", "busy", "priority-late"],
            inflight=["busy"], retry={"future": 10.1, "priority-first": 10.0, "background-last": 9.0}.items())
        self.assertEqual(ns["_ds_select_poll_batch"](10.0, 4),
                         ["priority-first", "priority-late", "background-first", "background-last"])
        self.assertEqual(ns["_ds_lock"].depth, 0)
        self.assertEqual(ns["_ds_inflight"], {"busy"})
        self.assertEqual(len(ns["_ds_pending"]), 6)
        self.assertEqual(ns["_ds_retry_after"]["future"], 10.1)

    def test_priority_full_batch_does_not_visit_tail_or_background(self):
        keys = ["text-%d" % i for i in range(1200)]
        ns = make_state(keys, reversed(keys))
        self.assertEqual(ns["_ds_select_poll_batch"](10.0), list(reversed(keys))[:96])
        self.assertEqual(ns["_ds_priority_pending"].visited, 96)
        self.assertEqual(ns["_ds_pending"].visited, 0)

    def test_background_full_batch_does_not_visit_tail(self):
        keys = ["text-%d" % i for i in range(1200)]
        ns = make_state(keys)
        self.assertEqual(ns["_ds_select_poll_batch"](10.0), keys[:96])
        self.assertEqual(ns["_ds_pending"].visited, 96)

    def test_mixed_batch_stops_after_last_needed_background_key(self):
        keys = ["text-%d" % i for i in range(1200)]
        ns = make_state(keys, keys[100:148], keys[:8], {keys[8]: 11.0}.items())
        self.assertEqual(ns["_ds_select_poll_batch"](10.0), keys[100:148] + keys[9:57])
        self.assertEqual(ns["_ds_pending"].visited, 57)

    def test_empty_zero_small_and_oversized_limits(self):
        for keys, limit, expected in [([], 96, []), (["one"], 0, []),
                                      (["one", "two"], 1, ["two"]),
                                      (["one", "two"], 2000, ["two", "one"])]:
            with self.subTest(limit=limit, keys=keys):
                ns = make_state(keys, keys[-1:])
                self.assertEqual(ns["_ds_select_poll_batch"](10.0, limit), expected)
                self.assertEqual(ns["_ds_lock"].depth, 0)

    def test_all_inflight_or_delayed_produces_no_batch(self):
        ns = make_state(["busy", "future"], ["future"], ["busy"], [("future", 11.0)])
        self.assertEqual(ns["_ds_select_poll_batch"](10.0), [])

    def test_varied_queue_states_preserve_selection_policy(self):
        rng = random.Random(20260916)
        for case in range(150):
            keys = ["text-%d" % i for i in range(rng.randrange(1, 220))]
            rng.shuffle(keys)
            priority = rng.sample(keys, rng.randrange(len(keys) + 1))
            priority.insert(0, "stale-priority")
            busy = set(rng.sample(keys, rng.randrange(len(keys) + 1)))
            retry = {key: rng.choice([0.0, 9.0, 10.0, 11.0]) for key in keys}
            limit = rng.choice([0, 1, 16, 96, 250])
            ready = {key for key in keys if key not in busy and retry[key] <= 10.0}
            expected = [key for key in priority + [key for key in keys if key not in priority]
                        if key in ready][:limit]
            ns = make_state(keys, priority, busy, retry.items())
            with self.subTest(case=case):
                self.assertEqual(ns["_ds_select_poll_batch"](10.0, limit), expected)

    def test_live_workers_remain_separate_and_inflight_is_not_polled(self):
        ns = make_state(["background", "dialogue", "menu", "later"], ["dialogue", "menu"])
        ns["_ds_queue_live"](["dialogue", "menu", "dialogue", "absent"], True)
        ns["_ds_queue_live"](["background", "menu", "background"], False)
        self.assertEqual(ns["_ds_fast_live_queue"], ["dialogue", "menu"])
        self.assertEqual(ns["_ds_live_queue"], ["background"])
        self.assertEqual(ns["signals"], ["fast-worker", "fast-wake", "normal-worker", "normal-wake"])
        self.assertEqual(ns["_ds_select_poll_batch"](10.0), ["later"])

    def test_promotion_keeps_existing_retry_and_deduplicates_priority(self):
        ns = make_state(["background", "dialogue"], retry=[("dialogue", 11.0)])
        ns.update({"_ds_string_types": (str,), "_ds_has_cjk": lambda _: False,
                   "_ds_is_terminal_negative": lambda *_: False, "_ds_memo": {},
                   "_ds_memo_put": lambda *_: None})
        ns["_ds_note_pending_many"](["dialogue", "dialogue"], True)
        self.assertEqual(ns["_ds_priority_pending"], ["dialogue"])
        self.assertEqual(ns["_ds_select_poll_batch"](10.0), ["background"])
        self.assertEqual(ns["_ds_select_poll_batch"](11.0), ["dialogue", "background"])

    def test_shared_server_pending_and_pass_results_do_not_become_translations(self):
        class StopWorker(BaseException):
            pass

        class Wake:
            waits = 0

            def wait(self):
                self.waits += 1
                if self.waits > 1:
                    raise StopWorker()

            def clear(self):
                pass

        keys = ["queued", "miss", "identity", "translated"]
        ns = make_state(keys, keys, keys)
        memo, terminal, refresh = [], [], []
        ns.update({
            "_ds_http": lambda *_: {"translations": {"identity": "identity", "translated": "result"},
                                    "sources": ["queued", "miss", "pass", "cache"]},
            "_ds_memo_put": lambda key, value, now: memo.append((key, value)),
            "_ds_mark_terminal_negative": terminal.append,
            "_ds_restore_renpy_tokens": lambda key, value: value,
            "_ds_refresh_interaction": lambda: refresh.append(True),
        })
        with self.assertRaises(StopWorker):
            ns["_ds_live_loop"](keys[:], Wake(), 16)
        self.assertEqual(memo, [("queued", "queued"), ("miss", "miss"), ("translated", "result")])
        self.assertEqual(terminal, ["identity"])
        self.assertEqual(ns["_ds_inflight"], set())
        self.assertEqual(ns["_ds_select_poll_batch"](10.0), [])
        self.assertEqual(ns["_ds_select_poll_batch"](10.5), ["queued", "miss"])
        self.assertEqual(ns["_ds_pending"], {"queued": 1, "miss": 1})
        self.assertEqual(refresh, [True])


if __name__ == "__main__":
    unittest.main(testRunner=unittest.TextTestRunner(stream=sys.stdout, verbosity=2))
