# check_payload_scripts.ps1 -- static checks for the engine runtime scripts that
# build_native.bat embeds into the launcher (resource.h 301-304) and that
# deploy.c / godot_patch.c write verbatim into game directories.
#
# Guards:
#   * byte hygiene: UTF-8 without BOM, LF only, no NUL (a NUL would truncate the
#     C string write path), trailing newline
#   * syntax: Ren'Py hook body parses as Python (Python 3 AST) and avoids
#     Python-3-only syntax (Ren'Py 7 still ships Python 2.7); RPG Maker hook
#     passes `node --check`
#   * Godot: the text anchors godot_patch.c rewrites are present exactly once
#   * server contract: every endpoint a script calls is routed by BOTH the C
#     server (http.c) and the C# server (Program.cs)
#
# Usage: powershell -NoProfile -ExecutionPolicy Bypass -File tests\payload_scripts\check_payload_scripts.ps1
$ErrorActionPreference = 'Stop'
$repo = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$failures = New-Object System.Collections.Generic.List[string]
$passed = 0

function Check([string]$name, [bool]$ok, [string]$detail = '') {
    if ($ok) { $script:passed++; Write-Host ("  ok   " + $name) }
    else { $script:failures.Add($name + ($(if ($detail) { " -- " + $detail } else { '' }))); Write-Host ("  FAIL " + $name + " " + $detail) -ForegroundColor Red }
}

$scripts = @(
    @{ Id = 301; Path = 'payloads\RenPy\iron_deepseek.rpy';        Kind = 'renpy' },
    @{ Id = 302; Path = 'payloads\RPGMaker\hook_rpgm_mv.js';       Kind = 'js' },
    @{ Id = 303; Path = 'payloads\Godot\dst_godot_runtime_g3.gd';  Kind = 'gd3' },
    @{ Id = 304; Path = 'payloads\Godot\dst_godot_runtime_g4.gd';  Kind = 'gd4' }
)

