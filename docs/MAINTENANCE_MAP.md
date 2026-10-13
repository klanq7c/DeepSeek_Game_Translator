# Maintenance Map

开发流程及发布必读：[开发与发布约定](DEVELOPMENT.md)。玩家日志写法、About、
下载附件和验证记录的归属以该文档为准；历史结果放在 `docs/development/`。

程序发布（2026-10-13）：`VERSION` 同时驱动 EXE 元数据、页脚、C# 编译信息和
Godot 预检版本。`prepare_program_release.ps1` 使用 `ds-translator_<版本>` 下载文件名，
包内仍保留中文程序名，并包含 README 引用的 `docs/UPDATE_*.md`、字体许可及清单。
打包后运行 `tests/build_artifacts/test_program_package.py`，检查真实 ZIP 的本地说明链接、
版本下载链接、许可字节、用户数据排除，以及 ZIP/单文件/当前构建的程序哈希一致性。

运行开销优化（2026-10-13）：`ui.c` / `UiPaint.cs` 复用主窗口 UI 线程的绘图缓冲，
只复用存储，每次仍完整绘制脏区；尺寸、DPI、显示设备变化与窗口销毁时释放。
`test_background_buffer.py` 源链接两版缓冲代码，验证 1000 次绘制只分配一次、
扩容、切换所有者、重复释放及八种 GDI 失败后的直接绘制和恢复。
`ui_probe.c` / `UiProbe.cs` 先记录直接绘制，再清除目标脏区，用全量及偏移局部
缓冲绘制恢复，并逐字节比较；它独立于两版画面相互对照。
Unity Mono 的 `RunCacheApplyTicks` 限定三个缓存消费者共享枚举结果的范围，
`InvalidateCacheApplySnapshot` 在作用域结束、重入、场景重置和控件激活时清空。
`tests/unity_runtime_contract` 源链接真实调度和扫描方法，保护枚举次数、下一轮
新对象、被节流的激活、发现过程中场景变化、异常释放、UGUI 独立运行及扫描预算。
相邻守卫继续覆盖 IL2CPP 动画标签与队列/缓存契约，快照不跨帧、不进入共享缓存。

同日回归复查补齐了快照失效边界：`TMPTextOnEnablePostfix` 覆盖实例化/添加组件后
才填入内容的文本；`ScanCachedVisibleObjects` 在命中写入或扫描异常后丢弃列表；
`FlushPendingTranslations` 在每次存活控件的队列写入前后丢弃列表，包括游戏回调
创建新控件后抛错的情况。仅无写入的巡检复用列表。真实调度/扫描/OnEnable/队列写入
代码通过源链接组合测试，原先四个晚一轮发现新文本的失败场景已转绿。
绘图守卫另增加实际 Windows GDI 检查：1000 次偏移重绘、200 次扩容/释放，确认
绘图缓冲释放后资源计数精确回到普通绘制的基线；首次 FillRect 初始化单独预热。

运行开销优化（2026-10-12）：Godot 3/4 的普通控件与菜单项在一次扫描中只创建一次
`_dst_text_plan`，依次交给 `_dst_plan_wanted`、`_dst_queue_plan`、`_dst_render_groups`。
渲染会修改计划的 `parts`，因此必须先按原文排队，且不能跨帧保存该计划。
`test_godot_settings_text.py` 在真实引擎的隔离测试包中计数，保护单次解析、部分命中、
URL/颜色槽与未命中原文；RPG Maker 动画守卫同时检查显示译文不会成为请求键。
`native/src/launcher_cs/Log.cs` 使用有界队列保留最近 2000 条历史，接收端仍收到每条日志；
`tests/launcher_parity/test_log_retention.ps1` 在完整构建中验证 10000 条输出完整且有序。
同轮构建发现 autoload 预检的退出竞争：旧的等待结果可能在读取就绪凭据后过期。
`ui.c` / `GodotLaunch.cs` 在收尾前重新读取进程状态，并将停止失败时已退出的探针
按真实退出结果记录；退出和状态读取失败都会撤销就绪凭据，不写成功缓存。
`test_godot_preflight_exit.py` 源链接两版真实收尾代码，用 8 种确定性进程时序验证；
原有 `lf_godot_autoload_crashed` 集成断言保持不变。

启动器服务关闭（2026-10-07）：`server_proc.c` / `ServerProcess.cs` 的
`wait_for_server_stopped` / `WaitForStopped` 按真实时钟计入 HTTP 耗时。
外部服务接受关闭请求但期限后仍响应时，`stop_server` / `Stop` 保留运行状态。
最近守卫 `tests/launcher_parity/run_server_lifecycle_tests.ps1` 在完整构建中运行，
覆盖 C/C# 五类关闭结果；端口 19999 被占用时明确跳过，真实服务另用 `-ServerSmoke`。

Godot 设置页拆词/部分命中（2026-10-01）：两代脚本的 `_dst_text_plan`、
`_dst_picker_end`、`_dst_query`、`_dst_render_plan` 共用逻辑分组规则，
按原 BBCode 槽分配已有译文并保留点击 URL；`_dst_queue_text` 使用同一查询。
`_dst_path_text` 保护实际路径但允许带斜杠的操作说明，`_dst_numeric_text`
保留数字分辨率。最近守卫是 `tests/payload_scripts/test_godot_settings_text.py`，
相邻守卫是 `test_godot_animated_text.py`、`test_godot_scene_refresh.py` 和
`test_godot_http_contract.py`；均由 payload checker 运行。

Use this file as the first lookup table after `AGENTS.md` and `CONTEXT.md`.
It is intentionally concrete: search these symbols before changing behavior.

> **Note (2026-09-06 / 2026-09-08):** the original `tests/` suites were removed
> at the owner's request on 2026-09-06 after a final full-suite verification
> (15/15 PASS). References to `tests/test_launcher.ps1`, `tests/*_probe.c`,
> `tests/run_all.ps1` etc. below are historical. On 2026-09-08 the language
> migration rebuilt four repo-resident guards; prefer these for new work:
>
> | Guard | Runs | Covers |
> | --- | --- | --- |
> | `tests/core_tests` (`dotnet build` + `dst_core_tests.exe`) | inside `build_native.bat` | `DstCore` golden parity with the C server (`TextRules`, `Json`, `CacheCodec`, `Contract`) |
> | `tests/payload_scripts/check_payload_scripts.ps1` | manual | byte hygiene + syntax of the extracted `.rpy/.js/.gd` payload scripts, RCDATA ID consistency, endpoint routing in both servers |
> | `tests/server_contract/run_contract_tests.ps1 [-Exe <server>]` (default: both binaries) | manual (starts servers on a free port, fake provider) | 74 HTTP contract checks per binary; `-PersistenceOnly` runs the 19 cache recovery/idempotence/restart checks without a provider |
> | `tests/launcher_parity/run_launcher_parity.ps1 [-ServerSmoke]` | inside `build_native.bat` (without smoke) | C vs C# launcher: detection, deploy/restore trees + logs, warmup scan dumps (collected texts/prevs/batch bodies), Godot patch/probe/preflight, one-click launch flow, window rendering (layout report + client-area bitmap), optional server lifecycle |

Per-game read-only QA: `scripts/audit_game_translation.ps1` consumes the C
launcher's `--detect-and-exit` and `--warmup-and-exit` reports plus the
base64 TSV cache. It separates scanned strings, existing XUnity translations,
cached translations, misses, and suspected marker damage; it always leaves
real gameplay/font/prose verification unresolved. `tests/launcher_parity/test_game_audit.ps1`
runs inside `build_native.bat`, using synthetic Ren'Py/RPG Maker/Unity/Godot
directories to check exact game exe reporting plus unchanged game/cache hashes.
The warmup scanners' cap logs
are mirrored in C and C#; the `warm_godot_cap` parity scenario pins that warning.

## Fast Locate

选择动画闪烁（2026-10-01）：Godot `_dst_scene_draw` 使用控件 `draw` 的延迟
连接，在同帧队列内恢复缓存并重绘，命中恢复不会调用 HTTP；同一控件每处理帧
最多两次，`scene-draw-repeat` 报告游戏绘制回写形成的循环。实际引擎守卫为
`tests/payload_scripts/test_godot_animated_text.py`（Godot 3/4，autoload/sidecar）。
Unity Mono 的 `TMPSetTextPrefix` / `UGUITextPrefix` 在去抖前查命中；
`tests/unity_runtime_contract` 源链接这两个 setter，并检查 IL2CPP 标签动画。
RPG Maker `translateConvertedRpgmText` 同时保留原始 `\\` 和 ESC 控制符，
`test_rpgm_runtime_refresh.js` 覆盖逐帧颜色/字号与完整控制符缓存；
`test_renpy_engine_runtime.py` 用实际 Text 重用对象验证动态标签和未命中新选项。

All-module runtime guards (2026-09-30):
`tests/payload_scripts/test_renpy_refresh_thread.py` protects main-thread UI
ownership and positive cache entries against late queued replies.
`test_renpy_engine_runtime.py --runtime <bundled-engine-root>` loads the shipped
hook in an isolated project with the actual Ren'Py Text tokenizer; configure
`RENPY7_RUNTIME` / `RENPY8_RUNTIME` for the payload checker. It tests Text tokens
and lifecycle callbacks, not GPU glyph rendering or commercial gameplay.
`tests/unity_runtime_contract/run_unity_tests.ps1` runs the built XUnity endpoint
against the shipped SDK context interfaces plus source-linked Mono/IL2CPP
renderer methods with simulated native font/property boundaries. Locate
`TmpRichText.TryGetTagEnd`, `TryGetTextureReadability`, `OnExtractTranslation`
and `ParseResponseObject`. The Unity contract is a `build_native.bat` gate.
`TmpRichText.cs` lives in the Mono source tree and is source-linked by both
IL2CPP projects; fingerprint inputs include those links. Set `UNITY_MANAGED_DIR`
and `IL2CPP_INTEROP_DIR` to real references to rebuild all five managed payloads.

