<#
.SYNOPSIS
  Launcher parity: C launcher (native/src/launcher) vs C# port (native/src/launcher_cs).

.DESCRIPTION
  Phase-3 migration guard. Both launchers expose hidden diagnostic modes that
  print deterministic UTF-8 reports to stdout; this script builds synthetic
  game directories under a temp root, runs both launchers on identical inputs
  and requires byte-identical results.

  1. Engine detection (--detect-and-exit vs --detect): every branch of
     engine.c - Ren'Py (rpy/rpyc/rpa), RPG Maker MV/MZ (www + flat layouts,
     owned-backup index, missing core), RPG Maker legacy (rxdata/rvdata/rvdata2),
     Unity Mono vs IL2CPP (GameAssembly.dll and *_Data\il2cpp_data), Godot (.pck,
     project.godot, .godot, godot_project.binary, embedded pck PE section + GDPC
     tail), exe scoring (stem_Data, stem.pck, directory leaf match, Game/RPG_RT,
     installer penalties, tie-break by name, ignored names), NW.js false
     positive, empty dir, nonexistent dir, trailing-separator variants.

  2. Deploy / restore (--deploy-and-exit / --restore-and-exit vs --deploy /
     --restore): each scenario is materialised twice, one copy per launcher;
     exit code, the mirrored log lines (log=...), result= and a SHA-256
     snapshot of the resulting tree must match. Covers Ren'Py, RPG Maker MV/MZ
     (index.html injection, stripping, backup, atomic write, NUL bytes,
     read-only files), RPG Maker legacy, Godot, unknown engines, and Unity:
     Mono (BepInEx 5/6 install, x86/x64 by PE machine, repair of a partial
     user BepInEx, Unity 6 upgrade, unsupported machine, stripped mscorlib ->
     corlib payload + doorstop/BepInEx.cfg INI edits + font patcher, ownership
     conflicts, plugin/Newtonsoft backups) and IL2CPP (be.755 runtime + XUnity
     tree, disabling bundled Mono plugin / Il2Cppmscorlib, config write, owned
     migration, user-modified preservation, marker-gated tree removal).
     Deploy/restore runs both launchers from a synthetic launcher root with tiny
     stand-in payloads (see "synthetic launcher root" below), so the real
     ~220 MB Unity payloads are never copied.

  3. Warmup scan (--warmup-and-exit vs --warmup): the scanners run in dump
     mode and every /prefetch or /cache/import body they would send is
     compared byte for byte (see "warmup scan parity" below). Covers Ren'Py
     (.rpy literals, escapes, prevs, 16-literal cap, recursion quirk, size/NUL),
     RPG Maker MV/MZ (events + database JSON, prefix codes, external TXT/CSV,
     flat layout, limits), Unity (XUnity translation files, *_Data assets and
     bundles, IL2CPP second _Data) and Godot (project text resources and quote
     classification, PO/CSV/Markdown, .translation locale rules, PCK formats
     1/2/3 with two-pass order, broken-pack raw fallback, embedded pck EXE,
     skip dirs/generated files/depth, 12000-item cap).

  4. Embedded payload sync (--sync-payloads-and-exit vs --sync-payloads): the
     released trees, stdout and exit codes must match, a second run must be a
     no-op, tampered targets must be restored and a blocked target must fail
     with exit 5 on both sides.

  5. Godot patch pack (--godot-patch-and-exit / --godot-promote-and-exit /
     --godot-launcher-and-exit vs --godot-patch / --godot-promote /
     --godot-launcher): loose project sidecar, format 1 sidecar pack (JSON,
     scene, Markdown, GDScript bytecode, OptimizedTranslation incl. Smaz,
     font entry), format 3 with and without autoload_prepend support, pack
     embedded in the EXE, the 128-per-batch / 1024-live-texts / 1800-strings-
     per-resource quotas, packaged game with nothing to translate, a busy
     active pack staged and later promoted, and the patch-launcher copy with
     its ownership marker. The built .pck must be byte-identical and both
     launchers must send the same /batch bodies to the fake local server this
     script hosts on 127.0.0.1:19999 (section skipped when the port is taken).

  6. Config and Godot preflight (--api-config* / --godot-probe* /
     --godot-preflight* vs the same flags without -and-exit): the provider
     preset table, the values loaded from api.ini and the preset index, a
     byte-identical api.ini after a write, the --main-pack rejection
     classifier over accepted and rejected outputs, and the headless preflight
     conclusion cache (miss/put/hit, kind isolation, file-change invalidation,
     byte-identical godot_preflight.ini).

  7. One-click launch flow (--launch-flow-and-exit vs --launch-flow): the part
     of ui.c that runs once the server is ready. Warmup runs in dump mode, the
     Godot headless preflight probes really start a stub console app (its
     stdout/exit code come from dst_fake_godot.ini beside it, and it records
     its own command line next to the game directory), and only the three
     points that really start something print
     spawn=<kind>|<exe>|<cmd>|<cwd> instead. Covers Ren'Py (launch then warm),
     RPG Maker (warm then launch), unknown engines, and Godot: loose project
     accepted and rejected, export with and without a patch pack, and a
     template that explicitly rejects --main-pack. Also --clear-cache*: the
     cache card text plus deleting the shared cache file.

  8. Window rendering (--ui-probe-and-exit vs --ui-probe): ui.c's window and
     drawing layer. Both launchers pin the DPI to 96, freeze the animation
     clock, neutralise the two runtime-identity strings and take the server
     state from an argument, then render an invisible main window's whole
     client area - background plus every child, owner-draw buttons included -
     into one 32bpp bitmap. The layout report and the bitmap must both match
     byte for byte, at four sizes and both server states. --ui-identity*
     asserts the runtime tag/subtitle that each binary really paints, the one
     difference the two windows are meant to have.

  9. Optional (-ServerSmoke): the C# ServerProcess module starts the real
     server binary, waits for /health and stops it. Needs port 19999 free;
     off by default because it would stop a server the user is running.

.PARAMETER CLauncher
  Path to the C launcher exe. Default: <repo>\ds游戏翻译器.exe (build_native.bat output)
.PARAMETER CsLauncher
  Path to the C# launcher exe. Default: built from native\src\launcher_cs.
.PARAMETER KeepFixtures
  Leave the temp fixture tree on disk for inspection.
.PARAMETER ServerSmoke
  Also run the server lifecycle smoke test against <repo>\native\dst_server.exe.
.PARAMETER ShowLogs
  Print the mirrored log= lines of every deploy/restore step (they are identical
  for both launchers once the step passed) to see which branches a scenario hit.
#>
[CmdletBinding()]
param(
    [string]$CLauncher = "",
    [string]$CsLauncher = "",
    [switch]$KeepFixtures,
    [switch]$ServerSmoke,
    # Print the (identical) launcher stdout for every deploy/restore step, to eyeball which branches a scenario hit.
    [switch]$ShowLogs
)

$ErrorActionPreference = 'Stop'
$repo = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path

# Godot 补丁场景用的假 /batch 服务器（见下方）。AcceptTcpClient 是不可中断的阻塞调用，
# Runspace.Close 叫不醒它，必须先 Stop() 监听套接字让它抛错退出。任何终止性错误也要
# 经过这里，否则监听线程会把整个脚本挂死，把一次失败变成一次超时。
$fakeState = $null
$fakeRunspace = $null
$fakePs = $null
$fakeHandle = $null
function Stop-FakeServer {
    if (-not $script:fakePs) { return }
    if ($script:fakeState['Listener']) { try { $script:fakeState['Listener'].Stop() } catch { } }
    $waited = 0
    while (-not $script:fakeHandle.IsCompleted -and $waited -lt 5000) { Start-Sleep -Milliseconds 25; $waited += 25 }
    if (-not $script:fakeHandle.IsCompleted) { $script:fakePs.Stop() }
    $script:fakePs.Dispose()
    $script:fakeRunspace.Dispose()
    $script:fakePs = $null
}
trap { Stop-FakeServer; break }

if (-not $CLauncher) {
    # 产品名含中文，按 build_native.bat 的做法用码点拼出来，避开脚本文件编码问题。
    $ds = 'ds' + [string][char]0x6e38 + [string][char]0x620f + [string][char]0x7ffb + [string][char]0x8bd1 + [string][char]0x5668
    $CLauncher = Join-Path $repo ($ds + '.exe')
}
if (-not (Test-Path -LiteralPath $CLauncher)) { throw "C launcher not found: $CLauncher (run build_native.bat first)" }

$csBuildDir = $null
if (-not $CsLauncher) {
    $proj = Join-Path $repo 'native\src\launcher_cs\launcher_cs.csproj'
    $csBuildDir = Join-Path $env:TEMP ('dst_launcher_cs_' + [guid]::NewGuid().ToString('N'))
    Write-Host "[build] dotnet build $proj -> $csBuildDir"
    & dotnet build $proj -c Release -nologo -v quiet -p:OutputPath="$csBuildDir\" | Out-Host
    if ($LASTEXITCODE -ne 0) { throw "dotnet build failed for launcher_cs" }
    $CsLauncher = Join-Path $csBuildDir 'dst_launcher_cs.exe'
}
if (-not (Test-Path -LiteralPath $CsLauncher)) { throw "C# launcher not found: $CsLauncher" }

# ---------------------------------------------------------------- fixtures

