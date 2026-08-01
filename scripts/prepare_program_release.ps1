param(
    [string]$Version = ""
)

$ErrorActionPreference = "Stop"

$repo = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
$versionFileContent = ""
$versionFile = Join-Path $repo "VERSION"
if (Test-Path -LiteralPath $versionFile) {
    $versionFileContent = (Get-Content -LiteralPath $versionFile -Raw).Trim()
}
if (-not $Version) {
    if ($versionFileContent) {
        $Version = $versionFileContent
    } else {
        $Version = "preview"
    }
}
if ($Version -notmatch '\A[0-9A-Za-z][0-9A-Za-z._+-]{0,63}\z' -or
    $Version -eq "." -or $Version -eq "..") {
    throw "Invalid release version. Use 1-64 ASCII letters, digits, dot, underscore, plus, or hyphen."
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
