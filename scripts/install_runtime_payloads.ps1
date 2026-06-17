param(
    [switch]$UnityMono5,
    [switch]$UnityMono6,
    [switch]$UnityIL2CPP,
    [switch]$Newtonsoft,
    [switch]$All,
    [switch]$Force
)

$ErrorActionPreference = "Stop"

if (-not $PSScriptRoot) {
    $ScriptRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
} else {
    $ScriptRoot = $PSScriptRoot
}

$Root = Split-Path -Parent $ScriptRoot
$DownloadCache = Join-Path $Root ".downloads\runtime-payloads"

if ($All) {
    $UnityMono5 = $true
    $UnityMono6 = $true
    $UnityIL2CPP = $true
}

if ($UnityMono5 -or $UnityMono6) {
    $Newtonsoft = $true
}

function Show-Usage {
    Write-Host "Usage:"
    Write-Host "  powershell -ExecutionPolicy Bypass -File scripts\install_runtime_payloads.ps1 -All"
    Write-Host "  powershell -ExecutionPolicy Bypass -File scripts\install_runtime_payloads.ps1 -UnityMono5"
    Write-Host "  powershell -ExecutionPolicy Bypass -File scripts\install_runtime_payloads.ps1 -UnityMono6"
    Write-Host "  powershell -ExecutionPolicy Bypass -File scripts\install_runtime_payloads.ps1 -UnityIL2CPP"
    Write-Host ""
    Write-Host "Options can be combined. Add -Force to redownload and replace runtime payloads."
}

if (-not ($UnityMono5 -or $UnityMono6 -or $UnityIL2CPP -or $Newtonsoft)) {
    Show-Usage
    exit 0
}

function Get-FullPath([string]$Path) {
    return [System.IO.Path]::GetFullPath($Path)
}

function Assert-UnderRoot([string]$Path) {
    $rootFull = Get-FullPath $Root
    $pathFull = Get-FullPath $Path
    if (-not $pathFull.StartsWith($rootFull, [System.StringComparison]::OrdinalIgnoreCase)) {
        throw "Refusing to write outside the program directory: $Path"
    }
}

