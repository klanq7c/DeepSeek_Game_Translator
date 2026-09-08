param(
    [string]$Launcher = ""
)

$ErrorActionPreference = "Stop"

$repo = Split-Path -Parent $PSScriptRoot
. (Join-Path $PSScriptRoot "TestProcessHarness.ps1")
$passed = 0
$failed = 0

function Assert-True([bool]$Condition, [string]$Message) {
    if (-not $Condition) { throw $Message }
}

function It([string]$Name, [scriptblock]$Body) {
    try {
        & $Body
        $script:passed++
        Write-Host "  PASS $Name" -ForegroundColor Green
    } catch {
        $script:failed++
        Write-Host "  FAIL $Name -- $($_.Exception.Message)" -ForegroundColor Red
    }
}

function Invoke-PowerShellScript {
    param(
        [Parameter(Mandatory = $true)][string]$Script,
        [string[]]$Arguments = @()
    )

    $previousPreference = $ErrorActionPreference
    $ErrorActionPreference = "Continue"
    try {
        $output = & powershell.exe -NoProfile -ExecutionPolicy Bypass -File $Script @Arguments 2>&1
        $exitCode = $LASTEXITCODE
    } finally {
        $ErrorActionPreference = $previousPreference
    }
    return [pscustomobject]@{
        ExitCode = $exitCode
        Output = ($output | Out-String)
    }
}

Write-Host "Release pipeline tests" -ForegroundColor Cyan

It "Release scripts reject an output version that differs from VERSION before staging" {
    $tempRoot = Join-Path ([IO.Path]::GetTempPath()) ("ds release version with spaces " + [Guid]::NewGuid().ToString("N"))
    try {
        foreach ($name in @("program", "source")) {
            $fakeRepo = Join-Path $tempRoot $name
            $fakeScripts = Join-Path $fakeRepo "scripts"
            New-Item -ItemType Directory -Path $fakeScripts -Force | Out-Null
            Set-Content -LiteralPath (Join-Path $fakeRepo "VERSION") -Value "0.3.3.8" -NoNewline -Encoding UTF8

            $sourceScript = if ($name -eq "program") {
                Join-Path $repo "scripts\prepare_program_release.ps1"
            } else {
                Join-Path $repo "scripts\prepare_open_source_release.ps1"
            }
            $copiedScript = Join-Path $fakeScripts ([IO.Path]::GetFileName($sourceScript))
            Copy-Item -LiteralPath $sourceScript -Destination $copiedScript

            $result = Invoke-PowerShellScript -Script $copiedScript -Arguments @("-Version", "9.9.9")
            Assert-True ($result.ExitCode -ne 0) "$name release unexpectedly accepted the wrong version"
            Assert-True ($result.Output -match 'must match VERSION') "$name release failed for the wrong reason: $($result.Output)"
            Assert-True (-not (Test-Path -LiteralPath (Join-Path $fakeRepo "build"))) "$name release staged files before validating VERSION"
        }
    } finally {
        Remove-Item -LiteralPath $tempRoot -Recurse -Force -ErrorAction SilentlyContinue
    }
}

It "Release scripts refuse to stage when VERSION is missing" {
    $tempRoot = Join-Path ([IO.Path]::GetTempPath()) ("ds release missing version with spaces " + [Guid]::NewGuid().ToString("N"))
    try {
        foreach ($name in @("program", "source")) {
            $fakeRepo = Join-Path $tempRoot $name
            $fakeScripts = Join-Path $fakeRepo "scripts"
            New-Item -ItemType Directory -Path $fakeScripts -Force | Out-Null
            $sourceScript = if ($name -eq "program") {
                Join-Path $repo "scripts\prepare_program_release.ps1"
            } else {
                Join-Path $repo "scripts\prepare_open_source_release.ps1"
            }
            Copy-Item -LiteralPath $sourceScript -Destination (Join-Path $fakeScripts ([IO.Path]::GetFileName($sourceScript)))

            $result = Invoke-PowerShellScript -Script (Join-Path $fakeScripts ([IO.Path]::GetFileName($sourceScript)))
            Assert-True ($result.ExitCode -ne 0) "$name release unexpectedly ran without VERSION"
            Assert-True ($result.Output -match 'Missing VERSION') "$name release did not report the missing version boundary: $($result.Output)"
            Assert-True (-not (Test-Path -LiteralPath (Join-Path $fakeRepo "build"))) "$name release staged files without VERSION"
        }
    } finally {
        Remove-Item -LiteralPath $tempRoot -Recurse -Force -ErrorAction SilentlyContinue
    }
}

