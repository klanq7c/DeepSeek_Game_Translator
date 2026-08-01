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

function Get-NumericConstant([string]$source, [string]$name) {
    $match = [regex]::Match($source, "private const (?:int|float|long) " + [regex]::Escape($name) + " = ([0-9.]+)")
    if (-not $match.Success) { throw "Could not find numeric constant $name" }
    return [double]::Parse($match.Groups[1].Value, [Globalization.CultureInfo]::InvariantCulture)
}

function Normalize-RequestText([string]$text) {
    if ([string]::IsNullOrWhiteSpace($text)) { return "" }
    return (($text.Trim() -split '\s+') -join ' ')
}

$repo = Split-Path -Parent $PSScriptRoot
$srcPath = Join-Path $repo "payloads\UnityTranslator\src\DeepSeekTranslator.cs"
$src = Get-Content -LiteralPath $srcPath -Raw
$longTextPlannerPath = Join-Path $repo "payloads\UnityTranslator\src\UnityLongTextPlanner.cs"
$fontPatcherPath = Join-Path $repo "payloads\UnityTranslator\src\FontPatcher\DeepSeekUnityFontPatcher.cs"
$il2cppEndpointPath = Join-Path $repo "payloads\UnityIL2CPP\DeepSeekXUnityTranslator\src\DeepSeekTranslateEndpoint.cs"
$tmpFallbackPath = Join-Path $repo "payloads\UnityIL2CPP\DeepSeekTMPFontFallback\src\TmpFontFallbackPlugin.cs"
$serverSrcPath = Join-Path $repo "native\src\server\api.c"
$fontPatcherSrc = Get-Content -LiteralPath $fontPatcherPath -Raw
$il2cppEndpointSrc = Get-Content -LiteralPath $il2cppEndpointPath -Raw
$tmpFallbackSrc = Get-Content -LiteralPath $tmpFallbackPath -Raw
$serverSrc = Get-Content -LiteralPath $serverSrcPath -Raw
$unityCoreSourcePaths = @($srcPath, $longTextPlannerPath, $il2cppEndpointPath, $tmpFallbackPath)
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
    if ($t.Length -gt 0) {
        $opening = [int][char]$t[0]
        $closing = if ($opening -eq 0x22) { 0x22 } elseif ($opening -eq 0x201c) { 0x201d } elseif ($opening -eq 0x2018) { 0x2019 } else { -1 }
        if ($closing -ge 0 -and ($t.Length -lt 2 -or [int][char]$t[$t.Length - 1] -ne $closing)) { return $true }
    }
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

It "Unity IMGUI labels use the generic async translation path" {
    foreach ($sample in @("What?", "Hi honey.", "- Car keys added to the inventory")) {
        Assert-False (Looks-LikeRuntimeStatusText $sample) "IMGUI story text was classified as runtime telemetry: $sample"
        Assert-False (Looks-LikeTypewriterFragment $sample) "complete IMGUI story text was classified as a fragment: $sample"
    }

    Assert-True ($src.Contains('PatchGuiStringControl("Label", new Type[3] { typeof(Rect), typeof(string), typeof(GUIStyle) }, "GUI.Label(Rect,String,GUIStyle)", "GUILabelPrefix")')) "Unity Mono must hook the managed GUI.Label string overload used by IMGUI games"
    Assert-True ($src.Contains('_harmony.Patch(methodInfo, new HarmonyMethod(typeof(DeepSeekTranslator), prefixName))')) "GUI controls must route through the requested IMGUI translation prefix"
    Assert-True ($src.Contains("private static void GUILabelPrefix(ref Rect position, ref string text, GUIStyle style)")) "IMGUI prefix must be strongly typed to the rendered rect, string, and style"
    Assert-True ($src.Contains('PatchGUILayoutLabel(new Type[2] { typeof(string), typeof(GUILayoutOption[]) }, "GUILayout.Label(String,Options)", "GUILayoutLabelPrefix")')) "Unity Mono must hook GUILayout.Label(string, options) used by layout-driven IMGUI games"
    Assert-True ($src.Contains('PatchGUILayoutLabel(new Type[3] { typeof(string), typeof(GUIStyle), typeof(GUILayoutOption[]) }, "GUILayout.Label(String,GUIStyle,Options)", "GUILayoutLabelStylePrefix")')) "Unity Mono must hook GUILayout.Label(string, style, options)"
    Assert-True ($src.Contains('PatchGUILayoutLabel(new Type[2] { typeof(GUIContent), typeof(GUILayoutOption[]) }, "GUILayout.Label(GUIContent,Options)", "GUILayoutContentLabelPrefix")')) "Unity Mono must hook GUILayout.Label(GUIContent, options) without rewriting shared translations"
    Assert-True ($src.Contains("private static void GUILayoutLabelPrefix(ref string text, GUILayoutOption[] options)")) "GUILayout string labels must expose a mutable rendered string"
    Assert-True ($src.Contains("private static void GUILayoutContentLabelPrefix(GUIContent content, GUILayoutOption[] options)")) "GUILayout GUIContent labels must route through renderer-local content handling"
    Assert-True ($src.Contains("TranslateImGuiString(ref text, GetEffectiveGUILayoutLabelStyle(style), false, ref unused)")) "GUILayout string labels must reuse the IMGUI async/cache translation path"
    Assert-True ($src.Contains("TranslateGUILayoutLabelContent(content, null)")) "GUILayout GUIContent labels must reuse the IMGUI async/cache translation path"
    Assert-True ($src.Contains('PatchGUILayoutStringControl("Button", new Type[2] { typeof(string), typeof(GUILayoutOption[]) }, "GUILayout.Button(String,Options)", "GUILayoutButtonPrefix")')) "layout-driven IMGUI buttons must be translated"
    Assert-True ($src.Contains('PatchGUILayoutStringControl("Toggle", new Type[3] { typeof(bool), typeof(string), typeof(GUILayoutOption[]) }, "GUILayout.Toggle(Bool,String,Options)", "GUILayoutTogglePrefix")')) "layout-driven IMGUI toggles must be translated"
    Assert-True ($src.Contains('PatchGUILayoutStringControl("Box", new Type[2] { typeof(string), typeof(GUILayoutOption[]) }, "GUILayout.Box(String,Options)", "GUILayoutBoxPrefix")')) "layout-driven IMGUI boxes must be translated"
    Assert-True ($src.Contains('PatchGUILayoutStringControl("SelectionGrid", new Type[4] { typeof(int), typeof(string[]), typeof(int), typeof(GUILayoutOption[]) }, "GUILayout.SelectionGrid(Int,StringArray,Int,Options)", "GUILayoutSelectionGridPrefix")')) "layout-driven IMGUI selection grids must be translated without mutating shared server behavior"
    Assert-True ($src.Contains('PatchGuiStringControl("Box", new Type[3] { typeof(Rect), typeof(string), typeof(GUIStyle) }, "GUI.Box(Rect,String,GUIStyle)", "GUIBoxPrefix")')) "fixed-layout IMGUI boxes must be translated"
    Assert-True ($src.Contains("private static void TranslateImGuiStringArray(ref string[] texts, GUIStyle style)")) "IMGUI option arrays must be cloned before display translation"
    Assert-False ($src.Contains('"GUILayout.TextField')) "editable IMGUI text fields must not be hooked as labels"
    Assert-False ($src.Contains('"GUILayout.TextArea')) "editable IMGUI text areas must not be hooked as labels"
    $objective = "- Get something to eat"
    Assert-True (Looks-LikeTypewriterFragment $objective) "the regression fixture must exercise the fragment-looking objective path"
    Assert-False ($src.Contains("!LooksLikeTypewriterFragment(text) && !_instance.IsTranslationRetryCoolingDown(text)")) "IMGUI must not permanently drop stable labels that only look like typewriter fragments"
    Assert-True ($src -match '(?s)private void QueueImGuiTranslation\(string text\).*?LooksLikeTypewriterFragment\(text\).*?TypewriterFragmentDebounceSeconds') "IMGUI fragment suspects must settle before entering the async queue"
    Assert-True ($src.Contains("QueueImGuiTranslation(text)")) "uncached IMGUI text must enter the shared async batch queue"
    Assert-True ($src.Contains("RequestSharedTranslation(text, GetRequestDomain(text)")) "IMGUI requests must reuse the existing deduplicated batch pipeline"
    Assert-True ($src.Contains("_imguiPending.Count >= MaxImGuiPending")) "IMGUI per-frame requests must remain bounded"
    Assert-True ($src.Contains("ApplyImGuiFont(style)")) "translated IMGUI labels must receive a CJK-capable font"
    Assert-True ($src.Contains("PrepareImGuiTranslatedShortLabelLayout(ref position, style, text, originalText)")) "translated IMGUI labels must resize the live draw rect where source and translation are both known"
    Assert-True ($src.Contains("TryMeasureImGuiContentSafe(style, content")) "IMGUI short labels must use the active style through the optional measurement boundary"
    Assert-True ($src.Contains("TextAnchor.MiddleCenter")) "center-aligned IMGUI labels must preserve their horizontal anchor when widened"
    Assert-True ($src.Contains("TextAnchor.MiddleRight")) "right-aligned IMGUI labels must preserve their right edge when widened"
    Assert-True ($src.Contains("[IMGUI-LAYOUT] short-label width")) "IMGUI label rect expansion must emit rate-limited renderer diagnostics"
    Assert-True ($src.Contains("ImGuiShortLabelWidth=")) "IMGUI label expansion count must remain visible in live diagnostics"
    Assert-False ($src.Contains("string[] value3 = new string[45]")) "live diagnostics must not require a manually synchronized array length"
    Assert-False ($src.Contains("AC.MenuLabel")) "Unity IMGUI support must not hard-code Adventure Creator"
    Assert-False ($src.Contains("TheNightDriver")) "Unity IMGUI support must not hard-code one game"
    Assert-False ($src.Contains("Bonfire_v0.83.0")) "Unity IMGUI support must not hard-code one Bonfire build path"
    Assert-True ($src.Contains('"Unity Translator", "3.1.126"')) "Unity Mono plugin version must include the current IMGUI control coverage, visible scene warmup, and host-lifetime fixes"
}

