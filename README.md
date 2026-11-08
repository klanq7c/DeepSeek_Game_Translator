# ds游戏翻译器

**新用户推荐下载（Windows）：[下载 0.4.1.1 EXE，下载后双击运行](https://github.com/klanq7c/DeepSeek_Game_Translator/releases/download/v0.4.1.1/ds-translator_0.4.1.1.exe)。**

## 更新日志

### 2026-11-08（0.4.1.1）

- **API 连通检测**：配置窗口新增“检测连通”，可在保存前验证当前地址、模型和 Key，并查看认证、权限、限流、超时等错误提示。
- **本地模型适配**：延长 Ollama、LM Studio 等本机服务的首次响应等待，显示模型加载提示；标准本机 Ollama 接口请求关闭思考模式，本地翻译默认采用较小批次，也可调整批次预算。
- **API 配置体验**：模型列表显示提供商返回的名称与 API ID，刷新保留当前选择；切换提供商时分别保留各家的 Key 草稿。配置和提示窗口统一为星夜主题。
- **半汉化补译**：继续翻译中英混合文本中的剩余英文，保护原有中文、变量和格式标记。
- **Ren'Py**：改进中文字体补字，保留游戏原有字体、图标和字体组规则；修复嵌套插值、长目录预热漏句，并减少无关文本的重复刷新。
- **RPG Maker**：MV/MZ 新增部分菜单及 UI 图片文字的自动识别与翻译；改进插件文本、中文换行及颜色控制符处理，修复命令字段次序不同造成的预热漏句。
- **Unity**：改进 Mono 与 IL2CPP 的富文本、中文字体及动态界面适配，修复字体覆盖层的颜色跟随和缓存读取失败后的覆盖问题，并优化可选深预取的扫描预算。
- **Godot**：完善富文本标签保护、导出资源采集和启动预检，减少格式损坏及错误回退。
- **启动与缓存稳定性**：启动、还原、清缓存和服务启停在后台执行，避免同时操作互相干扰；改进长路径文件操作，严格检查提供商响应与缓存记录，避免异常内容进入译文缓存。

[更新详情与升级方法](docs/UPDATE_2026-11-08.md)

### 2026-10-13（0.4.1.0）

- **启动页重新设计**：加入星夜主题、暖白操作卡片和悬停动画；后台、最小化或系统关闭动画时停止装饰刷新。
- **Godot**：修复菜单动画写回英文、设置文字被标签拆分和部分缓存未命中影响显示的问题；改进动态控件、场景切换、导出包识别与启动预检。
- **其他引擎**：修复 Ren'Py 旧版刷新和迟到响应问题、RPG Maker 菜单补译与断线恢复；改进 Unity 动态菜单、富文本保护及 IL2CPP 中文字体回退。
- **API 与缓存**：支持实时刷新模型目录并保留手动选择；修复 Unicode 模型名保存、响应超时、缓存写入失败恢复和不完整末行追加问题。
- **运行开销**：减少启动器绘图、Godot 文本解析和 Unity 控件扫描中的重复工作。

[更新详情与升级方法](docs/UPDATE_2026-10-13.md)

### 2026-09-09（0.4.0.0）

- **API 配置**：提供商改为下拉选择，支持常见云端服务及本机 Ollama、LM Studio。
- **术语表**：可为人名、武器、技能等专有名词指定固定译名。
- **Godot**：导出游戏使用外部翻译补丁包，保留原始游戏包。
- **兼容性修复**：修复部分 Unity IL2CPP 游戏被识别为 Mono，以及 RPG Maker 长文件扫描漏句的问题。

### 2026-08-01（0.3.3.8）

- **Unity**：改进部分过度裁剪游戏的文本翻译，修复部分版本的字体编码问题，并优化批量文本处理。
- **Godot**：修复通用翻译模块中的兼容问题。
- **RPG Maker**：更新游戏支持并修复翻译偏移问题。

### 2026-07-15（0.3.2.7）

- **Godot**：新增引擎翻译支持，后续持续完善兼容性。
- **Ren'Py**：改善多选项语句的翻译延迟。
- **RPG Maker**：修复长译文溢出文本框及文本刷新问题。
- **Unity**：改进游戏兼容性。

## 项目简介

ds游戏翻译器是一个本地游戏翻译工具，目标是支持：

- Ren'Py 游戏
- RPG Maker 游戏，包括旧版本和 MV/MZ 风格项目
- Unity 游戏，包括 Mono/BepInEx、BepInEx 5/6、IL2CPP/XUnity 相关路径
- Godot 游戏，当前支持导出包/工程识别，并可扫描 `.po`、`.csv`、`.gd`、`.tscn`、`.tres`、`.translation` 和 `.pck` 中的可翻译文本做缓存预热

程序会在本机启动一个 C 语言本地翻译/缓存服务，并由启动器按游戏引擎部署对应 hook 或插件。设计目标是：缓存命中立即返回，缓存未命中时后台请求 API，尽量不让游戏运行时等待远程接口。

> 本项目与 DeepSeek、Unity、BepInEx、XUnity.AutoTranslator、Ren'Py、RPG Maker、Godot 或任何游戏厂商均无官方关联。相关名称只用于说明兼容目标。

## 下载

### 推荐下载

**[下载 ds游戏翻译器 0.4.1.1.exe（Windows，下载后双击运行）](https://github.com/klanq7c/DeepSeek_Game_Translator/releases/download/v0.4.1.1/ds-translator_0.4.1.1.exe)**

只需下载这个 EXE，放到一个固定文件夹后双击打开，再配置 API 并选择游戏目录。
浏览器保存的文件名为 `ds-translator_0.4.1.1.exe`。首次运行会自动释放所需的自有组件；
Unity 游戏另需按下方说明安装对应运行组件。

备用：[下载 ZIP 程序包，解压后运行](https://github.com/klanq7c/DeepSeek_Game_Translator/releases/download/v0.4.1.1/ds-translator_0.4.1.1.zip)。
它与 EXE 的程序功能相同，附带说明文档，**二选一即可**。

源码和校验文件供开发或检查文件使用。普通使用无需下载它们，也无需下载
GitHub 附件区自动显示的 `Source code (zip)` 或 `Source code (tar.gz)`。

[全部附件与更新说明](https://github.com/klanq7c/DeepSeek_Game_Translator/releases/tag/v0.4.1.1)

为了降低侵权和授权风险，下载包不直接内置 BepInEx、XUnity、Unity 官方 DLL、游戏文件、TMP 字体包、翻译记忆或 API key。Unity 第三方运行时和 XUnity TMP 字体 AssetBundle 由用户通过命令行脚本从上游项目下载。

## 使用方式

1. 下载 `ds-translator_0.4.1.1.exe`，或解压 `ds-translator_0.4.1.1.zip` 后运行里面的 `ds游戏翻译器.exe`。更新已有安装时，将单文件启动器改名为 `ds游戏翻译器.exe`，替换原目录中的同名程序，再重新启动翻译器及游戏。
2. 首次运行时，启动器会自动生成/更新这些自有组件：
   - `native\dst_server.exe`
   - `native\dst_server_cs.exe`
   - `scripts\install_runtime_payloads.ps1`
   - `config\api.ini.example`
   - `config\launcher.ini.example`
   - `config\glossary.example.tsv`
   - 本项目自有 Unity 插件 DLL
3. 在启动器里点击“配置 API”，选择提供商并填写自己的 API key。点击“刷新模型”从当前厂商获取最新目录，在可编辑下拉框中自主选择模型，也可以直接填写模型 ID。已有 Key 的配置打开时自动刷新；更换厂商、地址或 Key 后请重新刷新。支持任意 OpenAI 兼容服务（DeepSeek、硅基流动、Moonshot、智谱、阿里百炼、OpenAI、Claude、Gemini 及本地 Ollama/LM Studio 等），本地回环服务的 key 可留空。

   提供商预设只填写地址，不再内置容易过期的模型名。切换到不同厂商地址后需重新选模型；刷新目录保留当前选择，显示获取时间，并提示当前模型是否仍在目录中。未列出的别名仍可手动保存。请选用支持聊天补全的模型，保存后重启翻译服务生效。

   填写完成后，可点击“检测连通”发送一次短请求验证是否可用；检测不会自动保存配置，云端检测可能产生少量费用。本机模型首次加载时请等待窗口提示。

   Claude、百炼的目录会读取后续分页，硅基流动会筛选聊天模型。百炼使用所填地址对应的目录；如果账号要求业务空间地址，请填写控制台提供的兼容接口完整地址。厂商未开放目录、Key 无权限、分页不完整或网络失败时，窗口会显示原因并保留手动输入，不会用旧列表伪装刷新成功。目录来自当前 API 地址和账号权限，不代表所有厂商公开发布的模型。
4. 如需翻译 Unity 游戏，在程序所在目录运行：

```powershell
powershell -ExecutionPolicy Bypass -File scripts\install_runtime_payloads.ps1 -All
```

5. 回到启动器，选择游戏目录，然后点击开始翻译。
6. 需要撤销部署时，先完全退出游戏，选择同一目录并点击“还原游戏”。启动器只移除能够确认由本程序部署的翻译文件，不会删除翻译缓存、用户模组或已有的 BepInEx；无法确认归属的 Unity 文件会保留并写入日志。
7. CACHE 状态卡显示共享缓存大小。点击旁边的“清除缓存”可删除 `translation_memory_c.tsv`；程序会先停止本地服务并在完成后恢复原运行状态。正在运行的游戏需要重启才能清除其进程内存缓存。

Ren'Py、RPG Maker 和 Godot 路径不需要下载 BepInEx/XUnity。Unity 路径如果缺少 payload，启动器日志会提示对应的安装命令。Godot 会扫描工程/导出包里的可翻译文本做缓存预热，并对导出的 `.pck` 生成外部补丁包（不改原包）。

完整用户说明见 `docs/USER_GUIDE.md`。
源码目录可运行 `scripts/audit_game_translation.ps1 -GamePath '游戏目录'` 做只读覆盖率与标记检查；它会给出原目录游戏 exe 的完整路径，并明确标记人工校对、字形排版和实际启动尚未验证。

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
model=YOUR_MODEL_ID
key=YOUR_API_KEY_HERE
timeout_ms=15000
concurrency=4
```

以上以 DeepSeek 地址为例，`YOUR_MODEL_ID` 需要替换为账号可用的聊天模型 ID。点击“刷新模型”获取实时目录；提供商返回显示名称时，列表同时显示名称与 API ID，保存的仍是 API ID。更多提供商地址见 `config\api.ini.example` 的注释。

真实的 `config/api.ini` 不要提交到仓库，也不要发给别人。

不同提供商需要各自的 API Key。切换提供商预设时，窗口会隔离各家的 Key；切回时恢复本次窗口内填写过的对应 Key。只有点击“保存配置”才写入当前配置，取消不会修改配置文件。401 提示认证问题，403 提示访问限制；请按窗口提示检查当前厂商的 Key、权限与连接。

## 当前状态

当前发布版本为 `0.4.1.1`。主要源码路径：

- `native/src/server/`、`native/src/launcher/`：本地 C 服务端和 Windows 启动器（当前发布主线）。
- `native/src/core/`：`DstCore` 共享核心（文本规则、JSON、缓存编解码、HTTP 契约常量），
  被 C# 服务端、C# 启动器和 Unity IL2CPP 端点以源文件链接方式复用。
- `native/src/server_cs/`：C# 平行服务端（与 C 服务端同一 HTTP 契约，可通过
  `config/launcher.ini` 的 `[server] binary=cs` 启用；已随启动器嵌入分发）。
- `native/src/launcher_cs/`：C# 启动器移植（功能已齐全，仍非发布主线）。已移植引擎识别、五种引擎
  （Ren'Py / RPG Maker MV/MZ / Unity Mono / Unity IL2CPP / Godot）的部署与还原、
  服务端进程管理、五种引擎的预热扫描（Ren'Py 脚本、RPG Maker 数据/外部文本、
  Unity XUnity 译文文件 + 资源/bundle、Godot 工程资源 + .translation + PCK 1/2/3/4 +
  内嵌 pck 的 EXE；两侧通过 `--warmup-and-exit`/`--warmup` 转储模式逐字节比对批次
  请求体）、内嵌 payload 自更新（同一批 14 个自有组件与已审核字体资源以 `EmbeddedResource` 嵌入，
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
  （默认 `dst_server.exe` 与 `dst_server_cs.exe`）执行相同的 HTTP 契约检查。
- `tests/payload_scripts/check_payload_scripts.ps1`：钩子脚本的字节卫生、
  语法、锚点与端点契约检查。

## 开源发布安全规则

发布源码包前运行：

```powershell
powershell -ExecutionPolicy Bypass -File scripts\prepare_open_source_release.ps1 -Version 0.4.1.1
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
