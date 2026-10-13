$ErrorActionPreference = 'Stop'
$repo = (Resolve-Path (Join-Path $PSScriptRoot '../..')).Path
$output = Join-Path $repo 'build/model_defaults'
[void][IO.Directory]::CreateDirectory($output)
$gcc = Join-Path $repo 'native/toolchain/w64devkit/bin/gcc.exe'
if (!(Test-Path $gcc)) { $gcc = (Get-Command gcc -ErrorAction Stop).Source }
$savedPath = $env:PATH
try {
    $env:PATH = (Split-Path -Parent $gcc) + ';' + $savedPath
    $server = Join-Path $repo 'native/src/server'
    & $gcc -std=c17 -O2 -Wall -Wextra -Werror -D_CRT_SECURE_NO_WARNINGS "-I$server" `
        "$PSScriptRoot/model_defaults_probe.c" "$server/buf.c" "$server/util.c" "$server/json.c" `
        -lwinhttp -o "$output/models_c.exe"
    if ($LASTEXITCODE -ne 0) { throw 'Native model request probe build failed' }
} finally { $env:PATH = $savedPath }
Copy-Item -LiteralPath "$repo/native/dst_server_cs.exe" -Destination "$output/dst_server_cs.exe"
$csc = Join-Path $env:WINDIR 'Microsoft.NET/Framework64/v4.0.30319/csc.exe'
& $csc /nologo /target:exe "/out:$output\models_cs.exe" "/reference:$output\dst_server_cs.exe" (Join-Path $PSScriptRoot 'ModelDefaultsProbe.cs')
if ($LASTEXITCODE -ne 0) { throw 'Managed model request probe build failed' }
& python "$PSScriptRoot/test_model_defaults.py" --c "$output/models_c.exe" --cs "$output/models_cs.exe" --root $output
if ($LASTEXITCODE -ne 0) { throw 'Model defaults regression failed' }
& python "$PSScriptRoot/test_model_encoding.py"
if ($LASTEXITCODE -ne 0) { throw 'Model encoding regression failed' }
