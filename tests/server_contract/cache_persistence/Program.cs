using System;
using System.Collections.Generic;
using System.IO;
using System.Reflection;
using System.Text;
using DstCore;
using DstServerCs;

internal static class Program
{
    private static int passed;
    private static int failed;
    private static readonly FieldInfo WriterField = typeof(CacheStore).GetField(
        "persistWriter", BindingFlags.Instance | BindingFlags.NonPublic);
    private static readonly MethodInfo CloseMethod = typeof(CacheStore).GetMethod(
        "CloseWriter", BindingFlags.Instance | BindingFlags.NonPublic);

    // 仅替换外部写入器边界，生产代码的归一化、映射、脏状态跟踪
    // 和追加循环仍以源链接方式使用原实现。
    private sealed class ObservedWriter : StreamWriter
    {
        public int Writes;
        public int Flushes;
        public int FailWriteAt;
        public bool FailFlush;
        public int PartialBytes;

        public ObservedWriter(string path)
            : base(new FileStream(path, FileMode.Append, FileAccess.Write, FileShare.Read),
                   new UTF8Encoding(false)) { }

        public override void Write(string value)
        {
            Writes++;
            if (Writes == FailWriteAt) {
                if (PartialBytes > 0) {
                    base.Write(value.Substring(0, PartialBytes));
                    base.Flush();
                }
                throw new IOException("Injected append failure");
            }
            base.Write(value);
        }

        public override void Flush()
        {
            Flushes++;
            if (FailFlush) throw new IOException("Injected flush failure");
            base.Flush();
        }
    }

    private sealed class Fixture : IDisposable
    {
        public readonly string Path;
        public readonly CacheStore Cache;

        public Fixture(string root, string name)
        {
            Path = System.IO.Path.Combine(root, name + ".tsv");
            Cache = new CacheStore(Path);
        }

        public ObservedWriter Observe()
        {
            CloseMethod.Invoke(Cache, null);
            var writer = new ObservedWriter(Path);
            WriterField.SetValue(Cache, writer);
            return writer;
        }

        public CacheStore Reload()
        {
            CloseMethod.Invoke(Cache, null);
            var reloaded = new CacheStore(Path);
            reloaded.Load();
            return reloaded;
        }

        public void Dispose() { CloseMethod.Invoke(Cache, null); }
    }

    private static void Check(string name, bool condition)
    {
        if (condition) passed++; else failed++;
        Console.WriteLine((condition ? "[PASS] " : "[FAIL] ") + name);
    }

    private static void Counts(string name, CacheStore.PersistResult r,
                               int accepted, int persisted, int rejected, string status)
    {
        Check(name + " counts (" + r.Accepted + "/" + r.Persisted + "/"
              + r.Rejected + "/" + r.Status + ")", r.Accepted == accepted
              && r.Persisted == persisted && r.Rejected == rejected && r.Status == status);
    }

    private static CacheStore.PersistResult CaptureFailure(Fixture f, string[] keys,
        string[] values, string stage)
    {
        var previous = Console.Error;
        using (var diagnostic = new StringWriter()) {
            CacheStore.PersistResult result;
            try {
                Console.SetError(diagnostic);
                result = f.Cache.SetManyPersist(keys, values);
            } finally {
                Console.SetError(previous);
            }
            string text = diagnostic.ToString();
            Check(stage + " records operation, exception and cache path",
                text.Contains("persist-" + stage + " failed")
                && text.Contains("IOException") && text.Contains(f.Path));
            Check(stage + " releases failed writer", WriterField.GetValue(f.Cache) == null);
            return result;
        }
    }

    private static void SuccessfulBatch(string root)
    {
        using (var f = new Fixture(root, "success")) {
            var writer = f.Observe();
            var keys = new string[128];
            var values = new string[keys.Length];
            for (int i = 0; i < keys.Length; i++) {
                keys[i] = "English line " + i;
                values[i] = "\u8bd1\u6587 " + i;
            }
            Counts("128-entry batch", f.Cache.SetManyPersist(keys, values), 128, 128, 0, "all");
            Check("128 entries require one explicit Flush", writer.Writes == 128 && writer.Flushes == 1);
            long length = new FileInfo(f.Path).Length;
            Counts("healthy duplicates", f.Cache.SetManyPersist(keys, values), 128, 128, 0, "all");
            Check("healthy duplicates neither write nor flush", writer.Writes == 128
                && writer.Flushes == 1 && new FileInfo(f.Path).Length == length);
            Counts("empty/identity rejection", f.Cache.SetManyPersist(
                new[] { "", "Identity", "Empty" }, new[] { "value", "Identity", "" }), 0, 0, 3, "all");
            Check("rejected entries do not flush", writer.Flushes == 1);
            // 允许共享写入，以便检查已刷新的字节，而无需先关闭
            // 生产写入器并隐式触发最后一次 Flush。
            var disk = new Dictionary<string, string>();
            bool rowsDecode = true;
            using (var stream = new FileStream(f.Path, FileMode.Open, FileAccess.Read, FileShare.ReadWrite))
            using (var reader = new StreamReader(stream, Encoding.UTF8)) {
                string line;
                while ((line = reader.ReadLine()) != null) {
                    string key, value;
                    rowsDecode &= CacheCodec.TryDecodeLine(line, out key, out value);
                    if (key != null) disk[key] = value;
                }
            }
            Check("all confirmed TSV rows decode", rowsDecode);
            bool equal = disk.Count == 128;
            for (int i = 0; i < keys.Length; i++) equal &= disk.TryGetValue(keys[i], out var value) && value == values[i];
            Check("all confirmed values are readable before closing the writer", equal);
        }
    }

