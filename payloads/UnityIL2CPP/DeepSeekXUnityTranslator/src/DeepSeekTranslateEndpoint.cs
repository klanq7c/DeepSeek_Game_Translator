using System;
using System.Collections;
using System.Collections.Generic;
using System.Diagnostics;
using System.Net;
using System.Reflection;
using System.Text;
using XUnity.AutoTranslator.Plugin.Core.Endpoints;
using XUnity.AutoTranslator.Plugin.Core.Endpoints.Http;
using XUnity.AutoTranslator.Plugin.Core.Web;

namespace DeepSeekTranslate;

/*
 * Unity IL2CPP / XUnity 专用翻译端点。
 *
 * 真实调用链：
 *   XUnity -> Initialize -> OnCreateRequest -> 本地 C 服务 /translate 或 /batch
 *          -> OnExtractTranslation -> XUnity 把结果写回游戏文本。
 *
 * 本类不是 Unity Mono 插件入口，也不会被 Ren'Py / RPG Maker 使用。
 * 它只适配 XUnity 的 HttpEndpoint 接口；翻译缓存、远程 API 队列和持久化
 * 仍全部归本地 C 服务所有。服务返回 source=miss/queued 时必须 Fail，让
 * XUnity 保留原文并按自身策略重试，不能把原文误记成成功译文。
 *
 * 标点替换属于 IL2CPP/TMP 渲染兼容层，只改交给 XUnity 的显示结果，
 * 不回写共享翻译缓存。
 */
public sealed class DeepSeekTranslateEndpoint : HttpEndpoint
{
    private string _baseUrl = "http://127.0.0.1:19999";
    private int _maxBatch = 16;
    private int _maxConcurrency = 8;
    private float _queueWaitSeconds = 30f;
    private float _queuePollIntervalSeconds = 0.2f;
    private bool _displaySafePunctuation = true;

    public override string Id => "DeepSeekTranslate";
    public override string FriendlyName => "DeepSeek Local Batch";
    public override int MaxConcurrency => _maxConcurrency;
    public override int MaxTranslationsPerRequest => _maxBatch;

    /* XUnity 在加载端点时调用一次。配置写入 XUnity 自己的 ini；
       钳制上限是为了与本地服务批量大小和通道池容量保持可控。 */
    public override void Initialize(IInitializationContext context)
    {
        _baseUrl = TrimSlash(context.GetOrCreateSetting("DeepSeek", "Url", _baseUrl));
        _maxBatch = Clamp(context.GetOrCreateSetting("DeepSeek", "MaxBatchSize", _maxBatch), 1, 32);
        _maxConcurrency = Clamp(context.GetOrCreateSetting("DeepSeek", "MaxConcurrency", _maxConcurrency), 1, 8);
        _queueWaitSeconds = ClampQueueWait(context.GetOrCreateSetting("DeepSeek", "QueueWaitSeconds", _queueWaitSeconds));
        _queuePollIntervalSeconds = ClampQueuePollInterval(context.GetOrCreateSetting("DeepSeek", "QueuePollIntervalSeconds", _queuePollIntervalSeconds));
        _displaySafePunctuation = context.GetOrCreateSetting("DeepSeek", "DisplaySafePunctuation", _displaySafePunctuation);
        context.SetTranslationDelay(ClampDelay(context.GetOrCreateSetting("DeepSeek", "TranslationDelay", 0.1f)));
        context.DisableSpamChecks();
    }

