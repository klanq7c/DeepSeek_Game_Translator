using System;
using System.Linq;
using System.Reflection;
using System.Runtime.Serialization;
using System.Text.Json;
using DeepSeekTranslate;
using XUnity.AutoTranslator.Plugin.Core.Endpoints;
using XUnity.AutoTranslator.Plugin.Core.Endpoints.Http;
using XUnity.AutoTranslator.Plugin.Core.Web;

internal static class Program
{
    private sealed class ReadableAtlas { public bool isReadable => true; }
    private sealed class UnreadableAtlas { public bool isReadable => false; }
    private sealed class StrippedAtlas { }
    private sealed class FaultyAtlas { public bool isReadable => throw new InvalidOperationException("native getter failed"); }
    private static int passed, failed;
    private static void Check(string name, bool ok)
    {
        Console.WriteLine((ok ? "[PASS] " : "[FAIL] ") + name);
        if (ok) passed++; else failed++;
    }

    private sealed class Context : IHttpTranslationExtractionContext, IHttpRequestCreationContext
    {
        internal string[] Result;
        internal string Error;
        public string[] UntranslatedTexts { get; set; }
        public string UntranslatedText => UntranslatedTexts[0];
        public UntranslatedTextInfo UntranslatedTextInfo => null;
        public UntranslatedTextInfo[] UntranslatedTextInfos => null;
        public string SourceLanguage => "en";
        public string DestinationLanguage => "zh-CN";
        public object UserState { get; set; }
        public XUnityWebRequest Request { get; private set; }
        public XUnityWebResponse Response { get; }
        internal Context(string[] texts, string json)
        {
            UntranslatedTexts = texts;
            Response = (XUnityWebResponse)FormatterServices.GetUninitializedObject(typeof(XUnityWebResponse));
            typeof(XUnityWebResponse).GetProperty("Data").SetValue(Response, json);
        }
        public void Complete(string value) { Result = new[] { value }; }
        public void Complete(string[] values) { Result = values; }
        public void Complete(XUnityWebRequest request) { Request = request; }
        public void Fail(string message) { Error = message; }
        public void Fail(string message, Exception exception) { Error = message + ": " + exception.Message; }
    }

    private static Context Extract(string[] texts, string json)
    {
        var context = new Context(texts, json);
        new DeepSeekTranslateEndpoint().OnExtractTranslation(context);
        return context;
    }

    private static void CheckExistingResponseBehavior()
    {
        foreach (string source in new[] { "cache", "api", "API_BATCH" })
        foreach (string field in new[] { "translated_text", "translation" })
        {
            string json = JsonSerializer.Serialize(new System.Collections.Generic.Dictionary<string, string> {
                ["source"] = source, [field] = "译文" });
            var result = Extract(new[] { "Existing text." }, json);
            Check("resolved source and legacy field " + source + "/" + field, result.Error == null && result.Result?[0] == "译文");
        }
        var whitespace = Extract(new[] { "Existing text." }, " \r\n{\"translation\":\"译文\",\"extra\":{\"value\":1},\"source\":\"cache\"}\t ");
        Check("valid JSON whitespace and unrelated fields remain supported", whitespace.Error == null && whitespace.Result?[0] == "译文");
        string unicodeValue = "你好\n第二行\t\"引号\"\\😀";
        var unicode = Extract(new[] { "Existing text.\nSecond line." }, JsonSerializer.Serialize(new { source = "cache", translation = unicodeValue }));
        Check("escaped Unicode newline tab quotes and slash preserved", unicode.Error == null && unicode.Result?[0] == unicodeValue);
        var oldBatch = Extract(new[] { "First.", "Second." }, "{\"sources\":[\"cache\",\"api\"],\"translations\":{\"Second.\":\"第二\",\"First.\":\"第一\"}}");
        Check("legacy batch map follows request order", oldBatch.Error == null && oldBatch.Result?[0] == "第一" && oldBatch.Result[1] == "第二");
        var disabled = new Context(new[] { "Continue." }, JsonSerializer.Serialize(new { source = "cache", translation = "继续，走！" }));
        var endpoint = new DeepSeekTranslateEndpoint();
        typeof(DeepSeekTranslateEndpoint).GetField("_displaySafePunctuation", BindingFlags.Instance | BindingFlags.NonPublic).SetValue(endpoint, false);
        endpoint.OnExtractTranslation(disabled);
        Check("disabling punctuation fallback retains saved display preference", disabled.Error == null && disabled.Result?[0] == "继续，走！");

        string mixed = "欢迎 Welcome back to the adventure.";
        var protect = typeof(DeepSeekTranslateEndpoint).GetMethod("ProtectMixedCjkForRequest", BindingFlags.Static | BindingFlags.NonPublic);
        object[] shortLabelArguments = { "欢迎 Welcome back.", null, null };
        string shortLabel = (string)protect.Invoke(null, shortLabelArguments);
        Check("short CJK labels keep the existing mixed-language threshold", shortLabel == "欢迎 Welcome back." && ((string[])shortLabelArguments[1]).Length == 0);
        string realPrefix = "简体中文译文: ";
        foreach (string sourceWithPrefix in new[] { realPrefix + "Open", realPrefix + "Open the door in this room." })
        {
            object[] prefixArguments = { sourceWithPrefix, null, null };
            protect.Invoke(null, prefixArguments);
            string[] prefixTokens = (string[])prefixArguments[1];
            string reply = prefixTokens.Length == 0 ? realPrefix + "打开" : prefixTokens[0] + ": 打开";
            var preservedPrefix = Extract(new[] { sourceWithPrefix }, JsonSerializer.Serialize(new { source = "cache", translation = reply }));
            Check("IL2CPP preserves a genuine source prompt-like prefix after Chinese restoration " + sourceWithPrefix,
                preservedPrefix.Error == null && preservedPrefix.Result?[0] == realPrefix + "打开");
        }
        var addedPrefix = Extract(new[] { realPrefix + "Open" }, JsonSerializer.Serialize(new { source = "cache", translation = realPrefix + realPrefix + "打开" }));
        Check("IL2CPP removes only an added model prefix above the source prefix", addedPrefix.Error == null && addedPrefix.Result?[0] == realPrefix + "打开");
        var ordinaryEcho = Extract(new[] { "Open" }, JsonSerializer.Serialize(new { source = "cache", translation = realPrefix + "打开" }));
        Check("IL2CPP still strips a model-only prefix for an ordinary source", ordinaryEcho.Error == null && ordinaryEcho.Result?[0] == "打开");
        var prefixedBatch = Extract(new[] { realPrefix + "Open", "Close" }, JsonSerializer.Serialize(new {
            sources = new[] { "cache", "cache" }, results = new[] { realPrefix + "打开", realPrefix + "关闭" } }));
        Check("IL2CPP batch prompt cleanup uses each original source independently",
            prefixedBatch.Error == null && prefixedBatch.Result?[0] == realPrefix + "打开" && prefixedBatch.Result[1] == "关闭");
        object[] protectArguments = { mixed, null, null };
        protect.Invoke(null, protectArguments);
        string[] tokens = (string[])protectArguments[1];
        string mixedReply = tokens[0] + " 回来了。";
        var restored = Extract(new[] { mixed }, JsonSerializer.Serialize(new { source = "cache", translation = mixedReply }));
        Check("mixed CJK suffix translation still restores protected text", restored.Error == null && restored.Result?[0] == "欢迎 回来了.");
        var missingToken = Extract(new[] { mixed }, JsonSerializer.Serialize(new { source = "cache", translation = "回来了。" }));
        Check("mixed CJK missing token still fails without publishing text", missingToken.Result == null && missingToken.Error != null);
        var mixedRequest = new Context(new[] { mixed }, "{}");
        endpoint.OnCreateRequest(mixedRequest);
        using (var body = JsonDocument.Parse(mixedRequest.Request.Data))
            Check("single route remains cache-only and protects mixed CJK", mixedRequest.Request.Address.AbsolutePath == "/translate" && body.RootElement.GetProperty("cache_only").GetBoolean() && body.RootElement.GetProperty("text").GetString() == (string)protect.Invoke(null, new object[] { mixed, null, null }));
        var batchRequest = new Context(new[] { "First.", "Second." }, "{}");
        endpoint.OnCreateRequest(batchRequest);
        using (var body = JsonDocument.Parse(batchRequest.Request.Data))
            Check("batch route remains cache-only and ordered", batchRequest.Request.Address.AbsolutePath == "/batch" && body.RootElement.GetProperty("cache_only").GetBoolean() && body.RootElement.GetProperty("texts")[0].GetString() == "First." && body.RootElement.GetProperty("texts")[1].GetString() == "Second.");
    }

