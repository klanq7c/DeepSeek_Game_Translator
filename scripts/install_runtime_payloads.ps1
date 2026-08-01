param(
    [switch]$UnityMono5,
    [switch]$UnityMono6,
    [switch]$UnityMonoCorlib,
    [switch]$UnityIL2CPP,
    [switch]$Newtonsoft,
    [switch]$TmpFonts,
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
$TmpFontBundleNames = @(
    "arialuni_sdf-u55to2017",
    "arialuni_sdf_u2018",
    "arialuni_sdf_u2019",
    "arialuni_sdf_u2021",
    "arialuni_sdf_u2022",
    "arialuni_sdf_u6000"
)

if ($All) {
    $UnityMono5 = $true
    $UnityMono6 = $true
    $UnityMonoCorlib = $true
    $UnityIL2CPP = $true
    $TmpFonts = $true
}

if ($UnityMono5 -or $UnityMono6 -or $UnityIL2CPP) {
    $TmpFonts = $true
}

if ($UnityMono5 -or $UnityMono6) {
    $Newtonsoft = $true
    $UnityMonoCorlib = $true
}

function Show-Usage {
    Write-Host "Usage:"
    Write-Host "  powershell -ExecutionPolicy Bypass -File scripts\install_runtime_payloads.ps1 -All"
    Write-Host "  powershell -ExecutionPolicy Bypass -File scripts\install_runtime_payloads.ps1 -UnityMono5"
    Write-Host "  powershell -ExecutionPolicy Bypass -File scripts\install_runtime_payloads.ps1 -UnityMono6"
    Write-Host "  powershell -ExecutionPolicy Bypass -File scripts\install_runtime_payloads.ps1 -UnityMonoCorlib"
    Write-Host "  powershell -ExecutionPolicy Bypass -File scripts\install_runtime_payloads.ps1 -UnityIL2CPP"
    Write-Host "  powershell -ExecutionPolicy Bypass -File scripts\install_runtime_payloads.ps1 -TmpFonts"
    Write-Host ""
    Write-Host "Options can be combined. Add -Force to redownload and replace runtime payloads."
}

if (-not ($UnityMono5 -or $UnityMono6 -or $UnityMonoCorlib -or $UnityIL2CPP -or $Newtonsoft -or $TmpFonts)) {
    Show-Usage
    exit 0
}

function Get-FullPath([string]$Path) {
    return [System.IO.Path]::GetFullPath($Path)
}

function Assert-NoReparseTraversal([string]$Path) {
    $pathFull = Get-FullPath $Path
    $volumeRoot = [System.IO.Path]::GetPathRoot($pathFull)
    if ([string]::IsNullOrWhiteSpace($volumeRoot)) {
        throw "Refusing a path without a filesystem root: $Path"
    }
    $cursor = $volumeRoot
    $relative = $pathFull.Substring($volumeRoot.Length)
    foreach ($part in ($relative -split '[\\/]')) {
        if ([string]::IsNullOrWhiteSpace($part)) { continue }
        $cursor = Join-Path $cursor $part
        if (-not (Test-Path -LiteralPath $cursor)) { continue }
        $item = Get-Item -LiteralPath $cursor -Force
        if (($item.Attributes -band [System.IO.FileAttributes]::ReparsePoint) -ne 0) {
            throw "Refusing to write, extract, or delete through a filesystem reparse point: $cursor"
        }
    }
}

function Assert-UnderRoot([string]$Path) {
    $rootFull = (Get-FullPath $Root).TrimEnd('\') + '\'
    $pathFull = Get-FullPath $Path
    if (-not $pathFull.StartsWith($rootFull, [System.StringComparison]::OrdinalIgnoreCase)) {
        throw "Refusing to write outside the program directory: $Path"
    }
    Assert-NoReparseTraversal $Root
    Assert-NoReparseTraversal $pathFull
}

function Get-FileSha256([string]$Path) {
    return (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant()
}

function Get-DownloadFile([string]$Name, [string]$Url, [string]$Sha256 = "") {
    Assert-UnderRoot $DownloadCache
    New-Item -ItemType Directory -Force -Path $DownloadCache | Out-Null

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

function Expand-WithExternalArchiveTool([string]$ArchivePath, [string]$Destination) {
    Assert-UnderRoot $Destination
    New-Item -ItemType Directory -Force -Path $Destination | Out-Null

    $tar = Get-Command tar.exe -ErrorAction SilentlyContinue
    if (-not $tar) {
        $tar = Get-Command tar -ErrorAction SilentlyContinue
    }
    if ($tar) {
        & $tar.Source -xf $ArchivePath -C $Destination
        if ($LASTEXITCODE -eq 0) {
            return
        }
        Write-Warning "tar could not extract $ArchivePath; trying 7-Zip if available."
    }

    $sevenZip = $null
    foreach ($candidate in @("7z.exe", "7za.exe", "7zr.exe")) {
        $sevenZip = Get-Command $candidate -ErrorAction SilentlyContinue
        if ($sevenZip) { break }
    }
    if ($sevenZip) {
        & $sevenZip.Source x "-o$Destination" "-y" $ArchivePath
        if ($LASTEXITCODE -eq 0) {
            return
        }
    }

    throw "Could not extract $ArchivePath. Install Windows tar/libarchive or 7-Zip, then rerun this script."
}

function Expand-MsiAdministrativeImage([string]$MsiPath, [string]$Destination) {
    Assert-UnderRoot $Destination
    New-Item -ItemType Directory -Force -Path $Destination | Out-Null
    $arguments = @(
        "/a",
        ('"' + $MsiPath + '"'),
        "/qn",
        "/norestart",
        ('TARGETDIR="' + $Destination + '"')
    )
    $process = Start-Process -FilePath "msiexec.exe" -ArgumentList $arguments -WindowStyle Hidden -Wait -PassThru
    if ($process.ExitCode -ne 0) {
        throw "Could not extract $MsiPath with msiexec /a (exit code $($process.ExitCode))."
    }
}

$MonoCorlibAssemblyNames = @(
    "mscorlib.dll",
    "Microsoft.CSharp.dll",
    "Mono.Posix.dll",
    "Mono.Security.dll",
    "System.dll",
    "System.Configuration.dll",
    "System.Core.dll",
    "System.Data.dll",
    "System.IO.Compression.dll",
    "System.IO.Compression.FileSystem.dll",
    "System.Net.Http.dll",
    "System.Numerics.dll",
    "System.Runtime.Serialization.dll",
    "System.Xml.dll"
)

function Test-MonoCorlibInstalled {
    $destination = Join-Path $Root "payloads\UnityMonoCorlib"
    foreach ($name in $MonoCorlibAssemblyNames) {
        if (-not (Test-Path -LiteralPath (Join-Path $destination $name))) {
            return $false
        }
    }
    return Test-Path -LiteralPath (Join-Path $destination ".dst-installed-by-ds")
}

function Install-MonoCorlib {
    if ((Test-MonoCorlibInstalled) -and -not $Force) {
        Write-Host "Using existing official Mono corlib payload"
        return
    }

    $sourceUrl = "https://download.mono-project.com/archive/6.12.0/windows-installer/mono-6.12.0.206-x64-0.msi"
    $sourceSha256 = "4125f57d97cfa88257915edc969e913de198cd8e22396a29849037479a0ac368"
    $msi = Get-DownloadFile `
        "mono-6.12.0.206-x64-0.msi" `
        $sourceUrl `
        $sourceSha256
    $temporary = Join-Path $DownloadCache ("mono_corlib_" + [Guid]::NewGuid().ToString("N"))
    Assert-UnderRoot $temporary

    try {
        Expand-MsiAdministrativeImage $msi $temporary
        $source = Join-Path $temporary "Mono\lib\mono\4.5"
        foreach ($name in $MonoCorlibAssemblyNames) {
            if (-not (Test-Path -LiteralPath (Join-Path $source $name))) {
                throw "Official Mono package is missing required corlib assembly $name."
            }
        }

        $destination = Join-Path $Root "payloads\UnityMonoCorlib"
        Assert-UnderRoot $destination
        if (Test-Path -LiteralPath $destination) {
            Remove-Item -LiteralPath $destination -Recurse -Force
        }
        New-Item -ItemType Directory -Force -Path $destination | Out-Null
        foreach ($name in $MonoCorlibAssemblyNames) {
            Copy-Item -LiteralPath (Join-Path $source $name) -Destination (Join-Path $destination $name) -Force
        }
        @"
owner=ds-game-translator
component=unity-mono-corlib
version=6.12.0.206
source=$sourceUrl
sha256=$sourceSha256
license=https://www.mono-project.com/docs/faq/licensing/
"@ | Set-Content -LiteralPath (Join-Path $destination ".dst-installed-by-ds") -Encoding ASCII
        Write-Host "Installed: official Mono 6.12.0.206 corlib payload for highly stripped Unity Mono games"
    } finally {
        Remove-Item -LiteralPath $temporary -Recurse -Force -ErrorAction SilentlyContinue
    }
}

function Install-BepInEx5Mono {
    $zipX64 = Get-DownloadFile `
        "BepInEx_win_x64_5.4.23.5.zip" `
        "https://github.com/BepInEx/BepInEx/releases/download/v5.4.23.5/BepInEx_win_x64_5.4.23.5.zip" `
        "82f9878551030f54657792c0740d9d51a09500eeae1fba21106b0c441e6732c4"
    $zipX86 = Get-DownloadFile `
        "BepInEx_win_x86_5.4.23.5.zip" `
        "https://github.com/BepInEx/BepInEx/releases/download/v5.4.23.5/BepInEx_win_x86_5.4.23.5.zip" `
        "37651c79e40d6f909572a4f461ac25350bb3ef8fe7fbd29f1aa8791a33b84c82"

    $dstX64 = Join-Path $Root "payloads\UnityMonoRuntime"
    $dstX86 = Join-Path $Root "payloads\UnityMonoRuntimeX86"
    Expand-PayloadZip $zipX64 $dstX64 -ClearFirst
    Expand-PayloadZip $zipX86 $dstX86 -ClearFirst
    Write-Host "Installed: Unity Mono / BepInEx 5 payloads (x64 and x86)"
}

function Install-BepInEx6Mono {
    $zipX64 = Get-DownloadFile `
        "BepInEx-Unity.Mono-win-x64-6.0.0-be.755+3fab71a.zip" `
        "https://builds.bepinex.dev/projects/bepinex_be/755/BepInEx-Unity.Mono-win-x64-6.0.0-be.755+3fab71a.zip"
    $zipX86 = Get-DownloadFile `
        "BepInEx-Unity.Mono-win-x86-6.0.0-be.755+3fab71a.zip" `
        "https://builds.bepinex.dev/projects/bepinex_be/755/BepInEx-Unity.Mono-win-x86-6.0.0-be.755+3fab71a.zip" `
        "6864b2a54d278bb0bf538de89c334d3fd11ebccb433fc02f9f8a1fe5dc653df5"

    $dstX64 = Join-Path $Root "payloads\UnityMonoRuntime6"
    $dstX86 = Join-Path $Root "payloads\UnityMonoRuntime6X86"
    Expand-PayloadZip $zipX64 $dstX64 -ClearFirst
    Expand-PayloadZip $zipX86 $dstX86 -ClearFirst
    Write-Host "Installed: Unity Mono / BepInEx 6 payloads (x64 and x86)"
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

function Test-TmpFontAssetBundlesInstalled {
    $fontDst = Join-Path $Root "payloads\UnityIL2CPP\TMPFontAssetBundles\BepInEx\font"
    foreach ($name in $TmpFontBundleNames) {
        if (-not (Test-Path -LiteralPath (Join-Path $fontDst $name))) {
            return $false
        }
    }
    return $true
}

function Install-TmpFontAssetBundles {
    if ((Test-TmpFontAssetBundlesInstalled) -and -not $Force) {
        Write-Host "Using existing TMP font asset bundles"
        return
    }

    $archive = Get-DownloadFile `
        "TMP_Font_AssetBundles_2025-12-08.7z" `
        "https://github.com/bbepis/XUnity.AutoTranslator/releases/download/v5.5.0/TMP_Font_AssetBundles_2025-12-08.7z" `
        "889e963fb9dbd4b64927e0adf5d9060e1d0fb9d6bceb0c407d0597643e2b54ec"

    $tmp = Join-Path $DownloadCache ("tmpfonts_" + [Guid]::NewGuid().ToString("N"))
    New-Item -ItemType Directory -Force -Path $tmp | Out-Null
    Assert-UnderRoot $tmp

    try {
        Expand-WithExternalArchiveTool $archive $tmp

        foreach ($name in $TmpFontBundleNames) {
            if (-not (Test-Path -LiteralPath (Join-Path $tmp $name))) {
                throw "TMP font asset bundle archive is missing $name."
            }
        }

        $bundleRoot = Join-Path $Root "payloads\UnityIL2CPP\TMPFontAssetBundles"
        $fontDst = Join-Path $bundleRoot "BepInEx\font"
        Assert-UnderRoot $bundleRoot
        Assert-UnderRoot $fontDst

        if (Test-Path -LiteralPath $bundleRoot) {
            Remove-Item -LiteralPath $bundleRoot -Recurse -Force
        }
        New-Item -ItemType Directory -Force -Path $fontDst | Out-Null

        foreach ($name in $TmpFontBundleNames) {
            Copy-Item -LiteralPath (Join-Path $tmp $name) -Destination (Join-Path $fontDst $name) -Force
        }
        Write-Host "Installed: XUnity TMP font asset bundles"
    } finally {
        Remove-Item -LiteralPath $tmp -Recurse -Force -ErrorAction SilentlyContinue
    }
}

function Install-NewtonsoftJson {
    $url = "https://www.nuget.org/api/v2/package/Newtonsoft.Json/13.0.4"
    $pkg = Get-DownloadFile `
        "Newtonsoft.Json.13.0.4.nupkg" `
        $url `
        "f09081d457405baf35a973fa0c50d6bf272ed683f2568c5a620a49da952f6529"
    $tmp = Join-Path $DownloadCache ("newtonsoft_" + [Guid]::NewGuid().ToString("N"))
    New-Item -ItemType Directory -Force -Path $tmp | Out-Null
    Assert-UnderRoot $tmp

    try {
        $zipPkg = Join-Path $tmp "Newtonsoft.Json.13.0.4.zip"
        Copy-Item -LiteralPath $pkg -Destination $zipPkg -Force
        Expand-Archive -LiteralPath $zipPkg -DestinationPath $tmp -Force
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
- Mono corlib 6.12.0.206: https://download.mono-project.com/archive/6.12.0/windows-installer/mono-6.12.0.206-x64-0.msi
- XUnity.AutoTranslator: https://github.com/bbepis/XUnity.AutoTranslator
- XUnity TMP font asset bundles: https://github.com/bbepis/XUnity.AutoTranslator/releases/tag/v5.5.0
- Newtonsoft.Json: https://github.com/JamesNK/Newtonsoft.Json

Do not commit downloaded payloads, Unity/game assemblies, user caches, logs, fonts, or real config files.
"@ | Set-Content -LiteralPath $notice -Encoding UTF8
}

if ($UnityMono5) { Install-BepInEx5Mono }
if ($UnityMono6) { Install-BepInEx6Mono }
if ($UnityMonoCorlib) { Install-MonoCorlib }
if ($UnityIL2CPP) { Install-XUnityIL2CPP }
if ($TmpFonts) { Install-TmpFontAssetBundles }
if ($Newtonsoft) { Install-NewtonsoftJson }

Write-DownloadedNotice

Write-Host ""
Write-Host "Runtime payload installation complete. You can now run ds translator and deploy to a game folder."
