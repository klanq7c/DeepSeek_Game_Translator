using System;
using System.Text;
using DstLauncher;

internal static class WarmupHttpProbe
{
    private static int Main(string[] args)
    {
        if (args.Length < 2) return 2;
        Console.OutputEncoding = new UTF8Encoding(false);
        Log.SetSink(Console.WriteLine);
        if (args.Length > 2) Warmup.DumpSink = (path, body) => Console.WriteLine("post=" + path + " " + Encoding.UTF8.GetString(body));
        return Warmup.Run(args[0], (Engine)int.Parse(args[1]));
    }
}
