# Source-level regression checks for Unity plugin text classification rules.

$ErrorActionPreference = "Stop"
$script:Pass = 0
$script:Fail = 0
$script:Errors = @()

function It([string]$name, [scriptblock]$body) {
    try {
        & $body
        $script:Pass++
        Write-Host ("  PASS  " + $name) -ForegroundColor Green
    } catch {
        $script:Fail++
        $script:Errors += "$name : $_"
        Write-Host ("  FAIL  " + $name + " :: " + $_) -ForegroundColor Red
    }
}

function Assert-True($value, $msg) {
    if (-not $value) { throw $msg }
}

function Assert-False($value, $msg) {
    if ($value) { throw $msg }
}

function Normalize-RequestText([string]$text) {
    if ([string]::IsNullOrWhiteSpace($text)) { return "" }
    return (($text.Trim() -split '\s+') -join ' ')
}

$repo = Split-Path -Parent $PSScriptRoot
$srcPath = Join-Path $repo "payloads\UnityTranslator\src\DeepSeekTranslator.cs"
$src = Get-Content -LiteralPath $srcPath -Raw
$unusedPrototypePath = Join-Path $repo "payloads\UnityTranslator\src\UnityTranslator"

if ($src -notmatch 'RichTextTagRegex\s*=\s*new Regex\("((?:\\.|[^"])*)"') {
    throw "Could not find RichTextTagRegex pattern in $srcPath"
}

$pattern = [regex]::Unescape($Matches[1])
$regex = [regex]::new($pattern, [System.Text.RegularExpressions.RegexOptions]::IgnoreCase)

function Get-VisibleText([string]$text) {
    return Normalize-RequestText (($regex.Replace($text, " ") -replace "\u200b|\ufeff", ""))
}

$allowedLatinResidue = [System.Collections.Generic.HashSet[string]]::new([System.StringComparer]::OrdinalIgnoreCase)
@("AI","API","CPU","DLC","FPS","GPU","HP","ID","MP","NPC","OK","RAM","UI","VR","VRAM","AMD","Intel","NVIDIA","GeForce","Ryzen","Windows","Direct","DirectX","Unity","Steam","DeepSeek","BepInEx","TMP") | ForEach-Object { [void]$allowedLatinResidue.Add($_) }

$commonCapitalizedEnglishWords = [System.Collections.Generic.HashSet[string]]::new([System.StringComparer]::OrdinalIgnoreCase)
@("A","An","And","Are","As","At","Be","But","By","Can","Do","Does","For","From","Good","Here","How","If","In","Into","Is","It","Let","Like","Maybe","No","Not","Now","Of","On","Or","Our","Some","That","The","Then","There","This","To","Very","We","Well","What","When","Where","Which","Who","Why","With","You","Your") | ForEach-Object { [void]$commonCapitalizedEnglishWords.Add($_) }

function Contains-Cjk([string]$text) {
    if ([string]::IsNullOrEmpty($text)) { return $false }
    foreach ($ch in $text.ToCharArray()) {
        $code = [int][char]$ch
        if (($code -ge 0x4e00 -and $code -le 0x9fff) -or ($code -ge 0x3000 -and $code -le 0x303f) -or ($code -ge 0xff00 -and $code -le 0xffef)) {
            return $true
        }
    }
    return $false
}

function Looks-LikeRuntimeStatusText([string]$visibleText) {
    if ([string]::IsNullOrWhiteSpace($visibleText)) { return $true }
    $text = Normalize-RequestText $visibleText
    if ([string]::IsNullOrWhiteSpace($text)) { return $true }
    if ($text -match '^v\s*\d+(?:\.\d+){1,4}(?:[-+._][A-Za-z0-9]+)*$') { return $true }
    if ($text -match '^(?:RAM|VRAM|VR|Window|Screen|Display|Resolution|FPS|CPU|GPU)\s*:') { return $true }
    if ($text -match '\b(?:\d{3,5}|#+)x(?:\d{3,5}|#+)@(?:\d{1,4}|#+)\s*Hz(?:\[[^\]]+\])?\b') { return $true }
    return @(
        "preloading",
        "preloading content",
        "loading level",
        "loading scene",
        "loading assets",
        "loading asset",
        "entering"
    ) -contains $text.ToLowerInvariant()
}

function Is-LikelyProtectedLatinTerm([string]$word) {
    if ([string]::IsNullOrWhiteSpace($word)) { return $false }
    if ($word -cmatch '^[A-Z]+$' -or $allowedLatinResidue.Contains($word)) { return $true }
    if ($word -match '\d') { return $true }
    if ($word.Substring([Math]::Min(1, $word.Length)) -cmatch '[A-Z]') { return $true }
    if ($word[0] -cmatch '[A-Z]' -and $word.Substring([Math]::Min(1, $word.Length)) -cmatch '[a-z]' -and -not $commonCapitalizedEnglishWords.Contains($word)) { return $true }
    return $false
}

function Latin-ResidueMatchesSourceWord([string]$residueWord, [string]$sourceWord) {
    if ([string]::Equals($sourceWord, $residueWord, [System.StringComparison]::OrdinalIgnoreCase)) { return $true }
    $shorter = if ($residueWord.Length -le $sourceWord.Length) { $residueWord } else { $sourceWord }
    $longer = if ($residueWord.Length -le $sourceWord.Length) { $sourceWord } else { $residueWord }
    if ($shorter.Length -lt 3 -or ($longer.Length - $shorter.Length) -gt 2) { return $false }
    return $longer.StartsWith($shorter, [System.StringComparison]::OrdinalIgnoreCase)
}

function Is-AllowedLatinResidue([string]$word, [string]$originalVisibleText) {
    if ([string]::IsNullOrWhiteSpace($word)) { return $false }
    if ($word -cmatch '^[A-Z]+$' -or $allowedLatinResidue.Contains($word)) { return $true }
    if ([string]::IsNullOrWhiteSpace($originalVisibleText)) { return $false }
    foreach ($m in [regex]::Matches($originalVisibleText, '[A-Za-z]{2,}')) {
        $sourceWord = $m.Value
        if ((Latin-ResidueMatchesSourceWord $word $sourceWord) -and (Is-LikelyProtectedLatinTerm $sourceWord)) {
            return $true
        }
    }
    return $false
}