    /*
     * XUnity counts Fail calls as endpoint errors and disables an endpoint after a
     * short consecutive run. The local server intentionally returns queued/miss
     * immediately, so reporting that normal asynchronous state as an error races
     * the cache fill and can shut translation down before the first result arrives.
     *
     * Keep the XUnity job alive in its coroutine. The first cache-only request
     * queues missing work, later requests read /cache/lookup only, and the normal
     * extraction path still performs the final source allow-list and source-echo
     * checks. Wall-clock and polling bounds keep provider failures visible instead
     * of turning them into permanently pending successes.
     */
    public override IEnumerator OnBeforeTranslate(IHttpTranslationContext context)
    {
        string[] texts = GetUntranslatedTexts(context);
        string[] requestTexts = ProtectMixedCjkTextsForRequest(texts);
        bool batch = texts.Length > 1;
        string translateUrl = _baseUrl + (batch ? "/batch" : "/translate");
        string translatePayload = BuildPayload(requestTexts, !batch);

        var client = new XUnityWebClient();
        XUnityWebResponse initialResponse = client.Send(CreateRequest(translateUrl, translatePayload));
        IEnumerator initialIterator = initialResponse.GetSupportedEnumerator();
        while (initialIterator.MoveNext())
        {
            yield return initialIterator.Current;
        }

        if (!HasUsableResponse(initialResponse))
        {
            yield break;
        }

        if (!TryCollectPendingTexts(initialResponse.Data, requestTexts, out string[] pendingTexts) ||
            pendingTexts.Length == 0)
        {
            yield break;
        }

        string lookupUrl = _baseUrl + "/cache/lookup";
        string lookupPayload = BuildLookupPayload(pendingTexts);
        Stopwatch stopwatch = Stopwatch.StartNew();
        double nextPollAt = 0.0;

        while (stopwatch.Elapsed.TotalSeconds < _queueWaitSeconds)
        {
            while (stopwatch.Elapsed.TotalSeconds < nextPollAt)
            {
                yield return null;
            }

            XUnityWebResponse lookupResponse = client.Send(CreateRequest(lookupUrl, lookupPayload));
            IEnumerator lookupIterator = lookupResponse.GetSupportedEnumerator();
            while (lookupIterator.MoveNext())
            {
                yield return lookupIterator.Current;
            }

            if (!HasUsableResponse(lookupResponse))
            {
                yield break;
            }
            if (HasAllLookupHits(lookupResponse.Data, pendingTexts))
            {
                yield break;
            }

            nextPollAt = stopwatch.Elapsed.TotalSeconds + _queuePollIntervalSeconds;
        }
    }

    public override void OnCreateRequest(IHttpRequestCreationContext context)
    {
        string[] texts = GetUntranslatedTexts(context);
        string[] requestTexts = ProtectMixedCjkTextsForRequest(texts);

        /* 单条和批量必须走与 C 服务公开契约一致的两个路由。请求创建阶段
           不等待远程模型；是否命中缓存由本地服务立即回答。 */
        bool batch = texts.Length > 1;
        string url = _baseUrl + (batch ? "/batch" : "/translate");
        string payload = BuildPayload(requestTexts, !batch);
        context.Complete(CreateRequest(url, payload));
    }

    public override void OnExtractTranslation(IHttpTranslationExtractionContext context)
    {
        string data = context.Response != null ? context.Response.Data : null;
        if (string.IsNullOrEmpty(data))
        {
            context.Fail("DeepSeek local server returned an empty response.");
            return;
        }

        string[] original = context.UntranslatedTexts;
        if (original == null || original.Length == 0)
        {
            original = new[] { context.UntranslatedText ?? string.Empty };
        }
        string[] requestTexts = ProtectMixedCjkTextsForRequest(original);

        if (original.Length <= 1)
        {
            string source = ReadStringProperty(data, "source");
            if (IsPassSource(source))
            {
                if (!string.Equals(requestTexts[0], original[0], StringComparison.Ordinal))
                {
                    context.Fail("DeepSeek local server passed a mixed-language request that still contains untranslated text.");
                    return;
                }
                /*
                 * XUnity 5.6.1 treats every Complete call as a successful
                 * translation and normally persists it to its generated text
                 * cache, even when source == translation. Its public endpoint
                 * context has no "complete without persistence" operation, so
                 * mark this job session-only at the closest XUnity boundary
                 * before completing the identity result.
                 *
                 * A future incompatible XUnity context layout cannot be fixed
                 * by the local server. In that case this path fails closed:
                 * the original remains visible, no identity mapping is
                 * reported as a successful translation, and context.Fail
                 * records the concrete compatibility diagnostic.
                 */
                if (!TryKeepIdentityResultsSessionOnly(context, new[] { true }, out string identityError))
                {
                    context.Fail("DeepSeek could not mark the XUnity pass result as session-only: " + identityError);
                    return;
                }
                context.Complete(original[0]);
                return;
            }
            if (!IsResolvedSource(source))
            {
                context.Fail("DeepSeek local server has no resolved translation yet.");
                return;
            }

            string one = ReadStringProperty(data, "translated_text") ?? ReadStringProperty(data, "translation");
            ProtectMixedCjkForRequest(
                original[0],
                out string[] oneTokens,
                out string[] oneSegments);
            if (!TryRestoreMixedCjk(
                    one,
                    oneTokens,
                    oneSegments,
                    out one,
                    out string oneRestoreError))
            {
                context.Fail("DeepSeek mixed-language translation lost protected text: " + oneRestoreError);
                return;
            }
            one = PrepareDisplayTranslation(one);
            if (string.IsNullOrEmpty(one))
            {
                context.Fail("DeepSeek local server returned no translation.");
                return;
            }
            if (string.Equals(one, original[0], StringComparison.Ordinal))
            {
                context.Fail("DeepSeek local server returned the untranslated source text.");
                return;
            }

            context.Complete(one);
            return;
        }

        /* 新版服务优先返回与输入同序的 results；translations 映射是为旧版
           服务保留的兼容读取路径，两者都必须完整覆盖原始批次。 */
        string[] results = ReadStringArrayProperty(data, "results");
        if (results == null || results.Length != original.Length)
        {
            results = ReadMapValues(data, "translations", requestTexts);
        }
        if (results == null || results.Length != original.Length)
        {
            context.Fail("DeepSeek local server returned an invalid batch response.");
            return;
        }

        string[] sources = ReadStringArrayProperty(data, "sources");
        if (!HasOnlyResolvedOrPassSources(sources, original.Length))
        {
            context.Fail("DeepSeek local server has unresolved batch translations.");
            return;
        }
        bool[] identityResults = new bool[results.Length];
        bool hasIdentityResult = false;
        for (int i = 0; i < results.Length; i++)
        {
            ProtectMixedCjkForRequest(
                original[i],
                out string[] itemTokens,
                out string[] itemSegments);
            /* 按 sources[i] 区分：pass 条目是恒等终态，放行原文；
               非 pass 条目回显原文仍是坏译文，整批 Fail。 */
            if (IsPassSource(sources[i]))
            {
                if (itemTokens.Length > 0)
                {
                    context.Fail("DeepSeek local server passed a mixed-language batch item that still contains untranslated text.");
                    return;
                }
                identityResults[i] = true;
                hasIdentityResult = true;
                results[i] = original[i];
                continue;
            }
            if (!TryRestoreMixedCjk(
                    results[i],
                    itemTokens,
                    itemSegments,
                    out results[i],
                    out string itemRestoreError))
            {
                context.Fail("DeepSeek mixed-language batch translation lost protected text: " + itemRestoreError);
                return;
            }
            if (string.IsNullOrEmpty(results[i]) || string.Equals(results[i], original[i], StringComparison.Ordinal))
            {
                context.Fail("DeepSeek local server returned an unresolved batch item.");
                return;
            }
        }
        results = PrepareDisplayTranslations(results);

        if (hasIdentityResult &&
            !TryKeepIdentityResultsSessionOnly(context, identityResults, out string batchIdentityError))
        {
            context.Fail("DeepSeek could not mark XUnity batch pass results as session-only: " + batchIdentityError);
            return;
        }

        context.Complete(results);
    }

