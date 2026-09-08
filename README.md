# ds游戏翻译器

## 更新日志

后续更新请继续把最新更新日志放在本节最上方。

### 20260909（0.4.0.0）

1、启动器配置 API 改成了提供商下拉，DeepSeek、硅基流动、Kimi、智谱、阿里百炼都能直接选，也可以接本机的 Ollama / LM Studio。
2、增加了术语表，人名、武器、技能这些专有名词可以固定成指定译名。
3、对 Godot 的通用翻译模块做了比较大的更新，导出游戏会生成外部补丁包来汉化，不会改原包。
4、修复了部分 Unity IL2CPP 游戏被识别成 Mono 的问题，也修了 RPG Maker 扫描长文件时可能漏句的问题。

版本：`0.4.0.0`

### 20260801（0.3.3.8）

1、对于部分unity游戏过度裁剪导致的文本翻译问题进行了优化。
2、对于部分unity版本的字体编码问题进行了修复，并且对翻译文本阵列优化了一下。
3、对于godot的通用翻译模块进行了优化，之前的翻译模块存在问题。
4、对于rpgm游戏进行了更新支持，修复了出现的翻译偏移问题。

版本：`0.3.3.8`

### 20260715（0.3.2.7）

1. 添加了对 Godot 引擎的翻译支持（现在还在优化阶段）。
2. 解决了 Ren'Py 翻译模块对于多选择项语句的翻译延迟问题。
3. 解决了 RPG Maker 模块对于长文本翻译的文本框溢出以及文本刷新问题。
4. 对 Unity 游戏的支持项目进行了优化。

版本：`0.3.2.7`

ds游戏翻译器是一个本地游戏翻译工具，目标是支持：

- Ren'Py 游戏
- RPG Maker 游戏，包括旧版本和 MV/MZ 风格项目
- Unity 游戏，包括 Mono/BepInEx、BepInEx 5/6、IL2CPP/XUnity 相关路径
- Godot 游戏，当前支持导出包/工程识别，并可扫描 `.po`、`.csv`、`.gd`、`.tscn`、`.tres`、`.translation` 和 `.pck` 中的可翻译文本做缓存预热

程序会在本机启动一个 C 语言本地翻译/缓存服务，并由启动器按游戏引擎部署对应 hook 或插件。设计目标是：缓存命中立即返回，缓存未命中时后台请求 API，尽量不让游戏运行时等待远程接口。

> 本项目与 DeepSeek、Unity、BepInEx、XUnity.AutoTranslator、Ren'Py、RPG Maker、Godot 或任何游戏厂商均无官方关联。相关名称只用于说明兼容目标。

## 下载

最新下载地址：

https://github.com/klanq7c/DeepSeek_Game_Translator/releases/tag/v0.4.0.0

推荐下载：

- `ds游戏翻译器_0.4.0.0.exe`：单文件启动器。首次运行会自动释放/更新本项目自有服务端、脚本、示例配置和自有 Unity 插件。
- `ds游戏翻译器_0.4.0.0.zip`：带说明文档和许可文件的 Windows 程序包，核心仍是 `ds游戏翻译器.exe`。
- `DeepSeek_Game_Translator_source_0.4.0.0.zip`：源码包，只包含自有源码、测试和文档。

为了降低侵权和授权风险，下载包不直接内置 BepInEx、XUnity、Unity 官方 DLL、游戏文件、TMP 字体包、翻译记忆或 API key。Unity 第三方运行时和 XUnity TMP 字体 AssetBundle 由用户通过命令行脚本从上游项目下载。

## 使用方式

1. 下载 `ds游戏翻译器_0.4.0.0.exe`，或解压 `ds游戏翻译器_0.4.0.0.zip` 后运行里面的 `ds游戏翻译器.exe`。
2. 首次运行时，启动器会自动生成/更新这些自有组件：
   - `native\dst_server.exe`
   - `native\dst_server_cs.exe`
   - `scripts\install_runtime_payloads.ps1`
   - `config\api.ini.example`
   - `config\launcher.ini.example`
   - `config\glossary.example.tsv`
   - 本项目自有 Unity 插件 DLL