It "Unity keeps a render-thread pump for UGUI cache apply" {
    Assert-True ($src.Contains("private volatile int _beforeRenderTickCount")) "render pump diagnostics must count Application.onBeforeRender ticks"
    Assert-True ($src.Contains("private int _beforeRenderPumpActive")) "render pump must have a reentrancy guard"
    Assert-True ($src.Contains("private void RegisterBeforeRenderPump()")) "Unity Mono must register a render-lifecycle pump"
    Assert-False ($src -match 'Application\.onBeforeRender\s*[+-]=') "stripped Unity players may omit Application.onBeforeRender accessors"
    Assert-False ($src -match 'Canvas\.willRenderCanvases\s*[+-]=') "stripped Unity players may omit Canvas.willRenderCanvases accessors"
    Assert-False ($src -match 'SceneManager\.sceneLoaded\s*[+-]=') "stripped Unity players may omit SceneManager.sceneLoaded accessors"
    Assert-True ($src.Contains('RegisterOptionalStaticEvent(typeof(Application), "onBeforeRender", "OnBeforeRenderPump"')) "UGUI games need a reflective onBeforeRender pump when the optional event exists"
    Assert-True ($src.Contains('RegisterOptionalStaticEvent(typeof(Canvas), "willRenderCanvases", "OnWillRenderCanvases"')) "the Canvas callback must use the same stripped-player boundary"
    Assert-True ($src.Contains('RegisterOptionalStaticEvent(typeof(SceneManager), "sceneLoaded", "OnSceneLoaded"')) "scene lifecycle registration must tolerate missing event accessors"
    Assert-True ($src.Contains("private void UnregisterOptionalStaticEvent(")) "reflectively registered Unity events must have a symmetric teardown path"
    Assert-True ($src.Contains('UnregisterOptionalStaticEvent("Application.onBeforeRender"')) "the render-lifecycle pump must be reflectively unregistered on plugin teardown"
    Assert-True ($src.Contains('UnregisterOptionalStaticEvent("Canvas.willRenderCanvases"')) "the Canvas callback must be reflectively unregistered on plugin teardown"
    Assert-True ($src.Contains('UnregisterOptionalStaticEvent("SceneManager.sceneLoaded"')) "the scene callback must be reflectively unregistered on plugin teardown"
    Assert-True ($src.Contains('PumpOnce("BEFORE_RENDER")')) "render pump must drive the same local-cache-first queue/apply path"
    Assert-False ($src -match '=\s*Font\.CreateDynamicFontFromOSFont\s*\(') "stripped Unity players may omit the public OS-font factory and must not hard-bind it"
    Assert-True ($src.Contains("private Font CreateDynamicFontFromOSFontSafe(string fontName, int fontSize)")) "Unity Mono OS-font creation needs a stripped-player compatibility boundary"
    Assert-True ($src -match '(?s)typeof\(Font\)\.GetMethod\("CreateDynamicFontFromOSFont".*?typeof\(string\).*?typeof\(int\)') "the optional OS-font factory must be resolved reflectively by its exact signature"
    Assert-True ($src.Contains('ReportCaughtException(ex, "renderer=UnityMono method=Font.CreateDynamicFontFromOSFont"')) "OS-font reflection failures must retain diagnostic context"
    Assert-True ($src.Contains("Optional Unity Font.CreateDynamicFontFromOSFont(string,int) is unavailable")) "a missing stripped-player font factory must remain visible in logs"
    Assert-False ($src -match 'new\s+Font\s*\(') "stripped Unity font constructors must not be hard-bound"
    Assert-True ($src.Contains("Font.Internal_CreateFontFromPath not found; the font file cannot be loaded safely")) "missing path-font support must remain visible without constructing an unusable Font"
    Assert-False ($src.Contains("Activator.CreateInstance(typeof(Font)) as Font")) "the retained internal font factory must not run after the stripped Font() constructor already initialized a null native font"
    Assert-True ($src.Contains("FormatterServices.GetUninitializedObject(typeof(Font)) as Font")) "retained internal font factories need a constructor-free managed Font shell"
    Assert-True ($src -match '(?s)typeof\(Font\)\.GetMethod\("Internal_CreateDynamicFont".*?BindingFlags\.NonPublic.*?typeof\(Font\).*?typeof\(string\[\]\).*?typeof\(int\)') "the stripped-player font boundary must resolve only Unity's retained dynamic OS-font factory"
    Assert-True ($src -match '(?s)FormatterServices\.GetUninitializedObject\(typeof\(Font\)\) as Font;\s*if \(ReferenceEquals\(val, null\)\).*?_internalCreateDynamicFontMethod\.Invoke\(null, new object\[3\].*?new string\[1\].*?fontName.*?fontSize') "an uninitialized Font shell must invoke the dynamic OS-font factory exactly once"
    Assert-False ($src -match '(?s)CreateDynamicFontFromOSFontSafe.*?typeof\(Font\)\.GetMethod\("Internal_CreateFont"') "Font.Internal_CreateFont(Font,string) is an asset-name constructor, not an OS-font fallback"
    Assert-True ($src.Contains("internal dynamic-font factory is unavailable")) "a stripped player without the real dynamic-font factory must remain diagnostic"
    Assert-True ($src.Contains("private static bool IsUsableFontForText(Font font, string text)")) "Unity Font fallbacks must verify native liveness and glyph coverage before display"
    Assert-True ($src -match '(?s)private static bool IsUsableFontForText\(Font font, string text\).*?font\.dynamic.*?font\.HasCharacter') "dynamic-font validation must reject non-dynamic assets that cannot render translated glyphs"
    Assert-True ($src -match '(?s)private void ApplyTMProTranslation\(.*?bool canTranslateTmp = CanTranslateTmp\(\);.*?if \(!canTranslateTmp && !ShouldUseTmpOverlay\(\)\).*?if \(!canTranslateTmp\).*?ApplyTmpOverlay\(.*?MarkProcessed\(') "periodic and registry TMP cache hits must use the live overlay when the stripped player cannot create a TMP_FontAsset"
    Assert-True ($src.Contains("TMP CJK font asset remains unavailable; renderer-local UGUI overlay fallback remains active.")) "TMP retry diagnostics must not report translation disabled while the overlay boundary is available"
    Assert-True ($src.Contains("private void PinRuntimeFont(Font font, string source)")) "runtime-created Unity fonts need explicit native lifetime ownership"
    Assert-True ($src -match '(?s)CreateDynamicFontFromOSFontSafe.*?PinRuntimeFont\(.*?fontName\)') "both reflected OS-font creation paths must pin their native font object"
    Assert-True ($src.Contains("HideFlags.DontUnloadUnusedAsset")) "runtime-created fonts must survive stripped-player scene resource cleanup"
    Assert-True ($src -match '(?s)private void PinRuntimeFont.*?Object\.DontDestroyOnLoad\(obj\)') "runtime-created fonts must be pinned across bootstrap scene replacement"
    Assert-True ($src.Contains('ReportCaughtException(ex, "renderer=UnityMono method=PinRuntimeFont source="')) "runtime-font lifetime failures must retain diagnostic context"
    Assert-False ($src -match 'new\s+WaitForSeconds\s*\(') "stripped Unity players may omit the WaitForSeconds constructor"
    Assert-True ($src.Contains("private static IEnumerator WaitForScaledSecondsSafe(float seconds)")) "coroutine delays need a stripped-player-compatible scaled-time iterator"
    Assert-True ($src -match '(?s)private static IEnumerator WaitForScaledSecondsSafe\(float seconds\).*?Time\.time.*?yield return null') "the compatibility delay must preserve scaled-time lifecycle semantics without a fixed thread sleep"
    Assert-True ($src.Contains("Unity WaitForSeconds(float) is unavailable; using the scaled-frame compatibility iterator")) "selection of the stripped coroutine-delay fallback must remain visible"
    Assert-False ($src -match '\bAssetBundle\s+\w+\s*=') "stripped Unity players may remove the AssetBundle type entirely"
    Assert-False ($src -match '=\s*AssetBundle\.[A-Za-z_]') "Unity Mono packaged-font loading must not hard-bind AssetBundle factories"
    Assert-True ($src.Contains('Type assetBundleType = ResolveAssetBundleTypeSafe();')) "packaged TMP fonts need a type-optional AssetBundle boundary"
    Assert-True ($src -match '(?s)private Type ResolveAssetBundleTypeSafe\(\).*?AccessTools\.TypeByName\("UnityEngine\.AssetBundle"\).*?Assembly\.Load\("UnityEngine\.AssetBundleModule"\)') "an unloaded retained AssetBundle module must be resolved without a hard type dependency"
    Assert-True ($src.Contains("Loaded optional UnityEngine.AssetBundleModule for packaged TMP fonts")) "optional AssetBundle module activation must remain visible"
    Assert-True ($src -match '(?s)MethodInfo loadFromFile = AccessTools\.Method\(assetBundleType, "LoadFromFile".*?MethodInfo loadAllAssets = AccessTools\.Method\(assetBundleType, "LoadAllAssets"') "packaged TMP font loading must resolve both AssetBundle calls reflectively"
    Assert-True ($src.Contains("UnityEngine.AssetBundle is unavailable; packaged TMP font bundles will be skipped")) "a stripped AssetBundle type must remain visible in diagnostics"
    Assert-False ($src -cmatch 'GetComponentsInChildren\(\s*(?:type|_tmpTextTypeCache)\s*,') "stripped Unity players may keep only generic GetComponentsInChildren overloads"
    Assert-True ($src.Contains("private static Component[] GetComponentsInChildrenByTypeSafe(object owner, Type componentType, bool includeInactive)")) "runtime-type hierarchy scans need a stripped-player compatibility boundary"
    Assert-True ($src -match '(?s)GetComponentsInChildrenByTypeSafe.*?IsGenericMethodDefinition.*?MakeGenericMethod\(componentType\).*?Invoke') "the hierarchy compatibility boundary must fall back to the retained generic overload"
    Assert-True ($src.Contains("dynamic GetComponentsInChildren(Type,bool) is unavailable; using its generic overload")) "selection of the generic hierarchy fallback must remain visible"
    Assert-True ($src.Contains("private static Object[] FindObjectsOfTypeAllSafe(Type componentType)")) "global object scans need a stripped-player compatibility boundary"
    Assert-True ($src -match '(?s)FindObjectsOfTypeAllSafe.*?Resources\.FindObjectsOfTypeAll\(componentType\).*?FindObjectsOfType.*?typeof\(Type\).*?typeof\(bool\).*?true') "the global scan boundary must fall back to the retained include-inactive Object API"
    Assert-True ($src.Contains("Resources.FindObjectsOfTypeAll(Type) returned no live objects; using Object.FindObjectsOfType(Type,true)")) "selection of the stripped global-object fallback must remain visible"
    Assert-True (([regex]::Matches($src, '=\s*Resources\.FindObjectsOfTypeAll\(componentType\)')).Count -eq 1) "all runtime-type global object enumeration must stay behind the compatibility boundary"
    Assert-False ($src -match 'Resources\.FindObjectsOfTypeAll\s*<') "generic global object enumeration must stay behind the compatibility boundary"
    Assert-True ($src -match '(?s)FindTmpTextObjectsSafe\(Type tmpTextType\).*?FindObjectsOfTypeAllSafe\(tmpTextType\)') "the TMP registry path must try the stripped-player global enumeration boundary first"
    Assert-True ($src.Contains("private static Object[] FindTmpTextObjectsSafe(Type tmpTextType)")) "TMP scans need a registry fallback when stripped native enumeration stays empty"
    Assert-True ($src -match '(?s)FindTmpTextObjectsSafe.*?AccessTools\.TypeByName\("TMPro\.TMP_UpdateManager"\).*?m_InternalUpdateQueue') "the TMP fallback must use the renderer-owned active text registry"
    Assert-True ($src.Contains("TMP global enumeration returned no live objects; using TMP_UpdateManager internal queue")) "selection of the TMP registry fallback must remain visible"
    Assert-True ($src.Contains("Object[] array = FindTmpTextObjectsSafe(_tmpTextTypeCache);")) "the backup TMP scanner must consume renderer-registered text objects"
    Assert-False ($src -match '\.GetAlpha\s*\(\s*\)') "stripped CanvasRenderer profiles may omit GetAlpha"
    Assert-False ($src -match '\.SetAlpha\s*\(') "stripped CanvasRenderer profiles may omit SetAlpha"
    Assert-True ($src.Contains("private static float GetCanvasRendererAlphaSafe(CanvasRenderer renderer, float fallback)")) "CanvasRenderer alpha reads need an optional-member boundary"
    Assert-True ($src.Contains("private static void SetCanvasRendererAlphaSafe(CanvasRenderer renderer, float alpha)")) "CanvasRenderer alpha writes need an optional-member boundary"
    Assert-True ($src.Contains("CanvasRenderer alpha accessors are unavailable; alpha diagnostics and repair will be skipped")) "missing CanvasRenderer alpha accessors must remain visible"
    Assert-False ($src -cmatch 'style\.CalcMinMaxWidth\s*\(') "stripped IMGUI profiles may omit GUIStyle.CalcMinMaxWidth"
    Assert-False ($src -cmatch 'style\.CalcSize\s*\(') "stripped IMGUI profiles may omit GUIStyle.CalcSize"
    Assert-False ($src -cmatch 'style\.alignment') "stripped IMGUI profiles may omit GUIStyle.alignment"
    Assert-False ($src -cmatch 'style\.font') "stripped IMGUI profiles may omit GUIStyle.font"
    Assert-True ($src.Contains("private static bool TryMeasureImGuiContentSafe(GUIStyle style, GUIContent content, out float requiredWidth, out TextAnchor alignment)")) "IMGUI layout enhancement needs an optional-member boundary"
    Assert-True ($src.Contains("GUIStyle measurement/font members are unavailable; related IMGUI visual enhancements will be skipped")) "missing GUIStyle visual members must remain visible"
    Assert-False ($src -match '\.SetTexture\(\s*"_MainTex"') "stripped Material profiles may omit the string SetTexture overload"
    Assert-True ($src.Contains("private static void SetMaterialTextureSafe(Material material, string propertyName, Texture texture)")) "TMP material texture assignment needs an optional-overload boundary"
    Assert-False ($src -match '\.GetPixel\s*\(') "stripped Texture2D profiles may omit GetPixel"
    Assert-True ($src.Contains("private static Color GetTexturePixelSafe(Texture2D texture, int x, int y)")) "TMP atlas reads need an optional-member boundary"
    Assert-True ($src.Contains('ReportCaughtException(ex, "source=BEFORE_RENDER")')) "render pump failures must be diagnostically visible"
    Assert-True ($src.Contains('BeforeRenderTicks=')) "live diagnostics must expose the render pump"
    Assert-False ($src.Contains("string[] value = new string[18]")) "runtime diagnostics must not require a manually synchronized array length"
    Assert-False ($src.Contains("string[] value2 = new string[21]")) "scan diagnostics must not require a manually synchronized array length"
    Assert-True ($src.Contains('Frame pump hooks installed')) "existing Time getter fallback pump must remain installed"
}

It "stripped Unity Mono font patcher restores only the missing dynamic-font declaration" {
    Assert-True ($fontPatcherSrc.Contains('yield return "UnityEngine.TextRenderingModule.dll"')) "font patcher must target only Unity's text-rendering module"
    Assert-False ($fontPatcherSrc.Contains('yield return "UnityEngine.CoreModule.dll"')) "font patcher must not rewrite unrelated Unity modules"
    Assert-True ($fontPatcherSrc -match '(?s)hasPublicFactory.*?hasInternalFactory.*?if \(hasPublicFactory \|\| hasInternalFactory\).*?return;') "font patcher must be a no-op when either usable factory declaration already exists"
    Assert-True ($fontPatcherSrc -match '(?s)new MethodDefinition\(\s*"Internal_CreateDynamicFont".*?MethodImplAttributes\.InternalCall.*?new ParameterDefinition\("font".*?new ParameterDefinition\(\s*"fontNames".*?new ArrayType\(assembly\.MainModule\.TypeSystem\.String\).*?new ParameterDefinition\(\s*"size".*?TypeSystem\.Int32') "font patcher must restore exactly Font,string[],int as an internal call declaration"
    Assert-False ($fontPatcherSrc -match '(?i)(CopyFile|Download|HttpClient|WebClient|System\.IO|Assembly\.Load|WriteAllBytes)') "font patcher must not copy, download, or redistribute Unity assemblies"
}

It "Unity Mono resolves the player version without a hard Application getter" {
    Assert-False ($src -match '(?<!property=)Application\.unityVersion(?! is unavailable)') "stripped Unity players may omit Application.get_unityVersion"
    Assert-True ($src.Contains("private string GetUnityVersionSafe()")) "Unity version selection must have a renderer-local compatibility boundary"
    Assert-True ($src.Contains('typeof(Application).GetProperty("unityVersion", BindingFlags.Public | BindingFlags.Static)')) "available Unity version properties should still be read reflectively"
    Assert-True ($src.Contains('Path.Combine(Paths.GameRootPath, "UnityPlayer.dll")')) "missing Unity version APIs must fall back to the installed player binary"
    Assert-True ($src.Contains("FileVersionInfo.GetVersionInfo(unityPlayerPath)")) "the UnityPlayer fallback must read official file-version metadata"
    Assert-True ($src.Contains('ReportCaughtException(ex, "renderer=UnityMono property=Application.unityVersion", "GetUnityVersionSafe")')) "Unity version reflection failures must retain diagnostic context"
}

It "Unity Mono pins and protects the BepInEx host lifetime" {
    Assert-True ($src.Contains("private bool EnsureDedicatedPluginHost()")) "the BepInEx instance must bootstrap a runtime host outside the disposable manager root"
    Assert-False ($src.Contains(".transform.root")) "stripped Unity players may omit Transform.get_root; the owned component is attached directly to its host"
    Assert-True ($src -match '(?s)private void Awake\(\)\s*\{\s*if \(!EnsureDedicatedPluginHost\(\)\)\s*\{\s*return;\s*\}.*?_instance = this;') "the disposable bootstrap instance must return before owning runtime state"
    Assert-True ($src.Contains('new GameObject("DeepSeekTranslatorRuntime")')) "the runtime host must have a generic dedicated root"
    Assert-True ($src -match '(?s)_pluginHostRoot = hostRoot;.*?hostRoot\.AddComponent<DeepSeekTranslator>\(\)') "the recursive runtime Awake must identify the dedicated root before initialization"
    Assert-True ($src -match '(?s)if \(\(Object\)\(object\)existingRoot != \(Object\)null\).*?if \(!ownsRuntime\).*?\(\(Behaviour\)this\)\.enabled = false;.*?LogWarning\("Disabled duplicate Unity Mono translator instance') "every later non-owner instance must be disabled instead of running an empty Update guard each frame"
    Assert-True ($src.Contains("private bool _runtimeOwner")) "only the dedicated instance may execute teardown"
    Assert-True ($src.Contains("private void PinPluginHostAcrossSceneLoads()")) "the Unity Mono runtime must own an explicit host-lifetime boundary"
    Assert-True ($src -match '(?s)private void Awake\(\).*?_instance = this;\s*PinPluginHostAcrossSceneLoads\(\);.*?ConfigureHttpFastPath\(\);') "the plugin host must be pinned before background work and hooks start"
    Assert-True ($src.Contains("Object.DontDestroyOnLoad((Object)(object)hostRoot)")) "scene transitions must not destroy the BepInEx plugin host"
    Assert-True ($src.Contains("Pinned BepInEx plugin host across scene loads")) "host-lifetime fallback activation must be visible in BepInEx logs"
    Assert-True ($src.Contains("private static GameObject _pluginHostRoot")) "the destroy guard must target only the plugin-owned root"
    Assert-True ($src.Contains("private void InstallPluginHostLifetimeHooks()")) "Unity games that explicitly purge persistent roots need a scoped lifetime hook"
    Assert-True ($src.Contains('"Destroy", new Type[1] { typeof(Object) }')) "the patchable one-argument Unity destroy wrapper must be guarded"
    Assert-True ($src.Contains("GameObjectSetActivePrefix")) "explicit host deactivation must be guarded before OnDisable tears down the runtime"
    Assert-True ($src.Contains("private static bool ShouldProtectPluginHost(Object candidate)")) "lifetime suppression must use one auditable target predicate"
    Assert-True ($src.Contains("_applicationQuitting")) "normal application shutdown must bypass host protection"
    Assert-True ($src.Contains("Blocked explicit destruction of the BepInEx plugin host")) "suppressed external destruction must emit a rate-limited diagnostic"
}

