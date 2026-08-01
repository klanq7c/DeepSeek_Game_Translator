$ErrorActionPreference = "Stop"
$repo = Split-Path -Parent $PSScriptRoot
$project = Join-Path $PSScriptRoot "UnityEndpointStateProbe\UnityEndpointStateProbe.csproj"
$endpoint = Join-Path $repo "payloads\UnityIL2CPP\DeepSeekXUnityTranslator\DeepSeekTranslate.dll"
$xunity = Join-Path $repo "payloads\UnityIL2CPP\XUnityAutoTranslator\BepInEx\plugins\XUnity.AutoTranslator\XUnity.AutoTranslator.Plugin.Core.dll"

foreach ($path in @($project, $endpoint, $xunity)) {
    if (-not (Test-Path -LiteralPath $path)) {
        throw "Unity endpoint state probe dependency is missing: $path"
    }
}

& dotnet run --project $project -c Release -- $endpoint $xunity
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
exit 0