Preservation guards (2026-09-30): the Unity runtime contract also covers legacy
response field aliases/maps, source variants, JSON escaping, the disabled
punctuation setting, mixed-CJK restoration, request routes, pending-subset
classification and complete cache-hit detection. Its pass-result
tests use the bundled SDK's actual HTTP/translation contexts and TranslationJob
with a fixture-owned completion callback, checking session-only persistence and
the SDK's explicit empty-result failure. Ren'Py refresh tests preserve positive
translation corrections, the negative-cache TTL and the positive-cache lifetime.

RPG Maker runtime completion/reconnect guards (2026-09-30):
`tests/payload_scripts/test_rpgm_runtime_refresh.js` loads the shipped hook and
drives public MV/MZ-style window methods with a deterministic asynchronous local
HTTP boundary. Its twelve scenarios protect static menu queued completion,
initial disconnect recovery, restart/lost-queue recovery, completed dialogue
awaiting input, no repeated redraw/enqueue on cache misses, scene/page changes,
hidden windows, reused text, retry cooldown and delayed failures across the poll
deadline, plus a faulty plugin window getter alongside a healthy menu. Locate
`scheduleVisibleUiCachePoll`, `noteVisibleUiCacheFailure`,
`requestLookup`, `requestLocalCacheKeys`, `isVisibleMessagePage` and
`scheduleActiveMessageCachePoll`. The payload checker also keeps the adjacent
save display/no disk reload, wait-control/font, Ren'Py queue and Godot guards.
This fixture uses simulated engine classes; it is not a commercial-game playtest.

Server buffer optimization guards (2026-09-16):
`tests/server_buffers/run_buffer_tests.ps1` compiles the real C
`native/src/server/buf.c` + `util.c` and checks JSON byte preservation,
cross-engine markup, and bounded allocation for long UTF-8 runs. It also loads
the built C# server to check `Program.BuildResponse` framing, UTF-8 lengths,
surrogates, and large bodies. `tests/server_contract/cache_persistence`
source-links `CacheStore` and injects write/flush faults to protect batch
confirmation and same-value recovery. Its bounded-load guard also checks that
an over-8-MiB damaged TSV line is consumed without buffering the whole line,
while Ren'Py/RPG Maker/Unity/Godot records after it still load. Both guards run
inside `build_native.bat`.
`tests/server_contract/provider_response` source-links the C# provider reader
and checks the 2 MiB byte limit, false length headers, UTF-8 bytes, and
cancellation. The fake-provider contract also sends valid JSON whose UTF-8
bytes exceed 2 MiB but whose character count does not; both servers must
leave it unresolved and uncached.
`tests/server_contract/test_provider_body_timeout.py` starts both real servers
against a local provider that sends HTTP headers and one body byte, then stalls.
It verifies the configured deadline releases the channel, returns a miss instead
of caching the original, and lets the next request translate and persist.
`build_native.bat` runs this guard after compiling both servers.

XUnity HTTP warmup guards (2026-09-16):
`tests/launcher_parity/run_warmup_http_tests.ps1` runs the real C/C# warmup
against a fake local server. Forty parity scenarios protect 128-line/64-KiB lookup
windows, immediate miss enqueue beyond the final 8,000-item collection limit,
original file bytes and backups, large-response splitting, fault diagnostics,
and RPG Maker isolation; request bodies and ordering must match between ports.
Six additional native allocation-fault scenarios cover request/response,
ASCII/Unicode parsing, file output and cache import. The launcher parity gate
runs it after releasing its Godot fake server. If
port 19999 is occupied, the guard prints a skip and leaves that service alone.
`tests/payload_scripts/test_renpy_poll_batch.py` exercises the shipped Ren'Py
selection and live-loop functions; the payload checker runs its ten cases.

Godot runtime cache FIFO guard (2026-09-24):
`tests/payload_scripts/test_godot_cache_ring.py` executes the actual
`_dst_cache_put` body extracted from both Godot sidecars with small adapters
for GDScript collections. It compares values and FIFO eviction order against
an independent reference through capacity, wraparound, and duplicate updates;
`check_payload_scripts.ps1` runs it. The 4096-slot ring avoids shifting the
entire cache-order array on each new translation after saturation. This guard
does not replace a real Godot 3/4 runtime smoke test.

Godot scene-switch display guard (2026-09-24): both runtime scripts subscribe to
`node_added` before the main scene enters, restore positive cache hits on text
controls, and coalesce `ready` / visibility events after game initialization and
its deferred setters. This avoids waiting for the 80/150-ms scan when a scene
recreates already translated UI. The event path performs no HTTP or cache writes;
ordinary misses keep using the bounded asynchronous scanner. Weak references are
pruned after scene changes; synchronous restoration is capped at 4096 controls
and 4096 items per control, with rate-limited diagnostics and the ordinary scan
retained above those limits. `tests/payload_scripts/test_godot_scene_refresh.py`
builds isolated packs and verifies the final CanvasItem draw state and visible
text across scene reload, parent ready/deferred assignments, 80-item menus,
visibility reuse, freed controls and unresolved text. Set `GODOT3_EXE` /
`GODOT4_EXE` before `check_payload_scripts.ps1` to run both real engines; without
them it explicitly reports the runtime gap and still checks shared lifecycle
ordering/cache-only ownership. `--mode autoload` supports exported engines that
disable the `--script` override; ordinary engines test both sidecar and autoload.
The scene fixture also nests `call_deferred` and `set_deferred`, guarding a
previous first-frame flash. Both payloads make one bounded cache-only restore
on the next scene process frame after their ready-event flush, before drawing;
they report signal-connect failures through the existing rate-limited diagnostic.

