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
        var unicode = Extract(new[] { "Existing text." }, JsonSerializer.Serialize(new { source = "cache", translation = unicodeValue }));
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
        var animated = MonoAnimatedSetterSeam._instance;
        var animatedText = new Text();
        var tmpText = new Object();
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
        bool il2cppAnimation = true;
        for (int frame = 0; frame < 60; frame++)
        {
            string before = "<color=#" + (frame % 2 == 0 ? "ff0000" : "0000ff") + "><link=\"choice," + (frame % 6) + "\">";
            string translated = before + "继续，走！</link></color>";
            var response = Extract(new[] { before + "Continue your adventure.</link></color>" }, JsonSerializer.Serialize(new { source = "cache", translated_text = translated }));
            il2cppAnimation &= response.Error == null && response.Result?[0] == translated.Replace("继续，", "继续,").Replace("！", "!");
        }
        Check("IL2CPP endpoint and TMP display preserve all 60 current animation tags and link ids", il2cppAnimation);
        Console.WriteLine("Unity endpoint contract: PASS=" + passed + " FAIL=" + failed);
        return failed == 0 ? 0 : 1;
    }
}