    private string[] PrepareDisplayTranslations(string[] values)
    {
        if (values == null) return values;
        for (int i = 0; i < values.Length; i++)
        {
            values[i] = StripTranslationPromptEchoPrefix(values[i]);
            if (_displaySafePunctuation)
            {
                values[i] = NormalizeForTmpDisplay(values[i]);
            }
        }
        return values;
    }

    private string PrepareDisplayTranslation(string value)
    {
        value = StripTranslationPromptEchoPrefix(value);
        return _displaySafePunctuation ? NormalizeForTmpDisplay(value) : value;
    }

    private static readonly string[] TranslationPromptEchoPrefixes =
    {
        "\u7ffb\u8bd1\u6210\u7b80\u4f53\u4e2d\u6587",
        "\u7ffb\u8bd1\u4e3a\u7b80\u4f53\u4e2d\u6587",
        "\u8bd1\u6210\u7b80\u4f53\u4e2d\u6587",
        "\u7b80\u4f53\u4e2d\u6587\u7ffb\u8bd1",
        "\u7b80\u4f53\u4e2d\u6587\u8bd1\u6587",
        "Simplified Chinese translation",
        "Translation to Simplified Chinese",
        "Translate to Simplified Chinese",
        "Translated into Simplified Chinese",
        "Translate this exact game text to Simplified Chinese. Return only the translation."
    };

    private static string StripTranslationPromptEchoPrefix(string text)
    {
        if (string.IsNullOrWhiteSpace(text)) return text;
        string candidate = text.TrimStart();
        bool changed = false;
        for (int pass = 0; pass < 3; pass++)
        {
            bool stripped = false;
            foreach (string prefix in TranslationPromptEchoPrefixes)
            {
                if (!candidate.StartsWith(prefix, StringComparison.OrdinalIgnoreCase)) continue;
                int index = prefix.Length;
                while (index < candidate.Length && (candidate[index] == ' ' || candidate[index] == '\t')) index++;
                if (index < candidate.Length && (candidate[index] == ':' || candidate[index] == '\uff1a'))
                {
                    index++;
                }
                else if (index >= candidate.Length || (candidate[index] != '\r' && candidate[index] != '\n'))
                {
                    continue;
                }
                while (index < candidate.Length && char.IsWhiteSpace(candidate[index])) index++;
                if (index >= candidate.Length) continue;
                candidate = candidate.Substring(index).TrimEnd();
                changed = true;
                stripped = true;
                break;
            }
            if (!stripped) break;
        }
        return changed ? candidate : text;
    }