3. 在启动器里点击“配置 API”，选择提供商并填写自己的 API key。支持任意 OpenAI 兼容服务（DeepSeek、硅基流动、Moonshot、智谱、阿里百炼、OpenAI、Claude、Gemini 及本地 Ollama/LM Studio 等），本地回环服务的 key 可留空。
4. 如需翻译 Unity 游戏，在程序所在目录运行：

```powershell
powershell -ExecutionPolicy Bypass -File scripts\install_runtime_payloads.ps1 -All
```

5. 回到启动器，选择游戏目录，然后点击开始翻译。
6. 需要撤销部署时，先完全退出游戏，选择同一目录并点击“还原游戏”。启动器只移除能够确认由本程序部署的翻译文件，不会删除翻译缓存、用户模组或已有的 BepInEx；无法确认归属的 Unity 文件会保留并写入日志。
7. CACHE 状态卡显示共享缓存大小。点击旁边的“清除缓存”可删除 `translation_memory_c.tsv`；程序会先停止本地服务并在完成后恢复原运行状态。正在运行的游戏需要重启才能清除其进程内存缓存。

Ren'Py、RPG Maker 和 Godot 路径不需要下载 BepInEx/XUnity。Unity 路径如果缺少 payload，启动器日志会提示对应的安装命令。Godot 会扫描工程/导出包里的可翻译文本做缓存预热，并对导出的 `.pck` 生成外部补丁包（不改原包）。

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

从 `0.3.2.7` 起，启动器会把本项目自有组件嵌入 `ds游戏翻译器.exe`。大多数更新只需要替换 `ds游戏翻译器.exe`，再次启动后它会自动同步：

