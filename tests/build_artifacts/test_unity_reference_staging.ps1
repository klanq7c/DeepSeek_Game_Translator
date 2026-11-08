#Requires -Version 5.1
param([string]$HelperShell = 'powershell.exe')
$ErrorActionPreference = 'Stop'
$repo = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\..'))
$helper = [IO.File]::ReadAllText((Join-Path $repo 'scripts\stage_unity_references.ps1'))
$batch = [IO.File]::ReadAllText((Join-Path $repo 'scripts\build_managed_payloads.bat'))
$tempBase = [IO.Path]::GetFullPath([IO.Path]::GetTempPath()).TrimEnd('\')
$suite = Join-Path $tempBase ('dst_refs_' + [Guid]::NewGuid().ToString('N').Substring(0, 8))
$utf8 = [Text.UTF8Encoding]::new($true)
$script:checks = 0
$script:fixtures = 0
$passed = $false

function Check([bool]$Condition, [string]$Message) {
    if (-not $Condition) { throw $Message }
    $script:checks++
}
function Write-Text([string]$Path, [string]$Text) {
    [void][IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($Path))
    [IO.File]::WriteAllText($Path, $Text, $utf8)
}
function Invoke-Child([string]$Exe, [string]$Arguments, [hashtable]$Environment = @{}) {
    $start = [Diagnostics.ProcessStartInfo]::new($Exe, $Arguments)
    $start.UseShellExecute = $false
    $start.CreateNoWindow = $true
    $start.RedirectStandardOutput = $true
    $start.RedirectStandardError = $true
    $start.WorkingDirectory = $suite
    $start.EnvironmentVariables.Remove('UNITY_MANAGED_DIR')
    $start.EnvironmentVariables.Remove('IL2CPP_INTEROP_DIR')
    $start.EnvironmentVariables.Remove('PSModulePath')
    foreach ($name in $Environment.Keys) { $start.EnvironmentVariables[$name] = $Environment[$name] }
    $process = [Diagnostics.Process]::new()
    $process.StartInfo = $start
    try {
        [void]$process.Start()
        $stdout = $process.StandardOutput.ReadToEndAsync()
        $stderr = $process.StandardError.ReadToEndAsync()
        if (-not $process.WaitForExit(60000)) {
            $process.Kill()
            throw "Owned fixture process timed out: $Exe $Arguments"
        }
        return [pscustomobject]@{ Exit = $process.ExitCode; Out = $stdout.Result; Err = $stderr.Result }
    } finally { $process.Dispose() }
}
function New-Fixture([string]$Suffix = '') {
    $script:fixtures++
    $root = Join-Path $suite ('f' + $script:fixtures + $Suffix)
    $output = Join-Path $root 'payloads\UnityTranslator\src\bin\Release\net472'
    foreach ($name in @('UnityEngine.CoreModule.dll', 'UnityEngine.dll', 'Unity.TextMeshPro.dll')) {
        Write-Text (Join-Path $output $name) ('fixture-' + $name)
    }
    Write-Text (Join-Path $root 'scripts\stage_unity_references.ps1') $helper
    [IO.File]::WriteAllText((Join-Path $root 'build.cmd'), $batch, [Text.Encoding]::ASCII)
    Write-Text (Join-Path $root 'payloads\UnityTranslator\src\UnityTranslator.csproj') 'owned fake compiler input'
    Write-Text (Join-Path $root 'payloads\UnityTranslator\DeepSeekUnityFontPatcher.dll') 'fixture'
    Write-Text (Join-Path $root 'payloads\UnityTranslator\Newtonsoft.Json.dll') 'fixture'
    Write-Text (Join-Path $root 'scripts\verify_build_artifacts.ps1') @'
param([string]$RecordManagedPayload, [switch]$ManagedPayloadsOnly)
if ($RecordManagedPayload) { Add-Content -LiteralPath (Join-Path $env:ROOT 'stamps.txt') -Value $RecordManagedPayload }
'@
    return [pscustomobject]@{ Root=$root; Output=$output; Cache=(Join-Path $root 'build\unity-managed-refs') }
}
function Stage($Fixture) {
    Invoke-Child $HelperShell ('-NoProfile -ExecutionPolicy Bypass -File "' + $Fixture.Root + '\scripts\stage_unity_references.ps1" -RepoRoot "' + $Fixture.Root + '"')
}
function Key($Result) {
    Check ($Result.Exit -eq 0 -and $Result.Out.Trim() -cmatch '^[0-9A-F]{64}$' -and -not $Result.Err) "Staging failed or polluted stdout: exit=$($Result.Exit)`nstdout=$($Result.Out)`nstderr=$($Result.Err)"
    return $Result.Out.Trim()
}
function Build($Fixture, [hashtable]$Environment = @{}) {
    $Environment.ROOT = $Fixture.Root + '\'
    $Environment.PATH = (Join-Path $suite 'tools') + ';' + $env:PATH
    # 与 build_native.bat 的 CALL 入口一致，检查返回调用者的真实状态。
    Invoke-Child $env:ComSpec ('/d /c call "' + $Fixture.Root + '\build.cmd"') $Environment
}
function Reject($Result, [string]$Pattern) {
    Check ($Result.Exit -ne 0) "Failure returned success: stdout=$($Result.Out) stderr=$($Result.Err)"
    Check (($Result.Out + $Result.Err) -match $Pattern) "Missing diagnostic '$Pattern': stdout=$($Result.Out) stderr=$($Result.Err)"
}
function Snapshot-State($Fixture) {
    return ((Get-ChildItem -LiteralPath $Fixture.Cache -Recurse -Force | Sort-Object FullName | ForEach-Object {
        if ($_.PSIsContainer) { 'directory|' + $_.FullName.Substring($Fixture.Cache.Length) }
        else { $_.FullName.Substring($Fixture.Cache.Length) + '|' + $_.Length + '|' + (Get-FileHash -LiteralPath $_.FullName).Hash + '|' + $_.LastWriteTimeUtc.Ticks }
    }) -join [Environment]::NewLine)
}
function Rewrite-Helper($Fixture, [string]$Anchor, [string]$Replacement) {
    Check (($helper.Split(@($Anchor), [StringSplitOptions]::None)).Count -eq 2) "Fault injection seam moved: $Anchor"
    Write-Text (Join-Path $Fixture.Root 'scripts\stage_unity_references.ps1') ($helper.Replace($Anchor, $Replacement))
}
function Clear-Output($Fixture) {
    # 只删除本夹具已知直接文件，模拟 --no-incremental 清理。
    foreach ($file in Get-ChildItem -LiteralPath $Fixture.Output -File) { [IO.File]::Delete($file.FullName) }
}

[void][IO.Directory]::CreateDirectory((Join-Path $suite 'tools'))
try {
    $helperBytes = [IO.File]::ReadAllBytes((Join-Path $repo 'scripts\stage_unity_references.ps1'))
    Check ([BitConverter]::ToString($helperBytes[0..2]) -eq 'EF-BB-BF') 'Helper requires UTF-8 BOM for Windows PowerShell 5.1'
    Check (-not @($batch.ToCharArray() | Where-Object { [int]$_ -gt 127 }).Count) 'CMD recipe must remain ASCII'
    # 使用真正的 EXE，避免 CMD 替身在未 CALL 时转移批处理控制流。
    Write-Text (Join-Path $suite 'fake.cs') @'
using System;
using System.IO;
class FakeDotnet {
    static int Main(string[] args) {
        try {
            string root = Environment.GetEnvironmentVariable("ROOT");
            string refs = null, flavor = null;
            foreach (string arg in args) {
                if (arg.StartsWith("-p:UnityManagedDir=")) refs = arg.Substring("-p:UnityManagedDir=".Length);
                if (arg.StartsWith("-p:BepInExFlavor=")) flavor = arg.Substring("-p:BepInExFlavor=".Length);
            }
            if (refs == null || (flavor != "5" && flavor != "6")) throw new Exception("unexpected compiler arguments");
            string output = Path.Combine(root, @"payloads\UnityTranslator\src\bin\Release\net472");
            if (Path.GetFullPath(refs).TrimEnd('\\') == output) throw new Exception("references inside cleaned output");
            if (flavor == "5") foreach (string file in Directory.GetFiles(output)) File.Delete(file);
            if (!File.Exists(Path.Combine(refs, "UnityEngine.CoreModule.dll"))) throw new Exception("reference lost after clean");
            File.AppendAllText(Path.Combine(root, "calls.txt"), flavor + "|" + refs + "\n");
            if (flavor == "6" && Environment.GetEnvironmentVariable("FAIL_SECOND") == "1") {
                Console.Error.WriteLine("injected second-stage failure"); return 47;
            }
            if (flavor == "6") output += "-bepinex6";
            Directory.CreateDirectory(output);
            File.WriteAllText(Path.Combine(output, "UnityTranslator.dll"), "fixture-" + flavor);
            return 0;
        } catch (Exception error) { Console.Error.WriteLine(error); return 61; }
    }
}
'@
    Write-Text (Join-Path $suite 'compile.ps1') @'
$ErrorActionPreference = 'Stop'
Add-Type -Path (Join-Path $PSScriptRoot 'fake.cs') -OutputAssembly (Join-Path $PSScriptRoot 'tools\dotnet.exe') -OutputType ConsoleApplication
'@
    $compiled = Invoke-Child 'powershell.exe' ('-NoProfile -ExecutionPolicy Bypass -File "' + $suite + '\compile.ps1"')
    Check ($compiled.Exit -eq 0) "Fake compiler build failed: $($compiled | Out-String)"

    $f = New-Fixture
    $key = Key (Stage $f)
    $state = Snapshot-State $f
    Check ((Key (Stage $f)) -ceq $key) 'Identical inputs changed key'
    Check ((Snapshot-State $f) -ceq $state) 'Repeated staging rewrote or grew cache'
    foreach ($name in @('UnityEngine.CoreModule.dll', 'UnityEngine.dll', 'Unity.TextMeshPro.dll')) {
        Check ((Get-FileHash (Join-Path $f.Output $name)).Hash -eq (Get-FileHash (Join-Path $f.Cache "$key\$name")).Hash) "Reference bytes changed: $name"
    }
    Clear-Output $f
    Check ((Key (Stage $f)) -ceq $key) 'Output cleanup lost current references'
    $built = Build $f
    Check ($built.Exit -eq 0) "Real batch two-stage flow failed: $($built | Out-String)"
    Check ((Get-Content (Join-Path $f.Root 'calls.txt')).Count -eq 2) 'Both Mono builds did not execute'
    Check (((Get-Content (Join-Path $f.Root 'calls.txt')) -join ',') -ceq ("5|$($f.Cache)\$key,6|$($f.Cache)\$key")) 'Mono stages selected different references'
    Check (((Get-Content (Join-Path $f.Root 'stamps.txt')) -join ',') -ceq 'UnityMono5,UnityMono6') 'Both stamps did not execute after compiler returned'
    Check ((Snapshot-State $f) -ceq $state) 'Two-stage build changed references'
    Reject (Build $f @{ FAIL_SECOND='1' }) 'injected second-stage failure'
    Check ((Get-Content (Join-Path $f.Root 'stamps.txt')).Count -eq 2) 'Failed second build published stamps'
    Check ((Build $f).Exit -eq 0) 'Next build failed after second-stage failure'
    Check ((Snapshot-State $f) -ceq $state) 'Recovery changed references'
    Write-Host '[PASS] byte identity, no cache growth/rewrites, both Mono stages, failed-stage recovery'

    # 真实批处理入口必须同时判定 helper 的退出码与输出协议。
    foreach ($body in @('Write-Output $key; exit 23', 'Write-Output "noise"; Write-Output $key',
        'Write-Output "INVALID"', 'Write-Output ("G" * 64)', 'exit 0')) {
        $f = New-Fixture
        [void](Key (Stage $f))
        Rewrite-Helper $f 'Write-Output $key' $body
        Reject (Build $f) 'Unity reference staging failed'
        Check (-not (Test-Path (Join-Path $f.Root 'calls.txt'))) 'Compiler ran after invalid helper result'
    }
    $f = New-Fixture
    $explicit = Join-Path $f.Root 'explicit'
    Write-Text (Join-Path $explicit 'UnityEngine.CoreModule.dll') 'explicit'
    $saved = Join-Path $f.Root 'payloads\UnityTranslator\src\UnityManagedRefs'
    Write-Text (Join-Path $saved 'UnityEngine.CoreModule.dll') 'saved'
    Check ((Build $f @{ UNITY_MANAGED_DIR=$explicit }).Exit -eq 0) 'Explicit references failed'
    Check ((Get-Content (Join-Path $f.Root 'calls.txt'))[0].EndsWith('|' + $explicit)) 'Explicit references lost priority'
    Check ((Build $f).Exit -eq 0) 'Saved references failed'
    Check ((Get-Content (Join-Path $f.Root 'calls.txt'))[2].EndsWith('|' + $saved)) 'Saved references lost priority'
    Reject (Build $f @{ UNITY_MANAGED_DIR=(Join-Path $f.Root 'missing') }) 'Invalid UNITY_MANAGED_DIR'
    Check (-not (Test-Path $f.Cache)) 'Explicit/saved references unexpectedly staged'
    Write-Host '[PASS] helper status/stdout protocol and explicit/saved reference priority'

    $f = New-Fixture
    $keys = @((Key (Stage $f)))
    Write-Text (Join-Path $f.Output 'UnityEngine.dll') 'different bytes'
    $keys += Key (Stage $f)
    Move-Item -LiteralPath (Join-Path $f.Output 'UnityEngine.dll') -Destination (Join-Path $f.Output 'UnityEngine.Other.dll')
    $keys += Key (Stage $f)
    [IO.File]::Delete((Join-Path $f.Output 'Unity.TextMeshPro.dll'))
    $keys += Key (Stage $f)
    Write-Text (Join-Path $f.Output 'Unity.TextMeshPro.dll') 'optional TMP'
    $keys += Key (Stage $f)
    Check (@($keys | Select-Object -Unique).Count -eq 5) 'Bytes/name/TMP membership not reflected in key'
    Check (@(Get-ChildItem $f.Cache -Directory).Count -eq 5) 'Unexpected version count'
    foreach ($key in $keys) { Check (Test-Path (Join-Path $f.Cache "$key\references.sha256")) 'Old version lost' }

    foreach ($kind in @('dll', 'manifest', 'extra-file', 'extra-directory', 'pointer', 'missing-current', 'source-directory', 'source-core-directory')) {
        $f = New-Fixture
        $key = Key (Stage $f)
        $snapshot = Join-Path $f.Cache $key
        switch ($kind) {
            'dll' { Write-Text (Join-Path $snapshot 'UnityEngine.dll') 'corrupt' }
            'manifest' { Write-Text (Join-Path $snapshot 'references.sha256') 'corrupt' }
            'extra-file' { Write-Text (Join-Path $snapshot 'unexpected.txt') 'extra' }
            'extra-directory' { [void][IO.Directory]::CreateDirectory((Join-Path $snapshot 'extra')) }
            'pointer' { Write-Text (Join-Path $f.Cache 'current.txt') '..\escape' }
            'missing-current' { Write-Text (Join-Path $f.Cache 'current.txt') ('A' * 64) }
            'source-directory' { Clear-Output $f; [IO.Directory]::Delete($f.Output); Write-Text $f.Output 'invalid directory' }
            'source-core-directory' { [IO.File]::Delete((Join-Path $f.Output 'UnityEngine.CoreModule.dll')); [void][IO.Directory]::CreateDirectory((Join-Path $f.Output 'UnityEngine.CoreModule.dll')) }
        }
        $before = Snapshot-State $f
        Reject (Stage $f) 'Unity reference|Cannot find path|Could not find|not a directory'
        Check ((Snapshot-State $f) -ceq $before) "Failure changed published cache: $kind"
    }
    Write-Host '[PASS] content addressing and corrupted input/cache rejection'

    $f = New-Fixture
    $key = Key (Stage $f)
    $state = Snapshot-State $f
    $lock = [IO.File]::Open((Join-Path $f.Cache 'stage.lock'), 'Open', 'ReadWrite', 'None')
    try { Reject (Stage $f) 'stage.lock|another process' } finally { $lock.Dispose() }
    Check ((Key (Stage $f)) -ceq $key) 'Lock not reusable after competing owner released'
    Check ((Snapshot-State $f) -ceq $state) 'Competing owner changed cache'

    foreach ($kind in @('copy', 'verification', 'source-change', 'publish', 'pointer')) {
        $f = New-Fixture
        $oldKey = Key (Stage $f)
        $oldSnapshot = [IO.File]::ReadAllText((Join-Path $f.Cache "$oldKey\references.sha256"))
        Write-Text (Join-Path $f.Output 'UnityEngine.dll') 'new version'
        $copy = '[IO.File]::Copy($file.Path, (Join-Path $pending $file.Name), $false)'
        $publish = '[IO.Directory]::Move($pending, $snapshot)'
        $holder = $null
        switch ($kind) {
            'copy' { Rewrite-Helper $f $copy 'throw "injected copy failure"' }
            'verification' { Rewrite-Helper $f $copy ($copy + '; [IO.File]::AppendAllText((Join-Path $pending $file.Name), "bad")') }
            'source-change' { Rewrite-Helper $f $copy ($copy + '; [IO.File]::AppendAllText($file.Path, "changed")') }
            'publish' { Rewrite-Helper $f $publish 'throw "injected publish failure"' }
            'pointer' { $holder = [IO.File]::Open((Join-Path $f.Cache 'current.txt'), 'Open', 'Read', 'Read') }
        }
        try {
            $failure = Stage $f
            Reject $failure 'injected|verification|changed during staging|pointer publish failed'
            if ($kind -eq 'pointer') {
                Check (($failure.Out + $failure.Err) -match 'type=System\.IO\.IOException\s+HResult=0x[0-9A-F]{8}') "Pointer failure lost exception type/HResult: stdout=$($failure.Out)`nstderr=$($failure.Err)"
            }
        }
        finally { if ($holder) { $holder.Dispose() } }
        Check ([IO.File]::ReadAllText((Join-Path $f.Cache 'current.txt')).Trim() -ceq $oldKey) "Failure changed current: $kind"
        Check ([IO.File]::ReadAllText((Join-Path $f.Cache "$oldKey\references.sha256")).Equals($oldSnapshot)) "Failure changed old snapshot: $kind"
        if ($kind -ne 'pointer') { Check (@(Get-ChildItem $f.Cache -Directory | Where-Object Name -Match '^[0-9A-F]{64}$').Count -eq 1) 'Partial version published' }
        Write-Text (Join-Path $f.Root 'scripts\stage_unity_references.ps1') $helper
        [void](Key (Stage $f))
    }
    Write-Host '[PASS] competing publisher, copy/validation/source-change/publish/pointer faults and lock recovery'

    # Junction 测试目标也归夹具所有；只移除链接本身，绝不递归遍历链接。
    foreach ($kind in @('source', 'cache', 'snapshot', 'source-file', 'manifest')) {
        $f = New-Fixture
        $key = Key (Stage $f)
        $target = Join-Path $f.Root ('link-target-' + $kind)
        switch ($kind) {
            'source' { $link = $f.Output }
            'cache' { $link = $f.Cache }
            'snapshot' { $link = Join-Path $f.Cache $key }
            'source-file' { $link = Join-Path $f.Output 'UnityEngine.CoreModule.dll' }
            'manifest' { $link = Join-Path $f.Cache "$key\references.sha256" }
        }
        if ($kind -in @('source-file', 'manifest')) {
            # 用目录重解析点覆盖文件名，测试无需开启 Windows 符号链接权限。
            [IO.File]::Move($link, $target + '.original')
            [void][IO.Directory]::CreateDirectory($target)
        } else { [IO.Directory]::Move($link, $target) }
        New-Item -ItemType Junction -Path $link -Target $target | Out-Null
        try { Reject (Stage $f) 'reparse point|not a regular file' }
        finally { [IO.Directory]::Delete($link) }
        Check (Test-Path $target) 'Link target was removed'
    }
    Write-Host '[PASS] source/cache/snapshot/file/manifest reparse ownership'
    $f = New-Fixture ' 中文 path'
    Check ((Build $f).Exit -eq 0) 'Unicode/spaced repository path failed'
    Clear-Output $f
    Check ((Build $f).Exit -eq 0) 'Unicode/spaced repository cache fallback failed'
    $f = New-Fixture
    Clear-Output $f
    Reject (Stage $f) 'No output references or verified Unity reference cache'
    Write-Host '[PASS] Unicode/spaced paths and missing dependency diagnostics'
    $passed = $true
    Write-Host "[PASS] Unity reference staging: $script:checks assertions ($HelperShell); real batch with owned compiler/filesystem boundaries"
} finally {
    if ($passed) {
        $resolved = [IO.Path]::GetFullPath($suite)
        if (-not $resolved.StartsWith($tempBase + '\dst_refs_', [StringComparison]::OrdinalIgnoreCase)) { throw 'Fixture cleanup boundary mismatch' }
        if (@(Get-ChildItem -LiteralPath $resolved -Recurse -Force | Where-Object { $_.Attributes -band [IO.FileAttributes]::ReparsePoint }).Count) { throw 'Reparse point retained for inspection' }
        Remove-Item -LiteralPath $resolved -Recurse -Force
    } else { Write-Host "[DIAGNOSTIC] Failed fixture retained: $suite" }
}
