param(
    [switch]$ManagedPayloadsOnly,
    [switch]$RequireComplete
)

$ErrorActionPreference = "Stop"
$repo = Split-Path -Parent $PSScriptRoot
$errors = [System.Collections.Generic.List[string]]::new()

function Get-SourceFiles {
    param(
        [Parameter(Mandatory = $true)][string]$Root,
        [string[]]$Extensions = @(".c", ".h", ".cs", ".csproj"),
        [string[]]$ExcludedSegments = @("bin", "obj", "UnityManagedRefs", "UnityInteropRefs")
    )

    if (-not (Test-Path -LiteralPath $Root)) { return @() }
    return @(Get-ChildItem -LiteralPath $Root -File -Recurse | Where-Object {
        $file = $_
        if ($Extensions -notcontains $file.Extension) { return $false }
        foreach ($segment in $ExcludedSegments) {
            if ($file.FullName -match ("[\\/]" + [regex]::Escape($segment) + "[\\/]")) {
                return $false
            }
        }
        return $true
    })
}

function Assert-OutputFresh {
    param(
        [Parameter(Mandatory = $true)][string]$Label,
        [Parameter(Mandatory = $true)][string]$Output,
        [Parameter(Mandatory = $true)][System.IO.FileInfo[]]$Inputs
    )

    if (-not (Test-Path -LiteralPath $Output)) {
        $script:errors.Add("$Label is missing: $Output")
        return
    }
    if ($Inputs.Count -eq 0) {
        $script:errors.Add("$Label has no source inputs to verify")
        return
    }

    $outputInfo = Get-Item -LiteralPath $Output
    $newest = $Inputs | Sort-Object LastWriteTimeUtc -Descending | Select-Object -First 1
    if ($newest.LastWriteTimeUtc -gt $outputInfo.LastWriteTimeUtc) {
        $script:errors.Add(
            "$Label is stale: source '$($newest.FullName)' " +
            "($($newest.LastWriteTimeUtc.ToString('o'))) is newer than '$Output' " +
            "($($outputInfo.LastWriteTimeUtc.ToString('o')))"
        )
    }
}

function Assert-ManagedPayloadsFresh {
    $monoSourceRoot = Join-Path $repo "payloads\UnityTranslator\src"
    $monoInputs = @(Get-SourceFiles -Root $monoSourceRoot | Where-Object {
        $_.FullName -notmatch "[\\/]FontPatcher[\\/]"
    })
    Assert-OutputFresh "Unity Mono BepInEx 5 payload" `
        (Join-Path $repo "payloads\UnityTranslator\UnityTranslator.dll") $monoInputs
    Assert-OutputFresh "Unity Mono BepInEx 6 payload" `
        (Join-Path $repo "payloads\UnityTranslator\UnityTranslator.BepInEx6.dll") $monoInputs

    $endpointRoot = Join-Path $repo "payloads\UnityIL2CPP\DeepSeekXUnityTranslator\src"
    Assert-OutputFresh "Unity IL2CPP XUnity endpoint" `
        (Join-Path $repo "payloads\UnityIL2CPP\DeepSeekXUnityTranslator\DeepSeekTranslate.dll") `
        @(Get-SourceFiles -Root $endpointRoot)

    $tmpRoot = Join-Path $repo "payloads\UnityIL2CPP\DeepSeekTMPFontFallback\src"
    Assert-OutputFresh "Unity IL2CPP TMP fallback payload" `
        (Join-Path $repo "payloads\UnityIL2CPP\DeepSeekTMPFontFallback\BepInEx\plugins\DeepSeekTMPFontFallback\DeepSeekTMPFontFallback.dll") `
        @(Get-SourceFiles -Root $tmpRoot)

    $fontPatcherRoot = Join-Path $repo "payloads\UnityTranslator\src\FontPatcher"
    Assert-OutputFresh "Unity Mono stripped-font patcher" `
        (Join-Path $repo "payloads\UnityTranslator\DeepSeekUnityFontPatcher.dll") `
        @(Get-SourceFiles -Root $fontPatcherRoot)
}

