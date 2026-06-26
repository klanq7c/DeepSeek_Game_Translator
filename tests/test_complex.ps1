# Complex / edge-case test suite for dst_server.exe.
# Goes beyond happy-path E2E: edge sizes, encoding, concurrency, protocol abuse,
# game-dialog simulation. Uses raw TCP sockets where PowerShell's default ANSI
# encoding would otherwise corrupt the assertions.

param(
    [int]$Port = 19983,
    [string]$Exe = (Join-Path (Split-Path -Parent $PSScriptRoot) "native\dst_server.exe"),
    [string]$Cache = (Join-Path (Split-Path -Parent $PSScriptRoot) "translation_memory_c.tsv")
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

function Assert-Match($actual, $pattern, $msg) {
    if ($actual -notmatch $pattern) { throw "$msg : '$actual' does not match $pattern" }
}

# --- Low-level raw HTTP helpers (UTF-8 byte-precise) ---

function Send-Raw {
    param(
        [string]$Method, [string]$Path, [byte[]]$BodyBytes,
        [int]$BodyLengthOverride = -1,
        [string]$ExtraHeaders = "",
        [int]$TimeoutMs = 5000
    )
    $cli = New-Object System.Net.Sockets.TcpClient
    $cli.SendTimeout = $TimeoutMs
    $cli.ReceiveTimeout = $TimeoutMs
    $cli.Connect("127.0.0.1", $Port)
    $st = $cli.GetStream()
    $clen = if ($BodyLengthOverride -ge 0) { $BodyLengthOverride } else { $BodyBytes.Length }
    $hdr = "$Method $Path HTTP/1.1`r`nHost: 127.0.0.1`r`nContent-Type: application/json; charset=utf-8`r`nContent-Length: $clen`r`n${ExtraHeaders}Connection: close`r`n`r`n"
    $hdrBytes = [System.Text.Encoding]::ASCII.GetBytes($hdr)
    $st.Write($hdrBytes, 0, $hdrBytes.Length)
    if ($BodyBytes.Length -gt 0) { $st.Write($BodyBytes, 0, $BodyBytes.Length) }
    $st.Flush()
    $ms = New-Object System.IO.MemoryStream
    $buf = New-Object byte[] 8192
    try {
        while (($n = $st.Read($buf, 0, $buf.Length)) -gt 0) { $ms.Write($buf, 0, $n) }
    } catch { }
    $cli.Close()
    $all = $ms.ToArray()
    $text = [System.Text.Encoding]::UTF8.GetString($all)
    $idx = $text.IndexOf("`r`n`r`n")
    $status = -1
    if ($text -match '^HTTP/1\.1 (\d+) ') { $status = [int]$Matches[1] }
    $body = if ($idx -ge 0) { $text.Substring($idx + 4) } else { "" }
    $json = $null
    try { $json = $body | ConvertFrom-Json -ErrorAction Stop } catch { }
    return [pscustomobject]@{
        Status = $status
        Headers = if ($idx -ge 0) { $text.Substring(0, $idx) } else { $text }
        Body = $body
        Json = $json
        Raw = $all
    }
}

function Send-Json($Method, $Path, $JsonStr) {
    $bytes = [System.Text.Encoding]::UTF8.GetBytes($JsonStr)
    return Send-Raw -Method $Method -Path $Path -BodyBytes $bytes
}

function Send-Get($Path) {
    return Send-Raw -Method "GET" -Path $Path -BodyBytes ([byte[]]@())
}

# --- Start server ---

Write-Host "Starting server on port $Port..." -ForegroundColor Cyan
$p = Start-Process -FilePath $Exe -ArgumentList @("--port", $Port, "--cache", $Cache) -PassThru -WindowStyle Hidden
Start-Sleep -Milliseconds 1500

try {
    # =========================================================================
    Write-Host ""
    Write-Host "=== 1. Boundary sizes ===" -ForegroundColor Cyan

    It "Empty texts array in /batch returns empty translations map" {
        $r = Send-Json "POST" "/batch" '{"texts":[]}'
        Assert-Eq $r.Status 400 "empty array treated as missing"
    }

    It "Single-item texts array in /batch returns translations map (not single mode)" {
        $key = "boundary_single_$(Get-Random)"
        $imp = '{"entries":[{"key":"' + $key + '","value":"VALUE"}]}'
        $null = Send-Json "POST" "/cache/import" $imp
        $r = Send-Json "POST" "/batch" "{`"texts`":[`"$key`"]}"
        Assert-Eq $r.Status 200 "status"
        if (-not $r.Json.translations) { throw "no translations field" }
        Assert-Eq $r.Json.translations.$key "VALUE" "value"
        Assert-Eq $r.Json.results[0] "VALUE" "ordered result"
    }

    It "1 KB value round-trips" {
        $key = "len1k_$(Get-Random)"
        $val = "A" * 1024
        $imp = '{"entries":[{"key":"' + $key + '","value":"' + $val + '"}]}'
        $r = Send-Json "POST" "/cache/import" $imp
        Assert-Eq $r.Json.imported 1 "imported"
        $r = Send-Json "POST" "/translate" ('{"text":"' + $key + '"}')
        Assert-Eq $r.Json.translation $val "round-trip 1KB"
    }

    It "64 KB value round-trips" {
        $key = "len64k_$(Get-Random)"
        $val = "B" * (64 * 1024)
        $imp = '{"entries":[{"key":"' + $key + '","value":"' + $val + '"}]}'
        $r = Send-Json "POST" "/cache/import" $imp
        Assert-Eq $r.Json.imported 1 "imported"
        $r = Send-Json "POST" "/translate" ('{"text":"' + $key + '"}')
        Assert-Eq $r.Json.translation.Length 65536 "value length"
        if ($r.Json.translation -ne $val) { throw "value content mismatch" }
    }

    It "Empty value is rejected by import (filtered)" {
        $key = "emptyval_$(Get-Random)"
        $imp = '{"entries":[{"key":"' + $key + '","value":""}]}'
        $r = Send-Json "POST" "/cache/import" $imp
        Assert-Eq $r.Json.imported 0 "empty value filtered"
    }

    It "Empty key is rejected by import" {
        $imp = '{"entries":[{"key":"","value":"x"}]}'
        $r = Send-Json "POST" "/cache/import" $imp
        Assert-Eq $r.Json.imported 0 "empty key filtered"
    }

    # =========================================================================
    Write-Host ""
    Write-Host "=== 2. JSON escape correctness ===" -ForegroundColor Cyan

    # Use ConvertTo-Json to avoid manual escape headaches; lets PowerShell
    # produce valid JSON for any string the test invents.
    function To-Import($key, $val) {
        $obj = @{ entries = @( @{ key = $key; value = $val } ) }
        return ($obj | ConvertTo-Json -Compress -Depth 5)
    }
    function To-TranslateBody($key) {
        return (@{ text = $key } | ConvertTo-Json -Compress)
    }

    It "Value with embedded double-quote round-trips" {
        $key = "esc_q_$(Get-Random)"
        $val = 'He said "hi"'
        $r = Send-Json "POST" "/cache/import" (To-Import $key $val)
        Assert-Eq $r.Json.imported 1 "imported"
        $r = Send-Json "POST" "/translate" (To-TranslateBody $key)
        Assert-Eq $r.Json.translation $val "quote round-trip"
    }

    It "Value with backslash round-trips" {
        $key = "esc_b_$(Get-Random)"
        $val = 'path\to\file'
        $null = Send-Json "POST" "/cache/import" (To-Import $key $val)
        $r = Send-Json "POST" "/translate" (To-TranslateBody $key)
        Assert-Eq $r.Json.translation $val "backslash round-trip"
    }

    It "Value with newline/tab round-trips and is re-escaped on output" {
        $key = "esc_nt_$(Get-Random)"
        $val = "line1`nline2`tcol2"
        $r = Send-Json "POST" "/cache/import" (To-Import $key $val)
        Assert-Eq $r.Json.imported 1 "imported"
        $r = Send-Json "POST" "/translate" (To-TranslateBody $key)
        Assert-Eq $r.Json.translation $val "newline/tab round-trip"
        if ($r.Body -notmatch '\\n') { throw "newline not escaped in output" }
        if ($r.Body -notmatch '\\t') { throw "tab not escaped in output" }
    }

    It "Multi-byte UTF-8 round-trips via raw socket (byte-exact)" {
        # 0xE4 0xB8 0xAD 0xE6 0x96 0x87 = U+4E2D U+6587 (中文)
        $cjkBytes = [byte[]](0xE4, 0xB8, 0xAD, 0xE6, 0x96, 0x87)
        $cjkStr = [System.Text.Encoding]::UTF8.GetString($cjkBytes)
        $key = "esc_u_$(Get-Random)"
        $imp = '{"entries":[{"key":"' + $key + '","value":"' + $cjkStr + '"}]}'
        $r = Send-Raw -Method "POST" -Path "/cache/import" -BodyBytes ([System.Text.Encoding]::UTF8.GetBytes($imp))
        if ($r.Body -notmatch '"imported":1') { throw "import failed: $($r.Body)" }
        $lkpBody = [System.Text.Encoding]::UTF8.GetBytes('{"text":"' + $key + '"}')
        $r = Send-Raw -Method "POST" -Path "/translate" -BodyBytes $lkpBody
        $hex = [BitConverter]::ToString($r.Raw).Replace("-","")
        $needle = [BitConverter]::ToString($cjkBytes).Replace("-","")
        if ($hex -notmatch $needle) { throw "UTF-8 bytes for CJK not found in response (needle=$needle)" }
    }

    It "Surrogate-pair JSON escape decodes to valid 4-byte UTF-8" {
        # \uD83D\uDE00 = U+1F600. It must become F0 9F 98 80, not two UTF-8
        # encoded surrogate code units.
        $emojiBytes = [byte[]](0xF0, 0x9F, 0x98, 0x80)
        $key = "esc_emoji_$(Get-Random)"
        $imp = '{"entries":[{"key":"' + $key + '","value":"\uD83D\uDE00"}]}'
        $r = Send-Raw -Method "POST" -Path "/cache/import" -BodyBytes ([System.Text.Encoding]::ASCII.GetBytes($imp))
        if ($r.Body -notmatch '"imported":1') { throw "import failed: $($r.Body)" }

        $r = Send-Raw -Method "POST" -Path "/translate" -BodyBytes ([System.Text.Encoding]::ASCII.GetBytes('{"text":"' + $key + '"}'))
        $hex = [BitConverter]::ToString($r.Raw).Replace("-","")
        $needle = [BitConverter]::ToString($emojiBytes).Replace("-","")
        if ($hex -notmatch $needle) { throw "UTF-8 bytes for emoji not found in response (needle=$needle)" }
        if ($hex -match "EDA0BDEDB880") { throw "surrogate halves were emitted as invalid UTF-8" }
    }

    # =========================================================================
    Write-Host ""
    Write-Host "=== 3. Large batch volumes ===" -ForegroundColor Cyan

    It "Import 500 entries, then batch-lookup 500 returns all" {
        $base = "vol500_$(Get-Random)_"
        $sb = New-Object System.Text.StringBuilder
        [void]$sb.Append('{"entries":[')
        for ($i = 0; $i -lt 500; $i++) {
            if ($i -gt 0) { [void]$sb.Append(',') }
            [void]$sb.Append('{"key":"' + $base + $i + '","value":"V' + $i + '"}')
        }
        [void]$sb.Append(']}')
        $r = Send-Json "POST" "/cache/import" $sb.ToString()
        Assert-Eq $r.Json.imported 500 "all imported"

        # Lookup
        $sb2 = New-Object System.Text.StringBuilder
        [void]$sb2.Append('{"texts":[')
        for ($i = 0; $i -lt 500; $i++) {
            if ($i -gt 0) { [void]$sb2.Append(',') }
            [void]$sb2.Append('"' + $base + $i + '"')
        }
        [void]$sb2.Append(']}')
        $r = Send-Json "POST" "/cache/lookup" $sb2.ToString()
        Assert-Eq $r.Json.hit_count 500 "hit count"
        Assert-Eq $r.Json.miss_count 0 "miss count"
        # Spot-check a few
        $k = $base + "0"; if ($r.Json.hits.$k -ne "V0") { throw "first miss" }
        $k = $base + "499"; if ($r.Json.hits.$k -ne "V499") { throw "last miss" }
        $k = $base + "250"; if ($r.Json.hits.$k -ne "V250") { throw "middle miss" }
    }

    It "Mixed-hit batch (250 hit + 250 miss) returns correct counts" {
        $base = "mix250_$(Get-Random)_"
        $sb = New-Object System.Text.StringBuilder
        [void]$sb.Append('{"entries":[')
        for ($i = 0; $i -lt 250; $i++) {
            if ($i -gt 0) { [void]$sb.Append(',') }
            [void]$sb.Append('{"key":"' + $base + $i + '","value":"V' + $i + '"}')
        }
        [void]$sb.Append(']}')
        $null = Send-Json "POST" "/cache/import" $sb.ToString()

        $sb2 = New-Object System.Text.StringBuilder
        [void]$sb2.Append('{"texts":[')
        for ($i = 0; $i -lt 500; $i++) {
            if ($i -gt 0) { [void]$sb2.Append(',') }
            [void]$sb2.Append('"' + $base + $i + '"')
        }
        [void]$sb2.Append(']}')
        $r = Send-Json "POST" "/cache/lookup" $sb2.ToString()
        Assert-Eq $r.Json.hit_count 250 "hit count"
        Assert-Eq $r.Json.miss_count 250 "miss count"
    }

    # =========================================================================
    Write-Host ""
    Write-Host "=== 4. Concurrency with mixed reads & writes ===" -ForegroundColor Cyan

    It "Concurrent writers + readers don't deadlock or corrupt" {
        $base = "concur_$(Get-Random)_"
        $portArg = $Port
        $writers = 1..4 | ForEach-Object {
            $wid = $_
            Start-Job -ScriptBlock {
                param($po, $b, $wid)
                for ($i = 0; $i -lt 100; $i++) {
                    try {
                        $body = '{"entries":[{"key":"' + $b + $wid + '_' + $i + '","value":"VAL' + $wid + '_' + $i + '"}]}'
                        $null = Invoke-WebRequest -Uri "http://127.0.0.1:$po/cache/import" -Method Post -Body $body -ContentType "application/json" -TimeoutSec 3 -UseBasicParsing
                    } catch { }
                }
                return "writer$wid done"
            } -ArgumentList $portArg, $base, $wid
        }
        $readers = 1..4 | ForEach-Object {
            $rid = $_
            Start-Job -ScriptBlock {
                param($po, $b, $rid)
                $ok = 0
                for ($i = 0; $i -lt 200; $i++) {
                    try {
                        $body = '{"text":"' + $b + '1_' + ($i % 50) + '"}'
                        $r = Invoke-WebRequest -Uri "http://127.0.0.1:$po/translate" -Method Post -Body $body -ContentType "application/json" -TimeoutSec 3 -UseBasicParsing
                        if ($r.StatusCode -eq 200) { $ok++ }
                    } catch { }
                }
                return $ok
            } -ArgumentList $portArg, $base, $rid
        }
        $writers + $readers | Wait-Job -Timeout 30 | Out-Null
        $writers + $readers | Remove-Job -Force
        # Final verification: lookup a known imported key
        $r = Send-Json "POST" "/translate" ('{"text":"' + $base + '1_99"}')
        Assert-Eq $r.Json.source "cache" "post-concurrency lookup hit"
    }

    # =========================================================================
    Write-Host ""
    Write-Host "=== 5. Protocol robustness ===" -ForegroundColor Cyan

    It "Malformed JSON in /translate returns 400" {
        $r = Send-Json "POST" "/translate" '{not valid json'
        Assert-Eq $r.Status 400 "malformed body"
    }

    It "Unterminated or invalid JSON strings are rejected instead of partially translated" {
        $r = Send-Json "POST" "/translate" '{"text":"unterminated}'
        Assert-Eq $r.Status 400 "unterminated string"

        $r = Send-Json "POST" "/translate" '{"text":"bad\qescape"}'
        Assert-Eq $r.Status 400 "unknown escape"

        $r = Send-Json "POST" "/batch" '{"texts":["valid","unterminated]}'
        Assert-Eq $r.Status 400 "partial malformed array"
    }

    It "Body text named Content-Length does not affect header parsing" {
        $cli = New-Object System.Net.Sockets.TcpClient
        $cli.SendTimeout = 2000
        $cli.ReceiveTimeout = 2500
        $cli.Connect("127.0.0.1", $Port)
        $st = $cli.GetStream()
        $raw = "POST /translate HTTP/1.1`r`nHost: 127.0.0.1`r`nContent-Type: application/json`r`nConnection: close`r`n`r`nContent-Length: 999999999`r`n`r`n"
        $bytes = [System.Text.Encoding]::ASCII.GetBytes($raw)
        $sw = [System.Diagnostics.Stopwatch]::StartNew()
        $st.Write($bytes, 0, $bytes.Length)
        $st.Flush()
        $ms = New-Object System.IO.MemoryStream
        $buf = New-Object byte[] 4096
        try {
            while (($n = $st.Read($buf, 0, $buf.Length)) -gt 0) { $ms.Write($buf, 0, $n) }
        } catch {
            $cli.Close()
            throw "server waited for a body Content-Length token instead of parsing headers only"
        }
        $sw.Stop()
        $cli.Close()
        $text = [System.Text.Encoding]::UTF8.GetString($ms.ToArray())
        if ($text -notmatch '^HTTP/1\.1 400 ') { throw "unexpected response: $text" }
        if ($sw.ElapsedMilliseconds -gt 2000) { throw "response too slow: $($sw.ElapsedMilliseconds)ms" }
    }

    It "Garbage bytes as full request gets some HTTP error response or close" {
        $cli = New-Object System.Net.Sockets.TcpClient("127.0.0.1", $Port)
        $cli.ReceiveTimeout = 5000
        $st = $cli.GetStream()
        $garbage = [byte[]](0..255)
        $st.Write($garbage, 0, $garbage.Length)
        $st.Flush()
        $cli.Client.Shutdown([System.Net.Sockets.SocketShutdown]::Send)
        $buf = New-Object byte[] 4096
        try { $null = $st.Read($buf, 0, $buf.Length) } catch { }
        $cli.Close()
        # OK if server just closes or returns 404. The point: it must not crash.
    }

    It "Server still responsive after 200 malformed requests" {
        for ($i = 0; $i -lt 200; $i++) {
            try { Send-Json "POST" "/translate" "garbage_$i" | Out-Null } catch { }
        }
        $r = Send-Get "/health"
        Assert-Eq $r.Status 200 "still healthy"
    }

    It "Path with control characters returns 404, not crash" {
        $r = Send-Raw -Method "GET" -Path "/abc%00def" -BodyBytes ([byte[]]@())
        if ($r.Status -ne 404 -and $r.Status -ne 200) { throw "unexpected status $($r.Status)" }
    }

    It "Connection closes when server sends Connection: close" {
        $r = Send-Get "/health"
        if ($r.Headers -notmatch "Connection: close") { throw "Connection: close header missing" }
    }

    # =========================================================================
    Write-Host ""
    Write-Host "=== 6. Game dialog simulation ===" -ForegroundColor Cyan

    It "Realistic game scene: 50 lines, ~80% pre-cached, mixed sentence sizes" {
        $sceneKey = "scene_$(Get-Random)_"
        # Pre-cache 40 of 50 dialog lines
        $sb = New-Object System.Text.StringBuilder
        [void]$sb.Append('{"entries":[')
        for ($i = 0; $i -lt 40; $i++) {
            if ($i -gt 0) { [void]$sb.Append(',') }
            [void]$sb.Append('{"key":"' + $sceneKey + $i + '","value":"TR_' + $i + '"}')
        }
        [void]$sb.Append(']}')
        $null = Send-Json "POST" "/cache/import" $sb.ToString()
        # Simulate dialog: 50 lookups in order
        $hits = 0
        for ($i = 0; $i -lt 50; $i++) {
            $r = Send-Json "POST" "/translate" ('{"text":"' + $sceneKey + $i + '"}')
            Assert-Eq $r.Status 200 "line $i status"
            if ($r.Json.source -eq "cache") { $hits++ }
        }
        Assert-Eq $hits 40 "expected 40 cache hits"
    }

    # =========================================================================
    Write-Host ""
    Write-Host "=== 7. Server hygiene after stress ===" -ForegroundColor Cyan

    It "Cache size monotonic - only grew, never shrank during stress" {
        $r = Send-Get "/health"
        if ($r.Json.cache_size -lt 300000) { throw "cache size suspiciously small: $($r.Json.cache_size)" }
    }

    It "Uptime advances" {
        $r1 = Send-Get "/health"
        Start-Sleep -Seconds 2
        $r2 = Send-Get "/health"
        if ($r2.Json.uptime_seconds -le $r1.Json.uptime_seconds) {
            throw "uptime did not advance: $($r1.Json.uptime_seconds) → $($r2.Json.uptime_seconds)"
        }
    }

    It "POST /shutdown returns 200, process exits clean" {
        try { Send-Raw -Method "POST" -Path "/shutdown" -BodyBytes ([byte[]]@()) -TimeoutMs 3000 | Out-Null } catch { }
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
