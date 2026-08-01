param(
    [string]$Version = "",
    [switch]$NoZip
)

$ErrorActionPreference = "Stop"

$repo = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
if (-not $Version) {
    $versionFile = Join-Path $repo "VERSION"
    if (Test-Path -LiteralPath $versionFile) {
        $Version = (Get-Content -LiteralPath $versionFile -Raw).Trim()
    }
    if (-not $Version) {
        $Version = "preview"
    }
}
if ($Version -notmatch '\A[0-9A-Za-z][0-9A-Za-z._+-]{0,63}\z' -or
    $Version -eq "." -or $Version -eq "..") {
    throw "Invalid release version. Use 1-64 ASCII letters, digits, dot, underscore, plus, or hyphen."
}
$outputRoot = Join-Path $repo "build\open_source"
$stageName = "DeepSeek_Game_Translator_source_$Version"
$stage = Join-Path $outputRoot $stageName

function Assert-Under([string]$Path, [string]$Root) {
    $fullPath = [System.IO.Path]::GetFullPath($Path)
    $fullRoot = [System.IO.Path]::GetFullPath($Root).TrimEnd('\') + '\'
    if (-not $fullPath.StartsWith($fullRoot, [System.StringComparison]::OrdinalIgnoreCase)) {
        throw "Refusing to write outside expected directory: $Path"
    }
    foreach ($candidate in @([System.IO.Path]::GetFullPath($Root), $fullPath)) {
        $volumeRoot = [System.IO.Path]::GetPathRoot($candidate)
        $cursor = $volumeRoot
        foreach ($part in ($candidate.Substring($volumeRoot.Length) -split '[\\/]')) {
            if ([string]::IsNullOrWhiteSpace($part)) { continue }
            $cursor = Join-Path $cursor $part
            if (-not (Test-Path -LiteralPath $cursor)) { continue }
            $item = Get-Item -LiteralPath $cursor -Force
            if (($item.Attributes -band [System.IO.FileAttributes]::ReparsePoint) -ne 0) {
                throw "Refusing release output through a filesystem reparse point: $cursor"
            }
        }
    }
}

$resolvedOutputRoot = [System.IO.Path]::GetFullPath($outputRoot)
$resolvedBuildRoot = [System.IO.Path]::GetFullPath((Join-Path $repo "build"))
if (-not $resolvedOutputRoot.StartsWith($resolvedBuildRoot.TrimEnd('\') + '\', [System.StringComparison]::OrdinalIgnoreCase)) {
    throw "Refusing to write release outside build directory: $resolvedOutputRoot"
}
Assert-Under $stage $resolvedOutputRoot

if (Test-Path -LiteralPath $stage) {
    $resolvedStage = (Resolve-Path -LiteralPath $stage).Path
    if (-not $resolvedStage.StartsWith($resolvedOutputRoot.TrimEnd('\') + '\', [System.StringComparison]::OrdinalIgnoreCase)) {
        throw "Refusing to clean unexpected release directory: $resolvedStage"
    }
    Remove-Item -LiteralPath $stage -Recurse -Force
}
New-Item -ItemType Directory -Path $stage | Out-Null

$includeFiles = @(
    ".gitignore",
    "AGENTS.md",
    "CONTEXT.md",
    "CONTRIBUTING.md",
    "LICENSE",
    "README.md",
    "SECURITY.md",
    "THIRD_PARTY_NOTICES.md",
    "TRADEMARKS.md",
    "VERSION",
    "OPEN_SOURCE_RELEASE.md",
    "build_native.bat",
    "start_server.bat",
    "config\api.ini.example",
    "config\launcher.ini.example"
)

$includeDirs = @(
    "assets",
    "native\src",
    "payloads\UnityTranslator\src",
    "payloads\UnityIL2CPP\DeepSeekXUnityTranslator\src",
    "payloads\UnityIL2CPP\DeepSeekTMPFontFallback\src",
    "tests",
    "scripts",
    "docs"
)

$excludedPathParts = @(
    "\bin\",
    "\obj\",
    "\UnityManagedRefs\",
    "\UnityInteropRefs\",
    "\_launcher_fixtures\"
)

$excludedExtensions = @(
    ".dll", ".exe", ".pdb", ".mdb", ".otf", ".ttf", ".ttc", ".woff", ".woff2",
    ".zip", ".7z", ".rar", ".tsv", ".log"
)

$binaryAssetExtensions = @(".ico", ".png")

function Is-SafeSourceFile([System.IO.FileInfo]$file) {
    $relative = $file.FullName.Substring($repo.Length).TrimStart("\", "/")
    foreach ($part in $excludedPathParts) {
        if ($file.FullName.IndexOf($part, [System.StringComparison]::OrdinalIgnoreCase) -ge 0) {
            return $false
        }
    }
    if ($excludedExtensions -contains $file.Extension.ToLowerInvariant()) {
        return $false
    }
    if ($relative -match '(^|\\)(api|launcher)\.ini$') {
        return $false
    }
    if ($relative -like "translation_memory*") {
        return $false
    }
    return $true
}

function Copy-RepoFile([string]$relativePath) {
    $src = Join-Path $repo $relativePath
    if (-not (Test-Path -LiteralPath $src)) {
        throw "Missing release input: $relativePath"
    }
    $dst = Join-Path $stage $relativePath
    $dstDir = Split-Path -Parent $dst
    if (-not (Test-Path -LiteralPath $dstDir)) {
        New-Item -ItemType Directory -Path $dstDir | Out-Null
    }
    Copy-Item -LiteralPath $src -Destination $dst -Force
}

foreach ($file in $includeFiles) {
    Copy-RepoFile $file
}

foreach ($dir in $includeDirs) {
    $srcDir = Join-Path $repo $dir
    if (-not (Test-Path -LiteralPath $srcDir)) {
        throw "Missing release input directory: $dir"
    }
    Get-ChildItem -LiteralPath $srcDir -Recurse -File | ForEach-Object {
        if (Is-SafeSourceFile $_) {
            $relative = $_.FullName.Substring($repo.Length).TrimStart("\", "/")
            Copy-RepoFile $relative
        }
    }
}

$forbiddenFiles = Get-ChildItem -LiteralPath $stage -Recurse -File | Where-Object {
    $excludedExtensions -contains $_.Extension.ToLowerInvariant()
}
if ($forbiddenFiles) {
    $list = ($forbiddenFiles | Select-Object -First 20 | ForEach-Object { $_.FullName }) -join "`n"
    throw "Release contains forbidden binary/cache files:`n$list"
}

$secretHits = @()
$scanPatterns = @(
    "sk-[A-Za-z0-9]{16,}",
    "(?i)api[_-]?key\s*=\s*[^<\s][^\r\n]+",
    "(?i)password\s*=\s*[^<\s][^\r\n]+",
    "(?i)secret\s*=\s*[^<\s][^\r\n]+",
    "(?i)Authorization:\s*Bearer\s+[A-Za-z0-9._-]+",
    "C:\\Users\\[A-Za-z0-9._-]+",
    "E:\\Projects\\[^\\\r\n]+",
    "/Users/[A-Za-z0-9._-]+",
    "/home/[A-Za-z0-9._-]+"
)

foreach ($file in Get-ChildItem -LiteralPath $stage -Recurse -File) {
    if ($binaryAssetExtensions -contains $file.Extension.ToLowerInvariant()) {
        continue
    }
    try {
        $text = Get-Content -LiteralPath $file.FullName -Raw -ErrorAction Stop
    } catch {
        $secretHits += "$($file.FullName): unreadable or non-text file"
        continue
    }
    foreach ($pattern in $scanPatterns) {
        if ($text -match $pattern) {
            $secretHits += "$($file.FullName): $pattern"
            break
        }
    }
}
if ($secretHits.Count -gt 0) {
    throw "Release contains possible secrets or local paths:`n$($secretHits -join "`n")"
}

& (Join-Path $PSScriptRoot "audit_open_source_release.ps1") -Path $stage

if (-not $NoZip) {
    $zipPath = Join-Path $outputRoot "$stageName.zip"
    Assert-Under $zipPath $resolvedOutputRoot
    if (Test-Path -LiteralPath $zipPath) {
        Remove-Item -LiteralPath $zipPath -Force
    }
    Compress-Archive -LiteralPath $stage -DestinationPath $zipPath -Force
    Write-Host "Created source archive: $zipPath"
}

Write-Host "Created source release directory: $stage"