It "Headless payload sync returns nonzero when a required payload cannot be written" {
    $productName = "ds" + [string][char]0x6e38 + [string][char]0x620f +
        [string][char]0x7ffb + [string][char]0x8bd1 + [string][char]0x5668
    $launcher = if ($Launcher) { $Launcher } else { Join-Path $repo ($productName + ".exe") }
    Assert-True (Test-Path -LiteralPath $launcher -PathType Leaf) "launcher fixture is missing: $launcher"

    $tempRoot = Join-Path ([IO.Path]::GetTempPath()) ("ds sync failure with spaces " + [Guid]::NewGuid().ToString("N"))
    $process = $null
    try {
        New-Item -ItemType Directory -Path $tempRoot -Force | Out-Null
        $tempLauncher = Join-Path $tempRoot ($productName + ".exe")
        Copy-Item -LiteralPath $launcher -Destination $tempLauncher
        Set-Content -LiteralPath (Join-Path $tempRoot "native") -Value "blocks required native directory" -NoNewline -Encoding ASCII

        $process = Start-TestOwnedProcess -FilePath $tempLauncher -ArgumentList @("--sync-payloads-and-exit") -WindowStyle Hidden
        if (-not (Wait-TestOwnedProcess -Process $process -TimeoutSec 30)) { throw "headless sync timed out" }
        Assert-True ($process.ExitCode -ne 0) "headless sync returned success after dst_server.exe could not be written"
    } finally {
        if ($process) { Close-TestOwnedProcess -Process $process }
        Remove-Item -LiteralPath $tempRoot -Recurse -Force -ErrorAction SilentlyContinue
    }
}