It "Unity visible scene warmup includes complete objective prose" {
    Assert-False (Looks-LikeTypewriterFragment "Venture out of the stone circle where you've just awoken and follow the main alley that comes out of it.") "complete objective prose must not be treated as a typewriter fragment"
    Assert-True ($src.Contains("private static bool LooksLikeVisibleWarmupText(string text)")) "visible scene warmup must have an explicit classifier"
    Assert-True ($src.Contains("LooksLikeNaturalLanguage(visibleText) && EndsWithSentenceBoundary(visibleText)")) "complete natural-language objective prose must be eligible for scene warmup"
    Assert-True ($src.Contains("Dictionary<string, HashSet<string>> warmupTextsByDomain")) "scene warmup must preserve request domains instead of forcing all text into UI"
    Assert-True ($src.Contains("string requestDomain = GetRequestDomain(candidate.OriginalText)")) "visible warmup must route long prose through the normal domain classifier"
    Assert-True ($src.Contains('No", "Inventory", "INVENTORY", "Use", "USE", "Equip", "EQUIP"')) "startup hot UI terms should cover common inventory/equipment labels without game paths"
    Assert-False ($serverSrc.Contains("LooksLikeVisibleWarmupText")) "visible scene warmup must stay in the Unity renderer layer"
}

It "Unity renderers restore leading decorations dropped by translation" {
    # Translation providers may treat a leading list marker as prompt syntax
    # and omit it. The renderer must rebuild that display-only decoration from
    # the source without rewriting shared translation memory.
    Assert-True ($src.Contains("private static string RestoreLeadingTextDecoration(string translated, string originalText)")) "Unity needs a renderer-local leading-decoration restorer"
    Assert-True ($src.Contains("private static bool TryGetLeadingTextDecoration(string text, out string decoration, out int contentStart)")) "leading decoration parsing must expose the exact source prefix"
    Assert-True ($src.Contains("private static bool IsLeadingTextDecorationMarker(char c)")) "decoration recognition must use an explicit marker allow-list"
    Assert-True ($src -match '(?s)private static string PrepareTranslatedTextForString\(string translated, string originalText = null\).*?RestoreLeadingTextDecoration\(translated, originalText\)') "IMGUI/Fungus string rendering must restore dropped leading decorations"
    Assert-True ($src -match '(?s)private static string PrepareTranslatedTextForComponent\(object component, string text, string originalText = null\).*?RestoreLeadingTextDecoration\(text, originalText\)') "TMP/UGUI rendering must keep the adjacent decoration behavior"
    Assert-True ($src.Contains('"Unity Translator", "3.1.126"')) "Unity Mono plugin version must include the current renderer, long-text, Fungus lookahead, color-tag, IMGUI, and host-lifetime fixes"
}

It "Unity TMP host-font coverage excludes fallback assets" {
    # TMP can report that a character exists through a fallback font even when
    # the host material/submesh cannot render that fallback. In that case CJK
    # disappears while ASCII such as height values remains visible.
    Assert-True ($src.Contains("new object[4] { text2, null, false, false }")) "host-font HasCharacters must inspect only the host atlas"
    Assert-True ($src -match '(?s)method3\.Invoke\(fontAsset, new object\[2\].*?codePoint,\s*false') "host-font HasCharacter must not accept fallback-only glyph coverage"
    $missingCharsMethod = [regex]::Match($src, '(?s)private static List<int> CollectMissingChars\(.*?(?=\s*private )').Value
    $explicitNoFallbackIndex = $missingCharsMethod.IndexOf('typeof(int),')
    $fallbackAwareListIndex = $missingCharsMethod.IndexOf('typeof(List<char>).MakeByRefType()')
    Assert-True ($explicitNoFallbackIndex -ge 0 -and $fallbackAwareListIndex -ge 0 -and $explicitNoFallbackIndex -lt $fallbackAwareListIndex) "explicit no-fallback HasCharacter must run before fallback-aware legacy HasCharacters overloads"
    Assert-True ($src -match '(?s)if \(AreAllGlyphsInAtlas\(value, translatedText\)\).*?propertyInfo\.SetValue\(tmpComponent, _chineseTMPFont\)') "fallback-only host coverage must still reach the direct CJK font swap"
    Assert-False ($serverSrc.Contains("CollectMissingChars")) "TMP glyph compatibility must remain in the Unity renderer adapter"
}

It "Unity TMP font resets revalidate the current CJK text immediately" {
    $fontPostfix = [regex]::Match($src, '(?s)private static void TMPFontPostfix\(.*?(?=\s*private )').Value
    Assert-True ($fontPostfix.Contains('GetCurrentComponentText(__instance, isTmp: true)')) "font reset hook must inspect the text already assigned to the TMP component"
    Assert-True ($fontPostfix.Contains('EnsureTMPFontCoversText(__instance, currentText)')) "font reset hook must revalidate CJK coverage instead of only attaching a fallback"
    Assert-True ($fontPostfix.Contains('InvokeForceMeshUpdate(__instance, __instance.GetType())')) "font reset repair must rebuild the TMP mesh in the same frame"
}

It "Unity TMP long CJK paragraphs replace justified alignment with left alignment" {
    Assert-True ($src.Contains("private void PrepareTmpCjkParagraphLayout(object tmpComponent, string translatedText)")) "TMP needs a renderer-local CJK paragraph layout adjustment"
    Assert-True ($src -match '(?s)PrepareTmpCjkParagraphLayout\(object tmpComponent, string translatedText\).*?Justified.*?Flush.*?Left') "justified and flush CJK paragraphs must become left-aligned"
    Assert-True ($src.Contains("RestoreTmpParagraphLayout(__instance, value)")) "incoming source text must restore the component's original alignment"
    Assert-True ($src -match '(?s)private static TextAnchor GetTmpTextAnchor\(object tmpComponent\).*?Justified.*?Flush.*?num = 0') "the legacy overlay fallback must map justified/flush TMP paragraphs to left alignment"
    Assert-False ($serverSrc.Contains("PrepareTmpCjkParagraphLayout")) "CJK paragraph geometry must remain in the Unity renderer adapter"
}

It "Unity TMP translated short-label groups share the sibling left edge" {
    Assert-True ($src.Contains("private void PrepareTmpTranslatedShortLabelLayout(object tmpComponent, string sourceText, string translatedText)")) "TMP short translations need renderer-local group alignment"
    Assert-True ($src -match '(?s)PrepareTmpTranslatedShortLabelLayout.*?peer\.transform\.parent != parent.*?peerRect\.rect\.width - rect\.width.*?peerAlignmentName\.IndexOf\("Left".*?leftPeerX\.Count < 2.*?position\.x = groupX') "short-label alignment must require two same-parent, same-size left peers before changing geometry"
    Assert-True ($src -match '(?s)RestoreTmpShortLabelLayout.*?OriginalAlignment.*?OriginalAnchoredPosition.*?_tmpShortLabelLayouts\.Remove') "temporary TMP short-label geometry must restore when the source text changes"
    Assert-True ($src.Contains("RestoreTmpShortLabelLayout(__instance, value);")) "TMP setters must restore source layout before handling replacement text"
    Assert-False ($serverSrc.Contains("PrepareTmpTranslatedShortLabelLayout")) "short-label geometry must stay in the Unity renderer adapter"
}

It "Unity UGUI synchronous cache hits apply the CJK font before writing text" {
    $uguiPrefix = [regex]::Match($src, '(?s)private static bool UGUITextPrefix\(.*?(?=\s*private static void UGUITextPostfix)').Value
    Assert-True ($uguiPrefix -match '(?s)if \(text != value\).*?_instance\.ApplyFont\(__instance\);.*?PrepareTranslatedTextForUGUIText') "cached UGUI translations must not remain on the source font where only ASCII digits render"
    Assert-True ($src -match '(?s)private bool ApplyCachedComponentTranslationNow\(.*?Text val = component as Text;.*?ApplyFont\(val\);.*?PrepareTranslatedTextForUGUIText') "scanner-applied UGUI cache hits must retain the same CJK font contract"
    Assert-False ($serverSrc.Contains("ApplyFont(__instance)")) "UGUI font compatibility must remain in the Unity renderer adapter"
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
    Assert-False (Looks-LikeRuntimeStatusText "The cannon is ready to fire! Now I can use a match to shoot the boss if it is visible.") "quest hint prose should remain translatable"
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
    Assert-True (Looks-LikeTypewriterFragment '"A fellow spirit awaits') "an opening dialogue quote without its closing quote must remain a typewriter fragment"
    Assert-False (Looks-LikeTypewriterFragment '"A fellow spirit awaits for communion."') "a complete quoted dialogue sentence must remain translatable"
    Assert-False (Looks-LikeTypewriterFragment ("Alright{0} from this point, the task belongs to us." -f $ellipsis)) "complete ellipsis sentence should not be rejected"
    Assert-False (Looks-LikeTypewriterFragment "Ready or not, I just want to find the archive. There must already be a team practicing.") "complete sentence should not be rejected"
    Assert-False (Looks-LikeTypewriterFragment ("It Feels strange... like we{0}re stepping into another world." -f ([string][char]0x2019))) "complete dialogue line with ellipsis should not be rejected"
    Assert-False (Looks-LikeTypewriterFragment "The cannon is ready to fire! Now I can use a match to shoot the boss if it is visible.") "complete rich UI quest hint should not be treated as a typewriter fragment"
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
    Assert-True ($src.Contains("Dictionary<string, HashSet<string>> warmupTextsByDomain")) "visible warmup misses must actively request translations by domain"
    Assert-True ($src.Contains("Dictionary<string, string> bucketTranslations = await WarmupTextsAsync(bucket.Value, bucket.Key);")) "visible warmup must call the server for each queued domain bucket"
    Assert-False ($src.Contains("ApplyWarmupTranslations(uiCandidates, new Dictionary<string, string>(StringComparer.Ordinal), generation);")) "scene warmup must not apply an empty translation map"
    Assert-True ($src.Contains("WaitForWarmupServerReadyAsync")) "warmup requests must wait briefly for the local server before giving up"
    Assert-True ($src -match '(?s)private async Task<Dictionary<string, string>> WarmupTextsAsync.*?if \(!await WaitForWarmupServerReadyAsync\(\)\)') "warmup batches must not trip server backoff before /health is ready"
    Assert-True ($src.Contains("tcpClient.ReceiveTimeout = timeoutMs;")) "raw HTTP timeout parameter must apply to reads"
    Assert-True ($src.Contains("networkStream.ReadTimeout = timeoutMs;")) "raw HTTP timeout parameter must apply to the response stream"
    Assert-False ($src.Contains("DeepPrefetchLoopCoroutine")) "old unstarted deep-prefetch coroutine path should not remain as misleading dead code"
    Assert-False ($src.Contains("ProcessDeepPrefetchQueueCoroutine")) "deep-prefetch should use the active async tick path only"
}

It "Unity activation scans can queue newly visible uncached UI text" {
    Assert-True ($src.Contains("private bool QueueCachedComponentTextIfAvailable")) "targeted visible text scan must report whether it queued or applied work"
    Assert-True ($src.Contains("QueueCachedTextsInHierarchy(GameObject root, int maxQueue, bool allowRemoteFallback = false)")) "UI tree scans must have an explicit remote fallback gate"
    Assert-True ($src.Contains("QueueDebouncedTextRequest(component, componentInstanceId, currentComponentText, isTmp);")) "visible uncached UI text must be able to enter the debounced remote queue"
    Assert-True ($src.Contains("Interlocked.Increment(ref _targetedCacheQueueCount);") -and $src.Contains("return true;")) "remote fallback must count against targeted activation work limits"
    Assert-True ($src.Contains("QueueCachedTextsInHierarchy(__instance, MaxTargetedCacheQueuesPerActivation, allowRemoteFallback: true);")) "GameObject activation must allow remote fallback for visible uncached UI text"
    Assert-True ($src.Contains("QueueCachedTextsInHierarchy(((Component)__instance).gameObject, MaxTargetedCacheQueuesPerActivation, allowRemoteFallback: true);")) "CanvasGroup visibility must allow remote fallback for visible uncached UI text"
    Assert-True ($src.Contains("TryPatchDeclaredTmpRenderDiscovery")) "TMP render discovery must cover objects created before plugin hooks"
    Assert-True ($src -match '(?s)TryPatchDeclaredTmpRenderDiscovery.*?"Rebuild".*?TMPTextRenderDiscoveryPostfix') "TMP render discovery must hook the declared canvas rebuild boundary"
    Assert-True ($src -match '(?s)private static void TMPTextRenderDiscoveryPostfix.*?QueueCachedComponentTextIfAvailable\(__instance, isTmp: true, allowRemoteFallback: true\)') "rendered TMP text must enter the same local-first remote-fallback pipeline"
    Assert-True ($src.Contains('base.Logger.LogInfo("Hooked " + tmpType.FullName + ".Rebuild for render discovery")')) "TMP render-discovery fallback selection must remain visible"
    Assert-True ($src -match '(?s)TryPatchTmpUpdateManagerDiscovery.*?"DoRebuilds".*?TMPUpdateManagerDiscoveryPostfix') "TMP registry discovery must hook the renderer manager's per-frame ownership boundary"
    Assert-True ($src -match '(?s)private static void TMPUpdateManagerDiscoveryPostfix.*?DiscoverTmpUpdateManagerRegistry') "the TMP manager hook must feed registered objects into the discovery pipeline"
    Assert-True ($src.Contains("Hooked TMPro.TMP_UpdateManager.DoRebuilds for registry discovery")) "TMP manager registry hook selection must remain visible"
    Assert-True ($src -match '(?s)DiscoverTmpUpdateManagerRegistry.*?!ReferenceEquals\(val, null\).*?IsComponentActive\(val\)') "the stripped TMP registry boundary must use CLR reference null semantics before Unity liveness checks"
}

It "Unity cached activation text is applied before the first visible frame" {
    Assert-True ($src.Contains("ApplyCachedComponentTranslationNow")) "activation paths need a direct main-thread apply helper for local cache hits"
    Assert-True ($src -match '(?s)private bool QueueCachedComponentTextIfAvailable.*?TryGetLocalTranslation\(currentComponentText, out var translated\).*?ApplyCachedComponentTranslationNow\(component, componentInstanceId, currentComponentText, translated, isTmp\)') "OnEnable/SetActive cache hits must be written immediately instead of waiting in the apply queue"
    Assert-False ($src -match '(?s)private bool QueueCachedComponentTextIfAvailable.*?TryGetLocalTranslation\(currentComponentText, out var translated\)\)\s*\{\s*QueueTranslationApply') "known translations must not expose source text while waiting for a later flush"
    Assert-True ($src -match '(?s)private static void TMPTextOnEnablePostfix.*?QueueCachedComponentTextIfAvailable') "TMP OnEnable must keep using the first-frame cache path"
    Assert-True ($src -match '(?s)private static void CanvasGroupAlphaPostfix.*?QueueCachedTextsInHierarchy') "CanvasGroup visibility changes must keep using the first-frame cache path"
}

It "Unity primes bounded local caches before hooks can render source text" {
    Assert-True ($src.Contains("FirstFrameLocalCacheFileBytes")) "first-frame cache loading must have a strict file-size bound"
    Assert-True ($src.Contains("FirstFrameLocalCacheEntryLimit")) "first-frame cache loading must have a strict entry-count bound"
    Assert-True ($src -match '(?s)LoadGlossary\(\);\s*PrimeSmallLocalCacheForFirstFrame\(\);\s*_ = BootCacheLoadAsync\(\);') "small local caches must be ready before Unity text hooks are installed"
    Assert-True ($src -match '(?s)private void PrimeSmallLocalCacheForFirstFrame\(\).*?FileInfo\(path\)\.Length > FirstFrameLocalCacheFileBytes.*?val.Count > FirstFrameLocalCacheEntryLimit.*?ImportServerCacheEntries') "first-frame priming must reject large files before importing entries"
    Assert-True ($src -match '(?s)private async Task BootCacheLoadAsync\(\).*?if \(!_firstFrameLocalCachePrimed\).*?await RunBackground') "large caches must retain the background import path"
}

