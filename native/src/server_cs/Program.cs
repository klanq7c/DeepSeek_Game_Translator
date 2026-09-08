using System;
using System.Collections.Generic;
using System.IO;
using System.Net;
using System.Net.Sockets;
using System.Text;
using System.Threading;
using System.Threading.Tasks;
using DstCore;

namespace DstServerCs
{
    /*
     * Program —— dst_server 的 C# 平行实现入口。
     *
     * 与 C 版本（native/src/server）保持同一契约：
     *   - 仅监听 127.0.0.1，路由/响应字段/错误码逐一对齐；
     *   - Connection: close 单请求模型，2MB 请求上限，128 连接上限；
     *   - 本地缓存优先，实时批量 + 后台预热双队列，前台优先；
     *   - /shutdown 排空在途连接后以退出码 0 结束。
     *
     * 实现差异：连接层用 async I/O（空闲连接不占线程，线程数测试天然满足）；
     * 合批窗口在锁外等待，保证入队路径不被阻塞。
     */

    internal static class Program
    {
        private const int PortDefault = 19999;
        private const int RecvInitial = 64 * 1024;
        private const int MaxReq = 2 * 1024 * 1024;
        private const int RecvTimeoutMs = 5000;
        private const int ConnectionLimit = 128;
        private const int BufferPoolLimit = 32;
        private const int AsyncQueueLimit = 65536;
        private const int AsyncByteLimit = 32 * 1024 * 1024;
        private const int LiveByteLimit = 32 * 1024 * 1024;
        private const int BatchMax = 48;
        private const int BatchCharBudget = 9600;
        private const int AsyncCoalesceMs = 25;
        private const int LiveCoalesceMs = 35;
        private const int PathCap = 8192;

        private static volatile bool stopping;
        private static int activeConnections;
        private static long connectionRejections;
        private static long connectionThreadFailures; // async 模型无连接线程（契约字段恒 0）
        private static long workerStartFailures;
        private static DateTime started = DateTime.UtcNow;

        private static CacheStore cache;
        private static ApiConfig api;
        private static SemaphoreSlim apiGate;
        private static TcpListener listener;

        // ---- 请求缓冲池：只缓存 64KiB 初始块，扩容块用后即弃（与 C 对齐） ----
        private static readonly object poolLock = new object();
        private static readonly Stack<byte[]> bufferPool = new Stack<byte[]>();
        private static long poolHits;
        private static long poolMisses;

        private static byte[] PoolAcquire()
        {
            lock (poolLock) {
                if (bufferPool.Count > 0) {
                    Interlocked.Increment(ref poolHits);
                    return bufferPool.Pop();
                }
            }
            Interlocked.Increment(ref poolMisses);
            return new byte[RecvInitial];
        }

        private static void PoolRelease(byte[] buffer)
        {
            if (buffer == null || buffer.Length != RecvInitial) return;
            lock (poolLock) {
                if (bufferPool.Count < BufferPoolLimit) bufferPool.Push(buffer);
            }
        }

        // ---- 后台预热队列：FIFO + 文本去重 + 字节预算（与 async_* 对齐） ----
        private sealed class AsyncJob
        {
            public string Text;
            public string Prev;
            public long Bytes;
        }

        private static readonly object asyncLock = new object();
        private static readonly Queue<AsyncJob> asyncQueue = new Queue<AsyncJob>();
        private static readonly HashSet<string> asyncKnown = new HashSet<string>();
        private static long asyncMemoryBytes;

        private static bool AsyncEnqueueMissCtx(string text, string prev)
        {
            if (api == null || !api.Enabled || !TextRules.ShouldTranslate(text)) return false;
            if (cache.Contains(text)) return false;
            long bytes = 64 + (text.Length + 1) * 2
                + (!string.IsNullOrEmpty(prev) ? (prev.Length + 1) * 2 : 0);
            lock (asyncLock) {
                if (asyncQueue.Count >= AsyncQueueLimit || asyncKnown.Contains(text)) return false;
                if (asyncMemoryBytes > AsyncByteLimit - bytes) {
                    // 字节预算耗尽：拒绝入队并限频诊断（与 C 的 budget rejection 一致）。
                    long n = Interlocked.Increment(ref asyncBudgetRejections);
                    if (n <= 3 || (n & (n - 1)) == 0) {
                        Console.Error.WriteLine(
                            "[http] async queue byte budget rejected #" + n);
                    }
                    return false;
                }
                asyncKnown.Add(text);
                asyncQueue.Enqueue(new AsyncJob { Text = text, Prev = prev, Bytes = bytes });
                asyncMemoryBytes += bytes;
                Monitor.Pulse(asyncLock);
                return true;
            }
        }

        private static long asyncBudgetRejections;

        private static void AsyncWorkerLoop()
        {
            while (!stopping) {
                var batch = AsyncPopBatch();
                if (batch == null) continue;
                try {
                    AsyncTranslatePending(batch);
                } finally {
                    // 无论翻译成败都必须收尾：否则字节预算只增不减、去重集合永不释放，
                    // 累计到 32MB 后所有 /prefetch 都会被永久拒绝（C 的 async_finish_jobs）。
                    AsyncFinishJobs(batch);
                }
            }
        }

        /* 一批 job 处理完毕：归还字节预算并从去重集合摘除，使同一文本在失败后
           可以再次排队重译（与 C 的 async_finish_jobs 对齐）。 */
        private static void AsyncFinishJobs(List<AsyncJob> jobs)
        {
            lock (asyncLock) {
                foreach (var job in jobs) {
                    asyncKnown.Remove(job.Text);
                    asyncMemoryBytes = asyncMemoryBytes >= job.Bytes
                        ? asyncMemoryBytes - job.Bytes
                        : 0;
                }
            }
        }

        private static List<AsyncJob> AsyncPopBatch()
        {
            var jobs = new List<AsyncJob>();
            // 与 C 的 async_pop_batch 一致：合批窗口持排他锁——保证先入队的
            // 相邻文本进入同一段上下文批次，其他 worker 在窗口内不会截走任务。
            lock (asyncLock) {
                while (asyncQueue.Count == 0 && !stopping) {
                    Monitor.Wait(asyncLock, 1000);
                }
                if (asyncQueue.Count == 0 || stopping) return null;
                jobs.Add(asyncQueue.Dequeue());
                long chars = jobs[0].Text.Length;
                while (jobs.Count < BatchMax) {
                    if (asyncQueue.Count == 0) {
                        // 合批窗口：等待窗口内可能到达的新任务（持锁等待）。
                        Monitor.Wait(asyncLock, AsyncCoalesceMs);
                    }
                    if (asyncQueue.Count == 0) break;
                    var next = asyncQueue.Peek();
                    if (chars + next.Text.Length > BatchCharBudget) break;
                    asyncQueue.Dequeue();
                    jobs.Add(next);
                    chars += next.Text.Length;
                }
            }
            return jobs;
        }

