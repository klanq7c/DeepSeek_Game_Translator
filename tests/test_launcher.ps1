# Regression tests for launcher engine detection and deploy routing.
param(
    [string]$ProbeGamePath = "",
    [string]$DeployRpgmPath = ""
)

$ErrorActionPreference = "Stop"

$root = Split-Path -Parent $PSScriptRoot
$bin = Join-Path $root "native\toolchain\w64devkit\bin"
$probeExe = Join-Path $PSScriptRoot "launcher_probe.exe"
$restoreProbeExe = Join-Path $PSScriptRoot "restore_probe.exe"
$godotCliProbeExe = Join-Path $PSScriptRoot "godot_cli_probe.exe"
$warmupProbeExe = Join-Path $PSScriptRoot "warmup_probe.exe"
$godotPatchProbeExe = Join-Path $PSScriptRoot "godot_patch_probe.exe"
$rpgmHookProbe = Join-Path $PSScriptRoot "rpgm_hook_probe.js"
$renpyHookProbe = Join-Path $PSScriptRoot "renpy_hook_probe.py"
$fixtures = Join-Path $PSScriptRoot "_launcher_fixtures"
$reparseOutside = Join-Path $PSScriptRoot "_launcher_reparse_outside"
$reparseLink = Join-Path $fixtures "reparse_link"
$restoreReparseRoot = Join-Path $fixtures "restore_reparse"
$restoreReparseLink = Join-Path $restoreReparseRoot "game"
$productName = "ds" + [string][char]0x6e38 + [string][char]0x620f + [string][char]0x7ffb + [string][char]0x8bd1 + [string][char]0x5668
$productExeName = "$productName.exe"
$productVersion = (Get-Content -LiteralPath (Join-Path $root "VERSION") -Raw -Encoding UTF8).Trim()
if ($productVersion -notmatch '^\d+\.\d+\.\d+\.\d+$') {
    throw "VERSION must contain a four-part product version"
}

$contextDoc = Get-Content -LiteralPath (Join-Path $root "CONTEXT.md") -Raw -Encoding UTF8
$maintenanceDoc = Get-Content -LiteralPath (Join-Path $root "docs\MAINTENANCE_MAP.md") -Raw -Encoding UTF8
$runAllSrc = Get-Content -LiteralPath (Join-Path $root "tests\run_all.ps1") -Raw -Encoding UTF8
if ($runAllSrc -match 'Get-Process\s+-Name\s+dst_server[^\r\n]*Stop-Process\s+-Force') {
    throw "the test runner must not force-stop a user-owned translation service by process name"
}
foreach ($needle in @(
    "native/src/launcher/engine.c",
    "native/src/launcher/deploy.c",
    "native/src/launcher/warmup.c",
    "native/src/launcher/warmup_internal.h",
    "native/src/launcher/godot_warmup.c",
    "native/src/launcher/godot_patch.c",
    "tests/godot_patch_probe.c",
    "tests/warmup_probe.c",
    "tests/launcher_probe.c",
    "tests/restore_probe.c",
    "tests/godot_cli_probe.c",
    "payloads/UnityTranslator/src",
    "payloads/UnityIL2CPP",
    "scripts/prepare_open_source_release.ps1",
    "scripts/prepare_program_release.ps1",
    "translation_memory_c.tsv",
    "ENGINE_GODOT",
    "deploy_godot",
    "warmup_godot",
    "warmup_scan_godot_resources",
    "godot_prepare_patch_pack"
)) {
    if (-not $maintenanceDoc.Contains($needle)) {
        throw "maintenance map must keep a fast lookup entry for $needle"
    }
}
foreach ($needle in @("Ren'Py", "RPG Maker", "Unity Mono", "Unity IL2CPP", "Godot", "Renderer compatibility layer", "Local server")) {
    if (-not $contextDoc.Contains($needle)) {
        throw "CONTEXT.md must define the shared project term: $needle"
    }
}

