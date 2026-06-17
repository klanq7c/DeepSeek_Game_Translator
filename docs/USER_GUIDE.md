# DS翻译器使用说明

本文面向直接下载程序的用户。推荐下载 Release 页面里的 `ds翻译器_0.3.1.7.exe`。

## 第一次使用

1. 把 `ds翻译器_0.3.1.7.exe` 放到一个你准备长期使用的目录，例如 `D:\Games\DSTranslator\`。
2. 双击运行一次程序。首次运行会自动释放/更新本项目自带组件：
   - `native\dst_server.exe`
   - `scripts\install_runtime_payloads.ps1`
   - `config\api.ini.example`
   - 自有 Unity 插件 DLL
3. 在启动器中点击“配置 API”，填写自己的 DeepSeek API Key 并保存。
4. 如果要翻译 Ren'Py 或 RPG Maker 游戏，可以直接选择游戏目录并开始。
5. 如果要翻译 Unity 游戏，在程序所在目录运行：

```powershell
powershell -ExecutionPolicy Bypass -File scripts\install_runtime_payloads.ps1 -All
```

运行完成后回到启动器，或重新运行你下载的同一个 exe，选择游戏目录并部署。

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

从 `0.3.1.7` 开始，大多数更新只需要替换 `ds翻译器.exe`。

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

### 下载的文件名为什么显示成 `ds._0.3.1.7.exe`？

GitHub 会规范化中文资源文件名。Release 页面已经给资产加了中文 label，下载后你可以把文件改名为 `ds翻译器.exe`，功能不受影响。

### 为什么还要运行插件安装命令？

为了降低授权和侵权风险，程序不会直接打包 BepInEx、XUnity、Newtonsoft.Json、Unity 官方 DLL、游戏文件、字体、翻译记忆或 API Key。Unity 所需第三方运行时由用户通过脚本从上游项目下载。

### Ren'Py 和 RPG Maker 也需要下载插件吗？

不需要。Ren'Py 和 RPG Maker 路径使用启动器自带 hook 和本地服务端。

### API Key 放在哪里？

推荐直接在启动器里点击“配置 API”保存。真实配置会写入 `config\api.ini`。不要把这个文件发给别人，也不要提交到公开仓库。

### 游戏已经装过旧插件，要怎么更新？

先完全退出游戏，再用新版 `ds翻译器.exe` 重新部署到游戏目录。Unity 游戏尤其需要完整退出后重启，旧 DLL 不会热更新。
