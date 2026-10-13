"""以源链接方式使用渲染器方法，模拟原生字体/属性边界。"""
import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


def method(source, name):
    match = re.search(r'^\s*(?:private|internal)\s+(?:static\s+)?(?:string|bool|void|int)\s+' + name + r'(?:<[^>]+>)?\(', source, re.M)
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
public class TestLogger { public readonly List<string> Warnings = new List<string>(); public void LogWarning(string message) { Warnings.Add(message); } }
public class Component : Object { public T GetComponent<T>() where T : class { return null; } }
public class Text : Component {
    public Object font;
    public Action OnWrite;
    private string _text;
    public string text { get { return _text; } set { _text = value; var action = OnWrite; OnWrite = null; if (action != null) action(); } }
}
public class TmpOverlayMarker { }
public sealed class MonoAnimatedSetterSeam : Object {
    public static MonoAnimatedSetterSeam _instance = new MonoAnimatedSetterSeam();
    public readonly Dictionary<string,string> Cache = new Dictionary<string,string>();
    public int Debounced, Applied;
    private int _hookCallCount;
    private Object _chineseFont = new Object();
    private bool CanHandleTmp() { return true; }
    private bool CanTranslateTmp() { return true; }
    private bool ShouldUseTmpOverlay() { return false; }
    private void BeginTmpExternalTextWrite(object node, string value) { }
    private static void RestoreTmpParagraphLayout(object node, string value) { }
    private static void RestoreTmpShortLabelLayout(object node, string value) { }
    private static void RestoreUGUIShortLabelLayoutCompatibility(object node) { }
    private void RestoreTmpOverlay(object node) { }
    private static int GetComponentInstanceId(object node) { return 1; }
    private bool TryRepairMixedTranslatedText(string value, out string original, out string repaired) { original = repaired = null; return false; }
    private static bool ContainsCjk(string value) { return value.Any(ch => ch >= 0x4e00 && ch <= 0x9fff); }
    private void ApplyTMPFont(object node) { Applied++; }
    private void ApplyFont(object node) { Applied++; }
    private static bool ShouldPreserveRichTextForDisplayWithColor(string value, string translated) { return true; }
    private static string PrepareTranslatedTextForComponent(object node, string value, string original) { return value; }
    private static string PrepareTranslatedTextForUGUIText(object node, string value, string original) { return value; }
    private static string StripRichTextForPlainText(string value) { return value; }
    private string NormalizeTmpPunctuationForMissingGlyphs(string value) { return value; }
    private bool EnsureTMPFontCoversText(object node, string value) { return true; }
    private bool ApplyTmpOverlay(object node, string value, string original, bool force = false) { return true; }
    private static void RevealTmpText(object node, string value) { }
    private void MarkProcessed(int id, string value) { }
    private void TryMarkAppliedCacheKeyForPersist(string original, string value) { }
    private void ClearInProgress(int id, string value) { }
    private void ClearProcessedIfChanged(int id, string value) { }
    private void QueueDebouncedTextRequest(object node, int id, string value, bool isTmp) { Debounced++; }
    private string Translate(string value) { return Cache.TryGetValue(value, out var result) ? result : value; }
    private void LogVerbose(string value) { }
    private static bool ShouldSkipText(string value) { return false; }
    private static string StripTrailingStatLines(string value) { return value; }
    private static string GetVisibleText(string value) { return Regex.Replace(value, "<[^>]*>", "").Trim(); }
    private static bool EndsWithSentenceBoundary(string value) { return value.EndsWith(".") || value.EndsWith("!") || value.EndsWith("?"); }
'''
for name in ('LooksLikeTypewriterFragment', 'HasUnclosedOuterDialogueQuote', 'IsTranslatorOwnedTmpOverlay', 'TMPSetTextPrefix', 'UGUITextPrefix'):
    text += method(mono, name) + '\n'
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
out.write_text(text, encoding='utf-8', newline='\n')
print('Source-linked Unity renderer seams generated')
