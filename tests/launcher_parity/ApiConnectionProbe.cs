using System;
using System.Text;
using DstLauncher;
[assembly: System.Runtime.Versioning.TargetFramework(".NETFramework,Version=v4.7.2")]
class ApiConnectionProbe
{
    static int Main(string[] args)
    {
        Console.OutputEncoding = new UTF8Encoding(false);
        if (args.Length == 2 && args[0] == "wait") { Console.WriteLine(ApiConnection.WaitMilliseconds(args[1])); return 0; }
        if (args.Length != 3) return 2;
        var r = ApiConnection.Test(args[0], args[1], args[2]);
        Console.WriteLine("success=" + (r.Success ? 1 : 0)); Console.WriteLine(r.Message); return 0;
    }
}
