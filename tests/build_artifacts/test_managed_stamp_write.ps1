#Requires -Version 5.1
$ErrorActionPreference = 'Stop'
$workspace = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$tokens = $null
$parseErrors = $null
$ast = [Management.Automation.Language.Parser]::ParseFile(
    (Join-Path $workspace 'scripts\verify_build_artifacts.ps1'), [ref]$tokens, [ref]$parseErrors)
if ($parseErrors.Count) { throw ($parseErrors | Out-String) }
$wanted = @('Get-FileSha256', 'Get-SourceFingerprint', 'Write-ManagedPayloadStamp',
    'Get-SourceFiles', 'Get-LinkedCompileSources', 'Get-ManagedPayloadDefinitions', 'Assert-ManagedPayloadFingerprint')
$functions = $ast.FindAll({ param($node) $node -is [Management.Automation.Language.FunctionDefinitionAst] }, $false)
foreach ($name in $wanted) {
    $function = @($functions | Where-Object Name -eq $name)
    if ($function.Count -ne 1) { throw "Missing real function: $name" }
    Invoke-Expression $function[0].Extent.Text
}

$fixtureRoot = Join-Path ([IO.Path]::GetTempPath()) ('dst_stamp_write_' + [Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $fixtureRoot | Out-Null
$repo = $fixtureRoot
$source = Join-Path $fixtureRoot 'source.cs'
$output = Join-Path $fixtureRoot 'payload.dll'
$stampPath = Join-Path $fixtureRoot 'stamp.json'
[IO.File]::WriteAllText($source, 'source v1')
[IO.File]::WriteAllText($output, 'output v1')
$definition = [pscustomobject]@{
    Name = 'fixture'; Label = 'fixture payload'; SourceRoot = $fixtureRoot
    Inputs = @(Get-Item -LiteralPath $source); RecipeInputs = @(Get-Item -LiteralPath $source)
    Output = $output; Stamp = $stampPath
}

$mapping = $null
$view = $null
$mappedFile = $null
$exclusive = $null
$recipeRoot = Join-Path $fixtureRoot 'recipe'
try {
    Write-ManagedPayloadStamp $definition
    $original = [IO.File]::ReadAllText($stampPath)
    $originalTime = [IO.File]::GetLastWriteTimeUtc($stampPath)
    $mappedFile = [IO.File]::Open($stampPath, [IO.FileMode]::Open, [IO.FileAccess]::Read,
        [IO.FileShare]::ReadWrite -bor [IO.FileShare]::Delete)
    $mapping = [IO.MemoryMappedFiles.MemoryMappedFile]::CreateFromFile(
        $mappedFile, ('dst_stamp_' + [Guid]::NewGuid().ToString('N')), 0, [IO.MemoryMappedFiles.MemoryMappedFileAccess]::Read,
        [IO.HandleInheritability]::None, $true)
    $view = $mapping.CreateViewAccessor(0, 0, [IO.MemoryMappedFiles.MemoryMappedFileAccess]::Read)
    Write-ManagedPayloadStamp $definition
    if ([IO.File]::ReadAllText($stampPath) -cne $original -or
        [IO.File]::GetLastWriteTimeUtc($stampPath) -ne $originalTime) { throw 'Unchanged stamp was rewritten' }
    Write-Host '[PASS] identical stamp keeps bytes/mtime while a reader maps the file'

    [IO.File]::WriteAllText($output, 'output v2')
    Write-ManagedPayloadStamp $definition
    $updated = [IO.File]::ReadAllText($stampPath)
    if ($updated -ceq $original) { throw 'Changed stamp was not replaced' }
    $record = $updated | ConvertFrom-Json
    if ($record.outputSha256 -ne (Get-FileSha256 $output)) { throw 'Updated stamp has wrong output hash' }
    $originalBytes = [Text.Encoding]::UTF8.GetBytes($original)
    for ($i = 0; $i -lt $originalBytes.Length; $i++) {
        if ($view.ReadByte($i) -ne $originalBytes[$i]) { throw 'Existing reader view was corrupted' }
    }
    Write-Host '[PASS] changed stamp replaces atomically while an existing reader retains its view'
    $view.Dispose(); $view = $null
    $mapping.Dispose(); $mapping = $null
    $mappedFile.Dispose(); $mappedFile = $null

    $exclusive = [IO.File]::Open($stampPath, [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::Read)
    [IO.File]::WriteAllText($output, 'output v3')
    $failed = $false
    $diagnosticPath = Join-Path $fixtureRoot 'diagnostic.txt'
    try { Write-ManagedPayloadStamp $definition 2> $diagnosticPath } catch [IO.IOException] { $failed = $true }
    catch [Management.Automation.MethodInvocationException] {
        if ($_.Exception.InnerException -isnot [IO.IOException]) { throw }
        $failed = $true
    }
    if (-not $failed) { throw 'Exclusive-lock failure was hidden' }
    $diagnostic = Get-Content -LiteralPath $diagnosticPath -Raw
    if ($diagnostic -notmatch 'Managed fingerprint write failed' -or $diagnostic -notmatch 'stamp.json') {
        throw 'Write failure did not record operation and stamp path'
    }
    $exclusive.Dispose(); $exclusive = $null
    if ([IO.File]::ReadAllText($stampPath) -cne $updated) { throw 'Failed stamp write damaged previous record' }
    if (@(Get-ChildItem -LiteralPath $fixtureRoot -Filter '*.tmp').Count) { throw 'Temporary stamp leaked after failure' }
    Write-Host '[PASS] replacement-denying lock is diagnosed, propagates failure, and retains the previous stamp'
    Write-ManagedPayloadStamp $definition
    if (([IO.File]::ReadAllText($stampPath) | ConvertFrom-Json).outputSha256 -ne (Get-FileSha256 $output)) {
        throw 'Write after lock release did not recover'
    }
    Write-Host '[PASS] next explicit write succeeds after lock ownership is released'

    # 使用生产定义，而不是在夹具里手工假定 RecipeInputs 已包含新 helper。
    $repo = $recipeRoot
    [void][IO.Directory]::CreateDirectory((Join-Path $repo 'scripts'))
    foreach ($name in @('build_managed_payloads.bat', 'stage_unity_references.ps1', 'install_runtime_payloads.ps1')) {
        [IO.File]::Copy((Join-Path $workspace "scripts\$name"), (Join-Path $repo "scripts\$name"))
    }
    foreach ($relative in @('payloads\UnityTranslator\src', 'payloads\UnityTranslator\src\FontPatcher',
        'payloads\UnityIL2CPP\DeepSeekXUnityTranslator\src', 'payloads\UnityIL2CPP\DeepSeekTMPFontFallback\src')) {
        $directory = Join-Path $repo $relative
        [void][IO.Directory]::CreateDirectory($directory)
        [IO.File]::WriteAllText((Join-Path $directory 'Fixture.cs'), 'owned recipe fixture')
    }
    $definitions = @(Get-ManagedPayloadDefinitions)
    if ($definitions.Count -ne 5) { throw 'Managed recipe definition coverage changed' }
    foreach ($item in $definitions) {
        [void][IO.Directory]::CreateDirectory((Split-Path -Parent $item.Output))
        [IO.File]::WriteAllText($item.Output, 'owned output')
        Write-ManagedPayloadStamp $item
    }
    $script:errors = [Collections.Generic.List[string]]::new()
    foreach ($item in $definitions) { Assert-ManagedPayloadFingerprint $item @($item.Inputs) }
    if ($script:errors.Count) { throw ($script:errors -join '; ') }
    $helperPath = Join-Path $repo 'scripts\stage_unity_references.ps1'
    $helperTime = [IO.File]::GetLastWriteTimeUtc($helperPath)
    [IO.File]::AppendAllText($helperPath, "`n# recipe fixture change`n")
    [IO.File]::SetLastWriteTimeUtc($helperPath, $helperTime)
    foreach ($item in (Get-ManagedPayloadDefinitions)) { Assert-ManagedPayloadFingerprint $item @($item.Inputs) }
    if ($script:errors.Count -ne 5 -or @($script:errors | Where-Object { $_ -notmatch 'build recipe fingerprint does not match' }).Count) {
        throw "Helper change did not invalidate exactly five real recipes: $($script:errors -join '; ')"
    }
    Write-Host '[PASS] helper content change invalidates all five production recipe definitions despite unchanged mtime'
} finally {
    if ($exclusive) { $exclusive.Dispose() }
    if ($view) { $view.Dispose() }
    if ($mapping) { $mapping.Dispose() }
    if ($mappedFile) { $mappedFile.Dispose() }
    $tempRoot = [IO.Path]::GetFullPath([IO.Path]::GetTempPath()).TrimEnd('\') + '\'
    $resolved = [IO.Path]::GetFullPath($fixtureRoot)
    if (-not $resolved.StartsWith($tempRoot, [StringComparison]::OrdinalIgnoreCase) -or
        -not (Split-Path -Leaf $resolved).StartsWith('dst_stamp_write_')) { throw "Unexpected fixture root: $resolved" }
    if (Test-Path -LiteralPath $recipeRoot) {
        $resolvedRecipe = [IO.Path]::GetFullPath($recipeRoot)
        if ($resolvedRecipe -ne (Join-Path $resolved 'recipe') -or
            (Get-Item -LiteralPath $resolvedRecipe -Force).Attributes -band [IO.FileAttributes]::ReparsePoint -or
            @(Get-ChildItem -LiteralPath $resolvedRecipe -Recurse -Force | Where-Object { $_.Attributes -band [IO.FileAttributes]::ReparsePoint }).Count) {
            throw 'Unexpected recipe fixture path or reparse point; retained'
        }
        Remove-Item -LiteralPath $resolvedRecipe -Recurse -Force
    }
    foreach ($entry in (Get-ChildItem -LiteralPath $resolved -Force)) {
        if ($entry.PSIsContainer -or ($entry.Attributes -band [IO.FileAttributes]::ReparsePoint)) {
            throw "Unexpected fixture entry, retained: $($entry.FullName)"
        }
        Remove-Item -LiteralPath $entry.FullName -Force
    }
    Remove-Item -LiteralPath $resolved -Force
}
