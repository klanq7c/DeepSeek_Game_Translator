#Requires -Version 5.1
# run_contract_tests.ps1 -- HTTP contract tests for the local translation server.
#
# Runs the same checks against every server binary given (default: the C server
# native\dst_server.exe and the C# server native\dst_server_cs.exe) using a fake
# OpenAI-compatible provider hosted in a background job. Both implementations
# must pass identically: this file is the executable definition of the shared
# server contract described in docs\USER_GUIDE.md and CONTEXT.md.
#
# Coverage (per server):
#   /health /capabilities OPTIONS, malformed input -> 400 (never a dropped
#   connection), browser-origin policy, live /translate + /batch (dedup, cache,
#   cache_only), /prefetch with context + memory accounting back to zero,
#   split-retry: terminal failures (500/401) are NOT amplified while shape
#   failures still split, prompt-echo stripping parity, cache import/export/
#   dump, idle-connection timeout -> 400 incomplete_request, 40-way concurrent
#   burst, /shutdown + TSV persistence + restart reload.
#
# Prerequisites: binaries already built (build_native.bat). Nothing is written
# inside the repository; work files go to %TEMP%\dst_contract_tests.
#
# Usage:
#   powershell -NoProfile -ExecutionPolicy Bypass -File tests\server_contract\run_contract_tests.ps1
#   ... -Exe native\dst_server_cs.exe          (one implementation only)
param(
    [string[]]$Exe,
    [int]$FakePort = 19990,
    [int]$FirstServerPort = 19985
)

$ErrorActionPreference = 'Continue'
[System.Net.ServicePointManager]::Expect100Continue = $false
$Root = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
if (-not $Exe -or $Exe.Count -eq 0) {
    $Exe = @((Join-Path $Root 'native\dst_server.exe'), (Join-Path $Root 'native\dst_server_cs.exe'))
} else {
    $Exe = @($Exe | ForEach-Object { if ([IO.Path]::IsPathRooted($_)) { $_ } else { Join-Path $Root $_ } })
}
$Work = Join-Path $env:TEMP 'dst_contract_tests'
if (Test-Path $Work) { Remove-Item -Recurse -Force $Work }
New-Item -ItemType Directory -Force -Path $Work | Out-Null
$ResultsPath = Join-Path $Work 'results.txt'
$script:Fail = 0
$script:Pass = 0

# Non-ASCII test vectors are built from code points so this file stays ASCII
# (PowerShell 5.1 mis-decodes BOM-less UTF-8 sources as the ANSI code page).
function U([int[]]$codes) { return -join ($codes | ForEach-Object { [char]$_ }) }
$YI      = U 0x8BD1                                      # "translated:" marker the fake prepends
$COLON   = U 0xFF1A                                      # fullwidth colon
$ECHOP   = U 0x7B80,0x4F53,0x4E2D,0x6587,0x7FFB,0x8BD1   # prompt-echo prefix
$BARE    = $ECHOP + (U 0x7248,0x672C,0x5DF2,0x4E0A,0x7EBF)  # prefix-like phrase, no separator
$IMPORTV = U 0x5BFC,0x5165,0x8BD1,0x6587

function Esc([string]$s) { if (-not $s) { return $s }; return [regex]::Replace($s, '[^\x20-\x7E\r\n\t]', { param($m) '\u{0:X4}' -f [int][char]$m.Value }) }
function Check([string]$name, [bool]$cond, [string]$detail) {
    $tag = 'FAIL'
    if ($cond) { $tag = 'PASS'; $script:Pass++ } else { $script:Fail++ }
    $line = "[$tag] $name" + $(if ($detail -and -not $cond) { " -- " + (Esc $detail) } else { '' })
    if ($cond) { Write-Host $line } else { Write-Host $line -ForegroundColor Red }
    Add-Content -Path $ResultsPath -Value $line -Encoding UTF8
}
function Note([string]$msg) { Write-Host $msg; Add-Content -Path $ResultsPath -Value $msg -Encoding UTF8 }