function Has-SuspiciousEnglishResidue([string]$original, [string]$translated) {
    $visibleOriginal = Get-VisibleText $original
    $visibleTranslated = Get-VisibleText $translated
    if ($visibleOriginal.Length -lt 12 -or -not (Contains-Cjk $visibleTranslated)) { return $false }
    foreach ($m in [regex]::Matches($visibleTranslated, '[A-Za-z]{2,}')) {
        if (-not (Is-AllowedLatinResidue $m.Value $visibleOriginal)) { return $true }
    }
    return $false
}

function Normalize-LooseLatinText([string]$text) {
    if ([string]::IsNullOrWhiteSpace($text)) { return "" }
    $builder = [System.Text.StringBuilder]::new()
    $lastWasSpace = $true
    foreach ($ch in $text.ToCharArray()) {
        $c = [char]$ch
        if ($c -eq [char]0x2019 -or $c -eq [char]0x2018) { $c = [char]"'" }
        $code = [int]$c
        $isAsciiLetterOrDigit = (($code -ge 65 -and $code -le 90) -or ($code -ge 97 -and $code -le 122) -or ($code -ge 48 -and $code -le 57))
        if ($isAsciiLetterOrDigit) {
            [void]$builder.Append(([char]::ToLowerInvariant($c)))
            $lastWasSpace = $false
        } elseif (-not $lastWasSpace) {
            [void]$builder.Append(' ')
            $lastWasSpace = $true
        }
    }
    return $builder.ToString().Trim()
}

function Get-MixedResidueLatinTail([string]$text) {
    $visible = Get-VisibleText $text
    if ([string]::IsNullOrWhiteSpace($visible)) { return "" }
    $seenCjk = $false
    for ($i = 0; $i -lt $visible.Length; $i++) {
        $ch = [string]$visible[$i]
        if (Contains-Cjk $ch) {
            $seenCjk = $true
        } elseif ($seenCjk -and $ch -cmatch '[A-Za-z]') {
            $tail = $visible.Substring($i).Trim()
            if (-not (Ends-WithSentenceBoundary $tail)) { return "" }
            return Normalize-LooseLatinText $tail
        }
    }
    return ""
}

function Get-LeadingCjkSignature([string]$text) {
    $visible = Get-VisibleText $text
    if ([string]::IsNullOrWhiteSpace($visible)) { return "" }
    $builder = [System.Text.StringBuilder]::new()
    foreach ($ch in $visible.ToCharArray()) {
        $s = [string]$ch
        if ($s -cmatch '[A-Za-z]') { break }
        if (Contains-Cjk $s) { [void]$builder.Append($ch) }
    }
    return $builder.ToString()
}

function Looks-LikeMixedTranslationResidue([string]$text) {
    $visible = Get-VisibleText $text
    if ([string]::IsNullOrWhiteSpace($visible) -or -not (Contains-Cjk $visible)) { return $false }
    $latinWords = 0
    $latinChars = 0
    foreach ($m in [regex]::Matches($visible, '[A-Za-z]{2,}')) {
        $word = $m.Value
        if ($word -cmatch '^[A-Z]+$' -or $allowedLatinResidue.Contains($word)) { continue }
        $latinWords++
        $latinChars += $word.Length
    }
    return ($latinWords -ge 2 -or $latinChars -ge 10)
}

