# Maintenance Map

Use this file as the first lookup table after `AGENTS.md` and `CONTEXT.md`.
It is intentionally concrete: search these symbols before changing behavior.

## Fast Locate

| Need to change | Start here | Guard/tests |
| --- | --- | --- |
| Engine enum/name | `native/src/launcher/engine.h`, `engine_name` in `native/src/launcher/engine.c` | `tests/launcher_probe.c`, `tests/test_launcher.ps1` |
| Engine detection order | `detect_engine`, `godot_export_or_project`, `find_subdir_suffix` in `native/src/launcher/engine.c` | Unity-with-`.pck` fixture in `tests/test_launcher.ps1` |
| Start button flow | `start_translation`, `warmup_launch_thread` in `native/src/launcher/ui.c` | launcher source checks in `tests/test_launcher.ps1` |
| Server lifetime/readiness | `native/src/launcher/server_proc.c` | `tests/test_launcher.ps1`, `tests/run_all.ps1 -SkipEndurance` |
| Ren'Py deploy | `deploy_renpy`, `deploy_renpy_font`, `RENPY_HOOK` in `native/src/launcher/deploy.c` | `tests/launcher_probe.c`, `tests/renpy_hook_probe.py`, Ren'Py checks in `tests/test_launcher.ps1` |
| RPG Maker deploy/runtime | `deploy_rpgm`, `deploy_rpgm_font`, `RPGM_HOOK`, `planMessagePage`, `safeRpgmTranslation`, `markRpgmDisplayTarget`, `canAutoRefreshWindow`, `isPersistenceScene`, `installMessageRuntimeHooks`, and `installPluginLoadHook` in `native/src/launcher/deploy.c` | `tests/rpgm_hook_probe.js`, `tests/launcher_probe.c`, RPG Maker checks in `tests/test_launcher.ps1` |
| Unity Mono deploy/runtime | `deploy_unity` in `deploy.c`; `DriverUpdate`, host `Update`, `StripTranslationPromptEchoPrefix`, `LoadServerCache`, `ImportServerCacheEntries`, `ScheduleAsyncApply`, and `PersistLocalCacheAsync` in `payloads/UnityTranslator/src/DeepSeekTranslator.cs` | `tests/test_unity_text_rules.ps1`, `tests/test_launcher.ps1` |
| Unity IL2CPP/XUnity deploy/runtime | `deploy_unity_il2cpp`; `TrimSlash`, `StripTranslationPromptEchoPrefix`, and `OnExtractTranslation` in `payloads/UnityIL2CPP/DeepSeekXUnityTranslator/src`; `PatchLoadedFontAssets` and `AddToListProperty` in `payloads/UnityIL2CPP/DeepSeekTMPFontFallback/src` | `tests/test_unity_text_rules.ps1`, `tests/test_launcher.ps1` |
| Godot deploy/warmup/patch pack | `deploy_godot` in `native/src/launcher/deploy.c`; `warmup_godot` in `native/src/launcher/warmup.c`; private warmup seam in `native/src/launcher/warmup_internal.h`; `warmup_scan_godot_resources`, `scan_godot_resource_dir`, `scan_godot_po_strings`, `scan_godot_csv_strings`, `scan_godot_binary_buffer` in `native/src/launcher/godot_warmup.c`; `godot_prepare_patch_pack` and `godot_prepare_runtime_sidecar` in `native/src/launcher/godot_patch.c` | `tests/warmup_probe.c`, `tests/godot_patch_probe.c`, `tests/launcher_probe.c`, `tests/test_launcher.ps1` |
| Warmup HTTP/cache queueing | `LocalHttp`, `post_prefetch_all`, `localhost_get_cached_translate` in `native/src/launcher/warmup.c` | `tests/test_launcher.ps1`, `tests/run_all.ps1 -SkipEndurance` |
| Shared server API/cache | `normalize_translation_result` in `native/src/server/util.c`; `native/src/server/api.c`, `native/src/server/cache.c` | `tests/test_server.ps1`, `tests/test_api_guard.ps1`, `tests/test_cache_export_persistence.ps1`, `tests/test_concurrency.ps1` |
| Failure diagnostics/fallback audit | `api_diag` in `native/src/server/api.c`; `_ds_report_exception` and `reportRpgmError` in `deploy.c`; `ReportCaughtException` in Unity Mono/TMP; `_dst_report_error` in `godot_patch.c` | `tests/test_server.ps1`, `tests/test_launcher.ps1`, `tests/test_unity_text_rules.ps1`, `tests/godot_patch_probe.c`, `docs/FAILURE_TRANSPARENCY.md` |
| Build output name/version | `VERSION`, `build_native.bat`, `native/src/launcher/ui.c` | version checks in `tests/test_launcher.ps1` |
| Native compiler warning gate | `-Wall -Wextra -Werror` flags in `build_native.bat` | build flag checks in `tests/test_launcher.ps1`, then `build_native.bat` |
| Runtime payload installer | `scripts/install_runtime_payloads.ps1`, `docs/RUNTIME_PAYLOADS.md` | installer checks in `tests/test_launcher.ps1` |
| Source-only package | `scripts/prepare_open_source_release.ps1`, `scripts/audit_open_source_release.ps1`, `OPEN_SOURCE_RELEASE.md` | `prepare_open_source_release.ps1 -NoZip` |
| Program package | `scripts/prepare_program_release.ps1` | package smoke with `ds游戏翻译器.exe --sync-payloads-and-exit` |

