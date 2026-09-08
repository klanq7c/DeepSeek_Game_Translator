# Project Context

This repository builds ds游戏翻译器, a local-first game translation launcher
and runtime. The important design shape is:

- The launcher detects the game engine, starts or adopts the local translation
  server, deploys the engine-specific runtime pieces, then warms the cache.
- The local C server owns the shared HTTP contract and translation cache. It
  must stay local-cache-first and must not block a game renderer on remote API
  latency.
- The local server reserves one configured remote channel for visible live
  text when background prefetch is active. A multi-text live request is
  deduplicated, split by both item and character budgets, and dispatched
  concurrently through the live worker pool. Provider results enter the cache
  in batches so one remote response needs one map-lock interval and one TSV
  flush rather than one flush per translated string.
- Engine adapters own renderer compatibility. Font coverage, rich text, glyph
  punctuation, typewriter behavior, overlays, and resource parsing belong in
  the closest Ren'Py, RPG Maker, Unity, or Godot layer.
- Translation memory, cache files, API keys, user config, logs, third-party
  runtimes, Unity/game assemblies, and generated build products are user or
  build artifacts. Do not commit or rewrite them unless explicitly requested.

## Domain Terms

- **Launcher**: the Win32 UI under `native/src/launcher`. It detects engines,
  deploys hooks/plugins, starts the local server, and triggers cache warmup.
  It also exposes hidden, window-less **diagnostic modes** for parity testing:
  `--detect-and-exit <dir>`, `--deploy-and-exit <dir>`, `--restore-and-exit
  <dir>`, `--warmup-and-exit <dir>`, `--godot-patch-and-exit <dir>`,
  `--godot-promote-and-exit <dir>` and `--godot-launcher-and-exit <dir>` print
  deterministic UTF-8 reports (and `log=` mirrored log lines) to the parent's
  redirected stdout; `--sync-payloads-and-exit` and `--godot-patch-worker` are
  the pre-existing helper modes.
- **C# launcher port** (`native/src/launcher_cs`, migration phase 3, feature
  complete since 2026-09-09): a module-by-module rewrite of the launcher in
  C#/net472. It is not shipped and is not the `build_native.bat` product; each
  ported module must produce byte-identical output to the C launcher under
  `tests/launcher_parity`. Ported: engine detection (`EngineDetector`),
  filesystem safety layer (`SafeFs`, mirrors `fsutil.c` reparse-point and
  Win32 error semantics), deploy + restore for all five engines (`Deploy` in
  `Deploy.cs` for Ren'Py/RPG Maker/Godot and `DeployUnity.cs` for Unity Mono
  BepInEx 5/6 runtime install/repair, stripped-mscorlib support, and IL2CPP
  BepInEx + XUnity with config ownership), server lifecycle
  (`ServerProcess`), and the warmup scanners for all five engines
  (`Warmup.cs`, `WarmupRenPy.cs`, `WarmupRpgm.cs`, `WarmupUnity.cs`,
  `WarmupGodot.cs` incl. PCK 1/2/3 directory parsing and embedded-EXE packs;
  batch bodies compared byte for byte through the `--warmup-and-exit`/
  `--warmup` dump modes), embedded payload self-update (`SelfUpdate.cs`;
  the same eleven first-party payloads are embedded as `EmbeddedResource` items
  named after the RCDATA macros, released trees compared through
  `--sync-payloads-and-exit`/`--sync-payloads`), and the Godot patch pack
  (`GodotPatch.cs`/`GodotPatchResources.cs`/`GodotPatchPack.cs`; the built
  `dst_godot_patch.pck` must be byte-identical and the `/batch` request bodies
  must match, verified against a fake deterministic local server through
  `--godot-patch-and-exit`/`--godot-patch`), the `api.ini` data layer
  (`ApiConfig`: provider presets, Profile-API read/write, byte-identical
  `api.ini`), the Godot launch preflight (`GodotProbe` `--main-pack` rejection
  classifier plus `GodotPreflightCache`, byte-identical
  `config\godot_preflight.ini`), and the one-click launch flow
  (`LaunchFlow`/`GodotLaunch`: engine-specific launch/warmup ordering, the three
  Godot headless preflight probes, the three translated launch paths, the
  detached patch worker, and the cache card/clear), and the Win32 window itself
  (`Win32`/`Theme`/`UiPaint`/`UiButtons`/`UiLayout`/`MainWindow` plus
  `ApiConfigDialog`, `FolderPicker`, `LauncherConfig`) — raw GDI through
  P/Invoke, no WinForms, verified pixel for pixel by the **window render probe**
  below. The build produces `build\launcher_cs\dst_launcher_cs.exe` as a
  `WinExe`, so it opens the same window when started with no arguments and
  attaches to the parent console only for the diagnostic modes.
