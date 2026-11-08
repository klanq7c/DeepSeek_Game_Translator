"""以源链接方式使用渲染器方法，模拟原生字体/属性边界。"""
import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


def method(source, name):
    match = re.search(r'^\s*(?:private|internal|public)\s+(?:static\s+)?(?:async\s+)?[\w<>,\[\] ()]+\s+' + name + r'(?:<[^>]+>)?\(', source, re.M)
    if not match:
        raise AssertionError('missing production method: ' + name)
    start = source.index('{', match.end())
    depth, i, quote = 1, start + 1, None
    while depth:
        if i >= len(source):
            raise AssertionError('unclosed production method: ' + name)
        ch = source[i]
        if quote:
            if ch == '\\': i += 2; continue
            if ch == quote: quote = None
        elif ch in ('"', "'"): quote = ch
        elif source.startswith('//', i):
            i = source.index('\n', i); continue
        elif source.startswith('/*', i):
            i = source.index('*/', i + 2) + 2; continue
        elif ch == '{': depth += 1
        elif ch == '}': depth -= 1
        i += 1
    return re.sub(r'\b(private|internal)\b', 'public', source[match.start():i], count=1)


mono = (ROOT / 'payloads/UnityTranslator/src/DeepSeekTranslator.cs').read_text(encoding='utf-8')
tmp = (ROOT / 'payloads/UnityIL2CPP/DeepSeekTMPFontFallback/src/TmpFontFallbackPlugin.cs').read_text(encoding='utf-8')
text = '''// Generated from the shipped payloads; do not edit.
using System;
using System.Collections.Generic;
using System.Reflection;
using System.Text;
using System.Linq;
using System.Text.RegularExpressions;
using System.Threading;
using System.Threading.Tasks;
using System.IO;
using Newtonsoft.Json.Linq;
public sealed class MonoRendererSeam {
    private object _chineseTMPFont = new object();
    private static readonly char[] TmpPunctuationFallbackChars = {'，','。','！','？','“','”','…'};
    public static readonly List<Exception> Errors = new List<Exception>();
    private static void ReportCaughtException(Exception error, string context, string operation) { Errors.Add(error); }
    private bool TryWarmTmpFontAssetVerified(object font, string text, out bool ignored) { ignored = false; return true; }
    private List<int> CollectMissingChars(object font, string text) { var result = new List<int>(); foreach (char ch in TmpPunctuationFallbackChars) result.Add(ch); return result; }
    private static string GetVisibleText(string text) { return text; }
'''
for name in ('NormalizeTmpPunctuationForMissingGlyphs', 'TryGetTmpPunctuationFallback', 'TryGetTextureReadability'):
    text += method(mono, name) + '\n'
text += '}\npublic static class Il2CppRendererSeam {\n'
for name in ('NormalizeTmpTextForFallback', 'ContainsCjk', 'ContainsAlwaysSafePunctuation', 'GetFallbackSafeReplacement'):
    text += method(tmp, name) + '\n'
