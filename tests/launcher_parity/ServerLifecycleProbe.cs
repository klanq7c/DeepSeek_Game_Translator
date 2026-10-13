using System;
using System.Diagnostics;
using System.Text;
using DstLauncher;

internal static class ServerLifecycleProbe
{
    private static int Main(string[] args)
    {
        if (args.Length != 1) return 2;
        Console.OutputEncoding = new UTF8Encoding(false);
        Launcher.Root = args[0];
        Log.SetSink(Console.WriteLine);
        ServerProcess.RefreshStatus();
        Console.WriteLine("before=" + (ServerProcess.Started ? 1 : 0));
        var clock = Stopwatch.StartNew();
        ServerProcess.Stop();
        long elapsed = clock.ElapsedMilliseconds;
        Console.WriteLine("stop_ms=" + elapsed);
        Console.WriteLine("after=" + (ServerProcess.Started ? 1 : 0));
        Console.WriteLine("alive=" + (ServerProcess.Alive() ? 1 : 0));
        ServerProcess.RefreshStatus();
        Console.WriteLine("refreshed=" + (ServerProcess.Started ? 1 : 0));
        return 0;
    }
}
