# DeepSeek Game Translator

DeepSeek Game Translator is a local game translation tool for Ren'Py,
RPG Maker, and Unity games. It runs a local C translation/cache server and
deploys engine-specific hooks or plugins so games can translate visible text
without blocking the game on remote API latency.

This repository is prepared for source-only open-source publication. The
working tree may contain local payloads, game caches, API keys, logs, and
third-party runtime files; do not publish the working directory directly.

This project is not affiliated with DeepSeek, Unity, BepInEx,
XUnity.AutoTranslator, Ren'Py, RPG Maker, or any game publisher. See
`TRADEMARKS.md`.

## Current Status

This project is best treated as an alpha/preview build. The core paths are:

- Native C local server and launcher under `native/src/`.
- Unity Mono/BepInEx plugin source under `payloads/UnityTranslator/src/`.
- Unity IL2CPP/XUnity endpoint source under
  `payloads/UnityIL2CPP/DeepSeekXUnityTranslator/src/`.
- Unity IL2CPP TMP font fallback source under
  `payloads/UnityIL2CPP/DeepSeekTMPFontFallback/src/`.
- Regression tests under `tests/`.

## Runtime Contract

- The local server listens on `http://127.0.0.1:19999` by default.
- Cache hits return immediately.
- Cache misses are queued for background API work and should not block the
  game runtime.
- Engine hooks must preserve tags, variables, control sequences, and
  renderer-specific behavior.

## Configuration

Copy `config/api.ini.example` to `config/api.ini` and set your own API key.
Never commit the real `config/api.ini`.

```ini
[api]
endpoint=https://api.deepseek.com/v1/chat/completions
model=deepseek-chat
key=YOUR_API_KEY_HERE
timeout_ms=15000
concurrency=4
```

## Build

Local development currently expects:

- A Windows C toolchain such as w64devkit, available at
  `native/toolchain/w64devkit` or on `PATH`.
- .NET SDK for C# plugin builds.
- Local Unity/BepInEx/XUnity reference DLLs when building Unity payloads.

The repository does not publish third-party runtime binaries or Unity/game
assemblies in source releases. See `THIRD_PARTY_NOTICES.md` and
`docs/DEPENDENCY_POLICY.md`.

```bat
build_native.bat
```

## Tests

```powershell
powershell -ExecutionPolicy Bypass -File tests\run_all.ps1 -SkipEndurance
```

## Open-Source Release

Generate a source-only release package with:

```powershell
powershell -ExecutionPolicy Bypass -File scripts\prepare_open_source_release.ps1
```

Publish only the generated archive under `build\open_source\`. See
`OPEN_SOURCE_RELEASE.md` for the release audit checklist. The package script
also runs `scripts\audit_open_source_release.ps1`.

## Contributing Safely

Read `CONTRIBUTING.md` before submitting changes. Tests must use synthetic
fixtures rather than copied game dialogue, screenshots, extracted scripts, or
translation memory.

## License

Original project source code is licensed under the MIT License. Third-party
components remain under their own licenses and are not bundled in source
releases unless explicitly reviewed for that release.
