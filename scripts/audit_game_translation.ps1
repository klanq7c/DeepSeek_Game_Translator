<#
.SYNOPSIS
  以只读方式报告单个游戏的翻译准备情况。

.DESCRIPTION
  使用启动器已有的检测及预热转储模式，不启动游戏或服务器，
  不部署钩子、不修改存档，也不向游戏目录写入文件。
  缓存覆盖仅表示：扫描到的源字符串在本地 TSV 或已有 XUnity
  翻译文件中具有不同于原文的译文；不评价行文质量、字体覆盖或实际渲染。
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$GamePath,
    [string]$LauncherPath = '',
    [string]$CachePath = '',
    [ValidateRange(0, 100)][int]$MaxExamples = 10,
    [switch]$AsJson
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$repo = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
if (-not $LauncherPath) {
    $LauncherPath = Join-Path $repo 'ds游戏翻译器.exe'
}
$launcher = (Get-Item -LiteralPath $LauncherPath -ErrorAction Stop).FullName
$coreAssemblyPath = Join-Path (Split-Path -Parent $launcher) 'native\dst_server_cs.exe'
if (-not (Test-Path -LiteralPath $coreAssemblyPath -PathType Leaf)) {
    throw "Shared cache rules are unavailable: $coreAssemblyPath"
}
$coreAssembly = [Reflection.Assembly]::LoadFrom($coreAssemblyPath)
$cacheCodecType = $coreAssembly.GetType('DstCore.CacheCodec', $true)
$normalizeMethod = $cacheCodecType.GetMethod('NormalizeForStore')
if (-not $normalizeMethod) { throw "Shared cache rules are missing NormalizeForStore: $coreAssemblyPath" }
$normalizeForStore = [Delegate]::CreateDelegate([Func[string,string,string]], $normalizeMethod)
$boundedType = $coreAssembly.GetType('DstServerCs.BoundedCacheLineReader', $true)
$boundedCtor = $boundedType.GetConstructor(
    [Reflection.BindingFlags]'Instance,Public,NonPublic', $null,
    [type[]]@([IO.TextReader], [int]), $null)
$boundedReadLine = $boundedType.GetMethod('ReadLine', [Reflection.BindingFlags]'Instance,Public,NonPublic')
if (-not $boundedCtor -or -not $boundedReadLine) {
    throw "Shared bounded cache reader is unavailable: $coreAssemblyPath"
}
if (-not $CachePath) {
    $CachePath = Join-Path (Split-Path -Parent $launcher) 'translation_memory_c.tsv'
}
$game = (Get-Item -LiteralPath $GamePath -ErrorAction Stop)
if (-not $game.PSIsContainer) { throw "GamePath is not a directory: $GamePath" }
$gameDir = $game.FullName

