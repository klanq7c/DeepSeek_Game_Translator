#Requires -Version 5.1
# run_contract_tests.ps1：本地翻译服务器的 HTTP 契约测试。
#
# 对指定的各个服务器二进制运行相同检查，默认包括 C 服务器
# native\dst_server.exe 和 C# 服务器 native\dst_server_cs.exe；
# 假 OpenAI 兼容提供商在后台任务中运行。两种实现
# 必须通过同样的检查；本文件是 docs\USER_GUIDE.md 和 CONTEXT.md
# 所述共享服务器契约的可执行定义。
#
# 每个服务器的覆盖范围：
#   /health、/capabilities、OPTIONS；畸形输入返回 400，不得直接断开连接；
#   浏览器来源策略；实时 /translate 和 /batch（去重、缓存、cache_only）；
#   带上下文的 /prefetch 及内存记账归零；
#   拆分重试：不得放大终止性失败（500/401），响应形状错误仍应拆分；
#   提示词回显剥离的一致性；缓存导入、导出和转储；
#   空闲连接超时返回 400 incomplete_request；40 路并发突发请求；
#   /shutdown、TSV 持久化及重启加载。
#
# 前提：已通过 build_native.bat 构建二进制。测试不会向仓库内写入文件，
# 工作文件位于 %TEMP%\dst_contract_tests。
#
# 用法：
#   powershell -NoProfile -ExecutionPolicy Bypass -File tests\server_contract\run_contract_tests.ps1
#   ... -Exe native\dst_server_cs.exe          （仅测试一种实现）
#   ... -PersistenceOnly                     （仅测试缓存恢复和幂等性）
param(
    [string[]]$Exe,
    [int]$FakePort = 19990,
    [int]$FirstServerPort = 19985,
    [switch]$PersistenceOnly
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

# 非 ASCII 测试向量由码点构造，避免测试数据受源码编码影响；
# 本文件使用带 BOM 的 UTF-8，确保 PowerShell 5.1 正确读取中文注释。
function U([int[]]$codes) { return -join ($codes | ForEach-Object { [char]$_ }) }
$YI      = U 0x8BD1                                      # 假提供商添加的“译”前缀标记
$COLON   = U 0xFF1A                                      # 全角冒号
$ECHOP   = U 0x7B80,0x4F53,0x4E2D,0x6587,0x7FFB,0x8BD1   # 提示词回显前缀
$BARE    = $ECHOP + (U 0x7248,0x672C,0x5DF2,0x4E0A,0x7EBF)  # 类似前缀但不带分隔符的短语
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
    # [string] 参数会把 $null 转为 ''；GET 探针不得写入请求体。
    if (-not [string]::IsNullOrEmpty($Body)) {
        $bytes = [Text.Encoding]::UTF8.GetBytes($Body)
        $req.ContentType = 'application/json'
        $req.ContentLength = $bytes.Length
        # 服务器可能在请求体写入前关闭套接字（关闭服务时会发生这种竞争），
        # 导致 GetRequestStream 抛出异常。此时应与响应路径一致，
        # 报告 Status = -1，避免某项检查已通过，PowerShell 却仍输出
        # 三个连续的空对象方法调用错误。
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

# ---------------------------------------------------------------- 假提供商
# 最小 OpenAI 兼容 /v1/chat/completions 实现，根据用户提示词中的
# 标记选择响应模式，让每个测试控制提供商行为：
#   FAIL500 / FAIL401 -> HTTP 错误（拆分重试的终止性失败）
#   BADSHAPE -> 批量响应多一个元素（形状错误，应拆分）
#   BADTYPE -> 批量响应含 null 条目（类型错误，应拆分）
#   BIGBODY -> 超过 2 MiB 字节上限的有效 UTF-8 JSON
#   ECHO -> 前缀 + 全角冒号 + 译文（前缀必须剥离）
#   ECHOBARE -> 只有类似前缀的短语，没有分隔符（必须保留）
if (-not $PersistenceOnly) {
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
            # 父进程拥有此测试专用端点。先退出阻塞的接收循环，再调用 Stop-Job；
            # Stop-Job 无法中断 AcceptTcpClient。
            if ($headText -match '^POST /__test_stop HTTP/') {
                $stopBytes = [Text.Encoding]::ASCII.GetBytes("HTTP/1.1 200 OK`r`nContent-Length: 0`r`nConnection: close`r`n`r`n")
                $stream.Write($stopBytes, 0, $stopBytes.Length)
                $stream.Flush()
                break
            }
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
            # PS 5.1 的 ConvertFrom-Json 将 JSON 数组作为单个 object[] 输出，用 foreach 展平。
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
            elseif ($user -match 'BADTYPE' -and $items.Count -ge 2) { $mode = 'BADTYPE' }
            elseif ($user -match 'BIGBODY') { $mode = 'BIGBODY' }
            elseif ($user -match 'ECHOBARE') { $mode = 'ECHOBARE' }
            elseif ($user -match 'ECHO') { $mode = 'ECHO' }
            Add-Content -Path $LogPath -Encoding UTF8 -Value ("{0}`t{1}`t{2}`t{3}" -f [DateTimeOffset]::UtcNow.ToUnixTimeMilliseconds(), $kind, $items.Count, $mode)

            $status = 200; $reason = 'OK'; $json = ''
            if ($mode -eq 'FAIL500') { $status = 500; $reason = 'Internal Server Error'; $json = '{"error":"fake outage"}' }
            elseif ($mode -eq 'FAIL401') { $status = 401; $reason = 'Unauthorized'; $json = '{"error":"bad key"}' }
            elseif ($mode -eq 'BIGBODY') { $json = '{"choices":[{"message":{"role":"assistant","content":"' + ($YI * 700000) + '"}}]}' }
            else {
                $out = @()
                foreach ($t in $items) {
                    if ($mode -eq 'ECHO') { $out += ($ECHOP + $COLON + $YI + ':' + $t) }
                    elseif ($mode -eq 'ECHOBARE') { $out += $BARE }
                    else { $out += ($YI + ':' + $t) }
                }
                if ($mode -eq 'BADSHAPE') { $out += $EXTRA }
                if ($mode -eq 'BADTYPE') { $out[0] = $null }
                if ($kind -eq 'single') { $content = [string]$out[0] }
                else { $content = ConvertTo-Json -InputObject $out -Compress }
                $json = ConvertTo-Json -Compress -Depth 6 -InputObject @{ choices = @(@{ message = @{ role = 'assistant'; content = $content } }) }
            }
            $rb = [Text.Encoding]::UTF8.GetBytes($json)
            $head = if ($mode -eq 'BIGBODY') {
                "HTTP/1.1 $status $reason`r`nContent-Type: application/json`r`nConnection: close`r`n`r`n"
            } else {
                "HTTP/1.1 $status $reason`r`nContent-Type: application/json`r`nContent-Length: $($rb.Length)`r`nConnection: close`r`n`r`n"
            }
            $hb = [Text.Encoding]::ASCII.GetBytes($head)
            $stream.Write($hb, 0, $hb.Length); $stream.Write($rb, 0, $rb.Length); $stream.Flush()
        } catch {
            Add-Content -Path $LogPath -Encoding UTF8 -Value ("ERR`t" + $_.Exception.Message)
        } finally {
            $client.Close()
        }
    }
    $listener.Stop()
}
$fakeReady = Wait-Until { try { $c = New-Object Net.Sockets.TcpClient('127.0.0.1', $FakePort); $c.Close(); $true } catch { $false } } 8000
Check 'fake provider listening' $fakeReady ''
}
function Fake-Count([string]$mode) {
    $lines = Get-Content $FakeLog -ErrorAction SilentlyContinue
    if (-not $lines) { return 0 }
    return @($lines | Where-Object { $_ -match "`t$mode$" }).Count
}
function Fake-Reset { Set-Content -Path $FakeLog -Value '' -Encoding UTF8 -NoNewline }