        private static void AsyncTranslatePending(List<AsyncJob> jobs)
        {
            var pending = new List<AsyncJob>();
            foreach (var job in jobs) {
                if (cache.Contains(job.Text)) continue;
                if (api != null && api.Enabled) pending.Add(job);
            }
            if (pending.Count == 0) return;

            var texts = new string[pending.Count];
            var prevs = new string[pending.Count];
            for (int i = 0; i < pending.Count; i++) {
                texts[i] = pending[i].Text;
                prevs[i] = pending[i].Prev ?? "";
            }

            ApiClient.WaitForForegroundAsync().Wait();
            if (stopping) return;
            // 二分重试：范围失败时对半拆分定位最小失败单元（≤4 条逐条兜底），
            // 取代旧的"整批失败→逐条重发"降级。NULL 槽位计 miss，绝不伪装成功。
            var translated = ApiClient.TranslateSplitRetry(api, texts, prevs, apiGate).Result;
            if (translated == null) return;
            PersistResolved(texts, translated);
        }

        private static void PersistResolved(string[] keys, string[] values)
        {
            var k = new List<string>();
            var v = new List<string>();
            for (int i = 0; i < keys.Length; i++) {
                if (TextRules.IsResolvedTranslation(keys[i], values[i])) {
                    k.Add(keys[i]);
                    v.Add(values[i]);
                }
            }
            if (k.Count > 0) cache.SetManyPersist(k.ToArray(), v.ToArray());
        }

        // ---- 实时队列：单条请求合批（与 live_* 对齐） ----
        private sealed class LiveJob
        {
            public string Text;
            public long Bytes;
            public TaskCompletionSource<string> Done =
                new TaskCompletionSource<string>(TaskCreationOptions.RunContinuationsAsynchronously);
        }

        private static readonly object liveLock = new object();
        private static readonly Queue<LiveJob> liveQueue = new Queue<LiveJob>();
        private static long liveMemoryBytes;
        private static long liveBudgetRejections;

        /* 与 C 的 live_translate_batched 对齐：从入队到拿到结果的整个等待期都
           标记为前台，使后台预热批次在实时请求排队时就让路，而不是等到远程
           调用真正发出。预算耗尽按 miss 处理并限频诊断。 */
        private static async Task<string> LiveTranslateBatched(string text)
        {
            var job = new LiveJob { Text = text, Bytes = (text.Length + 1) * 2 + 64 };
            lock (liveLock) {
                // 关停后 live worker 已退出并清空队列；此时再入队没有人会完成它。
                if (stopping) return null;
                if (liveMemoryBytes + job.Bytes > LiveByteLimit) {
                    long n = Interlocked.Increment(ref liveBudgetRejections);
                    if (n <= 3 || (n & (n - 1)) == 0) {
                        Console.Error.WriteLine(
                            "[http] live queue byte budget rejected #" + n
                            + " (used=" + liveMemoryBytes + ", limit=" + LiveByteLimit + ")");
                    }
                    return null;
                }
                liveMemoryBytes += job.Bytes;
                liveQueue.Enqueue(job);
                Monitor.Pulse(liveLock);
            }
            ApiClient.ForegroundEnter();
            try {
                return await job.Done.Task.ConfigureAwait(false);
            } finally {
                ApiClient.ForegroundLeave();
            }
        }

        /* 与 C 的 live_pop_batch 对齐：队空时阻塞等待；弹出首个后在合批窗口内
           继续攒，受条目数与字符预算约束。持锁等待（Monitor.Wait 会临时释放锁），
           不再用信号量计数——信号量许可数与队列长度会在合批时失同步。 */
        private static List<LiveJob> LivePopBatch()
        {
            var jobs = new List<LiveJob>();
            lock (liveLock) {
                while (liveQueue.Count == 0 && !stopping) {
                    Monitor.Wait(liveLock, 1000);
                }
                if (liveQueue.Count == 0) return null;
                jobs.Add(liveQueue.Dequeue());
                long chars = jobs[0].Text.Length;
                while (jobs.Count < BatchMax) {
                    if (liveQueue.Count == 0) {
                        Monitor.Wait(liveLock, LiveCoalesceMs);
                    }
                    if (liveQueue.Count == 0) break;
                    var next = liveQueue.Peek();
                    if (chars + next.Text.Length > BatchCharBudget) break;
                    liveQueue.Dequeue();
                    jobs.Add(next);
                    chars += next.Text.Length;
                }
            }
            return jobs;
        }

        private static void LiveWorkerLoop()
        {
            while (!stopping) {
                var jobs = LivePopBatch();
                if (jobs == null) continue;
                LiveTranslatePending(jobs);
            }
            // 关停后队列里残留的实时任务必须以 miss 收尾，否则等待方会永久挂起。
            List<LiveJob> leftover;
            lock (liveLock) {
                leftover = new List<LiveJob>(liveQueue);
                liveQueue.Clear();
                foreach (var job in leftover) liveMemoryBytes -= job.Bytes;
            }
            foreach (var job in leftover) job.Done.TrySetResult(null);
        }

        /* 与 C 的 live_translate_jobs 对齐：实时路径本身就是前台（前台标记由
           LiveTranslateBatched 在调用方一侧持有），这里只负责调远程、回写缓存、
           唤醒等待方。失败条目转入后台队列补译。 */
        private static void LiveTranslatePending(List<LiveJob> jobs)
        {
            var texts = new string[jobs.Count];
            for (int i = 0; i < jobs.Count; i++) texts[i] = jobs[i].Text;

            string[] results = null;
            if (api != null && api.Enabled && !stopping) {
                // 本方法运行在专用线程上，同步等待不会占用线程池线程。
                if (jobs.Count == 1) {
                    var one = ApiClient.TranslateSingle(api, texts[0], apiGate).Result;
                    if (one != null) results = new[] { one };
                } else {
                    results = ApiClient.TranslateBatch(api, texts, apiGate).Result;
                }
            }

            lock (liveLock) {
                foreach (var job in jobs) liveMemoryBytes -= job.Bytes;
            }
            for (int i = 0; i < jobs.Count; i++) {
                string value = results != null ? results[i] : null;
                if (value != null && TextRules.IsResolvedTranslation(texts[i], value)) {
                    var persisted = cache.SetManyPersist(
                        new[] { texts[i] }, new[] { value });
                    if (persisted.Accepted > 0) {
                        jobs[i].Done.TrySetResult(value);
                        continue;
                    }
                }
                // 实时批量失败的条目转入后台队列（与 C 的 live 路径一致）：
                // 下次由后台批量+二分重试填充缓存，而不是让英文永久漏出。
                AsyncEnqueueMissCtx(texts[i], null);
                jobs[i].Done.TrySetResult(null);
            }
        }

        // ---- translate_value：单条文本的核心翻译决策（与 http.c 对齐） ----
        private sealed class TranslateOutcome
        {
            public string Value;
            public string Source;
        }

