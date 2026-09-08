$ErrorActionPreference = "Stop"

$repo = Split-Path -Parent $PSScriptRoot
. (Join-Path $PSScriptRoot "TestProcessHarness.ps1")

$tempRoot = Join-Path ([IO.Path]::GetTempPath()) ("ds process harness with spaces " + [Guid]::NewGuid().ToString("N"))
$external = $null
$owned = $null
try {
    New-Item -ItemType Directory -Path $tempRoot | Out-Null

    $missingExe = Join-Path $tempRoot "missing executable.exe"
    $missingOut = Join-Path $tempRoot "missing stdout.log"
    $missingErr = Join-Path $tempRoot "missing stderr.log"
    $startupFailure = $null
    try {
        Start-TestOwnedProcess -FilePath $missingExe `
            -RedirectStandardOutput $missingOut -RedirectStandardError $missingErr
    } catch {
        $startupFailure = $_.Exception
    }
    if (-not $startupFailure) { throw "missing executable unexpectedly started" }
    if ($startupFailure.Message -notmatch "cannot find|not found" -or
        $startupFailure.Message -match "Cannot bind argument to parameter 'Id'") {
        throw "harness masked the original process startup failure: $($startupFailure.Message)"
    }
    foreach ($redirectPath in @($missingOut, $missingErr)) {
        $exclusive = [IO.File]::Open(
            $redirectPath,
            [IO.FileMode]::OpenOrCreate,
            [IO.FileAccess]::ReadWrite,
            [IO.FileShare]::None)
        $exclusive.Dispose()
    }

    $probeSource = Join-Path $tempRoot "argument_probe.c"
    $probeExe = Join-Path $tempRoot "argument probe.exe"
    @'
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

int main(int argc, char **argv) {
    if (argc >= 3 && strcmp(argv[1], "--write") == 0) {
        FILE *out = fopen(argv[2], "wb");
        if (!out) return 2;
        for (int i = 3; i < argc; i++) fprintf(out, "%s\n", argv[i]);
        fclose(out);
        return 0;
    }
    if (argc >= 3 && strcmp(argv[1], "--sleep") == 0) {
        Sleep((DWORD)strtoul(argv[2], NULL, 10));
        return 0;
    }
    return 3;
}
'@ | Set-Content -LiteralPath $probeSource -Encoding ASCII

    $bin = Join-Path $repo "native\toolchain\w64devkit\bin"
    $previousPath = $env:PATH
    $env:PATH = "$bin;$env:PATH"
    try {
        & (Join-Path $bin "gcc.exe") $probeSource -o $probeExe
        if ($LASTEXITCODE -ne 0) { throw "could not build process harness probe" }
    } finally {
        $env:PATH = $previousPath
    }

    $argumentLog = Join-Path $tempRoot "arguments with spaces.log"
    $argumentProcess = Start-TestOwnedProcess -FilePath $probeExe `
        -ArgumentList @("--write", $argumentLog, "value with spaces", "plain", "trailing\")
    if (-not (Wait-TestOwnedProcess -Process $argumentProcess -TimeoutSec 10)) {
        throw "argument probe did not exit"
    }
    Complete-TestOwnedProcess -Process $argumentProcess
    $arguments = @(Get-Content -LiteralPath $argumentLog)
    if ($arguments.Count -ne 3 -or
        $arguments[0] -ne "value with spaces" -or
        $arguments[1] -ne "plain" -or
        $arguments[2] -ne "trailing\") {
        throw "native argument quoting changed values: $($arguments -join '|')"
    }

    $owned = Start-TestOwnedProcess -FilePath $probeExe -ArgumentList @("--sleep", "30000")
    $external = Start-Process -FilePath $probeExe -ArgumentList @("--sleep", "30000") -PassThru -WindowStyle Hidden
    Stop-TestOwnedProcess -Process $owned
    $owned.Refresh()
    $external.Refresh()
    if (-not $owned.HasExited) { throw "owned process was not stopped" }
    if ($external.HasExited) { throw "cleanup stopped a process it did not own" }

    $refused = $false
    try {
        Stop-TestOwnedProcess -Process $external
    } catch {
        $refused = $_.Exception.Message -match "not owned"
    }
    if (-not $refused) { throw "harness did not reject an unowned process" }

    Write-Host "Test process harness PASS" -ForegroundColor Green
} finally {
    Invoke-TestOwnedCleanup -ErrorAction SilentlyContinue
    if ($external -and -not $external.HasExited) {
        Stop-Process -Id $external.Id -Force -ErrorAction SilentlyContinue
    }
    Remove-Item -LiteralPath $tempRoot -Recurse -Force -ErrorAction SilentlyContinue
}
