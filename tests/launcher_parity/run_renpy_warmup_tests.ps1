[CmdletBinding()]
param([string]$CsLauncher = '')
$ErrorActionPreference = 'Stop'
$repo = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$fixture = Join-Path $repo 'build\renpy_warmup'
[void][IO.Directory]::CreateDirectory($fixture)
$gcc = Join-Path $repo 'native\toolchain\w64devkit\bin\gcc.exe'
if (!(Test-Path -LiteralPath $gcc)) { $gcc = (Get-Command gcc -ErrorAction Stop).Source }
$launcher = Join-Path $repo 'native\src\launcher'
$env:PATH = (Split-Path -Parent $gcc) + ';' + $env:PATH
$cProbe = Join-Path $fixture 'warmup_c.exe'
& $gcc -std=c17 -O2 -Wall -Wextra -Werror -municode -D_CRT_SECURE_NO_WARNINGS "-I$launcher" `
    "$PSScriptRoot\warmup_http_probe.c" "$launcher\warmup.c" "$launcher\godot_warmup.c" `
    "$launcher\fsutil.c" "$launcher\globals.c" "$launcher\engine.c" -lwinhttp -lgdi32 -o $cProbe
if ($LASTEXITCODE -ne 0) { throw 'RenPy warmup C probe build failed' }
if (!$CsLauncher) {
    $managed = Join-Path $fixture 'managed'
    & dotnet build "$repo\native\src\launcher_cs\launcher_cs.csproj" -c Release -nologo -v quiet "-p:OutputPath=$managed\"
    if ($LASTEXITCODE -ne 0) { throw 'RenPy warmup C# launcher build failed' }
    $CsLauncher = Join-Path $managed 'dst_launcher_cs.exe'
}
Copy-Item -LiteralPath $CsLauncher -Destination (Join-Path $fixture 'dst_launcher_cs.exe')
$csProbe = Join-Path $fixture 'warmup_cs.exe'
$csc = Join-Path $env:WINDIR 'Microsoft.NET\Framework64\v4.0.30319\csc.exe'
& $csc /nologo /target:exe "/out:$csProbe" "/reference:$fixture\dst_launcher_cs.exe" "$PSScriptRoot\WarmupHttpProbe.cs"
if ($LASTEXITCODE -ne 0) { throw 'RenPy warmup C# probe build failed' }
& python "$PSScriptRoot\test_renpy_warmup.py" --c $cProbe --cs $csProbe --root (Join-Path $fixture 'games')
if ($LASTEXITCODE -ne 0) { throw 'RenPy warmup semantic regression failed' }