It "Unity first-translation latency stays within the interactive budget" {
    Assert-True ((Get-NumericConstant $src "UiTextSettleDebounceSeconds") -le 0.04) "stable UI text should begin translation within 40ms"
    Assert-True ((Get-NumericConstant $src "TextSettleDebounceSeconds") -le 0.22) "stable dialogue should begin translation within 220ms"
    Assert-True ((Get-NumericConstant $src "TypewriterFragmentDebounceSeconds") -le 0.65) "completed typewriter text should not wait more than 650ms"
    Assert-True ((Get-NumericConstant $src "MaxClientBatchSize") -ge 16) "Unity client batches should match the local server's 16-item batch capacity"
    Assert-True ((Get-NumericConstant $src "MaxDebouncedStartsPerTick") -ge 20) "normal mode should start enough ready translations per frame"
    Assert-True ((Get-NumericConstant $src "MaxPendingApplyPerFlush") -ge 12) "normal mode should write back a scene-sized response without a long frame tail"
    Assert-True ($src.Contains("GetMaxDebouncedStartsPerTick")) "translation start throughput must remain performance-mode aware"
    Assert-True ($src -match '(?s)private int GetMaxDebouncedStartsPerTick\(\).*?IsHighPerformance\(\).*?return 48;.*?IsEcoPerformance\(\).*?return 8;.*?return MaxDebouncedStartsPerTick;') "high/eco modes need explicit translation-start budgets"
    Assert-True ($src -match '(?s)private int GetMaxPendingApplyPerFlush\(\).*?IsHighPerformance\(\).*?return 24;.*?IsEcoPerformance\(\).*?return 4;.*?return MaxPendingApplyPerFlush;') "high/eco modes need explicit translation-writeback budgets"
    Assert-True ($src.Contains("list.Count >= GetMaxDebouncedStartsPerTick()")) "debounced work must consume the performance-aware start budget"
}