        /* 全程异步：连接处理运行在线程池上，这里若同步阻塞等待实时结果，
           128 条并发连接会把线程池占满并饿死自己所等待的任务。 */
        private static async Task<TranslateOutcome> TranslateValueAsync(
            string text, bool cacheOnly, bool queueMiss)
        {
            var v = cache.Get(text);
            if (v != null) return new TranslateOutcome { Value = v, Source = "cache" };
            if (!TextRules.ShouldTranslate(text)) {
                return new TranslateOutcome { Value = text, Source = "pass" };
            }
            if (cacheOnly) {
                bool queued = queueMiss && AsyncEnqueueMissCtx(text, null);
                return new TranslateOutcome { Value = text, Source = queued ? "queued" : "miss" };
            }
            if (api != null && api.Enabled) {
                var live = await LiveTranslateBatched(text).ConfigureAwait(false);
                if (live != null && TextRules.IsResolvedTranslation(text, live)) {
                    var persisted = cache.SetManyPersist(new[] { text }, new[] { live });
                    if (persisted.Accepted > 0) {
                        return new TranslateOutcome { Value = live, Source = "api_batch" };
                    }
                }
            }
            return new TranslateOutcome { Value = text, Source = "miss" };
        }

        // ---- 请求模型与路由实现 ----

        private sealed class Request
        {
            public string Method;
            public string Path;
            public string Query;
            public string Body;
            public object Dom;
            public bool OriginPresent;
            public string CorsOrigin = "null";
        }

        /* 与 C 的 json_array_value 对齐：字段是单个字符串 → 单元素列表；字段是
           全字符串数组 → 列表；含任何非字符串元素（数字/null/对象）或字段缺失 →
           空列表。C# 侧此前用硬转换 (string)，遇到 [1,2] 或 [null] 会抛出
           InvalidCastException/ArgumentNullException 并静默断连，而不是回 400。 */
        private static List<string> TopStringList(object dom, string key)
        {
            var list = new List<string>();
            var obj = dom as Dictionary<string, object>;
            if (obj == null) return list;
            object raw;
            if (!obj.TryGetValue(key, out raw) || raw == null) return list;
            var one = raw as string;
            if (one != null) {
                list.Add(one);
                return list;
            }
            var arr = raw as List<object>;
            if (arr == null) return list;
            foreach (var item in arr) {
                var s = item as string;
                if (s == null) {
                    list.Clear();
                    return list;
                }
                list.Add(s);
            }
            return list;
        }

        private static async Task<string> RespTranslateSingleAsync(string text, bool cacheOnly)
        {
            var outcome = await TranslateValueAsync(text, cacheOnly, cacheOnly).ConfigureAwait(false);
            var sb = new StringBuilder(64);
            sb.Append("{\"translation\":").Append(Json.Escape(outcome.Value));
            sb.Append(",\"translated_text\":").Append(Json.Escape(outcome.Value));
            sb.Append(",\"source\":\"").Append(outcome.Source).Append("\"}");
            return sb.ToString();
        }

        private static async Task<string> RespBatchAsync(Request req)
        {
            var texts = TopStringList(req.Dom, "texts");
            if (texts.Count == 0) {
                // texts 为空时尝试单 text 字段（与 C 的 op_batch 入参兼容层一致）。
                var one = Json.TopString(req.Dom, "text");
                if (one != null) texts.Add(one);
            }
            bool cacheOnly = Json.TopBool(req.Dom, "cache_only");
            bool single = req.Path == "/translate" && texts.Count == 1;
            if (single) return await RespTranslateSingleAsync(texts[0], cacheOnly).ConfigureAwait(false);
            if (texts.Count == 0) return null; // 由路由层回 400

            int n = texts.Count;
            var results = new string[n];
            var sources = new string[n];

            // 去重：同文本只占一个提供商槽位，翻译完成后把结果回填到重复项。
            var rootOf = new int[n];
            var seenIndex = new Dictionary<string, int>();
            var missIdx = new List<int>();
            for (int i = 0; i < n; i++) {
                var t = texts[i];
                int j;
                if (seenIndex.TryGetValue(t, out j)) {
                    rootOf[i] = j;
                    continue;
                }
                seenIndex[t] = i;
                rootOf[i] = i;
                var v = cache.Get(t);
                if (v != null) {
                    results[i] = v;
                    sources[i] = "cache";
                } else if (!TextRules.ShouldTranslate(t)) {
                    results[i] = t;
                    sources[i] = "pass";
                } else {
                    missIdx.Add(i);
                    sources[i] = "miss";
                }
            }

            if (missIdx.Count > 0) {
                if (cacheOnly) {
                    foreach (var i in missIdx) {
                        bool queued = AsyncEnqueueMissCtx(texts[i], null);
                        sources[i] = queued ? "queued" : "miss";
                        results[i] = texts[i];
                    }
                } else if (api != null && api.Enabled) {
                    ApiClient.ForegroundEnter();
                    try {
                        // 切块并按字符预算分组，多块经通道池并行发出（与 C 的
                        // foreground 多通道行为一致）。
                        var chunks = new List<int[]>();
                        var current = new List<int>();
                        long chars = 0;
                        foreach (var i in missIdx) {
                            var t = texts[i];
                            if (current.Count > 0
                                && (current.Count >= BatchMax
                                    || chars + t.Length > BatchCharBudget)) {
                                chunks.Add(current.ToArray());
                                current.Clear();
                                chars = 0;
                            }
                            current.Add(i);
                            chars += t.Length;
                        }
                        if (current.Count > 0) chunks.Add(current.ToArray());

                        var chunkTasks = new Task[chunks.Count];
                        for (int c = 0; c < chunks.Count; c++) {
                            var idxs = chunks[c];
                            chunkTasks[c] = Task.Run(async () => {
                                var batch = new string[idxs.Length];
                                for (int k = 0; k < idxs.Length; k++) {
                                    batch[k] = texts[idxs[k]];
                                }
                                // 二分重试：批量失败时对半拆分定位最小失败单元。
                                var translated = await ApiClient.TranslateSplitRetry(
                                    api, batch, null, apiGate).ConfigureAwait(false);
                                for (int k = 0; k < idxs.Length; k++) {
                                    if (translated[k] != null
                                        && TextRules.IsResolvedTranslation(
                                            batch[k], translated[k])) {
                                        results[idxs[k]] = translated[k];
                                        sources[idxs[k]] = "api_batch";
                                    }
                                }
                            });
                        }
                        // await 而不是 Task.WaitAll：不能在线程池线程上同步等待
                        // 排在同一个线程池里的分块任务。
                        await Task.WhenAll(chunkTasks).ConfigureAwait(false);
                    } finally {
                        ApiClient.ForegroundLeave();
                    }
                    // 与 C 的 live_translate_jobs 一致：实时路径仍未解析的条目排入
                    // 后台补译，让缓存能为下次填充，而不是让英文永久漏出。
                    foreach (var i in missIdx) {
                        if (results[i] == null) AsyncEnqueueMissCtx(texts[i], null);
                    }
                }
                foreach (var i in missIdx) {
                    if (results[i] == null) results[i] = texts[i];
                }
            }

            // 把重复项的最终结果回填（翻译已完成，root 的 results 此时已就绪）。
            for (int i = 0; i < n; i++) {
                if (rootOf[i] == i) continue;
                results[i] = results[rootOf[i]];
                sources[i] = sources[rootOf[i]];
            }

            // 仅把解析为有效翻译的结果回写缓存（不缓存 miss/queued 透传原文）。
            // 只遍历去重根：重复项与根同值，重复回写只会虚增 accepted 计数。
            var pk = new List<string>();
            var pv = new List<string>();
            for (int i = 0; i < n; i++) {
                if (rootOf[i] != i) continue;
                if (sources[i] == "api_batch"
                    && TextRules.IsResolvedTranslation(texts[i], results[i])) {
                    pk.Add(texts[i]);
                    pv.Add(results[i]);
                }
            }
            if (pk.Count > 0) cache.SetManyPersist(pk.ToArray(), pv.ToArray());

            var seenKey = new HashSet<string>();
            var map = new StringBuilder(n * 16);
            map.Append('{');
            bool first = true;
            for (int i = 0; i < n; i++) {
                var t = texts[i];
                if (!seenKey.Add(t)) continue;
                if (!first) map.Append(',');
                first = false;
                map.Append(Json.Escape(t)).Append(':').Append(Json.Escape(results[i]));
            }
            map.Append('}');

            var sb = new StringBuilder(n * 24);
            sb.Append("{\"translations\":").Append(map);
            sb.Append(",\"results\":[");
            for (int i = 0; i < n; i++) {
                if (i > 0) sb.Append(',');
                sb.Append(Json.Escape(results[i]));
            }
            sb.Append("],\"sources\":[");
            for (int i = 0; i < n; i++) {
                if (i > 0) sb.Append(',');
                sb.Append('"').Append(sources[i]).Append('"');
            }
            sb.Append("]}");
            return sb.ToString();
        }