text += '}\n'
text += r'''
// 仅模拟 Unity/原生属性和字体边界；属性设置器的决策使用生产代码。
public class Object { public readonly TestLogger Logger = new TestLogger(); }
public class TestLogger { public readonly List<string> Warnings = new List<string>(); public void LogWarning(string message) { Warnings.Add(message); } public void LogInfo(string message) { } }
public class Component : Object { public T GetComponent<T>() where T : class { return null; } }
public class Text : Component {
    public Object font;
    public bool supportRichText { get; set; }
    public Action OnWrite;
    private string _text;
    public string text { get { return _text; } set { _text = value; var action = OnWrite; OnWrite = null; if (action != null) action(); } }
}
public class TmpText : Component { public bool richText { get; set; } }
public class TmpOverlayMarker { }
public struct Rect { }
public class GUIStyle { public Object font; }
public class GUIContent { public string text; }
public sealed class MonoAnimatedSetterSeam : Object {
    public static MonoAnimatedSetterSeam _instance = new MonoAnimatedSetterSeam();
    public readonly Dictionary<string,string> Cache = new Dictionary<string,string>();
    private Dictionary<string,string> _cache { get { return Cache; } }
    private readonly Dictionary<string,string> _glossary = new Dictionary<string,string>();
    private int _cacheMutationGeneration;
    public bool UseProductionCacheLookup;
    public int Debounced, Applied, FontCoverageChecks;
    public bool TmpFontCoversText = true, PendingOverlay, OverlayRecordsCompletion;
    private int _hookCallCount;
    private Object _chineseFont = new Object();
    private bool CanHandleTmp() { return true; }
    private bool CanTranslateTmp() { return true; }
    private bool ShouldUseTmpOverlay() { return false; }
    private void BeginTmpExternalTextWrite(object node, string value) { }
    private static void RestoreTmpParagraphLayout(object node, string value) { }
    private static void RestoreTmpShortLabelLayout(object node, string value) { }
    private static void RestoreUGUIShortLabelLayoutCompatibility(object node) { }
    private void RestoreTmpOverlay(object node) { PendingOverlay = false; }
    private static int GetComponentInstanceId(object node) { return 1; }
    private bool TryRepairMixedTranslatedText(string value, out string original, out string repaired) { original = repaired = null; return false; }
    private static bool ContainsCjk(string value) { return value.Any(ch => ch >= 0x4e00 && ch <= 0x9fff); }
    private void ApplyTMPFont(object node) { Applied++; }
    private void ApplyFont(object node) { Applied++; if (node is Text text) text.font = _chineseFont; }
    private void ApplyImGuiFont(GUIStyle style) { Applied++; if (style != null) style.font = _chineseFont; }
    private void QueueImGuiTranslation(string value) { Debounced++; }
    private static GUIStyle GetEffectiveGUILayoutLabelStyle(GUIStyle style) { return style; }
    private static void PrepareImGuiTranslatedShortLabelLayout(ref Rect rect, GUIStyle style, string value, string original) { }
    // 格式决策、文本清理和组件准备均取生产代码；这里只隔离原生布局边界。
    public static readonly List<Exception> Errors = new List<Exception>();
    private static void ReportCaughtException(Exception error) { Errors.Add(error); }
    private static string RestoreOuterLineBreaks(string value, string original) { return value; }
    private static void PrepareUGUITranslatedShortLabelLayout(Text node, string value, string original) { }
    private static string ApplyUGUITextLayoutCompatibility(Text node, string value) { return value; }
    private static string PreserveUguiTypewriterSourceLength(Text node, string value, string original) { return value; }
    private string NormalizeTmpPunctuationForMissingGlyphs(string value) { return value; }
    private bool EnsureTMPFontCoversText(object node, string value) { FontCoverageChecks++; return TmpFontCoversText; }
    private bool ApplyTmpOverlay(object node, string value, string original, bool force = false, bool rememberCompletedOutput = true) { PendingOverlay = force; OverlayRecordsCompletion = rememberCompletedOutput; return true; }
    private static void RevealTmpText(object node, string value) { }
    private void MarkProcessed(int id, string value) { }
    private void TryMarkAppliedCacheKeyForPersist(string original, string value) { }
    private void ClearInProgress(int id, string value) { }
    private void ClearProcessedIfChanged(int id, string value) { }
    private void QueueDebouncedTextRequest(object node, int id, string value, bool isTmp) { Debounced++; }
    private string Translate(string value) { if (UseProductionCacheLookup) return TryGetLocalTranslation(value, out var translated) ? translated : value; return Cache.TryGetValue(value, out var result) ? result : value; }
    private static bool IsAcceptableTranslation(string source, string result) { return !string.IsNullOrWhiteSpace(result) && result != source; }
    private bool TryTranslateQuantityListFromLocalTerms(string source, out string result) { result = null; return false; }
    private void MarkLocalCacheKeyLocked(string key) { }
    private void ClearMixedRepairMemo() { }
    private void ClearTranslationRetryState(string key) { }
    private void ScheduleLocalCachePersist() { }
    private bool IsTranslationRetryCoolingDown(string value) { return false; }
    private void LogVerbose(string value) { }
    private static bool ShouldSkipText(string value) { return false; }
    private static string StripTrailingStatLines(string value) { return value; }
    private static bool EndsWithSentenceBoundary(string value) { return value.EndsWith(".") || value.EndsWith("!") || value.EndsWith("?"); }
'''
payload_type = re.search(r'private sealed class ProtectedTextPayload\b.*?\n\t}', mono, re.S)
assert payload_type, 'missing production protected text payload'
text += payload_type[0].replace('private sealed class', 'public sealed class', 1) + '\n'
for field in ('_tmpLineStructureRejections', 'CompletedMixedOutputs', 'CompletedMixedOutputOrder', 'RichTextTagRegex', 'NumericTokenRegex', 'LooseColorOpenFragmentRegex', 'LooseColorCloseFragmentRegex',
              'LooseSizeOpenFragmentRegex', 'LooseSizeCloseFragmentRegex', 'PlaceholderTokenRegex',
              'NestedBrokenRichTextRegex', 'BrokenOpeningRichTextDashRegex', 'BareClosingRichTextRegex',
              'BareOpeningRichTextRegex', 'EmptyAngleTagRegex', 'MalformedAngleFragmentRegex',
              'DanglingTagSlashRegex', 'MojibakeSignalChars', 'TranslationPromptEchoPrefixes', 'UnityTextSupportedTags'):
    declaration = re.search(r'^\s*private [^\r\n;]*\b' + field + r'\b[^;]*;', mono, re.M)
    assert declaration, 'missing production field: ' + field
    text += declaration[0] + '\n'
