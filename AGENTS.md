# AI Project Requirements

This file is the standing instruction for any AI or developer modifying this
repository. Read it before making changes.

## Project Goal

ds游戏翻译器 is intended to become a general-purpose game
translation tool for:

- Ren'Py games
- RPG Maker games, including supported legacy and MV/MZ-style variants
- Unity games across supported versions, including Mono/BepInEx and IL2CPP

The long-term goal is broad compatibility across these engines, not a
single-game or single-engine patch.

## Non-Regression Rule

Any change must preserve existing behavior unless the user explicitly asks to
change it. Do not fix one engine by breaking another engine.

Before changing shared code, check whether it is used by Ren'Py, RPG Maker, or
Unity paths. If it is shared, treat the change as cross-engine and verify the
affected paths.

Before making a fix, classify its blast radius as one or more of:

- local C server/API/cache
- launcher detection/deploy/warmup
- Ren'Py hook/warmup
- RPG Maker hook/warmup
- Unity Mono/BepInEx 5 or 6 plugin
- Unity IL2CPP/XUnity/TMP fallback payload

Renderer compatibility fixes must stay in the renderer-specific layer whenever
possible. For example, font coverage, missing glyph replacement, typewriter
handling, and overlay behavior belong in the relevant Ren'Py/RPG Maker/Unity
hook or payload, not in the global translation memory. Do not normalize or
rewrite cached translations globally just to satisfy one engine's renderer.

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

- `build_native.bat`
- `powershell -File tests\run_all.ps1 -SkipEndurance`
- Targeted tests under `tests\` when the change is narrow
- UI screenshot/manual verification for launcher layout changes
- Endurance or stress tests when touching server lifetime, HTTP, cache, or
  concurrency behavior

If a verification step cannot be run, report that clearly and explain why.

## Modification Guidance

- Prefer small, engine-aware changes over broad rewrites.
- Keep engine-specific behavior isolated when possible.
- Add or update regression tests when fixing a bug that could return.
- Preserve public API response fields and launcher workflows unless there is a
  deliberate migration plan.
- When unsure whether a change affects original functionality, stop and inspect
  the relevant Ren'Py, RPG Maker, and Unity paths first.

## User Intent Summary

The user's stated requirement is: this project should be a universal
translator for Ren'Py, RPG Maker, and Unity games across versions. Future AI
agents must preserve original working functionality while making any change.
