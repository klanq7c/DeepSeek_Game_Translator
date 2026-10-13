[CmdletBinding()]
param([string]$LauncherPath = '')

$ErrorActionPreference = 'Stop'
$repo = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
if (-not $LauncherPath) { $LauncherPath = Join-Path $repo 'ds游戏翻译器.exe' }
$audit = Join-Path $repo 'scripts\audit_game_translation.ps1'
$tempRoot = Join-Path $env:TEMP ('dst_game_audit_' + [Guid]::NewGuid().ToString('N'))
$utf8 = New-Object System.Text.UTF8Encoding($false)

function Write-Text([string]$path, [string]$value) {
    [IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($path)) | Out-Null
    [IO.File]::WriteAllText($path, $value, $utf8)
}
function Write-CacheRow([string]$path, [string]$key, [string]$value) {
    $row = [Convert]::ToBase64String($utf8.GetBytes($key)) + "`t" +
           [Convert]::ToBase64String($utf8.GetBytes($value)) + "`n"
    [IO.File]::AppendAllText($path, $row, [Text.Encoding]::ASCII)
}
function Assert([bool]$condition, [string]$message) {
    if (-not $condition) { throw $message }
}
function Get-Sha256([string]$path) {
    $sha = [Security.Cryptography.SHA256]::Create()
    $stream = [IO.File]::OpenRead($path)
    try { [BitConverter]::ToString($sha.ComputeHash($stream)).Replace('-', '') }
    finally { $stream.Dispose(); $sha.Dispose() }
}

