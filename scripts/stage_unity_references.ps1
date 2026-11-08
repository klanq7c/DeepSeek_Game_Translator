param([Parameter(Mandatory = $true)][string]$RepoRoot)

$ErrorActionPreference = 'Stop'
$root = [IO.Path]::GetFullPath($RepoRoot)
$sourceDirectory = Join-Path $root 'payloads\UnityTranslator\src\bin\Release\net472'
$cacheRoot = Join-Path $root 'build\unity-managed-refs'
$utf8 = [Text.UTF8Encoding]::new($false)

function Assert-PlainPath([string]$Path) {
    # 缓存只能写入实际构建目录；拒绝重解析点，避免沿链接写入外部依赖。
    $cursor = [IO.Path]::GetFullPath($Path)
    while ($cursor) {
        if (Test-Path -LiteralPath $cursor) {
            if ((Get-Item -LiteralPath $cursor -Force).Attributes -band [IO.FileAttributes]::ReparsePoint) {
                throw "Unity reference path is a reparse point: $cursor"
            }
        }
        $cursor = [IO.Path]::GetDirectoryName($cursor)
    }
}

function Get-Hash([byte[]]$Bytes) {
    $sha = [Security.Cryptography.SHA256]::Create()
    try { return ([BitConverter]::ToString($sha.ComputeHash($Bytes))).Replace('-', '') }
    finally { $sha.Dispose() }
}

function Get-Inventory([string]$Directory, [switch]$Snapshot) {
    Assert-PlainPath $Directory
    if (-not [IO.Directory]::Exists($Directory)) { throw "Unity reference input is not a directory: $Directory" }
    $files = @{}
    foreach ($file in Get-ChildItem -LiteralPath $Directory -Force) {
        $reference = $file.Name -like 'UnityEngine*.dll' -or $file.Name -eq 'Unity.TextMeshPro.dll'
        if ($Snapshot -and -not $reference -and $file.Name -ne 'references.sha256') {
            throw "Unexpected entry in Unity reference snapshot: $($file.FullName)"
        }
        if (-not $reference) { continue }
        if ($file.PSIsContainer -or ($file.Attributes -band [IO.FileAttributes]::ReparsePoint)) {
            throw "Unity reference is not a regular file: $($file.FullName)"
        }
        $stream = [IO.File]::OpenRead($file.FullName)
        $sha = [Security.Cryptography.SHA256]::Create()
        try {
            $length = $stream.Length
            $hash = ([BitConverter]::ToString($sha.ComputeHash($stream))).Replace('-', '')
        } finally { $sha.Dispose(); $stream.Dispose() }
        $files[$file.Name] = [pscustomobject]@{ Name = $file.Name; Path = $file.FullName; Length = $length; Hash = $hash }
    }
    if (-not $files.ContainsKey('UnityEngine.CoreModule.dll')) {
        throw "Unity reference snapshot is missing UnityEngine.CoreModule.dll: $Directory"
    }
    [string[]]$names = @($files.Keys)
    [Array]::Sort($names, [StringComparer]::Ordinal)
    $manifest = [Text.StringBuilder]::new("dst-unity-managed-refs-v1`n")
    $entries = @(foreach ($name in $names) {
        $file = $files[$name]
        [void]$manifest.Append($name).Append("`t").Append($file.Length.ToString([Globalization.CultureInfo]::InvariantCulture)).Append("`t").Append($file.Hash).Append("`n")
        $file
    })
    $text = $manifest.ToString()
    return [pscustomobject]@{ Entries = $entries; Manifest = $text; Key = (Get-Hash $utf8.GetBytes($text)) }
}

function Assert-Snapshot([string]$Directory, [string]$Key) {
    $inventory = Get-Inventory $Directory -Snapshot
    $manifestPath = Join-Path $Directory 'references.sha256'
    Assert-PlainPath $manifestPath
    if ($inventory.Key -cne $Key -or -not [IO.File]::Exists($manifestPath) -or
        [IO.File]::ReadAllText($manifestPath, $utf8) -cne $inventory.Manifest) {
        throw "Unity reference snapshot failed content verification: $Directory"
    }
}

