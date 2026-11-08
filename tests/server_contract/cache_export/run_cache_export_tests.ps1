#Requires -Version 5.1
param([switch]$Benchmark)
$ErrorActionPreference = 'Stop'
$repo = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '../../..')).Path
$output = Join-Path $repo 'build/cache_export'
New-Item -ItemType Directory -Path $output -Force | Out-Null
$gcc = Join-Path $repo 'native/toolchain/w64devkit/bin/gcc.exe'
if (-not (Test-Path -LiteralPath $gcc)) { $gcc = (Get-Command gcc -ErrorAction Stop).Source }
$source = Join-Path $repo 'native/src/server'
$exe = Join-Path $output 'cache_export_tests.exe'
$commonArgs = @('-std=c17', '-O2', '-Wall', '-Wextra', '-Werror', ('-I' + $source),
    (Join-Path $source 'buf.c'), (Join-Path $source 'util.c'), (Join-Path $source 'b64.c'))
$savedPath = $env:PATH
try {
    $env:PATH = (Split-Path -Parent $gcc) + ';' + $savedPath
    & $gcc @commonArgs (Join-Path $PSScriptRoot 'cache_export_tests.c') -o $exe
    if ($LASTEXITCODE -ne 0) { throw 'Cache export guard build failed' }
    if ($Benchmark) {
        $probe = Join-Path $output 'cache_export_probe.exe'
        & $gcc @commonArgs (Join-Path $PSScriptRoot 'cache_export_probe.c') -lpsapi -o $probe
        if ($LASTEXITCODE -ne 0) { throw 'Cache export benchmark build failed' }
    }
} finally { $env:PATH = $savedPath }

# 夹具只写唯一目录，保留失败证据；不读取或改动用户缓存。
$fixture = Join-Path ([IO.Path]::GetFullPath([IO.Path]::GetTempPath())) ('dst_cache_export_' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $fixture | Out-Null
& $exe (Join-Path $fixture 'map.tsv') (Join-Path $fixture 'entries.tsv')
if ($LASTEXITCODE -ne 0) { throw "Cache export guard failed; fixture retained at $fixture" }
Write-Host "Cache export fixture: $fixture"
if ($Benchmark) {
    # 可选测量不参与时间阈值门禁；每个进程三次预热、三十次样本。
    foreach ($count in @(10000, 100000)) {
        foreach ($bytes in @(256, 1024)) {
            foreach ($format in @('map', 'entries')) {
                & $probe $count $bytes $format 30
                if ($LASTEXITCODE -ne 0) { throw 'Cache export benchmark failed' }
            }
        }
    }
}
