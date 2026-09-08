# ds游戏翻译器使用说明

本文面向直接下载程序的用户。推荐下载 Release 页面里的 `ds游戏翻译器_0.4.0.0.exe`。

## 第一次使用

1. 把 `ds游戏翻译器_0.4.0.0.exe` 放到一个你准备长期使用的目录，例如 `D:\Games\DSTranslator\`。
2. 双击运行一次程序。首次运行会自动释放/更新本项目自带组件：
   - `native\dst_server.exe`
   - `native\dst_server_cs.exe`
   - `scripts\install_runtime_payloads.ps1`
   - `config\api.ini.example`
   - `config\launcher.ini.example`
   - `config\glossary.example.tsv`
   - 自有 Unity 插件 DLL
3. 在启动器中点击“配置 API”，从“提供商”下拉选择服务（预设会自动填好 API 地址和示例模型名，仍可手动修改），填入自己的 API Key 并保存。任意 OpenAI 兼容（chat/completions）服务均可使用；本机回环服务（Ollama、LM Studio）的 Key 可留空。
4. 如果要翻译 Ren'Py、RPG Maker 或 Godot 游戏，可以直接选择游戏目录并开始。
5. 如果要翻译 Unity 游戏，在程序所在目录运行：

```powershell
powershell -ExecutionPolicy Bypass -File scripts\install_runtime_payloads.ps1 -All
```

运行完成后回到启动器，或重新运行你下载的同一个 exe，选择游戏目录并部署。

### 支持哪些 API 提供商？

本地服务说 OpenAI 兼容（chat/completions）协议，预设包含：

| 提供商 | API 地址 |
| --- | --- |
| DeepSeek（默认） | `https://api.deepseek.com/v1/chat/completions` |
| 硅基流动 | `https://api.siliconflow.cn/v1/chat/completions` |
| Moonshot Kimi | `https://api.moonshot.cn/v1/chat/completions` |
| 智谱 GLM | `https://open.bigmodel.cn/api/paas/v4/chat/completions` |
| 阿里百炼 | `https://dashscope.aliyuncs.com/compatible-mode/v1/chat/completions` |
| OpenAI | `https://api.openai.com/v1/chat/completions` |
| Claude（OpenAI 兼容） | `https://api.anthropic.com/v1/chat/completions` |
| Gemini（OpenAI 兼容） | `https://generativelanguage.googleapis.com/v1beta/openai/chat/completions` |
| Ollama（本地） | `http://127.0.0.1:11434/v1/chat/completions` |
| LM Studio（本地） | `http://127.0.0.1:1234/v1/chat/completions` |

模型名以各平台当前模型列表为准，填错时本地服务日志会记录提供商返回的 HTTP 状态。非本机地址必须使用 HTTPS；本地回环服务的 Key 可留空。选择“自定义”可手动填写任何其他 OpenAI 兼容端点。

### 接入本地模型（Ollama / LM Studio）

翻译器可以直接使用本机运行的开源模型，无需联网、无 API 费用：