    private sealed class SessionOwner
    {
        public object job;
        public object[] jobsArray;
        public string[] Result;
        public string Error;
        public void Complete(string[] result) { Result = result; }
        public void Fail(string message, Exception exception) { Error = message + ": " + exception?.Message; }
    }

    private static void SetSdkField(object instance, string name, object value)
    {
        instance.GetType().GetField(name, BindingFlags.Instance | BindingFlags.NonPublic).SetValue(instance, value);
    }

    private static SessionOwner ExtractSession(string[] texts, string json, bool displaySafe = true, bool rawSdk = false)
    {
        // 使用随附 SDK 的实际 HTTP/translation context 和 TranslationJob；完成回调由夹具拥有。
        var sdk = typeof(HttpEndpoint).Assembly;
        var owner = new SessionOwner { jobsArray = texts.Length > 1 ? new object[texts.Length] : null };
        var jobType = sdk.GetType("XUnity.AutoTranslator.Plugin.Core.TranslationJob", true);
        for (int i = 0; i < texts.Length; i++)
        {
            object job = FormatterServices.GetUninitializedObject(jobType);
            jobType.GetProperty("SaveResultGlobally").SetValue(job, true);
            if (owner.jobsArray != null) owner.jobsArray[i] = job; else owner.job = job;
        }
        var inner = FormatterServices.GetUninitializedObject(sdk.GetType("XUnity.AutoTranslator.Plugin.Core.Endpoints.TranslationContext", true));
        SetSdkField(inner, "_untranslatedTexts", texts);
        SetSdkField(inner, "_complete", new Action<string[]>(owner.Complete));
        SetSdkField(inner, "_fail", new Action<string, Exception>(owner.Fail));
        var outer = FormatterServices.GetUninitializedObject(sdk.GetType("XUnity.AutoTranslator.Plugin.Core.Endpoints.Http.HttpTranslationContext", true));
        SetSdkField(outer, "_context", inner);
        SetSdkField(outer, "<Response>k__BackingField", new Context(texts, json).Response);
        var context = (IHttpTranslationExtractionContext)outer;
        if (rawSdk)
        {
            if (texts.Length == 1) context.Complete(texts[0]); else context.Complete(texts);
        }
        else
        {
            var endpoint = new DeepSeekTranslateEndpoint();
            typeof(DeepSeekTranslateEndpoint).GetField("_displaySafePunctuation", BindingFlags.Instance | BindingFlags.NonPublic).SetValue(endpoint, displaySafe);
            endpoint.OnExtractTranslation(context);
        }
        return owner;
    }

    private static bool SavesGlobally(object job) => (bool)job.GetType().GetProperty("SaveResultGlobally").GetValue(job);