    private static string BuildPayload(string[] texts, bool single)
    {
        var sb = new StringBuilder(texts.Length * 64 + 32);
        if (single)
        {
            sb.Append("{\"text\":");
            AppendJsonString(sb, texts[0]);
            sb.Append(",\"cache_only\":true}");
        }
        else
        {
            sb.Append("{\"texts\":[");
            for (int i = 0; i < texts.Length; i++)
            {
                if (i != 0) sb.Append(',');
                AppendJsonString(sb, texts[i]);
            }
            sb.Append("],\"cache_only\":true}");
        }
        return sb.ToString();
    }

    private static string BuildLookupPayload(string[] texts)
    {
        var sb = new StringBuilder(texts.Length * 64 + 24);
        sb.Append("{\"texts\":[");
        for (int i = 0; i < texts.Length; i++)
        {
            if (i != 0) sb.Append(',');
            AppendJsonString(sb, texts[i]);
        }
        sb.Append("]}");
        return sb.ToString();
    }

    /*
     * Some Unity games progressively append English to a TMP component whose
     * prefix XUnity already replaced with Chinese. The shared server correctly
     * treats ordinary CJK text as terminal pass-through, so sending that mixed
     * component unchanged would strand the new English suffix.
     *
     * Protect existing Han runs with stable ASCII variables only for text that
     * also contains a meaningful untranslated Latin passage. The local server
     * can then translate the suffix without weakening the cross-engine CJK
     * heuristic. Every protected CJK token must survive exactly once; missing
     * or duplicated tokens fail closed in TryRestoreMixedCjk and are reported
     * through XUnity's context.Fail boundary.
     */
    private static string[] ProtectMixedCjkTextsForRequest(string[] texts)
    {
        var protectedTexts = new string[texts.Length];
        for (int i = 0; i < texts.Length; i++)
        {
            protectedTexts[i] = ProtectMixedCjkForRequest(
                texts[i],
                out _,
                out _);
        }
        return protectedTexts;
    }

    private static string ProtectMixedCjkForRequest(
        string text,
        out string[] tokens,
        out string[] protectedSegments)
    {
        tokens = Array.Empty<string>();
        protectedSegments = Array.Empty<string>();
        if (string.IsNullOrEmpty(text) ||
            !ContainsCjk(text) ||
            !HasMeaningfulLatinOutsideMarkup(text))
        {
            return text;
        }

        var tokenList = new List<string>();
        var segmentList = new List<string>();
        var output = new StringBuilder(text.Length + 64);
        string prefix = "{{DST_CJK_" + StableTextHash(text).ToString("X16");
        while (text.IndexOf(prefix, StringComparison.Ordinal) >= 0)
        {
            prefix += "X";
        }

        int copyStart = 0;
        for (int i = 0; i < text.Length;)
        {
            if (!IsBasicCjk(text[i]))
            {
                i++;
                continue;
            }

            int segmentStart = i;
            while (i < text.Length && IsBasicCjk(text[i])) i++;
            output.Append(text, copyStart, segmentStart - copyStart);

            string segment = text.Substring(segmentStart, i - segmentStart);
            string token = prefix + "_" + tokenList.Count.ToString("D4") + "}}";
            output.Append(token);
            tokenList.Add(token);
            segmentList.Add(segment);
            copyStart = i;
        }
        output.Append(text, copyStart, text.Length - copyStart);

        tokens = tokenList.ToArray();
        protectedSegments = segmentList.ToArray();
        return output.ToString();
    }

    private static bool TryRestoreMixedCjk(
        string translated,
        string[] tokens,
        string[] protectedSegments,
        out string restored,
        out string diagnostic)
    {
        restored = translated;
        diagnostic = null;
        if (tokens == null || protectedSegments == null || tokens.Length != protectedSegments.Length)
        {
            diagnostic = "protected CJK token metadata is invalid.";
            return false;
        }
        if (tokens.Length == 0) return true;
        if (string.IsNullOrEmpty(translated))
        {
            diagnostic = "translation is empty before protected CJK token restoration.";
            return false;
        }

        for (int i = 0; i < tokens.Length; i++)
        {
            string token = tokens[i];
            int first = restored.IndexOf(token, StringComparison.Ordinal);
            if (first < 0)
            {
                diagnostic = "protected CJK token " + i + " is missing.";
                return false;
            }
            if (restored.IndexOf(token, first + token.Length, StringComparison.Ordinal) >= 0)
            {
                diagnostic = "protected CJK token " + i + " was duplicated.";
                return false;
            }
            restored = restored.Replace(token, protectedSegments[i]);
        }
        return true;
    }

