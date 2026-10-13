using System;
using System.Text;
using DstLauncher;
class ApiModelsProbe
{
    static int Main(string[] args)
    {
        Console.OutputEncoding = new UTF8Encoding(false);
        if (args.Length < 2) return 2;
        if (args[0] == "url") { Console.WriteLine(ApiModels.ModelsUrl(args[1]) ?? "INVALID"); return 0; }
        var r = args[0] == "catalog" ? (ApiModels.Result)typeof(ApiModels).GetMethod("FetchCatalog",
            System.Reflection.BindingFlags.NonPublic | System.Reflection.BindingFlags.Static).Invoke(null,
            new object[] { args[1], args[2], int.Parse(args[3]) }) : ApiModels.Fetch(args[1], args.Length > 2 ? args[2] : "");
        Console.WriteLine("count=" + r.Items.Length); Console.WriteLine(r.Message);
        foreach (string id in r.Items) Console.WriteLine(id);
        return 0;
    }
}