$godotPatchSrc = Get-Content -LiteralPath (Join-Path $root "native\src\launcher\godot_patch.c") -Raw -Encoding UTF8
if ($godotPatchSrc -notmatch 'godot_smaz_decompress' -or
    $godotPatchSrc -notmatch 'dup_optimized_translation_string' -or
    $godotPatchSrc -notmatch 'comp_size == uncomp_size') {
    throw "Godot OptimizedTranslation patching must handle compressed and uncompressed string payloads"
}
if ($godotPatchSrc -notmatch 'compare_pck_entries_for_patch' -or
    $godotPatchSrc -notmatch 'localization_dialogic/' -or
    $godotPatchSrc -notmatch 'negotation') {
    throw "Godot patching must prioritize scene/dialog resources before large repeatable side-content packs"
}
if ($godotPatchSrc -notmatch 'GODOT_PATCH_SMALL_DIALOGIC_BYTES' -or
    $godotPatchSrc -notmatch 'small_dialogic' -or
    $godotPatchSrc -notmatch 'GODOT_PATCH_LIVE_LIMIT 1024u') {
    throw "Godot patching must keep small Dialogic scene/combat resources inside the live translation window"
}
if (-not $godotPatchSrc.Contains("godot_wrap_cjk_translation") -or
    -not $godotPatchSrc.Contains("Do not insert synthetic hard") -or
    $godotPatchSrc.Contains("GODOT_PATCH_CJK_WRAP_COLS") -or
    $godotPatchSrc.Contains("line_cols + cols") -or
    $godotPatchSrc.Contains('bb_add(&out, "\n", 1)') -or
    -not $godotPatchSrc.Contains("wrapped ? wrapped : translated")) {
    throw "Godot patching must let renderer text boxes wrap CJK instead of inserting fixed-column hard line breaks"
}
if (-not $godotPatchSrc.Contains("GODOT_PATCH_MAX_SOURCE_TEXT_BYTES 2400u") -or
    -not $godotPatchSrc.Contains("GODOT_PATCH_MAX_TRANSLATION_TEXT_BYTES 4096u") -or
    $godotPatchSrc.Contains("n > 800") -or
    $godotPatchSrc.Contains("n > 1600")) {
    throw "Godot patching must collect and accept long glossary Translation descriptions"
}
if (-not $godotPatchSrc.Contains("GODOT_PCK_V1_HEADER_SIZE 88u") -or
    -not $godotPatchSrc.Contains("offsets_are_absolute") -or
    -not $godotPatchSrc.Contains("normalize_embedded_pck_v1_offsets") -or
    -not $godotPatchSrc.Contains("info.format != 1") -or
    -not $godotPatchSrc.Contains("if (!original_base) return 1;")) {
    throw "Godot patching must keep support for Godot 3.x PCK format 1 embedded packs with absolute offsets"
}
if (-not $godotPatchSrc.Contains("GODOT_PATCH_ENTRY_TEXT_RESOURCE") -or
    -not $godotPatchSrc.Contains("collect_json_text_patches") -or
    -not $godotPatchSrc.Contains("collect_scene_text_patches") -or
    -not $godotPatchSrc.Contains("collect_markdown_text_patches") -or
    -not $godotPatchSrc.Contains("godot_display_text_key") -or
    -not $godotPatchSrc.Contains('"name", "editorname"') -or
    -not $godotPatchSrc.Contains('"kin", "traittype"') -or
    -not $godotPatchSrc.Contains("godot_patch_text_resource_path") -or
    -not $godotPatchSrc.Contains('ends_with_i(path, ".tscn")') -or
    -not $godotPatchSrc.Contains('ends_with_i(path, ".tres")') -or
    -not $godotPatchSrc.Contains('ends_with_i(path, ".json")') -or
    -not $godotPatchSrc.Contains('ends_with_i(path, ".md")')) {
    throw "Godot patching must patch display text in .tscn/.tres/.json/Markdown resources when no Translation resources exist"
}
$godotTextPathBody = [regex]::Match(
    $godotPatchSrc,
    'static int godot_patch_text_resource_path\(const char \*path\) \{(?s:.*?)^\}',
    [System.Text.RegularExpressions.RegexOptions]::Multiline
).Value
$godotLooseTextPathBody = [regex]::Match(
    $godotPatchSrc,
    'static int loose_project_text_path\(const char \*path\) \{(?s:.*?)^\}',
    [System.Text.RegularExpressions.RegexOptions]::Multiline
).Value
if ($godotTextPathBody.Contains('ends_with_i(path, ".gd")') -or
    $godotLooseTextPathBody.Contains('ends_with_i(path, ".gd")') -or
    $godotPatchSrc.Contains('collect_gdscript_source_text_patches((const char *)buf')) {
    throw "Godot patching must not treat .gd source scripts as ordinary text resources; script constants need a runtime or whitelist-only path"
}
foreach ($deadGodotSymbol in @(
    "godot_wide_is_fullwidth",
    "godot_wide_is_closing_punct",
    "bb_add_wchar_utf8",
    "collect_binary_strings",
    "wanted_gdscript_source_text",
    "gdscript_source_string_context_is_display_text",
    "gdscript_parse_string_range",
    "collect_gdscript_source_text_patches",
    "bb_godot_string",
    "build_translation_resource"
)) {
    if ($godotPatchSrc.Contains($deadGodotSymbol)) {
        throw "Godot patch module must not retain unreachable legacy implementation: $deadGodotSymbol"
    }
}
if (-not $godotPatchSrc.Contains("build_loose_project_patch_pack") -or
    -not $godotPatchSrc.Contains("write_loose_patch_pack") -or
    -not $godotPatchSrc.Contains("collect_loose_project_overrides") -or
    -not $godotPatchSrc.Contains("res://project.godot") -or
    -not $godotPatchSrc.Contains("GODOT_PATCH_LOOSE_FONT_MIN_PRIORITY") -or
    -not $godotPatchSrc.Contains("no readable PCK source or loose project text overrides")) {
    throw "Godot patching must support loose exported projects with a minimal external override pack"
}
if (-not $godotPatchSrc.Contains("GODOT_RUNTIME_SCRIPT_NAME") -or
    -not $godotPatchSrc.Contains("godot_is_loose_project") -or
    -not $godotPatchSrc.Contains("godot_prepare_runtime_sidecar") -or
    -not $godotPatchSrc.Contains("HTTPRequest.new") -or
    -not $godotPatchSrc.Contains("http://127.0.0.1:19999/batch") -or
    -not $godotPatchSrc.Contains("DST_BATCH_SIZE") -or
    -not $godotPatchSrc.Contains("DST_MAX_INFLIGHT") -or
    -not $godotPatchSrc.Contains("DST_MAX_CACHE") -or
    -not $godotPatchSrc.Contains("DST_MAX_MISS") -or
    -not $godotPatchSrc.Contains("_dst_cache_order") -or
    -not $godotPatchSrc.Contains("_dst_cache_put") -or
    -not $godotPatchSrc.Contains("_dst_queue_head") -or
    $godotPatchSrc.Contains("_dst_queue.pop_front()") -or
    -not $godotPatchSrc.Contains("_dst_trim_miss") -or
    -not $godotPatchSrc.Contains("_dst_http_pool") -or
    -not $godotPatchSrc.Contains("_dst_busy") -or
    -not $godotPatchSrc.Contains("_dst_recent_miss") -or
    -not $godotPatchSrc.Contains("_dst_split_bbcode") -or
    -not $godotPatchSrc.Contains("_dst_render_cached") -or
    -not $godotPatchSrc.Contains("_dst_plain_wanted") -or
    -not $godotPatchSrc.Contains('parts.append({\"tag\": true') -or
    -not $godotPatchSrc.Contains('node.get(\"bbcode_enabled\") == true') -or
    -not $godotPatchSrc.Contains("is_visible_in_tree") -or
    -not $godotPatchSrc.Contains("C:/Windows/Fonts/simhei.ttf") -or
    -not $godotPatchSrc.Contains("DynamicFontData.new") -or
    -not $godotPatchSrc.Contains('add_font_override(\"font\"') -or
    -not $godotPatchSrc.Contains("delete_file_safe(final_pack)") -or
    -not $godotPatchSrc.Contains("prepared runtime sidecar instead of a --main-pack overlay")) {
    throw "Godot loose projects must use a runtime sidecar instead of a minimal --main-pack overlay that breaks res:// directory enumeration"
}
if (-not $godotPatchSrc.Contains("GODOT_RUNTIME_SIDECAR_G3") -or
    -not $godotPatchSrc.Contains("GODOT_RUNTIME_SIDECAR_G4") -or
    -not $godotPatchSrc.Contains("godot_runtime_major_from_source") -or
    -not $godotPatchSrc.Contains("info.format == 1 ? 3 : 4") -or
    -not $godotPatchSrc.Contains("godot_runtime_major_from_project") -or
    -not $godotPatchSrc.Contains('Callable(self, \"_dst_done\")') -or
    -not $godotPatchSrc.Contains("change_scene_to_file(scene)") -or
    -not $godotPatchSrc.Contains("FileAccess.file_exists") -or
    -not $godotPatchSrc.Contains("FontFile.new") -or
    -not $godotPatchSrc.Contains("add_theme_font_override") -or
    -not $godotPatchSrc.Contains("OS.get_cmdline_user_args()") -or
    -not $godotPatchSrc.Contains("JSON.stringify")) {
    throw "Godot runtime translation must select an engine-compatible Godot 3 or Godot 4 sidecar"
}
if (-not $godotPatchSrc.Contains("GODOT_PCK_V3_HEADER_SIZE 112u") -or
    -not $godotPatchSrc.Contains("info->directory_offset = read_u64le(header + 32)") -or
    -not $godotPatchSrc.Contains("embed_format3_runtime_scripts") -or
    -not $godotPatchSrc.Contains("build_runtime_override") -or
    -not $godotPatchSrc.Contains("[autoload_prepend]") -or
    -not $godotPatchSrc.Contains("GODOT_AUTOLOAD_OVERRIDE_PATH") -or
    -not $godotPatchSrc.Contains("info.engine_minor >= 6u") -or
    -not $godotPatchSrc.Contains("godot_patch_pack_has_runtime_autoload")) {
    throw "Godot 4.6 PCK format 3 must use its tail directory and prepend a runtime autoload through override.cfg"
}
$godot4Sidecar = [regex]::Match(
    $godotPatchSrc,
    'static const char GODOT_RUNTIME_SIDECAR_G4\[\] =(?s:.*?)(?=^static int godot_runtime_major_from_source)',
    [System.Text.RegularExpressions.RegexOptions]::Multiline
).Value
if ([string]::IsNullOrWhiteSpace($godot4Sidecar) -or
    -not $godot4Sidecar.Contains("DST_MAX_CACHE") -or
    -not $godot4Sidecar.Contains("DST_MAX_MISS") -or
    -not $godot4Sidecar.Contains('"const DST_MAX_TRACKED_CONTROLS = 4096\n"') -or
    -not $godot4Sidecar.Contains("_dst_cache_order") -or
    -not $godot4Sidecar.Contains("_dst_queue_head") -or
    $godot4Sidecar.Contains("_dst_queue.pop_front()") -or
    -not $godot4Sidecar.Contains("node_added.connect") -or
    -not $godot4Sidecar.Contains("req.process_mode = Node.PROCESS_MODE_ALWAYS") -or
    -not $godot4Sidecar.Contains("timer.process_mode = Node.PROCESS_MODE_ALWAYS") -or
    $godot4Sidecar.Contains('"if self is Node\n"') -or
    $godot4Sidecar.Contains('"self.set(\"process_mode\""') -or
    -not $godot4Sidecar.Contains("_dst_track_control") -or
    -not $godot4Sidecar.Contains("_dst_scan_controls()") -or
    -not $godot4Sidecar.Contains("_dst_control_cursor") -or
    -not $godot4Sidecar.Contains("while scanned < count") -or
    -not $godot4Sidecar.Contains("_dst_recent_miss") -or
    -not $godot4Sidecar.Contains("_dst_mark_miss")) {
    throw "Godot 4 runtime translation must bound cache and failed-request state"
}
if (-not $godotPatchSrc.Contains("GODOT_PATCH_ENTRY_GDSCRIPT_BYTECODE") -or
    -not $godotPatchSrc.Contains("godot_patch_gdscript_bytecode_path") -or
    -not $godotPatchSrc.Contains("collect_gdscript_bytecode_patches") -or
    -not $godotPatchSrc.Contains("build_gdscript_bytecode_patch") -or
    -not $godotPatchSrc.Contains("wanted_gdscript_constant_text") -or
    -not $godotPatchSrc.Contains("gdscript_translation_preserves_format_tokens") -or
    -not $godotPatchSrc.Contains('memcmp(buf, "GDSC", 4)') -or
    -not $godotPatchSrc.Contains("version != 13") -or
    -not $godotPatchSrc.Contains('/scripts/menus/') -or
    -not $godotPatchSrc.Contains('/in_game_editor/scripts/resource/scenario_action_data.gdc') -or
    -not $godotPatchSrc.Contains('/in_game_editor/scripts/static/target_type.gdc') -or
    -not $godotPatchSrc.Contains('/in_game_editor/scripts/static/unit_stat.gdc')) {
    throw "Godot patching must safely patch selected GDScript bytecode display constants without scanning arbitrary binary text"
}
if (-not $godotPatchSrc.Contains("GODOT_PATCH_ENTRY_FONT") -or
    -not $godotPatchSrc.Contains("GODOT_PATCH_MAX_FONT_BYTES") -or
    -not $godotPatchSrc.Contains("GODOT_PATCH_MAX_FONT_REPLACEMENTS") -or
    -not $godotPatchSrc.Contains("godot_patch_font_resource_priority") -or
    -not $godotPatchSrc.Contains("font_replacements") -or
    -not $godotPatchSrc.Contains("rocker") -or
    -not $godotPatchSrc.Contains("logo") -or
    -not $godotPatchSrc.Contains("find_system_cjk_font") -or
    -not $godotPatchSrc.Contains("append_file_from_path") -or
    -not $godotPatchSrc.Contains('ascii_ends_with_i(path, ".ttf")') -or
    -not $godotPatchSrc.Contains('ascii_ends_with_i(path, ".otf")')) {
    throw "Godot patching must limit renderer-local CJK font replacement to a bounded non-decorative font set"
}
if (-not $godotPatchSrc.Contains("godot_source_looks_like_path_or_url") -or
    $godotPatchSrc.Contains("strchr(s, '/') || strchr(s, '\\')")) {
    throw "Godot source filtering must not reject every slash-bearing sentence as a resource path"
}
if (-not $godotPatchSrc.Contains("godot_patch_story_text_path") -or
    -not $godotPatchSrc.Contains("godot_patch_startup_text_path") -or
    -not $godotPatchSrc.Contains("godot_patch_ui_data_path") -or
    -not $godotPatchSrc.Contains('ascii_ends_with_i(path, "/skills.json")') -or
    -not $godotPatchSrc.Contains('ascii_ends_with_i(path, "/heroes.json")') -or
    -not $godotPatchSrc.Contains('ascii_ends_with_i(path, "/date_traits.json")') -or
    -not $godotPatchSrc.Contains('ascii_contains_i(path, "/prologue/")') -or
    -not $godotPatchSrc.Contains('ascii_contains_i(path, "/intro/")') -or
    -not $godotPatchSrc.Contains('ascii_contains_i(path, "tutorial")') -or
    -not $godotPatchSrc.Contains("godot_patch_low_value_tooling_path") -or
    -not $godotPatchSrc.Contains('ascii_ends_with_i(path, ".json")') -or
    -not $godotPatchSrc.Contains('ascii_contains_i(path, "/data/")')) {
    throw "Godot text-resource patching must prioritize high-value story/data JSON before low-value tooling resources"
}
if (-not $godotPatchSrc.Contains("GODOT_PATCH_NEXT_NAME") -or
    -not $godotPatchSrc.Contains("GODOT_PATCH_BUILDING_NAME") -or
    -not $godotPatchSrc.Contains("godot_promote_staged_patch_pack") -or
    -not $godotPatchSrc.Contains("move_file_safe(build_pack, final_pack") -or
    -not $godotPatchSrc.Contains("move_file_safe(build_pack, next_pack") -or
    -not $godotPatchSrc.Contains("active patch pack is busy; staged refreshed pack for next launch") -or
    -not $godotPatchSrc.Contains("_wcsicmp(fd.cFileName, GODOT_PATCH_NEXT_NAME)") -or
    -not $godotPatchSrc.Contains("_wcsicmp(fd.cFileName, GODOT_PATCH_BUILDING_NAME)")) {
    throw "Godot patch refresh must build atomically and stage next-run packs instead of reusing stale or partial patch packs"
}
if (-not $godotPatchSrc.Contains("godot_leading_rich_tag_span") -or
    -not $godotPatchSrc.Contains("godot_dup_patch_query_text") -or
    -not $godotPatchSrc.Contains("godot_restore_patch_query_text") -or
    -not $godotPatchSrc.Contains("batch_texts[j] = queries[j] ? queries[j] : texts->v[i + j]") -or
    -not $godotPatchSrc.Contains("strcmp(results[j], batch_texts[j])")) {
    throw "Godot patching must translate rich-text bodies while preserving renderer-local style tags"
}

if (Test-Path (Join-Path $bin "gcc.exe")) {
    $env:PATH = "$bin;$env:PATH"
} else {
    if (-not (Get-Command gcc -ErrorAction SilentlyContinue)) {
        throw "Missing gcc. Install w64devkit and add its bin directory to PATH, or place it at $bin"
    }
    if (-not (Get-Command windres -ErrorAction SilentlyContinue)) {
        throw "Missing windres. Install w64devkit and add its bin directory to PATH, or place it at $bin"
    }
}

