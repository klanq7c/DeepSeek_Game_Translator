# UnityTranslator Plugin Source

This directory contains the maintained source for the Unity Mono/BepInEx
translator plugin.

## Build Notes

The project file uses local reference folders for BepInEx, Harmony,
Newtonsoft.Json, TextMeshPro, and Unity engine assemblies. These dependencies
are not bundled in source releases. Obtain them from their upstream projects
or from a local game/Unity installation that you are allowed to use for
development.

To compile this project, provide a Unity managed assembly folder either by:

- copying the needed Unity DLLs into `UnityManagedRefs` next to this file, or
- passing `UnityManagedDir` to MSBuild/dotnet, for example:

```bat
dotnet build UnityTranslator.csproj -c Release -p:UnityManagedDir="C:\Path\To\Game_Data\Managed"
```

The repository-level `build_native.bat` also understands this setup:

- If `UNITY_MANAGED_DIR` is set, it rebuilds this plugin and copies the output
  to `payloads\UnityTranslator\UnityTranslator.dll`.
- If `UnityManagedRefs` exists next to this file, it uses that folder.
- If neither is available, it leaves the existing runtime DLL in place and
  continues building the launcher/server so other engine paths are not blocked.

Expected Unity-side references include:

- `UnityEngine.CoreModule.dll`
- `UnityEngine.TextRenderingModule.dll`
- `UnityEngine.UIModule.dll`
- `UnityEngine.UI.dll`
- `UnityEngine.AssetBundleModule.dll`
- `UnityEngine.IMGUIModule.dll`
- `UnityEngine.InputLegacyModule.dll`
- `Unity.TextMeshPro.dll`

Do not publish Unity engine assemblies, game assemblies, BepInEx runtime DLLs,
or generated plugin binaries in the source repository.