for name in ('GetCompletedOutputKey', 'RememberCompletedOutput', 'IsLocalizedText', 'ContainsRichTextTags', 'ReplaceRichTextTags', 'HaveSameRichTextTags', 'ShouldPreserveRichTextForDisplay',
             'ShouldPreserveRichTextForDisplayWithColor', 'ContainsColorRichTextTag',
             'TryGetOuterRichTextWrapper', 'IsColorOpeningTag', 'IsTmpColorShorthandTagBody',
             'GetVisibleText', 'NormalizeRequestText', 'LooksLikeUiText',
             'PrepareTranslatedTextForComponent', 'TryEnableRichText', 'SanitizeRichTextForUnityText',
             'SanitizeTranslationArtifacts', 'RepairUtf8DecodedAsGbk', 'GetMojibakeScore', 'CountCjkCharacters',
             'StripTranslationPromptEchoPrefix', 'RestoreLeadingTextDecoration',
             'ProtectTextForTranslation', 'RestoreProtectedText', 'IsTokenDominatedFragment', 'CollectWarmupRequestItems',
             'StoreCachedTranslation', 'TryGetLocalTranslation',
             'StripRichTextForPlainText', 'StripResidualRichTextFragments', 'NormalizeDisplayWhitespace',
             'PrepareTranslatedTextForUGUIText', 'PreparePendingCjkDisplay', 'PrepareTranslatedTextForString',
             'GUILabelPrefix', 'TranslateImGuiString', 'TranslateGUILayoutLabelContent', 'LooksLikeTypewriterFragment',
             'HasUnclosedOuterDialogueQuote', 'IsTranslatorOwnedTmpOverlay', 'TMPSetTextPrefix', 'UGUITextPrefix',
             'CountTextLineBreaks', 'TmpLineStructurePreserved', 'ReportTmpLineStructureRejection'):
    text += method(mono, name) + '\n'
