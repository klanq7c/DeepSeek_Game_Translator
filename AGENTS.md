# AI Project Requirements

This file is the standing instruction for any AI or developer modifying this
repository. Read it before making changes.

After this file, read `CONTEXT.md` for the shared domain vocabulary and
`docs/MAINTENANCE_MAP.md` for the fastest code/test lookup table.

## Agent skills

### Issue tracker

Issues and PRDs are tracked as local Markdown under `.scratch/`. See
`docs/agents/issue-tracker.md`.

### Triage labels

The repository uses the default five-role triage vocabulary. See
`docs/agents/triage-labels.md`.

### Domain docs

This is a single-context repository rooted at `CONTEXT.md`. See
`docs/agents/domain.md`.

## Project Goal

ds游戏翻译器 is intended to become a general-purpose game
translation tool for:

- Ren'Py games
- RPG Maker games, including supported legacy and MV/MZ-style variants
- Unity games across supported versions, including Mono/BepInEx and IL2CPP
- Godot games, starting with exported/project resource detection and cache warmup

The long-term goal is broad compatibility across these engines, not a
single-game or single-engine patch.

## Non-Regression Rule

Any change must preserve existing behavior unless the user explicitly asks to
change it. Do not fix one engine by breaking another engine.

Before changing shared code, check whether it is used by Ren'Py, RPG Maker,
Unity, or Godot paths. If it is shared, treat the change as cross-engine and
verify the affected paths.

Before making a fix, classify its blast radius as one or more of:

- local C server/API/cache
- launcher detection/deploy/warmup
- Ren'Py hook/warmup
- RPG Maker hook/warmup
- Unity Mono/BepInEx 5 or 6 plugin
- Unity IL2CPP/XUnity/TMP fallback payload
- Godot detection/resource warmup

Renderer compatibility fixes must stay in the renderer-specific layer whenever
possible. For example, font coverage, missing glyph replacement, typewriter
handling, and overlay behavior belong in the relevant Ren'Py/RPG Maker/Unity
hook or payload, or Godot-specific resource layer, not in the global
translation memory. Do not normalize or rewrite cached translations globally
just to satisfy one engine's renderer.

When fixing a regression in one engine, add or update a guard that protects the
closest adjacent behavior in the other engines or in the shared server contract.
This is mandatory for changes touching deploy payloads, cache import/export,
warmup, rich text/tag handling, font fallback, concurrency, or persistence.

## Compatibility Requirements

- Launcher behavior must remain compatible with existing engine detection,
  deployment, cache warmup, local server startup, API configuration, and game
  launch flow.
- The local C server must keep the current runtime contract: local cache first,
  immediate response when API work is queued or missing, and no blocking game
  runtime on remote API latency.
- Translation hooks must protect tags, variables, color/control sequences, and
  engine-specific markup.
- Engine hooks must handle their own display compatibility, including CJK font
  coverage or fallback, without corrupting shared cached translations.
- Cache misses, queued translations, or pass-through originals must not be
  written back as successful translated text.
- Existing plugin payloads and deployment folders must remain usable for games
  that already worked.
- Do not hard-code a specific user's game path or machine path into source code.
- Do not delete or rewrite user translation memory or config files unless the
  user explicitly requests it.

## Expected Verification

For code changes, run the closest practical verification before finishing:

- `build_native.bat` — builds the C server, C# server, launcher, and the C#
 launcher port; runs `tests\core_tests` (shared-core golden parity) and
 `tests\launcher_parity` (C vs C# launcher detection/deploy/restore/warmup/
 payload sync/Godot patch pack/api.ini/Godot preflight/one-click launch flow/
 window rendering) as gates. The Godot patch scenarios bind
 127.0.0.1:19999 for a fake `/batch` server and are skipped with a printed
 note when the port is already taken. Keep the batch file ASCII-only
 (cmd.exe misparses multibyte bytes).
- For launcher layout changes: the `ui` scenarios in `tests\launcher_parity`
  compare the layout report and a client-area bitmap between the two launchers
  at three window sizes and both server states. They pin C↔C# identity, not
  correctness, so still take a screenshot of the real window at your own DPI —
  the probe runs at a fixed 96 DPI with a frozen animation clock.
- For server/translation changes: `tests\server_contract\run_contract_tests.ps1`
  (fake provider, 50 checks per binary; runs both servers by default, or pass
  `-Exe native\dst_server_cs.exe` for one). Both servers must pass; they share
  one contract.
