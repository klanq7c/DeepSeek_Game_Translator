# Dependency and Redistribution Policy

The public source repository should be source-only. Runtime dependencies are
external inputs installed by the user with `scripts/install_runtime_payloads.ps1`.

## Allowed in Source Releases

- Original C source under `native/src/`.
- Original C# source under `payloads/**/src/`.
- PowerShell and C test sources.
- Documentation, license files, and configuration examples.

## Not Allowed in Source Releases

- BepInEx runtime folders.
- Mono runtime/class-library payloads, including `payloads/UnityMonoCorlib`.
- XUnity.AutoTranslator runtime folders.
- Unity managed assemblies and IL2CPP interop assemblies.
- Game assemblies or decompiled game source.
- w64devkit, .NET runtime files, compiler toolchains, or downloaded archives.
- Generated `.exe`, `.dll`, `.pdb`, `.mdb`, `bin/`, and `obj/` output.
- CJK font files or TMP font bundles unless the exact font and license are
  reviewed and included in the release notices.
- User caches, translation memory, logs, diagnostics, or local configuration.

## Program Packages

Program packages may include this project's own compiled launcher, server, and
plugin binaries. These first-party binaries may be embedded inside the launcher
so users can update by replacing `ds游戏翻译器.exe`. Third-party
BepInEx/XUnity/Newtonsoft payloads and TMP font asset bundles should normally
be downloaded on the user's machine by `scripts/install_runtime_payloads.ps1`.
The same rule applies to the pinned official Mono corlib used only for clearly
stripped Unity Mono players: the installer verifies the upstream MSI hash and
extracts a minimal runtime allowlist on the user's machine.

`DeepSeekUnityFontPatcher.dll` is a first-party BepInEx 5 preloader patcher.
It adds one missing method declaration to the in-memory
`UnityEngine.TextRenderingModule` metadata only for clearly stripped players.
It does not contain, download, copy, replace, or redistribute a Unity assembly.
The launcher deploys it with an exact ownership snapshot and preserves any
unowned or user-modified file at the same path.

If a future binary release directly bundles third-party runtime files, create a
manifest listing every included file, its upstream project, version, license,
source URL, and reason for redistribution. Without that manifest, keep the
runtime as a post-download installer step.
