$ErrorActionPreference = 'Stop'
$repo = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$font = Join-Path $repo 'assets\fonts\NotoSansCJKsc-Regular.otf'
$license = Join-Path $repo 'assets\fonts\OFL.txt'
$expectedFontHash = '2c76254f6fc379fddfce0a7e84fb5385bb135d3e399294f6eeb6680d0365b74b'
$expectedLicenseHash = '6a73f9541c2de74158c0e7cf6b0a58ef774f5a780bf191f2d7ec9cc53efe2bf2'
function Get-Sha256Hex([string]$path) {
    $sha = [Security.Cryptography.SHA256]::Create()
    $stream = [IO.File]::OpenRead($path)
    try { return [BitConverter]::ToString($sha.ComputeHash($stream)).Replace('-', '') }
    finally { $stream.Dispose(); $sha.Dispose() }
}
if ((Get-Sha256Hex $font) -ine $expectedFontHash -or
    (Get-Sha256Hex $license) -ine $expectedLicenseHash) {
    throw 'Bundled font or OFL differs from the reviewed upstream bytes.'
}
$temp = Join-Path $repo ('build\font_bundle_tests_' + [guid]::NewGuid().ToString('N'))
[void][IO.Directory]::CreateDirectory($temp)
try {
    $gcc = Join-Path $repo 'native\toolchain\w64devkit\bin\gcc.exe'
    if (!(Test-Path -LiteralPath $gcc)) { $gcc = (Get-Command gcc -ErrorAction Stop).Source }
    $env:PATH = (Split-Path -Parent $gcc) + ';' + $env:PATH
    $rootRc = $repo.Replace('\', '/')
    $rc = Join-Path $temp 'fonts.rc'
    @("107 RCDATA `"$rootRc/assets/fonts/NotoSansCJKsc-Regular.otf`"",
      "108 RCDATA `"$rootRc/assets/fonts/OFL.txt`"") |
        Set-Content -LiteralPath $rc -Encoding ascii
    $res = Join-Path $temp 'fonts.o'
    & windres $rc -O coff -o $res
    if ($LASTEXITCODE -ne 0) { throw 'Font probe resource build failed' }
    $launcher = Join-Path $repo 'native\src\launcher'
    $cProbe = Join-Path $temp 'font_c.exe'
    & $gcc -std=c17 -O2 -Wall -Wextra -Werror -ffunction-sections -fdata-sections -municode `
        -D_CRT_SECURE_NO_WARNINGS "-I$launcher" `
        (Join-Path $PSScriptRoot 'font_bundle_probe.c') `
        "$launcher\font_bundle.c" "$launcher\fsutil.c" "$launcher\globals.c" "$launcher\embedded.c" `
        $res '-Wl,--gc-sections' -lgdi32 -o $cProbe
    if ($LASTEXITCODE -ne 0) { throw 'Native font bundle probe build failed' }
    $nativeRoot = Join-Path $temp 'native-root'
    $nativeGames = Join-Path $temp 'native-games'
    [void][IO.Directory]::CreateDirectory($nativeRoot)
    [void][IO.Directory]::CreateDirectory($nativeGames)
    & $cProbe $nativeRoot $nativeGames
    if ($LASTEXITCODE -ne 0) { throw "Native no-system-font probe failed: $LASTEXITCODE" }

    $csDir = Join-Path $temp 'managed'
    [void][IO.Directory]::CreateDirectory($csDir)
    $csLauncher = Join-Path $repo 'build\launcher_cs\dst_launcher_cs.exe'
    Copy-Item -LiteralPath $csLauncher -Destination (Join-Path $csDir 'dst_launcher_cs.exe')
    $csc = Join-Path $env:WINDIR 'Microsoft.NET\Framework64\v4.0.30319\csc.exe'
    $csProbe = Join-Path $csDir 'font_cs.exe'
    & $csc /nologo /target:exe "/out:$csProbe" "/reference:$csDir\dst_launcher_cs.exe" `
        (Join-Path $PSScriptRoot 'FontBundleProbe.cs')
    if ($LASTEXITCODE -ne 0) { throw 'C# font bundle probe build failed' }
    $managedRoot = Join-Path $temp 'managed-root'
    $managedGame = Join-Path $temp 'managed-game'
    [void][IO.Directory]::CreateDirectory($managedRoot)
    & $csProbe $managedRoot $managedGame
    if ($LASTEXITCODE -ne 0) { throw "C# font bundle probe failed: $LASTEXITCODE" }
    foreach ($root in @($nativeRoot, $managedRoot)) {
        if ((Get-Sha256Hex (Join-Path $root 'assets\fonts\NotoSansCJKsc-Regular.otf')) -ine $expectedFontHash -or
            (Get-Sha256Hex (Join-Path $root 'assets\fonts\OFL.txt')) -ine $expectedLicenseHash) {
            throw "Font extracted with changed bytes: $root"
        }
    }
    Write-Host 'Font bundle tests PASS: reviewed hashes and C/C# no-system-font fallback.'
} finally {
    $buildRoot = [IO.Path]::GetFullPath((Join-Path $repo 'build')).TrimEnd('\') + '\'
    $resolved = [IO.Path]::GetFullPath($temp)
    if (!$resolved.StartsWith($buildRoot, [StringComparison]::OrdinalIgnoreCase) -or
        !(Split-Path -Leaf $resolved).StartsWith('font_bundle_tests_')) {
        throw "Unsafe font fixture cleanup path: $resolved"
    }
    if (Test-Path -LiteralPath $resolved) {
        if (Get-ChildItem -LiteralPath $resolved -Force -Recurse | Where-Object {
            $_.Attributes -band [IO.FileAttributes]::ReparsePoint
        }) { throw "Font fixtures contain a reparse point; retained: $resolved" }
        Remove-Item -LiteralPath $resolved -Recurse -Force
    }
}
