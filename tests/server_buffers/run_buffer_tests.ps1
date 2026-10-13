param([string]$ServerCs = '')
$ErrorActionPreference = 'Stop'
$repo = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '../..')).Path
if (-not $ServerCs) { $ServerCs = Join-Path $repo 'native/dst_server_cs.exe' }
$output = Join-Path $repo 'build/server_buffers'
New-Item -ItemType Directory -Path $output -Force | Out-Null
$gcc = Join-Path $repo 'native/toolchain/w64devkit/bin/gcc.exe'
if (-not (Test-Path -LiteralPath $gcc)) { $gcc = (Get-Command gcc -ErrorAction Stop).Source }
$nativeTest = Join-Path $output 'dst_json_buffer_tests.exe'
$savedPath = $env:PATH
try {
    $env:PATH = (Split-Path -Parent $gcc) + ';' + $savedPath
    & $gcc -std=c17 -O2 -Wall -Wextra -Werror -DDST_TEST_ALLOC_COUNTERS `
        ('-I' + (Join-Path $repo 'native/src/server')) `
        (Join-Path $PSScriptRoot 'json_buffer_tests.c') `
        (Join-Path $repo 'native/src/server/buf.c') `
        (Join-Path $repo 'native/src/server/util.c') -o $nativeTest
    if ($LASTEXITCODE -ne 0) { throw 'Native JSON buffer test build failed' }
} finally { $env:PATH = $savedPath }
& $nativeTest
if ($LASTEXITCODE -ne 0) { throw 'Native JSON buffer checks failed' }
& dotnet build (Join-Path $PSScriptRoot 'server_buffers.csproj') -c Release --nologo -v quiet ('-p:OutputPath=' + $output + '\')
if ($LASTEXITCODE -ne 0) { throw 'C# response buffer test build failed' }
& (Join-Path $output 'dst_response_buffer_tests.exe') $ServerCs
if ($LASTEXITCODE -ne 0) { throw 'C# response buffer checks failed' }