        private static string RespPrefetch(Request req)
        {
            var texts = TopStringList(req.Dom, "texts");
            if (texts.Count == 0) {
                var one = Json.TopString(req.Dom, "text");
                if (one != null) texts.Add(one);
            }
            var prevs = TopStringList(req.Dom, "prevs");
            bool usePrevs = prevs.Count == texts.Count; // 失配整体忽略（与 op_prefetch 一致）
            int queued = 0;
            for (int i = 0; i < texts.Count; i++) {
                var p = usePrevs ? prevs[i] : null;
                if (AsyncEnqueueMissCtx(texts[i], p)) queued++;
            }
            return "{\"status\":\"queued\",\"queued\":" + queued + "}";
        }

        private static string RespLookup(Request req)
        {
            var texts = TopStringList(req.Dom, "texts");
            var sb = new StringBuilder(64);
            sb.Append("{\"hits\":{");
            int hits = 0;
            foreach (var t in texts) {
                var v = cache.Get(t);
                if (v == null) continue;
                if (hits > 0) sb.Append(',');
                sb.Append(Json.Escape(t)).Append(':').Append(Json.Escape(v));
                hits++;
            }
            sb.Append("},\"hit_count\":").Append(hits);
            sb.Append(",\"miss_count\":").Append(texts.Count - hits).Append('}');
            return sb.ToString();
        }

        private static string RespImport(Request req)
        {
            var entries = (List<object>)Json.TopArray(req.Dom, "entries")
                ?? new List<object>();
            var keys = new List<string>();
            var values = new List<string>();
            foreach (var e in entries) {
                var obj = e as Dictionary<string, object>;
                if (obj == null) continue;
                var k = obj.TryGetValue("key", out var kk) ? kk as string : null;
                var v = obj.TryGetValue("value", out var vv) ? vv as string : null;
                if (!string.IsNullOrEmpty(k) && !string.IsNullOrEmpty(v)) {
                    keys.Add(k);
                    values.Add(v);
                }
            }
            var result = cache.SetManyPersist(keys.ToArray(), values.ToArray());
            string status = "ok";
            if (result.Status == "failed") status = "error";
            else if (result.Status == "partial"
                     || (result.Accepted > 0 && result.Rejected > 0)) status = "partial";
            else if (result.Accepted == 0 && result.Rejected > 0) status = "rejected";
            return "{\"status\":\"" + status + "\",\"imported\":" + result.Persisted
                + ",\"accepted\":" + result.Accepted
                + ",\"rejected\":" + result.Rejected
                + ",\"memory_only\":" + (result.Accepted - result.Persisted) + "}";
        }

        private static string RespDump()
        {
            var snap = cache.Snapshot();
            var sb = new StringBuilder(64);
            sb.Append("{\"cache\":{");
            for (int i = 0; i < snap.Length; i++) {
                if (i > 0) sb.Append(',');
                sb.Append(Json.Escape(snap[i].Key)).Append(':')
                  .Append(Json.Escape(snap[i].Value));
            }
            sb.Append("},\"count\":").Append(snap.Length).Append('}');
            return sb.ToString();
        }

        private static string RespExport()
        {
            var snap = cache.Snapshot();
            var sb = new StringBuilder(64);
            sb.Append("{\"entries\":[");
            for (int i = 0; i < snap.Length; i++) {
                if (i > 0) sb.Append(',');
                sb.Append("{\"key\":").Append(Json.Escape(snap[i].Key))
                  .Append(",\"value\":").Append(Json.Escape(snap[i].Value)).Append('}');
            }
            sb.Append("],\"count\":").Append(snap.Length).Append('}');
            return sb.ToString();
        }

        private static string RespHealth()
        {
            long asyncLen, asyncMem, liveLen, liveMem;
            lock (asyncLock) { asyncLen = asyncQueue.Count; asyncMem = asyncMemoryBytes; }
            lock (liveLock) { liveLen = liveQueue.Count; liveMem = liveMemoryBytes; }
            long poolCached;
            lock (poolLock) poolCached = bufferPool.Count;
            return "{\"status\":\"ok\",\"server\":\"dst_server_cs\",\"cache_size\":"
                + cache.Size
                + ",\"async_queue\":" + asyncLen
                + ",\"async_memory_bytes\":" + asyncMem
                + ",\"async_memory_limit\":" + AsyncByteLimit
                + ",\"live_queue\":" + liveLen
                + ",\"live_memory_bytes\":" + liveMem
                + ",\"live_memory_limit\":" + LiveByteLimit
                + ",\"request_buffer_pool_cached\":" + poolCached
                + ",\"request_buffer_pool_limit\":" + BufferPoolLimit
                + ",\"request_buffer_pool_hits\":" + Interlocked.Read(ref poolHits)
                + ",\"request_buffer_pool_misses\":" + Interlocked.Read(ref poolMisses)
                + ",\"active_connections\":" + Interlocked.CompareExchange(ref activeConnections, 0, 0)
                + ",\"connection_limit\":" + ConnectionLimit
                + ",\"connection_rejections\":" + Interlocked.Read(ref connectionRejections)
                + ",\"connection_thread_failures\":" + Interlocked.Read(ref connectionThreadFailures)
                + ",\"worker_start_failures\":" + Interlocked.Read(ref workerStartFailures)
                + ",\"api_enabled\":" + (api != null && api.Enabled ? "true" : "false")
                + ",\"runtime_cache_only\":false,\"uptime_seconds\":"
                + (long)(DateTime.UtcNow - started).TotalSeconds + "}";
        }

