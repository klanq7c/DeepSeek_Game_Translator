<#
.SYNOPSIS
  以只读方式报告单个游戏的翻译准备情况。

.DESCRIPTION
  使用启动器已有的检测及预热转储模式，不启动游戏或服务器，
  不部署钩子、不修改存档，也不向游戏目录写入文件。
  缓存覆盖仅表示：扫描到的源字符串在本地 TSV 或已有 XUnity
  翻译文件中具有不同于原文的译文；不评价行文质量、字体覆盖或实际渲染。
  可用 DetailsPath 将扫描到的全量唯一键及复核原因写入新的 JSONL 文件；
  目标父目录须已存在且不含重解析点，目标须位于游戏目录外，不覆盖已有文件。
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$GamePath,
    [string]$LauncherPath = '',
    [string]$CachePath = '',
    [ValidateRange(0, 100)][int]$MaxExamples = 10,
    [string]$DetailsPath = '',
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

function Assert-AuditPlainDirectory([string]$directory) {
    # 导出只接受普通目录；逐级拒绝重解析点，避免路径别名绕过游戏目录边界。
    $current = Get-Item -LiteralPath $directory -Force -ErrorAction Stop
    if (-not $current.PSIsContainer) { throw "Audit output parent is not a directory: $directory" }
    while ($null -ne $current) {
        if (($current.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
            throw "Audit details require a path without reparse points: $($current.FullName)"
        }
        $current = $current.Parent
    }
}

if ($DetailsPath) {
    $DetailsPath = $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($DetailsPath)
    $DetailsPath = [IO.Path]::GetFullPath($DetailsPath)
    if (-not [IO.Path]::GetExtension($DetailsPath).Equals('.jsonl', [StringComparison]::OrdinalIgnoreCase)) {
        throw 'DetailsPath must name a new .jsonl file.'
    }
    $gamePrefix = $gameDir.TrimEnd('\', '/') + [IO.Path]::DirectorySeparatorChar
    if ($DetailsPath.StartsWith($gamePrefix, [StringComparison]::OrdinalIgnoreCase)) {
        throw 'Audit details must be outside the game directory.'
    }
    if (Test-Path -LiteralPath $DetailsPath) { throw "Audit details already exist; choose a new file: $DetailsPath" }
    Assert-AuditPlainDirectory $gameDir
    Assert-AuditPlainDirectory ([IO.Path]::GetDirectoryName($DetailsPath))
}

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
$unityTagBoundary = $null
$unityTagRegex = $null
if ($detected['engine_id'] -in @('3', '4')) {
    # 与显示层源链接同一已知标签表和完整属性解析器；缺失时明确失败，不能以空签名通过。
    $unityTagType = $coreAssembly.GetType('DstUnity.TmpRichText', $true)
    $unityTagBoundary = $unityTagType.GetMethod('TryGetTagEnd')
    $unityTagPattern = $unityTagType.GetField('KnownTagPattern').GetRawConstantValue()
    $unityTagRegex = [regex]::new($unityTagPattern, [Text.RegularExpressions.RegexOptions]::IgnoreCase)
}
if ($exe -and -not (Test-Path -LiteralPath $exe -PathType Leaf)) {
    throw "Launcher reported a game exe that no longer exists: $exe"
}

$sources = New-Object 'System.Collections.Generic.HashSet[string]' ([StringComparer]::Ordinal)
$resourceTranslations = New-Object 'System.Collections.Generic.Dictionary[string,string]' ([StringComparer]::Ordinal)
$sourceOrigins = New-Object 'System.Collections.Generic.Dictionary[string,int]' ([StringComparer]::Ordinal)
$rejectedResource = New-Object 'System.Collections.Generic.Dictionary[string,int]' ([StringComparer]::Ordinal)
function Add-AuditSource([string]$text, [int]$origin) {
    [void]$sources.Add($text)
    $flags = 0
    [void]$sourceOrigins.TryGetValue($text, [ref]$flags)
    $sourceOrigins[$text] = $flags -bor $origin
}
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
                    Add-AuditSource $item 1
                }
            }
        } elseif ($endpoint -eq '/cache/import') {
            foreach ($entry in $body.entries) {
                if ($entry.key -isnot [string] -or $entry.value -isnot [string] -or
                    $entry.key.Length -eq 0 -or $entry.value.Length -eq 0) { continue }
                Add-AuditSource $entry.key 2
                $clean = $normalizeForStore.Invoke($entry.key, $entry.value)
                if ($null -ne $clean) { $resourceTranslations[$entry.key] = $clean }
                else {
                    $count = 0
                    [void]$rejectedResource.TryGetValue($entry.key, [ref]$count)
                    $rejectedResource[$entry.key] = $count + 1
                }
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
$rejectedCache = New-Object 'System.Collections.Generic.Dictionary[string,int]' ([StringComparer]::Ordinal)
$invalidCacheCandidates = New-Object 'System.Collections.Generic.Dictionary[string,int]' ([StringComparer]::Ordinal)
function Add-InvalidCacheCandidate([string]$key) {
    if ($null -eq $key -or -not $sources.Contains($key)) { return }
    $count = 0
    [void]$invalidCacheCandidates.TryGetValue($key, [ref]$count)
    $invalidCacheCandidates[$key] = $count + 1
}
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
                $key = $null
                try {
                    $key = $utf8.GetString([Convert]::FromBase64String($row.Substring(0, $tab)))
                    if (-not $sources.Contains($key)) { continue }
                    $value = $utf8.GetString([Convert]::FromBase64String($row.Substring($tab + 1)))
                    $clean = $normalizeForStore.Invoke($key, $value)
                    if ($null -ne $clean) { $cached[$key] = $clean }
                    else {
                        $rejectedCandidateRows++
                        $count = 0
                        [void]$rejectedCache.TryGetValue($key, [ref]$count)
                        $rejectedCache[$key] = $count + 1
                    }
                } catch [FormatException] {
                    $invalidCacheRows++
                    Add-InvalidCacheCandidate $key
                } catch [Text.DecoderFallbackException] {
                    $invalidCacheRows++
                    Add-InvalidCacheCandidate $key
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
function Find-RenPyMarkerEnd([string]$value, [int]$start, [char]$open, [char]$close, [int[]]$ends) {
    if ($ends[$start] -ne 0) { return $ends[$start] }
    $pending = New-Object 'System.Collections.Generic.Stack[int]'
    $pending.Push($start)
    for ($scan = $start + 1; $scan -lt $value.Length; $scan++) {
        if ($value[$scan] -eq $open) { $pending.Push($scan) }
        elseif ($value[$scan] -eq $close) {
            $ends[$pending.Pop()] = $scan
            if ($pending.Count -eq 0) { return $scan }
        }
    }
    # 保留嵌套边界及未闭合结论，后续恢复扫描不再反复遍历同一段正文。
    while ($pending.Count) { $ends[$pending.Pop()] = -1 }
    return -1
}

function Get-RenPyMarkerSignature([string]$value) {
    $tokens = New-Object Text.StringBuilder
    $ends = [int[]]::new($value.Length)
    # 固定起点并原子匹配数字/标志，避免损坏缓存中的长零串触发反复回溯。
    $percentSuffix = [regex]'\G(?>[#0 +\-]*)(?>[0-9]+|\*)?(?:\.(?>[0-9]+|\*))?[hlL]?[diouxXeEfFgGcrsa]'
    $cursor = 0
    while ($cursor -lt $value.Length) {
        $start = $cursor
        $open = $value[$cursor]
        if ($open -eq '[' -or $open -eq '{') {
            # Ren'Py 使用双开括号显示字面量；其中的可见文字允许被翻译。
            if ($cursor + 1 -lt $value.Length -and $value[$cursor + 1] -eq $open) { $cursor += 2; continue }
            $close = if ($open -eq '[') { ']' } else { '}' }
            $end = Find-RenPyMarkerEnd $value $start $open $close $ends
            if ($end -lt 0) {
                # 坏片段只比较结构种类；继续检查其中仍可识别的独立变量。
                $token = 'unclosed:' + $open
                $cursor = $start + 1
            } else {
                $token = $value.Substring($start, $end - $start + 1)
                $cursor = $end + 1
            }
            [void]$tokens.Append($token.Length).Append(':').Append($token)
            continue
        }
        if ($open -eq '%' -and $cursor + 1 -lt $value.Length) {
            if ($value[$cursor + 1] -eq '%') { $cursor += 2; continue }
            if ($value[$cursor + 1] -eq '(') {
                # 旧式映射键可以为空，也可以包含平衡括号；完整保留格式后缀。
                $end = Find-RenPyMarkerEnd $value ($start + 1) '(' ')' $ends
                if ($end -ge 0) {
                    $suffix = $percentSuffix.Match($value, $end + 1)
                    if ($suffix.Success) {
                        $cursor = $suffix.Index + $suffix.Length
                        $token = $value.Substring($start, $cursor - $start)
                        [void]$tokens.Append($token.Length).Append(':').Append($token)
                        continue
                    }
                }
                # 无合法格式后缀的 %(说明) 可是普通百分比文本，不能据此判错；
                # 回到字面扫描，继续检查其内部的 []/{} 或其他完整格式项。
            }
        }
        $cursor++
    }
    # 顺序也是结构的一部分：不能将错误嵌套或调换变量排序后视为相同。
    return $tokens.ToString()
}

function Get-UnityMarkerSignature([string]$value) {
    $tags = New-Object Text.StringBuilder
    $plain = New-Object Text.StringBuilder
    $at = 0
    $noParse = $false
    $noParseEnd = [regex]::new('(?i)<\s*/\s*noparse\s*>')
    while ($at -lt $value.Length) {
        if ($noParse) {
            # noparse 内的引号与尖括号只是正文；不能用普通标签边界吞掉结束标签。
            $closing = $noParseEnd.Match($value, $at)
            if (-not $closing.Success) { [void]$plain.Append($value, $at, $value.Length - $at); break }
            [void]$plain.Append($value, $at, $closing.Index - $at)
            [void]$tags.Append($closing.Length).Append(':').Append($closing.Value)
            $at = $closing.Index + $closing.Length
            $noParse = $false
            continue
        }
        $match = $unityTagRegex.Match($value, $at)
        if (-not $match.Success) { [void]$plain.Append($value, $at, $value.Length - $at); break }
        [void]$plain.Append($value, $at, $match.Index - $at)
        $args = [object[]]@($value, $match.Index, $match.Index)
        $complete = $unityTagBoundary.Invoke($null, $args)
        $length = if ($complete) { [int]$args[2] - $match.Index + 1 } else { $match.Length }
        $token = $value.Substring($match.Index, $length)
        [void]$tags.Append($token.Length).Append(':').Append($token)
        if ($token -match '^<\s*noparse\s*>$') { $noParse = $true }
        $at = $match.Index + $length
    }
    # 标签属性不参与正文占位符比较；格式参数允许正文调整次序，标签顺序则不可排序。
    # 双花括号是字面量，数字字段保留对齐和格式说明；不执行 String.Format。
    $pattern = '(?i)\\(?:[A-Za-z]{1,8}(?:\[[^\]\r\n]{0,80}\])?|[{}$.|!><^\\])|%[0-9]+|\{(?:[A-Za-z_][^{}\r\n]{0,80}|[0-9]+(?:,\s*-?[0-9]+)?(?::[^{}\r\n]*)?)\}|\[/?(?:b|i|u|s|color|font|size|url|center|right|left|wave|shake|outline|img)\b[^\]\r\n]*\]'
    $fieldRegex = [regex]::new('\G(?:' + $pattern + ')')
    $body = $plain.ToString()
    $fields = @(for ($cursor = 0; $cursor -lt $body.Length;) {
        if ($cursor + 1 -lt $body.Length -and
            (($body[$cursor] -eq '{' -and $body[$cursor + 1] -eq '{') -or
             ($body[$cursor] -eq '}' -and $body[$cursor + 1] -eq '}'))) { $cursor += 2; continue }
        $field = $fieldRegex.Match($body, $cursor)
        if ($field.Success -and $field.Index -eq $cursor) { $field.Value; $cursor += $field.Length }
        else { $cursor++ }
    })
    [Array]::Sort($fields, [StringComparer]::Ordinal)
    [void]$tags.Append('|fields:')
    foreach ($field in $fields) { [void]$tags.Append($field.Length).Append(':').Append($field) }
    return $tags.ToString()
}

function Get-GodotMarkerSignature([string]$value) {
    # 与 godot_next_bbcode 的识别边界一致：名称完整、区分大小写，闭合标签、
    # 带参数或成对自定义效果参与比较；普通方括号正文仍允许翻译。
    $known = @('b', 'i', 'u', 's', 'code', 'url', 'img', 'font', 'font_size',
        'color', 'bgcolor', 'fgcolor', 'outline_size', 'outline_color', 'center', 'left', 'right',
        'fill', 'indent', 'table', 'cell', 'ul', 'ol', 'li', 'lb', 'rb', 'br', 'p', 'lang',
        'opentype_features', 'dropcap', 'paragraph', 'tab', 'hr', 'fade', 'shake', 'wave',
        'tornado', 'rainbow', 'pulse')
    $tags = New-Object Text.StringBuilder
    $plain = New-Object Text.StringBuilder
    $cursor = 0
    while ($cursor -lt $value.Length) {
        $start = $value.IndexOf('[', $cursor)
        if ($start -lt 0) { [void]$plain.Append($value, $cursor, $value.Length - $cursor); break }
        [void]$plain.Append($value, $cursor, $start - $cursor)
        $end = $value.IndexOf(']', $start + 1)
        if ($end -lt 0) { [void]$plain.Append($value, $start, $value.Length - $start); break }
        $body = $value.Substring($start + 1, $end - $start - 1)
        $match = [regex]::Match($body, '^(/?)([A-Za-z][A-Za-z0-9_]{0,62})([^A-Za-z0-9_].*)?$', [Text.RegularExpressions.RegexOptions]::Singleline)
        $isTag = $false
        if ($match.Success) {
            $name = $match.Groups[2].Value
            $tail = $match.Groups[3].Value
            $isTag = $match.Groups[1].Length -gt 0 -or $tail.Contains('=')
            if ($tail.Length -eq 0) {
                $isTag = $isTag -or $name -cin $known -or
                    $value.IndexOf('[/' + $name + ']', $end + 1, [StringComparison]::Ordinal) -ge 0
            }
        }
        $length = $end - $start + 1
        if ($isTag) { [void]$tags.Append($length).Append(':').Append($value, $start, $length) }
        else { [void]$plain.Append($value, $start, $length) }
        $cursor = $end + 1
    }
    # 只比较已知格式令牌，不执行 GDScript 格式化；正文参数允许调整次序，
    # 标签序列和属性保持原样。标签属性中的 % 或花括号不成为正文变量。
    $pattern = '%s|%d|%1\.2f|%%|%[0-9]+|(?<!\\)\{[A-Za-z_][^{}\r\n]{0,80}\}'
    $fields = @([regex]::Matches($plain.ToString(), $pattern) | ForEach-Object { $_.Value })
    [Array]::Sort($fields, [StringComparer]::Ordinal)
    [void]$tags.Append('|fields:')
    foreach ($field in $fields) { [void]$tags.Append($field.Length).Append(':').Append($field) }
    $balance = 0
    $negative = $false
    foreach ($ch in $value.ToCharArray()) {
        if ($ch -eq '[') { $balance++ }
        elseif ($ch -eq ']') { $balance--; if ($balance -lt 0) { $negative = $true } }
    }
    [void]$tags.Append('|brackets:').Append($balance).Append(':').Append($negative)
    return $tags.ToString()
}

function Get-MarkerSignature([string]$value, [bool]$ignoreTiming = $false) {
    if ($detected['engine_id'] -eq '1') { return Get-RenPyMarkerSignature $value }
    if ($detected['engine_id'] -in @('3', '4')) { return Get-UnityMarkerSignature $value }
    if ($detected['engine_id'] -eq '6') { return Get-GodotMarkerSignature $value }
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
$detailsWriter = $null
$detailsTemp = $null
$detailsTempOwned = $false
$detailsEntries = 0
try {
    if ($DetailsPath) {
        # 同目录独占临时文件逐行写入，完成后才发布；失败不能留下貌似完整的清单。
        $detailsTemp = Join-Path ([IO.Path]::GetDirectoryName($DetailsPath)) ('.dst-audit-' + [Guid]::NewGuid().ToString('N') + '.tmp')
        $detailsStream = [IO.File]::Open($detailsTemp, [IO.FileMode]::CreateNew, [IO.FileAccess]::Write, [IO.FileShare]::None)
        $detailsTempOwned = $true
        try { $detailsWriter = New-Object IO.StreamWriter($detailsStream, (New-Object Text.UTF8Encoding($false))) }
        catch { $detailsStream.Dispose(); throw }
    }
    foreach ($source in $sortedSources) {
        $translation = $null
        $translationSource = 'none'
        $markerReview = 'not_checked'
        if ($resourceTranslations.TryGetValue($source, [ref]$translation)) {
            $fromResource++
            $translationSource = 'resource'
        } elseif ($cached.TryGetValue($source, [ref]$translation)) {
            $fromCache++
            $translationSource = 'cache'
        } else {
            $missing++
            if ($missingExamples.Count -lt $MaxExamples) { $missingExamples.Add($source) }
        }
        if ($translationSource -ne 'none') {
            $markerReview = 'none'
            if ((Get-MarkerSignature $source) -cne (Get-MarkerSignature $translation)) {
                $markerMismatch++
                if ($detected['engine_id'] -ne '2' -or
                    (Get-MarkerSignature $source $true) -cne (Get-MarkerSignature $translation $true)) {
                    $structuralMismatch++
                    $markerReview = 'structural'
                    if ($structuralExamples.Count -lt $MaxExamples) {
                        $structuralExamples.Add([ordered]@{ source = $source; translation = $translation })
                    }
                } else {
                    $timingOnlyMismatch++
                    $markerReview = 'timing'
                    if ($timingExamples.Count -lt $MaxExamples) {
                        $timingExamples.Add([ordered]@{ source = $source; translation = $translation })
                    }
                }
            }
        }
        if ($null -ne $detailsWriter) {
            $discovery = @()
            if (($sourceOrigins[$source] -band 1) -ne 0) { $discovery += 'launcher_prefetch' }
            if (($sourceOrigins[$source] -band 2) -ne 0) { $discovery += 'resource_import' }
            $badCache = 0; $badResource = 0; $invalidCache = 0
            [void]$rejectedCache.TryGetValue($source, [ref]$badCache)
            [void]$rejectedResource.TryGetValue($source, [ref]$badResource)
            [void]$invalidCacheCandidates.TryGetValue($source, [ref]$invalidCache)
            $issues = @()
            if ($translationSource -eq 'none') {
                $issues += 'missing_translation'
                if ($badCache -gt 0) { $issues += 'rejected_cache_candidate' }
                if ($badResource -gt 0) { $issues += 'rejected_resource_candidate' }
                if ($invalidCache -gt 0) { $issues += 'invalid_cache_candidate' }
            }
            if ($markerReview -eq 'structural') { $issues += 'structural_marker_mismatch' }
            if ($markerReview -eq 'timing') { $issues += 'timing_control_mismatch' }
            $entry = [ordered]@{
                schema_version = 1
                engine = $detected['engine']
                source = $source
                translation = $translation
                discovery = $discovery
                translation_source = $translationSource
                marker_review = $markerReview
                rejected_cache_candidates = $badCache
                rejected_resource_candidates = $badResource
                invalid_cache_candidates = $invalidCache
                issues = $issues
            }
            $detailsWriter.WriteLine(($entry | ConvertTo-Json -Depth 6 -Compress))
            $detailsEntries++
        }
    }
    if ($null -ne $detailsWriter) {
        $detailsWriter.Flush()
        $detailsWriter.Dispose()
        $detailsWriter = $null
        # 不覆盖目标；并发创建同名文件时明确失败，保留对方已有文件。
        Assert-AuditPlainDirectory ([IO.Path]::GetDirectoryName($DetailsPath))
        [IO.File]::Move($detailsTemp, $DetailsPath)
        $detailsTemp = $null
    }
} finally {
    try {
        if ($null -ne $detailsWriter) { $detailsWriter.Dispose() }
    } finally {
        if ($detailsTempOwned -and $detailsTemp -and [IO.File]::Exists($detailsTemp)) {
            try { [IO.File]::Delete($detailsTemp) }
            catch { Write-Warning "Could not remove incomplete audit details $detailsTemp`: $($_.Exception.Message)" }
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
if ($detected['engine_id'] -eq '1') { $scope += "；Ren'Py 标记仅与原文结构对照，不执行模板，也不推断旧式替换设置" }
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
    marker_consistent_translation = $covered - $markerMismatch
    followup_texts = $missing + $markerMismatch
    marker_mismatch_suspects = $markerMismatch
    structural_marker_mismatch_suspects = $structuralMismatch
    timing_control_mismatch_suspects = $timingOnlyMismatch
    missing_examples = $missingExamples.ToArray()
    marker_examples = $markerExamples.ToArray()
    details_file = $(if ($DetailsPath) { $DetailsPath } else { $null })
    details_entries = $detailsEntries
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
    Write-Output "已有译文且未发现标记差异：$($report.marker_consistent_translation)；待处理或复核：$($report.followup_texts)。不代表语义、字形或实机显示已通过。"
    if ($DetailsPath) { Write-Output "扫描全量明细：$DetailsPath（$detailsEntries 条；不受示例数量限制）" }
    Write-Output "译文人工校对：未完成；字体与排版：未实机验证；原 exe 启动：未实测。"
    Write-Output $report.scope
    if ($missingExamples.Count) { Write-Output "未命中示例："; $missingExamples | ForEach-Object { Write-Output "  $_" } }
    if ($markerExamples.Count) {
        Write-Output "标记疑点示例："
        $markerExamples | ForEach-Object { Write-Output "  原文：$($_.source)"; Write-Output "  译文：$($_.translation)" }
    }
    if ($scanLogs.Count) { Write-Output "扫描日志："; $scanLogs | ForEach-Object { Write-Output "  $_" } }
}
