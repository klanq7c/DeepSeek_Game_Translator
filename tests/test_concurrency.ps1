# Regression test: API calls must run concurrently across the channel pool.
# A slow fake API (1200ms per call) serves 6 parallel single-text misses; with
# the default full-width channel pool the wall time must stay well below the
# ~7.2s a fully serialized API path would need.
param(
    [int]$Port = 19985,
    [int]$FakeApiPort = 0,
    [string]$Exe = (Join-Path (Split-Path -Parent $PSScriptRoot) "native\dst_server.exe")
)

$ErrorActionPreference = "Stop"
Add-Type -AssemblyName System.Net.Http
$script:Pass = 0
$script:Fail = 0
$script:Errors = @()

function It([string]$name, [scriptblock]$body) {
    try {
        & $body
        $script:Pass++
        Write-Host ("  PASS  " + $name) -ForegroundColor Green
    } catch {
        $script:Fail++
        $script:Errors += "$name : $_"
        Write-Host ("  FAIL  " + $name + " :: " + $_) -ForegroundColor Red
    }
}

function Send-Json([string]$path, [string]$method = "GET", $body = $null, [int]$timeout = 5) {
    $u = "http://127.0.0.1:$Port$path"
    if ($body) {
        $r = Invoke-WebRequest -Uri $u -Method $method -Body $body -ContentType "application/json" -TimeoutSec $timeout -UseBasicParsing
    } else {
        $r = Invoke-WebRequest -Uri $u -Method $method -TimeoutSec $timeout -UseBasicParsing
    }
    return [pscustomobject]@{
        Status = $r.StatusCode
        Body = $r.Content
        Json = ($r.Content | ConvertFrom-Json -ErrorAction SilentlyContinue)
    }
}

function Wait-Server {
    for ($i = 0; $i -lt 30; $i++) {
        try {
            $r = Send-Json "/health" "GET" $null 1
            if ($r.Status -eq 200) { return }
        } catch {
            Start-Sleep -Milliseconds 100
        }
    }
    throw "server did not start"
}

function Get-FreeTcpPort {
    $listener = [System.Net.Sockets.TcpListener]::new([System.Net.IPAddress]::Parse("127.0.0.1"), 0)
    $listener.Start()
    try {
        return ([System.Net.IPEndPoint]$listener.LocalEndpoint).Port
    } finally {
        $listener.Stop()
    }
}

if ($FakeApiPort -le 0) {
    $FakeApiPort = Get-FreeTcpPort
}

$tmp = Join-Path $env:TEMP ("dst_concurrency_" + [guid]::NewGuid().ToString("N"))
New-Item -ItemType Directory -Path $tmp | Out-Null
$cache = Join-Path $tmp "cache.tsv"
$apiConfig = Join-Path $tmp "api.ini"
$serverStderr = Join-Path $tmp "server.stderr.log"
Set-Content -LiteralPath $apiConfig -Encoding ASCII -Value @"
[api]
endpoint=http://127.0.0.1:$FakeApiPort/v1/chat/completions
model=fake-model
key=fake-key
timeout_ms=15000
"@

$ApiDelayMs = 1200

# Slow fake API: a TcpListener shared by several handler runspaces so requests
# are served concurrently. Each call sleeps, then echoes "ZH|"+input back as a
# successful translation.
$listener = [System.Net.Sockets.TcpListener]::new([System.Net.IPAddress]::Parse("127.0.0.1"), $FakeApiPort)
$listener.Start()

$startedCalls = [System.Collections.Concurrent.ConcurrentQueue[datetime]]::new()
$completedCalls = [System.Collections.Concurrent.ConcurrentQueue[datetime]]::new()