function Add-ResourceReaderType {
    if ("DsArtifactResourceReader" -as [type]) { return }
    Add-Type -TypeDefinition @"
using System;
using System.Runtime.InteropServices;

public static class DsArtifactResourceReader
{
    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    public static extern IntPtr LoadLibraryExW(string fileName, IntPtr file, uint flags);

    [DllImport("kernel32.dll", SetLastError = true)]
    public static extern IntPtr FindResourceW(IntPtr module, IntPtr name, IntPtr type);

    [DllImport("kernel32.dll", SetLastError = true)]
    public static extern IntPtr LoadResource(IntPtr module, IntPtr resource);

    [DllImport("kernel32.dll", SetLastError = true)]
    public static extern uint SizeofResource(IntPtr module, IntPtr resource);

    [DllImport("kernel32.dll", SetLastError = true)]
    public static extern IntPtr LockResource(IntPtr resourceData);

    [DllImport("kernel32.dll")]
    [return: MarshalAs(UnmanagedType.Bool)]
    public static extern bool FreeLibrary(IntPtr module);
}
"@
}

function Get-EmbeddedResourceHash {
    param(
        [Parameter(Mandatory = $true)][string]$Executable,
        [Parameter(Mandatory = $true)][int]$ResourceId
    )

    Add-ResourceReaderType
    $loadLibraryAsDataFile = 0x00000002
    $loadLibraryAsImageResource = 0x00000020
    $module = [DsArtifactResourceReader]::LoadLibraryExW(
        $Executable, [IntPtr]::Zero, $loadLibraryAsDataFile -bor $loadLibraryAsImageResource)
    if ($module -eq [IntPtr]::Zero) {
        throw "LoadLibraryExW failed for '$Executable' (Win32 $([Runtime.InteropServices.Marshal]::GetLastWin32Error()))"
    }

    try {
        $resource = [DsArtifactResourceReader]::FindResourceW(
            $module, [IntPtr]$ResourceId, [IntPtr]10)
        if ($resource -eq [IntPtr]::Zero) {
            throw "resource $ResourceId is missing from '$Executable'"
        }
        $size = [DsArtifactResourceReader]::SizeofResource($module, $resource)
        $loaded = [DsArtifactResourceReader]::LoadResource($module, $resource)
        $pointer = [DsArtifactResourceReader]::LockResource($loaded)
        if ($size -eq 0 -or $loaded -eq [IntPtr]::Zero -or $pointer -eq [IntPtr]::Zero) {
            throw "resource $ResourceId could not be read from '$Executable'"
        }

        $bytes = New-Object byte[] $size
        [Runtime.InteropServices.Marshal]::Copy($pointer, $bytes, 0, [int]$size)
        $sha = [Security.Cryptography.SHA256]::Create()
        try {
            return ([BitConverter]::ToString($sha.ComputeHash($bytes))).Replace("-", "")
        } finally {
            $sha.Dispose()
        }
    } finally {
        [void][DsArtifactResourceReader]::FreeLibrary($module)
    }
}

function Assert-EmbeddedResourceMatches {
    param(
        [Parameter(Mandatory = $true)][string]$Launcher,
        [Parameter(Mandatory = $true)][int]$ResourceId,
        [Parameter(Mandatory = $true)][string]$Payload
    )

    if (-not (Test-Path -LiteralPath $Payload)) {
        $script:errors.Add("payload for resource $ResourceId is missing: $Payload")
        return
    }
    try {
        $embeddedHash = Get-EmbeddedResourceHash $Launcher $ResourceId
        $payloadHash = (Get-FileHash -LiteralPath $Payload -Algorithm SHA256).Hash
        if ($embeddedHash -ne $payloadHash) {
            $script:errors.Add(
                "launcher resource $ResourceId does not match '$Payload': " +
                "embedded=$embeddedHash payload=$payloadHash")
        }
    } catch {
        $script:errors.Add($_.Exception.Message)
    }
}

