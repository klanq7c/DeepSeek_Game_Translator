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

function Invoke-DetailsFault([string]$gamePath, [string]$cachePath, [string]$outputPath, [string]$mode) {
    $faultState = @{ Count=0; Mode=$mode; Path=$outputPath }
    # 只替换 JSON 行编码这个输出边界；扫描、缓存读取和临时文件发布仍走真实脚本。
    function ConvertTo-Json {
        [CmdletBinding()]
        param([Parameter(ValueFromPipeline=$true)]$InputObject, [int]$Depth=2, [switch]$Compress)
        process {
            if ($InputObject -is [Collections.IDictionary] -and $InputObject.Contains('schema_version')) {
                $faultState.Count++
                if ($faultState.Count -eq 2) {
                    if ($faultState.Mode -eq 'encode') { throw 'injected audit entry encoding failure' }
                    [IO.File]::WriteAllText($faultState.Path, 'concurrent owner', (New-Object Text.UTF8Encoding($false)))
                }
            }
            Microsoft.PowerShell.Utility\ConvertTo-Json -InputObject $InputObject -Depth $Depth -Compress:$Compress
        }
    }
    $failure = $null
    try { & $audit -GamePath $gamePath -LauncherPath $LauncherPath -CachePath $cachePath -DetailsPath $outputPath -AsJson | Out-Null }
    catch { $failure = $_.Exception.Message }
    Assert ($null -ne $failure -and $faultState.Count -ge 2) "details fault was not exercised: $mode / $failure"
    if ($mode -eq 'encode') {
        Assert ($failure -like '*injected audit entry encoding failure*' -and -not (Test-Path -LiteralPath $outputPath)) 'encoding fault published an incomplete report or lost its diagnostic'
    } else {
        Assert ([IO.File]::ReadAllText($outputPath) -eq 'concurrent owner') 'publish replaced a concurrently created target'
    }
    Assert (@(Get-ChildItem -LiteralPath ([IO.Path]::GetDirectoryName($outputPath)) -Filter '.dst-audit-*.tmp' -Force).Count -eq 0) 'failed detail export retained a partial file'
}