$handlerScript = {
    param($Listener, [int]$DelayMs, $StartedCalls, $CompletedCalls)

    function Read-HttpRequestBody($stream) {
        $reader = New-Object System.IO.StreamReader($stream, [System.Text.Encoding]::ASCII, $false, 4096, $true)
        $first = $reader.ReadLine()
        if ($null -eq $first) { return "" }
        $length = 0
        $expectContinue = $false
        while ($true) {
            $line = $reader.ReadLine()
            if ($null -eq $line -or $line -eq "") { break }
            if ($line.StartsWith("Content-Length:", [System.StringComparison]::OrdinalIgnoreCase)) {
                [int]::TryParse($line.Substring(15).Trim(), [ref]$length) | Out-Null
            }
            if ($line.StartsWith("Expect:", [System.StringComparison]::OrdinalIgnoreCase) -and
                $line.IndexOf("100-continue", [System.StringComparison]::OrdinalIgnoreCase) -ge 0) {
                $expectContinue = $true
            }
        }
        if ($expectContinue) {
            $continueBytes = [System.Text.Encoding]::ASCII.GetBytes("HTTP/1.1 100 Continue`r`n`r`n")
            $stream.Write($continueBytes, 0, $continueBytes.Length)
            $stream.Flush()
        }
        if ($length -le 0) { return "" }
        $buf = New-Object char[] $length
        $read = 0
        while ($read -lt $length) {
            $n = $reader.Read($buf, $read, $length - $read)
            if ($n -le 0) { break }
            $read += $n
        }
        if ($read -le 0) { return "" }
        return -join ($buf[0..($read - 1)])
    }

    function Get-TranslatedContent([string]$body) {
        try {
            $obj = $body | ConvertFrom-Json
            $msg = @($obj.messages | Where-Object { $_.role -eq "user" })[-1]
            $content = [string]$msg.content
            $trimmed = $content.Trim()
            if ($trimmed.StartsWith("[")) {
                $items = $trimmed | ConvertFrom-Json
                $out = @($items | ForEach-Object { "ZH|" + $_ })
                return ConvertTo-Json -InputObject $out -Compress
            }
            $parts = $content -split "`n", 2
            if ($parts.Count -gt 1) { return "ZH|" + $parts[1] }
            return "ZH|" + $content
        } catch {
            return "ZH|unparsed"
        }
    }

    while ($true) {
        try {
            $client = $Listener.AcceptTcpClient()
        } catch {
            break
        }
        try {
            $stream = $client.GetStream()
            $content = Get-TranslatedContent (Read-HttpRequestBody $stream)
            $StartedCalls.Enqueue([datetime]::UtcNow)
            Start-Sleep -Milliseconds $DelayMs
            $payload = @{ choices = @(@{ message = @{ content = $content } }) } | ConvertTo-Json -Depth 5 -Compress
            $bodyBytes = [System.Text.Encoding]::UTF8.GetBytes($payload)
            $header = "HTTP/1.1 200 OK`r`nContent-Type: application/json; charset=utf-8`r`nContent-Length: $($bodyBytes.Length)`r`nConnection: close`r`n`r`n"
            $headerBytes = [System.Text.Encoding]::ASCII.GetBytes($header)
            $stream.Write($headerBytes, 0, $headerBytes.Length)
            $stream.Write($bodyBytes, 0, $bodyBytes.Length)
            $CompletedCalls.Enqueue([datetime]::UtcNow)
        } catch {
        } finally {
            $client.Close()
        }
    }
}

$handlers = @()
for ($i = 0; $i -lt 8; $i++) {
    $ps = [powershell]::Create()
    $ps.AddScript($handlerScript).AddArgument($listener).AddArgument($ApiDelayMs).AddArgument($startedCalls).AddArgument($completedCalls) | Out-Null
    $handlers += [pscustomobject]@{ Shell = $ps; Handle = $ps.BeginInvoke() }
}

