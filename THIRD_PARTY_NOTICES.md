# Third-Party Notices

This repository is intended to publish only original source code, tests, and
documentation. Runtime payloads and build references are not included in the
open-source source package unless their redistribution has been reviewed for
that specific release.

## Not bundled in source releases

- Unity managed assemblies and IL2CPP interop assemblies: proprietary game or
  Unity runtime files. Do not commit or redistribute them in this repository.
- BepInEx runtime payloads: upstream project is licensed separately
  (BepInEx states LGPL-2.1 for the main project).
- XUnity.AutoTranslator runtime payloads: upstream project is licensed
  separately and should be downloaded from upstream releases by users.
- Harmony/HarmonyX, MonoMod, Mono.Cecil, Newtonsoft.Json, .NET runtime files,
  and other DLL dependencies: obtain from upstream packages or runtime bundles.
- Noto or other CJK font files: publish only when the exact font file and
  license are included and reviewed for the release.
- w64devkit and other compiler/toolchain binaries: obtain from upstream.
- Game files, extracted game assemblies, decompiled game source, screenshots,
  translation caches, and translation memory TSV files: never publish.

## Referenced upstream projects

- BepInEx: https://github.com/BepInEx/BepInEx
- HarmonyX: https://github.com/BepInEx/HarmonyX
- Harmony: https://github.com/pardeike/Harmony
- XUnity.AutoTranslator: https://github.com/bbepis/XUnity.AutoTranslator
- Newtonsoft.Json: https://github.com/JamesNK/Newtonsoft.Json
- Noto CJK fonts: https://github.com/googlefonts/noto-cjk
- w64devkit: https://github.com/skeeto/w64devkit

The project license in `LICENSE` applies only to this project's original
source code. Third-party files remain under their own licenses.
