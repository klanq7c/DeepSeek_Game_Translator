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
- **Local server**: `native/src/server/dst_server.exe`. It exposes `/health`,
  `/translate`, `/batch`, `/prefetch`, `/cache/import`, `/cache/lookup`,
  `/cache/export`, and `/cache/dump`.
- **Detection adapter**: `native/src/launcher/engine.c`. It maps a selected
  game directory to `Engine`.
- **Deploy adapter**: `native/src/launcher/deploy.c`. It writes hooks, copies
  first-party plugins, installs launcher-owned support files, and tells users
  when third-party payloads are missing.
- **Warmup scanner**: `native/src/launcher/warmup.c` plus engine scanner
  adapters such as `native/src/launcher/godot_warmup.c`. It reads existing
  game resources and queues likely text through the local server without
  writing speculative translations as successful cache entries.
- **Runtime payload**: engine-specific code used inside a game, such as the
  Ren'Py hook, RPG Maker hook, UnityTranslator plugin, XUnity endpoint, or TMP
  font fallback.
- **Source-only release**: public repository/archive without third-party
  runtime binaries, user data, caches, logs, Unity/game assemblies, or real
  config files.
- **Program release**: standalone `ds游戏翻译器.exe` package containing the
  launcher, local server, scripts, example config, and first-party plugin DLLs.
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
  Older exports retain the sidecar command-line path. Original game `.pck` or
  embedded packs are not rewritten.