function Get-FileSha256([string]$Path) {
    return (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant()
}

function Get-DownloadFile([string]$Name, [string]$Url, [string]$Sha256 = "") {
    New-Item -ItemType Directory -Force -Path $DownloadCache | Out-Null
    Assert-UnderRoot $DownloadCache

    $dst = Join-Path $DownloadCache $Name
    if ($Force -and (Test-Path -LiteralPath $dst)) {
        Remove-Item -LiteralPath $dst -Force
    }

    if (-not (Test-Path -LiteralPath $dst)) {
        Write-Host "Downloading $Name"
        Write-Host "  $Url"
        Invoke-WebRequest -UseBasicParsing -Uri $Url -OutFile $dst
    } else {
        Write-Host "Using cached $Name"
    }

    if ($Sha256) {
        $actual = Get-FileSha256 $dst
        if ($actual -ne $Sha256.ToLowerInvariant()) {
            Remove-Item -LiteralPath $dst -Force -ErrorAction SilentlyContinue
            throw "SHA256 mismatch for $Name. Expected $Sha256, got $actual."
        }
    }

    return $dst
}

function Expand-PayloadZip([string]$ZipPath, [string]$Destination, [switch]$ClearFirst) {
    Assert-UnderRoot $Destination
    if ($ClearFirst -and (Test-Path -LiteralPath $Destination)) {
        Remove-Item -LiteralPath $Destination -Recurse -Force
    }
    New-Item -ItemType Directory -Force -Path $Destination | Out-Null
    Expand-Archive -LiteralPath $ZipPath -DestinationPath $Destination -Force
}

function Install-BepInEx5Mono {
    $zip = Get-DownloadFile `
        "BepInEx_win_x64_5.4.23.5.zip" `
        "https://github.com/BepInEx/BepInEx/releases/download/v5.4.23.5/BepInEx_win_x64_5.4.23.5.zip" `
        "82f9878551030f54657792c0740d9d51a09500eeae1fba21106b0c441e6732c4"

    $dst = Join-Path $Root "payloads\UnityMonoRuntime"
    Expand-PayloadZip $zip $dst -ClearFirst
    Write-Host "Installed: Unity Mono / BepInEx 5 payload"
}

function Install-BepInEx6Mono {
    $zip = Get-DownloadFile `
        "BepInEx-Unity.Mono-win-x64-6.0.0-be.755+3fab71a.zip" `
        "https://builds.bepinex.dev/projects/bepinex_be/755/BepInEx-Unity.Mono-win-x64-6.0.0-be.755+3fab71a.zip"

    $dst = Join-Path $Root "payloads\UnityMonoRuntime6"
    Expand-PayloadZip $zip $dst -ClearFirst
    Write-Host "Installed: Unity Mono / BepInEx 6 payload"
}

function Install-XUnityIL2CPP {
    $bepZip = Get-DownloadFile `
        "BepInEx-Unity.IL2CPP-win-x64-6.0.0-be.755+3fab71a.zip" `
        "https://builds.bepinex.dev/projects/bepinex_be/755/BepInEx-Unity.IL2CPP-win-x64-6.0.0-be.755+3fab71a.zip"

    $xunityZip = Get-DownloadFile `
        "XUnity.AutoTranslator-BepInEx-IL2CPP-5.6.1.zip" `
        "https://github.com/bbepis/XUnity.AutoTranslator/releases/download/v5.6.1/XUnity.AutoTranslator-BepInEx-IL2CPP-5.6.1.zip" `
        "9d6b26e9d4957459bdb64b6d4852edb39cd5e8d31c28e0a157cefd6510ada811"

    $redirectorZip = Get-DownloadFile `
        "XUnity.ResourceRedirector-BepInEx-IL2CPP-2.1.0.zip" `
        "https://github.com/bbepis/XUnity.AutoTranslator/releases/download/v5.6.1/XUnity.ResourceRedirector-BepInEx-IL2CPP-2.1.0.zip" `
        "4c41901736e6f1ff78a3fb786bbacb003af960d5459bc3880d5019371317226c"

    $runtimeDst = Join-Path $Root "payloads\UnityIL2CPP\BepInExRuntime"
    $xunityDst = Join-Path $Root "payloads\UnityIL2CPP\XUnityAutoTranslator"

    Expand-PayloadZip $bepZip $runtimeDst -ClearFirst
    Expand-PayloadZip $xunityZip $xunityDst -ClearFirst
    Expand-PayloadZip $redirectorZip $xunityDst
    Write-Host "Installed: Unity IL2CPP / BepInEx 6 + XUnity payload"
}

function Install-NewtonsoftJson {
    $url = "https://www.nuget.org/api/v2/package/Newtonsoft.Json/13.0.4"
    $pkg = Get-DownloadFile "Newtonsoft.Json.13.0.4.nupkg" $url
    $tmp = Join-Path $DownloadCache ("newtonsoft_" + [Guid]::NewGuid().ToString("N"))
    New-Item -ItemType Directory -Force -Path $tmp | Out-Null
    Assert-UnderRoot $tmp

    try {
        Expand-Archive -LiteralPath $pkg -DestinationPath $tmp -Force
        $dll = Join-Path $tmp "lib\net45\Newtonsoft.Json.dll"
        if (-not (Test-Path -LiteralPath $dll)) {
            throw "Newtonsoft.Json.dll not found in downloaded package."
        }

        $dst = Join-Path $Root "payloads\UnityTranslator\Newtonsoft.Json.dll"
        Assert-UnderRoot $dst
        New-Item -ItemType Directory -Force -Path (Split-Path -Parent $dst) | Out-Null
        Copy-Item -LiteralPath $dll -Destination $dst -Force
        Write-Host "Installed: Newtonsoft.Json.dll"
    } finally {
        Remove-Item -LiteralPath $tmp -Recurse -Force -ErrorAction SilentlyContinue
    }
}

function Write-DownloadedNotice {
    $notice = Join-Path $Root "payloads\THIRD_PARTY_DOWNLOADED.md"
    Assert-UnderRoot $notice
    New-Item -ItemType Directory -Force -Path (Split-Path -Parent $notice) | Out-Null
    @"
# Downloaded runtime payloads

These files were downloaded on this machine by scripts/install_runtime_payloads.ps1.
They are not part of the DS Translator source release.

- BepInEx: https://github.com/BepInEx/BepInEx and https://builds.bepinex.dev/projects/bepinex_be
- XUnity.AutoTranslator: https://github.com/bbepis/XUnity.AutoTranslator
- Newtonsoft.Json: https://github.com/JamesNK/Newtonsoft.Json

Do not commit downloaded payloads, Unity/game assemblies, user caches, logs, fonts, or real config files.
"@ | Set-Content -LiteralPath $notice -Encoding UTF8
}

if ($UnityMono5) { Install-BepInEx5Mono }
if ($UnityMono6) { Install-BepInEx6Mono }
if ($UnityIL2CPP) { Install-XUnityIL2CPP }
if ($Newtonsoft) { Install-NewtonsoftJson }

Write-DownloadedNotice

Write-Host ""
Write-Host "Runtime payload installation complete. You can now run ds translator and deploy to a game folder."