        // ---- HTTP 层：Origin 策略、请求读取、响应 ----

        private static bool OriginValueAllowed(string value)
        {
            if (value.Equals("null", StringComparison.OrdinalIgnoreCase)) return true;
            if (value.Equals("file://", StringComparison.OrdinalIgnoreCase)) return true;
            string[] prefixes = {
                "http://127.0.0.1", "https://127.0.0.1",
                "http://localhost", "https://localhost",
                "http://[::1]", "https://[::1]"
            };
            foreach (var prefix in prefixes) {
                if (!value.StartsWith(prefix, StringComparison.OrdinalIgnoreCase)) continue;
                var suffix = value.Substring(prefix.Length);
                if (suffix.Length == 0) return true;
                if (suffix[0] != ':' || suffix.Length == 1 || suffix.Length > 6) continue;
                uint port = 0;
                bool ok = true;
                for (int i = 1; i < suffix.Length; i++) {
                    if (suffix[i] < '0' || suffix[i] > '9') { ok = false; break; }
                    port = port * 10u + (uint)(suffix[i] - '0');
                }
                if (ok && port >= 1 && port <= 65535) return true;
            }
            return false;
        }

        private static bool IsBrowserAdminPath(string path)
        {
            return path == "/shutdown" || path == "/cache/import"
                || path == "/cache/dump" || path == "/cache/export";
        }

        private static string QueryGet(string query, string name)
        {
            if (query == null) return null;
            foreach (var part in query.Split('&')) {
                int eq = part.IndexOf('=');
                if (eq < 0) continue;
                if (part.Substring(0, eq) != name) continue;
                return UrlDecodeRange(part.Substring(eq + 1));
            }
            return null;
        }

        private static string UrlDecodeRange(string s)
        {
            var bytes = new List<byte>(s.Length);
            byte[] ascii = Encoding.ASCII.GetBytes(s);
            for (int i = 0; i < ascii.Length; i++) {
                if (ascii[i] == '+') bytes.Add(0x20);
                else if (ascii[i] == '%' && i + 2 < ascii.Length) {
                    int a = HexVal((char)ascii[i + 1]);
                    int b = HexVal((char)ascii[i + 2]);
                    if (a >= 0 && b >= 0) {
                        bytes.Add((byte)((a << 4) | b));
                        i += 2;
                    } else {
                        bytes.Add(ascii[i]);
                    }
                } else {
                    bytes.Add(ascii[i]);
                }
            }
            return Encoding.UTF8.GetString(bytes.ToArray());
        }

        private static int HexVal(char c)
        {
            if (c >= '0' && c <= '9') return c - '0';
            if (c >= 'a' && c <= 'f') return c - 'a' + 10;
            if (c >= 'A' && c <= 'F') return c - 'A' + 10;
            return -1;
        }

        private static byte[] BuildResponse(string cors, int status, string msg,
                                            string body, bool plain)
        {
            var sb = new StringBuilder(128);
            sb.Append("HTTP/1.1 ").Append(status).Append(' ').Append(msg).Append("\r\n")
              .Append("Content-Type: ")
              .Append(plain ? "text/plain; charset=utf-8" : "application/json; charset=utf-8")
              .Append("\r\n")
              .Append("Content-Length: ").Append(Encoding.UTF8.GetByteCount(body)).Append("\r\n")
              .Append("Access-Control-Allow-Origin: ").Append(cors).Append("\r\n")
              .Append("Access-Control-Allow-Methods: GET, POST, OPTIONS\r\n")
              .Append("Access-Control-Allow-Headers: Content-Type\r\n")
              .Append("Connection: close\r\n\r\n")
              .Append(body);
            return Encoding.UTF8.GetBytes(sb.ToString());
        }

        /* 连接层诊断：限频写 stderr，不含请求文本。对端提前断开、发送失败、
           关闭异常都属于外部传输边界，不能静默吞掉（AGENTS.md 失败透明规则）。 */
        private static long httpDiagCount;

        private static void HttpDiag(string stage, Exception ex)
        {
            long n = Interlocked.Increment(ref httpDiagCount);
            if (n <= 3 || (n & (n - 1)) == 0) {
                // 附带底层 SocketError（IOException 通常包着 SocketException），否则
                // 排障时只看到异常类型名，无法区分对端重置/超时/本地中止。
                var se = (ex as SocketException) ?? (ex != null ? ex.InnerException as SocketException : null);
                string detail = "";
                if (se != null) detail = "/" + se.SocketErrorCode;
                else if (ex != null && ex.InnerException != null) detail = "/" + ex.InnerException.GetType().Name;
                Console.Error.WriteLine("[http] " + stage + " failed #" + n
                    + (ex != null ? " (" + ex.GetType().Name + detail + ")" : "")
                    + (ex != null && n <= 3 ? ": " + ex.Message : ""));
            }
        }

        private static async Task SendAsync(Socket s, Request req, int status, string msg,
                                            string body, bool plain = false)
        {
            var bytes = BuildResponse(req != null ? req.CorsOrigin : "null",
                                      status, msg, body, plain);
            try {
                int sent = 0;
                while (sent < bytes.Length) {
                    int n = await s.SendAsync(
                        new ArraySegment<byte>(bytes, sent, bytes.Length - sent),
                        SocketFlags.None).ConfigureAwait(false);
                    if (n <= 0) break;
                    sent += n;
                }
            } catch (SocketException ex) {
                HttpDiag("send", ex);
            } catch (ObjectDisposedException ex) {
                HttpDiag("send", ex);
            }
        }

        /* net472 的 NetworkStream.ReadAsync 不响应取消令牌（基类实现只在启动前检查
           一次），所以 recv 超时必须用 WhenAny 实现；否则空闲连接永不超时，会一直
           占着连接配额。返回 -1 表示超时：挂起的读仍未完成（调用方发完响应后必须
           关闭套接字令它以异常结束，且不得把 buffer 归还池）。 */
        private static async Task<int> ReadWithTimeoutAsync(
            NetworkStream stream, byte[] buffer, int offset, int count, int timeoutMs)
        {
            var read = stream.ReadAsync(buffer, offset, count);
            using (var delayCts = new CancellationTokenSource()) {
                var delay = Task.Delay(timeoutMs, delayCts.Token);
                var done = await Task.WhenAny(read, delay).ConfigureAwait(false);
                if (done == read) {
                    delayCts.Cancel();
                    return await read.ConfigureAwait(false);
                }
            }
            // 超时：观察挂起读最终的异常，避免未观察任务告警。
            _ = read.ContinueWith(
                t => { var observed = t.Exception; },
                TaskContinuationOptions.OnlyOnFaulted | TaskContinuationOptions.ExecuteSynchronously);
            return -1;
        }

