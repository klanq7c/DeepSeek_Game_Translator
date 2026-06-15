# Contributing

This project accepts only original source code, synthetic test fixtures, and
documentation that can be redistributed openly.

## Do Not Commit

- API keys, tokens, cookies, credentials, or private endpoints.
- `config/api.ini`, `config/launcher.ini`, `.env`, or local machine paths.
- Game files, extracted game scripts, decompiled game code, screenshots, logs,
  crash dumps, save files, or any asset copied from a game.
- Translation memory, generated translations, static translation files, or
  cache files produced while playing a game.
- Unity managed assemblies, IL2CPP interop assemblies, BepInEx runtime files,
  XUnity.AutoTranslator runtime files, .NET runtime files, w64devkit binaries,
  or generated plugin/server/launcher binaries.
- Font files unless the exact file and redistribution license were explicitly
  reviewed for the release.

## Tests and Fixtures

Use synthetic strings for regression tests. A bug may be described in terms of
its shape, but do not paste copyrighted game dialogue or extracted game text
into tests.

Good:

```text
Ready or not, I just want to find the archive.
The trainee is calm, focused, and ready to start the next exercise.
```

Avoid:

```text
Real dialogue copied from a commercial or private game.
```

## Dependencies

Third-party dependencies must be referenced as external inputs or downloaded
from upstream by the user. Source releases intentionally omit runtime payloads.

## Before Opening a Change

Run:

```powershell
powershell -ExecutionPolicy Bypass -File tests\run_all.ps1 -SkipEndurance
powershell -ExecutionPolicy Bypass -File scripts\prepare_open_source_release.ps1 -Version local-check
```

The generated source package must pass the release audit with no secrets,
binary payloads, fonts, caches, or local paths.
