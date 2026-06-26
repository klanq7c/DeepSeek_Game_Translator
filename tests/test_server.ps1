# End-to-end test for dst_server.exe.
param(
    [int]$Port = 19981,
    [string]$Exe = (Join-Path (Split-Path -Parent $PSScriptRoot) "native\dst_server.exe"),
    [string]$Cache = (Join-Path (Split-Path -Parent $PSScriptRoot) "translation_memory_c.tsv")
)

$ErrorActionPreference = "Stop"
$script:Pass = 0
$script:Fail = 0
$script:Errors = @()
$httpSourcePath = Join-Path (Split-Path -Parent $PSScriptRoot) "native\src\server\http.c"
$httpSource = Get-Content -LiteralPath $httpSourcePath -Raw
$utilSource = Get-Content -LiteralPath (Join-Path (Split-Path -Parent $PSScriptRoot) "native\src\server\util.c") -Raw
$bufSource = Get-Content -LiteralPath (Join-Path (Split-Path -Parent $PSScriptRoot) "native\src\server\buf.c") -Raw
$jsonSource = Get-Content -LiteralPath (Join-Path (Split-Path -Parent $PSScriptRoot) "native\src\server\json.c") -Raw

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

function Assert-Eq($actual, $expected, $msg) {
    if ($actual -ne $expected) { throw "$msg : expected '$expected' got '$actual'" }
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

Write-Host "Starting server on port $Port..." -ForegroundColor Cyan
$p = Start-Process -FilePath $Exe -ArgumentList @("--port", $Port, "--cache", $Cache) -PassThru -WindowStyle Hidden
Start-Sleep -Milliseconds 1500

$testKey1 = "__test_e2e_key1_$(Get-Random)"
$testKey2 = "__test_e2e_key2_$(Get-Random)"

try {
    Write-Host ""
    Write-Host "=== Health & capabilities ===" -ForegroundColor Cyan

    It "GET /health returns 200 with status ok" {
        $r = Send-Json "/health"
        Assert-Eq $r.Status 200 "status"
        Assert-Eq $r.Json.status "ok" "status field"
        if ($r.Json.cache_size -lt 0) { throw "cache_size negative" }
        if ($r.Json.runtime_cache_only -ne $false) { throw "runtime_cache_only flag" }
        $healthFields = $r.Json.PSObject.Properties.Name
        if ($healthFields -notcontains "request_buffer_pool_cached") { throw "request buffer pool cached count missing" }
        if ($healthFields -notcontains "request_buffer_pool_limit") { throw "request buffer pool limit missing" }
        if ($healthFields -notcontains "request_buffer_pool_hits") { throw "request buffer pool hit count missing" }
        if ($healthFields -notcontains "request_buffer_pool_misses") { throw "request buffer pool miss count missing" }
        if ($r.Json.request_buffer_pool_limit -le 0 -or $r.Json.request_buffer_pool_limit -gt 64) { throw "request buffer pool limit outside bounded range" }
        if ($r.Json.request_buffer_pool_cached -lt 0 -or $r.Json.request_buffer_pool_cached -gt $r.Json.request_buffer_pool_limit) { throw "request buffer pool cached count outside bounded range" }
        if ($r.Json.request_buffer_pool_hits -lt 0) { throw "request buffer pool hits negative" }
        if ($r.Json.request_buffer_pool_misses -lt 0) { throw "request buffer pool misses negative" }
    }

    It "GET /capabilities returns batch+cache_only" {
        $r = Send-Json "/capabilities"
        Assert-Eq $r.Status 200 "status"
        Assert-Eq $r.Json.supports.batch $true "batch"
        Assert-Eq $r.Json.supports.cache_only $true "cache_only"
        Assert-Eq $r.Json.supports.xunity_batch_endpoint $true "xunity_batch_endpoint"
    }

    It "Request buffer pool retains only fixed initial blocks" {
        if ($httpSource -notmatch 'if \(capacity != HTTP_RECV_INITIAL\)\s*\{\s*free\(buffer\);\s*return;') {
            throw "expanded request buffers must bypass the pool"
        }
        if ($httpSource -notmatch 'request_buffer_release\(req, cap\);') {
            throw "connection cleanup must return the buffer with its actual capacity"
        }
        if ($httpSource -notmatch '#define HTTP_REQUEST_BUFFER_POOL_LIMIT\s+\d+') {
            throw "request buffer pool must have a compile-time hard cap"
        }
    }

    It "C allocation and parser helpers guard overflow and malformed ownership" {
        if ($utilSource -notmatch 'void \*xcalloc\(size_t count, size_t size\)') {
            throw "server allocations need one checked calloc helper"
        }
        if ($bufSource -notmatch 'SIZE_MAX - b->len - 1') {
            throw "dynamic buffer growth must reject size_t addition overflow"
        }
        if ($jsonSource -notmatch 'list_free\(&l\);\s*return l;') {
            throw "malformed arrays must release partial elements"
        }
        if ($jsonSource -notmatch 'if \(!closed \|\| invalid\)') {
            throw "JSON strings must require a closing quote and valid escapes"
        }
    }

    It "Socket timeouts are mandatory for connection worker lifetime" {
        if ($httpSource -notmatch 'if \(setsockopt\(s, SOL_SOCKET, SO_RCVTIMEO') {
            throw "receive timeout failure must terminate the connection"
        }
        if ($httpSource -notmatch 'setsockopt\(s, SOL_SOCKET, SO_SNDTIMEO') {
            throw "send timeout failure must terminate the connection"
        }
    }

    Write-Host ""
    Write-Host "=== Translate single ===" -ForegroundColor Cyan

    It "POST /translate {text:x} returns translation/translated_text/source" {
        $body = '{"text":"definitely_unknown_xyz_zzz"}'
        $r = Send-Json "/translate" "POST" $body
        Assert-Eq $r.Status 200 "status"
        if (-not $r.Json.translation) { throw "no translation field" }
        if (-not $r.Json.translated_text) { throw "no translated_text field" }
        Assert-Eq $r.Json.source "miss" "source"
    }

    It "POST /translate missing text returns 400" {
        $caught = $false
        try { $null = Send-Json "/translate" "POST" '{}' }
        catch [System.Net.WebException] {
            $code = [int]$_.Exception.Response.StatusCode
            Assert-Eq $code 400 "expected 400"
            $caught = $true
        }
        if (-not $caught) { throw "did not raise" }
    }

    It "POST /translate empty body returns 400" {
        $caught = $false
        try { $null = Send-Json "/translate" "POST" '' }
        catch [System.Net.WebException] {
            $code = [int]$_.Exception.Response.StatusCode
            Assert-Eq $code 400 "expected 400"
            $caught = $true
        }
        if (-not $caught) { throw "did not raise" }
    }

    Write-Host ""
    Write-Host "=== Cache import + lookup ===" -ForegroundColor Cyan

    It "POST /cache/import accepts entries" {
        $body = '{"entries":[{"key":"' + $testKey1 + '","value":"value_one"},{"key":"' + $testKey2 + '","value":"value_two"}]}'
        $r = Send-Json "/cache/import" "POST" $body
        Assert-Eq $r.Status 200 "status"
        Assert-Eq $r.Json.status "ok" "status field"
        Assert-Eq $r.Json.imported 2 "imported count"
    }

    It "POST /cache/lookup returns hits for imported keys" {
        $body = '{"texts":["' + $testKey1 + '","' + $testKey2 + '","_miss_xyz"]}'
        $r = Send-Json "/cache/lookup" "POST" $body
        Assert-Eq $r.Status 200 "status"
        Assert-Eq $r.Json.hit_count 2 "hit count"
        Assert-Eq $r.Json.miss_count 1 "miss count"
        Assert-Eq $r.Json.hits.$testKey1 "value_one" "first hit value"
        Assert-Eq $r.Json.hits.$testKey2 "value_two" "second hit value"
    }

    It "POST /translate of imported key returns cached value" {
        $body = '{"text":"' + $testKey1 + '"}'
        $r = Send-Json "/translate" "POST" $body
        Assert-Eq $r.Status 200 "status"
        Assert-Eq $r.Json.translation "value_one" "translation"
        Assert-Eq $r.Json.source "cache" "source"
    }

    It "GET /translate supports XUnity CustomTranslate query format" {
        $encoded = [uri]::EscapeDataString($testKey2)
        $r = Invoke-WebRequest -Uri "http://127.0.0.1:$Port/translate?from=en&to=zh-CN&text=$encoded" -Method Get -TimeoutSec 5 -UseBasicParsing
        Assert-Eq $r.StatusCode 200 "status"
        Assert-Eq $r.Content "value_two" "plain translation"
        if ($r.Headers["Content-Type"] -notmatch "text/plain") { throw "expected text/plain" }
    }

    It "GET /translate live miss fails instead of poisoning XUnity cache" {
        $encoded = [uri]::EscapeDataString("__live_miss_$(Get-Random)")
        $caught = $false
        try {
            $null = Invoke-WebRequest -Uri "http://127.0.0.1:$Port/translate?from=en&to=zh-CN&text=$encoded" -Method Get -TimeoutSec 5 -UseBasicParsing
        } catch [System.Net.WebException] {
            $code = [int]$_.Exception.Response.StatusCode
            Assert-Eq $code 503 "expected 503"
            $caught = $true
        }
        if (-not $caught) { throw "live miss did not fail" }
    }

    It "GET /translate cache_only miss still echoes for launcher lookup" {
        $plain = "__cache_only_miss_$(Get-Random)"
        $encoded = [uri]::EscapeDataString($plain)
        $r = Invoke-WebRequest -Uri "http://127.0.0.1:$Port/translate?cache_only=true&text=$encoded" -Method Get -TimeoutSec 5 -UseBasicParsing
        Assert-Eq $r.StatusCode 200 "status"
        Assert-Eq $r.Content $plain "plain cache miss"
    }

    It "POST /batch with multiple texts returns translations map" {
        $body = '{"texts":["' + $testKey1 + '","' + $testKey2 + '","_miss_xyz"]}'
        $r = Send-Json "/batch" "POST" $body
        Assert-Eq $r.Status 200 "status"
        Assert-Eq $r.Json.translations.$testKey1 "value_one" "key1"
        Assert-Eq $r.Json.translations.$testKey2 "value_two" "key2"
        Assert-Eq $r.Json.translations._miss_xyz "_miss_xyz" "miss echoes input"
        Assert-Eq $r.Json.results[0] "value_one" "ordered result 1"
        Assert-Eq $r.Json.results[1] "value_two" "ordered result 2"
        Assert-Eq $r.Json.results[2] "_miss_xyz" "ordered result 3"
        Assert-Eq $r.Json.sources[0] "cache" "source 1"
        Assert-Eq $r.Json.sources[1] "cache" "source 2"
        Assert-Eq $r.Json.sources[2] "miss" "source 3"
    }

    Write-Host ""
    Write-Host "=== HTTP semantics ===" -ForegroundColor Cyan

    It "GET /unknown returns 404 not_found" {
        $caught = $false
        try { $null = Send-Json "/unknown_route" }
        catch [System.Net.WebException] {
            Assert-Eq ([int]$_.Exception.Response.StatusCode) 404 "expected 404"
            $caught = $true
        }
        if (-not $caught) { throw "did not raise" }
    }

    It "OPTIONS / returns 204 with CORS headers" {
        $cli = New-Object System.Net.Sockets.TcpClient("127.0.0.1", $Port)
        $st = $cli.GetStream()
        $req = "OPTIONS / HTTP/1.1`r`nHost: 127.0.0.1`r`nConnection: close`r`n`r`n"
        $bytes = [System.Text.Encoding]::ASCII.GetBytes($req)
        $st.Write($bytes, 0, $bytes.Length)
        $buf = New-Object byte[] 4096
        $n = $st.Read($buf, 0, $buf.Length)
        $cli.Close()
        $resp = [System.Text.Encoding]::ASCII.GetString($buf, 0, $n)
        if ($resp -notmatch "HTTP/1\.1 204") { throw "expected 204" }
        if ($resp -notmatch "Access-Control-Allow-Origin") { throw "CORS header missing" }
    }

    It "Huge Content-Length is capped, no crash" {
        $cli = New-Object System.Net.Sockets.TcpClient("127.0.0.1", $Port)
        $st = $cli.GetStream()
        $body = '{"text":"x"}'
        $req = "POST /translate HTTP/1.1`r`nHost: 127.0.0.1`r`nContent-Type: application/json`r`nContent-Length: 99999999999999`r`nConnection: close`r`n`r`n" + $body
        $bytes = [System.Text.Encoding]::ASCII.GetBytes($req)
        $st.Write($bytes, 0, $bytes.Length)
        $st.Flush()
        $cli.Client.Shutdown([System.Net.Sockets.SocketShutdown]::Send)
        Start-Sleep -Milliseconds 500
        $buf = New-Object byte[] 8192
        $n = 0
        try { $n = $st.Read($buf, 0, $buf.Length) } catch { }
        $cli.Close()
        $resp = [System.Text.Encoding]::ASCII.GetString($buf, 0, $n)
        if ($resp -notmatch "^HTTP/1\.1") { throw "no HTTP response" }
    }

    It "CJK round-trip via raw socket preserves UTF-8 bytes" {
        function Read-All($stream) {
            $ms = New-Object System.IO.MemoryStream
            $buf = New-Object byte[] 4096
            while (($n = $stream.Read($buf, 0, $buf.Length)) -gt 0) {
                $ms.Write($buf, 0, $n)
            }
            return $ms.ToArray()
        }
        function Send-Raw([string]$path, [byte[]]$bodyBytes) {
            $cli = New-Object System.Net.Sockets.TcpClient("127.0.0.1", $Port)
            $st = $cli.GetStream()
            $hdr = "POST $path HTTP/1.1`r`nHost: 127.0.0.1`r`nContent-Type: application/json; charset=utf-8`r`nContent-Length: $($bodyBytes.Length)`r`nConnection: close`r`n`r`n"
            $hdrBytes = [System.Text.Encoding]::ASCII.GetBytes($hdr)
            $st.Write($hdrBytes, 0, $hdrBytes.Length)
            $st.Write($bodyBytes, 0, $bodyBytes.Length)
            $st.Flush()
            $allBytes = Read-All $st
            $cli.Close()
            return $allBytes
        }
        # 0xE8 0xAF 0x91 0xE6 0x96 0x87 = 译文 (U+8BD1 U+6587)
        $cjkBytes = [byte[]](0xE8, 0xAF, 0x91, 0xE6, 0x96, 0x87)
        $cjkStr = [System.Text.Encoding]::UTF8.GetString($cjkBytes)
        $key = "cjk_$(Get-Random)"

        $imp = '{"entries":[{"key":"' + $key + '","value":"' + $cjkStr + '"}]}'
        $respBytes = Send-Raw "/cache/import" ([System.Text.Encoding]::UTF8.GetBytes($imp))
        $resp = [System.Text.Encoding]::UTF8.GetString($respBytes)
        if ($resp -notmatch '"imported":1') { throw "import failed: $resp" }

        $lkp = '{"texts":["' + $key + '"]}'
        $respBytes = Send-Raw "/cache/lookup" ([System.Text.Encoding]::UTF8.GetBytes($lkp))
        # Verify the CJK bytes appear verbatim in the response body
        $hex = [BitConverter]::ToString($respBytes).Replace("-","")
        $needle = [BitConverter]::ToString($cjkBytes).Replace("-","")
        if ($hex -notmatch $needle) {
            throw "CJK bytes not found in response. needle=$needle  body_hex=$hex"
        }
    }

    It "Concurrent /translate 50 requests all 200" {
        $p_port = $Port
        $jobs = 1..50 | ForEach-Object {
            $i = $_
            Start-Job -ScriptBlock {
                param($po, $idx)
                try {
                    $body = '{"text":"concurrent_' + $idx + '"}'
                    $r = Invoke-WebRequest -Uri "http://127.0.0.1:$po/translate" -Method Post -Body $body -ContentType "application/json" -TimeoutSec 5 -UseBasicParsing
                    return $r.StatusCode
                } catch { return -1 }
            } -ArgumentList $p_port, $i
        }
        $jobs | Wait-Job | Out-Null
        $results = $jobs | Receive-Job
        $jobs | Remove-Job
        $ok = ($results | Where-Object { $_ -eq 200 }).Count
        if ($ok -ne 50) { throw "only $ok / 50 succeeded" }
    }

    It "Request buffer pool reuses blocks without exceeding its generation cap" {
        $r = Send-Json "/health"
        if ($r.Json.request_buffer_pool_hits -le 0) { throw "request buffer pool never recorded a reuse hit" }
        if ($r.Json.request_buffer_pool_misses -le 0) { throw "request buffer pool never recorded an allocation miss" }
        if ($r.Json.request_buffer_pool_cached -le 0) { throw "request buffer pool retained no reusable initial blocks" }
        if ($r.Json.request_buffer_pool_cached -gt $r.Json.request_buffer_pool_limit) { throw "request buffer pool exceeded its hard cap" }
    }

    Write-Host ""
    Write-Host "=== Shutdown ===" -ForegroundColor Cyan

    It "POST /shutdown causes process to exit with code 0" {
        try { Send-Json "/shutdown" "POST" $null 2 | Out-Null } catch { }
        $p | Wait-Process -Timeout 5
        Assert-Eq $p.ExitCode 0 "exit code"
    }
} finally {
    if (-not $p.HasExited) {
        Write-Host "Force-stopping server..." -ForegroundColor Yellow
        Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue
    }
}

Write-Host ""
Write-Host "=============================" -ForegroundColor Cyan
Write-Host ("Pass: " + $Pass + "   Fail: " + $Fail) -ForegroundColor $(if ($Fail -eq 0) { "Green" } else { "Red" })
if ($Fail -gt 0) {
    Write-Host "Failures:" -ForegroundColor Red
    $Errors | ForEach-Object { Write-Host "  $_" -ForegroundColor Red }
    exit 1
}
exit 0