        private static async Task<Request> ReadRequestAsync(Socket s)
        {
            var buffer = PoolAcquire();
            // recv 超时后被中止的读仍可能异步写入该缓冲区：此时不得归还池，
            // 否则下一条连接可能拿到一块正被覆写的缓冲。
            bool poolSafe = true;
            try {
                using (var stream = new NetworkStream(s, ownsSocket: false)) {
                    int len = 0;
                    int headerEnd = -1;
                    long contentLength = 0;
                    string headerBlock = null;

                    while (true) {
                        if (headerEnd < 0 && len >= 4) {
                            var probe = Encoding.ASCII.GetString(buffer, 0, len);
                            int idx = probe.IndexOf("\r\n\r\n", StringComparison.Ordinal);
                            if (idx >= 0) {
                                headerEnd = idx + 4;
                                headerBlock = probe.Substring(0, idx);
                                var cl = ParseContentLength(headerBlock);
                                if (cl < 0) {
                                    await SendAsync(s, null, 400, "Bad Request",
                                        "{\"error\":\"invalid_content_length\"}").ConfigureAwait(false);
                                    return null;
                                }
                                contentLength = cl;
                                if (headerEnd > MaxReq || contentLength > MaxReq - headerEnd) {
                                    await SendAsync(s, null, 413, "Payload Too Large",
                                        "{\"error\":\"request_too_large\"}").ConfigureAwait(false);
                                    return null;
                                }
                            }
                        }
                        if (headerEnd >= 0 && len - headerEnd >= contentLength) break;
                        if (len >= buffer.Length) {
                            if (len >= MaxReq) {
                                await SendAsync(s, null, 413, "Payload Too Large",
                                    "{\"error\":\"request_too_large\"}").ConfigureAwait(false);
                                return null;
                            }
                            var grown = new byte[Math.Min(buffer.Length * 2, MaxReq)];
                            Buffer.BlockCopy(buffer, 0, grown, 0, len);
                            PoolRelease(buffer);
                            buffer = grown;
                        }
                        int read;
                        try {
                            read = await ReadWithTimeoutAsync(
                                stream, buffer, len, buffer.Length - len, RecvTimeoutMs)
                                .ConfigureAwait(false);
                        } catch (IOException ex) {
                            HttpDiag("recv", ex);
                            return null;
                        } catch (ObjectDisposedException ex) {
                            HttpDiag("recv", ex);
                            return null;
                        }
                        if (read < 0) {
                            // recv 超时：与 C 的 serve_one 一致（recv 返回 <=0 且请求未收齐
                            // → 400 incomplete_request）。先 Shutdown(Send) 让响应随 FIN
                            // 正常送达（带挂起读直接 Close 会走中止式关闭，把刚发出的响应
                            // 一并作废），再 Close 令仍挂起的读以异常结束；该缓冲区不再回池。
                            // Shutdown 后 s.Connected 为 false，HandleConnection 的排空会跳过。
                            poolSafe = false;
                            await SendAsync(s, null, 400, "Bad Request",
                                "{\"error\":\"incomplete_request\"}").ConfigureAwait(false);
                            try { s.Shutdown(SocketShutdown.Send); }
                            catch (SocketException ex) { HttpDiag("timeout-shutdown", ex); }
                            catch (ObjectDisposedException ex) { HttpDiag("timeout-shutdown", ex); }
                            try { s.Close(); } catch (ObjectDisposedException) { }
                            return null;
                        }
                        if (read == 0) {
                            // 对端在收齐请求前关闭：头部或正文不完整 → 400。
                            await SendAsync(s, null, 400, "Bad Request",
                                "{\"error\":\"incomplete_request\"}").ConfigureAwait(false);
                            return null;
                        }
                        len += read;
                    }

                    // ---- 解析 Origin（在请求行之前，与 C 顺序一致） ----
                    var req = new Request();
                    int originCount = 0;
                    string originValue = null;
                    var headerLines = headerBlock.Split(
                        new[] { "\r\n" }, StringSplitOptions.None);
                    for (int i = 1; i < headerLines.Length; i++) {
                        var line = headerLines[i];
                        int colon = line.IndexOf(':');
                        if (colon <= 0) continue;
                        if (!line.Substring(0, colon).Trim().Equals(
                                "Origin", StringComparison.OrdinalIgnoreCase)) continue;
                        originCount++;
                        originValue = line.Substring(colon + 1).Trim(' ', '\t');
                    }
                    if (originCount > 1 || (originCount == 1 && (
                            originValue.Length == 0 || originValue.Length >= 256
                            || !OriginValueAllowed(originValue)))) {
                        await SendAsync(s, req, 403, "Forbidden",
                            "{\"error\":\"origin_forbidden\"}").ConfigureAwait(false);
                        return null;
                    }
                    if (originCount == 1) {
                        req.OriginPresent = true;
                        req.CorsOrigin = originValue;
                    }

                    // ---- 解析请求行 ----
                    var head = Encoding.ASCII.GetString(buffer, 0, headerEnd);
                    var lineEnd = head.IndexOf("\r\n", StringComparison.Ordinal);
                    var requestLine = lineEnd >= 0 ? head.Substring(0, lineEnd) : head;
                    var parts = requestLine.Split(' ');
                    bool malformed = parts.Length != 3 || parts[0].Length == 0
                        || parts[0].Length >= 16
                        || (parts[2] != "HTTP/1.1" && parts[2] != "HTTP/1.0")
                        || parts[1].Length >= PathCap;
                    if (!malformed) {
                        foreach (var ch in parts[0]) {
                            if (!char.IsLetter(ch)) { malformed = true; break; }
                        }
                    }
                    if (!malformed) {
                        foreach (var ch in parts[1]) {
                            if (ch <= 0x20 || ch == 0x7f) { malformed = true; break; }
                        }
                    }
                    if (malformed) {
                        await SendAsync(s, req, 400, "Bad Request",
                            "{\"error\":\"malformed_request_line\"}").ConfigureAwait(false);
                        return null;
                    }
                    var rawPath = parts[1];

                    req.Body = contentLength > 0
                        ? Encoding.UTF8.GetString(buffer, headerEnd, (int)contentLength)
                        : "";
                    var path = rawPath;
                    var query = (string)null;
                    int qm = path.IndexOf('?');
                    if (qm >= 0) {
                        query = path.Substring(qm + 1);
                        path = path.Substring(0, qm);
                    }
                    req.Method = parts[0];
                    req.Path = path.ToLowerInvariant();
                    req.Query = query;
                    try {
                        req.Dom = string.IsNullOrEmpty(req.Body)
                            ? null : Json.Parse(req.Body);
                    } catch (Json.ParseException) {
                        req.Dom = null; // 畸形 JSON 由各路由按 C 语义处理
                    }
                    return req;
                }
            } finally {
                if (poolSafe) PoolRelease(buffer);
            }
        }

