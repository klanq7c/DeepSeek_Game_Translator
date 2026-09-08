param(
    [string]$Version = ""
)

$ErrorActionPreference = "Stop"

$repo = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
$versionFile = Join-Path $repo "VERSION"
if (-not (Test-Path -LiteralPath $versionFile -PathType Leaf)) {
    throw "Missing VERSION file: $versionFile"
}
$versionFileContent = (Get-Content -LiteralPath $versionFile -Raw).Trim()
if (-not $versionFileContent) { throw "Missing VERSION value in $versionFile" }
if (-not $Version) {
    $Version = $versionFileContent
}
if ($Version -notmatch '\A[0-9A-Za-z][0-9A-Za-z._+-]{0,63}\z' -or
    $Version -eq "." -or $Version -eq "..") {
    throw "Invalid release version. Use 1-64 ASCII letters, digits, dot, underscore, plus, or hyphen."
}
if (-not [string]::Equals($Version, $versionFileContent, [StringComparison]::Ordinal)) {
    throw "Release version '$Version' must match VERSION '$versionFileContent'."
}

# Verify the complete artifact set before creating any release output.
$artifactVerifier = Join-Path $PSScriptRoot "verify_build_artifacts.ps1"
if (-not (Test-Path -LiteralPath $artifactVerifier -PathType Leaf)) {
    throw "Missing build artifact verifier: $artifactVerifier"
}
$previousPreference = $ErrorActionPreference
$ErrorActionPreference = "Continue"
try {
    $verifyOutput = & powershell.exe -NoProfile -ExecutionPolicy Bypass `
        -File $artifactVerifier -RequireComplete 2>&1
    $verifyExitCode = $LASTEXITCODE
} finally {
    $ErrorActionPreference = $previousPreference
}
if ($verifyOutput) { $verifyOutput | ForEach-Object { Write-Host $_ } }
if ($verifyExitCode -ne 0) {
    throw "Complete build artifact verification failed; no program release was created."
}

$distRoot = Join-Path $repo "build\dist"
$DsName = "ds" + [string][char]0x6e38 + [string][char]0x620f + [string][char]0x7ffb + [string][char]0x8bd1 + [string][char]0x5668
$UsageName = "README_" + [string][char]0x4f7f + [string][char]0x7528 + [string][char]0x8bf4 + [string][char]0x660e + ".txt"
$stageName = "${DsName}_$Version"
$stage = Join-Path $distRoot $stageName
$zipPath = Join-Path $distRoot "$stageName.zip"
$singleExePath = Join-Path $distRoot "$stageName.exe"

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

Assert-Under $distRoot (Join-Path $repo "build")
Assert-Under $stage $distRoot
Assert-Under $zipPath $distRoot
Assert-Under $singleExePath $distRoot
New-Item -ItemType Directory -Force -Path $distRoot | Out-Null

if (Test-Path -LiteralPath $stage) {
    Assert-Under $stage $distRoot
    Remove-Item -LiteralPath $stage -Recurse -Force
}
New-Item -ItemType Directory -Path $stage | Out-Null

function Copy-ReleaseFile([string]$SourceRelative, [string]$DestRelative = $SourceRelative) {
    $src = Join-Path $repo $SourceRelative
    if (-not (Test-Path -LiteralPath $src)) {
        throw "Missing release input: $SourceRelative"
    }
    $dst = Join-Path $stage $DestRelative
    $parent = Split-Path -Parent $dst
    if ($parent -and -not (Test-Path -LiteralPath $parent)) {
        New-Item -ItemType Directory -Force -Path $parent | Out-Null
    }
    Copy-Item -LiteralPath $src -Destination $dst -Force
}

function Test-StagedLauncherPayloads([string]$LauncherPath) {
    $smokeRoot = Join-Path ([IO.Path]::GetTempPath()) `
        ("ds program release smoke with spaces " + [Guid]::NewGuid().ToString("N"))
    $smokeProcess = $null
    try {
        New-Item -ItemType Directory -Path $smokeRoot | Out-Null
        $smokeLauncher = Join-Path $smokeRoot ([IO.Path]::GetFileName($LauncherPath))
        Copy-Item -LiteralPath $LauncherPath -Destination $smokeLauncher -Force

        $smokeProcess = Start-Process -FilePath $smokeLauncher `
            -ArgumentList "--sync-payloads-and-exit" -PassThru
        if (-not $smokeProcess.WaitForExit(30000)) {
            Stop-Process -Id $smokeProcess.Id -Force -ErrorAction SilentlyContinue
            throw "Standalone launcher payload sync exceeded 30 seconds."
        }
        if ($smokeProcess.ExitCode -ne 0) {
            throw "Standalone launcher payload sync failed with exit code $($smokeProcess.ExitCode)."
        }

        $payloadPairs = @(
            @("native\dst_server.exe", "native\dst_server.exe"),
            @("native\dst_server_cs.exe", "native\dst_server_cs.exe"),
            @("scripts\install_runtime_payloads.ps1", "scripts\install_runtime_payloads.ps1"),
            @("config\api.ini.example", "config\api.ini.example"),
            @("config\launcher.ini.example", "config\launcher.ini.example"),
            @("config\glossary.example.tsv", "config\glossary.example.tsv"),
            @("payloads\UnityTranslator\UnityTranslator.dll", "payloads\UnityTranslator\UnityTranslator.dll"),
            @("payloads\UnityTranslator\UnityTranslator.BepInEx6.dll", "payloads\UnityTranslator\UnityTranslator.BepInEx6.dll"),
            @("payloads\UnityIL2CPP\DeepSeekXUnityTranslator\DeepSeekTranslate.dll", "payloads\UnityIL2CPP\DeepSeekXUnityTranslator\DeepSeekTranslate.dll"),
            @("payloads\UnityIL2CPP\DeepSeekTMPFontFallback\BepInEx\plugins\DeepSeekTMPFontFallback\DeepSeekTMPFontFallback.dll", "payloads\UnityIL2CPP\DeepSeekTMPFontFallback\BepInEx\plugins\DeepSeekTMPFontFallback\DeepSeekTMPFontFallback.dll"),
            @("payloads\UnityTranslator\DeepSeekUnityFontPatcher.dll", "payloads\UnityTranslator\DeepSeekUnityFontPatcher.dll")
        )
        foreach ($pair in $payloadPairs) {
            $emitted = Join-Path $smokeRoot $pair[0]
            $expected = Join-Path $repo $pair[1]
            if (-not (Test-Path -LiteralPath $emitted -PathType Leaf)) {
                throw "Standalone launcher did not emit required payload '$($pair[0])'."
            }
            if (-not (Test-Path -LiteralPath $expected -PathType Leaf)) {
                throw "Program release input is missing required payload '$($pair[1])'."
            }
            $emittedHash = (Get-FileHash -LiteralPath $emitted -Algorithm SHA256).Hash
            $expectedHash = (Get-FileHash -LiteralPath $expected -Algorithm SHA256).Hash
            if ($emittedHash -ne $expectedHash) {
                throw "Standalone launcher emitted stale payload '$($pair[0])'."
            }
        }

        foreach ($forbiddenRelative in @(
            "config\api.ini",
            "config\launcher.ini",
            "translation_memory_c.tsv"
        )) {
            if (Test-Path -LiteralPath (Join-Path $smokeRoot $forbiddenRelative)) {
                throw "Standalone launcher payload sync created user-owned file '$forbiddenRelative'."
            }
        }
    } finally {
        if ($smokeProcess -and -not $smokeProcess.HasExited) {
            Stop-Process -Id $smokeProcess.Id -Force -ErrorAction SilentlyContinue
        }
        if (Test-Path -LiteralPath $smokeRoot) {
            Remove-Item -LiteralPath $smokeRoot -Recurse -Force -ErrorAction SilentlyContinue
        }
    }
}

Copy-ReleaseFile "$DsName.exe" "$DsName.exe"
if ($versionFileContent) {
    $stagedExe = Join-Path $stage "$DsName.exe"
    $versionInfo = (Get-Item -LiteralPath $stagedExe).VersionInfo
    $fileVersion = ([string]$versionInfo.FileVersion).Trim()
    $productVersion = ([string]$versionInfo.ProductVersion).Trim()
    if ($fileVersion -ne $versionFileContent -or $productVersion -ne $versionFileContent) {
        throw "$DsName.exe version metadata does not match VERSION '$versionFileContent'; rebuild with build_native.bat before releasing."
    }
    # Keep checking the compile-time footer string as a separate guard against
    # a launcher whose VERSIONINFO was edited without rebuilding the program.
    $exeText = [System.Text.Encoding]::Unicode.GetString([System.IO.File]::ReadAllBytes($stagedExe))
    if (-not $exeText.Contains($versionFileContent)) {
        throw "$DsName.exe does not contain the compile-time VERSION string '$versionFileContent'; rebuild with build_native.bat before releasing."
    }
}
Test-StagedLauncherPayloads (Join-Path $stage "$DsName.exe")
Copy-ReleaseFile "README.md" "README.md"
Copy-ReleaseFile "LICENSE" "LICENSE"
Copy-ReleaseFile "THIRD_PARTY_NOTICES.md" "THIRD_PARTY_NOTICES.md"
Copy-ReleaseFile "docs\DEPENDENCY_POLICY.md" "docs\DEPENDENCY_POLICY.md"
Copy-ReleaseFile "docs\RUNTIME_PAYLOADS.md" "docs\RUNTIME_PAYLOADS.md"
Copy-ReleaseFile "docs\USER_GUIDE.md" "docs\USER_GUIDE.md"
Copy-ReleaseFile "docs\USER_GUIDE.md" $UsageName

$allowedBinaries = @("$DsName.exe")
$forbidden = @()
$secretHits = @()
$stageFull = (Resolve-Path -LiteralPath $stage).Path

foreach ($file in Get-ChildItem -LiteralPath $stage -Recurse -File) {
    $rel = $file.FullName.Substring($stageFull.Length + 1).Replace("\", "/")
    if ($file.Extension -in ".exe", ".dll", ".pdb", ".mdb") {
        if ($allowedBinaries -notcontains $file.Name) {
            $forbidden += $file.FullName
        }
    }
    if ($rel -match '(^|/)native/|(^|/)payloads/|(^|/)scripts/|(^|/)config/|translation_memory|\.tsv$|\.log$|\.ini$|UnityEngine|Assembly-CSharp|GameAssembly|BepInExRuntime|UnityMonoRuntime|XUnityAutoTranslator|Newtonsoft\.Json\.dll|\.ttf$|\.ttc$|\.otf$') {
        $forbidden += $file.FullName
    }
    if ($file.Extension -notin ".exe", ".dll") {
        try {
            $text = Get-Content -LiteralPath $file.FullName -Raw -ErrorAction Stop
        } catch {
            $secretHits += "$($file.FullName): unreadable or non-text file"
            continue
        }
        foreach ($pattern in @(
            "sk-[A-Za-z0-9]{16,}",
            "(?i)api[_-]?key\s*=\s*[^<\s][^\r\n]+",
            "(?i)password\s*=\s*[^<\s][^\r\n]+",
            "(?i)secret\s*=\s*[^<\s][^\r\n]+",
            "C:\\Users\\[A-Za-z0-9._-]+",
            "E:\\Projects\\[^\\\r\n]+",
            "/Users/[A-Za-z0-9._-]+",
            "/home/[A-Za-z0-9._-]+"
        )) {
            if ($text -match $pattern) {
                $secretHits += "$($file.FullName): $pattern"
                break
            }
        }
    }
}

if ($forbidden.Count -gt 0) {
    throw "Program package contains forbidden files:`n$($forbidden -join "`n")"
}
if ($secretHits.Count -gt 0) {
    throw "Program package contains possible secrets/local paths:`n$($secretHits -join "`n")"
}

Copy-Item -LiteralPath (Join-Path $stage "$DsName.exe") -Destination $singleExePath -Force

if (Test-Path -LiteralPath $zipPath) {
    Remove-Item -LiteralPath $zipPath -Force
}
Compress-Archive -LiteralPath $stage -DestinationPath $zipPath -Force

Write-Host "Created program archive: $zipPath"
Write-Host "Created standalone launcher: $singleExePath"