# 同名重载中使用实际显示路径调用的 string 版本。
text += method(mono[mono.index('private static string RestoreOuterRichTextWrapper(string text, string originalText)'):], 'RestoreOuterRichTextWrapper') + '\n'
text += method(mono[mono.index('private static string RestoreOuterRichTextWrapper(string text, ProtectedTextPayload payload)'):], 'RestoreOuterRichTextWrapper') + '\n'
text += '}\n'
out = ROOT / 'build/unity_runtime_contract/GeneratedSeams.cs'
text += r'''
// 仅替换 Unity 时钟、对象枚举和显示属性边界，三种调度器与扫描器直接取生产源码。
public sealed class MonoCacheScanSeam : Object {
    public static MonoCacheScanSeam _instance;
    public int _textEnableHookCount, EnableLookups;
    public static class Time { public static float realtimeSinceStartup = 100; }
    public Type _tmpTextTypeCache = typeof(Object);
    public Object[] TmpObjects = new Object[] { new Object(), new Object() };
    public Text[] UguiObjects = new Text[] { new Text(), new Text() };
    public int TmpDiscoveries, UguiDiscoveries, Inspected;
    public bool TmpSupported = true, ThrowOnDiscover;
    public Action OnFlush, OnInspect, OnDiscover;
    public readonly HashSet<Object> Seen = new HashSet<Object>();
    public readonly HashSet<Object> ApplyHits = new HashSet<Object>();
    public readonly List<Exception> Errors = new List<Exception>();
    public bool SnapshotsReleased { get { return _cacheApplySnapshotDepth == 0 && _cacheApplyTmpSnapshot == null && _cacheApplyUguiSnapshot == null; } }
    private bool CanHandleTmp() { return TmpSupported; }
    private void EnsureScannerInitialized(string source) { }
    // 模拟 OnEnable 时控件尚未填入文字，因此即时查询不能命中。
    private bool QueueCachedComponentTextIfAvailable(object node, bool isTmp, bool allowRemoteFallback) { EnableLookups++; return false; }
    private Object[] FindTmpTextObjectsSafe(Type type) {
        TmpDiscoveries++;
        if (ThrowOnDiscover) throw new InvalidOperationException("discovery failed");
        Object[] result = (Object[])TmpObjects.Clone();
        var action = OnDiscover; OnDiscover = null; if (action != null) action();
        return result;
    }
    private T[] FindObjectsOfTypeAllSafe<T>() { UguiDiscoveries++; return (T[])(object)UguiObjects.Clone(); }
    private bool TryApplyCachedTmpText(Object node) {
        Inspected++; Seen.Add(node);
        var action = OnInspect; OnInspect = null; if (action != null) action();
        if (ApplyHits.Remove(node)) { _cacheApplyHitCount++; return true; }
        return false;
    }
    private bool TryApplyCachedUguiText(Text node) { return TryApplyCachedTmpText(node); }
    private void FlushPendingTranslations() { var action = OnFlush; OnFlush = null; if (action != null) action(); FlushPendingTranslationsFromSource(); }
    public class PendingTranslationApply {
        public WeakReference ComponentRef;
        public int InstanceId;
        public string OriginalText, TranslatedText;
        public bool IsTmp, PreserveRichText;
    }
    private readonly object _pendingLock = new object();
    public readonly List<PendingTranslationApply> _pendingApplyQueue = new List<PendingTranslationApply>();
    private readonly HashSet<string> _pendingApplyKeys = new HashSet<string>();
    private int _flushSkipped, _flushApplied;
    private static class AccessTools { public static PropertyInfo Property(Type type, string name) { return type.GetProperty(name); } }
    private int GetMaxPendingApplyPerFlush() { return 12; }
    private string BuildPendingApplyKey(int id, string original) { return id + original; }
    private static bool IsUnityObjectAlive(object node) { return node != null; }
    private bool IsSameSourceText(string a, string b) { return a == b; }
    private bool TryRepairMixedTranslatedText(string text, out string original, out string translated) { original = translated = null; return false; }
    private void RestoreTmpOverlay(object node) { }
    private void ApplyTMProTranslation(object node, int id, string original, string translated, PropertyInfo text, PropertyInfo font, MethodInfo force, bool preserve) { text.SetValue(node, translated); }
    private void ApplyFont(Text node) { }
    private string PrepareTranslatedTextForUGUIText(Text node, string translated, string original, bool preserve) { return translated; }
    private void MarkProcessed(int id, string source) { }
    private void TryMarkAppliedCacheKeyForPersist(string original, string translated) { }
    private void ReportCaughtException(Exception error, string context) { Errors.Add(error); }
}
'''.rstrip()[:-1]
fields = (
    '_tmpCacheScanWindow', '_uguiCacheScanWindow', '_cacheApplySnapshotDepth',
    '_cacheApplySnapshotGeneration', '_cacheApplyTmpSnapshot', '_cacheApplyUguiSnapshot',
    '_sceneCacheApplyPassesRemaining', '_sceneCacheApplyGeneration', '_sceneWarmupGeneration',
    '_nextSceneCacheApplyRealtime', '_uiCacheApplyUntilRealtime', '_nextUiCacheApplyRealtime',
    '_cacheMutationGeneration', '_lastPeriodicCacheApplyGeneration', '_nextPeriodicCacheApplyRealtime',
    '_cacheApplyHitCount', '_cacheApplyPassCount', '_periodicCacheApplyIdlePasses',
    '_lastUiActivationRealtime', '_uiActivationBurstCount',
    'SceneCacheApplyIntervalSeconds', 'MaxSceneCacheAppliesPerPass', 'MaxSceneCacheInspectionsPerPass',
    'UiActivationCacheApplyIntervalSeconds', 'MaxUiActivationCacheAppliesPerPass', 'MaxUiActivationCacheInspectionsPerPass',
    'MaxPeriodicCacheAppliesPerPass', 'MaxPeriodicCacheInspectionsPerPass', 'PeriodicCacheApplyActiveIntervalSeconds',
    'PeriodicCacheApplyIdleIntervalSeconds', 'PeriodicCacheApplyIdlePassThreshold',
    'UiActivationThrottleSeconds', 'UiActivationCacheApplyWindowSeconds')
