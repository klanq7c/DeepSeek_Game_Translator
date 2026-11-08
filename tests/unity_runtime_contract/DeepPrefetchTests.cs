using System;
using System.Collections.Generic;
using System.Linq;
using System.Text.Json;
using System.Threading.Tasks;
using D = DeepPrefetchSeam;

internal static class DeepPrefetchTests
{
    private static D.Object[] Assets(int count)
    {
        return Enumerable.Range(1, count).Select(id => (D.Object)new D.TextAsset { Id = id }).ToArray();
    }

    private static int Sample(D seam, string label, int budget = 260, int textBudget = 80)
    {
        seam.BeginSample();
        int previous = seam._deepScannedObjects.Count;
        seam.CollectDeepPrefetchCandidates(budget, textBudget);
        Console.WriteLine("[DEEP-SAMPLE] " + JsonSerializer.Serialize(new {
            scenario = label, discovered = seam.Discovered, discoveryCalls = seam.DiscoveryCalls,
            inspected = seam.Inspected, newlyProcessed = seam._deepScannedObjects.Count - previous,
            queued = seam._deepPrefetchQueue.Count
        }));
        return seam.Inspected;
    }

    public static void Run(Action<string, bool> check)
    {
        var seen = new D { Assets = Assets(10000) };
        foreach (var obj in seen.Assets) seen._deepScannedObjects.Add(obj.Id);
        check("deep known objects consume the inspection budget", Sample(seen, "known-10000") <= 260);
        check("deep known objects remain deduplicated", seen._deepScannedObjects.Count == 10000 && seen._deepPrefetchQueue.Count == 0);
        check("deep discovery remains distinct from bounded inspection", seen.Discovered == 10000);

        var inactive = new D { Behaviours = Enumerable.Range(1, 10000).Select(id => new D.MonoBehaviour { Id = id, Active = false }).ToArray() };
        check("deep inactive components consume the inspection budget", Sample(inactive, "inactive-10000") <= 260);
        check("deep inactive components are not marked processed", inactive._deepScannedObjects.Count == 0);
        var nulls = new D { Assets = new D.Object[10000] };
        check("deep null entries consume the inspection budget", Sample(nulls, "null-10000") <= 260);

        var tail = new D { Assets = Assets(10000) };
        foreach (var obj in tail.Assets.Take(9999)) tail._deepScannedObjects.Add(obj.Id);
        ((D.TextAsset)tail.Assets[9999]).Content = "Tail dialogue.";
        bool bounded = true;
        int passes = 0;
        for (; passes < 40 && tail._deepPrefetchQueue.Count == 0; passes++)
        {
            tail.BeginSample();
            tail.CollectDeepPrefetchCandidates(260, 80);
            bounded &= tail.Inspected <= 260;
        }
        check("deep repeated passes keep the inspection budget", bounded);
        check("deep cursor eventually reaches unseen tail", tail._deepPrefetchQueue.SequenceEqual(new[] { "Tail dialogue." }));
        Console.WriteLine("[DEEP-SAMPLE] " + JsonSerializer.Serialize(new { scenario = "known-prefix-tail", passes }));

        var fair = new D {
            Assets = Assets(10000),
            Scripts = new[] { new D.ScriptableObject { Id = 20001, Content = "Script dialogue." } },
            Behaviours = new[] { new D.MonoBehaviour { Id = 30001, Content = "Component dialogue." } }
        };
        bounded = true;
        for (int i = 0; i < 3; i++) {
            fair.BeginSample(); fair.CollectDeepPrefetchCandidates(260, 80);
            bounded &= fair.Inspected <= 260;
        }
        check("deep category rotation respects one shared inspection budget", bounded);
        check("deep busy TextAssets do not starve scripts and components", fair._deepPrefetchQueue.Contains("Script dialogue.") && fair._deepPrefetchQueue.Contains("Component dialogue."));

        var capped = new D { Assets = Assets(1000) };
        foreach (D.TextAsset obj in capped.Assets) obj.Content = "Dialogue line " + obj.Id + ".";
        Sample(capped, "text-limit");
        check("deep text limit still stops at 80 candidates", capped._deepPrefetchQueue.Count == 80 && capped._deepScannedObjects.Count == 80 && capped.Inspected == 80);
        var tiny = new D { Assets = Assets(100) };
        check("deep object budget is independent from text yield", Sample(tiny, "empty-text", 7) == 7 && tiny._deepScannedObjects.Count == 7);
        var zero = new D { Assets = Assets(1) };
        Sample(zero, "zero-object-budget", 0);
        check("deep zero object budget performs no discovery or marking", zero.DiscoveryCalls == 0 && zero._deepScannedObjects.Count == 0);
        Sample(zero, "zero-text-budget", 260, 0);
        check("deep zero text budget performs no discovery or marking", zero.DiscoveryCalls == 0 && zero._deepScannedObjects.Count == 0);

        var dynamic = new D { Assets = Assets(300) };
        dynamic.CollectDeepPrefetchCandidates(260, 80);
        dynamic.Assets = new[] { (D.Object)new D.TextAsset { Id = 20001, Content = "New object after shrink." } };
        dynamic.CollectDeepPrefetchCandidates(260, 80);
        check("deep changed array length keeps cursor valid", dynamic._deepPrefetchQueue.Contains("New object after shrink."));

        var pruned = new D { Assets = Assets(10000) };
        var inspectedIds = new HashSet<int>();
        bounded = true;
        for (int i = 0; i < 39; i++) {
            pruned.BeginSample(); pruned.CollectDeepPrefetchCandidates(260, 80);
            inspectedIds.UnionWith(D.Object.ReadIds);
            bounded &= pruned.Inspected <= 260;
            // 模拟原有容量回收；游标不应随 visited 集合回到数组开头。
            if (pruned._deepScannedObjects.Count > 4096) pruned._deepScannedObjects.Clear();
        }
        check("deep visited-set pruning retains bounded forward progress", bounded && inspectedIds.Count == 10000);
        pruned._deepPrefetchScanWindow.Reset();
        pruned.BeginSample(); pruned.CollectDeepPrefetchCandidates(1, 80);
        check("deep scene reset restarts category and object cursors", D.Object.ReadIds.SequenceEqual(new[] { 1 }));
        var activated = new D { Behaviours = new[] { new D.MonoBehaviour { Id = 40001, Active = false, Content = "Activated dialogue." } } };
        activated.CollectDeepPrefetchCandidates(260, 80);
        activated.Behaviours[0].Active = true;
        activated.CollectDeepPrefetchCandidates(260, 80);
        check("deep inactive-to-active transition remains discoverable", activated._deepPrefetchQueue.Contains("Activated dialogue."));

        var tick = new D { Assets = Assets(1), _deepPrefetchEnabled = new D.ConfigEntry<bool>(false) };
        D.Time.realtimeSinceStartup = 10f;
        tick.RunDeepPrefetchTick();
        check("deep disabled runtime performs no discovery", tick.DiscoveryCalls == 0 && tick._deepPrefetchActive == 0);
        tick._deepPrefetchEnabled.Value = true; tick.Eco = true; tick.RunDeepPrefetchTick();
        check("deep eco runtime performs no discovery", tick.DiscoveryCalls == 0);
        tick.Eco = false; tick.Pending = true; tick.RunDeepPrefetchTick();
        check("deep live work defers scan and releases active flag", tick.DiscoveryCalls == 0 && tick._deepPrefetchActive == 0);
        tick.Pending = false; tick._deepPrefetchActive = 1; tick.RunDeepPrefetchTick();
        check("deep in-flight worker prevents overlapping scan", tick.DiscoveryCalls == 0);
        tick._deepPrefetchActive = 0; tick._lastDeepPrefetchRealtime = 9f; tick.RunDeepPrefetchTick();
        check("deep scan interval remains enforced", tick.DiscoveryCalls == 0);
        tick._lastDeepPrefetchRealtime = -999f; D.Time.realtimeSinceStartup = 0.5f; tick.RunDeepPrefetchTick();
        check("deep initial delay remains enforced", tick.DiscoveryCalls == 0);
        D.Time.realtimeSinceStartup = 10f;
        ((D.TextAsset)tick.Assets[0]).ThrowOnRead = true;
        tick.RunDeepPrefetchTick();
        check("deep scan exception is diagnosed and releases active flag", tick.Errors.Count == 1 && tick._deepPrefetchActive == 0);

        CheckWorker(check, false).GetAwaiter().GetResult();
        CheckWorker(check, true).GetAwaiter().GetResult();
    }