Assert-ManagedPayloadsFresh

if ($RequireComplete -or -not $ManagedPayloadsOnly) {
    $serverInputs = @(Get-SourceFiles -Root (Join-Path $repo "native\src\server"))
    $server = Join-Path $repo "native\dst_server.exe"
    Assert-OutputFresh "native translation server" $server $serverInputs

    $launcherInputs = [System.Collections.Generic.List[System.IO.FileInfo]]::new()
    foreach ($file in (Get-SourceFiles -Root (Join-Path $repo "native\src\launcher"))) {
        $launcherInputs.Add($file)
    }
    $embeddedFiles = @(
        $server,
        (Join-Path $repo "scripts\install_runtime_payloads.ps1"),
        (Join-Path $repo "config\api.ini.example"),
        (Join-Path $repo "config\launcher.ini.example"),
        (Join-Path $repo "assets\app_icon.ico"),
        (Join-Path $repo "VERSION"),
        (Join-Path $repo "payloads\UnityTranslator\UnityTranslator.dll"),
        (Join-Path $repo "payloads\UnityTranslator\UnityTranslator.BepInEx6.dll"),
        (Join-Path $repo "payloads\UnityIL2CPP\DeepSeekXUnityTranslator\DeepSeekTranslate.dll"),
        (Join-Path $repo "payloads\UnityIL2CPP\DeepSeekTMPFontFallback\BepInEx\plugins\DeepSeekTMPFontFallback\DeepSeekTMPFontFallback.dll"),
        (Join-Path $repo "payloads\UnityTranslator\DeepSeekUnityFontPatcher.dll")
    )
    foreach ($path in $embeddedFiles) {
        if (Test-Path -LiteralPath $path) {
            $launcherInputs.Add((Get-Item -LiteralPath $path))
        }
    }

    $launcherBase = "ds" +
        [string][char]0x6e38 + [string][char]0x620f +
        [string][char]0x7ffb + [string][char]0x8bd1 + [string][char]0x5668
    $launcher = Join-Path $repo ($launcherBase + ".exe")
    Assert-OutputFresh "native launcher" $launcher @($launcherInputs)
    if (Test-Path -LiteralPath $launcher) {
        Assert-EmbeddedResourceMatches $launcher 101 $server
        Assert-EmbeddedResourceMatches $launcher 102 (Join-Path $repo "scripts\install_runtime_payloads.ps1")
        Assert-EmbeddedResourceMatches $launcher 103 (Join-Path $repo "config\api.ini.example")
        Assert-EmbeddedResourceMatches $launcher 104 (Join-Path $repo "config\launcher.ini.example")
        Assert-EmbeddedResourceMatches $launcher 201 (Join-Path $repo "payloads\UnityTranslator\UnityTranslator.dll")
        Assert-EmbeddedResourceMatches $launcher 202 (Join-Path $repo "payloads\UnityTranslator\UnityTranslator.BepInEx6.dll")
        Assert-EmbeddedResourceMatches $launcher 203 (Join-Path $repo "payloads\UnityIL2CPP\DeepSeekXUnityTranslator\DeepSeekTranslate.dll")
        Assert-EmbeddedResourceMatches $launcher 204 (Join-Path $repo "payloads\UnityIL2CPP\DeepSeekTMPFontFallback\BepInEx\plugins\DeepSeekTMPFontFallback\DeepSeekTMPFontFallback.dll")
        Assert-EmbeddedResourceMatches $launcher 205 (Join-Path $repo "payloads\UnityTranslator\DeepSeekUnityFontPatcher.dll")
    }
}

if ($errors.Count -gt 0) {
    Write-Host "Build artifact verification FAILED:" -ForegroundColor Red
    foreach ($message in $errors) {
        Write-Host ("  - " + $message) -ForegroundColor Red
    }
    exit 1
}

Write-Host "Build artifact verification PASS." -ForegroundColor Green
exit 0