$root = Join-Path $env:TEMP ('dst_detect_parity_' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $root | Out-Null

function New-Fixture([string]$name) {
    $d = Join-Path $root $name
    New-Item -ItemType Directory -Path $d -Force | Out-Null
    return $d
}
function Touch([string]$path, [byte[]]$bytes = $null) {
    $dir = Split-Path -Parent $path
    if (-not (Test-Path -LiteralPath $dir)) { New-Item -ItemType Directory -Path $dir -Force | Out-Null }
    if ($null -eq $bytes) { $bytes = [byte[]]@(0x78) }
    [IO.File]::WriteAllBytes($path, $bytes)
}
function Mkdir([string]$path) { New-Item -ItemType Directory -Path $path -Force | Out-Null }

# Minimal PE with a section named "pck" and a trailing GDPC magic (Godot 4 embedded pack).
function New-PckPeBytes([bool]$withSection, [bool]$withTail) {
    $dos = New-Object byte[] 64
    $dos[0] = 0x4D; $dos[1] = 0x5A                       # MZ
    [BitConverter]::GetBytes([int32]64).CopyTo($dos, 60)  # e_lfanew = 64
    $sig = [byte[]](0x50, 0x45, 0x00, 0x00)              # PE\0\0
    $fh = New-Object byte[] 20
    [BitConverter]::GetBytes([uint16]1).CopyTo($fh, 2)    # NumberOfSections
    [BitConverter]::GetBytes([uint16]0).CopyTo($fh, 16)   # SizeOfOptionalHeader
    $sh = New-Object byte[] 40
    if ($withSection) { $sh[0] = 0x70; $sh[1] = 0x63; $sh[2] = 0x6B } # "pck"
    else { $sh[0] = 0x2E; $sh[1] = 0x74; $sh[2] = 0x65; $sh[3] = 0x78; $sh[4] = 0x74 } # ".text"
    $pad = New-Object byte[] 16
    $tail = if ($withTail) { [byte[]](0x47, 0x44, 0x50, 0x43) } else { [byte[]](0x00, 0x00, 0x00, 0x00) }
    return [byte[]]($dos + $sig + $fh + $sh + $pad + $tail)
}

$fixtures = New-Object System.Collections.Generic.List[string]

# Ren'Py
$d = New-Fixture 'renpy_rpy';  Touch "$d\game\script.rpy"; Touch "$d\Game.exe"; $fixtures.Add($d)
$d = New-Fixture 'renpy_rpyc'; Touch "$d\game\script.rpyc"; Touch "$d\renpy_rpyc.exe"; Touch "$d\lib\py3-windows-x86_64\python.exe"; $fixtures.Add($d)
$d = New-Fixture 'renpy_rpa';  Touch "$d\game\archive.rpa"; Touch "$d\a.exe"; Touch "$d\b.exe"; $fixtures.Add($d)
$d = New-Fixture 'renpy_game_dir_empty'; Mkdir "$d\game"; Touch "$d\x.exe"; $fixtures.Add($d)

# RPG Maker MV/MZ: www layout
$d = New-Fixture 'rpgm_mv_www'
Touch "$d\www\index.html"; Touch "$d\www\data\System.json"; Touch "$d\www\js\main.js"; Touch "$d\www\js\rpg_core.js"; Touch "$d\Game.exe"
$fixtures.Add($d)
# MZ flat layout
$d = New-Fixture 'rpgm_mz_flat'
Touch "$d\index.html"; Touch "$d\data\System.json"; Touch "$d\js\main.js"; Touch "$d\js\rmmz_core.js"; Touch "$d\Game.exe"
$fixtures.Add($d)
# Owned backup replaces index.html after deploy
$d = New-Fixture 'rpgm_mv_owned_backup'
Touch "$d\www\index.html.dst-backup"; Touch "$d\www\data\System.json"; Touch "$d\www\js\main.js"; Touch "$d\www\js\rpg_core.js"; Touch "$d\Game.exe"
$fixtures.Add($d)
# Missing core js -> not RPG Maker (NW.js false positive guard)
$d = New-Fixture 'nwjs_not_rpgm'
Touch "$d\www\index.html"; Touch "$d\www\js\main.js"; Touch "$d\www\data\System.json"; Touch "$d\nw.exe"
$fixtures.Add($d)
# System.json is a directory -> must fail the is_dir check
$d = New-Fixture 'rpgm_system_is_dir'
Touch "$d\index.html"; Mkdir "$d\data\System.json"; Touch "$d\js\main.js"; Touch "$d\js\rpg_core.js"; Touch "$d\Game.exe"
$fixtures.Add($d)
# www present but www\js missing while flat root is valid -> flat wins
$d = New-Fixture 'rpgm_www_without_js_flat_valid'
Touch "$d\www\index.html"; Touch "$d\index.html"; Touch "$d\data\System.json"; Touch "$d\js\main.js"; Touch "$d\js\rmmz_core.js"; Touch "$d\Game.exe"
$fixtures.Add($d)

# RPG Maker legacy
$d = New-Fixture 'rpgm_xp';   Touch "$d\Data\Scripts.rxdata";  Touch "$d\Game.exe"; Touch "$d\RPG_RT.exe"; $fixtures.Add($d)
$d = New-Fixture 'rpgm_vx';   Touch "$d\Data\Scripts.rvdata";  Touch "$d\Game.exe"; $fixtures.Add($d)
$d = New-Fixture 'rpgm_vxace'; Touch "$d\Data\Scripts.rvdata2"; Touch "$d\Game.exe"; $fixtures.Add($d)
$d = New-Fixture 'rpgm_legacy_data_only_dir'; Mkdir "$d\Data"; Touch "$d\Game.exe"; $fixtures.Add($d)

# Unity
$d = New-Fixture 'unity_mono'
Mkdir "$d\MyGame_Data\Managed"; Touch "$d\MyGame.exe"; Touch "$d\UnityCrashHandler64.exe"; Touch "$d\UnityPlayer.dll"
$fixtures.Add($d)
$d = New-Fixture 'unity_il2cpp_gameassembly'
Mkdir "$d\MyGame_Data"; Touch "$d\GameAssembly.dll"; Touch "$d\MyGame.exe"
$fixtures.Add($d)
$d = New-Fixture 'unity_il2cpp_data_dir'
Mkdir "$d\MyGame_Data\il2cpp_data\Metadata"; Touch "$d\MyGame.exe"
$fixtures.Add($d)
$d = New-Fixture 'unity_data_suffix_case'
Mkdir "$d\Foo_DATA"; Touch "$d\Foo.exe"; Touch "$d\Setup.exe"
$fixtures.Add($d)
# *_Data is a file, not a dir -> not Unity
$d = New-Fixture 'unity_data_is_file'
Touch "$d\Foo_Data"; Touch "$d\Foo.exe"
$fixtures.Add($d)
# Unity with stray .pck -> Unity must win over Godot
$d = New-Fixture 'unity_with_stray_pck'
Mkdir "$d\Foo_Data"; Touch "$d\Foo.exe"; Touch "$d\stray.pck"
$fixtures.Add($d)

# Godot
$d = New-Fixture 'godot_pck';            Touch "$d\game.pck"; Touch "$d\game.exe"; $fixtures.Add($d)
$d = New-Fixture 'godot_project';        Touch "$d\project.godot"; $fixtures.Add($d)
$d = New-Fixture 'godot_dot_dir';        Mkdir "$d\.godot"; $fixtures.Add($d)
$d = New-Fixture 'godot_binary_project'; Touch "$d\godot_project.binary"; $fixtures.Add($d)
$d = New-Fixture 'godot_embedded_pck'
Touch "$d\game.exe" (New-PckPeBytes $true $true); Touch "$d\other.exe"
$fixtures.Add($d)
$d = New-Fixture 'godot_embedded_tail_only'   # GDPC tail but no pck section -> not Godot
Touch "$d\game.exe" (New-PckPeBytes $false $true)
$fixtures.Add($d)
$d = New-Fixture 'godot_embedded_section_only' # pck section but no GDPC tail -> not Godot
Touch "$d\game.exe" (New-PckPeBytes $true $false)
$fixtures.Add($d)
$d = New-Fixture 'godot_embedded_ignored_name' # embedded pck in a CrashHandler-named exe is ignored
Touch "$d\FooCrashHandler.exe" (New-PckPeBytes $true $true)
$fixtures.Add($d)

# exe scoring
$d = New-Fixture 'exe_tie_break_name'; Touch "$d\zeta.exe"; Touch "$d\Alpha.exe"; Touch "$d\beta.exe"; $fixtures.Add($d)
$d = New-Fixture 'exe_leaf_match';     Touch "$d\other.exe"; Touch "$d\EXE_LEAF_MATCH.exe"; $fixtures.Add($d)
$d = New-Fixture 'exe_installer_penalty'; Touch "$d\unins000.exe"; Touch "$d\Uninstall.exe"; Touch "$d\Config.exe"; Touch "$d\Configuration.exe"; Touch "$d\Installer.exe"; Touch "$d\Install.exe"; Touch "$d\Setup.exe"; Touch "$d\zzz.exe"; $fixtures.Add($d)
$d = New-Fixture 'exe_only_penalized';  Touch "$d\Setup.exe"; Touch "$d\unins001.exe"; $fixtures.Add($d)
$d = New-Fixture 'exe_ignored_only';    Touch "$d\dst_server.exe"; Touch "$d\DeepSeekTranslator.exe"; Touch "$d\dst_godot_patch.exe"; Touch "$d\ds游戏翻译器.exe"; Touch "$d\FooCrashHandler.exe"; $fixtures.Add($d)
$d = New-Fixture 'exe_dir_named_exe';   Mkdir "$d\folder.exe"; Touch "$d\real.exe"; $fixtures.Add($d)
$d = New-Fixture 'exe_stem_pck';        Touch "$d\a.exe"; Touch "$d\b.exe"; Touch "$d\b.pck"; $fixtures.Add($d)
$d = New-Fixture 'exe_stem_data_vs_pck'; Mkdir "$d\a_Data"; Touch "$d\a.exe"; Touch "$d\b.exe"; Touch "$d\b.pck"; $fixtures.Add($d)
$d = New-Fixture 'exe_unicode_名字';     Touch "$d\游戏.exe"; Touch "$d\ゲーム.exe"; $fixtures.Add($d)
$d = New-Fixture 'exe_bare_dot_exe';    Touch "$d\.exe"; Touch "$d\x.exe"; $fixtures.Add($d)

# misc
$d = New-Fixture 'empty_dir'; $fixtures.Add($d)
$fixtures.Add((Join-Path $root 'does_not_exist'))

# trailing-separator variants of a few fixtures (path_join must not double the slash)
$fixtures.Add((Join-Path $root 'unity_mono') + '\')
$fixtures.Add((Join-Path $root 'rpgm_mv_www') + '\')
$fixtures.Add((Join-Path $root 'godot_pck') + '/')

# ---------------------------------------------------------------- run

# CommandLineToArgvW-safe quoting (PS 5.1 has no ProcessStartInfo.ArgumentList):
# a run of N backslashes right before a quote must be doubled, embedded quotes escaped.
function Quote-Arg([string]$a) {
    if ($a -eq '') { return '""' }
    $sb = New-Object System.Text.StringBuilder
    $null = $sb.Append('"')
    $bs = 0
    foreach ($ch in $a.ToCharArray()) {
        if ($ch -eq '\') { $bs++; continue }
        if ($ch -eq '"') { $null = $sb.Append('\', $bs * 2 + 1).Append('"'); $bs = 0; continue }
        if ($bs -gt 0) { $null = $sb.Append('\', $bs); $bs = 0 }
        $null = $sb.Append($ch)
    }
    if ($bs -gt 0) { $null = $sb.Append('\', $bs * 2) }
    $null = $sb.Append('"')
    return $sb.ToString()
}

function Invoke-Detect([string]$exe, [string[]]$argv) {
    $psi = New-Object System.Diagnostics.ProcessStartInfo
    $psi.FileName = $exe
    $psi.UseShellExecute = $false
    $psi.RedirectStandardOutput = $true
    $psi.RedirectStandardError = $true
    $psi.StandardOutputEncoding = [Text.Encoding]::UTF8
    $psi.StandardErrorEncoding = [Text.Encoding]::UTF8
    $psi.CreateNoWindow = $true
    $psi.Arguments = (($argv | ForEach-Object { Quote-Arg $_ }) -join ' ')
    $p = [System.Diagnostics.Process]::Start($psi)
    $out = $p.StandardOutput.ReadToEnd()
    $err = $p.StandardError.ReadToEnd()
    if (-not $p.WaitForExit(30000)) { try { $p.Kill() } catch { } ; throw "timeout: $exe $($argv -join ' ')" }
    return @{ Code = $p.ExitCode; Out = $out; Err = $err }
}

$pass = 0; $fail = 0
$failures = New-Object System.Collections.Generic.List[string]
foreach ($dir in $fixtures) {
    $c = Invoke-Detect $CLauncher @('--detect-and-exit', $dir)
    $cs = Invoke-Detect $CsLauncher @('--detect', $dir)
    $label = $dir.Substring($root.Length).TrimStart('\')
    if ($c.Code -ne 0 -or $cs.Code -ne 0) {
        $fail++
        $failures.Add("[$label] exit codes C=$($c.Code) C#=$($cs.Code)`n  C stderr: $($c.Err)`n  C# stderr: $($cs.Err)")
        continue
    }
    if ($c.Out -ne $cs.Out) {
        $fail++
        $failures.Add("[$label] output differs`n--- C ---`n$($c.Out)--- C# ---`n$($cs.Out)")
        continue
    }
    if ($c.Out -notmatch '^engine=') {
        $fail++
        $failures.Add("[$label] C launcher produced no report (stdout not inherited?)`n$($c.Out)")
        continue
    }
    $pass++
    $first = ($c.Out -split "`n")[0]
    Write-Host ("  ok  {0,-36} {1}" -f $label, $first)
}

# usage error parity: missing dir argument -> exit 2, no stdout
$cU = Invoke-Detect $CLauncher @('--detect-and-exit')
$csU = Invoke-Detect $CsLauncher @('--detect')
if ($cU.Code -eq 2 -and $csU.Code -eq 2 -and $cU.Out -eq '' -and $csU.Out -eq '') { $pass++; Write-Host "  ok  usage-error exit code 2" }
else { $fail++; $failures.Add("usage error: C=$($cU.Code)/'$($cU.Out)' C#=$($csU.Code)/'$($csU.Out)'") }

# ---------------------------------------------------------------- deploy / restore parity
#
# Each scenario is materialised twice (identical trees under c\ and cs\), the C launcher
# runs on one copy and the C# launcher on the other, and we compare: exit code, the full
# stdout (log= lines + result=), and a snapshot of the resulting tree (relative path,
# attributes, SHA-256). Paths inside log lines differ only by the c\ / cs\ segment, so the
# snapshot and stdout are normalised by replacing each copy's root with <ROOT>.

$sha = [System.Security.Cryptography.SHA256]::Create()
function Snapshot-Tree([string]$root) {
    $lines = New-Object System.Collections.Generic.List[string]
    Get-ChildItem -LiteralPath $root -Recurse -Force | Sort-Object FullName | ForEach-Object {
        $rel = $_.FullName.Substring($root.Length).TrimStart('\')
        if ($_.PSIsContainer) { $lines.Add("D $rel") }
        else {
            $hash = [BitConverter]::ToString($sha.ComputeHash([IO.File]::ReadAllBytes($_.FullName))).Replace('-', '')
            $ro = if ($_.Attributes -band [IO.FileAttributes]::ReadOnly) { 'R' } else { '-' }
            $lines.Add("F $ro $rel $hash")
        }
    }
    return ($lines -join "`n")
}

$renpyHookBytes = [IO.File]::ReadAllBytes((Join-Path $repo 'payloads\RenPy\iron_deepseek.rpy'))
$rpgmHookBytes = [IO.File]::ReadAllBytes((Join-Path $repo 'payloads\RPGMaker\hook_rpgm_mv.js'))
$asciiEnc = [Text.Encoding]::ASCII

$indexPlain = "<!DOCTYPE html>`r`n<html>`r`n<head>`r`n<meta charset=`"UTF-8`">`r`n</head>`r`n<body style=`"background-color: black`">`r`n<script type=`"text/javascript`" src=`"js/libs/pixi.js`"></script>`r`n<script type=`"text/javascript`" src=`"js/main.js`"></script>`r`n</body>`r`n</html>`r`n"
$indexAlreadyHooked = "<!DOCTYPE html>`r`n<html>`r`n<body>`r`n<script type=`"text/javascript`" src=`"js/libs/pixi.js`"></script>`r`n   `r`n<script type=`"text/javascript`" src=`"js/hook_rpgm_mv.js`"></script>`r`n<script type=`"text/javascript`" src=`"js/main.js`"></script>`r`n</body>`r`n</html>`r`n"
$indexSelfClosing = "<html><body>`n<SCRIPT SRC=`"js/hook_rpgm_mv.js`" />`n<script src=`"js/main.js`"></script>`n</body></html>"
$indexNoMain = "<html><body>`n<p>hook_rpgm_mv.js mentioned in text only</p>`n</body></html>"
$indexNoBodyNoMain = "<html><head><title>x</title></head></html>"
$indexWithNul = $asciiEnc.GetBytes("<html><body>`n<script src=`"js/main.js`"></script>`n") + [byte[]](0) + $asciiEnc.GetBytes("<script src=`"js/hook_rpgm_mv.js`"></script></body></html>")

function New-RpgmTree([string]$d, [object]$index, [bool]$www) {
    $cr = if ($www) { Join-Path $d 'www' } else { $d }
    Touch "$cr\data\System.json"; Touch "$cr\js\main.js"; Touch "$cr\js\rpg_core.js"; Touch "$d\Game.exe"
    if ($index -is [string]) { Touch "$cr\index.html" ($asciiEnc.GetBytes($index)) }
    elseif ($null -ne $index) { Touch "$cr\index.html" ([byte[]]$index) }
}

# ---------------------------------------------------------------- synthetic launcher root
# Unity deploy copies BepInEx/XUnity/TMP payloads from <launcher-dir>\payloads. The real
# payloads are ~220 MB per deploy, so the deploy/restore scenarios run both launchers against
# a synthetic root with tiny stand-in payloads: the C launcher is copied there (g_root is its
# exe directory; the diagnostic modes never touch anything else under g_root) and the C#
# launcher gets the same directory via --root. The tree below also deliberately omits
# UnityMonoRuntime6X86 so the "missing payload" branch is exercised.
function New-PeBytes([uint16]$machine) {
    if ($machine -eq 0) { return $asciiEnc.GetBytes('not a PE file at all, just padding bytes .................') }
    $dos = New-Object byte[] 64
    $dos[0] = 0x4D; $dos[1] = 0x5A
    [BitConverter]::GetBytes([int32]64).CopyTo($dos, 60)
    $sig = [byte[]](0x50, 0x45, 0x00, 0x00)
    $fh = New-Object byte[] 20
    [BitConverter]::GetBytes([uint16]$machine).CopyTo($fh, 0)
    return [byte[]]($dos + $sig + $fh + (New-Object byte[] 8))
}
$PE_X86 = [uint16]0x014c; $PE_X64 = [uint16]0x8664; $PE_ARM = [uint16]0x01c4

$tpl5 = $asciiEnc.GetBytes('synthetic UnityTranslator.dll (BepInEx 5 template)')
$tpl6 = $asciiEnc.GetBytes('synthetic UnityTranslator.BepInEx6.dll (BepInEx 6 template)')
$newtonsoftBytes = $asciiEnc.GetBytes('synthetic Newtonsoft.Json.dll')
$fontPatcherBytes = $asciiEnc.GetBytes('synthetic DeepSeekUnityFontPatcher.dll')
$corlibMarkerBytes = $asciiEnc.GetBytes("synthetic-corlib-marker`n")
$corlibComplete = $asciiEnc.GetBytes('MZ.. mscorlib .. System.IO .. WriteAllText ..')
$corlibStripped = $asciiEnc.GetBytes('MZ.. mscorlib .. System.IO .. (no file writer)')
$xunityMarkerText = "ds-game-translator:xunity-auto-translator:v1`n"

$synthRoot = Join-Path $root 'synth_root'
Mkdir $synthRoot
$CLauncherSynth = Join-Path $synthRoot (Split-Path -Leaf $CLauncher)
Copy-Item -LiteralPath $CLauncher -Destination $CLauncherSynth
$P = Join-Path $synthRoot 'payloads'
Touch "$P\UnityTranslator\UnityTranslator.dll" $tpl5
Touch "$P\UnityTranslator\UnityTranslator.BepInEx6.dll" $tpl6
Touch "$P\UnityTranslator\Newtonsoft.Json.dll" $newtonsoftBytes
Touch "$P\UnityTranslator\DeepSeekUnityFontPatcher.dll" $fontPatcherBytes
foreach ($rt in @(@('UnityMonoRuntime', $PE_X64, $false), @('UnityMonoRuntimeX86', $PE_X86, $false), @('UnityMonoRuntime6', $PE_X64, $true))) {
    $name, $machine, $six = $rt
    Touch "$P\$name\winhttp.dll" (New-PeBytes $machine)
    Touch "$P\$name\doorstop_config.ini" ($asciiEnc.GetBytes("[General]`r`nenabled = true`r`n`r`n[UnityMono]`r`ntarget_assembly = BepInEx\core\BepInEx.Preloader.dll`r`n"))
    Touch "$P\$name\.doorstop_version" ($asciiEnc.GetBytes("4.0.0"))
    if ($six) {
        Touch "$P\$name\BepInEx\core\BepInEx.Unity.Mono.dll" ($asciiEnc.GetBytes("bep6 core $name"))
        Touch "$P\$name\BepInEx\core\BepInEx.Unity.Mono.Preloader.dll" ($asciiEnc.GetBytes("bep6 preloader $name"))
    } else {
        Touch "$P\$name\BepInEx\core\BepInEx.dll" ($asciiEnc.GetBytes("bep5 core $name"))
        Touch "$P\$name\BepInEx\core\BepInEx.Preloader.dll" ($asciiEnc.GetBytes("bep5 preloader $name"))
    }
    Touch "$P\$name\BepInEx\core\0Harmony.dll" ($asciiEnc.GetBytes("harmony $name"))
}
Touch "$P\UnityMonoCorlib\.dst-installed-by-ds" $corlibMarkerBytes
Touch "$P\UnityMonoCorlib\mscorlib.dll" $corlibComplete
Touch "$P\UnityMonoCorlib\System.dll" ($asciiEnc.GetBytes('synthetic System.dll'))
Touch "$P\UnityIL2CPP\BepInExRuntime\doorstop_config.ini" ($asciiEnc.GetBytes("[General]`r`nenabled = true`r`n"))
Touch "$P\UnityIL2CPP\BepInExRuntime\winhttp.dll" (New-PeBytes $PE_X64)
Touch "$P\UnityIL2CPP\BepInExRuntime\.doorstop_version" ($asciiEnc.GetBytes("4.0.0-il2cpp"))
Touch "$P\UnityIL2CPP\BepInExRuntime\dotnet\coreclr.dll" ($asciiEnc.GetBytes('synthetic coreclr'))
Touch "$P\UnityIL2CPP\BepInExRuntime\dotnet\shared\System.Runtime.dll" ($asciiEnc.GetBytes('synthetic System.Runtime'))
Touch "$P\UnityIL2CPP\BepInExRuntime\BepInEx\core\BepInEx.Core.dll" ($asciiEnc.GetBytes('synthetic BepInEx.Core'))
Touch "$P\UnityIL2CPP\BepInExRuntime\BepInEx\core\BepInEx.Unity.IL2CPP.dll" ($asciiEnc.GetBytes('synthetic BepInEx.Unity.IL2CPP'))
Touch "$P\UnityIL2CPP\BepInExRuntime\BepInEx\patchers\Il2CppInteropPatcher.dll" ($asciiEnc.GetBytes('synthetic patcher'))
Touch "$P\UnityIL2CPP\XUnityAutoTranslator\BepInEx\core\XUnity.Common.dll" ($asciiEnc.GetBytes('synthetic XUnity.Common'))
Touch "$P\UnityIL2CPP\XUnityAutoTranslator\BepInEx\plugins\XUnity.AutoTranslator\XUnity.AutoTranslator.Plugin.Core.dll" ($asciiEnc.GetBytes('synthetic XUnity core'))
Touch "$P\UnityIL2CPP\XUnityAutoTranslator\BepInEx\plugins\XUnity.AutoTranslator\Translators\FullNameSplitter.dll" ($asciiEnc.GetBytes('synthetic translator'))
Touch "$P\UnityIL2CPP\XUnityAutoTranslator\BepInEx\plugins\XUnity.ResourceRedirector\XUnity.ResourceRedirector.dll" ($asciiEnc.GetBytes('synthetic redirector'))
Touch "$P\UnityIL2CPP\DeepSeekXUnityTranslator\DeepSeekTranslate.dll" ($asciiEnc.GetBytes('synthetic DeepSeekTranslate endpoint'))
Touch "$P\UnityIL2CPP\TMPFontAssetBundles\BepInEx\font\cjk_fallback.bundle" ($asciiEnc.GetBytes('synthetic font bundle'))
Touch "$P\UnityIL2CPP\DeepSeekTMPFontFallback\BepInEx\plugins\DeepSeekTMPFontFallback\DeepSeekTMPFontFallback.dll" ($asciiEnc.GetBytes('synthetic TMP fallback'))

function New-UnityMonoTree([string]$d, [string]$version, [uint16]$machine, [bool]$stripped) {
    Touch "$d\MyGame.exe" (New-PeBytes $machine)
    Touch "$d\UnityPlayer.dll" (New-PeBytes $machine)
    Touch "$d\UnityCrashHandler64.exe"
    Touch "$d\MyGame_Data\globalgamemanagers" ($asciiEnc.GetBytes("....$version....`0`0"))
    Touch "$d\MyGame_Data\Managed\mscorlib.dll" $(if ($stripped) { $corlibStripped } else { $corlibComplete })
    Touch "$d\MyGame_Data\Managed\Assembly-CSharp.dll"
}
function New-UnityIl2cppTree([string]$d, [uint16]$machine) {
    Touch "$d\MyGame.exe" (New-PeBytes $machine)
    Touch "$d\UnityPlayer.dll" (New-PeBytes $machine)
    Touch "$d\GameAssembly.dll" (New-PeBytes $machine)
    Touch "$d\MyGame_Data\il2cpp_data\Metadata\global-metadata.dat"
    Touch "$d\MyGame_Data\globalgamemanagers" ($asciiEnc.GetBytes("....2022.3.1f1...."))
}

# scenario table: name -> { Build = scriptblock(dir); Steps = ordered list of 'deploy'/'restore' }
$scenarios = New-Object System.Collections.Specialized.OrderedDictionary
$scenarios['renpy_deploy_then_restore'] = @{
    Build = { param($d) Touch "$d\game\script.rpy"; Touch "$d\Game.exe" }
    Steps = @('deploy', 'restore')
}
$scenarios['renpy_deploy_twice_idempotent'] = @{
    Build = { param($d) Touch "$d\game\script.rpy"; Touch "$d\Game.exe" }
    Steps = @('deploy', 'deploy')
}
$scenarios['renpy_stale_rpyc_readonly'] = @{
    Build = { param($d)
        Touch "$d\game\script.rpy"; Touch "$d\game\iron_deepseek.rpyc" ([byte[]](1,2,3))
        Set-ItemProperty -LiteralPath "$d\game\iron_deepseek.rpyc" -Name IsReadOnly -Value $true
        Touch "$d\Game.exe" }
    Steps = @('deploy')
}
$scenarios['renpy_rpyc_is_directory'] = @{
    Build = { param($d) Touch "$d\game\script.rpy"; Mkdir "$d\game\iron_deepseek.rpyc"; Touch "$d\Game.exe" }
    Steps = @('deploy')
}
$scenarios['renpy_font_already_present'] = @{
    Build = { param($d) Touch "$d\game\archive.rpa"; Touch "$d\game\ds_font.ttf" ([byte[]](9,9)); Touch "$d\Game.exe" }
    Steps = @('deploy', 'restore')
}
$scenarios['renpy_restore_untouched'] = @{
    Build = { param($d) Touch "$d\game\script.rpy"; Touch "$d\Game.exe" }
    Steps = @('restore')
}
$scenarios['rpgm_www_deploy_restore'] = @{
    Build = { param($d) New-RpgmTree $d $indexPlain $true }
    Steps = @('deploy', 'restore')
}
$scenarios['rpgm_flat_deploy_restore'] = @{
    Build = { param($d) New-RpgmTree $d $indexPlain $false }
    Steps = @('deploy', 'restore')
}
$scenarios['rpgm_deploy_twice_idempotent'] = @{
    Build = { param($d) New-RpgmTree $d $indexPlain $true }
    Steps = @('deploy', 'deploy')
}
$scenarios['rpgm_already_hooked_with_blank_line'] = @{
    Build = { param($d) New-RpgmTree $d $indexAlreadyHooked $true }
    Steps = @('deploy', 'restore')
}
$scenarios['rpgm_self_closing_uppercase_tag'] = @{
    Build = { param($d) New-RpgmTree $d $indexSelfClosing $true }
    Steps = @('restore', 'deploy')
}
$scenarios['rpgm_hook_name_in_text_only'] = @{
    Build = { param($d) New-RpgmTree $d $indexNoMain $true }
    Steps = @('deploy', 'restore')
}
$scenarios['rpgm_no_body_no_main'] = @{
    Build = { param($d) New-RpgmTree $d $indexNoBodyNoMain $true }
    Steps = @('deploy')
}
$scenarios['rpgm_index_with_embedded_nul'] = @{
    Build = { param($d) New-RpgmTree $d $indexWithNul $true }
    Steps = @('deploy', 'restore')
}
$scenarios['rpgm_restore_from_backup_only'] = @{
    Build = { param($d) New-RpgmTree $d $null $true; Touch "$d\www\index.html.dst-backup" ($asciiEnc.GetBytes($indexPlain)) }
    Steps = @('restore')
}
$scenarios['rpgm_restore_backup_kept_when_index_modified'] = @{
    Build = { param($d) New-RpgmTree $d $indexPlain $true; Touch "$d\www\index.html.dst-backup" ($asciiEnc.GetBytes('<html>user backup</html>')) }
    Steps = @('restore')
}
$scenarios['rpgm_readonly_index'] = @{
    Build = { param($d) New-RpgmTree $d $indexPlain $true
        Set-ItemProperty -LiteralPath "$d\www\index.html" -Name IsReadOnly -Value $true }
    Steps = @('deploy')
}
$scenarios['rpgm_flat_mz_without_exe'] = @{
    Build = { param($d) Touch "$d\index.html" ($asciiEnc.GetBytes($indexPlain)); Touch "$d\data\System.json"; Touch "$d\js\main.js"; Touch "$d\js\rmmz_core.js" }
    Steps = @('deploy', 'restore')
}
$scenarios['rpgm_legacy_noop'] = @{
    Build = { param($d) Touch "$d\Data\Scripts.rxdata"; Touch "$d\Game.exe" }
    Steps = @('deploy', 'restore')
}
$scenarios['godot_deploy_restore_owned_patch'] = @{
    Build = { param($d) Touch "$d\game.pck"; Touch "$d\game.exe"; Touch "$d\dst_godot_patch.exe"; Touch "$d\dst_godot_patch.exe.dst-owned"; Touch "$d\dst_godot_runtime.gd" }
    Steps = @('deploy', 'restore')
}
$scenarios['godot_restore_unowned_patch_exe'] = @{
    Build = { param($d) Touch "$d\project.godot"; Touch "$d\dst_godot_patch.exe"; Touch "$d\dst_godot_patch.pck" }
    Steps = @('restore')
}
$scenarios['unknown_engine'] = @{
    Build = { param($d) Touch "$d\readme.txt" }
    Steps = @('deploy', 'restore')
}
$scenarios['nonexistent_dir'] = @{
    Build = { param($d) }
    Steps = @('restore')
    SkipCreate = $true
}

# ---- Unity Mono (BepInEx 5/6 runtime install, repair, stripped corlib, ownership) ----
$scenarios['unity_mono_deploy_restore'] = @{
    Build = { param($d) New-UnityMonoTree $d '2021.3.5f1' $PE_X64 $false }
    Steps = @('deploy', 'restore')
}
$scenarios['unity_mono_deploy_twice_idempotent'] = @{
    Build = { param($d) New-UnityMonoTree $d '2021.3.5f1' $PE_X64 $false }
    Steps = @('deploy', 'deploy')
}
$scenarios['unity6_mono_deploy_restore'] = @{
    Build = { param($d) New-UnityMonoTree $d '6000.0.23f1' $PE_X64 $false }
    Steps = @('deploy', 'restore')
}
$scenarios['unity6_mono_x86_missing_payload'] = @{
    Build = { param($d) New-UnityMonoTree $d '6000.1.0f1' $PE_X86 $false }
    Steps = @('deploy')
}
$scenarios['unity_mono_x86_deploy'] = @{
    Build = { param($d) New-UnityMonoTree $d '2019.4.40f1' $PE_X86 $false }
    Steps = @('deploy', 'restore')
}
$scenarios['unity_mono_unsupported_machine'] = @{
    Build = { param($d) New-UnityMonoTree $d '2020.3.0f1' $PE_ARM $false }
    Steps = @('deploy')
}
$scenarios['unity_mono_unknown_machine_defaults_x64'] = @{
    Build = { param($d) New-UnityMonoTree $d '5.6.7f1' 0 $false }
    Steps = @('deploy')
}
$scenarios['unity_mono_no_version_string'] = @{
    Build = { param($d) New-UnityMonoTree $d 'no-digits-here' $PE_X64 $false }
    Steps = @('deploy')
}
$scenarios['unity_mono_stripped_deploy_restore'] = @{
    Build = { param($d) New-UnityMonoTree $d '2021.3.5f1' $PE_X64 $true }
    Steps = @('deploy', 'restore')
}
$scenarios['unity_mono_stripped_deploy_twice'] = @{
    Build = { param($d) New-UnityMonoTree $d '2021.3.5f1' $PE_X64 $true }
    Steps = @('deploy', 'deploy')
}
$scenarios['unity_mono_stripped_existing_ini_sections'] = @{
    Build = { param($d) New-UnityMonoTree $d '2018.4.0f1' $PE_X64 $true
        # user-provided BepInEx 5 with its own configs: the stripped-runtime keys must be inserted, not appended as duplicate sections
        Touch "$d\winhttp.dll" (New-PeBytes $PE_X64)
        Touch "$d\doorstop_config.ini" ($asciiEnc.GetBytes("; user doorstop`r`n[UnityMono]`r`nenabled = true`r`n[Other]`r`nx = 1`r`n"))
        Touch "$d\.doorstop_version"
        Touch "$d\BepInEx\core\BepInEx.dll" ($asciiEnc.GetBytes('user bepinex'))
        Touch "$d\BepInEx\core\BepInEx.Preloader.dll" ($asciiEnc.GetBytes('user preloader'))
        Touch "$d\BepInEx\config\BepInEx.cfg" ($asciiEnc.GetBytes("[Logging]`r`n`r`n## comment`r`nUnityLogListening = true`r`n`r`n[Chainloader]`r`nHideManagerGameObject = false")) }
    Steps = @('deploy', 'restore')
}
$scenarios['unity_mono_stripped_user_owned_conflict'] = @{
    Build = { param($d) New-UnityMonoTree $d '2021.3.5f1' $PE_X64 $true
        Touch "$d\doorstop_config.ini" ($asciiEnc.GetBytes("[UnityMono]`r`ndll_search_path_override = Custom`r`n"))
        Touch "$d\doorstop_config.ini.dst-stripped-owned" ($asciiEnc.GetBytes("[UnityMono]`r`ndll_search_path_override = BepInEx\unstripped_corlib;BepInEx\core`r`n")) }
    Steps = @('deploy', 'restore')
}
$scenarios['unity_mono_user_plugin_backup_restore'] = @{
    Build = { param($d) New-UnityMonoTree $d '2021.3.5f1' $PE_X64 $false
        Touch "$d\BepInEx\plugins\UnityTranslator.dll" ($asciiEnc.GetBytes('user-built UnityTranslator'))
        Touch "$d\BepInEx\plugins\Newtonsoft.Json.dll" ($asciiEnc.GetBytes('user Newtonsoft 12.0')) }
    Steps = @('deploy', 'restore')
}
$scenarios['unity_mono_existing_partial_bepinex_repair'] = @{
    Build = { param($d) New-UnityMonoTree $d '2021.3.5f1' $PE_X64 $false
        Touch "$d\BepInEx\plugins\SomeOtherMod.dll" ($asciiEnc.GetBytes('other mod')) }
    Steps = @('deploy', 'restore')
}
$scenarios['unity_mono_existing_bepinex_wrong_loader_arch'] = @{
    Build = { param($d) New-UnityMonoTree $d '2021.3.5f1' $PE_X86 $false
        Touch "$d\winhttp.dll" (New-PeBytes $PE_X64)
        Touch "$d\doorstop_config.ini"; Touch "$d\.doorstop_version"
        Touch "$d\BepInEx\core\BepInEx.dll"; Touch "$d\BepInEx\core\BepInEx.Preloader.dll" }
    Steps = @('deploy')
}
$scenarios['unity6_mono_upgrade_from_bepinex5'] = @{
    Build = { param($d) New-UnityMonoTree $d '6000.0.1f1' $PE_X64 $false
        Touch "$d\winhttp.dll" (New-PeBytes $PE_X64); Touch "$d\doorstop_config.ini"; Touch "$d\.doorstop_version"
        Touch "$d\BepInEx\core\BepInEx.dll"; Touch "$d\BepInEx\core\BepInEx.Preloader.dll" }
    Steps = @('deploy', 'restore')
}
$scenarios['unity_mono_restore_untouched'] = @{
    Build = { param($d) New-UnityMonoTree $d '2021.3.5f1' $PE_X64 $false }
    Steps = @('restore')
}
$scenarios['unity_mono_restore_modified_plugin_preserved'] = @{
    Build = { param($d) New-UnityMonoTree $d '2021.3.5f1' $PE_X64 $false
        Touch "$d\BepInEx\plugins\UnityTranslator.dll" ($asciiEnc.GetBytes('modified after deploy'))
        Touch "$d\BepInEx\plugins\UnityTranslator.dll.dst-backup" ($asciiEnc.GetBytes('original user plugin'))
        Touch "$d\BepInEx\patchers\DeepSeekUnityFontPatcher.dll" ($asciiEnc.GetBytes('unowned patcher'))
        Touch "$d\BepInEx\unstripped_corlib\mscorlib.dll" ($asciiEnc.GetBytes('no marker')) }
    Steps = @('restore')
}

# ---- Unity IL2CPP (BepInEx be.755 + XUnity, config ownership, disable old Mono files) ----
$scenarios['unity_il2cpp_deploy_restore'] = @{
    Build = { param($d) New-UnityIl2cppTree $d $PE_X64 }
    Steps = @('deploy', 'restore')
}
$scenarios['unity_il2cpp_deploy_twice_idempotent'] = @{
    Build = { param($d) New-UnityIl2cppTree $d $PE_X64 }
    Steps = @('deploy', 'deploy')
}
$scenarios['unity_il2cpp_unknown_machine_defaults_x64'] = @{
    Build = { param($d) New-UnityIl2cppTree $d 0 }
    Steps = @('deploy')
}
$scenarios['unity_il2cpp_x86_skipped'] = @{
    Build = { param($d) New-UnityIl2cppTree $d $PE_X86 }
    Steps = @('deploy', 'restore')
}
$scenarios['unity_il2cpp_old_bundled_mono_plugin_disabled'] = @{
    Build = { param($d) New-UnityIl2cppTree $d $PE_X64
        Touch "$d\BepInEx\plugins\UnityTranslator.dll" $tpl5
        Touch "$d\BepInEx\plugins\UnityTranslator.pdb" ($asciiEnc.GetBytes('pdb'))
        Touch "$d\BepInEx\core\Il2Cppmscorlib.dll" ($asciiEnc.GetBytes('old interop mscorlib')) }
    Steps = @('deploy', 'restore')
}
$scenarios['unity_il2cpp_foreign_mono_plugin_kept'] = @{
    Build = { param($d) New-UnityIl2cppTree $d $PE_X64
        Touch "$d\BepInEx\plugins\UnityTranslator.dll" ($asciiEnc.GetBytes('third-party plugin with same name'))
        Touch "$d\BepInEx\plugins\UnityTranslator.dll.disabled" ($asciiEnc.GetBytes('unknown disabled copy')) }
    Steps = @('deploy', 'restore')
}
$scenarios['unity_il2cpp_config_owned_migration'] = @{
    Build = { param($d) New-UnityIl2cppTree $d $PE_X64
        # XUnity appended its own section to the live config but the managed key still equals the ownership snapshot -> migrate in place
        Touch "$d\BepInEx\config\AutoTranslatorConfig.ini" ($asciiEnc.GetBytes("[Behaviour]`nMaxCharactersPerTranslation=200`nEnableBatching=True`n`n[XUnityAdded]`nFoo=1`n"))
        Touch "$d\BepInEx\config\AutoTranslatorConfig.ini.dst-owned" ($asciiEnc.GetBytes("[Behaviour]`nMaxCharactersPerTranslation=200`nEnableBatching=True`n")) }
    Steps = @('deploy', 'restore')
}
$scenarios['unity_il2cpp_config_user_modified_preserved'] = @{
    Build = { param($d) New-UnityIl2cppTree $d $PE_X64
        Touch "$d\BepInEx\config\AutoTranslatorConfig.ini" ($asciiEnc.GetBytes("[Behaviour]`nMaxCharactersPerTranslation=999`n"))
        Touch "$d\BepInEx\config\AutoTranslatorConfig.ini.dst-owned" ($asciiEnc.GetBytes("[Behaviour]`nMaxCharactersPerTranslation=200`n")) }
    Steps = @('deploy', 'restore')
}
$scenarios['unity_il2cpp_user_config_backed_up'] = @{
    Build = { param($d) New-UnityIl2cppTree $d $PE_X64
        Touch "$d\BepInEx\config\AutoTranslatorConfig.ini" ($asciiEnc.GetBytes("[Service]`nEndpoint=GoogleTranslate`n")) }
    Steps = @('deploy', 'restore')
}
$scenarios['unity_il2cpp_preexisting_xunity_dir_no_marker'] = @{
    Build = { param($d) New-UnityIl2cppTree $d $PE_X64
        Touch "$d\BepInEx\plugins\XUnity.AutoTranslator\XUnity.AutoTranslator.Plugin.Core.dll" ($asciiEnc.GetBytes('user xunity 5.3')) }
    Steps = @('deploy', 'restore')
}
$scenarios['unity_il2cpp_restore_untouched'] = @{
    Build = { param($d) New-UnityIl2cppTree $d $PE_X64 }
    Steps = @('restore')
}
$scenarios['unity_il2cpp_restore_mscorlib_conflict'] = @{
    Build = { param($d) New-UnityIl2cppTree $d $PE_X64
        Touch "$d\BepInEx\core\Il2Cppmscorlib.dll" ($asciiEnc.GetBytes('new'))
        Touch "$d\BepInEx\core\Il2Cppmscorlib.dll.disabled" ($asciiEnc.GetBytes('old')) }
    Steps = @('restore')
}

$cRootArg = $synthRoot
foreach ($name in $scenarios.Keys) {
    $sc = $scenarios[$name]
    $cDir = Join-Path $root "dr\c\$name"
    $csDir = Join-Path $root "dr\cs\$name"
    if (-not $sc.SkipCreate) {
        Mkdir $cDir; Mkdir $csDir
        & $sc.Build $cDir
        & $sc.Build $csDir
    }
    $step = 0
    $scenarioFailed = $false
    foreach ($op in $sc.Steps) {
        $step++
        $cFlag = if ($op -eq 'deploy') { '--deploy-and-exit' } else { '--restore-and-exit' }
        $csFlag = if ($op -eq 'deploy') { '--deploy' } else { '--restore' }
        $c = Invoke-Detect $CLauncherSynth @($cFlag, $cDir)
        $cs = Invoke-Detect $CsLauncher @($csFlag, $csDir, '--root', $cRootArg)
        $cOut = $c.Out.Replace($cDir, '<ROOT>')
        $csOut = $cs.Out.Replace($csDir, '<ROOT>')
        $cTree = if (Test-Path -LiteralPath $cDir) { (Snapshot-Tree $cDir) } else { '<missing>' }
        $csTree = if (Test-Path -LiteralPath $csDir) { (Snapshot-Tree $csDir) } else { '<missing>' }
        $label = "$name/$step-$op"
        if ($c.Code -ne $cs.Code) {
            $fail++; $scenarioFailed = $true
            $failures.Add("[$label] exit codes C=$($c.Code) C#=$($cs.Code)`n--- C ---`n$cOut--- C# ---`n$csOut")
            continue
        }
        if ($cOut -ne $csOut) {
            $fail++; $scenarioFailed = $true
            $failures.Add("[$label] stdout differs`n--- C ---`n$cOut--- C# ---`n$csOut")
            continue
        }
        if ($cTree -ne $csTree) {
            $fail++; $scenarioFailed = $true
            $failures.Add("[$label] resulting tree differs`n--- C ---`n$cTree`n--- C# ---`n$csTree")
            continue
        }
        if ($cOut -notmatch '(?m)^result=-?\d+$') {
            $fail++; $scenarioFailed = $true
            $failures.Add("[$label] no result= line`n$cOut")
            continue
        }
        $pass++
        $resultLine = ([regex]::Match($cOut, '(?m)^result=(-?\d+)$')).Groups[1].Value
        Write-Host ("  ok  {0,-52} result={1}" -f $label, $resultLine)
        if ($ShowLogs) { ($cOut -split "`n") | Where-Object { $_ -like 'log=*' } | ForEach-Object { Write-Host "        $_" } }
    }
}
# fixtures that exercise the deployed hook bytes: after a deploy, the hook file must equal the payload source
$hookCheckDir = Join-Path $root 'dr\c\renpy_deploy_twice_idempotent\game\iron_deepseek.rpy'
if (Test-Path -LiteralPath $hookCheckDir) {
    $deployed = [IO.File]::ReadAllBytes($hookCheckDir)
    if ([Linq.Enumerable]::SequenceEqual($deployed, $renpyHookBytes)) { $pass++; Write-Host "  ok  deployed Ren'Py hook is byte-identical to payloads\RenPy\iron_deepseek.rpy" }
    else { $fail++; $failures.Add("deployed Ren'Py hook differs from payload source") }
}
$hookCheckJs = Join-Path $root 'dr\c\rpgm_deploy_twice_idempotent\www\js\hook_rpgm_mv.js'
if (Test-Path -LiteralPath $hookCheckJs) {
    $deployed = [IO.File]::ReadAllBytes($hookCheckJs)
    if ([Linq.Enumerable]::SequenceEqual($deployed, $rpgmHookBytes)) { $pass++; Write-Host "  ok  deployed RPGM hook is byte-identical to payloads\RPGMaker\hook_rpgm_mv.js" }
    else { $fail++; $failures.Add("deployed RPGM hook differs from payload source") }
}

# ---------------------------------------------------------------- warmup scan parity
#
# --warmup-and-exit (C) / --warmup (C#): the warmup scanners run in dump mode - no network,
# no cache lookup, no translation-file rewrite - and print every batch body that would have
# been POSTed as "post=<path> <json>". Collected texts, their order, the Ren'Py prevs array,
# the JSON escaping, the mirrored log= lines and result= must all match byte for byte.
# Fixtures are read-only for both launchers, so one copy is shared; a tree snapshot before
# and after guards that dump mode really does not touch the game directory.

$utf8 = New-Object Text.UTF8Encoding $false
function Write-Utf8([string]$path, [string]$text) { Touch $path ($utf8.GetBytes($text)) }
function Write-Utf8Bom([string]$path, [string]$text) { Touch $path ([byte[]](0xEF, 0xBB, 0xBF) + $utf8.GetBytes($text)) }
# Unity serialised string: little-endian u32 length + payload (+ optional alignment padding).
function U32Str([string]$s, [int]$pad = 0) {
    $b = $asciiEnc.GetBytes($s)
    return [byte[]]([BitConverter]::GetBytes([uint32]$b.Length) + $b + (New-Object byte[] $pad))
}
$NUL = [byte[]]@(0)

function New-RenpyGame([string]$d) { Touch "$d\Game.exe"; Mkdir "$d\game" }
function New-RpgmWww([string]$d) {
    Touch "$d\www\index.html"; Touch "$d\www\js\main.js"; Touch "$d\www\js\rpg_core.js"; Touch "$d\Game.exe"
    Write-Utf8 "$d\www\data\System.json" '{"gameTitle":"x"}'
}
function New-UnityGame([string]$d) { Mkdir "$d\MyGame_Data\Managed"; Touch "$d\MyGame.exe"; Touch "$d\UnityPlayer.dll" }

$warmupScenarios = New-Object System.Collections.Specialized.OrderedDictionary

# ---- Ren'Py ----
$warmupScenarios['warm_renpy_dialogue'] = {
    param($d) New-RenpyGame $d
    Write-Utf8 "$d\game\script.rpy" @"
label start:
    e "Hello there, traveler! Welcome to the town."
    "It was a dark and stormy night."
    menu:
        "Go left":
            jump left
        "Go right":
            jump right
    image bg = "bg.png"
    show eileen happy
    define e = Character("Eileen")
    play music "track.ogg"
    e "Emoji test \ud83d\ude00 and lone \ud83d surrogate and \udc00 low."
    "Multiple" "literals on one" "line, all collected"
    "Short"
    "sixteen_or_more_chars"
    'single quoted line here.'
    `$ x = "assigned before quote"
    "Tab\tand\nnewline\rand\\backslash"
    "Unicode escape \u00e9 done."
    "Café au lait — non-ASCII ok"
    "这是中文，不预热"
    "http://example.com/page"
    "image.jpg"
    "unterminated literal
    """triple quoted docstring"""
    e "Ends with \u12"
"@
    Write-Utf8 "$d\game\iron_deepseek.rpy" '"launcher hook literal must be skipped"'
    Write-Utf8 "$d\game\empty.rpy" ''
}
# prevs are per file: the second file's first text must have prev="" again; 17 literals on one
# line hit the 16-literal cap.
$warmupScenarios['warm_renpy_prevs_and_cap'] = {
    param($d) New-RenpyGame $d
    Write-Utf8 "$d\game\a.rpy" "`"First in a.`"`n`"Second in a.`"`n"
    Write-Utf8 "$d\game\b.rpy" "`"First in b.`"`n`"Second in b.`"`n"
    $many = (1..17 | ForEach-Object { "`"literal number $_ ok`"" }) -join ' '
    Write-Utf8 "$d\game\c.rpy" "$many`n"
}
# Recursion: subdirectories are only entered when the parent directory itself has *.rpy
# (FindFirstFileW(*.rpy) failure returns early in the C version). depth-13 is skipped.
$warmupScenarios['warm_renpy_recursion'] = {
    param($d) New-RenpyGame $d
    Write-Utf8 "$d\game\root.rpy" "`"Root level line.`"`n"
    Write-Utf8 "$d\game\tl\chinese\strings.rpy" "`"Nested translation line.`"`n"
    Write-Utf8 "$d\game\tl\deeper\again\x.rpy" "`"Deeper nested line.`"`n"
    Write-Utf8 "$d\game\nosub\deep.rpy" "`"Under nosub, but nosub has rpy so found.`"`n"
    Write-Utf8 "$d\game\onlysub\inner\lost.rpy" "`"Parent has no .rpy, never scanned.`"`n"
    $deep = "$d\game"; for ($i = 1; $i -le 13; $i++) { $deep = "$deep\d$i"; Write-Utf8 "$deep\l.rpy" "`"Depth $i line here.`"`n" }
}
# Oversized script (> 8 MiB) and a NUL byte ending a file early.
$warmupScenarios['warm_renpy_size_and_nul'] = {
    param($d) New-RenpyGame $d
    $big = New-Object byte[] (8 * 1024 * 1024 + 1); Touch "$d\game\big.rpy" $big
    Touch "$d\game\nul.rpy" ($utf8.GetBytes("`"Before the NUL byte.`"`n") + [byte[]](0) + $utf8.GetBytes("`"After the NUL byte.`"`n"))
    Write-Utf8 "$d\game\crlf.rpy" "`"CRLF line one.`"`r`n`"CRLF line two.`"`r`n"
}
$warmupScenarios['warm_renpy_no_game_dir'] = { param($d) Touch "$d\Game.exe"; Touch "$d\game.rpa" }

# ---- RPG Maker MV/MZ ----
$warmupScenarios['warm_rpgm_events_and_db'] = {
    param($d) New-RpgmWww $d
    Write-Utf8 "$d\www\data\Map001.json" @"
{"displayName":"Town Square","name":"EditorLabelNotShown","events":[null,{"id":1,"name":"EV001","pages":[{"list":[
{"code":101,"indent":0,"parameters":["Actor1",0,0,2,"Harold"]},
{"code":401,"indent":0,"parameters":["\\C[2]Harold\\C[0]: Welcome to our village."]},
{"code":401,"indent":0,"parameters":["Please make yourself at home."]},
{"code":102,"indent":0,"parameters":[["Yes, thank you.","No, I must go."],1,0,2,0]},
{"code":405,"indent":0,"parameters":["Scrolling epilogue text line."]},
{"code":108,"indent":0,"parameters":["A comment, not shown"]},
{"code":101,"indent":0,"parameters":["Actor1",0,0,2]},
{"code":401,"indent":0,"parameters":["Single line message."]},
{"code":0,"indent":0,"parameters":[]}
]}]}]}
"@
    Write-Utf8 "$d\www\data\Actors.json" '[null,{"id":1,"name":"Harold","nickname":"The Brave","profile":"A young knight.\nHe likes bread.","note":"<skip:me>"},{"id":2,"name":"\\N[1] jr.","nickname":""}]'
    Write-Utf8 "$d\www\data\Items.json" '[null,{"id":1,"name":"Potion","description":"Restores 100 HP.\\I[64] Nice.","note":""},{"id":2,"name":"C:\\Users\\bad\\path","description":"\\FS[24]Big text \\{here\\}"}]'
    Write-Utf8 "$d\www\data\Skills.json" '[null,{"id":1,"name":"Fire","message1":" casts %1!","message2":"","description":"Burn \\C[20]them\\C[0] all"}]'
    Write-Utf8 "$d\www\data\System.json" '{"gameTitle":"Test Game","currencyUnit":"G","elements":["","Physical","Fire","Ice"],"equipTypes":["","Weapon","Shield"],"terms":{"basic":["Level","Lv","HP","HP","MP","MP"],"commands":["Fight","Escape","Attack",null,"Guard"],"params":["Max HP","Max MP"],"messages":{"actionFailure":"There was no effect on %1!","alwaysDash":"Always Dash"}},"skillTypes":["","Magic","Special"]}'
    Write-Utf8 "$d\www\data\MapInfos.json" '[null,{"id":1,"expanded":false,"name":"Town Square Map","order":1,"parentId":0,"scrollX":0,"scrollY":0}]'
    Write-Utf8Bom "$d\www\data\Weapons.json" '[null,{"id":1,"name":"Bronze Sword","description":"A sword made of bronze. \u00e9\ud83d\ude00"}]'
}
# Speaker-prefix bodies (\n<Name>, \pop, <tag>) are collected twice: with and without prefix.
$warmupScenarios['warm_rpgm_prefix_codes'] = {
    param($d) New-RpgmWww $d
    Write-Utf8 "$d\www\data\Map002.json" @"
{"events":[null,{"pages":[{"list":[
{"code":101,"parameters":["",0,0,2]},
{"code":401,"parameters":["\\n<Harold>Good morning, everyone."]},
{"code":101,"parameters":["",0,0,2]},
{"code":401,"parameters":["\\nd1[Boss] Prepare to fight!"]},
{"code":101,"parameters":["",0,0,2]},
{"code":401,"parameters":["<Marsha>: I have heard rumours."]},
{"code":101,"parameters":["",0,0,2]},
{"code":401,"parameters":["\\POP[3]<Sage>Ancient wisdom follows."]},
{"code":101,"parameters":["",0,0,2]},
{"code":401,"parameters":["\\XX[1]Unknown code rejects the line."]},
{"code":101,"parameters":["",0,0,2]},
{"code":401,"parameters":["Trailing backslash rejected\\"]},
{"code":101,"parameters":["",0,0,2]},
{"code":401,"parameters":["Gold: \\G and \\\\ escaped and \\$ ok"]},
{"code":101,"parameters":["",0,0,2]},
{"code":401,"parameters":["\\V[1] variable and \\P[2] party"]}
]}]}]}
"@
}
# Plugin external texts: Recipes.txt, quest/Quests.txt (Galv), CSV with header detection and
# without, delimiter detection (; , tab), quoted fields with embedded newlines, skipped asset dirs.
$warmupScenarios['warm_rpgm_external_text'] = {
    param($d) New-RpgmWww $d
    Write-Utf8Bom "$d\www\data\Recipes.txt" "# comment line`nIron Sword Recipe`n<recipe 1>`n  Requires two iron bars.  `n</recipe>`nC:\\bad\\path`n"
    Write-Utf8 "$d\www\quest\Quests.txt" "<quest 1:Find the Lost Cat|Easy|Side>`nThe cat was last seen near the well.`n<quest 2:Main Story - Chapter One - The Beginning|Hard>`n<quest 3:NoPipeTitle>`n<quest 4 missing colon>`n</quest>`n"
    Write-Utf8 "$d\www\img\meta.txt" "Image metadata must be skipped."
    Write-Utf8 "$d\www\js\plugins\notes.txt" "JavaScript folder skipped too."
    Write-Utf8 "$d\www\game_messages.csv" "id,English,Japanese`nmsg_001,`"Hello, world!`",こんにちは`nmsg_002,`"Line one`nline two`",テスト`nmsg_003,`"Quoted `"`"inner`"`" text`",引用`n"
    Write-Utf8 "$d\www\semicolon.csv" "key;en_US;de`nGREET;Good day to you.;Guten Tag`n"
    Write-Utf8 "$d\www\tab.csv" "id`tsource`ttarget`n1`tTab separated source text.`tX`n"
    Write-Utf8 "$d\www\noheader.csv" "npc_01,Hello adventurer, welcome!,other words here`nitem_99,short_id,Another full sentence.`n"
    Write-Utf8 "$d\www\nodelim.csv" "just one column no delimiter`n"
}
# Flat MZ layout (no www/), CRLF text file, oversized json skipped, NUL in json ends parse,
# trailing whitespace after the closing bracket (pins the C end-of-buffer fix of 2026-09-09).
$warmupScenarios['warm_rpgm_flat_and_limits'] = {
    param($d)
    Touch "$d\index.html"; Touch "$d\js\main.js"; Touch "$d\js\rmmz_core.js"; Touch "$d\Game.exe"
    Write-Utf8 "$d\data\System.json" '{"gameTitle":"Flat MZ Game"}'
    Write-Utf8 "$d\data\Armors.json" "[null,{`"id`":1,`"name`":`"Leather Armor`",`"description`":`"Light and flexible.   `"}]`r`n   `n`t"
    Touch "$d\data\Classes.json" ($utf8.GetBytes('[null,{"id":1,"name":"Before NUL Class"},') + [byte[]](0) + $utf8.GetBytes('{"id":2,"name":"After NUL Class"}]'))
    Touch "$d\data\Huge.json" (New-Object byte[] (64 * 1024 * 1024 + 1))
    Write-Utf8 "$d\data\Lines.txt" "Windows line one.`r`nWindows line two.`r`n`r`nLast without newline"
}
$warmupScenarios['warm_rpgm_no_content_root'] = { param($d) Touch "$d\Game.exe"; Touch "$d\www\index.html" }

# ---- Unity (XUnity translation files + *_Data assets/bundles) ----
$warmupScenarios['warm_unity_xunity_and_assets'] = {
    param($d) New-UnityGame $d
    Write-Utf8Bom "$d\BepInEx\Translation\zh-CN\Text\_AutoGeneratedTranslations.txt" @"
// comment line
# another comment
; and another
Hello adventurer=你好，冒险者
Identity entry=Identity entry
Empty value entry=
Escaped \= equals and \\ backslash=转义
Multi\nline\ttext=多行
Already Chinese 你好=X
=no key
noequals line
Short=x
"@
    Write-Utf8 "$d\BepInEx\Translation\zh-CN\Text\_Substitutions.txt" "Skipped=跳过"
    Write-Utf8 "$d\Translation\zh-CN\Text\extra.txt" "Second directory entry=第二"
    Write-Utf8 "$d\AutoTranslator\Translation\zh-CN\Text\third.txt" "Hello adventurer=重复的键不再导入`nThird directory entry=第三"
    # resources.assets: u32-prefixed ASCII strings amid binary noise
    $assets = [byte[]]@(0, 1, 2, 3, 0xFF, 0xFE) + (U32Str "Welcome to the dungeon, hero." 3) + (U32Str "Base Layer" 2) + (U32Str "m_Material" 2) +
              (U32Str "<color=#ff0000>Danger!</color> Ahead <quest>lies</quest> the beast." 1) + (U32Str "Untitled (Clone)") + (U32Str "_internalName") +
              (U32Str "x") + (U32Str "NoPunctuationHere") + (U32Str "Tab`tand newline`nallowed inside.") + (New-Object byte[] 9)
    Touch "$d\MyGame_Data\resources.assets" $assets
    Touch "$d\MyGame_Data\sharedassets0.assets" ((U32Str "Shared asset dialogue line, yes.") + (New-Object byte[] 12))
    Touch "$d\MyGame_Data\level0" ((U32Str "Level zero says hi!") + (New-Object byte[] 12))
    Touch "$d\MyGame_Data\level0x" ((U32Str "Not a level file, ignored.") + (New-Object byte[] 12))
    Touch "$d\MyGame_Data\globalgamemanagers" ((U32Str "Global managers text here.") + (New-Object byte[] 12))
    Touch "$d\MyGame_Data\other.dat" ((U32Str "Unknown extension, ignored.") + (New-Object byte[] 12))
    # AssetBundle: printable runs separated by binary bytes; serialiser residue variants
    $bundle = [byte[]]@(0x55, 0x6E, 0x69, 0x74, 0x79, 0x46, 0x53, 0x00) +
              $asciiEnc.GetBytes("[speaker:Harold] Good morning, my friend.0") + [byte[]]@(0, 1) +
              $asciiEnc.GetBytes("- Collect five herbs2") + $NUL +
              $asciiEnc.GetBytes("What do you mean?(") + $NUL +
              $asciiEnc.GetBytes("lowercase start rejected.") + $NUL +
              $asciiEnc.GetBytes("ALLCAPS SHOUTING TEXT.") + $NUL +
              $asciiEnc.GetBytes("MixedCase Word rejected here.") + $NUL +
              $asciiEnc.GetBytes("First line here.`r`nSecond line here.") + $NUL +
              $asciiEnc.GetBytes("<b>Bold</b> rich text, with tags.") + $NUL +
              $asciiEnc.GetBytes("ab") + $NUL +
              $asciiEnc.GetBytes(("Run " + ("x" * 1300) + " overflow.")) + $NUL +
              $asciiEnc.GetBytes("Tail run without terminator.")
    Touch "$d\MyGame_Data\StreamingAssets.unity3d" $bundle
    Touch "$d\MyGame_Data\pack.bundle" ($asciiEnc.GetBytes("Bundle extension text works.") + (New-Object byte[] 4))
    Touch "$d\MyGame_Data\pack.assetbundle" ($asciiEnc.GetBytes("Assetbundle extension text works.") + (New-Object byte[] 4))
    Touch "$d\MyGame_Data\empty.bundle" (New-Object byte[] 0)
    Touch "$d\MyGame_Data\Managed\Assembly-CSharp.dll" ((U32Str "Managed subdir is not scanned.") + (New-Object byte[] 12))
}
$warmupScenarios['warm_unity_il2cpp_second_data_dir'] = {
    param($d) Mkdir "$d\MyGame_Data\il2cpp_data\Metadata"; Touch "$d\MyGame.exe"; Touch "$d\GameAssembly.dll"
    Touch "$d\MyGame_Data\resources.assets" ((U32Str "IL2CPP game resources text.") + (New-Object byte[] 12))
    Touch "$d\Other_Data\resources.assets" ((U32Str "Second data folder text.") + (New-Object byte[] 12))
    Touch "$d\Huge_Data\resources.assets" (New-Object byte[] (64 * 1024 * 1024 + 1))
}
$warmupScenarios['warm_unity_nothing_to_do'] = { param($d) New-UnityGame $d }

# ---- Godot (project/export tree, .translation, .pck formats 1/2/3, embedded pck EXE) ----
# Godot PCK builder. $entries: hashtables @{ Path; Data = [byte[]]; Pad; OffsetOverride; SizeOverride }.
# Pad -> NUL-pad the path to a 4-byte multiple (as Godot does). Format 1 stores absolute offsets
# (+$absBase when the pack lives inside an EXE); 2 stores file_base-relative offsets with the
# directory after the header; 3 puts the directory (u32 count + entries) at directory_offset.
function New-GodotPck([int]$format, [object[]]$entries, [long]$absBase = 0, [int]$gap = 0, [int]$minor = 4) {
    $metaSize = 36; $headerSize = 112
    if ($format -eq 1) { $metaSize = 32; $headerSize = 88 } elseif ($format -eq 2) { $headerSize = 100 }
    $paths = New-Object 'System.Collections.Generic.List[byte[]]'
    $dirSize = 0; $totalData = 0
    foreach ($e in $entries) {
        $pb = $utf8.GetBytes([string]$e.Path)
        if ($e.Pad) { $padTo = [int]([Math]::Ceiling(($pb.Length + 1) / 4.0) * 4); $pb = [byte[]]($pb + (New-Object byte[] ($padTo - $pb.Length))) }
        $paths.Add($pb); $dirSize += 4 + $pb.Length + $metaSize; $totalData += ([byte[]]$e.Data).Length
    }
    if ($format -eq 3) { $dataStart = $headerSize; $dirStart = $headerSize + $totalData + $gap; $fileBase = $headerSize; $total = $dirStart + 4 + $dirSize }
    else { $dirStart = $headerSize; $dataStart = $headerSize + $dirSize + $gap; $fileBase = $dataStart; $total = $dataStart + $totalData }
    $buf = New-Object byte[] $total
    $asciiEnc.GetBytes('GDPC').CopyTo($buf, 0)
    [BitConverter]::GetBytes([uint32]$format).CopyTo($buf, 4)
    $major = if ($format -eq 1) { 3 } else { 4 }
    [BitConverter]::GetBytes([uint32]$major).CopyTo($buf, 8)
    [BitConverter]::GetBytes([uint32]$minor).CopyTo($buf, 12)
    if ($format -eq 1) { [BitConverter]::GetBytes([uint32]$entries.Count).CopyTo($buf, 84) }
    else {
        [BitConverter]::GetBytes([uint64]$fileBase).CopyTo($buf, 24)
        if ($format -eq 2) { [BitConverter]::GetBytes([uint32]$entries.Count).CopyTo($buf, 96) }
        else { [BitConverter]::GetBytes([uint64]$dirStart).CopyTo($buf, 32); [BitConverter]::GetBytes([uint32]$entries.Count).CopyTo($buf, $dirStart) }
    }
    $dpos = if ($format -eq 3) { $dirStart + 4 } else { $dirStart }
    $running = 0
    for ($i = 0; $i -lt $entries.Count; $i++) {
        $e = $entries[$i]; $pb = $paths[$i]; $data = [byte[]]$e.Data
        [BitConverter]::GetBytes([uint32]$pb.Length).CopyTo($buf, $dpos); $dpos += 4
        $pb.CopyTo($buf, $dpos); $dpos += $pb.Length
        $off = if ($format -eq 1) { [uint64]($absBase + $dataStart + $running) } else { [uint64]$running }
        if ($null -ne $e.OffsetOverride) { $off = [uint64]$e.OffsetOverride }
        $size = [uint64]$data.Length
        if ($null -ne $e.SizeOverride) { $size = [uint64]$e.SizeOverride }
        [BitConverter]::GetBytes($off).CopyTo($buf, $dpos); $dpos += 8
        [BitConverter]::GetBytes($size).CopyTo($buf, $dpos); $dpos += 8
        $dpos += 16                              # md5 (zeros)
        if ($format -ne 1) { $dpos += 4 }        # flags
        $data.CopyTo($buf, $dataStart + $running); $running += $data.Length
    }
    return $buf
}
# PE with a decoy .text section and a "pck" section holding a pack (Godot 4 embedded layout:
# pack bytes, u64 pack size, GDPC tail). Format 1 offsets are made absolute to the EXE.
function New-GodotEmbeddedExe([int]$format, [object[]]$entries, [bool]$tail) {
    $dataOff = 168                               # 64 + 4 + 20 + 2 * 40
    $pck = New-GodotPck $format $entries $dataOff
    $dos = New-Object byte[] 64; $dos[0] = 0x4D; $dos[1] = 0x5A; [BitConverter]::GetBytes([int32]64).CopyTo($dos, 60)
    $sig = [byte[]](0x50, 0x45, 0x00, 0x00)
    $fh = New-Object byte[] 20
    [BitConverter]::GetBytes([uint16]0x8664).CopyTo($fh, 0); [BitConverter]::GetBytes([uint16]2).CopyTo($fh, 2); [BitConverter]::GetBytes([uint16]0).CopyTo($fh, 16)
    $sh1 = New-Object byte[] 40; $asciiEnc.GetBytes('.text').CopyTo($sh1, 0)
    [BitConverter]::GetBytes([uint32]16).CopyTo($sh1, 16); [BitConverter]::GetBytes([uint32]$dataOff).CopyTo($sh1, 20)
    $sh2 = New-Object byte[] 40; $asciiEnc.GetBytes('pck').CopyTo($sh2, 0)
    [BitConverter]::GetBytes([uint32]$pck.Length).CopyTo($sh2, 16); [BitConverter]::GetBytes([uint32]$dataOff).CopyTo($sh2, 20)
    $sizeBytes = [BitConverter]::GetBytes([uint64]$pck.Length)
    $gd = if ($tail) { [byte[]](0x47, 0x44, 0x50, 0x43) } else { [byte[]](0, 0, 0, 0) }
    return [byte[]]($dos + $sig + $fh + $sh1 + $sh2 + $pck + $sizeBytes + $gd)
}
function GodotEntry([string]$path, [byte[]]$data, [bool]$pad = $true) { return @{ Path = $path; Data = $data; Pad = $pad } }
function NulJoin([string[]]$parts) {
    $out = New-Object 'System.Collections.Generic.List[byte]'
    foreach ($p in $parts) { $out.AddRange($utf8.GetBytes($p)); $out.Add(0) }
    return [byte[]]$out.ToArray()
}

# Text resources: quote classification (translatable keys, tr()/translate()/N_()/RTR(), metadata
# keys, [section] heads), free-string rules, plain-line fallback, skip dirs, generated files, depth.
$warmupScenarios['warm_godot_project_text'] = {
    param($d)
    Write-Utf8 "$d\project.godot" @'
; Engine configuration file.
config_version=5

[application]

config/name="Legend of the Parity Test"
config/description="A long description of the game, for testing."
run/main_scene="res://scenes/main.tscn"

[display]
window/size/viewport_width=1152
'@
    Write-Utf8 "$d\scenes\main.tscn" @'
[gd_scene load_steps=3 format=3 uid="uid://abc123"]

[ext_resource type="Script" path="res://scripts/main.gd" id="1_x"]

[node name="Main" type="Control"]

[node name="StartButton" type="Button" parent="."]
text = "Start Game"
tooltip_text = "Begin your adventure now."
hint_tooltip = 'Legacy tooltip, single quoted.'

[node name="Title" type="Label" parent="."]
text = "Legend of the Parity Test"
TEXT = "Upper case key still counts"
theme_override_fonts/font = ExtResource("2_font")

[node name="Rich" type="RichTextLabel" parent="."]
bbcode_text = "[b]Bold[/b] welcome, hero!"
placeholder_text = "Type here"
window_title="No spaces around equals"
script = ExtResource("1_x")
metadata/_custom = "hidden metadata"
   text   =   "Padded key and value"
[connection signal="pressed" from="StartButton" to="." method="_on_start"]
'@
    Write-Utf8 "$d\scripts\main.gd" @'
extends Control

const GREETING := "Welcome back, commander."
var short_id = "btn_ok"
var label_text = 'Single quoted sentence here.'

func _ready():
    $Title.text = tr("Options")
    print("debug marker")
    push_warning("Some warning text with spaces")
    var s = TranslationServer.translate("Continue")
    var n = N_("Load")
    var r = RTR("Quit")
    var t = tr ( "Spaced call" )
    var no = mytr("Notatr")
    var d = {"key": "value with spaces here"}
    # don't treat this apostrophe as a string start
    """
    Docstring-ish triple quote
    """
    var multi = "unterminated string
    var esc = "Line\nbreak and \"quoted\" \u00e9"
    dialog_text = "Are you sure?"
    emit_signal("my_signal")
    $Label.text = "Text %s" % name
    var path = "res://icons/a.png"
    var node = "NodePath value here"
    var resource = "resource word first"
    var thirtythree = "abcdefghijklmnopqrstuvwxyzabcdefg"
    var cjk = "这是中文文本"
'@
    Write-Utf8 "$d\dialogue\intro.txt" @'
Welcome to the arena!
# comment line
; ini comment
[section]
key=value
short
"quoted line stays for quoted scanner"
Eighteen+ chars no punct here
msgid "po directive"
msgstr "translation"
Plain line with period.
'@
    Write-Utf8 "$d\data\items.json" '{"name":"Iron Sword","description":"A sturdy blade, forged in the north.","icon":"res://icons/sword.png","id":"itm_001"}'
    Write-Utf8 "$d\i18n\ui.tres" "[gd_resource type=`"Theme`" format=3]`n`n[resource]`nresource_name = `"MainTheme`"`ndefault_font_size = 16`n"
    Write-Utf8Bom "$d\notes\bom.txt" "BOM line collected fine.`r`nSecond BOM line, also fine.`r`n"
    Touch "$d\nul.txt" ($utf8.GetBytes("Before NUL in godot text.`n") + [byte[]](0) + $utf8.GetBytes("After NUL never seen.`n"))
    Write-Utf8 "$d\.godot\imported\cache.tscn" "text = `"Import cache text skipped`""
    Write-Utf8 "$d\.import\x.tscn" "text = `"Dot import text skipped`""
    Write-Utf8 "$d\addons\plugin\plugin.gd" "var x = tr(`"Addon text skipped`")"
    Write-Utf8 "$d\saves\slot1.txt" "Save slot text skipped, yes."
    Write-Utf8 "$d\save\slot1.txt" "Singular save text skipped, yes."
    Write-Utf8 "$d\export_presets\preset.txt" "Export preset text skipped, yes."
    Write-Utf8 "$d\dst_godot_runtime.gd" "var x = tr(`"Launcher runtime script skipped`")"
    Touch "$d\dst_godot_patch.pck" ($utf8.GetBytes("Launcher patch pack text skipped."))
    Touch "$d\dst_godot_patch.next.pck" ($utf8.GetBytes("Launcher next patch pack text skipped."))
    Write-Utf8 "$d\dst_godot_patch.building" "Building marker text skipped."
    $deep = $d; for ($i = 1; $i -le 11; $i++) { $deep = "$deep\d$i"; Write-Utf8 "$deep\l.txt" "Depth $i line here, ok." }
}
# PO (msgid/msgid_plural/continuations, msgstr ignored), Godot translation CSV (source column by
# header), unknown-header CSV fallback (short ids skipped), Markdown dialogue scripts.
$warmupScenarios['warm_godot_po_csv_md'] = {
    param($d)
    Write-Utf8 "$d\project.godot" "config_version=5`n"
    Write-Utf8 "$d\locale\en.po" @'
# translator comment
msgid ""
msgstr ""
"Content-Type: text/plain; charset=UTF-8\n"

#: scenes/main.tscn
msgid "Hello, world of PO files."
msgstr "你好"

msgctxt "menu"
msgid "Save game"
msgstr ""

msgid ""
"Multi-line msgid part one, "
"and part two continues here."
msgstr ""

msgid "Apples"
msgid_plural "Many apples, plural form."
msgstr[0] ""
msgstr[1] ""

msgid "Escaped \"quotes\" and \\n literal"
msgstr "x"
msgid_x "not a msgid marker but text[5] is underscore"
msgid	"Tab separated msgid works."
msgid "Unterminated msgid line
msgid "Last entry flushed at EOF."
'@
    Write-Utf8Bom "$d\locale\strings.csv" @'
keys,en,zh_CN,fr
MENU_START,"Start","开始","Démarrer"
MENU_QUIT,Quit,退出,Quitter
DLG_1,"He said ""hello"", then left.",x,y
DLG_2,  "Leading spaces before quote"  ,x,y
# comment row
EMPTY,,,
SHORTROW
LONG,"Sentence with, a comma inside.",a,b
'@
    Write-Utf8 "$d\locale\unknown.csv" @'
id,name_x,value
item_01,Rusty Dagger,12
npc_bob,Bob,hp
q_1,"A quest to remember, always.",1
plain,Plain,text
'@
    Write-Utf8 "$d\locale\english_col.csv" "term,English,Japanese`nT1,Prefer English header column.,日本`n"
    Write-Utf8 "$d\locale\en_gb.csv" "id,en-GB,de`nX,British source column wins.,Deutsch`n"
    Write-Utf8 "$d\locale\source_text.csv" "id,text,source_text`nX,lower score column,Higher score column wins here.`n"
    Write-Utf8 "$d\story\chapter1.md" @'
# Chapter One: The Beginning
##   Sub heading here
Some narration line, with punctuation.
> Speaker: quoted line skipped
---
***
___
[center]Centered visible text[/center]
[window_opens]
[b]Bold[/b] and [i]italic[/i] mixed line
```code
inside fence, skipped entirely.
```
After the fence, back to normal.
~~~
tilde fence skipped too
~~~
Final line
[unclosed bracket line
[/close]Only closing tag then text[/close]
#
'@
}
# Standalone PCK format 1: NUL-padded paths, text/translation entries first, compiled resources in
# the second pass, locale suffix rules on .translation, out-of-range/oversized entries skipped.
$warmupScenarios['warm_godot_pck_v1'] = {
    param($d)
    Touch "$d\game.exe"
    $tscn = $utf8.GetBytes("[node name=`"Menu`" type=`"Control`"]`ntext = `"Pack menu start`"`nvar free = `"Free pack string, yes.`"`n")
    $md = $utf8.GetBytes("# Pack heading`nPack markdown body line.`n")
    $enTr = NulJoin @('RSRC', 'OptimizedTranslation', 'messages', 'Pack translation entry one.', 'Go', 'Start', 'bin', 'Play', "Tabbed`tentry here",
                      'Café non-ascii entry', 'テキストです', 'これは日本語の文章です', '[color=red]Danger ahead![/color] Careful now.', '[b]Only tag[/b]',
                      'Pack translation entry one.', 'trailing tag text [i]', '_underscore first', '{brace first', 'node name value', 'res://path/in/text')
    $enTr = [byte[]]($enTr + [byte[]](0xFF, 0xFE) + $utf8.GetBytes('invalid prefix') + [byte[]](0) +
                     $utf8.GetBytes('overlong ') + [byte[]](0xC0, 0x80) + [byte[]](0) +
                     $utf8.GetBytes('surrogate ') + [byte[]](0xED, 0xA0, 0x80) + [byte[]](0) +
                     $utf8.GetBytes('truncated ') + [byte[]](0xE6, 0x97) + [byte[]](0) +
                     $utf8.GetBytes('four byte ok ') + [byte[]](0xF0, 0x9F, 0x98, 0x80) + [byte[]](0) +
                     $utf8.GetBytes('Unterminated pack tail entry'))
    $entries = @(
        (GodotEntry 'res://compiled/scene.scn' (NulJoin @('Deferred scene text entry.'))),
        (GodotEntry 'res://scripts/x.gdc' (NulJoin @('Compiled script string here.')) $false),
        (GodotEntry 'res://scenes/menu.tscn' $tscn),
        (GodotEntry 'res://story/intro.md' $md $false),
        (GodotEntry 'res://locale/ui.en.translation' $enTr),
        (GodotEntry 'res://locale/ui.zh.translation' (NulJoin @('Chinese pack translation skipped.'))),
        (GodotEntry 'res://locale/ui.en_US.translation' (NulJoin @('US english translation entry.'))),
        (GodotEntry 'res://locale/ui.english.translation' (NulJoin @('English word locale rejected entry.'))),
        (GodotEntry 'res://locale/plain.translation' (NulJoin @('Plain translation entry text.'))),
        (GodotEntry 'res://locale/UI.EN-GB.TRANSLATION' (NulJoin @('Upper case suffix entry text.'))),
        (GodotEntry 'res://img/a.png' (NulJoin @('PNG ignored text entry.'))),
        (GodotEntry 'res://data/notes.txt' $utf8.GetBytes("Pack notes free line here.`n")),
        (GodotEntry 'res://data/table.csv' $utf8.GetBytes("key,source`nA,`"Pack CSV source cell.`"`n")),
        (GodotEntry 'res://locale/pack.po' $utf8.GetBytes("msgid `"Pack PO entry.`"`nmsgstr `"`"`n")),
        (GodotEntry 'project.godot' $utf8.GetBytes("config/description=`"Pack project description.`"`n")),
        (GodotEntry 'res://project.binary' (NulJoin @('Binary project not matched by name.'))),
        (GodotEntry 'godot_project.binary' (NulJoin @('Godot project binary entry text.'))),
        (GodotEntry 'res://empty.txt' (New-Object byte[] 0)),
        (GodotEntry 'res://data/exe_base.res' (NulJoin @('Resource extension entry text.')))
    )
    $entries += @{ Path = 'res://data/out_of_range.txt'; Data = $utf8.GetBytes("Out of range entry skipped.`n"); Pad = $true; OffsetOverride = 0x7FFFFFFF }
    $entries += @{ Path = 'res://data/too_big.txt'; Data = $utf8.GetBytes("Oversized entry skipped.`n"); Pad = $true; SizeOverride = 0x10000000 }
    Touch "$d\game.pck" (New-GodotPck 1 $entries)
}
# Format 2 (relative offsets, gap between directory and data), format 3 (directory at the end),
# broken/foreign packs falling back to raw byte scanning, root .translation and godot_project.binary.
$warmupScenarios['warm_godot_pck_v2_v3_fallback'] = {
    param($d)
    Touch "$d\game.exe"
    $v2 = @(
        (GodotEntry 'res://ui/v2.tscn' $utf8.GetBytes("text = `"Version two pack text`"`n")),
        (GodotEntry 'res://ui/v2.gde' (NulJoin @('Version two deferred script text.')) $false),
        (GodotEntry 'res://ui/v2.en.translation' (NulJoin @('Version two translation entry.')))
    )
    Touch "$d\a_v2.pck" (New-GodotPck 2 $v2 0 16)
    $v3 = @(
        (GodotEntry 'res://ui/v3.tscn' $utf8.GetBytes("tooltip_text = `"Version three pack text`"`n")),
        (GodotEntry 'res://ui/v3.res' (NulJoin @('Version three deferred resource text.'))),
        (GodotEntry 'res://ui/v3.po' $utf8.GetBytes("msgid `"Version three PO entry.`"`n"))
    )
    Touch "$d\b_v3.pck" (New-GodotPck 3 $v3 0 8)
    # GDPC header, format 1, one entry whose path length is 0 -> directory parse fails -> raw scan
    $broken = New-Object byte[] 88; $asciiEnc.GetBytes('GDPC').CopyTo($broken, 0); [BitConverter]::GetBytes([uint32]1).CopyTo($broken, 4); [BitConverter]::GetBytes([uint32]1).CopyTo($broken, 84)
    Touch "$d\c_broken.pck" ([byte[]]($broken + (New-Object byte[] 4) + $utf8.GetBytes('Fallback raw scan sentence.') + [byte[]](0)))
    Touch "$d\d_wrongmagic.pck" ([byte[]]($asciiEnc.GetBytes('XXXX') + (New-Object byte[] 90) + $utf8.GetBytes('Not a pack but scanned raw, yes.') + [byte[]](0)))
    Touch "$d\e_tiny.pck" ($utf8.GetBytes('Tiny pack.'))
    $v9 = New-Object byte[] 112; $asciiEnc.GetBytes('GDPC').CopyTo($v9, 0); [BitConverter]::GetBytes([uint32]9).CopyTo($v9, 4)
    Touch "$d\f_unknown_format.pck" ([byte[]]($v9 + $utf8.GetBytes('Unknown format raw text here.') + [byte[]](0)))
    Touch "$d\g_empty.pck" (New-Object byte[] 0)
    # format 2 with file_count beyond the limit -> invalid -> raw scan of the whole file
    $many = New-Object byte[] 100; $asciiEnc.GetBytes('GDPC').CopyTo($many, 0); [BitConverter]::GetBytes([uint32]2).CopyTo($many, 4); [BitConverter]::GetBytes([uint32]200001).CopyTo($many, 96)
    Touch "$d\h_too_many.pck" ([byte[]]($many + $utf8.GetBytes('Too many files raw text here.') + [byte[]](0)))
    Touch "$d\root.translation" (NulJoin @('Root translation entry text.', 'Root', 'Ab'))
    Touch "$d\zh.translation" (NulJoin @('Root zh translation is still scanned by file name.'))
    Touch "$d\godot_project.binary" (NulJoin @('Binary project setting text.', 'ProjectSettings', 'application/config/name'))
    Touch "$d\huge.translation" (New-Object byte[] (64 * 1024 * 1024 + 1))
}
# Embedded packs: PE "pck" section with a format 2 pack (GDPC tail also drives detection), a
# format 1 pack with EXE-absolute offsets, a PE without pck section, and a non-PE .exe.
$warmupScenarios['warm_godot_embedded_exe'] = {
    param($d)
    $v2 = @(
        (GodotEntry 'res://ui/embedded.tscn' $utf8.GetBytes("text = `"Embedded pack text`"`n")),
        (GodotEntry 'res://ui/embedded.en.translation' (NulJoin @('Embedded translation entry.')))
    )
    Touch "$d\game.exe" (New-GodotEmbeddedExe 2 $v2 $true)
    $v1 = @(
        (GodotEntry 'res://ui/legacy.tscn' $utf8.GetBytes("text = `"Legacy embedded pack text`"`n")),
        (GodotEntry 'res://ui/legacy.scn' (NulJoin @('Legacy embedded deferred text.')))
    )
    Touch "$d\game_v1.exe" (New-GodotEmbeddedExe 1 $v1 $false)
    Touch "$d\tool.exe" (New-PckPeBytes $false $false)
    Touch "$d\notpe.exe" ($utf8.GetBytes('Not a PE file at all, really.'))
    Touch "$d\UnityCrashHandler64.exe" (New-GodotEmbeddedExe 2 $v2 $true)
}
# 12000-item cap (24 batches), oversized text file skipped, cap stops directory walk.
$warmupScenarios['warm_godot_cap'] = {
    param($d)
    Write-Utf8 "$d\project.godot" "config_version=5`n"
    $sb = New-Object Text.StringBuilder
    for ($i = 1; $i -le 12050; $i++) { [void]$sb.Append("Capacity line number $i, filler text.`n") }
    Write-Utf8 "$d\big.txt" $sb.ToString()
    Touch "$d\huge.txt" (New-Object byte[] (64 * 1024 * 1024 + 1))
    Write-Utf8 "$d\zzz_after_cap.txt" "Never reached after the cap, sorry.`n"
}
$warmupScenarios['warm_godot_nothing_to_do'] = { param($d) Write-Utf8 "$d\project.godot" "config_version=5`n" }

# ---- engines without a warmup path ----
$warmupScenarios['warm_rpgm_legacy_noop'] = { param($d) Touch "$d\Data\Scripts.rvdata2"; Touch "$d\Game.exe" }
$warmupScenarios['warm_unknown_noop'] = { param($d) Touch "$d\readme.txt" }

foreach ($name in $warmupScenarios.Keys) {
    $d = Join-Path $root "ws\$name"
    Mkdir $d
    & $warmupScenarios[$name] $d
    $before = Snapshot-Tree $d
    $c = Invoke-Detect $CLauncher @('--warmup-and-exit', $d)
    $cs = Invoke-Detect $CsLauncher @('--warmup', $d)
    $after = Snapshot-Tree $d
    $label = "warmup/$name"
    if ($c.Code -ne $cs.Code) {
        $fail++; $failures.Add("[$label] exit codes C=$($c.Code) C#=$($cs.Code)`n--- C ---`n$($c.Out)$($c.Err)--- C# ---`n$($cs.Out)$($cs.Err)")
        continue
    }
    if ($c.Out -ne $cs.Out) {
        $fail++; $failures.Add("[$label] stdout differs`n--- C ---`n$($c.Out)--- C# ---`n$($cs.Out)")
        continue
    }
    if ($c.Out -notmatch '(?m)^result=0$') {
        $fail++; $failures.Add("[$label] no result=0 line`n$($c.Out)")
        continue
    }
    if ($before -ne $after) {
        $fail++; $failures.Add("[$label] dump mode modified the game directory`n--- before ---`n$before`n--- after ---`n$after")
        continue
    }
    $pass++
    $posts = @($c.Out -split "`n" | Where-Object { $_ -like 'post=*' })
    $texts = 0
    foreach ($p in $posts) { $texts += ([regex]::Matches($p, '(?<!\\)"(?:[^"\\]|\\.)*"')).Count }
    Write-Host ("  ok  {0,-52} batches={1} strings={2}" -f $label, $posts.Count, $texts)
    if ($ShowLogs) { ($c.Out -split "`n") | Where-Object { $_ -like 'log=*' -or $_ -like 'post=*' } | ForEach-Object { Write-Host "        $_" } }
}

# ---------------------------------------------------------------- embedded payload sync parity
#
# --sync-payloads-and-exit (C) / --sync-payloads --root (C#): both launchers release their
# embedded first-party payloads (dst_server.exe, dst_server_cs.exe, installer script, ini
# examples, optional Unity assemblies) into an empty root. The released trees must be
# byte-identical, and stdout (log= lines + result=) and exit codes equal (0, or 5 when a
# required component is missing or could not be written). Then: a second run is a no-op on
# both sides (no target rewritten, no sync log), a tampered target is restored, a foreign
# file in the root is left alone, and a blocked target is reported as a failure (exit 5).
$syncC = Join-Path $root 'sync_c'; Mkdir $syncC
$syncCs = Join-Path $root 'sync_cs'; Mkdir $syncCs
$syncExeName = Split-Path -Leaf $CLauncher
Copy-Item -LiteralPath $CLauncher -Destination (Join-Path $syncC $syncExeName)
function Snapshot-SyncTree([string]$dir) {
    # the copied C launcher itself is not a payload; drop it from the comparison
    return (((Snapshot-Tree $dir) -split "`n") | Where-Object { $_ -notmatch ('^F . ' + [regex]::Escape($syncExeName) + ' ') }) -join "`n"
}
$c = Invoke-Detect (Join-Path $syncC $syncExeName) @('--sync-payloads-and-exit')
$cs = Invoke-Detect $CsLauncher @('--sync-payloads', '--root', $syncCs)
$treeC = Snapshot-SyncTree $syncC
$treeCs = Snapshot-SyncTree $syncCs
$released = @(($treeC -split "`n") | Where-Object { $_ -like 'F *' }).Count
if ($c.Code -ne $cs.Code -or $c.Out -ne $cs.Out) {
    $fail++; $failures.Add("[sync-payloads] exit/stdout differ C=$($c.Code) C#=$($cs.Code)`n--- C ---`n$($c.Out)$($c.Err)--- C# ---`n$($cs.Out)$($cs.Err)")
} elseif ($treeC -ne $treeCs) {
    $fail++; $failures.Add("[sync-payloads] released trees differ`n--- C ---`n$treeC`n--- C# ---`n$treeCs")
} elseif ($released -lt 5 -or $cs.Out -notmatch "(?m)^log=Built-in components synced: $released file\(s\)\.$") {
    $fail++; $failures.Add("[sync-payloads] expected >= 5 released files and a matching sync log (released=$released)`n$($cs.Out)")
} elseif ($cs.Out -notmatch "(?m)^result=$($c.Code)$") {
    $fail++; $failures.Add("[sync-payloads] result line does not match the exit code`n$($cs.Out)")
} else {
    $pass++; Write-Host ("  ok  {0,-52} files={1} exit={2}" -f 'sync-payloads/first run', $released, $c.Code)
}
# second run (both launchers): identical content -> nothing rewritten, no sync log; foreign file untouched
$stampBefore = @{}
foreach ($d in @($syncC, $syncCs)) {
    Get-ChildItem -LiteralPath $d -Recurse -File | ForEach-Object { $stampBefore[$_.FullName] = $_.LastWriteTimeUtc.Ticks }
    Touch "$d\config\api.ini" ($asciiEnc.GetBytes("[api]`r`nkey=user-secret`r`n"))
}
Start-Sleep -Milliseconds 30
$c2 = Invoke-Detect (Join-Path $syncC $syncExeName) @('--sync-payloads-and-exit')
$cs2 = Invoke-Detect $CsLauncher @('--sync-payloads', '--root', $syncCs)
$rewritten = @(Get-ChildItem -LiteralPath @($syncC, $syncCs) -Recurse -File | Where-Object { $stampBefore.ContainsKey($_.FullName) -and $stampBefore[$_.FullName] -ne $_.LastWriteTimeUtc.Ticks })
if ($c2.Code -eq $c.Code -and $cs2.Code -eq $c.Code -and $c2.Out -eq $cs2.Out -and $rewritten.Count -eq 0 -and
    $cs2.Out -notmatch 'Built-in components synced' -and
    [IO.File]::ReadAllText("$syncC\config\api.ini") -eq "[api]`r`nkey=user-secret`r`n" -and
    [IO.File]::ReadAllText("$syncCs\config\api.ini") -eq "[api]`r`nkey=user-secret`r`n") {
    $pass++; Write-Host ("  ok  {0,-52} rewritten=0" -f 'sync-payloads/second run is a no-op')
} else {
    $fail++; $failures.Add("[sync-payloads] second run rewrote $($rewritten.Count) file(s), touched api.ini, or outputs differ`n--- C ---`n$($c2.Out)--- C# ---`n$($cs2.Out)")
}
# tampered target (same size, different bytes) and a truncated target are both restored, on both sides
$orig1 = [IO.File]::ReadAllBytes("$syncCs\config\api.ini.example")
$bad1 = [byte[]]$orig1.Clone(); $bad1[0] = [byte](($bad1[0] + 1) -band 0xFF)
$orig2 = [IO.File]::ReadAllBytes("$syncCs\scripts\install_runtime_payloads.ps1")
foreach ($d in @($syncC, $syncCs)) {
    [IO.File]::WriteAllBytes("$d\config\api.ini.example", $bad1)
    [IO.File]::WriteAllBytes("$d\scripts\install_runtime_payloads.ps1", $orig2[0..([Math]::Min(10, $orig2.Length - 1))])
}
$c3 = Invoke-Detect (Join-Path $syncC $syncExeName) @('--sync-payloads-and-exit')
$cs3 = Invoke-Detect $CsLauncher @('--sync-payloads', '--root', $syncCs)
$fixedAll = $true
foreach ($d in @($syncC, $syncCs)) {
    if (-not [Linq.Enumerable]::SequenceEqual([byte[]][IO.File]::ReadAllBytes("$d\config\api.ini.example"), [byte[]]$orig1)) { $fixedAll = $false }
    if (-not [Linq.Enumerable]::SequenceEqual([byte[]][IO.File]::ReadAllBytes("$d\scripts\install_runtime_payloads.ps1"), [byte[]]$orig2)) { $fixedAll = $false }
    if (Test-Path -LiteralPath "$d\config\api.ini.example.dstmp") { $fixedAll = $false }
}
if ($c3.Code -eq $c.Code -and $cs3.Code -eq $c.Code -and $c3.Out -eq $cs3.Out -and $fixedAll -and
    $cs3.Out -match '(?m)^log=Built-in components synced: 2 file\(s\)\.$') {
    $pass++; Write-Host ("  ok  {0,-52} restored=2" -f 'sync-payloads/tampered targets restored')
} else {
    $fail++; $failures.Add("[sync-payloads] tampered targets not restored (fixedAll=$fixedAll)`n--- C ---`n$($c3.Out)--- C# ---`n$($cs3.Out)")
}
# a target that is a directory cannot be replaced: counted as failed, exit 5, other files still synced
foreach ($d in @($syncC, $syncCs)) {
    $blocker = "$d\config\launcher.ini.example"
    Remove-Item -LiteralPath $blocker -Force; Mkdir $blocker; Touch "$blocker\keep.txt"
    [IO.File]::WriteAllBytes("$d\config\api.ini.example", $bad1)
}
$c4 = Invoke-Detect (Join-Path $syncC $syncExeName) @('--sync-payloads-and-exit')
$cs4 = Invoke-Detect $CsLauncher @('--sync-payloads', '--root', $syncCs)
$blockedOk = $true
foreach ($d in @($syncC, $syncCs)) {
    if (-not [Linq.Enumerable]::SequenceEqual([byte[]][IO.File]::ReadAllBytes("$d\config\api.ini.example"), [byte[]]$orig1)) { $blockedOk = $false }
    if (-not (Test-Path -LiteralPath "$d\config\launcher.ini.example\keep.txt")) { $blockedOk = $false }
}
if ($c4.Code -eq 5 -and $cs4.Code -eq 5 -and $c4.Out -eq $cs4.Out -and $blockedOk -and
    $cs4.Out -match '(?m)^log=Built-in component update failed: launcher\.ini\.example$' -and
    $cs4.Out -match '(?m)^log=Warning: 1 built-in component\(s\) could not be updated\. Close running games/server and restart the launcher\.$' -and
    $cs4.Out -match '(?m)^result=5$') {
    $pass++; Write-Host ("  ok  {0,-52} exit=5" -f 'sync-payloads/blocked target reported')
} else {
    $fail++; $failures.Add("[sync-payloads] blocked target not reported identically (C=$($c4.Code) C#=$($cs4.Code) blockedOk=$blockedOk)`n--- C ---`n$($c4.Out)--- C# ---`n$($cs4.Out)")
}

# ---------------------------------------------------------------- Godot patch pack parity
#
# --godot-patch-and-exit / --godot-promote-and-exit / --godot-launcher-and-exit (C) against
# --godot-patch / --godot-promote / --godot-launcher (C#). Every scenario is materialised
# twice and both launchers must agree on: exit code, normalised stdout (log= lines, pack= /
# launcher=, result=), the resulting directory tree (relative path + SHA-256, so the built
# .pck must be byte-identical), and the exact sequence of /batch request bodies.
#
# The patcher talks to the local server at 127.0.0.1:19999, so the section runs against a
# fake deterministic /batch endpoint. Translation rules (identical for both launchers):
#   "KEEPME…"  -> echoed back unchanged      (client must drop it: result == query)
#   "BADFMT…"  -> unbalanced bracket         (client must drop it: format not preserved)
#   otherwise  -> "<CJK>:" + text, non-ASCII sent as \uXXXX so the JSON unescape path runs.
# If port 19999 is already taken the section is skipped loudly instead of touching a live
# server; skipped checks are reported in the summary line.
$godotSkip = $null
$busyPatch = Get-NetTCPConnection -LocalPort 19999 -State Listen -ErrorAction SilentlyContinue
if ($busyPatch) { $godotSkip = "port 19999 already has a listener (pid $($busyPatch.OwningProcess))" }

$fakeState = [hashtable]::Synchronized(@{ Bodies = New-Object System.Collections.Generic.List[string]; Error = $null })
if (-not $godotSkip) {
    $fakeRunspace = [runspacefactory]::CreateRunspace()
    $fakeRunspace.Open()
    $fakeRunspace.SessionStateProxy.SetVariable('state', $fakeState)
    $fakePs = [powershell]::Create()
    $fakePs.Runspace = $fakeRunspace
    $null = $fakePs.AddScript({
        $enc = New-Object Text.UTF8Encoding $false
        function Esc([string]$s) {
            $sb = New-Object Text.StringBuilder
            foreach ($ch in $s.ToCharArray()) {
                $c = [int]$ch
                if ($ch -eq '"') { $null = $sb.Append('\"') }
                elseif ($ch -eq '\') { $null = $sb.Append('\\') }
                elseif ($c -lt 32 -or $c -gt 126) { $null = $sb.AppendFormat('\u{0:x4}', $c) }
                else { $null = $sb.Append($ch) }
            }
            return $sb.ToString()
        }
        $listener = New-Object Net.Sockets.TcpListener ([Net.IPAddress]::Loopback), 19999
        $listener.Start()
        # the main thread stops the listener to unblock AcceptTcpClient (a runspace cannot
        # interrupt a blocking socket call, so Runspace.Close alone would hang here)
        $state['Listener'] = $listener
        $state['Ready'] = $true
        try {
            while ($true) {
                $client = $listener.AcceptTcpClient()
                try {
                    $client.NoDelay = $true
                    $stream = $client.GetStream()
                    $head = New-Object Collections.Generic.List[byte]
                    $one = New-Object byte[] 1
                    while ($true) {
                        if ($stream.Read($one, 0, 1) -le 0) { break }
                        $head.Add($one[0])
                        $n = $head.Count
                        if ($n -ge 4 -and $head[$n - 4] -eq 13 -and $head[$n - 3] -eq 10 -and $head[$n - 2] -eq 13 -and $head[$n - 1] -eq 10) { break }
                    }
                    $headText = [Text.Encoding]::ASCII.GetString($head.ToArray())
                    $len = 0
                    if ($headText -match '(?im)^Content-Length:\s*(\d+)') { $len = [int]$Matches[1] }
                    $body = New-Object byte[] $len
                    $got = 0
                    while ($got -lt $len) {
                        $r = $stream.Read($body, $got, $len - $got)
                        if ($r -le 0) { break }
                        $got += $r
                    }
                    $bodyText = $enc.GetString($body, 0, $got)
                    $state.Bodies.Add($bodyText)
                    $texts = @()
                    try { $texts = @(($bodyText | ConvertFrom-Json).texts) } catch { $state.Error = "bad request body: $bodyText" }
                    $parts = New-Object Collections.Generic.List[string]
                    foreach ($t in $texts) {
                        $s = [string]$t
                        if ($s.StartsWith('KEEPME')) { $parts.Add('"' + (Esc $s) + '"') }
                        elseif ($s.StartsWith('BADFMT')) { $parts.Add('"' + (Esc ([string][char]0x8bd1 + ':[' + $s)) + '"') }
                        else { $parts.Add('"' + (Esc ([string][char]0x8bd1 + ':' + $s)) + '"') }
                    }
                    $payload = $enc.GetBytes('{"results":[' + ($parts -join ',') + ']}')
                    $header = [Text.Encoding]::ASCII.GetBytes("HTTP/1.1 200 OK`r`nContent-Type: application/json`r`nContent-Length: $($payload.Length)`r`nConnection: close`r`n`r`n")
                    $stream.Write($header, 0, $header.Length)
                    $stream.Write($payload, 0, $payload.Length)
                    $stream.Flush()
                    $client.Client.Shutdown([Net.Sockets.SocketShutdown]::Send)
                } finally { $client.Close() }
            }
        } catch {
            # listener.Stop() from the main thread unblocks AcceptTcpClient with an exception
        } finally { $listener.Stop() }
    })
    $fakeHandle = $fakePs.BeginInvoke()
    $waited = 0
    while (-not $fakeState['Ready'] -and $waited -lt 5000) { Start-Sleep -Milliseconds 25; $waited += 25 }
    if (-not $fakeState['Ready']) { $godotSkip = 'fake /batch server did not start' }
}

$godotSkipped = 0
if ($godotSkip) {
    Write-Host "  --  godot-patch parity skipped: $godotSkip"
}

# GDSC v13 bytecode: "GDSC", version, identifier/constant counts, then identifiers
# ([u32 len][bytes]) and constants ([u32 type] + payload; type 4 = String, 4-byte padded).
function New-GdscBytes([string[]]$identifiers, [object[]]$constants) {
    $out = New-Object Collections.Generic.List[byte]
    $out.AddRange($asciiEnc.GetBytes('GDSC'))
    $out.AddRange([BitConverter]::GetBytes([uint32]13))
    $out.AddRange([BitConverter]::GetBytes([uint32]$identifiers.Count))
    $out.AddRange([BitConverter]::GetBytes([uint32]$constants.Count))
    $out.AddRange((New-Object byte[] 8))                   # token/function counts (skipped)
    foreach ($id in $identifiers) {
        $b = $utf8.GetBytes($id)
        $out.AddRange([BitConverter]::GetBytes([uint32]$b.Length))
        $out.AddRange($b)
    }
    foreach ($c in $constants) {
        if ($c -is [string]) {
            $b = $utf8.GetBytes($c)
            $pad = (4 - ($b.Length % 4)) % 4
            $out.AddRange([BitConverter]::GetBytes([uint32]4))
            $out.AddRange([BitConverter]::GetBytes([uint32]$b.Length))
            $out.AddRange($b)
            if ($pad) { $out.AddRange((New-Object byte[] $pad)) }
        } else {
            $out.AddRange([BitConverter]::GetBytes([uint32]$c.Type))
            $out.AddRange((New-Object byte[] $c.Size))
        }
    }
    return [byte[]]$out.ToArray()
}
# OptimizedTranslation resource: property 5 = PackedInt32Array (0x20) hash table, property 6 =
# PackedByteArray (0x1f) string heap. Each record is 4 ints: hash, offset, comp size, uncomp size
# (comp != uncomp means the heap slice is Smaz-compressed).
function New-OptimizedTranslation([object[]]$records) {
    $heap = New-Object Collections.Generic.List[byte]
    $table = New-Object Collections.Generic.List[uint32]
    $table.Add([uint32]$records.Count)                     # bucket size
    $table.Add([uint32]0)                                  # bucket hash
    $h = 11
    foreach ($r in $records) {
        $bytes = [byte[]]$r.Bytes
        $table.Add([uint32]$h); $h += 11
        $table.Add([uint32]$heap.Count)
        $table.Add([uint32]$bytes.Length)
        $table.Add([uint32]$r.Uncomp)
        $heap.AddRange($bytes)
    }
    $out = New-Object Collections.Generic.List[byte]
    $out.AddRange($asciiEnc.GetBytes('RSRC'))
    $out.AddRange([BitConverter]::GetBytes([uint32]5))
    $out.AddRange([BitConverter]::GetBytes([uint32]0x20))
    $out.AddRange([BitConverter]::GetBytes([uint32]$table.Count))
    foreach ($v in $table) { $out.AddRange([BitConverter]::GetBytes([uint32]$v)) }
    $out.AddRange([BitConverter]::GetBytes([uint32]6))
    $out.AddRange([BitConverter]::GetBytes([uint32]0x1f))
    $out.AddRange([BitConverter]::GetBytes([uint32]$heap.Count))
    $out.AddRange($heap)
    $pad = (4 - ($heap.Count % 4)) % 4
    if ($pad) { $out.AddRange((New-Object byte[] $pad)) }
    $out.AddRange($asciiEnc.GetBytes('TAIL8ify'))          # bytes after the heap must survive
    return [byte[]]$out.ToArray()
}
function TransRec([byte[]]$bytes, [int]$uncomp) { return @{ Bytes = $bytes; Uncomp = $uncomp } }

$gpJson = @'
{
  "items": [
    { "id": "potion", "name": "Healing Potion", "description": "Restores %d HP to one ally.", "icon": "res://art/potion.png" },
    { "id": "sword", "name": "KEEPME Blade", "description": "BADFMT [b]sharp[/b] and heavy." }
  ],
  "name": "Item Database"
}
'@
$gpScene = @'
[gd_scene load_steps=2 format=2]

[node name="Menu" type="Control"]

[node name="Start" type="Button" parent="."]
text = "Start a new journey"
tooltip_text = "Leaves the current save untouched."
icon = "res://art/start.png"

[node name="Quit" type="Button" parent="."]
text = "Quit to desktop"
'@
$gpMarkdown = @'
# Chapter One

The rain had not stopped for three days.

```
code fences are copied verbatim
```

> Quoted lines stay as they are.

[link]: res://somewhere

She looked up and said nothing at all.
'@
$gpGdc = New-GdscBytes @('_ready__', 'on_press') @(
    'Are you sure?', 'Title', 'pressed', 'some_signal', 'Yes',
    @{ Type = 2; Size = 4 }, @{ Type = 5; Size = 8 })
$gpTranslation = New-OptimizedTranslation @(
    (TransRec ($utf8.GetBytes("Continue Game") + [byte[]]@(0)) 14),
    (TransRec ([byte[]]@(0x01)) 3),
    (TransRec ([byte[]]@(254, 0x41)) 1),
    (TransRec ([byte[]](255, 4) + $asciiEnc.GetBytes('Hello')) 5))
$gpPckEntries = @(
    (GodotEntry 'res://data/items.json' ($utf8.GetBytes($gpJson))),
    (GodotEntry 'res://scenes/menu.tscn' ($utf8.GetBytes($gpScene))),
    (GodotEntry 'res://lang/en/story.md' ($utf8.GetBytes($gpMarkdown))),
    (GodotEntry 'res://scripts/menus/confirm_popup.gdc' $gpGdc),
    (GodotEntry 'res://ui/menus.en.translation' $gpTranslation),
    (GodotEntry 'res://art/potion.png' ($asciiEnc.GetBytes('PNGDATA'))))

$patchScenarios = [ordered]@{}
# loose project (no pack anywhere): stale packs are removed and a runtime sidecar is written
$patchScenarios['gp_loose_project'] = {
    param($d)
    Write-Utf8 "$d\project.godot" "config_version=5`n`n[application]`n`nconfig/name=`"Parity`"`nrun/main_scene=`"res://scenes/menu.tscn`"`n"
    Write-Utf8 "$d\scenes\menu.tscn" $gpScene
    Touch "$d\Game.exe"
    Touch "$d\dst_godot_patch.pck" ($asciiEnc.GetBytes('stale pack'))
    Touch "$d\dst_godot_patch.next.pck" ($asciiEnc.GetBytes('stale staged pack'))
}
# Godot 3 sidecar pack (format 1): json / tscn / md / gdc / OptimizedTranslation / font entry
$patchScenarios['gp_pck_v1_resources'] = {
    param($d)
    Touch "$d\Game.exe"
    Touch "$d\Game.pck" (New-GodotPck 1 ($gpPckEntries + @((GodotEntry 'res://fonts/NotoSans-Regular.ttf' ($asciiEnc.GetBytes('TTFDATA'))))))
    Touch "$d\Other.pck" (New-GodotPck 1 @((GodotEntry 'res://data/items.json' ($utf8.GetBytes($gpJson)))))
}
# format 3 with engine 4.6: runtime script + autoload + override.cfg are embedded and the
# directory is rebuilt; the existing override.cfg keeps its own keys
$patchScenarios['gp_pck_v3_autoload'] = {
    param($d)
    Touch "$d\Game.exe"
    Touch "$d\Game.pck" (New-GodotPck 3 ($gpPckEntries + @(
        (GodotEntry 'res://override.cfg' ($asciiEnc.GetBytes("[display]`nwindow/size/viewport_width=1280")))
    )) 0 0 6)
}
# format 3 with engine 4.4: no autoload_prepend support, only the runtime script is embedded
$patchScenarios['gp_pck_v3_no_autoload'] = {
    param($d)
    Touch "$d\Game.exe"
    Touch "$d\Game.pck" (New-GodotPck 3 $gpPckEntries)
}
# pack embedded in the executable (format 1, absolute offsets): copied out and normalised
$patchScenarios['gp_embedded_exe'] = {
    param($d)
    Touch "$d\Game.exe" (New-GodotEmbeddedExe 1 $gpPckEntries $true)
}
# batching math: 128 texts per /batch, the first 1024 texts of a pack are translated live and
# everything after that is cache_only, including the partial batch that crosses the quota, and
# a single resource never contributes more than 1800 strings
function New-BulkJson([int]$count, [string]$tag) {
    $sb = New-Object Text.StringBuilder
    $null = $sb.Append('{"items":[')
    for ($i = 0; $i -lt $count; $i++) {
        if ($i) { $null = $sb.Append(',') }
        $null = $sb.AppendFormat('{{"id":"{0}{1:d4}","name":"{0} entry {1:d4} of the batch test"}}', $tag, $i)
    }
    $null = $sb.Append(']}')
    return $utf8.GetBytes($sb.ToString())
}
$patchScenarios['gp_batch_live_quota'] = {
    param($d)
    Touch "$d\Game.exe"
    Touch "$d\Game.pck" (New-GodotPck 1 @(
        (GodotEntry 'res://data/items.json' (New-BulkJson 500 'Alpha')),
        (GodotEntry 'res://data/skills.json' (New-BulkJson 700 'Bravo')),
        (GodotEntry 'res://data/heroes.json' (New-BulkJson 1850 'Delta'))))
}
# packaged game with nothing translatable: no pack is installed at all
$patchScenarios['gp_no_translatable_entries'] = {
    param($d)
    Touch "$d\Game.exe"
    Touch "$d\Game.pck" (New-GodotPck 1 @((GodotEntry 'res://art/potion.png' ($asciiEnc.GetBytes('PNGDATA')))))
}

if (-not $godotSkip) {
    foreach ($name in $patchScenarios.Keys) {
        $dirs = @{}
        foreach ($side in @('c', 'cs')) {
            $dirs[$side] = Join-Path $root "gp\$side\$name"
            Mkdir $dirs[$side]
            & $patchScenarios[$name] $dirs[$side]
        }
        $fakeState.Bodies.Clear()
        $c = Invoke-Detect $CLauncher @('--godot-patch-and-exit', $dirs['c'])
        $bodiesC = @($fakeState.Bodies.ToArray())
        $fakeState.Bodies.Clear()
        $cs = Invoke-Detect $CsLauncher @('--godot-patch', $dirs['cs'])
        $bodiesCs = @($fakeState.Bodies.ToArray())
        $outC = $c.Out -replace [regex]::Escape($dirs['c']), '<DIR>'
        $outCs = $cs.Out -replace [regex]::Escape($dirs['cs']), '<DIR>'
        $treeC = Snapshot-Tree $dirs['c']
        $treeCs = Snapshot-Tree $dirs['cs']
        $label = "godot-patch/$name"
        if ($c.Code -ne $cs.Code) {
            $fail++; $failures.Add("[$label] exit codes C=$($c.Code) C#=$($cs.Code)`n--- C ---`n$($c.Out)$($c.Err)--- C# ---`n$($cs.Out)$($cs.Err)")
        } elseif ($outC -ne $outCs) {
            $fail++; $failures.Add("[$label] stdout differs`n--- C ---`n$outC--- C# ---`n$outCs")
        } elseif ($treeC -ne $treeCs) {
            $fail++; $failures.Add("[$label] resulting trees differ`n--- C ---`n$treeC`n--- C# ---`n$treeCs")
        } elseif (($bodiesC -join "`n") -ne ($bodiesCs -join "`n")) {
            $fail++; $failures.Add("[$label] /batch request bodies differ`n--- C ---`n$($bodiesC -join "`n")`n--- C# ---`n$($bodiesCs -join "`n")")
        } elseif ($fakeState.Error) {
            $fail++; $failures.Add("[$label] fake server rejected a request: $($fakeState.Error)")
        } else {
            $pass++
            $res = if ($outC -match '(?m)^result=(\d+)$') { $Matches[1] } else { '?' }
            Write-Host ("  ok  {0,-52} result={1} batches={2}" -f $label, $res, $bodiesC.Count)
            if ($ShowLogs) { ($outC -split "`n") | Where-Object { $_ -like 'log=*' -or $_ -like 'pack=*' } | ForEach-Object { Write-Host "        $_" } }
        }
    }

    # A locked active pack must be staged as dst_godot_patch.next.pck and promoted on the next
    # run: while the lock is held both launchers report the same Windows error, after the lock
    # is released both promote the staged pack.
    $stageDirs = @{}
    $locks = @{}
    foreach ($side in @('c', 'cs')) {
        $sd = Join-Path $root "gp\$side\gp_staged_busy"
        Mkdir $sd
        & $patchScenarios['gp_pck_v1_resources'] $sd
        Touch "$sd\dst_godot_patch.pck" ($asciiEnc.GetBytes('previous pack held open by the running game'))
        $stageDirs[$side] = $sd
        $locks[$side] = [IO.File]::Open("$sd\dst_godot_patch.pck", 'Open', 'Read', 'None')
    }
    $fakeState.Bodies.Clear()
    $c = Invoke-Detect $CLauncher @('--godot-patch-and-exit', $stageDirs['c'])
    $fakeState.Bodies.Clear()
    $cs = Invoke-Detect $CsLauncher @('--godot-patch', $stageDirs['cs'])
    $cP = Invoke-Detect $CLauncher @('--godot-promote-and-exit', $stageDirs['c'])
    $csP = Invoke-Detect $CsLauncher @('--godot-promote', $stageDirs['cs'])
    foreach ($side in @('c', 'cs')) { $locks[$side].Close() }
    $cP2 = Invoke-Detect $CLauncher @('--godot-promote-and-exit', $stageDirs['c'])
    $csP2 = Invoke-Detect $CsLauncher @('--godot-promote', $stageDirs['cs'])
    $outC = ($c.Out + $cP.Out + $cP2.Out) -replace [regex]::Escape($stageDirs['c']), '<DIR>'
    $outCs = ($cs.Out + $csP.Out + $csP2.Out) -replace [regex]::Escape($stageDirs['cs']), '<DIR>'
    $treeC = Snapshot-Tree $stageDirs['c']
    $treeCs = Snapshot-Tree $stageDirs['cs']
    if ($outC -ne $outCs -or $treeC -ne $treeCs) {
        $fail++; $failures.Add("[godot-patch/staged_busy] stdout or tree differs`n--- C ---`n$outC$treeC`n--- C# ---`n$outCs$treeCs")
    } elseif ($outC -notmatch '(?m)^log=Godot: active patch pack is busy; staged refreshed pack for next launch: <DIR>\\dst_godot_patch\.next\.pck$' -or
              $outC -notmatch '(?m)^log=Godot: refreshed patch pack exists but could not be promoted yet\. Windows error: \d+$' -or
              $outC -notmatch '(?m)^log=Godot: promoted refreshed patch pack from previous run\.$' -or
              (Test-Path -LiteralPath "$($stageDirs['c'])\dst_godot_patch.next.pck")) {
        $fail++; $failures.Add("[godot-patch/staged_busy] staging/promotion path not taken`n$outC")
    } else {
        $pass++; Write-Host ("  ok  {0,-52} staged -> promoted" -f 'godot-patch/staged_busy')
    }

    # --godot-launcher: hard-linked copy plus ownership marker, idempotent on the second run,
    # and refused (result=0) when the copy exists without the marker.
    $lnDirs = @{}
    foreach ($side in @('c', 'cs')) {
        $ld = Join-Path $root "gp\$side\gp_launcher"
        Mkdir $ld
        Touch "$ld\Game.exe" ($asciiEnc.GetBytes('game executable bytes'))
        Touch "$ld\Game.pck" (New-GodotPck 1 @((GodotEntry 'res://data/items.json' ($utf8.GetBytes($gpJson)))))
        $lnDirs[$side] = $ld
    }
    $l1 = Invoke-Detect $CLauncher @('--godot-launcher-and-exit', $lnDirs['c'])
    $l1cs = Invoke-Detect $CsLauncher @('--godot-launcher', $lnDirs['cs'])
    $l2 = Invoke-Detect $CLauncher @('--godot-launcher-and-exit', $lnDirs['c'])
    $l2cs = Invoke-Detect $CsLauncher @('--godot-launcher', $lnDirs['cs'])
    foreach ($side in @('c', 'cs')) { Remove-Item -LiteralPath "$($lnDirs[$side])\dst_godot_patch.exe.dst-owned" -Force }
    $l3 = Invoke-Detect $CLauncher @('--godot-launcher-and-exit', $lnDirs['c'])
    $l3cs = Invoke-Detect $CsLauncher @('--godot-launcher', $lnDirs['cs'])
    $outC = ($l1.Out + $l2.Out + $l3.Out) -replace [regex]::Escape($lnDirs['c']), '<DIR>'
    $outCs = ($l1cs.Out + $l2cs.Out + $l3cs.Out) -replace [regex]::Escape($lnDirs['cs']), '<DIR>'
    $treeC = Snapshot-Tree $lnDirs['c']
    $treeCs = Snapshot-Tree $lnDirs['cs']
    if ($outC -ne $outCs -or $treeC -ne $treeCs) {
        $fail++; $failures.Add("[godot-patch/launcher_copy] stdout or tree differs`n--- C ---`n$outC$treeC`n--- C# ---`n$outCs$treeCs")
    } elseif ($outC -notmatch '(?m)^log=Godot: prepared a launcher-owned executable for exports that disable --main-pack\.$' -or
              $outC -notmatch '(?m)^launcher=<DIR>\\dst_godot_patch\.exe$' -or
              $outC -notmatch '(?m)^log=Godot: preserved an existing dst_godot_patch\.exe without a launcher ownership marker\.$' -or
              @($outC -split "`n" | Where-Object { $_ -eq 'result=1' }).Count -ne 2 -or
              @($outC -split "`n" | Where-Object { $_ -eq 'result=0' }).Count -ne 1) {
        $fail++; $failures.Add("[godot-patch/launcher_copy] ownership marker path not taken`n$outC")
    } else {
        $pass++; Write-Host ("  ok  {0,-52} link + marker + refusal" -f 'godot-patch/launcher_copy')
    }
} else {
    $godotSkipped = $patchScenarios.Count + 2
}

# ---------------------------------------------------------------- config / preflight parity
#
# api.ini data layer (--api-config-and-exit / --api-config-set-and-exit vs --api-config /
# --api-config-set), the --main-pack rejection classifier (--godot-probe*) and the headless
# preflight conclusion cache (--godot-preflight*). Both launchers get their own root, so the
# printed api.ini/godot_preflight.ini paths are normalised away; everything else — the preset
# table, the loaded values, the written INI bytes, the signature string (which contains the
# probed file's mtime/size, so both sides are pointed at one shared file) and the cache
# hit/miss results — must be identical.
$cfgC = Join-Path $root 'cfg_c'; Mkdir $cfgC
$cfgCs = Join-Path $root 'cfg_cs'; Mkdir $cfgCs
$cfgExe = Join-Path $cfgC (Split-Path -Leaf $CLauncher)
Copy-Item -LiteralPath $CLauncher -Destination $cfgExe
$cfgShared = Join-Path $root 'cfg_shared'; Mkdir $cfgShared
Touch "$cfgShared\Game.exe" ($asciiEnc.GetBytes('preflight signature target'))

function Hex-Or-Missing([string]$path) {
    if (-not (Test-Path -LiteralPath $path)) { return 'missing' }
    return [BitConverter]::ToString([IO.File]::ReadAllBytes($path))
}
function Invoke-ConfigPair([string[]]$cArgs, [string[]]$csArgs) {
    $c = Invoke-Detect $cfgExe $cArgs
    $cs = Invoke-Detect $CsLauncher ($csArgs + @('--root', $cfgCs))
    return @{
        C = $c
        Cs = $cs
        OutC = $c.Out -replace [regex]::Escape($cfgC), '<ROOT>'
        OutCs = $cs.Out -replace [regex]::Escape($cfgCs), '<ROOT>'
    }
}
function Check-ConfigPair([string]$label, [object]$r, [string[]]$mustMatch) {
    if ($r.C.Code -ne $r.Cs.Code) {
        $script:fail++; $script:failures.Add("[$label] exit codes C=$($r.C.Code) C#=$($r.Cs.Code)`n--- C ---`n$($r.C.Out)$($r.C.Err)--- C# ---`n$($r.Cs.Out)$($r.Cs.Err)")
        return $false
    }
    if ($r.OutC -ne $r.OutCs) {
        $script:fail++; $script:failures.Add("[$label] stdout differs`n--- C ---`n$($r.OutC)--- C# ---`n$($r.OutCs)")
        return $false
    }
    foreach ($m in $mustMatch) {
        if ($r.OutC -notmatch $m) {
            $script:fail++; $script:failures.Add("[$label] expected /$m/ in`n$($r.OutC)")
            return $false
        }
    }
    return $true
}

# defaults when api.ini does not exist yet, plus the full provider preset table
$r = Invoke-ConfigPair @('--api-config-and-exit') @('--api-config')
if (Check-ConfigPair 'api-config/defaults' $r @(
        '(?m)^presets=11$',
        '(?m)^preset0=[^|]+\|-\|-$',
        '(?m)^preset1=DeepSeek\|https://api\.deepseek\.com/v1/chat/completions\|deepseek-v4-flash$',
        '(?m)^endpoint=https://api\.deepseek\.com/v1/chat/completions$',
        '(?m)^model=deepseek-v4-flash$',
        '(?m)^key=$',
        '(?m)^selected=1$')) {
    $pass++; Write-Host ("  ok  {0,-52} presets=11" -f 'api-config/defaults')
}
# writing the three keys must produce byte-identical api.ini files (Profile API encoding rules)
$r = Invoke-ConfigPair @('--api-config-set-and-exit', 'https://api.moonshot.cn/v1/chat/completions', 'kimi-latest-8k', 'sk-parity-KEY') `
                       @('--api-config-set', 'https://api.moonshot.cn/v1/chat/completions', 'kimi-latest-8k', 'sk-parity-KEY')
$iniC = Hex-Or-Missing "$cfgC\config\api.ini"
$iniCs = Hex-Or-Missing "$cfgCs\config\api.ini"
if (Check-ConfigPair 'api-config/save' $r @('(?m)^log=API .+<ROOT>\\config\\api\.ini$', '(?m)^result=1$')) {
    if ($iniC -ne $iniCs -or $iniC -eq 'missing') {
        $fail++; $failures.Add("[api-config/save] api.ini bytes differ`n--- C ---`n$iniC`n--- C# ---`n$iniCs")
    } else {
        $pass++; Write-Host ("  ok  {0,-52} identical api.ini" -f 'api-config/save')
    }
}
# reading back: a non-preset endpoint falls back to "custom" (selected=0)
$r = Invoke-ConfigPair @('--api-config-and-exit') @('--api-config')
$r2 = Invoke-ConfigPair @('--api-config-set-and-exit', 'https://intranet.example/v1/chat', 'house-model', '') `
                        @('--api-config-set', 'https://intranet.example/v1/chat', 'house-model', '')
$r3 = Invoke-ConfigPair @('--api-config-and-exit') @('--api-config')
if ((Check-ConfigPair 'api-config/reload' $r @('(?m)^model=kimi-latest-8k$', '(?m)^key=sk-parity-KEY$', '(?m)^selected=3$')) -and
    (Check-ConfigPair 'api-config/custom-endpoint' $r2 @('(?m)^result=1$')) -and
    (Check-ConfigPair 'api-config/custom-endpoint-reload' $r3 @('(?m)^endpoint=https://intranet\.example/v1/chat$', '(?m)^key=$', '(?m)^selected=0$'))) {
    $pass++; Write-Host ("  ok  {0,-52} preset lookup + custom fallback" -f 'api-config/reload')
}

# --main-pack rejection classifier: only an option-rejection diagnostic that also names the
# option counts; unrelated crashes and case variants are pinned too
$probeTexts = @(
    'Unknown option: --main-pack',
    'ERROR: Invalid command line: --MAIN-PACK',
    'godot: unrecognized option "--main-pack"',
    'Main-Pack is NOT SUPPORTED by this template',
    'ERROR: failed to load autoload script, exiting',
    '--main-pack accepted, loading pack',
    'unknown option --script',
    '')
$r = Invoke-ConfigPair (@('--godot-probe-and-exit') + $probeTexts) (@('--godot-probe') + $probeTexts)
if (Check-ConfigPair 'godot-probe/classifier' $r @(
        '(?m)^reject0=1$', '(?m)^reject1=1$', '(?m)^reject2=1$', '(?m)^reject3=1$',
        '(?m)^reject4=0$', '(?m)^reject5=0$', '(?m)^reject6=0$', '(?m)^reject7=0$')) {
    $pass++; Write-Host ("  ok  {0,-52} 8 outputs classified" -f 'godot-probe/classifier')
}

# preflight cache: miss -> put(ok) -> hit; a different kind and a changed file are misses; a
# transient conclusion (put < 0) leaves no entry behind
$r = Invoke-ConfigPair @('--godot-preflight-and-exit', 'sidecar', "$cfgShared\Game.exe", 'export', '1') `
                       @('--godot-preflight', 'sidecar', "$cfgShared\Game.exe", 'export', '1')
$ok1 = Check-ConfigPair 'godot-preflight/put-hit' $r @('(?m)^get=-1$', '(?m)^get2=1$', '(?m)^sig=\|.+\\Game\.exe\|[0-9a-f]{16}\|26\|\|export$')
$r = Invoke-ConfigPair @('--godot-preflight-and-exit', 'sidecar', "$cfgShared\Game.exe", 'export', '-1') `
                       @('--godot-preflight', 'sidecar', "$cfgShared\Game.exe", 'export', '-1')
$ok2 = Check-ConfigPair 'godot-preflight/hit' $r @('(?m)^get=1$')
$r = Invoke-ConfigPair @('--godot-preflight-and-exit', 'mainpack', "$cfgShared\Game.exe", 'export', '0') `
                       @('--godot-preflight', 'mainpack', "$cfgShared\Game.exe", 'export', '0')
$ok3 = Check-ConfigPair 'godot-preflight/other-kind' $r @('(?m)^get=-1$', '(?m)^get2=0$')
$r = Invoke-ConfigPair @('--godot-preflight-and-exit', 'sidecar', "$cfgShared\missing.exe", 'export', '-1') `
                       @('--godot-preflight', 'sidecar', "$cfgShared\missing.exe", 'export', '-1')
$ok4 = Check-ConfigPair 'godot-preflight/missing-file' $r @('(?m)^sig=\|.+\\missing\.exe\|0\|0\|\|export$', '(?m)^get=-1$')
Touch "$cfgShared\Game.exe" ($asciiEnc.GetBytes('preflight signature target, now a different size'))
$r = Invoke-ConfigPair @('--godot-preflight-and-exit', 'sidecar', "$cfgShared\Game.exe", 'export', '-1') `
                       @('--godot-preflight', 'sidecar', "$cfgShared\Game.exe", 'export', '-1')
$ok5 = Check-ConfigPair 'godot-preflight/changed-file' $r @('(?m)^get=-1$')
$preC = Hex-Or-Missing "$cfgC\config\godot_preflight.ini"
$preCs = Hex-Or-Missing "$cfgCs\config\godot_preflight.ini"
if ($ok1 -and $ok2 -and $ok3 -and $ok4 -and $ok5) {
    if ($preC -ne $preCs) {
        $fail++; $failures.Add("[godot-preflight] godot_preflight.ini bytes differ`n--- C ---`n$preC`n--- C# ---`n$preCs")
    } else {
        $pass++; Write-Host ("  ok  {0,-52} miss/put/hit + identical ini" -f 'godot-preflight/cache')
    }
}

# ---------------------------------------------------------------- launch flow parity
#
# ui.c 的一键流程（服务器就绪之后）：--launch-flow-and-exit vs --launch-flow. Warmup runs
# for real in dump mode (post= lines), the Godot headless preflight probes really start the
# game executable — a stub console app whose stdout/exit code/sleep come from a control file
# next to it, and which appends its own command line to dst_fake_godot_calls.txt — and only
# the three "really start something" points become spawn=<kind>|<exe>|<cmd>|<cwd> plan lines.
# Compared: stdout (paths normalised), the resulting tree, and the recorded probe command
# lines, so both the decisions and the probe arguments have to match.
$lfC = Join-Path $root 'lf_c'; Mkdir $lfC
$lfCs = Join-Path $root 'lf_cs'; Mkdir $lfCs
$lfExe = Join-Path $lfC (Split-Path -Leaf $CLauncher)
Copy-Item -LiteralPath $CLauncher -Destination $lfExe

$fakeGodot = Join-Path $root 'fake_godot_template.exe'
Add-Type -OutputType ConsoleApplication -OutputAssembly $fakeGodot -TypeDefinition @'
using System;
using System.IO;
using System.Threading;

public static class FakeGodot
{
    public static int Main(string[] args)
    {
        string dir = AppDomain.CurrentDomain.BaseDirectory;
        string text = "";
        int code = 0, sleep = 0;
        string cfg = Path.Combine(dir, "dst_fake_godot.ini");
        if (File.Exists(cfg)) {
            foreach (string line in File.ReadAllLines(cfg)) {
                int eq = line.IndexOf('=');
                if (eq <= 0) continue;
                string k = line.Substring(0, eq).Trim();
                string v = line.Substring(eq + 1).Trim();
                if (k == "stdout") text = v.Replace("\\n", "\n");
                else if (k == "exit") code = int.Parse(v);
                else if (k == "sleepms") sleep = int.Parse(v);
            }
        }
        /* The call log lives beside the game directory, not inside it: the Godot warmup
           scanner would otherwise pick up its own probe command lines as game text. */
        string self = dir.TrimEnd('\\');
        File.AppendAllText(Path.Combine(Path.GetDirectoryName(self), Path.GetFileName(self) + ".calls.txt"),
                           Environment.CommandLine + "\n");
        if (text.Length > 0) { Console.Out.Write(text); Console.Out.Flush(); }
        if (sleep > 0) Thread.Sleep(sleep);
        return code;
    }
}
'@

function New-FakeGodotExe([string]$dir, [string]$name, [string]$stdoutText, [int]$exitCode, [int]$sleepMs = 0) {
    Copy-Item -LiteralPath $fakeGodot -Destination (Join-Path $dir $name)
    Write-Utf8 (Join-Path $dir 'dst_fake_godot.ini') "stdout=$stdoutText`nexit=$exitCode`nsleepms=$sleepMs`n"
}
function Godot-ProbeCalls([string]$dir) {
    $log = $dir.TrimEnd('\') + '.calls.txt'
    if (-not (Test-Path -LiteralPath $log)) { return '' }
    return [IO.File]::ReadAllText($log)
}

$launchScenarios = [ordered]@{}
# Ren'Py: launch first, then warm the script in the background
$launchScenarios['lf_renpy'] = {
    param($d)
    New-RenpyGame $d
    Write-Utf8 "$d\game\script.rpy" "label start:`n    e `"Hello there, traveler.`"`n    `"It was a dark and stormy night.`"`n"
}
# RPG Maker: import first, launch after (its plugin's live lookups must hit imported lines)
$launchScenarios['lf_rpgm'] = {
    param($d)
    New-RpgmWww $d
    Write-Utf8 "$d\www\data\Items.json" '[null,{"id":1,"name":"Healing Potion","description":"Restores a little HP."}]'
}
# Unknown engine: no deploy-specific ordering, warm then launch
$launchScenarios['lf_unknown'] = {
    param($d)
    Touch "$d\Game.exe"
}
# Godot loose project: sidecar is written and its preflight succeeds
$launchScenarios['lf_godot_loose_ok'] = {
    param($d)
    Write-Utf8 "$d\project.godot" "config_version=5`n`n[application]`n`nconfig/name=`"Parity`"`n"
    Write-Utf8 "$d\scenes\menu.tscn" $gpScene
    New-FakeGodotExe $d 'Game.exe' 'Godot Engine v4.2.stable - preflight ok' 0
}
# Godot loose project: the export template rejects --script, so the normal launch is used
$launchScenarios['lf_godot_loose_reject'] = {
    param($d)
    Write-Utf8 "$d\project.godot" "config_version=5`n`n[application]`n`nconfig/name=`"Parity`"`n"
    New-FakeGodotExe $d 'Game.exe' 'ERROR: Script does not exist in this template.' 3
}
# Godot export without a patch pack: generic runtime sidecar now, detached patch refresh after
$launchScenarios['lf_godot_export_nopack'] = {
    param($d)
    Touch "$d\Game.pck" (New-GodotPck 1 @((GodotEntry 'res://data/items.json' ($utf8.GetBytes($gpJson)))))
    New-FakeGodotExe $d 'Game.exe' '' 0
}
# Godot export with a patch pack: launcher-owned exe copy + embedded sidecar script
$launchScenarios['lf_godot_export_pack'] = {
    param($d)
    Touch "$d\Game.pck" (New-GodotPck 1 @((GodotEntry 'res://data/items.json' ($utf8.GetBytes($gpJson)))))
    Touch "$d\dst_godot_patch.pck" (New-GodotPck 1 @(
        (GodotEntry 'res://data/items.json' ($utf8.GetBytes($gpJson))),
        (GodotEntry 'res://dst_godot_runtime.gd' ($utf8.GetBytes("extends SceneTree`n")))))
    New-FakeGodotExe $d 'Game.exe' '' 0
}
# Godot export whose template rejects --main-pack: the launcher copy is refused (an existing
# dst_godot_patch.exe without an ownership marker), the sidecar preflight fails, and the
# isolated --main-pack probe gets an explicit option-rejection diagnostic ⇒ normal launch.
$launchScenarios['lf_godot_mainpack_rejected'] = {
    param($d)
    Touch "$d\Game.pck" (New-GodotPck 1 @((GodotEntry 'res://data/items.json' ($utf8.GetBytes($gpJson)))))
    Touch "$d\dst_godot_patch.pck" (New-GodotPck 1 @((GodotEntry 'res://data/items.json' ($utf8.GetBytes($gpJson)))))
    Touch "$d\dst_godot_patch.exe" ($asciiEnc.GetBytes('user-provided copy without an ownership marker'))
    New-FakeGodotExe $d 'Game.exe' 'Unknown option: --main-pack. Try --help for a list of options.' 1
}

$lfOut = @{}
foreach ($name in $launchScenarios.Keys) {
    $dirs = @{}
    foreach ($side in @('c', 'cs')) {
        $dirs[$side] = Join-Path $root "lf\$side\$name"
        Mkdir $dirs[$side]
        & $launchScenarios[$name] $dirs[$side]
    }
    $c = Invoke-Detect $lfExe @('--launch-flow-and-exit', $dirs['c'])
    $cs = Invoke-Detect $CsLauncher @('--launch-flow', $dirs['cs'], '--root', $lfCs)
    # the launcher binaries and their roots differ by construction; everything else must not
    $outC = $c.Out -replace [regex]::Escape($dirs['c']), '<DIR>' -replace [regex]::Escape($lfExe), '<LAUNCHER>' -replace [regex]::Escape($lfC), '<ROOT>'
    $outCs = $cs.Out -replace [regex]::Escape($dirs['cs']), '<DIR>' -replace [regex]::Escape($CsLauncher), '<LAUNCHER>' -replace [regex]::Escape($lfCs), '<ROOT>'
    $callsC = (Godot-ProbeCalls $dirs['c']) -replace [regex]::Escape($dirs['c']), '<DIR>'
    $callsCs = (Godot-ProbeCalls $dirs['cs']) -replace [regex]::Escape($dirs['cs']), '<DIR>'
    $treeC = Snapshot-Tree $dirs['c']
    $treeCs = Snapshot-Tree $dirs['cs']
    $lfOut[$name] = $outC
    $label = "launch-flow/$name"
    if ($c.Code -ne $cs.Code) {
        $fail++; $failures.Add("[$label] exit codes C=$($c.Code) C#=$($cs.Code)`n--- C ---`n$($c.Out)$($c.Err)--- C# ---`n$($cs.Out)$($cs.Err)")
    } elseif ($outC -ne $outCs) {
        $fail++; $failures.Add("[$label] stdout differs`n--- C ---`n$outC--- C# ---`n$outCs")
    } elseif ($callsC -ne $callsCs) {
        $fail++; $failures.Add("[$label] headless probe command lines differ`n--- C ---`n$callsC`n--- C# ---`n$callsCs")
    } elseif ($treeC -ne $treeCs) {
        $fail++; $failures.Add("[$label] resulting trees differ`n--- C ---`n$treeC`n--- C# ---`n$treeCs")
    } else {
        $pass++
        $spawns = @($outC -split "`n" | Where-Object { $_ -like 'spawn=*' }).Count
        $probes = @($callsC -split "`n" | Where-Object { $_ -ne '' }).Count
        Write-Host ("  ok  {0,-52} spawn={1} probes={2}" -f $label, $spawns, $probes)
        if ($ShowLogs) { ($outC -split "`n") | Where-Object { $_ -like 'log=*' -or $_ -like 'spawn=*' -or $_ -like 'status=*' } | ForEach-Object { Write-Host "        $_" } }
    }
}

# Per-scenario assertions on the C output: each flow must have taken its intended branch, not
# just the same branch on both sides.
$lfExpect = [ordered]@{
    'lf_renpy' = @('(?m)^spawn=shell\|<DIR>\\Game\.exe\|\|<DIR>$', '(?m)^post=/prefetch ')
    'lf_rpgm' = @('(?m)^post=/prefetch ', '(?m)^spawn=shell\|<DIR>\\Game\.exe\|\|<DIR>$')
    'lf_unknown' = @('(?m)^spawn=shell\|<DIR>\\Game\.exe\|\|<DIR>$')
    'lf_godot_loose_ok' = @(
        '(?m)^log=Godot: loose project detected; launching with runtime translation sidecar\.$',
        '(?m)^spawn=proc\|<DIR>\\Game\.exe\|"<DIR>\\Game\.exe" --path "<DIR>" --script "res://dst_godot_runtime\.gd" --language en\|<DIR>$',
        '(?m)^spawn=worker\|<LAUNCHER>\|"<LAUNCHER>" --godot-patch-worker "<DIR>"\|<ROOT>$')
    'lf_godot_loose_reject' = @(
        '(?m)^log=Godot: runtime sidecar preflight exited with code 3\.$',
        '(?m)^log=Godot: runtime-sidecar launch failed; falling back to normal game launch\.$',
        '(?m)^spawn=shell\|<DIR>\\Game\.exe\|\|<DIR>$')
    'lf_godot_export_nopack' = @(
        '(?m)^log=Godot: no patch pack yet; using the generic runtime translator for this launch\.$',
        '(?m)^log=Launching Godot export with runtime translator before static patch is ready: <DIR>\\Game\.exe$',
        '(?m)^spawn=proc\|<DIR>\\Game\.exe\|"<DIR>\\Game\.exe" --script "<DIR>\\dst_godot_runtime\.gd" --language en\|<DIR>$')
    'lf_godot_export_pack' = @(
        '(?m)^log=Godot: launching with external translation patch pack\.$',
        '(?m)^spawn=proc\|<DIR>\\dst_godot_patch\.exe\|"<DIR>\\dst_godot_patch\.exe" --script "res://dst_godot_runtime\.gd" --language en\|<DIR>$')
    'lf_godot_mainpack_rejected' = @(
        '(?m)^log=Godot: preserved an existing dst_godot_patch\.exe without a launcher ownership marker\.$',
        '(?m)^log=Godot: --main-pack probe received an explicit option-rejection diagnostic \(exit 1\)\.$',
        '(?m)^log=Godot: template rejects --main-pack; falling back to a normal launch without the translation patch pack\.$',
        '(?m)^spawn=shell\|<DIR>\\Game\.exe\|\|<DIR>$')
}
foreach ($name in $lfExpect.Keys) {
    # the first run's output: a second run would answer some probes from the preflight cache
    $out = $lfOut[$name]
    $missing = @($lfExpect[$name] | Where-Object { $out -notmatch $_ })
    if ($missing.Count) {
        $fail++; $failures.Add("[launch-flow/$name branch] expected $($missing -join ' , ') in`n$out")
    } else {
        $pass++; Write-Host ("  ok  {0,-52} {1} branch markers" -f "launch-flow/$name branch", $lfExpect[$name].Count)
    }
}

# --clear-cache-and-exit / --clear-cache: the shared cache file is deleted, a missing file
# counts as cleared, and a directory in its place is preserved with the same diagnostic.
$ccC = Join-Path $root 'cc_c'; Mkdir $ccC
$ccCs = Join-Path $root 'cc_cs'; Mkdir $ccCs
$ccExe = Join-Path $ccC (Split-Path -Leaf $CLauncher)
Copy-Item -LiteralPath $CLauncher -Destination $ccExe
function Invoke-ClearCache() {
    $c = Invoke-Detect $ccExe @('--clear-cache-and-exit')
    $cs = Invoke-Detect $CsLauncher @('--clear-cache', '--root', $ccCs)
    return @{
        C = $c
        Cs = $cs
        OutC = $c.Out -replace [regex]::Escape($ccC), '<ROOT>'
        OutCs = $cs.Out -replace [regex]::Escape($ccCs), '<ROOT>'
    }
}
$ccOk = $true
# 1. nothing to delete
$r = Invoke-ClearCache
$ccOk = (Check-ConfigPair 'clear-cache/missing' $r @('(?m)^cache=0\.0 MB$', '(?m)^result=1$')) -and $ccOk
# 2. a real cache file (1.5 MiB => "1.5 MB") is deleted
foreach ($d in @($ccC, $ccCs)) { Touch "$d\translation_memory_c.tsv" (New-Object byte[] (1024 * 1024 + 512 * 1024)) }
$r = Invoke-ClearCache
$ccOk = (Check-ConfigPair 'clear-cache/deleted' $r @('(?m)^cache=1\.5 MB$', '(?m)^cache2=0\.0 MB$', '(?m)^result=1$')) -and $ccOk
# 3. a directory in its place is preserved and reported
foreach ($d in @($ccC, $ccCs)) { Mkdir "$d\translation_memory_c.tsv" }
$r = Invoke-ClearCache
$ccOk = (Check-ConfigPair 'clear-cache/directory' $r @(
    '(?m)^log=清除缓存：目标路径是目录，已保留：<ROOT>\\translation_memory_c\.tsv$', '(?m)^result=0$')) -and $ccOk
foreach ($d in @($ccC, $ccCs)) { Remove-Item -LiteralPath "$d\translation_memory_c.tsv" -Recurse -Force }
if ($ccOk) {
    $pass++; Write-Host ("  ok  {0,-52} missing/deleted/directory" -f 'clear-cache/core')
}

# ---------------------------------------------------------------- window rendering parity
#
# ui.c 的窗口与绘制层：--ui-probe-and-exit vs --ui-probe. The window layer has no textual
# output to diff, so the probe turns the drawing itself into a comparable artifact: both
# launchers pin the DPI to 96, freeze the animation clock at tick 0, neutralise the two
# runtime-identity strings and take the server state from an argument, then create an
# invisible WS_POPUP main window whose client area is exactly <w>x<h>, paint the background
# and every child into one 32bpp DIB (owner-draw buttons through a synthesised
# DRAWITEMSTRUCT, standard controls through WM_PRINTCLIENT) and write an uncompressed BMP.
#
# Compared: the report (client size, dpi, the real runtime tag, and every control's id,
# client rect and text) line by line, and the BMP byte for byte. A one-component colour
# difference anywhere - a rounded corner, a gradient step, a glyph, the app icon - fails.
#
# Tick 0 is deliberate: sinf(-pi/2) and cosf(0) land on exactly -1.0f and 1.0f there, so the
# breathing dots and the hero beam do not depend on either runtime's libm last bit.
$uiRoot = Join-Path $root 'ui'; Mkdir $uiRoot
$uiExe = Join-Path $uiRoot (Split-Path -Leaf $CLauncher)
Copy-Item -LiteralPath $CLauncher -Destination $uiExe

# The version chip in the rail footer is painted from the build version: the C launcher gets
# it from -DDS_TRANSLATOR_VERSION, the C# one from the same VERSION file at build time. A
# mismatch here means the two binaries were not built from the same tree, which would also
# desync config\godot_preflight.ini, so assert it instead of normalising it away.
$uiScenarios = @(
    @{ Name = 'ui/1200x780-offline'; W = 1200; H = 780;  Alive = 0 },
    @{ Name = 'ui/1200x780-online';  W = 1200; H = 780;  Alive = 1 },
    @{ Name = 'ui/1080x700-min';     W = 1080; H = 700;  Alive = 1 },
    @{ Name = 'ui/1600x1000-wide';   W = 1600; H = 1000; Alive = 0 }
)
foreach ($s in $uiScenarios) {
    $bmpC = Join-Path $uiRoot ('c_{0}x{1}_{2}.bmp' -f $s.W, $s.H, $s.Alive)
    $bmpCs = Join-Path $uiRoot ('cs_{0}x{1}_{2}.bmp' -f $s.W, $s.H, $s.Alive)
    $c = Invoke-Detect $uiExe @('--ui-probe-and-exit', "$($s.W)", "$($s.H)", "$($s.Alive)", $bmpC)
    $cs = Invoke-Detect $CsLauncher @('--ui-probe', "$($s.W)", "$($s.H)", "$($s.Alive)", $bmpCs, '--root', $uiRoot)

    $problems = New-Object System.Collections.Generic.List[string]
    if ($c.Code -ne 0 -or $cs.Code -ne 0) { $problems.Add("exit codes differ or non-zero: C=$($c.Code) C#=$($cs.Code)") }
    if ($c.Out -ne $cs.Out) { $problems.Add("probe report differs`n--- C ---`n$($c.Out)--- C# ---`n$($cs.Out)") }
    if ($c.Out -notmatch '(?m)^result=1$') { $problems.Add("C probe did not render: $($c.Out)$($c.Err)") }
    if ($c.Out -notmatch "(?m)^client=$($s.W)\|$($s.H)$") { $problems.Add('client size is not the requested one') }
    if ($c.Out -notmatch '(?m)^dpi=96$') { $problems.Add('probe did not pin the DPI to 96') }
    if ($c.Out -notmatch '(?m)^alive=' + $s.Alive + '$') { $problems.Add('server state was not applied') }
    if (-not (Test-Path -LiteralPath $bmpC) -or -not (Test-Path -LiteralPath $bmpCs)) {
        $problems.Add('one of the launchers wrote no bitmap')
    } else {
        $hashC = (Get-FileHash -LiteralPath $bmpC -Algorithm SHA256).Hash
        $hashCs = (Get-FileHash -LiteralPath $bmpCs -Algorithm SHA256).Hash
        $expected = 54 + $s.W * $s.H * 4
        if ((Get-Item -LiteralPath $bmpC).Length -ne $expected) { $problems.Add('C bitmap is not the expected 32bpp size') }
        if ($hashC -ne $hashCs) {
            # locate the first differing pixel so a regression names a place on screen
            $ba = [System.IO.File]::ReadAllBytes($bmpC); $bb = [System.IO.File]::ReadAllBytes($bmpCs)
            $where = 'sizes differ'
            if ($ba.Length -eq $bb.Length) {
                for ($i = 54; $i -lt $ba.Length; $i++) {
                    if ($ba[$i] -ne $bb[$i]) {
                        $p = [math]::Floor(($i - 54) / 4)
                        $where = 'first differs at x={0} y={1}' -f ($p % $s.W), [math]::Floor($p / $s.W)
                        break
                    }
                }
            }
            $problems.Add("rendered client area differs ($where)`n  C  $hashC`n  C# $hashCs")
        }
    }
    if ($problems.Count -eq 0) {
        $pass++
        Write-Host ("  ok  {0,-52} {1} controls, pixel-identical" -f $s.Name, ([regex]::Matches($c.Out, '(?m)^ctl=')).Count)
    } else {
        $fail++
        $failures.Add("$($s.Name): " + ($problems -join "`n  "))
    }
}

# The rail footer tag and the hero subtitle are the one thing the two windows are meant to
# render differently - they name the implementation that painted the frame. The probe
# neutralises them so the rest of the pixels stay comparable, so the real values are asserted
# through their own diagnostic instead of being silently normalised away.
$idC = Invoke-Detect $uiExe @('--ui-identity-and-exit')
$idCs = Invoke-Detect $CsLauncher @('--ui-identity')
$idProblems = New-Object System.Collections.Generic.List[string]
if ($idC.Out -notmatch '(?m)^runtime_tag=C native runtime$') { $idProblems.Add("C rail tag: $($idC.Out)") }
if ($idC.Out -notmatch '(?m)^subtitle=C native - local cache') { $idProblems.Add("C subtitle: $($idC.Out)") }
if ($idCs.Out -notmatch '(?m)^runtime_tag=C# managed runtime$') { $idProblems.Add("C# rail tag: $($idCs.Out)") }
if ($idCs.Out -notmatch '(?m)^subtitle=C# managed - local cache') { $idProblems.Add("C# subtitle: $($idCs.Out)") }
if ($idC.Out -eq $idCs.Out) { $idProblems.Add('both binaries claim the same runtime identity') }
if ($idProblems.Count -eq 0) {
    $pass++; Write-Host ("  ok  {0,-52} declared divergence, neutral in the pixel diff" -f 'ui/runtime-identity')
} else {
    $fail++; $failures.Add('ui/runtime-identity: ' + ($idProblems -join "`n  "))
}

Stop-FakeServer

if ($ServerSmoke) {
    $busy = Get-NetTCPConnection -LocalPort 19999 -State Listen -ErrorAction SilentlyContinue
    if ($busy) {
        $fail++; $failures.Add("server smoke: port 19999 already has a listener (pid $($busy.OwningProcess)); refusing to stop it")
    } else {
        $s = Invoke-Detect $CsLauncher @('--server-smoke', '--root', $repo)
        $lingering = Get-Process dst_server, dst_server_cs -ErrorAction SilentlyContinue
        if ($s.Code -eq 0 -and $s.Out -match '(?m)^started=1$' -and $s.Out -match '(?m)^alive_after_stop=0$' -and -not $lingering) {
            $pass++; Write-Host "  ok  server smoke (start -> /health -> stop, no lingering process)"
        } else {
            $fail++
            $failures.Add("server smoke failed (exit $($s.Code), lingering=$([bool]$lingering))`n$($s.Out)$($s.Err)")
        }
    }
}

if (-not $KeepFixtures) {
    Remove-Item -LiteralPath $root -Recurse -Force -ErrorAction SilentlyContinue
    # the throwaway dotnet output dir is ours only when this script created it
    if ($csBuildDir) { Remove-Item -LiteralPath $csBuildDir -Recurse -Force -ErrorAction SilentlyContinue }
} else { Write-Host "fixtures kept at $root" }

Write-Host ""
$skipNote = if ($godotSkipped -gt 0) { ", $godotSkipped skipped ($godotSkip)" } else { '' }
Write-Host "launcher parity: $pass passed, $fail failed$skipNote"
foreach ($f in $failures) { Write-Host "FAIL $f" }
if ($fail -gt 0) { exit 1 }
exit 0