function Invoke-Http([string]$Method, [string]$Url, [string]$Body, [string]$Origin) {
    $req = [System.Net.HttpWebRequest]::Create($Url)
    $req.Method = $Method
    $req.Timeout = 60000
    $req.KeepAlive = $false
    if ($Origin) { $req.Headers.Add('Origin', $Origin) }
    if ($null -ne $Body) {
        $bytes = [Text.Encoding]::UTF8.GetBytes($Body)
        $req.ContentType = 'application/json'
        $req.ContentLength = $bytes.Length
        # A server that closes the socket before the body is written (the shutdown
        # teardown races this) makes GetRequestStream throw. Report it as the same
        # Status = -1 the response path uses instead of letting PowerShell print
        # three cascading null-method errors around a check that still passed.
        try {
            $s = $req.GetRequestStream(); $s.Write($bytes, 0, $bytes.Length); $s.Close()
        } catch [System.Net.WebException] {
            return @{ Status = -1; Body = "request body not sent: $($_.Exception.Message)" }
        }
    } elseif ($Method -eq 'POST') {
        $req.ContentLength = 0
    }
    $resp = $null
    try { $resp = $req.GetResponse() }
    catch [System.Net.WebException] {
        $resp = $_.Exception.Response
        if ($null -eq $resp) { return @{ Status = -1; Body = $_.Exception.Message } }
    }
    $sr = New-Object IO.StreamReader($resp.GetResponseStream(), [Text.Encoding]::UTF8)
    $text = $sr.ReadToEnd()
    $code = [int]$resp.StatusCode
    $resp.Close()
    return @{ Status = $code; Body = $text }
}
function J($text) { try { return ($text | ConvertFrom-Json) } catch { return $null } }
function Wait-Until([scriptblock]$cond, [int]$timeoutMs, [int]$stepMs = 200) {
    $deadline = [DateTime]::UtcNow.AddMilliseconds($timeoutMs)
    while ([DateTime]::UtcNow -lt $deadline) {
        if (& $cond) { return $true }
        Start-Sleep -Milliseconds $stepMs
    }
    return (& $cond)
}