| Need to change | Start here | Guard/tests |
| --- | --- | --- |
| Engine enum/name | `native/src/launcher/engine.h`, `engine_name` in `native/src/launcher/engine.c`; C# mirror `Engine`/`EngineDetector.Name` in `native/src/launcher_cs/EngineDetector.cs` | `tests/launcher_parity/run_launcher_parity.ps1` (`engine=`/`engine_id=` must match on every fixture) |
| Engine detection order | `detect_engine`, `rpgm_content_root`, `godot_export_or_project`, `find_subdir_suffix`, `unity_is_il2cpp` in `native/src/launcher/engine.c`; C# mirror `EngineDetector.Detect` etc. Note: `path_join` clears `out` first, so `path_join(p, .., p, ..)` was a latent bug (the `*_Data\il2cpp_data` branch never fired until 2026-09-08); `path_join` now handles `a == out` in place | 44 detection fixtures in `tests/launcher_parity/run_launcher_parity.ps1` (flat RPG Maker, NW.js false positive, Unity-with-`.pck`, `il2cpp_data` without `GameAssembly.dll`, embedded-pck Godot exe, exe scoring/tie-break) |
| Launcher diagnostic modes (no window) | `run_diagnostic_from_cmd`, `write_detect_report` in `native/src/launcher/main.c`; `g_log_to_stdout`, `write_stdout_utf8`, exported `deploy_for_engine` in `native/src/launcher/ui.c`; `g_warmup_dump_stdout`/`warmup_dump_post` in `native/src/launcher/warmup.c` (`--warmup-and-exit`: scanners run for real, every batch body is printed as `post=<path> <json>` instead of POSTed, cache lookup forced to miss, no file rewrite); `--sync-payloads-and-exit` in `main.c` now also sets `g_log_to_stdout` and prints `result=0|5`; `--godot-patch-and-exit` / `--godot-promote-and-exit` / `--godot-launcher-and-exit` print `pack=`/`launcher=` before `result=`; `--launch-flow-and-exit` runs `run_engine_launch_flow` with `g_launch_dry_run` (the three real process starts become `spawn=<kind>|<exe>|<cmd>|<cwd>`, warmup dumps `post=` lines, `set_status` mirrors `status=` lines) and `--clear-cache-and-exit` prints `cache=`/`cache2=` around `clear_cache_file`; `run_config_diagnostic_from_cmd` in `main.c` (`--api-config-and-exit` preset table + loaded values + `selected=`, `--api-config-set-and-exit <e> <m> <k>`, `--godot-probe-and-exit <text>...` ⇒ `reject<i>=`, `--godot-preflight-and-exit <kind> <file> <text> <put>` ⇒ `sig=`/`get=`/`get2=`); `run_ui_probe_from_cmd` in `main.c` + `native/src/launcher/ui_probe.c` (`--ui-probe-and-exit <w> <h> <alive> <bmp>` ⇒ layout report on stdout + a 32bpp BMP, `--ui-identity-and-exit` ⇒ the real runtime tag/subtitle); C# side `Program.cs` (`--detect`, `--deploy`, `--restore`, `--warmup`, `--sync-payloads`, `--godot-patch`, `--godot-promote`, `--godot-launcher`, `--launch-flow`, `--clear-cache`, `--api-config`, `--api-config-set`, `--godot-probe`, `--godot-preflight`, `--ui-probe`, `--ui-identity`, `--server-smoke`, `--godot-patch-worker`, `--root`), `Warmup.DumpSink`, `LaunchFlow.SpawnSink`, `Log.SetStatusSink`. `--root` is stripped in a pre-pass because the variable-argument modes consume everything after their flag. The C# launcher is a `WinExe`, so `Program.Main` calls `AttachConsole(ATTACH_PARENT_PROCESS)` before any diagnostic writes; with no diagnostic flag it falls through to `MainWindow.Run()` | `tests/launcher_parity/run_launcher_parity.ps1`; usage error must exit 2 on both |
| C# launcher port (language migration phase 3, complete) | `native/src/launcher_cs/launcher_cs.csproj` (net472 `WinExe`, `ApplicationIcon` = the same `assets/app_icon.ico` windres embeds; scripts embedded as `EmbeddedResource` with RCDATA macro names), `PathUtil` (`path_join` semantics, Win32 `FindFirstFileW` enumeration), `SafeFs` (`fsutil.c` port: reparse-point fail-closed checks, `EnsureDir`, `WriteBytes`, `CopyFileSafe`, `MoveFileSafe`, `DeleteFileSafe`, `CopyTreeSafe`, `FileSizeAtMost`, `LastError`; `Win32Find.Entry.Size`), `Log` (sink for `append_log`), `EmbeddedScripts`, `Deploy` (partial class: `Deploy.cs` Ren'Py/RPGM/Godot + `Restore`, `DeployUnity.cs` Unity Mono/IL2CPP deploy + restore, PE machine probe, `globalgamemanagers` version scan, byte-level INI editing), `ServerProcess` (`server_proc.c` port incl. `launcher.ini [server] binary`), `Warmup` (partial class for all five engines, see "Warmup scanners" below), `SelfUpdate` (`self_update.c` port: same 11-entry payload table in the same order), `FileMatchesBytes` 64 KiB streaming compare, `WriteBytesAtomic` `.dstmp` + `MoveFileSafe(REPLACE_EXISTING|COPY_ALLOWED|WRITE_THROUGH)`, identical English log wording; payloads are `EmbeddedResource` items in the csproj with the RCDATA macro names as `LogicalName`, `Condition="Exists(...)"` so a missing optional Unity assembly does not break `dotnet build` while `EmbeddedScripts.BytesOrNull` still reports it missing at run time), `GodotPatch` (partial class over `GodotPatch.cs`/`GodotPatchResources.cs`/`GodotPatchPack.cs`, see "Godot deploy/warmup/patch pack" below), `ApiConfig` (`api_config.c` data layer, see "API configuration" below), `GodotProbe`/`GodotPreflightCache` (`GodotPreflight.cs`, see "Godot launch preflight" below), `LaunchFlow`/`GodotLaunch` (`ui.c` one-click flow and Godot launch decisions, see "One-click launch flow" below), `Win32`/`Theme`/`UiPaint`/`UiButtons`/`UiLayout`/`MainWindow`/`ApiConfigDialog`/`FolderPicker`/`LauncherConfig`/`UiProbe` (`ui.c` + `main.c` window and drawing layer, see "Launcher visual system" below). No C-only launcher modules remain | `dotnet build` + `tests/launcher_parity` gate at the end of `build_native.bat` (195 checks as of 2026-09-09: detection fixtures, usage error, deploy/restore steps, hook-byte identity, 22 warmup scan scenarios, 4 `sync-payloads` checks: released trees + stdout identical, second run rewrites nothing and leaves a user `api.ini` alone, tampered/truncated targets restored, directory-blocked target ⇒ exit 5 on both, 9 `godot-patch` checks against a fake `/batch` server, 3 `api-config` checks incl. byte-identical `api.ini`, 1 `godot-probe` classifier check over 8 outputs, 1 `godot-preflight` cache check incl. byte-identical `godot_preflight.ini`, 8 `launch-flow` scenarios + 8 branch-marker checks, 1 `clear-cache` check, 4 `ui` render scenarios comparing the layout report and a pixel-identical client-area bitmap + 1 runtime-identity check); `-ServerSmoke` for the server lifecycle (needs port 19999 free); `-ShowLogs` prints per-step log lines and dumped `post=` bodies |
| Warmup scanners (C ↔ C# byte parity) | `native/src/launcher/warmup.c` ↔ `native/src/launcher_cs/Warmup.cs` (byte-string convention: every `string` char is one `char*` byte, so `Length`/`IndexOf`/`Contains` are `strlen`/`strchr`/`strstr`; `ByteStr.CStr` truncates at the first NUL like a C string; `TextList`/`PairList` dedup + limits; `ShouldWarmText`, `HasCjkUtf8`, `JsonStringAt`, `Atoi`, `AppendJson`, `BuildPrefetchBody`/`BuildImportBody`, `LocalHttp`), `WarmupRenPy.cs` (`RenpyStringAt`, `RenpySkipStatement` on the untrimmed line, `Prev` per file, 16-literal cap, `*.rpy` enumeration: no `.rpy` in a directory ⇒ its subdirectories are not entered — inherited from C, keep both sides in step if you change it), `WarmupRpgm.cs` (`ShouldWarmRpgmText` control codes, `DupRpgmPrefixedBody`, `RpgmMessageBlock` 401 merging, `MapNNN.json` name skip, TXT/CSV external texts, `RpgmCsvDelimiter`/header scoring), `WarmupUnity.cs` (XUnity `ParseTranslationFile` import/identity/empty split + cached rewrite with `BackupOnce`, `ScanUnityAssetFile` u32-prefixed ASCII, `ScanUnityBundleFile` streaming runs + `CollectUnityBundleSegment` residue stripping, `LooksLikeUnityBundleText`), `native/src/launcher/godot_warmup.c` ↔ `WarmupGodot.cs` (`ShouldWarmGodotText` + targeted/free/binary collectors, quote classification `GodotTranslatableQuote`/`GodotMetadataQuote`/`GodotCallNameBeforeQuote`, `ForEachGodotLine` shared line loop, PO `msgid`/`msgid_plural` continuation, CSV `GodotCsvSourceColumn` header scoring + short-id fallback, Markdown fences/headings/BBCode-wrapped lines, `ValidUtf8TextPayload` + `ScanGodotBinaryBuffer` + `CollectGodotBinaryPayload` tag splitting, `.translation` locale rule `GodotPackTranslationPath` (only `en`/`en_*`/`en-*` suffixes), PCK formats 1/2/3 `ReadGodotPckInfo`/`ScanGodotPckAt` two-pass (deferred `.scn/.res/.gdc/.gde`), directory-parse failure ⇒ whole-file raw scan, `FindPePckSection` embedded EXE, skip dirs `.godot/.import/addons/export_presets/saves/save`, launcher-generated file names, depth 10, 12000 cap). C fix 2026-09-09: `parse_rpgm_json_file` stepped one byte past the NUL terminator when the file ended in whitespace | `warm_*` scenarios in `tests/launcher_parity/run_launcher_parity.ps1` (Ren'Py dialogue/escapes/prevs/cap/recursion/size/NUL/CRLF, RPGM events+database/prefix codes/external TXT+CSV/flat layout/limits, Unity XUnity files + assets/bundles + IL2CPP second `_Data`, Godot project text/PO+CSV+MD/PCK v1/v2+v3+fallback/embedded EXE/cap, no-op engines; `New-GodotPck`/`New-GodotEmbeddedExe` build the packs); each scenario also asserts the tree is unchanged after both dumps |
| Start button flow | `start_translation`, `warmup_launch_thread` in `native/src/launcher/ui.c`; C# mirror `MainWindow.StartTranslation`/`WarmupLaunchThread` (STA worker thread because `LaunchFlow` uses `ShellExecuteW`) | the sequence after the server is ready is covered by the `launch-flow` scenarios in `tests/launcher_parity`; the button wiring itself by the `ui/*` render scenarios |
| Cross-engine one-click failure gate | `run_deploy_launch_flow` / `run_engine_launch_flow` / `launch_game_with_params` in `native/src/launcher/ui.c`, mirrored by `LaunchFlow.RunAfterDeploy` / `RunEngineLaunchFlow` / `LaunchGameWithParams` in `native/src/launcher_cs/LaunchFlow.cs`: a supported engine's failed deployment or failed game process start returns failure and cannot end in the normal success status. Successful Ren'Py/RPG Maker/Unity deployment is reported as deployed, with in-game loading still unconfirmed. Unknown and legacy RPG Maker engines explicitly report that no live hook was deployed. Unity Mono `UnityTranslator.dll.dst-owned` records the exact deployed bytes; IL2CPP switching and restore compare it with the live file before disabling/removing an older launcher-owned DLL, preserving modified third-party files. | `deploy-launch-flow/*`, `renpy_refresh_stale_hook`, `rpgm_refresh_stale_hook`, `unity_*old_owned_snapshot*`, and modified-plugin scenarios in `tests/launcher_parity/run_launcher_parity.ps1`; C/C# stdout, result, spawn plans, and game trees must match |
| Unity IL2CPP disabled-file collision | `disable_existing_file` in `native/src/launcher/deploy.c` and `DeployUnity.DisableExistingFile` in `native/src/launcher_cs/DeployUnity.cs` refuse an existing `.disabled` target and use a non-replacing move, so a user backup cannot be overwritten. A conflicting Mono DLL or `Il2Cppmscorlib.dll` aborts deployment and the one-click launch. PDB files are not needed at runtime: deployment leaves them alone, and restore preserves any historical `.pdb.disabled` file whose owner cannot be proved. | `unity_il2cpp_*disabled_collision` deploy/restore scenarios and `deploy-launch-flow/unity_il2cpp_disabled_collision` in `tests/launcher_parity/run_launcher_parity.ps1`; assert original bytes survive, C/C# parity holds, and no game spawn occurs on a deployment conflict |
| Unity Mono one-time backup safety | `backup_unity_current_if_safe` in `native/src/launcher/deploy.c` and `DeployUnity.BackupUnityCurrentIfSafe` in `native/src/launcher_cs/DeployUnity.cs` refuse to overwrite a plugin/dependency file if its existing `.dst-backup` differs from the live file. `UnityTranslator.dll.dst-owned` and `Newtonsoft.Json.dll.dst-owned` identify exact launcher-deployed bytes across payload upgrades; restore compares the snapshot before replacing either file with its user backup. | `unity_mono_second_*_edit_with_backup`, `unity_mono_matching_backups_allow_deploy`, `unity_mono_upgrade_owned_dependency`, and `deploy-launch-flow/unity_mono_backup_conflict` in `tests/launcher_parity/run_launcher_parity.ps1`; verify file bytes and C/C# tree/log parity |
| One-click launch flow (engine ordering, Godot launch decisions) | `run_engine_launch_flow`, `launch_game_for_engine`, `launch_game_with_params`, `launch_godot_with_pack`, `launch_godot_export_with_runtime_sidecar`, `launch_godot_with_runtime_sidecar`, `start_godot_patch_worker`, `godot_runtime_sidecar_preflight`, `godot_runtime_autoload_preflight`, `godot_main_pack_supported`, `cache_size_text`, `clear_cache_file` in `native/src/launcher/ui.c`; C# mirror `native/src/launcher_cs/LaunchFlow.cs` (game launch, engine ordering, cache card/clear) and `GodotLaunch.cs` (three preflight probes, three translated launch paths, patch worker). Ordering is engine-specific and load-bearing: Ren'Py launches first and warms afterwards, Unity/XUnity imports first, Godot launches, warms, then detaches the patch refresh worker. `LaunchFlow.CmdCap` mirrors `WCHAR cmd[MAX_PATH * 12]` and `FormatChecked` refuses a truncated command line exactly like `wide_format_checked` | 8 `launch-flow` scenarios in `tests/launcher_parity/run_launcher_parity.ps1` (Ren'Py, RPG Maker, unknown engine, Godot loose project accepted/rejected, Godot export with and without a pack, `--main-pack` explicitly rejected). The preflight probes really start a stub console app whose stdout/exit code come from `dst_fake_godot.ini` beside it and which appends its own command line to `<dir>.calls.txt` (outside the scanned game directory), so probe arguments are compared too; plus per-scenario branch-marker assertions |
| Restore-game button/ownership rules | `restore_selected_game` in `native/src/launcher/ui.c` (C# mirror `MainWindow.RestoreSelectedGame`); `restore_game`, `restore_remove_verified_payload_files`, and `restore_*` helpers in `native/src/launcher/deploy.c` (C# mirror `Deploy.Restore`) | restore steps per engine in `tests/launcher_parity/run_launcher_parity.ps1`; historical: `tests/restore_probe.c`, restore source checks in `tests/test_launcher.ps1` |
| Filesystem mutation/reparse/path safety | `path_has_reparse_point`, `path_append_suffix`, `wide_format_checked`, `copy_file_safe`, `files_equal_streaming`, `copy_tree_safe`, `move_file_safe`, and `delete_file_safe` in `native/src/launcher/fsutil.c`; `Assert-NoReparseTraversal` in `scripts/install_runtime_payloads.ps1`; `Assert-Under` in both release scripts | path/command truncation, junction write/restore, and locked identical payload redeploy fixtures in `tests/launcher_probe.c`, `tests/restore_probe.c`, and `tests/test_launcher.ps1` |
| Cache card/clear-cache flow | `update_cache_card`, `clear_translation_cache` in `native/src/launcher/ui.c` (C# mirror `MainWindow.UpdateCacheCard`/`ClearTranslationCache`; the non-UI parts are `cache_size_text` and `clear_cache_file` ↔ `LaunchFlow.CacheSizeText`/`ClearCacheFile`/`CacheFileReadable`); shutdown readiness in `native/src/launcher/server_proc.c` | `clear-cache/core` in `tests/launcher_parity/run_launcher_parity.ps1` (missing file counts as cleared, a 1.5 MiB file is deleted, a directory in its place is preserved with the same diagnostic); historical: cache-clear source checks in `tests/test_launcher.ps1` |
| Launcher visual system | palette in `native/src/launcher/globals.c` ↔ `native/src/launcher_cs/Theme.cs` (same RGB triples, `Sc` calls `kernel32!MulDiv` so rounding cannot drift, `Mix` keeps C's `float` math and `(int)` truncation); `apply_window_chrome`, `draw_nocturne_sky`, `draw_panel_shell`, `draw_hero_data_line`, `paint_background`, `card_text_brush` in `native/src/launcher/ui.c` ↔ `UiPaint.cs`; `draw_button`, `draw_button_icon`, `install_button_hover_tracking`, the hover easing in `tick_ui_animation` ↔ `UiButtons.cs`; `compute_layout`/`layout`/`tick_ui_animation` ↔ `UiLayout.cs`; `wndproc`/`wWinMain` in `native/src/launcher/main.c` plus `append_log`/`set_status`/`update_cache_card`/`refresh_engine`/`apply_fonts`/`browse_folder`/`start_translation`/`restore_selected_game`/`clear_translation_cache` ↔ `MainWindow.cs`; `show_api_config`/`api_wndproc` in `api_config.c` ↔ `ApiConfigDialog.cs`; `SHBrowseForFolderW` ↔ `FolderPicker.cs`; `save/load_last_game_dir` ↔ `LauncherConfig.cs`. All drawing goes through raw GDI on both sides — no `System.Drawing`/WinForms, because any managed redraw would change anti-aliasing and rounding. Deliberate divergence: the log-card runtime tag names the implementation (`C native runtime` vs `C# managed runtime`), exposed by `--ui-identity*` and neutralised during the pixel diff | `ui/*` scenarios in `tests/launcher_parity/run_launcher_parity.ps1` (`--ui-probe-and-exit` vs `--ui-probe`: DPI pinned to 96, animation clock frozen at 0/7500/15000/29999ms, server state from an argument, invisible `WS_POPUP` window rendered background-plus-children into one 32bpp BMP; the layout report and the bitmap must both match byte for byte at 1200×780/1080×700/1600×1000/1064×661 and both server states), plus `--ui-identity*`; then desktop/minimum-window screenshots for anything the probe cannot see (real fonts at the user's DPI, DWM chrome) |
| Server lifetime/readiness | `native/src/launcher/server_proc.c` | `tests/test_launcher.ps1`, `tests/run_all.ps1 -SkipEndurance` |
| Embedded engine scripts (extracted 2026-09-08) | `payloads/RenPy/iron_deepseek.rpy` (RCDATA 301), `payloads/RPGMaker/hook_rpgm_mv.js` (302), `payloads/Godot/dst_godot_runtime_g3.gd` (303), `payloads/Godot/dst_godot_runtime_g4.gd` (304); IDs in `native/src/launcher/resource.h`; `embedded_resource_bytes`/`embedded_script_text` in `native/src/launcher/embedded.c`; RCDATA lines in `build_native.bat`; `.gitattributes` forces LF | `tests/payload_scripts/check_payload_scripts.ps1` (UTF-8 no BOM, LF, trailing newline, Python 2-compatible AST, `node --check`, GDScript anchors, endpoint routing), `Assert-EmbeddedResourceMatches` in `scripts/verify_build_artifacts.ps1`, deployed-bytes check in `tests/launcher_parity` |
| Ren'Py deploy | `deploy_renpy`, `deploy_renpy_font`, `remove_stale_renpy_hook_bytecode` in `native/src/launcher/deploy.c` (hook text from `embedded_script_text(IDR_SCRIPT_RENPY_HOOK)`); C# mirror `Deploy.RenPy` | Ren'Py deploy/restore scenarios in `tests/launcher_parity` (stale read-only `.rpyc`, `.rpyc` directory, font already present, idempotent redeploy) |
| RPG Maker deploy/runtime | `rpgm_content_root` in `native/src/launcher/engine.c`; `deploy_rpgm`, `deploy_rpgm_font`, `strip_owned_rpgm_hook_tags`, `backup_file_once`, `write_file_bytes_atomic` in `native/src/launcher/deploy.c` (hook text from `embedded_script_text(IDR_SCRIPT_RPGM_HOOK)`; C# mirror `Deploy.Rpgm`/`StripOwnedRpgmHookTags`/`CStr`); hook internals `planMessagePage`, `safeRpgmTranslation`, `hasSuspiciousRpgmSourceResidue`, `requestLocalCacheKeys`, `trDisplay`, `trBitmap`, `refreshActiveRpgmMessage`, `refreshPersistenceDisplays`, `schedulePersistenceCachePoll`, `walkSceneWindows`, `walkSceneOwnedWindows`, `markRpgmDisplayTarget`, `canAutoRefreshWindow`, `isPersistenceScene`, `installMessageRuntimeHooks`, `installAllRpgmHooks`, `installTopLevelWindowHooks`, `attachPluginScriptLoadHook`, and `installPluginLoadHook` in `payloads/RPGMaker/hook_rpgm_mv.js`; `warmup_scan_rpgm_resources` in `native/src/launcher/warmup.c` | `node --check` and `test_rpgm_save_display.js` in `tests/payload_scripts`; www/flat, already-hooked, self-closing/uppercase tag, NUL byte, read-only index, backup-only restore scenarios in `tests/launcher_parity` (historical: `tests/rpgm_hook_probe.js`) |

RPG Maker plugin parameter warmup (2026-09-27): `scan_rpgm_plugin_parameters`
in `native/src/launcher/warmup.c` and `Warmup.ScanRpgmPluginParameters` in
`native/src/launcher_cs/WarmupRpgm.cs` read only generated `js/plugins.js`
under www/ or a flat MZ root. Enabled plugin UI strings are scanned before
external TXT/CSV and `data/*.json` so common save/help labels receive priority
under the 40,000-item cap. `warm_rpgm_plugin_ui_parameters`,
`warm_rpgm_bad_plugin_manifest`, `warm_rpgm_flat_and_limits`, and
`warm_renpy_ignores_rpgm_plugins` in launcher parity guard selection,
diagnostics, C/C# byte parity, and cross-engine isolation.

Ren'Py/RPG Maker font ownership (2026-09-26): `deploy_renpy_font` and
`deploy_rpgm_font` copy system fonts only when no `ds_font.ttf/.ttc/.otf`
exists, and record a small `.dst-owned` marker. `restore_owned_cjk_font`
removes only unchanged, marked copies. C# mirrors are in `Deploy.cs`;
`renpy_*font*` and `rpgm_user_*font*` launcher parity scenarios protect user
fonts, including OTF. The RPG Maker hook registers OTF alongside TTF/TTC.

RPG Maker MV late-plugin/font guard (2026-09-25): `tests/payload_scripts/test_rpgm_boot_font.js`
loads the shipped hook, replaces message methods as YEP plugins can, fires DOM/load
lifecycle events, and checks that a cached Between message displays in Chinese.
It also checks that a provider-omitted terminal `\.` wait command is restored,
other missing control commands (including YEP's bare `\fi` italic toggle) are
rejected, CJK draw/width use the fallback font, and subsequent Latin text
retains the game's font. The hook owns this
repair; shared translation memory and Ren'Py/Unity/Godot renderers remain unchanged.

RPG Maker message-page no-blank guard (2026-09-25): the same Node guard now
covers Between's cached two-line page with a missing `\.` wait code, an
outstanding local-cache request, and a translated result arriving while the
window is paused or while the page is finished and waiting for input. A missing
wait code is restored at the translated text end
only when no other control command needs repair; this can shift wait timing and
is recorded by `restore-inline-wait-control`. Missing cache data displays the
original immediately. A paused or waiting page keeps its visible text until
`Window_Message.update` can resume; an active page repaints synchronously after
restart so its contents do not remain blank for a frame. A completed page also
repaints synchronously and skips its already played waits before restoring the
input pause. Other missing markup
is still rejected and cannot create a successful cache entry.

| Unity Mono deploy/runtime | `unity_player_machine`, `ensure_bepinex_mono`, `repair_existing_bepinex_mono_runtime`, `unity_mscorlib_is_clearly_stripped`, `ensure_stripped_unity_mono_support`, `update_stripped_ini_setting`, `install_stripped_unity_font_patcher`, `deploy_unity`, `restore_unity_mono` in `deploy.c`; C# mirror `Deploy.Unity`/`RestoreUnityMono` and helpers in `native/src/launcher_cs/DeployUnity.cs`; `DeepSeekUnityFontPatcher` in `payloads/UnityTranslator/src/FontPatcher`; `DriverUpdate`, host `Update`, `TMPSetTextAnyPrefix`/`Postfix`/`Finalizer`, `RegisterOptionalStaticEvent`, `GetUnityVersionSafe`, `CreateDynamicFontFromOSFontSafe`, `PinRuntimeFont`, `HideTmpSourceForOverlay`, `RestoreTmpSourceVisibility`, `FindObjectsOfTypeAllSafe`, `DiscoverTmpUpdateManagerRegistry`, `ResolveAssetBundleTypeSafe`, `WaitForScaledSecondsSafe`, `GetComponentsInChildrenByTypeSafe`, `TryMeasureImGuiContentSafe`, `InstallFungusHooks`, `QueueFungusLookahead`, `PrefetchServerTextsAsync`, `StripTranslationPromptEchoPrefix`, `LoadServerCache`, `TryIsolateOversizedLocalCacheLocked`, `ImportServerCacheEntries`, `ScheduleAsyncApply`, `UnityLongTextPlanner`, `TryQueueLongTextTranslation`, `CompleteLongTextSegment`, and `PersistLocalCacheAsync` in `payloads/UnityTranslator/src` | `unity_mono_*` / `unity6_mono_*` deploy+restore scenarios in `tests/launcher_parity/run_launcher_parity.ps1` (BepInEx 5/6 install, x86/x64, partial-BepInEx repair, wrong loader arch, Unity 6 upgrade, unsupported machine, stripped corlib + INI edits + font patcher, ownership conflicts, plugin/Newtonsoft backups; synthetic payload root); historical: `tests/launcher_probe.c`, `tests/restore_probe.c`, `tests/test_unity_text_rules.ps1`, `tests/test_launcher.ps1` |
| Unity Mono visible-cache scan | `ApplyCachedVisibleTextPass` in `payloads/UnityTranslator/src/DeepSeekTranslator.cs` keeps scene-load, immediate UI, and new-cache-generation scans complete. Follow-up and idle periodic scans use `VisibleScanWindow` to cap component inspection and rotate through cache misses; TMP and UGUI have separate cursors. Unity object discovery still enumerates the current renderer objects on each pass. | `tests/unity_visible_scan` (6 source-linked scheduling checks, run by `build_native.bat`), plus BepInEx 5/6 compilation against available Unity Managed references. |
| Unity IL2CPP/XUnity deploy/runtime | `deploy_unity_il2cpp`, `write_xunity_config`, `migrate_xunity_owned_ini_setting`, `restore_unity_il2cpp`, `restore_xunity_plugin`, `restore_remove_owned_tree` in `deploy.c`; C# mirror `Deploy.UnityIl2cpp`/`RestoreUnityIl2cpp` in `native/src/launcher_cs/DeployUnity.cs`; `TrimSlash`, `StripTranslationPromptEchoPrefix`, `ProtectMixedCjkForRequest`, `TryRestoreMixedCjk`, `IsPassSource`, `TryCollectPendingTexts`, `TryKeepIdentityResultsSessionOnly`, `HasOnlyResolvedOrPassSources`, and `OnExtractTranslation` in `payloads/UnityIL2CPP/DeepSeekXUnityTranslator/src`; `ShouldUseFastNormalizeScan`, `ShouldDeferTextSetterPatchForFreshInterop`, `ScheduleTextSetterPatchRetry`, `PatchLoadedFontAssets`, `AddToListProperty`, `TryWarmFallbackGlyphs`, `PatchTmpHostFontFromText`, `RestoreTmpOriginalFont`, `TryWarmUguiGlyphs`, `LoadAssetBundleFromFile`, `TryBeginFallbackBundleAsync`, and `TryCompleteFallbackBundleAsync` in `payloads/UnityIL2CPP/DeepSeekTMPFontFallback/src` | `unity_il2cpp_*` deploy+restore scenarios in `tests/launcher_parity/run_launcher_parity.ps1` (x64/x86/unknown PE machine, bundled vs foreign Mono plugin disabling, Il2Cppmscorlib disable/restore, config write/backup, owned migration, user-modified preservation, marker-gated XUnity tree removal); `tests/core_tests` for `TextRules` parity; historical: `tests/test_unity_text_rules.ps1`, `tests/test_unity_endpoint_state.ps1`, `tests/UnityEndpointStateProbe`, `tests/launcher_probe.c`, `tests/test_launcher.ps1` |
| Godot deploy/warmup/patch pack | `deploy_godot` in `native/src/launcher/deploy.c`; `warmup_godot` in `native/src/launcher/warmup.c`; private warmup seam in `native/src/launcher/warmup_internal.h`; `warmup_scan_godot_resources`, `scan_godot_resource_dir`, `scan_godot_markdown_strings`, `scan_godot_binary_buffer`, `collect_godot_binary_payload` in `native/src/launcher/godot_warmup.c` (C# mirror `native/src/launcher_cs/WarmupGodot.cs`, same function names in PascalCase); `godot_prepare_patch_pack`, `collect_markdown_text_patches`, `build_godot_runtime_autoload`, `_dst_track_control`, `_dst_scan_controls`, `PROCESS_MODE_ALWAYS`, `godot_prepare_patch_launcher`, `build_runtime_override`, `godot_promote_staged_patch_pack`, `godot_patch_pack_has_runtime_autoload`, and `godot_prepare_runtime_sidecar` in `native/src/launcher/godot_patch.c` (C# mirror split by layer: `GodotPatch.cs` = PCK header/IO, path classifiers + `godot_patch_entry_priority`, text filters, JSON, Smaz table, `PatchHttp` `/batch`; `GodotPatchResources.cs` = text/scene/markdown spans, GDSC v13 constant pool, OptimizedTranslation heap rebuild; `GodotPatchPack.cs` = source discovery, `PatchPackFile`, format 3 runtime/autoload/`override.cfg` embedding, loose overlay, the seven public entry points); `godot_output_explicitly_rejects_main_pack` in `native/src/launcher/godot_probe.c`; `run_engine_launch_flow`, `godot_runtime_autoload_preflight`, and `godot_main_pack_supported` in `native/src/launcher/ui.c` | `godot-patch/*` scenarios in `tests/launcher_parity/run_launcher_parity.ps1` (fake deterministic `/batch` server on 127.0.0.1:19999; compares exit code, normalised stdout, byte-identical output trees, and the exact `/batch` request bodies; skipped loudly when the port is taken); historical: `tests/warmup_probe.c`, `tests/godot_patch_probe.c`, `tests/godot_cli_probe.c`, `tests/launcher_probe.c`, `tests/test_launcher.ps1` |
| Warmup HTTP/cache queueing | `LocalHttp`, `post_prefetch_all`, `localhost_get_cached_translate` in `native/src/launcher/warmup.c`; C# mirror `LocalHttp` (`HttpWebRequest`, 2xx-only acceptance, one rejection log per session, `/health` 200 gate) and `Warmup.PostPrefetchAll` in `native/src/launcher_cs/Warmup.cs` | batch bodies compared by the `warm_*` scenarios in `tests/launcher_parity`; historical: `tests/test_launcher.ps1`, `tests/run_all.ps1 -SkipEndurance` |
| Context-aware warmup prompt & optional glossary | `prevs` field on `AsyncJob` and `op_prefetch` in `native/src/server/http.c`; `api_translate_batch_ctx`, `api_glossary_load_file`, `build_ctx_request` in `native/src/server/api.c`; `default_glossary_path` and `--glossary` in `native/src/server/main.c`; `prev_items` on `TextList` and the Ren'Py line-adjacency tracking in `scan_renpy_script_dir` in `native/src/launcher/warmup.c`; `config/glossary.example.tsv` | `tests/test_context_glossary.ps1` |
| Shared server API/cache | `normalize_translation_result` in `native/src/server/util.c`; `HTTP_CONNECTION_LIMIT`, `ASYNC_QUEUE_BYTE_LIMIT`, `apply_origin_policy`, `ensure_worker_pool`, `async_worker_pool_size`, `live_translate_group`, and `op_batch` in `native/src/server/http.c`; `cache_set_many_persist` and `cache_diag` in `native/src/server/cache.c`; `endpoint_transport_allowed` and `API_MAX_RESPONSE_BYTES` in `native/src/server/api.c` | `tests/test_server.ps1`, `tests/test_complex.ps1`, `tests/test_api_guard.ps1`, `tests/test_cache_export_persistence.ps1`, `tests/test_concurrency.ps1` |
| Shared C# core `DstCore` (language migration phase 1) | `native/src/core/TextRules.cs` (`NormalizeTranslationResult`, `StripPromptEcho`, `HasCjk`), `Json.cs` (strict parser + escaping, `MaxDepth`), `CacheCodec.cs` (`NormalizeForStore`, `EncodeLine`/`AppendLine`/`TryDecodeLine` for the TSV+base64 cache), `Contract.cs` (`DefaultPort`, `Path*`, `Field*`, `Source*`, `IsResolvedSource`/`IsPendingSource`); source-linked by `native/src/server_cs/server_cs.csproj`, `payloads/UnityIL2CPP/DeepSeekXUnityTranslator/src/*.csproj` (TextRules + Contract only), `native/src/launcher_cs/launcher_cs.csproj`, `tests/core_tests/core_tests.csproj`. The XUnity payload fingerprint (`scripts/verify_build_artifacts.ps1` `Get-LinkedCompileSources`) includes the linked core files | `tests/core_tests` (100 golden checks, run by `build_native.bat`); C parity source: `normalize_translation_result` in `native/src/server/util.c`, `native/src/server/json.c`, `native/src/server/cache.c` |
| C# parallel server (language migration phase 2) | `native/src/server_cs/server_cs.csproj`, `Program.cs` (routes/queues/origin/limits), `Core.cs` (`CacheStore` over `CacheCodec`, tracks memory-only values until a successful batch append and flush), `Api.cs` (api.ini via GetPrivateProfileStringA, prompts, glossary); `Json`/`TextRules` now live in `native/src/core`; built by `build_native.bat` before resource generation and embedded in the launcher as RCDATA 105 (`IDR_PAYLOAD_DST_SERVER_CS`, synced by `self_update.c`, packaged by `scripts/prepare_program_release.ps1`); selected via `launcher.ini [server] binary` | `tests/server_contract/run_contract_tests.ps1` (fake OpenAI-compatible provider; 74 checks per binary, both by default); freshness + embedded-hash checks in `scripts/verify_build_artifacts.ps1` |
| C# realtime batch persistence | `LiveBatchPersistence.Persist` in `native/src/server_cs/LiveBatchPersistence.cs` filters resolved slots and calls `CacheStore.SetManyPersist` once per provider batch; `Program.LiveTranslatePending` then completes or queues each slot | `tests/server_contract/cache_persistence` observes one explicit Flush for a 48-slot batch with Ren'Py/RPG Maker/Unity/Godot markup and rejected echoes; `tests/server_contract/run_contract_tests.ps1` reloads all 40 concurrent live values after restart on both servers |
| C# server ↔ C server parity points (2026-09-08 audit) | `AsyncFinishJobs` (mirrors `async_finish_jobs`: byte budget + `asyncKnown` release after every batch), `LivePopBatch` (Monitor-based coalescing mirroring `live_pop_batch`; foreground marked in `LiveTranslateBatched` like `live_translate_batched`), `TopStringList` (mirrors `json_array_value`: any non-string element → empty list → 400), `TextRules.StripPromptEcho`/`PrefixEqualAt`/`IsAsciiSpace` (mirror `translation_after_prompt_prefix`: separator + non-empty remainder required, ASCII-only case folding/whitespace), `TranslateValueAsync`/`RespBatchAsync`/`RouteAsync`/`SendAsync` (no sync-over-async on the thread pool), `ReadWithTimeoutAsync` (net472 recv timeout → `400 incomplete_request` + `Shutdown(Send)` before `Close`, mirroring `serve_one`), pre-close drain (`NetworkStream` built before `Shutdown(Send)`; net472 `Shutdown` clears `Connected`), `HttpDiag` (stage + `SocketError`), `/shutdown` → `shutting_down` | `tests/server_contract/run_contract_tests.ps1` automates the former manual list (translate/batch/prefetch/cache-import/cache-lookup, `[1,2]`/`[null]` texts → 400, `/health` `async_memory_bytes` returns to 0 after drain, idle socket gets `400 incomplete_request` then EOF at ~5 s, terminal 500/401 provider failure costs exactly one request per background batch, shape or non-string batch item failure splits and retries, prompt-echo stripping parity, shutdown persistence). Run it against both `-Exe` binaries before changing either side |
| Server binary switch (native vs C# parallel) | `select_server_binary` in `native/src/launcher/server_proc.c`; C# mirror `ServerProcess.SelectServerBinary` in `native/src/launcher_cs/ServerProcess.cs`; `launcher.ini [server] binary`; `config/launcher.ini.example` | `tests/launcher_parity/run_launcher_parity.ps1 -ServerSmoke` (run it once with a temp root whose `config\launcher.ini` sets `binary=cs`, as done 2026-09-08) |
| Godot launch preflight (probe + result cache, 2026-09-06; ported 2026-09-09) | `godot_probe.c` (`godot_output_explicitly_rejects_main_pack`: the output must mention `main-pack` **and** an option-rejection marker; a non-zero exit alone is not a conclusion) and `godot_preflight_cache.h/.c` (FNV-1a keyed ini at `config\godot_preflight.ini`, sig = translator version + kind + exe/dir/pack/script file mtimes+sizes) in `native/src/launcher`; C# mirror `GodotProbe`/`GodotPreflightCache` in `native/src/launcher_cs/GodotPreflight.cs` (`Sig` keeps the C truncation-at-`cap` semantics; the translator version comes from the generated `DsBuildInfo.g.cs`, which the csproj fills from `VERSION` exactly like `build_native.bat`'s `-DDS_TRANSLATOR_VERSION`). Consumers (`godot_runtime_sidecar_preflight`, `godot_runtime_autoload_preflight`, `godot_main_pack_supported` in `ui.c`) are mirrored in `GodotLaunch.cs`; only deterministic conclusions are cached (success, and explicit `--main-pack` rejection) — timeouts and inconclusive probes re-run every launch; registered in `build_native.bat` `LCH_SRC` | `godot-probe/classifier` (8 outputs: 4 rejections incl. case/quote variants, 4 non-conclusions) and `godot-preflight/cache` (miss → put → hit, kind isolation, missing file ⇒ `0|0` fields, size change invalidates, byte-identical `godot_preflight.ini`) in `tests/launcher_parity/run_launcher_parity.ps1` |
| API configuration (`api.ini`) | `api_config_preset_count`/`api_config_preset`/`api_config_load`/`api_config_preset_index`/`api_config_save` in `native/src/launcher/api_config.c` (data layer, declared in `api_config.h`; the modal dialog `show_api_config`/`api_wndproc` sits in the same file); C# mirrors `ApiConfig` in `native/src/launcher_cs/ApiConfig.cs` (data) and `ApiConfigDialog` in `ApiConfigDialog.cs` (dialog; same fixed pixel geometry and stock system controls, so it is unscaled on both sides). Both sides read and write through the Windows Profile API (`GetPrivateProfileStringW`/`WritePrivateProfileStringW`) rather than parsing the INI, so an existing file keeps its encoding, line endings and unrelated sections; the 11 provider presets must stay in the same order because `selected=` is an index | `api-config/defaults`, `api-config/save` (byte-identical `api.ini`), `api-config/reload` (preset lookup + custom-endpoint fallback to `selected=0`) in `tests/launcher_parity/run_launcher_parity.ps1` |
| Token cost optimization (2026-09-06) | shared system prompt prefix `API_SYSTEM_PREFIX` + per-mode suffixes, `estimate_max_tokens` (2x input + per-line JSON overhead, capped 8192), glossary hit-filtering in `build_system_content`, ctx prevs dedup (skips prev == in-batch predecessor) in `build_ctx_request` in `native/src/server/api.c`; `api_translate_split_retry` (binary-split batch failure retry, ≤4 items go per-item; splits only while the thread-local `g_api_last_fail` set by `api_diag`/`api_fail_class` is `API_FAIL_SPLIT` — transport/auth/429/5xx abort via `api_split_abort_diag`; `prevs` may be NULL) wired into `async_translate_pending` in `native/src/server/http.c`; all mirrored in `native/src/server_cs/Api.cs` (`TranslateSplitRetry`, `FailureInfo`, `HttpStatusSplittable`, `SplitAbortDiag`) and `Program.cs` (async/live/RespBatch paths; live and RespBatch failures requeue to async like C) | verified end to end with a captured-request fake local model on both servers (shared prefix/max_tokens/glossary hit+miss/split-retry-via-cache/prev-dedup assertions); no automated suite remains after `tests/` removal. Split-retry abort classification (2026-09-08) is source-reviewed only — verify with an unreachable endpoint (expect 1 request per batch, `split-retry aborted` on stderr) and a shape-failing fake model (expect the ~13-request partial-failure tree) |
| Failure diagnostics/fallback audit | `api_diag` in `native/src/server/api.c`; `_ds_report_exception` and `reportRpgmError` in `deploy.c`; `ReportCaughtException` in Unity Mono/TMP; `_dst_report_error` in `godot_patch.c` | `tests/test_server.ps1`, `tests/test_launcher.ps1`, `tests/test_unity_text_rules.ps1`, `tests/godot_patch_probe.c`, `docs/FAILURE_TRANSPARENCY.md` |
| Build output name/version and embedded-payload parity | `VERSION`, `build_native.bat`, `scripts/verify_build_artifacts.ps1`, `native/src/launcher/ui.c` | managed source/recipe/output fingerprints and mtime-independence in `tests/test_build_artifact_fingerprints.ps1`, embedded resource hash check in `scripts/verify_build_artifacts.ps1`, version checks in `tests/test_launcher.ps1` |
| Native compiler warning gate | `-Wall -Wextra -Werror` flags in `build_native.bat` | build flag checks in `tests/test_launcher.ps1`, then `build_native.bat` |
| Test child-process ownership and Windows argument quoting | `Start-TestOwnedProcess`, `Close-TestOwnedProcess`, and `Invoke-TestOwnedCleanup` in `tests/TestProcessHarness.ps1` | `tests/test_process_harness.ps1`, then `tests/run_all.ps1 -SkipEndurance` |
| Local-server load and memory fixtures | `New-TestLoadCacheFixture`, `New-TestLoadClient`, `Wait-TestTranslationServerReady`, and `Read-TestLoadRequestCount` in `tests/TestLoadHarness.ps1`; request diagnostics in `tests/bench_client.c` | `tests/test_endurance.ps1`, `tests/test_memory_generations.ps1`, then `tests/run_all.ps1` |
| Runtime payload installer | `scripts/install_runtime_payloads.ps1`, `Assert-UnderRoot`, `Assert-NoReparseTraversal`, `docs/RUNTIME_PAYLOADS.md` | installer checks in `tests/test_launcher.ps1` |
| Source-only package | `scripts/prepare_open_source_release.ps1`, `scripts/audit_open_source_release.ps1`, `OPEN_SOURCE_RELEASE.md` | `prepare_open_source_release.ps1 -NoZip` |
| Program package | `scripts/prepare_program_release.ps1` | package smoke with `ds游戏翻译器.exe --sync-payloads-and-exit` |

## Translation Latency Tuning

API model discovery (2026-09-18): `native/src/launcher/api_models.c/.h` and
`native/src/launcher_cs/ApiModels.cs` own bounded GET `/models`, authentication,
transport validation and catalog parsing. `api_config.c` / `ApiConfigDialog.cs`
own background request lifetime, editable model selection and refresh status.
`tests/launcher_parity/run_api_models_tests.ps1` uses a fake provider on a dynamic
loopback port (91 scenarios, both implementations); it is a `build_native.bat`
gate. It covers same-host official adapters, Anthropic cursors, DashScope pages,
page failures/limits and manual fallback. `api-config/ui` compares eight dialog
controls plus selection/manual input, all-provider model clearing, exact catalog
membership, stale-response rejection, empty-save rejection and failure retention
through `--api-ui-probe-and-exit` / `--api-ui-probe`. The probe never fetches a
catalog or saves configuration and omits all field contents from its report.

Presets have no model field: never reintroduce versioned examples as automatic
selections. The dialog requires an explicit catalog/manual choice when no saved
model exists; the server compatibility default remains separate.

DeepSeek model naming (2026-09-18): server default/example model is the official
stable API ID `deepseek-flash` (V4.1 Flash). Both server `api` implementations
also recognize this ID for non-thinking translation requests. The source-linked
native request builders and built C# shared message builder are guarded by
`tests/server_contract/run_model_defaults_tests.ps1` (20 cases; build gate),
including old explicit IDs, custom provider isolation and unchanged INI bytes.

| Path | Primary tuning symbols | Guard/tests |
| --- | --- | --- |
| Ren'Py visible work | `_ds_fast_live_queue` and the fast `_ds_live_loop` batch limit in `RENPY_HOOK` | `tests/renpy_hook_probe.py`, `tests/test_launcher.ps1` |
| RPG Maker queued dialogue | `ACTIVE_MESSAGE_POLL_INITIAL_MS`, `ACTIVE_MESSAGE_POLL_MAX_MS`, `scheduleActiveMessageCachePoll` | `tests/rpgm_hook_probe.js`, `tests/test_launcher.ps1` |
| Unity Mono batching/prefetch | `MaxClientBatchSize`, `DeepPrefetchBatchSize`, `GameScriptWarmupBatchSize`, `GameScriptWarmupPauseMs` | `tests/test_unity_text_rules.ps1` |
| Unity IL2CPP queued lookup | `GetQueuePollDelaySeconds` and `QueuePollIntervalSeconds` | `tests/UnityEndpointStateProbe`, `tests/test_unity_endpoint_state.ps1` |
| Godot 4 runtime scan | `DST_TRACKED_CONTROL_SLICE`, `DST_FULL_SCAN_EVERY`, `_dst_scan_controls`, `_dst_scan` | `tests/godot_patch_probe.c`, `tests/test_launcher.ps1` |

## Search Shortcuts

Godot HTTP boundary guard (2026-09-30):
`tests/payload_scripts/test_godot_http_contract.py` runs both shipped scripts
in isolated real Godot packs against a fake service on a dynamic localhost port.
It covers valid and identity/blank responses, nonstring translation entries,
invalid translation maps, truncated transfers, BBCode/whitespace preservation,
four stalled 48-text request slots, subsequent recovery, and paused-menu scans.
The production 12-second deadline is shortened only in the fixture. Both
autoload and sidecar modes run; set `GODOT3_EXE` / `GODOT4_EXE` to actual engine
binaries (a Godot 4 console wrapper cannot run alone after being renamed).
The payload checker invokes this guard and explicitly notes missing engines.
Owners: `_dst_start`, `_dst_http_done` (Godot 3), `_dst_done` (Godot 4), and the
Godot 3 HTTP/timer `PAUSE_MODE_PROCESS` settings. No shared cache text is changed.

```powershell
rg -n "ENGINE_GODOT|deploy_godot|warmup_godot|warmup_scan_godot_resources|godot_prepare_patch_pack|scan_godot" native/src/launcher tests
rg -n "ENGINE_RENPY|deploy_renpy|warmup_renpy|IDR_SCRIPT_RENPY_HOOK" native/src/launcher native/src/launcher_cs tests
rg -n "ENGINE_RPGM|deploy_rpgm|warmup_rpgm|IDR_SCRIPT_RPGM_HOOK" native/src/launcher native/src/launcher_cs tests
rg -n "deploy_unity|deploy_unity_il2cpp|UnityTranslator|DeepSeekUnityFontPatcher|DeepSeekTMPFontFallback|XUnity" native payloads tests
rg -n "cache_only|/cache/import|/cache/lookup|/prefetch|translation_memory" native payloads tests scripts docs
rg -n "prepare_.*release|audit_open_source|install_runtime_payloads|build_native" scripts tests README.md docs
rg -n "DstCore|CacheCodec|Contract\.(Path|Field|Source)" native payloads tests
rg -n "detect-and-exit|deploy-and-exit|restore-and-exit|warmup-and-exit|g_log_to_stdout|g_warmup_dump_stdout|NotPorted" native tests
rg -n "ui-probe|g_ui_probe|g_anim_tick_override|ui_runtime_tag|AnimTickOverride|ProbeMode" native tests
rg -n "should_warm_|ShouldWarm|textlist_add|TextList|post_prefetch_batch|BuildPrefetchBody" native/src/launcher native/src/launcher_cs tests
```

## Add Or Replace An Engine Path

1. Add or update `Engine` in `native/src/launcher/engine.h` **and** the C#
   mirror `Engine` in `native/src/launcher_cs/EngineDetector.cs` (same order
   and values; `engine_id=` is compared by the parity test).
2. Add detection in `detect_engine` and `EngineDetector.Detect`; keep
   broad/weak markers after stronger engine markers. Example: Unity `_Data`
   stays before Godot `.pck`.
3. Add deploy behavior in `deploy.c` and declaration in `deploy.h`; mirror it in
   `native/src/launcher_cs/Deploy.cs` or make `Deploy.ForEngine`/`Restore`
   return `Deploy.NotPorted` for it (never fake a result).
4. Route it in `deploy_for_engine`, `start_translation` and
   `warmup_translations`; mirror the warmup scanner in a new
   `native/src/launcher_cs/Warmup<Engine>.cs` partial (byte-string convention,
   see `Warmup.cs` header) or make `Warmup.Run` return `Deploy.NotPorted` for
   it, and add `warm_*` scenarios to the parity script.
5. If the engine ships an embedded script, add it as a real file under
   `payloads/<Engine>/`, register the RCDATA ID in `resource.h`,
   `build_native.bat`, `scripts/verify_build_artifacts.ps1`,
   `launcher_cs.csproj` (`EmbeddedResource`), and extend
   `tests/payload_scripts/check_payload_scripts.ps1`.
6. Add detection fixtures and deploy/restore scenarios to
   `tests/launcher_parity/run_launcher_parity.ps1`. If the deploy copies from
   `payloads\`, add tiny stand-ins to the script's synthetic launcher root
   rather than pointing the scenarios at the real payload tree.
7. Update user docs only after behavior and tests match.

## Change The Launcher Window

星夜首页（2026-10-12）：`UiLayout` / `compute_layout` 保留原 16 个控件，
顶部星轨使用冻结时钟绘制，工作区采用暖白卡片。33ms 心跳只刷新星空、金线和
状态点；后台/最小化暂停，系统减少动画设置关闭装饰刷新。`DST_UI_PROBE_TICK`
仅用于探针冻结到 0–30000ms；`ui/motion-*` 比较两版像素并限制变化范围。
`ui/1064x661-client` 覆盖 1080×700 外框对应的客户区，日志必须位于卡片内边距中。
真实窗口验收与假服务回归分开运行，防止窗口的 `/health` 探测干扰 19999 假服务记录。

The window exists twice (`native/src/launcher/ui.c` + `main.c` and
`native/src/launcher_cs/Ui*.cs` + `MainWindow.cs`) and the parity gate compares
rendered pixels, so a one-sided edit fails the build.

1. Change the C side first, then mirror it in the matching C# file listed in
   "Launcher visual system" above. Keep the same GDI call order: a different
   order of `FillRect`/`RoundRect`/`DrawTextW` changes anti-aliased edges even
   when the geometry is identical.
2. Use `Theme.Sc` (not `w * dpi / 96`) for every pixel and `Theme.Mix` for every
   blend, so both sides truncate the same way.
3. Anything time-based must read `anim_tick()` / `Theme.AnimTick()` instead of
   `GetTickCount`, otherwise the probe cannot freeze it and the bitmap diff
   becomes flaky.
4. New controls need an entry in `probe_report` / `UiProbe.Report` (id, rect,
   text) and, if owner-drawn, a `DRAWITEMSTRUCT` branch in `print_child` /
   `UiProbe.PrintChild`.
5. Re-run `tests\launcher_parity\run_launcher_parity.ps1`; on a mismatch it
   prints the first differing pixel's coordinates. Then take a real screenshot
   of both binaries — the probe pins layout and colour, not DWM chrome or the
   user's DPI.
6. Explorer file drops are registered on the main window and its child controls
   in `main.c` / `MainWindow.cs`. A child subclasses `WM_DROPFILES` and forwards
   ownership to the main window, which calls `DragFinish` once. The `ui/drop-*`
   parity scenarios send an in-process HDROP through the real window procedure
   for an EXE, directory and invalid file, and assert that no game files change.

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

Managed fingerprint publication (2026-09-30):
`Write-ManagedPayloadStamp` in `scripts/verify_build_artifacts.ps1` compares
exact bytes before writing and publishes changed stamps through a unique sibling
plus atomic `File.Replace`. `tests/build_artifacts/test_managed_stamp_write.ps1`
source-links the actual writer and fingerprints in a temporary fixture. It
holds a real Windows read-only memory map, checks unchanged bytes/mtime and old
reader views, then denies replacement to verify failure diagnostics, old-stamp
preservation, temporary-file cleanup and recovery after the holder releases its
lock. This is a `build_native.bat` gate; build stamps remain mandatory inputs.

Use the narrowest useful check first, then climb when blast radius grows:

```powershell
powershell -ExecutionPolicy Bypass -File tests\payload_scripts\check_payload_scripts.ps1
cmd /c build_native.bat            # builds everything, runs tests\core_tests and tests\launcher_parity
powershell -ExecutionPolicy Bypass -File tests\server_contract\run_contract_tests.ps1 -Exe native\dst_server.exe -Exe native\dst_server_cs.exe
powershell -ExecutionPolicy Bypass -File tests\launcher_parity\run_launcher_parity.ps1 -ServerSmoke   # needs port 19999 free
powershell -ExecutionPolicy Bypass -File scripts\prepare_open_source_release.ps1 -Version <version> -NoZip
powershell -ExecutionPolicy Bypass -File scripts\prepare_program_release.ps1 -Version <version>
```

`build_native.bat` must stay ASCII-only: `cmd.exe` seeks batch files by
character count, so a multibyte comment desynchronises every following line
(observed 2026-09-08 as `'delims' is not recognized...` from line 9 onward).

For program package parity, run the produced standalone exe once with
`--sync-payloads-and-exit` in a temporary directory and check that the local
server, scripts, example config, and first-party Unity plugin DLLs are emitted.

Godot PCK v4 (2026-09-22): `godot_patch.c` / `GodotPatch.cs` +
`GodotPatchPack.cs` and `godot_warmup.c` / `WarmupGodot.cs` accept ordinary
v4 directories and reject unsupported pack/entry flags. `godot_prepare_runtime_pack`
/ `PrepareRuntimePack` constructs a first-launch module without HTTP;
`ui.c` / `GodotLaunch.cs` uses it before falling back to a sidecar.
`gp_pck_v4_*`, `warm_godot_pck_v4`, and `lf_godot_v4_first_launch` in launcher
parity protect generation, warmup, original resources and launch selection.

Godot update guard (2026-09-23): `godot_patch_source_current` /
`GodotPatch.SourceCurrent` check a byte fingerprint beside each generated pack
that binds the source PCK, both bundled Godot scripts and `VERSION`;
`godot_prepare_patch_launcher` / `GodotPatch.PreparePatchLauncher` refresh only
owned EXE copies when game bytes change. `godot_runtime_*_preflight` /
`GodotLaunch.Runtime*Preflight` require exit 73 from new scripts and include
the rebuilt pack in the probe cache key. The launcher shows an unconfirmed
runtime warning when it falls back. Guards: `godot-patch/launcher_copy`,
`launch-flow/godot-source-refresh`, `launch-flow/godot-payload-upgrade`,
`lf_godot_loose_ignored`, and payload checks.

Unicode API configuration: `ApiConfigUnicode.cs` mirrors the atomic ANSI-to-UTF16
save in `api_config.c`; `ApiConfigText.ModelFits` is source-linked into the C#
launcher and server. `test_model_encoding.py` is run by the model defaults gate
and covers both launchers feeding both servers, emoji/CJK, length and read-only
failures, preservation of unrelated settings, and no writes on server load.
Bundled CJK font fallback (2026-09-26): `assets/fonts/` contains the reviewed
original Noto Sans CJK SC OTF, full SIL OFL, and upstream SHA-256 manifest.
`font_bundle.c` / `FontBundle.cs` materialize the verified embedded asset;
`deploy.c` / `Deploy.cs` own Ren'Py, RPG Maker, and Unity copies and restore.
Godot font selection uses the same bundle only after system candidates fail;
the IL2CPP TMP plugin reads `TranslationFonts/` after system font directories.
`tests/launcher_parity/run_font_bundle_tests.ps1` simulates absent Windows
fonts and checks both launcher copies, ownership, byte hashes, and restore.
Godot encrypted directory/preflight (2026-10-01):
`godot_encrypted_pack.c` / `GodotEncryptedPack.cs` normalize checksum-verified
PCK v3 directories only in owned patch copies. `build_godot_runtime_autoload` /
`BuildRuntimeAutoload` emit the dedicated autoload load proof; `ui.c` /
`GodotLaunch.RuntimeAutoloadPreflight` observe complete proof lines from live
owned probes instead of racing game startup with `SceneTree.quit`.
`tests/launcher_parity/test_godot_encrypted_pack.py` is a build gate (24 C/C#
runtime-pack cases); launcher parity pins missing/misleading/crashed proofs.
The source stamp is V4 so earlier generated autoloads rebuild on upgrade.
