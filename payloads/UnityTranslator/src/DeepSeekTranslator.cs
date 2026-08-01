using System;
using System.Collections;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Net;
using System.Net.Sockets;
using System.Reflection;
using System.Runtime.CompilerServices;
using System.Runtime.Serialization;
using System.Text;
using System.Text.RegularExpressions;
using System.Threading;
using System.Threading.Tasks;
using BepInEx;
using BepInEx.Configuration;
#if BEPINEX6
using BepInEx.Unity.Mono;
#endif
using HarmonyLib;
using Newtonsoft.Json;
using Newtonsoft.Json.Linq;
using UnityEngine;
using UnityEngine.SceneManagement;
using UnityEngine.UI;

/*
 * Unity Mono / BepInEx 5 ...6 共用的运行时插件... *
 * 真实入口...Awake()，完整主链路为：
 *   Awake -> 加载本地缓存/字体 -> 安装 Harmony hook -> 创建 TranslatorDriver
 *   -> TMP/UGUI setter、场景扫描或预热发现文本
 *   -> 本地缓存命中在主线程立即写回，未命中再进入异步应用管... *   -> 未命中经批量队列请求本地 C 服务
 *   -> 后台网络线程只产生字符串结果，Unity 对象写入统一回到 PumpOnce... *
 * 引擎边界：本文件只随 Unity Mono payload 部署。Ren'Py、RPG Maker ... * Unity IL2CPP 分别使用启动...hook、XUnity 端点...IL2CPP TMP fallback... * 不会执行这里的代码... *
 * 所有权约定... *   - _cache / _glossary / _localCacheKeys 由本插件拥有，访问时..._cache... *   - 请求、去抖、待应用和组件状态由 _pendingLock 保护... *   - _serverStateLock 只保护熔断状态；
 *   - _cachePersistLock 只保护持久化调度标志，不包住磁盘 IO... *   - UnityEngine.Object 只能...Unity 主线程读取或写入，后台线程不得持有强引用... */
[BepInPlugin("com.deepseek.translator", "Unity Translator", "3.1.126")]
public class DeepSeekTranslator : BaseUnityPlugin
{
	private const float CollapsedTmpOverlayWidth = 24f;

	private const float NarrowTmpOverlayWidthToHeight = 0.75f;

	private const float NarrowTmpOverlayParentWidthMultiplier = 2.5f;

	/* TMP 字体无法覆盖译文时创建的 UGUI 覆盖层状态；WeakReference 避免阻止场景卸载...*/
	private sealed class TmpOverlayState : MonoBehaviour
	{
		public Text overlayText;

		public float originalAlpha = -1f;

		public Color originalColor = Color.white;

		public bool hasOriginalColor;

		public bool sourceWasEnabled;

		public bool hasSourceEnabledState;

		public int pendingExternalWriteDepth;

		public string pendingExternalTextNormalized;

		public string sourceText;

		public string sourceNormalized;

		public string displayNormalized;

		public WeakReference componentRef;

		public bool registered;

		public int overlayGeometryRetryCount;
	}

	private sealed class TmpOverlayMarker : MonoBehaviour
	{
	}

	private sealed class UguiShortLabelLayoutState
	{
		public HorizontalWrapMode OriginalMode;

		public string AppliedText;
	}

	private sealed class TmpParagraphLayoutState
	{
		public object OriginalAlignment;

		public string AppliedText;
	}

	private sealed class TmpShortLabelLayoutState
	{
		public object OriginalAlignment;

		public Vector2 OriginalAnchoredPosition;

		public string AppliedText;
	}

	private static readonly ConditionalWeakTable<Text, UguiShortLabelLayoutState> _uguiShortLabelLayouts = new ConditionalWeakTable<Text, UguiShortLabelLayoutState>();

	private static readonly ConditionalWeakTable<Component, TmpParagraphLayoutState> _tmpParagraphLayouts = new ConditionalWeakTable<Component, TmpParagraphLayoutState>();

	private static readonly ConditionalWeakTable<Component, TmpShortLabelLayoutState> _tmpShortLabelLayouts = new ConditionalWeakTable<Component, TmpShortLabelLayoutState>();

	/* 后台翻译完成后等待主线程写回组件的不可变工作项...*/
	private sealed class PendingTranslationApply
	{
		public WeakReference ComponentRef;

		public int InstanceId;

		public string OriginalText;

		public string TranslatedText;

		public bool IsTmp;

		public bool PreserveRichText;
	}

	/* 发给服务端前的占位符保护结果；Tokens 保存富文...变量的还原映射...*/
	private sealed class ProtectedTextPayload
	{
		public string OriginalText;

		public string RequestText;

		public Dictionary<string, string> Tokens = new Dictionary<string, string>(StringComparer.Ordinal);
	}

	/* 相同 domain+原文共用一次远程请求，Callbacks 承载所有等待者...*/
	private sealed class PendingBatchRequest
	{
		public string Key;

		public string OriginalText;

		public string Domain;

		public ProtectedTextPayload Payload;

		public bool LowPriority;

		public List<Action<string>> Callbacks = new List<Action<string>>();
	}

	/* One long source fans out to bounded atomic requests. The aggregate remains
	   unresolved until every segment succeeds, so partial translations never
	   enter the full-text cache or reach a Unity renderer. */
	private sealed class PendingLongTextRequest
	{
		public string Key;

		public string OriginalText;

		public IReadOnlyList<UnityLongTextPart> Parts;

		public ProtectedTextPayload[] Payloads;

		public string[] Results;

		public int Remaining;

		public bool Failed;

		public List<Action<string>> Callbacks = new List<Action<string>>();
	}

	/* typewriter 文本先在主线程去抖，稳定后才允许进入远程队列...*/
	private sealed class DebouncedTextRequest
	{
		public WeakReference ComponentRef;

		public int InstanceId;

		public string Text;

		public bool IsTmp;

		public bool PreserveRichText;

		public bool LowPriority;

		public float UpdatedAt;
	}

	/* 场景预热只保存弱引用；异步返回时组件可能已经销毁或切场景...*/
	private sealed class WarmupCandidate
	{
		public WeakReference ComponentRef;

		public int InstanceId;

		public string OriginalText;

		public bool IsTmp;
	}

	/* 独立...DontDestroyOnLoad 的主线程泵。插件宿...Update 不可靠时...	   Harmony ...Time getter 后缀仍会调用同一...PumpOnce 作为补充入口...*/
	private sealed class TranslatorDriver : MonoBehaviour
	{
		public DeepSeekTranslator Owner;

		public void Initialize(DeepSeekTranslator owner)
		{
			Owner = owner;
		}

		public Coroutine StartRoutine(IEnumerator routine)
		{
			return ((MonoBehaviour)this).StartCoroutine(routine);
		}

		private void Update()
		{
			Owner?.DriverUpdate();
		}

		private void OnDestroy()
		{
			if ((Object)(object)Owner != (Object)null && Owner._driver == this)
			{
				Owner._driver = null;
			}
		}
	}

	public const string PluginGuid = "com.deepseek.translator";

	private const string PluginRuntimeVersion = "3.1.126";

	private static DeepSeekTranslator _instance;

	private static GameObject _pluginHostRoot;

	private static int _pluginHostLifetimeBlockCount;

	private static readonly object _caughtExceptionLogLock = new object();

	private static readonly Dictionary<string, int> _caughtExceptionCounts = new Dictionary<string, int>(StringComparer.Ordinal);

	/* Compatibility exception boundary policy:
	   1. It prevents one version-specific Unity/TMP reflection failure from disabling unrelated renderers.
	   2. It cannot be fixed in the shared server because the failing member exists only in the loaded game engine.
	   3. It preserves the existing local fallback but never hides the failure: counts and exception details are logged.
	   4. BepInEx receives the method, exception type/message, occurrence count, and first full stack trace. */
	private static void ReportCaughtException(Exception exception, string context = null, [System.Runtime.CompilerServices.CallerMemberName] string operation = null)
	{
		Exception root = (exception is TargetInvocationException && exception.InnerException != null) ? exception.InnerException : exception;
		string key = (operation ?? "unknown") + "|" + root.GetType().FullName;
		int count;
		lock (_caughtExceptionLogLock)
		{
			_caughtExceptionCounts.TryGetValue(key, out count);
			count++;
			_caughtExceptionCounts[key] = count;
		}
		bool shouldLog = count <= 3 || (count & (count - 1)) == 0;
		if (shouldLog)
		{
			string detail = count == 1 ? root.ToString() : root.GetType().FullName + ": " + root.Message;
			string message = "[EXCEPTION-BOUNDARY] operation=" + (operation ?? "unknown") + " occurrence=" + count + (string.IsNullOrWhiteSpace(context) ? string.Empty : " context=" + context) + " error=" + detail;
			if (_instance != null)
			{
				_instance.Logger.LogWarning(message);
			}
			else
			{
				System.Diagnostics.Trace.TraceWarning(message);
			}
		}
	}

	private TranslatorDriver _driver;

	private Harmony _harmony;

	private EventInfo _sceneLoadedEvent;

	private Delegate _sceneLoadedCallback;

	private EventInfo _canvasRenderEvent;

	private Delegate _canvasRenderCallback;

	private EventInfo _beforeRenderEvent;

	private Delegate _beforeRenderCallback;

	private ConfigEntry<string> _serverUrl;

	private ConfigEntry<string> _fontName;

	private ConfigEntry<string> _customFontPath;

	private ConfigEntry<string> _fontMode;

	private ConfigEntry<string> _performanceMode;

	private ConfigEntry<bool> _debugMode;

	private ConfigEntry<bool> _deepPrefetchEnabled;

	private ConfigEntry<bool> _serverCachePreloadEnabled;

	/* 网络、缓存和重试策略。常量必须直接用于真实调用点，禁止仅作“说明”...*/
	private const int HttpTimeoutMs = 30000;

	private const int ServerOfflineBaseBackoffMs = 2500;

	private const int ServerOfflineMaxBackoffMs = 30000;

	private const int ServerFailureLogIntervalMs = 6000;

	private const int TransientTranslationRetryCooldownSeconds = 1;

	private const int RejectedTranslationRetryCooldownSeconds = 8;

	private const int MaxRejectedTranslationRetries = 3;

	/* 共享翻译状态。_localCacheKeys 只记录本游戏实际使用过的键，防止...	   可选的全服缓存预载再次完整写入游戏目录...*/
	private readonly Dictionary<string, string> _glossary = new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase);

	private readonly Dictionary<string, string> _cache = new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase);

	private readonly HashSet<string> _localCacheKeys = new HashSet<string>(StringComparer.OrdinalIgnoreCase);

	private volatile bool _firstFrameLocalCachePrimed;

	private volatile bool _bootCacheLoadComplete;

	private readonly Dictionary<string, string> _mixedRepairOriginals = new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase);

	private readonly Dictionary<string, string> _mixedRepairTranslations = new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase);

	private readonly HashSet<string> _mixedRepairMisses = new HashSet<string>(StringComparer.OrdinalIgnoreCase);

	private readonly Dictionary<string, DateTime> _translationRetryCooldowns = new Dictionary<string, DateTime>(StringComparer.OrdinalIgnoreCase);

	private readonly Dictionary<string, int> _translationRejectCounts = new Dictionary<string, int>(StringComparer.OrdinalIgnoreCase);

	private readonly HashSet<string> _translationRetryAbandoned = new HashSet<string>(StringComparer.OrdinalIgnoreCase);

	private readonly object _translationRetryCooldownLock = new object();

	private readonly Dictionary<string, PendingBatchRequest> _pendingBatchRequests = new Dictionary<string, PendingBatchRequest>(StringComparer.Ordinal);

	private readonly Dictionary<string, PendingLongTextRequest> _pendingLongTextRequests = new Dictionary<string, PendingLongTextRequest>(StringComparer.Ordinal);

	private readonly List<string> _pendingBatchQueue = new List<string>();

	private readonly HashSet<string> _warmupRequestedSources = new HashSet<string>(StringComparer.Ordinal);

	private readonly HashSet<string> _fungusPrefetchSeen = new HashSet<string>(StringComparer.Ordinal);

	private readonly List<WeakReference> _activeTmpOverlays = new List<WeakReference>();

	private readonly HashSet<int> _deepScannedObjects = new HashSet<int>();

	private readonly HashSet<int> _cjkRenderDiagnostics = new HashSet<int>();

	private readonly HashSet<string> _deepPrefetchSeen = new HashSet<string>(StringComparer.Ordinal);

	private readonly Queue<string> _deepPrefetchQueue = new Queue<string>();

	private readonly HashSet<string> _imguiPending = new HashSet<string>(StringComparer.Ordinal);

	private readonly Dictionary<string, float> _imguiSettleStartedAt = new Dictionary<string, float>(StringComparer.Ordinal);

	private int _gameScriptWarmupStarted;

	/* 跨线程调度状态。后台任务只能在锁内增删工作项；真正读写 Unity 组件
	   ...Driver/Canvas/Frame hook 触发的主线程泵完成...*/
	private readonly object _pendingLock = new object();

	private readonly object _serverStateLock = new object();

	/* 诊断线程仅在 DebugMode 下创建。停止事件属于插件生命周期，OnDestroy 置位...	   线程立即退出，避免场景/插件卸载后仍由闭包强引用整个翻译器实例...*/
	private ManualResetEvent _diagnosticsStop;

	private Thread _diagnosticsThread;

	private readonly HashSet<int> _inProgress = new HashSet<int>();

	private readonly Dictionary<int, string> _inProgressSources = new Dictionary<int, string>();

	private readonly Dictionary<int, string> _translatedComponents = new Dictionary<int, string>();

	private readonly List<PendingTranslationApply> _pendingApplyQueue = new List<PendingTranslationApply>();

	private readonly HashSet<string> _pendingApplyKeys = new HashSet<string>(StringComparer.Ordinal);

	private readonly Dictionary<int, DebouncedTextRequest> _debouncedTextRequests = new Dictionary<int, DebouncedTextRequest>();

	private bool _tmpFontFromChineseSource;

	private bool _tmpFontCreationExhausted;

	private bool _tmpFontPackageSearchExhausted;

	private int _sceneWarmupGeneration;

	private bool _batchFlushScheduled;

	private float _lastSceneLoadRealtime;

	private readonly object _cachePersistLock = new object();

	private readonly object _cacheFileWriteLock = new object();

	private volatile bool _shuttingDown;

	private volatile bool _applicationQuitting;

	private bool _runtimeOwner;

	private bool _unityVersionResolved;

	private string _resolvedUnityVersion = string.Empty;

	private bool _cachePersistScheduled;

	private bool _cachePersistDirty;

	private volatile int _hookCallCount;

	private volatile int _tmpRegistryHookCount;

	private volatile int _tmpRegistryRegisteredCount;

	private volatile int _canvasRenderCount;

	private volatile int _driverTickCount;

	private volatile int _framePumpTickCount;

	private volatile int _beforeRenderTickCount;

	private int _beforeRenderPumpActive;

	private int _framePumpActive;

	private float _lastScanRealtime = -999f;

	private float _lastCanvasFlushRealtime = -999f;

	private float _lastDeepPrefetchRealtime = -999f;

	private float _nextSceneCacheApplyRealtime = -999f;

	private float _uiCacheApplyUntilRealtime = -999f;

	private float _nextUiCacheApplyRealtime = -999f;

	private float _nextPeriodicCacheApplyRealtime = -999f;

	private float _nextOverlayValidationRealtime = -999f;

	private float _lastUiActivationRealtime = -999f;

	private DateTime _serverBackoffUntilUtc = DateTime.MinValue;

	private DateTime _lastServerFailureLogUtc = DateTime.MinValue;

	private int _serverFailureCount;

	private int _deepPrefetchActive;

	private int _sceneCacheApplyGeneration;

	private int _sceneCacheApplyPassesRemaining;

	private readonly Dictionary<int, bool> _canvasGroupVisibleStates = new Dictionary<int, bool>();

	private bool _scannerInitialized;

	private Type _tmpTextTypeCache;

	private Type _textAssetTypeCache;

	private PropertyInfo _textPropCache;

	private PropertyInfo _fontPropCache;

	private MethodInfo _forceMeshMethodCache;

	private volatile int _scanCount;

	private volatile int _totalTmpFound;

	private volatile int _translateCacheHits;

	private volatile int _asyncScheduled;

	private volatile int _flushApplied;

	private volatile int _flushSkipped;

	private volatile int _uiActivationBurstCount;

	private volatile int _cacheApplyPassCount;

	private volatile int _cacheApplyHitCount;

	private volatile int _textEnableHookCount;

	private volatile int _targetedCacheQueueCount;

	private volatile int _uguiShortLabelOverflowCount;

	private volatile int _imguiShortLabelWidthCount;

	private volatile int _fontApplyFailures;

	private volatile int _fontApplyAttached;

	private bool _overlayDisabled = true;

	private bool _cjkFontWarningLogged;

	private bool _tmpFontFromPackage;

	private bool _tmpFontFromExisting;

	private bool _tmpFontFromRuntime;

	private string _tmpFontSource = "none";

	private string _tmpFontBundlePath;

	private object _tmpFontUsabilityCachedAsset;

	private bool _tmpFontUsabilityCachedResult;

	private volatile int _tmpOverlayRestoredCount;

	private volatile int _glyphRetryCount;

	private volatile int _glyphAtlasMissCount;

	private volatile int _alphaRescuedCount;

	private volatile int _directSwapCount;

	private volatile int _hostAtlasWarmedCount;

	private volatile int _softMaskRefreshCount;

	private readonly HashSet<int> _softMaskRefreshedOnce = new HashSet<int>();

	private const int UiClientBatchWindowMs = 0;

	private const int SystemClientBatchWindowMs = 1;

	private const int DefaultClientBatchWindowMs = 2;

	private const int MaxClientBatchSize = 16;

	/* 与本地服...API 通道池对齐：最...4 个批次并行，避免单次往返串行化...*/
	private const int MaxConcurrentBatchFlushes = 4;

	private const int LongTextParallelThreshold = 480;

	private const int LongTextTargetSegmentChars = 320;

	private const int MaxParallelLongTextSegments = 4;

	private const string LongTextLaneMarker = "|unity-long|";

	/* flush 异常退出后的重启必须限速，否则确定性错误会变成高频死循环...*/
	private const int BatchFlushFaultRestartDelayMs = 1000;

	private const int FreshLocalCacheMinEntries = 1200;

	/* 单个游戏真实使用的本地缓存通常只有几千条。超过此值视为旧版整库污染，
	   不能重新标成本地键，否则每次持久化都会校验并重写整库...*/
	private const int OversizedLocalCacheEntryLimit = 50000;

	private const long OversizedLocalCacheFileBytes = 8L * 1024L * 1024L;

	private const int FirstFrameLocalCacheEntryLimit = 2048;

	private const long FirstFrameLocalCacheFileBytes = 256L * 1024L;

	private const int MaxFontWarmupCacheEntries = 2048;

	private const float SceneWarmupDelaySeconds = 0.05f;

	private const float SceneWarmupSecondPassDelaySeconds = 0.35f;

	private const float SceneWarmupThirdPassDelaySeconds = 1f;

	private const float SceneCacheApplyDelaySeconds = 0f;

	private const float SceneCacheApplyIntervalSeconds = 0.08f;

	private const float DeferredServerCacheSyncDelaySeconds = 8f;

	private const float DeferredServerCacheSyncIdlePollSeconds = 0.35f;

	private const float FastScanIntervalSeconds = 0.5f;

	private const float SlowScanIntervalSeconds = 5f;

	private const float FastScanWindowSeconds = 2f;

	private const float UiActivationCacheApplyWindowSeconds = 1.2f;

	private const float UiActivationCacheApplyIntervalSeconds = 0.12f;

	private const float UiActivationThrottleSeconds = 0.12f;

	private const float PeriodicCacheApplyIntervalSeconds = 0.25f;

	private const float OverlayValidationIntervalSeconds = 0.5f;

	private const float CanvasFlushIntervalSeconds = 0.08f;

	private const int CachePersistDebounceMs = 6000;

	private const int FramePumpGateMask = 63;

	private const float TextSettleDebounceSeconds = 0.2f;

	private const float TypewriterFragmentDebounceSeconds = 0.6f;

	private const float UiTextSettleDebounceSeconds = 0.03f;

	private const string UguiTypewriterLengthPaddingTag = "<color=#00000000></color>";

	private const int MaxDebouncedStartsPerTick = 24;

	private const int MaxSceneWarmupCandidates = 96;

	private const int MaxPendingApplyPerFlush = 12;

	private const int MaxSceneCacheAppliesPerPass = 256;

	private volatile int _immediateSceneApplyCount;

	private volatile int _immediateSceneHits;

	private float _lastAlphaSweepRealtime = -999f;

	private const float AlphaSweepIntervalSeconds = 8f;

	private volatile int _alphaSweepRunCount;

	private volatile int _alphaSweepHealedCount;

	private volatile int _canvasGroupHiddenCount;

	private float _lastStatePruneRealtime = -999f;

	private const int FirstWritesToLog = 12;

	private static int _firstWritesLogged;

	private volatile int _fontMaterialFixed;

	private volatile int _fontMaterialAlreadyOk;

	private volatile int _fontMaterialFixFailed;

	private const int MaxUiActivationCacheAppliesPerPass = 128;

	private const int MaxPeriodicCacheAppliesPerPass = 32;

	private const int MaxTargetedCacheQueuesPerActivation = 48;

	private const int MaxTmpRegistryDiscoveryPerPass = 128;

	private const float TmpRegistryDiscoveryIntervalSeconds = 0.5f;

	private float _lastTmpRegistryDiscoveryRealtime = -999f;

	private const int SceneCacheApplyPasses = 6;

	private const int MaxRemoteLowPriorityPendingRequests = 4;

	private const int MaxOverlayValidationsPerPump = 4;

	private const int MaxTrackedComponentStates = 4096;

	private const int MaxPendingComponentWork = 512;

	private const int MaxImGuiPending = 512;

	private const int MaxMixedRepairMemoized = 512;

	private const int MaxDeepPrefetchSeen = 4096;

	private const int MaxDeepPrefetchQueue = 512;

	private const int FungusLookaheadMaxCommands = 24;

	private const int FungusPrefetchMaxRequestTexts = 32;

	private const int MaxFungusPrefetchSeen = 2048;

	private const float StatePruneIntervalSeconds = 30f;

	private const float DeepPrefetchInitialDelaySeconds = 1f;

	private const float DeepPrefetchScanIntervalSeconds = 2f;

	private const float DeepPrefetchChunkPauseSeconds = 0.25f;

	private const int DeepPrefetchMaxObjectsPerScan = 260;

	private const int DeepPrefetchMaxTextsPerScan = 80;

	private const int DeepPrefetchBatchSize = 8;

	private const int MaxGameScriptWarmupTexts = 512;

	private const int GameScriptWarmupBatchSize = 16;

	private const int GameScriptWarmupPauseMs = 150;

	private const int WarmupServerReadyWaitMs = 5000;

	private const int WarmupServerReadyPollMs = 200;

	private const double ServerCacheSkipRatio = 1.05;

	private static readonly TimeSpan FreshLocalCacheMaxAge = TimeSpan.FromHours(12.0);

	/* 标签名刻意保守：带值标签必须有 '='，裸标签必须完整匹配标签词，
	   从而不..."< The lobby... >" ..."<Mark looked at me.>" 当成富文本...*/
	private static readonly Regex RichTextTagRegex = new Regex("<\\s*/?\\s*(?:b|i|u|s)\\s*>|<\\s*#[0-9A-Fa-f]{3,8}\\s*>|<\\s*(?:color|size|material)\\s*=\\s*[^>]+>|<\\s*/\\s*(?:color|size|material)\\s*>|<\\s*quad\\b[^>]*>|<\\s*br\\s*/?\\s*>|<\\s*(?:mark|sprite|line-height|link|font-weight|font|align|alpha|cspace|indent|line-indent|margin|mspace|pos|rotate|space|style|voffset|width|gradient|page)\\s*(?:=|\\s+[A-Za-z-]+\\s*=)[^>]*>|<\\s*(?:nobr|noparse|allcaps|smallcaps|lowercase|uppercase|strikethrough)\\s*>|<\\s*/\\s*(?:mark|sprite|line-height|link|font-weight|font|align|alpha|cspace|indent|line-indent|margin|mspace|pos|rotate|space|style|voffset|width|gradient|page|nobr|noparse|allcaps|smallcaps|lowercase|uppercase|strikethrough)\\s*>", RegexOptions.IgnoreCase | RegexOptions.Compiled);

	private static readonly Regex EmptyAngleTagRegex = new Regex("<\\s*>", RegexOptions.Compiled);

	private static readonly Regex MalformedAngleFragmentRegex = new Regex("<\\s*(?:/|\\\\)?\\s*>\\s*[\\\\/]*", RegexOptions.Compiled);

	private static readonly Regex DanglingTagSlashRegex = new Regex("(?<=>)\\s*[\\\\/]+(?=\\s|$|[,.!?，。！？；：])", RegexOptions.Compiled);

	private static readonly Regex BareClosingRichTextRegex = new Regex("(?<!<)(/\\s*(?:color|size|b|i|u|material|quad)\\s*>)", RegexOptions.IgnoreCase | RegexOptions.Compiled);

	/* 这里不能修复 b/i/u 单字母标签：...B 结尾的十六进制颜色会误匹...	   "B>" 并被插入多余 '<'，破坏原本有效的 color 标签...*/
	private static readonly Regex BareOpeningRichTextRegex = new Regex("(?<![</])((?:color|size|material|quad)(?:\\s*=\\s*[^>\\s]+)?\\s*>)", RegexOptions.IgnoreCase | RegexOptions.Compiled);

	private static readonly Regex NestedBrokenRichTextRegex = new Regex("</\\s*<\\s*(color|size|b|i|u|material|quad)\\s*>", RegexOptions.IgnoreCase | RegexOptions.Compiled);

	private static readonly Regex BrokenOpeningRichTextDashRegex = new Regex("<\\s*(color|size|material|quad)\\s*-\\s*=", RegexOptions.IgnoreCase | RegexOptions.Compiled);

	private static readonly Regex LooseColorOpenFragmentRegex = new Regex("(?i)[<:/\\\\-]*\\s*c?\\s*olor\\s*[-=]+\\s*(?:#[0-9A-F]{1,8}|[A-Za-z]+)?\\s*>?", RegexOptions.Compiled);

	private static readonly Regex LooseColorCloseFragmentRegex = new Regex("(?i)[<:/\\\\-]*\\s*/\\s*c?olor\\s*>?", RegexOptions.Compiled);

	/* 百分比字号标签必须整体吞掉，否则纯文本路径会残留 "%>"...*/
	private static readonly Regex LooseSizeOpenFragmentRegex = new Regex("(?i)[<:/\\\\-]*\\s*size\\s*[-=]+\\s*\\d+\\s*%?\\s*>?", RegexOptions.Compiled);

	private static readonly Regex LooseSizeCloseFragmentRegex = new Regex("(?i)[<:/\\\\-]*\\s*/\\s*size\\s*>?", RegexOptions.Compiled);

	private static readonly Regex PlaceholderTokenRegex = new Regex("__DS_TOKEN_\\d+__", RegexOptions.Compiled);

	private static readonly Regex NumericTokenRegex = new Regex("\\b\\d+(?:\\s*/\\s*\\d+)?\\b", RegexOptions.Compiled);

	private static readonly Regex LatinWordRegex = new Regex("[A-Za-z]{2,}", RegexOptions.Compiled);

	private static readonly Regex RuntimeVersionTextRegex = new Regex("^v\\s*\\d+(?:\\.\\d+){1,4}(?:[-+._][A-Za-z0-9]+)*$", RegexOptions.IgnoreCase | RegexOptions.Compiled);

	private static readonly Regex RuntimeStatusPrefixRegex = new Regex("^(?:RAM|VRAM|VR|Window|Screen|Display|Resolution|FPS|CPU|GPU)\\s*:", RegexOptions.IgnoreCase | RegexOptions.Compiled);

	private static readonly Regex RuntimeResolutionTextRegex = new Regex("\\b(?:\\d{3,5}|#+)x(?:\\d{3,5}|#+)@(?:\\d{1,4}|#+)\\s*Hz(?:\\[[^\\]]+\\])?\\b", RegexOptions.IgnoreCase | RegexOptions.Compiled);

	private static readonly Regex InventoryQuantityLineRegex = new Regex("^\\s*(?<name>[^\\r\\n<>$][^\\r\\n]*?\\S)\\s+x\\s*(?<count>\\d+)\\s*$", RegexOptions.IgnoreCase | RegexOptions.Compiled);

	private static readonly Regex InventoryMoneyLineRegex = new Regex("^\\s*[$￥¥]\\s*\\d[\\d,]*(?:\\.\\d+)?\\s*$", RegexOptions.Compiled);

	private static readonly Regex ColoredRichTextSpanRegex = new Regex("<\\s*color\\s*=\\s*[^>]+>(?<text>.*?)<\\s*/\\s*color\\s*>", RegexOptions.IgnoreCase | RegexOptions.Singleline | RegexOptions.Compiled);

	/* ●、□、...常用于属性点和标签填充，但多数打...CJK 图集缺字；不替换会迫...	   整个组件降级到无法渲...<mark> ...UGUI 覆盖层...*/
	private static readonly char[] TmpPunctuationFallbackChars = new char[20]
	{
		'\u3001', '\u3002', '\uff0c', '\uff1f', '\uff01', '\uff1a', '\uff1b', '\uff08', '\uff09', '\u3010',
		'\u3011', '\u201c', '\u201d', '\u2018', '\u2019', '\u2026', '\u2014', '\u25cf', '\u25a1', '\u00b7'
	};

	private static readonly Dictionary<int, bool> HistoryComponentCache = new Dictionary<int, bool>();

	private static Type _historyTmpTextType;

	private static readonly HashSet<string> AllowedLatinResidue = new HashSet<string>(StringComparer.OrdinalIgnoreCase)
	{
		"AI", "API", "CPU", "DLC", "FPS", "GPU", "HP", "ID", "MP", "NPC",
		"OK", "RAM", "UI", "VR", "VRAM", "AMD", "Intel", "NVIDIA", "GeForce", "Ryzen",
		"Windows", "Direct", "DirectX", "Unity", "Steam", "DeepSeek", "BepInEx", "TMP"
	};

	private static readonly HashSet<string> CommonCapitalizedEnglishWords = new HashSet<string>(StringComparer.OrdinalIgnoreCase)
	{
		"A", "An", "And", "Are", "As", "At", "Be", "But", "By", "Can",
		"Do", "Does", "For", "From", "Good", "Here", "How", "If", "In", "Into",
		"Is", "It", "Let", "Like", "Maybe", "No", "Not", "Now", "Of", "On",
		"Or", "Our", "Some", "That", "The", "Then", "There", "This", "To", "Very",
		"We", "Well", "What", "When", "Where", "Which", "Who", "Why", "With", "You",
		"Your"
	};

	private static readonly HashSet<char> MojibakeSignalChars = new HashSet<char>(new char[16]
	{
		'锛', '銆', '绂', '缁', '诲', '紑', '璇', '娓', '垙', '寮',
		'€', '鈥', '妯', '傚', '姞', '\ufffd'
	});

	private static readonly HashSet<string> UnityTextSupportedTags = new HashSet<string>(StringComparer.OrdinalIgnoreCase) { "b", "i", "size", "color", "material", "quad" };

	private static readonly string[] StartupHotTexts = new string[37]
	{
		"Continue", "CONTINUE", "Leave", "LEAVE", "Load Game", "LOAD GAME", "New Game", "NEW GAME", "Screen", "SCREEN",
		"Main Menu", "MAIN MENU", "Info", "INFO", "Sound", "SOUND", "Cheats", "CHEATS", "Debug", "DEBUG",
		"Exit", "EXIT", "Exit Game", "EXIT GAME", "Quest Log", "QUEST LOG", "Back", "OK", "Cancel", "Yes",
		"No", "Inventory", "INVENTORY", "Use", "USE", "Equip", "EQUIP"
	};

	private static volatile int _asyncQueueLoggedCount;

	private static readonly HashSet<string> _asyncQueueLogged = new HashSet<string>();

	private static int _tmpExternalWriteBoundaryLogged;

	private static int _tmpOverlayColorOwnershipLogged;

	private static int _tmpCollapsedOverlayRectLogged;

	private static int _tmpOverlayReparentLogged;

	private static volatile int _setTextAnyOverloadCount;

	private Font _chineseFont;

	private MethodInfo _createDynamicFontFromOsFontMethod;

	private MethodInfo _internalCreateDynamicFontMethod;

	private bool _createDynamicFontFromOsFontResolved;

	private object _chineseTMPFont;

	private static readonly HashSet<int> _atlasMissLogged = new HashSet<int>();

	private static readonly object _atlasMissLogLock = new object();

	private static readonly Dictionary<string, MethodInfo> _quietMethodCache = new Dictionary<string, MethodInfo>();

	private static readonly object _quietMethodCacheLock = new object();

	private static readonly Dictionary<string, MethodInfo> _genericChildrenMethodCache = new Dictionary<string, MethodInfo>();

	private static readonly object _genericChildrenMethodCacheLock = new object();

	private static int _genericChildrenFallbackLogged;

	private static MethodInfo _findObjectsOfTypeWithInactiveMethod;

	private static MethodInfo _findObjectsOfTypeLegacyMethod;

	private static MethodInfo _findObjectsOfTypeGenericMethod;

	private static int _findObjectsMethodsResolved;

	private static int _preferObjectFindObjectsFallback;

	private static int _objectFindObjectsFallbackLogged;

	private static int _tmpUpdateManagerFallbackLogged;

	private static int _tmpUpdateManagerProbeLogged;

	private static int _tmpUpdateManagerPopulatedLogged;

	private static int _canvasRendererAlphaFallbackLogged;

	private static int _guiStyleVisualFallbackLogged;

	private static int _materialTextureFallbackLogged;

	private static int _texturePixelFallbackLogged;

	private Type _tmpSubMeshTypeCache;

	private Type _tmpSubMeshUITypeCache;

	private static readonly object _quietFieldCacheLock = new object();

	private static readonly Dictionary<string, FieldInfo> _quietFieldCache = new Dictionary<string, FieldInfo>();

	private static readonly object _quietPropertyCacheLock = new object();

	private static readonly Dictionary<string, PropertyInfo> _quietPropertyCache = new Dictionary<string, PropertyInfo>();

	private static int _deepInspectCount;

	private const int DeepInspectMax = 3;

	private const int CjkRenderDiagnosticMax = 16;

	/* BepInEx 真实入口。初始化顺序很重要：先准备配...缓存/字体，再安装 hook...	   最后启动预热和扫描。任何后台工作都不得在这里同步等待远...API...*/
	private void Awake()
	{
		if (!EnsureDedicatedPluginHost())
		{
			return;
		}
		_runtimeOwner = true;
		_shuttingDown = false;
		_instance = this;
		PinPluginHostAcrossSceneLoads();
		_lastSceneLoadRealtime = Time.realtimeSinceStartup;
		_serverUrl = base.Config.Bind("General", "ServerUrl", "http://127.0.0.1:19999", "Translation server URL");
		_fontName = base.Config.Bind("General", "FontName", "Microsoft YaHei UI", "Fallback Chinese font");
		_customFontPath = base.Config.Bind("General", "CustomFontPath", "", "Preferred font file path");
		_fontMode = base.Config.Bind("General", "FontMode", "auto", "Font handling mode: auto, inject, replace, overlay, none");
		_performanceMode = base.Config.Bind("General", "PerformanceMode", "normal", "Runtime performance mode: high, normal, eco");
		_debugMode = base.Config.Bind("General", "DebugMode", defaultValue: false, "Enable debug logging");
		_deepPrefetchEnabled = base.Config.Bind("General", "DeepPrefetchRuntime", defaultValue: false, "Runtime deep prefetch. Disabled by default because it competes with live dialogue API calls.");
		_serverCachePreloadEnabled = base.Config.Bind("General", "ServerCachePreload", defaultValue: false, "Full server cache preload. Disabled by default to avoid importing hundreds of thousands of translations into the game process.");
		ConfigureHttpFastPath();
		LoadGlossary();
		PrimeSmallLocalCacheForFirstFrame();
		_ = BootCacheLoadAsync();
		if (IsFontModeNone())
		{
			base.Logger.LogWarning("[FONT] FontMode=none; font discovery and TMP translation are disabled.");
		}
		else
		{
			FindChineseFont();
		}
		EnsureDriver();
		_harmony = new Harmony("com.deepseek.translator");
		InstallHooks();
		RegisterSceneLoadedCallback();
		base.Logger.LogInfo("=== ds游戏翻译器 v" + (typeof(DeepSeekTranslator).GetCustomAttribute<BepInPlugin>()?.Version?.ToString() ?? "unknown") + " Ready ===");
		RegisterCanvasRenderCallback();
		RegisterBeforeRenderPump();
		LogCoroutineDelayCompatibility();
		if (!IsFontModeNone())
		{
			StartManagedCoroutine(EnsureTmpFontReadyCoroutine());
		}
		StartStartupWarmup();
		StartManagedCoroutine(ScanTextComponentsCoroutine());
		if (_debugMode.Value)
		{
			/* 周期诊断会分配字符串并写盘，只能显式启用；正常运行不创建常驻线程...			   诊断是可选旁路，不能...Awake 早退控制，否则未来追加的真实初始化会被跳过...*/
		string pluginDir = Path.GetDirectoryName(base.Info.Location);
		DeepSeekTranslator translatorRef = this;
		string diagnosticFontMode = GetFontMode();
		string diagnosticPerformanceMode = GetPerformanceMode();
		string diagnosticServerUrl = _serverUrl?.Value;
		bool diagnosticHasUsableTmpFont = HasUsableTmpFont();
		bool diagnosticHasChineseTmpFont = !ReferenceEquals(_chineseTMPFont, null);
		_diagnosticsStop = new ManualResetEvent(initialState: false);
		Thread thread = new Thread((ThreadStart)delegate
		{
			if (_diagnosticsStop.WaitOne(5000))
			{
				return;
			}
			try
			{
				string path = Path.Combine(pluginDir, "translator_runtime_diag.txt");
				string[] value = new string[]
				{
					$"DiagTime={DateTime.Now}",
					"Version=" + PluginRuntimeVersion,
					"FontMode=" + diagnosticFontMode,
					"PerformanceMode=" + diagnosticPerformanceMode,
					$"HookCalls={translatorRef._hookCallCount}",
					$"TmpRegistryHookCalls={translatorRef._tmpRegistryHookCount}",
					$"TmpRegistryRegistered={translatorRef._tmpRegistryRegisteredCount}",
					$"CanvasRenderCalls={translatorRef._canvasRenderCount}",
					$"DriverTicks={translatorRef._driverTickCount}",
					$"FramePumpTicks={translatorRef._framePumpTickCount}",
					$"BeforeRenderTicks={translatorRef._beforeRenderTickCount}",
					$"CacheSize={translatorRef.GetCacheCount()}",
					$"GlossarySize={translatorRef.GetGlossaryCount()}",
					$"ProcessedCount={translatorRef.GetTranslatedComponentCount()}",
					$"UiActivationBursts={translatorRef._uiActivationBurstCount}",
					$"CacheApplyPasses={translatorRef._cacheApplyPassCount}",
					$"CacheApplyHits={translatorRef._cacheApplyHitCount}",
					$"TextEnableHooks={translatorRef._textEnableHookCount}",
					$"TargetedCacheQueues={translatorRef._targetedCacheQueueCount}",
					$"InProgressCount={translatorRef.GetInProgressCount()}",
					"ServerUrl=" + diagnosticServerUrl
				};
				File.WriteAllText(path, string.Join("\n", value));
			}
			catch (Exception reportedException)
			{
				ReportCaughtException(reportedException);
			}
			if (_diagnosticsStop.WaitOne(10000))
			{
				return;
			}
			try
			{
				string path2 = Path.Combine(pluginDir, "translator_scan_diag.txt");
				int count;
				lock (translatorRef._pendingLock)
				{
					count = translatorRef._pendingApplyQueue.Count;
				}
				string[] value2 = new string[]
				{
					$"ScanDiagTime={DateTime.Now}",
					"FontMode=" + diagnosticFontMode,
					"PerformanceMode=" + diagnosticPerformanceMode,
					$"ScanCount={translatorRef._scanCount}",
					$"CanvasRenderCalls={translatorRef._canvasRenderCount}",
					$"DriverTicks={translatorRef._driverTickCount}",
					$"FramePumpTicks={translatorRef._framePumpTickCount}",
					$"BeforeRenderTicks={translatorRef._beforeRenderTickCount}",
					$"TotalTMPFound={translatorRef._totalTmpFound}",
					$"TranslateCacheHits={translatorRef._translateCacheHits}",
					$"AsyncScheduled={translatorRef._asyncScheduled}",
					$"FlushApplied={translatorRef._flushApplied}",
					$"FlushSkipped={translatorRef._flushSkipped}",
					$"PendingQueueSize={count}",
					$"ProcessedCount={translatorRef.GetTranslatedComponentCount()}",
					$"InProgressCount={translatorRef.GetInProgressCount()}",
					$"CacheSize={translatorRef.GetCacheCount()}",
					$"UiActivationBursts={translatorRef._uiActivationBurstCount}",
					$"CacheApplyPasses={translatorRef._cacheApplyPassCount}",
					$"CacheApplyHits={translatorRef._cacheApplyHitCount}",
					$"TextEnableHooks={translatorRef._textEnableHookCount}",
					$"TargetedCacheQueues={translatorRef._targetedCacheQueueCount}"
				};
				File.WriteAllText(path2, string.Join("\n", value2));
			}
			catch (Exception reportedException)
			{
				ReportCaughtException(reportedException);
			}
			while (!_diagnosticsStop.WaitOne(5000))
			{
				try
				{
					string path3 = Path.Combine(pluginDir, "translator_live_diag.txt");
					int count2;
					int count3;
					lock (translatorRef._pendingLock)
					{
						count2 = translatorRef._pendingApplyQueue.Count;
						count3 = translatorRef._activeTmpOverlays.Count;
					}
					string[] value3 = new string[]
					{
						$"LiveDiagTime={DateTime.Now}",
						"Version=" + PluginRuntimeVersion,
						"FontMode=" + diagnosticFontMode,
						"PerformanceMode=" + diagnosticPerformanceMode,
						$"OverlayDisabled={translatorRef._overlayDisabled}",
						$"HasUsableTmpFont={diagnosticHasUsableTmpFont}",
						$"ChineseTMPFont={diagnosticHasChineseTmpFont}",
						$"TmpFontFromPackage={translatorRef._tmpFontFromPackage}",
						$"TmpFontFromExisting={translatorRef._tmpFontFromExisting}",
						"TmpFontSource=" + translatorRef._tmpFontSource,
						"TmpFontBundlePath=" + translatorRef._tmpFontBundlePath,
						$"TmpFontFromChinese={translatorRef._tmpFontFromChineseSource}",
						$"TmpFontExhausted={translatorRef._tmpFontCreationExhausted}",
						$"TmpFontPackageSearchExhausted={translatorRef._tmpFontPackageSearchExhausted}",
						$"ActiveOverlayCount={count3}",
						$"OverlayRestored={translatorRef._tmpOverlayRestoredCount}",
						$"TotalTMPFound={translatorRef._totalTmpFound}",
						$"CacheSize={translatorRef.GetCacheCount()}",
						$"PendingQueueSize={count2}",
						$"ProcessedCount={translatorRef.GetTranslatedComponentCount()}",
						$"InProgressCount={translatorRef.GetInProgressCount()}",
						$"CacheApplyHits={translatorRef._cacheApplyHitCount}",
						$"TextEnableHooks={translatorRef._textEnableHookCount}",
						$"ScanCount={translatorRef._scanCount}",
						$"HookCalls={translatorRef._hookCallCount}",
						$"TmpRegistryHookCalls={translatorRef._tmpRegistryHookCount}",
						$"TmpRegistryRegistered={translatorRef._tmpRegistryRegisteredCount}",
						$"FontApplyAttached={translatorRef._fontApplyAttached}",
						$"FontApplyFailures={translatorRef._fontApplyFailures}",
						$"GlyphRetryCount={translatorRef._glyphRetryCount}",
						$"GlyphAtlasMissCount={translatorRef._glyphAtlasMissCount}",
						$"AlphaRescuedCount={translatorRef._alphaRescuedCount}",
						$"DirectSwapCount={translatorRef._directSwapCount}",
						$"HostAtlasWarmedCount={translatorRef._hostAtlasWarmedCount}",
						$"SoftMaskRefreshCount={translatorRef._softMaskRefreshCount}",
						$"UguiShortLabelOverflow={translatorRef._uguiShortLabelOverflowCount}",
						$"ImGuiShortLabelWidth={translatorRef._imguiShortLabelWidthCount}",
						$"ImmediateSceneApplyCount={translatorRef._immediateSceneApplyCount}",
						$"ImmediateSceneHits={translatorRef._immediateSceneHits}",
						$"AlphaSweepRunCount={translatorRef._alphaSweepRunCount}",
						$"AlphaSweepHealedCount={translatorRef._alphaSweepHealedCount}",
						$"CanvasGroupHiddenCount={translatorRef._canvasGroupHiddenCount}",
						$"FirstWritesLogged={Volatile.Read(ref _firstWritesLogged)}",
						$"FontMaterialFixed={translatorRef._fontMaterialFixed}",
						$"FontMaterialAlreadyOk={translatorRef._fontMaterialAlreadyOk}",
						$"FontMaterialFixFailed={translatorRef._fontMaterialFixFailed}",
						$"SetTextAnyOverloadCount={_setTextAnyOverloadCount}",
						$"AsyncQueueLogged={_asyncQueueLoggedCount}"
					};
					File.WriteAllText(path3, string.Join("\n", value3));
				}
				catch (Exception reportedException)
				{
					ReportCaughtException(reportedException);
				}
			}
		});
		thread.IsBackground = true;
		thread.Name = "DST_Diag";
		_diagnosticsThread = thread;
		thread.Start();
		try
		{
			File.WriteAllText(Path.Combine(pluginDir, "translator_diag.txt"), $"AwakeTime={DateTime.Now}\nVersion={PluginRuntimeVersion}\nEnabled={((Behaviour)this).enabled}\nGameObjectActive={((Component)this).gameObject.activeSelf}\nHideFlags={((Object)((Component)this).gameObject).hideFlags}\nCanvasCallback=registered\n");
		}
		catch (Exception reportedException)
		{
			ReportCaughtException(reportedException);
		}
		}
	}

	private bool EnsureDedicatedPluginHost()
	{
		GameObject currentRoot = ((Component)this).gameObject;
		GameObject existingRoot = _pluginHostRoot;
		if ((Object)(object)existingRoot != (Object)null)
		{
			bool ownsRuntime = (Object)(object)currentRoot == (Object)(object)existingRoot;
			if (!ownsRuntime)
			{
				/* A later BepInEx manager instance can be created while the dedicated
				   runtime root is still alive. It has no configuration/runtime state and
				   cannot be repaired by the owner instance. Disabling only this duplicate
				   prevents an empty Update from running every frame; the owner remains
				   active, and the Update owner guard records the same invariant in code. */
				((Behaviour)this).enabled = false;
				base.Logger.LogWarning("Disabled duplicate Unity Mono translator instance; dedicated runtime owner is already active.");
			}
			return ownsRuntime;
		}
		GameObject hostRoot = null;
		try
		{
			hostRoot = new GameObject("DeepSeekTranslatorRuntime");
			((Object)hostRoot).hideFlags = (HideFlags)61;
			_pluginHostRoot = hostRoot;
			Object.DontDestroyOnLoad((Object)(object)hostRoot);
			DeepSeekTranslator runtime = hostRoot.AddComponent<DeepSeekTranslator>();
			if ((Object)(object)runtime == (Object)null)
			{
				throw new InvalidOperationException("Unity did not create the dedicated translator runtime component.");
			}
			base.Logger.LogInfo("Dedicated translator runtime host started outside the BepInEx manager root");
			/* 专用宿主已接管运行时；本实例保持未初始化（_serverUrl 等均为 null），必须禁用，
			   否则其 Update 会每帧空跑 PumpOnce 并在批量请求处 NRE。 */
			((Behaviour)this).enabled = false;
			return false;
		}
		catch (Exception ex)
		{
			_pluginHostRoot = null;
			if ((Object)(object)hostRoot != (Object)null)
			{
				Object.Destroy((Object)(object)hostRoot);
			}
			ReportCaughtException(ex, "boundary=dedicated-plugin-host-bootstrap");
			return true;
		}
	}

	private void PinPluginHostAcrossSceneLoads()
	{
		/* A scene transition may destroy the BepInEx manager root in some Unity games.
		   The game owns that lifecycle, so it cannot be repaired in the shared server.
		   Pinning only extends the plugin host lifetime; normal quit still reaches OnDestroy.
		   The startup log records that this compatibility boundary was activated. */
		try
		{
			GameObject hostRoot = ((Component)this).gameObject;
			_pluginHostRoot = hostRoot;
			Object.DontDestroyOnLoad((Object)(object)hostRoot);
			base.Logger.LogInfo("Pinned BepInEx plugin host across scene loads");
		}
		catch (Exception ex)
		{
			ReportCaughtException(ex, "boundary=plugin-host-lifetime");
		}
	}

	private void EnsureDriver()
	{
		/* Driver 与插件宿主解耦并跨场景存活，集中执行所...Unity 对象操作...		   创建失败时协程可退回插件自身，Harmony 帧泵仍提供第二条活跃路径...*/
		if ((Object)(object)_driver != (Object)null)
		{
			return;
		}
		try
		{
			GameObject val = new GameObject("DeepSeekTranslatorDriver");
			((Object)val).hideFlags = (HideFlags)61;
			Object.DontDestroyOnLoad((Object)(object)val);
			_driver = val.AddComponent<TranslatorDriver>();
			_driver.Initialize(this);
			base.Logger.LogInfo("Dedicated translator driver started");
		}
		catch (Exception ex)
		{
			base.Logger.LogWarning("Dedicated translator driver failed to start: " + ex.Message);
		}
	}

	private Coroutine StartManagedCoroutine(IEnumerator routine)
	{
		if (routine == null)
		{
			return null;
		}
		EnsureDriver();
		if (!((Object)(object)_driver != (Object)null))
		{
			return ((MonoBehaviour)this).StartCoroutine(routine);
		}
		return _driver.StartRoutine(routine);
	}

	private void RegisterSceneLoadedCallback()
	{
		RegisterOptionalStaticEvent(typeof(SceneManager), "sceneLoaded", "OnSceneLoaded", ref _sceneLoadedEvent, ref _sceneLoadedCallback);
	}

	private void RegisterCanvasRenderCallback()
	{
		RegisterOptionalStaticEvent(typeof(Canvas), "willRenderCanvases", "OnWillRenderCanvases", ref _canvasRenderEvent, ref _canvasRenderCallback);
	}

	private void RegisterBeforeRenderPump()
	{
		RegisterOptionalStaticEvent(typeof(Application), "onBeforeRender", "OnBeforeRenderPump", ref _beforeRenderEvent, ref _beforeRenderCallback);
	}

	private void UnregisterSceneLoadedCallback()
	{
		UnregisterOptionalStaticEvent("SceneManager.sceneLoaded", ref _sceneLoadedEvent, ref _sceneLoadedCallback);
	}

	private void UnregisterCanvasRenderCallback()
	{
		UnregisterOptionalStaticEvent("Canvas.willRenderCanvases", ref _canvasRenderEvent, ref _canvasRenderCallback);
	}

	private void UnregisterBeforeRenderPump()
	{
		UnregisterOptionalStaticEvent("Application.onBeforeRender", ref _beforeRenderEvent, ref _beforeRenderCallback);
	}

	/* Highly stripped Mono players may remove optional Unity event accessors even
	   though the declaring type remains. The game build owns that incompatibility,
	   so each event is bound reflectively and its absence is logged. Skipping one
	   lifecycle callback can reduce scan responsiveness, but it never accepts or
	   persists a translation and the driver/frame hooks remain active. */
	private void RegisterOptionalStaticEvent(Type ownerType, string eventName, string handlerName, ref EventInfo registeredEvent, ref Delegate registeredCallback)
	{
		if (registeredCallback != null)
		{
			return;
		}
		try
		{
			EventInfo eventInfo = ownerType.GetEvent(eventName, BindingFlags.Public | BindingFlags.NonPublic | BindingFlags.Static);
			MethodInfo handler = GetType().GetMethod(handlerName, BindingFlags.Instance | BindingFlags.NonPublic);
			if (eventInfo == null || eventInfo.EventHandlerType == null || handler == null)
			{
				base.Logger.LogWarning("Optional Unity event unavailable: " + ownerType.FullName + "." + eventName + "; continuing with remaining translation pumps.");
				return;
			}
			Delegate callback = Delegate.CreateDelegate(eventInfo.EventHandlerType, this, handler, throwOnBindFailure: false);
			if (callback == null)
			{
				base.Logger.LogWarning("Optional Unity event signature mismatch: " + ownerType.FullName + "." + eventName + "; continuing with remaining translation pumps.");
				return;
			}
			eventInfo.AddEventHandler(null, callback);
			registeredEvent = eventInfo;
			registeredCallback = callback;
			base.Logger.LogInfo("Registered " + ownerType.FullName + "." + eventName + " callback");
		}
		catch (Exception ex)
		{
			ReportCaughtException(ex, "renderer=UnityMono event=" + ownerType.FullName + "." + eventName, "RegisterOptionalStaticEvent");
		}
	}

	private void UnregisterOptionalStaticEvent(string eventName, ref EventInfo registeredEvent, ref Delegate registeredCallback)
	{
		EventInfo eventInfo = registeredEvent;
		Delegate callback = registeredCallback;
		registeredEvent = null;
		registeredCallback = null;
		if (eventInfo == null || callback == null)
		{
			return;
		}
		try
		{
			eventInfo.RemoveEventHandler(null, callback);
		}
		catch (Exception ex)
		{
			ReportCaughtException(ex, "renderer=UnityMono event=" + eventName, "UnregisterOptionalStaticEvent");
		}
	}

	private string GetFontMode()
	{
		return (_fontMode?.Value ?? "auto").Trim().ToLowerInvariant();
	}

	private bool IsFontModeNone()
	{
		return GetFontMode() == "none";
	}

	private bool IsTmpOverlayMode()
	{
		return GetFontMode() == "overlay";
	}

	private bool IsAutoFontMode()
	{
		return GetFontMode() == "auto";
	}

	private string GetPerformanceMode()
	{
		return (_performanceMode?.Value ?? "normal").Trim().ToLowerInvariant();
	}

	private bool IsHighPerformance()
	{
		return GetPerformanceMode() == "high";
	}

	private bool IsEcoPerformance()
	{
		return GetPerformanceMode() == "eco";
	}

	private int GetFramePumpGateMask()
	{
		if (IsHighPerformance())
		{
			return 31;
		}
		if (IsEcoPerformance())
		{
			return 127;
		}
		return FramePumpGateMask;
	}

	private float GetFastScanIntervalSeconds()
	{
		if (IsHighPerformance())
		{
			return 0.25f;
		}
		if (IsEcoPerformance())
		{
			return 1f;
		}
		return FastScanIntervalSeconds;
	}

	private float GetSlowScanIntervalSeconds()
	{
		if (IsHighPerformance())
		{
			return 2.5f;
		}
		if (IsEcoPerformance())
		{
			return 8f;
		}
		return SlowScanIntervalSeconds;
	}

	private float GetFastScanWindowSeconds()
	{
		if (IsHighPerformance())
		{
			return 3f;
		}
		if (IsEcoPerformance())
		{
			return 1f;
		}
		return FastScanWindowSeconds;
	}

	private int GetMaxPendingApplyPerFlush()
	{
		if (IsHighPerformance())
		{
			return 24;
		}
		if (IsEcoPerformance())
		{
			return 4;
		}
		return MaxPendingApplyPerFlush;
	}

	private int GetMaxDebouncedStartsPerTick()
	{
		if (IsHighPerformance())
		{
			return 48;
		}
		if (IsEcoPerformance())
		{
			return 8;
		}
		return MaxDebouncedStartsPerTick;
	}

	private void DriverUpdate()
	{
		try
		{
			Interlocked.Increment(ref _driverTickCount);
			PumpOnce("DRIVER");
		}
		catch (Exception ex)
		{
			try
			{
				File.WriteAllText(Path.Combine(Path.GetDirectoryName(base.Info.Location), "translator_driver_error.txt"), $"Driver update error at {DateTime.Now}: {ex}\nInner: {ex.InnerException}");
			}
			catch (Exception reportedException)
			{
				ReportCaughtException(reportedException);
			}
		}
	}

	private static void FramePumpPostfix()
	{
		_instance?.FramePumpFromHook();
	}

	private void FramePumpFromHook()
	{
		if (Interlocked.Exchange(ref _framePumpActive, 1) == 1)
		{
			return;
		}
		try
		{
			if ((Interlocked.Increment(ref _framePumpTickCount) & GetFramePumpGateMask()) == 0)
			{
				PumpOnce("FRAME");
			}
		}
		catch (Exception ex)
		{
			ReportCaughtException(ex, "source=FRAME");
		}
		finally
		{
			Interlocked.Exchange(ref _framePumpActive, 0);
		}
	}

	private void OnBeforeRenderPump()
	{
		if (Interlocked.Exchange(ref _beforeRenderPumpActive, 1) == 1)
		{
			return;
		}
		try
		{
			Interlocked.Increment(ref _beforeRenderTickCount);
			PumpOnce("BEFORE_RENDER");
		}
		catch (Exception ex)
		{
			ReportCaughtException(ex, "source=BEFORE_RENDER");
		}
		finally
		{
			Interlocked.Exchange(ref _beforeRenderPumpActive, 0);
		}
	}

	private void PumpOnce(string source)
	{
		/* 主线程唯一汇合点：修复覆盖...透明度、消费缓存命中、处理去抖与
		   待应用队列，再按性能模式扫描。source 只用于诊断，不改变语义...*/
		try
		{
			RunOverlayValidationTick();
			RunAlphaSweepTick();
			RunSceneCacheApplyTick();
			RunUiCacheApplyTick();
			RunPeriodicCacheApplyTick();
			FlushDebouncedTextRequests();
			FlushPendingTranslations();
			RunDeepPrefetchTick();
			float num = ((Mathf.Max(0f, Time.realtimeSinceStartup - _lastSceneLoadRealtime) <= GetFastScanWindowSeconds()) ? GetFastScanIntervalSeconds() : GetSlowScanIntervalSeconds());
			RunScannerTick(Math.Max(10, (int)(num * 1000f)), source);
		}
		catch (Exception reportedException)
		{
			ReportCaughtException(reportedException);
		}
	}

	private void OnWillRenderCanvases()
	{
		try
		{
			Interlocked.Increment(ref _canvasRenderCount);
			if (_debugMode.Value && _canvasRenderCount <= 3)
			{
				try
				{
					File.AppendAllText(Path.Combine(Path.GetDirectoryName(base.Info.Location), "translator_canvas_fired.txt"), $"Canvas callback #{_canvasRenderCount} at {DateTime.Now}\n");
				}
				catch (Exception reportedException)
				{
					ReportCaughtException(reportedException);
				}
			}
			float realtimeSinceStartup = Time.realtimeSinceStartup;
			if (realtimeSinceStartup - _lastCanvasFlushRealtime >= CanvasFlushIntervalSeconds)
			{
				_lastCanvasFlushRealtime = realtimeSinceStartup;
				RunOverlayValidationTick();
				RunSceneCacheApplyTick();
				RunUiCacheApplyTick();
				RunPeriodicCacheApplyTick();
				FlushPendingTranslations();
				FlushDebouncedTextRequests();
			}
		}
		catch (Exception ex)
		{
			try
			{
				File.WriteAllText(Path.Combine(Path.GetDirectoryName(base.Info.Location), "translator_canvas_error.txt"), $"Canvas callback error at {DateTime.Now}: {ex}\nInner: {ex.InnerException}");
			}
			catch (Exception reportedException)
			{
				ReportCaughtException(reportedException);
			}
		}
	}

	private void RunScannerTick(int intervalMs, string source)
	{
		float realtimeSinceStartup = Time.realtimeSinceStartup;
		float num = Math.Max(0.001f, (float)intervalMs / 1000f);
		if (realtimeSinceStartup - _lastScanRealtime < num)
		{
			return;
		}
		_lastScanRealtime = realtimeSinceStartup;
		EnsureScannerInitialized(source);
		try
		{
			ScanOnce();
		}
		catch (Exception ex)
		{
			ReportCaughtException(ex, "source=" + source);
		}
	}

	private void RequestUiCacheApplyBurst()
	{
		float realtimeSinceStartup = Time.realtimeSinceStartup;
		if (realtimeSinceStartup - _lastUiActivationRealtime < UiActivationThrottleSeconds)
		{
			return;
		}
		_lastUiActivationRealtime = realtimeSinceStartup;
		Interlocked.Increment(ref _uiActivationBurstCount);
		_uiCacheApplyUntilRealtime = Math.Max(_uiCacheApplyUntilRealtime, realtimeSinceStartup + UiActivationCacheApplyWindowSeconds);
		_nextUiCacheApplyRealtime = Math.Min(_nextUiCacheApplyRealtime, realtimeSinceStartup);
		_nextPeriodicCacheApplyRealtime = Math.Min(_nextPeriodicCacheApplyRealtime, realtimeSinceStartup);
		try
		{
			ApplyCachedVisibleTextPass(MaxUiActivationCacheAppliesPerPass);
		}
		catch (Exception ex)
		{
			ReportCaughtException(ex, "phase=immediate");
		}
	}

	private void RunUiCacheApplyTick()
	{
		float realtimeSinceStartup = Time.realtimeSinceStartup;
		if (realtimeSinceStartup > _uiCacheApplyUntilRealtime || realtimeSinceStartup < _nextUiCacheApplyRealtime)
		{
			return;
		}
		_nextUiCacheApplyRealtime = realtimeSinceStartup + UiActivationCacheApplyIntervalSeconds;
		try
		{
			ApplyCachedVisibleTextPass(MaxUiActivationCacheAppliesPerPass);
		}
		catch (Exception ex)
		{
			ReportCaughtException(ex, "phase=activation");
		}
	}

	private void RunPeriodicCacheApplyTick()
	{
		float realtimeSinceStartup = Time.realtimeSinceStartup;
		if (realtimeSinceStartup < _nextPeriodicCacheApplyRealtime)
		{
			return;
		}
		_nextPeriodicCacheApplyRealtime = realtimeSinceStartup + PeriodicCacheApplyIntervalSeconds;
		try
		{
			ApplyCachedVisibleTextPass(MaxPeriodicCacheAppliesPerPass);
		}
		catch (Exception ex)
		{
			ReportCaughtException(ex, "phase=periodic");
		}
	}

	private void RunOverlayValidationTick()
	{
		if (!_overlayDisabled || ShouldUseTmpOverlay())
		{
			float realtimeSinceStartup = Time.realtimeSinceStartup;
			if (!(realtimeSinceStartup < _nextOverlayValidationRealtime))
			{
				_nextOverlayValidationRealtime = realtimeSinceStartup + OverlayValidationIntervalSeconds;
				ValidateActiveTmpOverlays(MaxOverlayValidationsPerPump);
			}
		}
	}

	private bool QueueCachedComponentTextIfAvailable(object component, bool isTmp, bool allowRemoteFallback)
	{
		if (!IsUnityObjectAlive(component))
		{
			return false;
		}
		try
		{
			if (isTmp && !CanHandleTmp())
			{
				return false;
			}
			EnsureScannerInitialized("TEXT_ENABLE");
			string currentComponentText = GetCurrentComponentText(component, isTmp);
			if (isTmp && HasPendingTmpExternalTextWrite(component))
			{
				return false;
			}
			if (isTmp)
			{
				EnsureTmpOverlayMatchesCurrentText(component, currentComponentText);
			}
			if (!string.IsNullOrWhiteSpace(currentComponentText))
			{
				int componentInstanceId = GetComponentInstanceId(component);
				if (TryRepairMixedTranslatedText(currentComponentText, out var originalText, out var repaired))
				{
					if (isTmp)
					{
						ApplyTMProTranslation(component, componentInstanceId, originalText, repaired, _textPropCache, _fontPropCache, _forceMeshMethodCache);
					}
					else
					{
						Text val = (Text)((component is Text) ? component : null);
						if (val != null)
						{
							ApplyFont(val);
							val.text = PrepareTranslatedTextForUGUIText(val, repaired, originalText);
							MarkProcessed(componentInstanceId, originalText);
							TryMarkAppliedCacheKeyForPersist(originalText, repaired);
						}
					}
					Interlocked.Increment(ref _targetedCacheQueueCount);
					return true;
				}
				else if (!ContainsCjk(currentComponentText) && TryGetLocalTranslation(currentComponentText, out var translated))
				{
					bool applied = ApplyCachedComponentTranslationNow(component, componentInstanceId, currentComponentText, translated, isTmp);
					if (applied)
					{
						Interlocked.Increment(ref _targetedCacheQueueCount);
					}
					return applied;
				}
				else if (!ContainsCjk(currentComponentText) && allowRemoteFallback && !WasProcessed(componentInstanceId, currentComponentText) && !IsInProgress(componentInstanceId, currentComponentText) && !ShouldSkipText(currentComponentText) && !IsTranslationRetryCoolingDown(currentComponentText))
				{
					QueueDebouncedTextRequest(component, componentInstanceId, currentComponentText, isTmp);
					Interlocked.Increment(ref _targetedCacheQueueCount);
					return true;
				}
			}
		}
		catch (Exception reportedException)
		{
			ReportCaughtException(reportedException);
		}
		return false;
	}

	private bool ApplyCachedComponentTranslationNow(object component, int componentInstanceId, string originalText, string translatedText, bool isTmp)
	{
		if (!IsUnityObjectAlive(component) || !IsAcceptableTranslation(originalText, translatedText))
		{
			return false;
		}
		if (isTmp)
		{
			if (!CanHandleTmp())
			{
				return false;
			}
			ApplyTMProTranslation(component, componentInstanceId, originalText, translatedText, _textPropCache, _fontPropCache, _forceMeshMethodCache);
			return true;
		}
		Text val = component as Text;
		if ((Object)(object)val == (Object)null)
		{
			return false;
		}
		ApplyFont(val);
		val.text = PrepareTranslatedTextForUGUIText(val, translatedText, originalText);
		MarkProcessed(componentInstanceId, originalText);
		TryMarkAppliedCacheKeyForPersist(originalText, translatedText);
		return true;
	}

	private bool LooksLikeUiGameObject(GameObject gameObject)
	{
		if ((Object)(object)gameObject == (Object)null)
		{
			return false;
		}
		try
		{
			return (Object)(object)gameObject.GetComponent<RectTransform>() != (Object)null || (Object)(object)gameObject.GetComponent<Canvas>() != (Object)null || (Object)(object)gameObject.GetComponent<CanvasGroup>() != (Object)null;
		}
		catch (Exception ex)
		{
			ReportCaughtException(ex);
			return false;
		}
	}

	private bool ShouldHandleCanvasGroupVisible(CanvasGroup canvasGroup, float alpha)
	{
		int componentInstanceId = GetComponentInstanceId(canvasGroup);
		bool flag = alpha > 0.01f && ((Component)canvasGroup).gameObject.activeInHierarchy;
		lock (_pendingLock)
		{
			_canvasGroupVisibleStates.TryGetValue(componentInstanceId, out var value);
			_canvasGroupVisibleStates[componentInstanceId] = flag;
			return flag && !value;
		}
	}

	private void QueueCachedTextsInHierarchy(GameObject root, int maxQueue, bool allowRemoteFallback = false)
	{
		if ((Object)(object)root == (Object)null || maxQueue <= 0)
		{
			return;
		}
		try
		{
			EnsureScannerInitialized("UI_TREE");
			int num = 0;
			if (_tmpTextTypeCache != null && CanHandleTmp())
			{
				Component[] componentsInChildren = GetComponentsInChildrenByTypeSafe(root, _tmpTextTypeCache, includeInactive: true);
				foreach (Component val in componentsInChildren)
				{
					if (num >= maxQueue)
					{
						return;
					}
					if (!((Object)(object)val == (Object)null) && IsComponentActive(val))
					{
						if (QueueCachedComponentTextIfAvailable(val, isTmp: true, allowRemoteFallback))
						{
							num++;
						}
					}
				}
			}
			Text[] componentsInChildren2 = root.GetComponentsInChildren<Text>(true);
			foreach (Text val2 in componentsInChildren2)
			{
				if (num >= maxQueue)
				{
					break;
				}
				if (!((Object)(object)val2 == (Object)null) && IsComponentActive(val2))
				{
					if (QueueCachedComponentTextIfAvailable(val2, isTmp: false, allowRemoteFallback))
					{
						num++;
					}
				}
			}
		}
		catch (Exception reportedException)
		{
			ReportCaughtException(reportedException);
		}
	}

	private void EnsureScannerInitialized(string source)
	{
		if (!_scannerInitialized)
		{
			_scannerInitialized = true;
			_tmpTextTypeCache = AccessTools.TypeByName("TMPro.TMP_Text");
			_textAssetTypeCache = AccessTools.TypeByName("UnityEngine.TextAsset");
			_textPropCache = ((_tmpTextTypeCache != null) ? AccessTools.Property(_tmpTextTypeCache, "text") : null);
			_fontPropCache = ((_tmpTextTypeCache != null) ? AccessTools.Property(_tmpTextTypeCache, "font") : null);
			_forceMeshMethodCache = ((_tmpTextTypeCache != null) ? (AccessTools.Method(_tmpTextTypeCache, "ForceMeshUpdate", new Type[2]
			{
				typeof(bool),
				typeof(bool)
			}) ?? AccessTools.Method(_tmpTextTypeCache, "ForceMeshUpdate", new Type[1] { typeof(bool) }) ?? AccessTools.Method(_tmpTextTypeCache, "ForceMeshUpdate", Type.EmptyTypes)) : null);
			base.Logger.LogWarning($"[{source}] Scanner init: TMPro={_tmpTextTypeCache != null}");
		}
	}

	private void ScanOnce()
	{
		_scanCount++;
		if (_scanCount % 10 == 1)
		{
			try
			{
				RefreshTmpSubMeshMaterials();
			}
			catch (Exception reportedException)
			{
				ReportCaughtException(reportedException);
			}
		}
		try
		{
			PruneLongRunningStateIfNeeded();
		}
		catch (Exception reportedException)
		{
			ReportCaughtException(reportedException);
		}
		int num = 0;
		int num2 = 0;
		int num3 = 0;
		int num4 = 0;
		int num5 = 0;
		int num6 = 0;
		int num7 = 0;
		int num8 = 0;
		int num9 = 0;
		List<string> list = new List<string>(8);
		if (_tmpTextTypeCache != null && CanHandleTmp())
		{
			Object[] array = FindTmpTextObjectsSafe(_tmpTextTypeCache);
			_totalTmpFound = array.Length;
			num = array.Length;
			Object[] array2 = array;
			foreach (Object val in array2)
			{
				if (val == (Object)null)
				{
					continue;
				}
				string text = _textPropCache?.GetValue(val) as string;
				EnsureTmpOverlayMatchesCurrentText(val, text);
				if (string.IsNullOrWhiteSpace(text) || !IsComponentActive(val))
				{
					continue;
				}
				num2++;
				ApplyTMPFont(val);
				int componentInstanceId = GetComponentInstanceId(val);
				if (ContainsCjk(text))
				{
					if (TryRepairMixedTranslatedText(text, out var originalText, out var repaired))
					{
						num4++;
						ApplyTMProTranslation(val, componentInstanceId, originalText, repaired, _textPropCache, _fontPropCache, _forceMeshMethodCache);
						continue;
					}
					num3++;
					text = NormalizeTmpPunctuationForMissingGlyphs(text);
					bool tmpFontCoversText = EnsureTMPFontCoversText(val, text);
					if (!IsTmpOverlayCurrent(val, text, text))
					{
						ApplyTMPFont(val);
					ApplyTmpOverlay(val, text, text, !tmpFontCoversText);
				}
				MarkProcessed(componentInstanceId, text);
				LogCjkRenderStateOnce(val, text);
				continue;
				}
				if (TryGetLocalTranslation(text, out var translated))
				{
					_translateCacheHits++;
					num4++;
					ApplyTMProTranslation(val, componentInstanceId, text, translated, _textPropCache, _fontPropCache, _forceMeshMethodCache);
					continue;
				}
				if (WasProcessed(componentInstanceId, text))
				{
					num5++;
					continue;
				}
				if (IsInProgress(componentInstanceId, text))
				{
					num6++;
					continue;
				}
				if (ShouldSkipText(text))
				{
					num7++;
					continue;
				}
				if (IsTranslationRetryCoolingDown(text))
				{
					num8++;
					continue;
				}
				QueueDebouncedTextRequest(val, componentInstanceId, text, isTmp: true);
				num9++;
				if (list.Count < 8)
				{
					list.Add((text.Length > 80) ? (text.Substring(0, 80) + "...") : text);
				}
			}
		}
		Text[] array3 = FindObjectsOfTypeAllSafe<Text>();
		foreach (Text val2 in array3)
		{
			if ((Object)(object)val2 == (Object)null || string.IsNullOrWhiteSpace(val2.text) || !IsComponentActive(val2))
			{
				continue;
			}
			string text2 = val2.text;
			int componentInstanceId2 = GetComponentInstanceId(val2);
			string translated2;
			if (ContainsCjk(text2))
			{
				if (TryRepairMixedTranslatedText(text2, out var originalText2, out var repaired2))
				{
					val2.text = PrepareTranslatedTextForUGUIText(val2, repaired2, originalText2);
					if ((Object)(object)_chineseFont != (Object)null)
					{
						val2.font = _chineseFont;
					}
					MarkProcessed(componentInstanceId2, originalText2);
					TryMarkAppliedCacheKeyForPersist(originalText2, repaired2);
				}
				else
				{
					if ((Object)(object)_chineseFont != (Object)null && (Object)(object)val2.font != (Object)(object)_chineseFont)
					{
						val2.font = _chineseFont;
					}
					MarkProcessed(componentInstanceId2, text2);
				}
				LogCjkRenderStateOnce(val2, text2);
			}
			else if (TryGetLocalTranslation(text2, out translated2))
			{
				val2.text = PrepareTranslatedTextForUGUIText(val2, translated2, text2);
				if ((Object)(object)_chineseFont != (Object)null)
				{
					val2.font = _chineseFont;
				}
				MarkProcessed(componentInstanceId2, text2);
				TryMarkAppliedCacheKeyForPersist(text2, translated2);
			}
			else
			{
				if (WasProcessed(componentInstanceId2, text2) || IsInProgress(componentInstanceId2, text2) || ShouldSkipText(text2))
				{
					continue;
				}
				if (IsTranslationRetryCoolingDown(text2))
				{
					continue;
				}
				QueueDebouncedTextRequest(val2, componentInstanceId2, text2, isTmp: false);
				num9++;
			}
		}
		if (_scanCount % 30 != 1)
		{
			return;
		}
		base.Logger.LogInfo($"[SCAN-DIAG] tick={_scanCount} tmp_total={num} active={num2} " + $"cjk={num3} cache_hit={num4} queued={num9} " + $"was_processed={num5} in_progress={num6} " + $"skip={num7} untranslatable={num8}");
		if (list.Count <= 0)
		{
			return;
		}
		foreach (string item in list)
		{
			base.Logger.LogInfo("[SCAN-MISS] '" + item + "'");
		}
	}

	private void RunAlphaSweepTick()
	{
		float realtimeSinceStartup = Time.realtimeSinceStartup;
		if (realtimeSinceStartup - _lastAlphaSweepRealtime < AlphaSweepIntervalSeconds)
		{
			return;
		}
		_lastAlphaSweepRealtime = realtimeSinceStartup;
		Interlocked.Increment(ref _alphaSweepRunCount);
		if (_tmpTextTypeCache == null || !CanHandleTmp())
		{
			return;
		}
		try
		{
			int num = 0;
			int alphaRescuedCount = _alphaRescuedCount;
			Object[] array = FindTmpTextObjectsSafe(_tmpTextTypeCache);
			foreach (Object val in array)
			{
				if (!(val == (Object)null) && IsComponentActive(val) && (_chineseTMPFont == null || AccessTools.Property(((object)val).GetType(), "font")?.GetValue(val) == _chineseTMPFont))
				{
					RescueStrandedAlpha(val);
				}
			}
			num = _alphaRescuedCount - alphaRescuedCount;
			if (num > 0)
			{
				Interlocked.Add(ref _alphaSweepHealedCount, num);
			}
		}
		catch (Exception reportedException)
		{
			ReportCaughtException(reportedException);
		}
	}

	private void RunSceneCacheApplyTick()
	{
		if (_sceneCacheApplyPassesRemaining <= 0)
		{
			return;
		}
		if (_sceneCacheApplyGeneration != _sceneWarmupGeneration)
		{
			_sceneCacheApplyPassesRemaining = 0;
			return;
		}
		float realtimeSinceStartup = Time.realtimeSinceStartup;
		if (!(realtimeSinceStartup < _nextSceneCacheApplyRealtime))
		{
			ApplyCachedVisibleTextPass(MaxSceneCacheAppliesPerPass);
			FlushPendingTranslations();
			_sceneCacheApplyPassesRemaining--;
			_nextSceneCacheApplyRealtime = realtimeSinceStartup + SceneCacheApplyIntervalSeconds;
		}
	}

	private void RunDeepPrefetchTick()
	{
		ConfigEntry<bool> deepPrefetchEnabled = _deepPrefetchEnabled;
		if (deepPrefetchEnabled == null || !deepPrefetchEnabled.Value || IsEcoPerformance())
		{
			return;
		}
		float realtimeSinceStartup = Time.realtimeSinceStartup;
		if (realtimeSinceStartup < DeepPrefetchInitialDelaySeconds ||
			realtimeSinceStartup - _lastDeepPrefetchRealtime < DeepPrefetchScanIntervalSeconds ||
			Interlocked.CompareExchange(ref _deepPrefetchActive, 1, 0) != 0)
		{
			return;
		}
		if (HasPendingClientTranslationWork())
		{
			Interlocked.Exchange(ref _deepPrefetchActive, 0);
			return;
		}
		_lastDeepPrefetchRealtime = realtimeSinceStartup;
		try
		{
			CollectDeepPrefetchCandidates(DeepPrefetchMaxObjectsPerScan, DeepPrefetchMaxTextsPerScan);
			_ = ProcessDeepPrefetchQueueAsync();
		}
		catch (Exception ex)
		{
			ReportCaughtException(ex);
			Interlocked.Exchange(ref _deepPrefetchActive, 0);
		}
	}

	private async Task ProcessDeepPrefetchQueueAsync()
	{
		_ = 1;
		try
		{
			while ((_deepPrefetchEnabled?.Value ?? false) && !HasPendingClientTranslationWork())
			{
				List<string> list = new List<string>(DeepPrefetchBatchSize);
				lock (_pendingLock)
				{
					while (_deepPrefetchQueue.Count > 0 && list.Count < DeepPrefetchBatchSize)
					{
						list.Add(_deepPrefetchQueue.Dequeue());
					}
				}
				if (list.Count == 0)
				{
					break;
				}
				await WarmupTextsAsync(list, "dialogue");
				await Task.Delay((int)(DeepPrefetchChunkPauseSeconds * 1000f));
			}
		}
		finally
		{
			Interlocked.Exchange(ref _deepPrefetchActive, 0);
		}
	}

	private void CollectDeepPrefetchCandidates(int maxObjects, int maxTexts)
	{
		EnsureScannerInitialized("DEEP");
		List<string> list = new List<string>(maxTexts);
		int num = 0;
		if (_textAssetTypeCache != null)
		{
			PropertyInfo propertyInfo = AccessTools.Property(_textAssetTypeCache, "text");
			Object[] array = FindObjectsOfTypeAllSafe(_textAssetTypeCache);
			foreach (Object val in array)
			{
				if (!(val == (Object)null) && TryMarkDeepScanned(val))
				{
					num++;
					ExtractDeepPrefetchText(propertyInfo?.GetValue(val) as string, list, maxTexts);
					if (num >= maxObjects || list.Count >= maxTexts)
					{
						break;
					}
				}
			}
		}
		if (num < maxObjects && list.Count < maxTexts)
		{
			ScriptableObject[] array2 = FindObjectsOfTypeAllSafe<ScriptableObject>();
			foreach (ScriptableObject val2 in array2)
			{
				if (!((Object)(object)val2 == (Object)null) && TryMarkDeepScanned(val2))
				{
					num++;
					ExtractDeepObjectStrings(val2, list, maxTexts);
					if (num >= maxObjects || list.Count >= maxTexts)
					{
						break;
					}
				}
			}
		}
		if (num < maxObjects && list.Count < maxTexts)
		{
			MonoBehaviour[] array3 = FindObjectsOfTypeAllSafe<MonoBehaviour>();
			foreach (MonoBehaviour val3 in array3)
			{
				if (!((Object)(object)val3 == (Object)null) && IsComponentActive(val3) && TryMarkDeepScanned(val3))
				{
					num++;
					ExtractDeepObjectStrings(val3, list, maxTexts);
					if (num >= maxObjects || list.Count >= maxTexts)
					{
						break;
					}
				}
			}
		}
		if (list.Count == 0)
		{
			return;
		}
		lock (_pendingLock)
		{
			foreach (string item in list)
			{
				_deepPrefetchQueue.Enqueue(item);
			}
		}
		LogVerbose($"[DEEP] queued {list.Count} prefetch texts from {num} objects");
	}

	private bool TryMarkDeepScanned(object obj)
	{
		Object val = (Object)((obj is Object) ? obj : null);
		if (val == null)
		{
			return false;
		}
		int instanceID = val.GetInstanceID();
		lock (_pendingLock)
		{
			return _deepScannedObjects.Add(instanceID);
		}
	}

	private void ExtractDeepObjectStrings(object obj, List<string> output, int maxTexts)
	{
		if (obj == null || output.Count >= maxTexts)
		{
			return;
		}
		FieldInfo[] fields = obj.GetType().GetFields(BindingFlags.Instance | BindingFlags.Public | BindingFlags.NonPublic);
		foreach (FieldInfo fieldInfo in fields)
		{
			if (output.Count >= maxTexts)
			{
				break;
			}
			try
			{
				if (fieldInfo.FieldType == typeof(string))
				{
					ExtractDeepPrefetchText(fieldInfo.GetValue(obj) as string, output, maxTexts);
				}
				else
				{
					if (fieldInfo.FieldType == typeof(string[]))
					{
						if (!(fieldInfo.GetValue(obj) is string[] array))
						{
							continue;
						}
						string[] array2 = array;
						foreach (string raw in array2)
						{
							ExtractDeepPrefetchText(raw, output, maxTexts);
							if (output.Count >= maxTexts)
							{
								return;
							}
						}
						continue;
					}
					if (!typeof(IEnumerable<string>).IsAssignableFrom(fieldInfo.FieldType) || !(fieldInfo.GetValue(obj) is IEnumerable<string> enumerable))
					{
						continue;
					}
					foreach (string item in enumerable)
					{
						ExtractDeepPrefetchText(item, output, maxTexts);
						if (output.Count >= maxTexts)
						{
							return;
						}
					}
					continue;
				}
			}
			catch (Exception reportedException)
			{
				ReportCaughtException(reportedException);
			}
		}
	}

	private void ExtractDeepPrefetchText(string raw, List<string> output, int maxTexts)
	{
		if (string.IsNullOrWhiteSpace(raw) || output.Count >= maxTexts)
		{
			return;
		}
		if (raw.Length <= 260)
		{
			TryAddDeepPrefetchCandidate(raw, output);
			return;
		}
		string[] array = raw.Replace("\r", "\n").Split(new char[] { '\n' }, StringSplitOptions.None);
		foreach (string text in array)
		{
			TryAddDeepPrefetchCandidate(text, output);
			if (output.Count >= maxTexts)
			{
				return;
			}
		}
		foreach (string item in ExtractQuotedSegments(raw, maxTexts - output.Count))
		{
			TryAddDeepPrefetchCandidate(item, output);
			if (output.Count >= maxTexts)
			{
				break;
			}
		}
	}

	private static IEnumerable<string> ExtractQuotedSegments(string raw, int maxSegments)
	{
		int resultCount = 0;
		StringBuilder builder = new StringBuilder();
		bool flag = false;
		bool escaped = false;
		foreach (char c in raw)
		{
			if (!flag)
			{
				if (c == '"')
				{
					flag = true;
					builder.Length = 0;
				}
				continue;
			}
			if (escaped)
			{
				builder.Append(c);
				escaped = false;
				continue;
			}
			switch (c)
			{
			case '\\':
				escaped = true;
				break;
			case '"':
			{
				string text = builder.ToString();
				if (!string.IsNullOrWhiteSpace(text))
				{
					yield return text;
					resultCount++;
					if (resultCount >= maxSegments)
					{
						yield break;
					}
				}
				flag = false;
				builder.Length = 0;
				break;
			}
			default:
				builder.Append(c);
				break;
			}
		}
	}

	private void TryAddDeepPrefetchCandidate(string text, List<string> output)
	{
		if (string.IsNullOrWhiteSpace(text))
		{
			return;
		}
		text = text.Trim();
		if (text.Length < 4 || text.Length > 220)
		{
			return;
		}
		string text2 = GetVisibleText(text).Trim();
		if (text2.Length < 4 || text2.Length > 220 || ContainsCjk(text2) || LooksLikeTypewriterFragment(text) || ShouldSkipText(text) || IsTranslationRetryCoolingDown(text) || !LooksLikeNaturalLanguage(text2) || TryGetLocalTranslation(text, out var _))
		{
			return;
		}
		string item = NormalizeRequestText(text);
		lock (_pendingLock)
		{
			if (!_deepPrefetchSeen.Add(item))
			{
				return;
			}
		}
		output.Add(text);
	}

	private static bool LooksLikeNaturalLanguage(string text)
	{
		if (string.IsNullOrWhiteSpace(text))
		{
			return false;
		}
		string text2 = text.ToLowerInvariant();
		if (text2.Contains("http://") || text2.Contains("https://") || text2.Contains("unityengine") || text2.Contains("addressable") || text2.Contains(".dll") || text2.Contains(".json"))
		{
			return false;
		}
		if (text.Count(char.IsLetter) < 4)
		{
			return false;
		}
		if (text.Count((char ch) => ch == '_' || ch == '/' || ch == '\\') >= 2)
		{
			return false;
		}
		if (!text.Any(char.IsWhiteSpace) && text.IndexOfAny(new char[6] { '.', ',', '!', '?', '\'', '"' }) < 0)
		{
			return text.Any(char.IsLower);
		}
		return true;
	}

	private void ApplyCachedVisibleTextPass(int maxApply)
	{
		EnsureScannerInitialized("CACHE");
		Interlocked.Increment(ref _cacheApplyPassCount);
		int num = 0;
		if (_tmpTextTypeCache != null && CanHandleTmp())
		{
			Object[] array = FindTmpTextObjectsSafe(_tmpTextTypeCache);
			foreach (Object val in array)
			{
				if (num >= maxApply)
				{
					return;
				}
				if (val == (Object)null || !IsComponentActive(val))
				{
					continue;
				}
				string text = _textPropCache?.GetValue(val) as string;
				EnsureTmpOverlayMatchesCurrentText(val, text);
				if (!string.IsNullOrWhiteSpace(text))
				{
					int componentInstanceId = GetComponentInstanceId(val);
					if (TryRepairMixedTranslatedText(text, out var originalText, out var repaired))
					{
						ApplyTMProTranslation(val, componentInstanceId, originalText, repaired, _textPropCache, _fontPropCache, _forceMeshMethodCache);
						Interlocked.Increment(ref _cacheApplyHitCount);
						num++;
					}
					else if (!ContainsCjk(text) && TryGetLocalTranslation(text, out var translated))
					{
						ApplyTMProTranslation(val, componentInstanceId, text, translated, _textPropCache, _fontPropCache, _forceMeshMethodCache);
						Interlocked.Increment(ref _cacheApplyHitCount);
						num++;
					}
					else if (!ContainsCjk(text) && !WasProcessed(componentInstanceId, text))
					{
						ShouldSkipText(text);
					}
				}
			}
		}
		Text[] array2 = FindObjectsOfTypeAllSafe<Text>();
		foreach (Text val2 in array2)
		{
			if (num >= maxApply)
			{
				break;
			}
			if ((Object)(object)val2 == (Object)null || !IsComponentActive(val2))
			{
				continue;
			}
			string text2 = val2.text;
			if (string.IsNullOrWhiteSpace(text2))
			{
				continue;
			}
			int componentInstanceId2 = GetComponentInstanceId(val2);
			if (TryRepairMixedTranslatedText(text2, out var originalText2, out var repaired2))
			{
				val2.text = PrepareTranslatedTextForUGUIText(val2, repaired2, originalText2);
				if ((Object)(object)_chineseFont != (Object)null)
				{
					val2.font = _chineseFont;
				}
				MarkProcessed(componentInstanceId2, originalText2);
				TryMarkAppliedCacheKeyForPersist(originalText2, repaired2);
				Interlocked.Increment(ref _cacheApplyHitCount);
				num++;
			}
			else if (ContainsCjk(text2))
			{
				continue;
			}
			else if (TryGetLocalTranslation(text2, out var translated2))
			{
				val2.text = PrepareTranslatedTextForUGUIText(val2, translated2, text2);
				if ((Object)(object)_chineseFont != (Object)null)
				{
					val2.font = _chineseFont;
				}
				MarkProcessed(componentInstanceId2, text2);
				TryMarkAppliedCacheKeyForPersist(text2, translated2);
				Interlocked.Increment(ref _cacheApplyHitCount);
				num++;
			}
			else if (!WasProcessed(componentInstanceId2, text2))
			{
				ShouldSkipText(text2);
			}
		}
	}

	private void ConfigureHttpFastPath()
	{
		try
		{
			ServicePointManager.DefaultConnectionLimit = Math.Max(ServicePointManager.DefaultConnectionLimit, 32);
			ServicePointManager.Expect100Continue = false;
			ServicePointManager.UseNagleAlgorithm = false;
		}
		catch (Exception reportedException)
		{
			ReportCaughtException(reportedException);
		}
	}

	private static string HttpPost(string url, string jsonBody, int timeoutMs = HttpTimeoutMs)
	{
		return RawHttpRequest(url, "POST", jsonBody, timeoutMs);
	}

	private static Task<T> RunBackground<T>(Func<T> work)
	{
		/* BepInEx/...Unity ...SynchronizationContext 不可靠，因此不依...Task.Run
		   的上下文行为；但也不能为每次 HTTP/磁盘任务创建一...OS 线程。工作项进入
		   CLR 线程池，返回值只能是托管数据，禁止在线程中读取或写入 Unity 对象...*/
		TaskCompletionSource<T> completion = new TaskCompletionSource<T>();
		try
		{
			if (!ThreadPool.QueueUserWorkItem(delegate
			{
				try
				{
					completion.SetResult(work());
				}
				catch (Exception exception)
				{
					completion.SetException(exception);
				}
			}))
			{
				completion.SetException(new InvalidOperationException("Unable to queue background work."));
			}
		}
		catch (Exception exception2)
		{
			completion.SetException(exception2);
		}
		return completion.Task;
	}

	private static string BuildTranslatePayload(string text, string domain)
	{
		bool priority = string.Equals(domain, "ui", StringComparison.OrdinalIgnoreCase);
		return "{\"text\":\"" + EscapeJson(text) + "\",\"engine\":\"unity\",\"domain\":\"" + EscapeJson(domain) + "\",\"priority\":" + (priority ? "true" : "false") + "}";
	}

	private static string BuildBatchPayload(IEnumerable<string> texts, string domain)
	{
		StringBuilder stringBuilder = new StringBuilder();
		stringBuilder.Append("{\"texts\":[");
		bool first = true;
		foreach (string text in texts ?? Enumerable.Empty<string>())
		{
			if (!first)
			{
				stringBuilder.Append(',');
			}
			first = false;
			stringBuilder.Append('"').Append(EscapeJson(text)).Append('"');
		}
		bool priority = string.Equals(domain, "ui", StringComparison.OrdinalIgnoreCase);
		stringBuilder.Append("],\"engine\":\"unity\",\"domain\":\"").Append(EscapeJson(domain)).Append("\",\"priority\":")
			.Append(priority ? "true" : "false")
			.Append('}');
		return stringBuilder.ToString();
	}

	private static string GetBatchTranslation(JObject response, int index, string originalText, ProtectedTextPayload payload)
	{
		if (response == null)
		{
			return null;
		}
		if (response["items"] is JArray items && index >= 0 && index < ((JContainer)items).Count)
		{
			JToken item = items[index];
			return ((item == null) ? null : ((object)(item[(object)"translation"] ?? item[(object)"translated_text"] ?? item[(object)"value"]))?.ToString());
		}
		if (response["translations"] is JObject translations)
		{
			string requestText = payload?.RequestText;
			string text = (!string.IsNullOrEmpty(requestText) ? ((object)translations[(object)requestText])?.ToString() : null);
			if (string.IsNullOrEmpty(text) && !string.IsNullOrEmpty(originalText))
			{
				text = ((object)translations[(object)originalText])?.ToString();
			}
			if (!string.IsNullOrEmpty(text))
			{
				return text;
			}
		}
		if (response["results"] is JArray results && index >= 0 && index < ((JContainer)results).Count)
		{
			return ((object)results[index])?.ToString();
		}
		return null;
	}

	/* 服务器 /batch 的 sources 与请求按序对齐：miss/queued 是回显原文，pass 是透传。 */
	private static string GetBatchResponseSource(JObject response, int index)
	{
		if (response != null && response["sources"] is JArray sources && index >= 0 && index < ((JContainer)sources).Count)
		{
			return ((object)sources[index])?.ToString();
		}
		return null;
	}

	private static bool IsBatchPassThroughSource(string source)
	{
		return string.Equals(source, "miss", StringComparison.OrdinalIgnoreCase)
			|| string.Equals(source, "queued", StringComparison.OrdinalIgnoreCase)
			|| string.Equals(source, "pass", StringComparison.OrdinalIgnoreCase);
	}

	private static string SerializeStringMap(IReadOnlyDictionary<string, string> map)
	{
		StringBuilder stringBuilder = new StringBuilder();
		stringBuilder.Append('{');
		bool first = true;
		if (map != null)
		{
			foreach (KeyValuePair<string, string> item in map)
			{
				if (!first)
				{
					stringBuilder.Append(',');
				}
				first = false;
				stringBuilder.Append('"').Append(EscapeJson(item.Key)).Append("\":\"").Append(EscapeJson(item.Value)).Append('"');
			}
		}
		stringBuilder.Append('}');
		return stringBuilder.ToString();
	}

	private static string EscapeJson(string text)
	{
		if (string.IsNullOrEmpty(text))
		{
			return string.Empty;
		}
		StringBuilder stringBuilder = new StringBuilder(text.Length);
		foreach (char c in text)
		{
			switch (c)
			{
			case '"':
				stringBuilder.Append("\\\"");
				continue;
			case '\\':
				stringBuilder.Append("\\\\");
				continue;
			case '\n':
				stringBuilder.Append("\\n");
				continue;
			case '\r':
				stringBuilder.Append("\\r");
				continue;
			case '\t':
				stringBuilder.Append("\\t");
				continue;
			case '\b':
				stringBuilder.Append("\\b");
				continue;
			case '\f':
				stringBuilder.Append("\\f");
				continue;
			}
			if (c < ' ')
			{
				stringBuilder.AppendFormat("\\u{0:X4}", (int)c);
			}
			else
			{
				stringBuilder.Append(c);
			}
		}
		return stringBuilder.ToString();
	}

	private static string HttpGet(string url, int timeoutMs = HttpTimeoutMs)
	{
		return RawHttpRequest(url, "GET", null, timeoutMs);
	}

	private static string RawHttpRequest(string url, string method, string jsonBody, int timeoutMs)
	{
		Uri uri = new Uri(url);
		string pathAndQuery = string.IsNullOrEmpty(uri.PathAndQuery) ? "/" : uri.PathAndQuery;
		int port = uri.Port > 0 ? uri.Port : 80;
		byte[] bodyBytes = string.IsNullOrEmpty(jsonBody) ? Array.Empty<byte>() : Encoding.UTF8.GetBytes(jsonBody);
		using (TcpClient tcpClient = new TcpClient())
		{
			tcpClient.ReceiveTimeout = timeoutMs;
			tcpClient.SendTimeout = timeoutMs;
			/* Connect 本身不受 Receive/SendTimeout 约束，目标不可达时会按系统默认
			   阻塞数十秒；BeginConnect + WaitOne 把连接也纳入同一超时预算。 */
			IAsyncResult connectResult = tcpClient.BeginConnect(uri.Host, port, null, null);
			using (connectResult.AsyncWaitHandle)
			{
				if (!connectResult.AsyncWaitHandle.WaitOne(timeoutMs))
				{
					tcpClient.Close();
					throw new TimeoutException("TCP connect to " + uri.Host + ":" + port + " timed out after " + timeoutMs + "ms.");
				}
				tcpClient.EndConnect(connectResult);
			}
			using (NetworkStream networkStream = tcpClient.GetStream())
			{
				networkStream.ReadTimeout = timeoutMs;
				networkStream.WriteTimeout = timeoutMs;
				string text = method + " " + pathAndQuery + " HTTP/1.1\r\n" + "Host: " + uri.Host + ":" + port + "\r\n" + "Accept: application/json\r\n" + "Connection: close\r\n";
				if (bodyBytes.Length > 0)
				{
					text = text + "Content-Type: application/json; charset=utf-8\r\n" + "Content-Length: " + bodyBytes.Length + "\r\n";
				}
				text += "\r\n";
				byte[] bytes = Encoding.ASCII.GetBytes(text);
				networkStream.Write(bytes, 0, bytes.Length);
				if (bodyBytes.Length > 0)
				{
					networkStream.Write(bodyBytes, 0, bodyBytes.Length);
				}
				using (MemoryStream memoryStream = new MemoryStream())
				{
					byte[] array = new byte[8192];
					int count;
					while ((count = networkStream.Read(array, 0, array.Length)) > 0)
					{
						memoryStream.Write(array, 0, count);
					}
					/* 默认 MemoryStream 的底层缓冲可公开；直接按 Length 解码，避...					   cache dump 等大响应再由 ToArray 复制一整份...*/
					return DecodeRawHttpResponse(memoryStream.GetBuffer(), checked((int)memoryStream.Length));
				}
			}
		}
	}

	private static string DecodeRawHttpResponse(byte[] response, int responseLength)
	{
		if (response == null || responseLength <= 0 || responseLength > response.Length)
		{
			return null;
		}
		int num = -1;
		for (int i = 0; i + 3 < responseLength; i++)
		{
			if (response[i] == 13 && response[i + 1] == 10 && response[i + 2] == 13 && response[i + 3] == 10)
			{
				num = i;
				break;
			}
		}
		if (num < 0)
		{
			return null;
		}
		string @string = Encoding.ASCII.GetString(response, 0, num);
		string[] array = @string.Split(new string[1] { "\r\n" }, StringSplitOptions.None);
		if (array.Length == 0)
		{
			return null;
		}
		string[] array2 = array[0].Split(new char[1] { ' ' }, StringSplitOptions.RemoveEmptyEntries);
		if (array2.Length < 2 || !int.TryParse(array2[1], out var result) || result < 200 || result >= 300)
		{
			return null;
		}
		int num2 = num + 4;
		int num3 = responseLength - num2;
		foreach (string text in array)
		{
			if (text.StartsWith("Content-Length:", StringComparison.OrdinalIgnoreCase) && int.TryParse(text.Substring("Content-Length:".Length).Trim(), out var result2))
			{
				num3 = Math.Min(num3, Math.Max(0, result2));
				break;
			}
		}
		return Encoding.UTF8.GetString(response, num2, num3);
	}

	private bool IsServerBackoffActive()
	{
		lock (_serverStateLock)
		{
			return DateTime.UtcNow < _serverBackoffUntilUtc;
		}
	}

	private void NoteServerRequestSucceeded()
	{
		lock (_serverStateLock)
		{
			_serverFailureCount = 0;
			_serverBackoffUntilUtc = DateTime.MinValue;
		}
	}

	private void NoteServerRequestFailed(Exception ex = null)
	{
		DateTime utcNow = DateTime.UtcNow;
		bool flag = false;
		TimeSpan timeSpan;
		lock (_serverStateLock)
		{
			_serverFailureCount = Math.Min(_serverFailureCount + 1, 6);
			timeSpan = TimeSpan.FromMilliseconds(Math.Min(
				ServerOfflineMaxBackoffMs,
				ServerOfflineBaseBackoffMs << Math.Min(_serverFailureCount - 1, 4)));
			_serverBackoffUntilUtc = utcNow + timeSpan;
			if (utcNow - _lastServerFailureLogUtc >= TimeSpan.FromMilliseconds(ServerFailureLogIntervalMs))
			{
				_lastServerFailureLogUtc = utcNow;
				flag = true;
			}
		}
		if (flag)
		{
			string arg = ((ex == null) ? "empty response" : ex.Message);
			base.Logger.LogWarning($"Translation server unavailable; pausing requests for {Math.Round(timeSpan.TotalSeconds, 1)}s ({arg})");
		}
	}

	private void OnApplicationQuit()
	{
		if (_runtimeOwner)
		{
			_applicationQuitting = true;
		}
	}

	private void OnDestroy()
	{
		if (!_runtimeOwner)
		{
			return;
		}
		/* 先停止会闭包引用本插件的诊断线程，再同步写最后一次本地快照并解绑入口...*/
		_shuttingDown = true;
		if (ReferenceEquals(_instance, this))
		{
			_instance = null;
			_pluginHostRoot = null;
		}
		_diagnosticsStop?.Set();
		Thread diagnosticsThread = _diagnosticsThread;
		if (diagnosticsThread != null && diagnosticsThread != Thread.CurrentThread && diagnosticsThread.IsAlive)
		{
			diagnosticsThread.Join(250);
		}
		_diagnosticsThread = null;
		FlushLocalCacheToDisk();
		UnregisterSceneLoadedCallback();
		UnregisterBeforeRenderPump();
		UnregisterCanvasRenderCallback();
		UnpatchHarmony();
		if ((Object)(object)_driver != (Object)null)
		{
			GameObject gameObject = ((Component)_driver).gameObject;
			_driver = null;
			if ((Object)(object)gameObject != (Object)null)
			{
				Object.Destroy((Object)(object)gameObject);
			}
		}
	}

	private void UnpatchHarmony()
	{
		if (_harmony == null)
		{
			return;
		}
		try
		{
			MethodInfo method = _harmony.GetType().GetMethod("UnpatchSelf", Type.EmptyTypes);
			if (method != null)
			{
				method.Invoke(_harmony, null);
				return;
			}
			_harmony.GetType().GetMethod("UnpatchAll", new Type[1] { typeof(string) })?.Invoke(_harmony, new object[1] { "com.deepseek.translator" });
		}
		catch (Exception ex)
		{
			base.Logger.LogWarning("Failed to unpatch Harmony hooks: " + (ex.InnerException?.Message ?? ex.Message));
		}
	}

	private void Update()
	{
		/* 幻影实例（非运行时宿主）未初始化，绝不能进入泵逻辑。 */
		if (!_runtimeOwner)
		{
			return;
		}
		/* 独立 driver 是正常泵；宿...Update 仅在 driver 丢失或被禁用时兜底...*/
		if ((Object)(object)_driver == (Object)null || !((Behaviour)_driver).isActiveAndEnabled)
		{
			DriverUpdate();
		}
	}

	private void OnSceneLoaded(Scene scene, LoadSceneMode mode)
	{
		_lastSceneLoadRealtime = Time.realtimeSinceStartup;
		ResetSceneScopedState(clearPendingComponentWork: true);
		BeginSceneWarmupGeneration();
		try
		{
			RefreshTmpSubMeshMaterials();
		}
		catch (Exception reportedException)
		{
			ReportCaughtException(reportedException);
		}
		try
		{
			int cacheApplyHitCount = _cacheApplyHitCount;
			ApplyCachedVisibleTextPass(int.MaxValue);
			int num = _cacheApplyHitCount - cacheApplyHitCount;
			Interlocked.Increment(ref _immediateSceneApplyCount);
			Interlocked.Add(ref _immediateSceneHits, num);
			if (_debugMode != null && _debugMode.Value)
			{
				base.Logger.LogInfo($"[SCENE-IMMEDIATE] {scene.name}: {num} cache-hit applies");
			}
		}
		catch (Exception ex)
		{
			base.Logger.LogWarning("[SCENE-IMMEDIATE] cache-apply failed: " + ex.Message);
		}
	}

	private void StartStartupWarmup()
	{
		_ = WarmupTextsAsync(StartupHotTexts, "ui");
		StartGameScriptWarmup();
		BeginSceneWarmupGeneration();
	}

	private void StartGameScriptWarmup()
	{
		if (Interlocked.Exchange(ref _gameScriptWarmupStarted, 1) != 0)
		{
			return;
		}
		_ = WarmupGameScriptTextsAsync();
	}

	private async Task WarmupGameScriptTextsAsync()
	{
		try
		{
			await Task.Delay(500);
			if (IsServerBackoffActive())
			{
				return;
			}
			List<string> texts = await RunBackground(() => CollectGameScriptWarmupTexts(MaxGameScriptWarmupTexts));
			if (texts.Count == 0)
			{
				return;
			}
			int warmed = 0;
			for (int i = 0; i < texts.Count; i += GameScriptWarmupBatchSize)
			{
				if (HasPendingClientTranslationWork())
				{
					await Task.Delay(300);
				}
				List<string> batch = texts.GetRange(i, Math.Min(GameScriptWarmupBatchSize, texts.Count - i));
				Dictionary<string, string> result = await WarmupTextsAsync(batch, "dialogue");
				warmed += result.Count;
				await Task.Delay(GameScriptWarmupPauseMs);
			}
			base.Logger.LogInfo($"[SCRIPT-PREFETCH] warmed {warmed}/{texts.Count} dialogue strings from game JSON");
		}
		catch (Exception ex)
		{
			base.Logger.LogWarning("[SCRIPT-PREFETCH] failed: " + (ex.InnerException?.Message ?? ex.Message));
		}
	}

	private List<string> CollectGameScriptWarmupTexts(int maxTexts)
	{
		List<string> output = new List<string>(Math.Min(maxTexts, 64));
		foreach (string path in GetGameScriptJsonCandidates())
		{
			if (output.Count >= maxTexts)
			{
				break;
			}
			try
			{
				JToken token = JToken.Parse(File.ReadAllText(path, Encoding.UTF8));
				ExtractGameScriptStrings(token, output, maxTexts);
			}
			catch (Exception reportedException)
			{
				ReportCaughtException(reportedException);
			}
		}
		return output.Distinct(StringComparer.Ordinal).Take(maxTexts).ToList();
	}

	private IEnumerable<string> GetGameScriptJsonCandidates()
	{
		string root = ResolveGameRoot();
		if (string.IsNullOrWhiteSpace(root) || !Directory.Exists(root))
		{
			yield break;
		}
		HashSet<string> yielded = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
		List<string> roots = new List<string> { root };
		try
		{
			roots.AddRange(Directory.GetDirectories(root, "*_Data", SearchOption.TopDirectoryOnly));
		}
		catch (Exception reportedException)
		{
			ReportCaughtException(reportedException);
		}
		foreach (string candidateRoot in roots)
		{
			if (string.IsNullOrWhiteSpace(candidateRoot) || !Directory.Exists(candidateRoot))
			{
				continue;
			}
			string[] exactNames = new string[4] { "dialogues.json", "dialogue.json", "dialogs.json", "script.json" };
			foreach (string name in exactNames)
			{
				string path = Path.Combine(candidateRoot, name);
				if (File.Exists(path) && yielded.Add(path))
				{
					yield return path;
				}
			}
			string[] patterns = new string[2] { "*dialog*.json", "*script*.json" };
			foreach (string pattern in patterns)
			{
				IEnumerable<string> files = Enumerable.Empty<string>();
				try
				{
					files = Directory.GetFiles(candidateRoot, pattern, SearchOption.AllDirectories);
				}
				catch (Exception reportedException)
				{
					ReportCaughtException(reportedException);
				}
				foreach (string path in files)
				{
					if (path.IndexOf("\\BepInEx\\", StringComparison.OrdinalIgnoreCase) >= 0 || path.IndexOf("\\Managed\\", StringComparison.OrdinalIgnoreCase) >= 0)
					{
						continue;
					}
					if (yielded.Add(path))
					{
						yield return path;
					}
				}
			}
		}
	}

	private void ExtractGameScriptStrings(JToken token, List<string> output, int maxTexts)
	{
		if (token == null || output.Count >= maxTexts)
		{
			return;
		}
		JValue value = token as JValue;
		if (value != null)
		{
			if (value.Type == JTokenType.String)
			{
				ExtractDeepPrefetchText(((object)value)?.ToString(), output, maxTexts);
			}
			return;
		}
		JObject obj = token as JObject;
		if (obj != null)
		{
			foreach (JProperty prop in obj.Properties())
			{
				if (output.Count >= maxTexts)
				{
					return;
				}
				ExtractGameScriptStrings(prop.Value, output, maxTexts);
			}
			return;
		}
		JArray array = token as JArray;
		if (array != null)
		{
			foreach (JToken item in array)
			{
				if (output.Count >= maxTexts)
				{
					return;
				}
				ExtractGameScriptStrings(item, output, maxTexts);
			}
		}
	}

	private int BeginSceneWarmupGeneration()
	{
		lock (_pendingLock)
		{
			_warmupRequestedSources.Clear();
		}
		int result = (_sceneCacheApplyGeneration = Interlocked.Increment(ref _sceneWarmupGeneration));
		_sceneCacheApplyPassesRemaining = SceneCacheApplyPasses;
		_nextSceneCacheApplyRealtime = Time.realtimeSinceStartup + SceneCacheApplyDelaySeconds;
		try
		{
			StartManagedCoroutine(SceneWarmupCoroutine(result));
		}
		catch (Exception reportedException)
		{
			ReportCaughtException(reportedException);
		}
		return result;
	}

	private void StartServerCacheSync()
	{
		if (_serverCachePreloadEnabled == null || !_serverCachePreloadEnabled.Value)
		{
			base.Logger.LogInfo("Full server cache preload disabled; visible text will use the local server cache on demand.");
			return;
		}
		if (ShouldDelayServerCacheSync())
		{
			base.Logger.LogInfo($"Server cache sync deferred: local cache ready ({GetCacheCount()} entries)");
			_ = DeferredServerCacheSyncAsync();
		}
		else
		{
			_ = LoadServerCacheFromApiAsync();
		}
	}

	private bool ShouldDelayServerCacheSync()
	{
		if (GetCacheCount() < FreshLocalCacheMinEntries)
		{
			return false;
		}
		try
		{
			string localCacheFilePath = GetLocalCacheFilePath();
			if (!File.Exists(localCacheFilePath))
			{
				return false;
			}
			return DateTime.UtcNow - File.GetLastWriteTimeUtc(localCacheFilePath) <= FreshLocalCacheMaxAge;
		}
		catch (Exception ex)
		{
			ReportCaughtException(ex);
			return false;
		}
	}

	private async Task DeferredServerCacheSyncAsync()
	{
		await Task.Delay((int)(DeferredServerCacheSyncDelaySeconds * 1000f));
		while (HasPendingClientTranslationWork())
		{
			await Task.Delay((int)(DeferredServerCacheSyncIdlePollSeconds * 1000f));
		}
		await LoadServerCacheFromApiAsync();
	}

	private bool HasPendingClientTranslationWork()
	{
		lock (_pendingLock)
		{
			return _batchFlushScheduled || _pendingBatchQueue.Count > 0 || _pendingApplyQueue.Count > 0 || _debouncedTextRequests.Count > 0 || _inProgress.Count > 0;
		}
	}

	private void LogVerbose(string message)
	{
		ConfigEntry<bool> debugMode = _debugMode;
		if (debugMode != null && debugMode.Value)
		{
			base.Logger.LogInfo(message);
		}
	}

	private void InstallPluginHostLifetimeHooks()
	{
		/* Some games explicitly purge persistent roots while replacing their bootstrap
		   scene. The shared server cannot repair a destroyed in-process adapter. Only the
		   plugin-owned root is protected, normal application quit is never suppressed, and
		   every blocked operation is recorded by LogPluginHostLifetimeBlock. */
		int patched = 0;
		HarmonyMethod prefix = new HarmonyMethod(typeof(DeepSeekTranslator), "PluginHostDestroyPrefix");
		try
		{
			MethodInfo destroy = AccessTools.Method(typeof(Object), "Destroy", new Type[1] { typeof(Object) });
			if (destroy != null)
			{
				_harmony.Patch(destroy, prefix);
				patched++;
			}
			MethodInfo destroyImmediate = AccessTools.Method(typeof(Object), "DestroyImmediate", new Type[1] { typeof(Object) });
			if (destroyImmediate != null)
			{
				_harmony.Patch(destroyImmediate, prefix);
				patched++;
			}
			base.Logger.LogInfo($"Plugin host lifetime hooks installed: {patched}");
		}
		catch (Exception ex)
		{
			ReportCaughtException(ex, "boundary=plugin-host-destroy-hook");
		}
	}

	private void InstallHooks()
	{
		/* Harmony hook 是文本进入管线的第一入口；扫描器只是漏网兜底...		   这里只挂 Unity Mono ...TMP/UGUI/Fungus，不参与 IL2CPP/XUnity...*/
		InstallPluginHostLifetimeHooks();
		Type type = AccessTools.TypeByName("TMPro.TMP_Text");
		if (type != null)
		{
			MethodInfo methodInfo = AccessTools.Method(type, "set_text", new Type[1] { typeof(string) });
			if (methodInfo != null)
			{
				_harmony.Patch(
					methodInfo,
					new HarmonyMethod(typeof(DeepSeekTranslator), "TMPSetTextPrefix"),
					new HarmonyMethod(typeof(DeepSeekTranslator), "TMPSetTextPostfix"),
					null,
					new HarmonyMethod(typeof(DeepSeekTranslator), "TMPSetTextFinalizer"),
					null);
				base.Logger.LogInfo("Hooked TMPro.set_text");
			}
			int num = 0;
			try
			{
				MethodInfo[] methods = type.GetMethods(BindingFlags.Instance | BindingFlags.Public | BindingFlags.NonPublic);
				foreach (MethodInfo methodInfo2 in methods)
				{
					if (methodInfo2.Name != "SetText")
					{
						continue;
					}
					ParameterInfo[] parameters = methodInfo2.GetParameters();
					if (parameters.Length == 0)
					{
						continue;
					}
					Type parameterType = parameters[0].ParameterType;
					if (parameterType != typeof(string) && parameterType != typeof(StringBuilder) && parameterType != typeof(char[]))
					{
						continue;
					}
					try
					{
					_harmony.Patch(
						methodInfo2,
						new HarmonyMethod(typeof(DeepSeekTranslator), "TMPSetTextAnyPrefix"),
						new HarmonyMethod(typeof(DeepSeekTranslator), "TMPSetTextAnyPostfix"),
						null,
						new HarmonyMethod(typeof(DeepSeekTranslator), "TMPSetTextAnyFinalizer"),
						null);
						base.Logger.LogInfo("Hooked TMPro.SetText(" + string.Join(",", parameters.Select((ParameterInfo p) => p.ParameterType.Name)) + ")");
						num++;
					}
					catch (Exception ex)
					{
						base.Logger.LogWarning("TMPro.SetText(" + string.Join(",", parameters.Select((ParameterInfo p) => p.ParameterType.Name)) + ") patch failed: " + ex.Message);
					}
				}
			}
			catch (Exception ex2)
			{
				base.Logger.LogWarning("TMPro SetText enumeration failed: " + ex2.Message);
			}
			base.Logger.LogInfo($"Total SetText overloads patched: {num}");
			MethodInfo methodInfo3 = AccessTools.PropertySetter(type, "font");
			if (methodInfo3 != null)
			{
				_harmony.Patch(methodInfo3, null, new HarmonyMethod(typeof(DeepSeekTranslator), "TMPFontPostfix"));
				base.Logger.LogInfo("Hooked TMPro.font");
			}
			TryPatchDeclaredTmpOnEnable(AccessTools.TypeByName("TMPro.TextMeshProUGUI"));
			TryPatchDeclaredTmpOnEnable(AccessTools.TypeByName("TMPro.TextMeshPro"));
			TryPatchDeclaredTmpRenderDiscovery(AccessTools.TypeByName("TMPro.TextMeshProUGUI"));
			TryPatchDeclaredTmpRenderDiscovery(AccessTools.TypeByName("TMPro.TextMeshPro"));
			TryPatchTmpUpdateManagerDiscovery();
		}
		else
		{
			base.Logger.LogWarning("TMPro type not found");
		}
		MethodInfo methodInfo4 = AccessTools.PropertySetter(typeof(Text), "text");
		if (methodInfo4 != null)
		{
			_harmony.Patch(methodInfo4, new HarmonyMethod(typeof(DeepSeekTranslator), "UGUITextPrefix"), new HarmonyMethod(typeof(DeepSeekTranslator), "UGUITextPostfix"));
			base.Logger.LogInfo("Hooked Text.set_text");
		}
		MethodInfo methodInfo5 = AccessTools.PropertySetter(typeof(Text), "font");
		if (methodInfo5 != null)
		{
			_harmony.Patch(methodInfo5, null, new HarmonyMethod(typeof(DeepSeekTranslator), "UGUIFontPostfix"));
			base.Logger.LogInfo("Hooked Text.font");
		}
		try
		{
			MethodInfo methodInfo6 = AccessTools.Method(typeof(GameObject), "SetActive", new Type[1] { typeof(bool) });
			if (methodInfo6 != null)
			{
				_harmony.Patch(methodInfo6, new HarmonyMethod(typeof(DeepSeekTranslator), "GameObjectSetActivePrefix"), new HarmonyMethod(typeof(DeepSeekTranslator), "GameObjectSetActivePostfix"));
				base.Logger.LogInfo("Hooked GameObject.SetActive");
			}
		}
		catch (Exception ex3)
		{
			base.Logger.LogWarning("GameObject.SetActive hook skipped: " + ex3.Message);
		}
		try
		{
			MethodInfo methodInfo7 = AccessTools.PropertySetter(typeof(CanvasGroup), "alpha");
			if (methodInfo7 != null)
			{
				_harmony.Patch(methodInfo7, null, new HarmonyMethod(typeof(DeepSeekTranslator), "CanvasGroupAlphaPostfix"));
				base.Logger.LogInfo("Hooked CanvasGroup.alpha");
			}
		}
		catch (Exception ex4)
		{
			base.Logger.LogWarning("CanvasGroup.alpha hook skipped: " + ex4.Message);
		}
		InstallImGuiHooks();
		InstallFungusHooks();
		InstallFramePumpHooks();
		base.Logger.LogInfo("All hooks installed");
	}

	private void TryPatchDeclaredTmpOnEnable(Type tmpType)
	{
		if (tmpType == null)
		{
			return;
		}
		try
		{
			MethodInfo methodInfo = AccessTools.Method(tmpType, "OnEnable");
			if (!(methodInfo == null))
			{
				if (methodInfo.DeclaringType != tmpType)
				{
					base.Logger.LogInfo("Skipped " + tmpType.FullName + ".OnEnable hook; inherited from " + methodInfo.DeclaringType?.FullName);
					return;
				}
				_harmony.Patch(methodInfo, null, new HarmonyMethod(typeof(DeepSeekTranslator), "TMPTextOnEnablePostfix"));
				base.Logger.LogInfo("Hooked " + tmpType.FullName + ".OnEnable");
			}
		}
		catch (Exception ex)
		{
			base.Logger.LogWarning(tmpType.FullName + ".OnEnable hook skipped: " + ex.Message);
		}
	}

	/* A stripped player can make both Resources and Object global enumeration return
	   empty arrays for live TMP objects that were created before plugin hooks. Canvas
	   rebuild is the retained renderer-owned boundary that still exposes the instance.
	   It only queues the normal local-first pipeline, cannot manufacture a successful
	   translation, and hook selection/failures remain visible in BepInEx diagnostics. */
	private void TryPatchDeclaredTmpRenderDiscovery(Type tmpType)
	{
		if (tmpType == null)
		{
			return;
		}
		try
		{
			MethodInfo methodInfo = AccessTools.Method(tmpType, "Rebuild", new Type[1] { typeof(CanvasUpdate) });
			if (methodInfo == null)
			{
				base.Logger.LogWarning(tmpType.FullName + ".Rebuild render-discovery hook skipped: overload not found");
				return;
			}
			if (methodInfo.DeclaringType != tmpType)
			{
				base.Logger.LogInfo("Skipped " + tmpType.FullName + ".Rebuild render-discovery hook; inherited from " + methodInfo.DeclaringType?.FullName);
				return;
			}
			_harmony.Patch(methodInfo, null, new HarmonyMethod(typeof(DeepSeekTranslator), "TMPTextRenderDiscoveryPostfix"));
			base.Logger.LogInfo("Hooked " + tmpType.FullName + ".Rebuild for render discovery");
		}
		catch (Exception ex)
		{
			base.Logger.LogWarning(tmpType.FullName + ".Rebuild render-discovery hook skipped: " + ex.Message);
		}
	}

	private void TryPatchTmpUpdateManagerDiscovery()
	{
		try
		{
			Type type = AccessTools.TypeByName("TMPro.TMP_UpdateManager");
			MethodInfo methodInfo = AccessTools.Method(type, "DoRebuilds", Type.EmptyTypes);
			if (methodInfo == null)
			{
				base.Logger.LogWarning("TMPro.TMP_UpdateManager.DoRebuilds registry-discovery hook skipped: method not found");
				return;
			}
			_harmony.Patch(methodInfo, null, new HarmonyMethod(typeof(DeepSeekTranslator), "TMPUpdateManagerDiscoveryPostfix"));
			base.Logger.LogInfo("Hooked TMPro.TMP_UpdateManager.DoRebuilds for registry discovery");
		}
		catch (Exception ex)
		{
			base.Logger.LogWarning("TMPro.TMP_UpdateManager.DoRebuilds registry-discovery hook skipped: " + ex.Message);
		}
	}

	private void InstallImGuiHooks()
	{
		PatchGuiStringControl("Label", new Type[3] { typeof(Rect), typeof(string), typeof(GUIStyle) }, "GUI.Label(Rect,String,GUIStyle)", "GUILabelPrefix");
		PatchGuiStringControl("Box", new Type[3] { typeof(Rect), typeof(string), typeof(GUIStyle) }, "GUI.Box(Rect,String,GUIStyle)", "GUIBoxPrefix");
		PatchGuiStringControl("Button", new Type[3] { typeof(Rect), typeof(string), typeof(GUIStyle) }, "GUI.Button(Rect,String,GUIStyle)", "GUIButtonPrefix");
		PatchGuiStringControl("Toggle", new Type[4] { typeof(Rect), typeof(bool), typeof(string), typeof(GUIStyle) }, "GUI.Toggle(Rect,Bool,String,GUIStyle)", "GUITogglePrefix");
		PatchGUILayoutLabel(new Type[2] { typeof(string), typeof(GUILayoutOption[]) }, "GUILayout.Label(String,Options)", "GUILayoutLabelPrefix");
		PatchGUILayoutLabel(new Type[3] { typeof(string), typeof(GUIStyle), typeof(GUILayoutOption[]) }, "GUILayout.Label(String,GUIStyle,Options)", "GUILayoutLabelStylePrefix");
		PatchGUILayoutLabel(new Type[2] { typeof(GUIContent), typeof(GUILayoutOption[]) }, "GUILayout.Label(GUIContent,Options)", "GUILayoutContentLabelPrefix");
		PatchGUILayoutLabel(new Type[3] { typeof(GUIContent), typeof(GUIStyle), typeof(GUILayoutOption[]) }, "GUILayout.Label(GUIContent,GUIStyle,Options)", "GUILayoutContentLabelStylePrefix");
		PatchGUILayoutStringControl("Button", new Type[2] { typeof(string), typeof(GUILayoutOption[]) }, "GUILayout.Button(String,Options)", "GUILayoutButtonPrefix");
		PatchGUILayoutStringControl("Button", new Type[3] { typeof(string), typeof(GUIStyle), typeof(GUILayoutOption[]) }, "GUILayout.Button(String,GUIStyle,Options)", "GUILayoutButtonStylePrefix");
		PatchGUILayoutStringControl("Button", new Type[2] { typeof(GUIContent), typeof(GUILayoutOption[]) }, "GUILayout.Button(GUIContent,Options)", "GUILayoutContentButtonPrefix");
		PatchGUILayoutStringControl("Button", new Type[3] { typeof(GUIContent), typeof(GUIStyle), typeof(GUILayoutOption[]) }, "GUILayout.Button(GUIContent,GUIStyle,Options)", "GUILayoutContentButtonStylePrefix");
		PatchGUILayoutStringControl("Toggle", new Type[3] { typeof(bool), typeof(string), typeof(GUILayoutOption[]) }, "GUILayout.Toggle(Bool,String,Options)", "GUILayoutTogglePrefix");
		PatchGUILayoutStringControl("Toggle", new Type[4] { typeof(bool), typeof(string), typeof(GUIStyle), typeof(GUILayoutOption[]) }, "GUILayout.Toggle(Bool,String,GUIStyle,Options)", "GUILayoutToggleStylePrefix");
		PatchGUILayoutStringControl("Toggle", new Type[3] { typeof(bool), typeof(GUIContent), typeof(GUILayoutOption[]) }, "GUILayout.Toggle(Bool,GUIContent,Options)", "GUILayoutContentTogglePrefix");
		PatchGUILayoutStringControl("Toggle", new Type[4] { typeof(bool), typeof(GUIContent), typeof(GUIStyle), typeof(GUILayoutOption[]) }, "GUILayout.Toggle(Bool,GUIContent,GUIStyle,Options)", "GUILayoutContentToggleStylePrefix");
		PatchGUILayoutStringControl("Box", new Type[2] { typeof(string), typeof(GUILayoutOption[]) }, "GUILayout.Box(String,Options)", "GUILayoutBoxPrefix");
		PatchGUILayoutStringControl("Box", new Type[3] { typeof(string), typeof(GUIStyle), typeof(GUILayoutOption[]) }, "GUILayout.Box(String,GUIStyle,Options)", "GUILayoutBoxStylePrefix");
		PatchGUILayoutStringControl("Box", new Type[2] { typeof(GUIContent), typeof(GUILayoutOption[]) }, "GUILayout.Box(GUIContent,Options)", "GUILayoutContentBoxPrefix");
		PatchGUILayoutStringControl("Box", new Type[3] { typeof(GUIContent), typeof(GUIStyle), typeof(GUILayoutOption[]) }, "GUILayout.Box(GUIContent,GUIStyle,Options)", "GUILayoutContentBoxStylePrefix");
		PatchGUILayoutStringControl("SelectionGrid", new Type[4] { typeof(int), typeof(string[]), typeof(int), typeof(GUILayoutOption[]) }, "GUILayout.SelectionGrid(Int,StringArray,Int,Options)", "GUILayoutSelectionGridPrefix");
		PatchGUILayoutStringControl("SelectionGrid", new Type[5] { typeof(int), typeof(string[]), typeof(int), typeof(GUIStyle), typeof(GUILayoutOption[]) }, "GUILayout.SelectionGrid(Int,StringArray,Int,GUIStyle,Options)", "GUILayoutSelectionGridStylePrefix");
	}

	private void PatchGuiStringControl(string methodName, Type[] parameterTypes, string signature, string prefixName)
	{
		try
		{
			MethodInfo methodInfo = AccessTools.Method(typeof(GUI), methodName, parameterTypes);
			if (methodInfo == null)
			{
				base.Logger.LogWarning(signature + " hook skipped: overload not found");
				return;
			}
			_harmony.Patch(methodInfo, new HarmonyMethod(typeof(DeepSeekTranslator), prefixName));
			base.Logger.LogInfo("Hooked " + signature);
		}
		catch (Exception ex)
		{
			base.Logger.LogWarning(signature + " hook skipped: " + ex.Message);
		}
	}

	private void PatchGUILayoutLabel(Type[] parameterTypes, string signature, string prefixName)
	{
		PatchGUILayoutStringControl("Label", parameterTypes, signature, prefixName);
	}

	private void PatchGUILayoutStringControl(string methodName, Type[] parameterTypes, string signature, string prefixName)
	{
		try
		{
			MethodInfo methodInfo = AccessTools.Method(typeof(GUILayout), methodName, parameterTypes);
			if (methodInfo == null)
			{
				base.Logger.LogWarning(signature + " hook skipped: overload not found");
				return;
			}
			_harmony.Patch(methodInfo, new HarmonyMethod(typeof(DeepSeekTranslator), prefixName));
			base.Logger.LogInfo("Hooked " + signature);
		}
		catch (Exception ex)
		{
			base.Logger.LogWarning(signature + " hook skipped: " + ex.Message);
		}
	}

	private void InstallFramePumpHooks()
	{
		int num = 0;
		HarmonyMethod postfix = new HarmonyMethod(typeof(DeepSeekTranslator), "FramePumpPostfix");
		string[] array = new string[3] { "deltaTime", "unscaledDeltaTime", "frameCount" };
		foreach (string text in array)
		{
			try
			{
				MethodInfo methodInfo = AccessTools.PropertyGetter(typeof(Time), text);
				if (methodInfo == null)
				{
					base.Logger.LogWarning("Frame pump getter not found: Time." + text);
					continue;
				}
				_harmony.Patch(methodInfo, null, postfix);
				num++;
			}
			catch (Exception ex)
			{
				base.Logger.LogWarning("Frame pump hook failed for Time." + text + ": " + ex.Message);
			}
		}
		base.Logger.LogInfo($"Frame pump hooks installed: {num}");
	}

	private void InstallFungusHooks()
	{
		int dialogueHooks = 0;
		int commandHooks = 0;
		int menuHooks = 0;
		Type type = AccessTools.TypeByName("Fungus.SayDialog");
		if (type != null)
		{
			MethodInfo[] array = (from m in AccessTools.GetDeclaredMethods(type)
				where m.Name == "Say"
				select m).ToArray();
			foreach (MethodInfo methodInfo in array)
			{
				ParameterInfo[] parameters = methodInfo.GetParameters();
				if (parameters.Length >= 1 && parameters[0].ParameterType == typeof(string))
				{
					_harmony.Patch(methodInfo, new HarmonyMethod(typeof(DeepSeekTranslator), "FungusSayPrefix"));
					dialogueHooks++;
				}
			}
		}
		Type type2 = AccessTools.TypeByName("Fungus.Say");
		if (type2 != null)
		{
			MethodInfo methodInfo2 = AccessTools.GetDeclaredMethods(type2).FirstOrDefault(method => method.Name == "OnEnter" && method.GetParameters().Length == 0);
			if (methodInfo2 != null)
			{
				_harmony.Patch(methodInfo2, null, new HarmonyMethod(typeof(DeepSeekTranslator), "FungusSayCommandOnEnterPostfix"));
				commandHooks++;
			}
		}
		Type type3 = AccessTools.TypeByName("Fungus.MenuDialog");
		if (type3 != null)
		{
			foreach (MethodInfo methodInfo3 in AccessTools.GetDeclaredMethods(type3))
			{
				ParameterInfo[] parameters = methodInfo3.GetParameters();
				if (methodInfo3.Name == "SetOptions" && parameters.Length >= 1 && parameters[0].ParameterType == typeof(List<string>))
				{
					_harmony.Patch(methodInfo3, new HarmonyMethod(typeof(DeepSeekTranslator), "FungusMenuPrefix"));
					menuHooks++;
				}
				else if (methodInfo3.Name == "AddOption" && parameters.Length >= 1 && parameters[0].ParameterType == typeof(string))
				{
					_harmony.Patch(methodInfo3, new HarmonyMethod(typeof(DeepSeekTranslator), "FungusMenuAddOptionPrefix"));
					menuHooks++;
				}
			}
		}
		if (dialogueHooks + commandHooks + menuHooks > 0)
		{
			base.Logger.LogInfo($"Fungus hooks installed: dialogue={dialogueHooks}, lookahead={commandHooks}, menu={menuHooks}");
		}
	}

	private void ObserveFirstAsyncQueue(object component, string originalText)
	{
		if (_asyncQueueLoggedCount >= 10 || string.IsNullOrWhiteSpace(originalText))
		{
			return;
		}
		bool flag;
		lock (_asyncQueueLogged)
		{
			flag = _asyncQueueLogged.Add(originalText);
		}
		if (!flag)
		{
			return;
		}
		int num = Interlocked.Increment(ref _asyncQueueLoggedCount);
		if (num > 10)
		{
			return;
		}
		try
		{
			string componentLogPath = GetComponentLogPath(component);
			string arg = ((originalText.Length > 80) ? (originalText.Substring(0, 80) + "…") : originalText);
			base.Logger.LogWarning($"[ASYNC-QUEUE #{num}] path={componentLogPath} text='{arg}'");
		}
		catch (Exception reportedException)
		{
			ReportCaughtException(reportedException);
		}
	}

	private void QueueImGuiTranslation(string text)
	{
		if (string.IsNullOrWhiteSpace(text) || ContainsCjk(text) || ShouldSkipText(text) || IsTranslationRetryCoolingDown(text))
		{
			return;
		}
		string key = NormalizeRequestText(text);
		if (LooksLikeTypewriterFragment(text))
		{
			float realtimeSinceStartup = Time.realtimeSinceStartup;
			lock (_pendingLock)
			{
				if (_imguiPending.Contains(key))
				{
					return;
				}
				if (_imguiSettleStartedAt.TryGetValue(key, out var value))
				{
					if (realtimeSinceStartup - value < TypewriterFragmentDebounceSeconds)
					{
						return;
					}
					_imguiSettleStartedAt.Remove(key);
				}
				else
				{
					if (_imguiSettleStartedAt.Count >= MaxImGuiPending)
					{
						string text2 = null;
						float num = float.MaxValue;
						foreach (KeyValuePair<string, float> item in _imguiSettleStartedAt)
						{
							if (item.Value < num)
							{
								num = item.Value;
								text2 = item.Key;
							}
						}
						if (text2 != null)
						{
							_imguiSettleStartedAt.Remove(text2);
						}
					}
					_imguiSettleStartedAt[key] = realtimeSinceStartup;
					return;
				}
			}
		}
		else
		{
			lock (_pendingLock)
			{
				_imguiSettleStartedAt.Remove(key);
			}
		}
		lock (_pendingLock)
		{
			if (_imguiPending.Contains(key) || _imguiPending.Count >= MaxImGuiPending)
			{
				return;
			}
			_imguiPending.Add(key);
		}
		ObserveFirstAsyncQueue(null, text);
		try
		{
			RequestSharedTranslation(text, GetRequestDomain(text), delegate
			{
				lock (_pendingLock)
				{
					_imguiPending.Remove(key);
				}
			});
		}
		catch (Exception ex)
		{
			lock (_pendingLock)
			{
				_imguiPending.Remove(key);
			}
			base.Logger.LogWarning("IMGUI translation queue failed for '" + PreviewForLog(text) + "': " + ex.Message);
		}
	}

	private void ApplyImGuiFont(GUIStyle style)
	{
		if (style == null || (Object)(object)_chineseFont == (Object)null)
		{
			return;
		}
		try
		{
			PropertyInfo fontProperty = GetPropertyQuiet(style.GetType(), "font");
			if (fontProperty == null || !fontProperty.CanRead || !fontProperty.CanWrite)
			{
				ReportGuiStyleVisualFallbackOnce();
				return;
			}
			Font currentFont = fontProperty.GetValue(style) as Font;
			if ((Object)(object)currentFont != (Object)(object)_chineseFont)
			{
				fontProperty.SetValue(style, _chineseFont);
			}
		}
		catch (Exception ex)
		{
			ReportCaughtException(ex, "renderer=IMGUI property=GUIStyle.font", "ApplyImGuiFont");
		}
	}

	private static bool TMPSetTextAnyPrefix(object __instance, object __originalMethod, object[] __args, out bool __state)
	{
		__state = false;
		if ((Object)(object)_instance == (Object)null || __args == null || __args.Length == 0)
		{
			return true;
		}
		Interlocked.Increment(ref _setTextAnyOverloadCount);
		Interlocked.Increment(ref _instance._hookCallCount);
		string text = null;
		object obj = __args[0];
		if (obj is string text2)
		{
			text = text2;
		}
		else if (obj is StringBuilder stringBuilder)
		{
			text = stringBuilder.ToString();
		}
		else if (obj is char[] value)
		{
			text = new string(value);
		}
		if (string.IsNullOrEmpty(text))
		{
			return true;
		}
		if (!_instance.CanHandleTmp())
		{
			return true;
		}
		if (obj is string)
		{
			string value2 = text;
			/* TMP SetText(string, ...) overloads do not consistently call the text
			   property setter. Own a separate boundary for this overload so both the
			   successful and exceptional paths can close the depth increment. */
			__state = true;
			bool result = TMPSetTextPrefix(__instance, ref value2);
			__args[0] = value2;
			return result;
		}
		if (_setTextAnyOverloadCount <= 5)
		{
			try
			{
				string componentLogPath = GetComponentLogPath(__instance);
				string text3 = ((text.Length > 60) ? (text.Substring(0, 60) + "…") : text);
				_instance.Logger.LogWarning($"[SETTEXT-OBSERVED #{_setTextAnyOverloadCount}] firstArgType={obj.GetType().Name} path={componentLogPath} text='{text3}'");
			}
			catch (Exception reportedException)
			{
				ReportCaughtException(reportedException);
			}
		}
		return true;
	}

	private static void TMPSetTextAnyPostfix(object __instance, object[] __args, bool __state)
	{
		if (!__state || (Object)(object)_instance == (Object)null)
		{
			return;
		}
		string value = (__args != null && __args.Length > 0) ? __args[0] as string : null;
		_instance.EndTmpExternalTextWrite(__instance, value);
	}

	/* External TMP SetText overloads may throw before their postfix. That game/TMP
	   exception cannot be repaired in the renderer or shared server. The finalizer
	   resets only translator-owned depth state, never suppresses the exception or
	   accepts a translation, and ReportCaughtException records the exact boundary. */
	private static void TMPSetTextAnyFinalizer(object __instance, Exception __exception, bool __state)
	{
		if (!__state || __exception == null || (Object)(object)_instance == (Object)null)
		{
			return;
		}
		_instance.ResetTmpExternalWriteDepthAfterSetterException(__instance, __exception, "tmp-SetText-overload-depth-reset");
	}

	private static bool TMPSetTextPrefix(object __instance, ref string value)
	{
		if ((Object)(object)_instance == (Object)null)
		{
			return true;
		}
		Interlocked.Increment(ref _instance._hookCallCount);
		if (!_instance.CanHandleTmp())
		{
			return true;
		}
		_instance.BeginTmpExternalTextWrite(__instance, value);
		RestoreTmpParagraphLayout(__instance, value);
		RestoreTmpShortLabelLayout(__instance, value);
		if (string.IsNullOrWhiteSpace(value))
		{
			_instance.RestoreTmpOverlay(__instance);
			return true;
		}
		int componentInstanceId = GetComponentInstanceId(__instance);
		if (ContainsCjk(value))
		{
			if (_instance.TryRepairMixedTranslatedText(value, out var originalText, out var repaired))
			{
				_instance.ApplyTMPFont(__instance);
				string repairedText = (ShouldPreserveRichTextForDisplayWithColor(originalText, repaired) ? PrepareTranslatedTextForComponent(__instance, repaired, originalText) : StripRichTextForPlainText(repaired));
				repairedText = _instance.NormalizeTmpPunctuationForMissingGlyphs(repairedText);
				bool repairedFontCoversText = _instance.EnsureTMPFontCoversText(__instance, repairedText);
				_instance.ApplyTmpOverlay(__instance, repairedText, originalText, !repairedFontCoversText);
				RevealTmpText(__instance, repairedText);
				value = repairedText;
				_instance.MarkProcessed(componentInstanceId, originalText);
				_instance.TryMarkAppliedCacheKeyForPersist(originalText, repaired);
				_instance.LogVerbose("[MIXED] repaired TMP text '" + originalText?.Substring(0, Math.Min(originalText?.Length ?? 0, 30)) + "'");
				return true;
			}
			_instance.ApplyTMPFont(__instance);
			string text = (ShouldPreserveRichTextForDisplayWithColor(value, value) ? PrepareTranslatedTextForComponent(__instance, value, value) : StripRichTextForPlainText(value));
			text = _instance.NormalizeTmpPunctuationForMissingGlyphs(text);
			bool tmpFontCoversText = _instance.EnsureTMPFontCoversText(__instance, text);
			_instance.ApplyTmpOverlay(__instance, text, value, !tmpFontCoversText);
			RevealTmpText(__instance, text);
			value = text;
			return true;
		}
		_instance.RestoreTmpOverlay(__instance);
		string rawText = value;
		_instance.ClearProcessedIfChanged(componentInstanceId, value);
		if (LooksLikeTypewriterFragment(value))
		{
			_instance.QueueDebouncedTextRequest(__instance, componentInstanceId, value, isTmp: true);
			return true;
		}
		string text2 = _instance.Translate(value);
		if (text2 != value)
		{
			string cachedText = text2;
			if (!_instance.CanHandleTmp())
			{
				_instance.ClearInProgress(componentInstanceId, value);
				return true;
			}
			text2 = (ShouldPreserveRichTextForDisplayWithColor(value, text2) ? PrepareTranslatedTextForComponent(__instance, text2, value) : StripRichTextForPlainText(text2));
			text2 = _instance.NormalizeTmpPunctuationForMissingGlyphs(text2);
			if (!_instance.CanTranslateTmp() && _instance.ShouldUseTmpOverlay())
			{
				if (!_instance.ApplyTmpOverlay(__instance, text2, value))
				{
					_instance.ClearInProgress(componentInstanceId, rawText);
					return true;
				}
				RevealTmpText(__instance, text2);
				_instance.MarkProcessed(componentInstanceId, rawText);
				_instance.TryMarkAppliedCacheKeyForPersist(rawText, cachedText);
				_instance.LogVerbose("[TMPro-overlay] sync translated '" + text2 + "'");
				return true;
			}
			_instance.ApplyTMPFont(__instance);
			bool tmpFontCoversText2 = _instance.EnsureTMPFontCoversText(__instance, text2);
			_instance.ApplyTmpOverlay(__instance, text2, value, !tmpFontCoversText2);
			RevealTmpText(__instance, text2);
			value = text2;
			_instance.MarkProcessed(componentInstanceId, rawText);
			_instance.TryMarkAppliedCacheKeyForPersist(rawText, cachedText);
			_instance.LogVerbose("[TMPro] sync translated '" + text2 + "'");
			return true;
		}
		if (ShouldSkipText(value))
		{
			return true;
		}
		_instance.QueueDebouncedTextRequest(__instance, componentInstanceId, value, isTmp: true);
		return true;
	}

	private static void TMPSetTextPostfix(object __instance, string value)
	{
		if ((Object)(object)_instance == (Object)null)
		{
			return;
		}
		_instance.EndTmpExternalTextWrite(__instance, value);
	}

	/* 原方法抛异常时 postfix 不会执行，pendingExternalWriteDepth 会永久卡住
	   并让所有周期校验跳过该组件；finalizer 兜底把深度归零。void 签名不改变
	   异常的传播，游戏仍收到自己的异常。 */
	private static void TMPSetTextFinalizer(object __instance, Exception __exception)
	{
		if (__exception == null || (Object)(object)_instance == (Object)null)
		{
			return;
		}
		_instance.ResetTmpExternalWriteDepthAfterSetterException(__instance, __exception, "tmp-set-text-depth-reset");
	}

	private static bool IsTranslatorOwnedTmpOverlay(Text component)
	{
		return (Object)(object)component != (Object)null &&
			(Object)(object)((Component)component).GetComponent<TmpOverlayMarker>() != (Object)null;
	}

	private static bool UGUITextPrefix(Text __instance, ref string value)
	{
		if ((Object)(object)_instance == (Object)null)
		{
			return true;
		}
		if (IsTranslatorOwnedTmpOverlay(__instance))
		{
			return true;
		}
		bool flag = ContainsCjk(value);
		if (!flag)
		{
			RestoreUGUIShortLabelLayoutCompatibility(__instance);
		}
		if (string.IsNullOrWhiteSpace(value))
		{
			return true;
		}
		int componentInstanceId = GetComponentInstanceId(__instance);
		if (flag)
		{
			if (_instance.TryRepairMixedTranslatedText(value, out var originalText, out var repaired))
			{
				if ((Object)(object)_instance._chineseFont != (Object)null && (Object)(object)__instance.font != (Object)(object)_instance._chineseFont)
				{
					__instance.font = _instance._chineseFont;
				}
				value = PrepareTranslatedTextForUGUIText(__instance, repaired, originalText);
				_instance.MarkProcessed(componentInstanceId, originalText);
				_instance.TryMarkAppliedCacheKeyForPersist(originalText, repaired);
				_instance.LogVerbose("[MIXED] repaired UGUI text '" + originalText?.Substring(0, Math.Min(originalText?.Length ?? 0, 30)) + "'");
				return true;
			}
			if ((Object)(object)_instance._chineseFont != (Object)null && (Object)(object)__instance.font != (Object)(object)_instance._chineseFont)
			{
				__instance.font = _instance._chineseFont;
			}
			value = PrepareTranslatedTextForUGUIText(__instance, value, value);
			return true;
		}
		string rawText = value;
		_instance.ClearProcessedIfChanged(componentInstanceId, value);
		if (LooksLikeTypewriterFragment(value))
		{
			_instance.QueueDebouncedTextRequest(__instance, componentInstanceId, value, isTmp: false);
			return true;
		}
		string text = _instance.Translate(value);
		if (text != value)
		{
			_instance.ApplyFont(__instance);
			value = PrepareTranslatedTextForUGUIText(__instance, text, rawText);
			_instance.MarkProcessed(componentInstanceId, rawText);
			_instance.TryMarkAppliedCacheKeyForPersist(rawText, text);
			_instance.LogVerbose("[Text] sync translated '" + text + "'");
			return true;
		}
		if (ShouldSkipText(value))
		{
			return true;
		}
		_instance.QueueDebouncedTextRequest(__instance, componentInstanceId, value, isTmp: false);
		return true;
	}

	private static void UGUITextPostfix(Text __instance)
	{
		if ((Object)(object)_instance == (Object)null || (Object)(object)__instance == (Object)null)
		{
			return;
		}
		if (IsTranslatorOwnedTmpOverlay(__instance))
		{
			return;
		}
		FinalizeUGUITextLayoutCompatibility(__instance);
	}

	private static void GUILabelPrefix(ref Rect position, ref string text, GUIStyle style)
	{
		if ((Object)(object)_instance == (Object)null || string.IsNullOrWhiteSpace(text))
		{
			return;
		}
		if (ContainsCjk(text))
		{
			_instance.ApplyImGuiFont(style);
			return;
		}
		string originalText = text;
		string translated = _instance.Translate(text);
		if (!string.Equals(translated, originalText, StringComparison.Ordinal))
		{
			text = PrepareTranslatedTextForString(translated, originalText);
			_instance.ApplyImGuiFont(style);
			PrepareImGuiTranslatedShortLabelLayout(ref position, style, text, originalText);
			_instance.TryMarkAppliedCacheKeyForPersist(originalText, translated);
			return;
		}
		if (!ShouldSkipText(text) && !_instance.IsTranslationRetryCoolingDown(text))
		{
			_instance.QueueImGuiTranslation(text);
		}
	}

	private static void GUILayoutLabelPrefix(ref string text, GUILayoutOption[] options)
	{
		TranslateGUILayoutLabelString(ref text, null);
	}

	private static void GUILayoutLabelStylePrefix(ref string text, GUIStyle style, GUILayoutOption[] options)
	{
		TranslateGUILayoutLabelString(ref text, style);
	}

	private static void GUILayoutContentLabelPrefix(GUIContent content, GUILayoutOption[] options)
	{
		TranslateGUILayoutLabelContent(content, null);
	}

	private static void GUILayoutContentLabelStylePrefix(GUIContent content, GUIStyle style, GUILayoutOption[] options)
	{
		TranslateGUILayoutLabelContent(content, style);
	}

	private static void GUIBoxPrefix(ref Rect position, ref string text, GUIStyle style)
	{
		TranslateImGuiString(ref text, style, false, ref position);
	}

	private static void GUIButtonPrefix(ref Rect position, ref string text, GUIStyle style)
	{
		TranslateImGuiString(ref text, style, false, ref position);
	}

	private static void GUITogglePrefix(ref Rect position, bool value, ref string text, GUIStyle style)
	{
		TranslateImGuiString(ref text, style, false, ref position);
	}

	private static void GUILayoutButtonPrefix(ref string text, GUILayoutOption[] options)
	{
		TranslateGUILayoutLabelString(ref text, GetEffectiveImGuiStyle(null, "button"));
	}

	private static void GUILayoutButtonStylePrefix(ref string text, GUIStyle style, GUILayoutOption[] options)
	{
		TranslateGUILayoutLabelString(ref text, GetEffectiveImGuiStyle(style, "button"));
	}

	private static void GUILayoutContentButtonPrefix(GUIContent content, GUILayoutOption[] options)
	{
		TranslateGUILayoutLabelContent(content, GetEffectiveImGuiStyle(null, "button"));
	}

	private static void GUILayoutContentButtonStylePrefix(GUIContent content, GUIStyle style, GUILayoutOption[] options)
	{
		TranslateGUILayoutLabelContent(content, GetEffectiveImGuiStyle(style, "button"));
	}

	private static void GUILayoutTogglePrefix(bool value, ref string text, GUILayoutOption[] options)
	{
		TranslateGUILayoutLabelString(ref text, GetEffectiveImGuiStyle(null, "toggle"));
	}

	private static void GUILayoutToggleStylePrefix(bool value, ref string text, GUIStyle style, GUILayoutOption[] options)
	{
		TranslateGUILayoutLabelString(ref text, GetEffectiveImGuiStyle(style, "toggle"));
	}

	private static void GUILayoutContentTogglePrefix(bool value, GUIContent content, GUILayoutOption[] options)
	{
		TranslateGUILayoutLabelContent(content, GetEffectiveImGuiStyle(null, "toggle"));
	}

	private static void GUILayoutContentToggleStylePrefix(bool value, GUIContent content, GUIStyle style, GUILayoutOption[] options)
	{
		TranslateGUILayoutLabelContent(content, GetEffectiveImGuiStyle(style, "toggle"));
	}

	private static void GUILayoutBoxPrefix(ref string text, GUILayoutOption[] options)
	{
		TranslateGUILayoutLabelString(ref text, GetEffectiveImGuiStyle(null, "box"));
	}

	private static void GUILayoutBoxStylePrefix(ref string text, GUIStyle style, GUILayoutOption[] options)
	{
		TranslateGUILayoutLabelString(ref text, GetEffectiveImGuiStyle(style, "box"));
	}

	private static void GUILayoutContentBoxPrefix(GUIContent content, GUILayoutOption[] options)
	{
		TranslateGUILayoutLabelContent(content, GetEffectiveImGuiStyle(null, "box"));
	}

	private static void GUILayoutContentBoxStylePrefix(GUIContent content, GUIStyle style, GUILayoutOption[] options)
	{
		TranslateGUILayoutLabelContent(content, GetEffectiveImGuiStyle(style, "box"));
	}

	private static void GUILayoutSelectionGridPrefix(int selected, ref string[] texts, int xCount, GUILayoutOption[] options)
	{
		TranslateImGuiStringArray(ref texts, GetEffectiveImGuiStyle(null, "button"));
	}

	private static void GUILayoutSelectionGridStylePrefix(int selected, ref string[] texts, int xCount, GUIStyle style, GUILayoutOption[] options)
	{
		TranslateImGuiStringArray(ref texts, GetEffectiveImGuiStyle(style, "button"));
	}

	private static void TranslateGUILayoutLabelString(ref string text, GUIStyle style)
	{
		Rect unused = default(Rect);
		TranslateImGuiString(ref text, GetEffectiveGUILayoutLabelStyle(style), false, ref unused);
	}

	private static void TranslateImGuiString(ref string text, GUIStyle style, bool adjustShortLabelRect, ref Rect position)
	{
		if ((Object)(object)_instance == (Object)null || string.IsNullOrWhiteSpace(text))
		{
			return;
		}
		GUIStyle effectiveStyle = GetEffectiveGUILayoutLabelStyle(style);
		if (ContainsCjk(text))
		{
			_instance.ApplyImGuiFont(effectiveStyle);
			return;
		}
		string originalText = text;
		string translated = _instance.Translate(text);
		if (!string.Equals(translated, originalText, StringComparison.Ordinal))
		{
			text = PrepareTranslatedTextForString(translated, originalText);
			_instance.ApplyImGuiFont(effectiveStyle);
			if (adjustShortLabelRect)
			{
				PrepareImGuiTranslatedShortLabelLayout(ref position, effectiveStyle, text, originalText);
			}
			_instance.TryMarkAppliedCacheKeyForPersist(originalText, translated);
			return;
		}
		if (!ShouldSkipText(text) && !_instance.IsTranslationRetryCoolingDown(text))
		{
			_instance.QueueImGuiTranslation(text);
		}
	}

	private static void TranslateGUILayoutLabelContent(GUIContent content, GUIStyle style)
	{
		if (content == null)
		{
			return;
		}
		string text = content.text;
		if ((Object)(object)_instance == (Object)null || string.IsNullOrWhiteSpace(text))
		{
			return;
		}
		GUIStyle effectiveStyle = GetEffectiveGUILayoutLabelStyle(style);
		if (ContainsCjk(text))
		{
			_instance.ApplyImGuiFont(effectiveStyle);
			return;
		}
		string originalText = text;
		string translated = _instance.Translate(text);
		if (!string.Equals(translated, originalText, StringComparison.Ordinal))
		{
			content.text = PrepareTranslatedTextForString(translated, originalText);
			_instance.ApplyImGuiFont(effectiveStyle);
			_instance.TryMarkAppliedCacheKeyForPersist(originalText, translated);
			return;
		}
		if (!ShouldSkipText(text) && !_instance.IsTranslationRetryCoolingDown(text))
		{
			_instance.QueueImGuiTranslation(text);
		}
	}

	private static GUIStyle GetEffectiveGUILayoutLabelStyle(GUIStyle style)
	{
		return GetEffectiveImGuiStyle(style, "label");
	}

	private static GUIStyle GetEffectiveImGuiStyle(GUIStyle style, string fallback)
	{
		if (style != null)
		{
			return style;
		}
		GUISkin skin = GUI.skin;
		if (skin == null)
		{
			return null;
		}
		switch (fallback)
		{
		case "button":
			return skin.button;
		case "toggle":
			return skin.toggle;
		case "box":
			return skin.box;
		default:
			return skin.label;
		}
	}

	private static void TranslateImGuiStringArray(ref string[] texts, GUIStyle style)
	{
		if (texts == null || texts.Length == 0 || (Object)(object)_instance == (Object)null)
		{
			return;
		}
		string[] translatedTexts = null;
		for (int i = 0; i < texts.Length; i++)
		{
			string item = texts[i];
			if (string.IsNullOrWhiteSpace(item))
			{
				continue;
			}
			string replacement = item;
			Rect unused = default(Rect);
			TranslateImGuiString(ref replacement, style, false, ref unused);
			if (!string.Equals(replacement, item, StringComparison.Ordinal))
			{
				if (translatedTexts == null)
				{
					translatedTexts = (string[])texts.Clone();
				}
				translatedTexts[i] = replacement;
			}
		}
		if (translatedTexts != null)
		{
			texts = translatedTexts;
		}
	}

	private static void PrepareImGuiTranslatedShortLabelLayout(ref Rect position, GUIStyle style, string translatedText, string sourceText)
	{
		if (style == null || string.IsNullOrEmpty(translatedText) || string.IsNullOrEmpty(sourceText))
		{
			return;
		}
		string visibleText = GetVisibleText(translatedText);
		string sourceVisibleText = GetVisibleText(sourceText);
		if (ContainsCjk(sourceVisibleText) || !ContainsCjk(visibleText) || visibleText.Length == 0 || visibleText.Length > 4 ||
			translatedText.IndexOf('\n') >= 0 || translatedText.IndexOf('\r') >= 0 || sourceText.IndexOf('\n') >= 0 || sourceText.IndexOf('\r') >= 0)
		{
			return;
		}
		try
		{
			GUIContent content = new GUIContent(translatedText);
			if (!TryMeasureImGuiContentSafe(style, content, out var requiredWidth, out var alignment))
			{
				return;
			}
			if (requiredWidth <= position.width + 0.5f)
			{
				return;
			}
			float originalWidth = position.width;
			float widthDelta = requiredWidth - originalWidth;
			switch (alignment)
			{
			case TextAnchor.UpperCenter:
			case TextAnchor.MiddleCenter:
			case TextAnchor.LowerCenter:
				position.x -= widthDelta * 0.5f;
				break;
			case TextAnchor.UpperRight:
			case TextAnchor.MiddleRight:
			case TextAnchor.LowerRight:
				position.x -= widthDelta;
				break;
			}
			position.width = requiredWidth;
			int count = Interlocked.Increment(ref _instance._imguiShortLabelWidthCount);
			if (count <= 3 || (count & (count - 1)) == 0)
			{
				_instance.Logger.LogInfo($"[IMGUI-LAYOUT] short-label width occurrence={count} originalWidth={originalWidth:0.0} requiredWidth={requiredWidth:0.0} visibleLength={visibleText.Length} sourceLength={sourceVisibleText.Length} alignment={alignment}");
			}
		}
		catch (Exception ex)
		{
			ReportCaughtException(ex, "renderer=IMGUI prepare-short-label-layout");
		}
	}

	private static bool ShouldProtectPluginHost(Object candidate)
	{
		DeepSeekTranslator runtime = _instance;
		GameObject hostRoot = _pluginHostRoot;
		if (ReferenceEquals(runtime, null) || runtime._applicationQuitting ||
			(Object)(object)candidate == (Object)null || (Object)(object)hostRoot == (Object)null)
		{
			return false;
		}
		if ((Object)(object)candidate == (Object)(object)hostRoot)
		{
			return true;
		}
		Component component = candidate as Component;
		return (Object)(object)component != (Object)null && (Object)(object)component.gameObject == (Object)(object)hostRoot;
	}

	private static bool PluginHostDestroyPrefix(Object obj)
	{
		if (!ShouldProtectPluginHost(obj))
		{
			return true;
		}
		LogPluginHostLifetimeBlock("Blocked explicit destruction of the BepInEx plugin host");
		return false;
	}

	private static bool GameObjectSetActivePrefix(GameObject __instance, bool value, ref bool __state)
	{
		__state = __instance.activeSelf;
		if (value || !ShouldProtectPluginHost(__instance))
		{
			return true;
		}
		LogPluginHostLifetimeBlock("Blocked explicit deactivation of the BepInEx plugin host");
		return false;
	}

	private static void LogPluginHostLifetimeBlock(string message)
	{
		int count = Interlocked.Increment(ref _pluginHostLifetimeBlockCount);
		if (count <= 3 || (count & (count - 1)) == 0)
		{
			DeepSeekTranslator runtime = _instance;
			if (!ReferenceEquals(runtime, null))
			{
				runtime.Logger.LogWarning($"[HOST-LIFETIME] {message}; occurrence={count}");
			}
		}
	}

	private static void GameObjectSetActivePostfix(GameObject __instance, bool value, bool __state)
	{
		if (!((Object)(object)_instance == (Object)null) && value && !__state && !((Object)(object)__instance == (Object)null) && _instance.LooksLikeUiGameObject(__instance))
		{
			_instance.QueueCachedTextsInHierarchy(__instance, MaxTargetedCacheQueuesPerActivation, allowRemoteFallback: true);
			_instance.RequestUiCacheApplyBurst();
		}
	}

	private static void CanvasGroupAlphaPostfix(CanvasGroup __instance, float value)
	{
		if (!((Object)(object)_instance == (Object)null) && !((Object)(object)__instance == (Object)null) && _instance.ShouldHandleCanvasGroupVisible(__instance, value))
		{
			_instance.QueueCachedTextsInHierarchy(((Component)__instance).gameObject, MaxTargetedCacheQueuesPerActivation, allowRemoteFallback: true);
		}
	}

	private static void TMPTextOnEnablePostfix(object __instance)
	{
		if (!((Object)(object)_instance == (Object)null) && __instance != null && _instance.CanHandleTmp())
		{
			Interlocked.Increment(ref _instance._textEnableHookCount);
			_instance.QueueCachedComponentTextIfAvailable(__instance, isTmp: true, allowRemoteFallback: false);
		}
	}

	private static void TMPTextRenderDiscoveryPostfix(object __instance)
	{
		if (!((Object)(object)_instance == (Object)null) && IsUnityObjectAlive(__instance) && _instance.CanHandleTmp())
		{
			_instance.QueueCachedComponentTextIfAvailable(__instance, isTmp: true, allowRemoteFallback: true);
		}
	}

	private static void TMPUpdateManagerDiscoveryPostfix(object __instance)
	{
		DeepSeekTranslator deepSeekTranslator = _instance;
		if (!ReferenceEquals(deepSeekTranslator, null))
		{
			Interlocked.Increment(ref deepSeekTranslator._tmpRegistryHookCount);
			deepSeekTranslator.DiscoverTmpUpdateManagerRegistry(__instance);
		}
	}

	private static void TMPFontPostfix(object __instance)
	{
		if (!((Object)(object)_instance == (Object)null) && IsUnityObjectAlive(__instance) && _instance.CanHandleTmp())
		{
			string currentText = GetCurrentComponentText(__instance, isTmp: true);
			if (ContainsCjk(currentText))
			{
				_instance.EnsureTMPFontCoversText(__instance, currentText);
				InvokeForceMeshUpdate(__instance, __instance.GetType());
				return;
			}
			_instance.ApplyTMPFont(__instance);
		}
	}

	private static void UGUIFontPostfix(Text __instance)
	{
		if (!((Object)(object)_instance == (Object)null) && !((Object)(object)_instance._chineseFont == (Object)null) && (Object)(object)__instance.font != (Object)(object)_instance._chineseFont)
		{
			__instance.font = _instance._chineseFont;
			_instance.LogVerbose("[FONT] Set Chinese font on Text");
		}
	}

	private static void FungusSayPrefix(object __instance, ref string text)
	{
		if (_instance == null)
		{
			return;
		}
		if (!string.IsNullOrWhiteSpace(text))
		{
			_instance.LogVerbose("[Fungus] Say='" + text + "'");
			string text2 = _instance.Translate(text);
			if (text2 != text)
			{
				text = PrepareTranslatedTextForString(text2, text);
				_instance.LogVerbose("[Fungus] Applied sync translation: '" + GetVisibleText(text) + "'");
			}
			else
			{
				_instance.QueueFungusAsyncTranslation(__instance, text);
			}
		}
	}

	private static void FungusSayCommandOnEnterPostfix(object __instance)
	{
		_instance?.QueueFungusLookahead(__instance);
	}

	private static void FungusMenuPrefix(ref List<string> options)
	{
		if (_instance == null || options == null)
		{
			return;
		}
		if (_instance._debugMode.Value)
		{
			_instance.Logger.LogInfo($"[Fungus] Menu options count={options.Count}");
		}
		for (int i = 0; i < options.Count; i++)
		{
			if (!string.IsNullOrWhiteSpace(options[i]))
			{
				string text = _instance.Translate(options[i]);
				if (text != options[i])
				{
					options[i] = text;
				}
			}
		}
	}

	private static void FungusMenuAddOptionPrefix(ref string text)
	{
		if (_instance == null || string.IsNullOrWhiteSpace(text))
		{
			return;
		}
		string translated = _instance.Translate(text);
		if (!string.Equals(translated, text, StringComparison.Ordinal))
		{
			text = PrepareTranslatedTextForString(translated, text);
		}
	}

	private void QueueFungusLookahead(object command)
	{
		if (_shuttingDown || !IsUnityObjectAlive(command))
		{
			return;
		}
		try
		{
			Type commandType = command.GetType();
			object parentBlock = AccessTools.Property(commandType, "ParentBlock")?.GetValue(command) ?? AccessTools.Field(commandType, "<ParentBlock>k__BackingField")?.GetValue(command);
			if (!IsUnityObjectAlive(parentBlock))
			{
				return;
			}
			IList commandList = AccessTools.Property(parentBlock.GetType(), "CommandList")?.GetValue(parentBlock) as IList ?? AccessTools.Field(parentBlock.GetType(), "commandList")?.GetValue(parentBlock) as IList;
			if (commandList == null || commandList.Count == 0)
			{
				return;
			}
			int commandIndex = -1;
			object indexValue = AccessTools.Property(commandType, "CommandIndex")?.GetValue(command) ?? AccessTools.Field(commandType, "<CommandIndex>k__BackingField")?.GetValue(command);
			if (indexValue is int value)
			{
				commandIndex = value;
			}
			if (commandIndex < 0 || commandIndex >= commandList.Count || !ReferenceEquals(commandList[commandIndex], command))
			{
				commandIndex = commandList.IndexOf(command);
			}
			if (commandIndex < 0)
			{
				return;
			}
			List<string> candidates = new List<string>();
			int end = Math.Min(commandList.Count, commandIndex + 1 + FungusLookaheadMaxCommands);
			for (int i = commandIndex + 1; i < end; i++)
			{
				object nextCommand = commandList[i];
				if (!IsUnityObjectAlive(nextCommand))
				{
					continue;
				}
				Type nextType = nextCommand.GetType();
				string candidate = null;
				if (string.Equals(nextType.FullName, "Fungus.Say", StringComparison.Ordinal))
				{
					candidate = AccessTools.Field(nextType, "storyText")?.GetValue(nextCommand) as string;
				}
				else if (string.Equals(nextType.FullName, "Fungus.Menu", StringComparison.Ordinal))
				{
					candidate = AccessTools.Field(nextType, "text")?.GetValue(nextCommand) as string;
				}
				if (!string.IsNullOrWhiteSpace(candidate))
				{
					candidates.Add(candidate);
				}
			}
			QueueFungusPrefetchTexts(candidates);
		}
		catch (Exception ex)
		{
			ReportCaughtException(ex, "adapter=Fungus;operation=collect-lookahead");
		}
	}

	private void QueueFungusPrefetchTexts(IEnumerable<string> candidates)
	{
		List<string> sourceKeys = new List<string>();
		List<string> requestTexts = new List<string>();
		HashSet<string> requestSeen = new HashSet<string>(StringComparer.Ordinal);
		foreach (string candidate in candidates ?? Enumerable.Empty<string>())
		{
			if (requestTexts.Count >= FungusPrefetchMaxRequestTexts || string.IsNullOrWhiteSpace(candidate) || ContainsCjk(candidate) || ShouldSkipText(candidate) || LooksLikeTypewriterFragment(candidate) || TryGetLocalTranslation(candidate, out _))
			{
				continue;
			}
			string sourceKey = NormalizeRequestText(candidate);
			if (string.IsNullOrWhiteSpace(sourceKey))
			{
				continue;
			}
			lock (_pendingLock)
			{
				if (_fungusPrefetchSeen.Count >= MaxFungusPrefetchSeen || !_fungusPrefetchSeen.Add(sourceKey))
				{
					continue;
				}
			}
			sourceKeys.Add(sourceKey);
			ProtectedTextPayload fullPayload = ProtectTextForTranslation(candidate);
			IReadOnlyList<UnityLongTextPart> parts = UnityLongTextPlanner.Plan(fullPayload.RequestText, LongTextParallelThreshold, LongTextTargetSegmentChars, MaxParallelLongTextSegments);
			if (parts != null && parts.Count > 1)
			{
				foreach (UnityLongTextPart part in parts)
				{
					if (requestTexts.Count >= FungusPrefetchMaxRequestTexts)
					{
						break;
					}
					string requestText = BuildLongTextSegmentPayload(part, fullPayload).RequestText;
					if (!string.IsNullOrWhiteSpace(requestText) && !IsTokenDominatedFragment(requestText) && requestSeen.Add(requestText))
					{
						requestTexts.Add(requestText);
					}
				}
			}
			else if (!IsTokenDominatedFragment(fullPayload.RequestText) && requestSeen.Add(fullPayload.RequestText))
			{
				requestTexts.Add(fullPayload.RequestText);
			}
		}
		if (requestTexts.Count == 0)
		{
			ReleaseFungusPrefetchKeys(sourceKeys);
			return;
		}
		_ = PrefetchServerTextsAsync(requestTexts, sourceKeys, "dialogue");
	}

	private async Task PrefetchServerTextsAsync(List<string> requestTexts, List<string> sourceKeys, string domain)
	{
		/* The optional lookahead can fail when the local server is restarting or an
		   older server lacks /prefetch. That boundary cannot be repaired inside the
		   renderer. It never marks a translation successful or suppresses the normal
		   visible-text request; the adapter and operation are recorded below. */
		if (_shuttingDown || requestTexts == null || requestTexts.Count == 0 || IsServerBackoffActive())
		{
			ReleaseFungusPrefetchKeys(sourceKeys);
			return;
		}
		try
		{
			string body = BuildBatchPayload(requestTexts, domain);
			string serverUrl = _serverUrl.Value;
			string response = await RunBackground(() => HttpPost(serverUrl + "/prefetch", body, 3000));
			if (string.IsNullOrWhiteSpace(response))
			{
				throw new IOException("The local server returned an empty prefetch response.");
			}
			NoteServerRequestSucceeded();
			LogVerbose($"[Fungus] queued {requestTexts.Count} protected lookahead texts");
		}
		catch (Exception ex)
		{
			ReleaseFungusPrefetchKeys(sourceKeys);
			ReportCaughtException(ex, $"adapter=Fungus;operation=prefetch;count={requestTexts.Count}");
		}
	}

	private void ReleaseFungusPrefetchKeys(IEnumerable<string> sourceKeys)
	{
		lock (_pendingLock)
		{
			foreach (string sourceKey in sourceKeys ?? Enumerable.Empty<string>())
			{
				_fungusPrefetchSeen.Remove(sourceKey);
			}
		}
	}

	private object ResolveFungusStoryComponent(object sayDialogInstance, out bool isTmp)
	{
		isTmp = false;
		if (!IsUnityObjectAlive(sayDialogInstance))
		{
			return null;
		}
		try
		{
			Type type = sayDialogInstance.GetType();
			object obj = AccessTools.Field(type, "storyTextAdapter")?.GetValue(sayDialogInstance);
			if (obj != null)
			{
				string[] array = new string[4] { "tmpro", "textUI", "textMesh", "textComponent" };
				foreach (string name in array)
				{
					object obj2 = AccessTools.Field(obj.GetType(), name)?.GetValue(obj);
					if (obj2 != null)
					{
						string fullName = obj2.GetType().FullName;
						isTmp = fullName != null && fullName.IndexOf("TMPro.", StringComparison.Ordinal) >= 0;
						return obj2;
					}
				}
				if (AccessTools.Property(obj.GetType(), "Text") != null)
				{
					object obj3 = AccessTools.Field(obj.GetType(), "textComponent")?.GetValue(obj);
					if (obj3 != null)
					{
						string fullName2 = obj3.GetType().FullName;
						isTmp = fullName2 != null && fullName2.IndexOf("TMPro.", StringComparison.Ordinal) >= 0;
						return obj3;
					}
				}
			}
			object obj4 = AccessTools.Field(type, "storyText")?.GetValue(sayDialogInstance);
			if (obj4 != null)
			{
				return obj4;
			}
		}
		catch (Exception ex)
		{
			ReportCaughtException(ex, "adapter=Fungus");
		}
		return null;
	}

	private void QueueFungusAsyncTranslation(object sayDialogInstance, string originalText)
	{
		if (string.IsNullOrWhiteSpace(originalText))
		{
			return;
		}
		bool isTmp;
		object component = ResolveFungusStoryComponent(sayDialogInstance, out isTmp);
		if (IsUnityObjectAlive(component) && (!isTmp || CanHandleTmp()))
		{
			int componentInstanceId = GetComponentInstanceId(component);
			if (TryMarkInProgress(componentInstanceId, originalText))
			{
				LogVerbose($"[Fungus] Queue async translation for id={componentInstanceId}");
				bool preserveColorTags = ContainsColorRichTextTag(originalText);
				ScheduleAsyncApply(component, componentInstanceId, originalText, isTmp, preserveRichText: preserveColorTags);
			}
		}
	}

	private void ApplyTranslation(object component, string translated, int instanceId, bool isTmp, bool preserveRichText = true, string originalText = null)
	{
		if (component == null)
		{
			return;
		}
		lock (_pendingLock)
		{
			_inProgress.Remove(instanceId);
		}
		try
		{
			if (!(component is Text) && !string.IsNullOrEmpty(translated))
			{
				RescueStrandedAlpha(component);
			}
			Text val = (Text)((component is Text) ? component : null);
				if (val != null)
				{
					ApplyFont(component);
					val.text = PrepareTranslatedTextForUGUIText(val, translated, originalText ?? translated, preserveRichText);
					TryMarkAppliedCacheKeyForPersist(originalText ?? translated, translated);
					return;
				}
			Type type = component.GetType();
			if (!CanHandleTmp())
			{
				return;
			}
			PropertyInfo propertyInfo = AccessTools.Property(type, "text");
			string sourceForFormatting = originalText ?? translated;
			translated = ((preserveRichText && ShouldPreserveRichTextForDisplayWithColor(sourceForFormatting, translated)) ? PrepareTranslatedTextForComponent(component, translated, sourceForFormatting) : StripRichTextForPlainText(translated));
			translated = NormalizeTmpPunctuationForMissingGlyphs(translated);
			if (!CanTranslateTmp())
			{
				if (!ApplyTmpOverlay(component, translated, sourceForFormatting))
				{
					return;
				}
				RevealTmpText(component, translated);
				TryMarkAppliedCacheKeyForPersist(sourceForFormatting, translated);
				return;
			}
			LogFirstWrites(component, type, translated);
			bool tmpFontCoversText = EnsureTMPFontCoversText(component, translated);
			propertyInfo?.SetValue(component, translated);
			RevealTmpText(component, translated);
			ApplyTmpOverlay(component, translated, sourceForFormatting, !tmpFontCoversText);
			InvokeForceMeshUpdate(component, type);
			if (!PostWriteHasMissingGlyph(component, translated))
			{
				return;
			}
			Interlocked.Increment(ref _glyphRetryCount);
			ApplyTmpOverlay(component, translated, sourceForFormatting, force: true);
		}
		catch (Exception ex)
		{
			base.Logger.LogError("ApplyTranslation failed: " + ex.Message);
		}
	}

	private void RescueStrandedAlpha(object tmpComponent)
	{
		try
		{
			Component overlayOwner = (Component)((tmpComponent is Component) ? tmpComponent : null);
			TmpOverlayState overlayState = overlayOwner?.GetComponent<TmpOverlayState>();
			if ((Object)(object)overlayState?.overlayText != (Object)null && ((Behaviour)overlayState.overlayText).enabled)
			{
				return;
			}
			Color tmpColor = GetTmpColor(tmpComponent);
			float num = TryReadColorAlpha(tmpComponent, "faceColor");
			float num2 = TryReadColorAlpha(tmpComponent, "outlineColor");
			float num3 = 1f;
			Component val = (Component)((tmpComponent is Component) ? tmpComponent : null);
			if (val != null && (Object)(object)val != (Object)null)
			{
				CanvasRenderer component = val.GetComponent<CanvasRenderer>();
				if ((Object)(object)component != (Object)null)
				{
					num3 = GetCanvasRendererAlphaSafe(component, 1f);
				}
			}
			if (!(tmpColor.a < 0.01f) && (float.IsNaN(num) || !(num < 0.01f)) && (float.IsNaN(num2) || !(num2 < 0.01f)) && !(num3 < 0.01f))
			{
				return;
			}
			if (HasInactiveCanvasGroupAncestor(tmpComponent))
			{
				Interlocked.Increment(ref _canvasGroupHiddenCount);
				if (_debugMode != null && _debugMode.Value)
				{
					base.Logger.LogInfo("[ALPHA-RESCUE] skipped (CanvasGroup hidden ancestor) on " + GetComponentLogPath(tmpComponent));
				}
				return;
			}
			tmpColor.a = 1f;
			SetTmpColor(tmpComponent, tmpColor);
			SetTmpAlpha(tmpComponent, 1f);
			Interlocked.Increment(ref _alphaRescuedCount);
			if (_debugMode != null && _debugMode.Value)
			{
				base.Logger.LogInfo($"[ALPHA-RESCUE] color.a={tmpColor.a:0.00} faceA={num:0.00} outlineA={num2:0.00} canvasA={num3:0.00} on {GetComponentLogPath(tmpComponent)}");
			}
		}
		catch (Exception reportedException)
		{
			ReportCaughtException(reportedException);
		}
	}

	private static bool HasInactiveCanvasGroupAncestor(object tmpComponent)
	{
		Component val = (Component)((tmpComponent is Component) ? tmpComponent : null);
		if (val == null || (Object)(object)val == (Object)null)
		{
			return false;
		}
		try
		{
			Transform val2 = val.transform;
			while ((Object)(object)val2 != (Object)null)
			{
				CanvasGroup[] components = ((Component)val2).GetComponents<CanvasGroup>();
				if (components != null)
				{
					foreach (CanvasGroup val3 in components)
					{
						if ((Object)(object)val3 != (Object)null && val3.alpha < 0.01f)
						{
							return true;
						}
					}
				}
				val2 = val2.parent;
			}
		}
		catch (Exception reportedException)
		{
			ReportCaughtException(reportedException);
		}
		return false;
	}

	private static float TryReadColorAlpha(object tmpComponent, string propName)
	{
		try
		{
			PropertyInfo propertyInfo = AccessTools.Property(tmpComponent?.GetType(), propName);
			if (propertyInfo == null || !propertyInfo.CanRead)
			{
				return float.NaN;
			}
			if (propertyInfo.GetValue(tmpComponent) is Color val)
			{
				return val.a;
			}
		}
		catch (Exception reportedException)
		{
			ReportCaughtException(reportedException);
		}
		return float.NaN;
	}

	private static bool PostWriteHasMissingGlyph(object tmpComponent, string translated)
	{
		if (tmpComponent == null || string.IsNullOrEmpty(translated))
		{
			return false;
		}
		try
		{
			object obj = GetPropertyQuiet(tmpComponent.GetType(), "textInfo")?.GetValue(tmpComponent);
			if (obj == null)
			{
				return false;
			}
			Type type = obj.GetType();
			PropertyInfo propertyQuiet = GetPropertyQuiet(type, "characterCount");
			int num = ((propertyQuiet != null) ? Convert.ToInt32(propertyQuiet.GetValue(obj)) : 0);
			if (num <= 0)
			{
				return false;
			}
			if (!(GetFieldQuiet(type, "characterInfo")?.GetValue(obj) is IList list))
			{
				return false;
			}
			for (int i = 0; i < num && i < list.Count; i++)
			{
				object obj2 = list[i];
				if (obj2 == null)
				{
					continue;
				}
				Type type2 = obj2.GetType();
				FieldInfo fieldQuiet = GetFieldQuiet(type2, "isVisible");
				if (fieldQuiet != null && Convert.ToBoolean(fieldQuiet.GetValue(obj2)))
				{
					FieldInfo fieldQuiet2 = GetFieldQuiet(type2, "character");
					if (((fieldQuiet2 != null) ? Convert.ToChar(fieldQuiet2.GetValue(obj2)) : '\0') >= '\u0080' && GetFieldQuiet(type2, "fontAsset")?.GetValue(obj2) == null)
					{
						return true;
					}
				}
			}
		}
		catch (Exception reportedException)
		{
			ReportCaughtException(reportedException);
		}
		return false;
	}

	private string Translate(string text)
	{
		/* 同步路径只查内存 glossary/cache，绝不发网络请求。未命中原样返回...		   setter hook 随后把文本放入去...批量异步管线...*/
		if (!TryGetLocalTranslation(text, out var translated))
		{
			return text;
		}
		return translated;
	}

	/* 疑似打字机片段不再被管线永久拒绝；分类只延长去抖等待时间...	   文本在等待窗内不再变化就视为最终文本，因此无句末标点的完整对白仍可翻译...*/
	private bool TryGetLocalTranslation(string text, out string translated)
	{
		translated = text;
		if (string.IsNullOrWhiteSpace(text))
		{
			return false;
		}
		string text2 = NormalizeRequestText(text);
		lock (_cache)
		{
			if (_glossary.TryGetValue(text, out var value))
			{
				value = SanitizeTranslationArtifacts(value);
				if (IsAcceptableTranslation(text, value))
				{
					translated = value;
					return true;
				}
			}
			if (!string.Equals(text2, text, StringComparison.Ordinal) && _glossary.TryGetValue(text2, out var value2))
			{
				value2 = SanitizeTranslationArtifacts(value2);
				if (IsAcceptableTranslation(text, value2))
				{
					translated = value2;
					return true;
				}
			}
			if (ContainsCjk(text) || ShouldSkipText(text))
			{
				return false;
			}
			if (_cache.TryGetValue(text, out var value3))
			{
				value3 = SanitizeTranslationArtifacts(value3);
				if (IsAcceptableTranslation(text, value3))
				{
					translated = value3;
					return true;
				}
			}
			if (!string.Equals(text2, text, StringComparison.Ordinal) && _cache.TryGetValue(text2, out var value4))
			{
				value4 = SanitizeTranslationArtifacts(value4);
				if (IsAcceptableTranslation(text, value4))
				{
					translated = value4;
					return true;
				}
			}
			if (TryTranslateQuantityListFromLocalTerms(text, out var quantityListTranslation))
			{
				translated = quantityListTranslation;
				return true;
			}
		}
		return false;
	}

	private bool TryTranslateQuantityListFromLocalTerms(string text, out string translated)
	{
		translated = null;
		if (string.IsNullOrWhiteSpace(text) || ContainsCjk(text))
		{
			return false;
		}
		string normalized = text.Replace("\r\n", "\n").Replace('\r', '\n');
		string[] lines = normalized.Split('\n');
		if (lines.Length < 2)
		{
			return false;
		}
		bool sawQuantityLine = false;
		StringBuilder stringBuilder = new StringBuilder(normalized.Length + 16);
		for (int i = 0; i < lines.Length; i++)
		{
			string line = lines[i];
			if (i > 0)
			{
				stringBuilder.Append('\n');
			}
			if (string.IsNullOrWhiteSpace(line))
			{
				continue;
			}
			if (!sawQuantityLine && InventoryMoneyLineRegex.IsMatch(line))
			{
				stringBuilder.Append(line.Trim());
				continue;
			}
			Match match = InventoryQuantityLineRegex.Match(line);
			if (!match.Success)
			{
				return false;
			}
			string itemName = match.Groups["name"].Value.Trim();
			if (!TryTranslateInventoryTerm(itemName, out var translatedName))
			{
				return false;
			}
			sawQuantityLine = true;
			stringBuilder.Append(translatedName);
			stringBuilder.Append(" x ");
			stringBuilder.Append(match.Groups["count"].Value);
		}
		if (!sawQuantityLine)
		{
			return false;
		}
		string candidate = stringBuilder.ToString();
		if (!IsAcceptableTranslation(text, candidate))
		{
			return false;
		}
		translated = candidate;
		return true;
	}

	private bool TryTranslateInventoryTerm(string term, out string translated)
	{
		translated = null;
		foreach (string candidate in GetInventoryTermCandidates(term))
		{
			if (TryGetDirectLocalTermTranslation(candidate, out translated))
			{
				return true;
			}
			if (TryInferTermTranslationFromCachedQuantityLists(candidate, out translated))
			{
				return true;
			}
			if (TryInferTermTranslationFromRichTextSpans(candidate, out translated))
			{
				return true;
			}
		}
		return false;
	}

	private bool TryGetDirectLocalTermTranslation(string term, out string translated)
	{
		translated = null;
		if (string.IsNullOrWhiteSpace(term))
		{
			return false;
		}
		string normalized = NormalizeRequestText(term);
		if (_glossary.TryGetValue(term, out var glossaryValue) || _glossary.TryGetValue(normalized, out glossaryValue) || _cache.TryGetValue(term, out glossaryValue) || _cache.TryGetValue(normalized, out glossaryValue))
		{
			glossaryValue = SanitizeTranslationArtifacts(glossaryValue);
			string visible = GetVisibleText(glossaryValue);
			if (ContainsCjk(visible) && !LooksLikeMixedTranslationResidue(glossaryValue))
			{
				translated = visible;
				return true;
			}
		}
		return false;
	}

	private bool TryInferTermTranslationFromCachedQuantityLists(string term, out string translated)
	{
		translated = null;
		string normalizedTerm = NormalizeInventoryTerm(term);
		if (string.IsNullOrWhiteSpace(normalizedTerm))
		{
			return false;
		}
		foreach (KeyValuePair<string, string> pair in _cache)
		{
			string[] sourceLines = pair.Key.Replace("\r\n", "\n").Replace('\r', '\n').Split('\n');
			string[] translatedLines = pair.Value.Replace("\r\n", "\n").Replace('\r', '\n').Split('\n');
			int count = Math.Min(sourceLines.Length, translatedLines.Length);
			for (int i = 0; i < count; i++)
			{
				Match source = InventoryQuantityLineRegex.Match(sourceLines[i]);
				if (!source.Success || !string.Equals(NormalizeInventoryTerm(source.Groups["name"].Value), normalizedTerm, StringComparison.OrdinalIgnoreCase))
				{
					continue;
				}
				Match target = InventoryQuantityLineRegex.Match(translatedLines[i]);
				if (!target.Success)
				{
					continue;
				}
				string candidate = GetVisibleText(target.Groups["name"].Value);
				if (ContainsCjk(candidate))
				{
					translated = candidate;
					return true;
				}
			}
		}
		return false;
	}

	private bool TryInferTermTranslationFromRichTextSpans(string term, out string translated)
	{
		translated = null;
		string normalizedTerm = NormalizeInventoryTerm(term);
		if (string.IsNullOrWhiteSpace(normalizedTerm))
		{
			return false;
		}
		foreach (KeyValuePair<string, string> pair in _cache)
		{
			MatchCollection sourceSpans = ColoredRichTextSpanRegex.Matches(pair.Key);
			if (sourceSpans.Count == 0)
			{
				continue;
			}
			MatchCollection targetSpans = ColoredRichTextSpanRegex.Matches(pair.Value);
			int count = Math.Min(sourceSpans.Count, targetSpans.Count);
			for (int i = 0; i < count; i++)
			{
				string sourceText = StripLeadingInventoryCount(GetVisibleText(sourceSpans[i].Groups["text"].Value));
				if (!string.Equals(NormalizeInventoryTerm(sourceText), normalizedTerm, StringComparison.OrdinalIgnoreCase))
				{
					continue;
				}
				string candidate = GetVisibleText(targetSpans[i].Groups["text"].Value);
				if (ContainsCjk(candidate))
				{
					translated = candidate;
					return true;
				}
			}
		}
		return false;
	}

	private static IEnumerable<string> GetInventoryTermCandidates(string term)
	{
		string normalized = NormalizeRequestText(term);
		if (string.IsNullOrWhiteSpace(normalized))
		{
			yield break;
		}
		yield return normalized;
		string singular = SingularizeInventoryTerm(normalized);
		if (!string.Equals(singular, normalized, StringComparison.OrdinalIgnoreCase))
		{
			yield return singular;
		}
	}

	private static string SingularizeInventoryTerm(string term)
	{
		if (string.IsNullOrWhiteSpace(term))
		{
			return term;
		}
		if (term.EndsWith("ies", StringComparison.OrdinalIgnoreCase) && term.Length > 4)
		{
			return term.Substring(0, term.Length - 3) + "y";
		}
		if (term.EndsWith("es", StringComparison.OrdinalIgnoreCase) && term.Length > 3)
		{
			string stem = term.Substring(0, term.Length - 2);
			if (stem.EndsWith("ch", StringComparison.OrdinalIgnoreCase) || stem.EndsWith("sh", StringComparison.OrdinalIgnoreCase) || stem.EndsWith("x", StringComparison.OrdinalIgnoreCase) || stem.EndsWith("s", StringComparison.OrdinalIgnoreCase))
			{
				return stem;
			}
		}
		if (term.EndsWith("s", StringComparison.OrdinalIgnoreCase) && !term.EndsWith("ss", StringComparison.OrdinalIgnoreCase) && term.Length > 3)
		{
			return term.Substring(0, term.Length - 1);
		}
		return term;
	}

	private static string StripLeadingInventoryCount(string text)
	{
		if (string.IsNullOrWhiteSpace(text))
		{
			return text;
		}
		return Regex.Replace(text.Trim(), "^\\d+\\s+", string.Empty);
	}

	private static string NormalizeInventoryTerm(string term)
	{
		if (string.IsNullOrWhiteSpace(term))
		{
			return string.Empty;
		}
		return SingularizeInventoryTerm(StripLeadingInventoryCount(NormalizeRequestText(term))).ToLowerInvariant();
	}

	private bool TryRepairMixedTranslatedText(string mixedText, out string originalText, out string translated)
	{
		originalText = null;
		translated = null;
		if (!LooksLikeMixedTranslationResidue(mixedText))
		{
			return false;
		}
		string latinTail = GetMixedResidueLatinTail(mixedText);
		string cjkPrefix = GetLeadingCjkSignature(mixedText);
		if (string.IsNullOrWhiteSpace(latinTail) || string.IsNullOrEmpty(cjkPrefix))
		{
			return false;
		}
		string mixedKey = NormalizeRequestText(GetVisibleText(mixedText));
		lock (_cache)
		{
			if (_mixedRepairTranslations.TryGetValue(mixedKey, out var cachedTranslation) && _mixedRepairOriginals.TryGetValue(mixedKey, out var cachedOriginal))
			{
				originalText = cachedOriginal;
				translated = cachedTranslation;
				return true;
			}
			if (_mixedRepairMisses.Contains(mixedKey))
			{
				return false;
			}
			if (TryRepairMixedTranslatedTextFromMap(_cache, latinTail, cjkPrefix, out originalText, out translated))
			{
				RememberMixedRepair(mixedKey, originalText, translated);
				return true;
			}
			if (TryRepairMixedTranslatedTextFromMap(_glossary, latinTail, cjkPrefix, out originalText, out translated))
			{
				RememberMixedRepair(mixedKey, originalText, translated);
				return true;
			}
			RememberMixedRepairMiss(mixedKey);
			return false;
		}
	}

	private void RememberMixedRepair(string mixedKey, string originalText, string translated)
	{
		if (string.IsNullOrWhiteSpace(mixedKey) || string.IsNullOrWhiteSpace(originalText) || string.IsNullOrWhiteSpace(translated))
		{
			return;
		}
		if (_mixedRepairTranslations.Count >= MaxMixedRepairMemoized)
		{
			_mixedRepairOriginals.Clear();
			_mixedRepairTranslations.Clear();
			_mixedRepairMisses.Clear();
		}
		_mixedRepairMisses.Remove(mixedKey);
		_mixedRepairOriginals[mixedKey] = originalText;
		_mixedRepairTranslations[mixedKey] = translated;
	}

	private void RememberMixedRepairMiss(string mixedKey)
	{
		if (string.IsNullOrWhiteSpace(mixedKey))
		{
			return;
		}
		if (_mixedRepairMisses.Count >= MaxMixedRepairMemoized)
		{
			_mixedRepairOriginals.Clear();
			_mixedRepairTranslations.Clear();
			_mixedRepairMisses.Clear();
		}
		_mixedRepairMisses.Add(mixedKey);
	}

	private void ClearMixedRepairMemo()
	{
		lock (_cache)
		{
			_mixedRepairOriginals.Clear();
			_mixedRepairTranslations.Clear();
			_mixedRepairMisses.Clear();
		}
	}

	private static bool TryRepairMixedTranslatedTextFromMap(IEnumerable<KeyValuePair<string, string>> map, string latinTail, string cjkPrefix, out string originalText, out string translated)
	{
		originalText = null;
		translated = null;
		foreach (KeyValuePair<string, string> pair in map)
		{
			if (string.IsNullOrWhiteSpace(pair.Key) || string.IsNullOrWhiteSpace(pair.Value) || ContainsCjk(pair.Key))
			{
				continue;
			}
			string originalVisible = NormalizeLooseLatinText(GetVisibleText(pair.Key));
			if (originalVisible.Length < latinTail.Length || !originalVisible.EndsWith(latinTail, StringComparison.Ordinal))
			{
				continue;
			}
			string value = SanitizeTranslationArtifacts(pair.Value);
			if (!ContainsCjk(value) || !GetLeadingCjkSignature(value).StartsWith(cjkPrefix, StringComparison.Ordinal) || LooksLikeMixedTranslationResidue(value))
			{
				continue;
			}
			if (!IsAcceptableTranslation(pair.Key, value))
			{
				continue;
			}
			originalText = pair.Key;
			translated = value;
			return true;
		}
		return false;
	}

	private async Task TranslateAsync(string text, Action<string> callback)
	{
		await TranslateAsync(text, GetRequestDomain(text), callback);
	}

	private async Task TranslateAsync(string text, string domain, Action<string> callback)
	{
		if (string.IsNullOrWhiteSpace(text) || ContainsCjk(text) || ShouldSkipText(text))
		{
			callback?.Invoke(text);
			return;
		}
		try
		{
			ProtectedTextPayload protectedPayload = ProtectTextForTranslation(text);
			if (IsTokenDominatedFragment(protectedPayload.RequestText))
			{
				callback?.Invoke(text);
				return;
			}
			string text2 = (string.IsNullOrWhiteSpace(domain) ? GetRequestDomain(text) : domain);
			if (IsServerBackoffActive())
			{
				callback?.Invoke(text);
				return;
			}
			string body = BuildTranslatePayload(protectedPayload.RequestText, text2);
			string serverUrl = _serverUrl.Value;
			string text3 = await RunBackground(() => HttpPost(serverUrl + "/translate", body));
			if (!string.IsNullOrEmpty(text3))
			{
				NoteServerRequestSucceeded();
				JObject val = JObject.Parse(text3);
				string text4 = ((object)(val["translated_text"] ?? val["translation"] ?? val["translated"]))?.ToString();
				text4 = RestoreProtectedText(text4, protectedPayload);
				if (!HasRequiredProtectedValues(text4, protectedPayload))
				{
					/* A provider/cache result that drops renderer tokens is a content
					   rejection, not a transport miss. The renderer is the first boundary
					   that can validate those tokens; bounded rejection prevents a bad
					   cached value from being requested forever, and abandonment is logged. */
					MarkRejectedTranslationRetry(text, text4);
					callback?.Invoke(text);
				}
				else if (IsAcceptableTranslation(text, text4))
				{
					StoreCachedTranslation(text, text4);
					callback?.Invoke(text4);
				}
				else
				{
					MarkRejectedTranslationRetry(text, text4);
					callback?.Invoke(text);
				}
			}
			else
			{
				NoteServerRequestFailed();
				callback?.Invoke(text);
			}
		}
		catch (Exception ex)
		{
			NoteServerRequestFailed(ex);
			if (_debugMode.Value)
			{
				base.Logger.LogInfo("TranslateAsync error: " + ex);
			}
			callback?.Invoke(text);
		}
	}

	private void StoreCachedTranslation(string original, string translated)
	{
		translated = SanitizeTranslationArtifacts(translated);
		/* 能走到这里的文本已经通过去抖稳定窗，可视为最终文本并安全缓存...		   即使结尾形态仍像打字机截断，也不能再次永久丢弃...*/
		if (!IsAcceptableTranslation(original, translated))
		{
			return;
		}
		lock (_cache)
		{
			_cache[original] = translated;
			string text = NormalizeRequestText(original);
			if (!string.Equals(text, original, StringComparison.Ordinal))
			{
				_cache[text] = translated;
			}
			MarkLocalCacheKeyLocked(original);
		}
		ClearMixedRepairMemo();
		ClearTranslationRetryState(original);
		ScheduleLocalCachePersist();
	}

	private void MarkLocalCacheKeyLocked(string key)
	{
		if (string.IsNullOrWhiteSpace(key))
		{
			return;
		}
		_localCacheKeys.Add(key);
		string text = NormalizeRequestText(key);
		if (!string.IsNullOrWhiteSpace(text))
		{
			_localCacheKeys.Add(text);
		}
	}

	private bool TryMarkAppliedCacheKeyForPersist(string original, string translated)
	{
		if (string.IsNullOrWhiteSpace(original) || string.IsNullOrWhiteSpace(translated))
		{
			return false;
		}
		translated = SanitizeTranslationArtifacts(translated);
		if (LooksLikeTypewriterFragment(original) || !IsAcceptableTranslation(original, translated))
		{
			return false;
		}
		bool flag = false;
		lock (_cache)
		{
			string text = NormalizeRequestText(original);
			bool flag2 = false;
			if (_cache.TryGetValue(original, out var value) && string.Equals(SanitizeTranslationArtifacts(value), translated, StringComparison.Ordinal))
			{
				flag2 = true;
			}
			else if (!string.Equals(text, original, StringComparison.Ordinal) && _cache.TryGetValue(text, out var value2) && string.Equals(SanitizeTranslationArtifacts(value2), translated, StringComparison.Ordinal))
			{
				flag2 = true;
			}
			if (!flag2)
			{
				return false;
			}
			int count = _localCacheKeys.Count;
			MarkLocalCacheKeyLocked(original);
			flag = _localCacheKeys.Count != count;
		}
		if (flag)
		{
			ScheduleLocalCachePersist();
		}
		return flag;
	}

	private bool IsTranslationRetryCoolingDown(string text)
	{
		if (string.IsNullOrWhiteSpace(text))
		{
			return false;
		}
		DateTime now = DateTime.UtcNow;
		string text2 = NormalizeRequestText(text);
		lock (_translationRetryCooldownLock)
		{
			return IsTranslationRetryBlockedLocked(text, now) || (!string.IsNullOrWhiteSpace(text2) && IsTranslationRetryBlockedLocked(text2, now));
		}
	}

	private bool IsTranslationRetryBlockedLocked(string text, DateTime now)
	{
		string key = text ?? string.Empty;
		if (_translationRetryAbandoned.Contains(key))
		{
			return true;
		}
		return IsTranslationRetryCoolingDownLocked(key, now);
	}

	private bool IsTranslationRetryCoolingDownLocked(string text, DateTime now)
	{
		if (!_translationRetryCooldowns.TryGetValue(text ?? string.Empty, out var retryAfter))
		{
			return false;
		}
		if (retryAfter > now)
		{
			return true;
		}
		_translationRetryCooldowns.Remove(text ?? string.Empty);
		return false;
	}

	private static string PreviewForLog(string text)
	{
		if (string.IsNullOrEmpty(text))
		{
			return string.Empty;
		}
		return text.Substring(0, Math.Min(text.Length, 30));
	}

	private void MarkRejectedTranslationRetry(string text, string rejectedTranslation)
	{
		if (string.IsNullOrWhiteSpace(text))
		{
			return;
		}
		DateTime retryAfter = DateTime.UtcNow.AddSeconds(RejectedTranslationRetryCooldownSeconds);
		string text2 = NormalizeRequestText(text);
		int attempts;
		bool abandoned;
		lock (_translationRetryCooldownLock)
		{
			attempts = GetTranslationRejectCountLocked(text);
			if (!string.IsNullOrWhiteSpace(text2))
			{
				attempts = Math.Max(attempts, GetTranslationRejectCountLocked(text2));
			}
			attempts++;
			SetTranslationRejectStateLocked(text, text2, attempts, retryAfter);
			abandoned = attempts >= MaxRejectedTranslationRetries;
			if (abandoned)
			{
				_translationRetryAbandoned.Add(text);
				_translationRetryCooldowns.Remove(text);
				if (!string.IsNullOrWhiteSpace(text2))
				{
					_translationRetryAbandoned.Add(text2);
					_translationRetryCooldowns.Remove(text2);
				}
			}
		}
		if (abandoned)
		{
			base.Logger.LogWarning("[RETRY] Abandoning repeatedly rejected translation after " + attempts + " attempts: '" + PreviewForLog(text) + "' -> '" + PreviewForLog(rejectedTranslation) + "'");
		}
	}

	private void MarkTranslationRetryCooldown(string text)
	{
		if (string.IsNullOrWhiteSpace(text))
		{
			return;
		}
		DateTime retryAfter = DateTime.UtcNow.AddSeconds(TransientTranslationRetryCooldownSeconds);
		lock (_translationRetryCooldownLock)
		{
			_translationRetryCooldowns[text] = retryAfter;
			string text2 = NormalizeRequestText(text);
			if (!string.IsNullOrWhiteSpace(text2))
			{
				_translationRetryCooldowns[text2] = retryAfter;
			}
		}
	}

	private int GetTranslationRejectCountLocked(string text)
	{
		return _translationRejectCounts.TryGetValue(text ?? string.Empty, out var attempts) ? attempts : 0;
	}

	private void SetTranslationRejectStateLocked(string text, string normalizedText, int attempts, DateTime retryAfter)
	{
		_translationRejectCounts[text] = attempts;
		_translationRetryCooldowns[text] = retryAfter;
		if (!string.IsNullOrWhiteSpace(normalizedText))
		{
			_translationRejectCounts[normalizedText] = attempts;
			_translationRetryCooldowns[normalizedText] = retryAfter;
		}
	}

	private void ClearTranslationRetryState(string text)
	{
		if (string.IsNullOrWhiteSpace(text))
		{
			return;
		}
		string text2 = NormalizeRequestText(text);
		lock (_translationRetryCooldownLock)
		{
			_translationRetryCooldowns.Remove(text);
			_translationRejectCounts.Remove(text);
			_translationRetryAbandoned.Remove(text);
			if (!string.IsNullOrWhiteSpace(text2))
			{
				_translationRetryCooldowns.Remove(text2);
				_translationRejectCounts.Remove(text2);
				_translationRetryAbandoned.Remove(text2);
			}
		}
	}

	private async Task<Dictionary<string, string>> WarmupTextsAsync(IEnumerable<string> texts, string domain)
	{
		List<string> list = (from text5 in texts?.Where((string text5) => !string.IsNullOrWhiteSpace(text5) && !ContainsCjk(text5) && !LooksLikeTypewriterFragment(text5) && !ShouldSkipText(text5) && !IsTranslationRetryCoolingDown(text5))
			select text5.Trim()).Distinct(StringComparer.Ordinal).ToList() ?? new List<string>();
		Dictionary<string, string> results = new Dictionary<string, string>(StringComparer.Ordinal);
		if (list.Count == 0)
		{
			return results;
		}
		if (!await WaitForWarmupServerReadyAsync())
		{
			LogVerbose($"[WARMUP] Server not ready; skipped {list.Count} queued warmup texts");
			return results;
		}
		List<(string Original, ProtectedTextPayload Payload)> requestItems = new List<(string, ProtectedTextPayload)>();
		foreach (string item2 in list)
		{
			if (TryGetLocalTranslation(item2, out var translated))
			{
				results[item2] = translated;
				continue;
			}
			ProtectedTextPayload protectedTextPayload = ProtectTextForTranslation(item2);
			if (!IsTokenDominatedFragment(protectedTextPayload.RequestText))
			{
				requestItems.Add((item2, protectedTextPayload));
			}
		}
		if (requestItems.Count == 0)
		{
			return results;
		}
		try
		{
			if (IsServerBackoffActive())
			{
				return results;
			}
			string text = (string.IsNullOrWhiteSpace(domain) ? "dialogue" : domain);
			string body = BuildBatchPayload(requestItems.Select(((string Original, ProtectedTextPayload Payload) tuple) => tuple.Payload.RequestText), text);
			string serverUrl = _serverUrl.Value;
			string text2 = await RunBackground(() => HttpPost(serverUrl + "/batch", body));
			if (string.IsNullOrEmpty(text2))
			{
				NoteServerRequestFailed();
				return results;
			}
			NoteServerRequestSucceeded();
			JObject response = JObject.Parse(text2);
			for (int num = 0; num < requestItems.Count; num++)
			{
				string item = requestItems[num].Original;
				string batchSource = GetBatchResponseSource(response, num);
				string text4 = RestoreProtectedText(GetBatchTranslation(response, num, item, requestItems[num].Payload), requestItems[num].Payload);
				if (IsBatchPassThroughSource(batchSource))
				{
					/* miss/queued/pass 回显：只瞬时冷却，不计拒绝、不写缓存。 */
					MarkTranslationRetryCooldown(item);
				}
				else if (!HasRequiredProtectedValues(text4, requestItems[num].Payload))
				{
					MarkRejectedTranslationRetry(item, text4);
				}
				else if (IsAcceptableTranslation(item, text4))
				{
					StoreCachedTranslation(item, text4);
					results[item] = text4;
				}
				else
				{
					MarkRejectedTranslationRetry(item, text4);
				}
			}
		}
		catch (Exception ex)
		{
			NoteServerRequestFailed(ex);
			if (_debugMode.Value)
			{
				base.Logger.LogInfo("WarmupTextsAsync error: " + ex);
			}
		}
		return results;
	}

	private async Task<bool> WaitForWarmupServerReadyAsync()
	{
		string serverUrl = _serverUrl?.Value;
		if (string.IsNullOrWhiteSpace(serverUrl))
		{
			return false;
		}
		try
		{
			return await RunBackground(delegate
			{
				DateTime deadline = DateTime.UtcNow.AddMilliseconds(WarmupServerReadyWaitMs);
				while (true)
				{
					try
					{
						if (!string.IsNullOrEmpty(HttpGet(serverUrl + "/health", 1000)))
						{
							return true;
						}
					}
					catch (Exception reportedException)
					{
						ReportCaughtException(reportedException);
					}
					if (DateTime.UtcNow >= deadline)
					{
						return false;
					}
					Thread.Sleep(WarmupServerReadyPollMs);
				}
			});
		}
		catch (Exception ex)
		{
			ReportCaughtException(ex);
			return false;
		}
	}

	private static string GetRequestDomain(string text)
	{
		if (!LooksLikeUiText(text))
		{
			return "dialogue";
		}
		return "ui";
	}

	private static bool ShouldPromoteToGlossary(string text)
	{
		if (string.IsNullOrWhiteSpace(text))
		{
			return false;
		}
		string visibleText = GetVisibleText(text);
		if (string.IsNullOrWhiteSpace(visibleText))
		{
			return false;
		}
		if (visibleText.Length <= 48)
		{
			return text.All((char ch) => ch != '\n' && ch != '\r');
		}
		return false;
	}

	private static int GetDomainPriority(string domain)
	{
		domain = GetBaseRequestDomain(domain);
		if (string.Equals(domain, "dialogue", StringComparison.OrdinalIgnoreCase))
		{
			return 0;
		}
		if (string.Equals(domain, "ui", StringComparison.OrdinalIgnoreCase))
		{
			return 1;
		}
		if (string.Equals(domain, "system", StringComparison.OrdinalIgnoreCase))
		{
			return 2;
		}
		return 3;
	}

	private static int GetClientBatchWindowMs(string domain)
	{
		domain = GetBaseRequestDomain(domain);
		if (string.Equals(domain, "ui", StringComparison.OrdinalIgnoreCase))
		{
			return UiClientBatchWindowMs;
		}
		if (string.Equals(domain, "system", StringComparison.OrdinalIgnoreCase))
		{
			return SystemClientBatchWindowMs;
		}
		return DefaultClientBatchWindowMs;
	}

	private static string BuildBatchRequestKey(string text, string domain)
	{
		return domain + "\n" + text;
	}

	private static string GetBaseRequestDomain(string domain)
	{
		if (string.IsNullOrWhiteSpace(domain))
		{
			return "dialogue";
		}
		int marker = domain.IndexOf(LongTextLaneMarker, StringComparison.Ordinal);
		return marker > 0 ? domain.Substring(0, marker) : domain;
	}

	private static string BuildLongTextLaneDomain(string domain, int segmentIndex)
	{
		return GetBaseRequestDomain(domain) + LongTextLaneMarker + (segmentIndex % MaxParallelLongTextSegments);
	}

	private void InvokePendingRequestCallbacks(IEnumerable<PendingBatchRequest> requests, IReadOnlyDictionary<string, string> results)
	{
		foreach (PendingBatchRequest request in requests)
		{
			string value;
			string obj = ((results != null && results.TryGetValue(request.Key, out value)) ? value : null);
			foreach (Action<string> callback in request.Callbacks)
			{
				try
				{
					callback?.Invoke(obj);
				}
				catch (Exception ex)
				{
					/* A renderer callback is external to the batch dispatcher. One callback
					   must not abort its peers, but normal DebugMode=false still records the
					   failed request boundary and never turns it into a successful result. */
					ReportCaughtException(ex, "boundary=batch-callback Callback failed for '" + PreviewForLog(request.OriginalText) + "'");
				}
			}
		}
	}

	private void RequestSharedTranslation(string text, string domain, Action<string> callback, bool lowPriority = false, bool allowLongTextSplit = true, ProtectedTextPayload preparedPayload = null)
	{
		/* ...domain+原文合并并发等待者。_batchFlushScheduled 是单一调度令牌...		   只有持有它的 flush 任务能消费队列，finally 必须清除或重启...*/
		if (string.IsNullOrWhiteSpace(text))
		{
			callback?.Invoke(text);
			return;
		}
		if (_shuttingDown)
		{
			callback?.Invoke(null);
			return;
		}
		if (TryGetLocalTranslation(text, out var translated))
		{
			callback?.Invoke(translated);
			return;
		}
		/* Every caller, including long-text segment fan-out, must honor the shared
		   transient/abandoned state. Call-site-only checks let rejected segments
		   bypass their retry budget. This returns no successful value, preserves
		   source display, and the rejection owner logs terminal abandonment. */
		if (IsTranslationRetryCoolingDown(text))
		{
			callback?.Invoke(null);
			return;
		}
		if (IsServerBackoffActive())
		{
			callback?.Invoke(null);
			return;
		}
		if (IsRemoteQueueSaturated(text, lowPriority))
		{
			callback?.Invoke(null);
			return;
		}
		string domain2 = (string.IsNullOrWhiteSpace(domain) ? GetRequestDomain(text) : domain);
		int clientBatchWindowMs = GetClientBatchWindowMs(domain2);
		ProtectedTextPayload protectedTextPayload = preparedPayload ?? ProtectTextForTranslation(text);
		if (IsTokenDominatedFragment(protectedTextPayload.RequestText))
		{
			callback?.Invoke(text);
			return;
		}
		if (allowLongTextSplit && TryQueueLongTextTranslation(text, domain2, protectedTextPayload, callback, lowPriority))
		{
			return;
		}
		string text2 = BuildBatchRequestKey(text, domain2);
		bool flag = false;
		lock (_pendingLock)
		{
			if (!_pendingBatchRequests.TryGetValue(text2, out var value))
			{
				value = new PendingBatchRequest
				{
					Key = text2,
					OriginalText = text,
					Domain = domain2,
					Payload = protectedTextPayload,
					LowPriority = lowPriority
				};
				_pendingBatchRequests[text2] = value;
				_pendingBatchQueue.Add(text2);
			}
			value.Callbacks.Add(callback);
			if (!_batchFlushScheduled)
			{
				_batchFlushScheduled = true;
				flag = true;
			}
		}
		if (flag)
		{
			_ = FlushPendingBatchRequestsAsync(clientBatchWindowMs);
		}
	}

	private bool TryQueueLongTextTranslation(string text, string domain, ProtectedTextPayload fullPayload, Action<string> callback, bool lowPriority)
	{
		IReadOnlyList<UnityLongTextPart> parts = UnityLongTextPlanner.Plan(
			fullPayload.RequestText,
			LongTextParallelThreshold,
			LongTextTargetSegmentChars,
			MaxParallelLongTextSegments);
		if (parts == null || parts.Count < 2)
		{
			return false;
		}

		string key = "long\n" + BuildBatchRequestKey(text, GetBaseRequestDomain(domain));
		PendingLongTextRequest state;
		lock (_pendingLock)
		{
			if (_pendingLongTextRequests.TryGetValue(key, out state))
			{
				state.Callbacks.Add(callback);
				return true;
			}

			ProtectedTextPayload[] payloads = new ProtectedTextPayload[parts.Count];
			for (int i = 0; i < parts.Count; i++)
			{
				payloads[i] = BuildLongTextSegmentPayload(parts[i], fullPayload);
			}
			state = new PendingLongTextRequest
			{
				Key = key,
				OriginalText = text,
				Parts = parts,
				Payloads = payloads,
				Results = new string[parts.Count],
				Remaining = parts.Count
			};
			state.Callbacks.Add(callback);
			_pendingLongTextRequests[key] = state;
		}

		for (int i = 0; i < parts.Count; i++)
		{
			int segmentIndex = i;
			ProtectedTextPayload segmentPayload = state.Payloads[segmentIndex];
			RequestSharedTranslation(
				segmentPayload.OriginalText,
				BuildLongTextLaneDomain(domain, segmentIndex),
				result => CompleteLongTextSegment(state, segmentIndex, result),
				lowPriority,
				allowLongTextSplit: false,
				preparedPayload: segmentPayload);
		}
		return true;
	}

	private static ProtectedTextPayload BuildLongTextSegmentPayload(UnityLongTextPart part, ProtectedTextPayload fullPayload)
	{
		ProtectedTextPayload payload = new ProtectedTextPayload
		{
			OriginalText = part.Text,
			RequestText = part.Text
		};
		foreach (KeyValuePair<string, string> token in fullPayload.Tokens)
		{
			if (payload.RequestText.IndexOf(token.Key, StringComparison.Ordinal) < 0)
			{
				continue;
			}
			payload.Tokens[token.Key] = token.Value;
			payload.OriginalText = payload.OriginalText.Replace(token.Key, token.Value);
		}
		return payload;
	}

	private static bool HasRequiredProtectedValues(string translated, ProtectedTextPayload payload)
	{
		if (string.IsNullOrWhiteSpace(translated))
		{
			return false;
		}
		if (payload == null)
		{
			return true;
		}
		foreach (string value in payload.Tokens.Values)
		{
			if (!string.IsNullOrEmpty(value) && translated.IndexOf(value, StringComparison.Ordinal) < 0)
			{
				return false;
			}
		}
		return true;
	}

	private void CompleteLongTextSegment(PendingLongTextRequest state, int segmentIndex, string result)
	{
		bool shouldComplete = false;
		lock (_pendingLock)
		{
			if (state.Remaining <= 0 || segmentIndex < 0 || segmentIndex >= state.Results.Length)
			{
				return;
			}
			ProtectedTextPayload payload = state.Payloads[segmentIndex];
			if (!IsAcceptableTranslation(payload.OriginalText, result) || !HasRequiredProtectedValues(result, payload))
			{
				state.Failed = true;
			}
			else
			{
				state.Results[segmentIndex] = result.Trim();
			}
			state.Remaining--;
			shouldComplete = state.Remaining == 0;
		}
		if (!shouldComplete)
		{
			return;
		}

		string combined = null;
		if (!state.Failed)
		{
			StringBuilder builder = new StringBuilder(state.OriginalText.Length);
			for (int i = 0; i < state.Parts.Count; i++)
			{
				builder.Append(state.Parts[i].LeadingWhitespace);
				builder.Append(state.Results[i]);
				builder.Append(state.Parts[i].TrailingWhitespace);
			}
			combined = builder.ToString();
			if (!IsAcceptableTranslation(state.OriginalText, combined))
			{
				state.Failed = true;
				combined = null;
			}
		}
		if (!state.Failed && combined != null)
		{
			StoreCachedTranslation(state.OriginalText, combined);
		}

		List<Action<string>> callbacks;
		lock (_pendingLock)
		{
			_pendingLongTextRequests.Remove(state.Key);
			callbacks = new List<Action<string>>(state.Callbacks);
		}
		InvokeLongTextCallbacks(callbacks, combined);
	}

	private void InvokeLongTextCallbacks(IEnumerable<Action<string>> callbacks, string result)
	{
		foreach (Action<string> callback in callbacks)
		{
			try
			{
				callback?.Invoke(result);
			}
			catch (Exception ex)
			{
				ReportCaughtException(ex, "boundary=long-text-callback");
			}
		}
	}

	private List<PendingBatchRequest> DequeuePendingBatchRequests(int maxCount)
	{
		lock (_pendingLock)
		{
			if (_pendingBatchQueue.Count == 0)
			{
				return new List<PendingBatchRequest>();
			}
			var list = (from item in _pendingBatchQueue.Select(delegate(string key, int index)
				{
					_pendingBatchRequests.TryGetValue(key, out var value);
					return new
					{
						Key = key,
						Index = index,
						Request = value
					};
				})
				where item.Request != null
				orderby item.Request.LowPriority ? 1 : 0, GetDomainPriority(item.Request.Domain), item.Index
				select item).Take(maxCount).ToList();
			if (list.Count == 0)
			{
				_pendingBatchQueue.Clear();
				return new List<PendingBatchRequest>();
			}
			HashSet<string> selectedKeys = new HashSet<string>(list.Select(item => item.Key), StringComparer.Ordinal);
			List<PendingBatchRequest> list2 = new List<PendingBatchRequest>(list.Count);
			foreach (var item in list)
			{
				list2.Add(item.Request);
				_pendingBatchRequests.Remove(item.Key);
			}
			_pendingBatchQueue.RemoveAll((string key) => selectedKeys.Contains(key));
			return list2;
		}
	}

	private async Task FlushPendingBatchRequestsAsync(int initialDelayMs)
	{
		bool faulted = false;
		try
		{
			if (initialDelayMs <= 0)
			{
				await Task.Yield();
			}
			else
			{
				await Task.Delay(initialDelayMs);
			}
			while (true)
			{
				Task[] workers = new Task[MaxConcurrentBatchFlushes];
				for (int i = 0; i < workers.Length; i++)
				{
					workers[i] = DrainPendingBatchQueueAsync();
				}
				await Task.WhenAll(workers);
				lock (_pendingLock)
				{
					if (_pendingBatchQueue.Count == 0)
					{
						break;
					}
				}
			}
		}
		catch (Exception ex)
		{
			faulted = true;
			try
			{
				NoteServerRequestFailed(ex);
				LogVerbose("[BATCH] Flush failed: " + ex.Message);
			}
			catch (Exception reportedException)
			{
				ReportCaughtException(reportedException);
			}
		}
		finally
		{
			/* 活性约束：_batchFlushScheduled ...true 时只允许此任务运行...			   无论上方如何退出，都必须清标志或重启，否则本局不再分发翻译...*/
			bool restart = false;
			try
			{
				lock (_pendingLock)
				{
					if (_pendingBatchQueue.Count == 0)
					{
						_batchFlushScheduled = false;
					}
					else
					{
						_batchFlushScheduled = true;
						restart = true;
					}
				}
			}
			catch (Exception ex)
			{
				ReportCaughtException(ex);
				faulted = true;
				restart = true;
			}
			if (restart)
			{
				_ = FlushPendingBatchRequestsAsync(faulted ? BatchFlushFaultRestartDelayMs : 0);
			}
		}
	}

	private async Task DrainPendingBatchQueueAsync()
	{
		while (true)
		{
			List<PendingBatchRequest> list = DequeuePendingBatchRequests(MaxClientBatchSize);
			if (list.Count == 0)
			{
				break;
			}
			try
			{
				await ProcessPendingBatchRequestsAsync(list);
			}
			catch (Exception ex)
			{
				try
				{
					NoteServerRequestFailed(ex);
					LogVerbose("[BATCH] Worker failed: " + ex.Message);
				}
				catch (Exception reportedException)
				{
					ReportCaughtException(reportedException);
				}
				try
				{
					InvokePendingRequestCallbacks(list, new Dictionary<string, string>(StringComparer.Ordinal));
				}
				catch (Exception reportedException)
				{
					ReportCaughtException(reportedException);
				}
			}
		}
	}

	private async Task ProcessPendingBatchRequestsAsync(List<PendingBatchRequest> batchRequests)
	{
		await Task.WhenAll((from @group in (from @group in batchRequests.GroupBy((PendingBatchRequest request) => request.Domain, StringComparer.OrdinalIgnoreCase)
				orderby GetDomainPriority(@group.Key)
				select new
				{
					Domain = @group.Key,
					Requests = @group.ToList()
				}).ToList()
			select ProcessPendingBatchGroupAsync(@group.Domain, @group.Requests)).ToArray());
	}

	private async Task ProcessPendingBatchGroupAsync(string domain, List<PendingBatchRequest> requests)
	{
		Dictionary<string, string> groupResults = new Dictionary<string, string>(StringComparer.Ordinal);
		if (IsServerBackoffActive())
		{
			InvokePendingRequestCallbacks(requests, groupResults);
			return;
		}
		try
		{
			string body = BuildBatchPayload(requests.Select((PendingBatchRequest request) => request.Payload.RequestText), GetBaseRequestDomain(domain));
			LogVerbose($"[BATCH] Sending {requests.Count} texts to /batch (domain={domain})");
			string serverUrl = _serverUrl.Value;
			string text = await RunBackground(() => HttpPost(serverUrl + "/batch", body));
			if (!string.IsNullOrEmpty(text))
			{
				NoteServerRequestSucceeded();
				JObject response = JObject.Parse(text);
				LogVerbose("[BATCH] response parsed");
				for (int num = 0; num < requests.Count; num++)
				{
					PendingBatchRequest pendingBatchRequest = requests[num];
					string batchSource = GetBatchResponseSource(response, num);
					string text2 = RestoreProtectedText(GetBatchTranslation(response, num, pendingBatchRequest.OriginalText, pendingBatchRequest.Payload), pendingBatchRequest.Payload);
					if (IsBatchPassThroughSource(batchSource))
					{
						/* miss/queued/pass 是服务器回显或透传：瞬时冷却后重试，
						   不计入拒绝，也不写入缓存。 */
						MarkTranslationRetryCooldown(pendingBatchRequest.OriginalText);
						LogVerbose("[BATCH] " + batchSource.ToUpperInvariant() + ": '" + pendingBatchRequest.OriginalText?.Substring(0, Math.Min(pendingBatchRequest.OriginalText?.Length ?? 0, 30)) + "'");
					}
					else if (!HasRequiredProtectedValues(text2, pendingBatchRequest.Payload))
					{
						/* Missing renderer tokens are provider/content rejection. They
						   cannot be repaired by the server transport boundary and must
						   consume the finite rejection budget instead of polling forever. */
						MarkRejectedTranslationRetry(pendingBatchRequest.OriginalText, text2);
						LogVerbose("[BATCH] MISSING-TOKENS: '" + pendingBatchRequest.OriginalText?.Substring(0, Math.Min(pendingBatchRequest.OriginalText?.Length ?? 0, 30)) + "' -> '" + text2?.Substring(0, Math.Min(text2?.Length ?? 0, 30)) + "'");
					}
					else if (!IsAcceptableTranslation(pendingBatchRequest.OriginalText, text2))
					{
						MarkRejectedTranslationRetry(pendingBatchRequest.OriginalText, text2);
						LogVerbose("[BATCH] REJECTED: '" + pendingBatchRequest.OriginalText?.Substring(0, Math.Min(pendingBatchRequest.OriginalText?.Length ?? 0, 30)) + "' -> '" + text2?.Substring(0, Math.Min(text2?.Length ?? 0, 30)) + "'");
					}
					else
					{
						StoreCachedTranslation(pendingBatchRequest.OriginalText, text2);
						groupResults[pendingBatchRequest.Key] = text2;
						LogVerbose("[BATCH] ACCEPTED: '" + pendingBatchRequest.OriginalText?.Substring(0, Math.Min(pendingBatchRequest.OriginalText?.Length ?? 0, 30)) + "' -> '" + text2?.Substring(0, Math.Min(text2?.Length ?? 0, 30)) + "'");
					}
				}
			}
			else
			{
				NoteServerRequestFailed();
			}
		}
		catch (Exception ex)
		{
			ReportCaughtException(ex, "boundary=live-batch-request");
			try
			{
				NoteServerRequestFailed(ex);
			}
			catch (Exception reportedException)
			{
				ReportCaughtException(reportedException);
			}
		}
		/* 回调异常不能越过本方法，否则整个 worker 链会失败，drain 级降级路...		   还可能让同批其它分组被重复回调...*/
		try
		{
			LogVerbose($"[BATCH] Invoking callbacks for {requests.Count} requests, {groupResults.Count} results");
			InvokePendingRequestCallbacks(requests, groupResults);
		}
		catch (Exception ex2)
		{
			ReportCaughtException(ex2, "boundary=batch-callback-dispatch");
		}
	}

	private void ScheduleAsyncApply(object component, int instanceId, string originalText, bool isTmp, bool preserveRichText = true, bool lowPriority = false)
	{
		ObserveFirstAsyncQueue(component, originalText);
		if (isTmp && !CanHandleTmp())
		{
			ClearInProgress(instanceId, originalText);
			return;
		}
		WeakReference componentRef = new WeakReference(component);
		RequestSharedTranslation(originalText, GetRequestDomain(originalText), delegate(string result)
		{
			try
			{
				ClearInProgress(instanceId, originalText);
				if (!string.IsNullOrWhiteSpace(result))
				{
					if (string.Equals(NormalizeRequestText(result), NormalizeRequestText(originalText), StringComparison.Ordinal))
					{
						MarkTranslationRetryCooldown(originalText);
						LogVerbose("[ASYNC] pass-through result left retryable: '" + originalText?.Substring(0, Math.Min(originalText?.Length ?? 0, 30)) + "'");
					}
					else
					{
						object target = componentRef.Target;
						if (!_shuttingDown && target != null)
						{
							QueueTranslationApply(target, instanceId, originalText, result, isTmp, preserveRichText);
						}
					}
				}
				else
				{
					MarkTranslationRetryCooldown(originalText);
				}
			}
			catch (Exception ex)
			{
				ClearInProgress(instanceId, originalText);
				base.Logger.LogWarning("[ASYNC] Callback error for '" + originalText?.Substring(0, Math.Min(originalText?.Length ?? 0, 30)) + "': " + ex.Message);
			}
		}, lowPriority);
	}

	private IEnumerator SceneWarmupCoroutine(int generation)
	{
		yield return (object)WaitForScaledSecondsSafe(SceneWarmupDelaySeconds);
		if (TryStartSceneWarmupPass(generation))
		{
			yield return (object)WaitForScaledSecondsSafe(SceneWarmupSecondPassDelaySeconds);
			if (TryStartSceneWarmupPass(generation))
			{
				yield return (object)WaitForScaledSecondsSafe(SceneWarmupThirdPassDelaySeconds);
				TryStartSceneWarmupPass(generation);
			}
		}
	}

	private bool TryStartSceneWarmupPass(int generation)
	{
		if (generation != _sceneWarmupGeneration)
		{
			return false;
		}
		List<WarmupCandidate> list = CollectSceneWarmupCandidates();
		if (list.Count > 0)
		{
			_ = WarmupVisibleCandidatesAsync(list, generation);
		}
		return true;
	}

	private List<WarmupCandidate> CollectSceneWarmupCandidates()
	{
		List<WarmupCandidate> list = new List<WarmupCandidate>();
		HashSet<string> seenTexts = new HashSet<string>(StringComparer.Ordinal);
		Type type = AccessTools.TypeByName("TMPro.TMP_Text");
		PropertyInfo propertyInfo = ((type != null) ? AccessTools.Property(type, "text") : null);
		if (type != null && propertyInfo != null && CanHandleTmp())
		{
			Object[] array = FindTmpTextObjectsSafe(type);
			foreach (Object val in array)
			{
				if (val == (Object)null || !IsComponentActive(val))
				{
					continue;
				}
				string text = propertyInfo.GetValue(val) as string;
				if (TryReserveWarmupText(text, seenTexts))
				{
					list.Add(new WarmupCandidate
					{
						ComponentRef = new WeakReference(val),
						InstanceId = GetComponentInstanceId(val),
						OriginalText = text,
						IsTmp = true
					});
					if (list.Count >= MaxSceneWarmupCandidates)
					{
						return list;
					}
				}
			}
		}
		Text[] array2 = FindObjectsOfTypeAllSafe<Text>();
		foreach (Text val2 in array2)
		{
			if (!((Object)(object)val2 == (Object)null) && IsComponentActive(val2) && TryReserveWarmupText(val2.text, seenTexts))
			{
				list.Add(new WarmupCandidate
				{
					ComponentRef = new WeakReference(val2),
					InstanceId = GetComponentInstanceId(val2),
					OriginalText = val2.text,
					IsTmp = false
				});
				if (list.Count >= MaxSceneWarmupCandidates)
				{
					break;
				}
			}
		}
		return list;
	}

	private bool TryReserveWarmupText(string text, ISet<string> seenTexts)
	{
		if (string.IsNullOrWhiteSpace(text) || ContainsCjk(text) || ShouldSkipText(text))
		{
			return false;
		}
		string text2 = NormalizeRequestText(text);
		if (string.IsNullOrWhiteSpace(text2) || !seenTexts.Add(text2))
		{
			return false;
		}
		if (IsTranslationRetryCoolingDown(text))
		{
			return false;
		}
		lock (_pendingLock)
		{
			return _warmupRequestedSources.Add(text2);
		}
	}

	private async Task WarmupVisibleCandidatesAsync(List<WarmupCandidate> candidates, int generation)
	{
		if (candidates == null || candidates.Count == 0)
		{
			return;
		}
		List<WarmupCandidate> uiCandidates = new List<WarmupCandidate>();
		Dictionary<string, HashSet<string>> warmupTextsByDomain = new Dictionary<string, HashSet<string>>(StringComparer.OrdinalIgnoreCase);
		foreach (WarmupCandidate candidate in candidates)
		{
			if (!candidate.IsTmp || CanHandleTmp())
			{
				if (TryGetLocalTranslation(candidate.OriginalText, out var translated))
				{
					QueueTranslationApply(candidate.ComponentRef?.Target, candidate.InstanceId, candidate.OriginalText, translated, candidate.IsTmp);
				}
				else if (LooksLikeVisibleWarmupText(candidate.OriginalText))
				{
					uiCandidates.Add(candidate);
					string requestDomain = GetRequestDomain(candidate.OriginalText);
					if (!warmupTextsByDomain.TryGetValue(requestDomain, out var bucket))
					{
						bucket = new HashSet<string>(StringComparer.Ordinal);
						warmupTextsByDomain[requestDomain] = bucket;
					}
					bucket.Add(candidate.OriginalText);
				}
			}
		}
		Dictionary<string, string> translations = new Dictionary<string, string>(StringComparer.Ordinal);
		foreach (KeyValuePair<string, HashSet<string>> bucket in warmupTextsByDomain)
		{
			Dictionary<string, string> bucketTranslations = await WarmupTextsAsync(bucket.Value, bucket.Key);
			foreach (KeyValuePair<string, string> pair in bucketTranslations)
			{
				translations[pair.Key] = pair.Value;
			}
		}
		ApplyWarmupTranslations(uiCandidates, translations, generation);
	}

	private static bool LooksLikeVisibleWarmupText(string text)
	{
		if (LooksLikeUiText(text))
		{
			return true;
		}
		string visibleText = GetVisibleText(text);
		if (visibleText.Length < 8 || visibleText.Length > 220)
		{
			return false;
		}
		return LooksLikeNaturalLanguage(visibleText) && EndsWithSentenceBoundary(visibleText);
	}

	private void ApplyWarmupTranslations(IEnumerable<WarmupCandidate> candidates, IReadOnlyDictionary<string, string> translations, int generation)
	{
		if (generation != _sceneWarmupGeneration || translations == null || translations.Count == 0)
		{
			return;
		}
		foreach (WarmupCandidate candidate in candidates)
		{
			if (!candidate.IsTmp || CanHandleTmp())
			{
				object component = candidate.ComponentRef?.Target;
				if (IsUnityObjectAlive(component) && translations.TryGetValue(candidate.OriginalText, out var value))
				{
					QueueTranslationApply(component, candidate.InstanceId, candidate.OriginalText, value, candidate.IsTmp);
				}
			}
		}
	}

	private static int GetComponentInstanceId(object component)
	{
		Object val = (Object)((component is Object) ? component : null);
		if (val != null)
		{
			return val.GetInstanceID();
		}
		return component?.GetHashCode() ?? 0;
	}

	private static bool IsUnityObjectAlive(object component)
	{
		if (component == null)
		{
			return false;
		}
		Object val = (Object)((component is Object) ? component : null);
		if (val != null)
		{
			return val != (Object)null;
		}
		return true;
	}

	private static bool IsComponentActive(object component)
	{
		Component val = (Component)((component is Component) ? component : null);
		if (val != null)
		{
			if ((Object)(object)val.gameObject == (Object)null || !val.gameObject.activeInHierarchy)
			{
				return false;
			}
			Behaviour val2 = (Behaviour)(object)((val is Behaviour) ? val : null);
			if (val2 != null)
			{
				return val2.isActiveAndEnabled;
			}
			return true;
		}
		return true;
	}

	private static bool IsLikelyHistoryComponent(object component)
	{
		Component val = (Component)((component is Component) ? component : null);
		if (val == null)
		{
			return false;
		}
		int instanceID = ((Object)val).GetInstanceID();
		lock (HistoryComponentCache)
		{
			if (HistoryComponentCache.TryGetValue(instanceID, out var value))
			{
				return value;
			}
		}
		bool flag = false;
		Transform val2 = val.transform;
		int num = 0;
		while ((Object)(object)val2 != (Object)null && num < 8)
		{
			string name = ((Object)val2).name;
			if (!string.IsNullOrWhiteSpace(name))
			{
				string text = name.ToLowerInvariant();
				if (text.Contains("log") || text.Contains("history") || text.Contains("record") || text.Contains("backlog") || text.Contains("transcript") || text.Contains("journal"))
				{
					flag = true;
					break;
				}
			}
			num++;
			val2 = val2.parent;
		}
		if (!flag)
		{
			try
			{
				ScrollRect componentInParent = val.GetComponentInParent<ScrollRect>();
				if ((Object)(object)componentInParent != (Object)null && (Object)(object)componentInParent.content != (Object)null && val.transform.IsChildOf((Transform)(object)componentInParent.content))
				{
					Text[] componentsInChildren = ((Component)componentInParent.content).GetComponentsInChildren<Text>(true);
					int num2 = ((componentsInChildren != null) ? componentsInChildren.Length : 0);
					Type type = _historyTmpTextType ?? (_historyTmpTextType = AccessTools.TypeByName("TMPro.TMP_Text"));
					if (type != null)
					{
						Component[] componentsInChildren2 = GetComponentsInChildrenByTypeSafe((Component)componentInParent.content, type, includeInactive: true);
						num2 += ((componentsInChildren2 != null) ? componentsInChildren2.Length : 0);
					}
					flag = num2 >= 10;
				}
			}
			catch (Exception reportedException)
			{
				ReportCaughtException(reportedException);
			}
		}
		lock (HistoryComponentCache)
		{
			if (HistoryComponentCache.Count > 4096)
			{
				HistoryComponentCache.Clear();
			}
			HistoryComponentCache[instanceID] = flag;
			return flag;
		}
	}

	private int GetRemotePendingCount()
	{
		lock (_pendingLock)
		{
			return _pendingBatchRequests.Count + _pendingLongTextRequests.Count + _inProgress.Count;
		}
	}

	private bool IsRemoteQueueSaturated(string text, bool lowPriority)
	{
		if (LooksLikeUiText(text))
		{
			return false;
		}
		if (!lowPriority)
		{
			return false;
		}
		int remotePendingCount = GetRemotePendingCount();
		if (lowPriority && remotePendingCount >= MaxRemoteLowPriorityPendingRequests)
		{
			return true;
		}
		return false;
	}

	private static ProtectedTextPayload ProtectTextForTranslation(string text)
	{
		/* 富文本标签、格式变量和数字先替换为稳定 token；服务端只翻译可见文字，
		   返回...RestoreProtectedText 按原值恢复，避免模型改写控制序列...*/
		ProtectedTextPayload payload = new ProtectedTextPayload
		{
			OriginalText = text ?? string.Empty,
			RequestText = (text ?? string.Empty)
		};
		if (string.IsNullOrWhiteSpace(payload.RequestText))
		{
			return payload;
		}
		int tokenIndex = 0;
		MatchEvaluator evaluator = delegate(Match match)
		{
			string text2 = $"__DS_TOKEN_{tokenIndex++}__";
			payload.Tokens[text2] = match.Value;
			return text2;
		};
		payload.RequestText = RichTextTagRegex.Replace(payload.RequestText, evaluator);
		payload.RequestText = NumericTokenRegex.Replace(payload.RequestText, evaluator);
		return payload;
	}

	private static string RestoreProtectedText(string text, ProtectedTextPayload payload)
	{
		if (string.IsNullOrWhiteSpace(text))
		{
			return text;
		}
		if (payload == null || payload.Tokens.Count == 0)
		{
			return SanitizeTranslationArtifacts(text);
		}
		foreach (KeyValuePair<string, string> token in payload.Tokens)
		{
			text = text.Replace(token.Key, token.Value);
		}
		text = SanitizeTranslationArtifacts(text);
		return RestoreOuterRichTextWrapper(text, payload);
	}

	private static string RestoreOuterRichTextWrapper(string text, ProtectedTextPayload payload)
	{
		return RestoreOuterRichTextWrapper(text, payload?.OriginalText);
	}

	private static string RestoreLeadingTextDecoration(string translated, string originalText)
	{
		if (string.IsNullOrWhiteSpace(translated) || string.IsNullOrWhiteSpace(originalText))
		{
			return translated;
		}
		if (!TryGetLeadingTextDecoration(originalText, out var decoration, out _))
		{
			return translated;
		}
		int contentStart;
		if (!TryGetLeadingTextDecoration(translated, out _, out contentStart))
		{
			contentStart = 0;
			while (contentStart < translated.Length && IsHorizontalWhitespace(translated[contentStart]))
			{
				contentStart++;
			}
			int markerStart = contentStart;
			while (contentStart < translated.Length && IsLeadingTextDecorationMarker(translated[contentStart]))
			{
				contentStart++;
			}
			if (contentStart == markerStart)
			{
				contentStart = 0;
			}
			else
			{
				while (contentStart < translated.Length && IsHorizontalWhitespace(translated[contentStart]))
				{
					contentStart++;
				}
			}
		}
		if (contentStart >= translated.Length || string.IsNullOrWhiteSpace(translated.Substring(contentStart)))
		{
			return translated;
		}
		return decoration + translated.Substring(contentStart);
	}

	private static bool TryGetLeadingTextDecoration(string text, out string decoration, out int contentStart)
	{
		decoration = null;
		contentStart = 0;
		if (string.IsNullOrEmpty(text))
		{
			return false;
		}
		int index = 0;
		while (index < text.Length && IsHorizontalWhitespace(text[index]))
		{
			index++;
		}
		int markerStart = index;
		while (index < text.Length && IsLeadingTextDecorationMarker(text[index]))
		{
			index++;
		}
		if (index == markerStart)
		{
			return false;
		}
		int separatorStart = index;
		while (index < text.Length && IsHorizontalWhitespace(text[index]))
		{
			index++;
		}
		if (index == separatorStart || index >= text.Length)
		{
			return false;
		}
		decoration = text.Substring(0, index);
		contentStart = index;
		return true;
	}

	private static bool IsHorizontalWhitespace(char c)
	{
		return c == ' ' || c == '\t';
	}

	private static bool IsLeadingTextDecorationMarker(char c)
	{
		return c == '-' || c == '\u2013' || c == '\u2014' || c == '\u2022' || c == '\u2023' || c == '\u25aa' || c == '\u25cf' || c == '\u25a0' || c == '\u25a1' || c == '*' || c == '>' || c == '+' || c == '\u00b7';
	}

	private static string RestoreOuterRichTextWrapper(string text, string originalText)
	{
		if (string.IsNullOrWhiteSpace(text) || string.IsNullOrWhiteSpace(originalText))
		{
			return text;
		}
		if (ContainsColorRichTextTag(text))
		{
			return text;
		}
		if (!TryGetOuterRichTextWrapper(originalText, out var openingTag, out var closingTag))
		{
			return text;
		}
		string visibleText = GetVisibleText(text);
		if (string.IsNullOrWhiteSpace(visibleText))
		{
			return text;
		}
		return openingTag + text + closingTag;
	}

	private static bool TryGetOuterRichTextWrapper(string text, out string openingTag, out string closingTag)
	{
		openingTag = null;
		closingTag = null;
		if (string.IsNullOrWhiteSpace(text))
		{
			return false;
		}
		string trimmed = text.Trim();
		Match open = RichTextTagRegex.Match(trimmed);
		if (!open.Success || open.Index != 0 || !IsColorOpeningTag(open.Value))
		{
			return false;
		}
		Match close = Regex.Match(trimmed, "<\\s*/\\s*color\\s*>\\s*$", RegexOptions.IgnoreCase);
		if (!close.Success || close.Index <= open.Index + open.Length)
		{
			return false;
		}
		string inner = trimmed.Substring(open.Length, close.Index - open.Length);
		if (string.IsNullOrWhiteSpace(GetVisibleText(inner)))
		{
			return false;
		}
		openingTag = open.Value;
		closingTag = close.Value.TrimEnd();
		return true;
	}

	private static bool IsColorOpeningTag(string tag)
	{
		if (string.IsNullOrWhiteSpace(tag))
		{
			return false;
		}
		string body = tag.Substring(1, tag.Length - 2).Trim();
		if (IsTmpColorShorthandTagBody(body))
		{
			return true;
		}
		return body.StartsWith("color", StringComparison.OrdinalIgnoreCase) && body.IndexOf('=') >= 0 && body[0] != '/';
	}

	private static bool ContainsColorRichTextTag(string text)
	{
		if (string.IsNullOrWhiteSpace(text))
		{
			return false;
		}
		foreach (Match match in RichTextTagRegex.Matches(text))
		{
			if (IsColorOpeningTag(match.Value))
			{
				return true;
			}
		}
		return false;
	}

	private static int GetMojibakeScore(string text)
	{
		if (string.IsNullOrEmpty(text))
		{
			return 0;
		}
		int num = 0;
		foreach (char c in text)
		{
			if (MojibakeSignalChars.Contains(c))
			{
				num += 2;
			}
			else if (c >= 'Ѐ' && c <= 'ӿ')
			{
				num += 2;
			}
			else if (c == '€')
			{
				num += 2;
			}
		}
		return num;
	}

	private static int CountCjkCharacters(string text)
	{
		if (string.IsNullOrEmpty(text))
		{
			return 0;
		}
		int num = 0;
		foreach (char c in text)
		{
			if ((c >= '一' && c <= '鿿') || (c >= '㐀' && c <= '䶿'))
			{
				num++;
			}
		}
		return num;
	}

	private static string RepairUtf8DecodedAsGbk(string text)
	{
		int mojibakeScore = GetMojibakeScore(text);
		if (mojibakeScore < 2)
		{
			return text;
		}
		try
		{
			Encoding encoding = Encoding.GetEncoding(936);
			string text2 = Encoding.UTF8.GetString(encoding.GetBytes(text));
			if (string.IsNullOrWhiteSpace(text2) || text2.IndexOf('\ufffd') >= 0)
			{
				return text;
			}
			if (GetMojibakeScore(text2) < mojibakeScore && CountCjkCharacters(text2) > 0)
			{
				return text2;
			}
		}
		catch (Exception reportedException)
		{
			ReportCaughtException(reportedException);
		}
		return text;
	}

	private static readonly string[] TranslationPromptEchoPrefixes = new string[10]
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
		if (string.IsNullOrWhiteSpace(text))
		{
			return text;
		}
		string candidate = text.TrimStart();
		bool changed = false;
		for (int pass = 0; pass < 3; pass++)
		{
			bool stripped = false;
			foreach (string prefix in TranslationPromptEchoPrefixes)
			{
				if (!candidate.StartsWith(prefix, StringComparison.OrdinalIgnoreCase))
				{
					continue;
				}
				int index = prefix.Length;
				while (index < candidate.Length && (candidate[index] == ' ' || candidate[index] == '\t'))
				{
					index++;
				}
				if (index < candidate.Length && (candidate[index] == ':' || candidate[index] == '\uff1a'))
				{
					index++;
				}
				else if (index >= candidate.Length || (candidate[index] != '\r' && candidate[index] != '\n'))
				{
					continue;
				}
				while (index < candidate.Length && char.IsWhiteSpace(candidate[index]))
				{
					index++;
				}
				if (index >= candidate.Length)
				{
					continue;
				}
				candidate = candidate.Substring(index).TrimEnd();
				changed = true;
				stripped = true;
				break;
			}
			if (!stripped)
			{
				break;
			}
		}
		return changed ? candidate : text;
	}

	private static string SanitizeTranslationArtifacts(string text)
	{
		if (string.IsNullOrWhiteSpace(text))
		{
			return text;
		}
		text = RepairUtf8DecodedAsGbk(text);
		text = StripTranslationPromptEchoPrefix(text);
		text = NestedBrokenRichTextRegex.Replace(text, "</$1>");
		text = BrokenOpeningRichTextDashRegex.Replace(text, "<$1=");
		text = BareClosingRichTextRegex.Replace(text, "<$1");
		text = BareOpeningRichTextRegex.Replace(text, "<$1");
		text = EmptyAngleTagRegex.Replace(text, string.Empty);
		text = MalformedAngleFragmentRegex.Replace(text, string.Empty);
		text = DanglingTagSlashRegex.Replace(text, string.Empty);
		text = text.Replace("< >", string.Empty).Replace("＜＞", string.Empty).Replace("〈〉", string.Empty);
		text = text.Replace("</>", string.Empty).Replace("<\\>", string.Empty).Replace("<>\\", string.Empty)
			.Replace("＜＞", string.Empty);
		return text;
	}

	private static string StripResidualRichTextFragments(string text)
	{
		if (string.IsNullOrWhiteSpace(text))
		{
			return text;
		}
		text = LooseColorOpenFragmentRegex.Replace(text, string.Empty);
		text = LooseColorCloseFragmentRegex.Replace(text, string.Empty);
		text = LooseSizeOpenFragmentRegex.Replace(text, string.Empty);
		text = LooseSizeCloseFragmentRegex.Replace(text, string.Empty);
		text = PlaceholderTokenRegex.Replace(text, string.Empty);
		text = text.Replace("</<u>", string.Empty).Replace("</<b>", string.Empty).Replace("</<i>", string.Empty)
			.Replace("</<size>", string.Empty)
			.Replace("</<color>", string.Empty);
		return text;
	}

	private static bool IsTokenDominatedFragment(string text)
	{
		if (string.IsNullOrWhiteSpace(text) || text.IndexOf("__DS_TOKEN_", StringComparison.Ordinal) < 0)
		{
			return false;
		}
		return Regex.Replace(PlaceholderTokenRegex.Replace(text, string.Empty), "[\\s\\W_]+", string.Empty).Length <= 6;
	}

	private static string NormalizeDisplayWhitespace(string text)
	{
		if (string.IsNullOrWhiteSpace(text))
		{
			return text;
		}
		text = text.Replace("\r\n", "\n").Replace('\r', '\n');
		text = Regex.Replace(text, "[ \\t]+\\n", "\n");
		text = Regex.Replace(text, "\\n{3,}", "\n\n");
		text = Regex.Replace(text, "[ \\t]{2,}", " ");
		return text.Trim();
	}

	private static string NormalizeRequestText(string text)
	{
		if (string.IsNullOrWhiteSpace(text))
		{
			return string.Empty;
		}
		return string.Join(" ", text.Trim().Split((char[])null, StringSplitOptions.RemoveEmptyEntries));
	}

	private static string GetVisibleText(string text)
	{
		if (string.IsNullOrWhiteSpace(text))
		{
			return string.Empty;
		}
		return NormalizeRequestText(RichTextTagRegex.Replace(text, " ").Replace("\u200b", "").Replace("\ufeff", ""));
	}

	private static bool LooksLikeUiText(string text)
	{
		string visibleText = GetVisibleText(text);
		if (string.IsNullOrWhiteSpace(visibleText))
		{
			return false;
		}
		if (text.Count((char ch) => ch == '\n' || ch == '\r') > 0)
		{
			return false;
		}
		return visibleText.Length <= 36;
	}

	private static bool EndsWithSentenceBoundary(string text)
	{
		if (string.IsNullOrWhiteSpace(text))
		{
			return false;
		}
		string text2 = text.Trim();
		if (text2.Length == 0)
		{
			return false;
		}
		char c = text2[text2.Length - 1];
		if (c == '\u2026')
		{
			return true;
		}
		return c == '.' || c == '!' || c == '?' || c == '~' || c == '>' || c == ')' || c == ']' || c == '"' || c == '\'' || c == ':' || c == ';' || c == '’' || c == '”' || c == '。' || c == '】' || c == '！' || c == '）' || c == '：' || c == '；' || c == '？';
	}

	private static readonly Regex TrailingStatLineRegex = new Regex("^[-+]?\\d[\\d.,]*\\s*[A-Za-z%]{0,16}$", RegexOptions.Compiled);

	/* 对话框常在正文下附加费用/属性行。这些是最...UI 文本，不是未完成...	   打字机截断；判断正文完整性前必须剥离短小、含数字且不像句子的尾行...	   否则整段会被长期误判为片段而永不翻译...*/
	private static bool IsTrailingStatLine(string visibleLine)
	{
		if (TrailingStatLineRegex.IsMatch(visibleLine))
		{
			return true;
		}
		return visibleLine.Length <= 48 && visibleLine.Any(char.IsDigit) && !EndsWithSentenceBoundary(visibleLine);
	}

	private static string StripTrailingStatLines(string text)
	{
		int end = text.Length;
		while (end > 0)
		{
			int lineStart = text.LastIndexOf('\n', end - 1);
			string line = GetVisibleText(text.Substring(lineStart + 1, end - (lineStart + 1))).Trim();
			if (line.Length == 0 || IsTrailingStatLine(line))
			{
				if (lineStart < 0)
				{
					return text;
				}
				end = lineStart;
				continue;
			}
			break;
		}
		return (end == text.Length) ? text : text.Substring(0, end);
	}

	private static bool LooksLikeTypewriterFragment(string text)
	{
		if (string.IsNullOrWhiteSpace(text))
		{
			return false;
		}
		text = StripTrailingStatLines(text);
		string visibleText = GetVisibleText(text);
		if (string.IsNullOrWhiteSpace(visibleText))
		{
			return false;
		}
		if (Regex.IsMatch(text, @"^\s{2,}") || Regex.IsMatch(text, @"\s{8,}"))
		{
			return true;
		}
		string text2 = visibleText.TrimStart('^').Trim();
		if (text2.Length == 0)
		{
			return false;
		}
		if (HasUnclosedOuterDialogueQuote(text2))
		{
			return true;
		}
		if (text2[0] == '<' && text2.IndexOf('>') < 0)
		{
			return true;
		}
		if (text2.Length >= 8 && Regex.IsMatch(text2, @"^[a-z]\)"))
		{
			return true;
		}
		if (text2.Length >= 4 && Regex.IsMatch(text2, @"^[a-z]{1,2}\s+"))
		{
			return true;
		}
		if (text2.Length >= 14 && char.IsLower(text2[0]) && text2.Any(char.IsWhiteSpace))
		{
			return true;
		}
		bool flag = text2.Any((char ch) => ch >= 'a' && ch <= 'z');
		bool flag2 = text2.Any((char ch) => ch == '.' || ch == '!' || ch == '?' || ch == ',' || ch == ';' || ch == ':' || ch == '\u2018' || ch == '\u2019' || ch == '\u201C' || ch == '\u201D' || ch == '\u2026');
		bool flag3 = text2.IndexOf('\u2026') >= 0 || text2.IndexOf("...", StringComparison.Ordinal) >= 0;
		if (text2.Length >= 8 && flag && text2.EndsWith(",", StringComparison.Ordinal))
		{
			return true;
		}
		if (text2.Length >= 8 && flag && flag3 && !EndsWithSentenceBoundary(text2))
		{
			return true;
		}
		if (text2.Length >= 14 && flag && !EndsWithSentenceBoundary(text2) && Regex.IsMatch(text2, @"\b[A-Za-z]{1,3}$"))
		{
			return true;
		}
		if (text2.Length >= 10 && flag && flag2 && !EndsWithSentenceBoundary(text2) && Regex.IsMatch(text2, @"\b(?:[A-Za-z]{1,3}|a|an|the|to|of|for|from|into|with|no|not)$", RegexOptions.IgnoreCase))
		{
			return true;
		}
		if (text2.Length >= 42 && flag && flag2 && !EndsWithSentenceBoundary(text2))
		{
			return true;
		}
		if (text2.Length >= 24 && flag && !EndsWithSentenceBoundary(text2) && Regex.IsMatch(text2, @"\b(?:a|an|the|to|of|for|and|or|but|with|from|into|about|what|where|there|already|want|just|think|feel|feels|practicing|practice|people|world|field)$", RegexOptions.IgnoreCase))
		{
			return true;
		}
		return false;
	}

	private static bool HasUnclosedOuterDialogueQuote(string text)
	{
		if (string.IsNullOrWhiteSpace(text))
		{
			return false;
		}
		string trimmed = text.Trim();
		if (trimmed.Length == 0)
		{
			return false;
		}
		char opening = trimmed[0];
		char closing;
		switch (opening)
		{
		case '"':
			closing = '"';
			break;
		case '\u201c':
			closing = '\u201d';
			break;
		case '\u2018':
			closing = '\u2019';
			break;
		default:
			return false;
		}
		return trimmed.Length < 2 || trimmed[trimmed.Length - 1] != closing;
	}

	private static float GetTextSettleDelaySeconds(string text)
	{
		if (LooksLikeTypewriterFragment(text))
		{
			return TypewriterFragmentDebounceSeconds;
		}
		if (LooksLikeUiText(text))
		{
			return UiTextSettleDebounceSeconds;
		}
		return TextSettleDebounceSeconds;
	}

	private static string SanitizeRichTextForUnityText(string text)
	{
		if (string.IsNullOrWhiteSpace(text))
		{
			return text;
		}
		text = SanitizeTranslationArtifacts(text);
		if (!RichTextTagRegex.IsMatch(text))
		{
			return text;
		}
		return RichTextTagRegex.Replace(text, delegate(Match match)
		{
			string value = match.Value;
			if (value.Length <= 2)
			{
				return string.Empty;
			}
			string text2 = value.Substring(1, value.Length - 2).Trim();
			if (text2.Length == 0)
			{
				return string.Empty;
			}
			if (IsTmpColorShorthandTagBody(text2))
			{
				return value;
			}
			if (text2[0] == '/')
			{
				text2 = text2.Substring(1).TrimStart();
			}
			int num = text2.IndexOfAny(new char[3] { '=', ' ', '/' });
			string text3 = ((num >= 0) ? text2.Substring(0, num) : text2);
			if (text3.Equals("br", StringComparison.OrdinalIgnoreCase))
			{
				return "\n";
			}
			return (!UnityTextSupportedTags.Contains(text3)) ? string.Empty : value;
		});
	}

	private static bool IsTmpColorShorthandTagBody(string text)
	{
		if (string.IsNullOrWhiteSpace(text))
		{
			return false;
		}
		text = text.Trim();
		if (text.Length < 4 || text.Length > 9 || text[0] != '#')
		{
			return false;
		}
		for (int i = 1; i < text.Length; i++)
		{
			char c = text[i];
			if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F')))
			{
				return false;
			}
		}
		return true;
	}

	private static bool ContainsRichTextTags(string text)
	{
		if (!string.IsNullOrWhiteSpace(text))
		{
			return RichTextTagRegex.IsMatch(text);
		}
		return false;
	}

	private static bool ShouldPreserveRichTextForDisplay(string originalText, string translatedText)
	{
		if (!ContainsRichTextTags(translatedText))
		{
			return true;
		}
		string visibleText = GetVisibleText(originalText);
		if (string.IsNullOrWhiteSpace(visibleText))
		{
			visibleText = GetVisibleText(translatedText);
		}
		if (visibleText.Length > 36)
		{
			return false;
		}
		if ((originalText?.IndexOfAny(new char[2] { '\r', '\n' }) ?? (-1)) >= 0)
		{
			return false;
		}
		if ((translatedText?.IndexOfAny(new char[2] { '\r', '\n' }) ?? (-1)) >= 0)
		{
			return false;
		}
		return LooksLikeUiText(visibleText);
	}

	private static bool ShouldPreserveRichTextForDisplayWithColor(string originalText, string translatedText)
	{
		if (ContainsColorRichTextTag(translatedText))
		{
			return true;
		}
		if (TryGetOuterRichTextWrapper(originalText, out _, out _))
		{
			return true;
		}
		return ShouldPreserveRichTextForDisplay(originalText, translatedText);
	}

	private static bool TryEnableRichText(object component)
	{
		if (component == null)
		{
			return false;
		}
		bool result = false;
		try
		{
			Text val = (Text)((component is Text) ? component : null);
			if (val != null)
			{
				val.supportRichText = true;
				result = val.supportRichText;
			}
			Type type = component.GetType();
			PropertyInfo property = type.GetProperty("supportRichText", BindingFlags.Instance | BindingFlags.Public | BindingFlags.NonPublic);
			if ((object)property != null && property.CanWrite && property.PropertyType == typeof(bool))
			{
				property.SetValue(component, true);
				result = true;
			}
			PropertyInfo property2 = type.GetProperty("richText", BindingFlags.Instance | BindingFlags.Public | BindingFlags.NonPublic);
			if ((object)property2 != null && property2.CanWrite && property2.PropertyType == typeof(bool))
			{
				property2.SetValue(component, true);
				result = true;
			}
		}
		catch (Exception ex)
		{
			ReportCaughtException(ex);
			return result;
		}
		return result;
	}

	private static string PrepareTranslatedTextForComponent(object component, string text, string originalText = null)
	{
		text = RestoreLeadingTextDecoration(text, originalText);
		text = RestoreOuterRichTextWrapper(text, originalText);
		/* UGUI Text 只支持少量标签，必须过滤不支持的标签；TMP 支持完整标签集，
		   若复...UGUI 过滤会误...<mark> 等真实格式...*/
		string text2 = ((component is Text) ? SanitizeRichTextForUnityText(text) : SanitizeTranslationArtifacts(text));
		if (!ContainsRichTextTags(text2))
		{
			return text2;
		}
		if (!TryEnableRichText(component))
		{
			return RichTextTagRegex.Replace(text2, string.Empty);
		}
		return text2;
	}

	private static string PrepareTranslatedTextForUGUIText(Text component, string translated, string originalText = null, bool preserveRichText = true)
	{
		if (string.IsNullOrWhiteSpace(translated))
		{
			return translated;
		}
		string sourceText = originalText ?? translated;
		string prepared;
		if (preserveRichText && ShouldPreserveRichTextForDisplayWithColor(sourceText, translated))
		{
			prepared = PrepareTranslatedTextForComponent(component, translated, sourceText);
		}
		else
		{
			prepared = StripRichTextForPlainText(translated);
		}
		prepared = RestoreOuterLineBreaks(prepared, sourceText);
		PrepareUGUITranslatedShortLabelLayout(component, prepared, sourceText);
		prepared = ApplyUGUITextLayoutCompatibility(component, prepared);
		return PreserveUguiTypewriterSourceLength(component, prepared, sourceText);
	}

	private static string PreserveUguiTypewriterSourceLength(Text component, string prepared, string sourceText)
	{
		if ((Object)(object)component == (Object)null || string.IsNullOrEmpty(prepared) || string.IsNullOrEmpty(sourceText) || prepared.Length >= sourceText.Length)
		{
			return prepared;
		}
		string visibleSource = GetVisibleText(sourceText);
		if (visibleSource.Length < 24 || !LooksLikeNaturalLanguage(visibleSource))
		{
			return prepared;
		}
		component.supportRichText = true;
		StringBuilder builder = new StringBuilder(prepared, sourceText.Length + UguiTypewriterLengthPaddingTag.Length);
		while (builder.Length < sourceText.Length)
		{
			builder.Append(UguiTypewriterLengthPaddingTag);
		}
		return builder.ToString();
	}

	private static string RestoreOuterLineBreaks(string text, string originalText)
	{
		if (string.IsNullOrEmpty(text) || string.IsNullOrEmpty(originalText))
		{
			return text;
		}
		string leading = ExtractLeadingLineBreaks(originalText);
		string trailing = ExtractTrailingLineBreaks(originalText);
		if (leading.Length > 0 && !StartsWithLineBreak(text))
		{
			text = leading + text;
		}
		if (trailing.Length > 0 && !EndsWithLineBreak(text))
		{
			text += trailing;
		}
		return text;
	}

	private static string ExtractLeadingLineBreaks(string text)
	{
		if (string.IsNullOrEmpty(text))
		{
			return string.Empty;
		}
		StringBuilder stringBuilder = null;
		for (int i = 0; i < text.Length; i++)
		{
			char c = text[i];
			if (c != '\r' && c != '\n')
			{
				break;
			}
			if (stringBuilder == null)
			{
				stringBuilder = new StringBuilder();
			}
			stringBuilder.Append('\n');
			if (c == '\r' && i + 1 < text.Length && text[i + 1] == '\n')
			{
				i++;
			}
		}
		return stringBuilder?.ToString() ?? string.Empty;
	}

	private static string ExtractTrailingLineBreaks(string text)
	{
		if (string.IsNullOrEmpty(text))
		{
			return string.Empty;
		}
		int count = 0;
		for (int i = text.Length - 1; i >= 0; i--)
		{
			char c = text[i];
			if (c != '\r' && c != '\n')
			{
				break;
			}
			count++;
			if (c == '\n' && i > 0 && text[i - 1] == '\r')
			{
				i--;
			}
		}
		return count <= 0 ? string.Empty : new string('\n', count);
	}

	private static bool StartsWithLineBreak(string text)
	{
		return !string.IsNullOrEmpty(text) && (text[0] == '\r' || text[0] == '\n');
	}

	private static bool EndsWithLineBreak(string text)
	{
		return !string.IsNullOrEmpty(text) && (text[text.Length - 1] == '\r' || text[text.Length - 1] == '\n');
	}

	private static string ApplyUGUITextLayoutCompatibility(Text component, string text)
	{
		if ((Object)(object)component == (Object)null || string.IsNullOrEmpty(text) || !ContainsCjk(text))
		{
			return text;
		}
		try
		{
			string visibleText = GetVisibleText(text);
			RectTransform rectTransform = ((Graphic)component).rectTransform;
			Rect rect = ((Object)(object)rectTransform == (Object)null) ? default(Rect) : rectTransform.rect;
			if (rect.width > 24f && visibleText.Length > 4)
			{
				component.horizontalOverflow = HorizontalWrapMode.Wrap;
				text = WrapTranslatedCjkForUGUI(text, rect.width, Math.Max(1, component.fontSize));
			}
			((Graphic)component).SetAllDirty();
		}
		catch (Exception ex)
		{
			ReportCaughtException(ex);
			return text;
		}
		return text;
	}

	private static void RestoreUGUIShortLabelLayoutCompatibility(Text component, string keepAppliedText = null)
	{
		if ((Object)(object)component == (Object)null || !_uguiShortLabelLayouts.TryGetValue(component, out var state))
		{
			return;
		}
		if (!string.IsNullOrEmpty(keepAppliedText) && string.Equals(state.AppliedText, keepAppliedText, StringComparison.Ordinal))
		{
			return;
		}
		_uguiShortLabelLayouts.Remove(component);
		try
		{
			component.horizontalOverflow = state.OriginalMode;
			((Graphic)component).SetAllDirty();
			RectTransform rectTransform = ((Graphic)component).rectTransform;
			if ((Object)(object)rectTransform != (Object)null)
			{
				LayoutRebuilder.MarkLayoutForRebuild(rectTransform);
				RectTransform parentRect = ((Transform)rectTransform).parent as RectTransform;
				if ((Object)(object)parentRect != (Object)null)
				{
					LayoutRebuilder.MarkLayoutForRebuild(parentRect);
				}
			}
		}
		catch (Exception ex)
		{
			ReportCaughtException(ex, "renderer=UGUI restore-short-label-layout path=" + GetComponentLogPath(component));
		}
	}

	private static void PrepareUGUITranslatedShortLabelLayout(Text component, string translatedText, string sourceText)
	{
		if ((Object)(object)component == (Object)null || string.IsNullOrEmpty(translatedText) || string.IsNullOrEmpty(sourceText))
		{
			return;
		}
		string visibleText = GetVisibleText(translatedText);
		string sourceVisibleText = GetVisibleText(sourceText);
		if (_uguiShortLabelLayouts.TryGetValue(component, out var existingState) &&
			string.Equals(existingState.AppliedText, translatedText, StringComparison.Ordinal) &&
			component.horizontalOverflow == HorizontalWrapMode.Overflow)
		{
			return;
		}
		if (ContainsCjk(sourceVisibleText))
		{
			RestoreUGUIShortLabelLayoutCompatibility(component, translatedText);
			return;
		}
		RestoreUGUIShortLabelLayoutCompatibility(component);
		if (!ContainsCjk(visibleText) || visibleText.Length == 0 || visibleText.Length > 4 || sourceVisibleText.Length == 0 || sourceVisibleText.Length > 12 ||
			translatedText.IndexOf('\n') >= 0 || translatedText.IndexOf('\r') >= 0 || sourceText.IndexOf('\n') >= 0 || sourceText.IndexOf('\r') >= 0)
		{
			return;
		}
		try
		{
			RectTransform rectTransform = ((Graphic)component).rectTransform;
			if ((Object)(object)rectTransform == (Object)null || component.horizontalOverflow != HorizontalWrapMode.Wrap)
			{
				return;
			}
			RectTransform parentRect = ((Transform)rectTransform).parent as RectTransform;
			LayoutElement layoutElement = ((Component)component).GetComponent<LayoutElement>();
			bool parentOwnsWidth = (Object)(object)parentRect != (Object)null &&
				((Object)(object)((Component)parentRect).GetComponent<ContentSizeFitter>() != (Object)null ||
				 (Object)(object)((Component)parentRect).GetComponent<HorizontalOrVerticalLayoutGroup>() != (Object)null);
			if ((Object)(object)layoutElement == (Object)null || !parentOwnsWidth)
			{
				return;
			}

			_uguiShortLabelLayouts.Remove(component);
			_uguiShortLabelLayouts.Add(component, new UguiShortLabelLayoutState
			{
				OriginalMode = component.horizontalOverflow,
				AppliedText = translatedText
			});
			component.horizontalOverflow = HorizontalWrapMode.Overflow;
			((Graphic)component).SetAllDirty();
			LayoutRebuilder.MarkLayoutForRebuild(rectTransform);
			LayoutRebuilder.MarkLayoutForRebuild(parentRect);
			int count = Interlocked.Increment(ref _instance._uguiShortLabelOverflowCount);
			if (count <= 3 || (count & (count - 1)) == 0)
			{
				Rect rect = rectTransform.rect;
				_instance.Logger.LogInfo($"[UGUI-LAYOUT] short-label overflow occurrence={count} phase=prepare path={GetComponentLogPath(component)} rectWidth={rect.width:0.0} rectHeight={rect.height:0.0} sourceLength={sourceVisibleText.Length} visibleLength={visibleText.Length}");
			}
		}
		catch (Exception ex)
		{
			ReportCaughtException(ex, "renderer=UGUI prepare-short-label-layout path=" + GetComponentLogPath(component));
		}
	}

	private static void FinalizeUGUITextLayoutCompatibility(Text component)
	{
		if ((Object)(object)component == (Object)null || !_uguiShortLabelLayouts.TryGetValue(component, out var state))
		{
			return;
		}
		if (!string.Equals(component.text, state.AppliedText, StringComparison.Ordinal))
		{
			RestoreUGUIShortLabelLayoutCompatibility(component);
			return;
		}
		try
		{
			RectTransform rectTransform = ((Graphic)component).rectTransform;
			if ((Object)(object)rectTransform == (Object)null)
			{
				return;
			}
			LayoutRebuilder.MarkLayoutForRebuild(rectTransform);
			RectTransform parentRect = ((Transform)rectTransform).parent as RectTransform;
			if ((Object)(object)parentRect != (Object)null)
			{
				LayoutRebuilder.MarkLayoutForRebuild(parentRect);
			}

			component.horizontalOverflow = HorizontalWrapMode.Overflow;
			((Graphic)component).SetAllDirty();
			LayoutRebuilder.MarkLayoutForRebuild(rectTransform);
			if ((Object)(object)parentRect != (Object)null)
			{
				LayoutRebuilder.MarkLayoutForRebuild(parentRect);
			}
		}
		catch (Exception ex)
		{
			ReportCaughtException(ex, "renderer=UGUI short-label-layout path=" + GetComponentLogPath(component));
		}
	}

	private static string WrapTranslatedCjkForUGUI(string text, float rectWidth, int fontSize)
	{
		if (string.IsNullOrEmpty(text) || rectWidth <= 24f || fontSize <= 0)
		{
			return text;
		}
		int maxUnits = Math.Max(12, Math.Min(96, (int)Math.Floor(rectWidth / Math.Max(8f, (float)fontSize * 0.92f) * 2f)));
		if (GetVisibleText(text).Length <= maxUnits / 2)
		{
			return text;
		}
		StringBuilder stringBuilder = new StringBuilder(text.Length + 8);
		int lineUnits = 0;
		for (int i = 0; i < text.Length; i++)
		{
			char c = text[i];
			if (c == '<')
			{
				int tagEnd = text.IndexOf('>', i + 1);
				if (tagEnd > i)
				{
					stringBuilder.Append(text, i, tagEnd - i + 1);
					i = tagEnd;
					continue;
				}
			}
			if (c == '\r' || c == '\n')
			{
				stringBuilder.Append('\n');
				if (c == '\r' && i + 1 < text.Length && text[i + 1] == '\n')
				{
					i++;
				}
				lineUnits = 0;
				continue;
			}
			int charUnits = GetUGUIWrapCharUnits(c);
			if (lineUnits > 0 && charUnits > 0 && lineUnits + charUnits > maxUnits && !IsPoorLineStartPunctuation(c))
			{
				stringBuilder.Append('\n');
				lineUnits = 0;
				if (char.IsWhiteSpace(c))
				{
					continue;
				}
			}
			stringBuilder.Append(c);
			lineUnits += charUnits;
		}
		return stringBuilder.ToString();
	}

	private static int GetUGUIWrapCharUnits(char c)
	{
		if (char.IsControl(c))
		{
			return 0;
		}
		if (c <= '\u007f')
		{
			return char.IsWhiteSpace(c) ? 1 : 1;
		}
		return 2;
	}

	private static bool IsPoorLineStartPunctuation(char c)
	{
		return c == '。' || c == '，' || c == '、' || c == '；' || c == '：' || c == '！' || c == '？' || c == ')' || c == '）' || c == ']' || c == '】' || c == '”' || c == '’' || c == '…';
	}

	private static string PrepareTranslatedTextForString(string translated, string originalText = null)
	{
		if (string.IsNullOrWhiteSpace(translated))
		{
			return translated;
		}
		translated = RestoreLeadingTextDecoration(translated, originalText);
		return SanitizeRichTextForUnityText(RestoreOuterRichTextWrapper(translated, originalText));
	}

	private string NormalizeTmpPunctuationForMissingGlyphs(string text)
	{
		if (string.IsNullOrEmpty(text) || _chineseTMPFont == null || text.IndexOfAny(TmpPunctuationFallbackChars) < 0)
		{
			return text;
		}
		TryWarmTmpFontAssetVerified(_chineseTMPFont, text, out var _);
		List<int> list = CollectMissingChars(_chineseTMPFont, GetVisibleText(text));
		if (list.Count == 0)
		{
			return text;
		}
		HashSet<int> missing = new HashSet<int>(list);
		StringBuilder stringBuilder = null;
		for (int i = 0; i < text.Length; i++)
		{
			char c = text[i];
			if (!missing.Contains(c) || !TryGetTmpPunctuationFallback(c, out var replacement))
			{
				if (stringBuilder != null)
				{
					stringBuilder.Append(c);
				}
				continue;
			}
			if (stringBuilder == null)
			{
				stringBuilder = new StringBuilder(text.Length);
				if (i > 0)
				{
					stringBuilder.Append(text, 0, i);
				}
			}
			stringBuilder.Append(replacement);
		}
		return stringBuilder?.ToString() ?? text;
	}

	private static bool TryGetTmpPunctuationFallback(char c, out string replacement)
	{
		switch (c)
		{
		case '\u3001':
		case '\uff0c':
			replacement = ",";
			return true;
		case '\u3002':
			replacement = ".";
			return true;
		case '\uff1f':
			replacement = "?";
			return true;
		case '\uff01':
			replacement = "!";
			return true;
		case '\uff1a':
			replacement = ":";
			return true;
		case '\uff1b':
			replacement = ";";
			return true;
		case '\uff08':
			replacement = "(";
			return true;
		case '\uff09':
			replacement = ")";
			return true;
		case '\u3010':
			replacement = "[";
			return true;
		case '\u3011':
			replacement = "]";
			return true;
		case '\u201c':
		case '\u201d':
			replacement = "\"";
			return true;
		case '\u2018':
		case '\u2019':
			replacement = "'";
			return true;
		case '\u2026':
			replacement = "...";
			return true;
		case '\u2014':
			replacement = "-";
			return true;
		case '\u25cf':
			replacement = "*";
			return true;
		case '\u25a1':
			replacement = " ";
			return true;
		case '\u00b7':
			replacement = "-";
			return true;
		default:
			replacement = null;
			return false;
		}
	}

	private static string StripRichTextForPlainText(string text)
	{
		if (string.IsNullOrWhiteSpace(text))
		{
			return text;
		}
		string input = SanitizeTranslationArtifacts(text);
		input = Regex.Replace(input, "</?br\\s*/?>", "\n", RegexOptions.IgnoreCase);
		input = RichTextTagRegex.Replace(input, string.Empty);
		input = StripResidualRichTextFragments(input);
		return NormalizeDisplayWhitespace(input);
	}

	private static bool IsUpperAsciiWord(string text)
	{
		if (!string.IsNullOrWhiteSpace(text) && text.Any(char.IsLetter))
		{
			return text.All((char ch) => !char.IsLetter(ch) || char.IsUpper(ch));
		}
		return false;
	}

	private static bool LooksLikeRuntimeStatusText(string visibleText)
	{
		if (string.IsNullOrWhiteSpace(visibleText))
		{
			return true;
		}
		string text = NormalizeRequestText(visibleText).Trim();
		if (string.IsNullOrWhiteSpace(text))
		{
			return true;
		}
		if (RuntimeVersionTextRegex.IsMatch(text) || RuntimeStatusPrefixRegex.IsMatch(text) || RuntimeResolutionTextRegex.IsMatch(text))
		{
			return true;
		}
		switch (text.ToLowerInvariant())
		{
		case "preloading":
		case "preloading content":
		case "loading level":
		case "loading scene":
		case "loading assets":
		case "loading asset":
		case "entering":
			return true;
		default:
			return false;
		}
	}

	private static bool ShouldSkipText(string text)
	{
		string visibleText = GetVisibleText(text);
		if (string.IsNullOrWhiteSpace(visibleText))
		{
			return true;
		}
		if (text.Count((char ch) => ch == '\n' || ch == '\r') >= 3 && visibleText.Length <= 12)
		{
			return true;
		}
		if (ContainsCjk(visibleText))
		{
			return true;
		}
		if (!visibleText.Any(char.IsLetter))
		{
			return true;
		}
		if (visibleText.Length == 1)
		{
			return true;
		}
		if (LooksLikeRuntimeStatusText(visibleText))
		{
			return true;
		}
		bool flag = visibleText.Any(char.IsLower);
		bool flag2 = visibleText.Any((char ch) => ch == '.' || ch == '!' || ch == '?' || ch == '~' || ch == '\'' || ch == '"' || ch == '…' || ch == '。' || ch == '！' || ch == '？');
		bool flag3 = visibleText.All((char ch) => char.IsUpper(ch) || char.IsDigit(ch) || ch == '/' || ch == '+' || ch == '-');
		if (visibleText.Length <= 2 && flag3 && !flag && !flag2)
		{
			return true;
		}
		if (visibleText.Length <= 3 && IsUpperAsciiWord(visibleText) && !flag && !flag2)
		{
			return true;
		}
		int num = visibleText.Count(char.IsLetter);
		if (num > 0 && num <= 2)
		{
			if (RichTextTagRegex.IsMatch(text) || visibleText.Any(char.IsDigit) || visibleText.Any(char.IsWhiteSpace))
			{
				return true;
			}
			if (visibleText.Length <= 2 && !flag && !flag2 && IsUpperAsciiWord(visibleText))
			{
				return true;
			}
		}
		return false;
	}

	private static bool IsAcceptableTranslation(string original, string translated)
	{
		if (string.IsNullOrWhiteSpace(original) || string.IsNullOrWhiteSpace(translated))
		{
			return false;
		}
		if (ShouldSkipText(original) && !IsShortLocalTranslationAllowed(original, translated))
		{
			return false;
		}
		string a = NormalizeRequestText(original);
		string b = NormalizeRequestText(translated);
		if (string.Equals(a, b, StringComparison.Ordinal))
		{
			return false;
		}
		string visibleText = GetVisibleText(original);
		string visibleText2 = GetVisibleText(translated);
		if (string.IsNullOrWhiteSpace(visibleText2))
		{
			return false;
		}
		if (!ContainsCjk(visibleText2) && visibleText.Count((char ch) => (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z')) >= 4 && visibleText.Any((char ch) => ch >= 'a' && ch <= 'z'))
		{
			return false;
		}
		if (visibleText.Length >= 12 && visibleText2.Length <= 1)
		{
			return false;
		}
		if (visibleText.Length >= 24 && visibleText2.Length < Math.Max(3, visibleText.Length / 6))
		{
			return false;
		}
		if (HasSuspiciousEnglishResidue(original, translated))
		{
			return false;
		}
		return true;
	}

	private static bool IsShortLocalTranslationAllowed(string original, string translated)
	{
		string visibleText = GetVisibleText(original);
		if (visibleText.Length >= 2 && visibleText.Length <= 4 && visibleText.Any(char.IsLetter))
		{
			return ContainsCjk(translated);
		}
		return false;
	}

	private static bool HasSuspiciousEnglishResidue(string original, string translated)
	{
		string visibleText = GetVisibleText(original);
		string visibleText2 = GetVisibleText(translated);
		if (visibleText.Length < 12 || !ContainsCjk(visibleText2))
		{
			return false;
		}
		foreach (Match item in LatinWordRegex.Matches(visibleText2))
		{
			string value = item.Value;
			if (!IsAllowedLatinResidue(value, visibleText))
			{
				return true;
			}
		}
		return false;
	}

	private static bool IsAllowedLatinResidue(string word, string originalVisibleText)
	{
		if (string.IsNullOrWhiteSpace(word))
		{
			return false;
		}
		if (word.All(char.IsUpper) || AllowedLatinResidue.Contains(word))
		{
			return true;
		}
		if (string.IsNullOrWhiteSpace(originalVisibleText))
		{
			return false;
		}
		foreach (Match match in LatinWordRegex.Matches(originalVisibleText))
		{
			string sourceWord = match.Value;
			if (LatinResidueMatchesSourceWord(word, sourceWord) && IsLikelyProtectedLatinTerm(sourceWord))
			{
				return true;
			}
		}
		return false;
	}

	/* 中文没有英语复数形态，受保护术语可能合法地由复数变单数。允许长度差不超
	   2 的词干级匹配，避免把这类译文长期误判为英文残留...*/
	private static bool LatinResidueMatchesSourceWord(string residueWord, string sourceWord)
	{
		if (string.Equals(sourceWord, residueWord, StringComparison.OrdinalIgnoreCase))
		{
			return true;
		}
		string shorter = (residueWord.Length <= sourceWord.Length) ? residueWord : sourceWord;
		string longer = (residueWord.Length <= sourceWord.Length) ? sourceWord : residueWord;
		if (shorter.Length < 3 || longer.Length - shorter.Length > 2)
		{
			return false;
		}
		return longer.StartsWith(shorter, StringComparison.OrdinalIgnoreCase);
	}

	private static bool IsLikelyProtectedLatinTerm(string word)
	{
		if (string.IsNullOrWhiteSpace(word))
		{
			return false;
		}
		if (word.All(char.IsUpper) || AllowedLatinResidue.Contains(word))
		{
			return true;
		}
		if (word.Any(char.IsDigit))
		{
			return true;
		}
		if (word.Skip(1).Any(char.IsUpper))
		{
			return true;
		}
		if (char.IsUpper(word[0]) && word.Skip(1).Any(char.IsLower) && !CommonCapitalizedEnglishWords.Contains(word))
		{
			return true;
		}
		return false;
	}

	private bool WasProcessed(int instanceId, string rawText)
	{
		lock (_pendingLock)
		{
			string value;
			return _translatedComponents.TryGetValue(instanceId, out value) && string.Equals(value, NormalizeRequestText(rawText), StringComparison.Ordinal);
		}
	}

	private int GetTranslatedComponentCount()
	{
		lock (_pendingLock)
		{
			return _translatedComponents.Count;
		}
	}

	private int GetCacheCount()
	{
		lock (_cache)
		{
			return _cache.Count;
		}
	}

	private int GetGlossaryCount()
	{
		lock (_cache)
		{
			return _glossary.Count;
		}
	}

	private int GetInProgressCount()
	{
		lock (_pendingLock)
		{
			return _inProgress.Count;
		}
	}

	private void MarkProcessed(int instanceId, string rawText)
	{
		lock (_pendingLock)
		{
			_translatedComponents[instanceId] = NormalizeRequestText(rawText);
		}
	}

	private void ClearProcessedIfChanged(int instanceId, string newText)
	{
		lock (_pendingLock)
		{
			if (_translatedComponents.TryGetValue(instanceId, out var value))
			{
				string b = NormalizeRequestText(newText);
				if (!string.Equals(value, b, StringComparison.Ordinal))
				{
					_translatedComponents.Remove(instanceId);
				}
			}
		}
	}

	private void ResetSceneScopedState(bool clearPendingComponentWork)
	{
		/* 场景对象状态不能跨场景复用。翻...cache/glossary 是进程级资产...		   因此这里仅回收“场景代”的组件 ID、预取队列和渲染器状态，不清共享译文...		   大场景退出时顺便收缩深扫描集合，避免 Clear 后仍永久保留峰值桶数组...*/
		lock (_pendingLock)
		{
			bool trimDeepScannedObjects = _deepScannedObjects.Count > MaxTrackedComponentStates;
			bool trimDeepPrefetchSeen = _deepPrefetchSeen.Count > MaxDeepPrefetchSeen;
			bool trimDeepPrefetchQueue = _deepPrefetchQueue.Count > MaxDeepPrefetchQueue;
			_translatedComponents.Clear();
			_canvasGroupVisibleStates.Clear();
			/* DontDestroyOnLoad 的 TMP overlay 会跨场景存活；清空注册表前先把存活
			   state 的 registered 复位，否则 RegisterTmpOverlay 会永远拒绝重新注册，
			   这些 overlay 将永久失去周期校验。 */
			foreach (WeakReference overlayRef in _activeTmpOverlays)
			{
				if (overlayRef.Target is TmpOverlayState liveOverlay && (Object)(object)liveOverlay != (Object)null)
				{
					liveOverlay.registered = false;
				}
			}
			_activeTmpOverlays.Clear();
			_deepScannedObjects.Clear();
			_deepPrefetchSeen.Clear();
			_deepPrefetchQueue.Clear();
			_fungusPrefetchSeen.Clear();
			if (trimDeepScannedObjects)
			{
				_deepScannedObjects.TrimExcess();
			}
			if (trimDeepPrefetchSeen)
			{
				_deepPrefetchSeen.TrimExcess();
			}
			if (trimDeepPrefetchQueue)
			{
				_deepPrefetchQueue.TrimExcess();
			}
			if (clearPendingComponentWork)
			{
				_debouncedTextRequests.Clear();
				_pendingApplyQueue.Clear();
				_pendingApplyKeys.Clear();
				_inProgress.Clear();
				_inProgressSources.Clear();
			}
		}
		lock (HistoryComponentCache)
		{
			/* InstanceID 可被新场景复用，跨场景保留不仅占内存，还可能命中旧分类...*/
			HistoryComponentCache.Clear();
		}
		lock (_softMaskRefreshedOnce)
		{
			_softMaskRefreshedOnce.Clear();
		}
	}

	private void PruneLongRunningStateIfNeeded()
	{
		float realtimeSinceStartup = Time.realtimeSinceStartup;
		if (realtimeSinceStartup - _lastStatePruneRealtime < StatePruneIntervalSeconds)
		{
			return;
		}
		_lastStatePruneRealtime = realtimeSinceStartup;
		lock (_pendingLock)
		{
			if (_translatedComponents.Count > MaxTrackedComponentStates)
			{
				_translatedComponents.Clear();
			}
			if (_canvasGroupVisibleStates.Count > MaxTrackedComponentStates)
			{
				_canvasGroupVisibleStates.Clear();
			}
			if (_deepScannedObjects.Count > MaxTrackedComponentStates)
			{
				_deepScannedObjects.Clear();
				_deepScannedObjects.TrimExcess();
			}
			if (_debouncedTextRequests.Count > MaxPendingComponentWork)
			{
				PruneDebouncedTextRequestsLocked(realtimeSinceStartup);
			}
			if (_pendingApplyQueue.Count > MaxPendingComponentWork)
			{
				PrunePendingApplyQueueLocked();
			}
			if (_inProgress.Count > MaxPendingComponentWork)
			{
				_inProgress.Clear();
				_inProgressSources.Clear();
			}
			if (_deepPrefetchQueue.Count > MaxDeepPrefetchQueue)
			{
				while (_deepPrefetchQueue.Count > MaxDeepPrefetchQueue)
				{
					_deepPrefetchQueue.Dequeue();
				}
			}
			if (_deepPrefetchSeen.Count > MaxDeepPrefetchSeen)
			{
				_deepPrefetchSeen.Clear();
			}
			if (_warmupRequestedSources.Count > MaxDeepPrefetchSeen)
			{
				_warmupRequestedSources.Clear();
			}
		}
		lock (_translationRetryCooldownLock)
		{
			if (_translationRetryCooldowns.Count > 0)
			{
				DateTime now = DateTime.UtcNow;
				foreach (string key in _translationRetryCooldowns.Where((KeyValuePair<string, DateTime> pair) => pair.Value <= now).Select((KeyValuePair<string, DateTime> pair) => pair.Key).ToList())
				{
					_translationRetryCooldowns.Remove(key);
				}
				if (_translationRetryCooldowns.Count > MaxPendingComponentWork)
				{
					_translationRetryCooldowns.Clear();
				}
			}
			if (_translationRejectCounts.Count > MaxPendingComponentWork)
			{
				_translationRejectCounts.Clear();
			}
			if (_translationRetryAbandoned.Count > MaxPendingComponentWork)
			{
				_translationRetryAbandoned.Clear();
			}
		}
		lock (_softMaskRefreshedOnce)
		{
			if (_softMaskRefreshedOnce.Count > MaxTrackedComponentStates)
			{
				_softMaskRefreshedOnce.Clear();
			}
		}
		lock (_atlasMissLogLock)
		{
			if (_atlasMissLogged.Count > MaxTrackedComponentStates)
			{
				_atlasMissLogged.Clear();
			}
		}
	}

	private void PruneDebouncedTextRequestsLocked(float now)
	{
		List<int> list = null;
		foreach (KeyValuePair<int, DebouncedTextRequest> item in _debouncedTextRequests)
		{
			object target = item.Value.ComponentRef?.Target;
			if (!IsUnityObjectAlive(target) || now - item.Value.UpdatedAt > 10f)
			{
				(list ?? (list = new List<int>())).Add(item.Key);
			}
		}
		if (list != null)
		{
			foreach (int item2 in list)
			{
				_debouncedTextRequests.Remove(item2);
			}
		}
		if (_debouncedTextRequests.Count > MaxPendingComponentWork)
		{
			_debouncedTextRequests.Clear();
		}
	}

	private void PrunePendingApplyQueueLocked()
	{
		_pendingApplyQueue.RemoveAll((PendingTranslationApply item) => !IsUnityObjectAlive(item.ComponentRef?.Target));
		if (_pendingApplyQueue.Count > MaxPendingComponentWork)
		{
			_pendingApplyQueue.RemoveRange(0, _pendingApplyQueue.Count - MaxPendingComponentWork);
		}
		_pendingApplyKeys.Clear();
		foreach (PendingTranslationApply item in _pendingApplyQueue)
		{
			_pendingApplyKeys.Add(BuildPendingApplyKey(item.InstanceId, item.OriginalText));
		}
	}

	private bool IsInProgress(int instanceId, string rawText = null)
	{
		lock (_pendingLock)
		{
			if (!_inProgress.Contains(instanceId))
			{
				return false;
			}
			if (rawText == null)
			{
				return true;
			}
			string value;
			return _inProgressSources.TryGetValue(instanceId, out value) && string.Equals(value, NormalizeRequestText(rawText), StringComparison.Ordinal);
		}
	}

	private bool TryMarkInProgress(int instanceId, string rawText = null)
	{
		string text = ((rawText == null) ? null : NormalizeRequestText(rawText));
		lock (_pendingLock)
		{
			if (_inProgress.Contains(instanceId))
			{
				if (text == null)
				{
					return false;
				}
				if (_inProgressSources.TryGetValue(instanceId, out var value) && string.Equals(value, text, StringComparison.Ordinal))
				{
					return false;
				}
			}
			_inProgress.Add(instanceId);
			if (text != null)
			{
				_inProgressSources[instanceId] = text;
			}
			return true;
		}
	}

	private void ClearInProgress(int instanceId, string rawText = null)
	{
		string text = ((rawText == null) ? null : NormalizeRequestText(rawText));
		lock (_pendingLock)
		{
			if (text == null || !_inProgressSources.TryGetValue(instanceId, out var value) || string.Equals(value, text, StringComparison.Ordinal))
			{
				_inProgress.Remove(instanceId);
				_inProgressSources.Remove(instanceId);
			}
		}
	}

	private void QueueDebouncedTextRequest(object component, int instanceId, string text, bool isTmp, bool preserveRichText = true)
	{
		if (component == null || string.IsNullOrWhiteSpace(text) || ContainsCjk(text) || ShouldSkipText(text) || (isTmp && !CanHandleTmp()) || IsTranslationRetryCoolingDown(text))
		{
			return;
		}
		bool lowPriority = IsLikelyHistoryComponent(component);
		if (IsRemoteQueueSaturated(text, lowPriority))
		{
			return;
		}
		lock (_pendingLock)
		{
			if (_debouncedTextRequests.TryGetValue(instanceId, out var existing) &&
				existing.IsTmp == isTmp &&
				ReferenceEquals(existing.ComponentRef?.Target, component) &&
				IsSameSourceText(existing.Text, text))
			{
				existing.PreserveRichText = preserveRichText;
				existing.LowPriority = lowPriority;
				return;
			}
			_debouncedTextRequests[instanceId] = new DebouncedTextRequest
			{
				ComponentRef = new WeakReference(component),
				InstanceId = instanceId,
				Text = text,
				IsTmp = isTmp,
				PreserveRichText = preserveRichText,
				LowPriority = lowPriority,
				UpdatedAt = Time.realtimeSinceStartup
			};
		}
	}

	private static string GetCurrentComponentText(object component, bool isTmp)
	{
		try
		{
			if (!isTmp)
			{
				Text val = (Text)((component is Text) ? component : null);
				if (val != null)
				{
					return val.text;
				}
			}
			return (component?.GetType().GetProperty("text", BindingFlags.Instance | BindingFlags.Public | BindingFlags.NonPublic))?.GetValue(component) as string;
		}
		catch (Exception ex)
		{
			ReportCaughtException(ex);
			return null;
		}
	}

	private void FlushDebouncedTextRequests()
	{
		List<DebouncedTextRequest> list = null;
		float realtimeSinceStartup = Time.realtimeSinceStartup;
		lock (_pendingLock)
		{
			if (_debouncedTextRequests.Count == 0)
			{
				return;
			}
			foreach (KeyValuePair<int, DebouncedTextRequest> item in _debouncedTextRequests)
			{
				DebouncedTextRequest value = item.Value;
				if (!(realtimeSinceStartup - value.UpdatedAt < GetTextSettleDelaySeconds(value.Text)))
				{
					if (list == null)
					{
						list = new List<DebouncedTextRequest>();
					}
					list.Add(value);
					if (list.Count >= GetMaxDebouncedStartsPerTick())
					{
						break;
					}
				}
			}
			if (list != null)
			{
				foreach (DebouncedTextRequest ready in list)
				{
					_debouncedTextRequests.Remove(ready.InstanceId);
				}
			}
		}
		if (list == null || list.Count == 0)
		{
			return;
		}
		foreach (DebouncedTextRequest item2 in list)
		{
			if (item2.IsTmp && !CanHandleTmp())
			{
				continue;
			}
			object obj = item2.ComponentRef?.Target;
			if (!IsUnityObjectAlive(obj))
			{
				continue;
			}
			string currentComponentText = GetCurrentComponentText(obj, item2.IsTmp);
			if (!IsSameSourceText(currentComponentText, item2.Text))
			{
				continue;
			}
			if (TryGetLocalTranslation(item2.Text, out var translated))
			{
				if (item2.IsTmp)
				{
					PropertyInfo property = obj.GetType().GetProperty("text", BindingFlags.Instance | BindingFlags.Public | BindingFlags.NonPublic);
					ApplyTMProTranslation(obj, item2.InstanceId, item2.Text, translated, property, null, null, item2.PreserveRichText);
					continue;
				}
				Text val = (Text)((obj is Text) ? obj : null);
				if (val != null)
				{
					val.text = PrepareTranslatedTextForUGUIText(val, translated, item2.Text, item2.PreserveRichText);
					if ((Object)(object)_chineseFont != (Object)null)
					{
						val.font = _chineseFont;
					}
					MarkProcessed(item2.InstanceId, item2.Text);
					TryMarkAppliedCacheKeyForPersist(item2.Text, translated);
				}
			}
			else if (IsTranslationRetryCoolingDown(item2.Text))
			{
				continue;
			}
			else if (IsRemoteQueueSaturated(item2.Text, item2.LowPriority))
			{
				QueueDebouncedTextRequest(obj, item2.InstanceId, item2.Text, item2.IsTmp, item2.PreserveRichText);
			}
			else if (TryMarkInProgress(item2.InstanceId, item2.Text))
			{
				_asyncScheduled++;
				ScheduleAsyncApply(obj, item2.InstanceId, item2.Text, item2.IsTmp, item2.PreserveRichText, item2.LowPriority);
			}
		}
	}

	private static bool IsSameSourceText(string current, string expected)
	{
		return string.Equals(NormalizeRequestText(current), NormalizeRequestText(expected), StringComparison.Ordinal);
	}

	private static string BuildPendingApplyKey(int instanceId, string originalText)
	{
		return $"{instanceId}\n{NormalizeRequestText(originalText)}";
	}

	private void QueueTranslationApply(object component, int instanceId, string originalText, string translatedText, bool isTmp, bool preserveRichText = true)
	{
		if ((isTmp && !CanHandleTmp()) || !IsAcceptableTranslation(originalText, translatedText))
		{
			return;
		}
		lock (_pendingLock)
		{
			string item = BuildPendingApplyKey(instanceId, originalText);
			if (_pendingApplyKeys.Add(item))
			{
				_pendingApplyQueue.Add(new PendingTranslationApply
				{
					ComponentRef = new WeakReference(component),
					InstanceId = instanceId,
					OriginalText = originalText,
					TranslatedText = translatedText,
					IsTmp = isTmp,
					PreserveRichText = preserveRichText
				});
			}
		}
	}

	private void FlushPendingTranslations()
	{
		List<PendingTranslationApply> range;
		lock (_pendingLock)
		{
			if (_pendingApplyQueue.Count == 0)
			{
				return;
			}
			int count = Math.Min(GetMaxPendingApplyPerFlush(), _pendingApplyQueue.Count);
			range = _pendingApplyQueue.GetRange(0, count);
			_pendingApplyQueue.RemoveRange(0, count);
			foreach (PendingTranslationApply item in range)
			{
				_pendingApplyKeys.Remove(BuildPendingApplyKey(item.InstanceId, item.OriginalText));
			}
		}
		foreach (PendingTranslationApply item2 in range)
		{
			object obj = item2.ComponentRef?.Target;
			if (!IsUnityObjectAlive(obj))
			{
				_flushSkipped++;
				continue;
			}
			try
			{
				if (item2.IsTmp)
				{
					if (CanHandleTmp())
					{
						PropertyInfo propertyInfo = AccessTools.Property(obj.GetType(), "text");
						string text = propertyInfo?.GetValue(obj) as string;
						string originalText = item2.OriginalText;
						string translatedText = item2.TranslatedText;
						if (!IsSameSourceText(text, item2.OriginalText) && (string.IsNullOrWhiteSpace(text) || !IsSameSourceText(text, item2.TranslatedText)))
						{
							if (!TryRepairMixedTranslatedText(text, out var repairedOriginal, out var repairedTranslated) || !IsSameSourceText(repairedOriginal, item2.OriginalText))
							{
								RestoreTmpOverlay(obj);
								continue;
							}
							originalText = repairedOriginal;
							translatedText = repairedTranslated;
						}
						ApplyTMProTranslation(obj, item2.InstanceId, originalText, translatedText, propertyInfo, null, null, item2.PreserveRichText);
						_flushApplied++;
					}
				}
				else
				{
					Text val = (Text)((obj is Text) ? obj : null);
					if (val != null)
					{
						string originalText2 = item2.OriginalText;
						string translatedText2 = item2.TranslatedText;
						if (!IsSameSourceText(val.text, item2.OriginalText))
						{
							if (!TryRepairMixedTranslatedText(val.text, out var repairedOriginal2, out var repairedTranslated2) || !IsSameSourceText(repairedOriginal2, item2.OriginalText))
							{
								continue;
							}
							originalText2 = repairedOriginal2;
							translatedText2 = repairedTranslated2;
						}
						ApplyFont(val);
						val.text = PrepareTranslatedTextForUGUIText(val, translatedText2, originalText2, item2.PreserveRichText);
						MarkProcessed(item2.InstanceId, originalText2);
						TryMarkAppliedCacheKeyForPersist(originalText2, translatedText2);
					}
				}
			}
			catch (Exception ex)
			{
				base.Logger.LogWarning("Queued apply failed (" + obj.GetType().FullName + "): " + (ex.InnerException?.Message ?? ex.Message));
			}
		}
	}

	private static void RevealTmpText(object target, string text = null)
	{
		if (target == null)
		{
			return;
		}
		try
		{
			Type type = target.GetType();
			int num = Math.Max(99999, GetVisibleText(text).Length + 64);
			SetTmpIntProperty(type, target, "maxVisibleCharacters", num);
			SetTmpIntProperty(type, target, "maxVisibleWords", num);
			SetTmpIntProperty(type, target, "maxVisibleLines", num);
		}
		catch (Exception reportedException)
		{
			ReportCaughtException(reportedException);
		}
	}

	private static void SetTmpIntProperty(Type type, object target, string propertyName, int value)
	{
		try
		{
			PropertyInfo propertyInfo = AccessTools.Property(type, propertyName);
			if (propertyInfo != null && propertyInfo.CanWrite && propertyInfo.PropertyType == typeof(int))
			{
				object obj = propertyInfo.GetValue(target);
				if (obj is int num && num < value)
				{
					propertyInfo.SetValue(target, value);
				}
			}
		}
		catch (Exception reportedException)
		{
			ReportCaughtException(reportedException);
		}
	}

	private static void InvokeForceMeshUpdate(object target, Type compType = null)
	{
		if (target == null)
		{
			return;
		}
		compType = compType ?? target.GetType();
		try
		{
			MethodInfo methodInfo = AccessTools.Method(compType, "ForceMeshUpdate", new Type[2]
			{
				typeof(bool),
				typeof(bool)
			});
			if (methodInfo != null)
			{
				methodInfo.Invoke(target, new object[2] { false, true });
				return;
			}
			MethodInfo methodInfo2 = AccessTools.Method(compType, "ForceMeshUpdate", new Type[1] { typeof(bool) });
			if (methodInfo2 != null)
			{
				methodInfo2.Invoke(target, new object[1] { true });
			}
			else
			{
				AccessTools.Method(compType, "ForceMeshUpdate", Type.EmptyTypes)?.Invoke(target, null);
			}
		}
		catch (Exception reportedException)
		{
			ReportCaughtException(reportedException);
		}
	}

	private void ApplyFont(object component)
	{
		/* 字体处理严格留在 Unity Mono 渲染层：UGUI 直接...Font，TMP ...		   fallback/动态图...覆盖层链路；不会改写 _cache 中的译文...*/
		if (!IsUnityObjectAlive(component))
		{
			return;
		}
		try
		{
			Text val = (Text)((component is Text) ? component : null);
			if (val != null)
			{
				if ((Object)(object)_chineseFont != (Object)null && (Object)(object)val.font != (Object)(object)_chineseFont)
				{
					val.font = _chineseFont;
				}
			}
			else
			{
				ApplyTMPFont(component);
			}
		}
		catch (Exception ex)
		{
			base.Logger.LogWarning("[FONT] ApplyFont failed: " + ex.Message);
		}
	}

	private static IList GetTmpFallbackList(object fontAsset)
	{
		if (fontAsset == null)
		{
			return null;
		}
		Type type = fontAsset.GetType();
		string[] array = new string[3] { "fallbackFontAssetTable", "fallbackFontAssets", "fallbackFontTable" };
		foreach (string name in array)
		{
			try
			{
				PropertyInfo propertyInfo = AccessTools.Property(type, name);
				if (propertyInfo != null && propertyInfo.GetValue(fontAsset) is IList result)
				{
					return result;
				}
				FieldInfo fieldInfo = AccessTools.Field(type, name);
				if (fieldInfo != null && fieldInfo.GetValue(fontAsset) is IList result2)
				{
					return result2;
				}
			}
			catch (Exception reportedException)
			{
				ReportCaughtException(reportedException);
			}
		}
		return null;
	}

	private static bool TryAddCharactersToTmpFontAsset(object fontAsset, string text)
	{
		if (fontAsset == null || string.IsNullOrWhiteSpace(text))
		{
			return false;
		}
		string visibleText = GetVisibleText(text);
		if (string.IsNullOrWhiteSpace(visibleText))
		{
			return false;
		}
		try
		{
			MethodInfo method = fontAsset.GetType().GetMethod("TryAddCharacters", new Type[3]
			{
				typeof(string),
				typeof(string).MakeByRefType(),
				typeof(bool)
			});
			if (method != null)
			{
				object[] parameters = new object[3] { visibleText, null, true };
				if (method.Invoke(fontAsset, parameters) is bool result)
				{
					return result;
				}
			}
		}
		catch (Exception reportedException)
		{
			ReportCaughtException(reportedException);
		}
		try
		{
			MethodInfo method2 = fontAsset.GetType().GetMethod("TryAddCharacters", new Type[2]
			{
				typeof(string),
				typeof(bool)
			});
			if (method2 != null)
			{
				object[] parameters2 = new object[2] { visibleText, true };
				if (method2.Invoke(fontAsset, parameters2) is bool result2)
				{
					return result2;
				}
			}
		}
		catch (Exception reportedException)
		{
			ReportCaughtException(reportedException);
		}
		return false;
	}

	private static void TryWarmTmpFontAsset(object fontAsset, string text)
	{
		TryWarmTmpFontAssetVerified(fontAsset, text, out var _);
	}

	private static bool TryWarmTmpFontAssetVerified(object fontAsset, string text, out int missingCount)
	{
		missingCount = 0;
		if (fontAsset == null || string.IsNullOrEmpty(text))
		{
			return true;
		}
		string visibleText = GetVisibleText(text);
		if (string.IsNullOrEmpty(visibleText))
		{
			return true;
		}
		TryAddCharactersToTmpFontAsset(fontAsset, visibleText);
		List<int> list = CollectMissingChars(fontAsset, visibleText);
		if (list.Count == 0)
		{
			return true;
		}
		missingCount = list.Count;
		try
		{
			Interlocked.Add(ref _instance._glyphAtlasMissCount, list.Count);
		}
		catch (Exception reportedException)
		{
			ReportCaughtException(reportedException);
		}
		StringBuilder stringBuilder = null;
		lock (_atlasMissLogLock)
		{
			foreach (int item in list)
			{
				if (_atlasMissLogged.Add(item))
				{
					if (stringBuilder == null)
					{
						stringBuilder = new StringBuilder();
					}
					if (stringBuilder.Length > 0)
					{
						stringBuilder.Append(", ");
					}
					stringBuilder.Append("U+").Append(item.ToString("X4"));
				}
			}
		}
		if (stringBuilder != null && (Object)(object)_instance != (Object)null)
		{
			try
			{
				_instance.Logger.LogWarning("[FONT-WARM] glyphs missing from CJK atlas (will use overlay fallback): " + stringBuilder);
			}
			catch (Exception reportedException)
			{
				ReportCaughtException(reportedException);
			}
		}
		return false;
	}

	private static List<int> CollectMissingChars(object fontAsset, string visible)
	{
		List<int> list = new List<int>();
		if (fontAsset == null || string.IsNullOrEmpty(visible))
		{
			return list;
		}
		StringBuilder stringBuilder = null;
		HashSet<int> hashSet = new HashSet<int>();
		string text = visible;
		for (int i = 0; i < text.Length; i++)
		{
			char c = text[i];
			int num = c;
			int charCount = 1;
			if (char.IsHighSurrogate(c) && i + 1 < text.Length && char.IsLowSurrogate(text[i + 1]))
			{
				/* emoji/扩展B 等代理项对必须按完整码点去重与检测，
				   否则半个代理项永远不在 atlas 中，会永远误报 missing。 */
				num = char.ConvertToUtf32(c, text[i + 1]);
				charCount = 2;
				i++;
			}
			if (num >= 128 && hashSet.Add(num))
			{
				if (stringBuilder == null)
				{
					stringBuilder = new StringBuilder();
				}
				stringBuilder.Append(text, i - (charCount - 1), charCount);
			}
		}
		if (stringBuilder == null || stringBuilder.Length == 0)
		{
			return list;
		}
		string text2 = stringBuilder.ToString();
		try
		{
			Type type = fontAsset.GetType();
			MethodInfo method = type.GetMethod("HasCharacters", new Type[4]
			{
				typeof(string),
				typeof(string).MakeByRefType(),
				typeof(bool),
				typeof(bool)
			});
			if (method != null)
			{
				// This is an own-atlas check. TMP fallback coverage can be true even
				// when the host material cannot render the fallback glyphs.
				object[] array = new object[4] { text2, null, false, false };
				object obj = method.Invoke(fontAsset, array);
				bool flag = default(bool);
				int num2;
				if (obj is bool)
				{
					flag = (bool)obj;
					num2 = 1;
				}
				else
				{
					num2 = 0;
				}
				if (((uint)num2 & (flag ? 1u : 0u)) != 0)
				{
					return list;
				}
				string text3 = array[1] as string;
				if (!string.IsNullOrEmpty(text3))
				{
					AddStringCodePoints(text3, list);
					return list;
				}
				AddStringCodePoints(text2, list);
				return list;
			}
			MethodInfo method3 = type.GetMethod("HasCharacter", new Type[2]
			{
				typeof(int),
				typeof(bool)
			});
			if (method3 != null)
			{
				text = text2;
				for (int j = 0; j < text.Length; j++)
				{
					char c2 = text[j];
					int codePoint = c2;
					if (char.IsHighSurrogate(c2) && j + 1 < text.Length && char.IsLowSurrogate(text[j + 1]))
					{
						codePoint = char.ConvertToUtf32(c2, text[j + 1]);
						j++;
					}
					object obj3 = method3.Invoke(fontAsset, new object[2]
					{
						codePoint,
						false
					});
					if (obj3 is bool && !(bool)obj3)
					{
						list.Add(codePoint);
					}
				}
				return list;
			}
			MethodInfo method2 = type.GetMethod("HasCharacters", new Type[2]
			{
				typeof(string),
				typeof(List<char>).MakeByRefType()
			});
			if (method2 != null)
			{
				object[] array2 = new object[2] { text2, null };
				object obj2 = method2.Invoke(fontAsset, array2);
				bool flag2 = default(bool);
				int num3;
				if (obj2 is bool)
				{
					flag2 = (bool)obj2;
					num3 = 1;
				}
				else
				{
					num3 = 0;
				}
				if (((uint)num3 & (flag2 ? 1u : 0u)) != 0)
				{
					return list;
				}
				if (array2[1] is List<char> list2)
				{
					AddStringCodePoints(new string(list2.ToArray()), list);
					return list;
				}
				AddStringCodePoints(text2, list);
				return list;
			}
			MethodInfo method4 = type.GetMethod("HasCharacter", new Type[1] { typeof(int) });
			if (method4 != null)
			{
				text = text2;
				for (int k = 0; k < text.Length; k++)
				{
					char c3 = text[k];
					int codePoint2 = c3;
					if (char.IsHighSurrogate(c3) && k + 1 < text.Length && char.IsLowSurrogate(text[k + 1]))
					{
						codePoint2 = char.ConvertToUtf32(c3, text[k + 1]);
						k++;
					}
					object obj4 = method4.Invoke(fontAsset, new object[1] { codePoint2 });
					if (obj4 is bool && !(bool)obj4)
					{
						list.Add(codePoint2);
					}
				}
				return list;
			}
		}
		catch (Exception reportedException)
		{
			ReportCaughtException(reportedException);
		}
		return list;
	}

	/* 把字符串按完整 Unicode 码点加入列表：代理项对合并为一个码点，
	   避免半个代理项被当成独立缺字而永远误报 missing。 */
	private static void AddStringCodePoints(string text, List<int> output)
	{
		for (int i = 0; i < text.Length; i++)
		{
			char c = text[i];
			if (char.IsHighSurrogate(c) && i + 1 < text.Length && char.IsLowSurrogate(text[i + 1]))
			{
				output.Add(char.ConvertToUtf32(c, text[i + 1]));
				i++;
			}
			else
			{
				output.Add(c);
			}
		}
	}

	private static bool AreAllGlyphsInAtlas(object fontAsset, string text)
	{
		if (fontAsset == null || string.IsNullOrEmpty(text))
		{
			return true;
		}
		return CollectMissingChars(fontAsset, GetVisibleText(text)).Count == 0;
	}

	private void ApplyTMPFont(object tmpComponent)
	{
		if (!HasUsableTmpFont() || !IsUnityObjectAlive(tmpComponent))
		{
			return;
		}
		try
		{
			PropertyInfo propertyInfo = AccessTools.Property(tmpComponent.GetType(), "font");
			object obj = propertyInfo?.GetValue(tmpComponent);
			if (obj == _chineseTMPFont)
			{
				return;
			}
			if (obj == null)
			{
				if (propertyInfo != null && propertyInfo.CanWrite)
				{
					propertyInfo.SetValue(tmpComponent, _chineseTMPFont);
					Interlocked.Increment(ref _fontApplyAttached);
					if (_debugMode.Value)
					{
						base.Logger.LogInfo("[FONT-APPLY] Component had no font; assigned CJK font directly: " + GetComponentLogPath(tmpComponent));
					}
					RestoreTmpOverlay(tmpComponent);
				}
				else
				{
					Interlocked.Increment(ref _fontApplyFailures);
					if (_debugMode.Value)
					{
						base.Logger.LogWarning("[FONT-APPLY] Component has null font and font property is not writable: " + GetComponentLogPath(tmpComponent));
					}
				}
				return;
			}
			bool flag = false;
			IList tmpFallbackList = GetTmpFallbackList(obj);
			if (tmpFallbackList != null)
			{
				if (!tmpFallbackList.Contains(_chineseTMPFont))
				{
					tmpFallbackList.Add(_chineseTMPFont);
					flag = true;
				}
			}
			else
			{
				PropertyInfo propertyInfo2 = AccessTools.Property(obj.GetType(), "fallbackFontAsset");
				if (propertyInfo2 != null)
				{
					if (propertyInfo2.GetValue(obj) == null)
					{
						propertyInfo2.SetValue(obj, _chineseTMPFont);
						flag = true;
					}
				}
				else
				{
					Interlocked.Increment(ref _fontApplyFailures);
					if (_debugMode.Value)
					{
						base.Logger.LogWarning("[FONT-APPLY] Cannot attach fallback to " + obj.GetType().FullName + " on " + GetComponentLogPath(tmpComponent) + "; CJK chars will be missing");
					}
				}
			}
			if (flag)
			{
				Interlocked.Increment(ref _fontApplyAttached);
				if (_debugMode.Value)
				{
					base.Logger.LogInfo("[FONT-APPLY] Attached CJK fallback to " + GetComponentLogPath(tmpComponent));
				}
			}
			// Some packed TMP atlases still miss punctuation after a fallback is attached.
			// The caller validates glyph coverage against the current text.
		}
		catch (Exception ex)
		{
			Interlocked.Increment(ref _fontApplyFailures);
			base.Logger.LogWarning("[FONT-APPLY] failed: " + ex.Message);
		}
	}

	private static bool IsHostFontDynamic(object fontAsset)
	{
		if (fontAsset == null)
		{
			return false;
		}
		try
		{
			Type type = fontAsset.GetType();
			object obj = null;
			FieldInfo fieldInfo = AccessTools.Field(type, "m_AtlasPopulationMode");
			if (fieldInfo != null)
			{
				obj = fieldInfo.GetValue(fontAsset);
			}
			if (obj == null)
			{
				PropertyInfo propertyInfo = AccessTools.Property(type, "atlasPopulationMode");
				if (propertyInfo != null && propertyInfo.CanRead)
				{
					obj = propertyInfo.GetValue(fontAsset);
				}
			}
			if (obj == null)
			{
				return false;
			}
			return obj.ToString().IndexOf("Dynamic", StringComparison.OrdinalIgnoreCase) >= 0;
		}
		catch (Exception ex)
		{
			ReportCaughtException(ex);
			return false;
		}
	}

	private static bool TryWarmHostFontAssetIfDynamic(object hostFontAsset, string text)
	{
		if (!IsHostFontDynamic(hostFontAsset) || string.IsNullOrEmpty(text))
		{
			return false;
		}
		string visibleText = GetVisibleText(text);
		if (string.IsNullOrEmpty(visibleText))
		{
			return true;
		}
		TryAddCharactersToTmpFontAsset(hostFontAsset, visibleText);
		return AreAllGlyphsInAtlas(hostFontAsset, text);
	}

	private static MethodInfo GetMethodQuiet(Type t, string name, Type[] paramTypes)
	{
		if (t == null || string.IsNullOrEmpty(name))
		{
			return null;
		}
		string text = ((paramTypes == null || paramTypes.Length == 0) ? "()" : string.Join(",", paramTypes.Select((Type p) => p?.FullName ?? "?")));
		string key = t.FullName + "::" + name + "::" + text;
		lock (_quietMethodCacheLock)
		{
			if (_quietMethodCache.TryGetValue(key, out var value))
			{
				return value;
			}
			MethodInfo methodInfo = null;
			try
			{
				BindingFlags bindingAttr = BindingFlags.Instance | BindingFlags.Public | BindingFlags.NonPublic;
				methodInfo = ((paramTypes != null) ? t.GetMethod(name, bindingAttr, null, paramTypes, null) : t.GetMethod(name, bindingAttr));
			}
			catch (Exception reportedException)
			{
				ReportCaughtException(reportedException);
			}
			_quietMethodCache[key] = methodInfo;
			return methodInfo;
		}
	}

	/* Some aggressively stripped Unity players retain Resources.FindObjectsOfTypeAll(Type)
	   but return an empty array even while live TMP/UGUI components are rendering. The
	   player owns that native enumeration mismatch, so use the retained Object scan as
	   an include-inactive fallback. A real empty scene remains empty, and choosing or
	   invoking the fallback is recorded instead of being treated as translation success. */
	private static Object[] FindObjectsOfTypeAllSafe(Type componentType)
	{
		if (componentType == null)
		{
			return new Object[0];
		}
		Object[] array = null;
		if (Volatile.Read(ref _preferObjectFindObjectsFallback) == 0)
		{
			try
			{
				array = Resources.FindObjectsOfTypeAll(componentType);
				if (array != null && array.Length > 0)
				{
					return array;
				}
			}
			catch (Exception ex)
			{
				ReportCaughtException(ex, "renderer=UnityMono method=Resources.FindObjectsOfTypeAll runtimeType=" + componentType.FullName, "FindObjectsOfTypeAllSafe");
			}
		}
		try
		{
			ResolveFindObjectsFallbackMethods();
			Object[] array2 = null;
			if (_findObjectsOfTypeWithInactiveMethod != null)
			{
				array2 = _findObjectsOfTypeWithInactiveMethod.Invoke(null, new object[2] { componentType, true }) as Object[];
			}
			else if (_findObjectsOfTypeLegacyMethod != null)
			{
				array2 = _findObjectsOfTypeLegacyMethod.Invoke(null, new object[1] { componentType }) as Object[];
			}
			else if (_findObjectsOfTypeGenericMethod != null)
			{
				array2 = _findObjectsOfTypeGenericMethod.MakeGenericMethod(componentType).Invoke(null, null) as Object[];
			}
			if (array2 != null && array2.Length > 0)
			{
				Interlocked.Exchange(ref _preferObjectFindObjectsFallback, 1);
				if (Interlocked.Exchange(ref _objectFindObjectsFallbackLogged, 1) == 0)
				{
					_instance?.Logger.LogWarning("Resources.FindObjectsOfTypeAll(Type) returned no live objects; using Object.FindObjectsOfType(Type,true) compatibility fallback.");
				}
				return array2;
			}
		}
		catch (Exception ex2)
		{
			ReportCaughtException(ex2, "renderer=UnityMono method=Object.FindObjectsOfType runtimeType=" + componentType.FullName, "FindObjectsOfTypeAllSafe");
		}
		return array ?? new Object[0];
	}

	private static T[] FindObjectsOfTypeAllSafe<T>() where T : Object
	{
		Object[] array = FindObjectsOfTypeAllSafe(typeof(T));
		List<T> list = new List<T>(array.Length);
		foreach (Object val in array)
		{
			if (val is T)
			{
				list.Add((T)val);
			}
		}
		return list.ToArray();
	}

	/* On some stripped TMP players both Unity global enumeration APIs stay empty even
	   while text is rendering. TMP_UpdateManager owns a persistent active-text queue,
	   so it is the closest renderer boundary that can recover those instances without
	   game-specific type names. Discovery alone never creates a cache success, and the
	   selected fallback or reflection failure is recorded. */
	private static Object[] FindTmpTextObjectsSafe(Type tmpTextType)
	{
		Object[] array = FindObjectsOfTypeAllSafe(tmpTextType);
		if (array.Length > 0 || tmpTextType == null)
		{
			return array;
		}
		try
		{
			Type type = AccessTools.TypeByName("TMPro.TMP_UpdateManager");
			if (type == null)
			{
				LogTmpUpdateManagerProbeOnce("type-unavailable");
				return array;
			}
			PropertyInfo propertyInfo = type.GetProperty("instance", BindingFlags.Static | BindingFlags.Public | BindingFlags.NonPublic);
			object obj = propertyInfo?.GetValue(null);
			if (obj == null)
			{
				FieldInfo fieldInfo = type.GetField("s_Instance", BindingFlags.Static | BindingFlags.Public | BindingFlags.NonPublic);
				obj = fieldInfo?.GetValue(null);
			}
			if (obj == null)
			{
				LogTmpUpdateManagerProbeOnce("instance-unavailable");
				return array;
			}
			Object[] array2 = GetTmpUpdateManagerRegisteredTextObjects(obj, tmpTextType);
			LogTmpUpdateManagerProbeOnce("registered=" + array2.Length);
			if (array2.Length > 0 && Interlocked.Exchange(ref _tmpUpdateManagerFallbackLogged, 1) == 0)
			{
				_instance?.Logger.LogWarning("TMP global enumeration returned no live objects; using TMP_UpdateManager internal queue compatibility fallback.");
			}
			return array2;
		}
		catch (Exception ex)
		{
			ReportCaughtException(ex, "renderer=UnityMono registry=TMPro.TMP_UpdateManager.m_InternalUpdateQueue", "FindTmpTextObjectsSafe");
			return array;
		}
	}

	private static Object[] GetTmpUpdateManagerRegisteredTextObjects(object updateManager, Type tmpTextType)
	{
		if (updateManager == null || tmpTextType == null)
		{
			return new Object[0];
		}
		FieldInfo fieldInfo = updateManager.GetType().GetField("m_InternalUpdateQueue", BindingFlags.Instance | BindingFlags.Public | BindingFlags.NonPublic);
		IEnumerable enumerable = fieldInfo?.GetValue(updateManager) as IEnumerable;
		if (enumerable == null)
		{
			return new Object[0];
		}
		List<Object> list = new List<Object>();
		foreach (object item in enumerable)
		{
			Object val = item as Object;
			if (!ReferenceEquals(val, null) && tmpTextType.IsInstanceOfType(item))
			{
				list.Add(val);
			}
		}
		return list.ToArray();
	}

	private static void LogTmpUpdateManagerProbeOnce(string outcome)
	{
		DeepSeekTranslator deepSeekTranslator = _instance;
		if (!ReferenceEquals(deepSeekTranslator, null) && deepSeekTranslator._debugMode.Value && Interlocked.Exchange(ref _tmpUpdateManagerProbeLogged, 1) == 0)
		{
			deepSeekTranslator.Logger.LogInfo("[TMP-REGISTRY-PROBE] " + outcome);
		}
	}

	private void DiscoverTmpUpdateManagerRegistry(object updateManager)
	{
		float realtimeSinceStartup = Time.realtimeSinceStartup;
		if (realtimeSinceStartup - _lastTmpRegistryDiscoveryRealtime < TmpRegistryDiscoveryIntervalSeconds)
		{
			return;
		}
		_lastTmpRegistryDiscoveryRealtime = realtimeSinceStartup;
		try
		{
			EnsureScannerInitialized("TMP_REGISTRY");
			Object[] array = GetTmpUpdateManagerRegisteredTextObjects(updateManager, _tmpTextTypeCache);
			_tmpRegistryRegisteredCount = array.Length;
			if (_debugMode.Value && _tmpRegistryHookCount <= 3)
			{
				base.Logger.LogInfo($"[TMP-REGISTRY-DIAG] hook={_tmpRegistryHookCount} registered={array.Length}");
			}
			int num = 0;
			int num2 = 0;
			int num3 = 0;
			foreach (Object val in array)
			{
				if (num >= MaxTmpRegistryDiscoveryPerPass)
				{
					break;
				}
				if (!ReferenceEquals(val, null) && IsComponentActive(val))
				{
					num2++;
					if (!string.IsNullOrWhiteSpace(GetCurrentComponentText(val, isTmp: true)))
					{
						num3++;
					}
					QueueCachedComponentTextIfAvailable(val, isTmp: true, allowRemoteFallback: true);
					num++;
				}
			}
			if (array.Length > 0 && _debugMode.Value && Interlocked.Exchange(ref _tmpUpdateManagerPopulatedLogged, 1) == 0)
			{
				bool fontReference = !ReferenceEquals(_chineseFont, null);
				bool fontUnityAlive = fontReference && (Object)(object)_chineseFont != (Object)null;
				base.Logger.LogInfo($"[TMP-REGISTRY-DIAG] populated registered={array.Length} active={num2} nonempty={num3} canHandle={CanHandleTmp()} fontRef={fontReference} fontUnityAlive={fontUnityAlive}");
			}
		}
		catch (Exception ex)
		{
			ReportCaughtException(ex, "renderer=UnityMono registry=TMPro.TMP_UpdateManager.DoRebuilds", "DiscoverTmpUpdateManagerRegistry");
		}
	}

	private static void ResolveFindObjectsFallbackMethods()
	{
		if (Volatile.Read(ref _findObjectsMethodsResolved) != 0)
		{
			return;
		}
		lock (_quietMethodCacheLock)
		{
			if (_findObjectsMethodsResolved != 0)
			{
				return;
			}
			MethodInfo[] methods = typeof(Object).GetMethods(BindingFlags.Static | BindingFlags.Public);
			foreach (MethodInfo methodInfo in methods)
			{
				if (!string.Equals(methodInfo.Name, "FindObjectsOfType", StringComparison.Ordinal))
				{
					continue;
				}
				ParameterInfo[] parameters = methodInfo.GetParameters();
				if (!methodInfo.IsGenericMethodDefinition && parameters.Length == 2 && parameters[0].ParameterType == typeof(Type) && parameters[1].ParameterType == typeof(bool))
				{
					_findObjectsOfTypeWithInactiveMethod = methodInfo;
				}
				else if (!methodInfo.IsGenericMethodDefinition && parameters.Length == 1 && parameters[0].ParameterType == typeof(Type))
				{
					_findObjectsOfTypeLegacyMethod = methodInfo;
				}
				else if (methodInfo.IsGenericMethodDefinition && parameters.Length == 0)
				{
					_findObjectsOfTypeGenericMethod = methodInfo;
				}
			}
			Volatile.Write(ref _findObjectsMethodsResolved, 1);
		}
	}

	/* Stripped UnityEngine profiles can remove the runtime-Type hierarchy overload
	   while retaining the equivalent generic overload. The external assembly owns
	   that mismatch. Reflection preserves hierarchy scanning without converting a
	   miss into a translation/cache success; selection and invocation failures are
	   recorded once through BepInEx and ReportCaughtException. */
	private static Component[] GetComponentsInChildrenByTypeSafe(object owner, Type componentType, bool includeInactive)
	{
		if (owner == null || componentType == null)
		{
			return new Component[0];
		}
		try
		{
			Type ownerType = owner.GetType();
			MethodInfo dynamicMethod = ownerType.GetMethod("GetComponentsInChildren", BindingFlags.Instance | BindingFlags.Public, null, new Type[2]
			{
				typeof(Type),
				typeof(bool)
			}, null);
			MethodInfo selectedMethod = dynamicMethod;
			if (selectedMethod == null)
			{
				string cacheKey = ownerType.FullName + "::" + componentType.FullName;
				lock (_genericChildrenMethodCacheLock)
				{
					if (!_genericChildrenMethodCache.TryGetValue(cacheKey, out selectedMethod))
					{
						MethodInfo genericDefinition = ownerType.GetMethods(BindingFlags.Instance | BindingFlags.Public)
							.FirstOrDefault((MethodInfo method) => method.Name == "GetComponentsInChildren" && method.IsGenericMethodDefinition && method.GetParameters().Length == 1 && method.GetParameters()[0].ParameterType == typeof(bool));
						selectedMethod = genericDefinition?.MakeGenericMethod(componentType);
						_genericChildrenMethodCache[cacheKey] = selectedMethod;
					}
				}
				if (selectedMethod != null && Interlocked.Exchange(ref _genericChildrenFallbackLogged, 1) == 0)
				{
					_instance?.Logger.LogWarning("[LIFECYCLE] Unity dynamic GetComponentsInChildren(Type,bool) is unavailable; using its generic overload.");
				}
			}
			if (selectedMethod == null)
			{
				return new Component[0];
			}
			object[] arguments = dynamicMethod != null
				? new object[2] { componentType, includeInactive }
				: new object[1] { includeInactive };
			Array values = selectedMethod.Invoke(owner, arguments) as Array;
			if (values == null || values.Length == 0)
			{
				return new Component[0];
			}
			Component[] components = values as Component[];
			if (components != null)
			{
				return components;
			}
			return values.Cast<object>().OfType<Component>().ToArray();
		}
		catch (Exception ex)
		{
			ReportCaughtException(ex, "renderer=UnityMono method=GetComponentsInChildren runtimeType=" + componentType.FullName, "GetComponentsInChildrenByTypeSafe");
			return new Component[0];
		}
	}

	private static void ReportCanvasRendererAlphaFallbackOnce()
	{
		if (Interlocked.Exchange(ref _canvasRendererAlphaFallbackLogged, 1) == 0)
		{
			_instance?.Logger.LogWarning("[RENDER] CanvasRenderer alpha accessors are unavailable; alpha diagnostics and repair will be skipped on this Unity profile.");
		}
	}

	private static float GetCanvasRendererAlphaSafe(CanvasRenderer renderer, float fallback)
	{
		if ((Object)(object)renderer == (Object)null)
		{
			return fallback;
		}
		try
		{
			MethodInfo method = GetMethodQuiet(renderer.GetType(), "GetAlpha", Type.EmptyTypes);
			if (method == null)
			{
				ReportCanvasRendererAlphaFallbackOnce();
				return fallback;
			}
			object value = method.Invoke(renderer, null);
			return (value is float) ? (float)value : fallback;
		}
		catch (Exception ex)
		{
			ReportCaughtException(ex, "renderer=UnityMono method=CanvasRenderer.GetAlpha", "GetCanvasRendererAlphaSafe");
			return fallback;
		}
	}

	private static void SetCanvasRendererAlphaSafe(CanvasRenderer renderer, float alpha)
	{
		if ((Object)(object)renderer == (Object)null)
		{
			return;
		}
		try
		{
			MethodInfo method = GetMethodQuiet(renderer.GetType(), "SetAlpha", new Type[1] { typeof(float) });
			if (method == null)
			{
				ReportCanvasRendererAlphaFallbackOnce();
				return;
			}
			method.Invoke(renderer, new object[1] { alpha });
		}
		catch (Exception ex)
		{
			ReportCaughtException(ex, "renderer=UnityMono method=CanvasRenderer.SetAlpha", "SetCanvasRendererAlphaSafe");
		}
	}

	private static void ReportGuiStyleVisualFallbackOnce()
	{
		if (Interlocked.Exchange(ref _guiStyleVisualFallbackLogged, 1) == 0)
		{
			_instance?.Logger.LogWarning("[RENDER] GUIStyle measurement/font members are unavailable; related IMGUI visual enhancements will be skipped on this Unity profile.");
		}
	}

	private static bool TryMeasureImGuiContentSafe(GUIStyle style, GUIContent content, out float requiredWidth, out TextAnchor alignment)
	{
		requiredWidth = 0f;
		alignment = TextAnchor.UpperLeft;
		if (style == null || content == null)
		{
			return false;
		}
		try
		{
			MethodInfo minMaxMethod = GetMethodQuiet(style.GetType(), "CalcMinMaxWidth", new Type[3]
			{
				typeof(GUIContent),
				typeof(float).MakeByRefType(),
				typeof(float).MakeByRefType()
			});
			MethodInfo sizeMethod = GetMethodQuiet(style.GetType(), "CalcSize", new Type[1] { typeof(GUIContent) });
			PropertyInfo alignmentProperty = GetPropertyQuiet(style.GetType(), "alignment");
			if (minMaxMethod == null || sizeMethod == null || alignmentProperty == null || !alignmentProperty.CanRead)
			{
				ReportGuiStyleVisualFallbackOnce();
				return false;
			}
			object[] widthArguments = new object[3] { content, 0f, 0f };
			minMaxMethod.Invoke(style, widthArguments);
			float minWidth = (widthArguments[1] is float) ? (float)widthArguments[1] : 0f;
			float maxWidth = (widthArguments[2] is float) ? (float)widthArguments[2] : 0f;
			object sizeValue = sizeMethod.Invoke(style, new object[1] { content });
			Vector2 size = (sizeValue is Vector2) ? (Vector2)sizeValue : default(Vector2);
			object alignmentValue = alignmentProperty.GetValue(style);
			if (alignmentValue is TextAnchor)
			{
				alignment = (TextAnchor)alignmentValue;
			}
			requiredWidth = Mathf.Ceil(Mathf.Max(size.x, Mathf.Max(minWidth, maxWidth)));
			return requiredWidth > 0f;
		}
		catch (Exception ex)
		{
			ReportCaughtException(ex, "renderer=IMGUI method=GUIStyle measurement", "TryMeasureImGuiContentSafe");
			return false;
		}
	}

	private static void SetMaterialTextureSafe(Material material, string propertyName, Texture texture)
	{
		if ((Object)(object)material == (Object)null || string.IsNullOrEmpty(propertyName))
		{
			return;
		}
		try
		{
			MethodInfo stringMethod = GetMethodQuiet(material.GetType(), "SetTexture", new Type[2] { typeof(string), typeof(Texture) });
			if (stringMethod != null)
			{
				stringMethod.Invoke(material, new object[2] { propertyName, texture });
				return;
			}
			MethodInfo idMethod = GetMethodQuiet(material.GetType(), "SetTexture", new Type[2] { typeof(int), typeof(Texture) });
			if (idMethod != null)
			{
				idMethod.Invoke(material, new object[2] { Shader.PropertyToID(propertyName), texture });
				if (Interlocked.Exchange(ref _materialTextureFallbackLogged, 1) == 0)
				{
					_instance?.Logger.LogWarning("[RENDER] Material.SetTexture(string,Texture) is unavailable; using the retained property-ID overload.");
				}
			}
		}
		catch (Exception ex)
		{
			ReportCaughtException(ex, "renderer=UnityMono method=Material.SetTexture", "SetMaterialTextureSafe");
		}
	}

	private static Color GetTexturePixelSafe(Texture2D texture, int x, int y)
	{
		if ((Object)(object)texture == (Object)null)
		{
			return default(Color);
		}
		try
		{
			MethodInfo pixelMethod = GetMethodQuiet(texture.GetType(), "GetPixel", new Type[2] { typeof(int), typeof(int) });
			if (pixelMethod != null)
			{
				object value = pixelMethod.Invoke(texture, new object[2] { x, y });
				return (value is Color) ? (Color)value : default(Color);
			}
			MethodInfo bilinearMethod = GetMethodQuiet(texture.GetType(), "GetPixelBilinear", new Type[2] { typeof(float), typeof(float) });
			if (bilinearMethod == null)
			{
				return default(Color);
			}
			float u = ((float)x + 0.5f) / Mathf.Max(1f, texture.width);
			float v = ((float)y + 0.5f) / Mathf.Max(1f, texture.height);
			object fallbackValue = bilinearMethod.Invoke(texture, new object[2] { u, v });
			if (Interlocked.Exchange(ref _texturePixelFallbackLogged, 1) == 0)
			{
				_instance?.Logger.LogWarning("[RENDER] Texture2D.GetPixel is unavailable; diagnostics will sample the retained bilinear API.");
			}
			return (fallbackValue is Color) ? (Color)fallbackValue : default(Color);
		}
		catch (Exception ex)
		{
			ReportCaughtException(ex, "renderer=UnityMono method=Texture2D pixel sample", "GetTexturePixelSafe");
			return default(Color);
		}
	}

	private void RefreshTmpSubMeshMaterials()
	{
		if (!HasUsableTmpFont() || _chineseTMPFont == null)
		{
			return;
		}
		try
		{
			object obj = GetPropertyQuiet(_chineseTMPFont.GetType(), "material")?.GetValue(_chineseTMPFont);
			Material val = (Material)((obj is Material) ? obj : null);
			if ((Object)(object)val == (Object)null)
			{
				return;
			}
			if ((object)_tmpSubMeshTypeCache == null)
			{
				_tmpSubMeshTypeCache = AccessTools.TypeByName("TMPro.TMP_SubMesh");
			}
			if ((object)_tmpSubMeshUITypeCache == null)
			{
				_tmpSubMeshUITypeCache = AccessTools.TypeByName("TMPro.TMP_SubMeshUI");
			}
			Type[] array = new Type[2] { _tmpSubMeshUITypeCache, _tmpSubMeshTypeCache };
			foreach (Type type in array)
			{
				if (type == null)
				{
					continue;
				}
				Object[] array2;
				try
				{
					array2 = FindObjectsOfTypeAllSafe(type);
				}
				catch (Exception ex)
				{
					ReportCaughtException(ex, "type=" + type.FullName);
					continue;
				}
				PropertyInfo propertyQuiet = GetPropertyQuiet(type, "sharedMaterial");
				PropertyInfo propertyQuiet2 = GetPropertyQuiet(type, "material");
				Object[] array3 = array2;
				foreach (Object val2 in array3)
				{
					if (val2 == (Object)null)
					{
						continue;
					}
					try
					{
						object obj3 = propertyQuiet?.GetValue(val2);
						Material val3 = (Material)((obj3 is Material) ? obj3 : null);
						if (!((Object)(object)val3 != (Object)null) || !((Object)(object)val3.shader != (Object)null) || !val3.HasProperty("_MainTex"))
						{
							if (propertyQuiet != null && propertyQuiet.CanWrite)
							{
								propertyQuiet.SetValue(val2, val);
								goto IL_0179;
							}
							if (propertyQuiet2 != null && propertyQuiet2.CanWrite)
							{
								propertyQuiet2.SetValue(val2, val);
								goto IL_0179;
							}
						}
						goto end_IL_00f9;
						IL_0179:
						end_IL_00f9:;
					}
					catch (Exception reportedException)
					{
						ReportCaughtException(reportedException);
					}
				}
			}
		}
		catch (Exception ex)
		{
			base.Logger.LogWarning("[SUBMESH-FIX] failed: " + ex.Message);
		}
	}

	private static FieldInfo GetFieldQuiet(Type t, string name)
	{
		if (t == null || string.IsNullOrEmpty(name))
		{
			return null;
		}
		string key = t.FullName + "::" + name;
		lock (_quietFieldCacheLock)
		{
			if (_quietFieldCache.TryGetValue(key, out var value))
			{
				return value;
			}
			FieldInfo fieldInfo = null;
			try
			{
				Type type = t;
				while (type != null && fieldInfo == null)
				{
					fieldInfo = type.GetField(name, BindingFlags.DeclaredOnly | BindingFlags.Instance | BindingFlags.Public | BindingFlags.NonPublic);
					type = type.BaseType;
				}
			}
			catch (Exception reportedException)
			{
				ReportCaughtException(reportedException);
			}
			_quietFieldCache[key] = fieldInfo;
			return fieldInfo;
		}
	}

	private static PropertyInfo GetPropertyQuiet(Type t, string name)
	{
		if (t == null || string.IsNullOrEmpty(name))
		{
			return null;
		}
		string key = t.FullName + "::" + name;
		lock (_quietPropertyCacheLock)
		{
			if (_quietPropertyCache.TryGetValue(key, out var value))
			{
				return value;
			}
			PropertyInfo propertyInfo = null;
			try
			{
				Type type = t;
				while (type != null && propertyInfo == null)
				{
					propertyInfo = type.GetProperty(name, BindingFlags.DeclaredOnly | BindingFlags.Instance | BindingFlags.Public | BindingFlags.NonPublic);
					type = type.BaseType;
				}
			}
			catch (Exception reportedException)
			{
				ReportCaughtException(reportedException);
			}
			_quietPropertyCache[key] = propertyInfo;
			return propertyInfo;
		}
	}

	private void RefreshSoftMaskHierarchy(object tmpComponent)
	{
		Component val = (Component)((tmpComponent is Component) ? tmpComponent : null);
		if (val == null || (Object)(object)val == (Object)null)
		{
			return;
		}
		try
		{
			Type type = ((object)val).GetType();
			MethodInfo methodQuiet = GetMethodQuiet(type, "SetMaterialDirty", Type.EmptyTypes);
			MethodInfo methodQuiet2 = GetMethodQuiet(type, "SetVerticesDirty", Type.EmptyTypes);
			MethodInfo methodQuiet3 = GetMethodQuiet(type, "SetLayoutDirty", Type.EmptyTypes);
			try
			{
				methodQuiet?.Invoke(val, null);
			}
			catch (Exception reportedException)
			{
				ReportCaughtException(reportedException);
			}
			try
			{
				methodQuiet2?.Invoke(val, null);
			}
			catch (Exception reportedException)
			{
				ReportCaughtException(reportedException);
			}
			try
			{
				methodQuiet3?.Invoke(val, null);
			}
			catch (Exception reportedException)
			{
				ReportCaughtException(reportedException);
			}
			FieldInfo fieldQuiet = GetFieldQuiet(type, "m_havePropertiesChanged");
			try
			{
				fieldQuiet?.SetValue(val, true);
			}
			catch (Exception reportedException)
			{
				ReportCaughtException(reportedException);
			}
			FieldInfo fieldQuiet2 = GetFieldQuiet(type, "m_haveLayoutChanged");
			try
			{
				fieldQuiet2?.SetValue(val, true);
			}
			catch (Exception reportedException)
			{
				ReportCaughtException(reportedException);
			}
			Interlocked.Increment(ref _softMaskRefreshCount);
		}
		catch (Exception reportedException)
		{
			ReportCaughtException(reportedException);
		}
	}

	private static Material ReadFontSharedMaterial(object tmpComponent, Type compType)
	{
		if (tmpComponent == null)
		{
			return null;
		}
		Type type = compType ?? tmpComponent.GetType();
		string[] array = new string[3] { "fontSharedMaterial", "material", "fontMaterial" };
		foreach (string name in array)
		{
			try
			{
				PropertyInfo propertyInfo = AccessTools.Property(type, name);
				if (!(propertyInfo == null))
				{
					object value = propertyInfo.GetValue(tmpComponent);
					Material val = (Material)((value is Material) ? value : null);
					if (val != null && (Object)(object)val != (Object)null)
					{
						return val;
					}
				}
			}
			catch (Exception reportedException)
			{
				ReportCaughtException(reportedException);
			}
		}
		return null;
	}

	private void LogFirstWrites(object component, Type compType, string translated)
	{
		if (Volatile.Read(ref _firstWritesLogged) >= FirstWritesToLog ||
			Interlocked.Increment(ref _firstWritesLogged) > FirstWritesToLog)
		{
			return;
		}
		try
		{
			string text = (string.IsNullOrEmpty(translated) ? "<empty>" : ((translated.Length > 40) ? (translated.Substring(0, 40) + "…") : translated));
			string componentLogPath = GetComponentLogPath(component);
			string text2 = "<n/a>";
			string text3 = "<n/a>";
			string text4 = "<n/a>";
			string text5 = "<n/a>";
			try
			{
				Component val = (Component)((component is Component) ? component : null);
				if (val != null && (Object)(object)val != (Object)null)
				{
					text2 = $"{val.gameObject.activeInHierarchy}";
					CanvasRenderer component2 = val.GetComponent<CanvasRenderer>();
					if ((Object)(object)component2 != (Object)null)
					{
						text4 = GetCanvasRendererAlphaSafe(component2, 1f).ToString("0.00");
					}
					StringBuilder stringBuilder = new StringBuilder();
					Transform val2 = val.transform;
					while ((Object)(object)val2 != (Object)null)
					{
						CanvasGroup component3 = ((Component)val2).GetComponent<CanvasGroup>();
						if ((Object)(object)component3 != (Object)null)
						{
							if (stringBuilder.Length > 0)
							{
								stringBuilder.Append(' ');
							}
							stringBuilder.Append($"{((Object)val2).name}.alpha={component3.alpha:0.00}");
						}
						val2 = val2.parent;
					}
					text5 = ((stringBuilder.Length > 0) ? stringBuilder.ToString() : "<no-CG>");
				}
				Color tmpColor = GetTmpColor(component);
				text3 = $"({tmpColor.r:0.00},{tmpColor.g:0.00},{tmpColor.b:0.00},{tmpColor.a:0.00})";
			}
			catch (Exception reportedException)
			{
				ReportCaughtException(reportedException);
			}
			base.Logger.LogWarning($"[FIRST-WRITE #{_firstWritesLogged}] path={componentLogPath} active={text2} color={text3} crAlpha={text4} cg={text5} translated='{text}'");
		}
		catch (Exception reportedException)
		{
			ReportCaughtException(reportedException);
		}
	}

	private static Texture2D TryReadAtlasTexture(object fontAsset)
	{
		if (fontAsset == null)
		{
			return null;
		}
		Type type = fontAsset.GetType();
		try
		{
			object obj = AccessTools.Property(type, "atlasTexture")?.GetValue(fontAsset);
			Texture2D val = (Texture2D)((obj is Texture2D) ? obj : null);
			if (val != null && (Object)(object)val != (Object)null)
			{
				return val;
			}
		}
		catch (Exception reportedException)
		{
			ReportCaughtException(reportedException);
		}
		try
		{
			object obj3 = AccessTools.Field(type, "m_AtlasTexture")?.GetValue(fontAsset);
			Texture2D val2 = (Texture2D)((obj3 is Texture2D) ? obj3 : null);
			if (val2 != null && (Object)(object)val2 != (Object)null)
			{
				return val2;
			}
		}
		catch (Exception reportedException)
		{
			ReportCaughtException(reportedException);
		}
		try
		{
			if (AccessTools.Property(type, "atlasTextures")?.GetValue(fontAsset) is Texture2D[] array && array.Length != 0 && (Object)(object)array[0] != (Object)null)
			{
				return array[0];
			}
		}
		catch (Exception reportedException)
		{
			ReportCaughtException(reportedException);
		}
		return null;
	}

	private static int TryReadIntMember(object target, Type t, string propName, string fieldName)
	{
		if (target == null)
		{
			return 0;
		}
		t = t ?? target.GetType();
		try
		{
			PropertyInfo propertyInfo = AccessTools.Property(t, propName);
			if (propertyInfo != null)
			{
				object value = propertyInfo.GetValue(target);
				if (value != null)
				{
					return Convert.ToInt32(value);
				}
			}
		}
		catch (Exception reportedException)
		{
			ReportCaughtException(reportedException);
		}
		try
		{
			FieldInfo fieldInfo = AccessTools.Field(t, fieldName);
			if (fieldInfo != null)
			{
				object value2 = fieldInfo.GetValue(target);
				if (value2 != null)
				{
					return Convert.ToInt32(value2);
				}
			}
		}
		catch (Exception reportedException)
		{
			ReportCaughtException(reportedException);
		}
		return 0;
	}

	private bool EnsureTMPFontCoversText(object tmpComponent, string translatedText)
	{
		if (!HasUsableTmpFont() || !IsUnityObjectAlive(tmpComponent))
		{
			return false;
		}
		PrepareTmpCjkParagraphLayout(tmpComponent, translatedText);
		try
		{
			Type type = tmpComponent.GetType();
			PropertyInfo propertyInfo = AccessTools.Property(type, "font");
			if (propertyInfo == null || !propertyInfo.CanWrite)
			{
				return false;
			}
			object value = propertyInfo.GetValue(tmpComponent);
			int missingCount;
			if (value == null)
			{
				propertyInfo.SetValue(tmpComponent, _chineseTMPFont);
				Interlocked.Increment(ref _fontApplyAttached);
				TryWarmTmpFontAssetVerified(_chineseTMPFont, translatedText, out missingCount);
				if (_debugMode != null && _debugMode.Value)
				{
					base.Logger.LogInfo("[FONT-APPLY] direct-assign (null host): " + GetComponentLogPath(tmpComponent));
				}
				return missingCount == 0;
			}
			if (value == _chineseTMPFont)
			{
				TryWarmTmpFontAssetVerified(_chineseTMPFont, translatedText, out var missingCount2);
				Component val = (Component)((tmpComponent is Component) ? tmpComponent : null);
				if (val != null && (Object)(object)val != (Object)null)
				{
					int instanceID = ((Object)val).GetInstanceID();
					bool flag;
					lock (_softMaskRefreshedOnce)
					{
						flag = _softMaskRefreshedOnce.Add(instanceID);
					}
					if (flag)
					{
						RefreshSoftMaskHierarchy(tmpComponent);
					}
				}
				return missingCount2 == 0;
			}
			if (!ContainsCjk(translatedText))
			{
				ApplyTMPFont(tmpComponent);
				return true;
			}
			if (TryWarmHostFontAssetIfDynamic(value, translatedText))
			{
				Interlocked.Increment(ref _hostAtlasWarmedCount);
				if (_debugMode != null && _debugMode.Value)
				{
					base.Logger.LogInfo("[FONT-APPLY] host-atlas warmed: " + GetComponentLogPath(tmpComponent));
				}
				ApplyTMPFont(tmpComponent);
				return true;
			}
			ApplyTMPFont(tmpComponent);
			bool cjkFontCoversText = TryWarmTmpFontAssetVerified(_chineseTMPFont, translatedText, out missingCount);
			if (AreAllGlyphsInAtlas(value, translatedText))
			{
				RefreshSoftMaskHierarchy(tmpComponent);
				return true;
			}
			if (!cjkFontCoversText)
			{
				return false;
			}
			propertyInfo.SetValue(tmpComponent, _chineseTMPFont);
			Interlocked.Increment(ref _directSwapCount);
			TryWarmTmpFontAssetVerified(_chineseTMPFont, translatedText, out var missingCount3);
			RefreshSoftMaskHierarchy(tmpComponent);
			DeepInspectPostSwap(tmpComponent, type, translatedText);
			if (_debugMode != null && _debugMode.Value)
			{
				base.Logger.LogInfo($"[FONT-APPLY] direct-swap (fallback unreliable) on {GetComponentLogPath(tmpComponent)} miss={missingCount3}");
			}
			return missingCount3 == 0;
		}
		catch (Exception ex)
		{
			Interlocked.Increment(ref _fontApplyFailures);
			base.Logger.LogWarning("[FONT-APPLY] EnsureTMPFontCoversText failed: " + ex.Message);
			return false;
		}
	}

	private static string GetComponentLogPath(object component)
	{
		try
		{
			Component val = (Component)((component is Component) ? component : null);
			if (val != null && (Object)(object)val != (Object)null)
			{
				string text = (((Object)(object)val.gameObject != (Object)null) ? ((Object)val.gameObject).name : "<null go>");
				return ((object)val).GetType().Name + "@" + text;
			}
			return component?.GetType().FullName ?? "<null>";
		}
		catch (Exception ex)
		{
			ReportCaughtException(ex);
			return "<unreachable>";
		}
	}

	private static Color GetTmpColor(object tmpComponent)
	{
		try
		{
			object obj = AccessTools.Property(tmpComponent?.GetType(), "color")?.GetValue(tmpComponent);
			if (obj is Color)
			{
				return (Color)obj;
			}
		}
		catch (Exception reportedException)
		{
			ReportCaughtException(reportedException);
		}
		return Color.white;
	}

	private static void SetTmpColorAlphaMember(object tmpComponent, string memberName, float alpha)
	{
		try
		{
			Type type = tmpComponent?.GetType();
			PropertyInfo propertyInfo = type?.GetProperty(memberName, BindingFlags.Instance | BindingFlags.Public | BindingFlags.NonPublic);
			if (propertyInfo?.GetValue(tmpComponent) is Color val)
			{
				val.a = alpha;
				propertyInfo.SetValue(tmpComponent, val);
				return;
			}
			FieldInfo fieldInfo = type?.GetField(memberName, BindingFlags.Instance | BindingFlags.Public | BindingFlags.NonPublic);
			if (fieldInfo?.GetValue(tmpComponent) is Color val2)
			{
				val2.a = alpha;
				fieldInfo.SetValue(tmpComponent, val2);
			}
		}
		catch (Exception reportedException)
		{
			ReportCaughtException(reportedException);
		}
	}

	private static void SetTmpAlpha(object tmpComponent, float alpha)
	{
		SetTmpColorAlphaMember(tmpComponent, "color", alpha);
		SetTmpColorAlphaMember(tmpComponent, "faceColor", alpha);
		SetTmpColorAlphaMember(tmpComponent, "outlineColor", alpha);
		try
		{
			Component val = (Component)((tmpComponent is Component) ? tmpComponent : null);
			if (val != null)
			{
				CanvasRenderer component = val.GetComponent<CanvasRenderer>();
				if (component != null)
				{
					SetCanvasRendererAlphaSafe(component, alpha);
				}
			}
		}
		catch (Exception reportedException)
		{
			ReportCaughtException(reportedException);
		}
	}

	private static void SetTmpColor(object tmpComponent, Color color)
	{
		try
		{
			AccessTools.Property(tmpComponent?.GetType(), "color")?.SetValue(tmpComponent, color);
		}
		catch (Exception reportedException)
		{
			ReportCaughtException(reportedException);
		}
	}

	private static int GetTmpFontSize(object tmpComponent)
	{
		try
		{
			object obj = AccessTools.Property(tmpComponent?.GetType(), "fontSize")?.GetValue(tmpComponent);
			if (obj is float num)
			{
				return Mathf.Max(12, Mathf.RoundToInt(num));
			}
			if (obj is double num2)
			{
				return Mathf.Max(12, Mathf.RoundToInt((float)num2));
			}
			if (obj is int num3)
			{
				return Mathf.Max(12, num3);
			}
		}
		catch (Exception reportedException)
		{
			ReportCaughtException(reportedException);
		}
		return 24;
	}

	private static bool GetTmpAutoSizing(object tmpComponent)
	{
		try
		{
			object value = AccessTools.Property(tmpComponent?.GetType(), "enableAutoSizing")?.GetValue(tmpComponent);
			return value is bool enabled && enabled;
		}
		catch (Exception reportedException)
		{
			ReportCaughtException(reportedException, "renderer=UnityMono property=enableAutoSizing");
			return false;
		}
	}

	private static int GetTmpFontSizeMin(object tmpComponent, int fallback)
	{
		return GetTmpFontSizeBound(tmpComponent, "fontSizeMin", fallback);
	}

	private static int GetTmpFontSizeMax(object tmpComponent, int fallback)
	{
		return GetTmpFontSizeBound(tmpComponent, "fontSizeMax", fallback);
	}

	private static int GetTmpFontSizeBound(object tmpComponent, string propertyName, int fallback)
	{
		try
		{
			object value = AccessTools.Property(tmpComponent?.GetType(), propertyName)?.GetValue(tmpComponent);
			if (value is float floatValue && floatValue > 0f)
			{
				return Mathf.Max(1, Mathf.RoundToInt(floatValue));
			}
			if (value is double doubleValue && doubleValue > 0.0)
			{
				return Mathf.Max(1, Mathf.RoundToInt((float)doubleValue));
			}
			if (value is int intValue && intValue > 0)
			{
				return intValue;
			}
		}
		catch (Exception reportedException)
		{
			ReportCaughtException(reportedException, "renderer=UnityMono property=" + propertyName);
		}
		return Mathf.Max(1, fallback);
	}

	private static bool GetTmpWordWrap(object tmpComponent)
	{
		try
		{
			object obj = AccessTools.Property(tmpComponent?.GetType(), "enableWordWrapping")?.GetValue(tmpComponent);
			if (obj is bool)
			{
				return (bool)obj;
			}
		}
		catch (Exception reportedException)
		{
			ReportCaughtException(reportedException);
		}
		return true;
	}

	private static bool TryGetLeftParagraphAlignment(object alignment, out object leftAlignment)
	{
		leftAlignment = null;
		if (alignment == null || !alignment.GetType().IsEnum)
		{
			return false;
		}
		string name = alignment.ToString();
		if (name.IndexOf("Justified", StringComparison.OrdinalIgnoreCase) < 0 &&
			name.IndexOf("Flush", StringComparison.OrdinalIgnoreCase) < 0)
		{
			return false;
		}
		string leftName = Regex.Replace(name, "Justified|Flush", "Left", RegexOptions.IgnoreCase);
		try
		{
			leftAlignment = Enum.Parse(alignment.GetType(), leftName, ignoreCase: true);
			return true;
		}
		catch (Exception reportedException)
		{
			ReportCaughtException(reportedException, "renderer=TMP;operation=map-cjk-paragraph-alignment");
			return false;
		}
	}

	private void PrepareTmpCjkParagraphLayout(object tmpComponent, string translatedText)
	{
		string visibleText = GetVisibleText(translatedText);
		if (!ContainsCjk(visibleText) || visibleText.Length < 8)
		{
			return;
		}
		Component component = tmpComponent as Component;
		if ((Object)(object)component == (Object)null)
		{
			return;
		}
		try
		{
			PropertyInfo alignmentProperty = AccessTools.Property(tmpComponent.GetType(), "alignment");
			if (alignmentProperty == null || !alignmentProperty.CanRead || !alignmentProperty.CanWrite)
			{
				return;
			}
			if (_tmpParagraphLayouts.TryGetValue(component, out var existing))
			{
				existing.AppliedText = translatedText;
				object current = alignmentProperty.GetValue(tmpComponent);
				if (TryGetLeftParagraphAlignment(current, out var restoredLeft))
				{
					alignmentProperty.SetValue(tmpComponent, restoredLeft);
				}
				return;
			}
			object original = alignmentProperty.GetValue(tmpComponent);
			if (!TryGetLeftParagraphAlignment(original, out var left))
			{
				return;
			}
			_tmpParagraphLayouts.Add(component, new TmpParagraphLayoutState
			{
				OriginalAlignment = original,
				AppliedText = translatedText
			});
			alignmentProperty.SetValue(tmpComponent, left);
			LogVerbose("[TMP-LAYOUT] changed justified/flush CJK paragraph to left alignment: " + GetComponentLogPath(tmpComponent));
		}
		catch (Exception ex)
		{
			ReportCaughtException(ex, "renderer=TMP;operation=prepare-cjk-paragraph-layout");
		}
	}

	private static bool TryGetLeftFromCenteredAlignment(object alignment, out object leftAlignment)
	{
		leftAlignment = null;
		if (alignment == null || !alignment.GetType().IsEnum)
		{
			return false;
		}
		string name = alignment.ToString();
		if (name.IndexOf("Center", StringComparison.OrdinalIgnoreCase) < 0)
		{
			return false;
		}
		try
		{
			leftAlignment = Enum.Parse(alignment.GetType(), Regex.Replace(name, "Center", "Left", RegexOptions.IgnoreCase), ignoreCase: true);
			return true;
		}
		catch (Exception ex)
		{
			ReportCaughtException(ex, "renderer=TMP;operation=map-short-label-alignment");
			return false;
		}
	}

	private void PrepareTmpTranslatedShortLabelLayout(object tmpComponent, string sourceText, string translatedText)
	{
		string sourceVisible = GetVisibleText(sourceText).Trim();
		string translatedVisible = GetVisibleText(translatedText).Trim();
		if (sourceVisible.Length < 5 || sourceVisible.Length > 16 || translatedVisible.Length == 0 || translatedVisible.Length > 4 ||
			ContainsCjk(sourceVisible) || !ContainsCjk(translatedVisible) || sourceVisible.IndexOfAny(new char[2] { '\r', '\n' }) >= 0 ||
			translatedVisible.IndexOfAny(new char[2] { '\r', '\n' }) >= 0)
		{
			return;
		}
		Component component = tmpComponent as Component;
		if ((Object)(object)component == (Object)null)
		{
			return;
		}
		RectTransform rectTransform = component.GetComponent<RectTransform>();
		Transform parent = component.transform.parent;
		if ((Object)(object)rectTransform == (Object)null || (Object)(object)parent == (Object)null)
		{
			return;
		}
		try
		{
			Type type = tmpComponent.GetType();
			PropertyInfo alignmentProperty = AccessTools.Property(type, "alignment");
			if (alignmentProperty == null || !alignmentProperty.CanRead || !alignmentProperty.CanWrite)
			{
				return;
			}
			object originalAlignment = alignmentProperty.GetValue(tmpComponent);
			if (!TryGetLeftFromCenteredAlignment(originalAlignment, out var leftAlignment))
			{
				return;
			}

			Rect rect = rectTransform.rect;
			List<float> leftPeerX = new List<float>(4);
			Component[] peers = GetComponentsInChildrenByTypeSafe(parent, type, includeInactive: false);
			foreach (Component peer in peers)
			{
				if ((Object)(object)peer == (Object)null || (Object)(object)peer == (Object)(object)component || peer.transform.parent != parent)
				{
					continue;
				}
				RectTransform peerRect = peer.GetComponent<RectTransform>();
				if ((Object)(object)peerRect == (Object)null || Mathf.Abs(peerRect.rect.width - rect.width) > 1f ||
					Mathf.Abs(peerRect.rect.height - rect.height) > 1f || Mathf.Abs(peerRect.anchoredPosition.x - rectTransform.anchoredPosition.x) > rect.width * 0.25f)
				{
					continue;
				}
				float yDistance = Mathf.Abs(peerRect.anchoredPosition.y - rectTransform.anchoredPosition.y);
				if (yDistance < 1f || yDistance > rect.height * 5f)
				{
					continue;
				}
				object peerAlignment = alignmentProperty.GetValue(peer);
				string peerAlignmentName = peerAlignment?.ToString() ?? string.Empty;
				if (peerAlignmentName.IndexOf("Left", StringComparison.OrdinalIgnoreCase) >= 0)
				{
					leftPeerX.Add(peerRect.anchoredPosition.x);
				}
			}
			if (leftPeerX.Count < 2)
			{
				return;
			}
			leftPeerX.Sort();
			float groupX = leftPeerX[leftPeerX.Count / 2];
			_tmpShortLabelLayouts.Remove(component);
			_tmpShortLabelLayouts.Add(component, new TmpShortLabelLayoutState
			{
				OriginalAlignment = originalAlignment,
				OriginalAnchoredPosition = rectTransform.anchoredPosition,
				AppliedText = translatedText
			});
			alignmentProperty.SetValue(tmpComponent, leftAlignment);
			Vector2 position = rectTransform.anchoredPosition;
			position.x = groupX;
			rectTransform.anchoredPosition = position;
			LogVerbose($"[TMP-LAYOUT] normalized translated short-label group alignment: {GetComponentLogPath(tmpComponent)} peers={leftPeerX.Count} x={groupX:0.0}");
		}
		catch (Exception ex)
		{
			ReportCaughtException(ex, "renderer=TMP;operation=prepare-short-label-layout path=" + GetComponentLogPath(tmpComponent));
		}
	}

	private static void RestoreTmpShortLabelLayout(object tmpComponent, string nextText)
	{
		Component component = tmpComponent as Component;
		if ((Object)(object)component == (Object)null || !_tmpShortLabelLayouts.TryGetValue(component, out var state) ||
			string.Equals(NormalizeRequestText(nextText), NormalizeRequestText(state.AppliedText), StringComparison.Ordinal))
		{
			return;
		}
		try
		{
			PropertyInfo alignmentProperty = AccessTools.Property(tmpComponent.GetType(), "alignment");
			if (alignmentProperty != null && alignmentProperty.CanWrite && state.OriginalAlignment != null)
			{
				alignmentProperty.SetValue(tmpComponent, state.OriginalAlignment);
			}
			RectTransform rectTransform = component.GetComponent<RectTransform>();
			if ((Object)(object)rectTransform != (Object)null)
			{
				rectTransform.anchoredPosition = state.OriginalAnchoredPosition;
			}
		}
		catch (Exception ex)
		{
			ReportCaughtException(ex, "renderer=TMP;operation=restore-short-label-layout path=" + GetComponentLogPath(tmpComponent));
		}
		finally
		{
			_tmpShortLabelLayouts.Remove(component);
		}
	}

	private static void RestoreTmpParagraphLayout(object tmpComponent, string nextText)
	{
		Component component = tmpComponent as Component;
		if ((Object)(object)component == (Object)null || !_tmpParagraphLayouts.TryGetValue(component, out var state))
		{
			return;
		}
		if (string.Equals(NormalizeRequestText(nextText), NormalizeRequestText(state.AppliedText), StringComparison.Ordinal))
		{
			return;
		}
		try
		{
			PropertyInfo alignmentProperty = AccessTools.Property(tmpComponent.GetType(), "alignment");
			if (alignmentProperty != null && alignmentProperty.CanWrite && state.OriginalAlignment != null)
			{
				alignmentProperty.SetValue(tmpComponent, state.OriginalAlignment);
			}
		}
		catch (Exception ex)
		{
			ReportCaughtException(ex, "renderer=TMP;operation=restore-paragraph-layout");
		}
		finally
		{
			_tmpParagraphLayouts.Remove(component);
		}
	}

	private static TextAnchor GetTmpTextAnchor(object tmpComponent)
	{
		try
		{
			string text = AccessTools.Property(tmpComponent?.GetType(), "alignment")?.GetValue(tmpComponent)?.ToString() ?? string.Empty;
			bool isParagraphFill = text.IndexOf("Justified", StringComparison.OrdinalIgnoreCase) >= 0 || text.IndexOf("Flush", StringComparison.OrdinalIgnoreCase) >= 0;
			int num = isParagraphFill ? 0 : ((text.IndexOf("Right", StringComparison.OrdinalIgnoreCase) >= 0) ? 2 : ((text.IndexOf("Left", StringComparison.OrdinalIgnoreCase) < 0) ? 1 : 0));
			int num2 = ((text.IndexOf("Top", StringComparison.OrdinalIgnoreCase) < 0) ? ((text.IndexOf("Bottom", StringComparison.OrdinalIgnoreCase) < 0 && text.IndexOf("Baseline", StringComparison.OrdinalIgnoreCase) < 0) ? 1 : 2) : 0);
			if (num2 == 0 && num == 0)
			{
				return (TextAnchor)0;
			}
			if (num2 == 0 && num == 1)
			{
				return (TextAnchor)1;
			}
			if (num2 == 0 && num == 2)
			{
				return (TextAnchor)2;
			}
			if (num2 == 1 && num == 0)
			{
				return (TextAnchor)3;
			}
			if (num2 == 1 && num == 1)
			{
				return (TextAnchor)4;
			}
			if (num2 == 1 && num == 2)
			{
				return (TextAnchor)5;
			}
			if (num2 == 2 && num == 0)
			{
				return (TextAnchor)6;
			}
			if (num2 == 2 && num == 1)
			{
				return (TextAnchor)7;
			}
			return (TextAnchor)8;
		}
		catch (Exception reportedException)
		{
			ReportCaughtException(reportedException);
		}
		return (TextAnchor)4;
	}

	private static Vector4 GetTmpMargin(object tmpComponent)
	{
		try
		{
			object obj = (tmpComponent?.GetType().GetProperty("margin", BindingFlags.Instance | BindingFlags.Public | BindingFlags.NonPublic))?.GetValue(tmpComponent);
			if (obj is Vector4)
			{
				return (Vector4)obj;
			}
		}
		catch (Exception reportedException)
		{
			ReportCaughtException(reportedException);
		}
		return Vector4.zero;
	}

	private static float GetTmpLineSpacing(object tmpComponent)
	{
		try
		{
			object obj = (tmpComponent?.GetType().GetProperty("lineSpacing", BindingFlags.Instance | BindingFlags.Public | BindingFlags.NonPublic))?.GetValue(tmpComponent);
			if (obj is float num)
			{
				return Mathf.Max(0.6f, (num <= 0f) ? 1f : num);
			}
			if (obj is double num2)
			{
				return Mathf.Max(0.6f, (float)((num2 <= 0.0) ? 1.0 : num2));
			}
		}
		catch (Exception reportedException)
		{
			ReportCaughtException(reportedException);
		}
		return 1f;
	}

	private static void ApplyTmpOverlayRect(Text overlay, object tmpComponent)
	{
		if ((Object)(object)overlay == (Object)null)
		{
			return;
		}
		try
		{
			Transform transform = ((Component)overlay).transform;
			RectTransform val = (RectTransform)(object)((transform is RectTransform) ? transform : null);
			if (!((Object)(object)val == (Object)null))
			{
				Vector4 tmpMargin = GetTmpMargin(tmpComponent);
				Component val2 = (Component)((tmpComponent is Component) ? tmpComponent : null);
				RectTransform val3 = (RectTransform)(object)((val2?.transform is RectTransform) ? val2.transform : null);
				Transform sourceParent = (Object)(object)val3 == (Object)null ? null : ((Transform)val3).parent;
				if ((Object)(object)sourceParent != (Object)null && transform.parent != sourceParent)
				{
					transform.SetParent(sourceParent, false);
					transform.SetAsLastSibling();
					if (Interlocked.Exchange(ref _tmpOverlayReparentLogged, 1) == 0)
					{
						_instance?.Logger.LogInfo(
							"[OVERLAY-LAYOUT] UGUI fallback followed a TMP source that acquired or changed its parent after overlay creation.");
					}
				}
				if ((Object)(object)val3 != (Object)null && transform.parent == sourceParent)
				{
					RectTransform parentRect = sourceParent as RectTransform;
					float sourceWidth = val3.rect.width;
					float sourceHeight = val3.rect.height;
					bool sourceRectIsNarrow = sourceWidth > CollapsedTmpOverlayWidth &&
						sourceWidth <= sourceHeight * NarrowTmpOverlayWidthToHeight &&
						(Object)(object)parentRect != (Object)null &&
						parentRect.rect.width >= sourceWidth * NarrowTmpOverlayParentWidthMultiplier;
					bool useParentRect = (sourceWidth <= CollapsedTmpOverlayWidth || sourceRectIsNarrow) &&
						(Object)(object)parentRect != (Object)null &&
						parentRect.rect.width > CollapsedTmpOverlayWidth &&
						parentRect.rect.height > CollapsedTmpOverlayWidth &&
						parentRect.rect.width <= Mathf.Max(96f, sourceHeight * 4f) &&
						parentRect.rect.height <= Mathf.Max(96f, sourceHeight * 2f);
					if (useParentRect)
					{
						val.anchorMin = Vector2.zero;
						val.anchorMax = Vector2.one;
						val.pivot = val3.pivot;
						val.offsetMin = new Vector2(tmpMargin.x, tmpMargin.w);
						val.offsetMax = new Vector2(0f - tmpMargin.z, 0f - tmpMargin.y);
						if (Interlocked.Exchange(ref _tmpCollapsedOverlayRectLogged, 1) == 0)
						{
							_instance?.Logger.LogInfo(
								$"[OVERLAY-LAYOUT] A collapsed or narrow TMP source rect ({sourceWidth:0.0}x{sourceHeight:0.0}) is using its immediate parent bounds ({parentRect.rect.width:0.0}x{parentRect.rect.height:0.0}) for the UGUI fallback.");
						}
					}
					else
					{
						val.anchorMin = val3.anchorMin;
						val.anchorMax = val3.anchorMax;
						val.pivot = val3.pivot;
						val.offsetMin = val3.offsetMin + new Vector2(tmpMargin.x, tmpMargin.w);
						val.offsetMax = val3.offsetMax + new Vector2(0f - tmpMargin.z, 0f - tmpMargin.y);
					}
					transform.localScale = ((Transform)val3).localScale;
					transform.localRotation = ((Transform)val3).localRotation;
				}
				else
				{
					val.anchorMin = Vector2.zero;
					val.anchorMax = Vector2.one;
					val.offsetMin = new Vector2(tmpMargin.x, tmpMargin.w);
					val.offsetMax = new Vector2(0f - tmpMargin.z, 0f - tmpMargin.y);
					transform.localScale = Vector3.one;
					transform.localRotation = Quaternion.identity;
				}
			}
		}
		catch (Exception reportedException)
		{
			ReportCaughtException(reportedException);
		}
	}

	private void EnsureTmpOverlayMatchesCurrentText(object tmpComponent, string currentText)
	{
		Component val = (Component)((tmpComponent is Component) ? tmpComponent : null);
		if (val == null)
		{
			return;
		}
		TmpOverlayState component = val.GetComponent<TmpOverlayState>();
		if (!((Object)(object)component?.overlayText == (Object)null) && ((Behaviour)component.overlayText).enabled)
		{
			string a = NormalizeRequestText(currentText);
			bool num = !string.IsNullOrWhiteSpace(component.sourceNormalized) && string.Equals(a, component.sourceNormalized, StringComparison.Ordinal);
			bool flag = !string.IsNullOrWhiteSpace(component.displayNormalized) && string.Equals(a, component.displayNormalized, StringComparison.Ordinal);
			if (!num && !flag)
			{
				RestoreTmpOverlay(tmpComponent);
			}
		}
	}

	private void RegisterTmpOverlay(TmpOverlayState overlayState, object tmpComponent)
	{
		if ((Object)(object)overlayState == (Object)null)
		{
			return;
		}
		overlayState.componentRef = new WeakReference(tmpComponent);
		if (overlayState.registered)
		{
			return;
		}
		lock (_pendingLock)
		{
			if (!overlayState.registered)
			{
				overlayState.registered = true;
				_activeTmpOverlays.Add(new WeakReference(overlayState));
			}
		}
	}

	private void ValidateActiveTmpOverlays(int maxCount)
	{
		for (int i = 0; i < maxCount; i++)
		{
			TmpOverlayState tmpOverlayState = null;
			lock (_pendingLock)
			{
				while (_activeTmpOverlays.Count > 0)
				{
					int index = _activeTmpOverlays.Count - 1;
					tmpOverlayState = _activeTmpOverlays[index].Target as TmpOverlayState;
					_activeTmpOverlays.RemoveAt(index);
					if ((Object)(object)tmpOverlayState != (Object)null && (Object)(object)tmpOverlayState.overlayText != (Object)null && ((Behaviour)tmpOverlayState.overlayText).enabled)
					{
						break;
					}
					if ((Object)(object)tmpOverlayState != (Object)null)
					{
						tmpOverlayState.registered = false;
					}
					tmpOverlayState = null;
				}
			}
			if ((Object)(object)tmpOverlayState == (Object)null)
			{
				break;
			}
			object obj = tmpOverlayState.componentRef?.Target;
			if (!IsUnityObjectAlive(obj))
			{
				tmpOverlayState.registered = false;
				continue;
			}
			if (tmpOverlayState.pendingExternalWriteDepth > 0)
			{
				lock (_pendingLock)
				{
					_activeTmpOverlays.Insert(0, new WeakReference(tmpOverlayState));
				}
				continue;
			}
			string currentComponentText = GetCurrentComponentText(obj, isTmp: true);
			EnsureTmpOverlayMatchesCurrentText(obj, currentComponentText);
			if ((Object)(object)tmpOverlayState.overlayText != (Object)null && ((Behaviour)tmpOverlayState.overlayText).enabled)
			{
				RefreshTmpOverlayLiveStyle(tmpOverlayState.overlayText, obj, tmpOverlayState);
				if (EnsureTmpOverlayGeometry(tmpOverlayState.overlayText, tmpOverlayState))
				{
					HideTmpSourceForOverlay(obj, tmpOverlayState);
				}
				else
				{
					RestoreTmpSourceVisibility(obj, tmpOverlayState);
				}
				lock (_pendingLock)
				{
					_activeTmpOverlays.Insert(0, new WeakReference(tmpOverlayState));
				}
			}
			else
			{
				tmpOverlayState.registered = false;
			}
		}
	}

	private bool IsTmpOverlayCurrent(object tmpComponent, string translatedText, string sourceText)
	{
		Component val = (Component)((tmpComponent is Component) ? tmpComponent : null);
		if (val == null)
		{
			return false;
		}
		TmpOverlayState component = val.GetComponent<TmpOverlayState>();
		if ((Object)(object)component?.overlayText == (Object)null || !((Behaviour)component.overlayText).enabled)
		{
			return false;
		}
		string b = NormalizeRequestText(sourceText ?? translatedText);
		string b2 = NormalizeRequestText(translatedText);
		if (string.Equals(component.sourceNormalized, b, StringComparison.Ordinal))
		{
			return string.Equals(component.displayNormalized, b2, StringComparison.Ordinal);
		}
		return false;
	}

	private void RestoreTmpOverlay(object tmpComponent)
	{
		Component val = (Component)((tmpComponent is Component) ? tmpComponent : null);
		if (val == null)
		{
			return;
		}
		TmpOverlayState component = val.GetComponent<TmpOverlayState>();
		if ((Object)(object)component == (Object)null)
		{
			return;
		}
		if ((Object)(object)component.overlayText != (Object)null)
		{
			if (((Behaviour)component.overlayText).enabled)
			{
				Interlocked.Increment(ref _tmpOverlayRestoredCount);
			}
			((Behaviour)component.overlayText).enabled = false;
			component.overlayText.text = string.Empty;
		}
		RestoreTmpSourceVisibility(tmpComponent, component);
		component.sourceText = null;
		component.sourceNormalized = null;
		component.displayNormalized = null;
		component.hasSourceEnabledState = false;
	}

	private void BeginTmpExternalTextWrite(object tmpComponent, string value)
	{
		Component component = tmpComponent as Component;
		if ((Object)(object)component == (Object)null)
		{
			return;
		}
		TmpOverlayState state = component.GetComponent<TmpOverlayState>();
		if ((Object)(object)state == (Object)null)
		{
			return;
		}
		if (state.pendingExternalWriteDepth == 0 &&
			((Object)(object)state.overlayText != (Object)null || state.hasSourceEnabledState) &&
			Interlocked.Exchange(ref _tmpExternalWriteBoundaryLogged, 1) == 0)
		{
			base.Logger.LogInfo("[OVERLAY-OWNERSHIP] A game-side TMP text setter replaced an overlaid label; overlay validation is deferred until the setter postfix commits the new text.");
		}
		state.pendingExternalWriteDepth++;
		state.pendingExternalTextNormalized = NormalizeRequestText(value);
	}

	private static bool HasPendingTmpExternalTextWrite(object tmpComponent)
	{
		Component component = tmpComponent as Component;
		if ((Object)(object)component == (Object)null)
		{
			return false;
		}
		TmpOverlayState state = component.GetComponent<TmpOverlayState>();
		return (Object)(object)state != (Object)null && state.pendingExternalWriteDepth > 0;
	}

	private void EndTmpExternalTextWrite(object tmpComponent, string value)
	{
		Component component = tmpComponent as Component;
		if ((Object)(object)component == (Object)null)
		{
			return;
		}
		TmpOverlayState state = component.GetComponent<TmpOverlayState>();
		if ((Object)(object)state == (Object)null || state.pendingExternalWriteDepth <= 0)
		{
			return;
		}
		state.pendingExternalWriteDepth--;
		if (state.pendingExternalWriteDepth > 0)
		{
			state.pendingExternalTextNormalized = NormalizeRequestText(value);
			return;
		}
		state.pendingExternalTextNormalized = null;
		string currentText = GetCurrentComponentText(tmpComponent, isTmp: true);
		EnsureTmpOverlayMatchesCurrentText(tmpComponent, currentText);
	}

	/* TMP set_text 原方法抛异常时 postfix 被跳过，这里由 finalizer 兜底归零深度，
	   异常本身不属于本插件的故障，但深度卡死是本插件的状态损坏，必须限速记录。 */
	private void ResetTmpExternalWriteDepthAfterSetterException(object tmpComponent, Exception setterException, string boundary)
	{
		Component component = tmpComponent as Component;
		if ((Object)(object)component == (Object)null)
		{
			return;
		}
		TmpOverlayState state = component.GetComponent<TmpOverlayState>();
		if ((Object)(object)state == (Object)null || state.pendingExternalWriteDepth <= 0)
		{
			return;
		}
		state.pendingExternalWriteDepth = 0;
		state.pendingExternalTextNormalized = null;
		ReportCaughtException(setterException, "boundary=" + boundary);
	}

	private static void RestoreTmpSourceVisibility(object tmpComponent, TmpOverlayState state)
	{
		// The overlay owns only alpha and enabled. RGB remains game-owned because
		// games commonly recolor a reusable TMP label while the overlay is active.
		if (state != null && state.hasOriginalColor)
		{
			SetTmpAlpha(tmpComponent, state.originalColor.a);
		}
		else if (state != null && state.originalAlpha >= 0f)
		{
			SetTmpAlpha(tmpComponent, state.originalAlpha);
		}
		else
		{
			SetTmpAlpha(tmpComponent, 1f);
		}
		if (state != null && state.hasSourceEnabledState && tmpComponent is Behaviour sourceBehaviour && (Object)(object)sourceBehaviour != (Object)null)
		{
			sourceBehaviour.enabled = state.sourceWasEnabled;
		}
	}

	private static void HideTmpSourceForOverlay(object tmpComponent, TmpOverlayState state)
	{
		if (tmpComponent is Behaviour sourceBehaviour && (Object)(object)sourceBehaviour != (Object)null)
		{
			if (state != null && !state.hasSourceEnabledState)
			{
				state.sourceWasEnabled = sourceBehaviour.enabled;
				state.hasSourceEnabledState = true;
			}
			sourceBehaviour.enabled = false;
		}
		SetTmpAlpha(tmpComponent, 0f);
	}

	private bool EnsureTmpOverlayGeometry(Text overlayText, TmpOverlayState state)
	{
		if ((Object)(object)overlayText == (Object)null || !((Behaviour)overlayText).enabled || !((Component)overlayText).gameObject.activeInHierarchy)
		{
			return false;
		}
		try
		{
			TextGenerator generator = overlayText.cachedTextGenerator;
			CanvasRenderer canvasRenderer = ((Component)overlayText).GetComponent<CanvasRenderer>();
			int visibleCharacters = generator?.characterCountVisible ?? 0;
			int materialCount = canvasRenderer?.materialCount ?? 0;
			if (visibleCharacters > 0 && materialCount > 0)
			{
				return true;
			}
			if (state == null || state.overlayGeometryRetryCount >= 8)
			{
				return false;
			}
			state.overlayGeometryRetryCount++;
			((Graphic)overlayText).SetAllDirty();
			((Graphic)overlayText).Rebuild(CanvasUpdate.PreRender);
			generator = overlayText.cachedTextGenerator;
			canvasRenderer = ((Component)overlayText).GetComponent<CanvasRenderer>();
			visibleCharacters = generator?.characterCountVisible ?? 0;
			materialCount = canvasRenderer?.materialCount ?? 0;
			bool ready = visibleCharacters > 0 && materialCount > 0;
			if (!ready && (state.overlayGeometryRetryCount == 1 || state.overlayGeometryRetryCount == 8))
			{
				base.Logger.LogWarning($"[OVERLAY-GEOMETRY] UGUI overlay rebuild did not produce visible geometry (attempt={state.overlayGeometryRetryCount}, chars={visibleCharacters}, materials={materialCount}); TMP source remains visible.");
			}
			return ready;
		}
		catch (Exception ex)
		{
			ReportCaughtException(ex, "renderer=UnityMono overlay=UGUI", "EnsureTmpOverlayGeometry");
			return false;
		}
	}

	private void RefreshTmpOverlayLiveStyle(Text overlayText, object tmpComponent, TmpOverlayState state)
	{
		if ((Object)(object)overlayText == (Object)null || tmpComponent == null || state == null)
		{
			return;
		}
		try
		{
			Color tmpColor = GetTmpColor(tmpComponent);
			if (!state.hasOriginalColor)
			{
				state.originalColor = tmpColor;
				state.originalAlpha = tmpColor.a;
				state.hasOriginalColor = true;
			}
			else
			{
				state.originalColor.r = tmpColor.r;
				state.originalColor.g = tmpColor.g;
				state.originalColor.b = tmpColor.b;
				if (tmpColor.a > 0.001f)
				{
					state.originalColor.a = tmpColor.a;
					state.originalAlpha = tmpColor.a;
				}
			}
			((Component)overlayText).transform.SetAsLastSibling();
			overlayText.font = _chineseFont;
			overlayText.fontSize = GetTmpFontSize(tmpComponent);
			bool tmpAutoSizing = GetTmpAutoSizing(tmpComponent);
			overlayText.resizeTextForBestFit = tmpAutoSizing;
			int fallbackMinSize = Mathf.Max(12, Mathf.RoundToInt((float)overlayText.fontSize * 0.55f));
			overlayText.resizeTextMinSize = tmpAutoSizing
				? GetTmpFontSizeMin(tmpComponent, fallbackMinSize)
				: fallbackMinSize;
			overlayText.resizeTextMaxSize = tmpAutoSizing
				? Mathf.Max(overlayText.resizeTextMinSize, GetTmpFontSizeMax(tmpComponent, overlayText.fontSize))
				: overlayText.fontSize;
			overlayText.lineSpacing = GetTmpLineSpacing(tmpComponent);
			overlayText.alignment = GetTmpTextAnchor(tmpComponent);
			overlayText.horizontalOverflow = GetTmpWordWrap(tmpComponent) ? HorizontalWrapMode.Wrap : HorizontalWrapMode.Overflow;
			overlayText.verticalOverflow = VerticalWrapMode.Truncate;
			ApplyTmpOverlayRect(overlayText, tmpComponent);
			((Graphic)overlayText).color = GetTmpOverlayDisplayColor(tmpComponent, tmpColor, state);
			((Graphic)overlayText).SetAllDirty();
		}
		catch (Exception ex)
		{
			ReportCaughtException(ex, "renderer=UnityMono overlay=UGUI phase=refresh-live-style", "RefreshTmpOverlayLiveStyle");
		}
	}

	private bool ApplyTmpOverlay(object tmpComponent, string translatedText, string sourceText = null, bool force = false)
	{
		/* 覆盖层是最终显示降级：仅当 TMP 字体不能完整覆盖文本时使用...		   ...TMP 组件仍保留，恢复时按保存的颜...透明度还原，避免破坏游戏逻辑...*/
		if ((!force && !IsTmpOverlayMode() && HasUsableTmpFont()) || (Object)(object)_chineseFont == (Object)null || string.IsNullOrWhiteSpace(translatedText))
		{
			RestoreTmpOverlay(tmpComponent);
			Component val = (Component)((tmpComponent is Component) ? tmpComponent : null);
			if (val != null && (Object)(object)val != (Object)null && (Object)(object)val.GetComponent<TmpOverlayState>() == (Object)null)
			{
				RescueStrandedAlpha(tmpComponent);
			}
			return false;
		}
		if (!IsUsableFontForText(_chineseFont, translatedText))
		{
			RestoreTmpOverlay(tmpComponent);
			if (!_cjkFontWarningLogged)
			{
				_cjkFontWarningLogged = true;
				base.Logger.LogWarning("[FONT] UGUI overlay font cannot render the translated glyphs; preserving the original TMP text.");
			}
			return false;
		}
		if (_overlayDisabled && !force && !ShouldUseTmpOverlay())
		{
			RestoreTmpOverlay(tmpComponent);
			Component val2 = (Component)((tmpComponent is Component) ? tmpComponent : null);
			if (val2 != null && (Object)(object)val2 != (Object)null && (Object)(object)val2.GetComponent<TmpOverlayState>() == (Object)null)
			{
				RescueStrandedAlpha(tmpComponent);
			}
			if (!_cjkFontWarningLogged)
			{
				_cjkFontWarningLogged = true;
				base.Logger.LogWarning("[v3.1.47] CJK TMP font unavailable and overlay disabled. Text set directly on TMP components - may render as squares if font lacks CJK glyphs.");
			}
			return false;
		}
		Component val3 = (Component)((tmpComponent is Component) ? tmpComponent : null);
		if (val3 == null)
		{
			return false;
		}
		Transform transform = val3.transform;
		RectTransform val4 = (RectTransform)(object)((transform is RectTransform) ? transform : null);
		if ((Object)(object)val4 == (Object)null)
		{
			return false;
		}
		if (IsTmpOverlayCurrent(tmpComponent, translatedText, sourceText ?? translatedText))
		{
			TmpOverlayState currentState = val3.GetComponent<TmpOverlayState>();
			RefreshTmpOverlayLiveStyle(currentState.overlayText, tmpComponent, currentState);
			if (EnsureTmpOverlayGeometry(currentState.overlayText, currentState))
			{
				HideTmpSourceForOverlay(tmpComponent, currentState);
				return true;
			}
			RestoreTmpSourceVisibility(tmpComponent, currentState);
			return false;
		}
		try
		{
			TmpOverlayState tmpOverlayState = val3.GetComponent<TmpOverlayState>();
			if ((Object)(object)tmpOverlayState == (Object)null)
			{
				tmpOverlayState = val3.gameObject.AddComponent<TmpOverlayState>();
			}
			if ((Object)(object)tmpOverlayState.overlayText == (Object)null)
			{
				GameObject val5 = new GameObject("__DeepSeekOverlay", new Type[5]
				{
					typeof(RectTransform),
					typeof(CanvasRenderer),
					typeof(Text),
					typeof(LayoutElement),
					typeof(TmpOverlayMarker)
				});
				((Object)val5).hideFlags = (HideFlags)52;
				RectTransform component = val5.GetComponent<RectTransform>();
				Transform val6 = ((Transform)val4).parent;
				if ((Object)(object)val6 == (Object)null)
				{
					val6 = (Transform)(object)val4;
				}
				((Transform)component).SetParent(val6, false);
				component.anchorMin = val4.anchorMin;
				component.anchorMax = val4.anchorMax;
				component.offsetMin = val4.offsetMin;
				component.offsetMax = val4.offsetMax;
				component.pivot = val4.pivot;
				((Transform)component).localScale = Vector3.one;
				tmpOverlayState.overlayText = val5.GetComponent<Text>();
				val5.GetComponent<LayoutElement>().ignoreLayout = true;
				((Graphic)tmpOverlayState.overlayText).raycastTarget = false;
				tmpOverlayState.overlayText.supportRichText = true;
				tmpOverlayState.overlayText.resizeTextForBestFit = false;
				tmpOverlayState.overlayText.horizontalOverflow = (HorizontalWrapMode)0;
				tmpOverlayState.overlayText.verticalOverflow = (VerticalWrapMode)1;
				LogVerbose("[FONT] Created TMP overlay for " + ((Object)val3.gameObject).name);
			}
			Text overlayText = tmpOverlayState.overlayText;
			if (!((Object)(object)overlayText == (Object)null))
			{
				RefreshTmpOverlayLiveStyle(overlayText, tmpComponent, tmpOverlayState);
				string sourceForFormatting = sourceText ?? translatedText;
				string text = (ShouldPreserveRichTextForDisplayWithColor(sourceForFormatting, translatedText) ? PrepareTranslatedTextForComponent(overlayText, translatedText, sourceForFormatting) : StripRichTextForPlainText(translatedText));
				if (string.IsNullOrWhiteSpace(GetVisibleText(text)) || (Object)(object)((Component)overlayText).GetComponentInParent<Canvas>() == (Object)null)
				{
					RestoreTmpOverlay(tmpComponent);
					return false;
				}
				overlayText.text = text;
				((Behaviour)overlayText).enabled = true;
				((Graphic)overlayText).SetAllDirty();
				RevealTmpText(tmpComponent, text);
				tmpOverlayState.overlayGeometryRetryCount = 0;
				tmpOverlayState.sourceText = sourceText ?? translatedText;
				tmpOverlayState.sourceNormalized = NormalizeRequestText(tmpOverlayState.sourceText);
				tmpOverlayState.displayNormalized = NormalizeRequestText(overlayText.text);
				RegisterTmpOverlay(tmpOverlayState, tmpComponent);
				if (!EnsureTmpOverlayGeometry(overlayText, tmpOverlayState))
				{
					RestoreTmpSourceVisibility(tmpComponent, tmpOverlayState);
					return false;
				}
				HideTmpSourceForOverlay(tmpComponent, tmpOverlayState);
				return true;
			}
			return false;
		}
		catch (Exception ex)
		{
			base.Logger.LogWarning("[FONT] ApplyTmpOverlay failed: " + ex.Message);
			RestoreTmpOverlay(tmpComponent);
			return false;
		}
	}

	private static Color GetTmpOverlayDisplayColor(object tmpComponent, Color currentColor, TmpOverlayState state)
	{
		// TMP shaders multiply the component's vertex color by _FaceColor. A UGUI
		// fallback that copies only TMP_Text.color can turn dark labels white-on-white.
		Color result = currentColor;
		if (result.a <= 0f && state != null)
		{
			if (state.hasOriginalColor && state.originalColor.a > 0f)
			{
				result.a = state.originalColor.a;
			}
			else if (state.originalAlpha >= 0f)
			{
				result.a = state.originalAlpha;
			}
		}
		if (result.a <= 0f)
		{
			result.a = 1f;
		}
		try
		{
			if (TryReadTmpFaceColor(tmpComponent, out Color faceColor))
			{
				result.r *= faceColor.r;
				result.g *= faceColor.g;
				result.b *= faceColor.b;
				result.a *= faceColor.a;
				if (Interlocked.Exchange(ref _tmpOverlayColorOwnershipLogged, 1) == 0)
				{
					_instance?.Logger.LogInfo("[OVERLAY-COLOR] UGUI fallback is using the live TMP vertex/face color and owns only source alpha/enabled state.");
				}
			}
		}
		catch (Exception ex)
		{
			ReportCaughtException(ex, "renderer=UnityMono overlay=UGUI property=_FaceColor", "GetTmpOverlayDisplayColor");
		}
		return result;
	}

	private static bool TryReadTmpFaceColor(object tmpComponent, out Color color)
	{
		color = Color.white;
		if (tmpComponent == null)
		{
			return false;
		}
		try
		{
			object faceValue = GetPropertyQuiet(tmpComponent.GetType(), "faceColor")?.GetValue(tmpComponent);
			if (faceValue is Color32 faceColor32)
			{
				color = new Color(
					faceColor32.r / 255f,
					faceColor32.g / 255f,
					faceColor32.b / 255f,
					faceColor32.a / 255f);
				return true;
			}
			if (faceValue is Color faceColor)
			{
				color = faceColor;
				return true;
			}
			Material material = ReadFontSharedMaterial(tmpComponent, tmpComponent.GetType());
			return (Object)(object)material != (Object)null && material.HasProperty("_FaceColor") &&
				TryReadMaterialColor(material, "_FaceColor", out color);
		}
		catch (Exception ex)
		{
			ReportCaughtException(ex, "renderer=UnityMono overlay=UGUI property=faceColor", "TryReadTmpFaceColor");
			return false;
		}
	}

	private static bool TryReadMaterialColor(Material material, string propertyName, out Color color)
	{
		color = Color.white;
		if ((Object)(object)material == (Object)null || string.IsNullOrEmpty(propertyName))
		{
			return false;
		}
		try
		{
			MethodInfo stringGetter = material.GetType().GetMethod(
				"GetColor",
				BindingFlags.Instance | BindingFlags.Public | BindingFlags.NonPublic,
				null,
				new Type[1] { typeof(string) },
				null);
			if (stringGetter != null && stringGetter.Invoke(material, new object[1] { propertyName }) is Color stringColor)
			{
				color = stringColor;
				return true;
			}
			MethodInfo propertyToId = typeof(Shader).GetMethod(
				"PropertyToID",
				BindingFlags.Static | BindingFlags.Public | BindingFlags.NonPublic,
				null,
				new Type[1] { typeof(string) },
				null);
			MethodInfo intGetter = material.GetType().GetMethod(
				"GetColor",
				BindingFlags.Instance | BindingFlags.Public | BindingFlags.NonPublic,
				null,
				new Type[1] { typeof(int) },
				null);
			if (propertyToId != null && intGetter != null &&
				propertyToId.Invoke(null, new object[1] { propertyName }) is int propertyId &&
				intGetter.Invoke(material, new object[1] { propertyId }) is Color idColor)
			{
				color = idColor;
				return true;
			}
		}
		catch (Exception ex)
		{
			ReportCaughtException(ex, "renderer=UnityMono overlay=UGUI property=" + propertyName, "TryReadMaterialColor");
		}
		return false;
	}

	private static string ResolveGameRoot()
	{
		if (!string.IsNullOrWhiteSpace(Paths.GameRootPath) && Directory.Exists(Paths.GameRootPath))
		{
			return Paths.GameRootPath;
		}
		return Environment.CurrentDirectory;
	}

	private static string GetLocalCacheFilePath()
	{
		return Path.Combine(ResolveGameRoot(), "TranslationCache", "unity_translation_cache.json");
	}

	private static object GetPreferredEnumValue(Type enumType, params string[] preferredNames)
	{
		if (enumType == null || !enumType.IsEnum)
		{
			return null;
		}
		foreach (string value in preferredNames)
		{
			if (!string.IsNullOrWhiteSpace(value) && Enum.IsDefined(enumType, value))
			{
				return Enum.Parse(enumType, value);
			}
		}
		Array values = Enum.GetValues(enumType);
		if (values.Length <= 0)
		{
			return null;
		}
		return values.GetValue(0);
	}

	private bool HasUsableTmpFont()
	{
		if (IsFontModeNone())
		{
			return false;
		}
		if (_chineseTMPFont == null)
		{
			return false;
		}
		if (!_tmpFontFromPackage && !_tmpFontFromExisting && !_tmpFontFromRuntime)
		{
			return false;
		}
		if (_tmpFontUsabilityCachedAsset == _chineseTMPFont)
		{
			return _tmpFontUsabilityCachedResult;
		}
		_tmpFontUsabilityCachedAsset = _chineseTMPFont;
		_tmpFontUsabilityCachedResult = LooksLikeCjkFontAsset(_chineseTMPFont);
		return _tmpFontUsabilityCachedResult;
	}

	private bool CanTranslateTmp()
	{
		if (IsFontModeNone())
		{
			return false;
		}
		return HasUsableTmpFont();
	}

	private bool CanHandleTmp()
	{
		if (!CanTranslateTmp())
		{
			return ShouldUseTmpOverlay();
		}
		return true;
	}

	private void LogCjkRenderStateOnce(object component, string text)
	{
		string visibleText = GetVisibleText(text);
		if (!IsUnityObjectAlive(component) || string.IsNullOrWhiteSpace(visibleText) || visibleText.Length < 32 || !ContainsCjk(visibleText))
		{
			return;
		}
		int componentInstanceId = GetComponentInstanceId(component);
		lock (_pendingLock)
		{
			if (_cjkRenderDiagnostics.Count >= CjkRenderDiagnosticMax || !_cjkRenderDiagnostics.Add(componentInstanceId))
			{
				return;
			}
		}
		try
		{
			Component unityComponent = component as Component;
			RectTransform rectTransform = unityComponent?.GetComponent<RectTransform>();
			string rect = (Object)(object)rectTransform == (Object)null
				? "<none>"
				: $"{rectTransform.rect.width:0.0}x{rectTransform.rect.height:0.0}@{rectTransform.anchoredPosition.x:0.0},{rectTransform.anchoredPosition.y:0.0} anchors={rectTransform.anchorMin.x:0.00},{rectTransform.anchorMin.y:0.00}/{rectTransform.anchorMax.x:0.00},{rectTransform.anchorMax.y:0.00} pivot={rectTransform.pivot.x:0.00},{rectTransform.pivot.y:0.00}";
			string preview = visibleText.Replace("\r", "\\r").Replace("\n", "\\n");
			if (preview.Length > 96)
			{
				preview = preview.Substring(0, 96) + "...";
			}
			Text uguiText = component as Text;
			if ((Object)(object)uguiText != (Object)null)
			{
				Font font = uguiText.font;
				char firstCjk = visibleText.FirstOrDefault(c => ContainsCjk(c.ToString()));
				bool hasFirstCjk = (Object)(object)font != (Object)null && firstCjk != '\0' && font.HasCharacter(firstCjk);
				TextGenerator generator = uguiText.cachedTextGenerator;
				int generatedVisible = generator?.characterCountVisible ?? -1;
				int generatedVertices = generator?.verts?.Count ?? -1;
				Material material = uguiText.material;
				string materialName = (Object)(object)material == (Object)null ? "<null>" : ((Object)material).name;
				string textureName = (Object)(object)material == (Object)null || !material.HasProperty("_MainTex") || (Object)(object)material.GetTexture("_MainTex") == (Object)null
					? "<null>"
					: ((Object)material.GetTexture("_MainTex")).name;
				base.Logger.LogWarning($"[CJK-RENDER-DIAG] renderer=UGUI path={GetComponentLogPath(component)} rect={rect} textLength={visibleText.Length} font='{((Object)(object)font == (Object)null ? "<null>" : ((Object)font).name)}' dynamic={((Object)(object)font != (Object)null && font.dynamic)} hasFirstCjk={hasFirstCjk} fontSize={uguiText.fontSize} alignment={uguiText.alignment} overflow={uguiText.horizontalOverflow}/{uguiText.verticalOverflow} bestFit={uguiText.resizeTextForBestFit} richText={uguiText.supportRichText} generatedVisible={generatedVisible} generatedVertices={generatedVertices} material='{materialName}' texture='{textureName}' text='{preview}'");
				return;
			}
			Type componentType = component.GetType();
			object fontAsset = GetPropertyQuiet(componentType, "font")?.GetValue(component);
			Material fontMaterial = ReadFontSharedMaterial(component, componentType);
			object alignment = GetPropertyQuiet(componentType, "alignment")?.GetValue(component);
			object maxVisibleCharacters = GetPropertyQuiet(componentType, "maxVisibleCharacters")?.GetValue(component);
			object maxVisibleWords = GetPropertyQuiet(componentType, "maxVisibleWords")?.GetValue(component);
			object maxVisibleLines = GetPropertyQuiet(componentType, "maxVisibleLines")?.GetValue(component);
			object overflowMode = GetPropertyQuiet(componentType, "overflowMode")?.GetValue(component);
			object characterSpacing = GetPropertyQuiet(componentType, "characterSpacing")?.GetValue(component);
			object wordSpacing = GetPropertyQuiet(componentType, "wordSpacing")?.GetValue(component);
			object lineSpacing = GetPropertyQuiet(componentType, "lineSpacing")?.GetValue(component);
			object paragraphSpacing = GetPropertyQuiet(componentType, "paragraphSpacing")?.GetValue(component);
			object richText = GetPropertyQuiet(componentType, "richText")?.GetValue(component);
			object preferredWidth = GetPropertyQuiet(componentType, "preferredWidth")?.GetValue(component);
			object renderedWidth = GetPropertyQuiet(componentType, "renderedWidth")?.GetValue(component);
			object textInfo = GetPropertyQuiet(componentType, "textInfo")?.GetValue(component);
			object generatedCharacters = textInfo == null ? null : GetPropertyQuiet(textInfo.GetType(), "characterCount")?.GetValue(textInfo);
			Texture mainTexture = (Object)(object)fontMaterial == (Object)null || !fontMaterial.HasProperty("_MainTex") ? null : fontMaterial.GetTexture("_MainTex");
			bool hasInlineParagraphAlignment = Regex.IsMatch(text ?? string.Empty, @"<\s*align\s*=", RegexOptions.IgnoreCase);
			base.Logger.LogWarning($"[CJK-RENDER-DIAG] renderer={componentType.FullName} path={GetComponentLogPath(component)} rect={rect} preferred/rendered={preferredWidth ?? "<null>"}/{renderedWidth ?? "<null>"} textLength={visibleText.Length} font='{((fontAsset as Object)?.name ?? "<null>")}' material='{(((Object)(object)fontMaterial == (Object)null) ? "<null>" : ((Object)fontMaterial).name)}' texture='{(((Object)(object)mainTexture == (Object)null) ? "<null>" : ((Object)mainTexture).name)}' alignment={alignment ?? "<null>"} spacing={characterSpacing ?? "<null>"}/{wordSpacing ?? "<null>"}/{lineSpacing ?? "<null>"}/{paragraphSpacing ?? "<null>"} richText={richText ?? "<null>"} inlineAlign={hasInlineParagraphAlignment} overflow={overflowMode ?? "<null>"} maxVisible={maxVisibleCharacters ?? "<null>"}/{maxVisibleWords ?? "<null>"}/{maxVisibleLines ?? "<null>"} generatedCharacters={generatedCharacters ?? "<null>"} text='{preview}'");
		}
		catch (Exception ex)
		{
			ReportCaughtException(ex, "renderer=CJK diagnostic path=" + GetComponentLogPath(component));
		}
	}

	private bool ShouldUseTmpOverlay()
	{
		if (!IsFontModeNone() && (IsTmpOverlayMode() || IsAutoFontMode()) && !HasUsableTmpFont())
		{
			return (Object)(object)_chineseFont != (Object)null;
		}
		return false;
	}

	private void SetChineseTmpFontAsset(object fontAsset, bool fromChineseSource, bool fromPackage = false, string source = null, string bundlePath = null)
	{
		_chineseTMPFont = fontAsset;
		_tmpFontFromChineseSource = fromChineseSource;
		_tmpFontFromPackage = fromPackage;
		_tmpFontFromExisting = !fromPackage && !string.IsNullOrWhiteSpace(source) && source.StartsWith("Existing:", StringComparison.OrdinalIgnoreCase);
		_tmpFontFromRuntime = !fromPackage && !_tmpFontFromExisting && !string.IsNullOrWhiteSpace(source) && source.StartsWith("Runtime:", StringComparison.OrdinalIgnoreCase);
		_tmpFontSource = source ?? ((fontAsset == null) ? "none" : (fromPackage ? "package" : "runtime"));
		_tmpFontBundlePath = bundlePath;
		_tmpFontUsabilityCachedAsset = null;
		_tmpFontUsabilityCachedResult = false;
		EnsureChineseTMPFontMaterial();
	}

	private void DeepInspectPostSwap(object tmpComponent, Type compType, string translatedText)
	{
		if (Volatile.Read(ref _deepInspectCount) >= DeepInspectMax ||
			Interlocked.Increment(ref _deepInspectCount) > DeepInspectMax)
		{
			return;
		}
		try
		{
			string componentLogPath = GetComponentLogPath(tmpComponent);
			string text = "<n/a>";
			try
			{
				Material val = ReadFontSharedMaterial(tmpComponent, compType);
				if ((Object)(object)val != (Object)null)
				{
					string text2 = (((Object)(object)val.shader != (Object)null) ? ((Object)val.shader).name : "<no shader>");
					Texture val2 = (val.HasProperty("_MainTex") ? val.GetTexture("_MainTex") : null);
					text = "name='" + ((Object)val).name + "' shader='" + text2 + "' mainTex='" + (((Object)(object)val2 != (Object)null) ? ((Object)val2).name : "<null>") + "'";
				}
				else
				{
					text = "<null material>";
				}
			}
			catch (Exception ex)
			{
				ReportCaughtException(ex, "member=material");
				text = "<read threw: " + ex.Message + ">";
			}
			string text3 = "<n/a>";
			string text4 = "<n/a>";
			string text5 = "<n/a>";
			try
			{
				Component val3 = (Component)((tmpComponent is Component) ? tmpComponent : null);
				if (val3 != null && (Object)(object)val3 != (Object)null)
				{
					text4 = val3.gameObject.activeInHierarchy.ToString();
					RectTransform component = val3.GetComponent<RectTransform>();
					if ((Object)(object)component != (Object)null)
					{
						Rect rect = component.rect;
						Vector2 size = rect.size;
						text3 = $"size=({size.x:0.0},{size.y:0.0}) anchoredPos=({component.anchoredPosition.x:0.0},{component.anchoredPosition.y:0.0})";
					}
					CanvasRenderer component2 = val3.GetComponent<CanvasRenderer>();
					if ((Object)(object)component2 != (Object)null)
					{
						Material material = component2.GetMaterial();
						text5 = string.Format("alpha={0:0.00} matCount={1} mat0='{2}'", GetCanvasRendererAlphaSafe(component2, 1f), component2.materialCount, ((Object)(object)material != (Object)null) ? ((Object)material).name : "<null>");
					}
				}
			}
			catch (Exception ex2)
			{
				ReportCaughtException(ex2, "member=font/material");
				text3 = "<read threw: " + ex2.Message + ">";
			}
			string text6 = "<n/a>";
			try
			{
				object obj = GetPropertyQuiet(compType, "textInfo")?.GetValue(tmpComponent);
				if (obj != null)
				{
					Type type = obj.GetType();
					int num = (int)(GetPropertyQuiet(type, "characterCount")?.GetValue(obj) ?? ((object)0));
					FieldInfo fieldQuiet = GetFieldQuiet(type, "meshInfo");
					int num2 = 0;
					if (fieldQuiet?.GetValue(obj) is IList list)
					{
						foreach (object item in list)
						{
							if (item != null && GetFieldQuiet(item.GetType(), "vertices")?.GetValue(item) is Array array)
							{
								num2 += array.Length;
							}
						}
					}
					text6 = $"chars={num} totalVerts={num2}";
				}
			}
			catch (Exception ex3)
			{
				ReportCaughtException(ex3, "member=textInfo");
				text6 = "<textInfo threw: " + ex3.Message + ">";
			}
			string text7 = "<n/a>";
			try
			{
				Color tmpColor = GetTmpColor(tmpComponent);
				text7 = $"({tmpColor.r:0.00},{tmpColor.g:0.00},{tmpColor.b:0.00},{tmpColor.a:0.00})";
			}
			catch (Exception reportedException)
			{
				ReportCaughtException(reportedException);
			}
			string text8 = "<n/a>";
			try
			{
				text8 = ((!(AccessTools.Property(compType, "text")?.GetValue(tmpComponent) is string text9)) ? "<null>" : ((text9.Length > 30) ? (text9.Substring(0, 30) + "…") : text9));
			}
			catch (Exception reportedException)
			{
				ReportCaughtException(reportedException);
			}
			base.Logger.LogWarning($"[POST-SWAP-DEEP #{_deepInspectCount}] path={componentLogPath} active={text4} text='{text8}' color={text7} rect={text3} cr={text5} mat={text} textInfo={text6}");
		}
		catch (Exception ex4)
		{
			try
			{
				base.Logger.LogWarning("[POST-SWAP-DEEP] threw: " + ex4.Message);
			}
			catch (Exception reportedException)
			{
				ReportCaughtException(reportedException);
			}
		}
	}

	private void TryClearMaterialReferenceManagerCache()
	{
		try
		{
			Type type = AccessTools.TypeByName("TMPro.MaterialReferenceManager");
			if (type == null)
			{
				return;
			}
			object obj = AccessTools.Property(type, "instance")?.GetValue(null) ?? AccessTools.Field(type, "s_Instance")?.GetValue(null);
			if (obj == null)
			{
				base.Logger.LogWarning("[FONT-MAT-FIX] MaterialReferenceManager.instance not reachable; skipping cache clear");
				return;
			}
			int num = 0;
			BindingFlags bindingAttr = BindingFlags.Instance | BindingFlags.Public | BindingFlags.NonPublic;
			FieldInfo[] fields = type.GetFields(bindingAttr);
			foreach (FieldInfo fieldInfo in fields)
			{
				Type fieldType = fieldInfo.FieldType;
				if (!fieldType.IsGenericType || fieldType.GetGenericTypeDefinition() != typeof(Dictionary<, >))
				{
					continue;
				}
				try
				{
					if (fieldInfo.GetValue(obj) is IDictionary { Count: >0, Count: var count } dictionary)
					{
						dictionary.Clear();
						num += count;
					}
				}
				catch (Exception reportedException)
				{
					ReportCaughtException(reportedException);
				}
			}
			base.Logger.LogWarning($"[FONT-MAT-FIX] cleared {num} entries from MaterialReferenceManager caches");
		}
		catch (Exception ex)
		{
			base.Logger.LogWarning("[FONT-MAT-FIX] MRM cache clear threw: " + ex.Message);
		}
	}

	private void EnsureChineseTMPFontMaterial()
	{
		if (_chineseTMPFont == null)
		{
			return;
		}
		try
		{
			Type type = _chineseTMPFont.GetType();
			object obj = AccessTools.Property(type, "material")?.GetValue(_chineseTMPFont);
			if ((Object)((obj is Material) ? obj : null) != (Object)null)
			{
				Interlocked.Increment(ref _fontMaterialAlreadyOk);
				return;
			}
			Texture2D val = TryReadAtlasTexture(_chineseTMPFont);
			if ((Object)(object)val == (Object)null)
			{
				Interlocked.Increment(ref _fontMaterialFixFailed);
				base.Logger.LogWarning("[FONT-MAT-FIX] no atlas texture found on runtime font asset - cannot construct material");
				return;
			}
			Shader val2 = Shader.Find("TextMeshPro/Distance Field") ?? Shader.Find("TMPro/Distance Field") ?? Shader.Find("TextMeshPro/Mobile/Distance Field") ?? Shader.Find("TMPro/Mobile/Distance Field");
			if ((Object)(object)val2 == (Object)null)
			{
				Interlocked.Increment(ref _fontMaterialFixFailed);
				base.Logger.LogWarning("[FONT-MAT-FIX] TextMeshPro/Distance Field shader not present in scene - cannot construct material");
				return;
			}
			Material val3 = new Material(val2)
			{
				name = "TranslatorCJK_AtlasMaterial"
			};
			if (val3.HasProperty("_MainTex"))
			{
				SetMaterialTextureSafe(val3, "_MainTex", (Texture)(object)val);
			}
			int num = TryReadIntMember(_chineseTMPFont, type, "atlasWidth", "m_AtlasWidth");
			int num2 = TryReadIntMember(_chineseTMPFont, type, "atlasHeight", "m_AtlasHeight");
			int num3 = TryReadIntMember(_chineseTMPFont, type, "atlasPadding", "m_AtlasPadding");
			if (num <= 0)
			{
				num = ((Texture)val).width;
			}
			if (num2 <= 0)
			{
				num2 = ((Texture)val).height;
			}
			float num4 = ((num3 > 0) ? ((float)num3 + 1f) : 5f);
			if (val3.HasProperty("_TextureWidth"))
			{
				val3.SetFloat("_TextureWidth", (float)num);
			}
			if (val3.HasProperty("_TextureHeight"))
			{
				val3.SetFloat("_TextureHeight", (float)num2);
			}
			if (val3.HasProperty("_GradientScale"))
			{
				val3.SetFloat("_GradientScale", num4);
			}
			if (val3.HasProperty("_WeightNormal"))
			{
				val3.SetFloat("_WeightNormal", 0f);
			}
			if (val3.HasProperty("_WeightBold"))
			{
				val3.SetFloat("_WeightBold", 0.75f);
			}
			bool flag = false;
			BindingFlags bindingAttr = BindingFlags.Instance | BindingFlags.Public | BindingFlags.NonPublic;
			List<string> list = new List<string>();
			List<string> list2 = new List<string>();
			for (Type type2 = type; type2 != null && type2 != typeof(object); type2 = type2.BaseType)
			{
				FieldInfo[] fields;
				try
				{
					fields = type2.GetFields(bindingAttr);
				}
				catch (Exception ex)
				{
					ReportCaughtException(ex, "type=" + type2.FullName);
					continue;
				}
				FieldInfo[] array = fields;
				foreach (FieldInfo fieldInfo in array)
				{
					if (!(fieldInfo.FieldType != typeof(Material)))
					{
						list.Add(type2.Name + "." + fieldInfo.Name);
						try
						{
							fieldInfo.SetValue(_chineseTMPFont, val3);
							flag = true;
							list2.Add(type2.Name + "." + fieldInfo.Name);
						}
						catch (Exception reportedException)
						{
							ReportCaughtException(reportedException);
						}
					}
				}
			}
			List<string> list3 = new List<string>();
			try
			{
				PropertyInfo[] properties = type.GetProperties(bindingAttr);
				foreach (PropertyInfo propertyInfo in properties)
				{
					if (propertyInfo.PropertyType != typeof(Material))
					{
						continue;
					}
					bool canWrite = propertyInfo.CanWrite;
					list3.Add(propertyInfo.Name + (canWrite ? "(rw)" : "(ro)"));
					if (canWrite)
					{
						try
						{
							propertyInfo.SetValue(_chineseTMPFont, val3);
							flag = true;
							list2.Add("prop:" + propertyInfo.Name);
						}
						catch (Exception reportedException)
						{
							ReportCaughtException(reportedException);
						}
					}
				}
			}
			catch (Exception reportedException)
			{
				ReportCaughtException(reportedException);
			}
			List<string> list4 = new List<string>();
			for (Type type3 = type; type3 != null && type3 != typeof(object); type3 = type3.BaseType)
			{
				FieldInfo[] fields2;
				try
				{
					fields2 = type3.GetFields(bindingAttr);
				}
				catch (Exception ex)
				{
					ReportCaughtException(ex, "type=" + type3.FullName);
					continue;
				}
				FieldInfo[] array = fields2;
				foreach (FieldInfo fieldInfo2 in array)
				{
					Type fieldType = fieldInfo2.FieldType;
					bool flag2 = fieldType == typeof(Material[]);
					bool flag3 = fieldType.IsGenericType && fieldType.GetGenericTypeDefinition() == typeof(List<>) && fieldType.GetGenericArguments()[0] == typeof(Material);
					if (!flag2 && !flag3)
					{
						continue;
					}
					list4.Add(type3.Name + "." + fieldInfo2.Name + ":" + (flag2 ? "[]" : "List"));
					try
					{
						if (flag2)
						{
							if (!(fieldInfo2.GetValue(_chineseTMPFont) is Material[] array2) || array2.Length == 0)
							{
								fieldInfo2.SetValue(_chineseTMPFont, new Material[1] { val3 });
							}
							else
							{
								array2[0] = val3;
							}
							flag = true;
							list2.Add(type3.Name + "." + fieldInfo2.Name + "[0]");
							continue;
						}
						if (!(fieldInfo2.GetValue(_chineseTMPFont) is IList list5))
						{
							IList list6 = (IList)Activator.CreateInstance(fieldType);
							list6.Add(val3);
							fieldInfo2.SetValue(_chineseTMPFont, list6);
						}
						else if (list5.Count == 0)
						{
							list5.Add(val3);
						}
						else
						{
							list5[0] = val3;
						}
						flag = true;
						list2.Add(type3.Name + "." + fieldInfo2.Name + "[0]");
					}
					catch (Exception ex)
					{
						base.Logger.LogWarning("[FONT-MAT-FIX] writing to " + type3.Name + "." + fieldInfo2.Name + " threw: " + ex.Message);
					}
				}
			}
			base.Logger.LogWarning("[FONT-MAT-FIX] type=" + type.FullName + " fields=[" + string.Join(",", list) + "] arrays=[" + string.Join(",", list4) + "] props=[" + string.Join(",", list3) + "]");
			try
			{
				int num5 = 0;
				int num6 = 0;
				try
				{
					if (AccessTools.Field(type, "m_GlyphTable")?.GetValue(_chineseTMPFont) is IList list7)
					{
						num5 = list7.Count;
					}
				}
				catch (Exception reportedException)
				{
					ReportCaughtException(reportedException);
				}
				try
				{
					if (AccessTools.Field(type, "m_CharacterTable")?.GetValue(_chineseTMPFont) is IList list8)
					{
						num6 = list8.Count;
					}
				}
				catch (Exception reportedException)
				{
					ReportCaughtException(reportedException);
				}
				bool flag4 = false;
				bool flag5 = false;
				try
				{
					MethodInfo methodInfo = AccessTools.Method(type, "HasCharacter", new Type[1] { typeof(int) });
					if (methodInfo != null)
					{
						flag4 = (bool)methodInfo.Invoke(_chineseTMPFont, new object[1] { 20013 });
						flag5 = (bool)methodInfo.Invoke(_chineseTMPFont, new object[1] { 25991 });
					}
				}
				catch (Exception reportedException)
				{
					ReportCaughtException(reportedException);
				}
				base.Logger.LogWarning($"[FONT-DEEP] glyphTable={num5} charTable={num6} hasChar(中)={flag4} hasChar(文)={flag5} atlas={((Texture)val).width}x{((Texture)val).height} format={val.format} isReadable={((Texture)val).isReadable}");
				try
				{
					if (((Texture)val).isReadable)
					{
						int num7 = ((Texture)val).width / 2;
						int num8 = ((Texture)val).height / 2;
						Color pixel = GetTexturePixelSafe(val, num7, num8);
						base.Logger.LogWarning($"[FONT-DEEP] atlas center pixel rgba=({pixel.r:0.00},{pixel.g:0.00},{pixel.b:0.00},{pixel.a:0.00})");
					}
					else
					{
						base.Logger.LogWarning("[FONT-DEEP] atlas not readable from CPU - cannot sample pixels");
					}
				}
				catch (Exception ex2)
				{
					base.Logger.LogWarning("[FONT-DEEP] pixel sample threw: " + ex2.Message);
				}
			}
			catch (Exception ex3)
			{
				base.Logger.LogWarning("[FONT-DEEP] inspect threw: " + ex3.Message);
			}
			if (flag)
			{
				TryClearMaterialReferenceManagerCache();
				Interlocked.Increment(ref _fontMaterialFixed);
				base.Logger.LogWarning(string.Format("[FONT-MAT-FIX] installed default material via [{0}] (atlas={1}x{2} padding={3} gradientScale={4:0.00})", string.Join(",", list2), num, num2, num3, num4));
			}
			else
			{
				Interlocked.Increment(ref _fontMaterialFixFailed);
				base.Logger.LogWarning("[FONT-MAT-FIX] no writable field/property accepted the new material");
			}
		}
		catch (Exception ex4)
		{
			Interlocked.Increment(ref _fontMaterialFixFailed);
			base.Logger.LogWarning("[FONT-MAT-FIX] exception: " + ex4.Message);
		}
	}

	private static bool LooksLikeCjkFontAsset(object fontAsset)
	{
		if (fontAsset == null)
		{
			return false;
		}
		List<string> list = new List<string>();
		Object val = (Object)((fontAsset is Object) ? fontAsset : null);
		if (val != null && !string.IsNullOrWhiteSpace(val.name))
		{
			list.Add(val.name);
		}
		Type type = fontAsset.GetType();
		try
		{
			object obj = AccessTools.Property(type, "sourceFontFile")?.GetValue(fontAsset);
			Object val2 = (Object)((obj is Object) ? obj : null);
			if (val2 != (Object)null && !string.IsNullOrWhiteSpace(val2.name))
			{
				list.Add(val2.name);
			}
		}
		catch (Exception reportedException)
		{
			ReportCaughtException(reportedException);
		}
		try
		{
			object obj3 = AccessTools.Property(type, "faceInfo")?.GetValue(fontAsset);
			if (obj3 != null)
			{
				Type type2 = obj3.GetType();
				FieldInfo field = type2.GetField("familyName");
				FieldInfo field2 = type2.GetField("styleName");
				string text = field?.GetValue(obj3)?.ToString();
				string text2 = field2?.GetValue(obj3)?.ToString();
				if (!string.IsNullOrWhiteSpace(text))
				{
					list.Add(text);
				}
				if (!string.IsNullOrWhiteSpace(text2))
				{
					list.Add(text2);
				}
			}
		}
		catch (Exception reportedException)
		{
			ReportCaughtException(reportedException);
		}
		string text3 = string.Join("|", list.Where((string t) => !string.IsNullOrWhiteSpace(t)));
		bool flag = false;
		if (!string.IsNullOrWhiteSpace(text3))
		{
			string[] array = new string[15]
			{
				"cjk", "noto", "hans", "hant", "chinese", "yahei", "msyh", "simhei", "simsun", "source han",
				"sourcehan", "pingfang", "heiti", "wenquanyi", "unicode"
			};
			foreach (string value in array)
			{
				if (text3.IndexOf(value, StringComparison.OrdinalIgnoreCase) >= 0)
				{
					flag = true;
					break;
				}
			}
		}
		try
		{
			if (AccessTools.Property(type, "characterTable")?.GetValue(fontAsset) is IEnumerable enumerable)
			{
				foreach (object item in enumerable)
				{
					if (item != null)
					{
						object obj5 = AccessTools.Property(item.GetType(), "unicode")?.GetValue(item);
						if (obj5 is uint num2 && num2 >= 19968 && num2 <= 40959)
						{
							return true;
						}
						if (obj5 is int num3 && num3 >= 19968 && num3 <= 40959)
						{
							return true;
						}
					}
				}
			}
		}
		catch (Exception reportedException)
		{
			ReportCaughtException(reportedException);
		}
		try
		{
			string text4 = (flag ? "知道任务返回背包" : "知道任务返回");
			if (TryAddCharactersToTmpFontAsset(fontAsset, text4))
			{
				return true;
			}
		}
		catch (Exception reportedException)
		{
			ReportCaughtException(reportedException);
		}
		return false;
	}

	private object FindExistingCjkTmpFont(Type tmpFontAssetType)
	{
		if (tmpFontAssetType == null)
		{
			return null;
		}
		try
		{
			Object[] array = FindObjectsOfTypeAllSafe(tmpFontAssetType);
			foreach (Object val in array)
			{
				if (val != (Object)null && LooksLikeCjkFontAsset(val))
				{
					base.Logger.LogInfo($"Using existing CJK TMP_FontAsset: {val}");
					return val;
				}
			}
		}
		catch (Exception ex)
		{
			base.Logger.LogWarning("Find existing CJK TMP_FontAsset failed: " + ex.Message);
		}
		return null;
	}

	private object TryUseExistingCjkTmpFont(Type tmpFontAssetType)
	{
		if (tmpFontAssetType == null || HasUsableTmpFont())
		{
			return _chineseTMPFont;
		}
		object obj = FindExistingCjkTmpFont(tmpFontAssetType);
		if (obj == null)
		{
			return null;
		}
		SetChineseTmpFontAsset(obj, fromChineseSource: true, fromPackage: false, "Existing:Resources");
		ConfigureTMPFallbackFont();
		base.Logger.LogInfo("[FONT-EXISTING] Using game-provided CJK TMP_FontAsset; no runtime TMP asset was created.");
		return obj;
	}

	private string GetUnityVersionSafe()
	{
		if (_unityVersionResolved)
		{
			return _resolvedUnityVersion;
		}
		_unityVersionResolved = true;
		try
		{
			PropertyInfo unityVersion = typeof(Application).GetProperty("unityVersion", BindingFlags.Public | BindingFlags.Static);
			if (unityVersion != null)
			{
				_resolvedUnityVersion = unityVersion.GetValue(null, null) as string ?? string.Empty;
			}
		}
		catch (Exception ex)
		{
			ReportCaughtException(ex, "renderer=UnityMono property=Application.unityVersion", "GetUnityVersionSafe");
		}
		if (!string.IsNullOrWhiteSpace(_resolvedUnityVersion))
		{
			return _resolvedUnityVersion;
		}
		try
		{
			string unityPlayerPath = Path.Combine(Paths.GameRootPath, "UnityPlayer.dll");
			if (File.Exists(unityPlayerPath))
			{
				System.Diagnostics.FileVersionInfo versionInfo = System.Diagnostics.FileVersionInfo.GetVersionInfo(unityPlayerPath);
				_resolvedUnityVersion = versionInfo.ProductVersion ?? versionInfo.FileVersion ?? string.Empty;
			}
		}
		catch (Exception ex)
		{
			ReportCaughtException(ex, "renderer=UnityMono file=UnityPlayer.dll", "GetUnityVersionSafe");
		}
		base.Logger.LogWarning("Application.unityVersion is unavailable; using UnityPlayer file version '" + _resolvedUnityVersion + "' for renderer-local font selection.");
		return _resolvedUnityVersion;
	}

	private IEnumerable<string> GetTmpFontBundleCandidates()
	{
		string pluginDir = Path.GetDirectoryName(base.Info.Location);
		if (string.IsNullOrWhiteSpace(pluginDir))
		{
			yield break;
		}
		string text = GetUnityVersionSafe();
		string[] array = text.Split(new char[] { '.' }, StringSplitOptions.None);
		List<string> list = new List<string>();
		if (!string.IsNullOrWhiteSpace(text))
		{
			list.Add(text);
		}
		if (array.Length >= 3)
		{
			list.Add(array[0] + "." + array[1] + "." + array[2]);
		}
		if (array.Length >= 2)
		{
			list.Add(array[0] + "." + array[1]);
		}
		if (array.Length >= 1 && !string.IsNullOrWhiteSpace(array[0]))
		{
			list.Add(array[0]);
		}
		list.Add("default");
		string[] fileNames = new string[5] { "TranslatorCJKFont.bundle", "TranslatorCJKFont.unity3d", "translator_cjk_tmpfont.bundle", "translator_font.bundle", "fonts.bundle" };
		string fontDir = Path.GetFullPath(Path.Combine(pluginDir, "..", "font"));
		string major = (array.Length >= 1) ? array[0] : string.Empty;
		List<string> packagedNames = new List<string>();
		HashSet<string> packagedNameSet = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
		string preferredPackage = null;
		switch (major)
		{
		case "6000":
			preferredPackage = "arialuni_sdf_u6000";
			break;
		case "2022":
			preferredPackage = "arialuni_sdf_u2022";
			break;
		case "2021":
			preferredPackage = "arialuni_sdf_u2021";
			break;
		case "2020":
		case "2019":
			preferredPackage = "arialuni_sdf_u2019";
			break;
		case "2018":
			preferredPackage = "arialuni_sdf_u2018";
			break;
		case "2017":
		case "5":
			preferredPackage = "arialuni_sdf-u55to2017";
			break;
		}
		if (!string.IsNullOrWhiteSpace(preferredPackage) && packagedNameSet.Add(preferredPackage))
		{
			packagedNames.Add(preferredPackage);
		}
		string[] knownPackagedNames = new string[6] { "arialuni_sdf_u2019", "arialuni_sdf_u2021", "arialuni_sdf_u2022", "arialuni_sdf_u2018", "arialuni_sdf-u55to2017", "arialuni_sdf_u6000" };
		foreach (string packagedName in knownPackagedNames)
		{
			if (packagedNameSet.Add(packagedName))
			{
				packagedNames.Add(packagedName);
			}
		}
		string[] array2;
		foreach (string versionKey in list.Where((string v) => !string.IsNullOrWhiteSpace(v)).Distinct(StringComparer.OrdinalIgnoreCase))
		{
			array2 = fileNames;
			foreach (string path in array2)
			{
				yield return Path.Combine(pluginDir, "TranslatorFonts", versionKey, path);
			}
		}
		foreach (string packagedName in packagedNames)
		{
			yield return Path.Combine(fontDir, packagedName);
		}
		array2 = fileNames;
		foreach (string path in array2)
		{
			yield return Path.Combine(fontDir, path);
		}
		array2 = fileNames;
		foreach (string path2 in array2)
		{
			yield return Path.Combine(pluginDir, path2);
		}
	}

	/* Unity modules are loaded on demand. A stripped game may ship the retained
	   AssetBundle module without referencing it from gameplay assemblies, leaving
	   AccessTools unable to see the type. Loading that shipped module by assembly
	   identity is the upstream-safe boundary; failure only disables the packaged
	   renderer font and is recorded without accepting any translation result. */
	private Type ResolveAssetBundleTypeSafe()
	{
		Type type = AccessTools.TypeByName("UnityEngine.AssetBundle");
		if (type != null)
		{
			return type;
		}
		try
		{
			Assembly assembly = Assembly.Load("UnityEngine.AssetBundleModule");
			type = assembly?.GetType("UnityEngine.AssetBundle", throwOnError: false);
			if (type != null)
			{
				base.Logger.LogInfo("[FONT-PACK] Loaded optional UnityEngine.AssetBundleModule for packaged TMP fonts.");
			}
			return type;
		}
		catch (Exception ex)
		{
			ReportCaughtException(ex, "renderer=UnityMono assembly=UnityEngine.AssetBundleModule", "ResolveAssetBundleTypeSafe");
			return null;
		}
	}

	private object LoadPackagedTmpFontAsset(Type tmpFontAssetType)
	{
		if (tmpFontAssetType == null)
		{
			return null;
		}
		if (HasUsableTmpFont())
		{
			return _chineseTMPFont;
		}
		if (_tmpFontPackageSearchExhausted)
		{
			return null;
		}
		Type assetBundleType = ResolveAssetBundleTypeSafe();
		if (assetBundleType == null)
		{
			base.Logger.LogWarning("[FONT-PACK] UnityEngine.AssetBundle is unavailable; packaged TMP font bundles will be skipped on this Unity profile.");
			_tmpFontPackageSearchExhausted = true;
			return null;
		}
		MethodInfo loadFromFile = AccessTools.Method(assetBundleType, "LoadFromFile", new Type[1] { typeof(string) });
		MethodInfo loadAllAssets = AccessTools.Method(assetBundleType, "LoadAllAssets", new Type[1] { typeof(Type) });
		if (loadFromFile == null || loadAllAssets == null)
		{
			base.Logger.LogWarning("[FONT-PACK] AssetBundle font-loading methods are unavailable; packaged TMP font bundles will be skipped on this Unity profile.");
			_tmpFontPackageSearchExhausted = true;
			return null;
		}
		foreach (string item in GetTmpFontBundleCandidates().Distinct(StringComparer.OrdinalIgnoreCase))
		{
			if (string.IsNullOrWhiteSpace(item) || !File.Exists(item))
			{
				continue;
			}
			object val = null;
			try
			{
				base.Logger.LogInfo("[FONT-PACK] Loading TMP font bundle: " + item);
				val = loadFromFile.Invoke(null, new object[1] { item });
				if (val == null)
				{
					base.Logger.LogWarning("[FONT-PACK] AssetBundle.LoadFromFile returned null: " + item);
					continue;
				}
				Array array = loadAllAssets.Invoke(val, new object[1] { tmpFontAssetType }) as Array;
				if (array == null || array.Length == 0)
				{
					base.Logger.LogWarning("[FONT-PACK] No TMP_FontAsset in bundle: " + item);
					continue;
				}
				foreach (object item2 in array)
				{
					Object val2 = item2 as Object;
					if (!(val2 == (Object)null))
					{
						if (LooksLikeCjkFontAsset(val2) || TryAddCharactersToTmpFontAsset(val2, "知道任务返回背包"))
						{
							_tmpFontPackageSearchExhausted = false;
							SetChineseTmpFontAsset(val2, fromChineseSource: true, fromPackage: true, "AssetBundle:" + Path.GetFileName(item), item);
							base.Logger.LogInfo($"[FONT-PACK] Loaded packaged CJK TMP font asset: {val2} ({item})");
							return val2;
						}
						base.Logger.LogWarning($"[FONT-PACK] Rejected TMP font asset without CJK coverage: {val2}");
					}
				}
			}
			catch (Exception ex)
			{
				ReportCaughtException(ex, "renderer=UnityMono method=AssetBundle reflective-font-load", "LoadPackagedTmpFontAsset");
			}
		}
		base.Logger.LogWarning("[FONT-PACK] No packaged TMP font bundle found for Unity " + GetUnityVersionSafe() + "; TMP translation disabled to avoid squares.");
		_tmpFontPackageSearchExhausted = true;
		return null;
	}

	private bool HasPackagedTmpFontBundleFile()
	{
		try
		{
			foreach (string item in GetTmpFontBundleCandidates().Distinct(StringComparer.OrdinalIgnoreCase))
			{
				if (!string.IsNullOrWhiteSpace(item) && File.Exists(item))
				{
					return true;
				}
			}
		}
		catch (Exception reportedException)
		{
			ReportCaughtException(reportedException);
		}
		return false;
	}

	private void RefreshVisibleTmpComponents()
	{
		try
		{
			Type type = AccessTools.TypeByName("TMPro.TMP_Text");
			if (type == null)
			{
				return;
			}
			Object[] array = FindTmpTextObjectsSafe(type);
			foreach (Object val in array)
			{
				if (IsUnityObjectAlive(val) && IsComponentActive(val))
				{
					ApplyTMPFont(val);
					InvokeForceMeshUpdate(val, ((object)val).GetType());
				}
			}
		}
		catch (Exception ex)
		{
			base.Logger.LogWarning("Refresh TMP components failed: " + ex.Message);
		}
	}

	private IEnumerator EnsureTmpFontReadyCoroutine()
	{
		yield return (object)WaitForScaledSecondsSafe(1.5f);
		for (int attempt = 1; attempt <= 6; attempt++)
		{
			if (HasUsableTmpFont())
			{
				ConfigureTMPFallbackFont();
				RefreshVisibleTmpComponents();
				BeginSceneWarmupGeneration();
				yield break;
			}
			Type type = AccessTools.TypeByName("TMPro.TMP_FontAsset");
			if (type != null)
			{
				if (HasPackagedTmpFontBundleFile())
				{
					LoadPackagedTmpFontAsset(type);
				}
				if (!HasUsableTmpFont())
				{
					TryUseExistingCjkTmpFont(type);
				}
				if (!HasUsableTmpFont())
				{
					EnsureTmpFontFromUnityFont(type);
				}
				if (HasUsableTmpFont())
				{
					ConfigureTMPFallbackFont();
					RefreshVisibleTmpComponents();
					BeginSceneWarmupGeneration();
					base.Logger.LogInfo($"TMP CJK font ready after retry {attempt}");
					yield break;
				}
			}
			yield return (object)WaitForScaledSecondsSafe(1.5f);
		}
		if (ShouldUseTmpOverlay())
		{
			base.Logger.LogWarning("TMP CJK font asset remains unavailable; renderer-local UGUI overlay fallback remains active.");
		}
		else
		{
			base.Logger.LogWarning("TMP CJK font asset is still unavailable; TMP translation remains disabled to avoid squares.");
		}
	}

	/* Some external stripped UnityEngine builds retain WaitForSeconds but remove
	   its float constructor. That assembly cannot be repaired by the plugin. This
	   scaled-frame iterator preserves the lifecycle delay without hiding failed
	   translations or creating cache entries; the selected fallback is logged at
	   startup when the constructor is absent. */
	private static IEnumerator WaitForScaledSecondsSafe(float seconds)
	{
		float startedAt = Time.time;
		float delay = Mathf.Max(0f, seconds);
		do
		{
			yield return null;
		}
		while (Time.time - startedAt < delay);
	}

	private void LogCoroutineDelayCompatibility()
	{
		try
		{
			if (typeof(WaitForSeconds).GetConstructor(new Type[1] { typeof(float) }) == null)
			{
				base.Logger.LogWarning("[LIFECYCLE] Unity WaitForSeconds(float) is unavailable; using the scaled-frame compatibility iterator.");
			}
		}
		catch (Exception ex)
		{
			ReportCaughtException(ex, "renderer=UnityMono method=WaitForSeconds..ctor(float)", "LogCoroutineDelayCompatibility");
		}
	}

	private object CreateTmpFontAssetFromFont(Type tmpFontAssetType, Font sourceFont)
	{
		if (tmpFontAssetType == null || (Object)(object)sourceFont == (Object)null)
		{
			return null;
		}
		try
		{
			Type type = AccessTools.TypeByName("UnityEngine.TextCore.LowLevel.GlyphRenderMode");
			Type type2 = AccessTools.TypeByName("TMPro.AtlasPopulationMode");
			if (type != null && type2 != null)
			{
				base.Logger.LogInfo($"[FONT-CREATE] Types found: GlyphRenderMode={type != null}, AtlasPopulationMode={type2 != null}");
				MethodInfo method = tmpFontAssetType.GetMethod("CreateFontAsset", BindingFlags.Static | BindingFlags.Public | BindingFlags.NonPublic, null, new Type[8]
				{
					typeof(Font),
					typeof(int),
					typeof(int),
					type,
					typeof(int),
					typeof(int),
					type2,
					typeof(bool)
				}, null);
				if (method != null)
				{
					object preferredEnumValue = GetPreferredEnumValue(type, "SDFAA", "SDFAA_HINTED", "SMOOTH_HINTED", "SMOOTH");
					object preferredEnumValue2 = GetPreferredEnumValue(type2, "Dynamic", "DynamicOS");
					if (preferredEnumValue != null && preferredEnumValue2 != null)
					{
						object obj = method.Invoke(null, new object[8] { sourceFont, 64, 6, preferredEnumValue, 4096, 4096, preferredEnumValue2, true });
						if (obj != null)
						{
							base.Logger.LogInfo("[FONT-CREATE] Rich CreateFontAsset returned non-null for " + ((Object)sourceFont).name);
							TryWarmTmpFontAsset(obj, "知道继续任务");
							if (LooksLikeCjkFontAsset(obj) || TryAddCharactersToTmpFontAsset(obj, "知道任务返回"))
							{
								base.Logger.LogInfo("Created TMP font asset from OS font with dynamic atlas: " + ((Object)sourceFont).name);
								return obj;
							}
							base.Logger.LogWarning("[FONT-CREATE] Rich CreateFontAsset returned an asset but TryAddCharacters failed (font has no usable face data): " + ((Object)sourceFont).name);
						}
						else
						{
							base.Logger.LogWarning("[FONT-CREATE] Rich CreateFontAsset.Invoke returned null for " + ((Object)sourceFont).name + " (TMP rejected the source Font)");
						}
					}
					else
					{
						base.Logger.LogWarning($"[FONT-CREATE] Rich CreateFontAsset enum lookup failed: glyphRenderMode={preferredEnumValue != null}, atlasPopulationMode={preferredEnumValue2 != null}");
					}
				}
				else
				{
					base.Logger.LogInfo("[FONT-CREATE] Rich CreateFontAsset overload not present; trying simple overload");
				}
			}
		}
		catch (Exception ex)
		{
			base.Logger.LogWarning("[FONT-CREATE] Rich CreateFontAsset failed for " + ((Object)sourceFont).name + ": " + (ex.InnerException?.Message ?? ex.Message));
		}
		try
		{
			MethodInfo method2 = tmpFontAssetType.GetMethod("CreateFontAsset", BindingFlags.Static | BindingFlags.Public | BindingFlags.NonPublic, null, new Type[1] { typeof(Font) }, null);
			if (method2 != null)
			{
				object obj2 = method2.Invoke(null, new object[1] { sourceFont });
				if (obj2 != null)
				{
					TryWarmTmpFontAsset(obj2, "知道继续任务");
					if (LooksLikeCjkFontAsset(obj2) || TryAddCharactersToTmpFontAsset(obj2, "知道任务返回"))
					{
						base.Logger.LogInfo("Created TMP font asset from OS font: " + ((Object)sourceFont).name);
						return obj2;
					}
					base.Logger.LogWarning("[FONT-CREATE] Simple CreateFontAsset returned an asset but TryAddCharacters failed: " + ((Object)sourceFont).name);
				}
				else
				{
					base.Logger.LogWarning("[FONT-CREATE] Simple CreateFontAsset.Invoke returned null for " + ((Object)sourceFont).name);
				}
			}
			else
			{
				base.Logger.LogWarning("[FONT-CREATE] Simple CreateFontAsset(Font) overload not found");
			}
		}
		catch (Exception ex2)
		{
			base.Logger.LogWarning("[FONT-CREATE] Simple CreateFontAsset failed for " + ((Object)sourceFont).name + ": " + (ex2.InnerException?.Message ?? ex2.Message));
		}
		return null;
	}

	private object CreateTmpFontAssetManually(Type tmpFontAssetType, Font sourceFont)
	{
		if (tmpFontAssetType == null || (Object)(object)sourceFont == (Object)null)
		{
			return null;
		}
		try
		{
			MethodInfo method = typeof(ScriptableObject).GetMethod("CreateInstance", BindingFlags.Static | BindingFlags.Public, null, new Type[1] { typeof(Type) }, null);
			if (method == null)
			{
				base.Logger.LogWarning("[FONT-MANUAL] ScriptableObject.CreateInstance not found");
				return null;
			}
			object obj = method.Invoke(null, new object[1] { tmpFontAssetType });
			if (obj == null)
			{
				base.Logger.LogWarning("[FONT-MANUAL] ScriptableObject.CreateInstance returned null");
				return null;
			}
			base.Logger.LogInfo("[FONT-MANUAL] Created TMP_FontAsset instance, setting up for: " + ((Object)sourceFont).name);
			FieldInfo fieldInfo = AccessTools.Field(tmpFontAssetType, "m_SourceFontFile");
			if (fieldInfo != null)
			{
				fieldInfo.SetValue(obj, sourceFont);
				base.Logger.LogInfo("[FONT-MANUAL] Set m_SourceFontFile");
			}
			else
			{
				AccessTools.Property(tmpFontAssetType, "sourceFontFile")?.SetValue(obj, sourceFont);
				base.Logger.LogInfo("[FONT-MANUAL] Set sourceFontFile via property");
			}
			Type type = AccessTools.TypeByName("TMPro.AtlasPopulationMode");
			if (type != null)
			{
				object preferredEnumValue = GetPreferredEnumValue(type, "Dynamic", "DynamicOS");
				if (preferredEnumValue != null)
				{
					FieldInfo fieldInfo2 = AccessTools.Field(tmpFontAssetType, "m_AtlasPopulationMode");
					if (fieldInfo2 != null)
					{
						fieldInfo2.SetValue(obj, preferredEnumValue);
						base.Logger.LogInfo($"[FONT-MANUAL] Set m_AtlasPopulationMode = {preferredEnumValue}");
					}
					else
					{
						AccessTools.Property(tmpFontAssetType, "atlasPopulationMode")?.SetValue(obj, preferredEnumValue);
					}
				}
			}
			FieldInfo fieldInfo3 = AccessTools.Field(tmpFontAssetType, "m_AtlasWidth");
			FieldInfo fieldInfo4 = AccessTools.Field(tmpFontAssetType, "m_AtlasHeight");
			fieldInfo3?.SetValue(obj, 4096);
			fieldInfo4?.SetValue(obj, 4096);
			FieldInfo fieldInfo5 = AccessTools.Field(tmpFontAssetType, "m_SamplingPointSize");
			if (fieldInfo5 != null)
			{
				if (fieldInfo5.FieldType == typeof(float))
				{
					fieldInfo5.SetValue(obj, 90f);
				}
				else
				{
					fieldInfo5.SetValue(obj, 90);
				}
			}
			AccessTools.Field(tmpFontAssetType, "m_AtlasPadding")?.SetValue(obj, 9);
			Type type2 = AccessTools.TypeByName("UnityEngine.TextCore.LowLevel.GlyphRenderMode");
			if (type2 != null)
			{
				object preferredEnumValue2 = GetPreferredEnumValue(type2, "SDFAA", "SDFAA_HINTED", "SMOOTH_HINTED", "SMOOTH");
				if (preferredEnumValue2 != null)
				{
					AccessTools.Field(tmpFontAssetType, "m_AtlasRenderMode")?.SetValue(obj, preferredEnumValue2);
				}
			}
			PropertyInfo propertyInfo = AccessTools.Property(tmpFontAssetType, "atlasTexture");
			if (propertyInfo != null && propertyInfo.GetValue(obj) == null)
			{
				Texture2D val = new Texture2D(4096, 4096, (TextureFormat)1, false);
				((Object)val).name = "TranslatorCJKAtlas";
				FieldInfo fieldInfo6 = AccessTools.Field(tmpFontAssetType, "m_AtlasTextures");
				if (fieldInfo6 != null)
				{
					fieldInfo6.SetValue(obj, new Texture2D[1] { val });
					base.Logger.LogInfo("[FONT-MANUAL] Set m_AtlasTextures");
				}
				FieldInfo fieldInfo7 = AccessTools.Field(tmpFontAssetType, "m_Material") ?? AccessTools.Field(tmpFontAssetType, "material");
				if (fieldInfo7 != null)
				{
					Material val2 = new Material(Shader.Find("TextMeshPro/Mobile/Distance Field") ?? Shader.Find("TextMeshPro/Distance Field") ?? Shader.Find("TMPro/Mobile/Distance Field") ?? Shader.Find("GUI/Text Shader") ?? Shader.Find("UI/Default"));
					if ((Object)(object)val2 != (Object)null)
					{
						SetMaterialTextureSafe(val2, "_MainTex", (Texture)(object)val);
						fieldInfo7.SetValue(obj, val2);
						base.Logger.LogInfo("[FONT-MANUAL] Created and set material");
					}
				}
			}
			try
			{
				FieldInfo fieldInfo8 = AccessTools.Field(tmpFontAssetType, "m_GlyphTable");
				if (fieldInfo8 != null && fieldInfo8.GetValue(obj) == null)
				{
					Type type3 = AccessTools.TypeByName("UnityEngine.TextCore.Glyph");
					if (type3 != null)
					{
						Type type4 = typeof(List<>).MakeGenericType(type3);
						fieldInfo8.SetValue(obj, Activator.CreateInstance(type4));
					}
				}
				FieldInfo fieldInfo9 = AccessTools.Field(tmpFontAssetType, "m_CharacterTable");
				if (fieldInfo9 != null && fieldInfo9.GetValue(obj) == null)
				{
					Type type5 = AccessTools.TypeByName("TMPro.TMP_Character");
					if (type5 != null)
					{
						Type type6 = typeof(List<>).MakeGenericType(type5);
						fieldInfo9.SetValue(obj, Activator.CreateInstance(type6));
					}
				}
			}
			catch (Exception ex)
			{
				base.Logger.LogInfo("[FONT-MANUAL] Table init: " + ex.Message);
			}
			try
			{
				FieldInfo fieldInfo10 = AccessTools.Field(tmpFontAssetType, "m_GlyphLookupDictionary");
				if (fieldInfo10 != null && fieldInfo10.GetValue(obj) == null)
				{
					Type type7 = AccessTools.TypeByName("UnityEngine.TextCore.Glyph");
					if (type7 != null)
					{
						Type type8 = typeof(Dictionary<, >).MakeGenericType(typeof(uint), type7);
						fieldInfo10.SetValue(obj, Activator.CreateInstance(type8));
					}
				}
				FieldInfo fieldInfo11 = AccessTools.Field(tmpFontAssetType, "m_CharacterLookupDictionary");
				if (fieldInfo11 != null && fieldInfo11.GetValue(obj) == null)
				{
					Type type9 = AccessTools.TypeByName("TMPro.TMP_Character");
					if (type9 != null)
					{
						Type type10 = typeof(Dictionary<, >).MakeGenericType(typeof(uint), type9);
						fieldInfo11.SetValue(obj, Activator.CreateInstance(type10));
					}
				}
			}
			catch (Exception ex2)
			{
				base.Logger.LogInfo("[FONT-MANUAL] Lookup init: " + ex2.Message);
			}
			Object val3 = (Object)((obj is Object) ? obj : null);
			if (val3 != null)
			{
				val3.name = "TranslatorCJK-" + ((Object)sourceFont).name;
			}
			try
			{
				AccessTools.Method(tmpFontAssetType, "ReadFontAssetDefinition")?.Invoke(obj, null);
				base.Logger.LogInfo("[FONT-MANUAL] Called ReadFontAssetDefinition");
			}
			catch (Exception ex3)
			{
				base.Logger.LogInfo("[FONT-MANUAL] ReadFontAssetDefinition: " + ex3.Message);
			}
			if (TryAddCharactersToTmpFontAsset(obj, "知道任务"))
			{
				base.Logger.LogInfo("[FONT-MANUAL] SUCCESS - TryAddCharacters verified for " + ((Object)sourceFont).name);
				return obj;
			}
			base.Logger.LogWarning("[FONT-MANUAL] TryAddCharacters failed for " + ((Object)sourceFont).name + "; rejecting TMP font asset");
			return null;
		}
		catch (Exception ex4)
		{
			base.Logger.LogWarning("[FONT-MANUAL] Failed: " + (ex4.InnerException?.Message ?? ex4.Message));
			return null;
		}
	}

	private void EnsureTmpFontFromUnityFont(Type tmpFontAssetType)
	{
		if (HasUsableTmpFont() || _tmpFontCreationExhausted)
		{
			return;
		}
		if (tmpFontAssetType == null)
		{
			_tmpFontCreationExhausted = true;
			return;
		}
		string text = ResolveCanonicalFontPath();
		if (!string.IsNullOrWhiteSpace(text))
		{
			base.Logger.LogInfo("[FONT-CREATE] Attempting runtime TMP_FontAsset creation from path " + text);
			object obj = CreateTmpFontAssetFromPath(tmpFontAssetType, text);
			if (obj != null)
			{
				SetChineseTmpFontAsset(obj, fromChineseSource: true, fromPackage: false, "Runtime:Path:" + Path.GetFileName(text));
				_tmpFontPackageSearchExhausted = false;
				_tmpFontCreationExhausted = false;
				base.Logger.LogInfo("[FONT-CREATE] Runtime TMP_FontAsset ready (" + _tmpFontSource + ")");
				return;
			}
		}
		if ((Object)(object)_chineseFont != (Object)null)
		{
			base.Logger.LogInfo("[FONT-CREATE] Attempting runtime TMP_FontAsset creation from Font " + ((Object)_chineseFont).name);
			object obj2 = CreateTmpFontAssetFromFont(tmpFontAssetType, _chineseFont);
			if (obj2 == null)
			{
				obj2 = CreateTmpFontAssetManually(tmpFontAssetType, _chineseFont);
			}
			if (obj2 != null)
			{
				SetChineseTmpFontAsset(obj2, fromChineseSource: true, fromPackage: false, "Runtime:Font:" + ((Object)_chineseFont).name);
				_tmpFontPackageSearchExhausted = false;
				_tmpFontCreationExhausted = false;
				base.Logger.LogInfo("[FONT-CREATE] Runtime TMP_FontAsset ready (" + _tmpFontSource + ")");
				return;
			}
		}
		_tmpFontCreationExhausted = true;
		base.Logger.LogWarning("[FONT-CREATE] Runtime TMP_FontAsset creation failed; TMP translation will stay disabled.");
	}

	private string ResolveCanonicalFontPath()
	{
		string text = _customFontPath?.Value;
		if (!string.IsNullOrWhiteSpace(text) && File.Exists(text))
		{
			return text;
		}
		string path = ResolveGameRoot();
		string text2 = (string.IsNullOrEmpty(base.Info?.Location) ? null : Path.GetDirectoryName(base.Info.Location));
		List<string> list = new List<string>
		{
			Path.Combine(path, "TranslationFonts", "NotoSansCJKsc-Regular.otf"),
			Path.Combine(path, "TranslationFonts", "NotoSansCJKsc-Regular.ttf"),
			Path.Combine(path, "TranslationFonts", "msyh.ttc"),
			Path.Combine(path, "TranslationFonts", "simhei.ttf")
		};
		if (!string.IsNullOrEmpty(text2))
		{
			list.Add(Path.Combine(text2, "TranslatorFont.otf"));
			list.Add(Path.Combine(text2, "TranslatorFont.ttf"));
		}
		foreach (string item in list)
		{
			try
			{
				if (!string.IsNullOrWhiteSpace(item) && File.Exists(item))
				{
					return item;
				}
			}
			catch (Exception reportedException)
			{
				ReportCaughtException(reportedException);
			}
		}
		return null;
	}

	private object CreateTmpFontAssetFromPath(Type tmpFontAssetType, string fontPath)
	{
		if (tmpFontAssetType == null || string.IsNullOrWhiteSpace(fontPath) || !File.Exists(fontPath))
		{
			return null;
		}
		try
		{
			Type type = AccessTools.TypeByName("UnityEngine.TextCore.LowLevel.GlyphRenderMode");
			Type type2 = AccessTools.TypeByName("TMPro.AtlasPopulationMode");
			if (type == null || type2 == null)
			{
				return null;
			}
			object preferredEnumValue = GetPreferredEnumValue(type, "SDFAA", "SDFAA_HINTED", "SMOOTH_HINTED", "SMOOTH");
			object preferredEnumValue2 = GetPreferredEnumValue(type2, "Dynamic", "DynamicOS");
			if (preferredEnumValue == null || preferredEnumValue2 == null)
			{
				return null;
			}
			MethodInfo method = tmpFontAssetType.GetMethod("CreateFontAsset", BindingFlags.Static | BindingFlags.Public | BindingFlags.NonPublic, null, new Type[9]
			{
				typeof(string),
				typeof(int),
				typeof(int),
				typeof(int),
				type,
				typeof(int),
				typeof(int),
				type2,
				typeof(bool)
			}, null);
			if (method != null)
			{
				object obj = method.Invoke(null, new object[9] { fontPath, 0, 64, 6, preferredEnumValue, 4096, 4096, preferredEnumValue2, true });
				if (obj != null)
				{
					TryWarmTmpFontAsset(obj, "知道继续任务");
					if (LooksLikeCjkFontAsset(obj) || TryAddCharactersToTmpFontAsset(obj, "知道任务返回"))
					{
						return obj;
					}
					base.Logger.LogWarning("[FONT-CREATE] Path-based asset rejected (no CJK coverage): " + fontPath);
				}
			}
			else
			{
				base.Logger.LogInfo("[FONT-CREATE] TMP path-based CreateFontAsset overload not present in this TMP version");
			}
		}
		catch (Exception ex)
		{
			base.Logger.LogWarning("[FONT-CREATE] Path-based CreateFontAsset failed for " + fontPath + ": " + (ex.InnerException?.Message ?? ex.Message));
		}
		return null;
	}

	private Font TryLoadCustomFontFile()
	{
		string text = _customFontPath?.Value;
		if (string.IsNullOrWhiteSpace(text) || !File.Exists(text))
		{
			text = ResolveCanonicalFontPath();
		}
		if (string.IsNullOrWhiteSpace(text) || !File.Exists(text))
		{
			return null;
		}
		Font obj = LoadFontFromFilePath(text);
		if ((Object)(object)obj != (Object)null)
		{
			base.Logger.LogInfo("Using custom font file: " + text);
		}
		return obj;
	}

	private Font LoadFontFromFilePath(string path)
	{
		if (string.IsNullOrWhiteSpace(path) || !File.Exists(path))
		{
			return null;
		}
		try
		{
			MethodInfo methodInfo = AccessTools.Method(typeof(Font), "Internal_CreateFontFromPath", new Type[2]
			{
				typeof(Font),
				typeof(string)
			});
			if (methodInfo == null)
			{
				base.Logger.LogWarning("[FONT-LOAD] Font.Internal_CreateFontFromPath not found; the font file cannot be loaded safely on this Unity profile.");
				return null;
			}
			Font val = FormatterServices.GetUninitializedObject(typeof(Font)) as Font;
			if (ReferenceEquals(val, null))
			{
				base.Logger.LogWarning("[FONT-LOAD] Unity Font wrapper could not be allocated without the stripped default constructor; the font file was not loaded.");
				return null;
			}
			methodInfo.Invoke(null, new object[2] { val, path });
			if ((Object)(object)val == (Object)null)
			{
				base.Logger.LogWarning("[FONT-LOAD] Retained Font.Internal_CreateFontFromPath returned without creating a live native font for " + path + ".");
				return null;
			}
			((Object)val).name = Path.GetFileNameWithoutExtension(path);
			PinRuntimeFont(val, path);
			return val;
		}
		catch (Exception ex)
		{
			base.Logger.LogWarning("[FONT-LOAD] Failed to load Font from " + path + ": " + (ex.InnerException?.Message ?? ex.Message));
			return null;
		}
	}

	/* Highly stripped UnityEngine builds can remove the public OS-font factory.
	   Internal_CreateFont(Font,string) is the Font(string) asset-name constructor;
	   it does not create a dynamic OS font and must never be treated as one. A
	   preloader compatibility patch may restore only Internal_CreateDynamicFont's
	   original internal-call declaration. If neither real factory exists, return
	   null so TMP keeps its source text instead of hiding it behind a false overlay.
	   This cannot accept or persist a translation, and every rejection is logged. */
	private Font CreateDynamicFontFromOSFontSafe(string fontName, int fontSize)
	{
		if (!_createDynamicFontFromOsFontResolved)
		{
			_createDynamicFontFromOsFontResolved = true;
			_createDynamicFontFromOsFontMethod = typeof(Font).GetMethod("CreateDynamicFontFromOSFont", BindingFlags.Public | BindingFlags.Static, null, new Type[2]
			{
				typeof(string),
				typeof(int)
			}, null);
			if (_createDynamicFontFromOsFontMethod == null)
			{
				_internalCreateDynamicFontMethod = typeof(Font).GetMethod("Internal_CreateDynamicFont", BindingFlags.NonPublic | BindingFlags.Static, null, new Type[3]
				{
					typeof(Font),
					typeof(string[]),
					typeof(int)
				}, null);
				if (_internalCreateDynamicFontMethod != null)
				{
					base.Logger.LogInfo("[FONT] Optional Unity Font.CreateDynamicFontFromOSFont(string,int) is unavailable; using the restored Font.Internal_CreateDynamicFont(Font,string[],int) boundary.");
				}
				else
				{
					base.Logger.LogWarning("[FONT] Optional Unity Font.CreateDynamicFontFromOSFont(string,int) is unavailable and the internal dynamic-font factory is unavailable; keeping TMP source text unless another verified font is found.");
				}
			}
		}
		if ((_createDynamicFontFromOsFontMethod == null && _internalCreateDynamicFontMethod == null) || string.IsNullOrWhiteSpace(fontName))
		{
			return null;
		}
		try
		{
			if (_createDynamicFontFromOsFontMethod != null)
			{
				Font font = _createDynamicFontFromOsFontMethod.Invoke(null, new object[2] { fontName, fontSize }) as Font;
				if (!IsUsableFontForText(font, "简体中文"))
				{
					base.Logger.LogWarning("[FONT] Unity OS-font factory returned a font without verified CJK glyph coverage for " + fontName + ".");
					return null;
				}
				PinRuntimeFont(font, fontName);
				return font;
			}
			/* The restored declaration is an internal call, so allocate only the
			   managed shell and let Unity create its native dynamic-font object once. */
			Font val = FormatterServices.GetUninitializedObject(typeof(Font)) as Font;
			if (ReferenceEquals(val, null))
			{
				base.Logger.LogWarning("[FONT] Unable to allocate an uninitialized Unity Font wrapper for the internal dynamic-font factory.");
				return null;
			}
			_internalCreateDynamicFontMethod.Invoke(null, new object[3]
			{
				val,
				new string[1] { fontName },
				fontSize
			});
			if ((Object)(object)val == (Object)null)
			{
				base.Logger.LogWarning("[FONT] Font.Internal_CreateDynamicFont returned without creating a live native font for " + fontName + ".");
				return null;
			}
			if (!IsUsableFontForText(val, "简体中文"))
			{
				base.Logger.LogWarning("[FONT] Font.Internal_CreateDynamicFont produced a font without verified CJK glyph coverage for " + fontName + ".");
				return null;
			}
			PinRuntimeFont(val, fontName);
			return val;
		}
		catch (Exception ex)
		{
			ReportCaughtException(ex, "renderer=UnityMono method=Font.CreateDynamicFontFromOSFont", "CreateDynamicFontFromOSFontSafe");
			return null;
		}
	}

	private static bool IsUsableFontForText(Font font, string text)
	{
		if (ReferenceEquals(font, null) || (Object)(object)font == (Object)null || string.IsNullOrWhiteSpace(text))
		{
			return false;
		}
		try
		{
			bool dynamicFont = font.dynamic;
			bool checkedGlyph = false;
			foreach (char c in GetVisibleText(text))
			{
				if (char.IsWhiteSpace(c) || char.IsControl(c))
				{
					continue;
				}
				checkedGlyph = true;
				if (!font.HasCharacter(c))
				{
					return false;
				}
			}
			return checkedGlyph || dynamicFont;
		}
		catch (Exception ex)
		{
			ReportCaughtException(ex, "renderer=UnityMono method=Font.HasCharacter", "IsUsableFontForText");
			return false;
		}
	}

	/* Dynamically created Font objects are external Unity resources. Some stripped
	   players unload their native object during the bootstrap scene transition even
	   while the managed Font remains referenced. The renderer cannot repair that
	   resource sweep upstream, so mark only plugin-created fonts as non-unloadable.
	   A pin failure disables later TMP overlay handling and remains diagnostic; it
	   never turns a miss, pass-through value, or queued request into cache success. */
	private void PinRuntimeFont(Font font, string source)
	{
		if (ReferenceEquals(font, null))
		{
			return;
		}
		try
		{
			Object obj = (Object)(object)font;
			obj.hideFlags |= HideFlags.DontUnloadUnusedAsset;
			Object.DontDestroyOnLoad(obj);
			LogVerbose("[FONT] Pinned runtime font native lifetime: " + source);
		}
		catch (Exception ex)
		{
			ReportCaughtException(ex, "renderer=UnityMono method=PinRuntimeFont source=" + source, "PinRuntimeFont");
		}
	}

	private void FindChineseFont()
	{
		string path = ResolveGameRoot();
		Font val = TryLoadCustomFontFile();
		if ((Object)(object)val != (Object)null)
		{
			_chineseFont = val;
		}
		string[] source = new string[7] { _fontName.Value, "Microsoft YaHei UI", "Microsoft YaHei", "SimHei", "SimSun", "Arial Unicode MS", "Arial" };
		if ((Object)(object)_chineseFont == (Object)null)
		{
			foreach (string item in source.Where((string n) => !string.IsNullOrWhiteSpace(n)).Distinct(StringComparer.OrdinalIgnoreCase))
			{
				try
				{
					Font val2 = CreateDynamicFontFromOSFontSafe(item, 18);
					if ((Object)(object)val2 != (Object)null)
					{
						_chineseFont = val2;
						base.Logger.LogInfo("Using system font: " + item);
						break;
					}
				}
				catch (Exception ex)
				{
					ReportCaughtException(ex, "renderer=UnityMono boundary=os-font-probe font=" + item);
				}
			}
		}
		Type type = AccessTools.TypeByName("TMPro.TMP_FontAsset");
		if (type != null)
		{
			if (HasPackagedTmpFontBundleFile())
			{
				LoadPackagedTmpFontAsset(type);
			}
			if (!HasUsableTmpFont())
			{
				TryUseExistingCjkTmpFont(type);
			}
			if (!HasUsableTmpFont())
			{
				EnsureTmpFontFromUnityFont(type);
			}
			if (HasUsableTmpFont())
			{
				ConfigureTMPFallbackFont();
			}
			else
			{
				base.Logger.LogWarning("[FONT-PACK] No usable TMP font for Unity " + GetUnityVersionSafe() + "; TMP translation disabled to avoid squares.");
				_tmpFontPackageSearchExhausted = true;
			}
		}
		string path2 = Path.Combine(path, "TranslationFonts", "NotoSansCJKsc-Regular.otf");
		if ((Object)(object)_chineseFont == (Object)null && File.Exists(path2))
		{
			try
			{
				Font[] array = FindObjectsOfTypeAllSafe<Font>();
				foreach (Font val3 in array)
				{
					if ((Object)(object)val3 != (Object)null && (((Object)val3).name == "NotoSansCJKsc-Regular" || ((Object)val3).name == "NotoSansCJKsc"))
					{
						_chineseFont = val3;
						base.Logger.LogInfo("Using bundled Font: " + ((Object)val3).name);
						return;
					}
				}
			}
			catch (Exception reportedException)
			{
				ReportCaughtException(reportedException);
			}
		}
		string[] array2 = new string[6] { _fontName.Value, "Microsoft YaHei UI", "Microsoft YaHei", "SimHei", "SimSun", "Arial" };
		if ((Object)(object)_chineseFont != (Object)null)
		{
			return;
		}
		string[] array3 = array2;
		foreach (string text in array3)
		{
			try
			{
				Font val4 = CreateDynamicFontFromOSFontSafe(text, 12);
				if ((Object)(object)val4 != (Object)null)
				{
					_chineseFont = val4;
					base.Logger.LogInfo("Using system font: " + text);
					return;
				}
			}
			catch (Exception reportedException)
			{
				ReportCaughtException(reportedException);
			}
		}
		base.Logger.LogWarning("No Chinese font found!");
	}

	private void ConfigureTMPFallbackFont()
	{
		try
		{
			Type type = AccessTools.TypeByName("TMPro.TMP_FontAsset");
			Type type2 = AccessTools.TypeByName("TMPro.TMP_Settings");
			if (type == null || type2 == null)
			{
				return;
			}
			object obj = AccessTools.Property(type2, "instance")?.GetValue(null);
			if (obj == null)
			{
				return;
			}
			object obj2 = (HasUsableTmpFont() ? _chineseTMPFont : null);
			if (obj2 == null)
			{
				obj2 = LoadPackagedTmpFontAsset(type);
			}
			if (obj2 != null)
			{
				SetChineseTmpFontAsset(obj2, fromChineseSource: true, _tmpFontFromPackage, _tmpFontSource, _tmpFontBundlePath);
				base.Logger.LogInfo("CJK font ready: " + obj2);
				PropertyInfo propertyInfo = AccessTools.Property(type2, "fallbackFontAssets");
				if (propertyInfo != null && propertyInfo.GetValue(obj) is IList list)
				{
					if (!list.Contains(obj2))
					{
						list.Add(obj2);
						base.Logger.LogInfo("Added to fallback list (total: " + list.Count + ")");
					}
					else
					{
						base.Logger.LogInfo("Fallback list already contains the CJK TMP font");
					}
				}
				WarmCjkAtlasFromCache();
			}
			else
			{
				base.Logger.LogWarning("No CJK TMP_FontAsset found");
			}
		}
		catch (Exception ex)
		{
			base.Logger.LogWarning("ConfigureTMPFallbackFont failed: " + ex.Message);
		}
	}

	private void WarmCjkAtlasFromCache()
	{
		if (!HasUsableTmpFont() || _chineseTMPFont == null)
		{
			return;
		}
		try
		{
			HashSet<char> unique = new HashSet<char>();
			lock (_cache)
			{
				int warmupEntryCount = 0;
				foreach (string localCacheKey in _localCacheKeys)
				{
					if (warmupEntryCount >= MaxFontWarmupCacheEntries)
					{
						break;
					}
					if (_cache.TryGetValue(localCacheKey, out var value))
					{
						Collect(value);
						warmupEntryCount++;
					}
				}
				if (_glossary != null)
				{
					foreach (string value2 in _glossary.Values)
					{
						Collect(value2);
					}
				}
			}
			if (unique.Count == 0)
			{
				base.Logger.LogInfo("[FONT-WARMUP] No CJK characters in cache to preheat");
				return;
			}
			char[] array = new char[unique.Count];
			unique.CopyTo(array);
			int num = 0;
			int num2 = 0;
			for (int i = 0; i < array.Length; i += 200)
			{
				int num3 = Math.Min(200, array.Length - i);
				string text = new string(array, i, num3);
				if (TryAddCharactersToTmpFontAsset(_chineseTMPFont, text))
				{
					num2 += num3;
				}
				num++;
			}
			base.Logger.LogInfo($"[FONT-WARMUP] Preheated {num2}/{unique.Count} CJK characters from local cache ({num} batches)");
			void Collect(string s)
			{
				if (!string.IsNullOrEmpty(s))
				{
					foreach (char c in s)
					{
						if (IsCjkCharForWarmup(c))
						{
							unique.Add(c);
						}
					}
				}
			}
		}
		catch (Exception ex)
		{
			base.Logger.LogWarning("[FONT-WARMUP] Atlas preheat failed: " + ex.Message);
		}
	}

	private static bool IsCjkCharForWarmup(char c)
	{
		if ((c < '一' || c > '鿿') && (c < '㐀' || c > '䶿') && (c < '\u3000' || c > '〿') && (c < '\uff00' || c > '\uffef'))
		{
			if (c >= '\u3040')
			{
				return c <= 'ヿ';
			}
			return false;
		}
		return true;
	}

	private void LoadGlossary()
	{
		/* glossary 是短文本优先映射，可来自内置默认值和游戏目录静态文件；
		   加载失败只降级为...部分词表，不影响本地服务和普通缓存...*/
		_glossary.Clear();
		foreach (KeyValuePair<string, string> item in new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase)
		{
			{ "Start", "开始" },
			{ "Continue", "继续" },
			{ "New Game", "新游戏" },
			{ "Load Game", "读取游戏" },
			{ "Save", "保存" },
			{ "Settings", "设置" },
			{ "Options", "选项" },
			{ "Leave", "离开" },
			{ "Quit", "退出" },
			{ "Inventory", "物品栏" },
			{ "Equipment", "装备" },
			{ "Skill", "技能" },
			{ "Status", "状态" },
			{ "Map", "地图" },
			{ "Menu", "菜单" },
			{ "Back", "返回" },
			{ "OK", "确定" },
			{ "Cancel", "取消" },
			{ "Yes", "是" },
			{ "No", "否" },
			{ "Close", "关闭" },
			{ "Confirm", "确认" },
			{ "Delete", "删除" },
			{ "Warning", "警告" },
			{ "Error", "错误" },
			{ "Help", "帮助" },
			{ "Screen", "屏幕" },
			{ "Info", "信息" },
			{ "Main Menu", "主菜单" },
			{ "Sound", "声音" },
			{ "Debug", "调试" },
			{ "Cheats", "作弊" },
			{ "Exit Game", "退出游戏" },
			{ "Collection", "收藏" },
			{ "Appearance", "外观" },
			{ "Human", "人类" },
			{ "High Elf", "高等精灵" },
			{ "Face Type", "脸型" },
			{ "Skin Color", "肤色" },
			{ "Hair Color", "发色" },
			{ "Eye Color", "眼睛颜色" },
			{ "Head Shape", "头型" },
			{ "Hair Style", "发型" },
			{ "Body Hair:", "体毛：" },
			{ "Beard:", "胡子：" },
			{ "Facial Hair", "面部毛发" },
			{ "Hairy", "毛发较多" },
			{ "Very Hairy", "毛发很多" },
			{ "Shaved", "剃净" },
			{ "Clean", "干净" },
			{ "Stubble", "胡茬" },
			{ "Dashing", "潇洒" },
			{ "Parted", "偏分" },
			{ "Dreadlocks", "脏辫" },
			{ "Cornrows", "玉米辫" },
			{ "Samson", "参孙发型" },
			{ "Peasant", "平民" },
			{ "Full", "浓密" },
			{ "Loose", "松散" },
			{ "Spiked", "刺猬头" },
			{ "Curly Top", "卷发顶部" },
			{ "Gladiator", "角斗士" },
			{ "Brown", "棕色" },
			{ "Black", "黑色" }
		})
		{
			_glossary[item.Key] = item.Value;
		}
		string path = Path.Combine(ResolveGameRoot(), "TranslationGlossary");
		if (Directory.Exists(path))
		{
			string[] files = Directory.GetFiles(path, "*.txt");
			foreach (string path2 in files)
			{
				LoadGlossaryFile(path2);
			}
			files = Directory.GetFiles(path, "*.csv");
			foreach (string path3 in files)
			{
				LoadGlossaryFile(path3);
			}
		}
		base.Logger.LogInfo($"Glossary loaded: {GetGlossaryCount()} entries");
	}

	private void LoadGlossaryFile(string path)
	{
		try
		{
			string[] array = File.ReadAllLines(path, Encoding.UTF8);
			for (int i = 0; i < array.Length; i++)
			{
				string text = array[i].Trim();
				if (string.IsNullOrEmpty(text) || text.StartsWith("#") || text.StartsWith("//"))
				{
					continue;
				}
				string[] array2 = ((!path.EndsWith(".csv", StringComparison.OrdinalIgnoreCase)) ? text.Split(new char[2] { '\t', '=' }, 2) : text.Split(new char[] { ',' }, 2, StringSplitOptions.None));
				if (array2.Length >= 2)
				{
					string text2 = array2[0].Trim();
					string text3 = SanitizeTranslationArtifacts(array2[1].Trim());
					if (IsAcceptableTranslation(text2, text3))
					{
						_glossary[text2] = text3;
					}
				}
			}
		}
		catch (Exception ex)
		{
			base.Logger.LogWarning("Failed to load glossary " + path + ": " + ex.Message);
		}
	}

	private void PrimeSmallLocalCacheForFirstFrame()
	{
		/* A small per-game cache is cheap enough to validate before hooks are installed.
		   This keeps known text out of the first rendered frame; large or polluted caches
		   retain the background path below so startup cannot stall on a full dump. */
		try
		{
			string path = GetLocalCacheFilePath();
			if (!File.Exists(path) || new FileInfo(path).Length > FirstFrameLocalCacheFileBytes)
			{
				return;
			}
			JObject val = JObject.Parse(File.ReadAllText(path));
			if (val.Count > FirstFrameLocalCacheEntryLimit)
			{
				return;
			}
			ImportServerCacheEntries(from prop in val.Properties()
				select new KeyValuePair<string, string>(prop.Name, ((object)prop.Value)?.ToString()), logPromotions: false, "first-frame local cache", persistAfterImport: false, markImportedAsLocal: true);
			_firstFrameLocalCachePrimed = true;
			base.Logger.LogInfo($"First-frame cache ready: {GetCacheCount()} entries");
		}
		catch (Exception reportedException)
		{
			ReportCaughtException(reportedException);
		}
	}

	private async Task BootCacheLoadAsync()
	{
		/* 大缓存可能被旧版本污染成数十万条；导入时每条都要跑正则校验，
		   因此仍然留在后台。小缓存已在安装 hook 前完成有界预装...*/
		try
		{
			if (!_firstFrameLocalCachePrimed)
			{
				await RunBackground(delegate
				{
					LoadServerCache();
					return true;
				});
			}
		}
		catch (Exception ex)
		{
			try
			{
				base.Logger.LogWarning("Boot cache load failed: " + ex.Message);
			}
			catch (Exception reportedException)
			{
				ReportCaughtException(reportedException);
			}
		}
		finally
		{
			/* Persistence must never replace an uninspected legacy cache before the
			   background loader has either imported or safely isolated it. */
			_bootCacheLoadComplete = true;
		}
		if (_shuttingDown)
		{
			return;
		}
		try
		{
			StartServerCacheSync();
		}
		catch (Exception ex2)
		{
			try
			{
				base.Logger.LogWarning("Server cache sync start failed: " + ex2.Message);
			}
			catch (Exception reportedException)
			{
				ReportCaughtException(reportedException);
			}
		}
	}

	private void LoadServerCache()
	{
		/* Hooks are already active while this background import runs. Merge the
		   disk snapshot so a faster live response cannot be erased by startup IO. */
		if (_shuttingDown)
		{
			return;
		}
		string text = Path.Combine(ResolveGameRoot(), "TranslationCache");
		if (!Directory.Exists(text))
		{
			return;
		}
		try
		{
			string path = Path.Combine(text, "unity_translation_cache.json");
			List<KeyValuePair<string, string>> diskEntries = null;
			/* Cache load/isolation and persistence replace the same file. Serializing all
			   file-system ownership here prevents a slow oversized-cache quarantine from
			   racing a live translation snapshot accepted after hooks started. */
			lock (_cacheFileWriteLock)
			{
				if (File.Exists(path))
				{
					if (new FileInfo(path).Length > OversizedLocalCacheFileBytes)
					{
						TryIsolateOversizedLocalCacheLocked(path, -1);
					}
					else
					{
						JObject val = JObject.Parse(File.ReadAllText(path));
						/* A legacy full-server dump can make every persist validate tens of
						   thousands of unrelated rows. Keep its exact bytes in a uniquely
						   versioned quarantine instead of truncating or overwriting it. */
						if (val.Count > OversizedLocalCacheEntryLimit)
						{
							TryIsolateOversizedLocalCacheLocked(path, val.Count);
						}
						else
						{
							diskEntries = (from prop in val.Properties()
								select new KeyValuePair<string, string>(prop.Name, ((object)prop.Value)?.ToString())).ToList();
						}
					}
				}
			}
			if (diskEntries != null)
			{
				ImportServerCacheEntries(diskEntries, logPromotions: false, "local cache file", persistAfterImport: false, markImportedAsLocal: true, preserveExisting: true);
			}
		}
		catch (Exception ex)
		{
			base.Logger.LogWarning("Cache load failed: " + ex.Message);
		}
		base.Logger.LogInfo($"Cache preloaded: {GetCacheCount()} entries");
	}

	private void TryIsolateOversizedLocalCacheLocked(string path, int entryCount)
	{
		/* External incompatibility: old plugin versions could persist a whole server
		   dump as this per-game cache, making startup and every rewrite unbounded.
		   The renderer cannot identify ownership row-by-row without paying that cost.
		   Isolation moves the exact user artifact to a unique sibling, never treats it
		   as a cache hit, never overwrites an older backup, and logs the retained path. */
		try
		{
			if (!File.Exists(path))
			{
				return;
			}
			string timestamp = DateTime.UtcNow.ToString("yyyyMMdd-HHmmssfff", System.Globalization.CultureInfo.InvariantCulture);
			string text = path + ".bak-oversized-" + timestamp;
			int collision = 1;
			while (File.Exists(text))
			{
				text = path + ".bak-oversized-" + timestamp + "-" + collision;
				collision++;
			}
			File.Move(path, text);
			string text2 = (entryCount >= 0) ? $"{entryCount} entries" : "more than " + OversizedLocalCacheFileBytes + " bytes";
			base.Logger.LogWarning($"Local cache file holds {text2} (legacy server-dump pollution); skipped import and isolated the exact artifact without overwriting prior backups. Backup: {Path.GetFileName(text)}");
		}
		catch (Exception ex)
		{
			ReportCaughtException(ex, "boundary=oversized-local-cache-isolation file=" + Path.GetFileName(path));
		}
	}

	private int ImportServerCacheEntries(IEnumerable<KeyValuePair<string, string>> entries, bool logPromotions, string sourceLabel, bool persistAfterImport, bool markImportedAsLocal = false, bool preserveExisting = false)
	{
		/* 只有可接受且非原文回声的结果才进..._cache。markImportedAsLocal 决定
		   是否进入本游戏持久化集合；服务端 dump/export 必须保持 memory-only...*/
		if (entries == null || _shuttingDown)
		{
			return 0;
		}
		int num = 0;
		int num2 = 0;
		int num3 = 0;
		foreach (KeyValuePair<string, string> entry in entries)
		{
			if (_shuttingDown)
			{
				break;
			}
			string key = entry.Key;
			string text = SanitizeTranslationArtifacts(entry.Value);
			if (!string.Equals(text, entry.Value, StringComparison.Ordinal))
			{
				num3++;
			}
			if (LooksLikeTypewriterFragment(key))
			{
				num3++;
				continue;
			}
			if (!IsAcceptableTranslation(key, text))
			{
				continue;
			}
			string text2 = NormalizeRequestText(key);
			bool flag = ShouldPromoteToGlossary(key);
			lock (_cache)
			{
				if (preserveExisting && (_cache.ContainsKey(key) || (!string.Equals(text2, key, StringComparison.Ordinal) && _cache.ContainsKey(text2))))
				{
					if (markImportedAsLocal)
					{
						MarkLocalCacheKeyLocked(key);
					}
					continue;
				}
				_cache[key] = text;
				if (!string.Equals(text2, key, StringComparison.Ordinal))
				{
					_cache[text2] = text;
				}
				if (flag)
				{
					_glossary[key] = text;
					num2++;
				}
				if (markImportedAsLocal)
				{
					MarkLocalCacheKeyLocked(key);
				}
			}
			ClearTranslationRetryState(key);
			num++;
		}
		if (num > 0)
		{
			ClearMixedRepairMemo();
			if (logPromotions)
			{
				base.Logger.LogInfo($"Loaded {num} cached translations from {sourceLabel} ({num2} promoted to glossary, {num3} cleaned)");
			}
			else
			{
				base.Logger.LogInfo($"Loaded {num} cached translations from {sourceLabel} ({num3} cleaned)");
			}
		}
		if (num > 0 && (persistAfterImport || (markImportedAsLocal && num3 > 0)))
		{
			ScheduleLocalCachePersist();
		}
		return num;
	}

	private void ScheduleLocalCachePersist()
	{
		/* dirty/scheduled 组成单消费者去抖协议。锁只保护两个标志；快照验证...		   File.WriteAllText 均在后台线程执行，避...Unity 主线程卡顿...*/
		bool flag = false;
		lock (_cachePersistLock)
		{
			if (_shuttingDown)
			{
				return;
			}
			_cachePersistDirty = true;
			if (!_cachePersistScheduled)
			{
				_cachePersistScheduled = true;
				flag = true;
			}
		}
		if (flag)
		{
			_ = PersistLocalCacheAsync();
		}
	}

	private async Task PersistLocalCacheAsync()
	{
		try
		{
			while (true)
			{
				await Task.Delay(CachePersistDebounceMs);
				if (_shuttingDown)
				{
					lock (_cachePersistLock)
					{
						_cachePersistDirty = false;
						_cachePersistScheduled = false;
					}
					break;
				}
				if (!_bootCacheLoadComplete)
				{
					/* The loader owns the pre-existing user artifact. Retain dirty state
					   and retry after another bounded debounce rather than overwriting it. */
					continue;
				}
				lock (_cachePersistLock)
				{
					if (!_cachePersistDirty)
					{
						_cachePersistScheduled = false;
						break;
					}
					_cachePersistDirty = false;
				}
				/* 快照过滤会对每条缓存运行较重的正则。单轮异常会保留 dirty
				   状态并重试，不能杀死整局持久化循环。整个快照与写盘放到后台线程，避...await
				   回到 Unity SynchronizationContext 后在主线程制造卡顿...*/
				bool persisted = false;
				try
				{
					persisted = await RunBackground(delegate
					{
						lock (_cacheFileWriteLock)
						{
							if (_shuttingDown)
							{
								return true;
							}
							Dictionary<string, string> dictionary = SnapshotLocalCacheForPersist();
							return dictionary.Count == 0 || WriteLocalCacheSnapshot(dictionary);
						}
					});
				}
				catch (Exception ex)
				{
					ReportCaughtException(ex, "boundary=local-cache-persist-cycle [CACHE] Persist cycle failed: " + ex.Message);
				}
				if (!persisted && !_shuttingDown)
				{
					/* File-system failures are external and cannot be repaired upstream.
					   Preserve dirty state so the bounded debounce loop retries; the old
					   atomic snapshot stays intact and the failure is recorded above. */
					lock (_cachePersistLock)
					{
						_cachePersistDirty = true;
					}
				}
				lock (_cachePersistLock)
				{
					if (!_cachePersistDirty)
					{
						_cachePersistScheduled = false;
						break;
					}
				}
			}
		}
		catch (Exception ex2)
		{
			/* 活性约束：持久化循环已死时 _cachePersistScheduled 不能继续...true...			   否则直到游戏退出都不会再启动保存任务...*/
			bool restart;
			lock (_cachePersistLock)
			{
				_cachePersistScheduled = false;
				restart = !_shuttingDown;
				if (restart)
				{
					_cachePersistDirty = true;
				}
			}
			ReportCaughtException(ex2, "boundary=local-cache-persist-loop");
			if (restart)
			{
				ScheduleLocalCachePersist();
			}
		}
	}

	private Dictionary<string, string> SnapshotLocalCacheForPersist()
	{
		/* 锁内只做键值浅拷贝；Sanitize/正则校验在锁外对副本执行，
		   避免全量正则在 _cache 锁内卡住主线程的 TryGetLocalTranslation。 */
		List<KeyValuePair<string, string>> pairs = new List<KeyValuePair<string, string>>();
		lock (_cache)
		{
			foreach (string localCacheKey in _localCacheKeys)
			{
				if (_cache.TryGetValue(localCacheKey, out var value))
				{
					pairs.Add(new KeyValuePair<string, string>(localCacheKey, value));
				}
			}
		}
		Dictionary<string, string> dictionary = new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase);
		foreach (KeyValuePair<string, string> pair in pairs)
		{
			string value = SanitizeTranslationArtifacts(pair.Value);
			if (!LooksLikeTypewriterFragment(pair.Key) && IsAcceptableTranslation(pair.Key, value))
			{
				dictionary[pair.Key] = value;
			}
		}
		return dictionary;
	}

	private bool WriteLocalCacheSnapshot(IReadOnlyDictionary<string, string> snapshot)
	{
		/* 原子写：先写临时文件再替换，避免写盘中断把整份本地缓存 JSON 报废；
		   失败走 ReportCaughtException 限速记录，不再仅 debug 模式可见。 */
		string tempPath = null;
		try
		{
			string localCacheFilePath = GetLocalCacheFilePath();
			string directoryName = Path.GetDirectoryName(localCacheFilePath);
			if (!string.IsNullOrWhiteSpace(directoryName))
			{
				Directory.CreateDirectory(directoryName);
			}
			tempPath = localCacheFilePath + ".tmp";
			File.WriteAllText(tempPath, SerializeStringMap(snapshot), Encoding.UTF8);
			if (File.Exists(localCacheFilePath))
			{
				File.Replace(tempPath, localCacheFilePath, null);
			}
			else
			{
				File.Move(tempPath, localCacheFilePath);
			}
			tempPath = null;
			return true;
		}
		catch (Exception ex)
		{
			ReportCaughtException(ex, "boundary=local-cache-persist");
			if (tempPath != null)
			{
				try
				{
					if (File.Exists(tempPath))
					{
						File.Delete(tempPath);
					}
				}
				catch (Exception reportedException)
				{
					ReportCaughtException(reportedException);
				}
			}
			return false;
		}
	}

	private void FlushLocalCacheToDisk()
	{
		if (!_bootCacheLoadComplete)
		{
			/* A very early process teardown can race the background cache inspection.
			   Keeping the pre-existing file is safer than replacing unknown user data;
			   no translation is reported as persisted, and the boundary is visible. */
			base.Logger.LogWarning("[CACHE] Final local-cache flush skipped because startup cache inspection had not completed; existing user cache was preserved.");
			return;
		}
		lock (_cachePersistLock)
		{
			_cachePersistDirty = false;
			_cachePersistScheduled = false;
		}
		lock (_cacheFileWriteLock)
		{
			Dictionary<string, string> dictionary = SnapshotLocalCacheForPersist();
			if (dictionary.Count > 0)
			{
				WriteLocalCacheSnapshot(dictionary);
			}
		}
	}

	private async Task<int> TryLoadServerCacheDumpAsync()
	{
		string serverUrl = _serverUrl.Value;
		string text = await RunBackground(() => HttpGet(serverUrl + "/cache/dump"));
		if (string.IsNullOrEmpty(text))
		{
			return 0;
		}
		JToken val = JObject.Parse(text)["cache"];
		if (val == null)
		{
			return 0;
		}
		return ImportServerCacheEntries(((IEnumerable<JProperty>)(object)val.Children<JProperty>()).Select((JProperty prop) => new KeyValuePair<string, string>(prop.Name, ((object)prop.Value)?.ToString())), logPromotions: false, "server cache dump", persistAfterImport: false);
	}

	private async Task<int> TryLoadServerCacheExportAsync()
	{
		string serverUrl = _serverUrl.Value;
		string text = await RunBackground(() => HttpPost(serverUrl + "/cache/export", "{}"));
		if (string.IsNullOrEmpty(text))
		{
			return 0;
		}
		JToken obj = JObject.Parse(text)["entries"];
		JArray val = (JArray)(object)((obj is JArray) ? obj : null);
		if (val == null)
		{
			return 0;
		}
		return ImportServerCacheEntries(((IEnumerable<JToken>)val).Select((JToken entry) => new KeyValuePair<string, string>((entry == null) ? null : ((object)entry[(object)"key"])?.ToString(), (entry == null) ? null : ((object)entry[(object)"value"])?.ToString())), logPromotions: true, "server export", persistAfterImport: false);
	}

	private async Task<int> TryGetServerCacheSizeAsync()
	{
		try
		{
			string serverUrl = _serverUrl.Value;
			string text = await RunBackground(() => HttpGet(serverUrl + "/health"));
			if (string.IsNullOrEmpty(text))
			{
				return 0;
			}
			JToken obj = JObject.Parse(text)["cache_size"];
			return (obj != null) ? Extensions.Value<int>((IEnumerable<JToken>)obj) : 0;
		}
		catch (Exception ex)
		{
			ReportCaughtException(ex);
			return 0;
		}
	}

	private async Task LoadServerCacheFromApiAsync()
	{
		Exception lastError = null;
		if (ShouldDelayServerCacheSync())
		{
			int num = await TryGetServerCacheSizeAsync();
			int localCacheCount = GetCacheCount();
			if (num > 0 && localCacheCount >= (int)Math.Ceiling((double)num * ServerCacheSkipRatio))
			{
				base.Logger.LogInfo($"Skipped server cache dump: local cache ({localCacheCount}) already covers server cache ({num})");
				return;
			}
		}
		for (int attempt = 0; attempt < 3; attempt++)
		{
			try
			{
				if (await TryLoadServerCacheDumpAsync() > 0 || await TryLoadServerCacheExportAsync() > 0)
				{
					return;
				}
			}
			catch (Exception ex)
			{
				ReportCaughtException(ex, "server-cache-load-attempt=" + (attempt + 1));
				lastError = ex;
			}
			await Task.Delay(600 * (attempt + 1));
		}
		if (lastError != null)
		{
			base.Logger.LogWarning("Server cache load failed: " + lastError.Message);
		}
	}

	private IEnumerator ScanTextComponentsCoroutine()
	{
		yield return (object)WaitForScaledSecondsSafe(GetSlowScanIntervalSeconds());
		LogVerbose("[SCAN] Backup scanner started");
		while (true)
		{
			yield return (object)WaitForScaledSecondsSafe(GetSlowScanIntervalSeconds());
			RunScannerTick((int)(GetSlowScanIntervalSeconds() * 1000f), "COROUTINE");
		}
	}

	private void ApplyTMProTranslation(object comp, int instId, string rawText, string translated, PropertyInfo textProp, PropertyInfo fontProp, MethodInfo forceMeshMethod, bool preserveRichText = true)
	{
		try
		{
			if (!IsUnityObjectAlive(comp) || !IsAcceptableTranslation(rawText, translated))
			{
				return;
			}
			Type type = comp.GetType();
			bool canTranslateTmp = CanTranslateTmp();
			if (!canTranslateTmp && !ShouldUseTmpOverlay())
			{
				return;
			}
			RescueStrandedAlpha(comp);
			translated = ((preserveRichText && ShouldPreserveRichTextForDisplayWithColor(rawText, translated)) ? PrepareTranslatedTextForComponent(comp, translated, rawText) : StripRichTextForPlainText(translated));
			translated = NormalizeTmpPunctuationForMissingGlyphs(translated);
			if (!canTranslateTmp)
			{
				if (!ApplyTmpOverlay(comp, translated, rawText))
				{
					return;
				}
				RevealTmpText(comp, translated);
				MarkProcessed(instId, rawText);
				TryMarkAppliedCacheKeyForPersist(rawText, translated);
				LogVerbose($"[TMPro-overlay] cached translated '{translated}' (id={instId})");
				return;
			}
			PrepareTmpTranslatedShortLabelLayout(comp, rawText, translated);
			bool tmpFontCoversText = EnsureTMPFontCoversText(comp, translated);
			if ((object)textProp == null)
			{
				textProp = AccessTools.Property(type, "text");
			}
			try
			{
				if (textProp != null)
				{
					textProp.SetValue(comp, translated);
					RevealTmpText(comp, translated);
				}
				else
				{
					/* text 属性不存在时直接尝试 m_text 字段；两者都不可写则早退，
					   且不能 MarkProcessed，否则未写入的组件会被误记为“已翻译”。 */
					FieldInfo textField = AccessTools.Field(type, "m_text");
					if (textField == null)
					{
						LogVerbose("[SCAN] No writable text member on " + type.FullName + "; source text left untranslated");
						return;
					}
					textField.SetValue(comp, translated);
					RevealTmpText(comp, translated);
				}
			}
			catch (Exception ex)
			{
				FieldInfo fieldInfo = AccessTools.Field(type, "m_text");
				if (fieldInfo == null)
				{
					throw;
				}
				fieldInfo.SetValue(comp, translated);
				RevealTmpText(comp, translated);
				ReportCaughtException(ex, "renderer=UnityMono boundary=tmp-text-property-fallback type=" + type.FullName);
			}
			ApplyTmpOverlay(comp, translated, rawText, !tmpFontCoversText);
			if (forceMeshMethod != null)
			{
				if (forceMeshMethod.GetParameters().Length == 2)
				{
					forceMeshMethod.Invoke(comp, new object[2] { false, true });
				}
				else if (forceMeshMethod.GetParameters().Length == 1)
				{
					forceMeshMethod.Invoke(comp, new object[1] { true });
				}
				else
				{
					forceMeshMethod.Invoke(comp, null);
				}
			}
			else
			{
				InvokeForceMeshUpdate(comp, type);
			}
			if (PostWriteHasMissingGlyph(comp, translated))
			{
				Interlocked.Increment(ref _glyphRetryCount);
				ApplyTmpOverlay(comp, translated, rawText, force: true);
			}
			MarkProcessed(instId, rawText);
			TryMarkAppliedCacheKeyForPersist(rawText, translated);
			LogVerbose($"[SCAN] '{rawText}' -> '{translated}' (id={instId})");
		}
		catch (Exception ex2)
		{
			base.Logger.LogWarning("TMPro apply failed (" + comp.GetType().FullName + ", '" + GetVisibleText(rawText) + "'): " + (ex2.InnerException?.Message ?? ex2.Message));
		}
	}

	private static bool ContainsCjk(string s)
	{
		if (string.IsNullOrEmpty(s))
		{
			return false;
		}
		foreach (char c in s)
		{
			if (c >= '一' && c <= '鿿')
			{
				return true;
			}
			if (c >= '\u3000' && c <= '〿')
			{
				return true;
			}
			if (c >= '\uff00' && c <= '\uffef')
			{
				return true;
			}
		}
		return false;
	}

	private static bool LooksLikeMixedTranslationResidue(string text)
	{
		string visibleText = GetVisibleText(text);
		if (string.IsNullOrWhiteSpace(visibleText) || !ContainsCjk(visibleText))
		{
			return false;
		}
		int latinWords = 0;
		int latinChars = 0;
		foreach (Match item in LatinWordRegex.Matches(visibleText))
		{
			string value = item.Value;
			if (AllowedLatinResidue.Contains(value) || value.All(char.IsUpper))
			{
				continue;
			}
			latinWords++;
			latinChars += value.Length;
		}
		return latinWords >= 2 || latinChars >= 10;
	}

	private static string GetMixedResidueLatinTail(string text)
	{
		string visibleText = GetVisibleText(text);
		if (string.IsNullOrWhiteSpace(visibleText))
		{
			return string.Empty;
		}
		bool seenCjk = false;
		for (int i = 0; i < visibleText.Length; i++)
		{
			char c = visibleText[i];
			if (ContainsCjk(c.ToString()))
			{
				seenCjk = true;
			}
			else if (seenCjk && ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')))
			{
				string tail = visibleText.Substring(i).Trim();
				if (!EndsWithSentenceBoundary(tail))
				{
					return string.Empty;
				}
				return NormalizeLooseLatinText(tail);
			}
		}
		return string.Empty;
	}

	private static string GetLeadingCjkSignature(string text)
	{
		string visibleText = GetVisibleText(text);
		if (string.IsNullOrWhiteSpace(visibleText))
		{
			return string.Empty;
		}
		StringBuilder stringBuilder = new StringBuilder();
		foreach (char c in visibleText)
		{
			if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z'))
			{
				break;
			}
			if (ContainsCjk(c.ToString()))
			{
				stringBuilder.Append(c);
			}
		}
		return stringBuilder.ToString();
	}

	private static string NormalizeLooseLatinText(string text)
	{
		if (string.IsNullOrWhiteSpace(text))
		{
			return string.Empty;
		}
		StringBuilder stringBuilder = new StringBuilder(text.Length);
		bool lastWasSpace = true;
		foreach (char c in text)
		{
			char c2 = c;
			if (c2 == '\u2019' || c2 == '\u2018')
			{
				c2 = '\'';
			}
			if ((c2 >= 'A' && c2 <= 'Z') || (c2 >= 'a' && c2 <= 'z') || (c2 >= '0' && c2 <= '9'))
			{
				stringBuilder.Append(char.ToLowerInvariant(c2));
				lastWasSpace = false;
			}
			else if (!lastWasSpace)
			{
				stringBuilder.Append(' ');
				lastWasSpace = true;
			}
		}
		return stringBuilder.ToString().Trim();
	}
}
