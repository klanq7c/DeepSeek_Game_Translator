#Requires -Version 5.1
$ErrorActionPreference = 'Stop'
$workspace = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\..'))
$shell = (Get-Process -Id $PID).Path
$tempBase = [IO.Path]::GetFullPath([IO.Path]::GetTempPath()).TrimEnd('\')
$fixture = Join-Path $tempBase ('dst_release_scan_' + [Guid]::NewGuid().ToString('N').Substring(0, 8))
$utf8 = [Text.UTF8Encoding]::new($true)
$script:checks = 0
$passed = $false
$name = 'pass' + 'word'
$cRelative = 'native/src/launcher/api_config.c'
$psRelative = 'tests/launcher_parity/run_launcher_parity.ps1'
$cLine = @([IO.File]::ReadAllLines((Join-Path $workspace $cRelative)) | Where-Object {
    $_.Contains('GetWindowLongW(d.key, GWL_STYLE) & ES_PASSWORD')
})
if ($cLine.Count -ne 1) { throw 'Production password-style probe changed; review its ownership' }
$cLine = $cLine[0]
$psFlag = "'(?m)^${name}=1$'"
$psLine = @([IO.File]::ReadAllLines((Join-Path $workspace $psRelative)) | Where-Object { $_.Contains($psFlag) })
if ($psLine.Count -ne 1) { throw 'Production password-style assertion changed; review its ownership' }
$psLine = $psLine[0]

function Check([bool]$Condition, [string]$Message) {
    if (-not $Condition) { throw $Message }
    $script:checks++
}
function Write-Fixture([string]$Relative, [string]$Text) {
    $target = Join-Path $fixture $Relative
    [void][IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($target))
    [IO.File]::WriteAllText($target, $Text, $utf8)
}
function Invoke-Audit {
    $start = [Diagnostics.ProcessStartInfo]::new($shell,
        ('-NoProfile -ExecutionPolicy Bypass -File "' + $workspace + '\scripts\audit_open_source_release.ps1" -Path "' + $fixture + '"'))
    $start.UseShellExecute = $false
    $start.CreateNoWindow = $true
    $start.RedirectStandardOutput = $true
    $start.RedirectStandardError = $true
    $start.EnvironmentVariables.Remove('PSModulePath')
    $process = [Diagnostics.Process]::new()
    $process.StartInfo = $start
    try {
        [void]$process.Start()
        $stdout = $process.StandardOutput.ReadToEndAsync()
        $stderr = $process.StandardError.ReadToEndAsync()
        if (-not $process.WaitForExit(30000)) { $process.Kill(); throw 'Owned audit process timed out' }
        return [pscustomobject]@{ Exit=$process.ExitCode; Out=$stdout.Result; Err=$stderr.Result }
    } finally { $process.Dispose() }
}
function Get-RealPatterns([string]$Relative, [string]$Variable) {
    $tokens = $null
    $errors = $null
    $ast = [Management.Automation.Language.Parser]::ParseFile((Join-Path $workspace $Relative), [ref]$tokens, [ref]$errors)
    if ($errors.Count) { throw ($errors | Out-String) }
    $assignments = @($ast.FindAll({ param($node)
        $node -is [Management.Automation.Language.AssignmentStatementAst] -and
        $node.Left -is [Management.Automation.Language.VariableExpressionAst] -and
        $node.Left.VariablePath.UserPath -eq $Variable
    }, $true))
    if ($assignments.Count -ne 1) { throw "Production pattern definition not found: $Relative/$Variable" }
    Invoke-Expression $assignments[0].Right.Extent.Text
}

try {
    [void][IO.Directory]::CreateDirectory((Join-Path $fixture 'assets/fonts'))
    foreach ($file in @('NotoSansCJKsc-Regular.otf', 'OFL.txt')) {
        [IO.File]::Copy((Join-Path $workspace "assets/fonts/$file"), (Join-Path $fixture "assets/fonts/$file"))
    }
    Write-Fixture $cRelative $cLine
    Write-Fixture $psRelative $psLine
    $cHash = (Get-FileHash -LiteralPath (Join-Path $fixture $cRelative)).Hash
    $psHash = (Get-FileHash -LiteralPath (Join-Path $fixture $psRelative)).Hash
    # 首先走真实审计入口，旧实现必须在这里复现误报。
    $audit = Invoke-Audit
    Check ($audit.Exit -eq 0) "Real audit rejected non-secret production probes: $($audit.Out) $($audit.Err)"
    Check ($audit.Out.Contains('recognized UI probe')) 'Accepted probe lacked a diagnostic'
    Check ((Get-FileHash -LiteralPath (Join-Path $fixture $cRelative)).Hash -ceq $cHash) 'Audit changed C source bytes'
    Check ((Get-FileHash -LiteralPath (Join-Path $fixture $psRelative)).Hash -ceq $psHash) 'Audit changed assertion source bytes'
    Write-Host '[PASS] actual audit accepts original UI probe/regex without editing the fixture'

    . (Join-Path $workspace 'scripts/release_content_scan.ps1')
    $cases = @(
        @{ Name='C probe'; Path=$cRelative; Text=$cLine; Reject=$false },
        @{ Name='PS assertion'; Path=$psRelative; Text=$psLine; Reject=$false },
        @{ Name='ordinary one'; Path='sample.txt'; Text=($name+'=1'); Reject=$true },
        @{ Name='ordinary zero'; Path='sample.txt'; Text=($name+'=0'); Reject=$true },
        @{ Name='empty assignment'; Path='sample.txt'; Text=($name+'= '); Reject=$false },
        @{ Name='placeholder'; Path='sample.txt'; Text=($name+'=<enter-value>'); Reject=$false },
        @{ Name='single character API key'; Path='sample.txt'; Text=('api'+'_key=x'); Reject=$true },
        @{ Name='single character secret'; Path='sample.txt'; Text=('sec'+'ret=x'); Reject=$true },
        @{ Name='same file'; Path=$cRelative; Text=($name+'=synthetic-only'); Reject=$true },
        @{ Name='same line after C probe'; Path=$cRelative; Text=($cLine+' '+$name+'=synthetic-only'); Reject=$true },
        @{ Name='same line before C probe'; Path=$cRelative; Text=($name+'=synthetic-only '+$cLine); Reject=$true },
        @{ Name='same line after assertion'; Path=$psRelative; Text=($psLine+' '+$name+'=synthetic-only'); Reject=$true },
        @{ Name='changed value'; Path=$cRelative; Text=$cLine.Replace(($name+'=1'),($name+'=2')); Reject=$true },
        @{ Name='changed condition'; Path=$cRelative; Text=$cLine.Replace('ES_PASSWORD','ES_READONLY'); Reject=$true },
        @{ Name='wrong source path'; Path='other.c'; Text=$cLine; Reject=$true },
        @{ Name='wrong assertion path'; Path='other.ps1'; Text=$psLine; Reject=$true },
        @{ Name='unanchored regex'; Path=$psRelative; Text=$psFlag.Replace('^',''); Reject=$true },
        @{ Name='changed regex value'; Path=$psRelative; Text=$psFlag.Replace('=1','=synthetic-only'); Reject=$true },
        @{ Name='API value enclosing probe'; Path=$psRelative; Text=('api'+'_key='+$psFlag); Reject=$true },
        @{ Name='adjacent token'; Path=$cRelative; Text=($cLine+' sk-'+('x'*20)); Reject=$true },
        @{ Name='adjacent secret'; Path=$psRelative; Text=($psLine+' sec'+'ret=synthetic-only'); Reject=$true },
        @{ Name='adjacent bearer'; Path=$cRelative; Text=($cLine+' Authoriz'+'ation: Bearer synthetic-only'); Reject=$true },
        @{ Name='local path'; Path=$cRelative; Text=($cLine+' C:'+'\Users\fixture-user'); Reject=$true }
    )
    foreach ($definition in @(
        @{Path='scripts/prepare_open_source_release.ps1'; Variable='scanPatterns'},
        @{Path='scripts/audit_open_source_release.ps1'; Variable='contentPatterns'}
    )) {
        $patterns = @(Get-RealPatterns $definition.Path $definition.Variable)
        foreach ($case in $cases) {
            $scan = Get-ReleaseContentScanText -RelativePath $case.Path -Text $case.Text
            $rejected = @($patterns | Where-Object { $scan -match $_ }).Count -gt 0
            Check ($rejected -eq $case.Reject) "$($definition.Path): wrong decision for $($case.Name)"
        }
    }
    Write-Host '[PASS] both real pattern sets reject adjacent/same-line credentials, short passwords and changed contexts'

    Write-Fixture $cRelative ($cLine+' '+$name+'=synthetic-only')
    $audit = Invoke-Audit
    Check ($audit.Exit -ne 0 -and $audit.Err.Contains('Forbidden content pattern')) 'Real audit accepted adjacent credential'
    Check (-not $audit.Err.Contains('synthetic-only')) 'Audit printed the candidate credential'
    Write-Fixture $cRelative $cLine
    Write-Fixture 'forbidden.dll' 'fixture'
    $audit = Invoke-Audit
    Check ($audit.Exit -ne 0 -and $audit.Err.Contains('Forbidden file extension')) 'Binary exclusion was lost'
    [IO.File]::Delete((Join-Path $fixture 'forbidden.dll'))
    Write-Fixture 'api.ini' 'fixture'
    $audit = Invoke-Audit
    Check ($audit.Exit -ne 0 -and $audit.Err.Contains('Forbidden file name')) 'User config exclusion was lost'
    [IO.File]::Delete((Join-Path $fixture 'api.ini'))
    [void][IO.Directory]::CreateDirectory((Join-Path $fixture 'TranslationCache'))
    $audit = Invoke-Audit
    Check ($audit.Exit -ne 0 -and $audit.Err.Contains('Forbidden directory')) 'Translation cache exclusion was lost'
    [IO.Directory]::Delete((Join-Path $fixture 'TranslationCache'))
    Check ((Invoke-Audit).Exit -eq 0) 'Audit did not recover after removing fixture violations'
    $passed = $true
    Write-Host "[PASS] release content scan: $script:checks assertions ($($PSVersionTable.PSVersion))"
} finally {
    if ($passed) {
        $resolved = [IO.Path]::GetFullPath($fixture)
        if (-not $resolved.StartsWith($tempBase+'\dst_release_scan_', [StringComparison]::OrdinalIgnoreCase)) { throw 'Unexpected fixture cleanup path' }
        if (@(Get-ChildItem -LiteralPath $resolved -Recurse -Force | Where-Object { $_.Attributes -band [IO.FileAttributes]::ReparsePoint }).Count) { throw 'Reparse fixture retained' }
        Remove-Item -LiteralPath $resolved -Recurse -Force
    } else { Write-Host "[DIAGNOSTIC] Failed fixture retained: $fixture" }
}