It "Unity long dialogue uses bounded lossless parallel segments" {
    Assert-True (Test-Path -LiteralPath $longTextPlannerPath) "Unity long-text planner source must exist"
    Add-Type -Path $longTextPlannerPath

    $short = [UnityLongTextPlanner]::Plan("A short dialogue line.", 480, 320, 4)
    Assert-True ($null -eq $short) "short Unity text must stay on the existing atomic request path"

    $long = (@(
        "First sentence carries enough context for a long game dialogue and ends safely.",
        "Second sentence includes __DS_TOKEN_0__ and keeps the protected token whole.",
        "Third sentence continues the scene with additional descriptive language for testing.",
        "Fourth sentence provides another complete thought that can translate independently.",
        "Fifth sentence closes the passage without changing spacing or punctuation."
    ) -join "  ")
    $long = $long + "`n" + $long
    $parts = [UnityLongTextPlanner]::Plan($long, 480, 320, 4)
    Assert-True ($null -ne $parts -and $parts.Count -ge 2 -and $parts.Count -le 4) "long Unity dialogue must fan out to two through four segments"
    $rebuilt = (($parts | ForEach-Object { $_.LeadingWhitespace + $_.Text + $_.TrailingWhitespace }) -join "")
    Assert-True ($rebuilt -ceq $long) "segment planning must preserve every source character and separator"
    Assert-True ((@($parts | Where-Object { $_.Text -match '__DS_TOKEN_[0-9]+$|^_[A-Z]*TOKEN_' }).Count) -eq 0) "protected tokens must never be cut across segment boundaries"

    $unsafe = [UnityLongTextPlanner]::Plan(("X" * 900), 480, 320, 4)
    Assert-True ($null -eq $unsafe) "text without a safe sentence or whitespace boundary must retain the atomic path"

    $cjkSentence = "これは空白を使わない長い会話文で、安全な全角句読点の位置だけを候補にします。次の文章も十分な長さを持たせ、単語や制御情報の途中では分割しません。"
    $cjkLong = $cjkSentence * 8
    $cjkParts = [UnityLongTextPlanner]::Plan($cjkLong, 480, 320, 4)
    Assert-True ($null -ne $cjkParts -and $cjkParts.Count -ge 2) "CJK dialogue without spaces must split after full-width sentence punctuation"
    $rebuiltCjk = (($cjkParts | ForEach-Object { $_.LeadingWhitespace + $_.Text + $_.TrailingWhitespace }) -join "")
    Assert-True ($rebuiltCjk -ceq $cjkLong) "CJK segmentation must remain lossless"

    Assert-True ($src.Contains("TryQueueLongTextTranslation")) "long dialogue must enter the Unity-only fan-out path"
    Assert-True ($src.Contains("GetBaseRequestDomain")) "parallel lane names must not leak into the server domain contract"
    Assert-True ($src -match 'BuildBatchPayload\(requests\.Select\(.*?GetBaseRequestDomain\(domain\)\)') "Unity long-text lanes must send the original domain to the shared server"
    Assert-True ((Get-NumericConstant $src "MaxParallelLongTextSegments") -eq 4) "long-text fan-out must stay bounded to four lanes"
    Assert-True ($src.Contains("StoreCachedTranslation(state.OriginalText, combined)")) "only a fully recombined long translation may populate the full-text cache"
    Assert-True ($src -match '(?s)CompleteLongTextSegment.*?state\.Failed.*?IsAcceptableTranslation.*?InvokeLongTextCallbacks') "a failed segment must keep the full source unresolved and retryable"
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

It "Unity core private members stay connected to runtime paths" {
    $implicitUnityMessages = @(
        "Awake", "Start", "Update", "LateUpdate", "FixedUpdate",
        "OnEnable", "OnDisable", "OnDestroy", "OnApplicationQuit", "OnGUI", "Reset", "Validate"
    )

    foreach ($path in $unityCoreSourcePaths) {
        $source = Get-Content -LiteralPath $path -Raw
        # 注释中的名字不能替未接线代码“续命”；保留字符串字面量，因为 Harmony
        # 的方法入口确实通过字符串名接线。
        $clean = [regex]::Replace(
            $source,
            '/\*.*?\*/',
            '',
            [System.Text.RegularExpressions.RegexOptions]::Singleline
        )
        $clean = [regex]::Replace($clean, '(?m)^\s*//.*(?:\r?\n|$)', '')

        $dead = [System.Collections.Generic.List[string]]::new()
        foreach ($match in [regex]::Matches($clean, '(?m)^\s*private\s+(?:static\s+)?(?:async\s+)?[\w<>,\.\[\]\?]+\s+([A-Za-z_]\w*)\s*\(')) {
            $name = $match.Groups[1].Value
            $count = [regex]::Matches($clean, '(?<![A-Za-z0-9_])' + [regex]::Escape($name) + '(?![A-Za-z0-9_])').Count
            if ($count -eq 1 -and $implicitUnityMessages -notcontains $name) {
                $dead.Add("method $name")
            }
        }
        foreach ($match in [regex]::Matches($clean, '(?m)^\s*private\s+(?:static\s+)?(?:readonly\s+)?(?:volatile\s+)?(?:const\s+)?[\w<>,\.\[\]\?]+\s+(_?[A-Za-z_]\w*)\s*(?:=|;)')) {
            $name = $match.Groups[1].Value
            $count = [regex]::Matches($clean, '(?<![A-Za-z0-9_])' + [regex]::Escape($name) + '(?![A-Za-z0-9_])').Count
            if ($count -eq 1) {
                $dead.Add("member $name")
            }
        }

        Assert-True ($dead.Count -eq 0) ("unreferenced private symbols in {0}: {1}" -f (Split-Path $path -Leaf), (($dead | Sort-Object -Unique) -join ", "))
    }

    Assert-False ($src.Contains("TMPSetTextMethodPrefix")) "removed pass-through Harmony prefix must not return as dead code"
    Assert-False ($src.Contains("_subMeshRescuedCount")) "write-only diagnostic counter must not return"
    Assert-False ($src -match 'private\s+(?:(?:static\s+readonly)|const)\s+bool\s+\w+\s*=\s*(?:true|false)') "hard-coded feature switches must not masquerade as active runtime configuration"
    Assert-False ($src.Contains("//IL_")) "decompiler warning comments must not obscure maintained source"

    $tmpFallbackSource = Get-Content -LiteralPath $tmpFallbackPath -Raw
    Assert-False ($tmpFallbackSource.Contains("CreateUnityFont(path)")) "UGUI system-font path branch is unreachable and must not return"
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
    Assert-True ((Get-NumericConstant $src "TypewriterFragmentDebounceSeconds") -ge 0.5) "fragment settle delay must remain long enough to reject mid-typewriter pauses"
    Assert-True ((Get-NumericConstant $src "TypewriterFragmentDebounceSeconds") -gt (Get-NumericConstant $src "TextSettleDebounceSeconds")) "fragment suspects must wait longer than stable dialogue"
    Assert-True ($src -match '(?s)private static float GetTextSettleDelaySeconds\(string text\)\s*\{\s*if \(LooksLikeTypewriterFragment\(text\)\)\s*\{\s*return TypewriterFragmentDebounceSeconds;') "fragment suspects must use the named long settle delay"
    Assert-False ($src.Contains("ContainsCjk(text) || LooksLikeTypewriterFragment(text) || ShouldSkipText(text)")) "debounce queue entry must not hard-reject fragments"
    Assert-False ($src.Contains("if (LooksLikeTypewriterFragment(item2.Text))")) "debounce flush must not hard-reject settled fragments"
    Assert-False ($src -match '(?s)private bool TryGetLocalTranslation\(string text, out string translated\).{0,400}?LooksLikeTypewriterFragment') "local cache lookup must not reject fragment-looking final text"
    Assert-True ($src.Contains("if (LooksLikeTypewriterFragment(key))")) "bulk cache imports must still filter legacy fragment keys"
    Assert-True ($src.Contains("text2[0] == '<' && text2.IndexOf('>') < 0")) "dangling rich-text fragments must be rejected"
    Assert-True ($src.Contains('text2.IndexOf(''\u2026'') >= 0')) "ellipsis fragments must be rejected"
}

It "Unity debounce stability follows source changes instead of repeated observations" {
    $queueMethod = [regex]::Match($src, '(?s)private void QueueDebouncedTextRequest\(.*?(?=\s*private static string GetCurrentComponentText)').Value
    Assert-True ($queueMethod -match '(?s)_debouncedTextRequests\.TryGetValue\(instanceId, out var existing\).*?ReferenceEquals\(existing\.ComponentRef\?\.Target, component\).*?IsSameSourceText\(existing\.Text, text\).*?return;.*?_debouncedTextRequests\[instanceId\] = new DebouncedTextRequest') "re-observing the same component text must preserve its original settle timestamp"
}

It "UGUI dialogue preserves source length without polluting cached translations" {
    Assert-True ($src.Contains('private const string UguiTypewriterLengthPaddingTag = "<color=#00000000></color>";')) "UGUI typewriter compatibility must use an invisible renderer-local marker"
    Assert-True ($src -match '(?s)private static string PreserveUguiTypewriterSourceLength\(Text component, string prepared, string sourceText\).*?LooksLikeNaturalLanguage\(visibleSource\).*?while \(builder\.Length < sourceText\.Length\).*?builder\.Append\(UguiTypewriterLengthPaddingTag\)') "long natural-language translations must keep the source string length seen by typewriter controllers"
    Assert-True ($src -match '(?s)private static string PrepareTranslatedTextForUGUIText\(.*?return PreserveUguiTypewriterSourceLength\(component, prepared, sourceText\);') "UGUI display preparation must apply length compatibility at the renderer boundary"
    $storeMethod = [regex]::Match($src, '(?s)private void StoreCachedTranslation\(.*?(?=\s*private )').Value
    Assert-False ($storeMethod.Contains('PreserveUguiTypewriterSourceLength')) "renderer-only padding must never enter the shared or local translation cache"
}

It "Unity activation scans run only on inactive-to-active edges" {
    Assert-True ($src -match '(?s)private static bool GameObjectSetActivePrefix\(GameObject __instance, bool value, ref bool __state\).*?__state = __instance\.activeSelf;') "SetActive prefix must capture the previous activeSelf state"
    Assert-True ($src -match '(?s)private static void GameObjectSetActivePostfix\(GameObject __instance, bool value, bool __state\).*?value && !__state.*?QueueCachedTextsInHierarchy') "repeated SetActive(true) calls must not rescan and requeue an already-active UI hierarchy"
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

It "Unity strips model prompt echoes before accepting or persisting translations" {
    Assert-True ($src.Contains("StripTranslationPromptEchoPrefix")) "Unity Mono must have a prompt-echo sanitizer"
    Assert-True ($src -match '(?s)private static string SanitizeTranslationArtifacts\(string text\).*?StripTranslationPromptEchoPrefix\(text\)') "all Mono cache and response imports must pass through prompt-echo cleanup"
    Assert-True ($src.Contains('\u7ffb\u8bd1\u6210\u7b80\u4f53\u4e2d\u6587')) "Mono cleanup must cover the real Chinese prompt echo"
    Assert-True ($il2cppEndpointSrc.Contains("StripTranslationPromptEchoPrefix")) "XUnity must defensively clean prompt echoes from compatible servers and old caches"
    Assert-True ($il2cppEndpointSrc -match '(?s)private string PrepareDisplayTranslation\(string value\).*?StripTranslationPromptEchoPrefix\(value\)') "single XUnity results must be cleaned before display"
    Assert-True ($il2cppEndpointSrc -match '(?s)private string\[\] PrepareDisplayTranslations\(string\[\] values\).*?StripTranslationPromptEchoPrefix\(values\[i\]\)') "batch XUnity results must be cleaned before display"
}

It "TMP overlay has a visible-render guard" {
    Assert-True ($src.Contains("GetComponentInParent<Canvas>()")) "overlay must not hide TMP when it cannot render on a Canvas"
    Assert-True ($src.Contains("SetParent(val6, false)")) "overlay should be parented beside the TMP component for UI draw order"
    Assert-True ($src.Contains("SetAllDirty()")) "overlay graphic must be marked dirty after text assignment"
    Assert-True ($src.Contains("private bool ApplyTmpOverlay(")) "overlay creation must report whether a visible replacement was established"
    Assert-True ($src -match '(?s)private bool ApplyTmpOverlay\(.*?IsUsableFontForText\(_chineseFont, translatedText\).*?return false;.*?HideTmpSourceForOverlay\(tmpComponent, .*?\).*?return true;') "TMP source renderer must be hidden only after the overlay font proves it can render the translated text"
    Assert-True ($src -match '(?s)if \(!canTranslateTmp\).*?if \(!ApplyTmpOverlay\(comp, translated, rawText\)\).*?return;.*?MarkProcessed') "a failed overlay must preserve the original TMP text and remain eligible for retry"
    Assert-True ($src.Contains("private bool EnsureTmpOverlayGeometry(Text overlayText, TmpOverlayState state)")) "overlay success must include generated UGUI geometry, not only a live component"
    Assert-True ($src -match '(?s)EnsureTmpOverlayGeometry\(.*?characterCountVisible.*?materialCount.*?Rebuild\(CanvasUpdate\.PreRender\)') "an active stripped-player overlay must synchronously retry its missing pre-render rebuild"
    Assert-True ($src -match '(?s)ApplyTmpOverlay\(.*?if \(!EnsureTmpOverlayGeometry\(overlayText, tmpOverlayState\)\).*?RestoreTmpSourceVisibility\(tmpComponent, tmpOverlayState\).*?return false;.*?HideTmpSourceForOverlay\(tmpComponent, tmpOverlayState\)') "TMP source text must stay visible until the overlay has real vertices and a material"
    Assert-True ($src -match '(?s)class TmpOverlayState.*?sourceWasEnabled.*?hasSourceEnabledState') "overlay state must remember whether the original TMP renderer was enabled"
    Assert-True ($src -match '(?s)private static void HideTmpSourceForOverlay\(.*?!state\.hasSourceEnabledState.*?sourceWasEnabled = sourceBehaviour\.enabled.*?enabled = false.*?SetTmpAlpha\(tmpComponent, 0f\)') "a verified overlay must capture and disable the TMP renderer at the same ownership boundary"
    Assert-True ($src -match '(?s)private static void RestoreTmpSourceVisibility\(.*?hasSourceEnabledState.*?enabled = state\.sourceWasEnabled') "overlay fallback and teardown must restore the source renderer's original enabled state"
    Assert-True ($src -match '(?s)ValidateActiveTmpOverlays\(.*?EnsureTmpOverlayGeometry.*?HideTmpSourceForOverlay') "overlay validation must reassert source ownership after game-side renderer refreshes"
    $applyOverlayMethod = [regex]::Match($src, '(?s)private bool ApplyTmpOverlay\(.*?(?=\s*private static Color GetTmpOverlayDisplayColor)').Value
    Assert-False ($applyOverlayMethod -match 'sourceWasEnabled\s*=\s*sourceBehaviour\.enabled') "inactive TMP templates must not snapshot enabled=false before a verified overlay actually takes renderer ownership"
    Assert-True ($src -match '(?s)private void RescueStrandedAlpha\(object tmpComponent\).*?TmpOverlayState.*?overlayText.*?enabled.*?return;') "alpha rescue must not fight an active verified TMP overlay"
    Assert-True ($src.Contains("private static bool GetTmpAutoSizing(object tmpComponent)")) "overlay layout must be able to detect source TMP auto-sizing"
    Assert-True ($src -match '(?s)RefreshTmpOverlayLiveStyle\(.*?bool tmpAutoSizing = GetTmpAutoSizing\(tmpComponent\).*?overlayText\.resizeTextForBestFit = tmpAutoSizing.*?GetTmpFontSizeMin\(tmpComponent.*?GetTmpFontSizeMax\(tmpComponent') "UGUI overlays should mirror best-fit bounds only when the source TMP opted into auto-sizing"
    Assert-True ($src -match '(?s)ValidateActiveTmpOverlays\(.*?RefreshTmpOverlayLiveStyle\(tmpOverlayState\.overlayText, obj, tmpOverlayState\).*?EnsureTmpOverlayGeometry') "active overlays must refresh geometry, wrapping, sizing, alignment, and color after a game-side setter finishes configuring its TMP component"
    Assert-True ($src -match '(?s)RefreshTmpOverlayLiveStyle\(.*?SetAsLastSibling.*?GetTmpFontSize\(tmpComponent\).*?GetTmpTextAnchor\(tmpComponent\).*?GetTmpWordWrap\(tmpComponent\).*?ApplyTmpOverlayRect\(overlayText, tmpComponent\).*?GetTmpOverlayDisplayColor') "live overlay refresh must mirror all renderer-owned style and rect properties used by dynamic text components"
}

It "TMP fallback overlays bypass UGUI self-hooks and expand collapsed source widths" {
    Assert-True ($src.Contains("private sealed class TmpOverlayMarker : MonoBehaviour")) "translator-owned UGUI overlays need an explicit marker"
    Assert-True ($src -match '(?s)private static bool UGUITextPrefix\(.*?IsTranslatorOwnedTmpOverlay\(__instance\).*?return true;') "plugin-owned overlays must bypass the ordinary UGUI translation and manual wrapping hook"
    Assert-True ($src -match '(?s)private static void UGUITextPostfix\(.*?IsTranslatorOwnedTmpOverlay\(__instance\).*?return;') "plugin-owned overlays must bypass ordinary UGUI post-layout mutation"
    Assert-True ($src -match '(?s)new GameObject\("__DeepSeekOverlay", new Type\[5\].*?typeof\(LayoutElement\).*?typeof\(TmpOverlayMarker\)') "every TMP fallback overlay must carry its layout and self-hook markers from creation"
    Assert-True ($src -match '(?s)GetComponent<LayoutElement>\(\).*?ignoreLayout = true') "a display-only TMP overlay must not be resized by its source parent layout group"
    Assert-True ($src -match '(?s)tmpOverlayState\.overlayText.*?raycastTarget = false') "a display-only TMP overlay must not intercept the game's buttons or pointer input"
    Assert-True ($src.Contains("private const float CollapsedTmpOverlayWidth = 24f;")) "collapsed TMP width detection needs an explicit renderer-local threshold"
    Assert-True ($src -match '(?s)ApplyTmpOverlayRect\(.*?Transform sourceParent = .*?\(\(Transform\)val3\)\.parent;.*?transform\.parent != sourceParent.*?transform\.SetParent\(sourceParent, false\).*?transform\.parent == sourceParent') "an overlay created while TMP was parentless must follow the source's live parent before copying its settled rect"
    Assert-True ($src -match '(?s)ApplyTmpOverlayRect\(.*?sourceWidth <= CollapsedTmpOverlayWidth.*?parentRect\.rect\.width > CollapsedTmpOverlayWidth.*?val\.anchorMin = Vector2\.zero.*?val\.anchorMax = Vector2\.one') "a zero-width TMP source must borrow its immediate usable parent rect instead of creating a zero-width translation"
    Assert-True ($src -match '(?s)sourceWidth <= sourceHeight \* NarrowTmpOverlayWidthToHeight.*?parentRect\.rect\.width >= sourceWidth \* NarrowTmpOverlayParentWidthMultiplier') "a TMP source that settles into a tall narrow column must keep borrowing the wider parent instead of reverting the translation to vertical text"
    Assert-False ($serverSrc.Contains("CollapsedTmpOverlayWidth")) "collapsed TMP layout repair must remain in the Unity renderer layer"
}

It "TMP external writes cannot be hidden by stale overlay re-entry" {
    Assert-True ($src -match '(?s)_harmony\.Patch\(\s*methodInfo,\s*new HarmonyMethod\(typeof\(DeepSeekTranslator\), "TMPSetTextPrefix"\),\s*new HarmonyMethod\(typeof\(DeepSeekTranslator\), "TMPSetTextPostfix"\)') "TMP text setters need a postfix ownership boundary"
    Assert-True ($src -match '(?s)_harmony\.Patch\(\s*methodInfo2,\s*new HarmonyMethod\(typeof\(DeepSeekTranslator\), "TMPSetTextAnyPrefix"\),\s*new HarmonyMethod\(typeof\(DeepSeekTranslator\), "TMPSetTextAnyPostfix"\),\s*null,\s*new HarmonyMethod\(typeof\(DeepSeekTranslator\), "TMPSetTextAnyFinalizer"\)') "every TMP SetText overload needs paired completion and exception boundaries"
    Assert-True ($src -match '(?s)TMPSetTextAnyPrefix\(.*?out bool __state\).*?__state = true;.*?TMPSetTextPrefix\(__instance, ref value2\)') "the SetText overload prefix must record whether it opened external-write ownership"
    Assert-True ($src -match '(?s)TMPSetTextAnyPostfix\(.*?bool __state\).*?EndTmpExternalTextWrite\(__instance, value\)') "successful SetText overloads must close external-write ownership"
    Assert-True ($src -match '(?s)TMPSetTextAnyFinalizer\(.*?Exception __exception, bool __state\).*?ResetTmpExternalWriteDepthAfterSetterException') "throwing SetText overloads must reset translator-owned depth without swallowing the game exception"
    Assert-True ($src -match '(?s)class TmpOverlayState.*?pendingExternalWriteDepth.*?pendingExternalTextNormalized') "overlay state must track an in-flight game-side TMP write"
    Assert-True ($src -match '(?s)TMPSetTextPrefix\(.*?BeginTmpExternalTextWrite\(__instance, value\).*?RestoreTmpOverlay\(__instance\)') "the game-side write must be marked before stale overlay state is restored"
    Assert-True ($src -match '(?s)TMPSetTextPostfix\(.*?EndTmpExternalTextWrite\(__instance, value\)') "the postfix must close the in-flight write after the original setter"
    Assert-True ($src -match '(?s)QueueCachedComponentTextIfAvailable\(.*?HasPendingTmpExternalTextWrite\(component\).*?return;') "the scanner must not reapply an overlay from the old TMP text while a setter is in flight"
    Assert-True ($src -match '(?s)ValidateActiveTmpOverlays\(.*?pendingExternalWriteDepth > 0.*?continue;') "overlay validation must not take renderer ownership during an external setter"
    Assert-True ($src -match '(?s)EndTmpExternalTextWrite\(.*?EnsureTmpOverlayMatchesCurrentText\(tmpComponent, currentText\)') "the completed setter must reconcile ownership against the newly committed text"
    Assert-True ($src.Contains("[OVERLAY-OWNERSHIP] A game-side TMP text setter replaced an overlaid label")) "external-write ownership fallback must emit a bounded diagnostic"
}

It "TMP overlay and rich-text restore preserve original colors" {
    Assert-True ($src.Contains("private static Color GetTmpOverlayDisplayColor")) "TMP overlay must compute display color from the current component color"
    Assert-True ($src.Contains("GetTmpOverlayDisplayColor(tmpComponent, tmpColor, state)")) "overlay color must include the live TMP component and not be frozen to the first cached color"
    Assert-True ($src -match '(?s)GetTmpOverlayDisplayColor\(object tmpComponent, Color currentColor, TmpOverlayState state\).*?TryReadTmpFaceColor\(tmpComponent, out Color faceColor\).*?result\.r \*= faceColor\.r.*?result\.g \*= faceColor\.g.*?result\.b \*= faceColor\.b') "UGUI overlays must combine TMP vertex color with the live TMP face color"
    Assert-True ($src -match '(?s)TryReadTmpFaceColor\(object tmpComponent, out Color color\).*?GetPropertyQuiet\(.*?"faceColor"\).*?Color32.*?ReadFontSharedMaterial.*?_FaceColor') "stripped TMP face color should use Color32 directly with a material fallback"
    $restoreVisibilityMethod = [regex]::Match($src, '(?s)private static void RestoreTmpSourceVisibility\(.*?(?=\s*private static void HideTmpSourceForOverlay)').Value
    Assert-False ($restoreVisibilityMethod.Contains("SetTmpColor(")) "overlay teardown must not overwrite RGB that the game changed while the source was hidden"
    Assert-True ($restoreVisibilityMethod -match 'SetTmpAlpha\(tmpComponent, state\.originalColor\.a\)') "overlay teardown should restore only the alpha owned by the plugin"
    Assert-True ($src -match '(?s)RefreshTmpOverlayLiveStyle\(.*?Color tmpColor = GetTmpColor\(tmpComponent\).*?originalColor\.r = tmpColor\.r.*?originalColor\.g = tmpColor\.g.*?originalColor\.b = tmpColor\.b') "overlay state must keep tracking live game-owned RGB even when source alpha is hidden"
    Assert-True ($src.Contains("[OVERLAY-COLOR] UGUI fallback is using the live TMP vertex/face color")) "live-color ownership fallback must emit a bounded diagnostic"
    Assert-False ($src.Contains("Color val7 = (tmpOverlayState.hasOriginalColor ? tmpOverlayState.originalColor : tmpColor);")) "overlay must not reuse stale originalColor as display color"
    Assert-True ($src.Contains("RestoreOuterRichTextWrapper")) "lost outer color rich-text wrappers must be restored after translation"
    Assert-True ($src.Contains("OriginalText = text ?? string.Empty")) "rich-text restore needs the original text to rebuild missing wrappers"
    Assert-True ($src.Contains("RestoreOuterRichTextWrapper(text, payload?.OriginalText)")) "protected-text restore must use the captured original text"
    Assert-True ($src.Contains("PrepareTranslatedTextForUGUIText")) "UGUI Text writes must preserve color rich text when supported"
    Assert-True ($src.Contains("value = PrepareTranslatedTextForUGUIText(__instance, text, rawText);")) "UGUI sync translation must keep source color wrappers"
    Assert-True ($src.Contains("val.text = PrepareTranslatedTextForUGUIText(val, translated, originalText ?? translated, preserveRichText);")) "UGUI async/cached translation must keep source color wrappers"
    Assert-True ($src.Contains("PrepareTranslatedTextForComponent(component, translated, sourceForFormatting)")) "TMP formatting must use the original source text when restoring wrappers"
}

It "UGUI translated CJK keeps source line padding and wraps without resizing menu labels" {
    Assert-True ($src.Contains("RestoreOuterLineBreaks(prepared, sourceText)")) "UGUI translation must preserve leading/trailing source line breaks"
    Assert-True ($src.Contains("ApplyUGUITextLayoutCompatibility(component, prepared)")) "UGUI translation must pass through renderer-local layout compatibility"
    Assert-True ($src.Contains("WrapTranslatedCjkForUGUI")) "UGUI translation needs a local CJK wrapping helper"
    Assert-True ($src.Contains("component.horizontalOverflow = HorizontalWrapMode.Wrap")) "UGUI CJK text should wrap instead of overflowing into nearby UI"
    Assert-False ($src.Contains("component.resizeTextForBestFit = true")) "UGUI translation must not enable best-fit because it can enlarge translated main-menu labels"
    Assert-False ($src.Contains("component.resizeTextMaxSize")) "UGUI translation must not mutate max font size for existing menu/button layouts"
    Assert-False ($serverSrc.Contains("WrapTranslatedCjkForUGUI")) "UGUI wrapping must stay out of the shared server/cache layer"
}

It "UGUI translated short labels bypass stale preferred-width layout caches" {
    Assert-True ($src.Contains('"UGUITextPostfix"')) "UGUI text writes need a post-set layout compatibility pass"
    Assert-True ($src.Contains("FinalizeUGUITextLayoutCompatibility(__instance)")) "UGUI post-set hook must finalize the live layout"
    Assert-True ($src.Contains("PrepareUGUITranslatedShortLabelLayout(component, prepared, sourceText)")) "short translated labels must be classified where both source and translation are known"
    Assert-True ($src.Contains("RestoreUGUIShortLabelLayoutCompatibility")) "temporary short-label overflow must be restored before later source text is rendered"
    Assert-True ($src.Contains("ConditionalWeakTable<Text, UguiShortLabelLayoutState>")) "short-label renderer state must not retain destroyed scene Text components"
    Assert-True ($src.Contains("GetComponent<LayoutElement>()")) "short-label overflow must require explicit layout ownership"
    Assert-True ($src.Contains("GetComponent<ContentSizeFitter>()")) "content-sized labels must be recognized"
    Assert-True ($src.Contains("GetComponent<HorizontalOrVerticalLayoutGroup>()")) "layout-group-owned labels must be recognized"
    Assert-False ($src.Contains("preferredWidth <= rectWidth + 0.5f")) "post-set preferredWidth can still describe the old English label and must not gate the repair"
    Assert-True ($src.Contains("component.horizontalOverflow = HorizontalWrapMode.Overflow")) "clipped short labels must remain on one visible line"
    Assert-True ($src.Contains("LayoutRebuilder.MarkLayoutForRebuild(parentRect)")) "the owning layout must be asked to recompute its width"
    Assert-True ($src.Contains("[UGUI-LAYOUT] short-label overflow")) "layout fallback activation must be diagnosed"
    Assert-False ($serverSrc.Contains("FinalizeUGUITextLayoutCompatibility")) "UGUI layout repair must stay out of the shared server/cache layer"
}

It "Unity inventory quantity lists can be rebuilt from local item term translations" {
    Assert-True ($src.Contains("InventoryQuantityLineRegex")) "Unity Mono must recognize item quantity lines like 'fongo fruits x 3'"
    Assert-True ($src.Contains("TryTranslateQuantityListFromLocalTerms")) "Unity Mono must rebuild changing inventory lists locally"
    Assert-True ($src.Contains("TryInferTermTranslationFromCachedQuantityLists")) "Unity Mono must learn item names from previous inventory list cache hits"
    Assert-True ($src.Contains("TryInferTermTranslationFromRichTextSpans")) "Unity Mono must learn item names from highlighted rich-text item mentions"
    Assert-True ($src.Contains("SingularizeInventoryTerm")) "plural item counts should fall back to singular cached terms"
    Assert-False ($serverSrc.Contains("InventoryQuantityLineRegex")) "inventory-list rendering repair must stay out of the shared server/cache layer"
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

It "Unity renderer state has scene and generation reclamation" {
    Assert-True ($src -match '(?s)private void ResetSceneScopedState\(bool clearPendingComponentWork\).*?_deepScannedObjects\.Clear\(\);.*?_deepPrefetchSeen\.Clear\(\);.*?_deepPrefetchQueue\.Clear\(\);') "Mono scene reset must release deep-scan and prefetch generation state"
    Assert-True ($src -match '(?s)private void ResetSceneScopedState\(bool clearPendingComponentWork\).*?lock \(HistoryComponentCache\).*?HistoryComponentCache\.Clear\(\);') "Mono scene reset must release cached scene component IDs"
    Assert-True ($src.Contains("_deepScannedObjects.Count > MaxTrackedComponentStates")) "Mono long-running pruning must bound deep-scanned object IDs"

    Assert-True ($tmpFallbackSrc.Contains("SweepStaleState()")) "IL2CPP TMP installer must periodically sweep destroyed Unity objects"
    Assert-True ($tmpFallbackSrc -match '(?s)public static void Apply\(\).*?SweepStaleState\(\);') "IL2CPP stale-state sweep must be wired into the slow Apply path"
    Assert-True ($tmpFallbackSrc.Contains("PatchedFontAssets.IntersectWith(")) "IL2CPP patched font IDs must be mark/swept"
    Assert-True ($tmpFallbackSrc.Contains("DirtiedTexts.IntersectWith(")) "IL2CPP TMP text IDs must be mark/swept"
    Assert-True ($tmpFallbackSrc.Contains("PatchedUguiTexts.IntersectWith(")) "IL2CPP UGUI text IDs must be mark/swept"
    Assert-True ($tmpFallbackSrc.Contains("LastTmpTextById.Remove(")) "IL2CPP last-text cache must remove destroyed text IDs"
    Assert-True ($tmpFallbackSrc -match '(?s)private static void SweepStaleState\(\).*?foreach \(int sourceId in StaleInteractiveSourceIds\).*?DestroyInteractiveOverlay\(sourceId\);.*?InteractiveOriginalTextBySourceId\.Remove\(sourceId\);.*?InteractiveOriginalColorBySourceId\.Remove\(sourceId\);') "IL2CPP stale interactive overlays must release strong Unity object references and source metadata"
}

It "Unity background work and diagnostics avoid unmanaged thread and IO churn" {
    Assert-True ($src -match '(?s)private static Task<T> RunBackground<T>\(Func<T> work\).*?ThreadPool\.QueueUserWorkItem') "background HTTP/cache work must reuse the managed thread pool"
    Assert-False ($src -match '(?s)private static Task<T> RunBackground<T>\(Func<T> work\).*?new Thread') "each request must not create a dedicated OS thread"
    Assert-True ($src -match '(?s)private void Awake\(\).*?if \(_debugMode\.Value\)\s*\{.*?DST_Diag') "continuous diagnostic files must be opt-in"
    Assert-False ($src -match '(?s)private void Awake\(\).*?if \(!_debugMode\.Value\)\s*\{.*?return;') "optional diagnostics must not early-return from Awake"
    Assert-True ($src.Contains("_diagnosticsStop.WaitOne")) "diagnostic thread must have a plugin-lifetime stop signal"
    Assert-True ($src -match '(?s)private void OnDestroy\(\).*?_diagnosticsStop\?\.Set\(\);') "plugin teardown must stop diagnostic work"
    Assert-False ($src -match '(?s)private void FlushDebouncedTextRequests\(\).*?_debouncedTextRequests\.ToList\(\)') "main-thread debounce pump must not clone the dictionary every tick"
    Assert-True ($src -match '(?s)private static string RawHttpRequest.*?memoryStream\.GetBuffer\(\).*?memoryStream\.Length') "raw HTTP response decoding must avoid a full ToArray copy"
    Assert-False ($src.Contains('Version=3.1.97')) "diagnostic output must not report a stale plugin version"
}

It "Unity exception boundaries never swallow failures silently" {
    $emptyCatch = 'catch(?:\s*\([^\)]*\))?\s*\{\s*\}'
    Assert-False ([regex]::IsMatch($src, $emptyCatch, [System.Text.RegularExpressions.RegexOptions]::Singleline)) "Unity Mono must not contain empty catch blocks"
    Assert-False ([regex]::IsMatch($tmpFallbackSrc, $emptyCatch, [System.Text.RegularExpressions.RegexOptions]::Singleline)) "IL2CPP TMP fallback must not contain empty catch blocks"
    Assert-True ($src.Contains("ReportCaughtException")) "Unity Mono compatibility fallbacks must report method and exception diagnostics"
    Assert-True ($tmpFallbackSrc.Contains("ReportCaughtException")) "IL2CPP TMP compatibility fallbacks must report method and exception diagnostics"
    foreach ($boundary in @(
        "boundary=batch-callback",
        "boundary=long-text-callback",
        "boundary=batch-callback-dispatch",
        "boundary=local-cache-persist-cycle",
        "boundary=local-cache-persist-loop"
    )) {
        Assert-True ($src.Contains($boundary)) "Unity Mono failures at $boundary must remain visible with DebugMode disabled"
    }
}

It "Unity Mono runtime has one active pump with a host fallback" {
    Assert-True ($src -match '(?s)private sealed class TranslatorDriver.*?private void Update\(\)\s*\{\s*Owner\?\.DriverUpdate\(\);') "the dedicated driver must remain the normal main-thread pump"
    Assert-True ($src -match '(?s)private void Update\(\).*?if \(\(Object\)\(object\)_driver == \(Object\)null \|\| !\(\(Behaviour\)_driver\)\.isActiveAndEnabled\)\s*\{\s*DriverUpdate\(\);') "the plugin host Update must only pump when the dedicated driver is unavailable"
    Assert-False ($src -match '(?s)private void Update\(\)\s*\{\s*DriverUpdate\(\);\s*\}') "the plugin host and dedicated driver must not both run PumpOnce every frame"
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

It "Unity Fungus prefetches upcoming block text without delaying the visible line" {
    $fungusPrefetch = [regex]::Match($src, '(?s)private async Task PrefetchServerTextsAsync\(.*?(?=\s*private void ReleaseFungusPrefetchKeys)').Value
    Assert-True ($src.Contains("private const int FungusLookaheadMaxCommands")) "Fungus lookahead must be explicitly bounded"
    Assert-True ((Get-NumericConstant $src "FungusLookaheadMaxCommands") -le 24) "Fungus lookahead must not scan an unbounded story block"
    Assert-True ($src.Contains("private void QueueFungusLookahead(object command)")) "Fungus commands need a targeted lookahead path"
    Assert-True ($src -match '(?s)QueueFungusLookahead\(object command\).*?ParentBlock.*?CommandIndex.*?CommandList') "Fungus lookahead must start after the active command in its own block"
    Assert-True ($src -match '(?s)QueueFungusLookahead\(object command\).*?Fungus\.Say.*?storyText.*?Fungus\.Menu.*?text') "Fungus lookahead must cover both dialogue and menu option commands"
    Assert-True ($src -match '(?s)InstallFungusHooks\(\).*?FungusSayCommandOnEnterPostfix.*?FungusMenuAddOptionPrefix') "Fungus hooks must use the command lifecycle and version-tolerant menu option hook"
    Assert-True ($src -match '(?s)private static void FungusSayCommandOnEnterPostfix.*?QueueFungusLookahead') "lookahead must run after the visible Say command has started"
    Assert-True ($fungusPrefetch.Contains('/prefetch')) "Fungus lookahead must use the non-blocking server prefetch endpoint"
    Assert-False ($fungusPrefetch.Contains('/batch')) "background Fungus lookahead must not occupy the synchronous visible-text endpoint"
    Assert-True ($src -match '(?s)ResetSceneScopedState\(bool clearPendingComponentWork\).*?_fungusPrefetchSeen\.Clear\(\);') "Fungus prefetch deduplication must be released with the scene"
}

It "Unity Fungus async writes preserve protected color tags" {
    $fungusAsync = [regex]::Match($src, '(?s)private void QueueFungusAsyncTranslation\(.*?(?=\s*private void ApplyTranslation)').Value
    $colorSource = 'Mom!<color=#FFFFFF00> I am home!</color>'
    Assert-True ($regex.IsMatch($colorSource)) "Unity rich-text protection must recognize Fungus color tags"
    Assert-True ((Get-VisibleText $colorSource) -eq 'Mom! I am home!') "color tags must stay structural rather than entering API-visible text"
    Assert-True ($fungusAsync.Contains("ContainsColorRichTextTag(originalText)")) "Fungus async rendering must detect source color tags"
    Assert-True ($fungusAsync.Contains("preserveRichText: preserveColorTags")) "Fungus async apply must preserve color tags when present"
    Assert-False ($fungusAsync.Contains("preserveRichText: false")) "Fungus async apply must not unconditionally strip protected color tags"
}

It "Unity boot cache import runs off the main thread" {
    # Importing a six-digit local cache inline in Awake (regex validators per
    # row) froze game startup for seconds. Texts shown before the cache lands
    # are healed by scanner cache-apply passes, like deferred-sync arrivals.
    Assert-True ($src -match '(?s)private async Task BootCacheLoadAsync\(\).{0,900}?await RunBackground\(delegate\s*\{\s*LoadServerCache\(\);') "boot cache import must run via RunBackground"
    Assert-True ($src.Contains("_ = BootCacheLoadAsync();")) "Awake must fire the boot cache load asynchronously"
    Assert-False ($src -match '(?s)LoadGlossary\(\);\s*LoadServerCache\(\);') "Awake must not import the local cache inline"
    Assert-True ($src -match '(?s)private async Task BootCacheLoadAsync\(\).{0,2000}?StartServerCacheSync\(\);') "server sync decision must wait for the local cache count"
    Assert-False ($src -match '(?s)private void LoadServerCache\(\).*?_cache\.Clear\(\);') "background boot import must not erase translations accepted after hooks start"
    Assert-False ($src -match '(?s)private void LoadServerCache\(\).*?_localCacheKeys\.Clear\(\);') "background boot import must not erase live per-game persistence keys"
    Assert-True ($src -match '(?s)private void LoadServerCache\(\).*?markImportedAsLocal: true, preserveExisting: true') "background local-cache import must preserve a newer live translation for the same key"
    Assert-True ($src -match '(?s)private int ImportServerCacheEntries\(.*?bool preserveExisting = false\).*?preserveExisting.*?_cache\.ContainsKey') "cache import must implement the live-entry preservation contract under the cache lock"
    Assert-True ($src -match '(?s)private bool TryGetLocalTranslation\(string text, out string translated\).*?lock \(_cache\)\s*\{\s*if \(_glossary\.TryGetValue\(text, out var value\)\)') "glossary fast path must lock against the background importer"
}

It "Unity Mono teardown does not retain scene objects or race the final cache write" {
    $scheduleAsyncApply = [regex]::Match($src, '(?s)private void ScheduleAsyncApply\(.*?(?=\s*private IEnumerator SceneWarmupCoroutine)').Value
    Assert-True ($src.Contains("private volatile bool _shuttingDown;")) "fire-and-forget work needs a plugin teardown gate"
    Assert-True ($src -match '(?s)private void OnDestroy\(\).*?_shuttingDown = true;.*?ReferenceEquals\(_instance, this\).*?_instance = null;') "teardown must stop new work and release the static plugin reference"
    Assert-True ($scheduleAsyncApply -match '(?s)WeakReference componentRef = new WeakReference\(component\);.*?RequestSharedTranslation') "remote callbacks must hold scene components weakly"
    Assert-False ($scheduleAsyncApply.Contains("QueueTranslationApply(component,")) "remote callbacks must not capture a strong scene component reference"
    Assert-True ($src.Contains("private readonly object _cacheFileWriteLock = new object();")) "cache writes need a dedicated ordering lock"
    Assert-True ($src -match '(?s)private async Task PersistLocalCacheAsync\(\).*?lock \(_cacheFileWriteLock\).*?_shuttingDown') "background persistence must serialize with and yield to final teardown persistence"
    Assert-True ($src -match '(?s)private void FlushLocalCacheToDisk\(\).*?lock \(_cacheFileWriteLock\)') "the final cache snapshot must share the persistence ordering lock"
}

It "Oversized local caches are isolated without rewriting user data" {
    # A local cache file polluted by legacy full-server-dump persists (250k+
    # rows, 25 MB) made every persist cycle revalidate and rewrite the whole
    # file on the Unity main thread -- periodic multi-second in-game freezes.
    Assert-True ($src.Contains("private const int OversizedLocalCacheEntryLimit")) "oversized local cache threshold must exist"
    Assert-True ($src.Contains("private const long OversizedLocalCacheFileBytes")) "oversized local cache must be detected by file size before JSON parse"
    Assert-True ($src.Contains("new FileInfo(path).Length > OversizedLocalCacheFileBytes")) "huge polluted cache files must be skipped before parse"
    Assert-True ($src.Contains("val.Count > OversizedLocalCacheEntryLimit")) "local cache load must detect dump pollution"
    Assert-True ($src.Contains("TryIsolateOversizedLocalCacheLocked(path, val.Count);")) "polluted local caches must be isolated before any new snapshot can replace them"
    Assert-True ($src.Contains('".bak-oversized-" + timestamp')) "every isolated cache must use a recognizable unique timestamp suffix"
    Assert-True ($src.Contains("File.Move(path, text);")) "oversized cache isolation must retain the original bytes by moving the file"
    Assert-False ($src.Contains('File.WriteAllText(path, "{}", Encoding.UTF8);')) "oversized cache handling must never erase the active user artifact"
    Assert-True ($src -match '(?s)private void LoadServerCache\(\).*?lock \(_cacheFileWriteLock\).*?File\.Exists\(path\).*?TryIsolateOversizedLocalCacheLocked') "cache inspection and isolation must share the persistence file lock"
    Assert-True ($src -match '(?s)private async Task PersistLocalCacheAsync\(\).*?!_bootCacheLoadComplete.*?continue;') "persistence must not overwrite an uninspected startup cache"
    Assert-True ($src -match '(?s)bool persisted = false;.*?return dictionary\.Count == 0 \|\| WriteLocalCacheSnapshot\(dictionary\);.*?if \(!persisted.*?_cachePersistDirty = true;') "failed atomic writes must retain dirty state for a bounded retry"
    Assert-True ($src.Contains("private bool WriteLocalCacheSnapshot(IReadOnlyDictionary<string, string> snapshot)")) "cache snapshot writes must report success to their owner"
}

It "Fire-and-forget scheduler flags cannot wedge on exceptions" {
    # _batchFlushScheduled / _cachePersistScheduled gate their own reschedule:
    # if the owning task dies without clearing the flag or restarting, the
    # translation pipeline or cache persistence silently stops for the session.
    Assert-True ($src -match '(?s)finally\s*\{.{0,1500}?_ = FlushPendingBatchRequestsAsync\(faulted \? BatchFlushFaultRestartDelayMs : 0\);') "flush restart must run inside finally so catch-path exceptions cannot skip it"
    Assert-True ($src -match '(?s)catch \(Exception ex\)\s*\{\s*faulted = true;') "flush failures must mark the cycle as faulted"
    Assert-True ($src.Contains("private const int BatchFlushFaultRestartDelayMs")) "faulted flush restarts must be rate-limited, not a hot loop"
    Assert-True ($src -match '(?s)\[BATCH\] Invoking callbacks for \{requests\.Count\} requests, \{groupResults\.Count\} results"\);\s*InvokePendingRequestCallbacks\(requests, groupResults\);\s*\}\s*catch') "group callback dispatch must not throw past the worker chain"
    Assert-True ($src -match '(?s)private async Task PersistLocalCacheAsync\(\)\s*\{\s*try\s*\{\s*while \(true\)') "cache persist loop must be wrapped in a top-level try"
    Assert-True ($src.Contains("[CACHE] Persist cycle failed: ")) "a bad snapshot/write must only cost one persist cycle, not the loop"
    Assert-True ($src -match '(?s)catch \(Exception ex2\)\s*\{.{0,500}?_cachePersistScheduled = false;') "fatal persist loop errors must release the scheduled flag for reschedule"
}

It "Unity async pass-through misses stay retryable while repeated rejected translations are bounded" {
    Assert-True ($src.Contains(") ? value : null);")) "batch callbacks must not echo the original on cache/API miss"
    Assert-True ($src.Contains("TransientTranslationRetryCooldownSeconds")) "temporary failures should use their own short retry cooldown"
    Assert-True ($src.Contains("RejectedTranslationRetryCooldownSeconds")) "quality-rejected translations should keep a separate bounded cooldown"
    Assert-True ((Get-NumericConstant $src "TransientTranslationRetryCooldownSeconds") -le 2) "transient pass-through misses must retry quickly"
    Assert-True ((Get-NumericConstant $src "RejectedTranslationRetryCooldownSeconds") -ge 5) "quality rejection retries must remain rate-limited"
    Assert-True ($src.Contains("MaxRejectedTranslationRetries")) "rejected translations need a finite retry budget"
    Assert-True ($src.Contains("MarkRejectedTranslationRetry(pendingBatchRequest.OriginalText, text2);")) "rejected batch responses should count toward the retry budget"
    Assert-True ($src -match '(?s)else if \(!HasRequiredProtectedValues\(text2, pendingBatchRequest\.Payload\)\).*?MarkRejectedTranslationRetry\(pendingBatchRequest\.OriginalText, text2\)') "batch translations that drop protected renderer tokens must consume the finite rejection budget"
    Assert-True ($src -match '(?s)else if \(!HasRequiredProtectedValues\(text4, requestItems\[num\]\.Payload\)\).*?MarkRejectedTranslationRetry\(item, text4\)') "warmup translations that drop protected tokens must consume the finite rejection budget"
    Assert-True ($src -match '(?s)private void RequestSharedTranslation\(.*?TryGetLocalTranslation\(text, out var translated\).*?IsTranslationRetryCoolingDown\(text\).*?IsServerBackoffActive') "all shared requests, including long-text segments, must honor cooldown and abandonment before network work"
    Assert-True ($src.Contains("_translationRetryAbandoned.Contains(key)")) "abandoned rejected translations must block future remote retries"
    Assert-True ($src.Contains("ClearTranslationRetryState(original);")) "successful accepted translations must clear retry rejection state"
    Assert-True ($src.Contains("MarkTranslationRetryCooldown(originalText);")) "async pass-through/null responses should cool down briefly before retry"
    Assert-True ($src.Contains("_translationRetryCooldowns.Remove(key);")) "retry cooldowns must be pruned after expiry"
    Assert-False ($src.Contains("_negativeCache")) "retryability must not use a permanent negative cache"
    Assert-False ($src.Contains("MarkKnownUntranslatable(pendingBatchRequest.OriginalText);")) "rejected batch responses must not permanently poison retryability"
    Assert-False ($src.Contains("MarkKnownUntranslatable(originalText);")) "async pass-through responses must not permanently poison retryability"
    Assert-True ($src.Contains("pass-through result left retryable")) "async original echoes should be documented as retryable"
}

It "XUnity waits asynchronously for queued local-cache results without accepting source echoes" {
    Assert-True ($il2cppEndpointSrc.Contains('\"cache_only\":true')) "XUnity requests must use cache-only mode so remote API latency stays off the game request path"
    Assert-True ($il2cppEndpointSrc.Contains("public override IEnumerator OnBeforeTranslate(IHttpTranslationContext context)")) "XUnity must keep queued translation jobs alive until the local cache can resolve them"
    Assert-True ($il2cppEndpointSrc.Contains("XUnityWebClient")) "queued-result polling must reuse XUnity's asynchronous HTTP client"
    Assert-True ($il2cppEndpointSrc.Contains("GetSupportedEnumerator")) "queued-result polling must yield the HTTP operation instead of blocking Unity's render thread"
    Assert-True ($il2cppEndpointSrc.Contains("Stopwatch.StartNew()")) "queued-result polling needs a wall-clock deadline independent of frame rate"
    Assert-True ($il2cppEndpointSrc.Contains("yield return null;")) "the cache polling interval must advance through Unity frames without Thread.Sleep"
    Assert-True ($il2cppEndpointSrc.Contains("QueueWaitSeconds")) "the queued-result wait budget must be configurable and bounded"
    Assert-True ($il2cppEndpointSrc.Contains("QueuePollIntervalSeconds")) "the local cache polling cadence must be configurable and bounded"
    Assert-False ($il2cppEndpointSrc.Contains("Thread.Sleep")) "the XUnity coroutine must never block the game thread while remote work is queued"
    Assert-True ($il2cppEndpointSrc.Contains("IsResolvedSource")) "XUnity endpoint must use an allow-list for successful server sources"
    Assert-True ($il2cppEndpointSrc.Contains('string.Equals(source, "cache"')) "cache responses must remain accepted"
    Assert-True ($il2cppEndpointSrc.Contains('string.Equals(source, "api"')) "resolved live responses from compatible servers may remain accepted"
    Assert-True ($il2cppEndpointSrc.Contains('string.Equals(source, "api_batch"')) "resolved batch responses from compatible servers may remain accepted"
    Assert-True ($il2cppEndpointSrc -match '(?s)private static bool HasOnlyResolvedOrPassSources.*?if \(sources == null \|\| sources.Length != expectedLength\) return false;') "missing or mismatched batch source metadata must fail closed"
    Assert-True ($il2cppEndpointSrc.Contains("IsPassSource")) "server pass sources must be recognized as an explicit identity terminal, not an endpoint error"
    Assert-True ($il2cppEndpointSrc.Contains("if (!IsResolvedSource(sources[i]) && !IsPassSource(sources[i])) return false;")) "unknown batch sources must still fail closed while pass remains an identity terminal"
    Assert-True ($il2cppEndpointSrc.Contains("string[] requestTexts = ProtectMixedCjkTextsForRequest(texts);")) "XUnity must derive a stable request representation before queueing progressively appended mixed-language text"
    Assert-True ($il2cppEndpointSrc.Contains("TryCollectPendingTexts(initialResponse.Data, requestTexts, out string[] pendingTexts)")) "mixed batches must derive the exact pending subset from the same protected request keys used by the server"
    Assert-True ($il2cppEndpointSrc -match '(?s)TryCollectPendingTexts\(.*?IsResolvedSource\(sources\[i\]\) \|\| IsPassSource\(sources\[i\]\).*?pending\.Add\(texts\[i\]\)') "pass and resolved items must be excluded from pending cache lookups"
    Assert-True ($il2cppEndpointSrc.Contains("TryKeepIdentityResultsSessionOnly")) "pass identities must disable XUnity global persistence before Complete"
    Assert-True ($il2cppEndpointSrc.Contains('"SaveResultGlobally"')) "identity-cache suppression must target XUnity's supported job flag"
    Assert-True ($il2cppEndpointSrc.Contains("string.Equals(one, original[0], StringComparison.Ordinal)")) "single original echoes must fail instead of entering XUnity's successful cache path"
    Assert-True ($il2cppEndpointSrc.Contains("string.Equals(results[i], original[i], StringComparison.Ordinal)")) "batch original echoes must fail instead of entering XUnity's successful cache path"
    Assert-False ($il2cppEndpointSrc -match 'IsUnresolvedSource|HasUnresolvedSource') "deny-list source checks must not return because pass and unknown states would be accepted"
}

It "Unity IL2CPP endpoint rejects whitespace-only server URLs" {
    Assert-True ($il2cppEndpointSrc -match '(?s)private static string TrimSlash\(string value\).*?string trimmed = value\?\.Trim\(\);.*?string\.IsNullOrEmpty\(trimmed\).*?127\.0\.0\.1:19999') "whitespace-only XUnity URLs must fall back to the local server"
}

It "IL2CPP TMP fallback revalidates mutable font tables without hot full scans" {
    Assert-True ((Get-NumericConstant $tmpFallbackSrc "SteadyNormalizeInterval") -ge 2.0) "setter-backed IL2CPP fallback should not enumerate every TMP text twice per second"
    Assert-True ($tmpFallbackSrc -match 'PatchLoadedFontAssets\(out bool fontTablesChanged\)') "font fallback repair must report table topology changes"
    Assert-False ($tmpFallbackSrc -match 'id == InstanceId\(_fallbackAsset\) \|\| PatchedFontAssets\.Contains\(id\)') "previously patched font IDs must still be checked after games rebuild their fallback tables"
    Assert-True ($tmpFallbackSrc -match 'AddToListProperty\([^\r\n]+out bool added\)') "font-list repair must distinguish existing entries from newly restored entries"
    Assert-True ($tmpFallbackSrc -match '(?s)if \(settingsChanged \|\| fontTablesChanged\).*?DirtiedTexts\.Clear\(\);') "restored fallback topology must force loaded TMP text meshes to rebuild"
}

It "IL2CPP interactive overlays do not freeze a cold auto-size measurement as their maximum" {
    $interactiveLayout = [regex]::Match($tmpFallbackSrc, '(?s)private static void ConfigureInteractiveOverlayLayout\(.*?(?=\s*private static void HideSourceTmpText)').Value
    Assert-True ($interactiveLayout.Contains('TryGetFloatProperty(source, "fontSizeMax", out float fontSizeMax)')) "interactive overlays must read the serialized auto-size upper bound"
    Assert-True ($interactiveLayout.Contains("preferredFontSize")) "interactive overlays need a settled preferred size independent of the current glyph measurement"
    Assert-True ($interactiveLayout -match 'Math\.Max\(fontSize,\s*fontSizeMax\)') "the cold current font size must not reduce a valid auto-size upper bound"
    Assert-False ($interactiveLayout -match 'TrySetPropertyIfExists\(overlay, "fontSizeMax", fontSize\)') "a transient tiny source font must never become the overlay's permanent maximum"
    Assert-True ($tmpFallbackSrc -match '1\.2\.20') "the IL2CPP renderer payload version must change with the compatibility behavior"
}

It "IL2CPP TMP fallback loads each physical bundle through one native boundary" {
    Assert-True ($tmpFallbackSrc.Contains("AttemptedFallbackBundlePaths")) "existing bundle paths must be attempted at most once per game process"
    Assert-True ($tmpFallbackSrc -match 'if \(!AttemptedFallbackBundlePaths\.Add\(path\)\)') "a null managed wrapper must not re-enter Unity's loader for the same physical bundle"
    Assert-True ($tmpFallbackSrc -match '(?s)private static int DetectUnityMajor\(\).*?Application\.unityVersion') "font selection must prefer Unity's reported version over unrelated version-like bytes"
    Assert-False ($tmpFallbackSrc -match '(?s)private static IEnumerable<string> SelectAssetCandidates\(\).*?FontByMajor\.Values') "a matching bundle failure must not fan out across incompatible Unity-major bundles"
    Assert-False ($tmpFallbackSrc -match 'LoadAssetBundleFromMemory\(') "a failed file load must not retry the same bundle bytes through memory"
    Assert-False ($tmpFallbackSrc -match 'LoadAssetBundleFromStream\(') "a failed file load must not retry the same bundle bytes through a stream"
    Assert-False ($tmpFallbackSrc.Contains("CopyFontAssetToTempPath")) "a failed file load must not retry an identical bundle from a temporary path"
    Assert-True ($tmpFallbackSrc -match 'Il2CppType\.From\(_tmpFontAssetType, false\)') "TMP AssetBundle extraction must resolve the generated IL2CPP type explicitly"
    Assert-True ($tmpFallbackSrc -match 'bundle\.LoadAllAssets\(tmpFontType\)') "TMP AssetBundle extraction must use the generated non-generic LoadAllAssets overload"
    Assert-True ($tmpFallbackSrc -match 'bundle\.LoadAsset\(name, tmpFontType\)') "named TMP assets must use the generated non-generic LoadAsset overload"
    Assert-True ($tmpFallbackSrc.Contains("AsTmpFontAsset")) "native TMP assets exposed through a base wrapper must be rewrapped before use"
    Assert-True ($tmpFallbackSrc -match '(?s)private static object AsTmpFontAsset\(.*?GetIl2CppObjectPointer\(asset\).*?new\[\] \{ typeof\(IntPtr\) \}.*?_tmpFontAssetType\.IsInstanceOfType\(wrapped\)') "TMP asset rewrapping must preserve the native pointer and validate the generated managed type"
    Assert-False ($tmpFallbackSrc -match 'foreach \(object asset in Enumerate\(bundle\.LoadAllAssets\(tmpFontType\)\)\)\s*\{\s*if \(asset != null\)') "a base UnityEngine.Object wrapper must never enter TMP lists without conversion"
    Assert-False ($tmpFallbackSrc.Contains("InvokeAssetBundleMethod")) "reflection must not select generic AssetBundle overloads that require unstripping"
    Assert-True ($tmpFallbackSrc -match '(?s)ShouldUseFastNormalizeScan\s*=>\s*_tmpTextType != null\s*&&\s*!_textSetterPatchInstalled\s*&&\s*!_textSetterPatchFailed\s*&&\s*!_textSetterPatchDeferredForFreshInterop\s*&&\s*_fallbackAsset != null') "a failed or fresh-interop-deferred TMP setter patch must drop to the steady scan interval instead of the 50ms full-scan hot path"
    Assert-True ($tmpFallbackSrc.Contains("TextSetterPatchRetryInterval = TimeSpan.FromSeconds(30)")) "failed TMP setter installation must retry on a bounded low-frequency lifecycle"
    Assert-True ($tmpFallbackSrc -match '(?s)public static void Apply\(\).*?if \(_fallbackAsset == null\).*?if \(_fallbackAsset == null\)\s*\{.*?return;.*?\}\s*_reportedFailure = false;.*?TryInstallTextSetterPatch\(\);') "IL2CPP TMP setter hooks must not be installed until a usable fallback asset exists"
    Assert-True ($tmpFallbackSrc.Contains("Font.Internal_CreateFontFromPath")) "stripped IL2CPP games need the generated path-based UGUI font fallback"
    Assert-True ($tmpFallbackSrc.Contains("Font.Internal_CreateFont")) "stripped IL2CPP games need the generated family-name UGUI font fallback"
    Assert-True ($tmpFallbackSrc.Contains("new Il2CppStringArray(1)")) "IL2CPP UGUI font creation must use the preserved font-name array overload"
    Assert-True ($tmpFallbackSrc.Contains("Font.Internal_CreateDynamicFont(font, fontNames, 90)")) "IL2CPP UGUI font creation must call the preserved internal dynamic-font icall directly"
    Assert-True ($tmpFallbackSrc -match '(?s)private static object EnsureUguiFont\(\).*?CreateUnityFontWithConstructor\(path\).*?CreateUnityFont\(faceName\)') "UGUI font fallback must prefer the generated Font(string) constructor before the internal dynamic-font fallback"
    Assert-False ($tmpFallbackSrc.Contains("Font.CreateDynamicFontFromOSFont(fontNames, 90)")) "the array factory still references a stripped constructor in some generated interop assemblies"
    Assert-True ($tmpFallbackSrc -match '(?s)private static object CreateSystemFallbackFontAsset\(\).*?CreateUnityFont\(fontName\).*?createTmpFont\.Invoke') "TMP runtime font creation must reuse the preserved internal Unity font factory"
    Assert-False ($tmpFallbackSrc.Contains("createFont.Invoke(null, new object[] { fontName, 90 })")) "TMP runtime font creation must not call the stripped single-name OS font factory"
    Assert-True ($tmpFallbackSrc -match '(?s)if \(_fallbackAsset == null\).*?_ = EnsureUguiFont\(\);.*?return;') "an incompatible TMP bundle must prepare the renderer-local UGUI fallback before returning"
    Assert-True ($tmpFallbackSrc.Contains("RefreshTmpTextsWithUguiOverlay")) "incompatible external TMP assets need a renderer-local UGUI display fallback"
    Assert-True ($tmpFallbackSrc -match '(?s)public static int NormalizeLoadedTextsFast\(\).*?if \(_fallbackAsset == null\)\s*\{\s*if \(_fallbackBundleRequest != null\).*?return 0;.*?return RefreshTmpTextsWithUguiOverlay\(\);') "the no-TMP-font path must wait for an in-flight real bundle before using the bounded UGUI overlay scan"
    Assert-True ($tmpFallbackSrc.Contains("new GameObject(TmpUguiOverlayName, new Il2CppSystem.Type[]")) "UGUI overlays must be created with RectTransform and Text in one native constructor call"
    Assert-True ($tmpFallbackSrc.Contains("GetComponent<RectTransform>()")) "IL2CPP TMP UI detection must query the native RectTransform component instead of relying on the Transform wrapper type"
    Assert-False ($tmpFallbackSrc.Contains("source.transform as RectTransform")) "IL2CPP Transform wrappers must not make Canvas TMP text look like world-space text"
    Assert-True ($tmpFallbackSrc.Contains('SetEnumPropertyIfExists(canvas, "renderMode", "ScreenSpaceOverlay")')) "world-space TMP fallback needs a renderer-local screen overlay canvas"
    Assert-True ($tmpFallbackSrc.Contains("ResolveActiveCamera")) "world-space TMP fallback must handle games whose camera is not tagged MainCamera"
    Assert-True ($tmpFallbackSrc.Contains("Camera.allCameras")) "camera fallback must use Unity's generated camera enumeration API before generic resource scans"
    Assert-True ($tmpFallbackSrc.Contains("camera.WorldToScreenPoint")) "world-space TMP bounds must be projected through the active Unity camera"
    Assert-True ($tmpFallbackSrc.Contains("renderer.bounds")) "world-space TMP overlay geometry must follow the live renderer bounds"
    Assert-True ($tmpFallbackSrc -match '(?s)private static bool TryUpdateTmpUguiOverlay\(.*?TrySetPropertyIfExists\(overlayText, "text", displayText\).*?HideTmpSourceForUguiOverlay') "source TMP must stay visible until the UGUI overlay accepts the translated text"
    Assert-True ($tmpFallbackSrc -match '(?s)private static void HideTmpSourceForUguiOverlay\(.*?TrySetPropertyIfExists\(sourceText, "enabled", false\)') "the accepted UGUI overlay must disable the original TMP renderer so stale missing-glyph geometry cannot remain visible"
    Assert-True ($tmpFallbackSrc -match '(?s)private static void DestroyTmpUguiOverlay\(.*?TrySetPropertyIfExists\(sourceText, "enabled", true\)') "removing a UGUI overlay must restore its original TMP renderer"
    Assert-True ($tmpFallbackSrc.Contains("DestroyTmpUguiOverlay(sourceId)")) "scene cleanup must release UGUI overlay objects and restore source state"
    Assert-False ($serverSrc.Contains("RefreshTmpTextsWithUguiOverlay")) "IL2CPP font display fallback must stay out of the shared server/cache layer"
    Assert-True ($tmpFallbackSrc.Contains("will not be retried in this process")) "a suppressed native retry must leave a diagnostic explaining the compatibility boundary"
}

It "IL2CPP TMP fallback retries assets that become available after startup" {
    Assert-True ($tmpFallbackSrc.Contains("FallbackLoadRetryInterval")) "TMP fallback retries must be rate-limited"
    Assert-True ($tmpFallbackSrc.Contains("_nextFallbackLoadAttemptUtc")) "TMP fallback must remember the next retry time instead of permanently giving up"
    Assert-True ($tmpFallbackSrc -match '(?s)if \(_fallbackAsset == null\).*?DateTime now = DateTime.UtcNow;.*?if \(now < _nextFallbackLoadAttemptUtc\).*?LoadFallbackFontAsset') "TMP fallback Apply must retry after the bounded delay"
    Assert-False ($tmpFallbackSrc -match '(?s)if \(_fallbackAsset == null\).*?if \(_reportedFailure\)\s*\{\s*return;') "the first missing asset must not permanently disable fallback loading"
}

It "IL2CPP TMP fallback patches fonts reached only through live text components" {
    Assert-True ($tmpFallbackSrc.Contains("PatchTmpHostFontFromText")) "TMP text refresh must patch a host font even when Unity resource enumeration omits that font asset"
    $patchLiveHostMethod = [regex]::Match($tmpFallbackSrc, '(?s)private static bool PatchTmpHostFontFromText\(.*?(?=\s*private static int RefreshLoadedTexts)').Value
    Assert-True ($patchLiveHostMethod -match '(?s)ContainsCjk\(value\).*?GetPropertyValue\(text, "font"\).*?fallbackFontAssetTable') "per-text host discovery must stay CJK-only and append the shared fallback"
    Assert-True ($patchLiveHostMethod -match '(?s)TryWarmFallbackGlyphs\(value, out bool glyphsAdded\).*?TrySetPropertyIfExists\(text, "font", _fallbackAsset\)') "after warming current glyphs, the CJK component must use the matching fallback material instead of its Latin host material"
    Assert-True ($patchLiveHostMethod -match '(?s)!ContainsCjk\(value\).*?RestoreTmpOriginalFont\(text\)') "a component reused for non-CJK text must restore the game's original TMP font"
    Assert-True ($tmpFallbackSrc.Contains("OriginalTmpFontByTextId")) "direct CJK font ownership must retain bounded restoration state"
    Assert-True ($tmpFallbackSrc -match '(?s)PrefixTmpTextString\(object __instance, ref string __0\).*?PatchTmpHostFontFromText\(__instance, normalized\)') "the TMP setter path must attach fallback before the translated string is rendered"
    Assert-True ($tmpFallbackSrc -match '(?s)RefreshLoadedTexts\(\).*?PatchTmpHostFontFromText\(text, current\)') "the slow lifecycle scan must repair texts written before setter patch installation"
    Assert-True ($tmpFallbackSrc -match '(?s)NormalizeLoadedTextsFast\(\).*?PatchTmpHostFontFromText\(text, current\)') "the compatibility scan must repair setters not covered by Harmony"
    Assert-False ($serverSrc.Contains("PatchTmpHostFontFromText")) "live TMP font discovery must remain isolated from shared translation memory"
}

It "IL2CPP dynamic TMP fallback expands glyphs for each translated sentence" {
    Assert-True ($tmpFallbackSrc.Contains("WarmedFallbackCharacters")) "the renderer must remember which CJK glyphs are already present in its dynamic atlas"
    Assert-True ($tmpFallbackSrc -match '(?s)private static bool TryWarmFallbackGlyphs\(string value, out bool glyphsAdded\).*?ContainsCjk\(value\).*?TryAddCharactersToTmpFontAsset\(_fallbackAsset, glyphs\).*?WarmedFallbackCharacters\.Add') "new CJK glyphs must be added explicitly before the sentence is rendered and recorded only after success"
    Assert-True ($tmpFallbackSrc -match '(?s)private static bool PatchTmpHostFontFromText\(object text, string value\).*?TryWarmFallbackGlyphs\(value, out bool glyphsAdded\).*?fallbackFontAssetTable.*?TrySetPropertyIfExists\(text, "font", _fallbackAsset\)') "both setter hooks and lifecycle scans must warm the sentence before assigning its matching CJK material"
    Assert-True ($tmpFallbackSrc -match '(?s)if \(added \|\| glyphsAdded\).*?DirtiedTexts\.Remove') "new atlas glyphs must force TMP to rebuild existing missing-glyph geometry"
    Assert-False ($serverSrc.Contains("WarmedFallbackCharacters")) "dynamic atlas state must stay inside the Unity renderer"
}

It "IL2CPP legacy UGUI fallback warms each translated sentence at its rendered size" {
    Assert-True ($tmpFallbackSrc.Contains("LastUguiTextById")) "UGUI glyph warmup must be keyed by live component text instead of repeated every scan"
    Assert-True ($tmpFallbackSrc -match '(?s)private static int PatchUguiTexts\(\).*?TryWarmUguiGlyphs\(font as Font, text, value\).*?LastUguiTextById\[id\] = value') "UGUI text must be warmed before its translated value is recorded as handled"
    Assert-True ($tmpFallbackSrc -match '(?s)private static bool TryWarmUguiGlyphs\(Font font, object text, string value\).*?GetPropertyValue\(text, "fontSize"\).*?GetPropertyValue\(text, "fontStyle"\).*?font\.RequestCharactersInTexture\(value, fontSize, fontStyle\)') "dynamic UGUI glyphs must be requested at the component's exact size and style"
    Assert-True ($tmpFallbackSrc -match '(?s)currentFont != null && InstanceId\(currentFont\) == fontId.*?if \(textChanged\).*?SetAllDirty') "a reused UGUI component must rebuild even when it already owns the fallback font"
    Assert-False ($serverSrc.Contains("TryWarmUguiGlyphs")) "UGUI atlas state must remain isolated from shared translation memory"
}

It "IL2CPP Unity 6000 ReadOnlySpan failures use the pre-native async bundle boundary" {
    Assert-True ($tmpFallbackSrc.Contains("_assetBundleInteropIncompatible")) "the process must remember a generated-interoperability failure at the AssetBundle boundary"
    Assert-True ($tmpFallbackSrc -match '(?s)private static AssetBundle LoadAssetBundleFromFile\(string path\).*?MissingMethodException.*?GetPinnableReference.*?_assetBundleInteropIncompatible = true') "the incompatibility flag must be limited to the observed ReadOnlySpan missing-method signature"
    Assert-True ($tmpFallbackSrc.Contains("AssetBundleCreateRequest _fallbackBundleRequest")) "the renderer lifecycle must own the asynchronous bundle request"
    Assert-True ($tmpFallbackSrc -match '(?s)private static bool TryBeginFallbackBundleAsync\(string path, string assetName\).*?_assetBundleInteropIncompatible.*?AssetBundle\.LoadFromFileAsync\(path\)') "the async retry must be limited to the exact pre-native ReadOnlySpan failure"
    Assert-True ($tmpFallbackSrc -match '(?s)private static object TryCompleteFallbackBundleAsync\(\).*?_fallbackBundleRequest\.isDone.*?_fallbackBundleRequest\.assetBundle.*?FindTmpFontAssetInBundle') "the Unity main-thread lifecycle must poll and extract the real TMP asset without blocking"
    Assert-True ($tmpFallbackSrc -match '(?s)public static void Apply\(\).*?_fallbackBundleRequest != null.*?_reportedFailure') "an in-flight bundle request must not be misreported as a permanent font failure"
    Assert-True ($tmpFallbackSrc -match '(?s)private static object LoadFallbackFontAsset\(\).*?TryCompleteFallbackBundleAsync\(\).*?CreateSystemFallbackFontAsset\(\).*?TryBeginFallbackBundleAsync\(') "the working dynamic path-font boundary must run before the async packaged-font terminal fallback"
    Assert-True ($tmpFallbackSrc -match '(?s)private static object CreateSystemFallbackFontAsset\(\).*?CreateSystemFontFileAsset\(\).*?CreateUnityFont\(fontName\).*?createTmpFont\.Invoke') "ReadOnlySpan failure in AssetBundle loading must not disable TMP's separate working path-font factory"
    Assert-True ($tmpFallbackSrc -match '(?s)NormalizeLoadedTextsFast\(\).*?_fallbackAsset == null.*?RefreshTmpTextsWithUguiOverlay') "if the family-name chain also fails, the rejected TMP object must still flow into the bounded overlay renderer"
    Assert-False ($serverSrc.Contains("_assetBundleInteropIncompatible")) "Unity interop compatibility must not leak into shared translation memory"
}

It "IL2CPP TMP setter Harmony patch defers after current-process interop generation" {
    Assert-True ($tmpFallbackSrc.Contains("_textSetterPatchDeferredForFreshInterop")) "the renderer must remember that the current process generated its IL2CPP interop"
    Assert-True ($tmpFallbackSrc -match '(?s)private static void TryInstallTextSetterPatch\(\).*?ShouldDeferTextSetterPatchForFreshInterop\(\).*?new Harmony\(') "fresh-interop deferral must run before Harmony creates or patches a TMP setter"
    Assert-True ($tmpFallbackSrc -match '(?s)ShouldDeferTextSetterPatchForFreshInterop\(\).*?Paths\.BepInExRootPath.*?interop.*?assembly-hash\.txt.*?GetLastWriteTimeUtc.*?Process\.GetCurrentProcess\(\).*?StartTime') "fresh interop detection must compare BepInEx's generated marker with the current process start"
    Assert-True ($tmpFallbackSrc -match 'markerWriteUtc\s*>=\s*processStartUtc') "only interop generated by the current process may defer the setter patch"
    Assert-True ($tmpFallbackSrc -match '(?s)ShouldUseFastNormalizeScan\s*=>.*?!_textSetterPatchDeferredForFreshInterop') "fresh-interop fallback must use the low-frequency renderer scan instead of a 50ms full scan"
    Assert-True ($tmpFallbackSrc.Contains("deferred for this process")) "the compatibility fallback must leave a bounded diagnostic"
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
