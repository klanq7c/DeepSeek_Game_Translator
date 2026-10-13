[CmdletBinding()]
param([string]$CsLauncher = '')
$ErrorActionPreference = 'Stop'
$repo = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$fixture = Join-Path $env:TEMP ('dst_api_models_' + [guid]::NewGuid().ToString('N'))
[void][IO.Directory]::CreateDirectory($fixture)
try {
    $gcc = Join-Path $repo 'native\toolchain\w64devkit\bin\gcc.exe'
    if (!(Test-Path $gcc)) { $gcc = (Get-Command gcc -ErrorAction Stop).Source }
    $env:PATH = (Split-Path -Parent $gcc) + ';' + $env:PATH
    $launcher = Join-Path $repo 'native\src\launcher'
    $cProbe = Join-Path $fixture 'api_models_c.exe'
    & $gcc -std=c17 -O2 -Wall -Wextra -Werror -municode -D_CRT_SECURE_NO_WARNINGS "-I$launcher" `
        "$PSScriptRoot\api_models_probe.c" -lwinhttp -o $cProbe
    if ($LASTEXITCODE -ne 0) { throw 'C model discovery probe build failed' }
    if (!$CsLauncher) { $CsLauncher = Join-Path $repo 'build\launcher_cs\dst_launcher_cs.exe' }
    Copy-Item -LiteralPath $CsLauncher -Destination (Join-Path $fixture 'dst_launcher_cs.exe')
    $csProbe = Join-Path $fixture 'api_models_cs.exe'
    $csc = Join-Path $env:WINDIR 'Microsoft.NET\Framework64\v4.0.30319\csc.exe'
    & $csc /nologo /target:exe "/out:$csProbe" "/reference:$fixture\dst_launcher_cs.exe" "$PSScriptRoot\ApiModelsProbe.cs"
    if ($LASTEXITCODE -ne 0) { throw 'C# model discovery probe build failed' }
    & python "$PSScriptRoot\test_api_models.py" --c $cProbe --cs $csProbe
    if ($LASTEXITCODE -ne 0) { throw 'API model discovery regression failed' }
} finally {
    $resolved = (Resolve-Path -LiteralPath $fixture).Path
    $tempRoot = [IO.Path]::GetFullPath($env:TEMP).TrimEnd('\') + '\'
    if (!$resolved.StartsWith($tempRoot, [StringComparison]::OrdinalIgnoreCase) -or
        !(Split-Path -Leaf $resolved).StartsWith('dst_api_models_')) { throw "Unsafe cleanup path: $resolved" }
    $entries = @((Get-Item -LiteralPath $resolved -Force)) + @(Get-ChildItem -LiteralPath $resolved -Force -Recurse)
    if (@($entries | Where-Object { $_.Attributes -band [IO.FileAttributes]::ReparsePoint }).Count) { throw "Fixture contains a reparse point: $resolved" }
    Remove-Item -LiteralPath $resolved -Recurse -Force
}