        private static long ParseContentLength(string headerBlock)
        {
            var lines = headerBlock.Split(new[] { "\r\n" }, StringSplitOptions.None);
            bool present = false;
            long value = 0;
            for (int i = 1; i < lines.Length; i++) {
                var line = lines[i];
                int colon = line.IndexOf(':');
                if (colon <= 0) continue;
                if (!line.Substring(0, colon).Trim().Equals(
                        "Content-Length", StringComparison.OrdinalIgnoreCase)) continue;
                if (present) return -2; // 重复 Content-Length 必须拒绝
                present = true;
                if (!ulong.TryParse(line.Substring(colon + 1).Trim(), out var parsed)
                    || parsed > long.MaxValue) {
                    return -2;
                }
                value = (long)parsed;
            }
            return present ? value : 0;
        }

        /* 单连接生命周期。异常边界说明（失败透明四点）：
           1) 防止的外部异常：对端提前断开/RST 引发的 SocketException、IOException、
              ObjectDisposedException；路由层的意外异常（属于本进程缺陷）。
           2) 为何不在上游修复：对端行为不可控；进程缺陷需要修，但不能让单连接
              的缺陷拖垮其余连接与后台队列。
           3) 是否会掩盖错误：不会——路由层意外异常会尽力回 500 并限频记录类型，
              传输类异常按阶段限频记录；任何路径都不会把失败写成缓存命中。
           4) 诊断记录位置：HttpDiag → stderr。 */
        private static async Task HandleConnection(Socket s)
        {
            try {
                s.NoDelay = true;
                var req = await ReadRequestAsync(s).ConfigureAwait(false);
                if (req == null) return;

                // 浏览器来源不得触达缓存管理/关停路由（与 C 的 browser_admin_path 一致）。
                if (req.OriginPresent && IsBrowserAdminPath(req.Path)) {
                    await SendAsync(s, req, 403, "Forbidden",
                        "{\"error\":\"origin_forbidden\"}").ConfigureAwait(false);
                    return;
                }

                try {
                    await RouteAsync(s, req).ConfigureAwait(false);
                } catch (SocketException ex) {
                    HttpDiag("route-transport", ex);
                } catch (IOException ex) {
                    HttpDiag("route-transport", ex);
                } catch (ObjectDisposedException ex) {
                    HttpDiag("route-transport", ex);
                } catch (Exception ex) {
                    // 路由层缺陷：记录类型并尽力回 500，让客户端拿到明确失败而不是断连。
                    HttpDiag("route-internal:" + ex.GetType().Name, null);
                    await SendAsync(s, req, 500, "Internal Server Error",
                        "{\"error\":\"internal_error\"}").ConfigureAwait(false);
                }
            } catch (SocketException ex) {
                HttpDiag("connection", ex);
            } catch (IOException ex) {
                HttpDiag("connection", ex);
            } catch (ObjectDisposedException ex) {
                HttpDiag("connection", ex);
            } catch (Exception ex) {
                // 请求解析层的进程缺陷：HandleConnection 是 fire-and-forget 任务，
                // 不在这里记录就只剩一条"未观察任务异常"，等于静默吞掉。
                HttpDiag("connection-internal:" + ex.GetType().Name, null);
            } finally {
                // 关闭前短暂排空未读的入站数据：当请求体仍在路上而已方已回错误
                // 响应时，直接 close 会因"有未读数据"触发 RST，把已发出的响应
                // 一并作废（客户端表现为连接被重置而非收到 4xx）。
                // 异步有界读：不再用同步 Receive 阻塞线程池线程 250ms。
                // 注意顺序：net472 的 Socket.Shutdown 会把 Connected 置为 false，而
                // NetworkStream 构造函数在 !Connected 时抛 IOException（无内层异常），
                // 所以流必须在 Shutdown 之前建好，否则排空永远不会真正执行。
                try {
                    if (s.Connected) {
                        using (var stream = new NetworkStream(s, ownsSocket: false)) {
                            s.Shutdown(SocketShutdown.Send);
                            var discard = new byte[4096];
                            while (await ReadWithTimeoutAsync(
                                       stream, discard, 0, discard.Length, 250)
                                       .ConfigureAwait(false) > 0) { }
                        }
                    }
                } catch (SocketException ex) {
                    // 对端已重置：排空目的已达成，仍限频记录以便观察异常断连频率。
                    HttpDiag("drain", ex);
                } catch (IOException ex) {
                    HttpDiag("drain", ex);
                } catch (ObjectDisposedException ex) {
                    HttpDiag("drain", ex);
                }
                try { s.Close(); } catch (ObjectDisposedException) { /* 已在超时路径关闭 */ }
                Interlocked.Decrement(ref activeConnections);
            }
        }

        private static async Task RouteAsync(Socket s, Request req)
        {
            var method = req.Method;
            var path = req.Path;

            if (method == "OPTIONS") {
                await SendAsync(s, req, 204, "No Content", "").ConfigureAwait(false);
                return;
            }

            if (method == "GET" && path == "/health") {
                await SendAsync(s, req, 200, "OK", RespHealth()).ConfigureAwait(false);
                return;
            }
            if (method == "GET" && path == "/capabilities") {
                await SendAsync(s, req, 200, "OK",
                    "{\"supports\":{\"batch\":true,\"cache_only\":true,\"xunity_custom_get\":true,\"xunity_batch_endpoint\":true},\"runtime_cache_only\":false}")
                    .ConfigureAwait(false);
                return;
            }
            if (method == "GET" && path == "/translate") {
                var text = QueryGet(req.Query, "text");
                var cacheOnlyS = QueryGet(req.Query, "cache_only");
                var cacheOnly = false;
                if (cacheOnlyS != null) {
                    cacheOnly = !cacheOnlyS.Equals("false", StringComparison.OrdinalIgnoreCase)
                                && cacheOnlyS != "0";
                }
                if (string.IsNullOrEmpty(text)) {
                    await SendAsync(s, req, 400, "Bad Request", "missing_text", plain: true)
                        .ConfigureAwait(false);
                    return;
                }
                var outcome = await TranslateValueAsync(text, cacheOnly, cacheOnly).ConfigureAwait(false);
                if (!cacheOnly && outcome.Source == "miss" && TextRules.ShouldTranslate(text)) {
                    await SendAsync(s, req, 503, "Service Unavailable",
                        "translation_unavailable", plain: true).ConfigureAwait(false);
                } else {
                    await SendAsync(s, req, 200, "OK", outcome.Value, plain: true).ConfigureAwait(false);
                }
                return;
            }
            if (method == "POST" && path == "/cache/import") {
                await SendAsync(s, req, 200, "OK", RespImport(req)).ConfigureAwait(false);
                return;
            }
            if (method == "GET" && path == "/cache/dump") {
                await SendAsync(s, req, 200, "OK", RespDump()).ConfigureAwait(false);
                return;
            }
            if (method == "POST" && path == "/cache/export") {
                await SendAsync(s, req, 200, "OK", RespExport()).ConfigureAwait(false);
                return;
            }
            if (method == "POST" && path == "/cache/lookup") {
                await SendAsync(s, req, 200, "OK", RespLookup(req)).ConfigureAwait(false);
                return;
            }
            if (method == "POST" && (path == "/translate" || path == "/batch" || path == "/")) {
                var body = await RespBatchAsync(req).ConfigureAwait(false);
                if (body == null) {
                    await SendAsync(s, req, 400, "Bad Request", "{\"error\":\"missing_text\"}")
                        .ConfigureAwait(false);
                    return;
                }
                await SendAsync(s, req, 200, "OK", body).ConfigureAwait(false);
                return;
            }
            if (method == "POST" && (path == "/prefetch" || path == "/warmup")) {
                await SendAsync(s, req, 200, "OK", RespPrefetch(req)).ConfigureAwait(false);
                return;
            }
            if (method == "POST" && path == "/shutdown") {
                // 响应体与 C 版逐字一致（http.c 的 /shutdown 返回 shutting_down）。
                await SendAsync(s, req, 200, "OK", "{\"status\":\"shutting_down\"}")
                    .ConfigureAwait(false);
                BeginShutdown();
                return;
            }
            await SendAsync(s, req, 404, "Not Found", "{\"error\":\"not_found\"}").ConfigureAwait(false);
        }