- `native\dst_server.exe`
- `native\dst_server_cs.exe`
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
model=deepseek-v4-flash
key=YOUR_API_KEY_HERE
timeout_ms=15000
concurrency=4
```

以上以 DeepSeek 为例；任意 OpenAI 兼容端点均可使用，更多提供商地址见 `config\api.ini.example` 的注释。

真实的 `config/api.ini` 不要提交到仓库，也不要发给别人。

## 当前状态

这是 `0.4.0.0` 预览版。主要源码路径：

- `native/src/server/`、`native/src/launcher/`：本地 C 服务端和 Windows 启动器（当前发布主线）。
- `native/src/core/`：`DstCore` 共享核心（文本规则、JSON、缓存编解码、HTTP 契约常量），
  被 C# 服务端、C# 启动器和 Unity IL2CPP 端点以源文件链接方式复用。
- `native/src/server_cs/`：C# 平行服务端（与 C 服务端同一 HTTP 契约，可通过
  `config/launcher.ini` 的 `[server] binary=cs` 启用；已随启动器嵌入分发）。
- `native/src/launcher_cs/`：C# 启动器移植（功能已齐全，仍非发布主线）。已移植引擎识别、五种引擎
  （Ren'Py / RPG Maker MV/MZ / Unity Mono / Unity IL2CPP / Godot）的部署与还原、
  服务端进程管理、五种引擎的预热扫描（Ren'Py 脚本、RPG Maker 数据/外部文本、
  Unity XUnity 译文文件 + 资源/bundle、Godot 工程资源 + .translation + PCK 1/2/3 +
  内嵌 pck 的 EXE；两侧通过 `--warmup-and-exit`/`--warmup` 转储模式逐字节比对批次
  请求体）、内嵌 payload 自更新（同一批十个自有文件以 `EmbeddedResource` 嵌入，
  `--sync-payloads-and-exit`/`--sync-payloads` 释放出的目录树逐字节一致）、
  Godot 外部补丁包（`.pck`/内嵌包复制后修补文本资源、GDScript 字节码、
  OptimizedTranslation 与字体条目，格式 3 追加运行时 autoload；两侧构建出的
  `dst_godot_patch.pck` 与发往 `/batch` 的请求体逐字节一致）、`api.ini` 读写
  （供应商预设表与 Profile API 读写，两侧写出的 `api.ini` 逐字节一致）、
  Godot 启动预检（`--main-pack` 拒绝判定与预检结论缓存，两侧写出的
  `config\godot_preflight.ini` 逐字节一致）、一键翻译流程（各引擎的启动/预热
  顺序、Godot 的三条翻译启动路径与三个无头预检、独立补丁刷新进程、缓存卡片与
  清缓存；两侧的日志、状态推进、预热批次、预检命令行与将要拉起的进程命令行
  逐字节一致）、Win32 窗口本体（调色板/DPI 缩放/GDI 绘制原语/布局/悬停动画/
  自绘按钮/消息循环，以及目录选择与 API 配置对话框；全部走 P/Invoke 原生 GDI，
  不使用 WinForms，两侧渲染出的客户区位图逐像素一致）。启动器移植已无 C 独有
  模块；今后新增模块若未移植，入口必须明确返回 `NotPorted`，不得伪装成功。
- `payloads/RenPy/`、`payloads/RPGMaker/`、`payloads/Godot/`：引擎钩子脚本源文件
  （`.rpy`/`.js`/`.gd`），构建时以 RCDATA 资源嵌入启动器。
- `payloads/UnityTranslator/src/`：Unity Mono/BepInEx 插件源码。
- `payloads/UnityIL2CPP/DeepSeekXUnityTranslator/src/`：Unity IL2CPP/XUnity 本地批量端点源码。
- `payloads/UnityIL2CPP/DeepSeekTMPFontFallback/src/`：Unity IL2CPP TMP 字体兜底源码。

## 构建

本地开发需要：

- Windows C 工具链，例如 w64devkit。可以把 `gcc.exe`/`windres.exe` 所在目录加入 `PATH`，也可以放在 `native/toolchain/w64devkit/bin/`。
- .NET SDK。
- 如需从 source-only 源码包构建完整 Unity 功能，先运行 `scripts\install_runtime_payloads.ps1 -All` 下载 BepInEx/XUnity/Newtonsoft/TMP 字体包。
- 构建 Unity Mono 插件时，需要设置 `UNITY_MANAGED_DIR` 指向目标 Unity 版本的 `Managed` DLL 目录；构建 IL2CPP TMP 字体兜底插件时，需要设置 `IL2CPP_INTEROP_DIR` 指向 BepInEx 为目标游戏生成的 interop DLL 目录。
- 如果只是直接使用 Release 程序包，`ds游戏翻译器.exe` 已嵌入本项目自有服务端和一方 Unity 插件 DLL，不需要自己编译这些源码。

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

旧的 `tests/` 回归套件已于 2026-09-06 移除；2026-09-08 起随语言迁移重建为
以下守卫，前两项由 `build_native.bat` 自动执行：

- `tests/core_tests/`：`DstCore` 与 C 实现的 golden 对拍（文本规则、JSON、
  缓存编解码、契约常量）。
- `tests/launcher_parity/run_launcher_parity.ps1`：C 与 C# 启动器在合成游戏
  目录上的识别/部署/还原输出与目录树哈希逐项比对，另含预热批次、Godot 补丁包、
  一键流程与窗口渲染（布局报告 + 客户区位图哈希）比对；加 `-ServerSmoke` 可额外
  验证服务端启动/健康检查/关闭（需 19999 端口空闲）。
- `tests/server_contract/run_contract_tests.ps1`：以假 provider 对两套服务端
  （默认 `dst_server.exe` 与 `dst_server_cs.exe`）各跑 50 项 HTTP 契约检查。
- `tests/payload_scripts/check_payload_scripts.ps1`：钩子脚本的字节卫生、
  语法、锚点与端点契约检查。

## 开源发布安全规则

发布源码包前运行：

```powershell
powershell -ExecutionPolicy Bypass -File scripts\prepare_open_source_release.ps1 -Version 0.4.0.0
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