# --- resource table consistency: resource.h, build_native.bat, verify_build_artifacts.ps1 ---
$resourceH = Get-Content (Join-Path $repo 'native\src\launcher\resource.h') -Raw -Encoding UTF8
$buildBat = Get-Content (Join-Path $repo 'build_native.bat') -Raw -Encoding UTF8
$verifyPs = Get-Content (Join-Path $repo 'scripts\verify_build_artifacts.ps1') -Raw -Encoding UTF8
foreach ($s in $scripts) {
    $rcPath = $s.Path.Replace('\', '/')
    Check "resource $($s.Id) declared in resource.h" ($resourceH -match ("\b" + $s.Id + "\b"))
    Check "resource $($s.Id) embedded by build_native.bat" ($buildBat.Contains("$($s.Id) RCDATA `"%ROOT_RC%$rcPath`""))
    Check "resource $($s.Id) verified by verify_build_artifacts.ps1" ($verifyPs.Contains("$($s.Id) (Join-Path `$repo `"$($s.Path)`")"))
}

# --- byte hygiene ---
$bytesByPath = @{}
foreach ($s in $scripts) {
    $full = Join-Path $repo $s.Path
    $bytes = [IO.File]::ReadAllBytes($full)
    $bytesByPath[$s.Path] = $bytes
    Check "$($s.Path): non-empty" ($bytes.Length -gt 0)
    Check "$($s.Path): no UTF-8 BOM" (-not ($bytes.Length -ge 3 -and $bytes[0] -eq 0xEF -and $bytes[1] -eq 0xBB -and $bytes[2] -eq 0xBF))
    Check "$($s.Path): no NUL byte" (-not ($bytes -contains 0))
    Check "$($s.Path): LF line endings only" (-not ($bytes -contains 13))
    Check "$($s.Path): ends with newline" ($bytes[$bytes.Length - 1] -eq 10)
    try { [void][Text.UTF8Encoding]::new($false, $true).GetString($bytes); $utf8 = $true } catch { $utf8 = $false }
    Check "$($s.Path): valid UTF-8" $utf8
}

function Get-Utf8Text([string]$rel) { return [Text.Encoding]::UTF8.GetString($bytesByPath[$rel]) }

# --- Ren'Py hook: Python syntax of the init block body ---
$renpyRel = 'payloads\RenPy\iron_deepseek.rpy'
$renpy = Get-Utf8Text $renpyRel
$renpyLines = $renpy.Split("`n")
Check "renpy: first line is 'init 999 python:'" ($renpyLines[0] -eq 'init 999 python:')
$bodyOk = $true
$body = New-Object System.Text.StringBuilder
for ($i = 1; $i -lt $renpyLines.Length; $i++) {
    $line = $renpyLines[$i]
    if ($line.Length -eq 0) { [void]$body.Append("`n"); continue }
    if (-not $line.StartsWith('    ')) { $bodyOk = $false; break }
    [void]$body.Append($line.Substring(4)).Append("`n")
}
Check "renpy: every body line is indented inside the init block" $bodyOk
$python = Get-Command python -ErrorAction SilentlyContinue
if ($python) {
    $tmp = Join-Path $env:TEMP ("dst_renpy_body_" + [Guid]::NewGuid().ToString('N') + ".py")
    [IO.File]::WriteAllText($tmp, $body.ToString(), [Text.UTF8Encoding]::new($false))
    try {
        $out = & python -c "import ast,sys; ast.parse(open(sys.argv[1],encoding='utf-8').read(), sys.argv[1]); print('parsed')" $tmp 2>&1
        Check "renpy: hook body parses as Python" ($LASTEXITCODE -eq 0 -and "$out" -match 'parsed') "$out"
    } finally { Remove-Item $tmp -ErrorAction SilentlyContinue }
} else {
    Check "renpy: hook body parses as Python" $false "python not found on PATH"
}
# Ren'Py 7.x runs Python 2.7; reject syntax that only Python 3 accepts.
$py3Only = @(
    @{ Name = 'f-string';        Regex = '(?<![A-Za-z0-9_])f["'']' },
    @{ Name = 'walrus :=';       Regex = ':=' },
    @{ Name = 'nonlocal';        Regex = '(?m)^\s*nonlocal\b' },
    @{ Name = 'async/await';     Regex = '(?m)^\s*(async\s+def|await)\b' },
    @{ Name = 'yield from';      Regex = '\byield\s+from\b' },
    @{ Name = 'print with kwargs'; Regex = 'print\([^)\n]*\b(end|sep|file)=' }
)
foreach ($rule in $py3Only) {
    Check "renpy: no Python-3-only syntax ($($rule.Name))" (-not ($renpy -match $rule.Regex))
}

# --- RPG Maker hook: JavaScript syntax ---
$node = Get-Command node -ErrorAction SilentlyContinue
if ($node) {
    $out = & node --check (Join-Path $repo 'payloads\RPGMaker\hook_rpgm_mv.js') 2>&1
    Check "rpgm: hook passes node --check" ($LASTEXITCODE -eq 0) "$out"
} else {
    Check "rpgm: hook passes node --check" $false "node not found on PATH"
}
$rpgm = Get-Utf8Text 'payloads\RPGMaker\hook_rpgm_mv.js'
Check "rpgm: hook is an IIFE in strict mode" ($rpgm.StartsWith("(function(){`n  'use strict';"))

# --- Godot sidecars: anchors rewritten by godot_patch.c (build_godot_runtime_autoload, godot_runtime_script_with_font) ---
function CountOf([string]$hay, [string]$needle) {
    $n = 0; $i = 0
    while (($i = $hay.IndexOf($needle, $i, [StringComparison]::Ordinal)) -ge 0) { $n++; $i += $needle.Length }
    return $n
}
$gdAnchors = @(
    @{ Text = "extends SceneTree`n";        Count = 1 },
    @{ Text = "func _initialize():`n";      Count = 1 },
    @{ Text = "`t`tquit(0)`n";              Count = 1 },
    @{ Text = "C:/Windows/Fonts/simhei.ttf"; Count = 1 }
)
$sceneBlocks = @{
    'gd3' = "`tvar scene = str(ProjectSettings.get_setting(`"application/run/main_scene`"))`n`tif scene != `"`":`n`t`tchange_scene(scene)`n"
    'gd4' = "`tvar scene = str(ProjectSettings.get_setting(`"application/run/main_scene`"))`n`tif scene != `"`":`n`t`tchange_scene_to_file(scene)`n"
}
foreach ($s in $scripts | Where-Object { $_.Kind -like 'gd*' }) {
    $gd = Get-Utf8Text $s.Path
    Check "$($s.Path): starts with 'extends SceneTree'" ($gd.StartsWith("extends SceneTree`n"))
    foreach ($a in $gdAnchors) {
        $c = CountOf $gd $a.Text
        Check "$($s.Path): anchor $([Regex]::Escape($a.Text).Replace('\n','\n')) appears $($a.Count)x" ($c -eq $a.Count) "found $c"
    }
    Check "$($s.Path): anchor get_root() present" ((CountOf $gd 'get_root()') -ge 1)
    Check "$($s.Path): main_scene switch block present once" ((CountOf $gd $sceneBlocks[$s.Kind]) -eq 1)
    $other = if ($s.Kind -eq 'gd3') { 'change_scene_to_file(' } else { "`t`tchange_scene(scene)" }
    Check "$($s.Path): no other-major scene API ($other)" ((CountOf $gd $other) -eq 0)
    $badIndent = @($gd.Split("`n") | Where-Object { $_ -match '^ +' })
    Check "$($s.Path): tab indentation only" ($badIndent.Count -eq 0) "$($badIndent.Count) space-indented line(s)"
}

# --- server contract: endpoints used by scripts must be routed by both servers ---
$httpC = Get-Content (Join-Path $repo 'native\src\server\http.c') -Raw -Encoding UTF8
$programCs = Get-Content (Join-Path $repo 'native\src\server_cs\Program.cs') -Raw -Encoding UTF8
foreach ($s in $scripts) {
    $text = Get-Utf8Text $s.Path
    $paths = New-Object System.Collections.Generic.HashSet[string]
    foreach ($m in [Regex]::Matches($text, '127\.0\.0\.1:19999(/[A-Za-z_/]+)')) { [void]$paths.Add($m.Groups[1].Value) }
    if ($s.Kind -eq 'renpy') {
        foreach ($m in [Regex]::Matches($text, "_ds_http\('(/[A-Za-z_/]+)'")) { [void]$paths.Add($m.Groups[1].Value) }
    }
    Check "$($s.Path): calls at least one server endpoint" ($paths.Count -gt 0)
    foreach ($p in $paths) {
        Check "$($s.Path): endpoint $p routed by C server" ($httpC.Contains("`"$p`""))
        Check "$($s.Path): endpoint $p routed by C# server" ($programCs.Contains("`"$p`""))
    }
}

Write-Host ""
if ($failures.Count -gt 0) {
    Write-Host ("payload script checks FAILED: {0} failure(s), {1} passed" -f $failures.Count, $passed) -ForegroundColor Red
    foreach ($f in $failures) { Write-Host ("  - " + $f) -ForegroundColor Red }
    exit 1
}
Write-Host ("payload script checks PASS: {0} checks" -f $passed) -ForegroundColor Green
exit 0
