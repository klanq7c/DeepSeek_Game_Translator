using System.Reflection;
using System.Runtime.Loader;

if (args.Length != 2)
{
    Console.Error.WriteLine("usage: UnityEndpointStateProbe <endpoint-dll> <xunity-core-dll>");
    return 2;
}

string endpointPath = Path.GetFullPath(args[0]);
string xunityPath = Path.GetFullPath(args[1]);
AssemblyLoadContext.Default.Resolving += (_, name) =>
{
    if (string.Equals(
            name.Name,
            "XUnity.AutoTranslator.Plugin.Core",
            StringComparison.OrdinalIgnoreCase))
    {
        return AssemblyLoadContext.Default.LoadFromAssemblyPath(xunityPath);
    }
    return null;
};

Assembly endpoint = AssemblyLoadContext.Default.LoadFromAssemblyPath(endpointPath);
Type endpointType = endpoint.GetType(
    "DeepSeekTranslate.DeepSeekTranslateEndpoint",
    throwOnError: true)!;
MethodInfo collectPending = endpointType.GetMethod(
    "TryCollectPendingTexts",
    BindingFlags.NonPublic | BindingFlags.Static)
    ?? throw new MissingMethodException(endpointType.FullName, "TryCollectPendingTexts");
MethodInfo protectMixedCjk = endpointType.GetMethod(
    "ProtectMixedCjkForRequest",
    BindingFlags.NonPublic | BindingFlags.Static)
    ?? throw new MissingMethodException(endpointType.FullName, "ProtectMixedCjkForRequest");
MethodInfo restoreMixedCjk = endpointType.GetMethod(
    "TryRestoreMixedCjk",
    BindingFlags.NonPublic | BindingFlags.Static)
    ?? throw new MissingMethodException(endpointType.FullName, "TryRestoreMixedCjk");

static void AssertEqual<T>(T expected, T actual, string label)
    where T : notnull
{
    if (!EqualityComparer<T>.Default.Equals(expected, actual))
    {
        throw new InvalidOperationException(
            $"{label}: expected '{expected}', got '{actual}'");
    }
}

void AssertPending(
    string label,
    string json,
    string[] texts,
    bool expectedValid,
    params string[] expectedPending)
{
    object?[] invokeArgs = { json, texts, null };
    bool valid = (bool)(collectPending.Invoke(null, invokeArgs)
        ?? throw new InvalidOperationException($"{label}: method returned null"));
    string[] pending = (string[]?)invokeArgs[2] ?? Array.Empty<string>();
    AssertEqual(expectedValid, valid, label + " validity");
    AssertEqual(
        string.Join("\u001f", expectedPending),
        string.Join("\u001f", pending),
        label + " pending subset");
}

AssertPending(
    "mixed pass/pending/resolved",
    "{\"sources\":[\"pass\",\"queued\",\"cache\",\"miss\"]}",
    new[] { "identity", "queue-a", "resolved", "queue-b" },
    true,
    "queue-a",
    "queue-b");
AssertPending(
    "terminal-only batch",
    "{\"sources\":[\"pass\",\"cache\"]}",
    new[] { "identity", "resolved" },
    true);
AssertPending(
    "unknown source fails closed",
    "{\"sources\":[\"pass\",\"unexpected\"]}",
    new[] { "identity", "unknown" },
    false);
AssertPending(
    "single pass",
    "{\"source\":\"pass\"}",
    new[] { "identity" },
    true);
AssertPending(
    "single queued",
    "{\"source\":\"queued\"}",
    new[] { "queued" },
    true,
    "queued");

string mixedLongText =
    "<i><color=#5d42ff>已经翻译的角色介绍。</color></i><br><br>" +
    string.Join(
        " ",
        Enumerable.Repeat(
            "This newly appended English paragraph must still reach the local translator.",
            8));
object?[] protectArgs = { mixedLongText, null, null };
string protectedText = (string)(protectMixedCjk.Invoke(null, protectArgs)
    ?? throw new InvalidOperationException("mixed CJK protection returned null"));
string[] tokens = (string[]?)protectArgs[1] ?? Array.Empty<string>();
string[] protectedSegments = (string[]?)protectArgs[2] ?? Array.Empty<string>();
AssertEqual(true, mixedLongText.Length > 400, "mixed incremental source exceeds old XUnity limit");
AssertEqual(false, protectedText.Any(ch => ch is >= '\u4e00' and <= '\u9fff'),
    "protected request must bypass the shared CJK pass heuristic");
AssertEqual(true, protectedText.Contains("newly appended English", StringComparison.Ordinal),
    "untranslated English suffix must remain in the request");
AssertEqual(true, tokens.Length > 0, "mixed request must retain protected CJK tokens");
AssertEqual(tokens.Length, protectedSegments.Length, "protected token/segment count");

string translatedWithToken =
    protectedText.Replace(
        "This newly appended English paragraph must still reach the local translator.",
        "新追加的英文段落仍必须进入本地翻译器。",
        StringComparison.Ordinal);
object?[] restoreArgs = { translatedWithToken, tokens, protectedSegments, null, null };
bool restoredOk = (bool)(restoreMixedCjk.Invoke(null, restoreArgs)
    ?? throw new InvalidOperationException("mixed CJK restore returned null"));
string restored = (string?)restoreArgs[3] ?? string.Empty;
AssertEqual(true, restoredOk, "mixed CJK token restore");
AssertEqual(true, restored.Contains("已经翻译的角色介绍。", StringComparison.Ordinal),
    "existing Chinese prefix must survive restoration");
AssertEqual(true, restored.Contains("新追加的英文段落", StringComparison.Ordinal),
    "translated suffix must survive restoration");

string missingTokenResult = translatedWithToken.Replace(tokens[0], string.Empty, StringComparison.Ordinal);
object?[] missingTokenArgs = { missingTokenResult, tokens, protectedSegments, null, null };
bool missingTokenAccepted = (bool)(restoreMixedCjk.Invoke(null, missingTokenArgs)
    ?? throw new InvalidOperationException("missing-token restore returned null"));
AssertEqual(false, missingTokenAccepted, "missing protected CJK token must fail closed");

Console.WriteLine("Unity endpoint state probe passed.");
return 0;
