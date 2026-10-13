using System;
using System.IO;
using System.Reflection;
using DstLauncher;

internal static class FontBundleProbe
{
    private static int Main(string[] args)
    {
        if (args.Length != 2) return 2;
        Launcher.Root = args[0];
        Type deploy = typeof(Deploy);
        var install = deploy.GetMethod("DeployBundledCjkFont", BindingFlags.NonPublic | BindingFlags.Static);
        var restore = deploy.GetMethod("RestoreBundledCjkFont", BindingFlags.NonPublic | BindingFlags.Static);
        Type statsType = deploy.GetNestedType("RestoreStats", BindingFlags.Public | BindingFlags.NonPublic);
        if (install == null || restore == null || statsType == null) return 3;
        string font = Path.Combine(args[1], "ds_font.otf");
        Directory.CreateDirectory(args[1]);
        if (!(bool)install.Invoke(null, new object[] { font })) return 4;
        if (!File.Exists(font) || !File.Exists(font + ".OFL.txt") ||
            !File.Exists(font + ".dst-owned")) return 5;
        object stats = Activator.CreateInstance(statsType, true);
        restore.Invoke(null, new object[] { font, stats });
        if (File.Exists(font) || File.Exists(font + ".OFL.txt")) return 6;
        File.WriteAllText(font, "USER");
        stats = Activator.CreateInstance(statsType, true);
        restore.Invoke(null, new object[] { font, stats });
        if (File.ReadAllText(font) != "USER") return 7;
        Type bundle = typeof(Deploy).Assembly.GetType("DstLauncher.FontBundle");
        var godotLicense = bundle.GetMethod("EnsureGodotLicense", BindingFlags.NonPublic | BindingFlags.Static);
        string godotDir = Path.Combine(args[1], "godot");
        Directory.CreateDirectory(godotDir);
        if (!(bool)godotLicense.Invoke(null, new object[] { godotDir }) ||
            !File.Exists(Path.Combine(godotDir, "dst_godot_patch.OFL.txt"))) return 8;
        Console.WriteLine("C# font bundle probe PASS: deploy, OFL, restore, user font, Godot license");
        return 0;
    }
}
