$ErrorActionPreference = "Stop"

function Assert-True {
    param([bool]$Condition, [string]$Message)
    if (-not $Condition) { throw $Message }
}

function Invoke-Verifier {
    param([string]$Script, [string[]]$Arguments)

    $output = @(& powershell.exe -ExecutionPolicy Bypass -NoProfile -File $Script @Arguments 2>&1)
    return [pscustomobject]@{
        ExitCode = $LASTEXITCODE
        Output = ($output -join "`n")
    }
}

$repo = Split-Path -Parent $PSScriptRoot
$tempRoot = Join-Path ([IO.Path]::GetTempPath()) ("ds artifact fingerprint " + [Guid]::NewGuid().ToString("N"))
try {
    $fakeScripts = Join-Path $tempRoot "scripts"
    New-Item -ItemType Directory -Path $fakeScripts -Force | Out-Null
    Copy-Item -LiteralPath (Join-Path $repo "scripts\verify_build_artifacts.ps1") `
        -Destination (Join-Path $fakeScripts "verify_build_artifacts.ps1")
    Copy-Item -LiteralPath (Join-Path $repo "build_native.bat") `
        -Destination (Join-Path $tempRoot "build_native.bat")
    Copy-Item -LiteralPath (Join-Path $repo "scripts\install_runtime_payloads.ps1") `
        -Destination (Join-Path $fakeScripts "install_runtime_payloads.ps1")
    $verifier = Join-Path $fakeScripts "verify_build_artifacts.ps1"

    $fixtureFiles = [ordered]@{
        "payloads\UnityTranslator\src\Translator.cs" = "mono-source"
        "payloads\UnityTranslator\src\UnityTranslator.csproj" = "mono-project"
        "payloads\UnityTranslator\src\FontPatcher\DeepSeekUnityFontPatcher.cs" = "patcher-source"
        "payloads\UnityTranslator\src\FontPatcher\DeepSeekUnityFontPatcher.csproj" = "patcher-project"
        "payloads\UnityTranslator\UnityTranslator.dll" = "mono5-output"
        "payloads\UnityTranslator\UnityTranslator.BepInEx6.dll" = "mono6-output"
        "payloads\UnityTranslator\DeepSeekUnityFontPatcher.dll" = "patcher-output"
        "payloads\UnityIL2CPP\DeepSeekXUnityTranslator\src\DeepSeekTranslateEndpoint.cs" = "endpoint-source"
        "payloads\UnityIL2CPP\DeepSeekXUnityTranslator\src\DeepSeekXUnityTranslator.csproj" = "endpoint-project"
        "payloads\UnityIL2CPP\DeepSeekXUnityTranslator\DeepSeekTranslate.dll" = "endpoint-output"
        "payloads\UnityIL2CPP\DeepSeekTMPFontFallback\src\TmpFontFallbackPlugin.cs" = "tmp-source"
        "payloads\UnityIL2CPP\DeepSeekTMPFontFallback\src\DeepSeekTMPFontFallback.csproj" = "tmp-project"
        "payloads\UnityIL2CPP\DeepSeekTMPFontFallback\BepInEx\plugins\DeepSeekTMPFontFallback\DeepSeekTMPFontFallback.dll" = "tmp-output"
    }
    foreach ($entry in $fixtureFiles.GetEnumerator()) {
        $path = Join-Path $tempRoot $entry.Key
        New-Item -ItemType Directory -Path (Split-Path -Parent $path) -Force | Out-Null
        [IO.File]::WriteAllText($path, $entry.Value, [Text.Encoding]::ASCII)
    }

    foreach ($name in @("UnityMono5", "UnityMono6", "UnityXUnity", "UnityTmpFallback", "UnityFontPatcher")) {
        $record = Invoke-Verifier $verifier @("-RecordManagedPayload", $name)
        Assert-True ($record.ExitCode -eq 0) "could not record $name fingerprint: $($record.Output)"
        Assert-True (Test-Path -LiteralPath (Join-Path $tempRoot "payloads\ManagedBuildStamps\$name.json")) `
            "$name fingerprint was not written to the managed stamp directory"
    }
    $baseline = Invoke-Verifier $verifier @("-ManagedPayloadsOnly")
    Assert-True ($baseline.ExitCode -eq 0) "fresh managed payload fixture failed verification: $($baseline.Output)"

    # Checkout, copy, and archive extraction can change mtimes without changing content.
    $managedSources = @(Get-ChildItem -LiteralPath (Join-Path $tempRoot "payloads") -File -Recurse | Where-Object {
        $_.Extension -in ".cs", ".csproj"
    })
    foreach ($source in $managedSources) {
        [IO.File]::SetLastWriteTimeUtc($source.FullName, ([DateTime]::UtcNow).AddHours(2))
    }
    $timestampOnly = Invoke-Verifier $verifier @("-ManagedPayloadsOnly")
    Assert-True ($timestampOnly.ExitCode -eq 0) `
        "unchanged managed payloads were rejected only because source mtimes were newer: $($timestampOnly.Output)"

    Add-Content -LiteralPath (Join-Path $tempRoot "build_native.bat") `
        -Value "`r`necho changed managed build recipe" -Encoding ASCII
    $recipeMismatch = Invoke-Verifier $verifier @("-ManagedPayloadsOnly")
    Assert-True ($recipeMismatch.ExitCode -ne 0) "changed build recipe unexpectedly passed the recorded fingerprints"
    Assert-True ($recipeMismatch.Output -match "build recipe fingerprint") `
        "build recipe mismatch diagnostic was not specific: $($recipeMismatch.Output)"

    foreach ($name in @("UnityMono5", "UnityMono6", "UnityXUnity", "UnityTmpFallback", "UnityFontPatcher")) {
        $record = Invoke-Verifier $verifier @("-RecordManagedPayload", $name)
        Assert-True ($record.ExitCode -eq 0) "could not refresh $name recipe fingerprint: $($record.Output)"
    }

    Add-Content -LiteralPath (Join-Path $fakeScripts "install_runtime_payloads.ps1") `
        -Value "`n# changed dependency acquisition recipe" -Encoding ASCII
    $dependencyRecipeMismatch = Invoke-Verifier $verifier @("-ManagedPayloadsOnly")
    Assert-True ($dependencyRecipeMismatch.ExitCode -ne 0) `
        "changed dependency acquisition recipe unexpectedly passed the recorded fingerprints"
    Assert-True ($dependencyRecipeMismatch.Output -match "build recipe fingerprint") `
        "dependency recipe mismatch diagnostic was not specific: $($dependencyRecipeMismatch.Output)"

    foreach ($name in @("UnityMono5", "UnityMono6", "UnityXUnity", "UnityTmpFallback", "UnityFontPatcher")) {
        $record = Invoke-Verifier $verifier @("-RecordManagedPayload", $name)
        Assert-True ($record.ExitCode -eq 0) "could not refresh $name dependency recipe fingerprint: $($record.Output)"
    }

    $tmpSource = Join-Path $tempRoot "payloads\UnityIL2CPP\DeepSeekTMPFontFallback\src\TmpFontFallbackPlugin.cs"
    $oldTimestamp = (Get-Item -LiteralPath $tmpSource).LastWriteTimeUtc.AddHours(-2)
    [IO.File]::WriteAllText($tmpSource, "changed-source-with-forged-old-timestamp", [Text.Encoding]::ASCII)
    (Get-Item -LiteralPath $tmpSource).LastWriteTimeUtc = $oldTimestamp
    $sourceMismatch = Invoke-Verifier $verifier @("-ManagedPayloadsOnly")
    Assert-True ($sourceMismatch.ExitCode -ne 0) "changed source passed because its timestamp was forged older"
    Assert-True ($sourceMismatch.Output -match "source fingerprint") "source mismatch diagnostic was not specific: $($sourceMismatch.Output)"

    $record = Invoke-Verifier $verifier @("-RecordManagedPayload", "UnityTmpFallback")
    Assert-True ($record.ExitCode -eq 0) "could not refresh TMP fixture fingerprint: $($record.Output)"
    $tmpOutput = Join-Path $tempRoot "payloads\UnityIL2CPP\DeepSeekTMPFontFallback\BepInEx\plugins\DeepSeekTMPFontFallback\DeepSeekTMPFontFallback.dll"
    $oldOutputTimestamp = (Get-Item -LiteralPath $tmpOutput).LastWriteTimeUtc.AddHours(-2)
    [IO.File]::WriteAllText($tmpOutput, "tampered-output-with-forged-old-timestamp", [Text.Encoding]::ASCII)
    (Get-Item -LiteralPath $tmpOutput).LastWriteTimeUtc = $oldOutputTimestamp
    $outputMismatch = Invoke-Verifier $verifier @("-ManagedPayloadsOnly")
    Assert-True ($outputMismatch.ExitCode -ne 0) "changed DLL passed because its timestamp was forged older"
    Assert-True ($outputMismatch.Output -match "output fingerprint") "output mismatch diagnostic was not specific: $($outputMismatch.Output)"

    Write-Host "Build artifact fingerprint tests PASS" -ForegroundColor Green
} finally {
    Remove-Item -LiteralPath $tempRoot -Recurse -Force -ErrorAction SilentlyContinue
}