    private static void LiveBatchFlushesOnce(string root)
    {
        using (var f = new Fixture(root, "live-batch")) {
            var writer = f.Observe();
            var keys = new string[48];
            var values = new string[48];
            for (int i = 0; i < keys.Length; i++) {
                keys[i] = "Live sentence " + i + " in the hall.";
                values[i] = "\u8bd1\u6587 " + i;
            }
            keys[0] = "RenPy [player] {b}line{/b}";
            values[0] = "\u8bd1 [player] {b}\u6587{/b}";
            keys[1] = "RPG \\c[2]line\\v[1]";
            values[1] = "\u8bd1 \\c[2]\u6587\\v[1]";
            keys[2] = "Unity <color=red>line</color>";
            values[2] = "\u8bd1 <color=red>\u6587</color>";
            keys[3] = "Godot [color=red]line[/color]";
            values[3] = "\u8bd1 [color=red]\u6587[/color]";
            values[46] = "Simplified Chinese translation: " + keys[46]; // 提示词回显也须拒绝。
            values[47] = keys[47]; // 原文回显不能入库，也不能当作实时成功。

            bool[] accepted = LiveBatchPersistence.Persist(f.Cache, keys, values);
            bool correct = accepted.Length == 48 && !accepted[46] && !accepted[47];
            for (int i = 0; i < 46; i++) correct &= accepted[i];
            Check("48-item live batch reports valid and echo slots", correct);
            Check("48-item live batch writes 46 rows with one explicit Flush",
                writer.Writes == 46 && writer.Flushes == 1);
            var reloaded = f.Reload();
            bool restored = reloaded.Size == 46 && reloaded.Get(keys[46]) == null && reloaded.Get(keys[47]) == null;
            for (int i = 0; i < 46; i++) restored &= reloaded.Get(keys[i]) == values[i];
            Check("live batch reload retains all 46 translations and four engine markup forms", restored);
        }
    }

    private static void MarkupAndOverwrites(string root)
    {
        using (var f = new Fixture(root, "markup")) {
            string[] keys = { "RenPy [player] {b}line{/b}", "RPG \\c[2]line\\v[1]",
                "Unity <color=red>line</color>", "Godot [color=red]line[/color]" };
            string[] values = { "\u8bd1 [player] {b}\u6587{/b}", "\u8bd1 \\c[2]\u6587\\v[1]",
                "\u8bd1 <color=red>\u6587</color>", "\u8bd1 [color=red]\u6587[/color]" };
            string[] wrapped = new string[values.Length];
            for (int i = 0; i < values.Length; i++) wrapped[i] = "Simplified Chinese translation: " + values[i];
            Counts("engine markup and prompt cleanup", f.Cache.SetManyPersist(keys, wrapped), 4, 4, 0, "all");
            var writer = f.Observe();
            Counts("A-B-A and duplicate final value", f.Cache.SetManyPersist(
                new[] { "Repeated", "Repeated", "Repeated", "Repeated" },
                new[] { "\u7532", "\u4e59", "\u7532", "\u7532" }), 4, 4, 0, "all");
            Check("repeated keys still flush once", writer.Flushes == 1);
            var reloaded = f.Reload();
            bool equal = reloaded.Get("Repeated") == "\u7532";
            for (int i = 0; i < keys.Length; i++) equal &= reloaded.Get(keys[i]) == values[i];
            Check("restart preserves all engine markup and final overwrite", equal);
        }
    }

