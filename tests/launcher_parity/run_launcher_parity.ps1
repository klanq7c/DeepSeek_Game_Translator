<#
.SYNOPSIS
  启动器一致性测试：C 启动器（native/src/launcher）与 C# 移植版（native/src/launcher_cs）。

.DESCRIPTION
  第三阶段迁移守卫。两个启动器均提供隐藏诊断模式，向标准输出打印确定的 UTF-8
  报告。本脚本在临时根目录下构造游戏目录，让两版处理相同输入，并要求结果逐字节一致。

  1. 引擎检测（--detect-and-exit 对应 --detect）：覆盖 engine.c 各分支，包括
     Ren'Py（rpy/rpyc/rpa）、RPG Maker MV/MZ（www 与扁平布局、归属备份索引、
     核心文件缺失）、旧版 RPG Maker（rxdata/rvdata/rvdata2）、Unity Mono 与 IL2CPP
     （GameAssembly.dll 和 *_Data\il2cpp_data）、Godot（.pck、project.godot、.godot、
     godot_project.binary、PE 内嵌 pck 区段及 GDPC 尾标记），以及程序评分
     （stem_Data、stem.pck、末级目录名匹配、Game/RPG_RT、安装程序降权、
     同分时按名称排序、忽略名单）、NW.js 误判、空目录、不存在目录及尾部分隔符变体。

  2. 部署/恢复（--deploy-and-exit / --restore-and-exit 对应 --deploy / --restore）：
     每个场景生成两份，每版启动器处理一份；比较退出码、镜像日志（log=...）、result=
     及结果目录树的 SHA-256 快照。覆盖 Ren'Py、RPG Maker MV/MZ（index.html 注入、
     移除注入、备份、原子写入、NUL 字节、只读文件）、旧版 RPG Maker、Godot、未知引擎及 Unity：
     Mono（BepInEx 5/6 安装、按 PE 类型选择 x86/x64、修复不完整的用户 BepInEx、
     Unity 6 升级、不支持的机器类型、裁剪 mscorlib 后的 corlib 组件及 doorstop/BepInEx.cfg
     配置与字体补丁、归属冲突、插件/Newtonsoft 备份）；IL2CPP（be.755 运行时与 XUnity
     目录树、禁用随附 Mono 插件/Il2Cppmscorlib、配置写入、受管理配置迁移、保留用户改动、
     仅凭归属标记删除目录树）。两版均使用带微型替代组件的合成启动器根目录，
     不复制实际约 220 MB 的 Unity 组件，详见“合成的启动器根目录”一节。

  3. 预热扫描（--warmup-and-exit 对应 --warmup）：以转储模式执行扫描器，逐字节比较
     原本要发送的 /prefetch 或 /cache/import 请求体，详见“预热扫描一致性”一节。
     覆盖 Ren'Py（.rpy 字面量、转义、prevs、16 个字面量上限、递归特殊行为、大小/NUL）、
     RPG Maker MV/MZ（事件/数据库 JSON、前缀控制码、外部 TXT/CSV、扁平布局、数量限制）、
     Unity（XUnity 翻译文件、*_Data 资源及资源包、IL2CPP 的第二个 _Data 目录），以及 Godot
     （项目文本资源、引号分类、PO/CSV/Markdown、.translation 语言规则、PCK 格式 1/2/3
     的两遍处理顺序、损坏包的原始扫描回退、EXE 内嵌 pck、跳过目录/生成文件/深度限制、12000 条上限）。

  4. 内嵌组件同步（--sync-payloads-and-exit 对应 --sync-payloads）：释放的目录树、标准输出
     和退出码必须一致；第二次运行应无变化；篡改的目标应恢复；目标被阻塞时两版均以退出码 5 失败。

  5. Godot 补丁包（--godot-patch-and-exit / --godot-promote-and-exit / --godot-launcher-and-exit
     对应 --godot-patch / --godot-promote / --godot-launcher）：覆盖散装项目侧车脚本、格式 1
     侧车包（JSON、场景、Markdown、GDScript 字节码、含 Smaz 的 OptimizedTranslation、字体条目）、
     支持或不支持 autoload_prepend 的格式 3 包、EXE 内嵌包、每批 128 条/实时 1024 条/
     每资源 1800 条的配额、无可翻译内容的打包游戏、活动包被占用时暂存及随后提升、
     带归属标记的补丁启动器副本。生成的 .pck 必须逐字节一致；两版向本脚本在
     127.0.0.1:19999 启动的假服务发送相同的 /batch 请求体，端口被占用时跳过本节。

  6. 配置与 Godot 预检（--api-config* / --godot-probe* / --godot-preflight* 对应去掉
     -and-exit 后的同名参数）：验证提供商预设表、api.ini 读取值和预设索引、写入后逐字节一致的
     api.ini、对接受/拒绝输出的 --main-pack 拒绝分类，以及无头预检结论缓存
     （未命中/写入/命中、类型隔离、文件变化失效、逐字节一致的 godot_preflight.ini）。

  7. 一键启动流程（--launch-flow-and-exit 对应 --launch-flow）：覆盖 ui.c 中服务就绪后的流程。
     预热以转储模式执行；Godot 无头预检实际启动控制台桩，它从同目录 dst_fake_godot.ini
     读取标准输出和退出码，并在游戏目录旁记录自身命令行；只有三个实际启动调用点改为输出
     spawn=<kind>|<exe>|<cmd>|<cwd>。覆盖 Ren'Py（先启动再预热）、RPG Maker（先预热再启动）、
     未知引擎及 Godot（散装项目接受/拒绝侧车脚本、有/无补丁包的导出游戏、明确拒绝 --main-pack
     的模板）。另外验证 --clear-cache*：缓存卡片文本及共享缓存文件删除。

  8. 窗口渲染（--ui-probe-and-exit 对应 --ui-probe）：覆盖 ui.c 的窗口和绘图层。两版将 DPI
     固定为 96、冻结动画时钟、将两个运行时身份字符串替换为中性内容，并从参数读取服务状态。
     随后将不可见主窗口的整个客户区（背景及所有子控件，包括自绘按钮）渲染为一张 32bpp 位图。
     在四种尺寸及两种服务状态下，布局报告和位图均须逐字节一致。--ui-identity* 单独断言各版
     实际绘制的运行时标签和副标题，这是两个窗口有意保留的差异。

  9. 可选（-ServerSmoke）：C# ServerProcess 模块启动真实服务器，等待 /health 后停止服务。
     需要端口 19999 空闲；默认不运行，以免停止用户正在使用的服务器。

.PARAMETER CLauncher
  C 启动器程序路径，默认 <repo>\ds游戏翻译器.exe（build_native.bat 产物）。
.PARAMETER CsLauncher
  C# 启动器程序路径，默认从 native\src\launcher_cs 构建。
.PARAMETER KeepFixtures
  保留临时夹具目录树，供后续检查。
.PARAMETER ServerSmoke
  额外对 <repo>\native\dst_server.exe 运行服务生命周期冒烟测试。
.PARAMETER ShowLogs
  打印每个部署/恢复步骤的镜像 log= 行，便于查看命中分支；步骤通过后，两版日志应一致。
#>
[CmdletBinding()]
param(
    [string]$CLauncher = "",
    [string]$CsLauncher = "",
    [switch]$KeepFixtures,
    [switch]$ServerSmoke,
    # 打印每个部署/恢复步骤中两版一致的标准输出，便于检查场景命中的分支。
    [switch]$ShowLogs
)

$ErrorActionPreference = 'Stop'
$repo = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path

# Godot 补丁场景用的假 /batch 服务器（见下方）。AcceptTcpClient 是不可中断的阻塞调用，
# Runspace.Close 叫不醒它，必须先 Stop() 监听套接字让它抛错退出。任何终止性错误也要
# 经过这里，否则监听线程会把整个脚本挂死，把一次失败变成一次超时。
$fakeState = $null
$fakeRunspace = $null
$fakePs = $null
$fakeHandle = $null
function Stop-FakeServer {
    if (-not $script:fakePs) { return }
    if ($script:fakeState['Listener']) { try { $script:fakeState['Listener'].Stop() } catch { } }
    $waited = 0
    while (-not $script:fakeHandle.IsCompleted -and $waited -lt 5000) { Start-Sleep -Milliseconds 25; $waited += 25 }
    if (-not $script:fakeHandle.IsCompleted) { $script:fakePs.Stop() }
    $script:fakePs.Dispose()
    $script:fakeRunspace.Dispose()
    $script:fakePs = $null
}
trap { Stop-FakeServer; break }

if (-not $CLauncher) {
    # 产品名含中文，按 build_native.bat 的做法用码点拼出来，避开脚本文件编码问题。
    $ds = 'ds' + [string][char]0x6e38 + [string][char]0x620f + [string][char]0x7ffb + [string][char]0x8bd1 + [string][char]0x5668
    $CLauncher = Join-Path $repo ($ds + '.exe')
}
if (-not (Test-Path -LiteralPath $CLauncher)) { throw "C launcher not found: $CLauncher (run build_native.bat first)" }

$csBuildDir = $null
if (-not $CsLauncher) {
    $proj = Join-Path $repo 'native\src\launcher_cs\launcher_cs.csproj'
    $csBuildDir = Join-Path $env:TEMP ('dst_launcher_cs_' + [guid]::NewGuid().ToString('N'))
    Write-Host "[build] dotnet build $proj -> $csBuildDir"
    & dotnet build $proj -c Release -nologo -v quiet -p:OutputPath="$csBuildDir\" | Out-Host
    if ($LASTEXITCODE -ne 0) { throw "dotnet build failed for launcher_cs" }
    $CsLauncher = Join-Path $csBuildDir 'dst_launcher_cs.exe'
}
if (-not (Test-Path -LiteralPath $CsLauncher)) { throw "C# launcher not found: $CsLauncher" }

# ---------------------------------------------------------------- 测试夹具

