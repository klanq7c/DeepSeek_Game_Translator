# Multi-wave idle-memory test for bounded process-lifetime pools.
param(
    [int]$Port = 19983,
    [int]$Waves = 3,
    [int]$WaveDurationSec = 8,
    [int]$Workers = 8,
    [int]$IdleSec = 3,
    [string]$Exe = (Join-Path (Split-Path -Parent $PSScriptRoot) "native\dst_server.exe"),
    [string]$Cache = (Join-Path (Split-Path -Parent $PSScriptRoot) "translation_memory_c.tsv")
)

$ErrorActionPreference = "Stop"
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$root = Split-Path -Parent $here
$bench = Join-Path $here "bench_client.exe"
$benchSrc = Join-Path $here "bench_client.c"
$binDir = Join-Path $root "native\toolchain\w64devkit\bin"

if (-not (Test-Path $bench) -or (Get-Item $benchSrc).LastWriteTime -gt (Get-Item $bench).LastWriteTime) {
    $env:PATH = "$binDir;$env:PATH"
    & (Join-Path $binDir "gcc.exe") -O2 $benchSrc -lws2_32 -o $bench
    if ($LASTEXITCODE -ne 0 -or -not (Test-Path $bench)) { throw "bench_client build failed" }
}

function Get-ServerSample([int]$ProcessId, [int]$Wave) {
    $proc = Get-Process -Id $ProcessId -ErrorAction Stop
    $health = Invoke-WebRequest -Uri "http://127.0.0.1:$Port/health" -TimeoutSec 5 -UseBasicParsing
    $json = $health.Content | ConvertFrom-Json
    if ($json.request_buffer_pool_cached -gt $json.request_buffer_pool_limit) {
        throw "request buffer pool exceeded cap after wave $Wave"
    }
    return [pscustomobject]@{
        Wave = $Wave
        WorkingSetMB = [math]::Round($proc.WorkingSet64 / 1MB, 2)
        PrivateMB = [math]::Round($proc.PrivateMemorySize64 / 1MB, 2)
        Handles = $proc.HandleCount
        Threads = $proc.Threads.Count
        PoolCached = [int]$json.request_buffer_pool_cached
        PoolLimit = [int]$json.request_buffer_pool_limit
        PoolHits = [long]$json.request_buffer_pool_hits
        PoolMisses = [long]$json.request_buffer_pool_misses
    }
}

$server = $null
$loadProcesses = @()
$tempFiles = @()
$samples = @()
$totalRequests = 0L

try {
    Write-Host "Starting generational memory test on port $Port..." -ForegroundColor Cyan
    $server = Start-Process -FilePath $Exe -ArgumentList @("--port", $Port, "--cache", $Cache) -PassThru -WindowStyle Hidden
    Start-Sleep -Milliseconds 1500
    $baseline = Get-ServerSample -ProcessId $server.Id -Wave 0

    for ($wave = 1; $wave -le $Waves; $wave++) {
        $benchOut = New-TemporaryFile
        $tempFiles += $benchOut.FullName
        $load = Start-Process -FilePath $bench `
            -ArgumentList @("--port", $Port, "--threads", $Workers, "--duration", $WaveDurationSec) `
            -PassThru -WindowStyle Hidden -RedirectStandardOutput $benchOut.FullName
        $loadProcesses += $load
        $load | Wait-Process -Timeout ($WaveDurationSec + 15)
        $requestCount = [long]((Get-Content $benchOut.FullName | Select-Object -First 1) -as [long])
        $totalRequests += $requestCount

        Start-Sleep -Seconds $IdleSec
        $sample = Get-ServerSample -ProcessId $server.Id -Wave $wave
        $samples += $sample
        Write-Host ("wave={0} requests={1:N0} idle_ws={2:N2}MB idle_private={3:N2}MB handles={4} threads={5} pool={6}/{7}" -f `
            $wave, $requestCount, $sample.WorkingSetMB, $sample.PrivateMB, $sample.Handles, $sample.Threads, $sample.PoolCached, $sample.PoolLimit)
    }

    if ($samples.Count -lt 2) { throw "at least two waves are required" }
    $first = $samples[0]
    $last = $samples[$samples.Count - 1]
    $privateGrowth = $last.PrivateMB - $first.PrivateMB
    $workingSetGrowth = $last.WorkingSetMB - $first.WorkingSetMB
    $handleGrowth = $last.Handles - $first.Handles
    $threadGrowth = $last.Threads - $first.Threads

    if ($last.PoolHits -le $baseline.PoolHits) { throw "request buffer pool did not record reuse across waves" }
    if ($privateGrowth -gt 12) { throw ("idle private memory grew {0:N2} MB from first to last wave" -f $privateGrowth) }
    if ($workingSetGrowth -gt 16) { throw ("idle working set grew {0:N2} MB from first to last wave" -f $workingSetGrowth) }
    if ($handleGrowth -gt 16) { throw "idle handle count kept growing across waves" }
    if ($threadGrowth -gt 8) { throw "idle thread count kept growing across waves" }

    Write-Host ""
    Write-Host ("Total requests: {0:N0}" -f $totalRequests)
    Write-Host ("First-to-last idle private growth: {0:N2} MB" -f $privateGrowth)
    Write-Host ("First-to-last idle working-set growth: {0:N2} MB" -f $workingSetGrowth)
    Write-Host "VERDICT: bounded across generations" -ForegroundColor Green
} finally {
    foreach ($load in $loadProcesses) {
        if (-not $load.HasExited) {
            Stop-Process -Id $load.Id -Force -ErrorAction SilentlyContinue
        }
    }
    if ($server -and -not $server.HasExited) {
        try {
            Invoke-WebRequest -Uri "http://127.0.0.1:$Port/shutdown" -Method Post -TimeoutSec 3 -UseBasicParsing | Out-Null
            $server | Wait-Process -Timeout 5 -ErrorAction SilentlyContinue
        } catch {
        }
    }
    if ($server -and -not $server.HasExited) {
        Stop-Process -Id $server.Id -Force -ErrorAction SilentlyContinue
    }
    foreach ($path in $tempFiles) {
        Remove-Item -LiteralPath $path -ErrorAction SilentlyContinue
    }
}

exit 0