    private static void FailedBatch(string root, string name, int failWriteAt,
                                    bool failFlush, bool seed)
    {
        using (var f = new Fixture(root, name)) {
            if (seed) Counts(name + " seed", f.Cache.SetManyPersist(
                new[] { "Known" }, new[] { "\u5df2\u5b58" }), 1, 1, 0, "all");
            var writer = f.Observe();
            writer.FailWriteAt = failWriteAt;
            writer.FailFlush = failFlush;
            string[] keys = seed
                ? new[] { "Known", "Repeated", "Repeated", "Repeated", "Tail" }
                : new[] { "Repeated", "Repeated", "Repeated", "Tail" };
            string[] values = seed
                ? new[] { "\u5df2\u5b58", "\u7532", "\u4e59", "\u7532", "\u5c3e" }
                : new[] { "\u7532", "\u4e59", "\u7532", "\u5c3e" };
            var result = CaptureFailure(f, keys, values, failFlush ? "flush" : "write");
            Counts(name, result, keys.Length, seed ? 1 : 0, 0, seed ? "partial" : "failed");
            Check(name + " retains final memory value and unattempted tail",
                f.Cache.Get("Repeated") == "\u7532" && f.Cache.Get("Tail") == "\u5c3e");
            if (failWriteAt == 3) Check(name + " actually leaves B on disk before recovery",
                f.Reload().Get("Repeated") == "\u4e59");
            long beforeRetry = new FileInfo(f.Path).Length;
            string[] retryKeys = { "Repeated", "Tail" };
            string[] retryValues = { "\u7532", "\u5c3e" };
            Counts(name + " same-value retry", f.Cache.SetManyPersist(retryKeys, retryValues), 2, 2, 0, "all");
            Check(name + " retries all unconfirmed values even after close flushed them",
                new FileInfo(f.Path).Length > beforeRetry);
            long afterRetry = new FileInfo(f.Path).Length;
            Counts(name + " successful retry clears dirty state", f.Cache.SetManyPersist(retryKeys, retryValues), 2, 2, 0, "all");
            Check(name + " recovered duplicates do not append", new FileInfo(f.Path).Length == afterRetry);
            var reloaded = f.Reload();
            Check(name + " recovered final values survive restart", reloaded.Get("Repeated") == "\u7532"
                && reloaded.Get("Tail") == "\u5c3e" && (!seed || reloaded.Get("Known") == "\u5df2\u5b58"));
        }
    }

    private static void PartialWriteAndTails(string root)
    {
        using (var f = new Fixture(root, "partial-bytes")) {
            var writer = f.Observe();
            writer.FailWriteAt = 1;
            writer.PartialBytes = 4;
            var keys = new[] { "Broken source" };
            var values = new[] { "Recovered translation" };
            Counts("partial bytes rejected", CaptureFailure(f, keys, values, "write"), 1, 0, 0, "failed");
            Counts("partial bytes same-value retry", f.Cache.SetManyPersist(keys, values), 1, 1, 0, "all");
            var restored = f.Reload();
            Check("partial write retry survives reload under original key", restored.Get(keys[0]) == values[0]);
            Check("partial write cannot manufacture a concatenated key", restored.Get("BroBroken source") == null);
        }
        string full = CacheCodec.EncodeLine("Existing [player]", "Translated [player]");
        string[] tails = { "QnJv", full.TrimEnd('\n'), full.TrimEnd('\n') + "\r", full, "" };
        for (int i = 0; i < tails.Length; i++) {
            using (var f = new Fixture(root, "tail-" + i)) {
                byte[] prefix = Encoding.UTF8.GetBytes(CacheCodec.EncodeLine("Adjacent <b>line</b>", "Safe <b>value</b>") + tails[i]);
                File.WriteAllBytes(f.Path, prefix);
                f.Cache.Load();
                Counts("tail " + i + " append", f.Cache.SetManyPersist(new[] { "Next key" }, new[] { "Next value" }), 1, 1, 0, "all");
                var restored = f.Reload();
                Check("tail " + i + " new and adjacent records reload", restored.Get("Next key") == "Next value"
                    && restored.Get("Adjacent <b>line</b>") == "Safe <b>value</b>");
                if (i >= 1 && i <= 3) Check("tail " + i + " complete final record retained", restored.Get("Existing [player]") == "Translated [player]");
                byte[] bytes = File.ReadAllBytes(f.Path);
                bool preserved = bytes.Length >= prefix.Length;
                for (int j = 0; j < prefix.Length && preserved; j++) preserved &= bytes[j] == prefix[j];
                Check("tail " + i + " preserves every existing byte", preserved);
                long expected = prefix.Length + (prefix.Length > 0 && prefix[prefix.Length - 1] != 10 ? 1 : 0)
                    + Encoding.UTF8.GetByteCount(CacheCodec.EncodeLine("Next key", "Next value"));
                Check("tail " + i + " adds only a necessary separator", bytes.Length == expected);
            }
        }
    }