function Write-NewFile([string]$Path, [string]$Text) {
    $stream = [IO.File]::Open($Path, [IO.FileMode]::CreateNew, [IO.FileAccess]::Write, [IO.FileShare]::None)
    try {
        $bytes = $utf8.GetBytes($Text)
        $stream.Write($bytes, 0, $bytes.Length)
        $stream.Flush($true)
    } finally { $stream.Dispose() }
}

Assert-PlainPath $cacheRoot
[void][IO.Directory]::CreateDirectory($cacheRoot)
$lockPath = Join-Path $cacheRoot 'stage.lock'
Assert-PlainPath $lockPath
# 同一引用库只允许一个发布者；竞争或文件系统错误直接报告，不靠延迟重试掩盖。
$cacheLock = [IO.File]::Open($lockPath, [IO.FileMode]::OpenOrCreate, [IO.FileAccess]::ReadWrite, [IO.FileShare]::None)
try {
    $currentPath = Join-Path $cacheRoot 'current.txt'
    Assert-PlainPath $currentPath
    if ([IO.Directory]::Exists($currentPath)) { throw "Invalid Unity reference cache pointer: $currentPath" }
    $current = ''
    if ([IO.File]::Exists($currentPath)) {
        $current = [IO.File]::ReadAllText($currentPath, $utf8).Trim()
        if ($current -cnotmatch '^[0-9A-F]{64}$') { throw "Invalid Unity reference cache pointer: $currentPath" }
        Assert-Snapshot (Join-Path $cacheRoot $current) $current
    }
    # 输出被清理可以复用 current；链接、错误类型和损坏的旧版本不能伪装成清理。
    Assert-PlainPath $sourceDirectory
    if ([IO.File]::Exists($sourceDirectory)) { throw "Unity reference input is not a directory: $sourceDirectory" }
    $corePath = Join-Path $sourceDirectory 'UnityEngine.CoreModule.dll'
    Assert-PlainPath $corePath
    if ([IO.Directory]::Exists($corePath)) { throw "Unity reference is not a regular file: $corePath" }
    if ([IO.File]::Exists($corePath)) {
        $inventory = Get-Inventory $sourceDirectory
        $key = $inventory.Key
        $snapshot = Join-Path $cacheRoot $key
        if (Test-Path -LiteralPath $snapshot) {
            Assert-Snapshot $snapshot $key
        } else {
            # 先复制并校验独占临时目录，成功后才发布。失败目录保留诊断，不修改旧快照。
            $pending = Join-Path $cacheRoot ('.pending-' + [Guid]::NewGuid().ToString('N'))
            [void][IO.Directory]::CreateDirectory($pending)
            foreach ($file in $inventory.Entries) {
                [IO.File]::Copy($file.Path, (Join-Path $pending $file.Name), $false)
            }
            Write-NewFile (Join-Path $pending 'references.sha256') $inventory.Manifest
            Assert-Snapshot $pending $key
            if ((Get-Inventory $sourceDirectory).Key -cne $key) {
                throw "Unity reference inputs changed during staging; retained $pending"
            }
            [IO.Directory]::Move($pending, $snapshot)
        }
        if ($current -cne $key) {
            $temporary = Join-Path $cacheRoot ('.current-' + [Guid]::NewGuid().ToString('N'))
            Write-NewFile $temporary ($key + "`n")
            try {
                # PowerShell 5.1 将 $null 绑定为空字符串；此处必须传入真正的 null。
                if ([IO.File]::Exists($currentPath)) { [IO.File]::Replace($temporary, $currentPath, [NullString]::Value) }
                else { [IO.File]::Move($temporary, $currentPath) }
            } catch {
                $cause = $_.Exception
                while ($cause.InnerException) { $cause = $cause.InnerException }
                $errorCode = '0x{0:X8}' -f $cause.HResult
                throw "Unity reference pointer publish failed: target=$currentPath temporary=$temporary type=$($cause.GetType().FullName) HResult=$errorCode error=$($cause.Message)"
            }
        }
    } elseif ($current) {
        # 非增量构建可能已清空输出目录，只有已完整校验的上一份引用可用于重建。
        $key = $current
        Assert-Snapshot (Join-Path $cacheRoot $key) $key
    } else {
        throw "No output references or verified Unity reference cache at $cacheRoot"
    }
    # 批处理只接收 ASCII 内容键；路径仍由 ROOT 构造，避免中文路径被控制台转码。
    Write-Output $key
} finally { $cacheLock.Dispose() }
