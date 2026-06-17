# 运行时 payload 安装

DS翻译器的源码仓库和默认下载包不直接附带第三方运行时。用户下载单文件 `ds翻译器.exe` 后，先运行一次启动器；启动器会自动生成 `scripts\install_runtime_payloads.ps1`。随后在程序所在目录运行下面的命令即可补齐 Unity 所需插件：

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
- `payloads/UnityMonoRuntime6`
- `payloads/UnityIL2CPP/BepInExRuntime`
- `payloads/UnityIL2CPP/XUnityAutoTranslator`
- `payloads/UnityTranslator/Newtonsoft.Json.dll`

当前固定版本：

- BepInEx 5.4.23.5 x64，用于旧版 Unity Mono。
- BepInEx 6.0.0-be.755+3fab71a x64，用于 Unity 6+ Mono 和 Unity IL2CPP。
- XUnity.AutoTranslator 5.6.1 IL2CPP。
- XUnity.ResourceRedirector 2.1.0 IL2CPP。
- Newtonsoft.Json 13.0.4。

这些文件会下载到用户本机，不进入源码仓库，也不会随源码包发布。脚本不会下载 Unity 官方 DLL、游戏文件、字体包、翻译记忆、日志或 API key。

`native\dst_server.exe`、`scripts\install_runtime_payloads.ps1`、示例配置和本项目自有 Unity 插件 DLL 由 `ds翻译器.exe` 自带并在启动时同步。以后更新通常只需要替换 `ds翻译器.exe`；第三方运行时目录仍由本脚本负责。
