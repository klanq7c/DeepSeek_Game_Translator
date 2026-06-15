# DS翻译器

版本：`0.3.1.7`

DS翻译器是一个本地游戏翻译工具，目标是支持：

- Ren'Py 游戏
- RPG Maker 游戏，包括旧版本和 MV/MZ 风格项目
- Unity 游戏，包括 Mono/BepInEx、BepInEx 5/6、IL2CPP/XUnity 相关路径

程序会在本机启动一个 C 语言本地翻译/缓存服务，并由启动器按游戏引擎部署对应 hook 或插件。设计目标是：缓存命中立即返回，缓存未命中时后台请求 API，尽量不让游戏运行时等待远程接口。

> 本项目与 DeepSeek、Unity、BepInEx、XUnity.AutoTranslator、Ren'Py、RPG Maker 或任何游戏厂商均无官方关联。相关名称只用于说明兼容目标。

## 下载

最新下载地址：

https://github.com/klanq7c/DeepSeek_Game_Translator/releases/tag/v0.3.1.7

推荐下载：

- `ds翻译器_0.3.1.7.zip`：最小可运行程序包，包含 `ds翻译器.exe` 和本地服务端。
- `DeepSeek_Game_Translator_source_0.3.1.7.zip`：源码包，只包含自有源码、测试和文档。

为了降低侵权和授权风险，下载包不内置 BepInEx、XUnity、Unity DLL、游戏文件、字体包、翻译记忆或 API key。Unity 等运行时依赖需要按文档从上游项目或你有权使用的本地环境准备。

## 使用方式

1. 解压 `ds翻译器_0.3.1.7.zip`。
2. 复制 `config/api.ini.example` 为 `config/api.ini`。
3. 在 `config/api.ini` 里填入自己的 API key。
4. 双击运行 `ds翻译器.exe`。
5. 在启动器里选择游戏目录，然后点击开始翻译。

本地服务默认监听：

```text
http://127.0.0.1:19999
```

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