# ---------------------------------------------------------------- fake provider
# Minimal OpenAI-compatible /v1/chat/completions. Reply mode is chosen from
# markers inside the user prompt so each test controls the provider behaviour:
#   FAIL500 / FAIL401  -> HTTP error (terminal for split-retry)
#   BADSHAPE           -> batch reply with one extra element (shape failure -> split)
#   ECHO               -> prefix + fullwidth colon + translation (must be stripped)
#   ECHOBARE           -> prefix-like phrase only, no separator (must be kept)
Note "== Fake OpenAI-compatible provider on 127.0.0.1:$FakePort =="
$FakeLog = Join-Path $Work 'fake_provider_requests.log'
New-Item -ItemType File -Force -Path $FakeLog | Out-Null
$fakeJob = Start-Job -ArgumentList $FakePort, $FakeLog -ScriptBlock {
    param($Port, $LogPath)
    function U([int[]]$codes) { return -join ($codes | ForEach-Object { [char]$_ }) }
    $YI    = U 0x8BD1
    $COLON = U 0xFF1A
    $ECHOP = U 0x7B80,0x4F53,0x4E2D,0x6587,0x7FFB,0x8BD1
    $BARE  = $ECHOP + (U 0x7248,0x672C,0x5DF2,0x4E0A,0x7EBF)
    $EXTRA = U 0x591A,0x4F59
    $listener = New-Object System.Net.Sockets.TcpListener([System.Net.IPAddress]::Loopback, $Port)
    $listener.Start()
    while ($true) {
        $client = $listener.AcceptTcpClient()
        try {
            $client.ReceiveTimeout = 10000
            $stream = $client.GetStream()
            $ms = New-Object IO.MemoryStream
            $buf = New-Object byte[] 65536
            $headerEnd = -1
            while ($headerEnd -lt 0) {
                $n = $stream.Read($buf, 0, $buf.Length)
                if ($n -le 0) { break }
                $ms.Write($buf, 0, $n)
                $arr = $ms.ToArray()
                for ($i = 0; $i -le $arr.Length - 4; $i++) {
                    if ($arr[$i] -eq 13 -and $arr[$i+1] -eq 10 -and $arr[$i+2] -eq 13 -and $arr[$i+3] -eq 10) { $headerEnd = $i + 4; break }
                }
            }
            if ($headerEnd -lt 0) { continue }
            $arr = $ms.ToArray()
            $headText = [Text.Encoding]::ASCII.GetString($arr, 0, $headerEnd)
            $cl = 0
            if ($headText -match '(?im)^Content-Length:\s*(\d+)') { $cl = [int]$Matches[1] }
            if ($headText -match '(?im)^Expect:\s*100-continue') {
                $c100 = [Text.Encoding]::ASCII.GetBytes("HTTP/1.1 100 Continue`r`n`r`n")
                $stream.Write($c100, 0, $c100.Length); $stream.Flush()
            }
            while ($ms.Length - $headerEnd -lt $cl) {
                $n = $stream.Read($buf, 0, $buf.Length)
                if ($n -le 0) { break }
                $ms.Write($buf, 0, $n)
            }
            $arr = $ms.ToArray()
            $body = [Text.Encoding]::UTF8.GetString($arr, $headerEnd, [Math]::Min($cl, $arr.Length - $headerEnd))
            $reqObj = ConvertFrom-Json -InputObject $body
            $user = [string](@($reqObj.messages | Where-Object { $_.role -eq 'user' })[-1].content)
            # PS 5.1 ConvertFrom-Json emits a JSON array as ONE object[]; flatten with foreach.
            $kind = 'single'; $items = @()
            if ($user.StartsWith('[')) {
                $kind = 'batch'
                foreach ($x in (ConvertFrom-Json -InputObject $user)) { $items += [string]$x }
            } elseif ($user.StartsWith('Previous lines')) {
                $kind = 'ctx'
                $marker = "Texts to translate:`n"
                $idx = $user.IndexOf($marker)
                foreach ($x in (ConvertFrom-Json -InputObject $user.Substring($idx + $marker.Length))) { $items += [string]$x }
            } else {
                $nl = $user.IndexOf("`n")
                $items = @($user.Substring($nl + 1))
            }
            $mode = 'ok'
            if ($user -match 'FAIL500') { $mode = 'FAIL500' }
            elseif ($user -match 'FAIL401') { $mode = 'FAIL401' }
            elseif ($user -match 'BADSHAPE' -and $items.Count -ge 2) { $mode = 'BADSHAPE' }
            elseif ($user -match 'ECHOBARE') { $mode = 'ECHOBARE' }
            elseif ($user -match 'ECHO') { $mode = 'ECHO' }
            Add-Content -Path $LogPath -Encoding UTF8 -Value ("{0}`t{1}`t{2}`t{3}" -f [DateTimeOffset]::UtcNow.ToUnixTimeMilliseconds(), $kind, $items.Count, $mode)

            $status = 200; $reason = 'OK'; $json = ''
            if ($mode -eq 'FAIL500') { $status = 500; $reason = 'Internal Server Error'; $json = '{"error":"fake outage"}' }
            elseif ($mode -eq 'FAIL401') { $status = 401; $reason = 'Unauthorized'; $json = '{"error":"bad key"}' }
            else {
                $out = @()
                foreach ($t in $items) {
                    if ($mode -eq 'ECHO') { $out += ($ECHOP + $COLON + $YI + ':' + $t) }
                    elseif ($mode -eq 'ECHOBARE') { $out += $BARE }
                    else { $out += ($YI + ':' + $t) }
                }
                if ($mode -eq 'BADSHAPE') { $out += $EXTRA }
                if ($kind -eq 'single') { $content = [string]$out[0] }
                else { $content = ConvertTo-Json -InputObject $out -Compress }
                $json = ConvertTo-Json -Compress -Depth 6 -InputObject @{ choices = @(@{ message = @{ role = 'assistant'; content = $content } }) }
            }
            $rb = [Text.Encoding]::UTF8.GetBytes($json)
            $head = "HTTP/1.1 $status $reason`r`nContent-Type: application/json`r`nContent-Length: $($rb.Length)`r`nConnection: close`r`n`r`n"
            $hb = [Text.Encoding]::ASCII.GetBytes($head)
            $stream.Write($hb, 0, $hb.Length); $stream.Write($rb, 0, $rb.Length); $stream.Flush()
        } catch {
            Add-Content -Path $LogPath -Encoding UTF8 -Value ("ERR`t" + $_.Exception.Message)
        } finally {
            $client.Close()
        }
    }
}
$fakeReady = Wait-Until { try { $c = New-Object Net.Sockets.TcpClient('127.0.0.1', $FakePort); $c.Close(); $true } catch { $false } } 8000
Check 'fake provider listening' $fakeReady ''
function Fake-Count([string]$mode) {
    $lines = Get-Content $FakeLog -ErrorAction SilentlyContinue
    if (-not $lines) { return 0 }
    return @($lines | Where-Object { $_ -match "`t$mode$" }).Count
}
function Fake-Reset { Set-Content -Path $FakeLog -Value '' -Encoding UTF8 -NoNewline }

