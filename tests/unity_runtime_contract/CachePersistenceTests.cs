using System;
using System.IO;
using System.Linq;
using System.Threading.Tasks;

internal static class CachePersistenceTests
{
    public static void Run(Action<string, bool> check)
    {
        string root = Path.Combine(Path.GetTempPath(), "dst_cache_load_" + Guid.NewGuid().ToString("N"));
        Directory.CreateDirectory(Path.Combine(root, "TranslationCache"));
        string path = Path.Combine(root, "TranslationCache", "unity_translation_cache.json");
        const string old = "{\"Old saved text.\":\"旧译文。\"}";
        try {
            var locked = new CachePersistenceSeam { Root = root };
            File.WriteAllText(path, old);
            using (var held = new FileStream(path, FileMode.Open, FileAccess.Read, FileShare.None)) {
                locked.BootCacheLoadAsync().GetAwaiter().GetResult();
            }
            locked.Cache["New text."] = "新译文。";
            locked.FlushLocalCacheToDisk();
            check("Mono read failure prevents final flush from replacing unread cache", File.ReadAllText(path) == old);
            check("Mono read failure is diagnosed", locked.Logger.Warnings.Count + locked.Errors.Count > 0);
            locked.ScheduleLocalCachePersist();
            bool recovered = System.Threading.SpinWait.SpinUntil(() => !locked._cachePersistScheduled, 2000);
            locked._shuttingDown = true;
            check("Mono deferred persistence reloads recovered file and retains old/new entries", recovered
                && File.ReadAllText(path).Contains("Old saved text.") && File.ReadAllText(path).Contains("New text."));

            File.WriteAllText(path, "{invalid");
            var invalid = new CachePersistenceSeam { Root = root };
            invalid.BootCacheLoadAsync().GetAwaiter().GetResult();
            invalid.Cache["New text."] = "新译文。";
            invalid.FlushLocalCacheToDisk();
            check("Mono malformed cache survives final flush byte for byte", File.ReadAllText(path) == "{invalid");

            File.WriteAllText(path, old);
            var healthy = new CachePersistenceSeam { Root = root };
            healthy.Cache["Old saved text."] = "实时更正。";
            healthy.BootCacheLoadAsync().GetAwaiter().GetResult();
            healthy.FlushLocalCacheToDisk();
            check("Mono successful import keeps newer live correction", File.ReadAllText(path).Contains("实时更正。"));

            var interrupted = new CachePersistenceSeam { Root = root };
            interrupted.OnImport = () => interrupted._shuttingDown = true;
            interrupted.BootCacheLoadAsync().GetAwaiter().GetResult();
            check("Mono interrupted import cannot authorize persistence", !interrupted._bootCacheLoadComplete);

            string oversized = "{\"Old saved text.\":\"" + new string('x', 11000) + "\"}";
            File.WriteAllText(path, oversized);
            var isolated = new CachePersistenceSeam { Root = root };
            using (var held = new FileStream(path, FileMode.Open, FileAccess.Read, FileShare.None)) {
                isolated.BootCacheLoadAsync().GetAwaiter().GetResult();
            }
            isolated.Cache["New text."] = "新译文。";
            isolated.FlushLocalCacheToDisk();
            check("Mono failed oversized isolation cannot replace user file", File.ReadAllText(path) == oversized);
            var retry = new CachePersistenceSeam { Root = root };
            retry.BootCacheLoadAsync().GetAwaiter().GetResult();
            check("Mono successful oversized isolation preserves exact backup", retry._bootCacheLoadComplete
                && !File.Exists(path) && Directory.GetFiles(Path.GetDirectoryName(path), "*.bak-oversized-*").Any(p => File.ReadAllText(p) == oversized));

            var missing = new CachePersistenceSeam { Root = root };
            missing.BootCacheLoadAsync().GetAwaiter().GetResult();
            missing.Cache["New text."] = "新译文。";
            missing.FlushLocalCacheToDisk();
            check("Mono new game without cache can persist", File.ReadAllText(path).Contains("New text."));
        } finally {
            // 测试只创建此唯一目录的直接文件；禁止递归清理越过边界。
            foreach (string file in Directory.GetFiles(Path.Combine(root, "TranslationCache"))) File.Delete(file);
            Directory.Delete(Path.Combine(root, "TranslationCache"));
            Directory.Delete(root);
        }
    }
}
