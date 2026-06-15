# Sustained-load endurance test using native bench_client.exe.
# Avoids PowerShell Start-Job pitfalls (which can hang in cleanup).
param(
    [int]$Port = 19982,
    [int]$DurationSec = 60,
    [int]$Workers = 4,
    [int]$SampleSec = 10,
    [string]$Exe = (Join-Path (Split-Path -Parent $PSScriptRoot) "native\dst_server.exe"),
    [string]$Cache = (Join-Path (Split-Path -Parent $PSScriptRoot) "translation_memory_c.tsv")
)

$ErrorActionPreference = "Stop"
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$bench = Join-Path $here "bench_client.exe"

# Build bench client if missing or older than source
$benchSrc = Join-Path $here "bench_client.c"
if (-not (Test-Path $bench) -or (Get-Item $benchSrc).LastWriteTime -gt (Get-Item $bench).LastWriteTime) {
    Write-Host "Building bench_client..." -ForegroundColor Cyan
    $root = Split-Path -Parent $here
    $binDir = Join-Path $root "native\toolchain\w64devkit\bin"
    $env:PATH = "$binDir;$env:PATH"
    & (Join-Path $binDir "gcc.exe") -O2 $benchSrc -lws2_32 -o $bench
    if (-not (Test-Path $bench)) { throw "bench_client build failed" }
}

Write-Host "Starting server on port $Port..." -ForegroundColor Cyan
$p = Start-Process -FilePath $Exe -ArgumentList @("--port", $Port, "--cache", $Cache) -PassThru -WindowStyle Hidden
Start-Sleep -Milliseconds 1500

# Start load generator as a child process; redirect stdout so we can read req count.
$benchOut = New-TemporaryFile
$benchProc = Start-Process -FilePath $bench `
    -ArgumentList @("--port", $Port, "--threads", $Workers, "--duration", $DurationSec) `
    -PassThru -WindowStyle Hidden `
    -RedirectStandardOutput $benchOut.FullName

# Sample memory/handles
$samples = @()
$t0 = Get-Date
$deadline = $t0.AddSeconds($DurationSec)
$proc0 = Get-Process -Id $p.Id
$mem0 = $proc0.WorkingSet64
$handles0 = $proc0.HandleCount
$threads0 = $proc0.Threads.Count
Write-Host ("t=  0s  mem={0:N1}MB  handles={1}  threads={2}" -f ($mem0/1MB), $handles0, $threads0)

while ((Get-Date) -lt $deadline) {
    Start-Sleep -Seconds $SampleSec
    $proc = Get-Process -Id $p.Id -ErrorAction SilentlyContinue
    if (-not $proc) { break }
    $elapsed = [int]((Get-Date) - $t0).TotalSeconds
    $mem = $proc.WorkingSet64
    $handles = $proc.HandleCount
    $threads = $proc.Threads.Count
    $samples += [pscustomobject]@{ ElapsedSec = $elapsed; MemMB = [math]::Round($mem/1MB, 1); Handles = $handles; Threads = $threads }
    Write-Host ("t={0,3}s  mem={1,5:N1}MB  handles={2,4}  threads={3,3}" -f $elapsed, ($mem/1MB), $handles, $threads)
}

# Wait for bench process to exit naturally
$benchProc | Wait-Process -Timeout 15 -ErrorAction SilentlyContinue
$totalReqs = 0
if (Test-Path $benchOut.FullName) {
    $totalReqs = [int]((Get-Content $benchOut.FullName | Select-Object -First 1) -as [int])
    Remove-Item $benchOut.FullName -ErrorAction SilentlyContinue
}

# Final sample (after workers stop, before server shutdown)
$procF = Get-Process -Id $p.Id -ErrorAction SilentlyContinue
$memF = if ($procF) { $procF.WorkingSet64 } else { 0 }
$handlesF = if ($procF) { $procF.HandleCount } else { 0 }
$threadsF = if ($procF) { $procF.Threads.Count } else { 0 }

# Shutdown server
try { Invoke-WebRequest -Uri "http://127.0.0.1:$Port/shutdown" -Method Post -TimeoutSec 3 -UseBasicParsing | Out-Null } catch { }
$p | Wait-Process -Timeout 5 -ErrorAction SilentlyContinue
$exitCode = $p.ExitCode

Write-Host ""
Write-Host "===============================" -ForegroundColor Cyan
Write-Host "Endurance test summary" -ForegroundColor Cyan
Write-Host "===============================" -ForegroundColor Cyan
Write-Host ("Duration:         {0}s" -f $DurationSec)
Write-Host ("Workers:          {0}" -f $Workers)
Write-Host ("Total requests:   {0:N0}" -f $totalReqs)
Write-Host ("Avg rps:          {0:N0}" -f ($(if ($DurationSec -gt 0) { $totalReqs / $DurationSec } else { 0 })))
Write-Host ""
Write-Host ("Memory  start:    {0:N1} MB" -f ($mem0/1MB))
Write-Host ("Memory  final:    {0:N1} MB" -f ($memF/1MB))
$memDelta = ($memF - $mem0)/1MB
Write-Host ("Memory  delta:    {0:N2} MB" -f $memDelta)
Write-Host ""
Write-Host ("Handles start:    {0}" -f $handles0)
Write-Host ("Handles final:    {0}" -f $handlesF)
$handleDelta = $handlesF - $handles0
Write-Host ("Handles delta:    {0:+#;-#;0}" -f $handleDelta)
Write-Host ""
Write-Host ("Threads start:    {0}" -f $threads0)
Write-Host ("Threads final:    {0}" -f $threadsF)
$threadDelta = $threadsF - $threads0
Write-Host ("Threads delta:    {0:+#;-#;0}" -f $threadDelta)
Write-Host ""
Write-Host ("Exit code:        {0}" -f $exitCode)

# Verdict
$leakSuspected = $false
if ($memDelta -gt 50) { Write-Host "WARN: memory grew >50MB" -ForegroundColor Yellow; $leakSuspected = $true }
if ($handleDelta -gt 100) { Write-Host "WARN: handle count grew >100" -ForegroundColor Yellow; $leakSuspected = $true }
if ($threadDelta -gt 20) { Write-Host "WARN: thread count grew >20" -ForegroundColor Yellow; $leakSuspected = $true }
if ($exitCode -ne 0) { Write-Host "WARN: non-zero exit code" -ForegroundColor Yellow; $leakSuspected = $true }
if (-not $leakSuspected) { Write-Host "VERDICT: stable" -ForegroundColor Green; exit 0 }
exit 1