# ---------------------------------------------------------------- 运行服务器
function Start-Server([string]$exe, [int]$port, [string]$dir, [string]$cachePath = '') {
    New-Item -ItemType Directory -Force -Path $dir | Out-Null
    $ini = Join-Path $dir 'api.ini'
    @("[api]", "endpoint=http://127.0.0.1:$FakePort/v1/chat/completions", "model=fake", "key=", "timeout_ms=5000", "concurrency=2") | Set-Content -Path $ini -Encoding ASCII
    $cache = if ($cachePath) { $cachePath } else { Join-Path $dir 'tm.tsv' }
    $stderr = Join-Path $dir ("stderr_" + [DateTime]::UtcNow.ToString('HHmmssfff') + ".log")
    $p = Start-Process -FilePath $exe -ArgumentList @('--port', "$port", '--cache', "`"$cache`"", '--api-config', "`"$ini`"") -WorkingDirectory (Split-Path $exe) -PassThru -WindowStyle Hidden -RedirectStandardError $stderr
    $null = $p.Handle   # PS 5.1 的特殊行为：必须在进程退出前访问句柄，之后才能读取 ExitCode
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

    # ---- 畸形输入必须返回 400，不得直接断开连接
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

    # ---- 浏览器来源策略
    $r = Invoke-Http 'POST' "$B/shutdown" '' 'null'
    Check "$label Origin:null POST /shutdown -> 403" ($r.Status -eq 403) "status=$($r.Status)"
    $r = Invoke-Http 'GET' "$B/health" $null 'http://evil.example'
    Check "$label Origin:evil GET /health -> 403" ($r.Status -eq 403) "status=$($r.Status)"
    $r = Invoke-Http 'GET' "$B/health" $null 'file://'
    Check "$label Origin:file:// GET /health -> 200" ($r.Status -eq 200) "status=$($r.Status)"

    # ---- 通过 /translate（单条）和 /batch 实时翻译
    $t1 = 'Welcome to the tavern, weary traveler.'
    $r = Invoke-Http 'POST' "$B/translate" (ConvertTo-Json -Compress @{ text = $t1 }) $null
    $j = J $r.Body
    Check "$label POST /translate live -> api_batch + CJK" ($r.Status -eq 200 -and $j.source -eq 'api_batch' -and $j.translation -eq ($YI + ':' + $t1) -and $j.translated_text -eq $j.translation) $r.Body
    $r = Invoke-Http 'GET' ("$B/translate?text=" + [Uri]::EscapeDataString($t1)) $null $null
    Check "$label GET /translate cache hit -> plain text" ($r.Status -eq 200 -and $r.Body -eq ($YI + ':' + $t1)) "status=$($r.Status) body=$($r.Body)"
    $r = Invoke-Http 'POST' "$B/batch" '{"texts":"A lone string instead of an array"}' $null
    $j = J $r.Body
    Check "$label POST /batch texts=<string> -> single-element batch" ($r.Status -eq 200 -and $j.sources.Count -eq 1 -and $j.sources[0] -eq 'api_batch') $r.Body
    # [2] 既无 ASCII 字母也无非 ASCII 字符，没有可翻译内容，应原样返回且不发送给提供商
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

    # ---- 带上下文的后台预取，以及内存记账归零
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

    # ---- 先前 cache_only 未命中的文本已入队，此时应已完成翻译
    $lk = J (Invoke-Http 'POST' "$B/cache/lookup" '{"texts":["Cache only miss line","Queued via cache_only body"]}' $null).Body
    Check "$label cache_only misses resolved in background" ($lk.hit_count -eq 2) ("hit_count=" + $lk.hit_count)

    # ---- 终止性失败不得被拆分重试放大
    # 受工作线程时序影响，6 条文本可能作为 1 或 2 个后台批次取出；
    # 不变量为：提供商调用次数等于中止批次数，每批调用一次，且总数不超过 2；
    # 放大失败的实现则会出现 1+2+4… 或逐条请求（至少 7 次）。
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

    # ---- 401 同样属于终止性失败
    Fake-Reset
    $abortsBefore = Abort-Count
    $bad401 = 1..5 | ForEach-Object { "FAIL401 auth sentence $_." }
    $r = Invoke-Http 'POST' "$B/prefetch" (ConvertTo-Json -Compress @{ texts = $bad401 }) $null
    $settled = Wait-Until { (Fake-Count 'FAIL401') -ge 1 } 15000
    Start-Sleep -Milliseconds 2500
    $n401 = Fake-Count 'FAIL401'
    $aborts = (Abort-Count) - $abortsBefore
    Check "$label terminal 401 failure -> 1 request per batch" ($settled -and $n401 -le 2 -and $n401 -eq $aborts) "requests=$n401 aborted_batches=$aborts"

    # ---- 形状错误仍应拆分：6 -> (3,3) -> 6 条单独请求，共 9 次，全部完成翻译
    Fake-Reset
    $shape = 1..6 | ForEach-Object { "BADSHAPE model drops an element $_." }
    $r = Invoke-Http 'POST' "$B/prefetch" (ConvertTo-Json -Compress @{ texts = $shape }) $null
    $done = Wait-Until { (J (Invoke-Http 'POST' "$B/cache/lookup" (ConvertTo-Json -Compress @{ texts = $shape }) $null).Body).hit_count -eq 6 } 25000
    Start-Sleep -Milliseconds 800
    $reqLines = @((Get-Content $FakeLog) | Where-Object { $_ -match "`t(BADSHAPE|ok)$" })
    $lk = J (Invoke-Http 'POST' "$B/cache/lookup" (ConvertTo-Json -Compress @{ texts = $shape }) $null).Body
    Check "$label shape failure splits and resolves all 6" $done ("hit_count=" + $lk.hit_count)
    # 一个 6 条批次：1 + 2 + 6 = 9 次；两个 3 条批次：2 + 6 = 8 次
    Check "$label shape failure request tree is 8 or 9 (binary split, then per-item)" ($reqLines.Count -eq 9 -or $reqLines.Count -eq 8) ("requests=" + $reqLines.Count + " :: " + (($reqLines | ForEach-Object { ($_ -split "`t")[1..3] -join '/' }) -join ' '))
    $err = (Get-Content $S.Stderr -Raw -ErrorAction SilentlyContinue)
    Check "$label stderr records batch-shape" ($err -match 'batch-shape') ''

    # ---- 提示词回显剥离的一致性
    Fake-Reset
    # 使用两次独立实时请求：假提供商按每次请求选择响应模式，
    # 因此两条文本不能共用一个批次。
    $echoTexts = @('ECHO the model repeats the prompt prefix.', 'ECHOBARE the model returns only a prefix-like phrase.')
    $r1 = J (Invoke-Http 'POST' "$B/translate" (ConvertTo-Json -Compress @{ text = $echoTexts[0] }) $null).Body
    $r2 = J (Invoke-Http 'POST' "$B/translate" (ConvertTo-Json -Compress @{ text = $echoTexts[1] }) $null).Body
    $lk = J (Invoke-Http 'POST' "$B/cache/lookup" (ConvertTo-Json -Compress @{ texts = $echoTexts }) $null).Body
    $v1 = [string]$lk.hits.($echoTexts[0]); $v2 = [string]$lk.hits.($echoTexts[1])
    Check "$label echo prefix with separator is stripped" ($v1 -eq ($YI + ':' + $echoTexts[0])) ("value=" + $v1 + " live_source=" + $r1.source)
    Check "$label prefix-like phrase without separator is kept intact" ($v2 -eq $BARE) ("value=" + $v2 + " expected=" + $BARE + " len=" + $v2.Length + "/" + $BARE.Length + " live_source=" + $r2.source)

    # ---- 缓存导入/导出/转储
    $r = Invoke-Http 'POST' "$B/cache/import" (ConvertTo-Json -Compress @{ entries = @(@{ key = 'Imported line'; value = $IMPORTV }, @{ key = 'Echo line'; value = 'Echo line' }) }) $null
    $j = J $r.Body
    Check "$label /cache/import accepts 1, rejects identity echo" ($r.Status -eq 200 -and $j.accepted -eq 1 -and $j.rejected -eq 1 -and $j.imported -eq 1) $r.Body
    $lk = J (Invoke-Http 'POST' "$B/cache/lookup" '{"texts":["Imported line","Echo line"]}' $null).Body
    Check "$label imported entry visible, echo not" ($lk.hit_count -eq 1 -and $lk.hits.'Imported line' -eq $IMPORTV) ("hit_count=" + $lk.hit_count)
    $size = (Get-Item -LiteralPath $S.Cache).Length
    $r = Invoke-Http 'POST' "$B/cache/import" (ConvertTo-Json -Compress @{ entries = @(@{ key = 'Imported line'; value = $IMPORTV }) }) $null
    $j = J $r.Body
    Check "$label healthy duplicate import is successful without another row" ($j.status -eq 'ok' -and $j.imported -eq 1 -and $j.memory_only -eq 0 -and (Get-Item -LiteralPath $S.Cache).Length -eq $size) $r.Body
    $r = Invoke-Http 'GET' "$B/cache/dump" $null $null
    $j = J $r.Body
    Check "$label /cache/dump count matches health.cache_size" ($r.Status -eq 200 -and $j.count -eq (J (Invoke-Http 'GET' "$B/health" $null $null).Body).cache_size) ("count=" + $j.count)
    $r = Invoke-Http 'POST' "$B/cache/export" '' $null
    Check "$label /cache/export 200" ($r.Status -eq 200) ("status=" + $r.Status + " body=" + $r.Body.Substring(0, [Math]::Min(120, $r.Body.Length)))

    # ---- 空闲连接必须由接收超时关闭
    # serve_one 对超时的不完整请求返回 400 incomplete_request 后关闭连接；
    # C# 服务器行为相同。读尽已有字节，然后应收到 EOF。
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

    # ---- 并发突发请求：40 路并行实时 /translate 调用都必须获得响应
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

    # 提供商可能返回长度正确但包含非字符串条目的 JSON 数组；
    # 必须拒绝整个批次并拆分。若工作线程抛出未处理异常，可能导致
    # C# 进程退出，或使已入队文本永久无法完成翻译。
    Fake-Reset
    $badType = 1..6 | ForEach-Object { "BADTYPE model null element sentence $_." }
    $r = Invoke-Http 'POST' "$B/prefetch" (ConvertTo-Json -Compress @{ texts = $badType }) $null
    $typeDone = Wait-Until {
        $response = Invoke-Http 'POST' "$B/cache/lookup" (ConvertTo-Json -Compress @{ texts = $badType }) $null
        $response.Status -eq 200 -and (J $response.Body).hit_count -eq 6
    } 25000
    $typeDrained = Wait-Until {
        $health = Invoke-Http 'GET' "$B/health" $null $null
        if ($health.Status -ne 200) { return $false }
        $snapshot = J $health.Body
        $null -ne $snapshot -and $snapshot.async_memory_bytes -eq 0 -and $snapshot.async_queue -eq 0
    } 5000
    $h = J (Invoke-Http 'GET' "$B/health" $null $null).Body
    Check "$label non-string provider item splits and resolves all 6" $typeDone ("health=" + ($null -ne $h) + " provider_requests=" + (Fake-Count 'BADTYPE'))
    Check "$label non-string provider item releases queue accounting" $typeDrained ("async_memory_bytes=" + $h.async_memory_bytes)

    # 请求体不足 2 Mi 个 UTF-16 字符，但超过 2 MiB UTF-8 字节；
    # 两版服务器都必须拒绝，不能将其写成已翻译的缓存值。
    Fake-Reset
    $largeText = 'BIGBODY provider response byte limit must stay unresolved.'
    $r = Invoke-Http 'POST' "$B/translate" (ConvertTo-Json -Compress @{ text = $largeText }) $null
    $j = J $r.Body
    $lk = J (Invoke-Http 'POST' "$B/cache/lookup" (ConvertTo-Json -Compress @{ texts = @($largeText) }) $null).Body
    Check "$label oversized UTF-8 provider response stays unresolved" `
        ($r.Status -eq 200 -and $j.translation -eq $largeText -and $lk.hit_count -eq 0) `
        ("status=" + $r.Status + " source=" + $j.source + " hits=" + $lk.hit_count)
    Check "$label oversized provider response records byte-limit diagnostic" `
        ((Get-Content $S.Stderr -Raw -ErrorAction SilentlyContinue) -match 'too-large') ''
    $sizeBefore = $h.cache_size

    # ---- 关闭、持久化及重启
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
        $burstReload = J (Invoke-Http 'POST' "$B/cache/lookup" (ConvertTo-Json -Compress @{ texts = $burst }) $null).Body
        $allBurstReloaded = $burstReload.hit_count -eq 40
        foreach ($text in $burst) { $allBurstReloaded = $allBurstReloaded -and $burstReload.hits.$text -eq ($YI + ':' + $text) }
        Check "$label all 40 live batch values survive restart" $allBurstReloaded ("hit_count=" + $burstReload.hit_count)
        $h = J (Invoke-Http 'GET' "$B/health" $null $null).Body
        Check "$label cache_size after reload == cache_size before shutdown" ($h.cache_size -eq $sizeBefore) ("cache_size=" + $h.cache_size + " expected=" + $sizeBefore)
        $r = Invoke-Http 'POST' "$B/shutdown" '' $null
        if ($r.Status -ne 200) { Note "$label note: teardown /shutdown returned $($r.Status) ($($r.Body))" }
        if (-not $S2.Proc.WaitForExit(15000)) { Note "$label note: reloaded server did not exit within 15s after teardown /shutdown" }
    }
}

# 此夹具不加入 API 工作；父目录缺失使首次追加失败，
# 创建目录即可修复外部故障，无需修改缓存的内存映射。
function Test-CachePersistence([string]$label, [string]$exe, [int]$port) {
    $ErrorActionPreference = 'Stop'
    Note ""
    Note "== $label cache persistence recovery =="
    $dir = Join-Path $Work ($label + '_persistence')
    $parent = Join-Path $dir 'initially_missing'
    $cachePath = Join-Path $parent 'tm.tsv'
    $S = $null
    $phase = 'start'
    try {
        $S = Start-Server $exe $port $dir $cachePath
        Check "$label persistence fixture ready" $S.Ready ''
        if (-not $S.Ready) { return }
        $B = $S.Base
        $body = ConvertTo-Json -Compress @{ entries = @(
            @{ key = 'Recovery line'; value = $IMPORTV },
            @{ key = 'Recovery [player] {b}line{/b}'; value = ($IMPORTV + ' [player] {b}' + $YI + '{/b}') }
        ) }
        $phase = 'failed append'
        $r = Invoke-Http 'POST' "$B/cache/import" $body $null
        $j = J $r.Body
        Check "$label failed append reports memory-only entries" ($r.Status -eq 200 -and $j.status -eq 'error' -and $j.accepted -eq 2 -and $j.imported -eq 0 -and $j.memory_only -eq 2) $r.Body
        $lookup = '{"texts":["Recovery line","Recovery [player] {b}line{/b}"]}'
        $lk = J (Invoke-Http 'POST' "$B/cache/lookup" $lookup $null).Body
        Check "$label failed append preserves usable in-memory translations" ($lk.hit_count -eq 2 -and $lk.hits.'Recovery line' -eq $IMPORTV) (ConvertTo-Json -Compress $lk)
        $diagnostic = Get-Content $S.Stderr -Raw
        Check "$label failed append records persistence diagnostic" ($diagnostic -match 'persist-open') $diagnostic
        New-Item -ItemType Directory -Path $parent | Out-Null
        # 一条有效的相邻记录，后接下一行中断写入后留下的四个字节。
        $tailKey = 'Tail guard <color=red>line</color>'
        $tailValue = $IMPORTV + ' <color=red>' + $YI + '</color>'
        $tailPrefix = [Convert]::ToBase64String([Text.Encoding]::UTF8.GetBytes($tailKey)) + "`t" +
            [Convert]::ToBase64String([Text.Encoding]::UTF8.GetBytes($tailValue)) + "`nQnJv"
        [IO.File]::WriteAllText($cachePath, $tailPrefix, (New-Object Text.UTF8Encoding($false)))
        $phase = 'same-value retry'
        $r = Invoke-Http 'POST' "$B/cache/import" $body $null
        $j = J $r.Body
        Check "$label same-value retry persists after path recovery" ($j.status -eq 'ok' -and $j.accepted -eq 2 -and $j.imported -eq 2 -and $j.memory_only -eq 0) $r.Body
        # 保持生产写入器打开，检查已刷新的文件内容。
        $tailStream = New-Object IO.FileStream($cachePath, [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::ReadWrite)
        $tailReader = New-Object IO.StreamReader($tailStream)
        try { $tailActual = $tailReader.ReadToEnd() } finally { $tailReader.Dispose() }
        Check "$label incomplete tail is preserved and separated before retry" ($tailActual.StartsWith($tailPrefix + "`n")) ''
        Check "$label incomplete tail emits a recovery diagnostic" ((Get-Content $S.Stderr -Raw) -match 'persist-tail') ''
        $size = if (Test-Path -LiteralPath $cachePath) { (Get-Item -LiteralPath $cachePath).Length } else { 0 }
        $r = Invoke-Http 'POST' "$B/cache/import" $body $null
        $j = J $r.Body
        $sizeAfter = if (Test-Path -LiteralPath $cachePath) { (Get-Item -LiteralPath $cachePath).Length } else { 0 }
        Check "$label persisted duplicates count as success without appending" ($size -gt 0 -and $sizeAfter -eq $size -and $j.status -eq 'ok' -and $j.imported -eq 2 -and $j.memory_only -eq 0) $r.Body
        $phase = 'mixed and overwritten values'
        $mixed = ConvertTo-Json -Compress @{ entries = @(
            @{ key = 'Recovery line'; value = $IMPORTV },
            @{ key = 'New line'; value = $YI },
            @{ key = 'Untranslated echo'; value = 'Untranslated echo' }
        ) }
        $r = Invoke-Http 'POST' "$B/cache/import" $mixed $null
        $j = J $r.Body
        Check "$label mixed duplicate/new/identity import counts correctly" ($j.status -eq 'partial' -and $j.accepted -eq 2 -and $j.imported -eq 2 -and $j.rejected -eq 1 -and $j.memory_only -eq 0) $r.Body
        # 同一批次中 A -> B -> B，内存和磁盘都必须保留 B。
        $overwrite = ConvertTo-Json -Compress @{ entries = @(
            @{ key = 'New line'; value = $IMPORTV },
            @{ key = 'New line'; value = $YI },
            @{ key = 'New line'; value = $YI }
        ) }
        $r = Invoke-Http 'POST' "$B/cache/import" $overwrite $null
        $j = J $r.Body
        Check "$label repeated keys in one batch retain correct persistence counts" ($j.status -eq 'ok' -and $j.accepted -eq 3 -and $j.imported -eq 3 -and $j.memory_only -eq 0) $r.Body
        $lk = J (Invoke-Http 'POST' "$B/cache/lookup" '{"texts":["New line","Untranslated echo"]}' $null).Body
        Check "$label overwrite keeps last value and rejected identity stays absent" ($lk.hit_count -eq 1 -and $lk.hits.'New line' -eq $YI -and $lk.miss_count -eq 1) (ConvertTo-Json -Compress $lk)
        $phase = 'batch commit'
        $bulkEntries = @()
        $bulkExpected = @{}
        $markup = @('[player] {b}line{/b}', '\c[2]line\v[1]', '<color=red>line</color>', '[color=red]line[/color]')
        for ($i = 0; $i -lt 128; $i++) {
            $key = 'Bulk line ' + $i
            $value = $YI + ':' + $i + ' ' + $markup[$i % $markup.Count]
            $bulkEntries += @{ key = $key; value = $value }
            $bulkExpected[$key] = $value
        }
        # A -> B -> A 即使与其他记录处于同一批次，重启后也必须保留 A。
        $bulkEntries += @(
            @{ key = 'Cycle line'; value = $IMPORTV },
            @{ key = 'Cycle line'; value = $YI },
            @{ key = 'Cycle line'; value = $IMPORTV }
        )
        $bulkExpected['Cycle line'] = $IMPORTV
        $r = Invoke-Http 'POST' "$B/cache/import" (ConvertTo-Json -Compress -Depth 5 @{ entries = $bulkEntries }) $null
        $j = J $r.Body
        Check "$label batch import including A-B-A confirms every accepted row" ($j.status -eq 'ok' -and $j.accepted -eq 131 -and $j.imported -eq 131 -and $j.memory_only -eq 0) $r.Body
        $bulkLookup = ConvertTo-Json -Compress @{ texts = @($bulkExpected.Keys) }
        $lk = J (Invoke-Http 'POST' "$B/cache/lookup" $bulkLookup $null).Body
        $bulkMatches = $lk.hit_count -eq $bulkExpected.Count
        foreach ($key in $bulkExpected.Keys) { $bulkMatches = $bulkMatches -and $lk.hits.$key -ceq $bulkExpected[$key] }
        Check "$label batch retains final values and all four engine markup forms" $bulkMatches (ConvertTo-Json -Compress $lk)
        $phase = 'restart'
        $r = Invoke-Http 'POST' "$B/shutdown" '' $null
        $exited = $S.Proc.WaitForExit(15000)
        Check "$label persistence fixture shuts down" ($r.Status -eq 200 -and $exited) $r.Body
        if (-not $exited) { return }
        $S.Proc.Dispose()
        $S = $null
        $S = Start-Server $exe $port $dir $cachePath
        Check "$label persistence fixture restarts" $S.Ready ''
        if (-not $S.Ready) { return }
        $B = $S.Base
        $tailLookup = ConvertTo-Json -Compress @{ texts = @($tailKey, 'BroRecovery line') }
        $tailHit = J (Invoke-Http 'POST' "$B/cache/lookup" $tailLookup $null).Body
        Check "$label tail repair retains adjacent markup without a concatenated key" ($tailHit.hit_count -eq 1 -and $tailHit.hits.$tailKey -ceq $tailValue) ''
        $lk = J (Invoke-Http 'POST' "$B/cache/lookup" '{"texts":["Recovery line","Recovery [player] {b}line{/b}","New line","Untranslated echo"]}' $null).Body
        Check "$label recovered entries and markup survive restart" ($lk.hit_count -eq 3 -and $lk.miss_count -eq 1 -and $lk.hits.'Recovery line' -eq $IMPORTV -and $lk.hits.'Recovery [player] {b}line{/b}' -eq ($IMPORTV + ' [player] {b}' + $YI + '{/b}') -and $lk.hits.'New line' -eq $YI) (ConvertTo-Json -Compress $lk)
        $lk = J (Invoke-Http 'POST' "$B/cache/lookup" $bulkLookup $null).Body
        $bulkMatches = $lk.hit_count -eq $bulkExpected.Count
        foreach ($key in $bulkExpected.Keys) { $bulkMatches = $bulkMatches -and $lk.hits.$key -ceq $bulkExpected[$key] }
        Check "$label complete batch and A-B-A final value survive restart" $bulkMatches (ConvertTo-Json -Compress $lk)
        $size = (Get-Item -LiteralPath $cachePath).Length
        $r = Invoke-Http 'POST' "$B/cache/import" $body $null
        $j = J $r.Body
        Check "$label disk-loaded duplicates count as persisted without appending" ($j.status -eq 'ok' -and $j.imported -eq 2 -and $j.memory_only -eq 0 -and (Get-Item -LiteralPath $cachePath).Length -eq $size) $r.Body
    } catch {
        Check "$label persistence fixture exception" $false ("phase=" + $phase + ': ' + $_.Exception.Message)
    } finally {
        if ($S -and $S.Proc) {
            if (-not $S.Proc.HasExited) {
                $r = Invoke-Http 'POST' "$($S.Base)/shutdown" '' $null
                if (-not $S.Proc.WaitForExit(15000)) {
                    Check "$label persistence fixture cleanup" $false 'Owned server did not exit after shutdown; terminating it.'
                    $S.Proc.Kill()
                    $S.Proc.WaitForExit()
                }
            }
            $S.Proc.Dispose()
        }
    }
}

$port = $FirstServerPort
foreach ($exePath in $Exe) {
    $label = [IO.Path]::GetFileNameWithoutExtension($exePath)
    if (-not $PersistenceOnly) { Test-Server $label $exePath $port }
    Test-CachePersistence $label $exePath $port
    $port++
}

if ($fakeJob) {
    $fakeStop = Invoke-Http 'POST' "http://127.0.0.1:$FakePort/__test_stop" '' $null
    $fakeFinished = Wait-Job -Job $fakeJob -Timeout 5
    Check 'fake provider exits cleanly' ($fakeStop.Status -eq 200 -and $null -ne $fakeFinished -and $fakeJob.State -eq 'Completed') ("status=" + $fakeStop.Status + " state=" + $fakeJob.State)
    if ($null -eq $fakeFinished) { Stop-Job -Job $fakeJob -ErrorAction SilentlyContinue }
    Remove-Job -Job $fakeJob -Force -ErrorAction SilentlyContinue
}

Note ""
Note ("== SUMMARY: PASS=" + $script:Pass + " FAIL=" + $script:Fail + " ==")
Note ("results: " + $ResultsPath)
exit $(if ($script:Fail -eq 0) { 0 } else { 1 })