    private static void BoundedLoad(string root)
    {
        using (var reader = new StringReader("12345678\r\n123456789\n\nA\rB\r\n")) {
            var lines = new BoundedCacheLineReader(reader, 8);
            bool tooLong;
            Check("bounded reader accepts exact limit", lines.ReadLine(out tooLong) == "12345678" && !tooLong);
            Check("bounded reader skips only over-limit line", lines.ReadLine(out tooLong) == "" && tooLong);
            Check("bounded reader preserves empty line", lines.ReadLine(out tooLong) == "" && !tooLong);
            Check("bounded reader handles CR-only delimiter", lines.ReadLine(out tooLong) == "A" && !tooLong);
            Check("bounded reader handles CRLF delimiter", lines.ReadLine(out tooLong) == "B" && !tooLong);
            Check("bounded reader reaches EOF", lines.ReadLine(out tooLong) == null && !tooLong);
        }
        using (var reader = new StringReader(new string('X', 16 * 1024 - 1) + "\r\nNext")) {
            var lines = new BoundedCacheLineReader(reader, 16 * 1024 - 1);
            bool tooLong;
            Check("bounded reader handles CRLF across buffer", lines.ReadLine(out tooLong).Length == 16 * 1024 - 1 && !tooLong
                && lines.ReadLine(out tooLong) == "Next" && !tooLong);
        }
        using (var reader = new StringReader("Good\n123456789")) {
            var lines = new BoundedCacheLineReader(reader, 8);
            bool tooLong;
            Check("bounded reader returns line before oversized EOF", lines.ReadLine(out tooLong) == "Good" && !tooLong
                && lines.ReadLine(out tooLong) == "" && tooLong
                && lines.ReadLine(out tooLong) == null && !tooLong);
        }

        using (var f = new Fixture(root, "oversized-load")) {
            string[] keys = { "RenPy [player] {b}line{/b}", "RPG \\c[2]line\\v[1]",
                "Unity <color=red>line</color>", "Godot [color=red]line[/color]" };
            string[] values = { "译 [player] {b}文{/b}", "译 \\c[2]文\\v[1]",
                "译 <color=red>文</color>", "译 [color=red]文[/color]" };
            using (var writer = new StreamWriter(f.Path, false, new UTF8Encoding(false))) {
                writer.Write(CacheCodec.EncodeLine(keys[0], values[0]));
                writer.Write(CacheCodec.EncodeLine(keys[1], values[1]));
                writer.Write(new string('Q', CacheCodec.MaxLineChars + 1));
                writer.Write('\n');
                writer.Write(CacheCodec.EncodeLine(keys[2], values[2]));
                writer.Write(CacheCodec.EncodeLine(keys[3], values[3]));
            }
            var previous = Console.Error;
            using (var diagnostic = new StringWriter()) {
                int loaded;
                try {
                    Console.SetError(diagnostic);
                    loaded = f.Cache.Load();
                } finally {
                    Console.SetError(previous);
                }
                bool allPresent = loaded == 4 && f.Cache.Size == 4;
                for (int i = 0; i < keys.Length; i++) allPresent &= f.Cache.Get(keys[i]) == values[i];
                Check("oversized cache row does not block four engine records", allPresent);
                Check("oversized cache row records bounded diagnostic", diagnostic.ToString().Contains("load-line-too-large")
                    && diagnostic.ToString().Contains(f.Path));
            }
        }
    }

    private static int Main()
    {
        string root = Path.Combine(Path.GetTempPath(), "dst_cache_persistence_" + Guid.NewGuid().ToString("N"));
        Directory.CreateDirectory(root);
        try {
            SuccessfulBatch(root);
            LiveBatchFlushesOnce(root);
            MarkupAndOverwrites(root);
            PartialWriteAndTails(root);
            BoundedLoad(root);
            FailedBatch(root, "write-first", 1, false, false);
            FailedBatch(root, "write-third", 3, false, false);
            FailedBatch(root, "write-third-with-known", 3, false, true);
            FailedBatch(root, "flush-final", 0, true, false);
            FailedBatch(root, "flush-final-with-known", 0, true, true);
        } finally {
            // 每个夹具只创建一个直接子文件；出现非预期内容时，
            // 不得递归删除临时根目录或跟随链接。
            foreach (string path in Directory.GetFiles(root)) File.Delete(path);
            Directory.Delete(root, false);
        }
        Console.WriteLine("Cache persistence: PASS=" + passed + " FAIL=" + failed);
        return failed == 0 ? 0 : 1;
    }
}
