# Regression tests for cache export endpoints and long persisted TSV rows.

param(
    [int]$Port = 19986,
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

function Send-Json([string]$path, [string]$method = "GET", $body = $null, [int]$timeout = 10) {
    $u = "http://127.0.0.1:$Port$path"
    if (-not [string]::IsNullOrEmpty($body)) {
        $r = Invoke-WebRequest -Uri $u -Method $method -Body $body -ContentType "application/json" -TimeoutSec $timeout -UseBasicParsing
    } else {
        $r = Invoke-WebRequest -Uri $u -Method $method -TimeoutSec $timeout -UseBasicParsing
    }
    return [pscustomobject]@{
        Status = $r.StatusCode
        Body = $r.Content
        Json = ($r.Content | ConvertFrom-Json -ErrorAction Stop)
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

$tmp = Join-Path $env:TEMP ("dst_cache_export_" + [guid]::NewGuid().ToString("N"))
New-Item -ItemType Directory -Path $tmp | Out-Null
$cache = Join-Path $tmp "cache.tsv"
$key = "persist_long_key_$(Get-Random)"
$value = "V" * (70 * 1024)
$b64Key = [Convert]::ToBase64String([System.Text.Encoding]::UTF8.GetBytes($key))
$b64Value = [Convert]::ToBase64String([System.Text.Encoding]::UTF8.GetBytes($value))
Set-Content -LiteralPath $cache -Encoding ASCII -Value ($b64Key + "`t" + $b64Value)

$p = $null
try {
    Write-Host "Starting server on port $Port with temporary cache..." -ForegroundColor Cyan
    $p = Start-Process -FilePath $Exe -ArgumentList @("--port", $Port, "--cache", $cache) -PassThru -WindowStyle Hidden
    Wait-Server

    Write-Host ""
    Write-Host "=== Cache export and persisted long rows ===" -ForegroundColor Cyan

    It "Persisted TSV row above 64KB loads intact" {
        $body = @{ text = $key } | ConvertTo-Json -Compress
        $r = Send-Json "/translate" "POST" $body
        Assert-Eq $r.Status 200 "status"
        Assert-Eq $r.Json.source "cache" "source"
        Assert-Eq $r.Json.translation.Length $value.Length "translation length"
        Assert-Eq $r.Json.translation $value "translation value"
    }

    It "GET /cache/dump returns cache object for plugin preload" {
        $r = Send-Json "/cache/dump"
        Assert-Eq $r.Status 200 "status"
        Assert-Eq $r.Json.count 1 "count"
        $dumped = $r.Json.cache.PSObject.Properties[$key].Value
        Assert-Eq $dumped $value "dumped value"
    }

    It "POST /cache/export returns entries array for legacy clients" {
        $r = Send-Json "/cache/export" "POST" "{}"
        Assert-Eq $r.Status 200 "status"
        Assert-Eq $r.Json.count 1 "count"
        $entry = @($r.Json.entries | Where-Object { $_.key -eq $key })[0]
        if ($null -eq $entry) { throw "export entry not found" }
        Assert-Eq $entry.value $value "exported value"
    }

    It "POST /shutdown exits cleanly" {
        try { Send-Json "/shutdown" "POST" "{}" 2 | Out-Null } catch { }
        $p | Wait-Process -Timeout 5
        Assert-Eq $p.ExitCode 0 "exit code"
    }
} finally {
    if ($p -and -not $p.HasExited) {
        Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue
    }
    Remove-Item -LiteralPath $tmp -Recurse -Force -ErrorAction SilentlyContinue
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