    private static void CheckPassSessionBehavior()
    {
        foreach (string original in new[] { "已经是中文。", "   " })
        {
            var pass = ExtractSession(new[] { original }, JsonSerializer.Serialize(new { source = "pass", translated_text = original }));
            Check("pass remains original and session-only: " + JsonSerializer.Serialize(original), pass.Error == null && pass.Result?[0] == original && !SavesGlobally(pass.job));
        }
        // 空串是否能完成由真实 SDK 决定；直接对照 SDK，不能捏造成功来通过测试。
        var rawEmpty = ExtractSession(new[] { "" }, "{}", rawSdk: true);
        var passEmpty = ExtractSession(new[] { "" }, "{\"source\":\"pass\",\"translated_text\":\"\"}");
        Check("actual SDK rejects empty completion with a diagnostic", rawEmpty.Result == null && rawEmpty.Error?.Contains("Received empty translation from translator.") == true);
        Check("empty pass preserves SDK failure and cannot save globally", passEmpty.Result == null && passEmpty.Error == rawEmpty.Error && !SavesGlobally(passEmpty.job));
        string batchJson = JsonSerializer.Serialize(new { sources = new[] { "cache", "pass" }, results = new[] { "你好。", "已经是中文。" } });
        var batch = ExtractSession(new[] { "Hello.", "已经是中文。" }, batchJson, displaySafe: false);
        Check("mixed batch preserves raw text when fallback is disabled", batch.Error == null && batch.Result?[0] == "你好。" && batch.Result[1] == "已经是中文。" && SavesGlobally(batch.jobsArray[0]) && !SavesGlobally(batch.jobsArray[1]));
        var safeBatch = ExtractSession(new[] { "Hello.", "已经是中文。" }, batchJson);
        Check("mixed batch retains existing display fallback and session-only pass", safeBatch.Error == null && safeBatch.Result?[0] == "你好." && safeBatch.Result[1] == "已经是中文." && SavesGlobally(safeBatch.jobsArray[0]) && !SavesGlobally(safeBatch.jobsArray[1]));
        string promptLikeOriginal = "简体中文译文: 已经是中文";
        var promptPass = ExtractSession(new[] { "Hello", promptLikeOriginal }, JsonSerializer.Serialize(new {
            sources = new[] { "cache", "pass" }, results = new[] { "你好", promptLikeOriginal } }));
        Check("IL2CPP batch pass keeps genuine Chinese prefix and session-only ownership", promptPass.Error == null
            && promptPass.Result?[1] == promptLikeOriginal && !SavesGlobally(promptPass.jobsArray[1]));
    }

    private static bool CollectPending(string json, string[] texts, out string[] pending)
    {
        object[] arguments = { json, texts, null };
        bool valid = (bool)typeof(DeepSeekTranslateEndpoint).GetMethod("TryCollectPendingTexts", BindingFlags.Static | BindingFlags.NonPublic).Invoke(null, arguments);
        pending = (string[])arguments[2];
        return valid;
    }

    private static void CheckQueueClassification()
    {
        Check("initial queued response remains pending", CollectPending("{\"source\":\"queued\"}", new[] { "Waiting." }, out string[] pending) && pending.Length == 1 && pending[0] == "Waiting.");
        foreach (string source in new[] { "cache", "pass" })
            Check("initial " + source + " response remains terminal", CollectPending(JsonSerializer.Serialize(new { source }), new[] { "Existing." }, out pending) && pending.Length == 0);
        Check("mixed queue polls only queued/miss slots in input order", CollectPending("{\"sources\":[\"cache\",\"queued\",\"pass\",\"miss\"]}", new[] { "Cached.", "Queued.", "中文", "Missing." }, out pending) && pending.Length == 2 && pending[0] == "Queued." && pending[1] == "Missing.");
        var hasHits = typeof(DeepSeekTranslateEndpoint).GetMethod("HasAllLookupHits", BindingFlags.Static | BindingFlags.NonPublic);
        Check("cache poll resolves only after every requested key hits", (bool)hasHits.Invoke(null, new object[] { "{\"hits\":{\"One.\":\"一\",\"Two.\":\"二\"}}", new[] { "One.", "Two." } }) && !(bool)hasHits.Invoke(null, new object[] { "{\"hits\":{\"One.\":\"一\"}}", new[] { "One.", "Two." } }));
        Check("unknown initial source never becomes completed work", !CollectPending("{\"source\":\"unrecognized\"}", new[] { "Waiting." }, out pending));
    }

    private static void ScheduleCachePasses(MonoCacheScanSeam scan)
    {
        scan._sceneCacheApplyPassesRemaining = 1;
        scan._sceneCacheApplyGeneration = scan._sceneWarmupGeneration;
        scan._nextSceneCacheApplyRealtime = scan._nextUiCacheApplyRealtime = scan._nextPeriodicCacheApplyRealtime = 0;
        scan._uiCacheApplyUntilRealtime = MonoCacheScanSeam.Time.realtimeSinceStartup + 10;
    }

