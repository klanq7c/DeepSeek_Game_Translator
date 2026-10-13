[CmdletBinding()]
param([string]$CsLauncher = '', [switch]$KeepFixtures)
$ErrorActionPreference = 'Stop'
$repo = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$busy = Get-NetTCPConnection -LocalPort 19999 -State Listen -ErrorAction SilentlyContinue
if ($busy) {
    Write-Host '[skip] Unity warmup HTTP guards: port 19999 is occupied; existing service left running.'
    exit 0
}
$fixture = Join-Path $env:TEMP ('dst_warmup_http_' + [guid]::NewGuid().ToString('N'))
[void][IO.Directory]::CreateDirectory($fixture)
try {
    $gcc = Join-Path $repo 'native\toolchain\w64devkit\bin\gcc.exe'
    if (!(Test-Path $gcc)) { $gcc = (Get-Command gcc -ErrorAction Stop).Source }
    $env:PATH = (Split-Path -Parent $gcc) + ';' + $env:PATH
    $launcher = Join-Path $repo 'native\src\launcher'
    $cProbe = Join-Path $fixture 'warmup_c.exe'
    $fsObject = Join-Path $fixture 'fsutil_fault.o'
    & $gcc -std=c17 -O2 -Wall -Wextra -Werror -D_CRT_SECURE_NO_WARNINGS -Drealloc=dst_test_realloc "-I$launcher" `
        -c "$launcher\fsutil.c" -o $fsObject
    if ($LASTEXITCODE -ne 0) { throw 'C allocation boundary probe build failed' }
    & $gcc -std=c17 -O2 -Wall -Wextra -Werror -municode -D_CRT_SECURE_NO_WARNINGS "-I$launcher" `
        "$PSScriptRoot\warmup_http_probe.c" "$launcher\warmup.c" "$launcher\godot_warmup.c" `
        $fsObject "$launcher\globals.c" "$launcher\engine.c" -lwinhttp -lgdi32 -o $cProbe
    if ($LASTEXITCODE -ne 0) { throw 'C warmup HTTP probe build failed' }
    if (!$CsLauncher) {
        $csOutput = Join-Path $fixture 'managed'
        & dotnet build "$repo\native\src\launcher_cs\launcher_cs.csproj" -c Release -nologo -v quiet `
            "-p:OutputPath=$csOutput\" "-p:IntermediateOutputPath=$fixture\obj\"
        if ($LASTEXITCODE -ne 0) { throw 'C# launcher build failed' }
        $CsLauncher = Join-Path $csOutput 'dst_launcher_cs.exe'
    }
    Copy-Item -LiteralPath $CsLauncher -Destination (Join-Path $fixture 'dst_launcher_cs.exe')
    $csProbe = Join-Path $fixture 'warmup_cs.exe'
    $csc = Join-Path $env:WINDIR 'Microsoft.NET\Framework64\v4.0.30319\csc.exe'
    & $csc /nologo /target:exe "/out:$csProbe" "/reference:$fixture\dst_launcher_cs.exe" "$PSScriptRoot\WarmupHttpProbe.cs"
    if ($LASTEXITCODE -ne 0) { throw 'C# warmup HTTP probe build failed' }
    & python "$PSScriptRoot\test_warmup_http.py" --c $cProbe --cs $csProbe --root (Join-Path $fixture 'games')
    if ($LASTEXITCODE -ne 0) { throw 'Warmup HTTP regression failed' }
} finally {
    if ($KeepFixtures) { Write-Host "Warmup HTTP fixtures: $fixture" }
    elseif (Test-Path -LiteralPath $fixture) {
        $resolved = (Resolve-Path -LiteralPath $fixture).Path
        $tempRoot = [IO.Path]::GetFullPath($env:TEMP).TrimEnd('\') + '\'
        if (!$resolved.StartsWith($tempRoot, [StringComparison]::OrdinalIgnoreCase) -or
            !(Split-Path -Leaf $resolved).StartsWith('dst_warmup_http_')) { throw "Unsafe cleanup path: $resolved" }
        $entries = @((Get-Item -LiteralPath $resolved -Force)) + @(Get-ChildItem -LiteralPath $resolved -Force -Recurse)
        $links = @($entries | Where-Object { $_.Attributes -band [IO.FileAttributes]::ReparsePoint })
        if ($links.Count) { throw "Fixture contains a reparse point; retained: $resolved" }
        Remove-Item -LiteralPath $resolved -Recurse -Force
    }
}
