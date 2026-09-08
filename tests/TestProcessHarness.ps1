if (-not (Get-Variable -Name TestOwnedProcesses -Scope Script -ErrorAction SilentlyContinue)) {
    $script:TestOwnedProcesses = [System.Collections.Generic.Dictionary[int, System.Diagnostics.Process]]::new()
}
if (-not (Get-Variable -Name TestOwnedRedirects -Scope Script -ErrorAction SilentlyContinue)) {
    $script:TestOwnedRedirects = [System.Collections.Generic.Dictionary[int, object]]::new()
}

function ConvertTo-NativeProcessArgument {
    param(
        [AllowNull()]
        [AllowEmptyString()]
        [string]$Value
    )

    if ($null -eq $Value -or $Value.Length -eq 0) { return '""' }
    if ($Value -notmatch '[\s"]') { return $Value }

    $builder = [Text.StringBuilder]::new($Value.Length + 2)
    [void]$builder.Append('"')
    $slashCount = 0
    foreach ($character in $Value.ToCharArray()) {
        if ($character -eq '\') {
            $slashCount++
            continue
        }
        if ($character -eq '"') {
            if ($slashCount -gt 0) {
                [void]$builder.Append(('\' * ($slashCount * 2)))
            }
            [void]$builder.Append('\"')
            $slashCount = 0
            continue
        }
        if ($slashCount -gt 0) {
            [void]$builder.Append(('\' * $slashCount))
            $slashCount = 0
        }
        [void]$builder.Append($character)
    }
    if ($slashCount -gt 0) {
        [void]$builder.Append(('\' * ($slashCount * 2)))
    }
    [void]$builder.Append('"')
    return $builder.ToString()
}

function Join-NativeProcessArguments {
    param([AllowEmptyCollection()][object[]]$ArgumentList = @())
    return (($ArgumentList | ForEach-Object {
        ConvertTo-NativeProcessArgument ([string]$_)
    }) -join ' ')
}

function Close-FailedTestProcess {
    param(
        [Parameter(Mandatory = $true)][System.Diagnostics.Process]$Process,
        [Parameter(Mandatory = $true)][bool]$Started
    )

    $cleanupErrors = [System.Collections.Generic.List[string]]::new()
    if ($Started) {
        try {
            if (-not $Process.HasExited) {
                Stop-Process -Id $Process.Id -Force -ErrorAction Stop
                [void]$Process.WaitForExit(5000)
            }
        } catch {
            $cleanupErrors.Add($_.Exception.Message)
        }
    }
    try { $Process.Dispose() } catch { $cleanupErrors.Add($_.Exception.Message) }
    return @($cleanupErrors)
}

function Close-TestRedirectStream {
    param([Parameter(Mandatory = $true)]$Stream)

    try {
        $Stream.Dispose()
        return @()
    } catch {
        return @($_.Exception.Message)
    }
}

function Get-TestOwnedProcess {
    param([Parameter(Mandatory = $true)][System.Diagnostics.Process]$Process)

    $registered = $null
    if (-not $script:TestOwnedProcesses.TryGetValue($Process.Id, [ref]$registered) -or
        -not [object]::ReferenceEquals($registered, $Process)) {
        throw "Process $($Process.Id) is not owned by this test harness."
    }
    return $registered
}

function Start-TestOwnedProcess {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory = $true)][string]$FilePath,
        [AllowEmptyCollection()][object[]]$ArgumentList = @(),
        [switch]$NoNewWindow,
        [ValidateSet("Normal", "Hidden", "Minimized", "Maximized")]
        [string]$WindowStyle = "Hidden",
        [string]$RedirectStandardOutput = "",
        [string]$RedirectStandardError = ""
    )

    $argumentLine = Join-NativeProcessArguments $ArgumentList
    $startInfo = [System.Diagnostics.ProcessStartInfo]::new()
    $startInfo.FileName = $FilePath
    $startInfo.Arguments = $argumentLine
    $startInfo.UseShellExecute = $false
    # 测试进程始终由 harness 拥有；隐藏窗口避免并行套件污染桌面。
    $startInfo.CreateNoWindow = $true
    $redirectState = [ordered]@{ OutputTask = $null; ErrorTask = $null; Streams = @() }
    if ($RedirectStandardOutput) {
        $startInfo.RedirectStandardOutput = $true
        $redirectState.Streams += [System.IO.FileStream]::new(
            $RedirectStandardOutput,
            [System.IO.FileMode]::Create,
            [System.IO.FileAccess]::Write,
            [System.IO.FileShare]::Read)
    }
    if ($RedirectStandardError) {
        $startInfo.RedirectStandardError = $true
        $redirectState.Streams += [System.IO.FileStream]::new(
            $RedirectStandardError,
            [System.IO.FileMode]::Create,
            [System.IO.FileAccess]::Write,
            [System.IO.FileShare]::Read)
    }
    $process = [System.Diagnostics.Process]::new()
    $process.StartInfo = $startInfo
    $processStarted = $false
    try {
        if (-not $process.Start()) { throw "Process.Start returned false for '$FilePath'." }
        $processStarted = $true
        if ($RedirectStandardOutput) {
            $redirectState.OutputTask = $process.StandardOutput.BaseStream.CopyToAsync($redirectState.Streams[0])
        }
        if ($RedirectStandardError) {
            $errorStreamIndex = if ($RedirectStandardOutput) { 1 } else { 0 }
            $redirectState.ErrorTask = $process.StandardError.BaseStream.CopyToAsync($redirectState.Streams[$errorStreamIndex])
        }
        $script:TestOwnedProcesses.Add($process.Id, $process)
        if ($RedirectStandardOutput -or $RedirectStandardError) {
            $script:TestOwnedRedirects.Add($process.Id, [pscustomobject]$redirectState)
        }
    } catch {
        $startupError = $_
        # Start 失败的 Process 没有有效 Id；辅助函数只清理由本函数确认启动的进程。
        $cleanupErrors = @(Close-FailedTestProcess -Process $process -Started $processStarted)
        foreach ($stream in $redirectState.Streams) {
            $cleanupErrors += @(Close-TestRedirectStream -Stream $stream)
        }
        if ($cleanupErrors.Count -gt 0) {
            Write-Warning ("Process startup cleanup failed for '$FilePath': " + ($cleanupErrors -join "; "))
        }
        $PSCmdlet.ThrowTerminatingError($startupError)
    }
    return $process
}

function Complete-TestOwnedRedirects {
    param([Parameter(Mandatory = $true)][int]$ProcessId)

    $redirectState = $null
    if (-not $script:TestOwnedRedirects.TryGetValue($ProcessId, [ref]$redirectState)) {
        return
    }
    try {
        if ($redirectState.OutputTask) { [void]$redirectState.OutputTask.GetAwaiter().GetResult() }
        if ($redirectState.ErrorTask) { [void]$redirectState.ErrorTask.GetAwaiter().GetResult() }
    } finally {
        foreach ($stream in $redirectState.Streams) { $stream.Dispose() }
        [void]$script:TestOwnedRedirects.Remove($ProcessId)
    }
}

function Wait-TestOwnedProcess {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory = $true)][System.Diagnostics.Process]$Process,
        [ValidateRange(1, 86400)][int]$TimeoutSec = 30
    )

    $owned = Get-TestOwnedProcess $Process
    $completed = $owned.WaitForExit($TimeoutSec * 1000)
    if ($completed) {
        # 进程结束后再返回，确保重定向文件已写完，调用方可以立即读取日志。
        Complete-TestOwnedRedirects -ProcessId $owned.Id
    }
    return $completed
}

