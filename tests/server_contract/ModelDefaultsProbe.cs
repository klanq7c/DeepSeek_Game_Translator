using System;
using System.Reflection;
using DstServerCs;
class ModelDefaultsProbe
{
    static int Main(string[] args)
    {
        Console.OutputEncoding = new System.Text.UTF8Encoding(false);
        if (args.Length != 1) return 2;
        var cfg = new ApiConfig();
        string initial = cfg.Model;
        cfg.InitDefaults();
        if (cfg.Model != initial) return 4;
        Console.WriteLine(cfg.Model);
        cfg.Load(args[0]);
        if (!cfg.Enabled) return 3;
        ApiClient.Configure(cfg);
        // 单条、批量和上下文请求共用此生产构造器。
        var build = typeof(ApiClient).GetMethod("BuildMessages", BindingFlags.Static | BindingFlags.NonPublic);
        Console.WriteLine(build.Invoke(null, new object[] { "system", "Hello {name}.", 512 }));
        return 0;
    }
}
