# Open Source Release Checklist

Use this checklist before publishing the project or creating a public archive.
The working directory contains local runtime payloads and user data, so do not
publish it directly.

## Required release method

1. Run:

   ```powershell
   powershell -ExecutionPolicy Bypass -File scripts\prepare_open_source_release.ps1
   ```

2. The script runs `scripts\audit_open_source_release.ps1` automatically.

3. Publish only the generated source archive under `build\open_source\`.

4. Do not publish the live working directory unless the audit below is clean.

## Files that must not be published

- `config\api.ini`, `.env`, API keys, access tokens, cookies, credentials.
- `config\launcher.ini` and any local game paths.
- `translation_memory*.tsv`, game translation caches, generated static
  translations, or any file containing extracted game text.
- `logs\`, BepInEx logs, launcher logs, diagnostic dumps.
- `artifacts\`, decompiled game code, screenshots, temporary probes.
- Unity managed assemblies, Unity IL2CPP interop assemblies, or game DLLs.
- BepInEx, XUnity.AutoTranslator, .NET runtime, w64devkit, and other
  third-party binary payloads unless that exact release includes full license
  notices and redistribution review.
- Font files unless the exact file and license are included and reviewed.
- Generated binaries such as `.exe`, `.dll`, `.pdb`, `bin\`, and `obj\`.

## Safe source package contents

- Original C server and launcher source under `native\src\`.
- Original Unity plugin sources under:
  - `payloads\UnityTranslator\src\`
  - `payloads\UnityIL2CPP\DeepSeekXUnityTranslator\src\`
  - `payloads\UnityIL2CPP\DeepSeekTMPFontFallback\src\`
- Tests under `tests\`, excluding compiled test binaries.
- Build scripts, configuration examples, license, notices, and documentation.

## Manual audit commands

Run these before release:

```powershell
powershell -ExecutionPolicy Bypass -File scripts\audit_open_source_release.ps1 -Path build\open_source\DeepSeek_Game_Translator_source_preview
rg -n "sk-[A-Za-z0-9]{16,}|api[_-]?key\s*=\s*[^<\s][^\r\n]+|Authorization:\s*Bearer\s+[A-Za-z0-9._-]+|password\s*=\s*[^<\s][^\r\n]+|secret\s*=\s*[^<\s][^\r\n]+" build\open_source
rg -n "C:\\Users\\[A-Za-z0-9._-]+|E:\\Projects\\[^\\\r\n]+|/Users/[A-Za-z0-9._-]+|/home/[A-Za-z0-9._-]+" build\open_source
Get-ChildItem build\open_source -Recurse -Include *.dll,*.exe,*.pdb,*.otf,*.ttf,*.tsv,*.zip,*.7z
```

All three checks must be empty except for harmless source-code strings that are
documented in the release notes.

## Notes

The source package intentionally does not include third-party runtimes. Build
and deployment documentation should tell users where to obtain those files from
their upstream projects and how to point the build to local reference folders.
