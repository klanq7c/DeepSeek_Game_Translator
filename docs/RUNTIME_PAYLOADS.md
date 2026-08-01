# 运行时 payload 安装

## Highly stripped Unity Mono players

Some Unity Mono games remove framework methods that BepInEx requires, such as
`System.IO.File.WriteAllText(string,string)`. Install the pinned official Mono
class-library payload with:

```powershell
powershell -ExecutionPolicy Bypass -File scripts\install_runtime_payloads.ps1 -UnityMonoCorlib
```

`-UnityMono5`, `-UnityMono6`, and `-All` also install this payload. The script
downloads Mono 6.12.0.206 from `download.mono-project.com`, verifies SHA-256
`4125f57d97cfa88257915edc969e913de198cd8e22396a29849037479a0ac368`,
administratively extracts the MSI, and copies only the required executable
4.5-profile class libraries to `payloads\UnityMonoCorlib`.

The launcher activates this payload only when the game's managed `mscorlib.dll`
clearly lacks the BepInEx-required writer. It deploys the files to the ASCII
relative path `BepInEx\unstripped_corlib`, updates Doorstop's search override,
and disables BepInEx Unity-log mirroring for that clearly stripped profile
because its Unity log callback APIs may also be absent. Config edits retain backups and ownership snapshots;
normal Unity Mono, BepInEx 5/6, IL2CPP, and other engines are unchanged.

For a clearly stripped BepInEx 5 player, the launcher also deploys this
project's first-party `DeepSeekUnityFontPatcher.dll`. The preloader patcher
restores only the missing in-memory
`Font.Internal_CreateDynamicFont(Font,string[],int)` declaration before the
text-rendering module loads. It does not download or redistribute Unity DLLs,
is not deployed to normal Mono/BepInEx 6/IL2CPP games, and uses an exact
`.dst-owned` snapshot so restore never deletes a user-modified patcher.

ds游戏翻译器的源码仓库和默认下载包不直接附带第三方运行时。用户下载单文件 `ds游戏翻译器.exe` 后，先运行一次启动器；启动器会自动生成 `scripts\install_runtime_payloads.ps1`。随后在程序所在目录运行下面的命令即可补齐 Unity 所需插件：

```powershell
powershell -ExecutionPolicy Bypass -File scripts\install_runtime_payloads.ps1 -All
```

按引擎单独安装也可以：

```powershell
# 旧版 Unity Mono / BepInEx 5
powershell -ExecutionPolicy Bypass -File scripts\install_runtime_payloads.ps1 -UnityMono5

# Unity 6+ Mono / BepInEx 6
powershell -ExecutionPolicy Bypass -File scripts\install_runtime_payloads.ps1 -UnityMono6

# Unity IL2CPP / BepInEx 6 + XUnity
powershell -ExecutionPolicy Bypass -File scripts\install_runtime_payloads.ps1 -UnityIL2CPP
```

脚本会从上游项目下载并安装到启动器期待的目录：

- `payloads/UnityMonoRuntime`
- `payloads/UnityMonoRuntimeX86`
- `payloads/UnityMonoRuntime6`
- `payloads/UnityMonoRuntime6X86`
- `payloads/UnityIL2CPP/BepInExRuntime`
- `payloads/UnityIL2CPP/XUnityAutoTranslator`
- `payloads/UnityIL2CPP/TMPFontAssetBundles/BepInEx/font`
- `payloads/UnityTranslator/Newtonsoft.Json.dll`
- `payloads/UnityTranslator/DeepSeekUnityFontPatcher.dll`（本项目自有构建产物，不是下载的 Unity DLL）

当前固定版本：

- BepInEx 5.4.23.5 x86/x64，用于旧版 Unity Mono。
- BepInEx 6.0.0-be.755+3fab71a x86/x64，用于 Unity 6+ Mono；x64 版本也用于 Unity IL2CPP。
- XUnity.AutoTranslator 5.6.1 IL2CPP。
- XUnity.ResourceRedirector 2.1.0 IL2CPP。
- XUnity TMP_Font_AssetBundles_2025-12-08，用于 Unity TMP 中文字体 fallback。
- Newtonsoft.Json 13.0.4。

这些文件会下载到用户本机，不进入源码仓库，也不会随源码包发布。脚本不会下载 Unity 官方 DLL、游戏文件、翻译记忆、日志或 API key。

`native\dst_server.exe`、`scripts\install_runtime_payloads.ps1`、示例配置、本项目自有 Unity 插件和 preloader patcher DLL 由 `ds游戏翻译器.exe` 自带并在启动时同步。以后更新通常只需要替换 `ds游戏翻译器.exe`；第三方运行时目录仍由本脚本负责。

面向普通用户的完整步骤见 `docs/USER_GUIDE.md`。
