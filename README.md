# ds游戏翻译器

版本：`0.3.1.7`

ds游戏翻译器是一个本地游戏翻译工具，目标是支持：

- Ren'Py 游戏
- RPG Maker 游戏，包括旧版本和 MV/MZ 风格项目
- Unity 游戏，包括 Mono/BepInEx、BepInEx 5/6、IL2CPP/XUnity 相关路径

程序会在本机启动一个 C 语言本地翻译/缓存服务，并由启动器按游戏引擎部署对应 hook 或插件。设计目标是：缓存命中立即返回，缓存未命中时后台请求 API，尽量不让游戏运行时等待远程接口。

> 本项目与 DeepSeek、Unity、BepInEx、XUnity.AutoTranslator、Ren'Py、RPG Maker 或任何游戏厂商均无官方关联。相关名称只用于说明兼容目标。

## 下载

最新下载地址：

https://github.com/klanq7c/DeepSeek_Game_Translator/releases/tag/v0.3.1.7

推荐下载：

- `ds游戏翻译器_0.3.1.7.exe`：单文件启动器。首次运行会自动释放/更新本项目自有服务端、脚本、示例配置和自有 Unity 插件。
- `ds游戏翻译器_0.3.1.7.zip`：带说明文档和许可文件的 Windows 程序包，核心仍是 `ds游戏翻译器.exe`。
- `DeepSeek_Game_Translator_source_0.3.1.7.zip`：源码包，只包含自有源码、测试和文档。

为了降低侵权和授权风险，下载包不直接内置 BepInEx、XUnity、Unity 官方 DLL、游戏文件、字体包、翻译记忆或 API key。Unity 第三方运行时由用户通过命令行脚本从上游项目下载。

## 使用方式

1. 下载 `ds游戏翻译器_0.3.1.7.exe`，或解压 `ds游戏翻译器_0.3.1.7.zip` 后运行里面的 `ds游戏翻译器.exe`。
2. 首次运行时，启动器会自动生成/更新这些自有组件：
   - `native\dst_server.exe`
   - `scripts\install_runtime_payloads.ps1`
   - `config\api.ini.example`
   - 本项目自有 Unity 插件 DLL
3. 在启动器里点击“配置 API”，填写自己的 API key。
4. 如需翻译 Unity 游戏，在程序所在目录运行：

```powershell
powershell -ExecutionPolicy Bypass -File scripts\install_runtime_payloads.ps1 -All
```

5. 回到启动器，选择游戏目录，然后点击开始翻译。

Ren'Py 和 RPG Maker 路径不需要下载 BepInEx/XUnity。Unity 路径如果缺少 payload，启动器日志会提示对应的安装命令。

完整用户说明见 `docs/USER_GUIDE.md`。

本地服务默认监听：

```text
http://127.0.0.1:19999
```

## 按需安装 Unity 依赖

```powershell
# 旧版 Unity Mono / BepInEx 5
powershell -ExecutionPolicy Bypass -File scripts\install_runtime_payloads.ps1 -UnityMono5

# Unity 6+ Mono / BepInEx 6
powershell -ExecutionPolicy Bypass -File scripts\install_runtime_payloads.ps1 -UnityMono6

# Unity IL2CPP / BepInEx 6 + XUnity
powershell -ExecutionPolicy Bypass -File scripts\install_runtime_payloads.ps1 -UnityIL2CPP
```

详见 `docs/RUNTIME_PAYLOADS.md`。

## 更新方式

从 `0.3.1.7` 起，启动器会把本项目自有组件嵌入 `ds游戏翻译器.exe`。大多数更新只需要替换 `ds游戏翻译器.exe`，再次启动后它会自动同步：

- `native\dst_server.exe`
- `scripts\install_runtime_payloads.ps1`
- `config\*.example`
- `payloads\UnityTranslator\UnityTranslator.dll`
- `payloads\UnityTranslator\UnityTranslator.BepInEx6.dll`
- `payloads\UnityIL2CPP\DeepSeekXUnityTranslator\DeepSeekTranslate.dll`
- `payloads\UnityIL2CPP\DeepSeekTMPFontFallback\...\DeepSeekTMPFontFallback.dll`

不会自动覆盖真实的 `config\api.ini`、翻译记忆、日志、游戏目录或第三方运行时。若以后第三方依赖版本变化，启动器日志或发布说明会提示重新运行 `scripts\install_runtime_payloads.ps1 -All`。已经启动的游戏不会热更新插件 DLL，更新后请完全退出游戏再重新部署/启动。

## 配置示例

```ini
[api]
endpoint=https://api.deepseek.com/v1/chat/completions
model=deepseek-chat
key=YOUR_API_KEY_HERE
timeout_ms=15000
concurrency=4
```

真实的 `config/api.ini` 不要提交到仓库，也不要发给别人。

## 当前状态

这是 `0.3.1.7` 预览版。主要源码路径：

- `native/src/`：本地 C 服务端和 Windows 启动器。
- `payloads/UnityTranslator/src/`：Unity Mono/BepInEx 插件源码。
- `payloads/UnityIL2CPP/DeepSeekXUnityTranslator/src/`：Unity IL2CPP/XUnity 本地批量端点源码。
- `payloads/UnityIL2CPP/DeepSeekTMPFontFallback/src/`：Unity IL2CPP TMP 字体兜底源码。
- `tests/`：回归测试。

## 构建

本地开发需要：

- Windows C 工具链，例如 w64devkit。
- .NET SDK。
- 构建 Unity 插件时，需要本地 Unity/BepInEx/XUnity 引用 DLL。

源码仓库不会发布第三方运行时二进制或 Unity/游戏程序集。依赖边界见：

- `THIRD_PARTY_NOTICES.md`
- `docs/DEPENDENCY_POLICY.md`
- `docs/RUNTIME_PAYLOADS.md`
- `docs/USER_GUIDE.md`

构建命令：

```bat
build_native.bat
```

## 测试

```powershell
powershell -ExecutionPolicy Bypass -File tests\run_all.ps1 -SkipEndurance
```

## 开源发布安全规则

发布源码包前运行：

```powershell
powershell -ExecutionPolicy Bypass -File scripts\prepare_open_source_release.ps1 -Version 0.3.1.7
```

该脚本会生成 source-only 包，并自动检查是否误带：

- API key
- 本地路径
- 翻译记忆和缓存
- 日志
- 字体
- DLL/EXE/PDB
- BepInEx/XUnity/Unity runtime
- 游戏内容

贡献代码前请先阅读 `CONTRIBUTING.md` 和 `SECURITY.md`。测试用例应使用合成文本，不要复制商业游戏对白、脚本或截图。

## 许可证

本项目自有源码使用 MIT License。第三方组件保留其各自许可证。
