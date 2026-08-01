# Run all dst_server test suites.
# Returns nonzero exit if any suite fails.
param(
    [switch]$SkipEndurance,
    [int]$EnduranceSec = 60,
    [int]$SuiteTimeoutSec = 1800
)

$ErrorActionPreference = "Continue"
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$results = @()

function Run-Suite($name, $script, $argList = @()) {
    Write-Host ""
    Write-Host ("##" * 30) -ForegroundColor Cyan
    Write-Host (" SUITE: " + $name) -ForegroundColor Cyan
    Write-Host ("##" * 30) -ForegroundColor Cyan
    $outLog = Join-Path $env:TEMP ("ds_runall_" + [Guid]::NewGuid().ToString("N") + ".out.log")
    $errLog = Join-Path $env:TEMP ("ds_runall_" + [Guid]::NewGuid().ToString("N") + ".err.log")
    $arguments = @("-ExecutionPolicy", "Bypass", "-NoProfile", "-File", ('"' + (Join-Path $here $script) + '"'))
    foreach ($a in $argList) { $arguments += [string]$a }
    $proc = Start-Process -FilePath "powershell" -ArgumentList $arguments -NoNewWindow -PassThru `
        -RedirectStandardOutput $outLog -RedirectStandardError $errLog
    try {
        $proc | Wait-Process -Timeout $SuiteTimeoutSec -ErrorAction Stop
        $code = $proc.ExitCode
    } catch {
        Write-Host (" SUITE TIMEOUT: " + $name + " exceeded " + $SuiteTimeoutSec + "s; terminating process tree.") -ForegroundColor Red
        & taskkill /PID $proc.Id /T /F 2>&1 | Out-Null
        $code = 1
    }
    if (Test-Path -LiteralPath $outLog) { Get-Content -LiteralPath $outLog | Out-Host }
    if (Test-Path -LiteralPath $errLog) { Get-Content -LiteralPath $errLog | Out-Host }
    Remove-Item -LiteralPath $outLog, $errLog -Force -ErrorAction SilentlyContinue
    $script:results += [pscustomobject]@{ Suite = $name; ExitCode = $code }
    return $code
}

Run-Suite "Build artifact/source consistency" "..\scripts\verify_build_artifacts.ps1" @("-RequireComplete") | Out-Null
Run-Suite "Basic E2E (test_server.ps1)" "test_server.ps1" | Out-Null
Run-Suite "Cache export/persistence (test_cache_export_persistence.ps1)" "test_cache_export_persistence.ps1" | Out-Null
Run-Suite "Complex / edge / encoding (test_complex.ps1)" "test_complex.ps1" | Out-Null
Run-Suite "API original echo guard (test_api_guard.ps1)" "test_api_guard.ps1" | Out-Null
Run-Suite "API channel pool concurrency (test_concurrency.ps1)" "test_concurrency.ps1" | Out-Null
Run-Suite "Unity text rules (test_unity_text_rules.ps1)" "test_unity_text_rules.ps1" | Out-Null
Run-Suite "Unity IL2CPP endpoint state machine" "test_unity_endpoint_state.ps1" | Out-Null
Run-Suite "Launcher detection / deploy routing (test_launcher.ps1)" "test_launcher.ps1" | Out-Null
if (-not $SkipEndurance) {
    Run-Suite "Generational idle-memory waves (test_memory_generations.ps1)" "test_memory_generations.ps1" | Out-Null
    Run-Suite ("Endurance (" + $EnduranceSec + "s sustained load)") "test_endurance.ps1" @("-DurationSec", $EnduranceSec) | Out-Null
}

Write-Host ""
Write-Host "==================================" -ForegroundColor Cyan
Write-Host " ALL SUITES SUMMARY" -ForegroundColor Cyan
Write-Host "==================================" -ForegroundColor Cyan
$failed = 0
foreach ($r in $results) {
    $tag = if ($r.ExitCode -eq 0) { "PASS" } else { "FAIL"; $failed++ }
    $color = if ($r.ExitCode -eq 0) { "Green" } else { "Red" }
    Write-Host ("  {0,-50} {1}" -f $r.Suite, $tag) -ForegroundColor $color
}

if ($failed -gt 0) { exit 1 }
exit 0
