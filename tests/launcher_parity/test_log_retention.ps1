[CmdletBinding()]
param()
$ErrorActionPreference = 'Stop'
$repo = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
# 接收端使用普通 C# 委托，直接覆盖窗口和诊断命令使用的真实 Log.Append。
$probe = @'
public static class LauncherLogRetentionProbe
{
    public static void Run()
    {
        var received = new System.Collections.Generic.List<string>();
        DstLauncher.Log.Clear();
        DstLauncher.Log.SetSink(received.Add);
        try {
            for (int i = 0; i < 10000; i++) DstLauncher.Log.Append("log-" + i);
            var snapshot = DstLauncher.Log.Snapshot();
            if (snapshot.Length != 2000 || snapshot[0] != "log-8000" || snapshot[1999] != "log-9999")
                throw new System.Exception("Recent log history is not bounded and ordered: retained=" + snapshot.Length);
            if (received.Count != 10000) throw new System.Exception("Log sink lost output when history wrapped");
            for (int i = 0; i < received.Count; i++)
                if (received[i] != "log-" + i) throw new System.Exception("Log sink order changed at " + i);
            DstLauncher.Log.Clear();
            if (DstLauncher.Log.Snapshot().Length != 0) throw new System.Exception("Clear retained old log history");
            if (snapshot[0] != "log-8000") throw new System.Exception("Clear mutated a previous snapshot");
            DstLauncher.Log.Append("one\r\ntwo\tthree");
            if (DstLauncher.Log.Snapshot()[0] != "one  two three" || received[10000] != "one  two three")
                throw new System.Exception("Log sanitization changed");
        } finally {
            DstLauncher.Log.SetSink(null);
            DstLauncher.Log.Clear();
        }
    }
}
'@
Add-Type -TypeDefinition ([IO.File]::ReadAllText((Join-Path $repo 'native\src\launcher_cs\Log.cs')) + "`n" + $probe)
[LauncherLogRetentionProbe]::Run()
Write-Host '[PASS] Log: 10000 lines delivered in order; latest 2000 retained; snapshot, clear and sanitization preserved.'