        private static void BeginShutdown()
        {
            if (stopping) return;
            stopping = true;
            lock (asyncLock) { Monitor.PulseAll(asyncLock); }
            lock (liveLock) { Monitor.PulseAll(liveLock); }
            try {
                listener.Stop();
            } catch (SocketException ex) {
                HttpDiag("listener-stop", ex);
            }
            // 排空由 Main 在 accept 循环结束后统一等待（与 C 的
            // G_CONNECTIONS_DRAINED 语义一致）。
        }

        private static void Die(string message)
        {
            Console.Error.WriteLine(message);
            Environment.Exit(1);
        }

        private static async Task AcceptLoopAsync()
        {
            while (!stopping) {
                Socket socket;
                try {
                    socket = await listener.AcceptSocketAsync().ConfigureAwait(false);
                } catch (ObjectDisposedException) {
                    // listener.Stop() 之后挂起的 accept 以此结束：正常关停路径。
                    if (stopping) break;
                    HttpDiag("accept-disposed", null);
                    break;
                } catch (InvalidOperationException) {
                    // Stop() 之后再次 BeginAccept 抛 "not listening"：同样是关停路径；
                    // 非关停时监听器已失效，继续循环只会热转。
                    if (stopping) break;
                    HttpDiag("accept-not-listening", null);
                    break;
                } catch (SocketException ex) {
                    if (stopping) break;
                    // 单次 accept 失败（如临时资源不足）：记录并短暂退避，避免热循环。
                    HttpDiag("accept", ex);
                    await Task.Delay(50).ConfigureAwait(false);
                    continue;
                }
                if (Interlocked.Increment(ref activeConnections) > ConnectionLimit) {
                    Interlocked.Decrement(ref activeConnections);
                    Interlocked.Increment(ref connectionRejections);
                    try { socket.Close(); } catch (ObjectDisposedException) { }
                    continue;
                }
                _ = HandleConnection(socket);
            }
        }

        private static int Main(string[] args)
        {
            int port = PortDefault;
            string cachePath = "translation_memory_c.tsv";
            string apiConfigPath = null;
            string glossaryPath = null;

            for (int i = 0; i < args.Length; i++) {
                if (args[i] == "--port") {
                    if (i + 1 >= args.Length) Die("missing --port value");
                    // 严格解析：拒绝 "19981junk" 之类的尾随垃圾（与 parse_port 对齐）。
                    if (!int.TryParse(args[++i], System.Globalization.NumberStyles.None,
                            System.Globalization.CultureInfo.InvariantCulture, out port)
                        || port < 1 || port > 65535) {
                        Die("invalid port");
                    }
                } else if (args[i] == "--cache") {
                    if (i + 1 >= args.Length) Die("missing --cache value");
                    cachePath = args[++i];
                } else if (args[i] == "--api-config") {
                    if (i + 1 >= args.Length) Die("missing --api-config value");
                    apiConfigPath = args[++i];
                } else if (args[i] == "--glossary") {
                    if (i + 1 >= args.Length) Die("missing --glossary value");
                    glossaryPath = args[++i];
                } else {
                    Die("unknown argument");
                }
            }
            // 超长缓存路径必须拒绝而不是截断（C 版 snprintf 边界检查）。
            if (cachePath.Length >= 1024) Die("cache path too long");

            cache = new CacheStore(cachePath);
            cache.Load();
            api = new ApiConfig();
            api.Load(apiConfigPath);
            if (glossaryPath != null) {
                ApiClient.LoadGlossary(api, glossaryPath);
            } else {
                ApiClient.LoadGlossary(api, DefaultGlossaryPath(cachePath));
            }
            apiGate = new SemaphoreSlim(Math.Max(1, api.Concurrency));
            ApiClient.Configure(api);

            listener = new TcpListener(IPAddress.Loopback, port);
            listener.Server.SetSocketOption(
                SocketOptionLevel.Socket, SocketOptionName.ReuseAddress, true);
            try {
                listener.Start(128);
            } catch (SocketException) {
                Die("bind failed");
            }

            var liveThread = new Thread(LiveWorkerLoop) { IsBackground = true };
            liveThread.Start();
            // 后台预热 worker 池：concurrency-1 个线程占满除前台保留通道之外的
            // 全部远程通道（与 C 的 async_worker_pool_size 语义一致）。
            int bgWorkers = api.Concurrency > 1 ? api.Concurrency - 1 : 1;
            for (int i = 0; i < bgWorkers; i++) {
                var t = new Thread(AsyncWorkerLoop) { IsBackground = true };
                try {
                    t.Start();
                } catch (OutOfMemoryException) {
                    Interlocked.Increment(ref workerStartFailures);
                }
            }

            Console.Error.WriteLine("dst_server_cs listening on http://127.0.0.1:" + port);
            AcceptLoopAsync().GetAwaiter().GetResult();
            // 排空在途连接（空闲连接在 recv 超时后关闭）；超时后仍以退出码 0 结束，
            // 与 C 的 G_CONNECTIONS_DRAINED 等待语义一致。
            var drainDeadline = DateTime.UtcNow.AddMilliseconds(RecvTimeoutMs + 2000);
            while (Interlocked.CompareExchange(ref activeConnections, 0, 0) > 0
                   && DateTime.UtcNow < drainDeadline) {
                Thread.Sleep(50);
            }
            return 0;
        }

        private static string DefaultGlossaryPath(string cachePath)
        {
            var dir = Path.GetDirectoryName(Path.GetFullPath(cachePath));
            return Path.Combine(string.IsNullOrEmpty(dir) ? "." : dir, "glossary.tsv");
        }
    }
}