    private static bool HasMeaningfulLatinOutsideMarkup(string text)
    {
        int letters = 0;
        int words = 0;
        int wordLength = 0;
        bool inTag = false;
        for (int i = 0; i < text.Length; i++)
        {
            char ch = text[i];
            if (inTag)
            {
                if (ch == '>') inTag = false;
                continue;
            }
            if (ch == '<')
            {
                if (wordLength >= 2) words++;
                wordLength = 0;
                inTag = true;
                continue;
            }
            if ((ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z'))
            {
                letters++;
                wordLength++;
                continue;
            }
            if (wordLength >= 2) words++;
            wordLength = 0;
        }
        if (wordLength >= 2) words++;
        return letters >= 16 && words >= 3;
    }

    private static bool IsBasicCjk(char ch)
    {
        return ch >= '\u4e00' && ch <= '\u9fff';
    }

    private static ulong StableTextHash(string text)
    {
        const ulong offset = 14695981039346656037UL;
        const ulong prime = 1099511628211UL;
        ulong hash = offset;
        for (int i = 0; i < text.Length; i++)
        {
            hash ^= text[i];
            hash *= prime;
        }
        return hash;
    }

    private static string TrimSlash(string value)
    {
        string trimmed = value?.Trim();
        if (string.IsNullOrEmpty(trimmed)) return "http://127.0.0.1:19999";
        trimmed = trimmed.TrimEnd('/');
        return string.IsNullOrEmpty(trimmed) ? "http://127.0.0.1:19999" : trimmed;
    }

    private static int Clamp(int value, int min, int max)
    {
        if (value < min) return min;
        if (value > max) return max;
        return value;
    }

    private static float ClampDelay(float value)
    {
        if (value < 0.1f) return 0.1f;
        if (value > 1.0f) return 1.0f;
        return value;
    }

    private static float ClampQueueWait(float value)
    {
        if (value < 2f) return 2f;
        if (value > 60f) return 60f;
        return value;
    }

    private static float ClampQueuePollInterval(float value)
    {
        if (value < 0.1f) return 0.1f;
        if (value > 1f) return 1f;
        return value;
    }

    private static string NormalizeForTmpDisplay(string text)
    {
        if (string.IsNullOrEmpty(text))
        {
            return text;
        }

        /* 只处理常见缺字标点。正文、富文本标签和缓存键保持原样，
           避免把某个 TMP 字体的限制扩散到共享翻译记忆。 */
        bool containsCjk = ContainsCjk(text);
        if (!containsCjk && !ContainsAlwaysSafePunctuation(text))
        {
            return text;
        }

        StringBuilder sb = null;
        for (int i = 0; i < text.Length; i++)
        {
            string replacement = GetFallbackSafeReplacement(text[i], containsCjk);
            if (replacement == null)
            {
                if (sb != null) sb.Append(text[i]);
                continue;
            }

            sb ??= new StringBuilder(text.Length + 4).Append(text, 0, i);
            sb.Append(replacement);
        }

        return sb == null ? text : sb.ToString();
    }

    private static bool ContainsCjk(string text)
    {
        for (int i = 0; i < text.Length; i++)
        {
            char ch = text[i];
            if (ch >= '\u4e00' && ch <= '\u9fff') return true;
        }
        return false;
    }

    private static bool ContainsAlwaysSafePunctuation(string text)
    {
        for (int i = 0; i < text.Length; i++)
        {
            switch (text[i])
            {
                case '\u3001':
                case '\u3002':
                case '\uff01':
                case '\uff08':
                case '\uff09':
                case '\uff0c':
                case '\uff0e':
                case '\uff0f':
                case '\uff1a':
                case '\uff1b':
                case '\uff1f':
                case '\uff3b':
                case '\uff3c':
                case '\uff3d':
                case '\uff5b':
                case '\uff5d':
                case '\uff5e':
                case '\uffe5':
                    return true;
            }
        }
        return false;
    }

    private static string GetFallbackSafeReplacement(char ch, bool containsCjk)
    {
        switch (ch)
        {
            case '\u3001': return ",";
            case '\u3002': return ".";
            case '\uff01': return "!";
            case '\uff08': return "(";
            case '\uff09': return ")";
            case '\uff0c': return ",";
            case '\uff0e': return ".";
            case '\uff0f': return "/";
            case '\uff1a': return ":";
            case '\uff1b': return ";";
            case '\uff1f': return "?";
            case '\uff3b': return "[";
            case '\uff3c': return "\\";
            case '\uff3d': return "]";
            case '\uff5b': return "{";
            case '\uff5d': return "}";
            case '\uff5e': return "~";
            case '\uffe5': return "\u00a5";
            case '\u00a0':
                return containsCjk ? " " : null;
            case '\u2018':
            case '\u2019':
            case '\u201a':
            case '\u201b':
                return containsCjk ? "'" : null;
            case '\u201c':
            case '\u201d':
            case '\u201e':
            case '\u201f':
                return containsCjk ? "\"" : null;
            case '\u2013':
            case '\u2014':
            case '\u2015':
                return containsCjk ? "-" : null;
            case '\u2026':
                return containsCjk ? "..." : null;
            default:
                return null;
        }
    }

    private static bool IsResolvedSource(string source)
    {
        return string.Equals(source, "cache", StringComparison.OrdinalIgnoreCase)
            || string.Equals(source, "api", StringComparison.OrdinalIgnoreCase)
            || string.Equals(source, "api_batch", StringComparison.OrdinalIgnoreCase);
    }

    /* 服务端对“无需翻译”的文本（已含 CJK、无翻译信号、空串等）返回 source=pass，
       结果恒等于原文。pass 是恒等终态而不是失败：按原文完成当前 XUnity job，
       但先关闭 SaveResultGlobally，避免写入自动生成翻译文件。端点请求全部带
       cache_only，恒等放行也绝不会写进共享翻译缓存。
       注意不能并入 IsResolvedSource：轮询分类里 pass 必须保持 Terminal，
       否则混合批次会等一个永远不会进缓存的 pass 文本直到轮询超时。 */
    private static bool IsPassSource(string source)
    {
        return string.Equals(source, "pass", StringComparison.OrdinalIgnoreCase);
    }

    private static bool HasOnlyResolvedOrPassSources(string[] sources, int expectedLength)
    {
        if (sources == null || sources.Length != expectedLength) return false;
        for (int i = 0; i < sources.Length; i++)
        {
            if (!IsResolvedSource(sources[i]) && !IsPassSource(sources[i])) return false;
        }
        return true;
    }

    private static string[] GetUntranslatedTexts(ITranslationContextBase context)
    {
        string[] texts = context.UntranslatedTexts;
        if (texts == null || texts.Length == 0)
        {
            texts = new[] { context.UntranslatedText ?? string.Empty };
        }
        return texts;
    }

    private static XUnityWebRequest CreateRequest(string url, string payload)
    {
        var request = new XUnityWebRequest("POST", url, payload);
        request.Headers[HttpRequestHeader.Accept] = "application/json";
        return request;
    }

    private static bool HasUsableResponse(XUnityWebResponse response)
    {
        return response != null
            && !response.IsTimedOut
            && response.Error == null
            && !string.IsNullOrEmpty(response.Data);
    }

    /*
     * Build the exact subset that can still enter the shared cache. pass is a
     * terminal identity result and must never be included in /cache/lookup;
     * resolved entries already hit the cache. Unknown or malformed source
     * metadata returns false so the normal extraction path can fail closed
     * without spending the queue wait budget on an invalid response.
     */
    private static bool TryCollectPendingTexts(string data, string[] texts, out string[] pendingTexts)
    {
        pendingTexts = Array.Empty<string>();
        if (texts == null || texts.Length == 0)
        {
            return false;
        }

        if (texts.Length <= 1)
        {
            string source = ReadStringProperty(data, "source");
            if (IsResolvedSource(source) || IsPassSource(source))
            {
                return true;
            }
            if (IsPendingSource(source))
            {
                pendingTexts = new[] { texts[0] };
                return true;
            }
            return false;
        }

        string[] sources = ReadStringArrayProperty(data, "sources");
        if (sources == null || sources.Length != texts.Length)
        {
            return false;
        }

        var pending = new List<string>();
        for (int i = 0; i < sources.Length; i++)
        {
            if (IsResolvedSource(sources[i]) || IsPassSource(sources[i]))
            {
                continue;
            }
            if (!IsPendingSource(sources[i]))
            {
                return false;
            }
            pending.Add(texts[i]);
        }
        pendingTexts = pending.ToArray();
        return true;
    }

    private static bool IsPendingSource(string source)
    {
        return string.Equals(source, "queued", StringComparison.OrdinalIgnoreCase)
            || string.Equals(source, "miss", StringComparison.OrdinalIgnoreCase);
    }

    private static bool HasAllLookupHits(string data, string[] texts)
    {
        string[] hits = ReadMapValues(data, "hits", texts);
        return hits != null && hits.Length == texts.Length;
    }

    /*
     * XUnity's endpoint API exposes only Complete/Fail. The bundled 5.6.1
     * implementation stores its TranslationJob in the private completion
     * callback closure, while SaveResultGlobally is the supported job flag
     * controlling the generated translation file. Reflection is restricted to
     * this external compatibility boundary. Any layout mismatch is returned to
     * OnExtractTranslation and emitted through context.Fail; it is never
     * converted into a successful identity translation.
     */
    private static bool TryKeepIdentityResultsSessionOnly(
        IHttpTranslationExtractionContext context,
        bool[] identityResults,
        out string diagnostic)
    {
        diagnostic = null;
        if (context == null || identityResults == null || identityResults.Length == 0)
        {
            diagnostic = "invalid identity-result context.";
            return false;
        }

        try
        {
            FieldInfo innerField = FindInstanceField(context.GetType(), "_context");
            object innerContext = innerField?.GetValue(context);
            if (innerContext == null)
            {
                diagnostic = "the XUnity HTTP context no longer exposes its translation context.";
                return false;
            }

            FieldInfo completeField = FindInstanceField(innerContext.GetType(), "_complete");
            Delegate complete = completeField?.GetValue(innerContext) as Delegate;
            object callbackOwner = complete?.Target;
            if (callbackOwner == null)
            {
                diagnostic = "the XUnity completion callback has no job owner.";
                return false;
            }

            FieldInfo jobsField = FindInstanceField(callbackOwner.GetType(), "jobsArray");
            if (jobsField?.GetValue(callbackOwner) is Array jobs)
            {
                if (jobs.Length != identityResults.Length)
                {
                    diagnostic = "the XUnity batch job count does not match the pass-result count.";
                    return false;
                }

                for (int i = 0; i < identityResults.Length; i++)
                {
                    if (identityResults[i] &&
                        !TrySetJobSessionOnly(jobs.GetValue(i), out diagnostic))
                    {
                        return false;
                    }
                }
                return true;
            }

            FieldInfo jobField = FindInstanceField(callbackOwner.GetType(), "job");
            object job = jobField?.GetValue(callbackOwner);
            if (identityResults.Length != 1 || !identityResults[0] || job == null)
            {
                diagnostic = "the XUnity completion callback does not expose the expected translation job.";
                return false;
            }
            return TrySetJobSessionOnly(job, out diagnostic);
        }
        catch (Exception ex)
        {
            diagnostic = ex.GetType().FullName + ": " + ex.Message;
            return false;
        }
    }

    private static bool TrySetJobSessionOnly(object job, out string diagnostic)
    {
        diagnostic = null;
        if (job == null)
        {
            diagnostic = "the XUnity translation job is null.";
            return false;
        }

        PropertyInfo saveResultGlobally = job.GetType().GetProperty(
            "SaveResultGlobally",
            BindingFlags.Instance | BindingFlags.Public | BindingFlags.NonPublic);
        if (saveResultGlobally == null || !saveResultGlobally.CanWrite ||
            saveResultGlobally.PropertyType != typeof(bool))
        {
            diagnostic = "the XUnity translation job has no writable SaveResultGlobally flag.";
            return false;
        }

        saveResultGlobally.SetValue(job, false);
        return true;
    }

    private static FieldInfo FindInstanceField(Type type, string name)
    {
        for (Type current = type; current != null; current = current.BaseType)
        {
            FieldInfo field = current.GetField(
                name,
                BindingFlags.Instance | BindingFlags.Public | BindingFlags.NonPublic |
                BindingFlags.DeclaredOnly);
            if (field != null)
            {
                return field;
            }
        }
        return null;
    }

    private static void AppendJsonString(StringBuilder sb, string value)
    {
        sb.Append('"');
        if (value != null)
        {
            foreach (char ch in value)
            {
                switch (ch)
                {
                    case '\\': sb.Append("\\\\"); break;
                    case '"': sb.Append("\\\""); break;
                    case '\n': sb.Append("\\n"); break;
                    case '\r': sb.Append("\\r"); break;
                    case '\t': sb.Append("\\t"); break;
                    default:
                        if (ch < ' ')
                        {
                            sb.Append("\\u");
                            sb.Append(((int)ch).ToString("x4"));
                        }
                        else
                        {
                            sb.Append(ch);
                        }
                        break;
                }
            }
        }
        sb.Append('"');
    }

    /*
     * 以下是面向本地服务固定响应结构的最小 JSON 读取器。
     * 该 payload 只引用 XUnity Core，故不额外携带 Newtonsoft.Json。
     * 解析失败统一返回 null，由 OnExtractTranslation 走 Fail 降级。
     */
    private static string ReadStringProperty(string json, string name)
    {
        int value = FindPropertyValue(json, name, 0);
        if (value < 0) return null;
        return ReadJsonString(json, ref value);
    }

    private static string[] ReadStringArrayProperty(string json, string name)
    {
        int value = FindPropertyValue(json, name, 0);
        if (value < 0) return null;
        return ReadJsonStringArray(json, ref value);
    }

    private static string[] ReadMapValues(string json, string propertyName, string[] keys)
    {
        int value = FindPropertyValue(json, propertyName, 0);
        if (value < 0 || value >= json.Length || json[value] != '{') return null;
        var map = new Dictionary<string, string>();
        value++;
        while (value < json.Length)
        {
            SkipWs(json, ref value);
            if (value < json.Length && json[value] == '}') break;
            string key = ReadJsonString(json, ref value);
            if (key == null) return null;
            SkipWs(json, ref value);
            if (value >= json.Length || json[value++] != ':') return null;
            string val = ReadJsonString(json, ref value);
            if (val == null) return null;
            map[key] = val;
            SkipWs(json, ref value);
            if (value < json.Length && json[value] == ',') value++;
        }

        var result = new string[keys.Length];
        for (int i = 0; i < keys.Length; i++)
        {
            if (!map.TryGetValue(keys[i], out result[i])) return null;
        }
        return result;
    }

    /* 只匹配响应顶层对象的键：批量响应里 translations/hits 映射的键是任意
       游戏文本，若恰好等于 "results"/"sources"/"hits" 等属性名，无深度跟踪
       的扫描会先命中嵌套键导致解析错位。用 {} 深度把匹配限制在顶层。 */
    private static int FindPropertyValue(string json, string name, int start)
    {
        int i = Math.Max(0, start);
        int depth = 0;
        while (i < json.Length)
        {
            char ch = json[i];
            if (ch == '"')
            {
                int keyStart = i;
                string key = ReadJsonString(json, ref i);
                if (key == null)
                {
                    i = keyStart + 1;
                    continue;
                }
                SkipWs(json, ref i);
                if (depth == 1 && i < json.Length && json[i] == ':' && key == name)
                {
                    i++;
                    SkipWs(json, ref i);
                    return i;
                }
                continue;
            }
            if (ch == '{')
            {
                depth++;
            }
            else if (ch == '}')
            {
                depth--;
                if (depth <= 0) return -1;
            }
            i++;
        }
        return -1;
    }

    private static string[] ReadJsonStringArray(string json, ref int i)
    {
        SkipWs(json, ref i);
        if (i >= json.Length || json[i++] != '[') return null;
        var values = new List<string>();
        while (i < json.Length)
        {
            SkipWs(json, ref i);
            if (i < json.Length && json[i] == ']')
            {
                i++;
                return values.ToArray();
            }
            string value = ReadJsonString(json, ref i);
            if (value == null) return null;
            values.Add(value);
            SkipWs(json, ref i);
            if (i < json.Length && json[i] == ',') i++;
        }
        return null;
    }

    private static string ReadJsonString(string json, ref int i)
    {
        SkipWs(json, ref i);
        if (i >= json.Length || json[i++] != '"') return null;
        var sb = new StringBuilder();
        while (i < json.Length)
        {
            char ch = json[i++];
            if (ch == '"') return sb.ToString();
            if (ch != '\\')
            {
                sb.Append(ch);
                continue;
            }
            if (i >= json.Length) return null;
            char esc = json[i++];
            switch (esc)
            {
                case '"': sb.Append('"'); break;
                case '\\': sb.Append('\\'); break;
                case '/': sb.Append('/'); break;
                case 'b': sb.Append('\b'); break;
                case 'f': sb.Append('\f'); break;
                case 'n': sb.Append('\n'); break;
                case 'r': sb.Append('\r'); break;
                case 't': sb.Append('\t'); break;
                case 'u':
                    if (i + 4 > json.Length) return null;
                    if (!ushort.TryParse(json.Substring(i, 4), System.Globalization.NumberStyles.HexNumber, null, out ushort cp)) return null;
                    sb.Append((char)cp);
                    i += 4;
                    break;
                default:
                    return null;
            }
        }
        return null;
    }

    private static void SkipWs(string json, ref int i)
    {
        while (i < json.Length)
        {
            char ch = json[i];
            if (ch != ' ' && ch != '\t' && ch != '\r' && ch != '\n') break;
            i++;
        }
    }
}
