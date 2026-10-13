param()
$ErrorActionPreference = 'Stop'
$repo = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\..'))
$project = Join-Path $PSScriptRoot 'unity_runtime_contract.csproj'
$output = Join-Path $repo 'build\unity_runtime_contract\bin'
& dotnet build $project -c Release -nologo -v quiet -p:OutputPath="$output\"
if ($LASTEXITCODE -ne 0) { throw 'Unity runtime contract test build failed' }
& dotnet (Join-Path $output 'dst_unity_runtime_contract.dll')
if ($LASTEXITCODE -ne 0) { throw 'Unity runtime contract failed' }