for field in fields:
    declaration = re.search(r'^\s*private [^\r\n;]*\b' + field + r'\b[^\r\n;]*;', mono, re.M)
    assert declaration, 'missing production field: ' + field
    text += (declaration[0] if field.endswith('ScanWindow') else declaration[0].replace('private ', 'public ', 1)) + '\n'
text += method(mono, 'ScanCachedVisibleObjects').replace('public int ScanCachedVisibleObjects', 'private int ScanCachedVisibleObjects') + '\n'
text += method(mono, 'FlushPendingTranslations').replace('public void FlushPendingTranslations', 'private void FlushPendingTranslationsFromSource') + '\n'
for name in ('InvalidateCacheApplySnapshot', 'RunCacheApplyTicks', 'ApplyCachedVisibleTextPass',
             'RunSceneCacheApplyTick', 'RunUiCacheApplyTick', 'RunPeriodicCacheApplyTick', 'RequestUiCacheApplyBurst', 'TMPTextOnEnablePostfix'):
    text += method(mono, name) + '\n'
text += '}\n'
# 防止调用者绕开共享范围或漏掉生命周期失效；行为仍由上面的真实方法验证。
assert 'RunCacheApplyTicks();' in method(mono, 'PumpOnce')
assert 'RunCacheApplyTicks();' in method(mono, 'OnWillRenderCanvases')
assert 'InvalidateCacheApplySnapshot();' in method(mono, 'ResetSceneScopedState')
out.parent.mkdir(parents=True, exist_ok=True)
text += (ROOT / 'tests/unity_runtime_contract/CachePersistenceSeam.cs.txt').read_text(encoding='utf-8')
for name in ('BootCacheLoadAsync', 'LoadServerCache', 'TryIsolateOversizedLocalCacheLocked',
             'PrimeSmallLocalCacheForFirstFrame', 'ScheduleLocalCachePersist', 'PersistLocalCacheAsync',
             'FlushLocalCacheToDisk', 'WriteLocalCacheSnapshot', 'SerializeStringMap', 'EscapeJson'):
    text += method(mono, name) + '\n'
text += '}\n'
text += (ROOT / 'tests/unity_runtime_contract/TmpOverlaySeam.cs.txt').read_text(encoding='utf-8')
for name in ('TryUpdateTmpUguiOverlay', 'HideTmpSourceForUguiOverlay', 'DestroyTmpUguiOverlay'):
    text += method(tmp, name) + '\n'