$mainSrc = Get-Content -LiteralPath (Join-Path $root "native\src\launcher\main.c") -Raw -Encoding UTF8
if ($mainSrc -match '(?s)case\s+WM_DESTROY:.*stop_server\s*\(') {
    throw "launcher must not stop the translation server when the window closes"
}
if ($mainSrc -notmatch 'sync_embedded_payloads\(\)') {
    throw "launcher must sync embedded first-party components on startup so exe-only updates work"
}
if ($mainSrc -notmatch '--sync-payloads-and-exit') {
    throw "launcher must keep a headless embedded payload sync mode for release verification"
}
if (-not $mainSrc.Contains($productName)) {
    throw "launcher window title must use the ds娓告垙缈昏瘧鍣?product name"
}
if ($mainSrc -notmatch 'IDI_APP_ICON' -or
    $mainSrc -notmatch 'RegisterClassExW' -or
    $mainSrc -notmatch 'hIconSm') {
    throw "launcher must load the bundled application icon for large and small window icons"
}
$uiSrc = Get-Content -LiteralPath (Join-Path $root "native\src\launcher\ui.c") -Raw -Encoding UTF8
if ($uiSrc -notmatch 'OutputDebugStringW\(line\)' -or $uiSrc -notmatch '!g_log \|\| !IsWindow\(g_log\)') {
    throw "launcher logging must be safe before UI controls exist"
}
if (-not $uiSrc.Contains($productName)) {
    throw "launcher UI/dialog branding must use the ds娓告垙缈昏瘧鍣?product name"
}
if ($uiSrc -notmatch 'DS_TRANSLATOR_VERSION_W' -or $uiSrc -match 'v3\.1\.70') {
    throw "launcher footer must use the VERSION-derived product version, not a hard-coded stale version"
}
$paintBody = [regex]::Match(
    $uiSrc,
    'void paint_background\(HWND hwnd, HDC dc\) \{(?s:.*?)^\}',
    [System.Text.RegularExpressions.RegexOptions]::Multiline
).Value
$tickBody = [regex]::Match(
    $uiSrc,
    'void tick_ui_animation\(HWND hwnd\) \{(?s:.*?)^\}',
    [System.Text.RegularExpressions.RegexOptions]::Multiline
).Value
$layoutBody = [regex]::Match(
    $uiSrc,
    'void layout\(HWND hwnd\) \{(?s:.*?)^\}',
    [System.Text.RegularExpressions.RegexOptions]::Multiline
).Value
if ([string]::IsNullOrWhiteSpace($paintBody) -or
    -not $uiSrc.Contains('draw_tech_grid') -or
    -not $uiSrc.Contains('draw_panel_shell') -or
    -not $uiSrc.Contains('draw_hero_data_line') -or
    -not $uiSrc.Contains('draw_button_icon') -or
    -not $uiSrc.Contains('MAKEINTRESOURCEW(IDI_APP_ICON)') -or
    -not $uiSrc.Contains('DwmSetWindowAttribute') -or
    -not $uiSrc.Contains('DarkMode_Explorer') -or
    -not $uiSrc.Contains('C_VIOLET') -or
    -not $uiSrc.Contains('C_AMBER') -or
    $mainSrc -notmatch 'SetTimer\(hwnd,\s*2,\s*\d+,\s*NULL\)' -or
    -not $mainSrc.Contains('tick_ui_animation(hwnd)') -or
    $paintBody.Contains('server_alive()')) {
    throw "launcher visual system must keep branded dark chrome, multi-accent panels, icon buttons, and IO-free lightweight animation"
}
if ([string]::IsNullOrWhiteSpace($tickBody) -or
    [string]::IsNullOrWhiteSpace($layoutBody) -or
    -not $mainSrc.Contains('WS_CLIPCHILDREN') -or
    -not $mainSrc.Contains('paint_background_buffered(hwnd, dc, &ps.rcPaint)') -or
    -not $mainSrc.Contains('case WM_PRINTCLIENT:') -or
    -not $tickBody.Contains('data_line') -or
    $tickBody.Contains('log_header') -or
    $tickBody.Contains('RECT hero') -or
    -not $layoutBody.Contains('SWP_NOREDRAW') -or
    -not $uiSrc.Contains('DT_END_ELLIPSIS')) {
    throw "launcher repainting must isolate animation from text, clip child controls, buffer the parent background, and constrain button text"
}
if ($mainSrc -notmatch 'IDC_RESTORE' -or
    $mainSrc -notmatch 'restore_selected_game\(\)' -or
    $uiSrc -notmatch 'MB_DEFBUTTON2' -or
    $uiSrc -notmatch 'restore_game\(g_game, engine\)') {
    throw "launcher must expose a confirmed restore-game action"
}
$clearCacheBody = [regex]::Match(
    $uiSrc,
    'void clear_translation_cache\(void\) \{(?s:.*?)^\}',
    [System.Text.RegularExpressions.RegexOptions]::Multiline
).Value
if ($mainSrc -notmatch 'IDC_CLEAR_CACHE' -or
    $mainSrc -notmatch 'clear_translation_cache\(\)' -or
    $uiSrc -notmatch '(?:MoveWindow|position_control)\(g_btn_clear_cache' -or
    [string]::IsNullOrWhiteSpace($clearCacheBody) -or
    -not $clearCacheBody.Contains('translation_memory_c.tsv') -or
    -not $clearCacheBody.Contains('MB_DEFBUTTON2') -or
    -not $clearCacheBody.Contains('stop_server();') -or
    -not $clearCacheBody.Contains('delete_file_safe(cache_path)') -or
    -not $clearCacheBody.Contains('start_server();') -or
    -not $clearCacheBody.Contains('server_stopped')) {
    throw "CACHE card must expose a confirmed clear action that stops the server before deleting the shared cache"
}
$fsutilSrc = Get-Content -LiteralPath (Join-Path $root "native\src\launcher\fsutil.c") -Raw
if ($fsutilSrc -notmatch 'static int read_handle_all' -or $fsutilSrc -notmatch 'static int write_handle_all') {
    throw "launcher file IO must centralize complete ReadFile/WriteFile loops"
}
if ($fsutilSrc -notmatch '\*out = NULL;\s*\*size = 0;') {
    throw "launcher file reads must clear output ownership before attempting IO"
}
if ($fsutilSrc -notmatch 'path_has_reparse_point' -or
    $fsutilSrc -notmatch 'FILE_ATTRIBUTE_REPARSE_POINT' -or
    $fsutilSrc -notmatch 'path_append_suffix' -or
    $fsutilSrc -notmatch 'move_file_safe' -or
    $fsutilSrc -notmatch 'delete_file_safe') {
    throw "launcher filesystem mutations must fail closed on directory and file reparse points"
}
$serverProcSrc = Get-Content -LiteralPath (Join-Path $root "native\src\launcher\server_proc.c") -Raw
$startServerBat = Get-Content -LiteralPath (Join-Path $root "start_server.bat") -Raw
if ($startServerBat -match '(?im)^\s*:loop\s*$' -or $startServerBat -match '(?im)^\s*goto\s+loop\s*$') {
    throw "manual server launcher must expose dst_server exit instead of restarting forever"
}
if ($startServerBat -notmatch 'server_exit=%ERRORLEVEL%' -or
    $startServerBat -notmatch 'exited with code %server_exit%') {
    throw "manual server launcher must preserve and log dst_server exit codes"
}
if ($serverProcSrc -notmatch 'server_http_alive') {
    throw "launcher must detect an already-running translation server"
}
if ($serverProcSrc -notmatch 'if\s*\(\s*server_http_alive\(200\)\s*\)') {
    throw "start_server must adopt an already-running translation server"
}
if ($serverProcSrc -notmatch 'void\s+refresh_server_status\s*\(' -or
    $serverProcSrc -notmatch '(?s)refresh_server_status.*?server_http_alive\(200\).*?g_server_started\s*=\s*1') {
    throw "launcher startup must reconcile UI state with an already-running translation server"
}
if ($serverProcSrc -notmatch 'wait_for_server_ready' -or
    $serverProcSrc -notmatch '(?s)CreateProcessW.*?wait_for_server_ready\(.*?reset_server_handle\(\).*?return\s+0') {
    throw "launcher must not report a newly spawned server as running until /health is reachable"
}
if ($serverProcSrc -notmatch 'static\s+int\s+request_server_shutdown' -or
    $serverProcSrc -notmatch 'WinHttpQueryHeaders' -or
    $serverProcSrc -notmatch 'GetExitCodeProcess') {
    throw "launcher shutdown and startup failures must retain HTTP status and child exit diagnostics"
}
if ($serverProcSrc -notmatch 'wait_for_server_stopped' -or
    $serverProcSrc -notmatch '(?s)request_server_shutdown\(\).*?wait_for_server_stopped\(1500\)') {
    throw "launcher must wait for an adopted server to release the cache before maintenance"
}
if ($mainSrc -notmatch '(?s)case\s+WM_CREATE:.*refresh_server_status\(\).*layout\(hwnd\);') {
    throw "launcher must refresh server status during startup before first layout"
}
if ($uiSrc -notmatch '(?s)if\s*\(!start_server\(\)\)\s*\{.{0,500}?return;') {
    throw "launcher must stop translation launch when the local server cannot start"
}
$engineSrc = Get-Content -LiteralPath (Join-Path $root "native\src\launcher\engine.c") -Raw -Encoding UTF8
if ($engineSrc -notmatch 'has_file_pattern\(p,\s*L"\*\.rpy"\)') {
    throw "Ren'Py detection must recognize source-only .rpy games, not only compiled .rpyc/.rpa packages"
}
if ($engineSrc -notmatch 'ENGINE_GODOT' -or
    $engineSrc -notmatch 'has_file_pattern\(dir,\s*L"\*\.pck"\)' -or
    $engineSrc -notmatch 'has_godot_embedded_exe' -or
    $engineSrc -notmatch 'GDPC' -or
    $engineSrc -notmatch 'project\.godot' -or
    $engineSrc -notmatch 'godot_project\.binary') {
    throw "launcher engine detection must recognize Godot sidecar/embedded exports and source projects"
}
$unityDetect = $engineSrc.IndexOf('find_subdir_suffix(dir, L"_Data")')
$godotDetect = $engineSrc.IndexOf('godot_export_or_project(dir)')
if ($unityDetect -lt 0 -or $godotDetect -lt 0 -or $unityDetect -gt $godotDetect) {
    throw "Unity _Data detection must stay before Godot .pck detection so Unity games with unrelated .pck files are not misclassified"
}
if (-not $engineSrc.Contains($productExeName) -or
    $engineSrc -notmatch 'DeepSeekTranslator\.exe' -or
    $engineSrc -notmatch 'dst_godot_patch\.exe' -or
    $engineSrc -notmatch 'dst_server\.exe') {
    throw "launcher engine detection must ignore current and legacy translator executables"
}
$warmupSrc = Get-Content -LiteralPath (Join-Path $root "native\src\launcher\warmup.c") -Raw
$warmupInternalSrc = Get-Content -LiteralPath (Join-Path $root "native\src\launcher\warmup_internal.h") -Raw
$godotWarmupSrc = Get-Content -LiteralPath (Join-Path $root "native\src\launcher\godot_warmup.c") -Raw
if (-not $godotWarmupSrc.Contains("GODOT_PCK_V3_HEADER_SIZE 112u") -or
    -not $godotWarmupSrc.Contains("directory_offset = read_u64le(header + 32)")) {
    throw "Godot warmup must read PCK format 3 resources through the pack-tail directory"
}
if ($warmupSrc -notmatch 'scan_renpy_script_dir') {
    throw "Ren'Py warmup must scan script files for prefetch candidates"
}
if ($warmupSrc -notmatch 'static void collect_renpy_line_strings' -or
    $warmupSrc -notmatch 'while \(\*cursor && seen < 16u' -or
    $warmupSrc -notmatch 'char \*text = renpy_string_at\(&cursor\);' -or
    $warmupSrc -notmatch 'collect_renpy_line_strings\(line, prefetch\);') {
    throw "Ren'Py warmup must scan every accepted-line string literal with escapes and apply its script-specific filter"
}
if ($warmupSrc -match 'const char \*fq = strchr\(line,\s*''"''\);') {
    throw "Ren'Py warmup must not regress to the old double-quote-only scanner"
}
if ($warmupSrc -notmatch 'ENGINE_RENPY\)\s+warmup_renpy') {
    throw "Ren'Py warmup must be wired into warmup_translations"
}
if ($warmupSrc -notmatch 'wide_ends_with_i\(fd\.cFileName,\s*L"\.rpy"\)') {
    throw "Ren'Py warmup must target .rpy scripts conservatively"
}
if ($warmupSrc -notmatch 'RENPY_WARMUP_MAX_ITEMS 100000') {
    throw "Ren'Py warmup must preheat the whole script, not the first 1200 lines"
}
if ($warmupSrc -notmatch 'UNITY_WARMUP_MAX_ITEMS 8000') {
    throw "Unity warmup must have its own larger cap so VN text can be prefetched before display"
}
if ($warmupSrc -notmatch 'unity_bundle_file_name' -or
    $warmupSrc -notmatch 'scan_unity_bundle_file' -or
    $warmupSrc -notmatch 'UNITY_BUNDLE_SCAN_MAX_BYTES') {
    throw "Unity warmup must stream common AssetBundle containers instead of skipping dialogue stored outside .assets files"
}
if ($warmupInternalSrc -notmatch 'GODOT_WARMUP_MAX_ITEMS 12000') {
    throw "Godot warmup must have its own resource scan cap"
}
if ($warmupSrc -notmatch 'RPGM_WARMUP_MAX_ITEMS 40000') {
    throw "RPG Maker warmup must not stop after the first 1200 texts in large games"
}
if ($warmupSrc -notmatch 'prefetch\.max_items = RENPY_WARMUP_MAX_ITEMS;') {
    throw "Ren'Py warmup list must raise its item cap"
}
if ($warmupSrc -notmatch 'WARMUP_BATCH_ITEMS 512') {
    throw "launcher warmup should post larger local batches for high-volume VN scripts"
}
if ($warmupSrc -notmatch 'prefetch\.max_items = UNITY_WARMUP_MAX_ITEMS;') {
    throw "Unity warmup list must raise its item cap without changing RPGM defaults"
}
if ($warmupSrc -notmatch 'prefetch\.max_items = GODOT_WARMUP_MAX_ITEMS;' -or
    $warmupSrc -notmatch 'warmup_scan_godot_resources\(dir, &prefetch\);' -or
    $godotWarmupSrc -notmatch 'scan_godot_resource_dir' -or
    $warmupSrc -notmatch 'ENGINE_GODOT\)\s+warmup_godot') {
    throw "Godot warmup must scan resources and be wired into warmup_translations"
}
if ($uiSrc -notmatch 'engine == ENGINE_GODOT(?s:.*?)warmup_translations\(dir, engine\)') {
    throw "Godot launch flow must run resource warmup after starting the game"
}
if ($godotWarmupSrc -notmatch 'scan_godot_po_strings' -or
    $godotWarmupSrc -notmatch 'scan_godot_csv_strings' -or
    $godotWarmupSrc -notmatch 'scan_godot_binary_buffer' -or
    $godotWarmupSrc -notmatch 'godot_translatable_quote') {
    throw "Godot warmup must parse PO/CSV/script contexts and UTF-8 binary payloads, not just generic quoted strings"
}
if ($warmupSrc -notmatch 'prefetch\.max_items = RPGM_WARMUP_MAX_ITEMS;') {
    throw "RPG Maker warmup list must use its engine-specific item cap"
}
if ($warmupSrc -notmatch 'should_warm_rpgm_text') {
    throw "RPG Maker warmup must preserve valid engine control sequences instead of rejecting every backslash"
}
if ($warmupSrc -notmatch 'warmup_scan_rpgm_resources\(dir,\s*&prefetch\);' -or
    $warmupSrc -notmatch 'scan_rpgm_external_texts\(content_root,\s*prefetch\);' -or
    $warmupSrc -notmatch 'scan_rpgm_csv_file' -or
    $warmupSrc -notmatch 'wide_ends_with_i\(fd\.cFileName,\s*L"\.csv"\)') {
    throw "RPG Maker warmup must resolve the content root and include plugin-owned external TXT/CSV localization files"
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
if ($uiSrc -notmatch 'static void run_engine_launch_flow\(const WCHAR \*dir, Engine engine\)' -or
    $uiSrc -notmatch '(?s)run_engine_launch_flow\(const WCHAR \*dir, Engine engine\).{0,500}?ENGINE_RENPY.{0,500}?launch_game_for_engine\(dir, engine\);.{0,500}?warmup_translations\(dir, engine\);' -or
    $uiSrc -notmatch 'run_engine_launch_flow\(a->dir, a->engine\);' -or
    $uiSrc -notmatch 'run_engine_launch_flow\(g_game, e\);') {
    throw "worker and synchronous fallback must share one engine launch flow, with Ren'Py launch before warmup"
}
if ($uiSrc -notmatch 'ENGINE_GODOT\)\s+deployed = deploy_godot') {
    throw "launcher start flow must route Godot games through the Godot deploy module"
}
if ($mainSrc -notmatch '--godot-patch-worker' -or
    $mainSrc -notmatch 'CommandLineToArgvW' -or
    $mainSrc -notmatch 'warmup_translations\(argv\[i \+ 1\], ENGINE_GODOT\)' -or
    $mainSrc -notmatch 'godot_prepare_patch_pack\(argv\[i \+ 1\]' -or
    $mainSrc -notmatch 'start_server\(\)' -or
    $uiSrc -notmatch 'start_godot_patch_worker' -or
    $uiSrc -notmatch 'godot_prepare_patch_launcher' -or
    $uiSrc -notmatch '--main-pack' -or
    $uiSrc -notmatch 'dst_godot_patch\.pck' -or
    $uiSrc -notmatch 'CreateProcessW\(runtime_exe,\s*cmd' -or
    $uiSrc -notmatch '\\"%s\\" --main-pack \\"%s\\" --language en') {
    throw "Godot launch flow must build and use an external translation patch pack through the detached worker"
}
if ($uiSrc -notmatch 'Godot: existing patch pack found; launching before cache warmup and patch refresh\.' -or
    $uiSrc -notmatch 'Godot: no patch pack yet; launching first, then warming resources for patch preparation\.' -or
    $uiSrc -notmatch '(?s)else if \(engine == ENGINE_GODOT\).*?int had_patch = exists_path\(patch\);.*?if \(had_patch\).*?launch_game_for_engine\(dir, engine\);.*?else.*?launch_game_for_engine\(dir, engine\);.*?warmup_translations\(dir, engine\);.*?start_godot_patch_worker\(dir\)') {
    throw "Godot launch must not block game startup on large patch-pack generation"
}
if (-not $uiSrc.Contains("godot_promote_staged_patch_pack(dir)") -or
    -not $uiSrc.Contains("launch_godot_with_runtime_sidecar") -or
    -not $uiSrc.Contains(' --path \"%s\"') -or
    -not $uiSrc.Contains(' --script \"res://dst_godot_runtime.gd\"') -or
    -not $uiSrc.Contains("godot_is_loose_project(dir)") -or
    -not $uiSrc.Contains("Godot: patch refresh worker started.") -or
    -not $uiSrc.Contains("detached patch refresh did not start; current game continues with the existing pack")) {
    throw "Godot launch must refresh stale existing patch packs in the background and promote staged packs before start"
}
if (-not $uiSrc.Contains("launch_godot_export_with_runtime_sidecar") -or
    -not $uiSrc.Contains('--main-pack \"%s\" --script \"%s\" --language en') -or
    -not $uiSrc.Contains('--script \"%s\" --language en') -or
    -not $uiSrc.Contains("no patch pack yet; using the generic runtime translator for this launch") -or
    -not $uiSrc.Contains("Launching Godot export with patch pack and runtime translator") -or
    -not $uiSrc.Contains("Launching Godot export with runtime translator before static patch is ready")) {
    throw "Godot exported packs must use the generic runtime translator both before and after static patch generation"
}
if (-not $uiSrc.Contains("godot_runtime_sidecar_preflight") -or
    -not $uiSrc.Contains("godot_runtime_autoload_preflight") -or
    -not $uiSrc.Contains("godot_patch_pack_has_runtime_autoload") -or
    -not $uiSrc.Contains('\"%s\" --headless -- --dst-preflight') -or
    -not $uiSrc.Contains("runtime autoload preflight failed; trying script-launch compatibility") -or
    -not $uiSrc.Contains("--dst-preflight") -or
    $uiSrc.Contains("--check-only") -or
    -not $uiSrc.Contains("WaitForSingleObject") -or
    -not $uiSrc.Contains("CREATE_NO_WINDOW") -or
    -not $uiSrc.Contains("Godot: runtime sidecar preflight failed; falling back to the static launch path.")) {
    throw "Godot runtime sidecars must be preflighted before launch so unsupported exports fall back safely"
}
if (-not $uiSrc.Contains("godot_output_explicitly_rejects_main_pack") -or
    -not $uiSrc.Contains("STARTF_USESTDHANDLES") -or
    -not $uiSrc.Contains("support remains inconclusive and translation launch will still be attempted") -or
    $uiSrc -match 'rejected\s*=\s*waited\s*==\s*WAIT_OBJECT_0\s*&&\s*[\r\n\s]*GetExitCodeProcess') {
    throw "Godot --main-pack compatibility must distinguish an explicit option rejection from unrelated headless startup failures"
}
$cacheSrc = Get-Content -LiteralPath (Join-Path $root "native\src\server\cache.c") -Raw
$httpSrc = Get-Content -LiteralPath (Join-Path $root "native\src\server\http.c") -Raw
if ($httpSrc -notmatch 'ASYNC_BATCH_MAX 48' -or
    $httpSrc -notmatch 'ASYNC_BATCH_CHAR_BUDGET 9600' -or
    $httpSrc -notmatch 'LIVE_BATCH_MAX 48' -or
    $httpSrc -notmatch 'LIVE_BATCH_CHAR_BUDGET 9600') {
    throw "server live and warmup translation paths should use larger API batches for faster VN catch-up"
}
if ($cacheSrc -notmatch '(?s)void cache_set_many_persist.*?AcquireSRWLockExclusive\(&c->io_lock\);.*?AcquireSRWLockExclusive\(&c->lock\);.*?cache_insert_locked.*?ReleaseSRWLockExclusive\(&c->lock\);.*?fprintf\(f,.*?if \(wrote && fflush\(f\) != 0\).*?ReleaseSRWLockExclusive\(&c->io_lock\);' -or
    $cacheSrc -notmatch '(?s)void cache_set_persist.*?cache_set_many_persist\(c, keys, values, 1\);') {
    throw "persistent cache batches must stay ordered on io_lock, release the map lock before disk IO, and flush once"
}
$deploySrc = Get-Content -LiteralPath (Join-Path $root "native\src\launcher\deploy.c") -Raw
if ($deploySrc -match 'catch\s*\(e\)\s*\{\s*\}') {
    throw "Embedded RPG Maker runtime must not silently swallow JavaScript exceptions"
}
if ($deploySrc -match 'except Exception:\\n"\s*\r?\n"\s*pass\\n') {
    throw "Embedded Ren'Py runtime must not silently pass broad Python exceptions"
}
if (-not $deploySrc.Contains('_ds_report_exception') -or -not $deploySrc.Contains('reportRpgmError')) {
    throw "Embedded Ren'Py and RPG Maker runtimes must expose named diagnostic reporters"
}
if (-not $deploySrc.Contains('except ImportError:') -or -not $deploySrc.Contains('except NameError:')) {
    throw "Expected Ren'Py capability detection must use precise exception types"
}
if (-not (Test-Path (Join-Path $root "scripts\install_runtime_payloads.ps1"))) {
    throw "runtime payload installer script must ship with source and program packages"
}
$runtimeInstallerSrc = Get-Content -LiteralPath (Join-Path $root "scripts\install_runtime_payloads.ps1") -Raw
if ($runtimeInstallerSrc -notmatch 'Assert-NoReparseTraversal' -or
    $runtimeInstallerSrc -notmatch 'FileAttributes\]::ReparsePoint' -or
    $runtimeInstallerSrc.IndexOf('Assert-UnderRoot $DownloadCache') -gt
    $runtimeInstallerSrc.IndexOf('New-Item -ItemType Directory -Force -Path $DownloadCache')) {
    throw "runtime installer must reject reparse traversal before creating, extracting, or deleting payload paths"
}
if ($runtimeInstallerSrc -notmatch 'Newtonsoft\.Json\.13\.0\.4\.zip' -or $runtimeInstallerSrc -notmatch 'Copy-Item -LiteralPath \$pkg -Destination \$zipPkg') {
    throw "runtime payload installer must rename NuGet .nupkg to .zip before Expand-Archive for Windows PowerShell 5.1"
}
if ($runtimeInstallerSrc -notmatch 'TMP_Font_AssetBundles_2025-12-08\.7z' -or
    $runtimeInstallerSrc -notmatch '889e963fb9dbd4b64927e0adf5d9060e1d0fb9d6bceb0c407d0597643e2b54ec' -or
    $runtimeInstallerSrc -notmatch 'payloads\\UnityIL2CPP\\TMPFontAssetBundles\\BepInEx\\font' -or
    $runtimeInstallerSrc -notmatch 'Expand-WithExternalArchiveTool') {
    throw "runtime payload installer must install XUnity TMP font asset bundles for public Unity TMP parity"
}
if ($runtimeInstallerSrc -notmatch 'BepInEx_win_x86_5\.4\.23\.5\.zip' -or
    $runtimeInstallerSrc -notmatch 'BepInEx-Unity\.Mono-win-x86-6\.0\.0-be\.755\+3fab71a\.zip' -or
    $runtimeInstallerSrc -notmatch 'UnityMonoRuntimeX86' -or
    $runtimeInstallerSrc -notmatch 'UnityMonoRuntime6X86') {
    throw "runtime payload installer must provide x86 and x64 BepInEx runtimes for Unity Mono"
}
if ($runtimeInstallerSrc -notmatch 'mono-6\.12\.0\.206-x64-0\.msi' -or
    $runtimeInstallerSrc -notmatch '4125f57d97cfa88257915edc969e913de198cd8e22396a29849037479a0ac368' -or
    $runtimeInstallerSrc -notmatch 'download\.mono-project\.com/archive/6\.12\.0/windows-installer' -or
    $runtimeInstallerSrc -notmatch 'UnityMonoCorlib' -or
    $runtimeInstallerSrc -notmatch '/a' -or
    $runtimeInstallerSrc -notmatch 'payloads\\UnityMonoCorlib') {
    throw "runtime payload installer must reproducibly extract the pinned official Mono corlib payload"
}
if ($deploySrc -notmatch 'unity_player_machine' -or
    $deploySrc -notmatch 'loader_machine != machine' -or
    $deploySrc -notmatch 'UnityMonoRuntime6X86') {
    throw "Unity Mono deploy must select Doorstop architecture from the Unity player and repair mismatches"
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
if ($deploySrc -notmatch 'say_menu_text_filter = _ds_say_menu_text_filter') {
    throw "Ren'Py hook must translate compiled say/menu script text via config.say_menu_text_filter"
}
if ($deploySrc -notmatch '_ds_protect_old_percent' -or
    $deploySrc -notmatch 'out\.append\(''%%''\)' -or
    $deploySrc -notmatch '_ds_protect_old_percent\(_ds_translate\(s, True\)\)') {
    throw "Ren'Py say/menu filter must escape literal percent signs before Ren'Py old_substitutions"
}
if (-not $deploySrc.Contains("_ds_restore_renpy_tokens") -or
    -not $deploySrc.Contains("_ds_restore_span_sequence(src, out, '[', ']')") -or
    -not $deploySrc.Contains("_ds_restore_span_sequence(src, out, '{', '}')") -or
    -not $deploySrc.Contains("len(out_spans) != len(src_spans)") -or
    -not $deploySrc.Contains("return src") -or
    -not $deploySrc.Contains("restored = _ds_restore_renpy_tokens(k, v)") -or
    -not $deploySrc.Contains("_ds_memo_put(k, restored, now)") -or
    -not $deploySrc.Contains("_ds_mark_terminal_negative(k)") -or
    -not $deploySrc.Contains("_ds_terminal_negative_ttl = 30.0") -or
    -not $deploySrc.Contains("_ds_is_terminal_negative(s, now)") -or
    -not $deploySrc.Contains("terminal-negative-recheck")) {
    throw "Ren'Py hook must restore interpolation variables and text tags, or preserve source when tokens are lost"
}
if ($deploySrc.Contains("x.open('POST',URL,false)") -or
    -not $deploySrc.Contains("function sendRpgmJson") -or
    -not $deploySrc.Contains("process.versions.nw") -or
    -not $deploySrc.Contains("require('http')") -or
    -not $deploySrc.Contains("x.open('POST',endpoint,true)") -or
    -not $deploySrc.Contains("pending[key]=true")) {
    throw "RPG Maker renderer hooks must use deduplicated asynchronous transport with an NW.js Node HTTP path"
}
if ($deploySrc.Contains("JSON.stringify({text:key,cache_only:true})") -or
    -not $deploySrc.Contains("sendRpgmJson(URL,{text:key},12000")) {
    throw "RPG Maker visible misses must bypass background warmup priority without blocking the renderer"
}
if (-not $deploySrc.Contains("function isLayoutOnly") -or
    -not $deploySrc.Contains("<br\\s*\\/?>") -or
    -not $deploySrc.Contains("hasCjk(s)||isLayoutOnly(s)")) {
    throw "RPG Maker layout-only message rows must not invalidate an otherwise translated atomic page"
}
if (-not $deploySrc.Contains("safeRpgmTranslation") -or
    -not $deploySrc.Contains("rpgmControlTokens") -or
    -not $deploySrc.Contains("original.length>24||joined.length>4000") -or
    -not $deploySrc.Contains("planMessagePage") -or
    -not $deploySrc.Contains("[plan.joined].concat(plan.candidates)") -or
    -not $deploySrc.Contains("MESSAGE_PAGE_BATCH_MAX=32") -or
    -not $deploySrc.Contains("MESSAGE_PAGE_CHAR_BUDGET=6000") -or
    -not $deploySrc.Contains("primeMessagePage(plan.liveCandidates)") -or
    -not $deploySrc.Contains("withLookupRequestsSuppressed(function(){return translateTextArray(original,false,true);})")) {
    throw "Large RPG Maker message pages must use control-safe, language-atomic per-line cache lookups instead of whole-page translation"
}
if (-not $deploySrc.Contains("translateConvertedRpgmText(s,true)") -or
    -not $deploySrc.Contains("requestLocalCacheKeys([orig],1)") -or
    -not $deploySrc.Contains("hasSuspiciousRpgmSourceResidue") -or
    -not $deploySrc.Contains("rpgmLatinWords")) {
    throw "RPG Maker first draws must use local-only cache hits and reject copied English clauses inside CJK results"
}
if (-not $deploySrc.Contains("messagePageHasLocalLookupPending") -or
    -not $deploySrc.Contains("deferRpgmMessageForLocalCache") -or
    -not $deploySrc.Contains("if(deferRpgmMessageForLocalCache(original,translated)){this._dsRpgmDeferredMessageStart=true; this._waitCount=Math.max(Number(this._waitCount)||0,1); return;}") -or
    -not $deploySrc.Contains("scheduleActiveMessageCachePoll") -or
    -not $deploySrc.Contains("ACTIVE_MESSAGE_POLL_WINDOW_MS") -or
    -not $deploySrc.Contains("active-message-cache-poll-timeout") -or
    -not $deploySrc.Contains("refreshActiveRpgmMessage") -or
    -not $deploySrc.Contains("translateMessageLines(original,true)") -or
    -not $deploySrc.Contains("refresh-active-message") -or
    -not $deploySrc.Contains("_dsRpgmDeferredMessageStart=true") -or
    -not $deploySrc.Contains("if(win._dsRpgmDeferredMessageStart&&typeof win.startMessage==='function')") -or
    $deploySrc.Contains("MESSAGE_FIRST_DRAW_WAIT_MS")) {
    throw "RPG Maker dialogue may gate only on an asynchronous local-cache read, must yield MZ's synchronous message loop, and must resume a deferred first message or update an active source page"
}
if (-not $deploySrc.Contains("primeGalvQuestCache") -or
    -not $deploySrc.Contains("MAX_QUEST_PRIME=128") -or
    -not $deploySrc.Contains("walkSceneWindows") -or
    -not $deploySrc.Contains("walkSceneOwnedWindows") -or
    -not $deploySrc.Contains("walkSceneOwnedWindows(scene,seen,state)") -or
    -not $deploySrc.Contains("list-scene-owned-windows") -or
    -not $deploySrc.Contains("read-scene-owned-window")) {
    throw "RPG Maker must prime current Galv quest text and refresh nested custom windows on first open"
}
if (-not $deploySrc.Contains("QUEST_PRIME_BATCH=16") -or
    -not $deploySrc.Contains("keys.slice(start,start+QUEST_PRIME_BATCH)") -or
    -not $deploySrc.Contains("if(changed) requestWindowRefresh()")) {
    throw "RPG Maker Galv quest prime must publish concurrent small batches incrementally"
}
if (-not $deploySrc.Contains("installCopiedDrawTextExHooks") -or
    -not $deploySrc.Contains("translateConvertedRpgmText") -or
    -not $deploySrc.Contains("_dsRpgmTranslationTarget")) {
    throw "RPG Maker must bridge copied drawTextEx renderers such as SRD HUD Maker without losing color controls"
}
if (-not $deploySrc.Contains("installTextExAutoWrapHook") -or
    -not $deploySrc.Contains("processNormalCharacter") -or
    -not $deploySrc.Contains("processNewLine(textState)") -or
    -not $deploySrc.Contains("textState.index=index") -or
    -not $deploySrc.Contains("withRpgmTextExWrap") -or
    -not $deploySrc.Contains("_dsRpgmTranslatedCjkText") -or
    -not $deploySrc.Contains("__deepSeekRpgmDiagnostics") -or
    -not $deploySrc.Contains("while(records.length>32)")) {
    throw "RPG Maker drawTextEx must wrap translated CJK at the renderer width and retain bounded diagnostics"
}
if (-not $deploySrc.Contains("requestLocalCacheKeys") -or
    -not $deploySrc.Contains("CACHE_LOOKUP_URL") -or
    -not $deploySrc.Contains("sendRpgmJson(CACHE_LOOKUP_URL,{texts:batch},1500") -or
    $deploySrc.Contains("CACHE_LOOKUP_URL,false") -or
    -not $deploySrc.Contains("[plan.joined].concat(plan.candidates)")) {
    throw "RPG Maker HUD and message text must use the local cache-only endpoint without blocking the render thread"
}
if (-not $deploySrc.Contains("installMessageRuntimeHooks") -or
    -not $deploySrc.Contains("installPluginLoadHook") -or
    -not $deploySrc.Contains("_dsRpgmPluginLoadHook") -or
    -not $deploySrc.Contains("script.addEventListener('load'")) {
    throw "RPG Maker hooks must reinstall at the plugin script load seam instead of relying only on fixed timers"
}
if (-not $deploySrc.Contains("installWindowOpenRefreshHook") -or
    -not $deploySrc.Contains("Window_Base.prototype.updateOpen") -or
    -not $deploySrc.Contains("_dsRpgmOpenRefreshHook") -or
    -not $deploySrc.Contains("!wasOpen&&nowOpen&&canAutoRefreshWindow(this)") -or
    -not $deploySrc.Contains("refresh-window-open-transition")) {
    throw "RPG Maker cache hits that arrive while a window is closed must redraw on the engine-owned open transition"
}
if (-not $deploySrc.Contains("HUDManager.types") -or
    -not $deploySrc.Contains("entry.class")) {
    throw "RPG Maker must discover private SRD HUD Maker text constructors through its public type registry"
}
if (-not $deploySrc.Contains("_ds_install_character_call_hook") -or
    -not $deploySrc.Contains("ADVCharacter") -or
    -not $deploySrc.Contains("_ds_cls.__call__ = _ds_character_call") -or
    -not $deploySrc.Contains("_ds_protect_old_percent(_ds_translate(what, True))")) {
    throw "Ren'Py hook must translate direct Character.__call__ dynamic dialogue"
}
if ($deploySrc -notmatch 'down_until') {
    throw "Ren'Py hook must back off when the local server is unreachable"
}
if (-not $deploySrc.Contains("while _ds_time.time() < _ds_state['down_until']:") -or
    -not $deploySrc.Contains("wake.wait(max(0.05, _ds_state['down_until'] - _ds_time.time()))")) {
    throw "Ren'Py live worker must honor server backoff before draining another batch"
}
if ($deploySrc.Contains("_ds_pending_placeholder") -or
    $deploySrc.Contains("return _ds_pending_placeholder") -or
    $deploySrc.Contains("\\u3010\\u7ffb\\u8bd1\\u4e2d")) {
    throw "Ren'Py reading path must keep source text readable while background live batch heals the miss"
}
if ($deploySrc.Contains("_ds_http('/translate'") -or
    -not $deploySrc.Contains("def _ds_memo_get(s):") -or
    -not $deploySrc.Contains("out = _ds_memo_get(s)") -or
    -not $deploySrc.Contains("def _ds_note_pending_many(texts, priority=False):") -or
    -not $deploySrc.Contains("def _ds_note_pending(s, priority=False):")) {
    throw "Ren'Py render hooks must stay HTTP-free and consult process memory before queueing background work"
}
if (-not $deploySrc.Contains("_ds_translate(s, True)") -or
    -not $deploySrc.Contains("_ds_translate(what, True)") -or
    -not $deploySrc.Contains("_ds_http('/cache/lookup', {'texts': batch}, 0.5)") -or
    -not $deploySrc.Contains("_ds_http('/batch', {'texts': batch}, 12.0)") -or
    -not $deploySrc.Contains("def _ds_next_poll_delay():") -or
    -not $deploySrc.Contains("_ds_wake.wait()") -or
    -not $deploySrc.Contains("_ds_wake.wait(delay)") -or
    $deploySrc.Contains("_ds_wake.wait(0.05)") -or
    -not $deploySrc.Contains("batch = _ds_select_poll_batch(now, 96)")) {
    throw "Ren'Py reading path must use deadline-driven cache lookup without fixed idle polling"
}
$renpyLookupIndex = $deploySrc.IndexOf("_ds_http('/cache/lookup'")
$renpyLiveIndex = $deploySrc.IndexOf("_ds_http('/batch'")
if ($renpyLookupIndex -lt 0 -or $renpyLiveIndex -lt 0 -or $renpyLookupIndex -gt $renpyLiveIndex -or
    -not $deploySrc.Contains("_ds_live_queue") -or
    -not $deploySrc.Contains("_ds_inflight") -or
    -not $deploySrc.Contains("def _ds_live_loop(queue, wake, batch_limit):")) {
    throw "Ren'Py must heal local-cache hits before dispatching misses on an independent live worker"
}
if (-not $deploySrc.Contains("_ds_install_menu_execute_hook") -or
    -not $deploySrc.Contains("_ds_note_pending_many(labels, True)") -or
    -not $deploySrc.Contains("_ds_fast_live_queue") -or
    -not $deploySrc.Contains("args=(_ds_fast_live_queue, _ds_fast_live_wake, 8)")) {
    throw "Ren'Py AST menus must prime complete visible choice groups on an independent small-batch worker"
}
if (-not $warmupSrc.Contains('!_wcsicmp(fd.cFileName, L"iron_deepseek.rpy")')) {
    throw "Ren'Py warmup must exclude the launcher-owned hook from game-text prefetch"
}
if ($deploySrc -notmatch '_ds_memo') {
    throw "Ren'Py UI translation must memoize lookups to avoid per-frame HTTP"
}
if ($deploySrc.Contains("_ds_memo.clear()") -or
    -not $deploySrc.Contains("len(_ds_memo) > 8000") -or
    -not $deploySrc.Contains("[:512]")) {
    throw "Ren'Py memoization must evict a bounded oldest slice instead of dropping every hot translation"
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
if ($deploySrc -notmatch 'len\(_ds_pending\) >= 1200') {
    throw "Ren'Py heal poller pending set must be bounded"
}
if ($deploySrc -notmatch "if _ds_pending\[k\] > 120") {
    throw "Ren'Py heal poller must abandon texts the server never translates"
}
if ($deploySrc -notmatch 'OverrideFont=\\n' -or $deploySrc -match 'OverrideFont=Microsoft YaHei') {
    throw "Unity IL2CPP must leave XUnity font override empty because the renderer plugin owns CJK fallback without stripped font factories"
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
if ($deploySrc -notmatch 'QueueWaitSeconds=30' -or $deploySrc -notmatch 'QueuePollIntervalSeconds=0\.2') {
    throw "Unity IL2CPP XUnity config must bound asynchronous local-cache polling"
}
if ($deploySrc -notmatch 'deploy_rpgm_font') {
    throw "RPG Maker deploy must ship a renderer-local CJK font"
}
if ($deploySrc -notmatch 'int\s+deploy_godot' -or
    $deploySrc -notmatch 'external translation patch pack' -or
    $deploySrc -notmatch 'original \.pck files are left unchanged') {
    throw "Godot deploy module must stay explicit about external patch-pack behavior"
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
if ($deploySrc -notmatch 'content_root,\s*L"fonts"' -or $deploySrc -notmatch 'ds_font\.ttf') {
    throw "RPG Maker CJK font must be deployed under the resolved content-root fonts directory"
}
$buildSrc = Get-Content -LiteralPath (Join-Path $root "build_native.bat") -Raw
$sourceReleaseSrc = Get-Content -LiteralPath (Join-Path $root "scripts\prepare_open_source_release.ps1") -Raw
$programReleaseSrc = Get-Content -LiteralPath (Join-Path $root "scripts\prepare_program_release.ps1") -Raw
$readmeSrc = Get-Content -LiteralPath (Join-Path $root "README.md") -Raw -Encoding UTF8
$userGuideSrc = Get-Content -LiteralPath (Join-Path $root "docs\USER_GUIDE.md") -Raw -Encoding UTF8
$openSourceReleaseDoc = Get-Content -LiteralPath (Join-Path $root "OPEN_SOURCE_RELEASE.md") -Raw -Encoding UTF8
$iconPng = Join-Path $root "assets\app_icon.png"
$iconIco = Join-Path $root "assets\app_icon.ico"
if (-not (Test-Path -LiteralPath $iconPng -PathType Leaf) -or
    -not (Test-Path -LiteralPath $iconIco -PathType Leaf)) {
    throw "launcher icon PNG and ICO assets must exist"
}
$iconBytes = [System.IO.File]::ReadAllBytes($iconIco)
if ($iconBytes.Length -lt 22 -or
    [BitConverter]::ToUInt16($iconBytes, 0) -ne 0 -or
    [BitConverter]::ToUInt16($iconBytes, 2) -ne 1) {
    throw "assets\app_icon.ico must be a valid Windows icon container"
}
$iconCount = [BitConverter]::ToUInt16($iconBytes, 4)
$iconSizes = @()
for ($i = 0; $i -lt $iconCount; $i++) {
    $entryOffset = 6 + (16 * $i)
    if ($entryOffset + 16 -gt $iconBytes.Length) {
        throw "assets\app_icon.ico has a truncated directory"
    }
    $iconSizes += $(if ($iconBytes[$entryOffset] -eq 0) { 256 } else { [int]$iconBytes[$entryOffset] })
}
foreach ($requiredSize in @(16, 32, 48, 256)) {
    if ($iconSizes -notcontains $requiredSize) {
        throw "assets\app_icon.ico is missing the ${requiredSize}x${requiredSize} frame"
    }
}
foreach ($doc in @($readmeSrc, $userGuideSrc, $openSourceReleaseDoc)) {
    if (-not $doc.Contains($productVersion)) {
        throw "release documentation must mention the current VERSION value"
    }
}
if ($sourceReleaseSrc -notmatch '\\_launcher_fixtures\\') {
    throw "source release script must exclude launcher test fixtures from public archives"
}
if ($sourceReleaseSrc -notmatch '"CONTEXT\.md"') {
    throw "source release script must include CONTEXT.md because AGENTS.md references it"
}
if ($sourceReleaseSrc -notmatch '"assets"') {
    throw "source release script must include the launcher icon assets"
}
foreach ($releaseScript in @($sourceReleaseSrc, $programReleaseSrc)) {
    if ($releaseScript -notmatch 'Invalid release version' -or
        $releaseScript -notmatch '\\A\[0-9A-Za-z\]') {
        throw "release scripts must reject path/control characters in version-derived output names"
    }
    if ($releaseScript -notmatch 'Assert-Under\s+\$stage') {
        throw "release scripts must validate the stage path before it exists"
    }
    if ($releaseScript -notmatch 'FileAttributes\]::ReparsePoint') {
        throw "release scripts must reject filesystem reparse traversal below build output roots"
    }
}
if ($buildSrc -notmatch 'APP_VERSION' -or $buildSrc -notmatch 'DS_TRANSLATOR_VERSION') {
    throw "build_native.bat must inject VERSION into the launcher footer at compile time"
}
if ($buildSrc -notmatch 'godot_warmup\.c' -or $buildSrc -notmatch 'godot_patch\.c') {
    throw "build_native.bat must compile the Godot warmup scanner and patch-pack modules"
}
if ($buildSrc -notmatch 'BepInExFlavor=5' -or $buildSrc -notmatch 'BepInExFlavor=6') {
    throw "build_native.bat must build both BepInEx 5 and BepInEx 6 UnityTranslator flavors"
}
if ($buildSrc -notmatch 'where gcc' -or $buildSrc -notmatch 'where windres') {
    throw "build_native.bat must support source-only users who install w64devkit on PATH"
}
if ($buildSrc -notmatch 'windres' -or $buildSrc -notmatch 'launcher_payloads\.rc' -or $buildSrc -notmatch 'launcher_payloads\.o') {
    throw "build_native.bat must embed first-party payload resources into the launcher"
}
if ($buildSrc -notmatch '1 ICON "%ROOT_RC%assets/app_icon\.ico"') {
    throw "build_native.bat must embed assets\app_icon.ico as the Windows application icon"
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
if ($buildSrc -notmatch 'DeepSeekUnityFontPatcher\.csproj' -or
    $buildSrc -notmatch 'DeepSeekUnityFontPatcher\.dll' -or
    $buildSrc -notmatch '205 RCDATA "%ROOT_RC%payloads/UnityTranslator/DeepSeekUnityFontPatcher\.dll"') {
    throw "build_native.bat must build and embed the stripped Unity Mono font metadata patcher"
}
if ($buildSrc -notmatch 'DeepSeekTMPFontFallback\.csproj') {
    throw "build_native.bat must wire the IL2CPP TMP font fallback source build when interop refs are available"
}
if ($buildSrc -notmatch 'XUnity\.AutoTranslator\.Plugin\.Core\.dll' -or
    $buildSrc -notmatch 'install_runtime_payloads\.ps1 -UnityIL2CPP') {
    throw "build_native.bat must check the XUnity runtime before compiling the IL2CPP endpoint from source"
}
if ($buildSrc -notmatch 'IL2CPP_INTEROP_DIR') {
    throw "DeepSeekTMPFontFallback build must document the required IL2CPP interop reference directory"
}
if ($buildSrc -notmatch 'UnityEngine\.TextRenderingModule\.dll') {
    throw "DeepSeekTMPFontFallback build must validate the UnityEngine.TextRenderingModule interop reference"
}
if ($deploySrc -notmatch 'install_stripped_unity_font_patcher' -or
    $deploySrc -notmatch 'BepInEx\\\\patchers\\\\DeepSeekUnityFontPatcher\.dll' -or
    $deploySrc -notmatch 'DeepSeekUnityFontPatcher\.dll\.dst-owned') {
    throw "stripped Unity Mono deploy must install the first-party font patcher with ownership metadata"
}
$selfUpdateSrc = Get-Content -LiteralPath (Join-Path $root "native\src\launcher\self_update.c") -Raw
if ($selfUpdateSrc -notmatch 'IDR_PAYLOAD_UNITY_FONT_PATCHER\s+205' -or
    $selfUpdateSrc -notmatch 'payloads\\\\UnityTranslator\\\\DeepSeekUnityFontPatcher\.dll') {
    throw "launcher self-update must synchronize the embedded stripped Unity Mono font patcher"
}
if ($buildSrc -notmatch 'launcher_build\.exe' -or
    $buildSrc -notmatch 'Move-Item -LiteralPath \$env:LAUNCHER_TMP' -or
    $buildSrc -notmatch 'Test-Path -LiteralPath \$legacy' -or
    $buildSrc -match '-o "%ROOT%DeepSeekTranslator\.exe"') {
    throw "build_native.bat must leave the root launcher artifact named ds娓告垙缈昏瘧鍣?exe"
}
if ([regex]::Matches($buildSrc, '-Wall -Wextra -Werror').Count -lt 2) {
    throw "native server and launcher builds must fail on compiler warnings"
}
$tmpFallbackProj = Get-Content -LiteralPath (Join-Path $root "payloads\UnityIL2CPP\DeepSeekTMPFontFallback\src\DeepSeekTMPFontFallback.csproj") -Raw
if ($tmpFallbackProj -notmatch '0Harmony') {
    throw "DeepSeekTMPFontFallback project must reference Harmony for first-frame TMP text patching"
}
$xunityEndpointSrc = Get-Content -LiteralPath (Join-Path $root "payloads\UnityIL2CPP\DeepSeekXUnityTranslator\src\DeepSeekTranslateEndpoint.cs") -Raw
if ($xunityEndpointSrc -notmatch 'private int _maxConcurrency = 8') {
    throw "DeepSeek XUnity endpoint should default to 8 local requests for faster cache-hit fanout"
}
if ($deploySrc -notmatch 'MaxCharactersPerTranslation=2500') {
    throw "Unity IL2CPP deploy must use XUnity's supported 2500-character maximum instead of the old 400-character limit"
}
if ($xunityEndpointSrc -notmatch 'ProtectMixedCjkForRequest' -or
    $xunityEndpointSrc -notmatch 'TryRestoreMixedCjk' -or
    $xunityEndpointSrc -notmatch 'protected CJK token') {
    throw "DeepSeek XUnity endpoint must protect translated CJK prefixes while translating newly appended English"
}
if ($xunityEndpointSrc -notmatch 'TranslationDelay\", 0\.1f') {
    throw "DeepSeek XUnity endpoint should default to XUnity's minimum accepted delay"
}
if ($xunityEndpointSrc -notmatch 'if \(value < 0\.1f\) return 0\.1f;') {
    throw "DeepSeek XUnity endpoint must clamp configured delay to XUnity's accepted minimum"
}
if ($xunityEndpointSrc -notmatch 'public override IEnumerator OnBeforeTranslate\(IHttpTranslationContext context\)' -or
    $xunityEndpointSrc -notmatch 'GetSupportedEnumerator' -or
    $xunityEndpointSrc -notmatch 'Stopwatch\.StartNew\(\)') {
    throw "DeepSeek XUnity endpoint must wait asynchronously for queued local-cache translations"
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
if ($tmpFallbackSrc -notmatch '1\.2\.20') {
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
if ($tmpFallbackSrc -notmatch 'SteadyNormalizeInterval = 2\.0f' -or $tmpFallbackSrc -notmatch 'ShouldUseFastNormalizeScan') {
    throw "IL2CPP TMP font fallback must slow the fallback scan once the setter patch is installed"
}
if ($tmpFallbackSrc -notmatch 'bundle\.LoadAllAssets\(tmpFontType\)' -or $tmpFallbackSrc -notmatch 'AsTmpFontAsset') {
    throw "IL2CPP TMP font fallback must use native typed AssetBundle overloads and rewrap base IL2CPP objects"
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
if ($selfUpdateSrc -notmatch 'FindResourceW' -or $selfUpdateSrc -notmatch 'move_file_safe' -or $selfUpdateSrc -notmatch 'file_matches_bytes') {
    throw "embedded payload sync must read Win32 resources, compare bytes, and replace atomically"
}
if ($selfUpdateSrc -match 'Newtonsoft\.Json' -or $selfUpdateSrc -match 'BepInExRuntime' -or $selfUpdateSrc -match 'XUnityAutoTranslator' -or $selfUpdateSrc -match 'TMPFontAssetBundles') {
    throw "embedded payload sync must not bundle third-party runtime payloads"
}
$programReleaseSrc = Get-Content -LiteralPath (Join-Path $root "scripts\prepare_program_release.ps1") -Raw
if ($programReleaseSrc -notmatch 'singleExePath' -or
    $programReleaseSrc -notmatch 'Copy-ReleaseFile "\$DsName\.exe" "\$DsName\.exe"' -or
    $programReleaseSrc -notmatch '0x6e38' -or
    $programReleaseSrc -notmatch '0x620f') {
    throw "program release script must produce a standalone ds娓告垙缈昏瘧鍣?exe asset"
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
if ($deploySrc -notmatch 'int restore_game\(' -or
    $deploySrc -notmatch 'strip_owned_rpgm_hook_tags' -or
    $deploySrc -notmatch '\.dst-owned' -or
    $deploySrc -notmatch '\.dst-installed-by-ds' -or
    $deploySrc -notmatch 'files_equal\(installed, payload\)') {
    throw "restore-game must use engine-specific ownership checks and preserve user-modified Unity files"
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

function New-GodotEmbeddedExe($path) {
    $parent = Split-Path -Parent $path
    New-Item -ItemType Directory -Force -Path $parent | Out-Null
    $bytes = New-Object byte[] 768
    $bytes[0] = 0x4d
    $bytes[1] = 0x5a
    [BitConverter]::GetBytes([int32]0x80).CopyTo($bytes, 0x3c)
    $bytes[0x80] = 0x50
    $bytes[0x81] = 0x45
    [BitConverter]::GetBytes([uint16]0x8664).CopyTo($bytes, 0x84)
    [BitConverter]::GetBytes([uint16]1).CopyTo($bytes, 0x86)
    [BitConverter]::GetBytes([uint16]0xf0).CopyTo($bytes, 0x94)
    [Text.Encoding]::ASCII.GetBytes("pck").CopyTo($bytes, 0x188)
    [Text.Encoding]::ASCII.GetBytes("GDPC").CopyTo($bytes, $bytes.Length - 4)
    [IO.File]::WriteAllBytes($path, $bytes)
}

function New-PeFile($path, [uint16]$machine) {
    $parent = Split-Path -Parent $path
    New-Item -ItemType Directory -Force -Path $parent | Out-Null
    $bytes = New-Object byte[] 512
    $bytes[0] = 0x4d
    $bytes[1] = 0x5a
    [BitConverter]::GetBytes([int32]0x80).CopyTo($bytes, 0x3c)
    $bytes[0x80] = 0x50
    $bytes[0x81] = 0x45
    [BitConverter]::GetBytes($machine).CopyTo($bytes, 0x84)
    [IO.File]::WriteAllBytes($path, $bytes)
}

try {
    New-File (Join-Path $fixtures "renpy\game\script.rpy") "label start:`n    `"Hello`""
    New-File (Join-Path $fixtures "renpy\game\iron_deepseek.rpyc") "stale launcher hook bytecode"
    New-File (Join-Path $fixtures "rpgm\www\index.html") "<html><body><script type=`"text/javascript`" src=`"js/plugins.js`"></script><script>window.assetName='hook_rpgm_mv.js';</script><script type=`"text/javascript`" src=`"js/main.js`"></script><script type=`"text/javascript`" src=`"js/hook_rpgm_mv.js`"></script></body></html>"
    New-Item -ItemType Directory -Force -Path (Join-Path $fixtures "rpgm\www\js") | Out-Null
    New-File (Join-Path $fixtures "rpgm\www\js\main.js") "window.onload = function() {};"
    New-File (Join-Path $fixtures "rpgm\www\js\rpg_core.js") "function Rectangle() {}"
    New-File (Join-Path $fixtures "rpgm\www\data\System.json") '{"gameTitle":"RPG Maker MV"}'
    New-File (Join-Path $fixtures "rpgm_flat\index.html") "<html><body><script type=`"text/javascript`" src=`"js/main.js`"></script></body></html>"
    New-File (Join-Path $fixtures "rpgm_flat\package.json") '{"name":"rmmz-game","main":"index.html"}'
    New-File (Join-Path $fixtures "rpgm_flat\js\rmmz_core.js") "function Rectangle() {}"
    New-File (Join-Path $fixtures "rpgm_flat\js\main.js") "window.onload = function() {};"
    New-File (Join-Path $fixtures "rpgm_flat\data\System.json") '{"gameTitle":"Flat RPG Maker MZ"}'
    New-File (Join-Path $fixtures "rpgm_flat\data\Map001.json") '{"events":[{"pages":[{"list":[{"code":401,"parameters":["Flat layout dialogue from Map001."]}]}]}]}'
    New-File (Join-Path $fixtures "rpgm_flat\game_messages.csv") "id;source;English`n1;`"Texto con delimitador; sigue`";`"Localized CSV dialogue must be prefetched before first display.`""
    New-File (Join-Path $fixtures "rpgm_flat_mv\index.html") "<html><body><script type=`"text/javascript`" src=`"js/main.js`"></script></body></html>"
    New-File (Join-Path $fixtures "rpgm_flat_mv\js\rpg_core.js") "function Rectangle() {}"
    New-File (Join-Path $fixtures "rpgm_flat_mv\js\main.js") "window.onload = function() {};"
    New-File (Join-Path $fixtures "rpgm_flat_mv\data\System.json") '{"gameTitle":"Flat RPG Maker MV"}'
    New-File (Join-Path $fixtures "generic_nw\index.html") "<html><body><script src=`"js/main.js`"></script></body></html>"
    New-File (Join-Path $fixtures "generic_nw\js\main.js") "console.log('plain NW.js app');"
    New-Item -ItemType Directory -Force -Path (Join-Path $fixtures "rpgm_fail\www\js") | Out-Null
    New-Item -ItemType Directory -Force -Path (Join-Path $fixtures "rpgm_fail\www\index.html") | Out-Null
    # rpgm_fail keeps a complete marker set so detection still resolves the www
    # content root; only index.html is an unreadable directory, which is what
    # must make deploy/restore fail.
    New-File (Join-Path $fixtures "rpgm_fail\www\js\main.js") "window.onload = function() {};"
    New-File (Join-Path $fixtures "rpgm_fail\www\js\rpg_core.js") "function Rectangle() {}"
    New-File (Join-Path $fixtures "rpgm_fail\www\data\System.json") '{"gameTitle":"Unreadable index RPGM"}'
    New-File (Join-Path $fixtures "godot\demo.pck") "Godot PCK`0Start your journey."
    New-GodotEmbeddedExe (Join-Path $fixtures "godot_embedded\EmbeddedGodot.exe")
    New-File (Join-Path $fixtures "godot_embedded\Config.exe") "not the game executable"
    New-File (Join-Path $fixtures "godot_embedded\dst_godot_patch.exe") "launcher-owned Godot patch executable"
    New-File (Join-Path $fixtures "godot_embedded\dst_godot_patch.pck") "launcher-owned Godot patch pack"

    New-File (Join-Path $fixtures "exe_select\Config.exe") "configuration helper"
    New-File (Join-Path $fixtures "exe_select\RealGame.exe") "game executable"
    New-Item -ItemType Directory -Force -Path (Join-Path $fixtures "exe_select\RealGame_Data") | Out-Null

    New-Item -ItemType Directory -Force -Path (Join-Path $fixtures "unity_mono\Example_Data") | Out-Null
    New-Item -ItemType Directory -Force -Path (Join-Path $fixtures "unity_mono\BepInEx\plugins") | Out-Null
    New-File (Join-Path $fixtures "unity_mono\mods.pck") "Unity-side pack must not override _Data detection."
    New-GodotEmbeddedExe (Join-Path $fixtures "unity_mono\GodotLikeHelper.exe")

    New-Item -ItemType Directory -Force -Path (Join-Path $fixtures "unity_stripped\Example_Data\Managed") | Out-Null
    New-File (Join-Path $fixtures "unity_stripped\Example_Data\globalgamemanagers") "2022.3.62f2"
    $strippedMscorlib = Join-Path $fixtures "unity_stripped\Example_Data\Managed\mscorlib.dll"
    [IO.File]::WriteAllBytes($strippedMscorlib, [Text.Encoding]::ASCII.GetBytes("MZ mscorlib System.IO ReadAllText deliberately lacks the BepInEx-required writer"))
    New-PeFile (Join-Path $fixtures "unity_stripped\UnityPlayer.dll") 0x8664

    New-Item -ItemType Directory -Force -Path (Join-Path $fixtures "unity6_mono\Example_Data") | Out-Null
    New-File (Join-Path $fixtures "unity6_mono\Example_Data\globalgamemanagers") "6000.3.8f1"
    New-PeFile (Join-Path $fixtures "unity6_mono\UnityPlayer.dll") 0x8664

    New-Item -ItemType Directory -Force -Path (Join-Path $fixtures "unity6_mono_x86\Example_Data") | Out-Null
    New-File (Join-Path $fixtures "unity6_mono_x86\Example_Data\globalgamemanagers") "6000.3.8f1"
    New-PeFile (Join-Path $fixtures "unity6_mono_x86\UnityPlayer.dll") 0x014c
    New-Item -ItemType Directory -Force -Path (Join-Path $fixtures "unity6_mono_x86\BepInEx\core") | Out-Null
    Copy-Item -LiteralPath (Join-Path $root "payloads\UnityMonoRuntime6\winhttp.dll") -Destination (Join-Path $fixtures "unity6_mono_x86\winhttp.dll")
    Copy-Item -LiteralPath (Join-Path $root "payloads\UnityMonoRuntime6\BepInEx\core\BepInEx.Unity.Mono.dll") -Destination (Join-Path $fixtures "unity6_mono_x86\BepInEx\core\BepInEx.Unity.Mono.dll")

    New-Item -ItemType Directory -Force -Path (Join-Path $fixtures "unity_il2cpp\Example_Data\il2cpp_data") | Out-Null
    New-Item -ItemType Directory -Force -Path (Join-Path $fixtures "unity_il2cpp\BepInEx\plugins") | Out-Null
    New-Item -ItemType Directory -Force -Path (Join-Path $fixtures "unity_il2cpp\BepInEx\core") | Out-Null
    Copy-Item -LiteralPath (Join-Path $root "payloads\UnityTranslator\UnityTranslator.dll") -Destination (Join-Path $fixtures "unity_il2cpp\BepInEx\plugins\UnityTranslator.dll")
    New-File (Join-Path $fixtures "unity_il2cpp\BepInEx\core\Il2Cppmscorlib.dll") "legacy-interop"
    New-File (Join-Path $fixtures "unity_il2cpp\BepInEx\config\AutoTranslatorConfig.ini") "[User]`nKeep=True"
    New-File (Join-Path $fixtures "unity_il2cpp\GameAssembly.dll") "il2cpp"
    New-File (Join-Path $fixtures "unity_il2cpp\doorstop_config.ini") "[UnityDoorstop]`nenabled=true"

    New-Item -ItemType Directory -Force -Path (Join-Path $fixtures "unity_custom\Example_Data\il2cpp_data") | Out-Null
    New-Item -ItemType Directory -Force -Path (Join-Path $fixtures "unity_custom\BepInEx\plugins") | Out-Null
    New-File (Join-Path $fixtures "unity_custom\BepInEx\plugins\UnityTranslator.dll") "custom-il2cpp-plugin"
    New-File (Join-Path $fixtures "unity_custom\GameAssembly.dll") "il2cpp"

    gcc -std=c17 -O2 -Wall -Wextra -Werror -municode -D_CRT_SECURE_NO_WARNINGS `
        -I"$root\native\src\launcher" `
        "$PSScriptRoot\launcher_probe.c" `
        "$root\native\src\launcher\engine.c" `
        "$root\native\src\launcher\fsutil.c" `
        "$root\native\src\launcher\deploy.c" `
        -lgdi32 -lmsimg32 -o "$probeExe"
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

    & $probeExe $root $fixtures
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

    New-Item -ItemType Directory -Force -Path $reparseOutside | Out-Null
    $reparseSource = Join-Path $fixtures "reparse_source.bin"
    New-File $reparseSource "safe source"
    New-Item -ItemType Junction -Path $reparseLink -Target $reparseOutside | Out-Null
    & $probeExe --fsutil-reparse $reparseSource $reparseLink
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
    if ((Test-Path -LiteralPath (Join-Path $reparseOutside "created-by-launcher")) -or
        (Test-Path -LiteralPath (Join-Path $reparseOutside "payload.bin"))) {
        throw "launcher filesystem helpers must not write through directory reparse points"
    }
    [System.IO.Directory]::Delete($reparseLink)

    New-Item -ItemType Directory -Force -Path $restoreReparseRoot | Out-Null
    New-File (Join-Path $reparseOutside "iron_deepseek.rpy") "outside sentinel"
    New-Item -ItemType Junction -Path $restoreReparseLink -Target $reparseOutside | Out-Null
    & $probeExe --restore-reparse $restoreReparseRoot
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
    if (-not (Test-Path -LiteralPath (Join-Path $reparseOutside "iron_deepseek.rpy"))) {
        throw "restore must not delete launcher-named files through a directory reparse point"
    }
    [System.IO.Directory]::Delete($restoreReparseLink)
    if ($ProbeGamePath) {
        & $probeExe --detect $ProbeGamePath
        if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
    }
    if ($DeployRpgmPath) {
        & $probeExe --deploy-rpgm $DeployRpgmPath
        if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
    }

    $python = Get-Command python -ErrorAction SilentlyContinue
    if ($python) {
        & $python.Source $renpyHookProbe (Join-Path $fixtures "renpy\game\iron_deepseek.rpy")
        if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
    }

    $rpgmIndex = Get-Content -Raw (Join-Path $fixtures "rpgm\www\index.html")
    $pluginsPos = $rpgmIndex.IndexOf("js/plugins.js")
    $hookRef = 'src="js/hook_rpgm_mv.js"'
    $hookPos = $rpgmIndex.IndexOf($hookRef)
    $mainPos = $rpgmIndex.IndexOf("js/main.js")
    if ($pluginsPos -lt 0 -or $hookPos -lt 0 -or $mainPos -lt 0 -or
        $hookPos -lt $pluginsPos -or $hookPos -gt $mainPos -or
        $hookPos -ne $rpgmIndex.LastIndexOf($hookRef)) {
        Write-Error "RPGM hook should be injected once after plugins.js and before main.js"
        exit 1
    }
    if (-not $rpgmIndex.Contains("window.assetName='hook_rpgm_mv.js'")) {
        Write-Error "RPGM deploy must not delete unrelated inline scripts that mention the hook filename"
        exit 1
    }
    $rpgmBackup = Join-Path $fixtures "rpgm\www\index.html.dst-backup"
    if (-not (Test-Path -LiteralPath $rpgmBackup)) {
        Write-Error "RPGM deploy must keep a one-time backup before rewriting index.html"
        exit 1
    }

    gcc -std=c17 -O2 -Wall -Wextra -Werror -municode -D_CRT_SECURE_NO_WARNINGS `
        -I"$root\native\src\launcher" `
        "$PSScriptRoot\warmup_probe.c" `
        "$root\native\src\launcher\engine.c" `
        -lwinhttp -o "$warmupProbeExe"
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

    & $warmupProbeExe (Join-Path $fixtures "rpgm_flat")
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

    gcc -std=c17 -O2 -municode -D_CRT_SECURE_NO_WARNINGS `
        -I"$root\native\src\launcher" `
        "$PSScriptRoot\godot_patch_probe.c" `
        "$root\native\src\launcher\godot_patch.c" `
        "$root\native\src\launcher\engine.c" `
        "$root\native\src\launcher\fsutil.c" `
        -lwinhttp -lgdi32 -o "$godotPatchProbeExe"
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

    & $godotPatchProbeExe $fixtures
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

    gcc -std=c17 -O2 -Wall -Wextra -Werror `
        -I"$root\native\src\launcher" `
        "$PSScriptRoot\godot_cli_probe.c" `
        "$root\native\src\launcher\godot_probe.c" `
        -o "$godotCliProbeExe"
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

    & $godotCliProbeExe
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

    & node $rpgmHookProbe (Join-Path $fixtures "rpgm\www\js\hook_rpgm_mv.js")
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

    gcc -std=c17 -O2 -Wall -Wextra -Werror -municode -D_CRT_SECURE_NO_WARNINGS `
        -I"$root\native\src\launcher" `
        "$PSScriptRoot\restore_probe.c" `
        "$root\native\src\launcher\engine.c" `
        "$root\native\src\launcher\fsutil.c" `
        "$root\native\src\launcher\deploy.c" `
        -lgdi32 -lmsimg32 -o "$restoreProbeExe"
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

    & $restoreProbeExe $root $fixtures
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
} finally {
    if (Test-Path -LiteralPath $reparseLink) {
        [System.IO.Directory]::Delete($reparseLink)
    }
    if (Test-Path -LiteralPath $restoreReparseLink) {
        [System.IO.Directory]::Delete($restoreReparseLink)
    }
    Remove-Item -LiteralPath $probeExe -Force -ErrorAction SilentlyContinue
    Remove-Item -LiteralPath $restoreProbeExe -Force -ErrorAction SilentlyContinue
    Remove-Item -LiteralPath $godotCliProbeExe -Force -ErrorAction SilentlyContinue
    Remove-Item -LiteralPath $warmupProbeExe -Force -ErrorAction SilentlyContinue
    Remove-Item -LiteralPath $godotPatchProbeExe -Force -ErrorAction SilentlyContinue
    Remove-Item -LiteralPath $fixtures -Recurse -Force -ErrorAction SilentlyContinue
    Remove-Item -LiteralPath $reparseOutside -Recurse -Force -ErrorAction SilentlyContinue
}