try {
    $game = Join-Path $tempRoot 'rpgm'
    Write-Text (Join-Path $game 'www\index.html') '<script src="js/main.js"></script>'
    Write-Text (Join-Path $game 'www\js\main.js') ''
    Write-Text (Join-Path $game 'www\js\rpg_core.js') ''
    Write-Text (Join-Path $game 'Game.exe') ''
    Write-Text (Join-Path $game 'www\data\System.json') '{"gameTitle":"x"}'
    Write-Text (Join-Path $game 'www\data\Map001.json') '{"events":[null,{"pages":[{"list":[{"code":401,"parameters":["Hello \\V[1]."]},{"code":401,"parameters":["Welcome {name}."]},{"code":401,"parameters":["Wait here\\."]},{"code":401,"parameters":["Welcome \\N[1]."]},{"code":401,"parameters":["Grow \\{loud\\} now."]},{"code":401,"parameters":["Echo source sentence."]},{"code":401,"parameters":["One more untranslated sentence."]}]}]}]}'
    $cache = Join-Path $tempRoot 'cache.tsv'
    Write-CacheRow $cache 'Hello \V[1].' '你好 \V[1]。'
    Write-CacheRow $cache 'Welcome {name}.' '欢迎。'
    Write-CacheRow $cache 'Wait here\.' '在这里等。'
    Write-CacheRow $cache 'Welcome \N[1].' '欢迎 \n[1]。'
    Write-CacheRow $cache 'Grow \{loud\} now.' '现在 \{大声\}。'
    Write-CacheRow $cache 'Echo source sentence.' 'Simplified Chinese translation: Echo source sentence.'
    [IO.File]::AppendAllText($cache, "broken cache row`n", [Text.Encoding]::ASCII)
    [IO.File]::AppendAllText($cache, ('x' * (8 * 1024 * 1024 + 1)) + "`n", [Text.Encoding]::ASCII)
    Write-CacheRow $cache 'One more untranslated sentence.' '还有一句已翻译的文本。'
    $beforeGame = Get-ChildItem -LiteralPath $game -Recurse -File | ForEach-Object { $_.FullName + ':' + (Get-Sha256 $_.FullName) }
    $beforeCache = Get-Sha256 $cache

    $json = & $audit -GamePath $game -LauncherPath $LauncherPath -CachePath $cache -AsJson
    $report = ($json -join "`n") | ConvertFrom-Json
    Assert ($report.engine -like '*RPG*') "wrong engine: $($report.engine)"
    Assert ($report.original_game_exe_path -eq (Join-Path $game 'Game.exe')) 'reported wrong game exe'
    Assert ($report.cache_translation -eq 6) "wrong cache count after overlong row: $($report.cache_translation)"
    Assert ($report.missing_translation -ge 1) 'missing translation was not counted'
    Assert ($report.marker_mismatch_suspects -eq 2) "marker mismatch or RPGM case handling failed: $($report.marker_mismatch_suspects)"
    Assert ($report.structural_marker_mismatch_suspects -eq 1 -and $report.timing_control_mismatch_suspects -eq 1) 'structural and pacing controls were not separated'
    Assert ($report.exe_launch -eq '未实测' -and $report.glyph_and_layout -eq '未实机验证') 'report overclaimed visual verification'
    Assert ($report.invalid_cache_rows_observed -eq 2) 'broken or overlong cache row was not counted'
    Assert ($report.rejected_candidate_cache_rows -eq 1 -and $report.missing_examples -contains 'Echo source sentence.') 'prompt echo was incorrectly counted as a cached translation'
    Assert (-not $report.scan_limit_reached) 'tiny fixture incorrectly reached scan cap'
    Assert (-not (($report.scan_logs -join ' ') -match '后台排队')) 'dry-run log was misreported as real queue work'
    Assert ((Get-Sha256 $cache) -eq $beforeCache) 'audit modified cache'
    $afterGame = Get-ChildItem -LiteralPath $game -Recurse -File | ForEach-Object { $_.FullName + ':' + (Get-Sha256 $_.FullName) }
    Assert (($beforeGame -join "`n") -eq ($afterGame -join "`n")) 'audit modified game tree'

    $renpy = Join-Path $tempRoot 'renpy'
    Write-Text (Join-Path $renpy 'Game.exe') ''
    Write-Text (Join-Path $renpy 'game\script.rpy') "label start:`n    `"Hello {player}.`"`n"
    $renpyJson = & $audit -GamePath $renpy -LauncherPath $LauncherPath -CachePath $cache -AsJson
    $renpyReport = ($renpyJson -join "`n") | ConvertFrom-Json
    Assert ($renpyReport.engine -eq "Ren'Py" -and $renpyReport.missing_translation -ge 1) 'RenPy scan was not reported'

    $unity = Join-Path $tempRoot 'unity'
    Write-Text (Join-Path $unity 'MyGame.exe') ''
    Write-Text (Join-Path $unity 'MyGame_Data\Managed\marker.txt') ''
    Write-Text (Join-Path $unity 'BepInEx\Translation\zh-CN\Text\existing.txt') "Hello adventurer=你好，冒险者`nUntranslated sentence.=`n"
    $unityJson = & $audit -GamePath $unity -LauncherPath $LauncherPath -CachePath $cache -AsJson
    $unityReport = ($unityJson -join "`n") | ConvertFrom-Json
    Assert ($unityReport.engine -eq 'Unity' -and $unityReport.resource_translation -eq 1 -and
            $unityReport.missing_translation -ge 1) 'Unity resource translations were not separated from misses'

    $godot = Join-Path $tempRoot 'godot'
    Write-Text (Join-Path $godot 'project.godot') "config_version=5`n"
    Write-Text (Join-Path $godot 'room.tscn') 'text = "Hello Godot player, welcome to this room."'
    $godotJson = & $audit -GamePath $godot -LauncherPath $LauncherPath -CachePath $cache -AsJson
    $godotReport = ($godotJson -join "`n") | ConvertFrom-Json
    Assert ($godotReport.engine -eq 'Godot' -and $null -eq $godotReport.original_game_exe_path -and
            $godotReport.missing_translation -ge 1) 'Godot project scan or missing exe state was misreported'
    Write-Host 'PASS game audit: RPG Maker, RenPy, Unity, Godot; cache, markers, exact exe, no game/cache writes'
} finally {
    # 只删除本测试以 GUID 创建的专属临时目录。
    $resolvedTemp = [IO.Path]::GetFullPath($tempRoot)
    $tempParent = [IO.Path]::GetFullPath($env:TEMP).TrimEnd('\') + '\'
    if ($resolvedTemp.StartsWith($tempParent, [StringComparison]::OrdinalIgnoreCase) -and
        [IO.Path]::GetFileName($resolvedTemp) -match '^dst_game_audit_[0-9a-f]{32}$' -and
        [IO.Directory]::Exists($resolvedTemp)) {
        [IO.Directory]::Delete($resolvedTemp, $true)
    }
}