It "Program release runs the complete artifact verifier before creating output" {
    $tempRoot = Join-Path ([IO.Path]::GetTempPath()) ("ds release verifier with spaces " + [Guid]::NewGuid().ToString("N"))
    try {
        $fakeScripts = Join-Path $tempRoot "scripts"
        New-Item -ItemType Directory -Path $fakeScripts -Force | Out-Null
        Set-Content -LiteralPath (Join-Path $tempRoot "VERSION") -Value "0.3.3.8" -NoNewline -Encoding UTF8
        Copy-Item -LiteralPath (Join-Path $repo "scripts\prepare_program_release.ps1") `
            -Destination (Join-Path $fakeScripts "prepare_program_release.ps1")
        @'
param([switch]$RequireComplete)
if (-not $RequireComplete) {
    Write-Error "VERIFY_GATE_MISSING_REQUIRE_COMPLETE"
    exit 24
}
Write-Error "VERIFY_GATE_SENTINEL"
exit 23
'@ | Set-Content -LiteralPath (Join-Path $fakeScripts "verify_build_artifacts.ps1") -Encoding UTF8

        $result = Invoke-PowerShellScript `
            -Script (Join-Path $fakeScripts "prepare_program_release.ps1") `
            -Arguments @("-Version", "0.3.3.8")
        Assert-True ($result.ExitCode -ne 0) "program release ignored the failed artifact verifier"
        Assert-True ($result.Output -match 'VERIFY_GATE_SENTINEL') "program release did not invoke verify_build_artifacts.ps1 -RequireComplete: $($result.Output)"
        Assert-True (-not (Test-Path -LiteralPath (Join-Path $tempRoot "build"))) "program release created output before the artifact gate passed"
    } finally {
        Remove-Item -LiteralPath $tempRoot -Recurse -Force -ErrorAction SilentlyContinue
    }
}

It "Headless payload sync succeeds in a path with spaces and emits only owned configuration templates" {
    $productName = "ds" + [string][char]0x6e38 + [string][char]0x620f +
        [string][char]0x7ffb + [string][char]0x8bd1 + [string][char]0x5668
    $launcherPath = if ($Launcher) { $Launcher } else { Join-Path $repo ($productName + ".exe") }
    $tempRoot = Join-Path ([IO.Path]::GetTempPath()) ("ds sync success with spaces " + [Guid]::NewGuid().ToString("N"))
    $process = $null
    try {
        New-Item -ItemType Directory -Path $tempRoot | Out-Null
        $tempLauncher = Join-Path $tempRoot ($productName + ".exe")
        Copy-Item -LiteralPath $launcherPath -Destination $tempLauncher

        $process = Start-TestOwnedProcess -FilePath $tempLauncher -ArgumentList @("--sync-payloads-and-exit") -WindowStyle Hidden
        if (-not (Wait-TestOwnedProcess -Process $process -TimeoutSec 30)) { throw "headless sync timed out" }
        Assert-True ($process.ExitCode -eq 0) "headless sync failed in a path with spaces"
        foreach ($relative in @(
            "native\dst_server.exe",
            "scripts\install_runtime_payloads.ps1",
            "config\api.ini.example",
            "config\launcher.ini.example"
        )) {
            Assert-True (Test-Path -LiteralPath (Join-Path $tempRoot $relative) -PathType Leaf) "headless sync omitted $relative"
        }
        Assert-True (-not (Test-Path -LiteralPath (Join-Path $tempRoot "config\api.ini"))) "headless sync created a real API config"
        Assert-True (-not (Test-Path -LiteralPath (Join-Path $tempRoot "translation_memory_c.tsv"))) "headless sync created translation memory"
    } finally {
        if ($process) { Close-TestOwnedProcess -Process $process }
        Remove-Item -LiteralPath $tempRoot -Recurse -Force -ErrorAction SilentlyContinue
    }
}

It "Native build preserves every C source path when the repository path contains spaces" {
    $tempRoot = Join-Path ([IO.Path]::GetTempPath()) ("ds native build path with spaces " + [Guid]::NewGuid().ToString("N"))
    $previousPath = $env:PATH
    $previousLog = $env:FAKE_BUILD_LOG
    try {
        $fakeRepo = Join-Path $tempRoot "repo with spaces"
        $fakeBin = Join-Path $tempRoot "fake tools"
        foreach ($directory in @(
            $fakeRepo,
            $fakeBin,
            (Join-Path $fakeRepo "scripts"),
            (Join-Path $fakeRepo "assets"),
            (Join-Path $fakeRepo "config"),
            (Join-Path $fakeRepo "native\src\server"),
            (Join-Path $fakeRepo "native\src\launcher"),
            (Join-Path $fakeRepo "payloads\UnityTranslator")
        )) {
            New-Item -ItemType Directory -Path $directory -Force | Out-Null
        }
        Copy-Item -LiteralPath (Join-Path $repo "build_native.bat") -Destination (Join-Path $fakeRepo "build_native.bat")
        Set-Content -LiteralPath (Join-Path $fakeRepo "VERSION") -Value "0.3.3.8" -NoNewline -Encoding ASCII
        Set-Content -LiteralPath (Join-Path $fakeRepo "assets\app_icon.ico") -Value "icon" -NoNewline -Encoding ASCII
        Set-Content -LiteralPath (Join-Path $fakeRepo "config\api.ini.example") -Value "api" -NoNewline -Encoding ASCII
        Set-Content -LiteralPath (Join-Path $fakeRepo "config\launcher.ini.example") -Value "launcher" -NoNewline -Encoding ASCII
        Set-Content -LiteralPath (Join-Path $fakeRepo "payloads\UnityTranslator\DeepSeekUnityFontPatcher.dll") -Value "patcher" -NoNewline -Encoding ASCII
        Set-Content -LiteralPath (Join-Path $fakeRepo "payloads\UnityTranslator\Newtonsoft.Json.dll") -Value "json" -NoNewline -Encoding ASCII
        @'
param([switch]$ManagedPayloadsOnly, [switch]$RequireComplete)
exit 0
'@ | Set-Content -LiteralPath (Join-Path $fakeRepo "scripts\verify_build_artifacts.ps1") -Encoding ASCII

        $fakeCompilerSource = Join-Path $tempRoot "fake_compiler.c"
        @'
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv) {
    const char *log_path = getenv("FAKE_BUILD_LOG");
    FILE *log = log_path ? fopen(log_path, "ab") : NULL;
    if (!log) return 10;
    const char *output = NULL;
    for (int i = 1; i < argc; i++) {
        fprintf(log, "ARG:%s\n", argv[i]);
        if (i > 1 && strcmp(argv[i - 1], "-o") == 0) output = argv[i];
    }
    fclose(log);
    if (output) {
        FILE *built = fopen(output, "wb");
        if (!built) return 11;
        fclose(built);
    }
    return 0;
}
'@ | Set-Content -LiteralPath $fakeCompilerSource -Encoding ASCII
        $realGcc = Join-Path $repo "native\toolchain\w64devkit\bin\gcc.exe"
        $fakeGcc = Join-Path $fakeBin "gcc.exe"
        $env:PATH = "$(Split-Path -Parent $realGcc);$previousPath"
        & $realGcc $fakeCompilerSource -o $fakeGcc
        Assert-True ($LASTEXITCODE -eq 0 -and (Test-Path -LiteralPath $fakeGcc)) "could not build fake compiler"
        Copy-Item -LiteralPath $fakeGcc -Destination (Join-Path $fakeBin "windres.exe")

        $logPath = Join-Path $tempRoot "compiler arguments.log"
        $env:FAKE_BUILD_LOG = $logPath
        $env:PATH = "$fakeBin;$previousPath"
        Push-Location $fakeRepo
        try {
            $buildOutput = & cmd.exe /d /c build_native.bat 2>&1
            $buildExit = $LASTEXITCODE
        } finally {
            Pop-Location
        }
        Assert-True ($buildExit -eq 0) "fake native build failed: $($buildOutput | Out-String)"

        $logged = @(Get-Content -LiteralPath $logPath)
        $expectedSources = @(
            "native\src\server\main.c", "native\src\server\util.c",
            "native\src\server\buf.c", "native\src\server\b64.c",
            "native\src\server\json.c", "native\src\server\cache.c",
            "native\src\server\api.c", "native\src\server\http.c",
            "native\src\launcher\main.c", "native\src\launcher\globals.c",
            "native\src\launcher\fsutil.c", "native\src\launcher\engine.c",
            "native\src\launcher\deploy.c", "native\src\launcher\server_proc.c",
            "native\src\launcher\api_config.c", "native\src\launcher\warmup.c",
            "native\src\launcher\godot_warmup.c", "native\src\launcher\godot_patch.c",
            "native\src\launcher\godot_probe.c", "native\src\launcher\ui.c",
            "native\src\launcher\self_update.c"
        )
        foreach ($relative in $expectedSources) {
            $expected = "ARG:" + (Join-Path $fakeRepo $relative)
            Assert-True ($logged -contains $expected) "build split or omitted source path '$relative'"
        }
    } finally {
        $env:PATH = $previousPath
        $env:FAKE_BUILD_LOG = $previousLog
        Remove-Item -LiteralPath $tempRoot -Recurse -Force -ErrorAction SilentlyContinue
    }
}

Write-Host ""
Invoke-TestOwnedCleanup -ErrorAction Continue
Write-Host ("Release pipeline summary: {0} passed, {1} failed" -f $passed, $failed)
if ($failed -gt 0) { exit 1 }
exit 0