try {
    $tokens = $null; $parseErrors = $null
    $auditAst = [Management.Automation.Language.Parser]::ParseFile($audit, [ref]$tokens, [ref]$parseErrors)
    Assert ($parseErrors.Count -eq 0) 'audit script does not parse'
    $auditFunctions = $auditAst.FindAll({ param($node) $node -is [Management.Automation.Language.FunctionDefinitionAst] }, $false)
    foreach ($name in @('Find-RenPyMarkerEnd', 'Get-RenPyMarkerSignature')) {
        $function = @($auditFunctions | Where-Object Name -eq $name)
        Assert ($function.Count -eq 1) "missing production audit function: $name"
        Invoke-Expression $function[0].Extent.Text
    }
    $godotFunction = @($auditFunctions | Where-Object Name -eq 'Get-GodotMarkerSignature')
    Assert ($godotFunction.Count -eq 1) 'Godot audit parser is missing'
    $auditKnownText = [regex]::Match($godotFunction[0].Extent.Text, '(?s)\$known = @\((.*?)\)').Groups[1].Value
    $auditKnown = [string[]]@([regex]::Matches($auditKnownText, "'([^']+)'") | ForEach-Object { $_.Groups[1].Value })
    $godotProduction = [IO.File]::ReadAllText((Join-Path $repo 'native\src\launcher\godot_patch.c'))
    $productionKnownText = [regex]::Match($godotProduction, '(?s)static const char \*godot_next_bbcode\([^;]+?known\[\] = \{(.*?)\}').Groups[1].Value
    $productionKnown = [string[]]@([regex]::Matches($productionKnownText, '"([^"]+)"') | ForEach-Object { $_.Groups[1].Value })
    Assert ($productionKnown.Length -gt 30 -and [Linq.Enumerable]::SequenceEqual($productionKnown, $auditKnown)) 'Godot audit tag vocabulary drifted from the resource layer'
    # 直接源链接正式解析器，保护完整百分号字段内的不透明字符及坏前缀后的变量。
    foreach ($row in @(
        @('50%%(full) [player]', '50%%(完整) [player]', $false),
        @('%({)s {{literal}', '%({)s {{字面}', $false),
        @('%([)s [[literal]', '%([)s [[字面]', $false),
        @('%(outer %(inner)x) words [player]', '%(外层 %(inner)x) 文本 [player]', $false),
        @('[names[slots[0]]]', '[names[slots[1]]]', $true),
        @('Hello %(player)s.', '你好%(player。', $true)
    )) {
        $different = (Get-RenPyMarkerSignature $row[0]) -cne (Get-RenPyMarkerSignature $row[1])
        Assert ($different -eq $row[2]) "RenPy marker parser boundary failed: $($row[0])"
    }
    $badPrefix = '%(' * 2048
    $badText = $badPrefix + 'Keep [player].'
    $boundaryCache = [int[]]::new($badText.Length)
    Assert ((Find-RenPyMarkerEnd $badText 1 '(' ')' $boundaryCache) -eq -1) 'unclosed mapping prefix was treated as complete'
    Assert (@($boundaryCache | Where-Object { $_ -eq -1 }).Count -eq 2048) 'nested missing boundaries were not memoized'
    Assert ((Get-RenPyMarkerSignature $badText) -cne (Get-RenPyMarkerSignature ($badPrefix + '保留文字。'))) 'long malformed prefix swallowed the remaining variable'

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

    # 全量清单独立于示例上限；真实扫描保留短按钮/物品名，跳过地图事件内部名。
    $manifestGame = Join-Path $tempRoot 'manifest-rpgm'
    Write-Text (Join-Path $manifestGame 'www\index.html') '<script src="js/main.js"></script>'
    Write-Text (Join-Path $manifestGame 'www\js\main.js') ''
    Write-Text (Join-Path $manifestGame 'www\js\rpg_core.js') ''
    Write-Text (Join-Path $manifestGame 'Game.exe') ''
    Write-Text (Join-Path $manifestGame 'www\data\System.json') '{"gameTitle":"x"}'
    Write-Text (Join-Path $manifestGame 'www\data\Items.json') '[{"name":"Bedroom Key"}]'
    $manifestSources = @('Save', 'Back', 'QUIT', 'Welcome {name}.', 'Echo candidate sentence.', 'Malformed cache candidate sentence.',
        "Original line one.`r`nOriginal line two.") + @(0..129 | ForEach-Object { 'Follow-up dialogue item {0:000}.' -f $_ })
    $commands = @($manifestSources | ForEach-Object { @{code=101; parameters=@()}; @{code=401; parameters=@($_)} })
    $manifestMap = @{events=@($null,@{name='Internal Route Signal';pages=@(@{list=$commands})})}
    Write-Text (Join-Path $manifestGame 'www\data\Map001.json') (ConvertTo-Json -InputObject $manifestMap -Depth 10 -Compress)
    $manifestCache = Join-Path $tempRoot 'manifest-cache.tsv'
    Write-CacheRow $manifestCache 'Save' '保存'
    Write-CacheRow $manifestCache 'Save' 'Save'
    Write-CacheRow $manifestCache 'QUIT' '退出'
    Write-CacheRow $manifestCache 'Welcome {name}.' '欢迎。'
    Write-CacheRow $manifestCache 'Echo candidate sentence.' 'Simplified Chinese translation: Echo candidate sentence.'
    Write-CacheRow $manifestCache "Original line one.`r`nOriginal line two." "原始第一行。`r`n原始第二行。"
    Write-CacheRow $manifestCache 'Bedroom Key' '卧室钥匙'
    [IO.File]::AppendAllText($manifestCache, ([Convert]::ToBase64String($utf8.GetBytes('Malformed cache candidate sentence.')) + "`t!not-base64!`n"), [Text.Encoding]::ASCII)
    [IO.File]::AppendAllText($manifestCache, ([Convert]::ToBase64String($utf8.GetBytes('Malformed cache candidate sentence.')) + "`t/w==`n!invalid-key!`t" + [Convert]::ToBase64String($utf8.GetBytes('无效键对应的候选。')) + "`n"), [Text.Encoding]::ASCII)
    $manifestHash = Get-Sha256 $manifestCache
    $manifestGameBefore = @(Get-ChildItem -LiteralPath $manifestGame -Recurse -File | ForEach-Object { $_.FullName + ':' + (Get-Sha256 $_.FullName) })
    $detailsPath = Join-Path $tempRoot 'entries.jsonl'
    $manifestJson = & $audit -GamePath $manifestGame -LauncherPath $LauncherPath -CachePath $manifestCache -MaxExamples 0 -DetailsPath $detailsPath -AsJson
    $manifestReport = ($manifestJson -join "`n") | ConvertFrom-Json
    $records = @([IO.File]::ReadAllLines($detailsPath, $utf8) | ForEach-Object { ConvertFrom-Json -InputObject $_ })
    $expectedSources = [string[]]@($manifestSources + 'Bedroom Key')
    [Array]::Sort($expectedSources, [StringComparer]::Ordinal)
    $actualSources = [string[]]@($records | ForEach-Object { $_.source })
    Assert ([Linq.Enumerable]::SequenceEqual($expectedSources, $actualSources)) "entry manifest keys differ: missing=$(@($expectedSources | Where-Object { $_ -cnotin $actualSources }) -join ' | '); extra=$(@($actualSources | Where-Object { $_ -cnotin $expectedSources }) -join ' | ')"
    Assert ($records.Count -eq $manifestReport.scanned_unique_texts -and $manifestReport.details_entries -eq $records.Count) 'entry manifest count differs from full scanned denominator'
    Assert ($manifestReport.missing_examples.Count -eq 0 -and $manifestReport.marker_examples.Count -eq 0) 'zero example limit was ignored'
    Assert ($manifestReport.cache_translation -eq 5 -and $manifestReport.marker_consistent_translation -eq 4 -and $manifestReport.followup_texts -eq $records.Count - 4) 'presence and marker-consistent counts were conflated'
    $back = @($records | Where-Object source -eq 'Back')[0]
    $damaged = @($records | Where-Object source -eq 'Welcome {name}.')[0]
    $echo = @($records | Where-Object source -eq 'Echo candidate sentence.')[0]
    $malformed = @($records | Where-Object source -eq 'Malformed cache candidate sentence.')[0]
    $saved = @($records | Where-Object source -eq 'Save')[0]
    Assert ($back.translation_source -eq 'none' -and $null -eq $back.translation -and $back.issues -contains 'missing_translation') 'missing source was not actionable'
    Assert ($damaged.translation_source -eq 'cache' -and $damaged.marker_review -eq 'structural' -and $damaged.issues -contains 'structural_marker_mismatch') 'damaged candidate lost translation provenance/reason'
    Assert ($echo.rejected_cache_candidates -eq 1 -and $echo.issues -contains 'rejected_cache_candidate' -and $echo.issues -contains 'missing_translation') 'rejected candidate disappeared from followup'
    Assert ($malformed.invalid_cache_candidates -eq 2 -and $malformed.issues -contains 'invalid_cache_candidate' -and $malformed.translation_source -eq 'none' -and $manifestReport.invalid_cache_rows_observed -eq 3) 'base64/UTF-8 errors lost their source key or an invalid key was attributed to the preceding row'
    Assert ($saved.rejected_cache_candidates -eq 1 -and $saved.translation -eq '保存' -and $saved.issues.Count -eq 0) 'late rejected cache row concealed an earlier valid translation'
    Assert (@($records | Where-Object { $_.schema_version -ne 1 -or $_.engine -notlike '*RPG*' -or $_.discovery -notcontains 'launcher_prefetch' }).Count -eq 0) 'manifest provenance is incomplete'
    Assert ((Get-Sha256 $manifestCache) -eq $manifestHash) 'manifest generation modified cache'
    $detailsHash = Get-Sha256 $detailsPath
    $refusedExisting = $false
    try { & $audit -GamePath $manifestGame -LauncherPath $LauncherPath -CachePath $manifestCache -DetailsPath $detailsPath -AsJson | Out-Null }
    catch { $refusedExisting = $true }
    Assert ($refusedExisting -and (Get-Sha256 $detailsPath) -eq $detailsHash) 'existing detail file was overwritten'
    $unsafeDetails = Join-Path $manifestGame 'www\data\..\audit.jsonl'
    $refusedGame = $false
    try { & $audit -GamePath $manifestGame -LauncherPath $LauncherPath -CachePath $manifestCache -DetailsPath $unsafeDetails -AsJson | Out-Null }
    catch { $refusedGame = $true }
    Assert ($refusedGame -and -not (Test-Path -LiteralPath $unsafeDetails)) 'detail export wrote into game directory'
    Invoke-DetailsFault $manifestGame $manifestCache (Join-Path $tempRoot 'failed-entries.jsonl') 'encode'
    Invoke-DetailsFault $manifestGame $manifestCache (Join-Path $tempRoot 'raced-entries.jsonl') 'publish'
    $junction = Join-Path $tempRoot 'game-alias'
    New-Item -ItemType Junction -Path $junction -Target $manifestGame -ErrorAction Stop | Out-Null
    try {
        $refusedAlias = $false
        try { & $audit -GamePath $manifestGame -LauncherPath $LauncherPath -CachePath $manifestCache -DetailsPath (Join-Path $junction 'unsafe.jsonl') -AsJson | Out-Null }
        catch { $refusedAlias = $true }
        Assert ($refusedAlias -and -not (Test-Path -LiteralPath (Join-Path $manifestGame 'unsafe.jsonl'))) 'reparse alias bypassed game directory protection'
        $aliasDetails = Join-Path $tempRoot 'alias-source.jsonl'
        $refusedAliasSource = $false
        try { & $audit -GamePath $junction -LauncherPath $LauncherPath -CachePath $manifestCache -DetailsPath $aliasDetails -AsJson | Out-Null }
        catch { $refusedAliasSource = $true }
        Assert ($refusedAliasSource -and -not (Test-Path -LiteralPath $aliasDetails)) 'game source alias concealed its protected tree'
    } finally {
        # 只删除本测试创建的链接本身，不递归经过链接目标。
        [IO.Directory]::Delete($junction)
    }
    $manifestGameAfter = @(Get-ChildItem -LiteralPath $manifestGame -Recurse -File | ForEach-Object { $_.FullName + ':' + (Get-Sha256 $_.FullName) })
    Assert (($manifestGameBefore -join "`n") -eq ($manifestGameAfter -join "`n") -and (Get-Sha256 $manifestCache) -eq $manifestHash) 'detail export or failure paths modified game/cache'
    Write-Host "PASS audit manifest: $($records.Count) exact source keys, zero examples, complete provenance/reasons, encode/publish failure, existing/game/reparse protection"

    $renpy = Join-Path $tempRoot 'renpy'
    Write-Text (Join-Path $renpy 'Game.exe') ''
    Write-Text (Join-Path $renpy 'game\script.rpy') "label start:`n    `"Hello {player}.`"`n"
    $renpyJson = & $audit -GamePath $renpy -LauncherPath $LauncherPath -CachePath $cache -AsJson
    $renpyReport = ($renpyJson -join "`n") | ConvertFrom-Json
    Assert ($renpyReport.engine -eq "Ren'Py" -and $renpyReport.missing_translation -ge 1) 'RenPy scan was not reported'

    # Ren'Py 标签、插值和 Python format 模板必须分别保留；字面双括号允许翻译。
    $longTarget = 'route_' + ('a' * 120)
    $renpyPairs = @(
        @('Hello [player].', '你好。'),
        @('Value {} now.', '当前数值。'),
        @('Total {0:.2f} now.', '总计。'),
        @("{a=$longTarget}Open the route.{/a}", "{a=changed_$longTarget}打开路线。{/a}"),
        @('{b}{i}Nested text.{/i}{/b}', '{i}{b}嵌套文字。{/b}{/i}'),
        @('Welcome %(player(name))s.', '欢迎。'),
        @('Width %(score)03d now.', '宽度%(score)s。'),
        @('Press [[Continue] and {{brace}.', '按 [[继续] 和 {{括号}。'),
        @('Values {} {0:.2f} {name} [names[slot]] now.', '数值 {} {0:.2f} {name} [names[slot]]。'),
        @('Values %(score(round(1)))03d %()s %% now.', '数值 %(score(round(1)))03d %()s %% 。'),
        @('Original unfinished { fragment.', '原本未完成 { 片段。'),
        @('Chance 50%(about half), [player].', '概率50%(大约一半)，[player]。'),
        @('Partial { hint [player].', '片段 { 提示。'),
        @('Partial %(hint [score].', '片段%(提示。'),
        @('Partial %(hint [name].', '片段%(提示[name]。'),
        @('Show <Example> text.', '显示 <示例> 文字。')
    )
    $renpyStory = "label start:`n"
    foreach ($pair in $renpyPairs) {
        $renpyStory += '    ' + (ConvertTo-Json -InputObject $pair[0] -Compress) + "`n"
        Write-CacheRow $cache $pair[0] $pair[1]
    }
    $renpyScript = Join-Path $renpy 'game\script.rpy'
    Write-Text $renpyScript $renpyStory
    $renpyScriptHash = Get-Sha256 $renpyScript
    $renpyCacheHash = Get-Sha256 $cache
    $renpyMarkerJson = & $audit -GamePath $renpy -LauncherPath $LauncherPath -CachePath $cache -MaxExamples 100 -AsJson
    $renpyMarkerReport = ($renpyMarkerJson -join "`n") | ConvertFrom-Json
    Assert ($renpyMarkerReport.cache_translation -eq $renpyPairs.Count) 'RenPy marker fixture did not reach the actual source/cache audit chain'
    $expectedSuspects = @('Hello [player].', 'Value {} now.', 'Total {0:.2f} now.',
        "{a=$longTarget}Open the route.{/a}", '{b}{i}Nested text.{/i}{/b}',
        'Welcome %(player(name))s.', 'Width %(score)03d now.',
        'Partial { hint [player].', 'Partial %(hint [score].')
    $actualSuspects = @($renpyMarkerReport.marker_examples | ForEach-Object { $_.source })
    Assert (@($expectedSuspects | Where-Object { $_ -cnotin $actualSuspects }).Count -eq 0 -and
            @($actualSuspects | Where-Object { $_ -cnotin $expectedSuspects }).Count -eq 0) "RenPy suspect keys differ: expected $($expectedSuspects -join ' | '); got $($actualSuspects -join ' | ')"
    Assert ($renpyMarkerReport.structural_marker_mismatch_suspects -eq 9) "RenPy marker audit expected 9 suspects, got $($renpyMarkerReport.structural_marker_mismatch_suspects): interpolation, format fields, long tags, order or literal handling failed"
    Assert ($renpyMarkerReport.timing_control_mismatch_suspects -eq 0) 'RenPy structural markers were classified as pacing controls'
    Assert ($renpyMarkerReport.scope -match '不执行模板.*不推断旧式替换设置') 'RenPy report overclaimed template or old-substitution validation'
    Assert ((Get-Sha256 $renpyScript) -eq $renpyScriptHash -and (Get-Sha256 $cache) -eq $renpyCacheHash) 'RenPy audit changed source or translation memory'

    $unity = Join-Path $tempRoot 'unity'
    Write-Text (Join-Path $unity 'MyGame.exe') ''
    Write-Text (Join-Path $unity 'MyGame_Data\Managed\marker.txt') ''
    Write-Text (Join-Path $unity 'BepInEx\Translation\zh-CN\Text\existing.txt') "Hello adventurer=你好，冒险者`nUntranslated sentence.=`n"
    $unityJson = & $audit -GamePath $unity -LauncherPath $LauncherPath -CachePath $cache -AsJson
    $unityReport = ($unityJson -join "`n") | ConvertFrom-Json
    Assert ($unityReport.engine -eq 'Unity' -and $unityReport.resource_translation -eq 1 -and
            $unityReport.missing_translation -ge 1) 'Unity resource translations were not separated from misses'

    $unityPairs = @(
        @('<link="quest>original">Continue now</link>', '<link="quest>changed">继续</link>'),
        @("<link='quest>original'>Continue here</link>", "<link='quest>changed'>继续</link>"),
        @('<color=red>Danger</color><color=green>Safe</color>', '<color=green>危险</color><color=red>安全</color>'),
        @('Current score: {0}', '当前得分'),
        @('Current value: {0,8:N2}', '当前数值：{0,8:N1}'),
        @('<link="quest>original">Continue safely</link>', '<link="quest>original">安全继续</link>'),
        @('<link="quest>{0}<color=red>">Open safely</link>', '<link="quest>{0}<color=red>">安全打开</link>'),
        @('Literal {{Example}} text', '字面 {{示例}} 文字'),
        @('<noparse>Literal <b> sample</noparse>', '<noparse>字面 <粗体> 示例</noparse>'),
        @('First {0} then {1}', '先 {1} 后 {0}'),
        @('Wrapped score: {{{0}}}', '包裹得分：{{{1}}}'),
        @('Wrapped value: {{{0,8:N2}}}', '包裹数值：{{{0,8:N1}}}'),
        @('Wrapped safe: {{{0}}}', '安全包裹：{{{0}}}'),
        @('Double literal: {{0}}', '双括号字面量：{{1}}'),
        @('<noparse>Literal <b title="quoted > {0}</noparse> after', '<noparse>字面 <b title="引号 > {0}</noparse> 之后'),
        @('<noparse>Literal <b title="unterminated</noparse> tail', '<noparse>字面 <i title="未闭合</noparse> 末尾'),
        @('< noparse>Spaced <b title="literal< / noparse > tail', '< noparse>空白 <i title="字面< / noparse > 末尾'),
        @('< noparse>Missing close <b title="literal< / noparse > tail', '< noparse>缺少结束 <i title="字面 末尾')
    )
    $unityText = Join-Path $unity 'BepInEx\Translation\zh-CN\Text\existing.txt'
    $unityLines = foreach ($pair in $unityPairs) {
        $pair[0].Replace('\', '\\').Replace('=', '\=') + '=' + $pair[1].Replace('\', '\\').Replace('=', '\=')
    }
    Write-Text $unityText ($unityLines -join "`n")
    $unityHash = Get-Sha256 $unityText
    $unityMarkerJson = & $audit -GamePath $unity -LauncherPath $LauncherPath -CachePath $cache -MaxExamples 100 -AsJson
    $unityMarkerReport = ($unityMarkerJson -join "`n") | ConvertFrom-Json
    $expectedUnity = @('<link="quest>original">Continue now</link>', "<link='quest>original'>Continue here</link>",
        '<color=red>Danger</color><color=green>Safe</color>', 'Current score: {0}', 'Current value: {0,8:N2}',
        'Wrapped score: {{{0}}}', 'Wrapped value: {{{0,8:N2}}}',
        '< noparse>Missing close <b title="literal< / noparse > tail')
    $actualUnity = @($unityMarkerReport.marker_examples | ForEach-Object { $_.source })
    Assert ($unityMarkerReport.resource_translation -eq $unityPairs.Count) 'Unity marker pairs did not reach the resource audit'
    Assert (@($expectedUnity | Where-Object { $_ -cnotin $actualUnity }).Count -eq 0 -and
        @($actualUnity | Where-Object { $_ -cnotin $expectedUnity }).Count -eq 0) "Unity suspect keys differ: $($actualUnity -join ' | ')"
    Assert ($unityMarkerReport.structural_marker_mismatch_suspects -eq 8 -and $unityMarkerReport.timing_control_mismatch_suspects -eq 0) 'Unity marker classification failed'
    Assert ((Get-Sha256 $unityText) -eq $unityHash) 'Unity audit changed translation resources'
    $resourceDetails = Join-Path $tempRoot 'unity-entries.jsonl'
    & $audit -GamePath $unity -LauncherPath $LauncherPath -CachePath $cache -MaxExamples 0 -DetailsPath $resourceDetails -AsJson | Out-Null
    $resourceRecords = @([IO.File]::ReadAllLines($resourceDetails, $utf8) | ForEach-Object { ConvertFrom-Json -InputObject $_ })
    Assert ($resourceRecords.Count -eq $unityPairs.Count -and @($resourceRecords | Where-Object { $_.translation_source -ne 'resource' -or $_.discovery -notcontains 'resource_import' }).Count -eq 0) 'resource import provenance was confused with cache hits'

    $godot = Join-Path $tempRoot 'godot'
    Write-Text (Join-Path $godot 'project.godot') "config_version=5`n"
    $emptyDetails = Join-Path $tempRoot 'empty-entries.jsonl'
    $emptyJson = & $audit -GamePath $godot -LauncherPath $LauncherPath -CachePath $cache -DetailsPath $emptyDetails -AsJson
    $emptyReport = ($emptyJson -join "`n") | ConvertFrom-Json
    Assert ($emptyReport.scanned_unique_texts -eq 0 -and $emptyReport.details_entries -eq 0 -and (Get-Item -LiteralPath $emptyDetails).Length -eq 0) 'empty scan invented a record or missed an empty manifest'
    Write-Text (Join-Path $godot 'room.tscn') 'text = "Hello Godot player, welcome to this room."'
    $godotJson = & $audit -GamePath $godot -LauncherPath $LauncherPath -CachePath $cache -AsJson
    $godotReport = ($godotJson -join "`n") | ConvertFrom-Json
    Assert ($godotReport.engine -eq 'Godot' -and $null -eq $godotReport.original_game_exe_path -and
            $godotReport.missing_translation -ge 1) 'Godot project scan or missing exe state was misreported'

    # 已有资源门禁按顺序比较 BBCode；审计不能把同数量的错误顺序排序后放行。
    $godotPairs = @(
        @('[color=red]Danger now[/color][color=green]Safe now[/color]', '[color=green]现在危险[/color][color=red]现在安全[/color]', $true),
        @('You have %d coins.', '你拥有金币。', $true),
        @('Welcome %s traveler.', '欢迎旅人。', $true),
        @('[custom_fx strength=2]Hello traveler[/custom_fx]', '你好旅人', $true),
        @('[custom_fx]Hello traveler[/custom_fx]', '你好旅人[/custom_fx]', $true),
        @('[font_size=20]Large words[/font_size]', '[font_size=12]大字[/font_size]', $true),
        @('[url=buy][b]Buy potion[/b][/url] for %d coins.', '[url=buy][b]购买药水[/b][/url]，%d金币。', $false),
        @('Choose [a longer phrase] from [100] entries.', '从 [100] 项中选择 [较长的短语]。', $false),
        @('Press [B] for Back.', '按 [返回键] 返回。', $false),
        @('Hint [b option] before continuing.', '继续前提示 [选项]。', $false),
        @('Use [lb] and [rb] to show brackets.', '使用 [lb] 和 [rb] 显示括号。', $false),
        @('Player %s has %d coins.', '金币 %d，玩家 %s。', $false),
        @('Choose [a hint] before leaving.', '离开前选择 [提示。', $true),
        @('Ratio %1.2f remains.', '比例 %1.3f 保持。', $true),
        @('Literal %% remaining.', '字面百分号剩余。', $true),
        @('Original unfinished [ fragment.', '原本未闭合 [ 片段。', $false)
    )
    $scene = "[gd_scene format=3]`n[node name=`"Menu`" type=`"RichTextLabel`"]`n"
    $godotExpected = @()
    foreach ($pair in $godotPairs) {
        $scene += 'text = ' + (ConvertTo-Json -InputObject $pair[0] -Compress) + "`n"
        Write-CacheRow $cache $pair[0] $pair[1]
        if ($pair[2]) { $godotExpected += $pair[0] }
    }
    $godotScene = Join-Path $godot 'room.tscn'
    Write-Text $godotScene $scene
    $godotHash = Get-Sha256 $godotScene
    $godotCacheHash = Get-Sha256 $cache
    $godotMarkerJson = & $audit -GamePath $godot -LauncherPath $LauncherPath -CachePath $cache -MaxExamples 100 -AsJson
    $godotMarkerReport = ($godotMarkerJson -join "`n") | ConvertFrom-Json
    $godotActual = @($godotMarkerReport.marker_examples | ForEach-Object { $_.source })
    Assert ($godotMarkerReport.cache_translation -eq $godotPairs.Count) "Godot pairs did not reach source/cache audit: scanned=$($godotMarkerReport.scanned_unique_texts) cached=$($godotMarkerReport.cache_translation) missing=$($godotMarkerReport.missing_examples -join ' | ')"
    Assert (@($godotExpected | Where-Object { $_ -cnotin $godotActual }).Count -eq 0 -and
        @($godotActual | Where-Object { $_ -cnotin $godotExpected }).Count -eq 0) "Godot suspect keys differ: $($godotActual -join ' | ')"
    Assert ($godotMarkerReport.structural_marker_mismatch_suspects -eq 9 -and $godotMarkerReport.timing_control_mismatch_suspects -eq 0) 'Godot marker counts failed'
    Assert ((Get-Sha256 $godotScene) -eq $godotHash -and (Get-Sha256 $cache) -eq $godotCacheHash) 'Godot audit modified resource/cache'
    Write-Host "PASS Godot audit: $($godotPairs.Count) pairs, $($godotExpected.Count) exact suspects, tag vocabulary matches resource layer"
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
