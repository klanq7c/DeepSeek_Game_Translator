param(
    [switch]$ManagedPayloadsOnly,
    [switch]$RequireComplete,
    [ValidateSet("UnityMono5", "UnityMono6", "UnityXUnity", "UnityTmpFallback", "UnityFontPatcher")]
    [string]$RecordManagedPayload = ""
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

# Source files a .csproj pulls in from outside its own directory via
# <Compile Include="..\..\native\src\core\X.cs" ...>. Wildcards are expanded
# relative to the project directory; missing files are ignored (the compiler
# would already have failed on them).
function Get-LinkedCompileSources {
    param([Parameter(Mandatory = $true)][string]$Csproj)

    if (-not (Test-Path -LiteralPath $Csproj -PathType Leaf)) { return @() }
    $projectDir = Split-Path -Parent $Csproj
    $content = Get-Content -LiteralPath $Csproj -Raw -Encoding UTF8
    $files = New-Object System.Collections.Generic.List[System.IO.FileInfo]
    foreach ($match in [regex]::Matches($content, '<Compile\s+Include="([^"]+)"')) {
        $pattern = $match.Groups[1].Value
        if ($pattern -notmatch '[\\/]\.\.[\\/]' -and $pattern -notmatch '^\.\.[\\/]') { continue }
        $resolved = Join-Path $projectDir $pattern
        foreach ($item in @(Get-ChildItem -Path $resolved -File -ErrorAction SilentlyContinue)) {
            $files.Add($item)
        }
    }
    return @($files)
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
    foreach ($definition in Get-ManagedPayloadDefinitions) {
        $inputs = @($definition.Inputs)
        Assert-ManagedPayloadFingerprint $definition $inputs
    }
}

function Get-ManagedPayloadDefinitions {
    $stampRoot = Join-Path $repo "payloads\ManagedBuildStamps"
    # The managed build recipe lives in its own script so that launcher/server
    # changes in build_native.bat do not invalidate payload stamps that can only
    # be re-recorded on a machine with the Unity/BepInEx reference assemblies.
    $recipeInputs = @(
        Get-Item -LiteralPath (Join-Path $repo "scripts\build_managed_payloads.bat")
        Get-Item -LiteralPath (Join-Path $repo "scripts\install_runtime_payloads.ps1")
    )
    $monoRoot = Join-Path $repo "payloads\UnityTranslator\src"
    $monoInputs = @(Get-SourceFiles -Root $monoRoot | Where-Object {
        $_.FullName -notmatch "[\\/]FontPatcher[\\/]"
    })
    $endpointRoot = Join-Path $repo "payloads\UnityIL2CPP\DeepSeekXUnityTranslator\src"
    # The endpoint links shared-core sources from native\src\core (see its .csproj);
    # a change there must invalidate the payload too, so fingerprint it repo-relative.
    $endpointInputs = @(Get-SourceFiles -Root $endpointRoot) +
        @(Get-LinkedCompileSources -Csproj (Join-Path $endpointRoot "DeepSeekXUnityTranslator.csproj"))
    $tmpRoot = Join-Path $repo "payloads\UnityIL2CPP\DeepSeekTMPFontFallback\src"
    $fontPatcherRoot = Join-Path $repo "payloads\UnityTranslator\src\FontPatcher"

    $items = @(
        [pscustomobject]@{
            Name = "UnityMono5"; Label = "Unity Mono BepInEx 5 payload"; SourceRoot = $monoRoot
            Inputs = $monoInputs; Output = Join-Path $repo "payloads\UnityTranslator\UnityTranslator.dll"
        },
        [pscustomobject]@{
            Name = "UnityMono6"; Label = "Unity Mono BepInEx 6 payload"; SourceRoot = $monoRoot
            Inputs = $monoInputs; Output = Join-Path $repo "payloads\UnityTranslator\UnityTranslator.BepInEx6.dll"
        },
        [pscustomobject]@{
            Name = "UnityXUnity"; Label = "Unity IL2CPP XUnity endpoint"; SourceRoot = $repo
            Inputs = $endpointInputs; Output = Join-Path $repo "payloads\UnityIL2CPP\DeepSeekXUnityTranslator\DeepSeekTranslate.dll"
        },
        [pscustomobject]@{
            Name = "UnityTmpFallback"; Label = "Unity IL2CPP TMP fallback payload"; SourceRoot = $tmpRoot
            Inputs = @(Get-SourceFiles -Root $tmpRoot); Output = Join-Path $repo "payloads\UnityIL2CPP\DeepSeekTMPFontFallback\BepInEx\plugins\DeepSeekTMPFontFallback\DeepSeekTMPFontFallback.dll"
        },
        [pscustomobject]@{
            Name = "UnityFontPatcher"; Label = "Unity Mono stripped-font patcher"; SourceRoot = $fontPatcherRoot
            Inputs = @(Get-SourceFiles -Root $fontPatcherRoot); Output = Join-Path $repo "payloads\UnityTranslator\DeepSeekUnityFontPatcher.dll"
        }
    )
    foreach ($item in $items) {
        $item | Add-Member -NotePropertyName Stamp -NotePropertyValue (Join-Path $stampRoot ($item.Name + ".json"))
        $item | Add-Member -NotePropertyName RecipeInputs -NotePropertyValue $recipeInputs
    }
    return $items
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

function Get-FileSha256 {
    param([Parameter(Mandatory = $true)][string]$Path)

    $stream = [IO.File]::OpenRead($Path)
    $sha = [Security.Cryptography.SHA256]::Create()
    try {
        return ([BitConverter]::ToString($sha.ComputeHash($stream))).Replace("-", "")
    } finally {
        $sha.Dispose()
        $stream.Dispose()
    }
}

function Get-SourceFingerprint {
    param(
        [Parameter(Mandatory = $true)][string]$Root,
        [Parameter(Mandatory = $true)][System.IO.FileInfo[]]$Inputs
    )

    $rootPath = [IO.Path]::GetFullPath($Root).TrimEnd([char[]]@('\', '/'))
    $builder = [Text.StringBuilder]::new()
    foreach ($file in ($Inputs | Sort-Object FullName)) {
        $fullPath = [IO.Path]::GetFullPath($file.FullName)
        if (-not $fullPath.StartsWith($rootPath + [IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase)) {
            throw "source '$fullPath' is outside fingerprint root '$rootPath'"
        }
        $relative = $fullPath.Substring($rootPath.Length + 1).Replace('\', '/')
        [void]$builder.Append($relative).Append("`t").Append((Get-FileSha256 $fullPath)).Append("`n")
    }
    $bytes = [Text.Encoding]::UTF8.GetBytes($builder.ToString())
    $sha = [Security.Cryptography.SHA256]::Create()
    try {
        return ([BitConverter]::ToString($sha.ComputeHash($bytes))).Replace("-", "")
    } finally {
        $sha.Dispose()
    }
}

function Write-ManagedPayloadStamp {
    param([Parameter(Mandatory = $true)]$Definition)

    $inputs = @($Definition.Inputs)
    if ($inputs.Count -eq 0) { throw "$($Definition.Label) has no source inputs to record" }
    if (-not (Test-Path -LiteralPath $Definition.Output -PathType Leaf)) {
        throw "$($Definition.Label) is missing: $($Definition.Output)"
    }
    $stamp = [ordered]@{
        schema = 2
        sourceSha256 = Get-SourceFingerprint $Definition.SourceRoot $inputs
        recipeSha256 = Get-SourceFingerprint $repo @($Definition.RecipeInputs)
        outputSha256 = Get-FileSha256 $Definition.Output
    }
    $stampDirectory = Split-Path -Parent $Definition.Stamp
    if (-not (Test-Path -LiteralPath $stampDirectory -PathType Container)) {
        New-Item -ItemType Directory -Path $stampDirectory -Force | Out-Null
    }
    $utf8NoBom = [Text.UTF8Encoding]::new($false)
    $json = ($stamp | ConvertTo-Json).Replace("`r`n", "`n")
    [IO.File]::WriteAllText($Definition.Stamp, ($json + "`n"), $utf8NoBom)
    Write-Host ("Recorded managed payload fingerprint: " + $Definition.Name) -ForegroundColor Green
}

function Assert-ManagedPayloadFingerprint {
    param(
        [Parameter(Mandatory = $true)]$Definition,
        [Parameter(Mandatory = $true)][System.IO.FileInfo[]]$Inputs
    )

    if (-not (Test-Path -LiteralPath $Definition.Stamp -PathType Leaf)) {
        $script:errors.Add("$($Definition.Label) fingerprint is missing: $($Definition.Stamp)")
        return
    }
    if (-not (Test-Path -LiteralPath $Definition.Output -PathType Leaf) -or $Inputs.Count -eq 0) { return }
    try {
        $stamp = Get-Content -LiteralPath $Definition.Stamp -Raw -Encoding UTF8 | ConvertFrom-Json
        if ($stamp.schema -ne 2 -or -not $stamp.sourceSha256 -or
            -not $stamp.recipeSha256 -or -not $stamp.outputSha256) {
            throw "unsupported or incomplete fingerprint record"
        }
        $sourceHash = Get-SourceFingerprint $Definition.SourceRoot $Inputs
        if ($sourceHash -ne ([string]$stamp.sourceSha256).ToUpperInvariant()) {
            $script:errors.Add("$($Definition.Label) source fingerprint does not match its recorded build")
        }
        $recipeHash = Get-SourceFingerprint $repo @($Definition.RecipeInputs)
        if ($recipeHash -ne ([string]$stamp.recipeSha256).ToUpperInvariant()) {
            $script:errors.Add("$($Definition.Label) build recipe fingerprint does not match its recorded build")
        }
        $outputHash = Get-FileSha256 $Definition.Output
        if ($outputHash -ne ([string]$stamp.outputSha256).ToUpperInvariant()) {
            $script:errors.Add("$($Definition.Label) output fingerprint does not match its recorded build")
        }
    } catch {
        $script:errors.Add("$($Definition.Label) fingerprint could not be verified: $($_.Exception.Message)")
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
        $payloadHash = Get-FileSha256 $Payload
        if ($embeddedHash -ne $payloadHash) {
            $script:errors.Add(
                "launcher resource $ResourceId does not match '$Payload': " +
                "embedded=$embeddedHash payload=$payloadHash")
        }
    } catch {
        $script:errors.Add($_.Exception.Message)
    }
}

if ($RecordManagedPayload) {
    if ($ManagedPayloadsOnly -or $RequireComplete) {
        throw "-RecordManagedPayload cannot be combined with verification modes"
    }
    $definition = @(Get-ManagedPayloadDefinitions | Where-Object Name -eq $RecordManagedPayload)
    if ($definition.Count -ne 1) { throw "unknown managed payload '$RecordManagedPayload'" }
    Write-ManagedPayloadStamp $definition[0]
    exit 0
}

Assert-ManagedPayloadsFresh

if ($RequireComplete -or -not $ManagedPayloadsOnly) {
    $serverInputs = @(Get-SourceFiles -Root (Join-Path $repo "native\src\server"))
    $serverInputs += Get-Item -LiteralPath (Join-Path $repo "build_native.bat")
    $server = Join-Path $repo "native\dst_server.exe"
    Assert-OutputFresh "native translation server" $server $serverInputs

    # C# server: its own sources plus the shared core it links (native\src\core).
    $serverCsInputs = @(Get-SourceFiles -Root (Join-Path $repo "native\src\server_cs"))
    $serverCsInputs += @(Get-SourceFiles -Root (Join-Path $repo "native\src\core"))
    $serverCs = Join-Path $repo "native\dst_server_cs.exe"
    Assert-OutputFresh "C# translation server" $serverCs $serverCsInputs

    $launcherInputs = [System.Collections.Generic.List[System.IO.FileInfo]]::new()
    foreach ($file in (Get-SourceFiles -Root (Join-Path $repo "native\src\launcher"))) {
        $launcherInputs.Add($file)
    }
    $launcherInputs.Add((Get-Item -LiteralPath (Join-Path $repo "build_native.bat")))
    $embeddedFiles = @(
        $server,
        $serverCs,
        (Join-Path $repo "scripts\install_runtime_payloads.ps1"),
        (Join-Path $repo "config\api.ini.example"),
        (Join-Path $repo "config\launcher.ini.example"),
        (Join-Path $repo "config\glossary.example.tsv"),
        (Join-Path $repo "assets\app_icon.ico"),
        (Join-Path $repo "VERSION"),
        (Join-Path $repo "payloads\RenPy\iron_deepseek.rpy"),
        (Join-Path $repo "payloads\RPGMaker\hook_rpgm_mv.js"),
        (Join-Path $repo "payloads\Godot\dst_godot_runtime_g3.gd"),
        (Join-Path $repo "payloads\Godot\dst_godot_runtime_g4.gd"),
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
        $expectedVersion = (Get-Content -LiteralPath (Join-Path $repo "VERSION") -Raw -Encoding UTF8).Trim()
        $versionInfo = (Get-Item -LiteralPath $launcher).VersionInfo
        $fileVersion = ([string]$versionInfo.FileVersion).Trim()
        $productVersion = ([string]$versionInfo.ProductVersion).Trim()
        if ($fileVersion -ne $expectedVersion) {
            $errors.Add("native launcher FileVersion '$($versionInfo.FileVersion)' does not match VERSION '$expectedVersion'")
        }
        if ($productVersion -ne $expectedVersion) {
            $errors.Add("native launcher ProductVersion '$($versionInfo.ProductVersion)' does not match VERSION '$expectedVersion'")
        }
        Assert-EmbeddedResourceMatches $launcher 101 $server
        Assert-EmbeddedResourceMatches $launcher 102 (Join-Path $repo "scripts\install_runtime_payloads.ps1")
        Assert-EmbeddedResourceMatches $launcher 103 (Join-Path $repo "config\api.ini.example")
        Assert-EmbeddedResourceMatches $launcher 104 (Join-Path $repo "config\launcher.ini.example")
        Assert-EmbeddedResourceMatches $launcher 105 $serverCs
        Assert-EmbeddedResourceMatches $launcher 106 (Join-Path $repo "config\glossary.example.tsv")
        # Engine runtime scripts: deployed verbatim by deploy.c/godot_patch.c (resource.h 301-304).
        Assert-EmbeddedResourceMatches $launcher 301 (Join-Path $repo "payloads\RenPy\iron_deepseek.rpy")
        Assert-EmbeddedResourceMatches $launcher 302 (Join-Path $repo "payloads\RPGMaker\hook_rpgm_mv.js")
        Assert-EmbeddedResourceMatches $launcher 303 (Join-Path $repo "payloads\Godot\dst_godot_runtime_g3.gd")
        Assert-EmbeddedResourceMatches $launcher 304 (Join-Path $repo "payloads\Godot\dst_godot_runtime_g4.gd")
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