function Complete-TestOwnedProcess {
    [CmdletBinding()]
    param([Parameter(Mandatory = $true)][System.Diagnostics.Process]$Process)

    $owned = Get-TestOwnedProcess $Process
    $owned.Refresh()
    if (-not $owned.HasExited) {
        throw "Owned process $($owned.Id) is still running."
    }
    Complete-TestOwnedRedirects -ProcessId $owned.Id
    [void]$script:TestOwnedProcesses.Remove($owned.Id)
}

function Stop-TestOwnedProcess {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory = $true)][System.Diagnostics.Process]$Process,
        [switch]$Tree,
        [ValidateRange(1, 60)][int]$TimeoutSec = 5
    )

    $owned = Get-TestOwnedProcess $Process
    $owned.Refresh()
    if (-not $owned.HasExited) {
        if ($Tree) {
            & taskkill.exe /PID $owned.Id /T /F *> $null
            $owned.Refresh()
            if ($LASTEXITCODE -ne 0 -and -not $owned.HasExited) {
                Stop-Process -Id $owned.Id -Force -ErrorAction Stop
            }
        } else {
            Stop-Process -Id $owned.Id -Force -ErrorAction Stop
        }
        [void]$owned.WaitForExit($TimeoutSec * 1000)
        $owned.Refresh()
        if (-not $owned.HasExited) {
            throw "Owned process $($owned.Id) did not stop within $TimeoutSec seconds."
        }
    }
    Complete-TestOwnedProcess -Process $owned
}

function Close-TestOwnedProcess {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory = $true)][System.Diagnostics.Process]$Process,
        [switch]$Tree
    )

    $owned = Get-TestOwnedProcess $Process
    $owned.Refresh()
    if ($owned.HasExited) {
        Complete-TestOwnedProcess -Process $owned
    } else {
        Stop-TestOwnedProcess -Process $owned -Tree:$Tree
    }
}

function Invoke-TestOwnedCleanup {
    [CmdletBinding()]
    param()

    $failures = @()
    foreach ($process in @($script:TestOwnedProcesses.Values)) {
        try {
            Stop-TestOwnedProcess -Process $process -Tree
        } catch {
            $failures += "process $($process.Id): $($_.Exception.Message)"
        }
    }
    if ($failures.Count -gt 0) {
        throw "Could not clean owned test processes: $($failures -join '; ')"
    }
}