# ---------------------------------------------------------------- server run
function Start-Server([string]$exe, [int]$port, [string]$dir) {
    New-Item -ItemType Directory -Force -Path $dir | Out-Null
    $ini = Join-Path $dir 'api.ini'
    @("[api]", "endpoint=http://127.0.0.1:$FakePort/v1/chat/completions", "model=fake", "key=", "timeout_ms=5000", "concurrency=2") | Set-Content -Path $ini -Encoding ASCII
    $cache = Join-Path $dir 'tm.tsv'
    $stderr = Join-Path $dir ("stderr_" + [DateTime]::UtcNow.ToString('HHmmssfff') + ".log")
    $p = Start-Process -FilePath $exe -ArgumentList @('--port', "$port", '--cache', "`"$cache`"", '--api-config', "`"$ini`"") -WorkingDirectory (Split-Path $exe) -PassThru -NoNewWindow -RedirectStandardError $stderr
    $null = $p.Handle   # PS 5.1 quirk: ExitCode is only readable later if the handle was touched before exit
    $ok = Wait-Until { (Invoke-Http 'GET' "http://127.0.0.1:$port/health" $null $null).Status -eq 200 } 15000
    return @{ Proc = $p; Port = $port; Base = "http://127.0.0.1:$port"; Cache = $cache; Stderr = $stderr; Ready = $ok }
}

function Test-Server([string]$label, [string]$exe, [int]$port) {
    Note ""
    Note "== $label ($exe) on port $port =="
    if (-not (Test-Path $exe)) { Check "$label binary exists" $false $exe; return }
    $dir = Join-Path $Work $label
    $S = Start-Server $exe $port $dir
    Check "$label starts and answers /health" $S.Ready ''
    if (-not $S.Ready) { try { $S.Proc.Kill() } catch {}; return }
    $B = $S.Base
    Fake-Reset

    $h = J (Invoke-Http 'GET' "$B/health" $null $null).Body
    Check "$label /health api_enabled=true" ($h.api_enabled -eq $true) ("server=" + $h.server)
    $r = Invoke-Http 'GET' "$B/capabilities" $null $null
    Check "$label /capabilities 200 + batch support" ($r.Status -eq 200 -and (J $r.Body).supports.batch -eq $true) $r.Body
    $r = Invoke-Http 'OPTIONS' "$B/translate" $null 'null'
    Check "$label OPTIONS -> 204" ($r.Status -eq 204) "status=$($r.Status)"

    # ---- malformed inputs must be 400, never a dropped connection
    $r = Invoke-Http 'POST' "$B/batch" '{"texts":[1,2]}' $null
    Check "$label POST /batch texts=[1,2] -> 400 missing_text" ($r.Status -eq 400 -and $r.Body -match 'missing_text') "status=$($r.Status) body=$($r.Body)"
    $r = Invoke-Http 'POST' "$B/batch" '{"texts":[null]}' $null
    Check "$label POST /batch texts=[null] -> 400" ($r.Status -eq 400) "status=$($r.Status) body=$($r.Body)"
    $r = Invoke-Http 'POST' "$B/batch" '{"texts":[' $null
    Check "$label POST /batch malformed JSON -> 400" ($r.Status -eq 400) "status=$($r.Status) body=$($r.Body)"
    $deep = ('[' * 5000) + (']' * 5000)
    $r = Invoke-Http 'POST' "$B/batch" "{`"texts`":$deep}" $null
    Check "$label POST /batch 5000-deep nesting -> 400 (no crash)" ($r.Status -eq 400) "status=$($r.Status)"
    $r = Invoke-Http 'GET' "$B/health" $null $null
    Check "$label still alive after deep nesting" ($r.Status -eq 200) ''

    # ---- browser-origin policy
    $r = Invoke-Http 'POST' "$B/shutdown" '' 'null'
    Check "$label Origin:null POST /shutdown -> 403" ($r.Status -eq 403) "status=$($r.Status)"
    $r = Invoke-Http 'GET' "$B/health" $null 'http://evil.example'
    Check "$label Origin:evil GET /health -> 403" ($r.Status -eq 403) "status=$($r.Status)"
    $r = Invoke-Http 'GET' "$B/health" $null 'file://'
    Check "$label Origin:file:// GET /health -> 200" ($r.Status -eq 200) "status=$($r.Status)"

    # ---- live translation via /translate (single) and /batch
    $t1 = 'Welcome to the tavern, weary traveler.'
    $r = Invoke-Http 'POST' "$B/translate" (ConvertTo-Json -Compress @{ text = $t1 }) $null
    $j = J $r.Body
    Check "$label POST /translate live -> api_batch + CJK" ($r.Status -eq 200 -and $j.source -eq 'api_batch' -and $j.translation -eq ($YI + ':' + $t1) -and $j.translated_text -eq $j.translation) $r.Body
    $r = Invoke-Http 'GET' ("$B/translate?text=" + [Uri]::EscapeDataString($t1)) $null $null
    Check "$label GET /translate cache hit -> plain text" ($r.Status -eq 200 -and $r.Body -eq ($YI + ':' + $t1)) "status=$($r.Status) body=$($r.Body)"
    $r = Invoke-Http 'POST' "$B/batch" '{"texts":"A lone string instead of an array"}' $null
    $j = J $r.Body
    Check "$label POST /batch texts=<string> -> single-element batch" ($r.Status -eq 200 -and $j.sources.Count -eq 1 -and $j.sources[0] -eq 'api_batch') $r.Body
    # [2] has no ASCII letters and no non-ASCII: no translation signal -> pass-through, never sent to the provider
    $texts = @('Open the ancient gate.', 'Open the ancient gate.', '12345 ... !!!', 'Health potion restores 50 HP.', $t1)
    $r = Invoke-Http 'POST' "$B/batch" (ConvertTo-Json -Compress @{ texts = $texts }) $null
    $j = J $r.Body
    $dupOk = ($j.results[0] -eq $j.results[1]) -and ($j.sources[0] -eq 'api_batch') -and ($j.sources[1] -eq 'api_batch')
    Check "$label POST /batch mixed: dedup + cache + api_batch" ($r.Status -eq 200 -and $dupOk -and $j.sources[4] -eq 'cache' -and $j.results.Count -eq 5) ("sources=" + ($j.sources -join ','))
    Check "$label POST /batch: no-signal text is pass-through (source=pass)" ($j.sources[2] -eq 'pass' -and $j.results[2] -eq $texts[2]) ("sources=" + ($j.sources -join ','))
    $r = Invoke-Http 'GET' "$B/translate?text=Cache%20only%20miss%20line&cache_only=1" $null $null
    Check "$label GET /translate cache_only miss -> 200 original" ($r.Status -eq 200 -and $r.Body -eq 'Cache only miss line') "status=$($r.Status) body=$($r.Body)"
    $r = Invoke-Http 'POST' "$B/batch" '{"texts":["Queued via cache_only body"],"cache_only":true}' $null
    $j = J $r.Body
    Check "$label POST /batch cache_only -> queued" ($r.Status -eq 200 -and $j.sources[0] -eq 'queued') $r.Body

    # ---- background prefetch with context + memory accounting returns to zero
    Fake-Reset
    $pf = 1..10 | ForEach-Object { "Prefetch line number $_ about dragons." }
    $prevs = @('') + ($pf | Select-Object -First 9)
    $r = Invoke-Http 'POST' "$B/prefetch" (ConvertTo-Json -Compress @{ texts = $pf; prevs = $prevs }) $null
    $j = J $r.Body
    Check "$label POST /prefetch queued=10" ($r.Status -eq 200 -and $j.queued -eq 10) $r.Body
    $done = Wait-Until { (J (Invoke-Http 'POST' "$B/cache/lookup" (ConvertTo-Json -Compress @{ texts = $pf }) $null).Body).hit_count -eq 10 } 20000
    $lk = J (Invoke-Http 'POST' "$B/cache/lookup" (ConvertTo-Json -Compress @{ texts = $pf }) $null).Body
    Check "$label prefetch resolves all 10 via /cache/lookup" $done ("hit_count=" + $lk.hit_count + " miss_count=" + $lk.miss_count)
    Check "$label prefetch used context (ctx) request" ((Fake-Count 'ok') -ge 1 -and ((Get-Content $FakeLog) -match "`tctx`t").Count -ge 1) ("requests=" + ((Get-Content $FakeLog) -join ' ; '))
    Start-Sleep -Milliseconds 800
    $h = J (Invoke-Http 'GET' "$B/health" $null $null).Body
    Check "$label /health async_memory_bytes==0 && async_queue==0 after drain" ($h.async_memory_bytes -eq 0 -and $h.async_queue -eq 0) ("async_memory_bytes=" + $h.async_memory_bytes + " async_queue=" + $h.async_queue)
    Check "$label /health live_memory_bytes==0 after live work" ($h.live_memory_bytes -eq 0) ("live_memory_bytes=" + $h.live_memory_bytes)

    # ---- earlier cache_only misses were queued and should now be resolved
    $lk = J (Invoke-Http 'POST' "$B/cache/lookup" '{"texts":["Cache only miss line","Queued via cache_only body"]}' $null).Body
    Check "$label cache_only misses resolved in background" ($lk.hit_count -eq 2) ("hit_count=" + $lk.hit_count)

    # ---- terminal failure must NOT be amplified by split-retry.
    # The 6 texts may be popped as 1 or 2 background batches (worker timing), so the
    # invariant is: provider calls == number of aborted batches (1 call per batch) and <= 2;
    # an amplifying implementation would show 1+2+4.. or per-item calls (>= 7).
    function Abort-Count { $e = (Get-Content $S.Stderr -Raw -ErrorAction SilentlyContinue); if (-not $e) { return 0 }; return ([regex]::Matches($e, 'split-retry aborted')).Count }
    Fake-Reset
    $abortsBefore = Abort-Count
    $bad = 1..6 | ForEach-Object { "FAIL500 outage sentence $_ for the split test." }
    $r = Invoke-Http 'POST' "$B/prefetch" (ConvertTo-Json -Compress @{ texts = $bad }) $null
    $settled = Wait-Until { (Fake-Count 'FAIL500') -ge 1 } 15000
    Start-Sleep -Milliseconds 2500
    $n500 = Fake-Count 'FAIL500'
    $aborts = (Abort-Count) - $abortsBefore
    Check "$label terminal 500 failure -> 1 request per batch, no split amplification" ($settled -and $n500 -le 2 -and $n500 -eq $aborts) "requests=$n500 aborted_batches=$aborts"
    $lk = J (Invoke-Http 'POST' "$B/cache/lookup" (ConvertTo-Json -Compress @{ texts = $bad }) $null).Body
    Check "$label terminal failure leaves 0 cache hits" ($lk.hit_count -eq 0) ("hit_count=" + $lk.hit_count)
    $h = J (Invoke-Http 'GET' "$B/health" $null $null).Body
    Check "$label accounting released after terminal failure" ($h.async_memory_bytes -eq 0 -and $h.async_queue -eq 0) ("async_memory_bytes=" + $h.async_memory_bytes)
    $err = (Get-Content $S.Stderr -Raw -ErrorAction SilentlyContinue)
    Check "$label stderr records split-retry aborted + http-status" ($err -match 'split-retry aborted' -and $err -match 'http-status') ''

    # ---- 401 is terminal too
    Fake-Reset
    $abortsBefore = Abort-Count
    $bad401 = 1..5 | ForEach-Object { "FAIL401 auth sentence $_." }
    $r = Invoke-Http 'POST' "$B/prefetch" (ConvertTo-Json -Compress @{ texts = $bad401 }) $null
    $settled = Wait-Until { (Fake-Count 'FAIL401') -ge 1 } 15000
    Start-Sleep -Milliseconds 2500
    $n401 = Fake-Count 'FAIL401'
    $aborts = (Abort-Count) - $abortsBefore
    Check "$label terminal 401 failure -> 1 request per batch" ($settled -and $n401 -le 2 -and $n401 -eq $aborts) "requests=$n401 aborted_batches=$aborts"

    # ---- shape failure still splits: 6 -> (3,3) -> 6 singles = 9 requests, all resolved
    Fake-Reset
    $shape = 1..6 | ForEach-Object { "BADSHAPE model drops an element $_." }
    $r = Invoke-Http 'POST' "$B/prefetch" (ConvertTo-Json -Compress @{ texts = $shape }) $null
    $done = Wait-Until { (J (Invoke-Http 'POST' "$B/cache/lookup" (ConvertTo-Json -Compress @{ texts = $shape }) $null).Body).hit_count -eq 6 } 25000
    Start-Sleep -Milliseconds 800
    $reqLines = @((Get-Content $FakeLog) | Where-Object { $_ -match "`t(BADSHAPE|ok)$" })
    $lk = J (Invoke-Http 'POST' "$B/cache/lookup" (ConvertTo-Json -Compress @{ texts = $shape }) $null).Body
    Check "$label shape failure splits and resolves all 6" $done ("hit_count=" + $lk.hit_count)
    # one 6-batch: 1 + 2 + 6 singles = 9; two 3-batches: 2 + 6 singles = 8
    Check "$label shape failure request tree is 8 or 9 (binary split, then per-item)" ($reqLines.Count -eq 9 -or $reqLines.Count -eq 8) ("requests=" + $reqLines.Count + " :: " + (($reqLines | ForEach-Object { ($_ -split "`t")[1..3] -join '/' }) -join ' '))
    $err = (Get-Content $S.Stderr -Raw -ErrorAction SilentlyContinue)
    Check "$label stderr records batch-shape" ($err -match 'batch-shape') ''

    # ---- prompt-echo stripping parity
    Fake-Reset
    # Two separate live requests: the fake picks its reply mode per provider request,
    # so the two texts must not share one batch.
    $echoTexts = @('ECHO the model repeats the prompt prefix.', 'ECHOBARE the model returns only a prefix-like phrase.')
    $r1 = J (Invoke-Http 'POST' "$B/translate" (ConvertTo-Json -Compress @{ text = $echoTexts[0] }) $null).Body
    $r2 = J (Invoke-Http 'POST' "$B/translate" (ConvertTo-Json -Compress @{ text = $echoTexts[1] }) $null).Body
    $lk = J (Invoke-Http 'POST' "$B/cache/lookup" (ConvertTo-Json -Compress @{ texts = $echoTexts }) $null).Body
    $v1 = [string]$lk.hits.($echoTexts[0]); $v2 = [string]$lk.hits.($echoTexts[1])
    Check "$label echo prefix with separator is stripped" ($v1 -eq ($YI + ':' + $echoTexts[0])) ("value=" + $v1 + " live_source=" + $r1.source)
    Check "$label prefix-like phrase without separator is kept intact" ($v2 -eq $BARE) ("value=" + $v2 + " expected=" + $BARE + " len=" + $v2.Length + "/" + $BARE.Length + " live_source=" + $r2.source)

    # ---- cache import / export / dump
    $r = Invoke-Http 'POST' "$B/cache/import" (ConvertTo-Json -Compress @{ entries = @(@{ key = 'Imported line'; value = $IMPORTV }, @{ key = 'Echo line'; value = 'Echo line' }) }) $null
    $j = J $r.Body
    Check "$label /cache/import accepts 1, rejects identity echo" ($r.Status -eq 200 -and $j.accepted -eq 1 -and $j.rejected -eq 1 -and $j.imported -eq 1) $r.Body
    $lk = J (Invoke-Http 'POST' "$B/cache/lookup" '{"texts":["Imported line","Echo line"]}' $null).Body
    Check "$label imported entry visible, echo not" ($lk.hit_count -eq 1 -and $lk.hits.'Imported line' -eq $IMPORTV) ("hit_count=" + $lk.hit_count)
    $r = Invoke-Http 'GET' "$B/cache/dump" $null $null
    $j = J $r.Body
    Check "$label /cache/dump count matches health.cache_size" ($r.Status -eq 200 -and $j.count -eq (J (Invoke-Http 'GET' "$B/health" $null $null).Body).cache_size) ("count=" + $j.count)
    $r = Invoke-Http 'POST' "$B/cache/export" '' $null
    Check "$label /cache/export 200" ($r.Status -eq 200) ("status=" + $r.Status + " body=" + $r.Body.Substring(0, [Math]::Min(120, $r.Body.Length)))

    # ---- idle connection must be closed by the recv timeout
    # serve_one answers a timed-out (incomplete) request with 400 incomplete_request
    # and then closes; the C# server does the same. Drain any bytes, then expect EOF.
    $tc = New-Object Net.Sockets.TcpClient('127.0.0.1', $port)
    $sw = [Diagnostics.Stopwatch]::StartNew()
    $idleGot = New-Object Text.StringBuilder
    $closed = Wait-Until {
        if (-not $tc.Client.Poll(1000, [Net.Sockets.SelectMode]::SelectRead)) { return $false }
        if ($tc.Client.Available -gt 0) {
            $bb = New-Object byte[] 8192
            $got = $tc.Client.Receive($bb)
            $null = $idleGot.Append([Text.Encoding]::ASCII.GetString($bb, 0, $got))
            return $false
        }
        return $true
    } 12000 100
    $sw.Stop(); $tc.Close()
    $idleText = $idleGot.ToString()
    Check "$label idle connection closed by recv timeout (~5 s)" ($closed -and $sw.ElapsedMilliseconds -ge 4500 -and $sw.ElapsedMilliseconds -le 9000) ("after " + $sw.ElapsedMilliseconds + " ms")
    Check "$label idle timeout answers 400 incomplete_request before close" ($idleText -match '^HTTP/1\.1 400' -and $idleText -match 'incomplete_request') ("got=" + $idleText.Replace("`r`n", ' | '))
    $h = J (Invoke-Http 'GET' "$B/health" $null $null).Body
    Check "$label active_connections back to <=1" ($h.active_connections -le 1) ("active_connections=" + $h.active_connections)

    # ---- concurrency burst: 40 parallel live /translate calls must all answer
    Fake-Reset
    [System.Net.ServicePointManager]::DefaultConnectionLimit = 200
    $burst = 1..40 | ForEach-Object { "Burst sentence $_ in the great hall." }
    $burstScript = {
        param($B, $t)
        [System.Net.ServicePointManager]::Expect100Continue = $false
        $req = [System.Net.HttpWebRequest]::Create("$B/translate")
        $req.Method = 'POST'; $req.Timeout = 60000; $req.KeepAlive = $false
        $bytes = [Text.Encoding]::UTF8.GetBytes((ConvertTo-Json -Compress @{ text = $t }))
        $req.ContentType = 'application/json'; $req.ContentLength = $bytes.Length
        try {
            $s = $req.GetRequestStream(); $s.Write($bytes, 0, $bytes.Length); $s.Close()
            $resp = $req.GetResponse(); $sr = New-Object IO.StreamReader($resp.GetResponseStream(), [Text.Encoding]::UTF8)
            $body = $sr.ReadToEnd(); $resp.Close(); return $body
        } catch { return "ERR " + $_.Exception.Message }
    }
    $pool = [runspacefactory]::CreateRunspacePool(1, 40)
    $pool.Open()
    $handles = @()
    foreach ($t in $burst) {
        $ps = [powershell]::Create()
        $ps.RunspacePool = $pool
        $null = $ps.AddScript($burstScript).AddArgument($B).AddArgument($t)
        $handles += @{ PS = $ps; H = $ps.BeginInvoke() }
    }
    $sw = [Diagnostics.Stopwatch]::StartNew()
    $outs = @()
    foreach ($hd in $handles) {
        $res = $hd.PS.EndInvoke($hd.H)
        $outs += [string]($res | Select-Object -First 1)
        $hd.PS.Dispose()
    }
    $sw.Stop()
    $pool.Close()
    $okCount = @($outs | Where-Object { $_ -match '"source":"api_batch"' }).Count
    Check "$label 40 parallel live translates all api_batch" ($okCount -eq 40) ("api_batch=$okCount of $($outs.Count) in $($sw.ElapsedMilliseconds) ms; sample=" + ($outs | Where-Object { $_ -notmatch 'api_batch' } | Select-Object -First 1))
    $liveReq = @((Get-Content $FakeLog) | Where-Object { $_ -match "`tok$" }).Count
    Note "$label provider calls for the 40-item burst: $liveReq (coalescing; informational)"
    $h = J (Invoke-Http 'GET' "$B/health" $null $null).Body
    Check "$label live_memory_bytes==0 after burst" ($h.live_memory_bytes -eq 0) ("live_memory_bytes=" + $h.live_memory_bytes)
    $sizeBefore = $h.cache_size

    # ---- shutdown + persistence + restart
    $r = Invoke-Http 'POST' "$B/shutdown" '' $null
    Check "$label /shutdown -> shutting_down" ($r.Status -eq 200 -and $r.Body -match 'shutting_down') $r.Body
    $exited = $S.Proc.WaitForExit(15000)
    $S.Proc.Refresh()
    $code = $null
    try { $code = $S.Proc.ExitCode } catch { $code = "n/a ($($_.Exception.GetType().Name))" }
    Check "$label process exits after shutdown (code 0)" ($exited -and $code -eq 0) ("exited=$exited code=$code")
    $tsvLines = @(Get-Content $S.Cache -Encoding UTF8 -ErrorAction SilentlyContinue).Count
    Check "$label TSV row count == cache_size before shutdown" ($tsvLines -eq $sizeBefore) "rows=$tsvLines cache_size=$sizeBefore"
    $S2 = Start-Server $exe $port $dir
    Check "$label restarts on persisted cache" $S2.Ready ''
    if ($S2.Ready) {
        $lk = J (Invoke-Http 'POST' "$B/cache/lookup" (ConvertTo-Json -Compress @{ texts = @($t1, 'Imported line', $pf[3], $echoTexts[1]) }) $null).Body
        Check "$label persisted entries reload" ($lk.hit_count -eq 4 -and $lk.hits.$t1 -eq ($YI + ':' + $t1) -and $lk.hits.($echoTexts[1]) -eq $BARE) ("hit_count=" + $lk.hit_count)
        $h = J (Invoke-Http 'GET' "$B/health" $null $null).Body
        Check "$label cache_size after reload == cache_size before shutdown" ($h.cache_size -eq $sizeBefore) ("cache_size=" + $h.cache_size + " expected=" + $sizeBefore)
        $r = Invoke-Http 'POST' "$B/shutdown" '' $null
        if ($r.Status -ne 200) { Note "$label note: teardown /shutdown returned $($r.Status) ($($r.Body))" }
        if (-not $S2.Proc.WaitForExit(15000)) { Note "$label note: reloaded server did not exit within 15s after teardown /shutdown" }
    }
}

$port = $FirstServerPort
foreach ($exePath in $Exe) {
    $label = [IO.Path]::GetFileNameWithoutExtension($exePath)
    Test-Server $label $exePath $port
    $port++
}

Stop-Job -Job $fakeJob -ErrorAction SilentlyContinue
Remove-Job -Job $fakeJob -Force -ErrorAction SilentlyContinue

Note ""
Note ("== SUMMARY: PASS=" + $script:Pass + " FAIL=" + $script:Fail + " ==")
Note ("results: " + $ResultsPath)
exit $(if ($script:Fail -eq 0) { 0 } else { 1 })