- For payload script changes (`payloads\RenPy`, `payloads\RPGMaker`,
  `payloads\Godot`): `tests\payload_scripts\check_payload_scripts.ps1`.
- For launcher server-lifecycle changes:
  `tests\launcher_parity\run_launcher_parity.ps1 -ServerSmoke` (needs port
  19999 free; it will stop a server it finds running, so do not run it against
  a user's live session).

The former `tests\` regression suites were removed at the owner's request on
2026-09-06 after a final full-suite pass; the suites above were rebuilt on
2026-09-08 during the language migration. Historical guard names in
`docs/MAINTENANCE_MAP.md` remain as references only.

If a verification step cannot be run, report that clearly and explain why.

## Modification Guidance

- Prefer small, engine-aware changes over broad rewrites.
- 翻译核心运行模块的说明性代码注释统一使用中文；技术标识、协议字段以及
  第三方版权或许可证原文保持原样，不要为了注释语言改动外部接口与测试名称。
- Keep engine-specific behavior isolated when possible.
- Add or update regression tests when fixing a bug that could return.
- Preserve public API response fields and launcher workflows unless there is a
  deliberate migration plan.
- When unsure whether a change affects original functionality, stop and inspect
  the relevant Ren'Py, RPG Maker, Unity, and Godot paths first.
- Language migration (C → C#) rules: shared text/JSON/cache/contract logic
  lives once in `native/src/core` (`DstCore`) and is source-linked, never
  copied. A behavior change in a C launcher module that already has a C#
  mirror (`engine.c`, `deploy.c` for all engines, `fsutil.c`, `embedded.c`,
  `server_proc.c`, `warmup.c`, `godot_warmup.c`, `self_update.c`,
  `godot_patch.c`, `api_config.c` including its dialog, `godot_probe.c`,
  `godot_preflight_cache.c`, and all of `ui.c` + `main.c`: the one-click launch
  flow, the Godot launch/preflight decisions, the cache card/clear, and the
  Win32 window's painting, layout, controls and message loop — every launcher
  module now has a mirror) must be applied to `native/src/launcher_cs` in the
  same change, or `build_native.bat` fails at the parity gate. Adding an
  embedded payload means adding it to both `launcher_payloads.rc` (via
  `build_native.bat`) and the csproj `EmbeddedResource` list with the same
  macro name. The translator version is another shared input: `build_native.bat`
  passes it to C as `-DDS_TRANSLATOR_VERSION` and the csproj generates
  `DsBuildInfo.g.cs` from the same `VERSION` file, because the Godot preflight
  cache key contains it. The parity gate compares rendered pixels, so a change
  to the window must keep `Theme.Sc`/`Theme.Mix` for every pixel and blend, read
  the freezable `anim_tick()`/`Theme.AnimTick()` instead of `GetTickCount`, and
  register any new control in both probe reports; see "Change The Launcher
  Window" in `docs/MAINTENANCE_MAP.md`. Any module that is added later and not
  mirrored must return `Deploy.NotPorted`-style explicit results, never a
  fabricated success.

## Failure Transparency

Do not hide faults to keep a renderer or test superficially green. The
following repair patterns are prohibited:

- empty `catch` / `except` blocks
- swallowing an exception without a diagnostic record
- returning from an error path before recording the operation and error
- wrapping an entire worker, render, deploy, or persistence flow in one broad
  exception boundary
- hard-coded results whose only purpose is to pass a test
- repeated null guards that conceal an invalid upstream state
- delays or retries used to mask a state race without identifying its owner
- deleting a failing test or weakening its assertions

Every new fallback must document all four points at its closest ownership
boundary or in `docs/FAILURE_TRANSPARENCY.md`:

1. the exact external exception or incompatibility it prevents
2. why the problem cannot be repaired further upstream
3. whether the fallback can hide the original error
4. where and how the diagnostic context is recorded

Network backoff, process-readiness polling, and renderer compatibility fallback
are allowed only when they are tied to an external boundary, do not turn a
failure into a successful translation/cache entry, and emit rate-limited
diagnostics. Fixed-delay hook installation is not allowed when an engine
lifecycle event is available.

## User Intent Summary

The user's stated requirement is: this project should be a universal
translator for Ren'Py, RPG Maker, Unity, and Godot games across versions.
Future AI agents must preserve original working functionality while making any
change.