    private static void CheckCacheScanReuse()
    {
        var scan = new MonoCacheScanSeam();
        ScheduleCachePasses(scan);
        scan.RunCacheApplyTicks();
        Check("Mono three due cache consumers enumerate TMP/UGUI once each", scan._cacheApplyPassCount == 3
            && scan.TmpDiscoveries == 1 && scan.UguiDiscoveries == 1 && scan.Seen.Count == 4 && scan.SnapshotsReleased);
        var added = new Text();
        scan.UguiObjects = new[] { added };
        ScheduleCachePasses(scan);
        scan.RunCacheApplyTicks();
        Check("Mono next pump discovers new objects and releases previous snapshot", scan.TmpDiscoveries == 2
            && scan.UguiDiscoveries == 2 && scan.Seen.Contains(added) && scan.SnapshotsReleased);

        var activation = new MonoCacheScanSeam();
        var activated = new Text();
        ScheduleCachePasses(activation);
        activation._lastUiActivationRealtime = MonoCacheScanSeam.Time.realtimeSinceStartup;
        activation.OnFlush = () => {
            activation.UguiObjects = new[] { activated };
            activation.RequestUiCacheApplyBurst();
        };
        activation.RunCacheApplyTicks();
        Check("Mono throttled activation invalidates snapshot before later consumers", activation.UguiDiscoveries == 2
            && activation.TmpDiscoveries == 2 && activation.Seen.Contains(activated) && activation.SnapshotsReleased);

        var immediate = new MonoCacheScanSeam();
        immediate.RequestUiCacheApplyBurst();
        immediate.UguiObjects = new[] { new Text() };
        MonoCacheScanSeam.Time.realtimeSinceStartup += 1;
        immediate.RequestUiCacheApplyBurst();
        Check("Mono immediate activation always discovers current objects outside a batch", immediate.TmpDiscoveries == 2
            && immediate.UguiDiscoveries == 2 && immediate.Seen.Contains(immediate.UguiObjects[0]) && immediate.SnapshotsReleased);

        var discovery = new MonoCacheScanSeam();
        ScheduleCachePasses(discovery);
        var newTmp = new Object();
        discovery.OnDiscover = () => {
            discovery.TmpObjects = new[] { newTmp };
            discovery.InvalidateCacheApplySnapshot();
        };
        discovery.RunCacheApplyTicks();
        Check("Mono scene invalidation during discovery cannot publish an outdated snapshot", discovery.TmpDiscoveries == 2
            && discovery.Seen.Contains(newTmp) && discovery.SnapshotsReleased);

        var nested = new MonoCacheScanSeam();
        ScheduleCachePasses(nested);
        nested.OnFlush = () => nested.RunCacheApplyTicks();
        nested.RunCacheApplyTicks();
        Check("Mono nested cache pump releases both snapshot scopes", nested.SnapshotsReleased && nested.TmpDiscoveries == 2);

        var faulty = new MonoCacheScanSeam();
        ScheduleCachePasses(faulty);
        faulty.ThrowOnDiscover = true;
        bool surfaced = false;
        try { faulty.RunCacheApplyTicks(); }
        catch (InvalidOperationException ex) { surfaced = ex.Message == "discovery failed"; }
        Check("Mono failed discovery surfaces to caller and releases snapshot", surfaced && faulty.SnapshotsReleased);
        faulty.ThrowOnDiscover = false;
        ScheduleCachePasses(faulty);
        faulty.RunCacheApplyTicks();
        Check("Mono successful next pump re-enumerates after failure", faulty.TmpDiscoveries == 2 && faulty.UguiDiscoveries == 1 && faulty.SnapshotsReleased);

        var ugui = new MonoCacheScanSeam { TmpSupported = false };
        ScheduleCachePasses(ugui);
        ugui.RunCacheApplyTicks();
        Check("Mono UGUI-only games do not enumerate unsupported TMP", ugui.TmpDiscoveries == 0 && ugui.UguiDiscoveries == 1 && ugui.Seen.Count == 2);

        var budget = new MonoCacheScanSeam();
        budget.TmpObjects = Enumerable.Range(0, 1000).Select(i => new Object()).ToArray();
        budget.UguiObjects = Enumerable.Range(0, 1000).Select(i => new Text()).ToArray();
        budget.ApplyCachedVisibleTextPass(32, 12);
        Check("Mono real caller retains bounded inspection", budget.Inspected == 12 && budget.SnapshotsReleased);
        budget._cacheMutationGeneration = 1;
        budget._nextPeriodicCacheApplyRealtime = MonoCacheScanSeam.Time.realtimeSinceStartup + 1000;
        budget.RunCacheApplyTicks();
        Check("Mono new translations bypass timer and fully inspect current objects", budget.Seen.Count == 2000 && budget.SnapshotsReleased);

        var enabled = new MonoCacheScanSeam();
        MonoCacheScanSeam._instance = enabled;
        ScheduleCachePasses(enabled);
        var lateText = new Object();
        enabled.OnFlush = () => {
            enabled.TmpObjects = new[] { lateText };
            MonoCacheScanSeam.TMPTextOnEnablePostfix(lateText);
            // 模拟游戏在 OnEnable 返回后设置内容，后续扫描现在可以命中。
            enabled.ApplyHits.Add(lateText);
        };
        enabled.RunCacheApplyTicks();
        Check("Mono newly enabled TMP filled after OnEnable translates in the same pump", enabled.EnableLookups == 1
            && enabled.Seen.Contains(lateText) && enabled._cacheApplyHitCount == 1 && enabled.SnapshotsReleased);

        var callback = new MonoCacheScanSeam();
        ScheduleCachePasses(callback);
        callback.ApplyHits.Add(callback.UguiObjects[0]);
        var callbackText = new Text();
        callback.OnInspect = () => callback.OnInspect = () => callback.OnInspect = () => {
            callback.UguiObjects = new[] { callbackText };
            callback.ApplyHits.Add(callbackText);
        };
        callback.RunCacheApplyTicks();
        Check("Mono text application callback can create a UGUI row without SetActive", callback.Seen.Contains(callbackText)
            && callback._cacheApplyHitCount == 2 && callback.SnapshotsReleased);

        var interrupted = new MonoCacheScanSeam();
        ScheduleCachePasses(interrupted);
        interrupted.OnInspect = () => { throw new InvalidOperationException("renderer failed"); };
        bool rendererError = false;
        try { interrupted.RunCacheApplyTicks(); }
        catch (InvalidOperationException ex) { rendererError = ex.Message == "renderer failed"; }
        Check("Mono renderer failure releases an already populated snapshot", rendererError && interrupted.SnapshotsReleased);

        foreach (bool throwAfterWrite in new[] { false, true }) {
            var pending = new MonoCacheScanSeam();
            ScheduleCachePasses(pending);
            Text target = pending.UguiObjects[0];
            target.text = "Open settings";
            var created = new Text();
            target.OnWrite = () => {
                pending.UguiObjects = new[] { created };
                pending.ApplyHits.Add(created);
                if (throwAfterWrite) throw new InvalidOperationException("game callback failed after creating text");
            };
            pending._pendingApplyQueue.Add(new MonoCacheScanSeam.PendingTranslationApply {
                ComponentRef = new WeakReference(target), InstanceId = 1,
                OriginalText = "Open settings", TranslatedText = "打开设置", IsTmp = false
            });
            pending.RunCacheApplyTicks();
            Check("Mono queued write invalidates discovery after native callback; throws=" + throwAfterWrite,
                pending.Seen.Contains(created) && pending._cacheApplyHitCount == 1 && pending.SnapshotsReleased
                && pending.Logger.Warnings.Count == (throwAfterWrite ? 1 : 0));
        }
    }

