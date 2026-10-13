# Third-Party Notices

This repository publishes original source code, tests, scripts, documentation,
and one reviewed Noto CJK font with its license. Third-party runtime payloads and TMP font asset bundles are
downloaded by the user with `scripts/install_runtime_payloads.ps1` after
unpacking the program package.

## Source releases

Source releases do not bundle executable runtime binaries. They include the
reviewed Noto CJK font and its OFL text alongside source code, tests, scripts,
documentation, and example configuration.

## Program packages

Program packages may include this project's own compiled executables and
plugins, including when they are embedded inside `ds游戏翻译器.exe`, plus the
reviewed Noto CJK font and scripts
that download runtime payloads from upstream projects. They should not directly
bundle third-party runtime payloads unless a release has an explicit
redistribution manifest and license texts.

## Never bundled

- Unity managed assemblies and IL2CPP interop assemblies copied from games or
  Unity installations: proprietary game or Unity runtime files.
- BepInEx and XUnity runtime folders in source releases. Users can install
  them with `scripts/install_runtime_payloads.ps1`.
- XUnity TMP font asset bundles in source releases. Users can install them
  with `scripts/install_runtime_payloads.ps1`.
- Other CJK font files: publish only when the exact font file and license are
  included and reviewed for the release.

## Bundled font

`assets/fonts/NotoSansCJKsc-Regular.otf` is an unmodified copy of Noto Sans
CJK SC Regular, version 2.004. Copyright notices are embedded in the font's
name table. It is licensed under SIL Open Font License 1.1; the full license is
`assets/fonts/OFL.txt`. The upstream commit, paths, and SHA-256 values are in
`assets/fonts/manifest.json`. The launcher embeds the font, license and manifest;
game copies carry a sibling `.OFL.txt` file, including a Godot patch companion
when that patch embeds the bundled font.
- w64devkit and other compiler/toolchain binaries: obtain from upstream.
- Game files, extracted game assemblies, decompiled game source, screenshots,
  translation caches, and translation memory TSV files: never publish.

## Referenced upstream projects

- BepInEx: https://github.com/BepInEx/BepInEx
- BepInEx bleeding-edge builds: https://builds.bepinex.dev/projects/bepinex_be
- HarmonyX: https://github.com/BepInEx/HarmonyX
- Harmony: https://github.com/pardeike/Harmony
- XUnity.AutoTranslator: https://github.com/bbepis/XUnity.AutoTranslator
- XUnity TMP font asset bundles: https://github.com/bbepis/XUnity.AutoTranslator/releases/tag/v5.5.0
- Newtonsoft.Json: https://github.com/JamesNK/Newtonsoft.Json
- Noto CJK fonts: https://github.com/googlefonts/noto-cjk
- w64devkit: https://github.com/skeeto/w64devkit

The project license in `LICENSE` applies only to this project's original
source code. Third-party files remain under their own licenses.
