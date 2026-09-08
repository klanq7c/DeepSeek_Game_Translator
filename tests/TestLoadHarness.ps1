$script:TestLoadCacheKey = "dst_endurance_hot_key"

function New-TestLoadCacheFixture {
    [CmdletBinding()]
    param(
        [string]$RequestedPath = "",
        [Parameter(Mandatory = $true)][string]$Name
    )

    $path = $RequestedPath
    if ([string]::IsNullOrWhiteSpace($path)) {
        $path = Join-Path ([IO.Path]::GetTempPath()) (
            "ds_" + $Name + "_" + [Guid]::NewGuid().ToString("N") + ".tsv")
    }
    $path = [IO.Path]::GetFullPath($path)
    if (Test-Path -LiteralPath $path) {
        throw "refusing to overwrite an existing load-test cache: $path"
    }
    $parent = Split-Path -Parent $path
    if (-not $parent -or -not (Test-Path -LiteralPath $parent -PathType Container)) {
        throw "load-test cache parent does not exist: $parent"
    }

    $key = [Convert]::ToBase64String(
        [Text.Encoding]::UTF8.GetBytes($script:TestLoadCacheKey))
    $value = [Convert]::ToBase64String(
        [Text.Encoding]::UTF8.GetBytes("dst_cached_result"))
    [IO.File]::WriteAllText($path, "$key`t$value`n", [Text.Encoding]::ASCII)

    return [pscustomobject]@{
        Path = $path
        Key = $script:TestLoadCacheKey
        Owned = $true
    }
}

function Remove-TestLoadCacheFixture {
    [CmdletBinding()]
    param([AllowNull()][object]$Fixture)

    if (-not $Fixture -or -not $Fixture.Owned -or -not $Fixture.Path) { return }
    if (Test-Path -LiteralPath $Fixture.Path) {
        Remove-Item -LiteralPath $Fixture.Path -Force -ErrorAction Stop
    }
}

function New-TestLoadClient {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory = $true)][string]$SourcePath,
        [Parameter(Mandatory = $true)][string]$CompilerPath
    )

    $outputPath = Join-Path ([IO.Path]::GetTempPath()) (
        "ds_bench_client_" + [Guid]::NewGuid().ToString("N") + ".exe")
    $compilerDirectory = Split-Path -Parent $CompilerPath
    $previousPath = $env:PATH
    try {
        $env:PATH = "$compilerDirectory;$env:PATH"
        & $CompilerPath -O2 -Wall -Wextra -Werror $SourcePath -lws2_32 -o $outputPath
        $compileExitCode = $LASTEXITCODE
    } finally {
        $env:PATH = $previousPath
    }
    if ($compileExitCode -ne 0 -or -not (Test-Path -LiteralPath $outputPath -PathType Leaf)) {
        if (Test-Path -LiteralPath $outputPath) {
            Remove-Item -LiteralPath $outputPath -Force -ErrorAction SilentlyContinue
        }
        throw "bench_client build failed"
    }
    return $outputPath
}

function Wait-TestTranslationServerReady {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory = $true)][System.Diagnostics.Process]$Process,
        [Parameter(Mandatory = $true)][int]$Port,
        [ValidateRange(1, 120)][int]$TimeoutSec = 30
    )

    $deadline = [DateTime]::UtcNow.AddSeconds($TimeoutSec)
    $lastReadinessError = "health endpoint has not responded"
    while (([DateTime]::UtcNow) -lt $deadline) {
        $Process.Refresh()
        if ($Process.HasExited) {
            throw "test server exited before readiness (code=$($Process.ExitCode))"
        }
        try {
            $response = Invoke-WebRequest -Uri "http://127.0.0.1:$Port/health" `
                -UseBasicParsing -TimeoutSec 1
            if ($response.StatusCode -eq 200) {
                $health = $response.Content | ConvertFrom-Json -ErrorAction Stop
                if ($health.status -eq "ok" -and [long]$health.cache_size -ge 1) {
                    $Process.Refresh()
                    if ($Process.HasExited) {
                        throw "test server exited during readiness verification"
                    }
                    return $health
                }
            }
        } catch {
            $lastReadinessError = $_.Exception.Message
        }
        Start-Sleep -Milliseconds 100
    }
    throw "test server did not become healthy with the hot cache within $TimeoutSec seconds: $lastReadinessError"
}

function Read-TestLoadRequestCount {
    [CmdletBinding()]
    param([Parameter(Mandatory = $true)][string]$Path)

    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        throw "load generator did not create its request-count output"
    }
    $text = (Get-Content -LiteralPath $Path -Raw).Trim()
    $count = 0L
    if (-not [long]::TryParse($text, [ref]$count)) {
        throw "load generator returned an invalid request count: '$text'"
    }
    return $count
}