## Search Shortcuts

```powershell
rg -n "ENGINE_GODOT|deploy_godot|warmup_godot|warmup_scan_godot_resources|godot_prepare_patch_pack|scan_godot" native/src/launcher tests
rg -n "ENGINE_RENPY|deploy_renpy|warmup_renpy|RENPY_HOOK" native/src/launcher tests
rg -n "ENGINE_RPGM|deploy_rpgm|warmup_rpgm|RPGM_HOOK" native/src/launcher tests
rg -n "deploy_unity|deploy_unity_il2cpp|UnityTranslator|DeepSeekTMPFontFallback|XUnity" native payloads tests
rg -n "cache_only|/cache/import|/cache/lookup|/prefetch|translation_memory" native payloads tests scripts docs
rg -n "prepare_.*release|audit_open_source|install_runtime_payloads|build_native" scripts tests README.md docs
```

## Add Or Replace An Engine Path

1. Add or update `Engine` in `native/src/launcher/engine.h`.
2. Add detection in `detect_engine`; keep broad/weak markers after stronger
   engine markers. Example: Unity `_Data` stays before Godot `.pck`.
3. Add deploy behavior in `deploy.c` and declaration in `deploy.h`.
4. Route it in `start_translation` and `warmup_translations`.
5. Add scanner/parser tests to `tests/warmup_probe.c` when warmup behavior
   changes.
6. Add launcher detection/deploy fixtures to `tests/launcher_probe.c` and
   `tests/test_launcher.ps1`.
7. Update user docs only after behavior and tests match.

## Blast Radius Checklist

Classify every change before editing:

- local C server/API/cache
- launcher detection/deploy/warmup
- Ren'Py hook/warmup
- RPG Maker hook/warmup
- Unity Mono/BepInEx 5 or 6 plugin
- Unity IL2CPP/XUnity/TMP fallback payload
- Godot detection/resource warmup
- release/package scripts

If a change crosses one of these boundaries, add the closest guard in the
affected engine and one adjacent guard for the engine most likely to regress.

## Things Not To Pollute

- Do not fix renderer display by rewriting shared `translation_memory_c.tsv`.
- Do not write cache misses, queued translations, or original echo pass-throughs
  as successful translated text.
- Do not move Unity TMP glyph punctuation fallback into `native/src/server`.
- Do not bundle third-party runtimes, Unity/game assemblies, real API config,
  logs, user cache files, or diagnostics into source-only releases.
- Do not rewrite original `.pck`/embedded archives from the Godot path; generate
  external patch packs instead.

## Verification Ladder

Use the narrowest useful check first, then climb when blast radius grows:

```powershell
powershell -ExecutionPolicy Bypass -File tests\test_launcher.ps1
powershell -File tests\run_all.ps1 -SkipEndurance
cmd /c build_native.bat
powershell -ExecutionPolicy Bypass -File scripts\prepare_open_source_release.ps1 -Version <version> -NoZip
powershell -ExecutionPolicy Bypass -File scripts\prepare_program_release.ps1 -Version <version>
powershell -File tests\run_all.ps1
```

For program package parity, run the produced standalone exe once with
`--sync-payloads-and-exit` in a temporary directory and check that the local
server, scripts, example config, and first-party Unity plugin DLLs are emitted.
