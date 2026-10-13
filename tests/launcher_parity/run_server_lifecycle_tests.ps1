[CmdletBinding()]
param([string]$CsLauncher = '', [switch]$KeepFixtures)
$ErrorActionPreference = 'Stop'
[Console]::OutputEncoding = New-Object Text.UTF8Encoding $false
$env:PYTHONIOENCODING = 'utf-8'
$repo = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
if (Get-NetTCPConnection -LocalPort 19999 -State Listen -ErrorAction SilentlyContinue) {
    Write-Host '[skip] Launcher lifecycle guards: port 19999 is occupied; existing service left running.'
    exit 0
}
$fixture = Join-Path $env:TEMP ('dst_server_lifecycle_' + [guid]::NewGuid().ToString('N'))
[void][IO.Directory]::CreateDirectory($fixture)
try {
    $gcc = Join-Path $repo 'native\toolchain\w64devkit\bin\gcc.exe'
    if (!(Test-Path $gcc)) { $gcc = (Get-Command gcc -ErrorAction Stop).Source }
    $env:PATH = (Split-Path -Parent $gcc) + ';' + $env:PATH
    $launcher = Join-Path $repo 'native\src\launcher'
    $cProbe = Join-Path $fixture 'lifecycle_c.exe'
    & $gcc -std=c17 -O2 -Wall -Wextra -Werror -municode -D_CRT_SECURE_NO_WARNINGS "-I$launcher" `
        "$PSScriptRoot\server_lifecycle_probe.c" "$launcher\server_proc.c" `
        "$launcher\fsutil.c" "$launcher\globals.c" -lwinhttp -lgdi32 -o $cProbe
    if ($LASTEXITCODE -ne 0) { throw 'C lifecycle probe build failed' }
    if (!$CsLauncher) { $CsLauncher = Join-Path $repo 'build\launcher_cs\dst_launcher_cs.exe' }
    Copy-Item -LiteralPath $CsLauncher -Destination (Join-Path $fixture 'dst_launcher_cs.exe')
    $csProbe = Join-Path $fixture 'lifecycle_cs.exe'
    $csc = Join-Path $env:WINDIR 'Microsoft.NET\Framework64\v4.0.30319\csc.exe'
    & $csc /nologo /target:exe "/out:$csProbe" "/reference:$fixture\dst_launcher_cs.exe" "$PSScriptRoot\ServerLifecycleProbe.cs"
    if ($LASTEXITCODE -ne 0) { throw 'C# lifecycle probe build failed' }
    & python "$PSScriptRoot\test_server_lifecycle.py" --c $cProbe --cs $csProbe --root (Join-Path $fixture 'data')
    if ($LASTEXITCODE -ne 0) { throw 'Launcher lifecycle regression failed' }
} finally {
    if ($KeepFixtures) { Write-Host "Lifecycle fixtures: $fixture" }
    elseif (Test-Path -LiteralPath $fixture) {
        $resolved = (Resolve-Path -LiteralPath $fixture).Path
        $tempRoot = [IO.Path]::GetFullPath($env:TEMP).TrimEnd('\') + '\'
        if (!$resolved.StartsWith($tempRoot, [StringComparison]::OrdinalIgnoreCase) -or
            !(Split-Path -Leaf $resolved).StartsWith('dst_server_lifecycle_')) { throw "Unsafe cleanup path: $resolved" }
        $entries = @((Get-Item -LiteralPath $resolved)) + @(Get-ChildItem -LiteralPath $resolved -Force -Recurse)
        if ($entries | Where-Object { $_.Attributes -band [IO.FileAttributes]::ReparsePoint }) { throw "Fixture contains a reparse point: $resolved" }
        Remove-Item -LiteralPath $resolved -Recurse
    }
}
