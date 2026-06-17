# Regression tests for launcher engine detection and deploy routing.
$ErrorActionPreference = "Stop"

$root = Split-Path -Parent $PSScriptRoot
$bin = Join-Path $root "native\toolchain\w64devkit\bin"
$probeExe = Join-Path $PSScriptRoot "launcher_probe.exe"
$fixtures = Join-Path $PSScriptRoot "_launcher_fixtures"

if (-not (Test-Path (Join-Path $bin "gcc.exe"))) {
    throw "Missing w64devkit gcc at $bin"
}

$env:PATH = "$bin;$env:PATH"

$mainSrc = Get-Content -LiteralPath (Join-Path $root "native\src\launcher\main.c") -Raw
if ($mainSrc -match '(?s)case\s+WM_DESTROY:.*stop_server\s*\(') {
    throw "launcher must not stop the translation server when the window closes"
}
if ($mainSrc -notmatch 'sync_embedded_payloads\(\)') {
    throw "launcher must sync embedded first-party components on startup so exe-only updates work"
}
if ($mainSrc -notmatch '--sync-payloads-and-exit') {
    throw "launcher must keep a headless embedded payload sync mode for release verification"
}
$uiSrc = Get-Content -LiteralPath (Join-Path $root "native\src\launcher\ui.c") -Raw
if ($uiSrc -notmatch 'OutputDebugStringW\(line\)' -or $uiSrc -notmatch '!g_log \|\| !IsWindow\(g_log\)') {
    throw "launcher logging must be safe before UI controls exist"
}
$serverProcSrc = Get-Content -LiteralPath (Join-Path $root "native\src\launcher\server_proc.c") -Raw
if ($serverProcSrc -notmatch 'server_http_alive') {
    throw "launcher must detect an already-running translation server"
}
if ($serverProcSrc -notmatch 'if\s*\(\s*server_http_alive\(200\)\s*\)') {
    throw "start_server must adopt an already-running translation server"
}
$engineSrc = Get-Content -LiteralPath (Join-Path $root "native\src\launcher\engine.c") -Raw
if ($engineSrc -notmatch 'has_file_pattern\(p,\s*L"\*\.rpy"\)') {
    throw "Ren'Py detection must recognize source-only .rpy games, not only compiled .rpyc/.rpa packages"
}
$warmupSrc = Get-Content -LiteralPath (Join-Path $root "native\src\launcher\warmup.c") -Raw
if ($warmupSrc -notmatch 'scan_renpy_script_dir') {
    throw "Ren'Py warmup must scan script files for prefetch candidates"
}
if ($warmupSrc -notmatch 'ENGINE_RENPY\)\s+warmup_renpy') {
    throw "Ren'Py warmup must be wired into warmup_translations"
}
if ($warmupSrc -notmatch 'wide_ends_with_i\(fd\.cFileName,\s*L"\.rpy"\)') {
    throw "Ren'Py warmup must target .rpy scripts conservatively"
}
if ($warmupSrc -notmatch 'RENPY_WARMUP_MAX_ITEMS 30000') {
    throw "Ren'Py warmup must preheat the whole script, not the first 1200 lines"
}
if ($warmupSrc -notmatch 'UNITY_WARMUP_MAX_ITEMS 8000') {
    throw "Unity warmup must have its own larger cap so VN text can be prefetched before display"
}
if ($warmupSrc -notmatch 'prefetch\.max_items = RENPY_WARMUP_MAX_ITEMS;') {
    throw "Ren'Py warmup list must raise its item cap"
}
if ($warmupSrc -notmatch 'prefetch\.max_items = UNITY_WARMUP_MAX_ITEMS;') {
    throw "Unity warmup list must raise its item cap without changing RPGM defaults"
}
if ($warmupSrc -notmatch 'static int local_http_post\(LocalHttp \*h') {
    throw "warmup must reuse one localhost connection across batches"
}
if ($warmupSrc -match 'localhost_post_timeout') {
    throw "launcher warmup must not keep the old unused single-request POST path"
}
if ($warmupSrc -notmatch 'local_http_wait_ready') {
    throw "warmup must wait for the local server health endpoint before posting batches"
}
if ($warmupSrc -notmatch 'WinHttpQueryHeaders\(req,\s*WINHTTP_QUERY_STATUS_CODE \| WINHTTP_QUERY_FLAG_NUMBER') {
    throw "warmup readiness checks must verify HTTP 200 rather than only opening a handle"
}
if ($warmupSrc -notmatch 'local_http_wait_ready\(&http,\s*8000\)') {
    throw "warmup must tolerate slow cache-load startup before dropping prefetch batches"
}
if ($warmupSrc -notmatch 'static size_t post_prefetch_all\(TextList \*prefetch\)') {
    throw "warmup prefetch helper must report the number of successfully queued texts"
}
if ($warmupSrc -notmatch 'size_t queued = post_prefetch_all\(&prefetch\);') {
    throw "Ren'Py/RPGM warmup logs must use successfully queued counts"
}
if ($uiSrc -notmatch '(?s)ENGINE_RENPY\)\s*\{.{0,400}?launch_game\(a->dir\);.{0,400}?warmup_translations\(a->dir, a->engine\);') {
    throw "Ren'Py games must launch before the whole-script prefetch, not after"
}
$cacheSrc = Get-Content -LiteralPath (Join-Path $root "native\src\server\cache.c") -Raw
if ($cacheSrc -notmatch '(?s)ReleaseSRWLockExclusive\(&c->lock\);\s*if \(!is_new\) return;') {
    throw "cache_set_persist must release the map lock before disk IO"
}
if ($cacheSrc -notmatch 'io_lock') {
    throw "TSV appends must serialize on a dedicated io lock, not the reader lock"
}
$deploySrc = Get-Content -LiteralPath (Join-Path $root "native\src\launcher\deploy.c") -Raw
if (-not (Test-Path (Join-Path $root "scripts\install_runtime_payloads.ps1"))) {
    throw "runtime payload installer script must ship with source and program packages"
}
$runtimeInstallerSrc = Get-Content -LiteralPath (Join-Path $root "scripts\install_runtime_payloads.ps1") -Raw
if ($runtimeInstallerSrc -notmatch 'Newtonsoft\.Json\.13\.0\.4\.zip' -or $runtimeInstallerSrc -notmatch 'Copy-Item -LiteralPath \$pkg -Destination \$zipPkg') {
    throw "runtime payload installer must rename NuGet .nupkg to .zip before Expand-Archive for Windows PowerShell 5.1"
}
if ($deploySrc -notmatch 'install_runtime_payloads\.ps1' -or $deploySrc -notmatch '-UnityMono5' -or $deploySrc -notmatch '-UnityMono6' -or $deploySrc -notmatch '-UnityIL2CPP') {
    throw "Unity deploy must tell users the exact runtime payload install command when payloads are missing"
}
if ($deploySrc -notmatch 'DeepSeekXUnityTranslator\\\\DeepSeekTranslate\.dll' -or $deploySrc -notmatch 'Translators\\\\DeepSeekTranslate\.dll') {
    throw "Unity IL2CPP deploy must copy the first-party DeepSeek XUnity endpoint outside the third-party XUnity payload"
}
if ($deploySrc -notmatch 'ds_font\.ttc') {
    throw "Ren'Py deploy must ship a CJK font next to the hook"
}
if ($deploySrc -notmatch 'say_dialogue') {
    throw "Ren'Py hook must override dialogue styles with the shipped CJK font"
}
if ($deploySrc -notmatch '0@ds_font\.ttc') {
    throw "Ren'Py hook must use the TTC face index syntax for collection fonts"
}
if ($deploySrc -notmatch 'replace_text = _ds_chain_replace') {
    throw "Ren'Py hook must translate screen/UI text via config.replace_text"
}
if ($deploySrc -notmatch 'down_until') {
    throw "Ren'Py hook must back off when the local server is unreachable"
}
if ($deploySrc -notmatch '_ds_memo') {
    throw "Ren'Py UI translation must memoize lookups to avoid per-frame HTTP"
}
if ($deploySrc -notmatch 'font_replacement_map') {
    throw "Ren'Py hook must map game fonts to the CJK font at the loader level"
}
if ($deploySrc -notmatch 'renpy\.restart_interaction\(\)') {
    throw "Ren'Py hook must refresh the screen when queued translations arrive"
}
if ($deploySrc -notmatch '_ds_t\.daemon = True') {
    throw "Ren'Py heal poller must be a daemon thread so games can exit"
}
if ($deploySrc -notmatch 'len\(_ds_pending\) >= 300') {
    throw "Ren'Py heal poller pending set must be bounded"
}
if ($deploySrc -notmatch "if _ds_pending\[k\] > 60") {
    throw "Ren'Py heal poller must abandon texts the server never translates"
}
if ($deploySrc -notmatch 'OverrideFont=Microsoft YaHei') {
    throw "Unity XUnityAutoTranslator config must default UGUI text to a CJK-capable font"
}
if ($deploySrc -notmatch 'MaxConcurrency=8') {
    throw "Unity IL2CPP XUnity config must use high local concurrency for cache-hit responsiveness"
}
if ($deploySrc -notmatch 'TranslationDelay=0\.1') {
    throw "Unity IL2CPP XUnity config must keep XUnity's minimum accepted translation delay"
}
if ($deploySrc -notmatch 'DisplaySafePunctuation=True') {
    throw "Unity IL2CPP XUnity config must enable renderer-local punctuation safety"
}
if ($deploySrc -notmatch 'deploy_rpgm_font') {
    throw "RPG Maker deploy must ship a renderer-local CJK font"
}
if ($deploySrc -notmatch 'DeepSeekCJK') {
    throw "RPG Maker hook must install a named CJK font face for canvas text"
}
if ($deploySrc -notmatch 'standardFontFace') {
    throw "RPG Maker MV hook must override Window_Base.standardFontFace for translated text"
}
if ($deploySrc -notmatch 'mainFontFace') {
    throw "RPG Maker MZ hook must override Game_System.mainFontFace for translated text"
}
if ($deploySrc -notmatch 'www\\\\fonts' -or $deploySrc -notmatch 'ds_font\.ttf') {
    throw "RPG Maker CJK font must be deployed under www\\fonts"
}
$buildSrc = Get-Content -LiteralPath (Join-Path $root "build_native.bat") -Raw
$sourceReleaseSrc = Get-Content -LiteralPath (Join-Path $root "scripts\prepare_open_source_release.ps1") -Raw
if ($sourceReleaseSrc -notmatch '\\_launcher_fixtures\\') {
    throw "source release script must exclude launcher test fixtures from public archives"
}
if ($buildSrc -notmatch 'BepInExFlavor=5' -or $buildSrc -notmatch 'BepInExFlavor=6') {
    throw "build_native.bat must build both BepInEx 5 and BepInEx 6 UnityTranslator flavors"
}
if ($buildSrc -notmatch 'windres' -or $buildSrc -notmatch 'launcher_payloads\.rc' -or $buildSrc -notmatch 'launcher_payloads\.o') {
    throw "build_native.bat must embed first-party payload resources into the launcher"
}
if ($buildSrc -notmatch 'native/dst_server\.exe' -or $buildSrc -notmatch 'scripts/install_runtime_payloads\.ps1' -or $buildSrc -notmatch 'payloads/UnityTranslator/UnityTranslator\.dll') {
    throw "launcher resources must include the server, runtime installer script, and first-party Unity plugin"
}
if ($buildSrc -notmatch 'self_update\.c') {
    throw "build_native.bat must link the embedded payload self-update module"
}
if ($buildSrc -notmatch 'UnityTranslator\.BepInEx6\.dll') {
    throw "build_native.bat must refresh the BepInEx6 UnityTranslator payload"
}
if ($buildSrc -notmatch 'DeepSeekTMPFontFallback\.csproj') {
    throw "build_native.bat must wire the IL2CPP TMP font fallback source build when interop refs are available"
}
if ($buildSrc -notmatch 'IL2CPP_INTEROP_DIR') {
    throw "DeepSeekTMPFontFallback build must document the required IL2CPP interop reference directory"
}
if ($buildSrc -notmatch 'UnityEngine\.TextRenderingModule\.dll') {
    throw "DeepSeekTMPFontFallback build must validate the UnityEngine.TextRenderingModule interop reference"
}
$tmpFallbackProj = Get-Content -LiteralPath (Join-Path $root "payloads\UnityIL2CPP\DeepSeekTMPFontFallback\src\DeepSeekTMPFontFallback.csproj") -Raw
if ($tmpFallbackProj -notmatch '0Harmony') {
    throw "DeepSeekTMPFontFallback project must reference Harmony for first-frame TMP text patching"
}
$xunityEndpointSrc = Get-Content -LiteralPath (Join-Path $root "payloads\UnityIL2CPP\DeepSeekXUnityTranslator\src\DeepSeekTranslateEndpoint.cs") -Raw
if ($xunityEndpointSrc -notmatch 'private int _maxConcurrency = 8') {
    throw "DeepSeek XUnity endpoint should default to 8 local requests for faster cache-hit fanout"
}
if ($xunityEndpointSrc -notmatch 'TranslationDelay\", 0\.1f') {
    throw "DeepSeek XUnity endpoint should default to XUnity's minimum accepted delay"
}
if ($xunityEndpointSrc -notmatch 'if \(value < 0\.1f\) return 0\.1f;') {
    throw "DeepSeek XUnity endpoint must clamp configured delay to XUnity's accepted minimum"
}
if ($xunityEndpointSrc -notmatch 'PrepareDisplayTranslation\(one\)' -or $xunityEndpointSrc -notmatch 'PrepareDisplayTranslations\(results\)') {
    throw "DeepSeek XUnity endpoint must sanitize IL2CPP TMP punctuation before XUnity writes text"
}
if ($xunityEndpointSrc -notmatch '\\u3002' -or $xunityEndpointSrc -notmatch '\\uff0c') {
    throw "DeepSeek XUnity endpoint must protect Chinese period/comma from TMP tofu before display"
}
$tmpFallbackSrc = Get-Content -LiteralPath (Join-Path $root "payloads\UnityIL2CPP\DeepSeekTMPFontFallback\src\TmpFontFallbackPlugin.cs") -Raw
if ($tmpFallbackSrc -match 'ManagedSpanWrapper' -or $tmpFallbackSrc -match 'UnityEngine\.Bindings') {
    throw "IL2CPP TMP font fallback must not depend on Unity-version-specific ManagedSpanWrapper internals"
}
if ($tmpFallbackSrc -notmatch '1\.2\.8') {
    throw "IL2CPP TMP font fallback version must be bumped when changing runtime behavior"
}
if ($tmpFallbackSrc -notmatch 'HarmonyLib' -or $tmpFallbackSrc -notmatch 'TryInstallTextSetterPatch' -or $tmpFallbackSrc -notmatch 'PrefixTmpTextString\(object __instance, ref string __0\)') {
    throw "IL2CPP TMP font fallback must intercept TMP text writes before the first rendered frame"
}
if ($tmpFallbackSrc -notmatch 'TryProtectInteractiveTmpTranslation' -or $tmpFallbackSrc -notmatch 'DeepSeekTranslationOverlay') {
    throw "IL2CPP TMP font fallback must protect interactive choice/button text with a visual overlay"
}
if ($tmpFallbackSrc -notmatch 'IsLogicSensitiveUiPath' -or $tmpFallbackSrc -notmatch 'notification' -or $tmpFallbackSrc -notmatch 'tutorial') {
    throw "IL2CPP TMP font fallback must protect notification/tutorial UI text without changing game logic text"
}
if ($tmpFallbackSrc -notmatch 'ConfigureInteractiveOverlayLayout' -or $tmpFallbackSrc -notmatch 'enableWordWrapping\", false' -or $tmpFallbackSrc -notmatch 'fontSizeMin') {
    throw "IL2CPP choice/button overlay must use compact no-wrap autosizing to avoid broken option line wrapping"
}
if ($tmpFallbackSrc -notmatch 'HasInteractiveComponentInParents' -or $tmpFallbackSrc -notmatch '__0 = preservedOriginal;') {
    throw "interactive TMP protection must preserve original button text for game logic"
}
if ($tmpFallbackSrc -notmatch 'InteractiveOriginalTextBySourceId' -or $tmpFallbackSrc -notmatch 'DestroyInteractiveOverlay') {
    throw "interactive TMP overlay protection must keep repeat refreshes stable and destroy stale overlay objects"
}
if ($tmpFallbackSrc -notmatch 'UnityEngine\.Object\.Destroy\(overlayComponent\.gameObject\)') {
    throw "interactive TMP overlays must not accumulate hidden GameObjects during long play sessions"
}
if ($tmpFallbackSrc -notmatch 'FastNormalizeInterval = 0\.05f' -or $tmpFallbackSrc -notmatch 'NormalizeLoadedTextsFast') {
    throw "IL2CPP TMP font fallback must keep a fast fallback path for text that bypasses the setter patch"
}
if ($tmpFallbackSrc -notmatch 'SteadyNormalizeInterval = 0\.5f' -or $tmpFallbackSrc -notmatch 'ShouldUseFastNormalizeScan') {
    throw "IL2CPP TMP font fallback must slow the fallback scan once the setter patch is installed"
}
if ($tmpFallbackSrc -notmatch 'MakeGenericMethod\(assetType\)') {
    throw "IL2CPP TMP font fallback must support generic AssetBundle LoadAsset/LoadAllAssets overloads"
}
if ($tmpFallbackSrc -notmatch 'NormalizeTmpTextForFallback' -or $tmpFallbackSrc -notmatch '\\uff0c' -or $tmpFallbackSrc -notmatch '\\u3002') {
    throw "IL2CPP TMP font fallback must normalize full-width comma before TMP rendering"
}
if ($tmpFallbackSrc -notmatch '\\uff1f' -or $tmpFallbackSrc -notmatch '\\u2026') {
    throw "IL2CPP TMP font fallback must normalize common unsupported punctuation before TMP rendering"
}
if (-not (Test-Path (Join-Path $root "payloads\UnityIL2CPP\DeepSeekTMPFontFallback\BepInEx\plugins\DeepSeekTMPFontFallback\DeepSeekTMPFontFallback.dll"))) {
    throw "IL2CPP TMP font fallback payload DLL must exist when source build is skipped"
}
$selfUpdateSrc = Get-Content -LiteralPath (Join-Path $root "native\src\launcher\self_update.c") -Raw
if ($selfUpdateSrc -notmatch 'FindResourceW' -or $selfUpdateSrc -notmatch 'MoveFileExW' -or $selfUpdateSrc -notmatch 'file_matches_bytes') {
    throw "embedded payload sync must read Win32 resources, compare bytes, and replace atomically"
}
if ($selfUpdateSrc -match 'Newtonsoft\.Json' -or $selfUpdateSrc -match 'BepInExRuntime' -or $selfUpdateSrc -match 'XUnityAutoTranslator') {
    throw "embedded payload sync must not bundle third-party runtime payloads"
}
$programReleaseSrc = Get-Content -LiteralPath (Join-Path $root "scripts\prepare_program_release.ps1") -Raw
if ($programReleaseSrc -notmatch 'singleExePath' -or $programReleaseSrc -notmatch 'Copy-ReleaseFile "DeepSeekTranslator\.exe" "\$DsName\.exe"') {
    throw "program release script must produce a standalone ds translator exe asset"
}
if ($programReleaseSrc -notmatch '\(\^\|/\)native/' -or $programReleaseSrc -notmatch '\(\^\|/\)payloads/' -or $programReleaseSrc -notmatch 'Newtonsoft\\\.Json') {
    throw "program release script must reject sidecar runtime payloads and third-party binaries"
}
$unityMonoSrc = Get-Content -LiteralPath (Join-Path $root "payloads\UnityTranslator\src\DeepSeekTranslator.cs") -Raw
if ($unityMonoSrc -notmatch 'NormalizeTmpPunctuationForMissingGlyphs') {
    throw "Unity Mono TMP display fallback must keep renderer-local punctuation normalization"
}
$serverApiSrc = Get-Content -LiteralPath (Join-Path $root "native\src\server\api.c") -Raw
if ($serverApiSrc -match 'NormalizeTmpTextForFallback' -or $serverApiSrc -match 'NormalizeTmpPunctuationForMissingGlyphs' -or $serverApiSrc -match '\\uff0c') {
    throw "renderer-specific glyph punctuation fallback must not be moved into the shared server/cache layer"
}
if ($deploySrc -notmatch 'EnableUIResizing=False') {
    throw "Unity IL2CPP XUnity deploy must not resize UI layouts because it can break click targets/input flow"
}
if ($deploySrc -notmatch 'IgnoreTextStartingWith=.*Confidence increased' -or $deploySrc -notmatch 'Confidence decreased') {
    throw "Unity IL2CPP XUnity deploy must ignore stat notification text that can control tutorial/input flow"
}

