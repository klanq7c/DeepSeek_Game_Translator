# Maintenance Map

Use this file as the first lookup table after `AGENTS.md` and `CONTEXT.md`.
It is intentionally concrete: search these symbols before changing behavior.

## Fast Locate

| Need to change | Start here | Guard/tests |
| --- | --- | --- |
| Engine enum/name | `native/src/launcher/engine.h`, `engine_name` in `native/src/launcher/engine.c` | `tests/launcher_probe.c`, `tests/test_launcher.ps1` |
| Engine detection order | `detect_engine`, `rpgm_content_root`, `godot_export_or_project`, `find_subdir_suffix` in `native/src/launcher/engine.c` | flat RPG Maker and generic NW.js fixtures, plus Unity-with-`.pck` fixture in `tests/test_launcher.ps1` |
| Start button flow | `start_translation`, `warmup_launch_thread` in `native/src/launcher/ui.c` | launcher source checks in `tests/test_launcher.ps1` |
| Restore-game button/ownership rules | `restore_selected_game` in `native/src/launcher/ui.c`; `restore_game`, `restore_remove_verified_payload_files`, and `restore_*` helpers in `native/src/launcher/deploy.c` | `tests/restore_probe.c`, restore source checks in `tests/test_launcher.ps1` |
| Filesystem mutation/reparse/path safety | `path_has_reparse_point`, `path_append_suffix`, `wide_format_checked`, `copy_file_safe`, `move_file_safe`, and `delete_file_safe` in `native/src/launcher/fsutil.c`; `Assert-NoReparseTraversal` in `scripts/install_runtime_payloads.ps1`; `Assert-Under` in both release scripts | path/command truncation and junction write/restore fixtures in `tests/launcher_probe.c` and `tests/test_launcher.ps1` |
| Cache card/clear-cache flow | `update_cache_card`, `clear_translation_cache` in `native/src/launcher/ui.c`; shutdown readiness in `native/src/launcher/server_proc.c` | cache-clear source checks in `tests/test_launcher.ps1`, then `tests/run_all.ps1 -SkipEndurance` |
| Launcher visual system | palette in `native/src/launcher/globals.c`; `apply_window_chrome`, `draw_tech_grid`, `draw_panel_shell`, `draw_panel_gradient`, `draw_soft_glow`, `draw_hero_data_line`, `draw_button_icon`, `paint_background`, `tick_ui_animation`, `install_button_hover_tracking`, and `card_text_brush` in `native/src/launcher/ui.c` | UI source checks in `tests/test_launcher.ps1`, then desktop/minimum-window screenshots |
| Server lifetime/readiness | `native/src/launcher/server_proc.c` | `tests/test_launcher.ps1`, `tests/run_all.ps1 -SkipEndurance` |
| Ren'Py deploy | `deploy_renpy`, `deploy_renpy_font`, `RENPY_HOOK` in `native/src/launcher/deploy.c` | `tests/launcher_probe.c`, `tests/renpy_hook_probe.py`, Ren'Py checks in `tests/test_launcher.ps1` |
| RPG Maker deploy/runtime | `rpgm_content_root` in `native/src/launcher/engine.c`; `deploy_rpgm`, `deploy_rpgm_font`, `RPGM_HOOK`, `planMessagePage`, `safeRpgmTranslation`, `hasSuspiciousRpgmSourceResidue`, `requestLocalCacheKeys`, `trDisplay`, `trBitmap`, `messagePageHasLocalLookupPending`, `deferRpgmMessageForLocalCache`, `refreshActiveRpgmMessage`, `walkSceneWindows`, `walkSceneOwnedWindows`, `markRpgmDisplayTarget`, `canAutoRefreshWindow`, `isPersistenceScene`, `installMessageRuntimeHooks`, `installAllRpgmHooks`, `installTopLevelWindowHooks`, `attachPluginScriptLoadHook`, and `installPluginLoadHook` in `native/src/launcher/deploy.c`; `warmup_scan_rpgm_resources` in `native/src/launcher/warmup.c` | `tests/rpgm_hook_probe.js`, `tests/launcher_probe.c`, `tests/warmup_probe.c`, `tests/restore_probe.c`, RPG Maker checks in `tests/test_launcher.ps1` |
| Unity Mono deploy/runtime | `unity_player_machine`, `ensure_bepinex_mono`, `unity_mscorlib_is_clearly_stripped`, `ensure_stripped_unity_mono_support`, `install_stripped_unity_font_patcher`, and `deploy_unity` in `deploy.c`; `DeepSeekUnityFontPatcher` in `payloads/UnityTranslator/src/FontPatcher`; `DriverUpdate`, host `Update`, `TMPSetTextAnyPrefix`/`Postfix`/`Finalizer`, `RegisterOptionalStaticEvent`, `GetUnityVersionSafe`, `CreateDynamicFontFromOSFontSafe`, `PinRuntimeFont`, `HideTmpSourceForOverlay`, `RestoreTmpSourceVisibility`, `FindObjectsOfTypeAllSafe`, `DiscoverTmpUpdateManagerRegistry`, `ResolveAssetBundleTypeSafe`, `WaitForScaledSecondsSafe`, `GetComponentsInChildrenByTypeSafe`, `TryMeasureImGuiContentSafe`, `InstallFungusHooks`, `QueueFungusLookahead`, `PrefetchServerTextsAsync`, `StripTranslationPromptEchoPrefix`, `LoadServerCache`, `TryIsolateOversizedLocalCacheLocked`, `ImportServerCacheEntries`, `ScheduleAsyncApply`, `UnityLongTextPlanner`, `TryQueueLongTextTranslation`, `CompleteLongTextSegment`, and `PersistLocalCacheAsync` in `payloads/UnityTranslator/src` | normal/stripped and x86/x64 deploy fixtures in `tests/launcher_probe.c`, `tests/restore_probe.c`, `tests/test_unity_text_rules.ps1`, `tests/test_launcher.ps1` |
| Unity IL2CPP/XUnity deploy/runtime | `deploy_unity_il2cpp`, `write_xunity_config`, and `migrate_xunity_owned_ini_setting`; `TrimSlash`, `StripTranslationPromptEchoPrefix`, `ProtectMixedCjkForRequest`, `TryRestoreMixedCjk`, `IsPassSource`, `TryCollectPendingTexts`, `TryKeepIdentityResultsSessionOnly`, `HasOnlyResolvedOrPassSources`, and `OnExtractTranslation` in `payloads/UnityIL2CPP/DeepSeekXUnityTranslator/src`; `ShouldUseFastNormalizeScan`, `ShouldDeferTextSetterPatchForFreshInterop`, `ScheduleTextSetterPatchRetry`, `PatchLoadedFontAssets`, `AddToListProperty`, `TryWarmFallbackGlyphs`, `PatchTmpHostFontFromText`, `RestoreTmpOriginalFont`, `TryWarmUguiGlyphs`, `LoadAssetBundleFromFile`, `TryBeginFallbackBundleAsync`, and `TryCompleteFallbackBundleAsync` in `payloads/UnityIL2CPP/DeepSeekTMPFontFallback/src` | `tests/test_unity_text_rules.ps1`, `tests/test_unity_endpoint_state.ps1`, `tests/UnityEndpointStateProbe`, `tests/launcher_probe.c`, `tests/test_launcher.ps1` |
| Godot deploy/warmup/patch pack | `deploy_godot` in `native/src/launcher/deploy.c`; `warmup_godot` in `native/src/launcher/warmup.c`; private warmup seam in `native/src/launcher/warmup_internal.h`; `warmup_scan_godot_resources`, `scan_godot_resource_dir`, `scan_godot_markdown_strings`, `scan_godot_binary_buffer`, `collect_godot_binary_payload` in `native/src/launcher/godot_warmup.c`; `godot_prepare_patch_pack`, `collect_markdown_text_patches`, `build_godot_runtime_autoload`, `_dst_track_control`, `_dst_scan_controls`, `PROCESS_MODE_ALWAYS`, `godot_prepare_patch_launcher`, `build_runtime_override`, `godot_promote_staged_patch_pack`, `godot_patch_pack_has_runtime_autoload`, and `godot_prepare_runtime_sidecar` in `native/src/launcher/godot_patch.c`; `godot_output_explicitly_rejects_main_pack` in `native/src/launcher/godot_probe.c`; `run_engine_launch_flow`, `godot_runtime_autoload_preflight`, and `godot_main_pack_supported` in `native/src/launcher/ui.c` | `tests/warmup_probe.c`, `tests/godot_patch_probe.c`, `tests/godot_cli_probe.c`, `tests/launcher_probe.c`, `tests/test_launcher.ps1` |
| Warmup HTTP/cache queueing | `LocalHttp`, `post_prefetch_all`, `localhost_get_cached_translate` in `native/src/launcher/warmup.c` | `tests/test_launcher.ps1`, `tests/run_all.ps1 -SkipEndurance` |
| Shared server API/cache | `normalize_translation_result` in `native/src/server/util.c`; `HTTP_CONNECTION_LIMIT`, `ASYNC_QUEUE_BYTE_LIMIT`, `apply_origin_policy`, `ensure_worker_pool`, `async_worker_pool_size`, `live_translate_group`, and `op_batch` in `native/src/server/http.c`; `cache_set_many_persist` and `cache_diag` in `native/src/server/cache.c`; `endpoint_transport_allowed` and `API_MAX_RESPONSE_BYTES` in `native/src/server/api.c` | `tests/test_server.ps1`, `tests/test_complex.ps1`, `tests/test_api_guard.ps1`, `tests/test_cache_export_persistence.ps1`, `tests/test_concurrency.ps1` |
| Failure diagnostics/fallback audit | `api_diag` in `native/src/server/api.c`; `_ds_report_exception` and `reportRpgmError` in `deploy.c`; `ReportCaughtException` in Unity Mono/TMP; `_dst_report_error` in `godot_patch.c` | `tests/test_server.ps1`, `tests/test_launcher.ps1`, `tests/test_unity_text_rules.ps1`, `tests/godot_patch_probe.c`, `docs/FAILURE_TRANSPARENCY.md` |
| Build output name/version and embedded-payload parity | `VERSION`, `build_native.bat`, `scripts/verify_build_artifacts.ps1`, `native/src/launcher/ui.c` | artifact/source/resource hash check in `scripts/verify_build_artifacts.ps1`, version checks in `tests/test_launcher.ps1` |
| Native compiler warning gate | `-Wall -Wextra -Werror` flags in `build_native.bat` | build flag checks in `tests/test_launcher.ps1`, then `build_native.bat` |
| Runtime payload installer | `scripts/install_runtime_payloads.ps1`, `Assert-UnderRoot`, `Assert-NoReparseTraversal`, `docs/RUNTIME_PAYLOADS.md` | installer checks in `tests/test_launcher.ps1` |
| Source-only package | `scripts/prepare_open_source_release.ps1`, `scripts/audit_open_source_release.ps1`, `OPEN_SOURCE_RELEASE.md` | `prepare_open_source_release.ps1 -NoZip` |
| Program package | `scripts/prepare_program_release.ps1` | package smoke with `ds游戏翻译器.exe --sync-payloads-and-exit` |

## Search Shortcuts

```powershell
rg -n "ENGINE_GODOT|deploy_godot|warmup_godot|warmup_scan_godot_resources|godot_prepare_patch_pack|scan_godot" native/src/launcher tests
rg -n "ENGINE_RENPY|deploy_renpy|warmup_renpy|RENPY_HOOK" native/src/launcher tests
rg -n "ENGINE_RPGM|deploy_rpgm|warmup_rpgm|RPGM_HOOK" native/src/launcher tests
rg -n "deploy_unity|deploy_unity_il2cpp|UnityTranslator|DeepSeekUnityFontPatcher|DeepSeekTMPFontFallback|XUnity" native payloads tests
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
cmd /c build_native.bat
powershell -File tests\run_all.ps1 -SkipEndurance
powershell -ExecutionPolicy Bypass -File scripts\prepare_open_source_release.ps1 -Version <version> -NoZip
powershell -ExecutionPolicy Bypass -File scripts\prepare_program_release.ps1 -Version <version>
powershell -File tests\run_all.ps1
```

For program package parity, run the produced standalone exe once with
`--sync-payloads-and-exit` in a temporary directory and check that the local
server, scripts, example config, and first-party Unity plugin DLLs are emitted.
