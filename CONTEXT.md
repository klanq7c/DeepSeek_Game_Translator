# Project Context

This repository builds ds游戏翻译器, a local-first game translation launcher
and runtime. The important design shape is:

- The launcher detects the game engine, starts or adopts the local translation
  server, deploys the engine-specific runtime pieces, then warms the cache.
- The local C server owns the shared HTTP contract and translation cache. It
  must stay local-cache-first and must not block a game renderer on remote API
  latency.
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
  stay atomic, while large pages use control-safe per-line results.
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
- **Unity Mono pump ownership**: the persistent `TranslatorDriver` is the
  normal owner of `PumpOnce`; the BepInEx plugin host `Update` is only a
  fallback when that driver is missing or disabled. Background cache imports
  merge with live results, remote callbacks retain scene components weakly,
  and teardown serializes the final per-game cache snapshot with background
  persistence.
- **Failure transparency boundary**: a narrow adapter edge where an external
  transport, process, engine version, or optional renderer API may fail while
  the game preserves source text. The boundary must log operation context,
  must not turn a failure into a cache hit, and is catalogued in
  `docs/FAILURE_TRANSPARENCY.md`.
- **Unity translation acceptance**: Mono and XUnity sanitize known provider
  prompt echoes before comparing, displaying, or persisting a result. A result
  that becomes empty or equal to the source after cleanup remains unresolved.
- **Unity IL2CPP fallback topology**: TMP fallback lists are mutable renderer
  state. Games and Addressables may replace them after startup, so the slow
  fallback pass must verify membership again and dirty loaded text meshes when
  it restores a missing fallback instead of trusting an instance-ID marker
  forever.

## Engine Ownership

- **Ren'Py**: detection and launch flow live in the launcher; Python hook and
  font deployment live in `deploy.c`; `.rpy` scanning lives in `warmup.c`.
- **RPG Maker MV/MZ**: JavaScript hook and `www/fonts` deployment live in
  `deploy.c`; JSON and plugin text scanning live in `warmup.c`.
- **Unity Mono/BepInEx 5/6**: launcher deploys BepInEx flavor-specific
  payloads; managed runtime behavior lives in `payloads/UnityTranslator/src`.
- **Unity IL2CPP/XUnity/TMP fallback**: launcher deploys XUnity endpoint and
  TMP fallback; IL2CPP runtime behavior lives under `payloads/UnityIL2CPP`.
- **Godot**: launcher detects exports/projects, warms resource text through
  `godot_warmup.c`, builds an external `dst_godot_patch.pck` through
  `godot_patch.c`, and generates a version-compatible runtime sidecar for
  visible dynamic UI text in Godot 3/4 exports and loose projects. The patch
  pack is launched with `--main-pack`; original game `.pck`/embedded packs are
  not rewritten.
