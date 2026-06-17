# Third-Party Notices

This repository publishes only original source code, tests, scripts, and
documentation. Third-party runtime payloads are downloaded by the user with
`scripts/install_runtime_payloads.ps1` after unpacking the program package.

## Source releases

Source releases do not bundle runtime binaries. They only contain original
source code, tests, scripts, documentation, and example configuration.

## Program packages

Program packages may include this project's own compiled executables and
plugins, including when they are embedded inside `ds翻译器.exe`, plus scripts
that download runtime payloads from upstream projects. They should not directly
bundle third-party runtime payloads unless a release has an explicit
redistribution manifest and license texts.

## Never bundled

- Unity managed assemblies and IL2CPP interop assemblies copied from games or
  Unity installations: proprietary game or Unity runtime files.
- BepInEx and XUnity runtime folders in source releases. Users can install
  them with `scripts/install_runtime_payloads.ps1`.
- Noto or other CJK font files: publish only when the exact font file and
  license are included and reviewed for the release.
- w64devkit and other compiler/toolchain binaries: obtain from upstream.
- Game files, extracted game assemblies, decompiled game source, screenshots,
  translation caches, and translation memory TSV files: never publish.

## Referenced upstream projects

- BepInEx: https://github.com/BepInEx/BepInEx
- BepInEx bleeding-edge builds: https://builds.bepinex.dev/projects/bepinex_be
- HarmonyX: https://github.com/BepInEx/HarmonyX
- Harmony: https://github.com/pardeike/Harmony
- XUnity.AutoTranslator: https://github.com/bbepis/XUnity.AutoTranslator
- Newtonsoft.Json: https://github.com/JamesNK/Newtonsoft.Json
- Noto CJK fonts: https://github.com/googlefonts/noto-cjk
- w64devkit: https://github.com/skeeto/w64devkit

The project license in `LICENSE` applies only to this project's original
source code. Third-party files remain under their own licenses.
