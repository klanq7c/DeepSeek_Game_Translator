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
so users can update by replacing `ds翻译器.exe`. Third-party
BepInEx/XUnity/Newtonsoft payloads should normally be downloaded on the user's
machine by `scripts/install_runtime_payloads.ps1`.

If a future binary release directly bundles third-party runtime files, create a
manifest listing every included file, its upstream project, version, license,
source URL, and reason for redistribution. Without that manifest, keep the
runtime as a post-download installer step.