function Ends-WithSentenceBoundary([string]$text) {
    if ([string]::IsNullOrWhiteSpace($text)) { return $false }
    $t = $text.Trim()
    if ($t.Length -eq 0) { return $false }
    $last = [string]$t[$t.Length - 1]
    return @(".", "!", "?", "~", ">", ")", "]", """", "'", ":", ";", ([string][char]0x2026)) -contains $last
}

function Is-TrailingStatLine([string]$visibleLine) {
    if ($visibleLine -match '^[-+]?\d[\d.,]*\s*[A-Za-z%]{0,16}$') { return $true }
    return ($visibleLine.Length -le 48) -and ($visibleLine -match '\d') -and (-not (Ends-WithSentenceBoundary $visibleLine))
}

function Strip-TrailingStatLines([string]$text) {
    $end = $text.Length
    while ($end -gt 0) {
        $lineStart = $text.LastIndexOf([char]"`n", $end - 1)
        $line = (Get-VisibleText ($text.Substring($lineStart + 1, $end - ($lineStart + 1)))).Trim()
        if ($line.Length -eq 0 -or (Is-TrailingStatLine $line)) {
            if ($lineStart -lt 0) { return $text }
            $end = $lineStart
            continue
        }
        break
    }
    if ($end -eq $text.Length) { return $text }
    return $text.Substring(0, $end)
}

function Looks-LikeTypewriterFragment([string]$text) {
    if ([string]::IsNullOrWhiteSpace($text)) { return $false }
    $text = Strip-TrailingStatLines $text
    $visible = Get-VisibleText $text
    if ([string]::IsNullOrWhiteSpace($visible)) { return $false }
    if ($text -match '^\s{2,}' -or $text -match '\s{8,}') { return $true }
    $t = $visible.TrimStart("^").Trim()
    if ($t.Length -gt 0 -and $t[0] -eq "<" -and -not $t.Contains(">")) { return $true }
    if ($t.Length -ge 8 -and $t -match '^[a-z]\)') { return $true }
    if ($t.Length -ge 4 -and $t -cmatch '^[a-z]{1,2}\s+') { return $true }
    if ($t.Length -ge 14 -and $t[0] -cmatch '[a-z]' -and $t -match '\s') { return $true }
    $hasLower = $t -cmatch '[a-z]'
    $hasPunctuation = $t -match '[\.\!\?,;:]'
    $hasEllipsis = $t.Contains([string][char]0x2026) -or $t.Contains("...")
    if ($t.Length -ge 8 -and $hasLower -and $t.EndsWith(",")) { return $true }
    if ($t.Length -ge 8 -and $hasLower -and $hasEllipsis -and -not (Ends-WithSentenceBoundary $t)) { return $true }
    if ($t.Length -ge 14 -and $hasLower -and -not (Ends-WithSentenceBoundary $t) -and $t -match '\b[A-Za-z]{1,3}$') { return $true }
    if ($t.Length -ge 10 -and $hasLower -and $hasPunctuation -and -not (Ends-WithSentenceBoundary $t) -and $t -match '\b(?:[A-Za-z]{1,3}|a|an|the|to|of|for|from|into|with|no|not)$') { return $true }
    if ($t.Length -ge 42 -and $hasLower -and $hasPunctuation -and -not (Ends-WithSentenceBoundary $t)) { return $true }
    if ($t.Length -ge 24 -and $hasLower -and -not (Ends-WithSentenceBoundary $t) -and $t -match '\b(?:a|an|the|to|of|for|and|or|but|with|from|into|about|what|where|there|already|want|just|think|feel|feels|practicing|practice|people|world|field)$') { return $true }
    return $false
}

Write-Host ""
Write-Host "=== Unity text rules ==="

It "Ren'Py/VN-style angle narration remains visible" {
    $text = "<i>< The station is busy... voices, footsteps, signal tones. >"
    $visible = Get-VisibleText $text
    Assert-True ($visible.Contains("The station is busy")) "angle narration was stripped as a tag"
    Assert-False ($regex.IsMatch("< The station is busy... >")) "plain angle narration should not match rich text regex"
    $plainThought = "<I have nothing to do at the console right now.>"
    $plainVisible = Get-VisibleText $plainThought
    Assert-True ($plainVisible.Contains("I have nothing to do")) "capital-I angle narration was stripped as an italic tag"
    Assert-False ($regex.IsMatch($plainThought)) "capital-I angle narration should not match italic rich text"
}

It "Unity rich-text tags are still protected" {
    $text = '<color=red>Hello</color><br/><i>World</i>'
    $matches = $regex.Matches($text)
    Assert-True ($matches.Count -ge 5) "expected color/br/i rich text tags to match"
    $visible = Get-VisibleText $text
    Assert-True ($visible -eq "Hello World") "rich text visible text mismatch: '$visible'"

    $tmpColor = '<#FF8800>Warning</color>'
    $tmpMatches = $regex.Matches($tmpColor)
    Assert-True ($tmpMatches.Count -eq 2) "expected TMP color shorthand tags to match"
    $tmpVisible = Get-VisibleText $tmpColor
    Assert-True ($tmpVisible -eq "Warning") "TMP color shorthand visible text mismatch: '$tmpVisible'"
}

It "Unity skips runtime status text before queueing translation" {
    Assert-True (Looks-LikeRuntimeStatusText "RAM: 31964 MB") "RAM telemetry should not be translated"
    Assert-True (Looks-LikeRuntimeStatusText "VR: Not active") "VR telemetry should not be translated"
    Assert-True (Looks-LikeRuntimeStatusText "Window: ####x####@##Hz[###dpi]") "dynamic window placeholder should not be translated"
    Assert-True (Looks-LikeRuntimeStatusText "Screen: 2560x1440@200Hz") "screen mode telemetry should not be translated"
    Assert-True (Looks-LikeRuntimeStatusText "v 0.4.8-sr") "build version should not be translated"
    Assert-True (Looks-LikeRuntimeStatusText "Preloading Content") "loading status should not be translated"
    Assert-True (Looks-LikeRuntimeStatusText "Loading Level") "loading level status should not be translated"
    Assert-True (Looks-LikeRuntimeStatusText "ENTERING") "scene-transition status should not be translated"

    Assert-False (Looks-LikeRuntimeStatusText "Crystal Transit Hub") "location titles should remain translatable"
    Assert-False (Looks-LikeRuntimeStatusText "The trainee is calm, focused, and ready to start the next exercise") "dialogue should remain translatable"
    Assert-False (Looks-LikeRuntimeStatusText "Load Game") "ordinary menu labels should remain translatable"

    Assert-True ($src.Contains("LooksLikeRuntimeStatusText(visibleText)")) "ShouldSkipText must call the runtime-status classifier"
    Assert-True ($src.Contains("RuntimeVersionTextRegex")) "version text guard must stay source-level"
    Assert-True ($src.Contains("RuntimeStatusPrefixRegex")) "telemetry prefix guard must stay source-level"
    Assert-True ($src.Contains("RuntimeResolutionTextRegex")) "resolution guard must stay source-level"
}

It "Typewriter fragments are rejected before translation/cache" {
    $ellipsis = [string][char]0x2026
    Assert-True (Looks-LikeTypewriterFragment "                                                                      m practicing.") "split tail fragment should be rejected"
    Assert-True (Looks-LikeTypewriterFragment "m practicing.") "normalized single-letter tail fragment should be rejected"
    Assert-True (Looks-LikeTypewriterFragment "ut I also want to meet new people.") "normalized two-letter tail fragment should be rejected"
    Assert-True (Looks-LikeTypewriterFragment ("Alright{0} fr" -f $ellipsis)) "short ellipsis fragment should be rejected"
    Assert-True (Looks-LikeTypewriterFragment ("Alright{0} from no" -f $ellipsis)) "short word-tail fragment should be rejected"
    Assert-True (Looks-LikeTypewriterFragment "<i") "dangling rich-text opener should be rejected"
    Assert-True (Looks-LikeTypewriterFragment "Ready or not, ") "trailing comma typewriter fragment should be rejected"
    Assert-True (Looks-LikeTypewriterFragment "You always think ab") "unfinished trailing word fragment should be rejected"
    Assert-True (Looks-LikeTypewriterFragment "Ready or not, I just want to find the archive. There must already be a tea") "unfinished long sentence should be rejected"
    Assert-True (Looks-LikeTypewriterFragment "                                                                                                            ne seems to know exactly where they're going.") "leading-space fragment should be rejected"
    Assert-False (Looks-LikeTypewriterFragment ("Alright{0} from this point, the task belongs to us." -f $ellipsis)) "complete ellipsis sentence should not be rejected"
    Assert-False (Looks-LikeTypewriterFragment "Ready or not, I just want to find the archive. There must already be a team practicing.") "complete sentence should not be rejected"
    Assert-False (Looks-LikeTypewriterFragment ("It Feels strange... like we{0}re stepping into another world." -f ([string][char]0x2019))) "complete dialogue line with ellipsis should not be rejected"
    Assert-False (Looks-LikeTypewriterFragment "Mira") "short character name should not be rejected"
}

It "Dialogue ending in a stat/cost line is final text, not a typewriter fragment" {
    $plain = "Excellent. I promise this plan will work well.`n-200 credits"
    Assert-False (Looks-LikeTypewriterFragment $plain) "dialogue with trailing cost line must stay translatable"

    $rich = "<size=150%><color=#E749B0>Guide</color></size>`nExcellent. I promise this plan will work well.`n<color=#C24B41>-200 credits</color>"
    Assert-False (Looks-LikeTypewriterFragment $rich) "rich-text dialogue with trailing cost line must stay translatable"

    $multiStat = "You confirm the plan with a handshake. It feels like a fresh start.`n+10 rep`n-200 credits"
    Assert-False (Looks-LikeTypewriterFragment $multiStat) "multiple trailing stat lines must be stripped before judging"

    Assert-True (Looks-LikeTypewriterFragment "Excellent. I promise this plan will wor") "mid-reveal fragment must still be rejected"
    Assert-True (Looks-LikeTypewriterFragment "Excellent. I promise this plan will wor`n-200 credits") "mid-reveal prose above a stat line must still be rejected"
    Assert-False (Looks-LikeTypewriterFragment "400 credits") "standalone stat label keeps its original classification"
    Assert-True ($src.Contains("StripTrailingStatLines(text)")) "Unity source must strip trailing stat lines before fragment judgment"

    # Payment summary blocks: multi-word stat lines ("Base payment 45",
    # "+7 credits from tags Friendly+++ Relaxing++") must also strip.
    $payment = "That was a precise demonstration, and I will record it as a success.`nBase payment 45`n+1 credits from skills`n+7 credits from tags Friendly+++ Relaxing++`n+53 credits`n+3 reputation"
    Assert-False (Looks-LikeTypewriterFragment $payment) "dialogue with a payment summary block must stay translatable"

    $requirement = "Escort the visitor to the exit and confirm the checklist is complete.`n3+ Charisma 5+ Confidence"
    Assert-False (Looks-LikeTypewriterFragment $requirement) "ritual entry with a requirement line must stay translatable"
}

It "TMP extended rich-text tags are stripped from visible text" {
    $lq = [string][char]0x201C
    $rq = [string][char]0x201D
    $chip = "<mark=#4CD94C39 padding=${lq}15, 2, 15, 2${rq}>Friendly</mark>"
    Assert-True ($regex.IsMatch($chip)) "mark tag with curly-quoted padding must match"
    Assert-True ((Get-VisibleText $chip) -eq "Friendly") "mark tag must strip to its inner text"
    Assert-True ((Get-VisibleText "<nobr>Shielded</nobr>") -eq "Shielded") "nobr tags must strip"
    Assert-True ((Get-VisibleText "<line-height=110%>Level up</line-height>") -eq "Level up") "line-height tags must strip"
    Assert-True ($regex.IsMatch('<sprite name="dot">')) "sprite tags must match"
    Assert-True ($regex.IsMatch('<link="codex_archivist">')) "link tags must match"

    $panelVisible = Get-VisibleText "<nobr><mark=#FF4C4C39 padding=${lq}15, 2${rq}>Winged</mark></nobr>`nWhen another character levels up, the Archivist gains 1 xp."
    Assert-False ($panelVisible -match '\b(?:mark|nobr|padding)\b') "tag words must not leak into visible text"

    Assert-False ($regex.IsMatch("< The station is busy... voices, footsteps, signal tones. >")) "plain angle narration must stay unmatched"
    Assert-False ($regex.IsMatch("<Mark looked at me, waiting.>")) "narration starting with a tag-like name must stay unmatched"
    Assert-False ($regex.IsMatch("<I have nothing to do at the console right now.>")) "capital-I angle narration must stay unmatched"

    # TMP renders the full tag set natively; only UGUI Text may filter tags,
    # otherwise translated text loses <mark> highlight chips.
    Assert-True ($src.Contains("string text2 = ((component is Text) ? SanitizeRichTextForUnityText(text) : SanitizeTranslationArtifacts(text));")) "TMP components must keep unsupported-by-UGUI tags like <mark>"
}

It "Unity repairs mixed translated typewriter residue" {
    $mixed = "濂藉惂... from this point, the task belongs to us."
    Assert-True (Looks-LikeMixedTranslationResidue $mixed) "Chinese prefix plus English sentence tail must be treated as mixed residue"
    Assert-True ((Get-MixedResidueLatinTail $mixed) -eq "from this point the task belongs to us") "mixed residue tail should normalize for cache matching"
    Assert-True ((Get-LeadingCjkSignature $mixed) -eq "濂藉惂") "Chinese prefix signature should be available for safe repair"
    Assert-False (Looks-LikeMixedTranslationResidue "浠庣幇鍦ㄥ紑濮嬶紝浣犲皢鍔犲叆Delta缁勩€?) "single protected-ish story term should not trigger repair"
    Assert-False (Looks-LikeMixedTranslationResidue "Mara绗戜簡銆?) "one proper name beside Chinese should not trigger repair"

    Assert-True ($src.Contains("TryRepairMixedTranslatedText(currentComponentText, out var originalText, out var repaired)")) "targeted cache path must repair mixed residue"
    Assert-True ($src.Contains("TryRepairMixedTranslatedText(text, out var originalText, out var repaired)")) "TMP scanner/cache path must repair mixed residue"
    Assert-True ($src.Contains("TryRepairMixedTranslatedText(text2, out var originalText2, out var repaired2)")) "UGUI scanner/cache path must repair mixed residue"
    Assert-True ($src.Contains("TryRepairMixedTranslatedText(value, out var originalText, out var repaired)")) "synchronous text setters must repair mixed residue"
    Assert-True ($src.Contains("TryRepairMixedTranslatedText(text, out var repairedOriginal, out var repairedTranslated)")) "queued async apply must accept repairable mixed current text"
    Assert-True ($src.Contains("ClearMixedRepairMemo()")) "mixed repair miss cache must be invalidated when translations are imported/stored"
}

It "Unity server cache preload does not force full local cache rewrites" {
    Assert-True ($src.Contains("private readonly HashSet<string> _localCacheKeys")) "plugin must track which cache entries belong in the local game cache"
    Assert-True ($src.Contains("ServerCachePreload")) "full server cache preload must be explicitly configurable"
    Assert-True ($src.Contains("defaultValue: false, `"Full server cache preload")) "full server cache preload must be off by default"
    Assert-True ($src.Contains("Full server cache preload disabled")) "disabled full preload should be visible in logs"
    Assert-True ($src.Contains("markImportedAsLocal: true")) "healthy local JSON cache imports must be marked for future persistence"
    Assert-True ($src.Contains('sourceLabel, bool persistAfterImport, bool markImportedAsLocal = false')) "server imports must be able to stay memory-only"
    Assert-True ($src.Contains('"server cache dump", persistAfterImport: false')) "server dump preload must not immediately persist all server entries"
    Assert-True ($src.Contains('"server export", persistAfterImport: false')) "legacy server export preload must not immediately persist all server entries"
    Assert-True ($src.Contains("foreach (string localCacheKey in _localCacheKeys)")) "local cache snapshot must write only local/game-used keys"
    Assert-True ($src.Contains("MaxFontWarmupCacheEntries")) "font warmup must cap local cache scanning"
    Assert-False ($src.Contains("foreach (string value in _cache.Values)")) "font warmup must not scan every server-preloaded cache value"
    Assert-True ($src.Contains("TryMarkAppliedCacheKeyForPersist(rawText, translated)")) "visible TMP cache hits should be promoted to the local game cache"
    Assert-True ($src.Contains("TryMarkAppliedCacheKeyForPersist(text2, translated2)")) "visible UGUI cache hits should be promoted to the local game cache"
}

It "Unity scene warmup actively requests visible UI misses" {
    Assert-True ($src.Contains("WarmupVisibleCandidatesAsync")) "scene warmup async path must exist"
    Assert-True ($src -match '(?s)private int BeginSceneWarmupGeneration\(\).{0,500}?StartManagedCoroutine\(SceneWarmupCoroutine\(result\)\);') "scene warmup generation must actually start the warmup coroutine"
    Assert-True ($src.Contains('await WarmupTextsAsync(hashSet, "ui")')) "visible UI warmup misses must actively request translations"
    Assert-False ($src.Contains("ApplyWarmupTranslations(uiCandidates, new Dictionary<string, string>(StringComparer.Ordinal), generation);")) "scene warmup must not apply an empty translation map"
    Assert-True ($src.Contains("WaitForWarmupServerReadyAsync")) "warmup requests must wait briefly for the local server before giving up"
    Assert-True ($src -match '(?s)private async Task<Dictionary<string, string>> WarmupTextsAsync.*?if \(!await WaitForWarmupServerReadyAsync\(\)\)') "warmup batches must not trip server backoff before /health is ready"
    Assert-True ($src.Contains("tcpClient.ReceiveTimeout = timeoutMs;")) "raw HTTP timeout parameter must apply to reads"
    Assert-True ($src.Contains("networkStream.ReadTimeout = timeoutMs;")) "raw HTTP timeout parameter must apply to the response stream"
    Assert-False ($src.Contains("DeepPrefetchLoopCoroutine")) "old unstarted deep-prefetch coroutine path should not remain as misleading dead code"
    Assert-False ($src.Contains("ProcessDeepPrefetchQueueCoroutine")) "deep-prefetch should use the active async tick path only"
}

It "Unity plugin source does not compile unused modular prototype code" {
    Assert-False (Test-Path -LiteralPath $unusedPrototypePath) "unused UnityTranslator prototype tree must not be present in the SDK-style project"
    Assert-False ($src.Contains("TranslatorEngine")) "real plugin entry must not depend on the removed prototype engine"
    Assert-False ($src.Contains("TranslationScheduler")) "real plugin entry must not depend on the removed prototype scheduler"
    Assert-False ($src.Contains("PrefetchPlanner")) "real plugin entry must not depend on the removed prototype prefetch planner"
    Assert-False ($src.Contains("MainThreadApplyQueue")) "real plugin entry must not depend on the removed prototype apply queue"
    Assert-False ($src.Contains("SceneCacheApplyCoroutine")) "scene cache apply must use the active tick path, not the removed coroutine path"
    Assert-False ($src.Contains("DeferredServerCacheSyncCoroutine")) "server cache sync must use the active async path, not the removed coroutine path"
    Assert-False ($src.Contains("TranslateAndApply(")) "old direct translate/apply path must not bypass debounce and batch dispatch"
    Assert-False ($src.Contains("ShouldUseTmpOverlayForText")) "old per-text overlay heuristic must not linger after component-aware overlay decisions"
    Assert-False ($src.Contains("LoadBundledFont")) "old bundled-font helper did not load the file it read and should not linger"
    Assert-False ($src.Contains("HiddenEntry")) "old hidden-awaiting state must not linger after source-visible pending behavior"
    Assert-False ($src.Contains("_hiddenAwaitingTranslation")) "old hidden-awaiting map must not linger after source-visible pending behavior"
    Assert-False ($src.Contains("HideTmpAwaitingTranslation")) "old pending-blanking helper must not linger after source-visible pending behavior"
}

It "Unity alpha rescue sweep is wired into the active frame pump" {
    Assert-True ($src -match '(?s)private void PumpOnce\(string source\).*?RunOverlayValidationTick\(\);\s*RunAlphaSweepTick\(\);') "alpha rescue sweep must run from the active pump"
    Assert-True ($src.Contains("realtimeSinceStartup - _lastAlphaSweepRealtime < AlphaSweepIntervalSeconds")) "alpha sweep cadence must use its named constant"
    Assert-True ($src.Contains("realtimeSinceStartup + SceneCacheApplyIntervalSeconds")) "scene cache apply cadence must use its named constant"
}

It "Unity source treats typewriter suspects as settle-delayed, not hard-rejected" {
    # Classification only buys a longer debounce settle; final-but-odd-looking
    # prose (e.g. no trailing punctuation) must still translate.
    Assert-True ($src.Contains("GetTextSettleDelaySeconds(value.Text)")) "debounce must wait by text type"
    Assert-True ($src -match '(?s)private static float GetTextSettleDelaySeconds\(string text\)\s*\{\s*if \(LooksLikeTypewriterFragment\(text\)\)\s*\{\s*return 0\.9f;') "fragment suspects must use the long settle delay"
    Assert-False ($src.Contains("ContainsCjk(text) || LooksLikeTypewriterFragment(text) || ShouldSkipText(text)")) "debounce queue entry must not hard-reject fragments"
    Assert-False ($src.Contains("if (LooksLikeTypewriterFragment(item2.Text))")) "debounce flush must not hard-reject settled fragments"
    Assert-False ($src -match '(?s)private bool TryGetLocalTranslation\(string text, out string translated\).{0,400}?LooksLikeTypewriterFragment') "local cache lookup must not reject fragment-looking final text"
    Assert-True ($src.Contains("if (LooksLikeTypewriterFragment(key))")) "bulk cache imports must still filter legacy fragment keys"
    Assert-True ($src.Contains("text2[0] == '<' && text2.IndexOf('>') < 0")) "dangling rich-text fragments must be rejected"
    Assert-True ($src.Contains('text2.IndexOf(''\u2026'') >= 0')) "ellipsis fragments must be rejected"
}

It "Prose without trailing punctuation must stay translatable end to end" {
    # Synthetic dialogue with no closing punctuation. The classifier may
    # flag it, but no pipeline gate may permanently drop it (source asserts
    # above); the hooks route suspects into the debounce queue instead.
    $unpunctuated = "So.. Errh... I'm not sure I fully understand. During field practice the signals are risky and I should retreat, but inside the training room I can inspect them safely? Or am I missing something"
    Assert-True (Looks-LikeTypewriterFragment $unpunctuated) "classifier may still flag unpunctuated prose as a suspect"
    Assert-True ($src -match '(?s)if \(LooksLikeTypewriterFragment\(value\)\)\s*\{\s*_instance\.QueueDebouncedTextRequest') "sync hooks must route suspects into the debounce queue"
}

It "Sanitize repairs must not corrupt valid tags" {
    # Hex colors ending in B (<color=#CFC59B>) must survive the bare-tag
    # repair: matching "B>" as a bare <b> tag injected a stray '<' and broke
    # the color tag into <color=#CFC59<B>.
    if ($src -notmatch 'BareOpeningRichTextRegex\s*=\s*new Regex\("((?:\\.|[^"])*)"') { throw "BareOpeningRichTextRegex not found" }
    $bareOpen = [regex]::new([regex]::Unescape($Matches[1]), [System.Text.RegularExpressions.RegexOptions]::IgnoreCase)
    $name = "<size=150%><color=#CFC59B>Aster</color></size>"
    Assert-True ($bareOpen.Replace($name, '<$1') -eq $name) "hex colors ending in B must not be 'repaired'"
    Assert-True ($bareOpen.Replace("<color=#8956FB>Signal</color>", '<$1') -eq "<color=#8956FB>Signal</color>") "hex colors ending in B inside UI strings must not be 'repaired'"
    Assert-True ($bareOpen.IsMatch("color=red>oops")) "genuinely bare color tags should still be repaired"

    # <size=150%> must strip fully in the plain-text path (no stray "%>").
    if ($src -notmatch 'LooseSizeOpenFragmentRegex\s*=\s*new Regex\("((?:\\.|[^"])*)"') { throw "LooseSizeOpenFragmentRegex not found" }
    $looseSize = [regex]::new([regex]::Unescape($Matches[1]))
    Assert-True ($looseSize.Replace("<size=150%>text", "") -eq "text") "percent size tags must strip without residue"
    Assert-True ($looseSize.Replace("<size=42>text", "") -eq "text") "plain size tags must still strip"
}

It "TMP overlay has a visible-render guard" {
    Assert-True ($src.Contains("GetComponentInParent<Canvas>()")) "overlay must not hide TMP when it cannot render on a Canvas"
    Assert-True ($src.Contains("SetParent(val6, false)")) "overlay should be parented beside the TMP component for UI draw order"
    Assert-True ($src.Contains("SetAllDirty()")) "overlay graphic must be marked dirty after text assignment"
}

It "TMP overlay and rich-text restore preserve original colors" {
    Assert-True ($src.Contains("private static Color GetTmpOverlayDisplayColor")) "TMP overlay must compute display color from the current component color"
    Assert-True ($src.Contains("Color val7 = GetTmpOverlayDisplayColor(tmpColor, tmpOverlayState)")) "overlay color must not be frozen to the first cached color"
    Assert-False ($src.Contains("Color val7 = (tmpOverlayState.hasOriginalColor ? tmpOverlayState.originalColor : tmpColor);")) "overlay must not reuse stale originalColor as display color"
    Assert-True ($src.Contains("RestoreOuterRichTextWrapper")) "lost outer color rich-text wrappers must be restored after translation"
    Assert-True ($src.Contains("OriginalText = text ?? string.Empty")) "rich-text restore needs the original text to rebuild missing wrappers"
    Assert-True ($src.Contains("RestoreOuterRichTextWrapper(text, payload?.OriginalText)")) "protected-text restore must use the captured original text"
    Assert-True ($src.Contains("PrepareTranslatedTextForUGUIText")) "UGUI Text writes must preserve color rich text when supported"
    Assert-True ($src.Contains("value = PrepareTranslatedTextForUGUIText(__instance, text, rawText);")) "UGUI sync translation must keep source color wrappers"
    Assert-True ($src.Contains("val.text = PrepareTranslatedTextForUGUIText(val, translated, originalText ?? translated, preserveRichText);")) "UGUI async/cached translation must keep source color wrappers"
    Assert-True ($src.Contains("PrepareTranslatedTextForComponent(component, translated, sourceForFormatting)")) "TMP formatting must use the original source text when restoring wrappers"
}

It "TMP translation releases typewriter visibility limits" {
    Assert-True ($src.Contains("RevealTmpText")) "TMP translations must release maxVisible counters"
    Assert-True ($src.Contains('"maxVisibleCharacters"')) "maxVisibleCharacters must be handled"
    Assert-True ($src.Contains('"maxVisibleWords"')) "maxVisibleWords must be handled"
    Assert-True ($src.Contains('"maxVisibleLines"')) "maxVisibleLines must be handled"
}

It "Missing geometric glyphs are replaced instead of forcing the overlay path" {
    # 鈼?鈻?路 missing from packaged CJK atlases must be substituted before
    # apply; otherwise the component falls back to the UGUI overlay, which
    # cannot render <mark> highlight chips.
    Assert-True ($src -match "(?s)TmpPunctuationFallbackChars\s*=\s*new char\[\d+\][^;]*'\\u25cf'") "U+25CF (鈼? must be in the punctuation fallback set"
    Assert-True ($src -match "(?s)TmpPunctuationFallbackChars\s*=\s*new char\[\d+\][^;]*'\\u25a1'") "U+25A1 (鈻? must be in the punctuation fallback set"
    Assert-True ($src -match "(?s)TmpPunctuationFallbackChars\s*=\s*new char\[\d+\][^;]*'\\u00b7'") "U+00B7 (路) must be in the punctuation fallback set"
    Assert-True ($src.Contains("case '\u25cf':")) "U+25CF must have a replacement mapping"
    Assert-True ($src.Contains("case '\u25a1':")) "U+25A1 must have a replacement mapping"
    Assert-True ($src.Contains("case '\u00b7':")) "U+00B7 must have a replacement mapping"
}

It "TMP sync cache path forces overlay when glyphs are missing" {
    Assert-True ($src.Contains("bool tmpFontCoversText2 = _instance.EnsureTMPFontCoversText(__instance, text2)")) "sync cache hits must verify TMP glyph coverage"
    Assert-True ($src.Contains("_instance.ApplyTmpOverlay(__instance, text2, value, !tmpFontCoversText2)")) "sync cache hits must force overlay for missing glyphs"
    Assert-True ($src.Contains("bool tmpFontCoversText = _instance.EnsureTMPFontCoversText(__instance, text)")) "already-CJK TMP writes must verify glyph coverage"
    Assert-True ($src.Contains("bool tmpFontCoversText = EnsureTMPFontCoversText(val, text)")) "scanner CJK path must verify TMP glyph coverage"
    Assert-True ($src.Contains("ApplyTmpOverlay(val, text, text, !tmpFontCoversText)")) "scanner CJK path must force overlay for missing glyphs"
    Assert-True ($src.Contains("return missingCount == 0;")) "null-font direct TMP assignment must still report missing glyphs"
    Assert-True ($src.Contains("NormalizeTmpPunctuationForMissingGlyphs")) "TMP display should replace missing full-width punctuation before rendering"
    Assert-True ($src.Contains("case '\uff1f':")) "full-width question mark must have a TMP-safe fallback"
}

It "Unity long-running component state is bounded" {
    Assert-True ($src.Contains("MaxTrackedComponentStates")) "tracked component state must have a cap"
    Assert-True ($src.Contains("MaxPendingComponentWork")) "pending component work must have a cap"
    Assert-True ($src.Contains("PruneLongRunningStateIfNeeded()")) "scanner must prune long-running state"
    Assert-True ($src.Contains("ResetSceneScopedState(clearPendingComponentWork: true)")) "scene loads must reset scene-scoped state"
    Assert-True ($src.Contains("_pendingApplyKeys.Clear()")) "pending apply key set must be rebuilt/cleared with queue pruning"
}

It "Unity batch dispatch keeps multiple batches in flight" {
    Assert-True ($src.Contains("MaxConcurrentBatchFlushes")) "batch flush concurrency constant must exist"
    Assert-True ($src.Contains("DrainPendingBatchQueueAsync")) "flush must drain via concurrent workers"
    Assert-True ($src.Contains("Callback failed for")) "batch callbacks must be isolated from each other"
    Assert-True ($src.Contains("Flush failed")) "fire-and-forget batch flush must have top-level exception logging"
    Assert-True ($src.Contains("_batchFlushScheduled = false;")) "batch flush flag must be reset on idle/error"
    Assert-True ($src.Contains("_ = FlushPendingBatchRequestsAsync(faulted ? BatchFlushFaultRestartDelayMs : 0);")) "pending work must be rescheduled after a flush failure"
    Assert-False ($src -match '(?s)while \(true\)\s*\{\s*List<PendingBatchRequest> list = DequeuePendingBatchRequests\(8\);.{0,400}?await ProcessPendingBatchRequestsAsync\(list\);\s*lock') "flush loop must not await batches one at a time"
}

It "Unity boot cache import runs off the main thread" {
    # Importing a six-digit local cache inline in Awake (regex validators per
    # row) froze game startup for seconds. Texts shown before the cache lands
    # are healed by scanner cache-apply passes, like deferred-sync arrivals.
    Assert-True ($src -match '(?s)private async Task BootCacheLoadAsync\(\).{0,900}?await RunBackground\(delegate\s*\{\s*LoadServerCache\(\);') "boot cache import must run via RunBackground"
    Assert-True ($src.Contains("_ = BootCacheLoadAsync();")) "Awake must fire the boot cache load asynchronously"
    Assert-False ($src -match '(?s)LoadGlossary\(\);\s*LoadServerCache\(\);') "Awake must not import the local cache inline"
    Assert-True ($src -match '(?s)private async Task BootCacheLoadAsync\(\).{0,2000}?StartServerCacheSync\(\);') "server sync decision must wait for the local cache count"
    Assert-True ($src -match '(?s)lock \(_cache\)\s*\{\s*_cache\.Clear\(\);\s*_localCacheKeys\.Clear\(\);') "cache reset must hold the lock now that the importer is concurrent"
    Assert-True ($src -match '(?s)private bool TryGetLocalTranslation\(string text, out string translated\).*?lock \(_cache\)\s*\{\s*if \(_glossary\.TryGetValue\(text, out var value\)\)') "glossary fast path must lock against the background importer"
}

It "Oversized local caches heal instead of stuttering every persist" {
    # A local cache file polluted by legacy full-server-dump persists (250k+
    # rows, 25 MB) made every persist cycle revalidate and rewrite the whole
    # file on the Unity main thread -- periodic multi-second in-game freezes.
    Assert-True ($src.Contains("private const int OversizedLocalCacheEntryLimit")) "oversized local cache threshold must exist"
    Assert-True ($src.Contains("private const long OversizedLocalCacheFileBytes")) "oversized local cache must be detected by file size before JSON parse"
    Assert-True ($src.Contains("new FileInfo(path).Length > OversizedLocalCacheFileBytes")) "huge polluted cache files must be skipped before parse"
    Assert-True ($src.Contains("bool oversized = val.Count > OversizedLocalCacheEntryLimit;")) "local cache load must detect dump pollution"
    Assert-True ($src.Contains("TryBackupOversizedLocalCache(path, val.Count);")) "polluted local caches must be backed up before the file shrinks"
    Assert-True ($src.Contains('".bak-oversized"')) "backup must use a recognizable suffix"
    Assert-True ($src.Contains('File.WriteAllText(path, "{}", Encoding.UTF8);')) "active oversized cache must be reset so next launch does not parse it again"
    Assert-True ($src -match '(?s)await RunBackground\(delegate\s*\{\s*Dictionary<string, string> dictionary = SnapshotLocalCacheForPersist\(\);.{0,300}?WriteLocalCacheSnapshot\(dictionary\);') "persist snapshot and file write must run off the Unity main thread"
}

It "Fire-and-forget scheduler flags cannot wedge on exceptions" {
    # _batchFlushScheduled / _cachePersistScheduled gate their own reschedule:
    # if the owning task dies without clearing the flag or restarting, the
    # translation pipeline or cache persistence silently stops for the session.
    Assert-True ($src -match '(?s)finally\s*\{\s*/\* Liveness guarantee.{0,1500}?_ = FlushPendingBatchRequestsAsync\(faulted \? BatchFlushFaultRestartDelayMs : 0\);') "flush restart must run inside finally so catch-path exceptions cannot skip it"
    Assert-True ($src -match '(?s)catch \(Exception ex\)\s*\{\s*faulted = true;') "flush failures must mark the cycle as faulted"
    Assert-True ($src.Contains("private const int BatchFlushFaultRestartDelayMs")) "faulted flush restarts must be rate-limited, not a hot loop"
    Assert-True ($src -match '(?s)\[BATCH\] Invoking callbacks for \{requests\.Count\} requests, \{groupResults\.Count\} results"\);\s*InvokePendingRequestCallbacks\(requests, groupResults, requestFailed\);\s*\}\s*catch') "group callback dispatch must not throw past the worker chain"
    Assert-True ($src -match '(?s)private async Task PersistLocalCacheAsync\(\)\s*\{\s*try\s*\{\s*while \(true\)') "cache persist loop must be wrapped in a top-level try"
    Assert-True ($src.Contains("[CACHE] Persist cycle failed: ")) "a bad snapshot/write must only cost one persist cycle, not the loop"
    Assert-True ($src -match '(?s)catch \(Exception ex2\)\s*\{\s*/\* Liveness guarantee.{0,500}?_cachePersistScheduled = false;') "fatal persist loop errors must release the scheduled flag for reschedule"
}

It "Unity async pass-through misses stay retryable while repeated rejected translations are bounded" {
    Assert-True ($src.Contains(") ? value : null);")) "batch callbacks must not echo the original on cache/API miss"
    Assert-True ($src.Contains("TranslationRetryCooldownSeconds")) "temporary failures should use a bounded retry cooldown"
    Assert-True ($src.Contains("MaxRejectedTranslationRetries")) "rejected translations need a finite retry budget"
    Assert-True ($src.Contains("MarkRejectedTranslationRetry(pendingBatchRequest.OriginalText, text2);")) "rejected batch responses should count toward the retry budget"
    Assert-True ($src.Contains("_translationRetryAbandoned.Contains(key)")) "abandoned rejected translations must block future remote retries"
    Assert-True ($src.Contains("ClearTranslationRetryState(original);")) "successful accepted translations must clear retry rejection state"
    Assert-True ($src.Contains("MarkTranslationRetryCooldown(originalText);")) "async pass-through/null responses should cool down briefly before retry"
    Assert-True ($src.Contains("_translationRetryCooldowns.Remove(key);")) "retry cooldowns must be pruned after expiry"
    Assert-False ($src.Contains("_negativeCache")) "retryability must not use a permanent negative cache"
    Assert-False ($src.Contains("MarkKnownUntranslatable(pendingBatchRequest.OriginalText);")) "rejected batch responses must not permanently poison retryability"
    Assert-False ($src.Contains("MarkKnownUntranslatable(originalText);")) "async pass-through responses must not permanently poison retryability"
    Assert-True ($src.Contains("pass-through result left retryable")) "async original echoes should be documented as retryable"
}


It "Unity accepts protected Latin story terms without allowing partial English" {
    $cjk = [string][char]0x597D
    $original1 = "Very well, Aster. I already have enough to know where you fit into the Orion Program."
    $translated1 = "$cjk Aster $cjk Orion Program $cjk"
    $hasResidue1 = Has-SuspiciousEnglishResidue $original1 $translated1
    Assert-False $hasResidue1 "proper nouns from the source should not reject a valid Chinese translation"

    $original2 = "The rules are simple: your designation is confidential. You may only discuss it with other Delta members."
    $translated2 = "$cjk Delta $cjk"
    $hasResidue2 = Has-SuspiciousEnglishResidue $original2 $translated2
    Assert-False $hasResidue2 "source story term Delta should be allowed inside a Chinese translation"

    $bad = "$cjk your designation is confidential."
    $hasBadResidue = Has-SuspiciousEnglishResidue $original2 $bad
    Assert-True $hasBadResidue "actual partial English translations must still be rejected"

    # Chinese keeps protected terms singular: original "Deltas" must allow "Delta".
    $original3 = "Absolutely! If you ever want to review the lesson, send me a note. We Deltas should stay in formation during drills!"
    $translated3 = "$cjk Delta $cjk"
    $hasResidue3 = Has-SuspiciousEnglishResidue $original3 $translated3
    Assert-False $hasResidue3 "singular form of a plural source story term must be allowed"

    $translated4 = "$cjk Deltas $cjk"
    $hasResidue4 = Has-SuspiciousEnglishResidue "The Delta annex is upstairs, past the common room." $translated4
    Assert-False $hasResidue4 "pluralized form of a singular source story term must be allowed"

    $stillBad = "$cjk Gamma $cjk"
    $hasStillBadResidue = Has-SuspiciousEnglishResidue $original3 $stillBad
    Assert-True $hasStillBadResidue "terms absent from the source must still be rejected"

    Assert-True ($src.Contains("IsAllowedLatinResidue")) "source-aware Latin residue guard should be present"
    Assert-True ($src.Contains("LatinResidueMatchesSourceWord")) "residue guard must accept stem-level matches of protected terms"
    Assert-True ($src.Contains("CommonCapitalizedEnglishWords")) "common sentence words must not be treated as protected terms"
}

It "Unity must not blank source text while waiting for translation" {
    Assert-False ($src.Contains("new string(' ', Math.Max(1, rawText.Length))")) "pending translation must not replace dialogue with spaces"
    Assert-False ($src.Contains("SetTextValue(component, string.Empty)")) "pending translation must not clear source text"
    Assert-False ($src.Contains("HideTmpAwaitingTranslation")) "pending translation should leave source text visible instead of routing through hidden-awaiting state"
}

Write-Host ""
Write-Host ("=============================")
Write-Host ("Pass: {0}   Fail: {1}" -f $script:Pass, $script:Fail)
if ($script:Fail -gt 0) {
    Write-Host "Failures:" -ForegroundColor Red
    $script:Errors | ForEach-Object { Write-Host (" - " + $_) -ForegroundColor Red }
    exit 1
}
exit 0