    private static async Task CheckWorker(Action<string, bool> check, bool disable)
    {
        var worker = new D {
            _deepPrefetchEnabled = new D.ConfigEntry<bool>(true),
            _deepPrefetchActive = 1,
            Provider = new TaskCompletionSource<bool>()
        };
        for (int i = 0; i < 20; i++) worker._deepPrefetchQueue.Enqueue("Queued dialogue " + i + ".");
        Task pending = worker.ProcessDeepPrefetchQueueAsync();
        check("deep worker sends 16 texts with existing dialogue priority; disable=" + disable,
            worker.Batches.Count == 1 && worker.Batches[0].Length == 16 && worker.Priorities[0] == "dialogue");
        if (disable) worker._deepPrefetchEnabled.Value = false;
        else worker.Pending = true;
        worker.Provider.SetResult(true);
        await pending;
        check("deep worker yields queued remainder and releases active flag; disable=" + disable,
            worker.Batches.Count == 1 && worker._deepPrefetchQueue.Count == 4 && worker._deepPrefetchActive == 0);
        worker._deepPrefetchEnabled.Value = true;
        worker.Pending = false;
        worker.Provider = new TaskCompletionSource<bool>();
        worker._deepPrefetchActive = 1;
        pending = worker.ProcessDeepPrefetchQueueAsync();
        worker.Provider.SetException(new InvalidOperationException("fixture provider failure"));
        bool faulted = false;
        try { await pending; } catch (InvalidOperationException ex) { faulted = ex.Message == "fixture provider failure"; }
        check("deep provider failure remains faulted and releases active flag; disable=" + disable, faulted && worker._deepPrefetchActive == 0);
    }
}