function Quote-WindowsArg([string]$value) {
    $builder = New-Object Text.StringBuilder
    [void]$builder.Append('"')
    $slashes = 0
    foreach ($ch in $value.ToCharArray()) {
        if ($ch -eq '\') { $slashes++; continue }
        if ($ch -eq '"') {
            [void]$builder.Append('\', $slashes * 2 + 1).Append('"')
            $slashes = 0
            continue
        }
        if ($slashes) { [void]$builder.Append('\', $slashes); $slashes = 0 }
        [void]$builder.Append($ch)
    }
    if ($slashes) { [void]$builder.Append('\', $slashes * 2) }
    [void]$builder.Append('"')
    $builder.ToString()
}

function Invoke-LauncherReadOnly([string]$mode) {
    # WinExe 不给 PowerShell 5.1 设置 LASTEXITCODE；直接等待进程并读 UTF-8 管道。
    $start = New-Object Diagnostics.ProcessStartInfo
    $start.FileName = $launcher
    $start.Arguments = (Quote-WindowsArg $mode) + ' ' + (Quote-WindowsArg $gameDir)
    $start.UseShellExecute = $false
    $start.CreateNoWindow = $true
    $start.RedirectStandardOutput = $true
    $start.RedirectStandardError = $true
    $start.StandardOutputEncoding = [Text.Encoding]::UTF8
    $start.StandardErrorEncoding = [Text.Encoding]::UTF8
    $process = [Diagnostics.Process]::Start($start)
    if (-not $process) { throw "Could not start launcher diagnostic: $launcher" }
    try {
        $stderrTask = $process.StandardError.ReadToEndAsync()
        $deadline = [DateTime]::UtcNow.AddMinutes(5)
        while ($true) {
            $remaining = [int][Math]::Max(0, ($deadline - [DateTime]::UtcNow).TotalMilliseconds)
            if ($remaining -eq 0) { throw "$mode timed out after 5 minutes." }
            $lineTask = $process.StandardOutput.ReadLineAsync()
            if (-not $lineTask.Wait($remaining)) { throw "$mode timed out after 5 minutes." }
            $line = $lineTask.Result
            if ($null -eq $line) { break }
            if ($line.Length) { $line }
        }
        $remaining = [int][Math]::Max(0, ($deadline - [DateTime]::UtcNow).TotalMilliseconds)
        if (-not $process.WaitForExit($remaining)) { throw "$mode timed out after 5 minutes." }
        $errorText = $stderrTask.Result
        if ($process.ExitCode -ne 0) {
            throw "$mode failed (exit $($process.ExitCode)): $errorText"
        }
    } finally {
        try {
            if (-not $process.HasExited) {
                $process.Kill()
                [void]$process.WaitForExit(5000)
            }
        } catch {
            Write-Warning "Could not stop diagnostic process $($process.Id): $($_.Exception.Message)"
        }
        $process.Dispose()
    }
}

$detected = @{}
foreach ($line in @(Invoke-LauncherReadOnly '--detect-and-exit')) {
    $split = $line.IndexOf('=')
    if ($split -gt 0) { $detected[$line.Substring(0, $split)] = $line.Substring($split + 1) }
}
if (-not $detected.ContainsKey('engine') -or -not $detected.ContainsKey('exe')) {
    throw 'Launcher detection report is incomplete.'
}
$exe = if ($detected['exe'] -eq '-') { $null } else { $detected['exe'] }
if ($exe -and -not (Test-Path -LiteralPath $exe -PathType Leaf)) {
    throw "Launcher reported a game exe that no longer exists: $exe"
}

$sources = New-Object 'System.Collections.Generic.HashSet[string]' ([StringComparer]::Ordinal)
$resourceTranslations = New-Object 'System.Collections.Generic.Dictionary[string,string]' ([StringComparer]::Ordinal)
$scanLogs = New-Object 'System.Collections.Generic.List[string]'
$postBatches = 0
$scanResult = $false
Invoke-LauncherReadOnly '--warmup-and-exit' | ForEach-Object {
    $line = $_
    if ($line -eq 'result=0') {
        $scanResult = $true
    } elseif ($line.StartsWith('log=', [StringComparison]::Ordinal)) {
        $message = $line.Substring(4)
        # 转储模式把本应 POST 的批次打印出来，启动器的“后台排队”日志
        # 在这里仅是模拟计数，不能作为真实入队或翻译完成的证据。
        if ($message -notmatch '预热翻译缓存|preheated translation cache') { $scanLogs.Add($message) }
    } elseif ($line.StartsWith('post=', [StringComparison]::Ordinal)) {
        $split = $line.IndexOf(' ', 5)
        if ($split -lt 0) { throw "Malformed warmup dump: $line" }
        $endpoint = $line.Substring(5, $split - 5)
        $body = $line.Substring($split + 1) | ConvertFrom-Json
        $postBatches++
        if ($endpoint -eq '/prefetch') {
            foreach ($item in $body.texts) {
                if ($item -is [string] -and $item.Length -gt 0) {
                    [void]$sources.Add($item)
                }
            }
        } elseif ($endpoint -eq '/cache/import') {
            foreach ($entry in $body.entries) {
                if ($entry.key -isnot [string] -or $entry.value -isnot [string] -or
                    $entry.key.Length -eq 0 -or $entry.value.Length -eq 0) { continue }
                [void]$sources.Add($entry.key)
                $clean = $normalizeForStore.Invoke($entry.key, $entry.value)
                if ($null -ne $clean) { $resourceTranslations[$entry.key] = $clean }
            }
        } else {
            throw "Unexpected warmup endpoint in diagnostic dump: $endpoint"
        }
    }
}
if (-not $scanResult) { throw 'Launcher warmup dump ended without result=0.' }

# 只保留本游戏扫描到的键，避免把全局翻译记忆读进内存。重复键按服务端
# cache_load 的规则由最后一条有效记录覆盖；损坏记录计数但不视为已翻译。
$cached = New-Object 'System.Collections.Generic.Dictionary[string,string]' ([StringComparer]::Ordinal)
$invalidCacheRows = 0
$rejectedCandidateRows = 0
$cacheExists = Test-Path -LiteralPath $CachePath -PathType Leaf
if ($cacheExists -and $sources.Count -gt 0) {
    $utf8 = New-Object System.Text.UTF8Encoding($false, $true)
    $share = [IO.FileShare]::ReadWrite -bor [IO.FileShare]::Delete
    $stream = [IO.File]::Open($CachePath, [IO.FileMode]::Open, [IO.FileAccess]::Read, $share)
    try {
        $reader = New-Object IO.StreamReader($stream, [Text.Encoding]::ASCII, $false, 65536, $true)
        try {
            # 服务端的限长读取器逐块丢弃超长损坏行，不先分配整行。
            $ctorArgs = [object[]]::new(2)
            $ctorArgs[0] = [IO.TextReader]$reader
            $ctorArgs[1] = 8 * 1024 * 1024
            $boundedReader = $boundedCtor.Invoke($ctorArgs)
            $readArgs = [object[]]::new(1)
            while ($true) {
                $readArgs[0] = $false
                $row = $boundedReadLine.Invoke($boundedReader, $readArgs)
                if ($null -eq $row) { break }
                if ($readArgs[0]) { $invalidCacheRows++; continue }
                $tab = $row.IndexOf("`t")
                if ($tab -lt 1 -or $tab -eq $row.Length - 1) { $invalidCacheRows++; continue }
                try {
                    $key = $utf8.GetString([Convert]::FromBase64String($row.Substring(0, $tab)))
                    if (-not $sources.Contains($key)) { continue }
                    $value = $utf8.GetString([Convert]::FromBase64String($row.Substring($tab + 1)))
                    $clean = $normalizeForStore.Invoke($key, $value)
                    if ($null -ne $clean) { $cached[$key] = $clean }
                    else { $rejectedCandidateRows++ }
                } catch [FormatException] {
                    $invalidCacheRows++
                } catch [Text.DecoderFallbackException] {
                    $invalidCacheRows++
                }
            }
        } finally { $reader.Dispose() }
    } finally { $stream.Dispose() }
}

# 这里只检查可确定的变量、控制码和常见富文本标记。语义、换行与
# 字体字形覆盖仍需在真实画面中检查；疑点不自动改写共享缓存。
$markerPattern = '(?i)\\(?:[A-Za-z]{1,8}(?:\[[^\]\r\n]{0,80}\])?|[{}$.|!><^\\])|%[0-9]+|(?<!\\)\{[A-Za-z_][^{}\r\n]{0,80}\}|\[/?(?:b|i|u|s|color|font|size|url|center|right|left|wave|shake|outline|img)\b[^\]\r\n]*\]|</?(?:b|i|u|s|color|font|size|sprite|link)\b[^>\r\n]*>'
$rpgmControlPattern = '(?i)(?:<WordWrap>|<br\s*\/?>|\\(?:c|i|fs|fr|fb|fi|oc|ow|v|n[1-5]?|nc|nr|nd[1-5]?|ndc|ndr|nt[1-5]?|ntc|ntr)(?:\[[^\]]{0,64}\]|<[^>\n]{0,64}>)|\\(?:fr|fb|fi)|\\[{}$.|!><^])'
$genericMarkerPattern = '(?i)%[0-9]+|(?<!\\)\{[A-Za-z_][^{}\r\n]{0,80}\}|\[/?(?:b|i|u|s|color|font|size|url|center|right|left|wave|shake|outline|img)\b[^\]\r\n]*\]|</?(?:b|i|u|s|color|font|size|sprite|link)\b[^>\r\n]*>'
function Get-MarkerSignature([string]$value, [bool]$ignoreTiming = $false) {
    if ($detected['engine_id'] -eq '2') {
        # RPG Maker 的控制码按顺序生效，大小写不区分；与运行时钩子的
        # rpgmControlTokens 保持同一识别范围，补充通用占位符单独比较。
        $controls = @([regex]::Matches($value, $rpgmControlPattern) | ForEach-Object { $_.Value.ToLowerInvariant() } |
            Where-Object { -not $ignoreTiming -or $_ -cnotin @('\.', '\|', '\!', '\^', '\>', '\<') })
        $generic = @([regex]::Matches($value, $genericMarkerPattern) | ForEach-Object { $_.Value })
        [Array]::Sort($generic, [StringComparer]::Ordinal)
        return 'controls:' + [string]::Join([char]0x1f, $controls) + '|generic:' + [string]::Join([char]0x1f, $generic)
    }
    $markers = @([regex]::Matches($value, $markerPattern) | ForEach-Object { $_.Value })
    [Array]::Sort($markers, [StringComparer]::Ordinal)
    [string]::Join([char]0x1f, $markers)
}

$fromResource = 0
$fromCache = 0
$missing = 0
$markerMismatch = 0
$structuralMismatch = 0
$timingOnlyMismatch = 0
$missingExamples = New-Object 'System.Collections.Generic.List[string]'
$structuralExamples = New-Object 'System.Collections.Generic.List[object]'
$timingExamples = New-Object 'System.Collections.Generic.List[object]'
$sortedSources = @($sources)
[Array]::Sort($sortedSources, [StringComparer]::Ordinal)
foreach ($source in $sortedSources) {
    $translation = $null
    if ($resourceTranslations.TryGetValue($source, [ref]$translation)) {
        $fromResource++
    } elseif ($cached.TryGetValue($source, [ref]$translation)) {
        $fromCache++
    } else {
        $missing++
        if ($missingExamples.Count -lt $MaxExamples) { $missingExamples.Add($source) }
        continue
    }
    if ((Get-MarkerSignature $source) -cne (Get-MarkerSignature $translation)) {
        $markerMismatch++
        if ((Get-MarkerSignature $source $true) -cne (Get-MarkerSignature $translation $true)) {
            $structuralMismatch++
            if ($structuralExamples.Count -lt $MaxExamples) {
                $structuralExamples.Add([ordered]@{ source = $source; translation = $translation })
            }
        } else {
            $timingOnlyMismatch++
            if ($timingExamples.Count -lt $MaxExamples) {
                $timingExamples.Add([ordered]@{ source = $source; translation = $translation })
            }
        }
    }
}
$markerExamples = New-Object 'System.Collections.Generic.List[object]'
foreach ($example in $structuralExamples) { if ($markerExamples.Count -lt $MaxExamples) { $markerExamples.Add($example) } }
foreach ($example in $timingExamples) { if ($markerExamples.Count -lt $MaxExamples) { $markerExamples.Add($example) } }
$covered = $fromResource + $fromCache
$coveragePercent = $null
if ($sources.Count -gt 0) { $coveragePercent = [Math]::Round(100.0 * $covered / $sources.Count, 1) }
$scanLimit = $null
$scanLimitReached = $false
foreach ($message in $scanLogs) {
    if ($message -match '^预热扫描达到 ([0-9]+) 条上限') {
        $scanLimit = [int]::Parse($Matches[1], [Globalization.CultureInfo]::InvariantCulture)
        $scanLimitReached = $true
        break
    }
}
$scope = '仅统计启动器预热扫描可见的文本；不代表全游戏文本或实际显示覆盖率'
if ($scanLimitReached) { $scope += '；本次达到预热采集上限，后续资源文本可能未进入统计' }
$report = [ordered]@{
    game_dir = $gameDir
    engine = $detected['engine']
    original_game_exe_path = $exe
    exe_launch = '未实测'
    scanned_unique_texts = $sources.Count
    scan_limit = $scanLimit
    scan_limit_reached = $scanLimitReached
    resource_translation = $fromResource
    cache_translation = $fromCache
    missing_translation = $missing
    cache_coverage_percent = $coveragePercent
    marker_mismatch_suspects = $markerMismatch
    structural_marker_mismatch_suspects = $structuralMismatch
    timing_control_mismatch_suspects = $timingOnlyMismatch
    missing_examples = $missingExamples.ToArray()
    marker_examples = $markerExamples.ToArray()
    cache_file = $CachePath
    cache_file_exists = $cacheExists
    invalid_cache_rows_observed = $invalidCacheRows
    rejected_candidate_cache_rows = $rejectedCandidateRows
    scan_post_batches = $postBatches
    scan_logs = $scanLogs.ToArray()
    prose_review = '未人工校对'
    glyph_and_layout = '未实机验证'
    built_in_chinese_option = '未检查'
    scope = $scope
}
if ($AsJson) {
    $report | ConvertTo-Json -Depth 6
} else {
    Write-Output "游戏：$gameDir"
    Write-Output "引擎：$($report.engine)"
    Write-Output "原目录游戏 exe：$(if ($exe) { $exe } else { '未找到' })（启动未实测）"
    Write-Output "预热扫描唯一文本：$($sources.Count)；资源自带译文：$fromResource；缓存译文：$fromCache；未命中：$missing"
    Write-Output "扫描范围缓存覆盖率：$(if ($null -eq $report.cache_coverage_percent) { '无法计算' } else { "$($report.cache_coverage_percent)%" })"
    Write-Output "疑似结构标记不一致：$structuralMismatch；仅等待/速度控制码不同：$timingOnlyMismatch；异常缓存行：$invalidCacheRows；候选译文被缓存规则拒绝：$rejectedCandidateRows"
    Write-Output "译文人工校对：未完成；字体与排版：未实机验证；原 exe 启动：未实测。"
    Write-Output $report.scope
    if ($missingExamples.Count) { Write-Output "未命中示例："; $missingExamples | ForEach-Object { Write-Output "  $_" } }
    if ($markerExamples.Count) {
        Write-Output "标记疑点示例："
        $markerExamples | ForEach-Object { Write-Output "  原文：$($_.source)"; Write-Output "  译文：$($_.translation)" }
    }
    if ($scanLogs.Count) { Write-Output "扫描日志："; $scanLogs | ForEach-Object { Write-Output "  $_" } }
}