    private static int Main()
    {
        var normal = Extract(new[] { "Hello there." }, JsonSerializer.Serialize(new { source = "cache", translated_text = "你好。" }));
        Check("resolved translation completes", normal.Error == null && normal.Result?[0] == "你好.");
        var markup = Extract(new[] { "<link=\"quest，1\">Continue</link>" }, JsonSerializer.Serialize(new {
            source = "cache", translated_text = "<link=\"quest，1\">继续，走！</link>" }));
        Check("TMP link identifier remains byte-identical", markup.Error == null && markup.Result?[0] == "<link=\"quest，1\">继续,走!</link>");
        var queued = Extract(new[] { "Hello there." }, JsonSerializer.Serialize(new { source = "queued", translated_text = "你好。" }));
        Check("queued response cannot complete even with different text", queued.Result == null && queued.Error != null);
        var echoed = Extract(new[] { "Hello there." }, JsonSerializer.Serialize(new { source = "cache", translated_text = "Hello there." }));
        Check("identity cannot become successful translation", echoed.Result == null && echoed.Error != null);
        var malformed = Extract(new[] { "Hello there." }, "{\"source\":\"cache\",\"translated_text\":\"你好\"");
        Check("truncated JSON fails with diagnostic", malformed.Result == null && malformed.Error != null);
        var wrongType = Extract(new[] { "Hello there." }, "{\"source\":\"cache\",\"translated_text\":123}");
        Check("nonstring translation is rejected", wrongType.Result == null && wrongType.Error != null);
        var mixed = Extract(new[] { "Hello.", "Goodbye." }, JsonSerializer.Serialize(new {
            sources = new[] { "cache", "queued" }, results = new[] { "你好。", "再见。" } }));
        Check("unresolved batch cannot partially complete", mixed.Result == null && mixed.Error != null);
        var batch = Extract(new[] { "Hello.", "Goodbye." }, JsonSerializer.Serialize(new {
            sources = new[] { "cache", "api_batch" }, results = new[] { "你好。", "再见。" } }));
        Check("resolved batch retains result order", batch.Error == null && batch.Result?.Length == 2 && batch.Result[0] == "你好." && batch.Result[1] == "再见.");
        var nested = Extract(new[] { "results", "sources" }, "{\"translations\":{\"results\":\"结果\",\"sources\":\"来源\"},\"sources\":[\"cache\",\"cache\"]}");
        Check("map keys cannot shadow top-level result metadata", nested.Error == null && nested.Result?[0] == "结果" && nested.Result[1] == "来源");
        var trailing = Extract(new[] { "Hello." }, "{\"source\":\"cache\",\"translation\":\"你好\"}garbage");
        Check("trailing garbage fails with diagnostic", trailing.Result == null && trailing.Error != null);
        var blank = Extract(new[] { "Hello." }, "{\"source\":\"cache\",\"translation\":\"   \"}");
        Check("blank single cannot hide visible text", blank.Result == null && blank.Error != null);
        var blankBatch = Extract(new[] { "Hello.", "Goodbye." }, "{\"sources\":[\"cache\",\"cache\"],\"results\":[\"你好\",\"   \"]}");
        Check("blank batch item cannot partially hide text", blankBatch.Result == null && blankBatch.Error != null);
        var mono = new MonoRendererSeam();
        foreach (string input in new[] {
            "<link=\"quest，1\">继续，走！</link>", "<sprite name='图标，蓝'>你好！",
            "<link=\"id>，1\">中文，继续！</link>", "<b>中文，继续！</b>" })
        {
            string expected = input.Replace("继续，", "继续,").Replace("中文，", "中文,").Replace("！", "!");
            Check("Mono tag-safe glyph fallback " + input, mono.NormalizeTmpPunctuationForMissingGlyphs(input) == expected);
            Check("IL2CPP tag-safe glyph fallback " + input, Il2CppRendererSeam.NormalizeTmpTextForFallback(input) == expected);
        }
        Check("literal angle comparison still normalizes", Il2CppRendererSeam.NormalizeTmpTextForFallback("生命 < 5，快跑！") == "生命 < 5,快跑!");
        Check("ordinary English unchanged", Il2CppRendererSeam.NormalizeTmpTextForFallback("Don't change English...") == "Don't change English...");
        Check("readable atlas metadata", MonoRendererSeam.TryGetTextureReadability(new ReadableAtlas(), out bool readable) && readable);
        Check("unreadable atlas metadata", MonoRendererSeam.TryGetTextureReadability(new UnreadableAtlas(), out readable) && !readable);
        Check("stripped metadata is explicitly unavailable", !MonoRendererSeam.TryGetTextureReadability(new StrippedAtlas(), out readable) && !readable && MonoRendererSeam.Errors.Count == 1);
        Check("native getter error is diagnosed", !MonoRendererSeam.TryGetTextureReadability(new FaultyAtlas(), out readable) && MonoRendererSeam.Errors.Count == 2);
        CheckExistingResponseBehavior();
        CheckPassSessionBehavior();
        CheckQueueClassification();
        CheckCacheScanReuse();
        CachePersistenceTests.Run(Check);
        TmpOverlayTests.Run(Check);
        TmpFontPopulationTests.Run(Check);
        DeepPrefetchTests.Run(Check);
        var animated = MonoAnimatedSetterSeam._instance;
        var animatedText = new Text();
        var tmpText = new TmpText();
        bool immediate = true;
        for (int frame = 0; frame < 60; frame++)
        {
            string source = "<color=#" + (frame % 2 == 0 ? "ff0000" : "0000ff") + ">continue your adventure</color>";
            string expected = source.Replace("continue your adventure", "继续冒险");
            animated.Cache[source] = expected;
            string tmpValue = source, uguiValue = source;
            immediate &= MonoAnimatedSetterSeam.LooksLikeTypewriterFragment(source);
            MonoAnimatedSetterSeam.TMPSetTextPrefix(tmpText, ref tmpValue);
            MonoAnimatedSetterSeam.UGUITextPrefix(animatedText, ref uguiValue);
            immediate &= tmpValue == expected && uguiValue == expected;
        }
        Check("Mono TMP and UGUI animated cached fragments resolve before debounce for 60 writes", immediate && animated.Debounced == 0 && animated.Applied == 120);
        string unknownTmp = "continue a new unknown adventure", unknownUgui = unknownTmp;
        MonoAnimatedSetterSeam.TMPSetTextPrefix(tmpText, ref unknownTmp);
        MonoAnimatedSetterSeam.UGUITextPrefix(animatedText, ref unknownUgui);
        Check("Mono uncached fragments preserve original and existing debounce", unknownTmp == unknownUgui && unknownTmp == "continue a new unknown adventure" && animated.Debounced == 2);
        foreach (string original in new[] {
            "<b>这是一段已经翻译好的长对话，需要保留人物强调的内容，即使长度超过短标签限制也不能去掉游戏原有的文字格式。</b>",
            "<i>第一行对话。\n第二行对话。</i>" })
        {
            string value = original;
            MonoAnimatedSetterSeam.TMPSetTextPrefix(tmpText, ref value);
            Check("Mono existing CJK dialogue retains original long/multiline styles", value == original);
        }
        bool richAnimation = true;
        for (int frame = 0; frame < 60; frame++)
        {
            string source = "<size=" + (24 + frame % 3) + "><b>Continue your adventure through this long hallway.</b>\n<i>Watch your step.</i></size>";
            string expected = source.Replace("Continue your adventure through this long hallway.", "继续穿过这条长廊。").Replace("Watch your step.", "小心脚下。");
            animated.Cache[source] = expected;
            string tmpValue = source, uguiValue = source;
            MonoAnimatedSetterSeam.TMPSetTextPrefix(tmpText, ref tmpValue);
            MonoAnimatedSetterSeam.UGUITextPrefix(animatedText, ref uguiValue);
            richAnimation &= tmpValue == expected && uguiValue == expected;
        }
        Check("Mono TMP and UGUI preserve all current long dialogue style tags for 60 writes", richAnimation);
        string longPlain = "Continue your adventure through this long hallway and watch your step.";
        Check("Mono unrelated added dialogue formatting retains existing compatibility policy",
            !MonoAnimatedSetterSeam.ShouldPreserveRichTextForDisplay(longPlain, "<b>继续冒险。</b>") &&
            !MonoAnimatedSetterSeam.ShouldPreserveRichTextForDisplay("<size=24>" + longPlain + "</size>", "<size=99>继续冒险。</size>"));
        string literalAngles = "< The lobby is ahead. >";
        Check("Mono literal angle text is not interpreted as formatting",
            MonoAnimatedSetterSeam.StripRichTextForPlainText(literalAngles) == literalAngles);
        foreach (string quote in new[] { "\"", "'" })
        {
            string originalLink = "<link=" + quote + "quest>original" + quote + ">" + longPlain + "</link>";
            string translatedLink = "<link=" + quote + "quest>changed" + quote + ">继续冒险。</link>";
            Check("Mono full quoted link attribute must match before preserving long dialogue " + quote,
                !MonoAnimatedSetterSeam.ShouldPreserveRichTextForDisplay(originalLink, translatedLink));
            Check("Mono identical quoted link attribute is preserved " + quote,
                MonoAnimatedSetterSeam.ShouldPreserveRichTextForDisplay(originalLink, originalLink.Replace(longPlain, "继续冒险。")));
            animated.Cache[originalLink] = translatedLink;
            string tmpFallback = originalLink;
            MonoAnimatedSetterSeam.TMPSetTextPrefix(tmpText, ref tmpFallback);
            Check("Mono plain fallback removes complete changed link attribute " + quote, tmpFallback == "继续冒险。");
            string styledLink = "<link=" + quote + "quest>original" + quote + "><b>" + longPlain + "</b></link>";
            animated.Cache[styledLink] = styledLink.Replace(longPlain, "继续冒险。");
            string uguiFallback = styledLink;
            MonoAnimatedSetterSeam.UGUITextPrefix(animatedText, ref uguiFallback);
            Check("Mono UGUI filters complete TMP link while preserving supported nested bold " + quote,
                uguiFallback == "<b>继续冒险。</b>");
            string unchangedLink = originalLink.Replace(longPlain, "继续冒险。");
            Check("Mono renderer without rich text removes complete quoted link " + quote,
                MonoAnimatedSetterSeam.PrepareTranslatedTextForComponent(new Object(), unchangedLink, originalLink) == "继续冒险。");
            Check("Mono visible text excludes complete quoted link attributes " + quote,
                MonoAnimatedSetterSeam.GetVisibleText(unchangedLink) == "继续冒险。");
            var protectedLink = MonoAnimatedSetterSeam.ProtectTextForTranslation(originalLink);
            Check("Mono request protects complete quoted link attribute " + quote,
                !protectedLink.RequestText.Contains("original") && protectedLink.Tokens.Values.Contains(originalLink.Substring(0, originalLink.IndexOf(longPlain))));
            Check("Mono protected quoted link roundtrip retains exact attributes " + quote,
                MonoAnimatedSetterSeam.RestoreProtectedText(protectedLink.RequestText.Replace(longPlain, "继续冒险。"), protectedLink) == unchangedLink);
            var adjacentLink = Extract(new[] { originalLink }, JsonSerializer.Serialize(new { source = "cache", translated_text = unchangedLink }));
            Check("IL2CPP endpoint preserves complete quoted link attributes " + quote,
                adjacentLink.Error == null && adjacentLink.Result?[0] == unchangedLink.Replace("。", "."));
        }
        foreach (string tag in new[] { "<link=\"quest>original\">", "<link=\"size=20>section\">", "<size=120%><b>" })
        {
            string end = tag.StartsWith("<link=") ? "</link>" : "</b></size>";
            string source = tag + longPlain + end;
            string translated = tag + "继续冒险。" + end;
            animated.Cache[source] = translated;
            string value = source;
            MonoAnimatedSetterSeam.TMPSetTextPrefix(tmpText, ref value);
            Check("Mono preparation preserves complete matching tag attributes " + tag, value == translated && tmpText.richText);
        }
        foreach (var row in new[] {
            new[] { "<color-=red>警告</<color>", "<color=red>警告</color>" },
            new[] { "<b>继续</ <b>", "<b>继续</b>" },
            new[] { "size=24>继续/size>", "<size=24>继续</size>" },
            new[] { "<b>继续</b>/ ", "<b>继续</b> " },
            new[] { "<b>__DS_INTACT_TAG_0</b> <__DS_INTACT_TAG_0>", "<b>__DS_INTACT_TAG_0</b> <__DS_INTACT_TAG_0>" },
            new[] { "<link=\"color>size=24>id\">继续</link>", "<link=\"color>size=24>id\">继续</link>" },
            new[] { "<link=\"</<color>size=24>id\">继续</link>", "<link=\"</<color>size=24>id\">继续</link>" } })
        {
            Check("Mono intact tag protection retains repair and literal text " + row[0],
                MonoAnimatedSetterSeam.SanitizeTranslationArtifacts(row[0]) == row[1]);
        }
        foreach (string id in new[] { "quest><br>original", "quest><color=red>original", "id>，section/size>" })
        {
            string source = "<link=\"" + id + "\"><b>" + longPlain + "</b></link>";
            string translated = source.Replace(longPlain, "继续冒险。");
            animated.Cache[source] = translated;
            string uguiValue = source;
            MonoAnimatedSetterSeam.UGUITextPrefix(animatedText, ref uguiValue);
            Check("Mono complete attribute filtering never renders embedded tag-like text " + id,
                uguiValue == "<b>继续冒险。</b>" && MonoAnimatedSetterSeam.StripRichTextForPlainText(translated) == "继续冒险。");
            var payload = MonoAnimatedSetterSeam.ProtectTextForTranslation(source);
            Check("Mono complete attribute is opaque to request protection " + id,
                !payload.RequestText.Contains(id) && MonoAnimatedSetterSeam.RestoreProtectedText(payload.RequestText.Replace(longPlain, "继续冒险。"), payload) == translated);
        }
        Check("Mono plain fallback preserves actual line breaks and literal comparisons",
            MonoAnimatedSetterSeam.StripRichTextForPlainText("<b>继续</b><br>生命 < 5") == "继续\n生命 < 5");
        Check("Mono UGUI still filters TMP-only tags while retaining supported styles",
            MonoAnimatedSetterSeam.PrepareTranslatedTextForComponent(animatedText, "<link=\"id\"><b>继续</b></link>", "<link=\"id\"><b>Continue</b></link>") == "<b>继续</b>");
        foreach (var row in new[] {
            new[] { "- Continue", "继续", "- 继续" },
            new[] { "> Continue", "* 继续", "> 继续" },
            new[] { "- Continue", "- 继续", "- 继续" },
            new[] { "Continue", "继续", "继续" },
            new[] { "- ", "继续", "继续" },
            new[] { "- Continue", "-", "-" } })
        {
            Check("Mono leading decoration restoration remains source-compatible " + row[0] + " / " + row[1],
                UnityTextFormatting.RestoreLeadingTextDecoration(row[1], row[0]) == row[2]);
        }
        Check("Mono leading decoration production entry uses extracted module",
            MonoAnimatedSetterSeam.RestoreLeadingTextDecoration("继续", "- Continue") == "- 继续");
        Check("Mono leading decoration restoration preserves empty input",
            UnityTextFormatting.RestoreLeadingTextDecoration(null, "- Continue") == null &&
            UnityTextFormatting.RestoreLeadingTextDecoration("继续", null) == "继续");
        bool il2cppAnimation = true;
        for (int frame = 0; frame < 60; frame++)
        {
            string before = "<color=#" + (frame % 2 == 0 ? "ff0000" : "0000ff") + "><link=\"choice," + (frame % 6) + "\">";
            string translated = before + "继续，走！</link></color>";
            var response = Extract(new[] { before + "Continue your adventure.</link></color>" }, JsonSerializer.Serialize(new { source = "cache", translated_text = translated }));
            il2cppAnimation &= response.Error == null && response.Result?[0] == translated.Replace("继续，", "继续,").Replace("！", "!");
        }
        Check("IL2CPP endpoint and TMP display preserve all 60 current animation tags and link ids", il2cppAnimation);
        bool mixedAnimation = true;
        for (int frame = 0; frame < 60; frame++) {
            string source = "请 <color=#" + (frame % 2 == 0 ? "ff0000" : "0000ff") + ">Open the door</color>。";
            string expected = source.Replace("Open the door", "打开门");
            animated.Cache[source] = expected;
            string tmpValue = source, uguiValue = source;
            MonoAnimatedSetterSeam.TMPSetTextPrefix(tmpText, ref tmpValue);
            MonoAnimatedSetterSeam.UGUITextPrefix(animatedText, ref uguiValue);
            mixedAnimation &= tmpValue == expected && uguiValue == expected;
        }
        Check("Mono half-localized TMP/UGUI preserve Chinese and animated colors for 60 writes", mixedAnimation);
        Check("Mono existing Chinese variables and acronyms are terminal", MonoAnimatedSetterSeam.IsLocalizedText("已完成 HP 100 <color=red>确认</color> [player]"));
        string namedSource = "请 Hello John.", namedOutput = "请 你好 John。";
        animated.Cache[namedSource] = namedOutput;
        MonoAnimatedSetterSeam.TMPSetTextPrefix(tmpText, ref namedSource);
        Check("Mono completed translation with Latin name is terminal", namedSource == namedOutput && MonoAnimatedSetterSeam.IsLocalizedText(namedOutput));
        string completedStyled = "<color=red>请 Hello Archer.</color>";
        string completedOutput = "<color=red>请 你好 Archer。</color>";
        animated.Cache[completedStyled] = completedOutput;
        MonoAnimatedSetterSeam.TMPSetTextPrefix(tmpText, ref completedStyled);
        foreach (string rewrite in new[] { "<color=blue>请 你好 Archer。</color>", "  请  你好 Archer。 \n", "<size=30><b>请 你好 Archer。</b></size>",
            "请 你<color=blue>好</color> Archer。", "请 你好 Ar<color=blue>cher</color>。", "请<br>你好 Archer。" })
        {
            string tmpRewrite = rewrite, uguiRewrite = rewrite;
            int queuedBefore = animated.Debounced;
            MonoAnimatedSetterSeam.TMPSetTextPrefix(tmpText, ref tmpRewrite);
            MonoAnimatedSetterSeam.UGUITextPrefix(animatedText, ref uguiRewrite);
            Check("Mono completed visible text survives presentation-only rewrites " + rewrite,
                MonoAnimatedSetterSeam.IsLocalizedText(rewrite) && animated.Debounced == queuedBefore);
        }
        string formatOnlySource = "<color=red>请 Continue</color>";
        MonoAnimatedSetterSeam.RememberCompletedOutput(formatOnlySource, "<b>请 Continue</b>");
        Check("Mono formatting an untranslated source cannot mark it completed", !MonoAnimatedSetterSeam.IsLocalizedText("请 Continue"));
        Check("Mono appended English content cannot inherit a completed prefix", !MonoAnimatedSetterSeam.IsLocalizedText(completedOutput + " Press Enter"));
        Check("Mono completed keys retain real word spaces and br separation",
            !MonoAnimatedSetterSeam.IsLocalizedText("请你好 Archer。") && !MonoAnimatedSetterSeam.IsLocalizedText("请 你好 Ar<br>cher。"));
        foreach (string shortSource in new[] { "请 <color=red>Save</color>", "请 <b>Exit</b>", "请 <b>SAVE</b>" })
        {
            var shortPayload = MonoAnimatedSetterSeam.ProtectTextForTranslation(shortSource);
            Check("Mono short mixed UI words survive protected-token filtering " + shortSource,
                !MonoAnimatedSetterSeam.IsTokenDominatedFragment(shortPayload.RequestText)
                && animated.CollectWarmupRequestItems(new[] { shortSource }, null).Count == 1);
        }
        foreach (string fragment in new[] { "请 <b>S</b>", "请 <b>Sa</b>", "<b>HP</b>", "请 <b>42</b>" })
            Check("Mono token-dominated fragments retain existing filtering " + fragment,
                MonoAnimatedSetterSeam.IsTokenDominatedFragment(MonoAnimatedSetterSeam.ProtectTextForTranslation(fragment).RequestText));
        foreach (bool nativeCoverage in new[] { true, false })
        {
            string pendingTmp = "请 Wait for the next train.", pendingUgui = pendingTmp;
            var pendingText = new Text();
            animated.TmpFontCoversText = nativeCoverage;
            int appliedBefore = animated.Applied, checkedBefore = animated.FontCoverageChecks, queuedBefore = animated.Debounced;
            MonoAnimatedSetterSeam.TMPSetTextPrefix(tmpText, ref pendingTmp);
            MonoAnimatedSetterSeam.UGUITextPrefix(pendingText, ref pendingUgui);
            Check("Mono unresolved mixed text retains CJK font handling; native coverage=" + nativeCoverage,
                pendingTmp == "请 Wait for the next train." && pendingUgui == pendingTmp && pendingText.font != null
                && animated.Applied >= appliedBefore + 2 && animated.FontCoverageChecks > checkedBefore
                && animated.PendingOverlay == !nativeCoverage && !animated.OverlayRecordsCompletion && animated.Debounced == queuedBefore + 2
                && !MonoAnimatedSetterSeam.IsLocalizedText(pendingTmp));
        }
        animated.TmpFontCoversText = true;
        string pendingMalformed = "请 Activate</<b>";
        string pendingPrepared = MonoAnimatedSetterSeam.PrepareTranslatedTextForComponent(tmpText, pendingMalformed, pendingMalformed, rememberCompletedOutput: false);
        Check("Mono pending overlay format repair never records completed English",
            pendingPrepared != pendingMalformed && !MonoAnimatedSetterSeam.IsLocalizedText(pendingPrepared));
        foreach (int route in new[] { 0, 1, 2 })
        {
            string pendingImGui = "请 Keep the existing words.";
            var style = new GUIStyle();
            var rect = new Rect();
            var content = new GUIContent { text = pendingImGui };
            int queuedBefore = animated.Debounced;
            if (route == 0) MonoAnimatedSetterSeam.GUILabelPrefix(ref rect, ref pendingImGui, style);
            else if (route == 1) MonoAnimatedSetterSeam.TranslateImGuiString(ref pendingImGui, style, false, ref rect);
            else MonoAnimatedSetterSeam.TranslateGUILayoutLabelContent(content, style);
            Check("Mono IMGUI unresolved mixed text keeps Chinese font and queues English; route=" + route,
                style.font != null && animated.Debounced == queuedBefore + 1
                && pendingImGui == "请 Keep the existing words." && content.text == pendingImGui);
        }
        animated.UseProductionCacheLookup = true;
        foreach (bool addedEcho in new[] { false, true })
        {
            string prefix = "简体中文译文: ";
            string source = prefix + (addedEcho ? "Unlock" : "Open");
            string expected = prefix + (addedEcho ? "解锁" : "打开");
            animated.StoreCachedTranslation(source, (addedEcho ? prefix : "") + expected);
            string tmpValue = source, uguiValue = source;
            MonoAnimatedSetterSeam.TMPSetTextPrefix(tmpText, ref tmpValue);
            MonoAnimatedSetterSeam.UGUITextPrefix(animatedText, ref uguiValue);
            Check("Mono cache writer and real TMP/UGUI setter preserve genuine source prefix; added echo=" + addedEcho,
                animated.Cache.TryGetValue(source, out string stored) && stored == expected && tmpValue == expected && uguiValue == expected);
            var payload = MonoAnimatedSetterSeam.ProtectTextForTranslation(source);
            Check("Mono response token restoration preserves genuine Chinese prefix; added echo=" + addedEcho,
                MonoAnimatedSetterSeam.RestoreProtectedText((addedEcho ? prefix : "") + expected, payload) == expected);
        }
        animated.Cache["简体中文译文: Close"] = "简体中文译文: 关闭";
        Check("Mono existing cache lookup retains genuine source prefix", animated.TryGetLocalTranslation("简体中文译文: Close", out string fromCache) && fromCache == "简体中文译文: 关闭");
        animated.StoreCachedTranslation("Ordinary source", "简体中文译文: 正常结果");
        Check("Mono ordinary source still removes a genuine model echo", animated.Cache["Ordinary source"] == "正常结果");
        animated.UseProductionCacheLookup = false;
        Console.WriteLine("Unity endpoint contract: PASS=" + passed + " FAIL=" + failed);
        return failed == 0 ? 0 : 1;
    }
}