$root = Join-Path $env:TEMP ('dst_detect_parity_' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $root | Out-Null

function New-Fixture([string]$name) {
    $d = Join-Path $root $name
    New-Item -ItemType Directory -Path $d -Force | Out-Null
    return $d
}
function Touch([string]$path, [byte[]]$bytes = $null) {
    $dir = Split-Path -Parent $path
    if (-not (Test-Path -LiteralPath $dir)) { New-Item -ItemType Directory -Path $dir -Force | Out-Null }
    if ($null -eq $bytes) { $bytes = [byte[]]@(0x78) }
    [IO.File]::WriteAllBytes($path, $bytes)
}
function Mkdir([string]$path) { New-Item -ItemType Directory -Path $path -Force | Out-Null }

# 最小 PE：包含名为 "pck" 的区段和末尾 GDPC 标记（Godot 4 内嵌包）。
function New-PckPeBytes([bool]$withSection, [bool]$withTail) {
    $dos = New-Object byte[] 64
    $dos[0] = 0x4D; $dos[1] = 0x5A                       # MZ
    [BitConverter]::GetBytes([int32]64).CopyTo($dos, 60)  # e_lfanew = 64
    $sig = [byte[]](0x50, 0x45, 0x00, 0x00)              # PE\0\0
    $fh = New-Object byte[] 20
    [BitConverter]::GetBytes([uint16]1).CopyTo($fh, 2)    # NumberOfSections
    [BitConverter]::GetBytes([uint16]0).CopyTo($fh, 16)   # SizeOfOptionalHeader
    $sh = New-Object byte[] 40
    if ($withSection) { $sh[0] = 0x70; $sh[1] = 0x63; $sh[2] = 0x6B } # "pck"
    else { $sh[0] = 0x2E; $sh[1] = 0x74; $sh[2] = 0x65; $sh[3] = 0x78; $sh[4] = 0x74 } # ".text"
    $pad = New-Object byte[] 16
    $tail = if ($withTail) { [byte[]](0x47, 0x44, 0x50, 0x43) } else { [byte[]](0x00, 0x00, 0x00, 0x00) }
    return [byte[]]($dos + $sig + $fh + $sh + $pad + $tail)
}

$fixtures = New-Object System.Collections.Generic.List[string]

# Ren'Py 检测
$d = New-Fixture 'renpy_rpy';  Touch "$d\game\script.rpy"; Touch "$d\Game.exe"; $fixtures.Add($d)
$d = New-Fixture 'renpy_rpyc'; Touch "$d\game\script.rpyc"; Touch "$d\renpy_rpyc.exe"; Touch "$d\lib\py3-windows-x86_64\python.exe"; $fixtures.Add($d)
$d = New-Fixture 'renpy_rpa';  Touch "$d\game\archive.rpa"; Touch "$d\a.exe"; Touch "$d\b.exe"; $fixtures.Add($d)
$d = New-Fixture 'renpy_game_dir_empty'; Mkdir "$d\game"; Touch "$d\x.exe"; $fixtures.Add($d)

# RPG Maker MV/MZ：www 目录布局
$d = New-Fixture 'rpgm_mv_www'
Touch "$d\www\index.html"; Touch "$d\www\data\System.json"; Touch "$d\www\js\main.js"; Touch "$d\www\js\rpg_core.js"; Touch "$d\Game.exe"
$fixtures.Add($d)
# MZ 扁平目录布局
$d = New-Fixture 'rpgm_mz_flat'
Touch "$d\index.html"; Touch "$d\data\System.json"; Touch "$d\js\main.js"; Touch "$d\js\rmmz_core.js"; Touch "$d\Game.exe"
$fixtures.Add($d)
# 部署后由工具拥有的备份替代 index.html
$d = New-Fixture 'rpgm_mv_owned_backup'
Touch "$d\www\index.html.dst-backup"; Touch "$d\www\data\System.json"; Touch "$d\www\js\main.js"; Touch "$d\www\js\rpg_core.js"; Touch "$d\Game.exe"
$fixtures.Add($d)
# 缺少核心 js 时不得识别为 RPG Maker（防止误判普通 NW.js 程序）
$d = New-Fixture 'nwjs_not_rpgm'
Touch "$d\www\index.html"; Touch "$d\www\js\main.js"; Touch "$d\www\data\System.json"; Touch "$d\nw.exe"
$fixtures.Add($d)
# System.json 为目录时，必须被 is_dir 检查拦截
$d = New-Fixture 'rpgm_system_is_dir'
Touch "$d\index.html"; Mkdir "$d\data\System.json"; Touch "$d\js\main.js"; Touch "$d\js\rpg_core.js"; Touch "$d\Game.exe"
$fixtures.Add($d)
# 存在 www 但缺少 www\js，而扁平根目录有效时，优先采用扁平布局
$d = New-Fixture 'rpgm_www_without_js_flat_valid'
Touch "$d\www\index.html"; Touch "$d\index.html"; Touch "$d\data\System.json"; Touch "$d\js\main.js"; Touch "$d\js\rmmz_core.js"; Touch "$d\Game.exe"
$fixtures.Add($d)

# 旧版 RPG Maker 检测
$d = New-Fixture 'rpgm_xp';   Touch "$d\Data\Scripts.rxdata";  Touch "$d\Game.exe"; Touch "$d\RPG_RT.exe"; $fixtures.Add($d)
$d = New-Fixture 'rpgm_vx';   Touch "$d\Data\Scripts.rvdata";  Touch "$d\Game.exe"; $fixtures.Add($d)
$d = New-Fixture 'rpgm_vxace'; Touch "$d\Data\Scripts.rvdata2"; Touch "$d\Game.exe"; $fixtures.Add($d)
$d = New-Fixture 'rpgm_legacy_data_only_dir'; Mkdir "$d\Data"; Touch "$d\Game.exe"; $fixtures.Add($d)

# Unity 检测
$d = New-Fixture 'unity_mono'
Mkdir "$d\MyGame_Data\Managed"; Touch "$d\MyGame.exe"; Touch "$d\UnityCrashHandler64.exe"; Touch "$d\UnityPlayer.dll"
$fixtures.Add($d)
$d = New-Fixture 'unity_il2cpp_gameassembly'
Mkdir "$d\MyGame_Data"; Touch "$d\GameAssembly.dll"; Touch "$d\MyGame.exe"
$fixtures.Add($d)
$d = New-Fixture 'unity_il2cpp_data_dir'
Mkdir "$d\MyGame_Data\il2cpp_data\Metadata"; Touch "$d\MyGame.exe"
$fixtures.Add($d)
$d = New-Fixture 'unity_data_suffix_case'
Mkdir "$d\Foo_DATA"; Touch "$d\Foo.exe"; Touch "$d\Setup.exe"
$fixtures.Add($d)
# *_Data 是文件而非目录时，不得识别为 Unity
$d = New-Fixture 'unity_data_is_file'
Touch "$d\Foo_Data"; Touch "$d\Foo.exe"
$fixtures.Add($d)
# Unity 目录中存在无关 .pck 时，Unity 判定必须优先于 Godot
$d = New-Fixture 'unity_with_stray_pck'
Mkdir "$d\Foo_Data"; Touch "$d\Foo.exe"; Touch "$d\stray.pck"
$fixtures.Add($d)

# Godot 检测
$d = New-Fixture 'godot_pck';            Touch "$d\game.pck"; Touch "$d\game.exe"; $fixtures.Add($d)
$d = New-Fixture 'godot_project';        Touch "$d\project.godot"; $fixtures.Add($d)
$d = New-Fixture 'godot_dot_dir';        Mkdir "$d\.godot"; $fixtures.Add($d)
$d = New-Fixture 'godot_binary_project'; Touch "$d\godot_project.binary"; $fixtures.Add($d)
$d = New-Fixture 'godot_embedded_pck'
Touch "$d\game.exe" (New-PckPeBytes $true $true); Touch "$d\other.exe"
$fixtures.Add($d)
$d = New-Fixture 'godot_embedded_tail_only'   # 仅有 GDPC 尾标记而无 pck 区段时，不得识别为 Godot
Touch "$d\game.exe" (New-PckPeBytes $false $true)
$fixtures.Add($d)
$d = New-Fixture 'godot_embedded_section_only' # 仅有 pck 区段而无 GDPC 尾标记时，不得识别为 Godot
Touch "$d\game.exe" (New-PckPeBytes $true $false)
$fixtures.Add($d)
$d = New-Fixture 'godot_embedded_ignored_name' # 忽略名称含 CrashHandler 的程序中的内嵌 pck
Touch "$d\FooCrashHandler.exe" (New-PckPeBytes $true $true)
$fixtures.Add($d)

# 游戏程序候选评分
$d = New-Fixture 'exe_tie_break_name'; Touch "$d\zeta.exe"; Touch "$d\Alpha.exe"; Touch "$d\beta.exe"; $fixtures.Add($d)
$d = New-Fixture 'exe_leaf_match';     Touch "$d\other.exe"; Touch "$d\EXE_LEAF_MATCH.exe"; $fixtures.Add($d)
$d = New-Fixture 'exe_installer_penalty'; Touch "$d\unins000.exe"; Touch "$d\Uninstall.exe"; Touch "$d\Config.exe"; Touch "$d\Configuration.exe"; Touch "$d\Installer.exe"; Touch "$d\Install.exe"; Touch "$d\Setup.exe"; Touch "$d\zzz.exe"; $fixtures.Add($d)
$d = New-Fixture 'exe_only_penalized';  Touch "$d\Setup.exe"; Touch "$d\unins001.exe"; $fixtures.Add($d)
$d = New-Fixture 'exe_ignored_only';    Touch "$d\dst_server.exe"; Touch "$d\DeepSeekTranslator.exe"; Touch "$d\dst_godot_patch.exe"; Touch "$d\ds游戏翻译器.exe"; Touch "$d\FooCrashHandler.exe"; $fixtures.Add($d)
$d = New-Fixture 'exe_dir_named_exe';   Mkdir "$d\folder.exe"; Touch "$d\real.exe"; $fixtures.Add($d)
$d = New-Fixture 'exe_stem_pck';        Touch "$d\a.exe"; Touch "$d\b.exe"; Touch "$d\b.pck"; $fixtures.Add($d)
$d = New-Fixture 'exe_stem_data_vs_pck'; Mkdir "$d\a_Data"; Touch "$d\a.exe"; Touch "$d\b.exe"; Touch "$d\b.pck"; $fixtures.Add($d)
$d = New-Fixture 'exe_unicode_名字';     Touch "$d\游戏.exe"; Touch "$d\ゲーム.exe"; $fixtures.Add($d)
$d = New-Fixture 'exe_bare_dot_exe';    Touch "$d\.exe"; Touch "$d\x.exe"; $fixtures.Add($d)

# 其他场景
$d = New-Fixture 'empty_dir'; $fixtures.Add($d)
$fixtures.Add((Join-Path $root 'does_not_exist'))

# 部分夹具增加尾部分隔符变体（path_join 不得重复添加斜杠）
$fixtures.Add((Join-Path $root 'unity_mono') + '\')
$fixtures.Add((Join-Path $root 'rpgm_mv_www') + '\')
$fixtures.Add((Join-Path $root 'godot_pck') + '/')

# ---------------------------------------------------------------- 执行测试

# 使用符合 CommandLineToArgvW 规则的引号转义（PS 5.1 没有 ProcessStartInfo.ArgumentList）：
# 紧邻引号之前的 N 个反斜杠必须翻倍，内嵌引号也必须转义。
function Quote-Arg([string]$a) {
    if ($a -eq '') { return '""' }
    $sb = New-Object System.Text.StringBuilder
    $null = $sb.Append('"')
    $bs = 0
    foreach ($ch in $a.ToCharArray()) {
        if ($ch -eq '\') { $bs++; continue }
        if ($ch -eq '"') { $null = $sb.Append('\', $bs * 2 + 1).Append('"'); $bs = 0; continue }
        if ($bs -gt 0) { $null = $sb.Append('\', $bs); $bs = 0 }
        $null = $sb.Append($ch)
    }
    if ($bs -gt 0) { $null = $sb.Append('\', $bs * 2) }
    $null = $sb.Append('"')
    return $sb.ToString()
}

function Invoke-Detect([string]$exe, [string[]]$argv) {
    $psi = New-Object System.Diagnostics.ProcessStartInfo
    $psi.FileName = $exe
    $psi.UseShellExecute = $false
    $psi.RedirectStandardOutput = $true
    $psi.RedirectStandardError = $true
    $psi.StandardOutputEncoding = [Text.Encoding]::UTF8
    $psi.StandardErrorEncoding = [Text.Encoding]::UTF8
    $psi.CreateNoWindow = $true
    $psi.Arguments = (($argv | ForEach-Object { Quote-Arg $_ }) -join ' ')
    $p = [System.Diagnostics.Process]::Start($psi)
    $out = $p.StandardOutput.ReadToEnd()
    $err = $p.StandardError.ReadToEnd()
    if (-not $p.WaitForExit(30000)) { try { $p.Kill() } catch { } ; throw "timeout: $exe $($argv -join ' ')" }
    return @{ Code = $p.ExitCode; Out = $out; Err = $err }
}

$pass = 0; $fail = 0
$failures = New-Object System.Collections.Generic.List[string]
foreach ($dir in $fixtures) {
    $c = Invoke-Detect $CLauncher @('--detect-and-exit', $dir)
    $cs = Invoke-Detect $CsLauncher @('--detect', $dir)
    $label = $dir.Substring($root.Length).TrimStart('\')
    if ($c.Code -ne 0 -or $cs.Code -ne 0) {
        $fail++
        $failures.Add("[$label] exit codes C=$($c.Code) C#=$($cs.Code)`n  C stderr: $($c.Err)`n  C# stderr: $($cs.Err)")
        continue
    }
    if ($c.Out -ne $cs.Out) {
        $fail++
        $failures.Add("[$label] output differs`n--- C ---`n$($c.Out)--- C# ---`n$($cs.Out)")
        continue
    }
    if ($c.Out -notmatch '^engine=') {
        $fail++
        $failures.Add("[$label] C launcher produced no report (stdout not inherited?)`n$($c.Out)")
        continue
    }
    $pass++
    $first = ($c.Out -split "`n")[0]
    Write-Host ("  ok  {0,-36} {1}" -f $label, $first)
}

# 参数错误的一致性：缺少目录参数时退出码为 2，标准输出为空
$cU = Invoke-Detect $CLauncher @('--detect-and-exit')
$csU = Invoke-Detect $CsLauncher @('--detect')
if ($cU.Code -eq 2 -and $csU.Code -eq 2 -and $cU.Out -eq '' -and $csU.Out -eq '') { $pass++; Write-Host "  ok  usage-error exit code 2" }
else { $fail++; $failures.Add("usage error: C=$($cU.Code)/'$($cU.Out)' C#=$($csU.Code)/'$($csU.Out)'") }

# ---------------------------------------------------------------- 部署/恢复一致性
#
# 每个场景生成两份相同目录树，分别置于 c\ 和 cs\ 下；C 启动器
# 处理一份，C# 启动器处理另一份。比较退出码、完整标准输出
# （log= 行和 result=）以及结果目录树的快照（相对路径、
# 属性和 SHA-256）。日志路径仅 c\ / cs\ 部分不同，因此将
# 各副本根路径替换为 <ROOT> 后再比较快照和标准输出。

$sha = [System.Security.Cryptography.SHA256]::Create()
function Snapshot-Tree([string]$root) {
    $lines = New-Object System.Collections.Generic.List[string]
    Get-ChildItem -LiteralPath $root -Recurse -Force | Sort-Object FullName | ForEach-Object {
        $rel = $_.FullName.Substring($root.Length).TrimStart('\')
        if ($_.PSIsContainer) { $lines.Add("D $rel") }
        else {
            $hash = [BitConverter]::ToString($sha.ComputeHash([IO.File]::ReadAllBytes($_.FullName))).Replace('-', '')
            $ro = if ($_.Attributes -band [IO.FileAttributes]::ReadOnly) { 'R' } else { '-' }
            $lines.Add("F $ro $rel $hash")
        }
    }
    return ($lines -join "`n")
}

$renpyHookBytes = [IO.File]::ReadAllBytes((Join-Path $repo 'payloads\RenPy\iron_deepseek.rpy'))
$rpgmHookBytes = [IO.File]::ReadAllBytes((Join-Path $repo 'payloads\RPGMaker\hook_rpgm_mv.js'))
$asciiEnc = [Text.Encoding]::ASCII

$indexPlain = "<!DOCTYPE html>`r`n<html>`r`n<head>`r`n<meta charset=`"UTF-8`">`r`n</head>`r`n<body style=`"background-color: black`">`r`n<script type=`"text/javascript`" src=`"js/libs/pixi.js`"></script>`r`n<script type=`"text/javascript`" src=`"js/main.js`"></script>`r`n</body>`r`n</html>`r`n"
$indexAlreadyHooked = "<!DOCTYPE html>`r`n<html>`r`n<body>`r`n<script type=`"text/javascript`" src=`"js/libs/pixi.js`"></script>`r`n   `r`n<script type=`"text/javascript`" src=`"js/hook_rpgm_mv.js`"></script>`r`n<script type=`"text/javascript`" src=`"js/main.js`"></script>`r`n</body>`r`n</html>`r`n"
$indexSelfClosing = "<html><body>`n<SCRIPT SRC=`"js/hook_rpgm_mv.js`" />`n<script src=`"js/main.js`"></script>`n</body></html>"
$indexNoMain = "<html><body>`n<p>hook_rpgm_mv.js mentioned in text only</p>`n</body></html>"
$indexNoBodyNoMain = "<html><head><title>x</title></head></html>"
$indexWithNul = $asciiEnc.GetBytes("<html><body>`n<script src=`"js/main.js`"></script>`n") + [byte[]](0) + $asciiEnc.GetBytes("<script src=`"js/hook_rpgm_mv.js`"></script></body></html>")

function New-RpgmTree([string]$d, [object]$index, [bool]$www) {
    $cr = if ($www) { Join-Path $d 'www' } else { $d }
    Touch "$cr\data\System.json"; Touch "$cr\js\main.js"; Touch "$cr\js\rpg_core.js"; Touch "$d\Game.exe"
    if ($index -is [string]) { Touch "$cr\index.html" ($asciiEnc.GetBytes($index)) }
    elseif ($null -ne $index) { Touch "$cr\index.html" ([byte[]]$index) }
}

# ---------------------------------------------------------------- 合成的启动器根目录
# Unity 部署从 <launcher-dir>\payloads 复制 BepInEx/XUnity/TMP 组件。
# 实际组件每次约需复制 220 MB，因此部署/恢复场景让两个启动器
# 使用带有微型替代组件的合成根目录。C 启动器复制到该目录中，
# 其程序目录即 g_root，诊断模式不访问 g_root 下的其他内容；C#
# 启动器则通过 --root 接收相同目录。下方目录树还特意省略
# UnityMonoRuntime6X86，以覆盖“缺少组件”分支。
function New-PeBytes([uint16]$machine) {
    if ($machine -eq 0) { return $asciiEnc.GetBytes('not a PE file at all, just padding bytes .................') }
    $dos = New-Object byte[] 64
    $dos[0] = 0x4D; $dos[1] = 0x5A
    [BitConverter]::GetBytes([int32]64).CopyTo($dos, 60)
    $sig = [byte[]](0x50, 0x45, 0x00, 0x00)
    $fh = New-Object byte[] 20
    [BitConverter]::GetBytes([uint16]$machine).CopyTo($fh, 0)
    return [byte[]]($dos + $sig + $fh + (New-Object byte[] 8))
}
$PE_X86 = [uint16]0x014c; $PE_X64 = [uint16]0x8664; $PE_ARM = [uint16]0x01c4

$tpl5 = $asciiEnc.GetBytes('synthetic UnityTranslator.dll (BepInEx 5 template)')
$tpl6 = $asciiEnc.GetBytes('synthetic UnityTranslator.BepInEx6.dll (BepInEx 6 template)')
$newtonsoftBytes = $asciiEnc.GetBytes('synthetic Newtonsoft.Json.dll')
$fontPatcherBytes = $asciiEnc.GetBytes('synthetic DeepSeekUnityFontPatcher.dll')
$corlibMarkerBytes = $asciiEnc.GetBytes("synthetic-corlib-marker`n")
$corlibComplete = $asciiEnc.GetBytes('MZ.. mscorlib .. System.IO .. WriteAllText ..')
$corlibStripped = $asciiEnc.GetBytes('MZ.. mscorlib .. System.IO .. (no file writer)')
$xunityMarkerText = "ds-game-translator:xunity-auto-translator:v1`n"

$synthRoot = Join-Path $root 'synth_root'
Mkdir $synthRoot
$CLauncherSynth = Join-Path $synthRoot (Split-Path -Leaf $CLauncher)
Copy-Item -LiteralPath $CLauncher -Destination $CLauncherSynth
$P = Join-Path $synthRoot 'payloads'
Touch "$P\UnityTranslator\UnityTranslator.dll" $tpl5
Touch "$P\UnityTranslator\UnityTranslator.BepInEx6.dll" $tpl6
Touch "$P\UnityTranslator\Newtonsoft.Json.dll" $newtonsoftBytes
Touch "$P\UnityTranslator\DeepSeekUnityFontPatcher.dll" $fontPatcherBytes
foreach ($rt in @(@('UnityMonoRuntime', $PE_X64, $false), @('UnityMonoRuntimeX86', $PE_X86, $false), @('UnityMonoRuntime6', $PE_X64, $true))) {
    $name, $machine, $six = $rt
    Touch "$P\$name\winhttp.dll" (New-PeBytes $machine)
    Touch "$P\$name\doorstop_config.ini" ($asciiEnc.GetBytes("[General]`r`nenabled = true`r`n`r`n[UnityMono]`r`ntarget_assembly = BepInEx\core\BepInEx.Preloader.dll`r`n"))
    Touch "$P\$name\.doorstop_version" ($asciiEnc.GetBytes("4.0.0"))
    if ($six) {
        Touch "$P\$name\BepInEx\core\BepInEx.Unity.Mono.dll" ($asciiEnc.GetBytes("bep6 core $name"))
        Touch "$P\$name\BepInEx\core\BepInEx.Unity.Mono.Preloader.dll" ($asciiEnc.GetBytes("bep6 preloader $name"))
    } else {
        Touch "$P\$name\BepInEx\core\BepInEx.dll" ($asciiEnc.GetBytes("bep5 core $name"))
        Touch "$P\$name\BepInEx\core\BepInEx.Preloader.dll" ($asciiEnc.GetBytes("bep5 preloader $name"))
    }
    Touch "$P\$name\BepInEx\core\0Harmony.dll" ($asciiEnc.GetBytes("harmony $name"))
}
Touch "$P\UnityMonoCorlib\.dst-installed-by-ds" $corlibMarkerBytes
Touch "$P\UnityMonoCorlib\mscorlib.dll" $corlibComplete
Touch "$P\UnityMonoCorlib\System.dll" ($asciiEnc.GetBytes('synthetic System.dll'))
Touch "$P\UnityIL2CPP\BepInExRuntime\doorstop_config.ini" ($asciiEnc.GetBytes("[General]`r`nenabled = true`r`n"))
Touch "$P\UnityIL2CPP\BepInExRuntime\winhttp.dll" (New-PeBytes $PE_X64)
Touch "$P\UnityIL2CPP\BepInExRuntime\.doorstop_version" ($asciiEnc.GetBytes("4.0.0-il2cpp"))
Touch "$P\UnityIL2CPP\BepInExRuntime\dotnet\coreclr.dll" ($asciiEnc.GetBytes('synthetic coreclr'))
Touch "$P\UnityIL2CPP\BepInExRuntime\dotnet\shared\System.Runtime.dll" ($asciiEnc.GetBytes('synthetic System.Runtime'))
Touch "$P\UnityIL2CPP\BepInExRuntime\BepInEx\core\BepInEx.Core.dll" ($asciiEnc.GetBytes('synthetic BepInEx.Core'))
Touch "$P\UnityIL2CPP\BepInExRuntime\BepInEx\core\BepInEx.Unity.IL2CPP.dll" ($asciiEnc.GetBytes('synthetic BepInEx.Unity.IL2CPP'))
Touch "$P\UnityIL2CPP\BepInExRuntime\BepInEx\patchers\Il2CppInteropPatcher.dll" ($asciiEnc.GetBytes('synthetic patcher'))
Touch "$P\UnityIL2CPP\XUnityAutoTranslator\BepInEx\core\XUnity.Common.dll" ($asciiEnc.GetBytes('synthetic XUnity.Common'))
Touch "$P\UnityIL2CPP\XUnityAutoTranslator\BepInEx\plugins\XUnity.AutoTranslator\XUnity.AutoTranslator.Plugin.Core.dll" ($asciiEnc.GetBytes('synthetic XUnity core'))
Touch "$P\UnityIL2CPP\XUnityAutoTranslator\BepInEx\plugins\XUnity.AutoTranslator\Translators\FullNameSplitter.dll" ($asciiEnc.GetBytes('synthetic translator'))
Touch "$P\UnityIL2CPP\XUnityAutoTranslator\BepInEx\plugins\XUnity.ResourceRedirector\XUnity.ResourceRedirector.dll" ($asciiEnc.GetBytes('synthetic redirector'))
Touch "$P\UnityIL2CPP\DeepSeekXUnityTranslator\DeepSeekTranslate.dll" ($asciiEnc.GetBytes('synthetic DeepSeekTranslate endpoint'))
Touch "$P\UnityIL2CPP\TMPFontAssetBundles\BepInEx\font\cjk_fallback.bundle" ($asciiEnc.GetBytes('synthetic font bundle'))
Touch "$P\UnityIL2CPP\DeepSeekTMPFontFallback\BepInEx\plugins\DeepSeekTMPFontFallback\DeepSeekTMPFontFallback.dll" ($asciiEnc.GetBytes('synthetic TMP fallback'))

function New-UnityMonoTree([string]$d, [string]$version, [uint16]$machine, [bool]$stripped) {
    Touch "$d\MyGame.exe" (New-PeBytes $machine)
    Touch "$d\UnityPlayer.dll" (New-PeBytes $machine)
    Touch "$d\UnityCrashHandler64.exe"
    Touch "$d\MyGame_Data\globalgamemanagers" ($asciiEnc.GetBytes("....$version....`0`0"))
    Touch "$d\MyGame_Data\Managed\mscorlib.dll" $(if ($stripped) { $corlibStripped } else { $corlibComplete })
    Touch "$d\MyGame_Data\Managed\Assembly-CSharp.dll"
}
function New-UnityIl2cppTree([string]$d, [uint16]$machine) {
    Touch "$d\MyGame.exe" (New-PeBytes $machine)
    Touch "$d\UnityPlayer.dll" (New-PeBytes $machine)
    Touch "$d\GameAssembly.dll" (New-PeBytes $machine)
    Touch "$d\MyGame_Data\il2cpp_data\Metadata\global-metadata.dat"
    Touch "$d\MyGame_Data\globalgamemanagers" ($asciiEnc.GetBytes("....2022.3.1f1...."))
}

# 场景表：名称 -> { Build = 接收目录的脚本块；Steps = 按顺序排列的 deploy/restore 操作 }
$scenarios = New-Object System.Collections.Specialized.OrderedDictionary
$scenarios['renpy_deploy_then_restore'] = @{
    Build = { param($d) Touch "$d\game\script.rpy"; Touch "$d\Game.exe" }
    Steps = @('deploy', 'restore')
}
$scenarios['renpy_deploy_twice_idempotent'] = @{
    Build = { param($d) Touch "$d\game\script.rpy"; Touch "$d\Game.exe" }
    Steps = @('deploy', 'deploy')
}
$scenarios['renpy_refresh_stale_hook'] = @{
    Build = { param($d) Touch "$d\game\script.rpy"; Touch "$d\Game.exe"
        Touch "$d\game\iron_deepseek.rpy" ($asciiEnc.GetBytes('stale hook from an older translator')) }
    Steps = @('deploy')
}
$scenarios['renpy_stale_rpyc_readonly'] = @{
    Build = { param($d)
        Touch "$d\game\script.rpy"; Touch "$d\game\iron_deepseek.rpyc" ([byte[]](1,2,3))
        Set-ItemProperty -LiteralPath "$d\game\iron_deepseek.rpyc" -Name IsReadOnly -Value $true
        Touch "$d\Game.exe" }
    Steps = @('deploy')
}
$scenarios['renpy_rpyc_is_directory'] = @{
    Build = { param($d) Touch "$d\game\script.rpy"; Mkdir "$d\game\iron_deepseek.rpyc"; Touch "$d\Game.exe" }
    Steps = @('deploy')
}
$scenarios['renpy_font_already_present'] = @{
    Build = { param($d) Touch "$d\game\archive.rpa"; Touch "$d\game\ds_font.ttf" ([byte[]](9,9)); Touch "$d\Game.exe" }
    Steps = @('deploy', 'restore')
}
$scenarios['renpy_user_otf_font_preserved'] = @{
    Build = { param($d) Touch "$d\game\script.rpy"; Touch "$d\game\ds_font.otf" ([byte[]](7,8,9)); Touch "$d\Game.exe" }
    Steps = @('deploy', 'restore')
}
$scenarios['renpy_modified_owned_font_preserved'] = @{
    Build = { param($d) Touch "$d\game\script.rpy"; Touch "$d\Game.exe"
        Touch "$d\game\ds_font.ttf" ([byte[]](4,5,6))
        Touch "$d\game\ds_font.ttf.dst-owned" ($asciiEnc.GetBytes("DST_FONT_OWNED_V1:simhei.ttf`n")) }
    Steps = @('restore')
}
$scenarios['renpy_restore_untouched'] = @{
    Build = { param($d) Touch "$d\game\script.rpy"; Touch "$d\Game.exe" }
    Steps = @('restore')
}
$scenarios['rpgm_www_deploy_restore'] = @{
    Build = { param($d) New-RpgmTree $d $indexPlain $true }
    Steps = @('deploy', 'restore')
}
$scenarios['rpgm_flat_deploy_restore'] = @{
    Build = { param($d) New-RpgmTree $d $indexPlain $false }
    Steps = @('deploy', 'restore')
}
$scenarios['rpgm_user_otf_font_preserved'] = @{
    Build = { param($d) New-RpgmTree $d $indexPlain $true
        Touch "$d\www\fonts\ds_font.otf" ([byte[]](7,8,9)) }
    Steps = @('deploy', 'restore')
}
$scenarios['rpgm_user_ttf_font_preserved'] = @{
    Build = { param($d) New-RpgmTree $d $indexPlain $true
        Touch "$d\www\fonts\ds_font.ttf" ([byte[]](9,9)) }
    Steps = @('deploy', 'restore')
}
$scenarios['rpgm_deploy_twice_idempotent'] = @{
    Build = { param($d) New-RpgmTree $d $indexPlain $true }
    Steps = @('deploy', 'deploy')
}
$scenarios['rpgm_refresh_stale_hook'] = @{
    Build = { param($d) New-RpgmTree $d $indexPlain $true
        Touch "$d\www\js\hook_rpgm_mv.js" ($asciiEnc.GetBytes('stale hook from an older translator')) }
    Steps = @('deploy')
}
$scenarios['rpgm_already_hooked_with_blank_line'] = @{
    Build = { param($d) New-RpgmTree $d $indexAlreadyHooked $true }
    Steps = @('deploy', 'restore')
}
$scenarios['rpgm_self_closing_uppercase_tag'] = @{
    Build = { param($d) New-RpgmTree $d $indexSelfClosing $true }
    Steps = @('restore', 'deploy')
}
$scenarios['rpgm_hook_name_in_text_only'] = @{
    Build = { param($d) New-RpgmTree $d $indexNoMain $true }
    Steps = @('deploy', 'restore')
}
$scenarios['rpgm_no_body_no_main'] = @{
    Build = { param($d) New-RpgmTree $d $indexNoBodyNoMain $true }
    Steps = @('deploy')
}
$scenarios['rpgm_index_with_embedded_nul'] = @{
    Build = { param($d) New-RpgmTree $d $indexWithNul $true }
    Steps = @('deploy', 'restore')
}
$scenarios['rpgm_restore_from_backup_only'] = @{
    Build = { param($d) New-RpgmTree $d $null $true; Touch "$d\www\index.html.dst-backup" ($asciiEnc.GetBytes($indexPlain)) }
    Steps = @('restore')
}
$scenarios['rpgm_restore_backup_kept_when_index_modified'] = @{
    Build = { param($d) New-RpgmTree $d $indexPlain $true; Touch "$d\www\index.html.dst-backup" ($asciiEnc.GetBytes('<html>user backup</html>')) }
    Steps = @('restore')
}
$scenarios['rpgm_readonly_index'] = @{
    Build = { param($d) New-RpgmTree $d $indexPlain $true
        Set-ItemProperty -LiteralPath "$d\www\index.html" -Name IsReadOnly -Value $true }
    Steps = @('deploy')
}
$scenarios['rpgm_flat_mz_without_exe'] = @{
    Build = { param($d) Touch "$d\index.html" ($asciiEnc.GetBytes($indexPlain)); Touch "$d\data\System.json"; Touch "$d\js\main.js"; Touch "$d\js\rmmz_core.js" }
    Steps = @('deploy', 'restore')
}
$scenarios['rpgm_legacy_noop'] = @{
    Build = { param($d) Touch "$d\Data\Scripts.rxdata"; Touch "$d\Game.exe" }
    Steps = @('deploy', 'restore')
}
$scenarios['godot_deploy_restore_owned_patch'] = @{
    Build = { param($d) Touch "$d\game.pck"; Touch "$d\game.exe"; Touch "$d\dst_godot_patch.exe"; Touch "$d\dst_godot_patch.exe.dst-owned"; Touch "$d\dst_godot_runtime.gd"; Touch "$d\dst_godot_patch.pck"; Touch "$d\dst_godot_patch.pck.dst-source"; Touch "$d\dst_godot_patch.next.pck"; Touch "$d\dst_godot_patch.next.pck.dst-source" }
    Steps = @('deploy', 'restore')
}
$scenarios['godot_restore_unowned_patch_exe'] = @{
    Build = { param($d) Touch "$d\project.godot"; Touch "$d\dst_godot_patch.exe"; Touch "$d\dst_godot_patch.pck" }
    Steps = @('restore')
}
$scenarios['unknown_engine'] = @{
    Build = { param($d) Touch "$d\readme.txt" }
    Steps = @('deploy', 'restore')
}
$scenarios['nonexistent_dir'] = @{
    Build = { param($d) }
    Steps = @('restore')
    SkipCreate = $true
}

# ---- Unity Mono（BepInEx 5/6 安装、修复、裁剪后的 corlib、文件归属）----
$scenarios['unity_mono_deploy_restore'] = @{
    Build = { param($d) New-UnityMonoTree $d '2021.3.5f1' $PE_X64 $false }
    Steps = @('deploy', 'restore')
}
$scenarios['unity_mono_deploy_twice_idempotent'] = @{
    Build = { param($d) New-UnityMonoTree $d '2021.3.5f1' $PE_X64 $false }
    Steps = @('deploy', 'deploy')
}
$scenarios['unity6_mono_deploy_restore'] = @{
    Build = { param($d) New-UnityMonoTree $d '6000.0.23f1' $PE_X64 $false }
    Steps = @('deploy', 'restore')
}
$scenarios['unity6_mono_x86_missing_payload'] = @{
    Build = { param($d) New-UnityMonoTree $d '6000.1.0f1' $PE_X86 $false }
    Steps = @('deploy')
}
$scenarios['unity_mono_x86_deploy'] = @{
    Build = { param($d) New-UnityMonoTree $d '2019.4.40f1' $PE_X86 $false }
    Steps = @('deploy', 'restore')
}
$scenarios['unity_mono_unsupported_machine'] = @{
    Build = { param($d) New-UnityMonoTree $d '2020.3.0f1' $PE_ARM $false }
    Steps = @('deploy')
}
$scenarios['unity_mono_unknown_machine_defaults_x64'] = @{
    Build = { param($d) New-UnityMonoTree $d '5.6.7f1' 0 $false }
    Steps = @('deploy')
}
$scenarios['unity_mono_no_version_string'] = @{
    Build = { param($d) New-UnityMonoTree $d 'no-digits-here' $PE_X64 $false }
    Steps = @('deploy')
}
$scenarios['unity_mono_stripped_deploy_restore'] = @{
    Build = { param($d) New-UnityMonoTree $d '2021.3.5f1' $PE_X64 $true }
    Steps = @('deploy', 'restore')
}
$scenarios['unity_mono_stripped_deploy_twice'] = @{
    Build = { param($d) New-UnityMonoTree $d '2021.3.5f1' $PE_X64 $true }
    Steps = @('deploy', 'deploy')
}
$scenarios['unity_mono_stripped_existing_ini_sections'] = @{
    Build = { param($d) New-UnityMonoTree $d '2018.4.0f1' $PE_X64 $true
        # 用户提供的 BepInEx 5 带有自有配置：应插入裁剪运行时所需键，不能追加重复小节
        Touch "$d\winhttp.dll" (New-PeBytes $PE_X64)
        Touch "$d\doorstop_config.ini" ($asciiEnc.GetBytes("; user doorstop`r`n[UnityMono]`r`nenabled = true`r`n[Other]`r`nx = 1`r`n"))
        Touch "$d\.doorstop_version"
        Touch "$d\BepInEx\core\BepInEx.dll" ($asciiEnc.GetBytes('user bepinex'))
        Touch "$d\BepInEx\core\BepInEx.Preloader.dll" ($asciiEnc.GetBytes('user preloader'))
        Touch "$d\BepInEx\config\BepInEx.cfg" ($asciiEnc.GetBytes("[Logging]`r`n`r`n## comment`r`nUnityLogListening = true`r`n`r`n[Chainloader]`r`nHideManagerGameObject = false")) }
    Steps = @('deploy', 'restore')
}
$scenarios['unity_mono_stripped_user_owned_conflict'] = @{
    Build = { param($d) New-UnityMonoTree $d '2021.3.5f1' $PE_X64 $true
        Touch "$d\doorstop_config.ini" ($asciiEnc.GetBytes("[UnityMono]`r`ndll_search_path_override = Custom`r`n"))
        Touch "$d\doorstop_config.ini.dst-stripped-owned" ($asciiEnc.GetBytes("[UnityMono]`r`ndll_search_path_override = BepInEx\unstripped_corlib;BepInEx\core`r`n")) }
    Steps = @('deploy', 'restore')
}
$scenarios['unity_mono_user_plugin_backup_restore'] = @{
    Build = { param($d) New-UnityMonoTree $d '2021.3.5f1' $PE_X64 $false
        Touch "$d\BepInEx\plugins\UnityTranslator.dll" ($asciiEnc.GetBytes('user-built UnityTranslator'))
        Touch "$d\BepInEx\plugins\Newtonsoft.Json.dll" ($asciiEnc.GetBytes('user Newtonsoft 12.0')) }
    Steps = @('deploy', 'restore')
}
$scenarios['unity_mono_second_plugin_edit_with_backup'] = @{
    Build = { param($d) New-UnityMonoTree $d '2021.3.5f1' $PE_X64 $false
        Touch "$d\BepInEx\plugins\UnityTranslator.dll" ($asciiEnc.GetBytes('new user plugin edit'))
        Touch "$d\BepInEx\plugins\UnityTranslator.dll.dst-backup" ($asciiEnc.GetBytes('prior user plugin edit'))
        Touch "$d\BepInEx\plugins\UnityTranslator.dll.dst-owned" ($asciiEnc.GetBytes('old launcher-owned plugin')) }
    Steps = @('deploy')
    ExpectedResult = 0
}
$scenarios['unity_mono_second_newtonsoft_edit_with_backup'] = @{
    Build = { param($d) New-UnityMonoTree $d '2021.3.5f1' $PE_X64 $false
        Touch "$d\BepInEx\plugins\Newtonsoft.Json.dll" ($asciiEnc.GetBytes('new user Newtonsoft edit'))
        Touch "$d\BepInEx\plugins\Newtonsoft.Json.dll.dst-backup" ($asciiEnc.GetBytes('prior user Newtonsoft edit')) }
    Steps = @('deploy')
    ExpectedResult = 0
}
$scenarios['unity_mono_matching_backups_allow_deploy'] = @{
    Build = { param($d) New-UnityMonoTree $d '2021.3.5f1' $PE_X64 $false
        $plugin = $asciiEnc.GetBytes('user plugin matching backup')
        $json = $asciiEnc.GetBytes('user Newtonsoft matching backup')
        Touch "$d\BepInEx\plugins\UnityTranslator.dll" $plugin
        Touch "$d\BepInEx\plugins\UnityTranslator.dll.dst-backup" $plugin
        Touch "$d\BepInEx\plugins\Newtonsoft.Json.dll" $json
        Touch "$d\BepInEx\plugins\Newtonsoft.Json.dll.dst-backup" $json }
    Steps = @('deploy', 'restore')
    ExpectedResult = 1
}
$scenarios['unity_mono_upgrade_owned_dependency'] = @{
    Build = { param($d) New-UnityMonoTree $d '2021.3.5f1' $PE_X64 $false
        $oldPlugin = $asciiEnc.GetBytes('old owned UnityTranslator')
        $oldJson = $asciiEnc.GetBytes('old owned Newtonsoft')
        Touch "$d\BepInEx\plugins\UnityTranslator.dll" $oldPlugin
        Touch "$d\BepInEx\plugins\UnityTranslator.dll.dst-owned" $oldPlugin
        Touch "$d\BepInEx\plugins\UnityTranslator.dll.dst-backup" ($asciiEnc.GetBytes('user plugin before launcher'))
        Touch "$d\BepInEx\plugins\Newtonsoft.Json.dll" $oldJson
        Touch "$d\BepInEx\plugins\Newtonsoft.Json.dll.dst-owned" $oldJson
        Touch "$d\BepInEx\plugins\Newtonsoft.Json.dll.dst-backup" ($asciiEnc.GetBytes('user Newtonsoft before launcher')) }
    Steps = @('deploy', 'restore')
    ExpectedResult = 1
}
$scenarios['unity_mono_existing_partial_bepinex_repair'] = @{
    Build = { param($d) New-UnityMonoTree $d '2021.3.5f1' $PE_X64 $false
        Touch "$d\BepInEx\plugins\SomeOtherMod.dll" ($asciiEnc.GetBytes('other mod')) }
    Steps = @('deploy', 'restore')
}
$scenarios['unity_mono_existing_bepinex_wrong_loader_arch'] = @{
    Build = { param($d) New-UnityMonoTree $d '2021.3.5f1' $PE_X86 $false
        Touch "$d\winhttp.dll" (New-PeBytes $PE_X64)
        Touch "$d\doorstop_config.ini"; Touch "$d\.doorstop_version"
        Touch "$d\BepInEx\core\BepInEx.dll"; Touch "$d\BepInEx\core\BepInEx.Preloader.dll" }
    Steps = @('deploy')
}
$scenarios['unity6_mono_upgrade_from_bepinex5'] = @{
    Build = { param($d) New-UnityMonoTree $d '6000.0.1f1' $PE_X64 $false
        Touch "$d\winhttp.dll" (New-PeBytes $PE_X64); Touch "$d\doorstop_config.ini"; Touch "$d\.doorstop_version"
        Touch "$d\BepInEx\core\BepInEx.dll"; Touch "$d\BepInEx\core\BepInEx.Preloader.dll" }
    Steps = @('deploy', 'restore')
}
$scenarios['unity_mono_restore_untouched'] = @{
    Build = { param($d) New-UnityMonoTree $d '2021.3.5f1' $PE_X64 $false }
    Steps = @('restore')
}
$scenarios['unity_mono_restore_modified_plugin_preserved'] = @{
    Build = { param($d) New-UnityMonoTree $d '2021.3.5f1' $PE_X64 $false
        Touch "$d\BepInEx\plugins\UnityTranslator.dll" ($asciiEnc.GetBytes('modified after deploy'))
        Touch "$d\BepInEx\plugins\UnityTranslator.dll.dst-backup" ($asciiEnc.GetBytes('original user plugin'))
        Touch "$d\BepInEx\patchers\DeepSeekUnityFontPatcher.dll" ($asciiEnc.GetBytes('unowned patcher'))
        Touch "$d\BepInEx\unstripped_corlib\mscorlib.dll" ($asciiEnc.GetBytes('no marker')) }
    Steps = @('restore')
}
$scenarios['unity_mono_restore_old_owned_snapshot'] = @{
    Build = { param($d) New-UnityMonoTree $d '2021.3.5f1' $PE_X64 $false
        $old = $asciiEnc.GetBytes('old launcher-owned Mono plugin')
        Touch "$d\BepInEx\plugins\UnityTranslator.dll" $old
        Touch "$d\BepInEx\plugins\UnityTranslator.dll.dst-owned" $old }
    Steps = @('restore')
}

# ---- Unity IL2CPP（BepInEx be.755 + XUnity、配置归属、禁用旧 Mono 文件）----
$scenarios['unity_il2cpp_deploy_restore'] = @{
    Build = { param($d) New-UnityIl2cppTree $d $PE_X64 }
    Steps = @('deploy', 'restore')
}
$scenarios['unity_il2cpp_deploy_twice_idempotent'] = @{
    Build = { param($d) New-UnityIl2cppTree $d $PE_X64 }
    Steps = @('deploy', 'deploy')
}
$scenarios['unity_il2cpp_unknown_machine_defaults_x64'] = @{
    Build = { param($d) New-UnityIl2cppTree $d 0 }
    Steps = @('deploy')
}
$scenarios['unity_il2cpp_x86_skipped'] = @{
    Build = { param($d) New-UnityIl2cppTree $d $PE_X86 }
    Steps = @('deploy', 'restore')
}
$scenarios['unity_il2cpp_old_bundled_mono_plugin_disabled'] = @{
    Build = { param($d) New-UnityIl2cppTree $d $PE_X64
        Touch "$d\BepInEx\plugins\UnityTranslator.dll" $tpl5
        Touch "$d\BepInEx\plugins\UnityTranslator.pdb" ($asciiEnc.GetBytes('pdb'))
        Touch "$d\BepInEx\core\Il2Cppmscorlib.dll" ($asciiEnc.GetBytes('old interop mscorlib')) }
    Steps = @('deploy', 'restore')
}
$scenarios['unity_il2cpp_old_owned_snapshot_disabled'] = @{
    Build = { param($d) New-UnityIl2cppTree $d $PE_X64
        $old = $asciiEnc.GetBytes('old launcher-owned Mono plugin')
        Touch "$d\BepInEx\plugins\UnityTranslator.dll" $old
        Touch "$d\BepInEx\plugins\UnityTranslator.dll.dst-owned" $old }
    Steps = @('deploy')
}
$scenarios['unity_il2cpp_old_owned_disabled_collision'] = @{
    Build = { param($d) New-UnityIl2cppTree $d $PE_X64
        $old = $asciiEnc.GetBytes('old launcher-owned Mono plugin')
        Touch "$d\BepInEx\plugins\UnityTranslator.dll" $old
        Touch "$d\BepInEx\plugins\UnityTranslator.dll.dst-owned" $old
        Touch "$d\BepInEx\plugins\UnityTranslator.dll.disabled" ($asciiEnc.GetBytes('user disabled DLL backup')) }
    Steps = @('deploy')
    ExpectedResult = 0
}
$scenarios['unity_il2cpp_old_interop_disabled_collision'] = @{
    Build = { param($d) New-UnityIl2cppTree $d $PE_X64
        Touch "$d\BepInEx\core\Il2Cppmscorlib.dll" ($asciiEnc.GetBytes('active old interop'))
        Touch "$d\BepInEx\core\Il2Cppmscorlib.dll.disabled" ($asciiEnc.GetBytes('user disabled interop backup')) }
    Steps = @('deploy')
    ExpectedResult = 0
}
$scenarios['unity_il2cpp_pdb_disabled_collision'] = @{
    Build = { param($d) New-UnityIl2cppTree $d $PE_X64
        Touch "$d\BepInEx\plugins\UnityTranslator.dll" $tpl5
        Touch "$d\BepInEx\plugins\UnityTranslator.pdb" ($asciiEnc.GetBytes('active pdb'))
        Touch "$d\BepInEx\plugins\UnityTranslator.pdb.disabled" ($asciiEnc.GetBytes('user disabled pdb backup')) }
    Steps = @('deploy', 'restore')
    ExpectedResults = @(1, 0)
}
$scenarios['unity_il2cpp_restore_old_owned_snapshot'] = @{
    Build = { param($d) New-UnityIl2cppTree $d $PE_X64
        $old = $asciiEnc.GetBytes('old launcher-owned Mono plugin')
        Touch "$d\BepInEx\plugins\UnityTranslator.dll.disabled" $old
        Touch "$d\BepInEx\plugins\UnityTranslator.dll.dst-owned" $old }
    Steps = @('restore')
}
$scenarios['unity_il2cpp_modified_old_owned_plugin_preserved'] = @{
    Build = { param($d) New-UnityIl2cppTree $d $PE_X64
        Touch "$d\BepInEx\plugins\UnityTranslator.dll" ($asciiEnc.GetBytes('user-modified Mono plugin'))
        Touch "$d\BepInEx\plugins\UnityTranslator.dll.dst-owned" ($asciiEnc.GetBytes('old launcher-owned Mono plugin')) }
    Steps = @('deploy')
}
$scenarios['unity_il2cpp_foreign_mono_plugin_kept'] = @{
    Build = { param($d) New-UnityIl2cppTree $d $PE_X64
        Touch "$d\BepInEx\plugins\UnityTranslator.dll" ($asciiEnc.GetBytes('third-party plugin with same name'))
        Touch "$d\BepInEx\plugins\UnityTranslator.dll.disabled" ($asciiEnc.GetBytes('unknown disabled copy')) }
    Steps = @('deploy', 'restore')
}
$scenarios['unity_il2cpp_config_owned_migration'] = @{
    Build = { param($d) New-UnityIl2cppTree $d $PE_X64
        # XUnity 向当前配置追加了自己的小节，但托管键仍与归属快照一致，应原地迁移
        Touch "$d\BepInEx\config\AutoTranslatorConfig.ini" ($asciiEnc.GetBytes("[Behaviour]`nMaxCharactersPerTranslation=200`nEnableBatching=True`n`n[XUnityAdded]`nFoo=1`n"))
        Touch "$d\BepInEx\config\AutoTranslatorConfig.ini.dst-owned" ($asciiEnc.GetBytes("[Behaviour]`nMaxCharactersPerTranslation=200`nEnableBatching=True`n")) }
    Steps = @('deploy', 'restore')
}
$scenarios['unity_il2cpp_config_user_modified_preserved'] = @{
    Build = { param($d) New-UnityIl2cppTree $d $PE_X64
        Touch "$d\BepInEx\config\AutoTranslatorConfig.ini" ($asciiEnc.GetBytes("[Behaviour]`nMaxCharactersPerTranslation=999`n"))
        Touch "$d\BepInEx\config\AutoTranslatorConfig.ini.dst-owned" ($asciiEnc.GetBytes("[Behaviour]`nMaxCharactersPerTranslation=200`n")) }
    Steps = @('deploy', 'restore')
}
$scenarios['unity_il2cpp_user_config_backed_up'] = @{
    Build = { param($d) New-UnityIl2cppTree $d $PE_X64
        Touch "$d\BepInEx\config\AutoTranslatorConfig.ini" ($asciiEnc.GetBytes("[Service]`nEndpoint=GoogleTranslate`n")) }
    Steps = @('deploy', 'restore')
}
$scenarios['unity_il2cpp_preexisting_xunity_dir_no_marker'] = @{
    Build = { param($d) New-UnityIl2cppTree $d $PE_X64
        Touch "$d\BepInEx\plugins\XUnity.AutoTranslator\XUnity.AutoTranslator.Plugin.Core.dll" ($asciiEnc.GetBytes('user xunity 5.3')) }
    Steps = @('deploy', 'restore')
}
$scenarios['unity_il2cpp_restore_untouched'] = @{
    Build = { param($d) New-UnityIl2cppTree $d $PE_X64 }
    Steps = @('restore')
}
$scenarios['unity_il2cpp_restore_mscorlib_conflict'] = @{
    Build = { param($d) New-UnityIl2cppTree $d $PE_X64
        Touch "$d\BepInEx\core\Il2Cppmscorlib.dll" ($asciiEnc.GetBytes('new'))
        Touch "$d\BepInEx\core\Il2Cppmscorlib.dll.disabled" ($asciiEnc.GetBytes('old')) }
    Steps = @('restore')
}

$cRootArg = $synthRoot
foreach ($name in $scenarios.Keys) {
    $sc = $scenarios[$name]
    $cDir = Join-Path $root "dr\c\$name"
    $csDir = Join-Path $root "dr\cs\$name"
    if (-not $sc.SkipCreate) {
        Mkdir $cDir; Mkdir $csDir
        & $sc.Build $cDir
        & $sc.Build $csDir
    }
    $step = 0
    $scenarioFailed = $false
    foreach ($op in $sc.Steps) {
        $step++
        $cFlag = if ($op -eq 'deploy') { '--deploy-and-exit' } else { '--restore-and-exit' }
        $csFlag = if ($op -eq 'deploy') { '--deploy' } else { '--restore' }
        $c = Invoke-Detect $CLauncherSynth @($cFlag, $cDir)
        $cs = Invoke-Detect $CsLauncher @($csFlag, $csDir, '--root', $cRootArg)
        $cOut = $c.Out.Replace($cDir, '<ROOT>')
        $csOut = $cs.Out.Replace($csDir, '<ROOT>')
        $cTree = if (Test-Path -LiteralPath $cDir) { (Snapshot-Tree $cDir) } else { '<missing>' }
        $csTree = if (Test-Path -LiteralPath $csDir) { (Snapshot-Tree $csDir) } else { '<missing>' }
        $label = "$name/$step-$op"
        if ($c.Code -ne $cs.Code) {
            $fail++; $scenarioFailed = $true
            $failures.Add("[$label] exit codes C=$($c.Code) C#=$($cs.Code)`n--- C ---`n$cOut--- C# ---`n$csOut")
            continue
        }
        if ($cOut -ne $csOut) {
            $fail++; $scenarioFailed = $true
            $failures.Add("[$label] stdout differs`n--- C ---`n$cOut--- C# ---`n$csOut")
            continue
        }
        if ($cTree -ne $csTree) {
            $fail++; $scenarioFailed = $true
            $failures.Add("[$label] resulting tree differs`n--- C ---`n$cTree`n--- C# ---`n$csTree")
            continue
        }
        if ($cOut -notmatch '(?m)^result=-?\d+$') {
            $fail++; $scenarioFailed = $true
            $failures.Add("[$label] no result= line`n$cOut")
            continue
        }
        $resultLine = ([regex]::Match($cOut, '(?m)^result=(-?\d+)$')).Groups[1].Value
        $expectedResult = if ($sc.ContainsKey('ExpectedResults')) { [string]$sc.ExpectedResults[$step - 1] }
            elseif ($sc.ContainsKey('ExpectedResult')) { [string]$sc.ExpectedResult }
            else { $null }
        if ($null -ne $expectedResult -and $resultLine -ne $expectedResult) {
            $fail++; $scenarioFailed = $true
            $failures.Add("[$label] expected result=$expectedResult, got result=$resultLine`n$cOut")
            continue
        }
        $pass++
        Write-Host ("  ok  {0,-52} result={1}" -f $label, $resultLine)
        if ($ShowLogs) { ($cOut -split "`n") | Where-Object { $_ -like 'log=*' } | ForEach-Object { Write-Host "        $_" } }
    }
}
# 验证部署后的钩子字节：部署完成后，钩子文件必须与组件源码一致
$hookCheckDir = Join-Path $root 'dr\c\renpy_deploy_twice_idempotent\game\iron_deepseek.rpy'
if (Test-Path -LiteralPath $hookCheckDir) {
    $deployed = [IO.File]::ReadAllBytes($hookCheckDir)
    if ([Linq.Enumerable]::SequenceEqual($deployed, $renpyHookBytes)) { $pass++; Write-Host "  ok  deployed Ren'Py hook is byte-identical to payloads\RenPy\iron_deepseek.rpy" }
    else { $fail++; $failures.Add("deployed Ren'Py hook differs from payload source") }
}
$hookCheckJs = Join-Path $root 'dr\c\rpgm_deploy_twice_idempotent\www\js\hook_rpgm_mv.js'
if (Test-Path -LiteralPath $hookCheckJs) {
    $deployed = [IO.File]::ReadAllBytes($hookCheckJs)
    if ([Linq.Enumerable]::SequenceEqual($deployed, $rpgmHookBytes)) { $pass++; Write-Host "  ok  deployed RPGM hook is byte-identical to payloads\RPGMaker\hook_rpgm_mv.js" }
    else { $fail++; $failures.Add("deployed RPGM hook differs from payload source") }
}

# 文件归属快照必须在翻译器升级后保留，用户修改过的文件也必须保持原样。
foreach ($side in @('c', 'cs')) {
    $base = Join-Path $root "dr\$side"
    $userFonts = @(
        @('renpy_font_already_present\game\ds_font.ttf', [byte[]](9,9)),
        @('renpy_user_otf_font_preserved\game\ds_font.otf', [byte[]](7,8,9)),
        @('renpy_modified_owned_font_preserved\game\ds_font.ttf', [byte[]](4,5,6)),
        @('rpgm_user_otf_font_preserved\www\fonts\ds_font.otf', [byte[]](7,8,9)),
        @('rpgm_user_ttf_font_preserved\www\fonts\ds_font.ttf', [byte[]](9,9))
    )
    $fontSafetyOk = $true
    foreach ($entry in $userFonts) {
        $fontPath = Join-Path $base $entry[0]
        if (-not (Test-Path -LiteralPath $fontPath) -or
            -not [Linq.Enumerable]::SequenceEqual([IO.File]::ReadAllBytes($fontPath), $entry[1])) {
            $fontSafetyOk = $false
        }
    }
    foreach ($owned in @('renpy_deploy_then_restore\game\ds_font.ttf',
                        'renpy_deploy_then_restore\game\ds_font.ttf.dst-owned',
                        'rpgm_www_deploy_restore\www\fonts\ds_font.ttf',
                        'rpgm_www_deploy_restore\www\fonts\ds_font.ttf.dst-owned')) {
        if (Test-Path -LiteralPath (Join-Path $base $owned)) { $fontSafetyOk = $false }
    }
    if ($fontSafetyOk) { $pass++; Write-Host "  ok  font ownership preserves user files and removes only owned copies ($side)" }
    else { $fail++; $failures.Add("[font-ownership/$side] user font changed or launcher-owned font remained") }
    $staleHook = Join-Path $base 'renpy_refresh_stale_hook\game\iron_deepseek.rpy'
    $staleJs = Join-Path $base 'rpgm_refresh_stale_hook\www\js\hook_rpgm_mv.js'
    $oldDisabled = Join-Path $base 'unity_il2cpp_old_owned_snapshot_disabled\BepInEx\plugins\UnityTranslator.dll.disabled'
    $oldActive = Join-Path $base 'unity_il2cpp_old_owned_snapshot_disabled\BepInEx\plugins\UnityTranslator.dll'
    $oldRestore = Join-Path $base 'unity_il2cpp_restore_old_owned_snapshot\BepInEx\plugins\UnityTranslator.dll.disabled'
    $monoRestore = Join-Path $base 'unity_mono_restore_old_owned_snapshot\BepInEx\plugins\UnityTranslator.dll'
    $modified = Join-Path $base 'unity_il2cpp_modified_old_owned_plugin_preserved\BepInEx\plugins\UnityTranslator.dll'
    $marker = Join-Path $base 'unity_mono_deploy_restore\BepInEx\plugins\UnityTranslator.dll.dst-owned'
    $dllCollision = Join-Path $base 'unity_il2cpp_old_owned_disabled_collision\BepInEx\plugins\UnityTranslator.dll.disabled'
    $interopCollision = Join-Path $base 'unity_il2cpp_old_interop_disabled_collision\BepInEx\core\Il2Cppmscorlib.dll.disabled'
    $pdbCollision = Join-Path $base 'unity_il2cpp_pdb_disabled_collision\BepInEx\plugins\UnityTranslator.pdb.disabled'
    $bundledPdb = Join-Path $base 'unity_il2cpp_old_bundled_mono_plugin_disabled\BepInEx\plugins\UnityTranslator.pdb'
    $foreignDisabled = Join-Path $base 'unity_il2cpp_foreign_mono_plugin_kept\BepInEx\plugins\UnityTranslator.dll.disabled'
    $secondPluginEdit = Join-Path $base 'unity_mono_second_plugin_edit_with_backup\BepInEx\plugins\UnityTranslator.dll'
    $secondJsonEdit = Join-Path $base 'unity_mono_second_newtonsoft_edit_with_backup\BepInEx\plugins\Newtonsoft.Json.dll'
    $restoredJsonMarker = Join-Path $base 'unity_mono_user_plugin_backup_restore\BepInEx\plugins\Newtonsoft.Json.dll.dst-owned'
    $matchingPlugin = Join-Path $base 'unity_mono_matching_backups_allow_deploy\BepInEx\plugins\UnityTranslator.dll'
    $matchingJson = Join-Path $base 'unity_mono_matching_backups_allow_deploy\BepInEx\plugins\Newtonsoft.Json.dll'
    $upgradedPlugin = Join-Path $base 'unity_mono_upgrade_owned_dependency\BepInEx\plugins\UnityTranslator.dll'
    $upgradedJson = Join-Path $base 'unity_mono_upgrade_owned_dependency\BepInEx\plugins\Newtonsoft.Json.dll'
    $ok = (Test-Path -LiteralPath $staleHook) -and
        [Linq.Enumerable]::SequenceEqual([IO.File]::ReadAllBytes($staleHook), $renpyHookBytes) -and
        (Test-Path -LiteralPath $staleJs) -and
        [Linq.Enumerable]::SequenceEqual([IO.File]::ReadAllBytes($staleJs), $rpgmHookBytes) -and
        (Test-Path -LiteralPath $oldDisabled) -and -not (Test-Path -LiteralPath $oldActive) -and
        -not (Test-Path -LiteralPath $oldRestore) -and -not (Test-Path -LiteralPath ($oldRestore -replace '\.disabled$', '.dst-owned')) -and
        -not (Test-Path -LiteralPath $monoRestore) -and -not (Test-Path -LiteralPath ($monoRestore + '.dst-owned')) -and
        (Test-Path -LiteralPath $modified) -and
        [IO.File]::ReadAllText($modified) -eq 'user-modified Mono plugin' -and
        -not (Test-Path -LiteralPath $marker) -and
        (Test-Path -LiteralPath $dllCollision) -and [IO.File]::ReadAllText($dllCollision) -eq 'user disabled DLL backup' -and
        (Test-Path -LiteralPath $interopCollision) -and [IO.File]::ReadAllText($interopCollision) -eq 'user disabled interop backup' -and
        (Test-Path -LiteralPath $pdbCollision) -and [IO.File]::ReadAllText($pdbCollision) -eq 'user disabled pdb backup' -and
        (Test-Path -LiteralPath $bundledPdb) -and [IO.File]::ReadAllText($bundledPdb) -eq 'pdb' -and
        -not (Test-Path -LiteralPath ($bundledPdb + '.disabled')) -and
        (Test-Path -LiteralPath $foreignDisabled) -and [IO.File]::ReadAllText($foreignDisabled) -eq 'unknown disabled copy' -and
        (Test-Path -LiteralPath $secondPluginEdit) -and [IO.File]::ReadAllText($secondPluginEdit) -eq 'new user plugin edit' -and
        (Test-Path -LiteralPath $secondJsonEdit) -and [IO.File]::ReadAllText($secondJsonEdit) -eq 'new user Newtonsoft edit' -and
        -not (Test-Path -LiteralPath $restoredJsonMarker) -and
        (Test-Path -LiteralPath $matchingPlugin) -and [IO.File]::ReadAllText($matchingPlugin) -eq 'user plugin matching backup' -and
        (Test-Path -LiteralPath $matchingJson) -and [IO.File]::ReadAllText($matchingJson) -eq 'user Newtonsoft matching backup' -and
        (Test-Path -LiteralPath $upgradedPlugin) -and [IO.File]::ReadAllText($upgradedPlugin) -eq 'user plugin before launcher' -and
        (Test-Path -LiteralPath $upgradedJson) -and [IO.File]::ReadAllText($upgradedJson) -eq 'user Newtonsoft before launcher' -and
        -not (Test-Path -LiteralPath ($upgradedPlugin + '.dst-owned')) -and
        -not (Test-Path -LiteralPath ($upgradedJson + '.dst-owned'))
    if ($ok) { $pass++; Write-Host "  ok  deploy refresh and old Unity ownership ($side)" }
    else { $fail++; $failures.Add("[deploy refresh and old Unity ownership/$side] incorrect file state") }
}

# 受支持的钩子部署失败时，一键启动不得报告翻译启动成功。
# 此模式执行真实部署流程，然后记录启动请求，不实际启动游戏。
$deployLaunchScenarios = [ordered]@{
    renpy_ok = @{ Build = { param($d) Touch "$d\game\script.rpy"; Touch "$d\Game.exe" }; Result = 1 }
    renpy_blocked_hook = @{ Build = { param($d) Touch "$d\game\script.rpy"; Touch "$d\Game.exe"; Mkdir "$d\game\iron_deepseek.rpy" }; Result = 0 }
    renpy_missing_exe = @{ Build = { param($d) Touch "$d\game\script.rpy" }; Result = 0; LaunchFailure = $true }
    rpgm_ok = @{ Build = { param($d) New-RpgmTree $d $indexPlain $true }; Result = 1 }
    rpgm_blocked_hook = @{ Build = { param($d) New-RpgmTree $d $indexPlain $true; Mkdir "$d\www\js\hook_rpgm_mv.js" }; Result = 0 }
    rpgm_missing_exe = @{ Build = { param($d) New-RpgmTree $d $indexPlain $true; Remove-Item -LiteralPath "$d\Game.exe" }; Result = 0; LaunchFailure = $true }
    unity_mono_missing_runtime = @{ Build = { param($d) New-UnityMonoTree $d '6000.1.0f1' $PE_X86 $false }; Result = 0 }
    unity_mono_backup_conflict = @{ Build = { param($d) New-UnityMonoTree $d '2021.3.5f1' $PE_X64 $false
        Touch "$d\BepInEx\plugins\UnityTranslator.dll" ($asciiEnc.GetBytes('new user plugin edit'))
        Touch "$d\BepInEx\plugins\UnityTranslator.dll.dst-backup" ($asciiEnc.GetBytes('prior user plugin edit'))
        Touch "$d\BepInEx\plugins\UnityTranslator.dll.dst-owned" ($asciiEnc.GetBytes('old launcher-owned plugin')) }; Result = 0 }
    unity_il2cpp_unsupported_arch = @{ Build = { param($d) New-UnityIl2cppTree $d $PE_X86 }; Result = 0 }
    unity_il2cpp_disabled_collision = @{ Build = { param($d) New-UnityIl2cppTree $d $PE_X64
        $old = $asciiEnc.GetBytes('old launcher-owned Mono plugin')
        Touch "$d\BepInEx\plugins\UnityTranslator.dll" $old
        Touch "$d\BepInEx\plugins\UnityTranslator.dll.dst-owned" $old
        Touch "$d\BepInEx\plugins\UnityTranslator.dll.disabled" ($asciiEnc.GetBytes('user disabled DLL backup')) }; Result = 0 }
    unity_il2cpp_blocked_old_plugin = @{ Build = { param($d) New-UnityIl2cppTree $d $PE_X64
        Touch "$d\BepInEx\plugins\UnityTranslator.dll" $tpl5
        Mkdir "$d\BepInEx\plugins\UnityTranslator.dll.disabled" }; Result = 0 }
    unity_il2cpp_blocked_old_interop = @{ Build = { param($d) New-UnityIl2cppTree $d $PE_X64
        Touch "$d\BepInEx\core\Il2Cppmscorlib.dll" ($asciiEnc.GetBytes('old interop'))
        Mkdir "$d\BepInEx\core\Il2Cppmscorlib.dll.disabled" }; Result = 0 }
    unknown_no_hook = @{ Build = { param($d) Touch "$d\Game.exe" }; Result = 1; NoHook = $true }
    rpgm_legacy_no_hook = @{ Build = { param($d) Touch "$d\Data\Scripts.rxdata"; Touch "$d\Game.exe" }; Result = 1; NoHook = $true }
}
foreach ($name in $deployLaunchScenarios.Keys) {
    $sc = $deployLaunchScenarios[$name]
    $cDir = Join-Path $root "dlf\c\$name"
    $csDir = Join-Path $root "dlf\cs\$name"
    Mkdir $cDir; Mkdir $csDir
    & $sc.Build $cDir
    & $sc.Build $csDir
    $c = Invoke-Detect $CLauncherSynth @('--deploy-launch-flow-and-exit', $cDir)
    $cs = Invoke-Detect $CsLauncher @('--deploy-launch-flow', $csDir, '--root', $synthRoot)
    $cOut = $c.Out.Replace($cDir, '<DIR>')
    $csOut = $cs.Out.Replace($csDir, '<DIR>')
    $spawnCount = @($cOut -split "`n" | Where-Object { $_ -like 'spawn=*' }).Count
    $postCount = @($cOut -split "`n" | Where-Object { $_ -like 'post=*' }).Count
    $expectedSpawn = if ($sc.Result -eq 1) { 1 } else { 0 }
    $statusOk = if ($sc.LaunchFailure) { $cOut -match '(?m)^status=游戏启动失败；请查看日志。$' }
        elseif ($sc.Result -eq 0) { $cOut -match '(?m)^status=翻译组件部署失败，未启动游戏；请查看日志。$' }
        elseif ($sc.NoHook) { $cOut -match '(?m)^status=游戏已启动；当前引擎未部署实时翻译模块，请查看日志。$' }
        else { $cOut -match '(?m)^status=游戏已启动；翻译组件已部署，游戏内加载待确认。$' }
    if ($c.Code -ne $cs.Code -or $cOut -ne $csOut -or
        (Snapshot-Tree $cDir) -ne (Snapshot-Tree $csDir) -or
        $cOut -notmatch "(?m)^result=$($sc.Result)$" -or
        $spawnCount -ne $expectedSpawn -or ($sc.Result -eq 0 -and $postCount -ne 0) -or
        -not $statusOk) {
        $fail++; $failures.Add("[deploy-launch-flow/$name] C/C# parity or outcome failed`n--- C ---`n$cOut--- C# ---`n$csOut")
    } else {
        $pass++; Write-Host ("  ok  {0,-52} result={1} spawn={2}" -f "deploy-launch-flow/$name", $sc.Result, $spawnCount)
    }
}

# ---------------------------------------------------------------- 预热扫描一致性
#
# --warmup-and-exit（C）/ --warmup（C#）：预热扫描器以转储模式运行，
# 不联网、不查缓存、不回写翻译文件；所有原本要 POST 的批次请求体
# 均输出为 "post=<path> <json>"。收集的文本、顺序、Ren'Py prevs 数组、
# JSON 转义、镜像 log= 行和 result= 都必须逐字节一致。
# 两个启动器均只读夹具，因此共用一份；执行前后比较目录树快照，
# 确保转储模式确实未修改游戏目录。

$utf8 = New-Object Text.UTF8Encoding $false
function Write-Utf8([string]$path, [string]$text) { Touch $path ($utf8.GetBytes($text)) }
function Write-Utf8Bom([string]$path, [string]$text) { Touch $path ([byte[]](0xEF, 0xBB, 0xBF) + $utf8.GetBytes($text)) }
# Unity 序列化字符串：小端 u32 长度 + 内容（可选对齐填充）。
function U32Str([string]$s, [int]$pad = 0) {
    $b = $asciiEnc.GetBytes($s)
    return [byte[]]([BitConverter]::GetBytes([uint32]$b.Length) + $b + (New-Object byte[] $pad))
}
$NUL = [byte[]]@(0)

function New-RenpyGame([string]$d) { Touch "$d\Game.exe"; Mkdir "$d\game" }
function New-RpgmWww([string]$d) {
    Touch "$d\www\index.html"; Touch "$d\www\js\main.js"; Touch "$d\www\js\rpg_core.js"; Touch "$d\Game.exe"
    Write-Utf8 "$d\www\data\System.json" '{"gameTitle":"x"}'
}
function New-UnityGame([string]$d) { Mkdir "$d\MyGame_Data\Managed"; Touch "$d\MyGame.exe"; Touch "$d\UnityPlayer.dll" }

$warmupScenarios = New-Object System.Collections.Specialized.OrderedDictionary

# ---- Ren'Py 预热 ----
$warmupScenarios['warm_renpy_dialogue'] = {
    param($d) New-RenpyGame $d
    Write-Utf8 "$d\game\script.rpy" @"
label start:
    e "Hello there, traveler! Welcome to the town."
    "It was a dark and stormy night."
    menu:
        "Go left":
            jump left
        "Go right":
            jump right
    image bg = "bg.png"
    show eileen happy
    define e = Character("Eileen")
    play music "track.ogg"
    e "Emoji test \ud83d\ude00 and lone \ud83d surrogate and \udc00 low."
    "Multiple" "literals on one" "line, all collected"
    "Short"
    "sixteen_or_more_chars"
    'single quoted line here.'
    `$ x = "assigned before quote"
    "Tab\tand\nnewline\rand\\backslash"
    "Unicode escape \u00e9 done."
    "Café au lait — non-ASCII ok"
    "这是中文，不预热"
    "http://example.com/page"
    "image.jpg"
    "unterminated literal
    """triple quoted docstring"""
    e "Ends with \u12"
"@
    Write-Utf8 "$d\game\iron_deepseek.rpy" '"launcher hook literal must be skipped"'
    Write-Utf8 "$d\game\empty.rpy" ''
}
# prevs 按文件独立计算：第二个文件的首条文本必须重新使用 prev=""；
# 单行包含 17 个字面量时，应触发最多 16 个的限制。
$warmupScenarios['warm_renpy_prevs_and_cap'] = {
    param($d) New-RenpyGame $d
    Write-Utf8 "$d\game\a.rpy" "`"First in a.`"`n`"Second in a.`"`n"
    Write-Utf8 "$d\game\b.rpy" "`"First in b.`"`n`"Second in b.`"`n"
    $many = (1..17 | ForEach-Object { "`"literal number $_ ok`"" }) -join ' '
    Write-Utf8 "$d\game\c.rpy" "$many`n"
}
# 递归规则：只有父目录自身含 *.rpy 时才进入子目录；C 版的
# FindFirstFileW(*.rpy) 失败会提前返回，深度为 13 的目录被跳过。
$warmupScenarios['warm_renpy_recursion'] = {
    param($d) New-RenpyGame $d
    Write-Utf8 "$d\game\root.rpy" "`"Root level line.`"`n"
    Write-Utf8 "$d\game\tl\chinese\strings.rpy" "`"Nested translation line.`"`n"
    Write-Utf8 "$d\game\tl\deeper\again\x.rpy" "`"Deeper nested line.`"`n"
    Write-Utf8 "$d\game\nosub\deep.rpy" "`"Under nosub, but nosub has rpy so found.`"`n"
    Write-Utf8 "$d\game\onlysub\inner\lost.rpy" "`"Parent has no .rpy, never scanned.`"`n"
    $deep = "$d\game"; for ($i = 1; $i -le 13; $i++) { $deep = "$deep\d$i"; Write-Utf8 "$deep\l.rpy" "`"Depth $i line here.`"`n" }
}
# 超过 8 MiB 的脚本，以及 NUL 字节导致文件提前结束的情况。
$warmupScenarios['warm_renpy_size_and_nul'] = {
    param($d) New-RenpyGame $d
    $big = New-Object byte[] (8 * 1024 * 1024 + 1); Touch "$d\game\big.rpy" $big
    Touch "$d\game\nul.rpy" ($utf8.GetBytes("`"Before the NUL byte.`"`n") + [byte[]](0) + $utf8.GetBytes("`"After the NUL byte.`"`n"))
    Write-Utf8 "$d\game\crlf.rpy" "`"CRLF line one.`"`r`n`"CRLF line two.`"`r`n"
}
$warmupScenarios['warm_renpy_no_game_dir'] = { param($d) Touch "$d\Game.exe"; Touch "$d\game.rpa" }
$warmupScenarios['warm_renpy_ignores_rpgm_plugins'] = {
    param($d) New-RenpyGame $d
    Write-Utf8 "$d\game\script.rpy" '"RenPy dialogue stays visible."'
    Write-Utf8 "$d\www\js\plugins.js" 'var $plugins = [{"status":true,"parameters":{"Load Help":"RPGM plugin text must not leak."}}];'
}

# ---- RPG Maker MV/MZ 预热 ----
$warmupScenarios['warm_rpgm_plugin_ui_parameters'] = {
    param($d) New-RpgmWww $d
    Write-Utf8 "$d\www\js\plugins.js" @'
// Generated by RPG Maker.
var $plugins =
[
{"name":"Active_UI","status":true,"description":"Not a runtime label.","parameters":{"Load Help":"Loads the data from the saved game.","Playtime":"Playtime:","Save Count":"Total Saves:","Confirm Yes":"Yes, load it.","Level Format":"\\c[16]Level %1","Delete Filename":"Damage2","Data Column 1":"variable 1, variable 2","Script":"this.lineHeight()","Nested Text":"[\"Not directly displayed\"]","Web Save":"RPG %1 File%2"}},
{"name":"Off_UI","status":false,"parameters":{"Load Help":"Disabled plugin text must not queue."}},
{"name":"Late_Status","parameters":{"Save Text":"Save the game now?"},"status":true}
];
'@
    Write-Utf8 "$d\www\data\Items.json" '[null,{"name":"Data item follows plugin labels."}]'
}
$warmupScenarios['warm_rpgm_bad_plugin_manifest'] = {
    param($d) New-RpgmWww $d
    Write-Utf8 "$d\www\js\plugins.js" 'var $plugins = [{"status":true,"parameters":{"Help Text":"Incomplete plugin text."}'
    Write-Utf8 "$d\www\data\Items.json" '[null,{"name":"Data still scanned after bad plugins."}]'
}
$warmupScenarios['warm_rpgm_events_and_db'] = {
    param($d) New-RpgmWww $d
    Write-Utf8 "$d\www\data\Map001.json" @"
{"displayName":"Town Square","name":"EditorLabelNotShown","events":[null,{"id":1,"name":"EV001","pages":[{"list":[
{"code":101,"indent":0,"parameters":["Actor1",0,0,2,"Harold"]},
{"code":401,"indent":0,"parameters":["\\C[2]Harold\\C[0]: Welcome to our village."]},
{"code":401,"indent":0,"parameters":["Please make yourself at home."]},
{"code":102,"indent":0,"parameters":[["Yes, thank you.","No, I must go."],1,0,2,0]},
{"code":405,"indent":0,"parameters":["Scrolling epilogue text line."]},
{"code":108,"indent":0,"parameters":["A comment, not shown"]},
{"code":101,"indent":0,"parameters":["Actor1",0,0,2]},
{"code":401,"indent":0,"parameters":["Single line message."]},
{"code":0,"indent":0,"parameters":[]}
]}]}]}
"@
    Write-Utf8 "$d\www\data\Actors.json" '[null,{"id":1,"name":"Harold","nickname":"The Brave","profile":"A young knight.\nHe likes bread.","note":"<skip:me>"},{"id":2,"name":"\\N[1] jr.","nickname":""}]'
    Write-Utf8 "$d\www\data\Items.json" '[null,{"id":1,"name":"Potion","description":"Restores 100 HP.\\I[64] Nice.","note":""},{"id":2,"name":"C:\\Users\\bad\\path","description":"\\FS[24]Big text \\{here\\}"}]'
    Write-Utf8 "$d\www\data\Skills.json" '[null,{"id":1,"name":"Fire","message1":" casts %1!","message2":"","description":"Burn \\C[20]them\\C[0] all"}]'
    Write-Utf8 "$d\www\data\System.json" '{"gameTitle":"Test Game","currencyUnit":"G","elements":["","Physical","Fire","Ice"],"equipTypes":["","Weapon","Shield"],"terms":{"basic":["Level","Lv","HP","HP","MP","MP"],"commands":["Fight","Escape","Attack",null,"Guard"],"params":["Max HP","Max MP"],"messages":{"actionFailure":"There was no effect on %1!","alwaysDash":"Always Dash"}},"skillTypes":["","Magic","Special"]}'
    Write-Utf8 "$d\www\data\MapInfos.json" '[null,{"id":1,"expanded":false,"name":"Town Square Map","order":1,"parentId":0,"scrollX":0,"scrollY":0}]'
    Write-Utf8Bom "$d\www\data\Weapons.json" '[null,{"id":1,"name":"Bronze Sword","description":"A sword made of bronze. \u00e9\ud83d\ude00"}]'
}
# 带说话人前缀的文本（\n<Name>、\pop、<tag>）分别收集带前缀和去前缀版本。
$warmupScenarios['warm_rpgm_prefix_codes'] = {
    param($d) New-RpgmWww $d
    Write-Utf8 "$d\www\data\Map002.json" @"
{"events":[null,{"pages":[{"list":[
{"code":101,"parameters":["",0,0,2]},
{"code":401,"parameters":["\\n<Harold>Good morning, everyone."]},
{"code":101,"parameters":["",0,0,2]},
{"code":401,"parameters":["\\nd1[Boss] Prepare to fight!"]},
{"code":101,"parameters":["",0,0,2]},
{"code":401,"parameters":["<Marsha>: I have heard rumours."]},
{"code":101,"parameters":["",0,0,2]},
{"code":401,"parameters":["\\POP[3]<Sage>Ancient wisdom follows."]},
{"code":101,"parameters":["",0,0,2]},
{"code":401,"parameters":["\\XX[1]Unknown code rejects the line."]},
{"code":101,"parameters":["",0,0,2]},
{"code":401,"parameters":["Trailing backslash rejected\\"]},
{"code":101,"parameters":["",0,0,2]},
{"code":401,"parameters":["Gold: \\G and \\\\ escaped and \\$ ok"]},
{"code":101,"parameters":["",0,0,2]},
{"code":401,"parameters":["\\V[1] variable and \\P[2] party"]}
]}]}]}
"@
}
# 插件外部文本：Recipes.txt、quest/Quests.txt（Galv），有表头和无表头的 CSV，
# 分隔符检测（分号、逗号、制表符），带内嵌换行的引号字段，以及跳过资源目录。
$warmupScenarios['warm_rpgm_external_text'] = {
    param($d) New-RpgmWww $d
    Write-Utf8Bom "$d\www\data\Recipes.txt" "# comment line`nIron Sword Recipe`n<recipe 1>`n  Requires two iron bars.  `n</recipe>`nC:\\bad\\path`n"
    Write-Utf8 "$d\www\quest\Quests.txt" "<quest 1:Find the Lost Cat|Easy|Side>`nThe cat was last seen near the well.`n<quest 2:Main Story - Chapter One - The Beginning|Hard>`n<quest 3:NoPipeTitle>`n<quest 4 missing colon>`n</quest>`n"
    Write-Utf8 "$d\www\img\meta.txt" "Image metadata must be skipped."
    Write-Utf8 "$d\www\js\plugins\notes.txt" "JavaScript folder skipped too."
    Write-Utf8 "$d\www\game_messages.csv" "id,English,Japanese`nmsg_001,`"Hello, world!`",こんにちは`nmsg_002,`"Line one`nline two`",テスト`nmsg_003,`"Quoted `"`"inner`"`" text`",引用`n"
    Write-Utf8 "$d\www\semicolon.csv" "key;en_US;de`nGREET;Good day to you.;Guten Tag`n"
    Write-Utf8 "$d\www\tab.csv" "id`tsource`ttarget`n1`tTab separated source text.`tX`n"
    Write-Utf8 "$d\www\noheader.csv" "npc_01,Hello adventurer, welcome!,other words here`nitem_99,short_id,Another full sentence.`n"
    Write-Utf8 "$d\www\nodelim.csv" "just one column no delimiter`n"
}
# 扁平 MZ 布局（无 www/）、CRLF 文本、跳过过大 JSON、NUL 终止 JSON 解析，
# 以及闭括号后的空白（固定 2026-09-09 的 C 版缓冲区末尾修复行为）。
$warmupScenarios['warm_rpgm_flat_and_limits'] = {
    param($d)
    Touch "$d\index.html"; Touch "$d\js\main.js"; Touch "$d\js\rmmz_core.js"; Touch "$d\Game.exe"
    Write-Utf8 "$d\data\System.json" '{"gameTitle":"Flat MZ Game"}'
    Write-Utf8 "$d\js\plugins.js" 'var $plugins = [{"status":true,"parameters":{"Help Text":"Flat layout plugin help."}}];'
    Write-Utf8 "$d\data\Armors.json" "[null,{`"id`":1,`"name`":`"Leather Armor`",`"description`":`"Light and flexible.   `"}]`r`n   `n`t"
    Touch "$d\data\Classes.json" ($utf8.GetBytes('[null,{"id":1,"name":"Before NUL Class"},') + [byte[]](0) + $utf8.GetBytes('{"id":2,"name":"After NUL Class"}]'))
    Touch "$d\data\Huge.json" (New-Object byte[] (64 * 1024 * 1024 + 1))
    Write-Utf8 "$d\data\Lines.txt" "Windows line one.`r`nWindows line two.`r`n`r`nLast without newline"
}
$warmupScenarios['warm_rpgm_no_content_root'] = { param($d) Touch "$d\Game.exe"; Touch "$d\www\index.html" }

# ---- Unity（XUnity 翻译文件及 *_Data 资源/资源包）----
$warmupScenarios['warm_unity_xunity_and_assets'] = {
    param($d) New-UnityGame $d
    Write-Utf8Bom "$d\BepInEx\Translation\zh-CN\Text\_AutoGeneratedTranslations.txt" @"
// comment line
# another comment
; and another
Hello adventurer=你好，冒险者
Identity entry=Identity entry
Empty value entry=
Escaped \= equals and \\ backslash=转义
Multi\nline\ttext=多行
Already Chinese 你好=X
=no key
noequals line
Short=x
"@
    Write-Utf8 "$d\BepInEx\Translation\zh-CN\Text\_Substitutions.txt" "Skipped=跳过"
    Write-Utf8 "$d\Translation\zh-CN\Text\extra.txt" "Second directory entry=第二"
    Write-Utf8 "$d\AutoTranslator\Translation\zh-CN\Text\third.txt" "Hello adventurer=重复的键不再导入`nThird directory entry=第三"
    # resources.assets：混在二进制数据中的 u32 长度前缀 ASCII 字符串
    $assets = [byte[]]@(0, 1, 2, 3, 0xFF, 0xFE) + (U32Str "Welcome to the dungeon, hero." 3) + (U32Str "Base Layer" 2) + (U32Str "m_Material" 2) +
              (U32Str "<color=#ff0000>Danger!</color> Ahead <quest>lies</quest> the beast." 1) + (U32Str "Untitled (Clone)") + (U32Str "_internalName") +
              (U32Str "x") + (U32Str "NoPunctuationHere") + (U32Str "Tab`tand newline`nallowed inside.") + (New-Object byte[] 9)
    Touch "$d\MyGame_Data\resources.assets" $assets
    Touch "$d\MyGame_Data\sharedassets0.assets" ((U32Str "Shared asset dialogue line, yes.") + (New-Object byte[] 12))
    Touch "$d\MyGame_Data\level0" ((U32Str "Level zero says hi!") + (New-Object byte[] 12))
    Touch "$d\MyGame_Data\level0x" ((U32Str "Not a level file, ignored.") + (New-Object byte[] 12))
    Touch "$d\MyGame_Data\globalgamemanagers" ((U32Str "Global managers text here.") + (New-Object byte[] 12))
    Touch "$d\MyGame_Data\other.dat" ((U32Str "Unknown extension, ignored.") + (New-Object byte[] 12))
    # AssetBundle：由二进制字节分隔的可打印片段，以及序列化残留变体
    $bundle = [byte[]]@(0x55, 0x6E, 0x69, 0x74, 0x79, 0x46, 0x53, 0x00) +
              $asciiEnc.GetBytes("[speaker:Harold] Good morning, my friend.0") + [byte[]]@(0, 1) +
              $asciiEnc.GetBytes("- Collect five herbs2") + $NUL +
              $asciiEnc.GetBytes("What do you mean?(") + $NUL +
              $asciiEnc.GetBytes("lowercase start rejected.") + $NUL +
              $asciiEnc.GetBytes("ALLCAPS SHOUTING TEXT.") + $NUL +
              $asciiEnc.GetBytes("MixedCase Word rejected here.") + $NUL +
              $asciiEnc.GetBytes("First line here.`r`nSecond line here.") + $NUL +
              $asciiEnc.GetBytes("<b>Bold</b> rich text, with tags.") + $NUL +
              $asciiEnc.GetBytes("ab") + $NUL +
              $asciiEnc.GetBytes(("Run " + ("x" * 1300) + " overflow.")) + $NUL +
              $asciiEnc.GetBytes("Tail run without terminator.")
    Touch "$d\MyGame_Data\StreamingAssets.unity3d" $bundle
    Touch "$d\MyGame_Data\pack.bundle" ($asciiEnc.GetBytes("Bundle extension text works.") + (New-Object byte[] 4))
    Touch "$d\MyGame_Data\pack.assetbundle" ($asciiEnc.GetBytes("Assetbundle extension text works.") + (New-Object byte[] 4))
    Touch "$d\MyGame_Data\empty.bundle" (New-Object byte[] 0)
    Touch "$d\MyGame_Data\Managed\Assembly-CSharp.dll" ((U32Str "Managed subdir is not scanned.") + (New-Object byte[] 12))
}
$warmupScenarios['warm_unity_il2cpp_second_data_dir'] = {
    param($d) Mkdir "$d\MyGame_Data\il2cpp_data\Metadata"; Touch "$d\MyGame.exe"; Touch "$d\GameAssembly.dll"
    Touch "$d\MyGame_Data\resources.assets" ((U32Str "IL2CPP game resources text.") + (New-Object byte[] 12))
    Touch "$d\Other_Data\resources.assets" ((U32Str "Second data folder text.") + (New-Object byte[] 12))
    Touch "$d\Huge_Data\resources.assets" (New-Object byte[] (64 * 1024 * 1024 + 1))
}
$warmupScenarios['warm_unity_nothing_to_do'] = { param($d) New-UnityGame $d }

# ---- Godot（项目/导出目录、.translation、PCK 格式 1/2/3、内嵌 pck 的 EXE）----
# Godot PCK 构造器。$entries 为哈希表数组：@{ Path; Data = [byte[]]; Pad; OffsetOverride; SizeOverride }。
# Pad 将路径用 NUL 填充到 4 字节倍数，与 Godot 一致。格式 1 使用绝对偏移，
# 包内嵌于 EXE 时加上 $absBase；格式 2 使用相对 file_base 的偏移，目录紧跟头部；
# 格式 3 将目录（u32 条目数 + 条目）放在 directory_offset 处。
function New-GodotPck([int]$format, [object[]]$entries, [long]$absBase = 0, [int]$gap = 0, [int]$minor = 4) {
    $metaSize = 36; $headerSize = 112
    if ($format -eq 1) { $metaSize = 32; $headerSize = 88 } elseif ($format -eq 2) { $headerSize = 100 }
    $paths = New-Object 'System.Collections.Generic.List[byte[]]'
    $dirSize = 0; $totalData = 0
    foreach ($e in $entries) {
        $pb = $utf8.GetBytes([string]$e.Path)
        if ($e.Pad) { $padTo = [int]([Math]::Ceiling(($pb.Length + 1) / 4.0) * 4); $pb = [byte[]]($pb + (New-Object byte[] ($padTo - $pb.Length))) }
        $paths.Add($pb); $dirSize += 4 + $pb.Length + $metaSize; $totalData += ([byte[]]$e.Data).Length
    }
    if ($format -in @(3, 4)) { $dataStart = $headerSize; $dirStart = $headerSize + $totalData + $gap; $fileBase = $headerSize; $total = $dirStart + 4 + $dirSize }
    else { $dirStart = $headerSize; $dataStart = $headerSize + $dirSize + $gap; $fileBase = $dataStart; $total = $dataStart + $totalData }
    $buf = New-Object byte[] $total
    $asciiEnc.GetBytes('GDPC').CopyTo($buf, 0)
    [BitConverter]::GetBytes([uint32]$format).CopyTo($buf, 4)
    $major = if ($format -eq 1) { 3 } else { 4 }
    [BitConverter]::GetBytes([uint32]$major).CopyTo($buf, 8)
    [BitConverter]::GetBytes([uint32]$minor).CopyTo($buf, 12)
    if ($format -eq 1) { [BitConverter]::GetBytes([uint32]$entries.Count).CopyTo($buf, 84) }
    else {
        [BitConverter]::GetBytes([uint64]$fileBase).CopyTo($buf, 24)
        if ($format -eq 2) { [BitConverter]::GetBytes([uint32]$entries.Count).CopyTo($buf, 96) }
        else { [BitConverter]::GetBytes([uint64]$dirStart).CopyTo($buf, 32); [BitConverter]::GetBytes([uint32]$entries.Count).CopyTo($buf, $dirStart) }
    }
    $dpos = if ($format -in @(3, 4)) { $dirStart + 4 } else { $dirStart }
    $running = 0
    for ($i = 0; $i -lt $entries.Count; $i++) {
        $e = $entries[$i]; $pb = $paths[$i]; $data = [byte[]]$e.Data
        [BitConverter]::GetBytes([uint32]$pb.Length).CopyTo($buf, $dpos); $dpos += 4
        $pb.CopyTo($buf, $dpos); $dpos += $pb.Length
        $off = if ($format -eq 1) { [uint64]($absBase + $dataStart + $running) } else { [uint64]$running }
        if ($null -ne $e.OffsetOverride) { $off = [uint64]$e.OffsetOverride }
        $size = [uint64]$data.Length
        if ($null -ne $e.SizeOverride) { $size = [uint64]$e.SizeOverride }
        [BitConverter]::GetBytes($off).CopyTo($buf, $dpos); $dpos += 8
        [BitConverter]::GetBytes($size).CopyTo($buf, $dpos); $dpos += 8
        $dpos += 16                              # MD5 字段填零
        if ($format -ne 1) { $dpos += 4 }        # 标志位
        $data.CopyTo($buf, $dataStart + $running); $running += $data.Length
    }
    return $buf
}
# PE 包含用于干扰的 .text 区段和存放包的 "pck" 区段；Godot 4 内嵌布局为
# 包字节、u64 包长度和 GDPC 尾标记。格式 1 偏移转换为相对 EXE 的绝对偏移。
function New-GodotEmbeddedExe([int]$format, [object[]]$entries, [bool]$tail) {
    $dataOff = 168                               # 64 + 4 + 20 + 2 * 40
    $pck = New-GodotPck $format $entries $dataOff
    $dos = New-Object byte[] 64; $dos[0] = 0x4D; $dos[1] = 0x5A; [BitConverter]::GetBytes([int32]64).CopyTo($dos, 60)
    $sig = [byte[]](0x50, 0x45, 0x00, 0x00)
    $fh = New-Object byte[] 20
    [BitConverter]::GetBytes([uint16]0x8664).CopyTo($fh, 0); [BitConverter]::GetBytes([uint16]2).CopyTo($fh, 2); [BitConverter]::GetBytes([uint16]0).CopyTo($fh, 16)
    $sh1 = New-Object byte[] 40; $asciiEnc.GetBytes('.text').CopyTo($sh1, 0)
    [BitConverter]::GetBytes([uint32]16).CopyTo($sh1, 16); [BitConverter]::GetBytes([uint32]$dataOff).CopyTo($sh1, 20)
    $sh2 = New-Object byte[] 40; $asciiEnc.GetBytes('pck').CopyTo($sh2, 0)
    [BitConverter]::GetBytes([uint32]$pck.Length).CopyTo($sh2, 16); [BitConverter]::GetBytes([uint32]$dataOff).CopyTo($sh2, 20)
    $sizeBytes = [BitConverter]::GetBytes([uint64]$pck.Length)
    $gd = if ($tail) { [byte[]](0x47, 0x44, 0x50, 0x43) } else { [byte[]](0, 0, 0, 0) }
    return [byte[]]($dos + $sig + $fh + $sh1 + $sh2 + $pck + $sizeBytes + $gd)
}
function GodotEntry([string]$path, [byte[]]$data, [bool]$pad = $true) { return @{ Path = $path; Data = $data; Pad = $pad } }
function NulJoin([string[]]$parts) {
    $out = New-Object 'System.Collections.Generic.List[byte]'
    foreach ($p in $parts) { $out.AddRange($utf8.GetBytes($p)); $out.Add(0) }
    return [byte[]]$out.ToArray()
}

# 文本资源：引号分类（可翻译键、tr()/translate()/N_()/RTR()、元数据键、
# [section] 头），自由字符串规则、整行回退、跳过目录、生成文件及递归深度。
$warmupScenarios['warm_godot_project_text'] = {
    param($d)
    Write-Utf8 "$d\project.godot" @'
; Engine configuration file.
config_version=5

[application]

config/name="Legend of the Parity Test"
config/description="A long description of the game, for testing."
run/main_scene="res://scenes/main.tscn"

[display]
window/size/viewport_width=1152
'@
    Write-Utf8 "$d\scenes\main.tscn" @'
[gd_scene load_steps=3 format=3 uid="uid://abc123"]

[ext_resource type="Script" path="res://scripts/main.gd" id="1_x"]

[node name="Main" type="Control"]

[node name="StartButton" type="Button" parent="."]
text = "Start Game"
tooltip_text = "Begin your adventure now."
hint_tooltip = 'Legacy tooltip, single quoted.'

[node name="Title" type="Label" parent="."]
text = "Legend of the Parity Test"
TEXT = "Upper case key still counts"
theme_override_fonts/font = ExtResource("2_font")

[node name="Rich" type="RichTextLabel" parent="."]
bbcode_text = "[b]Bold[/b] welcome, hero!"
placeholder_text = "Type here"
window_title="No spaces around equals"
script = ExtResource("1_x")
metadata/_custom = "hidden metadata"
   text   =   "Padded key and value"
[connection signal="pressed" from="StartButton" to="." method="_on_start"]
'@
    Write-Utf8 "$d\scripts\main.gd" @'
extends Control

const GREETING := "Welcome back, commander."
var short_id = "btn_ok"
var label_text = 'Single quoted sentence here.'

func _ready():
    $Title.text = tr("Options")
    print("debug marker")
    push_warning("Some warning text with spaces")
    var s = TranslationServer.translate("Continue")
    var n = N_("Load")
    var r = RTR("Quit")
    var t = tr ( "Spaced call" )
    var no = mytr("Notatr")
    var d = {"key": "value with spaces here"}
    # don't treat this apostrophe as a string start
    """
    Docstring-ish triple quote
    """
    var multi = "unterminated string
    var esc = "Line\nbreak and \"quoted\" \u00e9"
    dialog_text = "Are you sure?"
    emit_signal("my_signal")
    $Label.text = "Text %s" % name
    var path = "res://icons/a.png"
    var node = "NodePath value here"
    var resource = "resource word first"
    var thirtythree = "abcdefghijklmnopqrstuvwxyzabcdefg"
    var cjk = "这是中文文本"
'@
    Write-Utf8 "$d\dialogue\intro.txt" @'
Welcome to the arena!
# comment line
; ini comment
[section]
key=value
short
"quoted line stays for quoted scanner"
Eighteen+ chars no punct here
msgid "po directive"
msgstr "translation"
Plain line with period.
'@
    Write-Utf8 "$d\data\items.json" '{"name":"Iron Sword","description":"A sturdy blade, forged in the north.","icon":"res://icons/sword.png","id":"itm_001"}'
    Write-Utf8 "$d\i18n\ui.tres" "[gd_resource type=`"Theme`" format=3]`n`n[resource]`nresource_name = `"MainTheme`"`ndefault_font_size = 16`n"
    Write-Utf8Bom "$d\notes\bom.txt" "BOM line collected fine.`r`nSecond BOM line, also fine.`r`n"
    Touch "$d\nul.txt" ($utf8.GetBytes("Before NUL in godot text.`n") + [byte[]](0) + $utf8.GetBytes("After NUL never seen.`n"))
    Write-Utf8 "$d\.godot\imported\cache.tscn" "text = `"Import cache text skipped`""
    Write-Utf8 "$d\.import\x.tscn" "text = `"Dot import text skipped`""
    Write-Utf8 "$d\addons\plugin\plugin.gd" "var x = tr(`"Addon text skipped`")"
    Write-Utf8 "$d\saves\slot1.txt" "Save slot text skipped, yes."
    Write-Utf8 "$d\save\slot1.txt" "Singular save text skipped, yes."
    Write-Utf8 "$d\export_presets\preset.txt" "Export preset text skipped, yes."
    Write-Utf8 "$d\dst_godot_runtime.gd" "var x = tr(`"Launcher runtime script skipped`")"
    Touch "$d\dst_godot_patch.pck" ($utf8.GetBytes("Launcher patch pack text skipped."))
    Touch "$d\dst_godot_patch.next.pck" ($utf8.GetBytes("Launcher next patch pack text skipped."))
    Write-Utf8 "$d\dst_godot_patch.building" "Building marker text skipped."
    $deep = $d; for ($i = 1; $i -le 11; $i++) { $deep = "$deep\d$i"; Write-Utf8 "$deep\l.txt" "Depth $i line here, ok." }
}
# PO（msgid/msgid_plural/续行，忽略 msgstr）、按表头选择源列的 Godot 翻译 CSV，
# 未知表头 CSV 的回退（跳过短标识符），以及 Markdown 对话脚本。
$warmupScenarios['warm_godot_po_csv_md'] = {
    param($d)
    Write-Utf8 "$d\project.godot" "config_version=5`n"
    Write-Utf8 "$d\locale\en.po" @'
# translator comment
msgid ""
msgstr ""
"Content-Type: text/plain; charset=UTF-8\n"

#: scenes/main.tscn
msgid "Hello, world of PO files."
msgstr "你好"

msgctxt "menu"
msgid "Save game"
msgstr ""

msgid ""
"Multi-line msgid part one, "
"and part two continues here."
msgstr ""

msgid "Apples"
msgid_plural "Many apples, plural form."
msgstr[0] ""
msgstr[1] ""

msgid "Escaped \"quotes\" and \\n literal"
msgstr "x"
msgid_x "not a msgid marker but text[5] is underscore"
msgid	"Tab separated msgid works."
msgid "Unterminated msgid line
msgid "Last entry flushed at EOF."
'@
    Write-Utf8Bom "$d\locale\strings.csv" @'
keys,en,zh_CN,fr
MENU_START,"Start","开始","Démarrer"
MENU_QUIT,Quit,退出,Quitter
DLG_1,"He said ""hello"", then left.",x,y
DLG_2,  "Leading spaces before quote"  ,x,y
# comment row
EMPTY,,,
SHORTROW
LONG,"Sentence with, a comma inside.",a,b
'@
    Write-Utf8 "$d\locale\unknown.csv" @'
id,name_x,value
item_01,Rusty Dagger,12
npc_bob,Bob,hp
q_1,"A quest to remember, always.",1
plain,Plain,text
'@
    Write-Utf8 "$d\locale\english_col.csv" "term,English,Japanese`nT1,Prefer English header column.,日本`n"
    Write-Utf8 "$d\locale\en_gb.csv" "id,en-GB,de`nX,British source column wins.,Deutsch`n"
    Write-Utf8 "$d\locale\source_text.csv" "id,text,source_text`nX,lower score column,Higher score column wins here.`n"
    Write-Utf8 "$d\story\chapter1.md" @'
# Chapter One: The Beginning
##   Sub heading here
Some narration line, with punctuation.
> Speaker: quoted line skipped
---
***
___
[center]Centered visible text[/center]
[window_opens]
[b]Bold[/b] and [i]italic[/i] mixed line
```code
inside fence, skipped entirely.
```
After the fence, back to normal.
~~~
tilde fence skipped too
~~~
Final line
[unclosed bracket line
[/close]Only closing tag then text[/close]
#
'@
}
# 独立的格式 1 PCK：NUL 填充路径，先处理文本/翻译条目，第二遍处理编译资源；
# 验证 .translation 的语言后缀规则，并跳过越界或过大的条目。
$warmupScenarios['warm_godot_pck_v1'] = {
    param($d)
    Touch "$d\game.exe"
    $tscn = $utf8.GetBytes("[node name=`"Menu`" type=`"Control`"]`ntext = `"Pack menu start`"`nvar free = `"Free pack string, yes.`"`n")
    $md = $utf8.GetBytes("# Pack heading`nPack markdown body line.`n")
    $enTr = NulJoin @('RSRC', 'OptimizedTranslation', 'messages', 'Pack translation entry one.', 'Go', 'Start', 'bin', 'Play', "Tabbed`tentry here",
                      'Café non-ascii entry', 'テキストです', 'これは日本語の文章です', '[color=red]Danger ahead![/color] Careful now.', '[b]Only tag[/b]',
                      'Pack translation entry one.', 'trailing tag text [i]', '_underscore first', '{brace first', 'node name value', 'res://path/in/text')
    $enTr = [byte[]]($enTr + [byte[]](0xFF, 0xFE) + $utf8.GetBytes('invalid prefix') + [byte[]](0) +
                     $utf8.GetBytes('overlong ') + [byte[]](0xC0, 0x80) + [byte[]](0) +
                     $utf8.GetBytes('surrogate ') + [byte[]](0xED, 0xA0, 0x80) + [byte[]](0) +
                     $utf8.GetBytes('truncated ') + [byte[]](0xE6, 0x97) + [byte[]](0) +
                     $utf8.GetBytes('four byte ok ') + [byte[]](0xF0, 0x9F, 0x98, 0x80) + [byte[]](0) +
                     $utf8.GetBytes('Unterminated pack tail entry'))
    $entries = @(
        (GodotEntry 'res://compiled/scene.scn' (NulJoin @('Deferred scene text entry.'))),
        (GodotEntry 'res://scripts/x.gdc' (NulJoin @('Compiled script string here.')) $false),
        (GodotEntry 'res://scenes/menu.tscn' $tscn),
        (GodotEntry 'res://story/intro.md' $md $false),
        (GodotEntry 'res://locale/ui.en.translation' $enTr),
        (GodotEntry 'res://locale/ui.zh.translation' (NulJoin @('Chinese pack translation skipped.'))),
        (GodotEntry 'res://locale/ui.en_US.translation' (NulJoin @('US english translation entry.'))),
        (GodotEntry 'res://locale/ui.english.translation' (NulJoin @('English word locale rejected entry.'))),
        (GodotEntry 'res://locale/plain.translation' (NulJoin @('Plain translation entry text.'))),
        (GodotEntry 'res://locale/UI.EN-GB.TRANSLATION' (NulJoin @('Upper case suffix entry text.'))),
        (GodotEntry 'res://img/a.png' (NulJoin @('PNG ignored text entry.'))),
        (GodotEntry 'res://data/notes.txt' $utf8.GetBytes("Pack notes free line here.`n")),
        (GodotEntry 'res://data/table.csv' $utf8.GetBytes("key,source`nA,`"Pack CSV source cell.`"`n")),
        (GodotEntry 'res://locale/pack.po' $utf8.GetBytes("msgid `"Pack PO entry.`"`nmsgstr `"`"`n")),
        (GodotEntry 'project.godot' $utf8.GetBytes("config/description=`"Pack project description.`"`n")),
        (GodotEntry 'res://project.binary' (NulJoin @('Binary project not matched by name.'))),
        (GodotEntry 'godot_project.binary' (NulJoin @('Godot project binary entry text.'))),
        (GodotEntry 'res://empty.txt' (New-Object byte[] 0)),
        (GodotEntry 'res://data/exe_base.res' (NulJoin @('Resource extension entry text.')))
    )
    $entries += @{ Path = 'res://data/out_of_range.txt'; Data = $utf8.GetBytes("Out of range entry skipped.`n"); Pad = $true; OffsetOverride = 0x7FFFFFFF }
    $entries += @{ Path = 'res://data/too_big.txt'; Data = $utf8.GetBytes("Oversized entry skipped.`n"); Pad = $true; SizeOverride = 0x10000000 }
    Touch "$d\game.pck" (New-GodotPck 1 $entries)
}
# 格式 2（相对偏移，目录和数据之间有间隔）、格式 3（目录位于末尾），
# 损坏/外来包回退到原始字节扫描，以及根目录 .translation 和 godot_project.binary。
$warmupScenarios['warm_godot_pck_v2_v3_fallback'] = {
    param($d)
    Touch "$d\game.exe"
    $v2 = @(
        (GodotEntry 'res://ui/v2.tscn' $utf8.GetBytes("text = `"Version two pack text`"`n")),
        (GodotEntry 'res://ui/v2.gde' (NulJoin @('Version two deferred script text.')) $false),
        (GodotEntry 'res://ui/v2.en.translation' (NulJoin @('Version two translation entry.')))
    )
    Touch "$d\a_v2.pck" (New-GodotPck 2 $v2 0 16)
    $v3 = @(
        (GodotEntry 'res://ui/v3.tscn' $utf8.GetBytes("tooltip_text = `"Version three pack text`"`n")),
        (GodotEntry 'res://ui/v3.res' (NulJoin @('Version three deferred resource text.'))),
        (GodotEntry 'res://ui/v3.po' $utf8.GetBytes("msgid `"Version three PO entry.`"`n"))
    )
    Touch "$d\b_v3.pck" (New-GodotPck 3 $v3 0 8)
    # GDPC 头、格式 1、唯一条目的路径长度为 0：目录解析失败后回退到原始扫描
    $broken = New-Object byte[] 88; $asciiEnc.GetBytes('GDPC').CopyTo($broken, 0); [BitConverter]::GetBytes([uint32]1).CopyTo($broken, 4); [BitConverter]::GetBytes([uint32]1).CopyTo($broken, 84)
    Touch "$d\c_broken.pck" ([byte[]]($broken + (New-Object byte[] 4) + $utf8.GetBytes('Fallback raw scan sentence.') + [byte[]](0)))
    Touch "$d\d_wrongmagic.pck" ([byte[]]($asciiEnc.GetBytes('XXXX') + (New-Object byte[] 90) + $utf8.GetBytes('Not a pack but scanned raw, yes.') + [byte[]](0)))
    Touch "$d\e_tiny.pck" ($utf8.GetBytes('Tiny pack.'))
    $v9 = New-Object byte[] 112; $asciiEnc.GetBytes('GDPC').CopyTo($v9, 0); [BitConverter]::GetBytes([uint32]9).CopyTo($v9, 4)
    Touch "$d\f_unknown_format.pck" ([byte[]]($v9 + $utf8.GetBytes('Unknown format raw text here.') + [byte[]](0)))
    Touch "$d\g_empty.pck" (New-Object byte[] 0)
    # 格式 2 的 file_count 超限：判为无效后扫描整个文件的原始字节
    $many = New-Object byte[] 100; $asciiEnc.GetBytes('GDPC').CopyTo($many, 0); [BitConverter]::GetBytes([uint32]2).CopyTo($many, 4); [BitConverter]::GetBytes([uint32]200001).CopyTo($many, 96)
    Touch "$d\h_too_many.pck" ([byte[]]($many + $utf8.GetBytes('Too many files raw text here.') + [byte[]](0)))
    Touch "$d\root.translation" (NulJoin @('Root translation entry text.', 'Root', 'Ab'))
    Touch "$d\zh.translation" (NulJoin @('Root zh translation is still scanned by file name.'))
    Touch "$d\godot_project.binary" (NulJoin @('Binary project setting text.', 'ProjectSettings', 'application/config/name'))
    Touch "$d\huge.translation" (New-Object byte[] (64 * 1024 * 1024 + 1))
}
# 内嵌包：PE 的 "pck" 区段内含格式 2 包（GDPC 尾标记也参与检测），
# 格式 1 包使用 EXE 绝对偏移，以及无 pck 区段的 PE 和非 PE 的 .exe。
$warmupScenarios['warm_godot_embedded_exe'] = {
    param($d)
    $v2 = @(
        (GodotEntry 'res://ui/embedded.tscn' $utf8.GetBytes("text = `"Embedded pack text`"`n")),
        (GodotEntry 'res://ui/embedded.en.translation' (NulJoin @('Embedded translation entry.')))
    )
    Touch "$d\game.exe" (New-GodotEmbeddedExe 2 $v2 $true)
    $v1 = @(
        (GodotEntry 'res://ui/legacy.tscn' $utf8.GetBytes("text = `"Legacy embedded pack text`"`n")),
        (GodotEntry 'res://ui/legacy.scn' (NulJoin @('Legacy embedded deferred text.')))
    )
    Touch "$d\game_v1.exe" (New-GodotEmbeddedExe 1 $v1 $false)
    Touch "$d\tool.exe" (New-PckPeBytes $false $false)
    Touch "$d\notpe.exe" ($utf8.GetBytes('Not a PE file at all, really.'))
    Touch "$d\UnityCrashHandler64.exe" (New-GodotEmbeddedExe 2 $v2 $true)
}
# 12000 条上限（24 批），跳过过大文本文件，达到上限后停止遍历目录。
$warmupScenarios['warm_godot_pck_v4'] = {
    param($d)
    $entries = @((GodotEntry 'res://ui/menu.tscn' $utf8.GetBytes("text = `"Version four visible menu`"`n")))
    Touch "$d\Game.exe" (New-GodotEmbeddedExe 4 $entries $true)
    Touch "$d\Game.pck" (New-GodotPck 4 $entries 0 0 7)
}
$warmupScenarios['warm_godot_cap'] = {
    param($d)
    Write-Utf8 "$d\project.godot" "config_version=5`n"
    $sb = New-Object Text.StringBuilder
    for ($i = 1; $i -le 12050; $i++) { [void]$sb.Append("Capacity line number $i, filler text.`n") }
    Write-Utf8 "$d\big.txt" $sb.ToString()
    Touch "$d\huge.txt" (New-Object byte[] (64 * 1024 * 1024 + 1))
    Write-Utf8 "$d\zzz_after_cap.txt" "Never reached after the cap, sorry.`n"
}
$warmupScenarios['warm_godot_nothing_to_do'] = { param($d) Write-Utf8 "$d\project.godot" "config_version=5`n" }

# ---- 不提供预热流程的引擎 ----
$warmupScenarios['warm_rpgm_legacy_noop'] = { param($d) Touch "$d\Data\Scripts.rvdata2"; Touch "$d\Game.exe" }
$warmupScenarios['warm_unknown_noop'] = { param($d) Touch "$d\readme.txt" }

foreach ($name in $warmupScenarios.Keys) {
    $d = Join-Path $root "ws\$name"
    Mkdir $d
    & $warmupScenarios[$name] $d
    $before = Snapshot-Tree $d
    $c = Invoke-Detect $CLauncher @('--warmup-and-exit', $d)
    $cs = Invoke-Detect $CsLauncher @('--warmup', $d)
    $after = Snapshot-Tree $d
    $label = "warmup/$name"
    if ($c.Code -ne $cs.Code) {
        $fail++; $failures.Add("[$label] exit codes C=$($c.Code) C#=$($cs.Code)`n--- C ---`n$($c.Out)$($c.Err)--- C# ---`n$($cs.Out)$($cs.Err)")
        continue
    }
    if ($c.Out -ne $cs.Out) {
        $fail++; $failures.Add("[$label] stdout differs`n--- C ---`n$($c.Out)--- C# ---`n$($cs.Out)")
        continue
    }
    if ($c.Out -notmatch '(?m)^result=0$') {
        $fail++; $failures.Add("[$label] no result=0 line`n$($c.Out)")
        continue
    }
    if ($before -ne $after) {
        $fail++; $failures.Add("[$label] dump mode modified the game directory`n--- before ---`n$before`n--- after ---`n$after")
        continue
    }
    if ($name -eq 'warm_godot_pck_v4' -and $c.Out -notmatch 'Version four visible menu') {
        $fail++; $failures.Add("[$label] v4 warmup missed its visible text"); continue
    }
    if ($name -eq 'warm_godot_cap' -and $c.Out -notmatch '(?m)^log=.*12000.*$') {
        $fail++; $failures.Add("[$label] scan-cap warning missing"); continue
    }
    if ($name -eq 'warm_rpgm_plugin_ui_parameters') {
        $required = @('Loads the data from the saved game.', 'Playtime:', 'Total Saves:',
                      'Yes, load it.', 'Save the game now?', 'Data item follows plugin labels.')
        $forbidden = @('Disabled plugin text must not queue.', 'Damage2',
                       'variable 1, variable 2', 'Not directly displayed', 'RPG %1 File%2')
        $missing = @($required | Where-Object { -not $c.Out.Contains($_) })
        $unexpected = @($forbidden | Where-Object { $c.Out.Contains($_) })
        if ($missing.Count -or $unexpected.Count -or
            $c.Out.IndexOf('Loads the data from the saved game.') -gt $c.Out.IndexOf('Data item follows plugin labels.')) {
            $fail++; $failures.Add("[$label] UI parameter selection or scan priority incorrect; missing=$missing unexpected=$unexpected`n$($c.Out)"); continue
        }
    }
    if ($name -eq 'warm_renpy_ignores_rpgm_plugins' -and
        ($c.Out -notmatch 'RenPy dialogue stays visible.' -or $c.Out -match 'RPGM plugin text must not leak.')) {
        $fail++; $failures.Add("[$label] RPGM plugin scan leaked into Ren'Py`n$($c.Out)"); continue
    }
    if ($name -eq 'warm_rpgm_bad_plugin_manifest' -and
        ($c.Out -notmatch 'Data still scanned after bad plugins.' -or
         $c.Out -notmatch 'RPGM 插件参数预热未完成')) {
        $fail++; $failures.Add("[$label] malformed plugin manifest hid data or lacked diagnostic`n$($c.Out)"); continue
    }
    if ($name -eq 'warm_rpgm_flat_and_limits' -and $c.Out -notmatch 'Flat layout plugin help.') {
        $fail++; $failures.Add("[$label] flat MZ plugin parameters missed`n$($c.Out)"); continue
    }
    $pass++
    $posts = @($c.Out -split "`n" | Where-Object { $_ -like 'post=*' })
    $texts = 0
    foreach ($p in $posts) { $texts += ([regex]::Matches($p, '(?<!\\)"(?:[^"\\]|\\.)*"')).Count }
    Write-Host ("  ok  {0,-52} batches={1} strings={2}" -f $label, $posts.Count, $texts)
    if ($ShowLogs) { ($c.Out -split "`n") | Where-Object { $_ -like 'log=*' -or $_ -like 'post=*' } | ForEach-Object { Write-Host "        $_" } }
}

# ---------------------------------------------------------------- 内嵌组件同步一致性
#
# --sync-payloads-and-exit（C）/ --sync-payloads --root（C#）：两个启动器
# 将内嵌的项目自有组件（dst_server.exe、dst_server_cs.exe、安装脚本、INI
# 示例和可选 Unity 程序集）释放到空根目录。结果目录树必须逐字节一致，
# 标准输出（log= 行和 result=）及退出码也必须一致：成功为 0，
# 必需组件缺失或无法写入为 5。随后再次运行，两版均不应有任何修改，
# 不回写目标、不记录同步日志；被篡改的目标应恢复，根目录中外来文件
# 应保留，目标被阻塞时必须报告失败（退出码 5）。
$syncC = Join-Path $root 'sync_c'; Mkdir $syncC
$syncCs = Join-Path $root 'sync_cs'; Mkdir $syncCs
$syncExeName = Split-Path -Leaf $CLauncher
Copy-Item -LiteralPath $CLauncher -Destination (Join-Path $syncC $syncExeName)
function Snapshot-SyncTree([string]$dir) {
    # 复制过来的 C 启动器自身不属于组件，将其从比较范围中排除
    return (((Snapshot-Tree $dir) -split "`n") | Where-Object { $_ -notmatch ('^F . ' + [regex]::Escape($syncExeName) + ' ') }) -join "`n"
}
$c = Invoke-Detect (Join-Path $syncC $syncExeName) @('--sync-payloads-and-exit')
$cs = Invoke-Detect $CsLauncher @('--sync-payloads', '--root', $syncCs)
$treeC = Snapshot-SyncTree $syncC
$treeCs = Snapshot-SyncTree $syncCs
$released = @(($treeC -split "`n") | Where-Object { $_ -like 'F *' }).Count
if ($c.Code -ne $cs.Code -or $c.Out -ne $cs.Out) {
    $fail++; $failures.Add("[sync-payloads] exit/stdout differ C=$($c.Code) C#=$($cs.Code)`n--- C ---`n$($c.Out)$($c.Err)--- C# ---`n$($cs.Out)$($cs.Err)")
} elseif ($treeC -ne $treeCs) {
    $fail++; $failures.Add("[sync-payloads] released trees differ`n--- C ---`n$treeC`n--- C# ---`n$treeCs")
} elseif ($released -lt 5 -or $cs.Out -notmatch "(?m)^log=Built-in components synced: $released file\(s\)\.$") {
    $fail++; $failures.Add("[sync-payloads] expected >= 5 released files and a matching sync log (released=$released)`n$($cs.Out)")
} elseif ($cs.Out -notmatch "(?m)^result=$($c.Code)$") {
    $fail++; $failures.Add("[sync-payloads] result line does not match the exit code`n$($cs.Out)")
} else {
    $pass++; Write-Host ("  ok  {0,-52} files={1} exit={2}" -f 'sync-payloads/first run', $released, $c.Code)
}
# 两版第二次运行：内容一致时不回写、不记录同步日志，外来文件保持原样
$stampBefore = @{}
foreach ($d in @($syncC, $syncCs)) {
    Get-ChildItem -LiteralPath $d -Recurse -File | ForEach-Object { $stampBefore[$_.FullName] = $_.LastWriteTimeUtc.Ticks }
    Touch "$d\config\api.ini" ($asciiEnc.GetBytes("[api]`r`nkey=user-secret`r`n"))
}
Start-Sleep -Milliseconds 30
$c2 = Invoke-Detect (Join-Path $syncC $syncExeName) @('--sync-payloads-and-exit')
$cs2 = Invoke-Detect $CsLauncher @('--sync-payloads', '--root', $syncCs)
$rewritten = @(Get-ChildItem -LiteralPath @($syncC, $syncCs) -Recurse -File | Where-Object { $stampBefore.ContainsKey($_.FullName) -and $stampBefore[$_.FullName] -ne $_.LastWriteTimeUtc.Ticks })
if ($c2.Code -eq $c.Code -and $cs2.Code -eq $c.Code -and $c2.Out -eq $cs2.Out -and $rewritten.Count -eq 0 -and
    $cs2.Out -notmatch 'Built-in components synced' -and
    [IO.File]::ReadAllText("$syncC\config\api.ini") -eq "[api]`r`nkey=user-secret`r`n" -and
    [IO.File]::ReadAllText("$syncCs\config\api.ini") -eq "[api]`r`nkey=user-secret`r`n") {
    $pass++; Write-Host ("  ok  {0,-52} rewritten=0" -f 'sync-payloads/second run is a no-op')
} else {
    $fail++; $failures.Add("[sync-payloads] second run rewrote $($rewritten.Count) file(s), touched api.ini, or outputs differ`n--- C ---`n$($c2.Out)--- C# ---`n$($cs2.Out)")
}
# 两版均应恢复被篡改的目标（大小相同但字节不同）和被截断的目标
$orig1 = [IO.File]::ReadAllBytes("$syncCs\config\api.ini.example")
$bad1 = [byte[]]$orig1.Clone(); $bad1[0] = [byte](($bad1[0] + 1) -band 0xFF)
$orig2 = [IO.File]::ReadAllBytes("$syncCs\scripts\install_runtime_payloads.ps1")
foreach ($d in @($syncC, $syncCs)) {
    [IO.File]::WriteAllBytes("$d\config\api.ini.example", $bad1)
    [IO.File]::WriteAllBytes("$d\scripts\install_runtime_payloads.ps1", $orig2[0..([Math]::Min(10, $orig2.Length - 1))])
}
$c3 = Invoke-Detect (Join-Path $syncC $syncExeName) @('--sync-payloads-and-exit')
$cs3 = Invoke-Detect $CsLauncher @('--sync-payloads', '--root', $syncCs)
$fixedAll = $true
foreach ($d in @($syncC, $syncCs)) {
    if (-not [Linq.Enumerable]::SequenceEqual([byte[]][IO.File]::ReadAllBytes("$d\config\api.ini.example"), [byte[]]$orig1)) { $fixedAll = $false }
    if (-not [Linq.Enumerable]::SequenceEqual([byte[]][IO.File]::ReadAllBytes("$d\scripts\install_runtime_payloads.ps1"), [byte[]]$orig2)) { $fixedAll = $false }
    if (Test-Path -LiteralPath "$d\config\api.ini.example.dstmp") { $fixedAll = $false }
}
if ($c3.Code -eq $c.Code -and $cs3.Code -eq $c.Code -and $c3.Out -eq $cs3.Out -and $fixedAll -and
    $cs3.Out -match '(?m)^log=Built-in components synced: 2 file\(s\)\.$') {
    $pass++; Write-Host ("  ok  {0,-52} restored=2" -f 'sync-payloads/tampered targets restored')
} else {
    $fail++; $failures.Add("[sync-payloads] tampered targets not restored (fixedAll=$fixedAll)`n--- C ---`n$($c3.Out)--- C# ---`n$($cs3.Out)")
}
# 目标是目录时不得替换：计为失败、退出码 5，其他文件仍继续同步
foreach ($d in @($syncC, $syncCs)) {
    $blocker = "$d\config\launcher.ini.example"
    Remove-Item -LiteralPath $blocker -Force; Mkdir $blocker; Touch "$blocker\keep.txt"
    [IO.File]::WriteAllBytes("$d\config\api.ini.example", $bad1)
}
$c4 = Invoke-Detect (Join-Path $syncC $syncExeName) @('--sync-payloads-and-exit')
$cs4 = Invoke-Detect $CsLauncher @('--sync-payloads', '--root', $syncCs)
$blockedOk = $true
foreach ($d in @($syncC, $syncCs)) {
    if (-not [Linq.Enumerable]::SequenceEqual([byte[]][IO.File]::ReadAllBytes("$d\config\api.ini.example"), [byte[]]$orig1)) { $blockedOk = $false }
    if (-not (Test-Path -LiteralPath "$d\config\launcher.ini.example\keep.txt")) { $blockedOk = $false }
}
if ($c4.Code -eq 5 -and $cs4.Code -eq 5 -and $c4.Out -eq $cs4.Out -and $blockedOk -and
    $cs4.Out -match '(?m)^log=Built-in component update failed: launcher\.ini\.example$' -and
    $cs4.Out -match '(?m)^log=Warning: 1 built-in component\(s\) could not be updated\. Close running games/server and restart the launcher\.$' -and
    $cs4.Out -match '(?m)^result=5$') {
    $pass++; Write-Host ("  ok  {0,-52} exit=5" -f 'sync-payloads/blocked target reported')
} else {
    $fail++; $failures.Add("[sync-payloads] blocked target not reported identically (C=$($c4.Code) C#=$($cs4.Code) blockedOk=$blockedOk)`n--- C ---`n$($c4.Out)--- C# ---`n$($cs4.Out)")
}

# ---------------------------------------------------------------- Godot 补丁包一致性
#
# 比较 C 的 --godot-patch-and-exit / --godot-promote-and-exit / --godot-launcher-and-exit
# 和 C# 的 --godot-patch / --godot-promote / --godot-launcher。每个场景生成两份，
# 两个启动器的退出码、归一化标准输出（log= 行、pack= / launcher=、result=）、
# 结果目录树（相对路径 + SHA-256，因此生成的 .pck 必须逐字节一致），
# 以及 /batch 请求体的完整顺序都必须一致。
#
# 补丁生成器连接 127.0.0.1:19999 本地服务，因此本节使用结果确定的
# 假 /batch 端点。两个启动器使用相同的翻译规则：
#   "KEEPME…" -> 原样返回（客户端必须丢弃：结果等于查询文本）
#   "BADFMT…" -> 返回未配对括号（客户端必须丢弃：格式未保留）
#   其他文本 -> "<CJK>:" + 原文；非 ASCII 字符以 \uXXXX 发送，覆盖 JSON 反转义流程。
# 端口 19999 已占用时，明确报告跳过本节，不接触正在运行的服务；
# 跳过的检查数量会显示在汇总行中。
$godotSkip = $null
$busyPatch = Get-NetTCPConnection -LocalPort 19999 -State Listen -ErrorAction SilentlyContinue
if ($busyPatch) { $godotSkip = "port 19999 already has a listener (pid $($busyPatch.OwningProcess))" }

$fakeState = [hashtable]::Synchronized(@{ Bodies = New-Object System.Collections.Generic.List[string]; Error = $null })
if (-not $godotSkip) {
    $fakeRunspace = [runspacefactory]::CreateRunspace()
    $fakeRunspace.Open()
    $fakeRunspace.SessionStateProxy.SetVariable('state', $fakeState)
    $fakePs = [powershell]::Create()
    $fakePs.Runspace = $fakeRunspace
    $null = $fakePs.AddScript({
        $enc = New-Object Text.UTF8Encoding $false
        function Esc([string]$s) {
            $sb = New-Object Text.StringBuilder
            foreach ($ch in $s.ToCharArray()) {
                $c = [int]$ch
                if ($ch -eq '"') { $null = $sb.Append('\"') }
                elseif ($ch -eq '\') { $null = $sb.Append('\\') }
                elseif ($c -lt 32 -or $c -gt 126) { $null = $sb.AppendFormat('\u{0:x4}', $c) }
                else { $null = $sb.Append($ch) }
            }
            return $sb.ToString()
        }
        $listener = New-Object Net.Sockets.TcpListener ([Net.IPAddress]::Loopback), 19999
        $listener.Start()
        # 主线程停止监听器，以解除 AcceptTcpClient 的阻塞；运行空间不能中断
        # 阻塞的套接字调用，因此仅调用 Runspace.Close 会在此挂起。
        $state['Listener'] = $listener
        $state['Ready'] = $true
        try {
            while ($true) {
                $client = $listener.AcceptTcpClient()
                try {
                    $client.NoDelay = $true
                    $stream = $client.GetStream()
                    $head = New-Object Collections.Generic.List[byte]
                    $one = New-Object byte[] 1
                    while ($true) {
                        if ($stream.Read($one, 0, 1) -le 0) { break }
                        $head.Add($one[0])
                        $n = $head.Count
                        if ($n -ge 4 -and $head[$n - 4] -eq 13 -and $head[$n - 3] -eq 10 -and $head[$n - 2] -eq 13 -and $head[$n - 1] -eq 10) { break }
                    }
                    $headText = [Text.Encoding]::ASCII.GetString($head.ToArray())
                    $len = 0
                    if ($headText -match '(?im)^Content-Length:\s*(\d+)') { $len = [int]$Matches[1] }
                    $body = New-Object byte[] $len
                    $got = 0
                    while ($got -lt $len) {
                        $r = $stream.Read($body, $got, $len - $got)
                        if ($r -le 0) { break }
                        $got += $r
                    }
                    $bodyText = $enc.GetString($body, 0, $got)
                    $state.Bodies.Add($bodyText)
                    $texts = @()
                    try { $texts = @(($bodyText | ConvertFrom-Json).texts) } catch { $state.Error = "bad request body: $bodyText" }
                    $parts = New-Object Collections.Generic.List[string]
                    foreach ($t in $texts) {
                        $s = [string]$t
                        if ($s.StartsWith('KEEPME')) { $parts.Add('"' + (Esc $s) + '"') }
                        elseif ($s.StartsWith('BADFMT')) { $parts.Add('"' + (Esc ([string][char]0x8bd1 + ':[' + $s)) + '"') }
                        else { $parts.Add('"' + (Esc ([string][char]0x8bd1 + ':' + $s)) + '"') }
                    }
                    $payload = $enc.GetBytes('{"results":[' + ($parts -join ',') + ']}')
                    $header = [Text.Encoding]::ASCII.GetBytes("HTTP/1.1 200 OK`r`nContent-Type: application/json`r`nContent-Length: $($payload.Length)`r`nConnection: close`r`n`r`n")
                    $stream.Write($header, 0, $header.Length)
                    $stream.Write($payload, 0, $payload.Length)
                    $stream.Flush()
                    $client.Client.Shutdown([Net.Sockets.SocketShutdown]::Send)
                } finally { $client.Close() }
            }
        } catch {
            # 主线程调用 listener.Stop()，使 AcceptTcpClient 通过异常解除阻塞
        } finally { $listener.Stop() }
    })
    $fakeHandle = $fakePs.BeginInvoke()
    $waited = 0
    while (-not $fakeState['Ready'] -and $waited -lt 5000) { Start-Sleep -Milliseconds 25; $waited += 25 }
    if (-not $fakeState['Ready']) { $godotSkip = 'fake /batch server did not start' }
}

$godotSkipped = 0
if ($godotSkip) {
    Write-Host "  --  godot-patch parity skipped: $godotSkip"
}

# GDSC v13 字节码："GDSC"、版本、标识符/常量数量，随后为标识符
# （[u32 长度][字节]）和常量（[u32 类型] + 内容；类型 4 为 String，按 4 字节填充）。
function New-GdscBytes([string[]]$identifiers, [object[]]$constants) {
    $out = New-Object Collections.Generic.List[byte]
    $out.AddRange($asciiEnc.GetBytes('GDSC'))
    $out.AddRange([BitConverter]::GetBytes([uint32]13))
    $out.AddRange([BitConverter]::GetBytes([uint32]$identifiers.Count))
    $out.AddRange([BitConverter]::GetBytes([uint32]$constants.Count))
    $out.AddRange((New-Object byte[] 8))                   # 词法单元/函数数量（跳过）
    foreach ($id in $identifiers) {
        $b = $utf8.GetBytes($id)
        $out.AddRange([BitConverter]::GetBytes([uint32]$b.Length))
        $out.AddRange($b)
    }
    foreach ($c in $constants) {
        if ($c -is [string]) {
            $b = $utf8.GetBytes($c)
            $pad = (4 - ($b.Length % 4)) % 4
            $out.AddRange([BitConverter]::GetBytes([uint32]4))
            $out.AddRange([BitConverter]::GetBytes([uint32]$b.Length))
            $out.AddRange($b)
            if ($pad) { $out.AddRange((New-Object byte[] $pad)) }
        } else {
            $out.AddRange([BitConverter]::GetBytes([uint32]$c.Type))
            $out.AddRange((New-Object byte[] $c.Size))
        }
    }
    return [byte[]]$out.ToArray()
}
# OptimizedTranslation 资源：属性 5 为 PackedInt32Array（0x20）哈希表，属性 6 为
# PackedByteArray（0x1f）字符串堆。每条记录包含 4 个整数：哈希、偏移、压缩长度、原始长度；
# 压缩长度与原始长度不同时，表示该堆片段经过 Smaz 压缩。
function New-OptimizedTranslation([object[]]$records) {
    $heap = New-Object Collections.Generic.List[byte]
    $table = New-Object Collections.Generic.List[uint32]
    $table.Add([uint32]$records.Count)                     # 桶大小
    $table.Add([uint32]0)                                  # 桶哈希
    $h = 11
    foreach ($r in $records) {
        $bytes = [byte[]]$r.Bytes
        $table.Add([uint32]$h); $h += 11
        $table.Add([uint32]$heap.Count)
        $table.Add([uint32]$bytes.Length)
        $table.Add([uint32]$r.Uncomp)
        $heap.AddRange($bytes)
    }
    $out = New-Object Collections.Generic.List[byte]
    $out.AddRange($asciiEnc.GetBytes('RSRC'))
    $out.AddRange([BitConverter]::GetBytes([uint32]5))
    $out.AddRange([BitConverter]::GetBytes([uint32]0x20))
    $out.AddRange([BitConverter]::GetBytes([uint32]$table.Count))
    foreach ($v in $table) { $out.AddRange([BitConverter]::GetBytes([uint32]$v)) }
    $out.AddRange([BitConverter]::GetBytes([uint32]6))
    $out.AddRange([BitConverter]::GetBytes([uint32]0x1f))
    $out.AddRange([BitConverter]::GetBytes([uint32]$heap.Count))
    $out.AddRange($heap)
    $pad = (4 - ($heap.Count % 4)) % 4
    if ($pad) { $out.AddRange((New-Object byte[] $pad)) }
    $out.AddRange($asciiEnc.GetBytes('TAIL8ify'))          # 字符串堆之后的字节必须保留
    return [byte[]]$out.ToArray()
}
function TransRec([byte[]]$bytes, [int]$uncomp) { return @{ Bytes = $bytes; Uncomp = $uncomp } }

$gpJson = @'
{
  "items": [
    { "id": "potion", "name": "Healing Potion", "description": "Restores %d HP to one ally.", "icon": "res://art/potion.png" },
    { "id": "sword", "name": "KEEPME Blade", "description": "BADFMT [b]sharp[/b] and heavy." }
  ],
  "name": "Item Database"
}
'@
$gpScene = @'
[gd_scene load_steps=2 format=2]

[node name="Menu" type="Control"]

[node name="Start" type="Button" parent="."]
text = "Start a new journey"
tooltip_text = "Leaves the current save untouched."
icon = "res://art/start.png"

[node name="Quit" type="Button" parent="."]
text = "Quit to desktop"
'@
$gpMarkdown = @'
# Chapter One

The rain had not stopped for three days.

```
code fences are copied verbatim
```

> Quoted lines stay as they are.

[link]: res://somewhere

She looked up and said nothing at all.
'@
$gpGdc = New-GdscBytes @('_ready__', 'on_press') @(
    'Are you sure?', 'Title', 'pressed', 'some_signal', 'Yes',
    @{ Type = 2; Size = 4 }, @{ Type = 5; Size = 8 })
$gpTranslation = New-OptimizedTranslation @(
    (TransRec ($utf8.GetBytes("Continue Game") + [byte[]]@(0)) 14),
    (TransRec ([byte[]]@(0x01)) 3),
    (TransRec ([byte[]]@(254, 0x41)) 1),
    (TransRec ([byte[]](255, 4) + $asciiEnc.GetBytes('Hello')) 5))
$gpPckEntries = @(
    (GodotEntry 'res://data/items.json' ($utf8.GetBytes($gpJson))),
    (GodotEntry 'res://scenes/menu.tscn' ($utf8.GetBytes($gpScene))),
    (GodotEntry 'res://lang/en/story.md' ($utf8.GetBytes($gpMarkdown))),
    (GodotEntry 'res://scripts/menus/confirm_popup.gdc' $gpGdc),
    (GodotEntry 'res://ui/menus.en.translation' $gpTranslation),
    (GodotEntry 'res://art/potion.png' ($asciiEnc.GetBytes('PNGDATA'))))

$patchScenarios = [ordered]@{}
# 散装项目（不存在包）：删除过期补丁包，并写入运行时侧车脚本
$patchScenarios['gp_loose_project'] = {
    param($d)
    Write-Utf8 "$d\project.godot" "config_version=5`n`n[application]`n`nconfig/name=`"Parity`"`nrun/main_scene=`"res://scenes/menu.tscn`"`n"
    Write-Utf8 "$d\scenes\menu.tscn" $gpScene
    Touch "$d\Game.exe"
    Touch "$d\dst_godot_patch.pck" ($asciiEnc.GetBytes('stale pack'))
    Touch "$d\dst_godot_patch.next.pck" ($asciiEnc.GetBytes('stale staged pack'))
}
# Godot 3 侧车包（格式 1）：JSON / tscn / Markdown / gdc / OptimizedTranslation / 字体条目
$patchScenarios['gp_pck_v1_resources'] = {
    param($d)
    Touch "$d\Game.exe"
    Touch "$d\Game.pck" (New-GodotPck 1 ($gpPckEntries + @((GodotEntry 'res://fonts/NotoSans-Regular.ttf' ($asciiEnc.GetBytes('TTFDATA'))))))
    Touch "$d\Other.pck" (New-GodotPck 1 @((GodotEntry 'res://data/items.json' ($utf8.GetBytes($gpJson)))))
}
# Godot 4.6 使用格式 3：嵌入运行时脚本、autoload 和 override.cfg，
# 并重建目录；已有 override.cfg 保留其自有键。
$patchScenarios['gp_pck_v3_autoload'] = {
    param($d)
    Touch "$d\Game.exe"
    Touch "$d\Game.pck" (New-GodotPck 3 ($gpPckEntries + @(
        (GodotEntry 'res://override.cfg' ($asciiEnc.GetBytes("[display]`nwindow/size/viewport_width=1280")))
    )) 0 0 6)
}
# Godot 4.4 使用格式 3：不支持 autoload_prepend，仅嵌入运行时脚本
$patchScenarios['gp_pck_v4_autoload'] = {
    param($d)
    Touch "$d\Game.exe"
    Touch "$d\Game.pck" (New-GodotPck 4 $gpPckEntries 0 0 7)
}
$patchScenarios['gp_pck_v4_embedded'] = {
    param($d)
    Touch "$d\Game.exe" (New-GodotEmbeddedExe 4 $gpPckEntries $true)
}
$patchScenarios['gp_pck_v4_sparse_rejected'] = {
    param($d)
    Touch "$d\Game.exe"
    $pack = New-GodotPck 4 $gpPckEntries 0 0 7
    [BitConverter]::GetBytes([uint32]6).CopyTo($pack, 20)
    Touch "$d\Game.pck" $pack
}
$patchScenarios['gp_pck_v4_delta_rejected'] = {
    param($d)
    Touch "$d\Game.exe"
    $pack = New-GodotPck 4 $gpPckEntries 0 0 7
    $entry = [int][BitConverter]::ToUInt64($pack, 32) + 4
    $flags = $entry + 4 + [int][BitConverter]::ToUInt32($pack, $entry) + 32
    [BitConverter]::GetBytes([uint32]4).CopyTo($pack, $flags)
    Touch "$d\Game.pck" $pack
}
$patchScenarios['gp_pck_v3_no_autoload'] = {
    param($d)
    Touch "$d\Game.exe"
    Touch "$d\Game.pck" (New-GodotPck 3 $gpPckEntries)
}
# 程序内嵌包（格式 1，绝对偏移）：复制出来并归一化偏移
$patchScenarios['gp_embedded_exe'] = {
    param($d)
    Touch "$d\Game.exe" (New-GodotEmbeddedExe 1 $gpPckEntries $true)
}
# 批次配额：每次 /batch 最多 128 条，包内前 1024 条使用实时翻译，
# 后续全部采用 cache_only，跨越配额边界的批次也按对应部分处理；
# 单个资源最多提供 1800 条字符串。
function New-BulkJson([int]$count, [string]$tag) {
    $sb = New-Object Text.StringBuilder
    $null = $sb.Append('{"items":[')
    for ($i = 0; $i -lt $count; $i++) {
        if ($i) { $null = $sb.Append(',') }
        $null = $sb.AppendFormat('{{"id":"{0}{1:d4}","name":"{0} entry {1:d4} of the batch test"}}', $tag, $i)
    }
    $null = $sb.Append(']}')
    return $utf8.GetBytes($sb.ToString())
}
$patchScenarios['gp_batch_live_quota'] = {
    param($d)
    Touch "$d\Game.exe"
    Touch "$d\Game.pck" (New-GodotPck 1 @(
        (GodotEntry 'res://data/items.json' (New-BulkJson 500 'Alpha')),
        (GodotEntry 'res://data/skills.json' (New-BulkJson 700 'Bravo')),
        (GodotEntry 'res://data/heroes.json' (New-BulkJson 1850 'Delta'))))
}
# 打包游戏没有可翻译内容时，不安装任何补丁包
$patchScenarios['gp_no_translatable_entries'] = {
    param($d)
    Touch "$d\Game.exe"
    Touch "$d\Game.pck" (New-GodotPck 1 @((GodotEntry 'res://art/potion.png' ($asciiEnc.GetBytes('PNGDATA')))))
}

if (-not $godotSkip) {
    foreach ($name in $patchScenarios.Keys) {
        $dirs = @{}
        foreach ($side in @('c', 'cs')) {
            $dirs[$side] = Join-Path $root "gp\$side\$name"
            Mkdir $dirs[$side]
            & $patchScenarios[$name] $dirs[$side]
        }
        $fakeState.Bodies.Clear()
        $c = Invoke-Detect $CLauncher @('--godot-patch-and-exit', $dirs['c'])
        $bodiesC = @($fakeState.Bodies.ToArray())
        $fakeState.Bodies.Clear()
        $cs = Invoke-Detect $CsLauncher @('--godot-patch', $dirs['cs'])
        $bodiesCs = @($fakeState.Bodies.ToArray())
        $outC = $c.Out -replace [regex]::Escape($dirs['c']), '<DIR>'
        $outCs = $cs.Out -replace [regex]::Escape($dirs['cs']), '<DIR>'
        $treeC = Snapshot-Tree $dirs['c']
        $treeCs = Snapshot-Tree $dirs['cs']
        $label = "godot-patch/$name"
        if ($c.Code -ne $cs.Code) {
            $fail++; $failures.Add("[$label] exit codes C=$($c.Code) C#=$($cs.Code)`n--- C ---`n$($c.Out)$($c.Err)--- C# ---`n$($cs.Out)$($cs.Err)")
        } elseif ($outC -ne $outCs) {
            $fail++; $failures.Add("[$label] stdout differs`n--- C ---`n$outC--- C# ---`n$outCs")
        } elseif ($treeC -ne $treeCs) {
            $fail++; $failures.Add("[$label] resulting trees differ`n--- C ---`n$treeC`n--- C# ---`n$treeCs")
        } elseif (($bodiesC -join "`n") -ne ($bodiesCs -join "`n")) {
            $fail++; $failures.Add("[$label] /batch request bodies differ`n--- C ---`n$($bodiesC -join "`n")`n--- C# ---`n$($bodiesCs -join "`n")")
        } elseif ($name -eq 'gp_pck_v4_autoload' -and
                   ($c.Code -ne 0 -or -not (Test-Path "$($dirs['c'])\dst_godot_patch.pck") -or
                    $asciiEnc.GetString([IO.File]::ReadAllBytes("$($dirs['c'])\dst_godot_patch.pck")) -notmatch '\[autoload_prepend\]')) {
            $fail++; $failures.Add("[$label] v4 must build a pack containing the runtime autoload: $outC")
        } elseif ($name -like 'gp_pck_v4_*_rejected' -and
                   ((Test-Path "$($dirs['c'])\dst_godot_patch.pck") -or $outC -notmatch 'unsupported')) {
            $fail++; $failures.Add("[$label] flagged v4 must be rejected with a diagnostic: $outC")
        } elseif ($name -eq 'gp_pck_v4_embedded' -and -not (Test-Path "$($dirs['c'])\dst_godot_patch.pck")) {
            $fail++; $failures.Add("[$label] v4 embedded pack was not built: $outC")
        } elseif ($fakeState.Error) {
            $fail++; $failures.Add("[$label] fake server rejected a request: $($fakeState.Error)")
        } else {
            $pass++
            $res = if ($outC -match '(?m)^result=(\d+)$') { $Matches[1] } else { '?' }
            Write-Host ("  ok  {0,-52} result={1} batches={2}" -f $label, $res, $bodiesC.Count)
            if ($ShowLogs) { ($outC -split "`n") | Where-Object { $_ -like 'log=*' -or $_ -like 'pack=*' } | ForEach-Object { Write-Host "        $_" } }
        }
    }

    # 正在使用的包被锁定时，必须暂存为 dst_godot_patch.next.pck，下次运行再提升；
    # 持锁期间，两个启动器应报告相同的 Windows 错误；
    # 释放锁后，两版均应提升暂存包。
    $stageDirs = @{}
    $locks = @{}
    foreach ($side in @('c', 'cs')) {
        $sd = Join-Path $root "gp\$side\gp_staged_busy"
        Mkdir $sd
        & $patchScenarios['gp_pck_v1_resources'] $sd
        Touch "$sd\dst_godot_patch.pck" ($asciiEnc.GetBytes('previous pack held open by the running game'))
        $stageDirs[$side] = $sd
        $locks[$side] = [IO.File]::Open("$sd\dst_godot_patch.pck", 'Open', 'Read', 'None')
    }
    $fakeState.Bodies.Clear()
    $c = Invoke-Detect $CLauncher @('--godot-patch-and-exit', $stageDirs['c'])
    $fakeState.Bodies.Clear()
    $cs = Invoke-Detect $CsLauncher @('--godot-patch', $stageDirs['cs'])
    $cP = Invoke-Detect $CLauncher @('--godot-promote-and-exit', $stageDirs['c'])
    $csP = Invoke-Detect $CsLauncher @('--godot-promote', $stageDirs['cs'])
    foreach ($side in @('c', 'cs')) { $locks[$side].Close() }
    $cP2 = Invoke-Detect $CLauncher @('--godot-promote-and-exit', $stageDirs['c'])
    $csP2 = Invoke-Detect $CsLauncher @('--godot-promote', $stageDirs['cs'])
    $outC = ($c.Out + $cP.Out + $cP2.Out) -replace [regex]::Escape($stageDirs['c']), '<DIR>'
    $outCs = ($cs.Out + $csP.Out + $csP2.Out) -replace [regex]::Escape($stageDirs['cs']), '<DIR>'
    $treeC = Snapshot-Tree $stageDirs['c']
    $treeCs = Snapshot-Tree $stageDirs['cs']
    if ($outC -ne $outCs -or $treeC -ne $treeCs) {
        $fail++; $failures.Add("[godot-patch/staged_busy] stdout or tree differs`n--- C ---`n$outC$treeC`n--- C# ---`n$outCs$treeCs")
    } elseif ($outC -notmatch '(?m)^log=Godot: active patch pack is busy; staged refreshed pack for next launch: <DIR>\\dst_godot_patch\.next\.pck$' -or
              $outC -notmatch '(?m)^log=Godot: refreshed patch pack exists but could not be promoted yet\. Windows error: \d+$' -or
              $outC -notmatch '(?m)^log=Godot: promoted refreshed patch pack from previous run\.$' -or
              (Test-Path -LiteralPath "$($stageDirs['c'])\dst_godot_patch.next.pck")) {
        $fail++; $failures.Add("[godot-patch/staged_busy] staging/promotion path not taken`n$outC")
    } else {
        $pass++; Write-Host ("  ok  {0,-52} staged -> promoted" -f 'godot-patch/staged_busy')
    }

    # --godot-launcher：创建硬链接副本和归属标记，第二次运行保持幂等；
    # 副本已存在却没有标记时，拒绝操作并返回 result=0。
    $lnDirs = @{}
    foreach ($side in @('c', 'cs')) {
        $ld = Join-Path $root "gp\$side\gp_launcher"
        Mkdir $ld
        Touch "$ld\Game.exe" ($asciiEnc.GetBytes('game executable bytes'))
        Touch "$ld\Game.pck" (New-GodotPck 1 @((GodotEntry 'res://data/items.json' ($utf8.GetBytes($gpJson)))))
        $lnDirs[$side] = $ld
    }
    $l1 = Invoke-Detect $CLauncher @('--godot-launcher-and-exit', $lnDirs['c'])
    $l1cs = Invoke-Detect $CsLauncher @('--godot-launcher', $lnDirs['cs'])
    $l2 = Invoke-Detect $CLauncher @('--godot-launcher-and-exit', $lnDirs['c'])
    $l2cs = Invoke-Detect $CsLauncher @('--godot-launcher', $lnDirs['cs'])
    foreach ($side in @('c', 'cs')) {
        Move-Item -LiteralPath "$($lnDirs[$side])\Game.exe" -Destination "$($lnDirs[$side])\Game.exe.old"
        Touch "$($lnDirs[$side])\Game.exe" ($asciiEnc.GetBytes('updated game executable bytes'))
    }
    $l3 = Invoke-Detect $CLauncher @('--godot-launcher-and-exit', $lnDirs['c'])
    $l3cs = Invoke-Detect $CsLauncher @('--godot-launcher', $lnDirs['cs'])
    $refreshedC = [IO.File]::ReadAllBytes("$($lnDirs['c'])\dst_godot_patch.exe")
    $refreshedCs = [IO.File]::ReadAllBytes("$($lnDirs['cs'])\dst_godot_patch.exe")
    foreach ($side in @('c', 'cs')) { Remove-Item -LiteralPath "$($lnDirs[$side])\dst_godot_patch.exe.dst-owned" -Force }
    $l4 = Invoke-Detect $CLauncher @('--godot-launcher-and-exit', $lnDirs['c'])
    $l4cs = Invoke-Detect $CsLauncher @('--godot-launcher', $lnDirs['cs'])
    $outC = ($l1.Out + $l2.Out + $l3.Out + $l4.Out) -replace [regex]::Escape($lnDirs['c']), '<DIR>'
    $outCs = ($l1cs.Out + $l2cs.Out + $l3cs.Out + $l4cs.Out) -replace [regex]::Escape($lnDirs['cs']), '<DIR>'
    $treeC = Snapshot-Tree $lnDirs['c']
    $treeCs = Snapshot-Tree $lnDirs['cs']
    if ($outC -ne $outCs -or $treeC -ne $treeCs) {
        $fail++; $failures.Add("[godot-patch/launcher_copy] stdout or tree differs`n--- C ---`n$outC$treeC`n--- C# ---`n$outCs$treeCs")
    } elseif ($outC -notmatch '(?m)^log=Godot: prepared a launcher-owned executable for exports that disable --main-pack\.$' -or
              $outC -notmatch '(?m)^launcher=<DIR>\\dst_godot_patch\.exe$' -or
              $outC -notmatch '(?m)^log=Godot: refreshed launcher-owned executable after the game changed\.$' -or
              $outC -notmatch '(?m)^log=Godot: preserved an existing dst_godot_patch\.exe without a launcher ownership marker\.$' -or
              [Text.Encoding]::ASCII.GetString($refreshedC) -ne 'updated game executable bytes' -or
              [Text.Encoding]::ASCII.GetString($refreshedCs) -ne 'updated game executable bytes' -or
              @($outC -split "`n" | Where-Object { $_ -eq 'result=1' }).Count -ne 3 -or
              @($outC -split "`n" | Where-Object { $_ -eq 'result=0' }).Count -ne 1) {
        $fail++; $failures.Add("[godot-patch/launcher_copy] ownership marker path not taken`n$outC")
    } else {
        $pass++; Write-Host ("  ok  {0,-52} link + marker + refusal" -f 'godot-patch/launcher_copy')
    }
} else {
    $godotSkipped = $patchScenarios.Count + 2
}

# ---------------------------------------------------------------- 配置/预检一致性
#
# api.ini 数据层（--api-config-and-exit / --api-config-set-and-exit 对应 --api-config /
# --api-config-set）、--main-pack 拒绝分类器（--godot-probe*），以及无头预检
# 结论缓存（--godot-preflight*）。两个启动器各用自己的根目录，
# 因此输出中的 api.ini/godot_preflight.ini 路径会归一化；其他内容，包括
# 预设表、读取值、写出的 INI 字节、签名字符串及缓存命中/未命中结果，
# 都必须一致。签名含被探测文件的修改时间和大小，
# 因此两版指向同一个共享文件。
$cfgC = Join-Path $root 'cfg_c'; Mkdir $cfgC
$cfgCs = Join-Path $root 'cfg_cs'; Mkdir $cfgCs
$cfgExe = Join-Path $cfgC (Split-Path -Leaf $CLauncher)
Copy-Item -LiteralPath $CLauncher -Destination $cfgExe
$cfgShared = Join-Path $root 'cfg_shared'; Mkdir $cfgShared
Touch "$cfgShared\Game.exe" ($asciiEnc.GetBytes('preflight signature target'))

function Hex-Or-Missing([string]$path) {
    if (-not (Test-Path -LiteralPath $path)) { return 'missing' }
    return [BitConverter]::ToString([IO.File]::ReadAllBytes($path))
}
function Invoke-ConfigPair([string[]]$cArgs, [string[]]$csArgs) {
    $c = Invoke-Detect $cfgExe $cArgs
    $cs = Invoke-Detect $CsLauncher ($csArgs + @('--root', $cfgCs))
    return @{
        C = $c
        Cs = $cs
        OutC = $c.Out -replace [regex]::Escape($cfgC), '<ROOT>'
        OutCs = $cs.Out -replace [regex]::Escape($cfgCs), '<ROOT>'
    }
}
function Check-ConfigPair([string]$label, [object]$r, [string[]]$mustMatch) {
    if ($r.C.Code -ne $r.Cs.Code) {
        $script:fail++; $script:failures.Add("[$label] exit codes C=$($r.C.Code) C#=$($r.Cs.Code)`n--- C ---`n$($r.C.Out)$($r.C.Err)--- C# ---`n$($r.Cs.Out)$($r.Cs.Err)")
        return $false
    }
    if ($r.OutC -ne $r.OutCs) {
        $script:fail++; $script:failures.Add("[$label] stdout differs`n--- C ---`n$($r.OutC)--- C# ---`n$($r.OutCs)")
        return $false
    }
    foreach ($m in $mustMatch) {
        if ($r.OutC -notmatch $m) {
            $script:fail++; $script:failures.Add("[$label] expected /$m/ in`n$($r.OutC)")
            return $false
        }
    }
    return $true
}

# API 对话框：全部八个控件（含刷新/状态）、选择模型及手动输入。
$r = Invoke-ConfigPair @('--api-ui-probe-and-exit') @('--api-ui-probe')
if (Check-ConfigPair 'api-config/ui' $r @('(?m)^control=206,400,184,144,30$', '(?m)^control=207,24,300,520,56$', '(?m)^select=1$', '(?m)^manual=1$', '(?m)^catalog_preserves=1$', '(?m)^catalog_new_id=1$', '(?m)^catalog_unlisted=1$', '(?m)^provider_same_keeps=1$', '(?m)^provider_change_clears=1$', '(?m)^provider_custom_keeps=1$', '(?m)^empty_model_rejected=1$', '(?m)^stale_catalog_ignored=1$', '(?m)^all_providers_clear=1$', '(?m)^catalog_listed=1$', '(?m)^catalog_case_sensitive=1$', '(?m)^catalog_failure_keeps=1$')) {
    $pass++; Write-Host ('  ok  {0,-52} 8 controls, editable model selection' -f 'api-config/ui')
}

# api.ini 尚不存在时的默认值，以及完整的提供商预设表
$r = Invoke-ConfigPair @('--api-config-and-exit') @('--api-config')
if (Check-ConfigPair 'api-config/defaults' $r @(
        '(?m)^presets=11$',
        '(?m)^preset0=[^|]+\|-$',
        '(?m)^preset1=DeepSeek\|https://api\.deepseek\.com/v1/chat/completions$',
        '(?m)^endpoint=https://api\.deepseek\.com/v1/chat/completions$',
        '(?m)^model=$',
        '(?m)^key=$',
        '(?m)^selected=1$')) {
    $pass++; Write-Host ("  ok  {0,-52} presets=11" -f 'api-config/defaults')
}
# 写入三个键后，api.ini 必须逐字节一致（Profile API 编码规则）
$r = Invoke-ConfigPair @('--api-config-set-and-exit', 'https://api.moonshot.cn/v1/chat/completions', 'kimi-latest-8k', 'sk-parity-KEY') `
                       @('--api-config-set', 'https://api.moonshot.cn/v1/chat/completions', 'kimi-latest-8k', 'sk-parity-KEY')
$iniC = Hex-Or-Missing "$cfgC\config\api.ini"
$iniCs = Hex-Or-Missing "$cfgCs\config\api.ini"
if (Check-ConfigPair 'api-config/save' $r @('(?m)^log=API .+<ROOT>\\config\\api\.ini$', '(?m)^result=1$')) {
    if ($iniC -ne $iniCs -or $iniC -eq 'missing') {
        $fail++; $failures.Add("[api-config/save] api.ini bytes differ`n--- C ---`n$iniC`n--- C# ---`n$iniCs")
    } else {
        $pass++; Write-Host ("  ok  {0,-52} identical api.ini" -f 'api-config/save')
    }
}
# 重新读取：非预设端点回退到“自定义”（selected=0）
$r = Invoke-ConfigPair @('--api-config-and-exit') @('--api-config')
$r2 = Invoke-ConfigPair @('--api-config-set-and-exit', 'https://intranet.example/v1/chat', 'house-model', '') `
                        @('--api-config-set', 'https://intranet.example/v1/chat', 'house-model', '')
$r3 = Invoke-ConfigPair @('--api-config-and-exit') @('--api-config')
if ((Check-ConfigPair 'api-config/reload' $r @('(?m)^model=kimi-latest-8k$', '(?m)^key=sk-parity-KEY$', '(?m)^selected=3$')) -and
    (Check-ConfigPair 'api-config/custom-endpoint' $r2 @('(?m)^result=1$')) -and
    (Check-ConfigPair 'api-config/custom-endpoint-reload' $r3 @('(?m)^endpoint=https://intranet\.example/v1/chat$', '(?m)^key=$', '(?m)^selected=0$'))) {
    $pass++; Write-Host ("  ok  {0,-52} preset lookup + custom fallback" -f 'api-config/reload')
}

# --main-pack 拒绝分类器：只有诊断明确表示选项被拒绝且包含该选项名才算命中；
# 同时固定无关崩溃及大小写变体的判定行为。
$probeTexts = @(
    'Unknown option: --main-pack',
    'ERROR: Invalid command line: --MAIN-PACK',
    'godot: unrecognized option "--main-pack"',
    'Main-Pack is NOT SUPPORTED by this template',
    'ERROR: failed to load autoload script, exiting',
    '--main-pack accepted, loading pack',
    'unknown option --script',
    '')
$r = Invoke-ConfigPair (@('--godot-probe-and-exit') + $probeTexts) (@('--godot-probe') + $probeTexts)
if (Check-ConfigPair 'godot-probe/classifier' $r @(
        '(?m)^reject0=1$', '(?m)^reject1=1$', '(?m)^reject2=1$', '(?m)^reject3=1$',
        '(?m)^reject4=0$', '(?m)^reject5=0$', '(?m)^reject6=0$', '(?m)^reject7=0$')) {
    $pass++; Write-Host ("  ok  {0,-52} 8 outputs classified" -f 'godot-probe/classifier')
}

# 预检缓存：未命中 -> 写入成功结论 -> 命中；不同类型或文件变化应未命中；
# 临时结论（put < 0）不得留下条目。
$r = Invoke-ConfigPair @('--godot-preflight-and-exit', 'sidecar', "$cfgShared\Game.exe", 'export', '1') `
                       @('--godot-preflight', 'sidecar', "$cfgShared\Game.exe", 'export', '1')
$ok1 = Check-ConfigPair 'godot-preflight/put-hit' $r @('(?m)^get=-1$', '(?m)^get2=1$', '(?m)^sig=\|.+\\Game\.exe\|[0-9a-f]{16}\|26\|\|export$')
$r = Invoke-ConfigPair @('--godot-preflight-and-exit', 'sidecar', "$cfgShared\Game.exe", 'export', '-1') `
                       @('--godot-preflight', 'sidecar', "$cfgShared\Game.exe", 'export', '-1')
$ok2 = Check-ConfigPair 'godot-preflight/hit' $r @('(?m)^get=1$')
$r = Invoke-ConfigPair @('--godot-preflight-and-exit', 'mainpack', "$cfgShared\Game.exe", 'export', '0') `
                       @('--godot-preflight', 'mainpack', "$cfgShared\Game.exe", 'export', '0')
$ok3 = Check-ConfigPair 'godot-preflight/other-kind' $r @('(?m)^get=-1$', '(?m)^get2=0$')
$r = Invoke-ConfigPair @('--godot-preflight-and-exit', 'sidecar', "$cfgShared\missing.exe", 'export', '-1') `
                       @('--godot-preflight', 'sidecar', "$cfgShared\missing.exe", 'export', '-1')
$ok4 = Check-ConfigPair 'godot-preflight/missing-file' $r @('(?m)^sig=\|.+\\missing\.exe\|0\|0\|\|export$', '(?m)^get=-1$')
Touch "$cfgShared\Game.exe" ($asciiEnc.GetBytes('preflight signature target, now a different size'))
$r = Invoke-ConfigPair @('--godot-preflight-and-exit', 'sidecar', "$cfgShared\Game.exe", 'export', '-1') `
                       @('--godot-preflight', 'sidecar', "$cfgShared\Game.exe", 'export', '-1')
$ok5 = Check-ConfigPair 'godot-preflight/changed-file' $r @('(?m)^get=-1$')
$preC = Hex-Or-Missing "$cfgC\config\godot_preflight.ini"
$preCs = Hex-Or-Missing "$cfgCs\config\godot_preflight.ini"
if ($ok1 -and $ok2 -and $ok3 -and $ok4 -and $ok5) {
    if ($preC -ne $preCs) {
        $fail++; $failures.Add("[godot-preflight] godot_preflight.ini bytes differ`n--- C ---`n$preC`n--- C# ---`n$preCs")
    } else {
        $pass++; Write-Host ("  ok  {0,-52} miss/put/hit + identical ini" -f 'godot-preflight/cache')
    }
}

# ---------------------------------------------------------------- 启动流程一致性
#
# ui.c 的一键流程（服务器就绪之后）：比较 --launch-flow-and-exit 与 --launch-flow。
# 预热以转储模式实际执行（post= 行）；Godot 无头预检确实启动游戏程序，
# 此程序是控制台桩，其标准输出、退出码和等待时长由同目录控制文件指定，
# 并将自身命令行追加至 dst_fake_godot_calls.txt；仅将三个真正启动目标的
# 调用点替换为 spawn=<kind>|<exe>|<cmd>|<cwd> 计划行。
# 比较归一化路径后的标准输出、结果目录树及记录的探测命令行，
# 确保流程决策和探测参数均一致。
$lfC = Join-Path $root 'lf_c'; Mkdir $lfC
$lfCs = Join-Path $root 'lf_cs'; Mkdir $lfCs
$lfExe = Join-Path $lfC (Split-Path -Leaf $CLauncher)
Copy-Item -LiteralPath $CLauncher -Destination $lfExe

$fakeGodot = Join-Path $root 'fake_godot_template.exe'
Add-Type -OutputType ConsoleApplication -OutputAssembly $fakeGodot -TypeDefinition @'
using System;
using System.IO;
using System.Threading;

public static class FakeGodot
{
    public static int Main(string[] args)
    {
        string dir = AppDomain.CurrentDomain.BaseDirectory;
        string text = "";
        int code = 0, sleep = 0, preflightCode = -1;
        string cfg = Path.Combine(dir, "dst_fake_godot.ini");
        if (File.Exists(cfg)) {
            foreach (string line in File.ReadAllLines(cfg)) {
                int eq = line.IndexOf('=');
                if (eq <= 0) continue;
                string k = line.Substring(0, eq).Trim();
                string v = line.Substring(eq + 1).Trim();
                if (k == "stdout") text = v.Replace("\\n", "\n");
                else if (k == "exit") code = int.Parse(v);
                else if (k == "preflightexit") preflightCode = int.Parse(v);
                else if (k == "sleepms") sleep = int.Parse(v);
            }
        }
        /* 调用日志放在游戏目录旁，避免 Godot 预热扫描器
           将自身的探测命令行误当成游戏文本。 */
        string self = dir.TrimEnd('\\');
        File.AppendAllText(Path.Combine(Path.GetDirectoryName(self), Path.GetFileName(self) + ".calls.txt"),
                           Environment.CommandLine + "\n");
        if (text.Length > 0) { Console.Out.Write(text); Console.Out.Flush(); }
        if (Array.IndexOf(args, "--dst-autoload-preflight") >= 0 && preflightCode == 73) {
            Console.Out.WriteLine("DST_GODOT_AUTOLOAD_READY_V1");
            Console.Out.Flush();
            Thread.Sleep(15000); // 真实父进程必须识别就绪凭据，并停止其拥有的探测进程。
            return 73;
        }
        if (sleep > 0) Thread.Sleep(sleep);
        if (Array.IndexOf(args, "--dst-preflight") >= 0 && preflightCode >= 0) return preflightCode;
        return code;
    }
}
'@

function New-FakeGodotExe([string]$dir, [string]$name, [string]$stdoutText, [int]$exitCode, [int]$sleepMs = 0, [int]$preflightCode = -1) {
    Copy-Item -LiteralPath $fakeGodot -Destination (Join-Path $dir $name)
    Write-Utf8 (Join-Path $dir 'dst_fake_godot.ini') "stdout=$stdoutText`nexit=$exitCode`nsleepms=$sleepMs`npreflightexit=$preflightCode`n"
}
function Godot-ProbeCalls([string]$dir) {
    $log = $dir.TrimEnd('\') + '.calls.txt'
    if (-not (Test-Path -LiteralPath $log)) { return '' }
    return [IO.File]::ReadAllText($log)
}

$launchScenarios = [ordered]@{}
# Ren'Py：先启动，再在后台预热脚本
$launchScenarios['lf_renpy'] = {
    param($d)
    New-RenpyGame $d
    Write-Utf8 "$d\game\script.rpy" "label start:`n    e `"Hello there, traveler.`"`n    `"It was a dark and stormy night.`"`n"
}
# RPG Maker：先导入再启动（插件的实时查询应命中已导入文本）
$launchScenarios['lf_rpgm'] = {
    param($d)
    New-RpgmWww $d
    Write-Utf8 "$d\www\data\Items.json" '[null,{"id":1,"name":"Healing Potion","description":"Restores a little HP."}]'
}
# 未知引擎：没有部署专属顺序，先预热再启动
$launchScenarios['lf_unknown'] = {
    param($d)
    Touch "$d\Game.exe"
}
# Godot 散装项目：写出侧车脚本，且预检成功
$launchScenarios['lf_godot_loose_ok'] = {
    param($d)
    Write-Utf8 "$d\project.godot" "config_version=5`n`n[application]`n`nconfig/name=`"Parity`"`n"
    Write-Utf8 "$d\scenes\menu.tscn" $gpScene
    New-FakeGodotExe $d 'Game.exe' 'Godot Engine v4.2.stable - preflight ok' 0 0 73
}
# Godot 散装项目：导出模板拒绝 --script，因此使用普通启动
$launchScenarios['lf_godot_loose_reject'] = {
    param($d)
    Write-Utf8 "$d\project.godot" "config_version=5`n`n[application]`n`nconfig/name=`"Parity`"`n"
    New-FakeGodotExe $d 'Game.exe' 'ERROR: Script does not exist in this template.' 3
}
# 忽略 --script 的模板会正常退出，但这不能证明钩子已加载。
$launchScenarios['lf_godot_loose_ignored'] = {
    param($d)
    Write-Utf8 "$d\project.godot" "config_version=5`n`n[application]`n`nconfig/name=`"Parity`"`n"
    New-FakeGodotExe $d 'Game.exe' '' 0
}
# Godot 导出游戏无补丁包：先用通用运行时侧车脚本，再独立刷新补丁
$launchScenarios['lf_godot_export_nopack'] = {
    param($d)
    Touch "$d\Game.pck" (New-GodotPck 1 @((GodotEntry 'res://data/items.json' ($utf8.GetBytes($gpJson)))))
    New-FakeGodotExe $d 'Game.exe' '' 0 0 73
}
# Godot 导出游戏有补丁包：使用启动器拥有的程序副本和内嵌侧车脚本
$launchScenarios['lf_godot_v4_first_launch'] = {
    param($d)
    Touch "$d\Game.pck" (New-GodotPck 4 $gpPckEntries 0 0 7)
    New-FakeGodotExe $d 'Game.exe' '' 0 0 73
}
$launchScenarios['lf_godot_v4_ignored'] = {
    param($d)
    Touch "$d\Game.pck" (New-GodotPck 4 $gpPckEntries 0 0 7)
    New-FakeGodotExe $d 'Game.exe' '' 0
}
$launchScenarios['lf_godot_autoload_error_marker'] = {
    param($d)
    Touch "$d\Game.pck" (New-GodotPck 4 $gpPckEntries 0 0 7)
    New-FakeGodotExe $d 'Game.exe' 'SCRIPT ERROR: print("DST_GODOT_AUTOLOAD_READY_V1") failed.' 0
}
$launchScenarios['lf_godot_autoload_crashed'] = {
    param($d)
    Touch "$d\Game.pck" (New-GodotPck 4 $gpPckEntries 0 0 7)
    New-FakeGodotExe $d 'Game.exe' 'DST_GODOT_AUTOLOAD_READY_V1\n' 1
}
$launchScenarios['lf_godot_export_pack'] = {
    param($d)
    Touch "$d\Game.pck" (New-GodotPck 1 @((GodotEntry 'res://data/items.json' ($utf8.GetBytes($gpJson)))))
    Touch "$d\dst_godot_patch.pck" (New-GodotPck 1 @(
        (GodotEntry 'res://data/items.json' ($utf8.GetBytes($gpJson))),
        (GodotEntry 'res://dst_godot_runtime.gd' ($utf8.GetBytes("extends SceneTree`n")))))
    New-FakeGodotExe $d 'Game.exe' '' 0
}
# Godot 导出模板拒绝 --main-pack：启动器副本因已有 dst_godot_patch.exe
# 却无归属标记而被拒绝，侧车预检失败，独立 --main-pack 探测
# 又得到明确的选项拒绝诊断，因此改为普通启动。
$launchScenarios['lf_godot_mainpack_rejected'] = {
    param($d)
    Touch "$d\Game.pck" (New-GodotPck 1 @((GodotEntry 'res://data/items.json' ($utf8.GetBytes($gpJson)))))
    Touch "$d\dst_godot_patch.pck" (New-GodotPck 1 @((GodotEntry 'res://data/items.json' ($utf8.GetBytes($gpJson)))))
    Touch "$d\dst_godot_patch.exe" ($asciiEnc.GetBytes('user-provided copy without an ownership marker'))
    New-FakeGodotExe $d 'Game.exe' 'Unknown option: --main-pack. Try --help for a list of options.' 1
}

$lfOut = @{}
foreach ($name in $launchScenarios.Keys) {
    $dirs = @{}
    foreach ($side in @('c', 'cs')) {
        $dirs[$side] = Join-Path $root "lf\$side\$name"
        Mkdir $dirs[$side]
        & $launchScenarios[$name] $dirs[$side]
    }
    $c = Invoke-Detect $lfExe @('--launch-flow-and-exit', $dirs['c'])
    $cs = Invoke-Detect $CsLauncher @('--launch-flow', $dirs['cs'], '--root', $lfCs)
    # 启动器二进制及其根目录按设计不同，其他内容必须一致
    $outC = $c.Out -replace [regex]::Escape($dirs['c']), '<DIR>' -replace [regex]::Escape($lfExe), '<LAUNCHER>' -replace [regex]::Escape($lfC), '<ROOT>'
    $outCs = $cs.Out -replace [regex]::Escape($dirs['cs']), '<DIR>' -replace [regex]::Escape($CsLauncher), '<LAUNCHER>' -replace [regex]::Escape($lfCs), '<ROOT>'
    $callsC = (Godot-ProbeCalls $dirs['c']) -replace [regex]::Escape($dirs['c']), '<DIR>'
    $callsCs = (Godot-ProbeCalls $dirs['cs']) -replace [regex]::Escape($dirs['cs']), '<DIR>'
    $treeC = Snapshot-Tree $dirs['c']
    $treeCs = Snapshot-Tree $dirs['cs']
    $lfOut[$name] = $outC
    $label = "launch-flow/$name"
    if ($c.Code -ne $cs.Code) {
        $fail++; $failures.Add("[$label] exit codes C=$($c.Code) C#=$($cs.Code)`n--- C ---`n$($c.Out)$($c.Err)--- C# ---`n$($cs.Out)$($cs.Err)")
    } elseif ($outC -ne $outCs) {
        $fail++; $failures.Add("[$label] stdout differs`n--- C ---`n$outC--- C# ---`n$outCs")
    } elseif ($callsC -ne $callsCs) {
        $fail++; $failures.Add("[$label] headless probe command lines differ`n--- C ---`n$callsC`n--- C# ---`n$callsCs")
    } elseif ($treeC -ne $treeCs) {
        $fail++; $failures.Add("[$label] resulting trees differ`n--- C ---`n$treeC`n--- C# ---`n$treeCs")
    } else {
        $pass++
        $spawns = @($outC -split "`n" | Where-Object { $_ -like 'spawn=*' }).Count
        $probes = @($callsC -split "`n" | Where-Object { $_ -ne '' }).Count
        Write-Host ("  ok  {0,-52} spawn={1} probes={2}" -f $label, $spawns, $probes)
        if ($ShowLogs) { ($outC -split "`n") | Where-Object { $_ -like 'log=*' -or $_ -like 'spawn=*' -or $_ -like 'status=*' } | ForEach-Object { Write-Host "        $_" } }
    }
}

# 对 C 输出逐场景断言：每个流程必须命中预期分支，
# 不能只验证两版碰巧走了相同分支。
$lfExpect = [ordered]@{
    'lf_renpy' = @('(?m)^spawn=shell\|<DIR>\\Game\.exe\|\|<DIR>$', '(?m)^post=/prefetch ', '(?m)^status=游戏已启动；翻译组件已部署，游戏内加载待确认。$')
    'lf_rpgm' = @('(?m)^post=/prefetch ', '(?m)^spawn=shell\|<DIR>\\Game\.exe\|\|<DIR>$', '(?m)^status=游戏已启动；翻译组件已部署，游戏内加载待确认。$')
    'lf_unknown' = @('(?m)^spawn=shell\|<DIR>\\Game\.exe\|\|<DIR>$')
    'lf_godot_loose_ok' = @(
        '(?m)^log=Godot: loose project detected; launching with runtime translation sidecar\.$',
        '(?m)^spawn=proc\|<DIR>\\Game\.exe\|"<DIR>\\Game\.exe" --path "<DIR>" --script "res://dst_godot_runtime\.gd" --language en\|<DIR>$',
        '(?m)^spawn=worker\|<LAUNCHER>\|"<LAUNCHER>" --godot-patch-worker "<DIR>"\|<ROOT>$')
    'lf_godot_loose_reject' = @(
        '(?m)^log=Godot: runtime sidecar preflight exited with code 3\.$',
        '(?m)^log=Godot: runtime-sidecar launch failed; falling back to normal game launch\.$',
        '(?m)^spawn=shell\|<DIR>\\Game\.exe\|\|<DIR>$')
    'lf_godot_loose_ignored' = @(
        '(?m)^log=Godot: runtime sidecar preflight exited with code 0\.$',
        '(?m)^spawn=shell\|<DIR>\\Game\.exe\|\|<DIR>$',
        '(?m)^status=Godot: 游戏已启动，但实时翻译模块未确认加载；请查看日志。$')
    'lf_godot_export_nopack' = @(
        '(?m)^log=Godot: no patch pack yet; using the generic runtime translator for this launch\.$',
        '(?m)^log=Launching Godot export with runtime translator before static patch is ready: <DIR>\\Game\.exe$',
        '(?m)^spawn=proc\|<DIR>\\Game\.exe\|"<DIR>\\Game\.exe" --script "<DIR>\\dst_godot_runtime\.gd" --language en\|<DIR>$')
    'lf_godot_v4_first_launch' = @(
        'preparing initial runtime pack without translation requests',
        'patched 3 resources with 0 translated strings',
        '(?m)^log=Godot: runtime autoload load proof confirmed; independent probe stopped\.$',
        '(?m)^spawn=proc\|<DIR>\\dst_godot_patch\.exe\|"<DIR>\\dst_godot_patch\.exe" --language en\|<DIR>$')
    'lf_godot_v4_ignored' = @(
        'preparing initial runtime pack without translation requests',
        '(?m)^log=Godot: runtime autoload preflight exited with code 0\.$',
        '(?m)^status=Godot: 游戏已启动，但实时翻译模块未确认加载；请查看日志。$')
    'lf_godot_autoload_error_marker' = @(
        '(?m)^log=Godot: runtime autoload preflight exited with code 0\.$',
        '(?m)^status=Godot: 游戏已启动，但实时翻译模块未确认加载；请查看日志。$')
    'lf_godot_autoload_crashed' = @(
        '(?m)^log=Godot: runtime autoload preflight exited with code 1\.$',
        '(?m)^status=Godot: 游戏已启动，但实时翻译模块未确认加载；请查看日志。$')
    'lf_godot_export_pack' = @(
        '(?m)^log=Godot: launching with external translation patch pack\.$',
        '(?m)^spawn=proc\|<DIR>\\dst_godot_patch\.exe\|"<DIR>\\dst_godot_patch\.exe" --script "res://dst_godot_runtime\.gd" --language en\|<DIR>$',
        '(?m)^status=Godot: 游戏已启动，但实时翻译模块未确认加载；请查看日志。$')
    'lf_godot_mainpack_rejected' = @(
        '(?m)^log=Godot: preserved an existing dst_godot_patch\.exe without a launcher ownership marker\.$',
        '(?m)^log=Godot: --main-pack probe received an explicit option-rejection diagnostic \(exit 1\)\.$',
        '(?m)^log=Godot: template rejects --main-pack; falling back to a normal launch without the translation patch pack\.$',
        '(?m)^spawn=shell\|<DIR>\\Game\.exe\|\|<DIR>$')
}
foreach ($name in $lfExpect.Keys) {
    # 使用首次运行的输出；再次运行时部分探测会直接使用预检缓存
    $out = $lfOut[$name]
    $missing = @($lfExpect[$name] | Where-Object { $out -notmatch $_ })
    if ($missing.Count) {
        $fail++; $failures.Add("[launch-flow/$name branch] expected $($missing -join ' , ') in`n$out")
    } else {
        $pass++; Write-Host ("  ok  {0,-52} {1} branch markers" -f "launch-flow/$name branch", $lfExpect[$name].Count)
    }
}

$stampScript = Join-Path $PSScriptRoot 'check_godot_source_stamp.py'
$stampOk = $true
foreach ($side in @('c', 'cs')) {
    $d = Join-Path $root "lf\$side\lf_godot_v4_first_launch"
    & python $stampScript "$d\Game.pck" "$d\dst_godot_patch.pck.dst-source" `
        (Join-Path $repo 'payloads\Godot\dst_godot_runtime_g3.gd') `
        (Join-Path $repo 'payloads\Godot\dst_godot_runtime_g4.gd') `
        (Join-Path $repo 'VERSION') `
        (Join-Path $repo 'assets\fonts\manifest.json')
    if ($LASTEXITCODE -ne 0) { $stampOk = $false }
}
if ($stampOk) {
    $pass++; Write-Host ("  ok  {0,-52} source + both payloads + font manifest + version" -f 'godot-patch/source-payload-stamp')
} else {
    $fail++; $failures.Add('[godot-patch/source-payload-stamp] marker does not bind source, both Godot payloads, font manifest, and VERSION')
}

# 源码内容改变但大小、修改时间相同时，尝试复用已生成的 v4 运行时；
# 字节指纹必须强制重建，仅按时间/长度缓存会静默保留旧运行时。
$staleDirs = @{ c = (Join-Path $root 'lf\c\lf_godot_v4_first_launch'); cs = (Join-Path $root 'lf\cs\lf_godot_v4_first_launch') }
$oldMarks = @{}
foreach ($side in @('c', 'cs')) {
    $d = $staleDirs[$side]
    $mark = "$d\dst_godot_patch.pck.dst-source"
    $oldMarks[$side] = [IO.File]::ReadAllText($mark)
    $source = "$d\Game.pck"
    $oldTime = [IO.File]::GetLastWriteTimeUtc($source)
    $bytes = [IO.File]::ReadAllBytes($source)
    $needle = $asciiEnc.GetBytes('PNGDATA')
    $at = -1
    for ($i = 0; $i -le $bytes.Length - $needle.Length; $i++) {
        $same = $true
        for ($j = 0; $j -lt $needle.Length; $j++) { if ($bytes[$i + $j] -ne $needle[$j]) { $same = $false; break } }
        if ($same) { $at = $i; break }
    }
    if ($at -lt 0) { throw 'PNGDATA fixture is missing from v4 source pack' }
    $bytes[$at + 6] = [byte][char]'E'
    [IO.File]::WriteAllBytes($source, $bytes)
    [IO.File]::SetLastWriteTimeUtc($source, $oldTime)
}
$staleC = Invoke-Detect $lfExe @('--launch-flow-and-exit', $staleDirs['c'])
$staleCs = Invoke-Detect $CsLauncher @('--launch-flow', $staleDirs['cs'], '--root', $lfCs)
$staleOutC = $staleC.Out -replace [regex]::Escape($staleDirs['c']), '<DIR>' -replace [regex]::Escape($lfExe), '<LAUNCHER>' -replace [regex]::Escape($lfC), '<ROOT>'
$staleOutCs = $staleCs.Out -replace [regex]::Escape($staleDirs['cs']), '<DIR>' -replace [regex]::Escape($CsLauncher), '<LAUNCHER>' -replace [regex]::Escape($lfCs), '<ROOT>'
$newMarkC = [IO.File]::ReadAllText("$($staleDirs['c'])\dst_godot_patch.pck.dst-source")
$newMarkCs = [IO.File]::ReadAllText("$($staleDirs['cs'])\dst_godot_patch.pck.dst-source")
if ($staleC.Code -ne $staleCs.Code -or $staleOutC -ne $staleOutCs -or
    (Snapshot-Tree $staleDirs['c']) -ne (Snapshot-Tree $staleDirs['cs']) -or
    $staleOutC -notmatch 'game pack or translator payload changed, or the source fingerprint is invalid' -or
    $staleOutC -notmatch 'preparing initial runtime pack without translation requests' -or
    $newMarkC -eq $oldMarks['c'] -or $newMarkCs -eq $oldMarks['cs'] -or $newMarkC -ne $newMarkCs) {
    $fail++; $failures.Add("[launch-flow/godot-source-refresh] stale source was reused or C/C# diverged`n--- C ---`n$staleOutC`n--- C# ---`n$staleOutCs")
} else {
    $pass++; Write-Host ("  ok  {0,-52} same size/time, new fingerprint" -f 'launch-flow/godot-source-refresh')
}

# 即使游戏未改动，也必须重建使用旧翻译器组件指纹生成的包；
# 仅降低归属元数据中的版本来模拟此次升级。
foreach ($side in @('c', 'cs')) {
    $marker = "$($staleDirs[$side])\dst_godot_patch.pck.dst-source"
    [IO.File]::WriteAllText($marker, ([IO.File]::ReadAllText($marker) -replace 'SOURCE_V4', 'SOURCE_V3'), $asciiEnc)
}
$upgradeC = Invoke-Detect $lfExe @('--launch-flow-and-exit', $staleDirs['c'])
$upgradeCs = Invoke-Detect $CsLauncher @('--launch-flow', $staleDirs['cs'], '--root', $lfCs)
$upgradeOutC = $upgradeC.Out -replace [regex]::Escape($staleDirs['c']), '<DIR>' -replace [regex]::Escape($lfExe), '<LAUNCHER>' -replace [regex]::Escape($lfC), '<ROOT>'
$upgradeOutCs = $upgradeCs.Out -replace [regex]::Escape($staleDirs['cs']), '<DIR>' -replace [regex]::Escape($CsLauncher), '<LAUNCHER>' -replace [regex]::Escape($lfCs), '<ROOT>'
if ($upgradeC.Code -ne $upgradeCs.Code -or $upgradeOutC -ne $upgradeOutCs -or
    (Snapshot-Tree $staleDirs['c']) -ne (Snapshot-Tree $staleDirs['cs']) -or
    $upgradeOutC -notmatch 'game pack or translator payload changed, or the source fingerprint is invalid' -or
    $upgradeOutC -notmatch 'preparing initial runtime pack without translation requests' -or
    -not ([IO.File]::ReadAllText("$($staleDirs['c'])\dst_godot_patch.pck.dst-source")).StartsWith('DST_GODOT_SOURCE_V4:')) {
    $fail++; $failures.Add("[launch-flow/godot-payload-upgrade] old translator stamp was reused or C/C# diverged`n--- C ---`n$upgradeOutC`n--- C# ---`n$upgradeOutCs")
} else {
    $pass++; Write-Host ("  ok  {0,-52} unchanged game, old translator stamp" -f 'launch-flow/godot-payload-upgrade')
}

# --clear-cache-and-exit / --clear-cache：删除共享缓存文件，不存在视为已清理；
# 同名目录应保留，并输出相同诊断。
$ccC = Join-Path $root 'cc_c'; Mkdir $ccC
$ccCs = Join-Path $root 'cc_cs'; Mkdir $ccCs
$ccExe = Join-Path $ccC (Split-Path -Leaf $CLauncher)
Copy-Item -LiteralPath $CLauncher -Destination $ccExe
function Invoke-ClearCache() {
    $c = Invoke-Detect $ccExe @('--clear-cache-and-exit')
    $cs = Invoke-Detect $CsLauncher @('--clear-cache', '--root', $ccCs)
    return @{
        C = $c
        Cs = $cs
        OutC = $c.Out -replace [regex]::Escape($ccC), '<ROOT>'
        OutCs = $cs.Out -replace [regex]::Escape($ccCs), '<ROOT>'
    }
}
$ccOk = $true
# 1. 没有需要删除的内容
$r = Invoke-ClearCache
$ccOk = (Check-ConfigPair 'clear-cache/missing' $r @('(?m)^cache=0\.0 MB$', '(?m)^result=1$')) -and $ccOk
# 2. 删除实际缓存文件（1.5 MiB 显示为 "1.5 MB"）
foreach ($d in @($ccC, $ccCs)) { Touch "$d\translation_memory_c.tsv" (New-Object byte[] (1024 * 1024 + 512 * 1024)) }
$r = Invoke-ClearCache
$ccOk = (Check-ConfigPair 'clear-cache/deleted' $r @('(?m)^cache=1\.5 MB$', '(?m)^cache2=0\.0 MB$', '(?m)^result=1$')) -and $ccOk
# 3. 同名目录应保留并报告
foreach ($d in @($ccC, $ccCs)) { Mkdir "$d\translation_memory_c.tsv" }
$r = Invoke-ClearCache
$ccOk = (Check-ConfigPair 'clear-cache/directory' $r @(
    '(?m)^log=清除缓存：目标路径是目录，已保留：<ROOT>\\translation_memory_c\.tsv$', '(?m)^result=0$')) -and $ccOk
foreach ($d in @($ccC, $ccCs)) { Remove-Item -LiteralPath "$d\translation_memory_c.tsv" -Recurse -Force }
if ($ccOk) {
    $pass++; Write-Host ("  ok  {0,-52} missing/deleted/directory" -f 'clear-cache/core')
}

# ---------------------------------------------------------------- 窗口渲染一致性
#
# ui.c 的窗口与绘制层：比较 --ui-probe-and-exit 与 --ui-probe。
# 窗口没有可供比较的文本输出，因此探针将绘制结果转为可比较的产物：
# 两个启动器将 DPI 固定为 96、动画时钟固定为 0，将两个运行时身份字符串
# 替换为中性占位内容，并从参数读取服务状态。随后创建不可见的
# WS_POPUP 主窗口，客户区严格为 <w>x<h>，将背景及全部子控件绘制到
# 同一张 32bpp DIB 中；自绘按钮使用合成的 DRAWITEMSTRUCT，
# 标准控件通过 WM_PRINTCLIENT 绘制，最后写出未压缩的 BMP。
#
# 逐行比较报告（客户区尺寸、DPI、真实运行时标签及每个控件的 ID、
# 客户区矩形和文本），并逐字节比较 BMP。任意位置即使仅一个颜色分量不同，
# 无论是圆角、渐变、字形还是应用图标，都应失败。
#
# 特意选择时钟 0：此时 sinf(-pi/2) 和 cosf(0) 恰好为 -1.0f 和 1.0f，
# 使呼吸圆点和主区域光束不受两种运行时数学库末位精度差异影响。
$uiRoot = Join-Path $root 'ui'; Mkdir $uiRoot
$uiExe = Join-Path $uiRoot (Split-Path -Leaf $CLauncher)
Copy-Item -LiteralPath $CLauncher -Destination $uiExe

# 页面底部的版本标签来自构建版本：C 启动器使用 -DDS_TRANSLATOR_VERSION，
# C# 在构建时读取同一个 VERSION 文件。这里不一致说明两个二进制
# 不是由同一源码树构建，也会使 config\godot_preflight.ini 不同步，
# 因此应断言一致，不能通过归一化掩盖差异。
$uiScenarios = @(
    @{ Name = 'ui/1200x780-offline'; W = 1200; H = 780;  Alive = 0 },
    @{ Name = 'ui/1200x780-online';  W = 1200; H = 780;  Alive = 1 },
    @{ Name = 'ui/1080x700-min';     W = 1080; H = 700;  Alive = 1 },
    @{ Name = 'ui/1600x1000-wide';   W = 1600; H = 1000; Alive = 0 },
    @{ Name = 'ui/1064x661-client'; W = 1064; H = 661; Alive = 0 },
    @{ Name = 'ui/motion-quarter'; W = 1200; H = 780; Alive = 0; Tick = 7500 },
    @{ Name = 'ui/motion-half'; W = 1200; H = 780; Alive = 0; Tick = 15000 },
    @{ Name = 'ui/motion-end'; W = 1200; H = 780; Alive = 0; Tick = 29999 }

)
$savedUiTick = $env:DST_UI_PROBE_TICK
try {
foreach ($s in $uiScenarios) {
    $tick = 0; if ($s.ContainsKey('Tick')) { $tick = $s.Tick }
    $env:DST_UI_PROBE_TICK = [string]$tick
    $bmpC = Join-Path $uiRoot ('c_{0}x{1}_{2}_{3}.bmp' -f $s.W, $s.H, $s.Alive, $tick)
    $bmpCs = Join-Path $uiRoot ('cs_{0}x{1}_{2}_{3}.bmp' -f $s.W, $s.H, $s.Alive, $tick)
    $c = Invoke-Detect $uiExe @('--ui-probe-and-exit', "$($s.W)", "$($s.H)", "$($s.Alive)", $bmpC)
    $cs = Invoke-Detect $CsLauncher @('--ui-probe', "$($s.W)", "$($s.H)", "$($s.Alive)", $bmpCs, '--root', $uiRoot)

    $problems = New-Object System.Collections.Generic.List[string]
    if ($c.Code -ne 0 -or $cs.Code -ne 0) { $problems.Add("exit codes differ or non-zero: C=$($c.Code) C#=$($cs.Code)") }
    if ($c.Out -ne $cs.Out) { $problems.Add("probe report differs`n--- C ---`n$($c.Out)--- C# ---`n$($cs.Out)") }
    if ($c.Out -notmatch '(?m)^result=1$') { $problems.Add("C probe did not render: $($c.Out)$($c.Err)") }
    if ($c.Out -notmatch "(?m)^client=$($s.W)\|$($s.H)$") { $problems.Add('client size is not the requested one') }
    if ($c.Out -notmatch '(?m)^dpi=96$') { $problems.Add('probe did not pin the DPI to 96') }
    if ($c.Out -notmatch '(?m)^alive=' + $s.Alive + '$') { $problems.Add('server state was not applied') }
    # 除两版一致性之外，单独约束控件完整性与真实最小窗口边界。
    $controls = [regex]::Matches($c.Out, '(?m)^ctl=[^|]+\|[0-9]+\|(-?\d+),(-?\d+),(-?\d+),(-?\d+)\|')
    if ($controls.Count -ne 16) { $problems.Add('expected all 16 functional controls') }
    foreach ($control in $controls) {
        $left = [int]$control.Groups[1].Value; $top = [int]$control.Groups[2].Value
        $right = [int]$control.Groups[3].Value; $bottom = [int]$control.Groups[4].Value
        if ($control.Value.StartsWith('ctl=log|') -and $bottom -gt ($s.H - 50)) { $problems.Add('log extends beyond the card inner bottom') }
        if ($left -lt 0 -or $top -lt 0 -or $right -gt $s.W -or $bottom -gt $s.H -or $right -le $left -or $bottom -le $top) {
            $problems.Add('control clipped or empty: ' + $control.Value)
        }
    }
    if (-not (Test-Path -LiteralPath $bmpC) -or -not (Test-Path -LiteralPath $bmpCs)) {
        $problems.Add('one of the launchers wrote no bitmap')
    } else {
        # 使用与 Snapshot-Tree 相同的 SHA-256 实现，不触发命令模块自动加载。
        $hashC = [BitConverter]::ToString($sha.ComputeHash([IO.File]::ReadAllBytes($bmpC)))
        $hashCs = [BitConverter]::ToString($sha.ComputeHash([IO.File]::ReadAllBytes($bmpCs)))
        $expected = 54 + $s.W * $s.H * 4
        if ((Get-Item -LiteralPath $bmpC).Length -ne $expected) { $problems.Add('C bitmap is not the expected 32bpp size') }
        if ($tick -gt 0) {
            $baseBmp = Join-Path $uiRoot 'c_1200x780_0_0.bmp'
            $baseBytes = [IO.File]::ReadAllBytes($baseBmp)
            $phaseBytes = [IO.File]::ReadAllBytes($bmpC)
            $changes = 0
            for ($pixel = 0; $pixel -lt ($s.W * $s.H); $pixel++) {
                $offset = 54 + $pixel * 4
                if ($baseBytes[$offset] -eq $phaseBytes[$offset] -and $baseBytes[$offset+1] -eq $phaseBytes[$offset+1] -and $baseBytes[$offset+2] -eq $phaseBytes[$offset+2]) { continue }
                $changes++
                $px = $pixel % $s.W; $py = [int][math]::Floor($pixel / $s.W)
                # 探针写入自顶向下的 BMP；这里只允许星空、金线和状态点变化。
                $allowed = ($px -ge 620 -and $px -lt 1160 -and $py -ge 75 -and $py -lt 240) -or ($px -ge 36 -and $px -lt 252 -and $py -ge 230 -and $py -lt 235) -or ($px -ge 1002 -and $px -lt 1024 -and $py -ge 27 -and $py -lt 47)
                if (-not $allowed) { $problems.Add("animation touched static content at $px,$py"); break }
            }
            if ($changes -eq 0) { $problems.Add('animation phase did not change decoration') }
        }
        if ($hashC -ne $hashCs) {
            # 定位首个不同的像素，让回归报告指出具体屏幕位置
            $ba = [System.IO.File]::ReadAllBytes($bmpC); $bb = [System.IO.File]::ReadAllBytes($bmpCs)
            $where = 'sizes differ'
            if ($ba.Length -eq $bb.Length) {
                for ($i = 54; $i -lt $ba.Length; $i++) {
                    if ($ba[$i] -ne $bb[$i]) {
                        $p = [math]::Floor(($i - 54) / 4)
                        $where = 'first differs at x={0} y={1}' -f ($p % $s.W), [math]::Floor($p / $s.W)
                        break
                    }
                }
            }
            $problems.Add("rendered client area differs ($where)`n  C  $hashC`n  C# $hashCs")
        }
    }
    if ($problems.Count -eq 0) {
        $pass++
        Write-Host ("  ok  {0,-52} {1} controls, pixel-identical" -f $s.Name, ([regex]::Matches($c.Out, '(?m)^ctl=')).Count)
    } else {
        $fail++
        $failures.Add("$($s.Name): " + ($problems -join "`n  "))
    }
}

} finally { $env:DST_UI_PROBE_TICK = $savedUiTick }

# 运行时页脚标签是两个窗口有意保留的差异；玩家副标题保持一致，
# 它们表明绘制窗口的实现类型。探针将这两处替换为中性内容，
# 使其他像素可比；真实值通过专门的诊断断言验证，
# 不能在归一化时静默忽略。
$idC = Invoke-Detect $uiExe @('--ui-identity-and-exit')
$idCs = Invoke-Detect $CsLauncher @('--ui-identity')
$idProblems = New-Object System.Collections.Generic.List[string]
if ($idC.Out -notmatch '(?m)^runtime_tag=C native runtime$') { $idProblems.Add("C rail tag: $($idC.Out)") }
if ($idC.Out -notmatch '(?m)^subtitle=选择一个游戏，让熟悉的语言带你继续探索。$') { $idProblems.Add("C subtitle: $($idC.Out)") }
if ($idCs.Out -notmatch '(?m)^runtime_tag=C# managed runtime$') { $idProblems.Add("C# rail tag: $($idCs.Out)") }
if ($idCs.Out -notmatch '(?m)^subtitle=选择一个游戏，让熟悉的语言带你继续探索。$') { $idProblems.Add("C# subtitle: $($idCs.Out)") }
if ($idC.Out -eq $idCs.Out) { $idProblems.Add('both binaries claim the same runtime identity') }
if ($idProblems.Count -eq 0) {
    $pass++; Write-Host ("  ok  {0,-52} declared divergence, neutral in the pixel diff" -f 'ui/runtime-identity')
} else {
    $fail++; $failures.Add('ui/runtime-identity: ' + ($idProblems -join "`n  "))
}

# 进程内 HDROP 到达真实主窗口/子窗口的 WndProc，不启动服务或游戏；
# 验证资源管理器拖放目标的注册及自动启动决策，
# 原有 UI 探针路径不得写入游戏或启动器配置。
$dropDir = Join-Path $root 'drop_probe_游戏'
Mkdir $dropDir
Touch (Join-Path $dropDir 'Game.exe')
Touch (Join-Path $dropDir 'notes.txt')
$dropBefore = Snapshot-Tree $dropDir
$dropCases = @(
    @{ Name = 'exe-on-path'; Path = (Join-Path $dropDir 'Game.exe'); Target = 'path'; Selected = $dropDir; Start = 1 },
    @{ Name = 'exe-on-button'; Path = (Join-Path $dropDir 'Game.exe'); Target = 'start'; Selected = $dropDir; Start = 1 },
    @{ Name = 'directory-on-main'; Path = $dropDir; Target = 'main'; Selected = $dropDir; Start = 1 },
    @{ Name = 'invalid-on-log'; Path = (Join-Path $dropDir 'notes.txt'); Target = 'log'; Selected = ''; Start = 0 }
)
foreach ($case in $dropCases) {
    $c = Invoke-Detect $uiExe @('--ui-drop-probe-and-exit', $case.Path, $case.Target)
    $cs = Invoke-Detect $CsLauncher @('--ui-drop-probe', $case.Path, $case.Target)
    $expected = "accept=1`nselected=$($case.Selected)`nstart=$($case.Start)`nresult=1`n"
    $label = 'ui/drop-' + $case.Name
    if ($c.Code -ne 0 -or $cs.Code -ne 0 -or $c.Out -ne $cs.Out -or
        $c.Out -ne $expected -or (Snapshot-Tree $dropDir) -ne $dropBefore) {
        $fail++; $failures.Add("[$label] drop probe mismatch or wrote game files`nexpected=$expected`nC=$($c.Out)$($c.Err)`nC#=$($cs.Out)$($cs.Err)")
    } else {
        $pass++; Write-Host ("  ok  {0,-52} selected={1} start={2}" -f $label, $case.Selected, $case.Start)
    }
}

Stop-FakeServer

# 实际预热 HTTP 与 XUnity 回填守卫；上面的 Godot 假服务器已释放端口。
$warmupHttpReport = & powershell -NoProfile -ExecutionPolicy Bypass -File "$PSScriptRoot\run_warmup_http_tests.ps1" -CsLauncher $CsLauncher 2>&1
$warmupHttpCode = $LASTEXITCODE
$warmupHttpReport | ForEach-Object { Write-Host $_ }
if ($warmupHttpCode -ne 0) {
    $fail++; $failures.Add("Unity warmup HTTP regression failed (exit $warmupHttpCode)")
} elseif (($warmupHttpReport -join "`n") -notmatch '\[skip\]') {
    $pass++
}

if ($ServerSmoke) {
    $busy = Get-NetTCPConnection -LocalPort 19999 -State Listen -ErrorAction SilentlyContinue
    if ($busy) {
        $fail++; $failures.Add("server smoke: port 19999 already has a listener (pid $($busy.OwningProcess)); refusing to stop it")
    } else {
        $s = Invoke-Detect $CsLauncher @('--server-smoke', '--root', $repo)
        $lingering = Get-Process dst_server, dst_server_cs -ErrorAction SilentlyContinue
        if ($s.Code -eq 0 -and $s.Out -match '(?m)^started=1$' -and $s.Out -match '(?m)^alive_after_stop=0$' -and -not $lingering) {
            $pass++; Write-Host "  ok  server smoke (start -> /health -> stop, no lingering process)"
        } else {
            $fail++
            $failures.Add("server smoke failed (exit $($s.Code), lingering=$([bool]$lingering))`n$($s.Out)$($s.Err)")
        }
    }
}

if (-not $KeepFixtures) {
    Remove-Item -LiteralPath $root -Recurse -Force -ErrorAction SilentlyContinue
    # 只有本脚本创建的临时 dotnet 输出目录才属于本次测试
    if ($csBuildDir) { Remove-Item -LiteralPath $csBuildDir -Recurse -Force -ErrorAction SilentlyContinue }
} else { Write-Host "fixtures kept at $root" }

Write-Host ""
$skipNote = if ($godotSkipped -gt 0) { ", $godotSkipped skipped ($godotSkip)" } else { '' }
Write-Host "launcher parity: $pass passed, $fail failed$skipNote"
foreach ($f in $failures) { Write-Host "FAIL $f" }
if ($fail -gt 0) { exit 1 }
exit 0
