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
        Console.Error.WriteLine("CONFIG {\"timeout_ms\":" + cfg.TimeoutMs
            + ",\"concurrency\":" + cfg.Concurrency
            + ",\"batch_max_items\":" + cfg.BatchMaxItems
            + ",\"batch_char_budget\":" + cfg.BatchCharBudget + "}");
        ApiClient.Configure(cfg);
        // 单条、批量和上下文请求共用此生产构造器。
        var build = typeof(ApiClient).GetMethod("BuildMessages", BindingFlags.Static | BindingFlags.NonPublic);
        var batch = typeof(ApiClient).GetMethod("BuildBatchUser", BindingFlags.Static | BindingFlags.NonPublic);
        var context = typeof(ApiClient).GetMethod("BuildCtxUser", BindingFlags.Static | BindingFlags.NonPublic);
        var single = typeof(ApiClient).GetField("SingleUserLine", BindingFlags.Static | BindingFlags.NonPublic);
        string[] texts = { "Hello {name}.", "\\C[1]Open menu." };
        string[] prevs = { "Previous line.", "Hello {name}." };
        string[] users = {
            (string)single.GetRawConstantValue() + texts[0],
            (string)batch.Invoke(null, new object[] { texts }),
            (string)context.Invoke(null, new object[] { texts, texts, prevs })
        };
        foreach (string user in users)
            Console.WriteLine(build.Invoke(null, new object[] { "system", user, 512 }));
        return 0;
    }
}