$server = $null
$client = $null
try {
    Write-Host "Starting server on port $Port with slow fake API on $FakeApiPort..." -ForegroundColor Cyan
    $server = Start-Process -FilePath $Exe -ArgumentList @("--port", $Port, "--cache", $cache, "--api-config", $apiConfig) -PassThru -WindowStyle Hidden -RedirectStandardError $serverStderr
    Wait-Server

    Write-Host ""
    Write-Host "=== API channel pool concurrency ===" -ForegroundColor Cyan

    [System.Net.ServicePointManager]::DefaultConnectionLimit = 64
    $client = [System.Net.Http.HttpClient]::new()
    $client.Timeout = [TimeSpan]::FromSeconds(30)

    $texts = @()
    for ($i = 0; $i -lt 6; $i++) { $texts += "concurrency_probe_${i}_$(Get-Random)" }
    $script:Elapsed = $null
    $script:Responses = @()

    It "6 parallel misses finish well under serialized time" {
        $sw = [System.Diagnostics.Stopwatch]::StartNew()
        $tasks = @()
        foreach ($t in $texts) {
            $content = [System.Net.Http.StringContent]::new('{"texts":["' + $t + '"]}', [System.Text.Encoding]::UTF8, "application/json")
            $tasks += $client.PostAsync("http://127.0.0.1:$Port/batch", $content)
        }
        if (-not [System.Threading.Tasks.Task]::WaitAll($tasks, 25000)) { throw "requests timed out" }
        $sw.Stop()
        $script:Elapsed = $sw.ElapsedMilliseconds
        foreach ($task in $tasks) {
            $script:Responses += ($task.Result.Content.ReadAsStringAsync().Result | ConvertFrom-Json)
        }
        Write-Host ("        elapsed: " + $script:Elapsed + " ms (serialized would be >= " + (6 * $ApiDelayMs) + " ms)")
        # 6 calls x 900ms: serial >= 5400ms, full pool ~ 900ms. 3000ms keeps
        # margin for slow machines while still failing if the pool serializes.
        if ($script:Elapsed -ge 3000) { throw "took $($script:Elapsed)ms; API calls appear serialized" }
    }

    It "every parallel miss got a real translation" {
        if ($script:Responses.Count -ne 6) { throw "expected 6 responses, got $($script:Responses.Count)" }
        for ($i = 0; $i -lt 6; $i++) {
            $r = $script:Responses[$i]
            $expected = "ZH|" + $texts[$i]
            if ($r.results[0] -ne $expected) { throw "result $i : expected '$expected' got '$($r.results[0])'" }
            if ($r.sources[0] -notin @("api", "api_batch")) { throw "source $i : got '$($r.sources[0])'" }
        }
    }

    It "one large live batch uses multiple API channels" {
        $largeTexts = @()
        for ($i = 0; $i -lt 96; $i++) {
            $largeTexts += "large_live_batch_${i}_$(Get-Random)"
        }
        $body = '{"texts":' + (ConvertTo-Json $largeTexts -Compress) + '}'
        $sw = [System.Diagnostics.Stopwatch]::StartNew()
        $response = Send-Json "/batch" "POST" $body 10
        $sw.Stop()
        Write-Host ("        96-text live batch elapsed: " + $sw.ElapsedMilliseconds + " ms")
        if ($response.Json.results.Count -ne 96) {
            throw "expected 96 batch results, got $($response.Json.results.Count)"
        }
        # Two 48-item provider batches cost 2400 ms when serialized. The live
        # worker pool should dispatch both through separate API channels.
        if ($sw.ElapsedMilliseconds -ge 1850) {
            throw "large live batch took $($sw.ElapsedMilliseconds)ms; remote chunks were serialized"
        }
    }

    It "duplicate live texts consume one provider slot" {
        $duplicateText = "duplicate_live_text_$(Get-Random)"
        $duplicates = @()
        for ($i = 0; $i -lt 64; $i++) { $duplicates += $duplicateText }
        $startedBefore = $startedCalls.Count
        $body = '{"texts":' + (ConvertTo-Json $duplicates -Compress) + '}'
        $response = Send-Json "/batch" "POST" $body 10
        $startedDelta = $startedCalls.Count - $startedBefore
        if ($response.Json.results.Count -ne 64) {
            throw "expected 64 duplicate results, got $($response.Json.results.Count)"
        }
        if ($startedDelta -ne 1) {
            throw "duplicate texts used $startedDelta provider calls instead of one"
        }
    }

    It "all parallel translations were cached" {
        $body = '{"texts":' + (ConvertTo-Json $texts -Compress) + '}'
        $lookup = Send-Json "/cache/lookup" "POST" $body
        if ($lookup.Json.hit_count -ne 6) { throw "expected 6 cache hits, got $($lookup.Json.hit_count)" }
    }

    It "foreground translation keeps a channel while prefetch is saturated" {
        $startedBefore = $startedCalls.Count
        $backgroundTexts = @()
        for ($i = 0; $i -lt 384; $i++) {
            $backgroundTexts += "background_saturation_${i}_$(Get-Random)"
        }
        $prefetchBody = '{"texts":' + (ConvertTo-Json $backgroundTexts -Compress) + '}'
        $prefetch = Send-Json "/prefetch" "POST" $prefetchBody 5
        if ($prefetch.Status -ne 200) { throw "prefetch status $($prefetch.Status)" }

        $deadline = [datetime]::UtcNow.AddSeconds(5)
        while ($startedCalls.Count -lt $startedBefore + 7 -and [datetime]::UtcNow -lt $deadline) {
            Start-Sleep -Milliseconds 20
        }
        if ($startedCalls.Count -lt $startedBefore + 7) {
            throw "background workers did not saturate the API pool"
        }
        Start-Sleep -Milliseconds 100

        $foregroundText = "foreground_reserved_channel_$(Get-Random)"
        $body = '{"texts":["' + $foregroundText + '"]}'
        $sw = [System.Diagnostics.Stopwatch]::StartNew()
        $response = Send-Json "/batch" "POST" $body 10
        $sw.Stop()
        Write-Host ("        foreground elapsed under saturated prefetch: " + $sw.ElapsedMilliseconds + " ms")
        if ($response.Json.results[0] -ne "ZH|$foregroundText") {
            throw "foreground translation did not resolve"
        }
        # One remote call costs 1200 ms. If all eight channels belong to
        # prefetch, the foreground waits for one and approaches 2400 ms.
        if ($sw.ElapsedMilliseconds -ge 1850) {
            throw "foreground waited $($sw.ElapsedMilliseconds)ms behind prefetch; no API channel was reserved"
        }
    }

    It "background prefetch yields to foreground translation work" {
        $root = Split-Path -Parent $PSScriptRoot
        $httpSrc = Get-Content -LiteralPath (Join-Path $root "native\src\server\http.c") -Raw
        $apiSrc = Get-Content -LiteralPath (Join-Path $root "native\src\server\api.c") -Raw
        $cacheSrc = Get-Content -LiteralPath (Join-Path $root "native\src\server\cache.c") -Raw
        if ($apiSrc -notmatch 'cfg->concurrency = API_CONCURRENCY_MAX;') {
            throw "server default concurrency must use the full API channel pool"
        }
        if ($httpSrc -notmatch 'g_foreground_requests') {
            throw "server must track foreground translation requests"
        }
        if ($httpSrc -notmatch 'async_wait_for_foreground\(\);') {
            throw "background prefetch workers must yield before API calls"
        }
        if ($httpSrc -notmatch '(?s)static int async_worker_pool_size\(void\).*?return n > 1 \? n - 1 : 1;') {
            throw "background prefetch must reserve one configured API channel for visible text"
        }
        if ($httpSrc -notmatch '(?s)static char \*live_translate_batched.*?foreground_enter\(\).*?foreground_leave\(\)') {
            throw "single live translations must hold foreground priority while waiting"
        }
        if ($httpSrc -notmatch '(?s)static void live_translate_group.*?foreground_enter\(\).*?foreground_leave\(\)' -or
            $httpSrc -notmatch '(?s)static void op_batch.*?live_translate_group\(texts, miss_n, translated\);') {
            throw "batch live translations must use the prioritized worker pool"
        }
        if ($cacheSrc -notmatch '(?s)void cache_set_many_persist.*?AcquireSRWLockExclusive\(&c->io_lock\);.*?AcquireSRWLockExclusive\(&c->lock\);.*?ReleaseSRWLockExclusive\(&c->lock\);.*?if \(wrote && fflush\(f\) != 0\).*?ReleaseSRWLockExclusive\(&c->io_lock\);' -or
            $httpSrc -notmatch 'cache_set_many_persist\(g_ctx->cache, persist_keys, persist_values, persist_n\);') {
            throw "provider batches must update the cache under one map lock and one disk flush"
        }
    }
} finally {
    try { if ($client) { $client.Dispose() } } catch { }
    try { if ($server -and -not $server.HasExited) { Send-Json "/shutdown" "POST" "{}" 2 | Out-Null } } catch { }
    try { if ($server -and -not $server.HasExited) { $server.WaitForExit(3000) | Out-Null } } catch { }
    try { if ($server -and -not $server.HasExited) { Stop-Process -Id $server.Id -Force -ErrorAction SilentlyContinue } } catch { }
    if ($script:Fail -gt 0 -and (Test-Path -LiteralPath $serverStderr)) {
        Write-Host "--- server diagnostics ---" -ForegroundColor Yellow
        Get-Content -LiteralPath $serverStderr | Select-Object -Last 30 | ForEach-Object { Write-Host $_ }
    }
    try { $listener.Stop() } catch { }
    foreach ($h in $handlers) {
        try { $h.Shell.Stop() } catch { }
        try { $h.Shell.Dispose() } catch { }
    }
    Remove-Item -LiteralPath $tmp -Recurse -Force -ErrorAction SilentlyContinue
}

Write-Host ""
Write-Host "============================="
Write-Host ("Pass: {0}   Fail: {1}" -f $script:Pass, $script:Fail)
if ($script:Fail -gt 0) {
    $script:Errors | ForEach-Object { Write-Host $_ -ForegroundColor Red }
    exit 1
}
exit 0