1. 安装并启动 [Ollama](https://ollama.com)（或 LM Studio），拉取一个指令模型，例如 `ollama pull qwen2.5:7b`。
2. 启动器 → “配置 API” → 提供商选 **Ollama (本地)**（或 LM Studio），Key 留空，保存。
3. 重启本地服务（点一次“启动服务器”开关），即可正常翻译。

本地模型注意事项：

- **超时自动放宽**：回环端点未显式配置 `timeout_ms` 时，服务器默认 180 秒（上限 300 秒），因为本地推理一批文本可能需要数分钟；远程 API 仍为 15 秒/60 秒。
- **并发建议调低**：本地推理通常只有一个计算槽，在 `config\api.ini` 中设 `concurrency=1`（或 2）可避免请求互相排队。
- **建议先预热**：Ren'Py 游戏可在启动前预热整本剧本，游戏运行时绝大多数台词直接命中缓存，几乎无等待。
- 本地小模型的翻译质量低于云端大模型，语种支持取决于所用模型。

## 还原游戏

1. 完全退出游戏。
2. 在启动器中选择此前部署过的游戏目录。
3. 点击“还原游戏”，阅读确认提示后继续。

还原操作按引擎移除本程序明确部署的 hook、插件或 Godot 外置文件，不删除翻译缓存、用户模组、现有 BepInEx/XUnity 运行时或游戏原始资源。Unity 文件与内置版本不一致、或 XUnity 配置已由用户修改时，启动器会保留它们并在日志中说明原因。

## 清除缓存

CACHE 卡显示本机共享翻译缓存 `translation_memory_c.tsv` 的大小。点击卡片内的“清除缓存”并确认后，启动器会暂时停止本地服务、删除该缓存文件，再按原状态重新启动服务。

该操作不会删除 API 配置、日志或游戏目录中的文件。已经运行的游戏可能还持有进程内存缓存，需要完全退出并重新启动游戏后才会完全生效。如果本地服务没有按时退出，启动器会取消删除并保留缓存文件。

## 术语表（可选）

如果希望特定名词（人名、武器、技能等）始终译成固定的中文名，可以使用术语表：

1. 复制 `config\glossary.example.tsv` 到程序根目录（与 `translation_memory_c.tsv` 同目录），改名为 `glossary.tsv`。
2. 按格式添加条目：每行一条，术语与译文之间用 TAB 分隔，`#` 开头的行为注释。
3. 通过启动器“启动服务器”重新启动本地服务（术语表在服务启动时加载）。

生效后，所有走远程 API 的翻译（包括 Ren'Py / RPG Maker / Unity / Godot 的预热和实时翻译）都会遵循这些译名。术语表最多加载 256 条，已缓存的旧翻译不受影响。

## 翻译上下文预热

Ren'Py 游戏预热时，启动器会把每句台词的上一句一起提交给翻译服务，远程模型会结合前后文翻译代词、语气和人名，减少孤立句子造成的误译。该功能自动启用，无需配置；RPG Maker / Unity / Godot 的预热行为不变。

## 按 Unity 类型单独安装

如果不想一次性下载所有 Unity 运行时，可以按游戏类型执行：

```powershell
# 旧版 Unity Mono / BepInEx 5
powershell -ExecutionPolicy Bypass -File scripts\install_runtime_payloads.ps1 -UnityMono5

# Unity 6+ Mono / BepInEx 6
powershell -ExecutionPolicy Bypass -File scripts\install_runtime_payloads.ps1 -UnityMono6

# Unity IL2CPP / BepInEx 6 + XUnity
powershell -ExecutionPolicy Bypass -File scripts\install_runtime_payloads.ps1 -UnityIL2CPP
```

如果不确定游戏类型，优先运行 `-All`。

## 更新方式

从 `0.3.2.7` 开始，大多数更新只需要替换 `ds游戏翻译器.exe`。

启动器每次启动都会检查并同步本项目自有组件，所以替换 exe 后会自动更新：

- 本地服务端
- 运行时安装脚本
- 示例配置
- 自有 Unity 插件 DLL

不会自动覆盖：

- `config\api.ini`
- 翻译记忆和缓存
- 日志
- 游戏目录
- BepInEx、XUnity、Newtonsoft.Json 等第三方运行时

如果发布说明提示第三方运行时版本变化，再重新运行：

```powershell
powershell -ExecutionPolicy Bypass -File scripts\install_runtime_payloads.ps1 -All -Force
```

已经启动的游戏不会热更新插件 DLL。更新后请完全退出游戏，再重新部署或重新启动游戏。

## 常见问题

### 下载的文件名为什么显示成 `ds._0.4.0.0.exe`？

GitHub 会规范化中文资源文件名。Release 页面已经给资产加了中文 label，下载后你可以把文件改名为 `ds游戏翻译器.exe`，功能不受影响。

### 为什么还要运行插件安装命令？

为了降低授权和侵权风险，程序不会直接打包 BepInEx、XUnity、Newtonsoft.Json、Unity 官方 DLL、游戏文件、TMP 字体包、翻译记忆或 API Key。Unity 所需第三方运行时和 XUnity TMP 字体 AssetBundle 由用户通过脚本从上游项目下载。

### Ren'Py 和 RPG Maker 也需要下载插件吗？

不需要。Ren'Py 和 RPG Maker 路径使用启动器自带 hook 和本地服务端。Godot 会扫描工程/导出包里的可翻译文本做缓存预热，并对导出的 `.pck` 生成外部补丁包（不改原包）；不需要 BepInEx/XUnity。

### API Key 放在哪里？

推荐直接在启动器里点击“配置 API”保存。真实配置会写入 `config\api.ini`。不要把这个文件发给别人，也不要提交到公开仓库。切换到其他 OpenAI 兼容提供商时同样保存在这里；本机回环服务（Ollama、LM Studio）不需要 Key，可留空。

### 游戏已经装过旧插件，要怎么更新？

先完全退出游戏，再用新版 `ds游戏翻译器.exe` 重新部署到游戏目录。Unity 游戏尤其需要完整退出后重启，旧 DLL 不会热更新。