- **Window render probe**: `--ui-probe-and-exit <w> <h> <alive> <bmp>` (C) and
  `--ui-probe <w> <h> <alive> <bmp>` (C#) paint the launcher window off-screen
  into a 32bpp bitmap and print a layout report (client size, DPI, every control
  id/rect/text). To make two machines and two languages comparable the probe
  pins DPI to 96, freezes the animation clock at tick 0, takes the server state
  from an argument, and skips the `WM_CREATE` side effects (payload sync, saved
  directory, timers). The rail footer tag and hero subtitle intentionally differ
  between the two binaries (`C native runtime` vs `C# managed runtime`); the
  probe renders neutral placeholders for them and `--ui-identity*` asserts the
  real strings separately.
- **Local server**: `native/src/server/dst_server.exe` (C) or the parallel
  `native/dst_server_cs.exe` (C#, selected with `launcher.ini [server]
  binary=cs`). Both implement the same HTTP contract: `/health`,
  `/capabilities`, `/translate`, `/batch`, `/prefetch`, `/cache/import`,
  `/cache/lookup`, `/cache/export`, `/cache/dump`, `/shutdown`. The contract is
  pinned by `tests/server_contract/run_contract_tests.ps1`, which runs the same
  50 checks against each binary with a fake OpenAI-compatible provider.
- **Shared core (`DstCore`)**: `native/src/core/*.cs` — `TextRules` (prompt echo
  stripping, CJK detection), `Json` (strict parser/escaper), `CacheCodec` (TSV
  + base64 cache line format), `Contract` (paths, JSON field names, `source`
  values). Source-linked (not a DLL) into the C# server, the XUnity endpoint
  payload, the C# launcher port, and `tests/core_tests`, which pins C parity
  with golden cases.
- **Embedded engine scripts**: the Ren'Py hook (`payloads/RenPy/iron_deepseek.rpy`),
  the RPG Maker hook (`payloads/RPGMaker/hook_rpgm_mv.js`) and the Godot 3/4
  runtime sidecars (`payloads/Godot/dst_godot_runtime_g{3,4}.gd`) are real
  source files (LF, UTF-8, no BOM — enforced by `.gitattributes` and
  `tests/payload_scripts`). `build_native.bat` embeds them as RCDATA 301–304;
  `deploy.c`/`godot_patch.c` read them through `embedded.c` and write them out
  byte for byte. They are no longer C string literals.
- **Detection adapter**: `native/src/launcher/engine.c`. It maps a selected
  game directory to `Engine`.
- **Deploy adapter**: `native/src/launcher/deploy.c`. It writes hooks, copies
  first-party plugins, installs launcher-owned support files, and tells users
  when third-party payloads are missing.
- **Warmup scanner**: `native/src/launcher/warmup.c` plus engine scanner
  adapters such as `native/src/launcher/godot_warmup.c`. It reads existing
  game resources and queues likely text through the local server without
  writing speculative translations as successful cache entries. The C# mirror
  in `native/src/launcher_cs/Warmup*.cs` treats every `string` as a C byte
  string (one char per byte) so that filters, limits, and JSON serialization
  stay byte-identical with the C scanner.
- **Runtime payload**: engine-specific code used inside a game, such as the
  Ren'Py hook, RPG Maker hook, UnityTranslator plugin, XUnity endpoint, or TMP
  font fallback.
- **Source-only release**: public repository/archive without third-party
  runtime binaries, user data, caches, logs, Unity/game assemblies, or real
  config files.
- **Program release**: standalone `ds游戏翻译器.exe` package containing the
  launcher, local server, scripts, example config, and first-party plugin DLLs.
- **Load-test fixture**: an owned temporary cache containing one fixed hot key,
  a temporary `bench_client` built from current source, and an explicitly
  disabled API configuration. Endurance and generational-memory tests use this
  fixture so they measure the local cache/HTTP path without loading, changing,
  or depending on user translation memory or remote provider latency. The
  native client writes its request count directly to an owned output file and
  reports transport-stage failures separately.
- **Renderer compatibility layer**: the hook or plugin closest to the renderer.
  Keep display fixes here instead of changing shared server/cache text.
- **Translation prompt echo**: provider output that prepends an instruction such
  as `翻译成简体中文：` or `Simplified Chinese translation:` to the real
  translation. The local server strips these wrappers at every cache ingress;
  engine adapters may repeat the cleanup defensively before accepting display
  text. Disk-loaded cache values heal in memory without rewriting user files.
- **RPG Maker message page**: the complete `Game_Message._texts` payload that a
  message plugin may merge from many event commands. The RPG Maker runtime
  plans cache candidates and translation granularity per page; small pages may
  use a whole-page result, while large pages populate control-safe per-line
  cache entries but render language-atomically: if any visible line is still a
  miss, the current page remains entirely source text. Visible misses on a
  large page are deduplicated and sent in bounded live batches; transformed
  key variants must not fan out into individual requests.
- **RPG Maker first-draw cache boundary**: ordinary window, help, auto-wrap,
  copied `drawTextEx`, choice, bitmap, and message paths make one bounded
  asynchronous `/cache/lookup` request per new display key. Message pages query
  both the complete multiline key and their renderer-safe line variants. The
  request is local-cache-only and never waits on a remote provider; ordinary
  renderers refresh after either a hit or miss. The refresh covers both normal
  display-tree children and plugin-owned scene window fields such as
  `_commandWindow`; a hit redraws from cache, while a miss gets one opportunity
  to enter the existing asynchronous live path.
- **RPG Maker cold-dialogue gate**: `Window_Message.startMessage` does not
  create an English message page only while its nonblocking localhost cache
  lookup is in flight. Remote provider work never gates the engine lifecycle.
  The normal MV/MZ message update retries after a cache hit, miss, error, or
  transport timeout. When the server returns a queued source result, the same
  visible page performs bounded, backoff-based `/cache/lookup` polling; a later
  cache hit restarts that page in Chinese without mutating
  `Game_Message._texts`, while page changes and timeout cancel the poll.
  The visible-page cache retry starts at 40 ms and backs off to the bounded
  steady interval, so a completed local-server result is not hidden behind a
  coarse first refresh.
- **RPG Maker display acceptance**: renderer-local results must retain ordered
  control tokens and must not contain a copied run of three or more source
  English words inside an otherwise CJK translation. Isolated names, acronyms,
  key labels, and game terms remain valid. Rejection affects only the RPG Maker
  display cache and never rewrites shared translation memory.
- **Renderer control sequence**: engine markup whose order is part of the
  display contract, such as RPG Maker `\c[]`, `\v[]`, `\i[]`, `<WordWrap>`,
  and `<br>`. A cached translation that drops or reorders these tokens is not a
  display-safe result.
- **RPG Maker asynchronous redraw boundary**: a translation completion may
  redraw only a window that actually rendered translatable text and is still
  open. Persistence scenes (`Scene_File`, save/load modes, and plugin-defined
  equivalents), closed/closing windows, and unrelated `Window_Base` instances
  must not be refreshed by the translation adapter; their `refresh()` methods
  may read saves or schedule delayed renderer work. A visible window in its
  opening transition remains a valid redraw target, including the first frame
  where `_opening` is true and `openness` is still zero.
- **Unity Mono pump ownership**: the disposable BepInEx manager component
  bootstraps the runtime onto a dedicated persistent plugin root before hooks
  or background work start. The runtime pins and narrowly protects that owned
  root from game-driven bootstrap cleanup. The persistent `TranslatorDriver`
  is the normal owner of `PumpOnce`; the plugin host `Update` is only a
  fallback when that driver is missing or disabled.
  Background cache imports
  merge with live results, remote callbacks retain scene components weakly,
  and teardown serializes the final per-game cache snapshot with background
  persistence. Fungus games use the active `Say.OnEnter` lifecycle to queue a
  bounded lookahead of later `Say` and `Menu` commands through `/prefetch`
  after the current visible line starts; current dialogue keeps the live lane,
  while version-specific `MenuDialog.AddOption` overloads consume warmed cache
  entries without relying on one historical `SetOptions` signature. Fungus
  first-miss async writes preserve supported source color tags in the UGUI
  renderer, matching the existing cache-hit path without widening rich-text
  handling to unsupported TMP-only tags.
  Live client work uses bounded 32-text batches, while script and optional deep
  prefetch use larger chunks with short yielding gaps; visible work remains
  ahead of background discovery.
- **Failure transparency boundary**: a narrow adapter edge where an external
  transport, process, engine version, or optional renderer API may fail while
  the game preserves source text. The boundary must log operation context,
  must not turn a failure into a cache hit, and is catalogued in
  `docs/FAILURE_TRANSPARENCY.md`.
- **Unity translation acceptance**: Mono and XUnity sanitize known provider
  prompt echoes before comparing, displaying, or persisting a result. A result
  that becomes empty or equal to the source after cleanup remains unresolved,
  except that a server `pass` source is an explicit identity terminal the
  XUnity endpoint may display for the current job only after disabling
  `SaveResultGlobally`; it enters neither the shared cache nor XUnity's
  generated translation file. Mixed XUnity batches poll only their
  `miss`/`queued` subset. Mono batch consumers read the response `sources`
  array: `miss`/`queued`/`pass` take a transient retry cooldown, while token
  loss and other content failures count toward rejection abandonment.
  XUnity also handles progressively appended TMP text: when an already
  translated CJK prefix is followed by a meaningful new Latin passage, the
  endpoint protects the existing Han runs with validated request-only tokens,
  translates the new passage, then restores the prefix. The shared server CJK
  heuristic remains unchanged, and token loss fails closed.
  Queued XUnity work polls the localhost cache at 50/100 ms during its initial
  retry window before returning to the configured steady cadence.
- **Unity IL2CPP fallback topology**: TMP fallback lists are mutable renderer
  state. Games and Addressables may replace them after startup, so the slow
  fallback pass must verify membership again and dirty loaded text meshes when
  it restores a missing fallback instead of trusting an instance-ID marker
  forever. Dynamic TMP assets explicitly add each translated sentence's new
  CJK glyphs before the component switches to that asset and its matching
  material; a component reused for Latin text restores the game's original
  font. Legacy UGUI has a separate dynamic atlas, so each changed CJK value is
  requested at the component's current font size/style before `SetAllDirty`.
  This renderer state never enters shared translation memory. When BepInEx
  generates `interop/assembly-hash.txt` in the current process, the renderer
  defers its TMP setter Harmony patch for that process: affected Unity 6000
  builds can otherwise terminate CoreCLR at the first dynamic setter
  invocation. The low-frequency renderer scan remains active, and the next
  game start installs the normal setter patch. Unity 6000 generated wrappers
  may also fail before native synchronous `AssetBundle.LoadFromFile` on a
  `ReadOnlySpan.GetPinnableReference` ABI mismatch. Only that exact failure
  enables the lifecycle-polled async loader; a working dynamic path-font
  boundary remains preferred.

## Engine Ownership

- **Ren'Py**: detection and launch flow live in the launcher; Python hook and
  font deployment live in `deploy.c`; `.rpy` scanning lives in `warmup.c`.
  Ren'Py `init python` globals share the game store, so hook-owned modules and
  helpers must use `_ds_*` names; common game variables such as `time` must not
  be able to replace a hook dependency after initialization. Visible dialogue
  and menu text must retain priority through `_ds_translate` and `_ds_fetch`,
  keeping them ahead of background UI and warmup work. At the engine start
  lifecycle, compiled-script `Menu` labels are deduplicated from
  `renpy.game.script.namemap` and prefetched before they become visible; the
  live `Menu.execute` hook remains the compatibility fallback.
  Visible dialogue and menu bursts use a dedicated 16-text live worker batch;
  background UI text cannot consume that priority queue.
- **RPG Maker MV/MZ**: JavaScript hook and `www/fonts` deployment live in
  `deploy.c`; JSON and plugin text scanning live in `warmup.c`. RPG Maker MV's
  `plugins.js` may create plugin script tags before the translator tag runs,
  while those scripts finish loading afterward and replace message methods.
  The runtime therefore observes both existing and future plugin script `load`
  events and reinstalls only the closest RPG Maker hooks after each load.
  RPG Maker MZ distributions may also keep only `main.js` in `index.html` and
  dynamically create the core `rmmz_*` and `plugins.js` tags after the
  translator runs; the hook observes those core script `load` events so engine
  classes become hookable without fixed-delay installation. Warmup also scans
  bounded external TXT and CSV localization resources field-by-field, including
  flat-layout root files such as `game_messages.csv`.
- **Unity Mono/BepInEx 5/6**: launcher reads the Unity player PE machine and
  deploys flavor- and architecture-specific x86/x64 BepInEx payloads; managed
  runtime behavior lives in `payloads/UnityTranslator/src`.
- **Unity IL2CPP/XUnity/TMP fallback**: launcher deploys XUnity endpoint and
  TMP fallback; IL2CPP runtime behavior lives under `payloads/UnityIL2CPP`.
- **Godot**: launcher detects exports/projects, warms resource text through
  `godot_warmup.c`, builds an external `dst_godot_patch.pck` through
  `godot_patch.c`, and generates a version-compatible runtime sidecar for
  visible dynamic UI text in Godot 3/4 exports and loose projects. Dialogue
  Markdown is scanned and patched as plain body lines while headings, speakers,
  commands, and code fences remain structural. Compiled `.scn`/`.res`/`.gdc`
  resources are warmup inputs; BBCode is queued with the same visible-segment
  keys used by the runtime. Godot 3 RichTextLabel uses `bbcode_text`, while
  Godot 4 reads rich text through `text`. PCK format 3 stores its directory at
  the header's pack-tail offset instead of directly after the header. Godot 4.6
  exports that disable `--script`/`--main-pack` use an owned matching executable
  plus an `override.cfg` `autoload_prepend` entry embedded in the copied patch
  pack. The Godot 4 autoload tracks newly added `Control` nodes, rotates through
  them before the whole-tree fallback, and keeps its timer/HTTP nodes in
  `PROCESS_MODE_ALWAYS` so pause menus still translate while game logic remains
  paused. Successful patch replacement removes any stale staged `.next.pck`.
  Godot 4 scans newly tracked controls in bounded 256-node slices every 80 ms
  and runs the compatibility whole-tree fallback every 25 ticks, avoiding a
  duplicate full traversal on every refresh.
  Older exports retain the sidecar command-line path. Original game `.pck` or
  embedded packs are not rewritten.
