# Dependency and Redistribution Policy

The public source repository should be source-only. Runtime dependencies are
external inputs unless a release explicitly includes a reviewed binary bundle
with complete notices.

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

## Binary Releases

A binary release is a separate legal/packaging decision from a source release.
Before publishing a binary release, create a manifest listing every included
file, its upstream project, version, license, source URL, and reason for
redistribution.

If that manifest cannot be produced, publish source only.