text += '}\n'
for class_name, source in (('MonoFontPopulationSeam', mono), ('Il2CppFontPopulationSeam', tmp)):
    text += 'public static class ' + class_name + ' {\n'
    text += '''
    public static readonly List<Exception> Errors = new List<Exception>();
    private static string GetVisibleText(string value) { return value; }
    private static void ReportCaughtException(Exception error, string context = null, string operation = null) { Errors.Add(error); }
    private static class TmpFontFallbackPlugin {
        public static void ReportCaughtException(Exception error, string context = null, string operation = null) { Errors.Add(error); }
    }
'''
    text += method(source, 'TryAddCharactersToTmpFontAsset') + '\n'
    if source is mono:
        text += method(source, 'ContainsCjk') + '\n'
    if source is tmp:
        text += method(source, 'BuildTryAddCharactersArgs') + '\n'
        text += 'public static object _fallbackAsset;\n'
        declaration = re.search(r'^\s*private [^\r\n;]*\bWarmedFallbackCharacters\b[^;]*;', tmp, re.M)
        text += declaration[0].replace('private ', 'public ', 1) + '\n'
        declaration = re.search(r'^\s*private const string CjkWarmupText[^;]*;', tmp, re.M)
        text += declaration[0] + '\n'
        for name in ('ContainsCjk', 'IsCjkFontCharacter', 'FontTagNameIs', 'TrySkipTmpFontTag', 'NeedsCjkFont', 'TryWarmFallbackGlyphs'):
            text += method(source, name) + '\n'
    text += '}\n'
for name in ('LoadFallbackFontAsset', 'TryCompleteFallbackBundleAsync'):
    assert '"atlasPopulationMode", "Dynamic"' not in method(tmp, name), 'loaded static font must retain its native population mode'
assert 'WarmedFallbackCharacters.Clear();' in method(tmp, 'Apply')
for name in ('PatchUguiTexts', 'PatchTmpHostFontFromText', 'RefreshTmpTextsWithUguiOverlay'):
    assert 'NeedsCjkFont(' in method(tmp, name), 'font consumer must include punctuation: ' + name
assert 'NeedsCjkFont(value, false)' in method(tmp, 'PatchUguiTexts'), 'UGUI must not discard unsupported TMP tag text'
text += (ROOT / 'tests/unity_runtime_contract/DeepPrefetchSeam.cs.txt').read_text(encoding='utf-8')
for field in ('_deepPrefetchEnabled', '_deepScannedObjects', '_deepPrefetchSeen', '_deepPrefetchQueue',
              '_lastDeepPrefetchRealtime', '_deepPrefetchActive', 'DeepPrefetchInitialDelaySeconds',
              'DeepPrefetchScanIntervalSeconds', 'DeepPrefetchChunkPauseSeconds',
              'DeepPrefetchMaxObjectsPerScan', 'DeepPrefetchMaxTextsPerScan', 'DeepPrefetchBatchSize'):
    declaration = re.search(r'^\s*private [^\r\n;]*\b' + field + r'\b[^\r\n;]*;', mono, re.M)
    assert declaration, 'missing production field: ' + field
    text += declaration[0].replace('private ', 'public ', 1) + '\n'
# 可选的新窗口声明让同一测量夹具也能运行修复前的生产源。
window = re.search(r'^\s*private readonly DeepPrefetchScanWindow _deepPrefetchScanWindow[^;]*;', mono, re.M)
if window:
    text += window[0].replace('private ', 'internal ', 1) + '\n'
    assert '_deepPrefetchScanWindow.Reset();' in method(mono, 'ResetSceneScopedState')
assert '"DeepPrefetchRuntime", defaultValue: false' in mono, 'deep prefetch must remain opt-in'
for name in ('CollectDeepPrefetchCandidates', 'TryMarkDeepScanned', 'ExtractDeepObjectStrings',
             'ExtractDeepPrefetchText', 'ExtractQuotedSegments', 'RunDeepPrefetchTick', 'ProcessDeepPrefetchQueueAsync'):
    text += method(mono, name) + '\n'
text += '}\n'
out.write_text(text, encoding='utf-8', newline='\n')
print('Source-linked Unity renderer seams generated')
