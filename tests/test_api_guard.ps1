# Regression tests for API responses that echo the original text.
param(
    [int]$Port = 19984,
    [int]$FakeApiPort = 0,
    [string]$Exe = (Join-Path (Split-Path -Parent $PSScriptRoot) "native\dst_server.exe")
)

$ErrorActionPreference = "Stop"
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

function Get-ApiCallCount([string]$path) {
    if (-not (Test-Path -LiteralPath $path)) { return 0 }
    return @((Get-Content -LiteralPath $path -ErrorAction SilentlyContinue) | Where-Object { $_ -eq "hit" }).Count
}

function Wait-ApiCalls([string]$path, [int]$minCount) {
    for ($i = 0; $i -lt 30; $i++) {
        if ((Get-ApiCallCount $path) -ge $minCount) { return }
        Start-Sleep -Milliseconds 100
    }
    throw "fake API was not called $minCount times"
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

function Wait-FakeApiReady($job, [string]$readyPath) {
    for ($i = 0; $i -lt 50; $i++) {
        if (Test-Path -LiteralPath $readyPath) { return }
        if ($job.State -eq "Failed") {
            Receive-Job $job -ErrorAction SilentlyContinue
            throw "fake API job failed"
        }
        Start-Sleep -Milliseconds 100
    }
    Receive-Job $job -ErrorAction SilentlyContinue
    throw "fake API did not start"
}

if ($FakeApiPort -le 0) {
    $FakeApiPort = Get-FreeTcpPort
}

$tmp = Join-Path $env:TEMP ("dst_api_guard_" + [guid]::NewGuid().ToString("N"))
New-Item -ItemType Directory -Path $tmp | Out-Null
$cache = Join-Path $tmp "cache.tsv"
$apiConfig = Join-Path $tmp "api.ini"
$callsFile = Join-Path $tmp "fake_api_calls.log"
$readyFile = Join-Path $tmp "fake_api_ready.txt"
$stopFile = Join-Path $tmp "fake_api_stop.txt"
Set-Content -LiteralPath $apiConfig -Encoding ASCII -Value @"
[api]
endpoint=http://127.0.0.1:$FakeApiPort/v1/chat/completions
model=fake-model
key=fake-key
timeout_ms=3000
"@
$seededKey = "__api_guard_disk_prompt_echo_$(Get-Random)"
$seededExpected = ('"\u78c1\u76d8\u65e7\u8bd1\u6587"' | ConvertFrom-Json)
$seededPrefix = ('"\u7ffb\u8bd1\u6210\u7b80\u4f53\u4e2d\u6587\uff1a"' | ConvertFrom-Json)
$seededKey64 = [Convert]::ToBase64String([System.Text.Encoding]::UTF8.GetBytes($seededKey))
$seededValue64 = [Convert]::ToBase64String([System.Text.Encoding]::UTF8.GetBytes($seededPrefix + $seededExpected))
[System.IO.File]::WriteAllText($cache, $seededKey64 + "`t" + $seededValue64 + "`n", [System.Text.Encoding]::ASCII)

$fakeApi = Start-Job -ArgumentList $FakeApiPort,$callsFile,$readyFile,$stopFile -ScriptBlock {
    param([int]$Port, [string]$CallsFile, [string]$ReadyFile, [string]$StopFile)

    function Convert-FakeOutput([string]$text) {
        if ($text.StartsWith("__api_guard_prompt_echo_cn_", [System.StringComparison]::Ordinal)) {
            return ('"\u7ffb\u8bd1\u6210\u7b80\u4f53\u4e2d\u6587\uff1a\u6d4b\u8bd5\u8bd1\u6587"' | ConvertFrom-Json)
        }
        if ($text.StartsWith("__api_guard_prompt_echo_en_", [System.StringComparison]::Ordinal)) {
            $second = ('"\u7b2c\u4e8c\u6761\u8bd1\u6587"' | ConvertFrom-Json)
            return "Simplified Chinese translation: $second"
        }
        return $text
    }

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

    function Get-EchoContent([string]$body) {
        try {
            $obj = $body | ConvertFrom-Json
            $msg = @($obj.messages | Where-Object { $_.role -eq "user" })[-1]
            $content = [string]$msg.content
            $trimmed = $content.Trim()
            if ($trimmed.StartsWith("[")) {
                $items = $trimmed | ConvertFrom-Json
                $translated = @($items | ForEach-Object { Convert-FakeOutput ([string]$_) })
                return (ConvertTo-Json -InputObject $translated -Compress)
            }
            $parts = $content -split "`n", 2
            if ($parts.Count -gt 1) { return Convert-FakeOutput $parts[1] }
            return Convert-FakeOutput $content
        } catch {
            return "original"
        }
    }

    $listener = [System.Net.Sockets.TcpListener]::new([System.Net.IPAddress]::Parse("127.0.0.1"), $Port)
    $listener.Start()
    Set-Content -LiteralPath $ReadyFile -Encoding ASCII -Value "ready"
    try {
        while ($true) {
            $client = $listener.AcceptTcpClient()
            $stream = $client.GetStream()
            $content = Get-EchoContent (Read-HttpRequestBody $stream)
            Add-Content -LiteralPath $CallsFile -Encoding ASCII -Value "hit"
            $payload = @{ choices = @(@{ message = @{ content = $content } }) } | ConvertTo-Json -Depth 5 -Compress
            $bodyBytes = [System.Text.Encoding]::UTF8.GetBytes($payload)
            $header = "HTTP/1.1 200 OK`r`nContent-Type: application/json; charset=utf-8`r`nContent-Length: $($bodyBytes.Length)`r`nConnection: close`r`n`r`n"
            $headerBytes = [System.Text.Encoding]::ASCII.GetBytes($header)
            $stream.Write($headerBytes, 0, $headerBytes.Length)
            $stream.Write($bodyBytes, 0, $bodyBytes.Length)
            $client.Close()
            if (Test-Path -LiteralPath $StopFile) { break }
        }
    } finally {
        $listener.Stop()
    }
}

$server = $null
try {
    Wait-FakeApiReady $fakeApi $readyFile
    Write-Host "Starting server on port $Port with fake API on $FakeApiPort..." -ForegroundColor Cyan
    $server = Start-Process -FilePath $Exe -ArgumentList @("--port", $Port, "--cache", $cache, "--api-config", $apiConfig) -PassThru -WindowStyle Hidden
    Wait-Server

    Write-Host ""
    Write-Host "=== API original echo guard ===" -ForegroundColor Cyan

    It "disk cache prompt echoes are healed in memory without rewriting the file" {
        $body = '{"texts":["' + $seededKey + '"]}'
        $lookup = Send-Json "/cache/lookup" "POST" $body
        Assert-Eq $lookup.Json.hit_count 1 "seeded disk translation should remain a hit"
        Assert-Eq $lookup.Json.hits.$seededKey $seededExpected "disk-loaded cache should expose only the clean translation"
        $diskLine = [System.IO.File]::ReadAllText($cache, [System.Text.Encoding]::ASCII)
        Assert-Eq $diskLine ($seededKey64 + "`t" + $seededValue64 + "`n") "startup healing must not rewrite user translation memory"
    }

    It "POST /batch treats original echo responses as misses" {
        $a = "__api_guard_batch_a_$(Get-Random)"
        $b = "__api_guard_batch_b_$(Get-Random)"
        $body = '{"texts":["' + $a + '","' + $b + '"]}'
        $before = Get-ApiCallCount $callsFile
        $r = Send-Json "/batch" "POST" $body 10
        Wait-ApiCalls $callsFile ($before + 1)
        Assert-Eq $r.Status 200 "status"
        Assert-Eq $r.Json.results[0] $a "first result"
        Assert-Eq $r.Json.results[1] $b "second result"
        Assert-Eq $r.Json.sources[0] "miss" "first source"
        Assert-Eq $r.Json.sources[1] "miss" "second source"

        $lookup = Send-Json "/cache/lookup" "POST" $body
        Assert-Eq $lookup.Json.miss_count 2 "batch original echoes must not be cached"
    }

    It "POST /prefetch does not persist original echo responses" {
        $a = "__api_guard_prefetch_a_$(Get-Random)"
        $b = "__api_guard_prefetch_b_$(Get-Random)"
        $body = '{"texts":["' + $a + '","' + $b + '"]}'
        $before = Get-ApiCallCount $callsFile
        $prefetch = Send-Json "/prefetch" "POST" $body
        Assert-Eq $prefetch.Status 200 "prefetch status"
        Wait-ApiCalls $callsFile ($before + 1)

        $lookup = Send-Json "/cache/lookup" "POST" $body
        Assert-Eq $lookup.Json.miss_count 2 "prefetch original echoes must not be cached"
    }

    It "cache imports clean prompt echoes before any engine reads them" {
        $key = "__api_guard_imported_prompt_echo_$(Get-Random)"
        $expected = ('"\u65e7\u7f13\u5b58\u8bd1\u6587"' | ConvertFrom-Json)
        $importBody = '{"entries":[{"key":"' + $key + '","value":"\u7ffb\u8bd1\u6210\u7b80\u4f53\u4e2d\u6587\uff1a\u65e7\u7f13\u5b58\u8bd1\u6587"}]}'
        $import = Send-Json "/cache/import" "POST" $importBody
        Assert-Eq $import.Status 200 "import status"
        Assert-Eq $import.Json.imported 1 "imported count"

        $lookupBody = '{"texts":["' + $key + '"]}'
        $lookup = Send-Json "/cache/lookup" "POST" $lookupBody
        Assert-Eq $lookup.Json.hit_count 1 "imported translation should remain a hit"
        Assert-Eq $lookup.Json.hits.$key $expected "shared cache reads must expose only the clean translation"
    }

    It "cache prompt cleanup preserves normal outer whitespace and line breaks" {
        $key = "__api_guard_preserve_whitespace_$(Get-Random)"
        $expected = ('" \n\u4fdd\u7559\u8fb9\u754c\n "' | ConvertFrom-Json)
        $importBody = '{"entries":[{"key":"' + $key + '","value":" \n\u4fdd\u7559\u8fb9\u754c\n "}]}'
        $import = Send-Json "/cache/import" "POST" $importBody
        Assert-Eq $import.Status 200 "import status"
        Assert-Eq $import.Json.imported 1 "imported count"

        $lookupBody = '{"texts":["' + $key + '"]}'
        $lookup = Send-Json "/cache/lookup" "POST" $lookupBody
        Assert-Eq $lookup.Json.hits.$key $expected "normal cache values must not be globally trimmed"
    }

    It "POST /prefetch strips model prompt echoes before caching translations" {
        $a = "__api_guard_prompt_echo_cn_$(Get-Random)"
        $b = "__api_guard_prompt_echo_en_$(Get-Random)"
        $body = '{"texts":["' + $a + '","' + $b + '"]}'
        $before = Get-ApiCallCount $callsFile
        $prefetch = Send-Json "/prefetch" "POST" $body
        Assert-Eq $prefetch.Status 200 "prefetch status"
        Wait-ApiCalls $callsFile ($before + 1)

        $lookup = $null
        for ($i = 0; $i -lt 30; $i++) {
            $lookup = Send-Json "/cache/lookup" "POST" $body
            if ($lookup.Json.hit_count -eq 2) { break }
            Start-Sleep -Milliseconds 100
        }
        Assert-Eq $lookup.Json.hit_count 2 "cleaned translations should be cached"
        $expectedA = ('"\u6d4b\u8bd5\u8bd1\u6587"' | ConvertFrom-Json)
        $expectedB = ('"\u7b2c\u4e8c\u6761\u8bd1\u6587"' | ConvertFrom-Json)
        Assert-Eq $lookup.Json.hits.$a $expectedA "Chinese prompt echo must be stripped"
        Assert-Eq $lookup.Json.hits.$b $expectedB "English prompt echo must be stripped"
    }
} finally {
    try { if ($server -and -not $server.HasExited) { Send-Json "/shutdown" "POST" "{}" 2 | Out-Null } } catch { }
    try { if ($server -and -not $server.HasExited) { $server.WaitForExit(3000) | Out-Null } } catch { }
    try { if ($server -and -not $server.HasExited) { Stop-Process -Id $server.Id -Force -ErrorAction SilentlyContinue } } catch { }
    try {
        Set-Content -LiteralPath $stopFile -Encoding ASCII -Value "stop"
        Invoke-WebRequest -Uri "http://127.0.0.1:$FakeApiPort/stop" -Method Post -Body "{}" -ContentType "application/json" -TimeoutSec 1 -UseBasicParsing | Out-Null
    } catch { }
    try { Wait-Job $fakeApi -Timeout 3 | Out-Null } catch { }
    if ($fakeApi.State -eq "Running") { Stop-Job $fakeApi -ErrorAction SilentlyContinue }
    Remove-Job $fakeApi -Force -ErrorAction SilentlyContinue
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