if (Test-Path $fixtures) {
    Remove-Item -LiteralPath $fixtures -Recurse -Force
}
New-Item -ItemType Directory -Force -Path $fixtures | Out-Null

function New-File($path, [string]$content = "") {
    $parent = Split-Path -Parent $path
    New-Item -ItemType Directory -Force -Path $parent | Out-Null
    Set-Content -LiteralPath $path -Value $content -NoNewline -Encoding UTF8
}

try {
    New-File (Join-Path $fixtures "renpy\game\script.rpy") "label start:`n    `"Hello`""
    New-File (Join-Path $fixtures "rpgm\www\index.html") "<html><body></body></html>"
    New-Item -ItemType Directory -Force -Path (Join-Path $fixtures "rpgm\www\js") | Out-Null

    New-Item -ItemType Directory -Force -Path (Join-Path $fixtures "unity_mono\Example_Data") | Out-Null
    New-Item -ItemType Directory -Force -Path (Join-Path $fixtures "unity_mono\BepInEx\plugins") | Out-Null

    New-Item -ItemType Directory -Force -Path (Join-Path $fixtures "unity6_mono\Example_Data") | Out-Null
    New-File (Join-Path $fixtures "unity6_mono\Example_Data\globalgamemanagers") "6000.3.8f1"

    New-Item -ItemType Directory -Force -Path (Join-Path $fixtures "unity_il2cpp\Example_Data\il2cpp_data") | Out-Null
    New-Item -ItemType Directory -Force -Path (Join-Path $fixtures "unity_il2cpp\BepInEx\plugins") | Out-Null
    New-Item -ItemType Directory -Force -Path (Join-Path $fixtures "unity_il2cpp\BepInEx\core") | Out-Null
    Copy-Item -LiteralPath (Join-Path $root "payloads\UnityTranslator\UnityTranslator.dll") -Destination (Join-Path $fixtures "unity_il2cpp\BepInEx\plugins\UnityTranslator.dll")
    New-File (Join-Path $fixtures "unity_il2cpp\GameAssembly.dll") "il2cpp"
    New-File (Join-Path $fixtures "unity_il2cpp\doorstop_config.ini") "[UnityDoorstop]`nenabled=true"

    New-Item -ItemType Directory -Force -Path (Join-Path $fixtures "unity_custom\Example_Data\il2cpp_data") | Out-Null
    New-Item -ItemType Directory -Force -Path (Join-Path $fixtures "unity_custom\BepInEx\plugins") | Out-Null
    New-File (Join-Path $fixtures "unity_custom\BepInEx\plugins\UnityTranslator.dll") "custom-il2cpp-plugin"
    New-File (Join-Path $fixtures "unity_custom\GameAssembly.dll") "il2cpp"

    gcc -std=c17 -O2 -municode -D_CRT_SECURE_NO_WARNINGS `
        -I"$root\native\src\launcher" `
        "$PSScriptRoot\launcher_probe.c" `
        "$root\native\src\launcher\engine.c" `
        "$root\native\src\launcher\fsutil.c" `
        "$root\native\src\launcher\deploy.c" `
        -lgdi32 -lmsimg32 -o "$probeExe"
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

    & $probeExe $root $fixtures
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
} finally {
    Remove-Item -LiteralPath $probeExe -Force -ErrorAction SilentlyContinue
    Remove-Item -LiteralPath $fixtures -Recurse -Force -ErrorAction SilentlyContinue
}
