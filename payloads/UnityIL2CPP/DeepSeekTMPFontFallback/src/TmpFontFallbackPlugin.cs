using System;
using System.Collections;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Reflection;
using System.Runtime.InteropServices;
using BepInEx;
using BepInEx.Logging;
using BepInEx.Unity.IL2CPP;
using HarmonyLib;
using Il2CppInterop.Runtime;
using Il2CppInterop.Runtime.InteropTypes.Arrays;
using UnityEngine;

namespace DeepSeekTMPFontFallback;

[BepInPlugin("com.deepseek.game-translator.tmp-font-fallback", "DeepSeek TMP Font Fallback", "1.2.8")]
public sealed class TmpFontFallbackPlugin : BasePlugin
{
    internal static ManualLogSource Logger;

    public override void Load()
    {
        Logger = Log;
        Log.LogInfo("DeepSeek TMP font fallback loaded.");
        AddComponent<TmpFontFallbackBehaviour>();
    }
}

public sealed class TmpFontFallbackBehaviour : MonoBehaviour
{
    // Poll fast during the first ~30s while the game is still wiring up TMP,
    // then keep a slow steady cadence forever so font assets and texts that
    // are loaded later (scene/addressable streaming) still get the Chinese
    // fallback. Stopping early left late content rendering as tofu boxes.
    private const float WarmupInterval = 1.0f;
    private const float SteadyInterval = 5.0f;
    private const float BackoffInterval = 30.0f;
    private const float FastNormalizeInterval = 0.05f;
    private const float SteadyNormalizeInterval = 0.5f;
    private const int WarmupTicks = 30;
    private const int BackoffAfterFailures = 5;

    private float _nextAttempt;
    private float _nextFastNormalize;
    private int _ticks;
    private int _consecutiveFailures;
    private bool _loggedFailure;

    public TmpFontFallbackBehaviour(IntPtr ptr) : base(ptr)
    {
    }

    private void Start()
    {
        TryPatch();
    }

    private void LateUpdate()
    {
        if (Time.unscaledTime < _nextFastNormalize)
        {
            return;
        }

        float interval = TmpFontFallbackInstaller.ShouldUseFastNormalizeScan
            ? FastNormalizeInterval
            : SteadyNormalizeInterval;
        _nextFastNormalize = Time.unscaledTime + interval;
        try
        {
            TmpFontFallbackInstaller.NormalizeLoadedTextsFast();
        }
        catch
        {
            // The slower Apply path logs persistent errors; this path is only a
            // frame-level safety net for text that bypasses the setter patch.
        }
    }

    private void Update()
    {
        if (Time.unscaledTime < _nextAttempt)
        {
            return;
        }

        float interval = _ticks < WarmupTicks ? WarmupInterval : SteadyInterval;
        if (_consecutiveFailures >= BackoffAfterFailures)
        {
            interval = BackoffInterval; // persistent errors: back off, but never give up
        }

        _nextAttempt = Time.unscaledTime + interval;
        _ticks++;
        TryPatch();
    }

    private void TryPatch()
    {
        try
        {
            TmpFontFallbackInstaller.Apply();
            _consecutiveFailures = 0;
        }
        catch (Exception ex)
        {
            _consecutiveFailures++;
            if (!_loggedFailure)
            {
                _loggedFailure = true;
                TmpFontFallbackPlugin.Logger.LogWarning("TMP font fallback patch error (will keep retrying): " + Unwrap(ex).Message);
            }
        }
    }

    private static Exception Unwrap(Exception ex)
    {
        return ex is TargetInvocationException && ex.InnerException != null ? ex.InnerException : ex;
    }
}

internal static class TmpFontFallbackInstaller
{
    private static readonly Dictionary<int, string> FontByMajor = new()
    {
        { 6000, "arialuni_sdf_u6000" },
        { 2022, "arialuni_sdf_u2022" },
        { 2021, "arialuni_sdf_u2021" },
        { 2019, "arialuni_sdf_u2019" },
        { 2018, "arialuni_sdf_u2018" },
        { 5, "arialuni_sdf-u55to2017" },
    };

    private static readonly string[] PreferredSystemFonts =
    {
        "Microsoft YaHei UI",
        "Microsoft YaHei",
        "SimHei",
        "SimSun",
        "DengXian",
        "Noto Sans CJK SC",
        "Arial Unicode MS",
    };

    private static readonly (string FileName, string FaceName)[] PreferredFontFiles =
    {
        ("msyh.ttc", "Microsoft YaHei"),
        ("msyhbd.ttc", "Microsoft YaHei"),
        ("simhei.ttf", "SimHei"),
        ("simsun.ttc", "SimSun"),
        ("Deng.ttf", "DengXian"),
        ("Alibaba-PuHuiTi-Regular.ttf", "Alibaba PuHuiTi"),
        ("STXIHEI.TTF", "STXihei"),
        ("STSONG.TTF", "STSong"),
    };

    private const string CjkWarmupText = "\u4e2d\u6587\u6c49\u5316\u7ffb\u8bd1\u6e38\u620f\u8bbe\u7f6e\u5f00\u59cb\u7ee7\u7eed\u8fd4\u56de\u786e\u8ba4\u53d6\u6d88\u4fdd\u5b58\u52a0\u8f7d\u83dc\u5355\u5bf9\u8bdd";

    private static readonly HashSet<int> PatchedFontAssets = new();
    private static readonly HashSet<int> DirtiedTexts = new();
    private static readonly HashSet<int> PatchedUguiTexts = new();
    private static readonly Dictionary<int, string> LastTmpTextById = new();
    private static readonly Dictionary<int, object> InteractiveOverlayTextBySourceId = new();
    private static readonly Dictionary<int, string> InteractiveOriginalTextBySourceId = new();
    private static readonly Dictionary<int, Color> InteractiveOriginalColorBySourceId = new();
    private static readonly HashSet<int> InteractiveOverlayTextIds = new();
    private static Type _tmpSettingsType;
    private static Type _tmpFontAssetType;
    private static Type _tmpTextType;
    private static Type _uguiTextType;
    private static object _fallbackAsset;
    private static object _uguiFont;
    private static AssetBundle _fallbackBundle;
    private static byte[] _fallbackManagedBundleBytes;
    private static Il2CppStructArray<byte> _fallbackBundleBytes;
    private static Il2CppSystem.IO.MemoryStream _fallbackBundleStream;
    private static bool _settingsPatched;
    private static bool _reportedNoTmp;
    private static bool _reportedNoUguiFont;
    private static bool _uguiFontLoadAttempted;
    private static bool _reportedFailure;
    private static bool _textSetterPatchInstalled;
    private static bool _textSetterPatchFailed;
    private static int _lastFontCount = -1;
    private static int _lastTextCount = -1;
    private static int _lastUguiTextCount = -1;
    private static Harmony _harmony;

    private const string InteractiveOverlayName = "DeepSeekTranslationOverlay";

    // Reflection lookups memoized so the perpetual Apply() poll doesn't
    // re-resolve the same members every tick. All access is on the Unity main
    // thread (MonoBehaviour.Update), so no locking is required.
    private static MethodInfo _findObjectsOfTypeAll;
    private static readonly Dictionary<Type, object> _il2CppTypeCache = new();
    private static readonly Dictionary<Type, MethodInfo> _instanceIdMethods = new();

    public static bool ShouldUseFastNormalizeScan => !_textSetterPatchInstalled;

    public static void Apply()
    {
        int uguiTextCount = PatchUguiTexts();
        if (uguiTextCount != _lastUguiTextCount)
        {
            _lastUguiTextCount = uguiTextCount;
            if (uguiTextCount > 0)
            {
                TmpFontFallbackPlugin.Logger.LogInfo("UGUI Chinese font active. Patched texts: " + uguiTextCount + ".");
            }
        }

        if (!ResolveTmpTypes())
        {
            if (!_reportedNoTmp)
            {
                _reportedNoTmp = true;
                TmpFontFallbackPlugin.Logger.LogInfo("TextMeshPro not detected; TMP font fallback is idle.");
            }
            return;
        }

        TryInstallTextSetterPatch();

        if (_fallbackAsset == null)
        {
            if (_reportedFailure)
            {
                return;
            }

            _fallbackAsset = LoadFallbackFontAsset();
            if (_fallbackAsset == null)
            {
                if (!_reportedFailure)
                {
                    _reportedFailure = true;
                    TmpFontFallbackPlugin.Logger.LogWarning("No TMP fallback font asset could be loaded.");
                }
                return;
            }
        }

        if (!_settingsPatched)
        {
            _settingsPatched = AddToListProperty(_tmpSettingsType, null, "fallbackFontAssets", _fallbackAsset);
        }

        int fontCount = PatchLoadedFontAssets();
        int textCount = RefreshLoadedTexts();
        if (fontCount != _lastFontCount || textCount != _lastTextCount)
        {
            _lastFontCount = fontCount;
            _lastTextCount = textCount;
            TmpFontFallbackPlugin.Logger.LogInfo("TMP Chinese fallback active. Patched font assets: " + fontCount + "; refreshed texts: " + textCount + ".");
        }
    }

    private static void TryInstallTextSetterPatch()
    {
        if (_textSetterPatchInstalled || _textSetterPatchFailed || _tmpTextType == null)
        {
            return;
        }

        try
        {
            _harmony ??= new Harmony("com.deepseek.game-translator.tmp-font-fallback.text");
            MethodInfo prefix = typeof(TmpFontFallbackInstaller).GetMethod(nameof(PrefixTmpTextString), BindingFlags.NonPublic | BindingFlags.Static);
            int patched = 0;

            MethodInfo setter = _tmpTextType.GetProperty("text")?.GetSetMethod();
            if (setter != null)
            {
                _harmony.Patch(setter, prefix: new HarmonyMethod(prefix));
                patched++;
            }

            foreach (MethodInfo method in _tmpTextType.GetMethods(BindingFlags.Instance | BindingFlags.Public))
            {
                if (method.Name != "SetText")
                {
                    continue;
                }

                ParameterInfo[] parameters = method.GetParameters();
                if (parameters.Length > 0 && parameters[0].ParameterType == typeof(string))
                {
                    _harmony.Patch(method, prefix: new HarmonyMethod(prefix));
                    patched++;
                }
            }

            if (patched > 0)
            {
                _textSetterPatchInstalled = true;
                TmpFontFallbackPlugin.Logger.LogInfo("TMP text setter punctuation guard installed. Patched methods: " + patched + ".");
            }
        }
        catch (Exception ex)
        {
            _textSetterPatchFailed = true;
            TmpFontFallbackPlugin.Logger.LogWarning("TMP text setter punctuation guard failed; falling back to fast scan: " + ex.Message);
        }
    }

    private static void PrefixTmpTextString(object __instance, ref string __0)
    {
        string normalized = NormalizeTmpTextForFallback(__0);
        if (TryProtectInteractiveTmpTranslation(__instance, normalized, out string preservedOriginal))
        {
            __0 = preservedOriginal;
            return;
        }

        RestoreInteractiveTmpTranslation(__instance, normalized);
        __0 = normalized;
    }

    private static bool ResolveTmpTypes()
    {
        _tmpSettingsType ??= FindType("TMPro.TMP_Settings");
        _tmpFontAssetType ??= FindType("TMPro.TMP_FontAsset");
        _tmpTextType ??= FindType("TMPro.TMP_Text");
        return _tmpSettingsType != null && _tmpFontAssetType != null;
    }

    private static Type FindType(string fullName)
    {
        foreach (Assembly assembly in AppDomain.CurrentDomain.GetAssemblies())
        {
            Type type = assembly.GetType(fullName, false);
            if (type != null)
            {
                return type;
            }
        }

        return null;
    }

    private static object LoadFallbackFontAsset()
    {
        foreach (string assetName in SelectAssetCandidates())
        {
            string path = Path.Combine(Paths.GameRootPath, "BepInEx", "font", assetName);
            if (!File.Exists(path))
            {
                TmpFontFallbackPlugin.Logger.LogWarning("TMP fallback font asset is missing: " + path);
                continue;
            }

            try
            {
                _fallbackBundle = LoadAssetBundleFromFile(path);
                if (_fallbackBundle == null)
                {
                    string tempPath = CopyFontAssetToTempPath(path, assetName);
                    if (tempPath != null)
                    {
                        _fallbackBundle = LoadAssetBundleFromFile(tempPath);
                    }
                }
                if (_fallbackBundle == null)
                {
                    _fallbackBundle = LoadAssetBundleFromMemory(path);
                }
                if (_fallbackBundle == null)
                {
                    _fallbackBundle = LoadAssetBundleFromStream(path);
                }
                if (_fallbackBundle == null)
                {
                    TmpFontFallbackPlugin.Logger.LogWarning("AssetBundle load returned null for " + assetName);
                    continue;
                }

                object fontAsset = FindTmpFontAssetInBundle(_fallbackBundle, assetName);
                if (fontAsset != null)
                {
                    SetPropertyIfExists(fontAsset, "isMultiAtlasTexturesEnabled", true);
                    SetEnumPropertyIfExists(fontAsset, "atlasPopulationMode", "Dynamic");
                    TmpFontFallbackPlugin.Logger.LogInfo("Loaded TMP fallback font asset from bundle: " + assetName);
                    return fontAsset;
                }

                TmpFontFallbackPlugin.Logger.LogWarning("No TMP_FontAsset found in " + assetName);
            }
            catch (Exception ex)
            {
                TmpFontFallbackPlugin.Logger.LogWarning("Failed to load TMP fallback font asset " + assetName + ": " + Unwrap(ex).Message);
            }
        }

        return CreateSystemFallbackFontAsset();
    }

    private static object FindTmpFontAssetInBundle(AssetBundle bundle, string assetName)
    {
        foreach (object asset in Enumerate(InvokeAssetBundleMethod(bundle, "LoadAllAssets", null, _tmpFontAssetType)))
        {
            if (IsTmpFontAsset(asset))
            {
                return asset;
            }
        }

        foreach (string name in GetAssetNames(bundle, assetName))
        {
            foreach (object asset in Enumerate(InvokeAssetBundleMethod(bundle, "LoadAssetWithSubAssets", name, _tmpFontAssetType)))
            {
                if (IsTmpFontAsset(asset))
                {
                    return asset;
                }
            }

            object typedAsset = InvokeAssetBundleMethod(bundle, "LoadAsset", name, _tmpFontAssetType);
            if (IsTmpFontAsset(typedAsset))
            {
                return typedAsset;
            }

            object untypedAsset = InvokeAssetBundleMethod(bundle, "LoadAsset", name, null);
            if (IsTmpFontAsset(untypedAsset))
            {
                return untypedAsset;
            }
        }

        foreach (object asset in Enumerate(InvokeAssetBundleMethod(bundle, "LoadAllAssets", null, null)))
        {
            if (IsTmpFontAsset(asset))
            {
                return asset;
            }
        }

        return null;
    }

    private static IEnumerable<string> GetAssetNames(AssetBundle bundle, string fallbackName)
    {
        HashSet<string> seen = new(StringComparer.OrdinalIgnoreCase);
        foreach (MethodInfo method in typeof(AssetBundle).GetMethods(BindingFlags.Instance | BindingFlags.Public | BindingFlags.NonPublic)
            .Where(m => m.Name == "GetAllAssetNames" && m.GetParameters().Length == 0))
        {
            object names = null;
            try
            {
                names = method.Invoke(bundle, null);
            }
            catch
            {
            }

            foreach (object name in Enumerate(names))
            {
                string text = name?.ToString();
                if (!string.IsNullOrEmpty(text) && seen.Add(text))
                {
                    yield return text;
                }
            }
        }

        foreach (string candidate in new[] { fallbackName, Path.GetFileNameWithoutExtension(fallbackName), "Arial Unicode SDF", "ArialUnicodeSDF" })
        {
            if (!string.IsNullOrEmpty(candidate) && seen.Add(candidate))
            {
                yield return candidate;
            }
        }
    }

    private static object InvokeAssetBundleMethod(AssetBundle bundle, string methodName, string assetName, Type assetType)
    {
        foreach (MethodInfo method in typeof(AssetBundle).GetMethods(BindingFlags.Instance | BindingFlags.Public | BindingFlags.NonPublic)
            .Where(m => m.Name == methodName))
        {
            MethodInfo callable = method;
            bool typeSuppliedByGeneric = false;
            if (callable.ContainsGenericParameters)
            {
                if (assetType == null || callable.GetGenericArguments().Length != 1)
                {
                    continue;
                }

                try
                {
                    callable = callable.MakeGenericMethod(assetType);
                    typeSuppliedByGeneric = true;
                }
                catch
                {
                    continue;
                }
            }

            object[] args = BuildAssetBundleAssetArgs(callable.GetParameters(), assetName, assetType, typeSuppliedByGeneric);
            if (args == null)
            {
                continue;
            }

            try
            {
                return callable.Invoke(bundle, args);
            }
            catch
            {
            }
        }

        return null;
    }

    private static object[] BuildAssetBundleAssetArgs(ParameterInfo[] parameters, string assetName, Type assetType, bool typeSuppliedByGeneric)
    {
        object[] args = new object[parameters.Length];
        bool hasName = false;
        bool hasType = false;
        for (int i = 0; i < parameters.Length; i++)
        {
            Type t = parameters[i].ParameterType;
            if (assetName != null && t == typeof(string))
            {
                args[i] = assetName;
                hasName = true;
            }
            else if (assetType != null && t == typeof(Type))
            {
                args[i] = assetType;
                hasType = true;
            }
            else if (assetType != null && t.FullName == "Il2CppSystem.Type")
            {
                args[i] = ResolveIl2CppType(assetType);
                hasType = true;
            }
            else
            {
                return null;
            }
        }

        if (assetName != null && !hasName)
        {
            return null;
        }
        if (assetType != null && !hasType && !typeSuppliedByGeneric)
        {
            return null;
        }

        return args;
    }

    private static bool IsTmpFontAsset(object asset)
    {
        return asset != null && (_tmpFontAssetType.IsInstanceOfType(asset) || asset.GetType().FullName == _tmpFontAssetType.FullName);
    }

    private static object CreateSystemFallbackFontAsset()
    {
        object fileFontAsset = CreateSystemFontFileAsset();
        if (fileFontAsset != null)
        {
            return fileFontAsset;
        }

        Type fontType = FindType("UnityEngine.Font");
        if (fontType == null)
        {
            return null;
        }

        MethodInfo createFont = fontType.GetMethods(BindingFlags.Static | BindingFlags.Public)
            .FirstOrDefault(m =>
            {
                ParameterInfo[] parameters = m.GetParameters();
                return m.Name == "CreateDynamicFontFromOSFont"
                    && parameters.Length == 2
                    && parameters[0].ParameterType == typeof(string)
                    && parameters[1].ParameterType == typeof(int);
            });
        MethodInfo createTmpFont = _tmpFontAssetType.GetMethods(BindingFlags.Static | BindingFlags.Public)
            .FirstOrDefault(m =>
            {
                ParameterInfo[] parameters = m.GetParameters();
                return m.Name == "CreateFontAsset"
                    && parameters.Length == 1
                    && parameters[0].ParameterType.FullName == "UnityEngine.Font";
            });
        if (createFont == null || createTmpFont == null)
        {
            return null;
        }

        foreach (string fontName in PreferredSystemFonts)
        {
            try
            {
                object font = createFont.Invoke(null, new object[] { fontName, 90 });
                if (font == null)
                {
                    continue;
                }

                object fontAsset = createTmpFont.Invoke(null, new[] { font });
                if (fontAsset == null)
                {
                    continue;
                }

                SetPropertyIfExists(fontAsset, "isMultiAtlasTexturesEnabled", true);
                SetEnumPropertyIfExists(fontAsset, "atlasPopulationMode", "Dynamic");
                InvokeNoThrow(fontAsset, "TryAddCharacters", CjkWarmupText, true);
                TmpFontFallbackPlugin.Logger.LogInfo("Created TMP fallback font asset from system font: " + fontName);
                return fontAsset;
            }
            catch (Exception ex)
            {
                TmpFontFallbackPlugin.Logger.LogInfo("System font fallback skipped for " + fontName + ": " + Unwrap(ex).Message);
            }
        }

        return null;
    }

    private static object CreateSystemFontFileAsset()
    {
        foreach (string fontDir in GetWindowsFontDirectories())
        {
            foreach ((string fileName, string faceName) in PreferredFontFiles)
            {
                string path = Path.Combine(fontDir, fileName);
                if (!File.Exists(path))
                {
                    continue;
                }

                try
                {
                    object fontAssetFromUnityFont = CreateTmpFontAssetFromUnityFontFile(path, faceName);
                    if (fontAssetFromUnityFont != null)
                    {
                        SetPropertyIfExists(fontAssetFromUnityFont, "isMultiAtlasTexturesEnabled", true);
                        SetEnumPropertyIfExists(fontAssetFromUnityFont, "atlasPopulationMode", "Dynamic");
                        TryAddCharactersToTmpFontAsset(fontAssetFromUnityFont, CjkWarmupText);
                        TmpFontFallbackPlugin.Logger.LogInfo("Created TMP fallback font asset from Unity Font file: " + path);
                        return fontAssetFromUnityFont;
                    }

                    object fontAsset = CreateTmpFontAssetFromPath(path);
                    if (fontAsset == null)
                    {
                        TmpFontFallbackPlugin.Logger.LogInfo("Font file fallback produced no TMP asset for " + path);
                        continue;
                    }

                    SetPropertyIfExists(fontAsset, "isMultiAtlasTexturesEnabled", true);
                    SetEnumPropertyIfExists(fontAsset, "atlasPopulationMode", "Dynamic");
                    TryAddCharactersToTmpFontAsset(fontAsset, CjkWarmupText);
                    TmpFontFallbackPlugin.Logger.LogInfo("Created TMP fallback font asset from system font file: " + path);
                    return fontAsset;
                }
                catch (Exception ex)
                {
                    TmpFontFallbackPlugin.Logger.LogInfo("Font file fallback skipped for " + faceName + ": " + Unwrap(ex).Message);
                }
            }
        }

        return null;
    }

    private static object CreateTmpFontAssetFromUnityFontFile(string fontPath, string faceName)
    {
        Type fontType = FindType("UnityEngine.Font");
        if (fontType == null)
        {
            return null;
        }

        ConstructorInfo ctor = fontType.GetConstructor(BindingFlags.Instance | BindingFlags.Public | BindingFlags.NonPublic, null, new[] { typeof(string) }, null);
        if (ctor == null)
        {
            return null;
        }

        foreach (string source in new[] { fontPath, faceName })
        {
            if (string.IsNullOrEmpty(source))
            {
                continue;
            }

            try
            {
                object font = ctor.Invoke(new object[] { source });
                if (font == null)
                {
                    continue;
                }

                SetPropertyIfExists(font, "name", Path.GetFileNameWithoutExtension(fontPath));
                object fontAsset = CreateTmpFontAssetFromUnityFont(font);
                if (fontAsset != null)
                {
                    return fontAsset;
                }
            }
            catch
            {
                TmpFontFallbackPlugin.Logger.LogInfo("Unity Font constructor fallback skipped for " + source + ".");
            }
        }

        return null;
    }

    private static object CreateTmpFontAssetFromUnityFont(object font)
    {
        MethodInfo createTmpFont = _tmpFontAssetType.GetMethods(BindingFlags.Static | BindingFlags.Public | BindingFlags.NonPublic)
            .FirstOrDefault(m =>
            {
                ParameterInfo[] parameters = m.GetParameters();
                return m.Name == "CreateFontAsset"
                    && parameters.Length == 1
                    && parameters[0].ParameterType.FullName == "UnityEngine.Font";
            });
        if (createTmpFont == null)
        {
            return null;
        }

        return createTmpFont.Invoke(null, new[] { font });
    }

    private static object CreateTmpFontAssetFromPath(string fontPath)
    {
        Type glyphRenderModeType = FindType("UnityEngine.TextCore.LowLevel.GlyphRenderMode");
        Type atlasPopulationModeType = FindType("TMPro.AtlasPopulationMode");
        object glyphRenderMode = GetPreferredEnumValue(glyphRenderModeType, "SDFAA", "SDFAA_HINTED", "SMOOTH_HINTED", "SMOOTH");
        object atlasPopulationMode = GetPreferredEnumValue(atlasPopulationModeType, "Dynamic", "DynamicOS");

        foreach (MethodInfo method in _tmpFontAssetType.GetMethods(BindingFlags.Static | BindingFlags.Public | BindingFlags.NonPublic)
            .Where(m => m.Name == "CreateFontAsset" && m.GetParameters().Length >= 5 && m.GetParameters().Any(p => p.ParameterType == typeof(string))))
        {
            object[] args = BuildCreateFontAssetArgs(method.GetParameters(), fontPath, glyphRenderModeType, glyphRenderMode, atlasPopulationModeType, atlasPopulationMode);
            if (args == null)
            {
                continue;
            }

            object asset = method.Invoke(null, args);
            if (asset != null)
            {
                return asset;
            }
            TmpFontFallbackPlugin.Logger.LogInfo("Path-based TMP CreateFontAsset returned null for " + fontPath + " using " + method);
        }

        return null;
    }

    private static object[] BuildCreateFontAssetArgs(ParameterInfo[] parameters, string fontPath, Type glyphRenderModeType, object glyphRenderMode, Type atlasPopulationModeType, object atlasPopulationMode)
    {
        object[] args = new object[parameters.Length];
        int intIndex = 0;
        bool hasPath = false;
        for (int i = 0; i < parameters.Length; i++)
        {
            Type t = parameters[i].ParameterType;
            if (t == typeof(string) && !hasPath)
            {
                args[i] = fontPath;
                hasPath = true;
            }
            else if (t == typeof(int))
            {
                args[i] = NextCreateFontAssetIntArg(parameters[i].Name, intIndex++);
            }
            else if (t == typeof(bool))
            {
                args[i] = true;
            }
            else if (glyphRenderModeType != null && t == glyphRenderModeType && glyphRenderMode != null)
            {
                args[i] = glyphRenderMode;
            }
            else if (atlasPopulationModeType != null && t == atlasPopulationModeType && atlasPopulationMode != null)
            {
                args[i] = atlasPopulationMode;
            }
            else
            {
                return null;
            }
        }

        return hasPath ? args : null;
    }

    private static int NextCreateFontAssetIntArg(string parameterName, int index)
    {
        string name = parameterName?.ToLowerInvariant() ?? string.Empty;
        if (name.Contains("face")) return 0;
        if (name.Contains("sampling") || name.Contains("point")) return 64;
        if (name.Contains("padding")) return 6;
        if (name.Contains("width") || name.Contains("height")) return 4096;

        int[] defaults = { 0, 64, 6, 4096, 4096 };
        return index < defaults.Length ? defaults[index] : 0;
    }

    private static object GetPreferredEnumValue(Type enumType, params string[] names)
    {
        if (enumType == null || !enumType.IsEnum)
        {
            return null;
        }

        foreach (string name in names)
        {
            try
            {
                if (Enum.IsDefined(enumType, name))
                {
                    return Enum.Parse(enumType, name);
                }
            }
            catch
            {
            }
        }

        Array values = Enum.GetValues(enumType);
        return values.Length > 0 ? values.GetValue(0) : null;
    }

    private static bool TryAddCharactersToTmpFontAsset(object fontAsset, string characters)
    {
        foreach (MethodInfo method in fontAsset.GetType().GetMethods(BindingFlags.Instance | BindingFlags.Public | BindingFlags.NonPublic)
            .Where(m => m.Name == "TryAddCharacters"))
        {
            object[] args = BuildTryAddCharactersArgs(method.GetParameters(), characters);
            if (args == null)
            {
                continue;
            }

            try
            {
                object result = method.Invoke(fontAsset, args);
                if (result is bool ok)
                {
                    if (ok)
                    {
                        return true;
                    }
                    continue;
                }

                return true;
            }
            catch
            {
            }
        }

        return false;
    }

    private static object[] BuildTryAddCharactersArgs(ParameterInfo[] parameters, string characters)
    {
        object[] args = new object[parameters.Length];
        bool hasCharacters = false;
        for (int i = 0; i < parameters.Length; i++)
        {
            Type t = parameters[i].ParameterType;
            if (t == typeof(string) && !hasCharacters)
            {
                args[i] = characters;
                hasCharacters = true;
            }
            else if (t == typeof(bool))
            {
                args[i] = true;
            }
            else if (t.IsByRef && t.GetElementType() == typeof(string))
            {
                args[i] = string.Empty;
            }
            else
            {
                return null;
            }
        }

        return hasCharacters ? args : null;
    }

    private static IEnumerable<string> GetWindowsFontDirectories()
    {
        string[] roots =
        {
            Environment.GetFolderPath(Environment.SpecialFolder.Windows),
            Environment.GetEnvironmentVariable("WINDIR"),
            Environment.GetEnvironmentVariable("SystemRoot"),
            @"C:\Windows",
        };

        foreach (string root in roots.Where(r => !string.IsNullOrEmpty(r)).Distinct(StringComparer.OrdinalIgnoreCase))
        {
            string fonts = Path.Combine(root, "Fonts");
            if (Directory.Exists(fonts))
            {
                yield return fonts;
            }
        }
    }

    private static void CopyToIl2CppArray(byte[] source, Il2CppStructArray<byte> destination)
    {
        IntPtr ptr = GetIl2CppArrayStartPointer(destination);
        if (ptr != IntPtr.Zero)
        {
            Marshal.Copy(source, 0, ptr, source.Length);
            return;
        }

        for (int i = 0; i < source.Length; i++)
        {
            destination[i] = source[i];
        }
    }

    private static IntPtr GetIl2CppArrayStartPointer(Il2CppStructArray<byte> array)
    {
        PropertyInfo arrayStartPointer = array.GetType().BaseType?.GetProperty(
            "ArrayStartPointer",
            BindingFlags.Instance | BindingFlags.Public | BindingFlags.NonPublic);
        if (arrayStartPointer == null)
        {
            return IntPtr.Zero;
        }

        return (IntPtr)arrayStartPointer.GetValue(array);
    }

    private static AssetBundle LoadAssetBundleFromStream(string path)
    {
        byte[] bytes = File.ReadAllBytes(path);
        _fallbackBundleBytes = new Il2CppStructArray<byte>(bytes.LongLength);
        CopyToIl2CppArray(bytes, _fallbackBundleBytes);
        _fallbackBundleStream = new Il2CppSystem.IO.MemoryStream(_fallbackBundleBytes, false);
        return AssetBundle.LoadFromStream(_fallbackBundleStream);
    }

    private static AssetBundle LoadAssetBundleFromFile(string path)
    {
        foreach (MethodInfo method in typeof(AssetBundle).GetMethods(BindingFlags.Static | BindingFlags.Public | BindingFlags.NonPublic)
            .Where(m => (m.Name == "LoadFromFile" || m.Name == "LoadFromFile_Internal") && m.ReturnType == typeof(AssetBundle)))
        {
            object[] args = BuildAssetBundleLoadArgs(method.GetParameters(), path, null);
            if (args == null)
            {
                continue;
            }

            try
            {
                return method.Invoke(null, args) as AssetBundle;
            }
            catch
            {
            }
        }

        return null;
    }

    private static AssetBundle LoadAssetBundleFromMemory(string path)
    {
        _fallbackManagedBundleBytes = File.ReadAllBytes(path);
        _fallbackBundleBytes = new Il2CppStructArray<byte>(_fallbackManagedBundleBytes.LongLength);
        CopyToIl2CppArray(_fallbackManagedBundleBytes, _fallbackBundleBytes);

        foreach (MethodInfo method in typeof(AssetBundle).GetMethods(BindingFlags.Static | BindingFlags.Public | BindingFlags.NonPublic)
            .Where(m => (m.Name == "LoadFromMemory" || m.Name == "LoadFromMemory_Internal") && m.ReturnType == typeof(AssetBundle)))
        {
            object[] args = BuildAssetBundleLoadArgs(method.GetParameters(), null, _fallbackBundleBytes);
            if (args == null)
            {
                continue;
            }

            try
            {
                return method.Invoke(null, args) as AssetBundle;
            }
            catch
            {
            }
        }

        return null;
    }

    private static object[] BuildAssetBundleLoadArgs(ParameterInfo[] parameters, string path, Il2CppStructArray<byte> bytes)
    {
        object[] args = new object[parameters.Length];
        bool hasPayload = false;
        for (int i = 0; i < parameters.Length; i++)
        {
            Type t = parameters[i].ParameterType;
            if (path != null && t == typeof(string))
            {
                args[i] = path;
                hasPayload = true;
            }
            else if (bytes != null && t.IsInstanceOfType(bytes))
            {
                args[i] = bytes;
                hasPayload = true;
            }
            else if (t == typeof(uint))
            {
                args[i] = 0u;
            }
            else if (t == typeof(ulong))
            {
                args[i] = 0UL;
            }
            else if (t == typeof(int))
            {
                args[i] = 0;
            }
            else if (t == typeof(long))
            {
                args[i] = 0L;
            }
            else
            {
                return null;
            }
        }

        return hasPayload ? args : null;
    }

    private static string CopyFontAssetToTempPath(string sourcePath, string assetName)
    {
        try
        {
            string dir = Path.Combine(Path.GetTempPath(), "DeepSeekTMPFontFallback");
            Directory.CreateDirectory(dir);
            string dest = Path.Combine(dir, assetName);
            File.Copy(sourcePath, dest, true);
            return dest;
        }
        catch
        {
            return null;
        }
    }

    private static string SelectAssetName()
    {
        int major = DetectUnityMajor();
        if (major >= 6000) return FontByMajor[6000];
        if (major >= 2022) return FontByMajor[2022];
        if (major >= 2021) return FontByMajor[2021];
        if (major >= 2019) return FontByMajor[2019];
        if (major >= 2018) return FontByMajor[2018];
        if (major > 0) return FontByMajor[5];
        return FontByMajor[2019];
    }

    private static IEnumerable<string> SelectAssetCandidates()
    {
        string selected = SelectAssetName();
        yield return selected;

        foreach (string assetName in FontByMajor.Values.Distinct())
        {
            if (assetName != selected)
            {
                yield return assetName;
            }
        }
    }

    private static int DetectUnityMajor()
    {
        foreach (string path in Directory.GetFiles(Paths.GameRootPath, "globalgamemanagers", SearchOption.AllDirectories))
        {
            try
            {
                byte[] bytes = File.ReadAllBytes(path);
                for (int i = 0; i + 6 < bytes.Length; i++)
                {
                    if (bytes[i] < '0' || bytes[i] > '9') continue;

                    int major = 0;
                    int j = i;
                    int digits = 0;
                    while (j < bytes.Length && bytes[j] >= '0' && bytes[j] <= '9' && digits < 4)
                    {
                        major = major * 10 + bytes[j] - '0';
                        j++;
                        digits++;
                    }
                    if (digits == 0 || j >= bytes.Length || bytes[j] != '.') continue;
                    j++;

                    int minorDigits = 0;
                    while (j < bytes.Length && bytes[j] >= '0' && bytes[j] <= '9' && minorDigits < 3)
                    {
                        j++;
                        minorDigits++;
                    }
                    if (minorDigits == 0 || j >= bytes.Length || bytes[j] != '.') continue;

                    if (major == 5 || (major >= 2017 && major <= 2023) || major >= 6000)
                    {
                        return major;
                    }
                }
            }
            catch
            {
            }
        }

        return 0;
    }

    private static int PatchUguiTexts()
    {
        _uguiTextType ??= FindType("UnityEngine.UI.Text");
        if (_uguiTextType == null)
        {
            return 0;
        }

        List<object> cjkTexts = null;
        foreach (object text in FindUnityObjects(_uguiTextType))
        {
            string value = GetStringProperty(text, "text");
            if (ContainsCjk(value))
            {
                cjkTexts ??= new List<object>();
                cjkTexts.Add(text);
            }
        }
        if (cjkTexts == null)
        {
            return PatchedUguiTexts.Count;
        }

        object font = EnsureUguiFont();
        if (font == null)
        {
            if (!_reportedNoUguiFont)
            {
                _reportedNoUguiFont = true;
                TmpFontFallbackPlugin.Logger.LogWarning("No UGUI Chinese font could be loaded.");
            }
            return PatchedUguiTexts.Count;
        }

        int fontId = InstanceId(font);
        foreach (object text in cjkTexts)
        {
            int id = InstanceId(text);
            object currentFont = GetPropertyValue(text, "font");
            if (currentFont != null && InstanceId(currentFont) == fontId)
            {
                PatchedUguiTexts.Add(id);
                continue;
            }

            if (TrySetPropertyIfExists(text, "font", font))
            {
                PatchedUguiTexts.Add(id);
                InvokeNoThrow(text, "SetAllDirty");
            }
        }

        return PatchedUguiTexts.Count;
    }

    private static object EnsureUguiFont()
    {
        if (_uguiFont != null)
        {
            return _uguiFont;
        }
        if (_uguiFontLoadAttempted)
        {
            return null;
        }

        _uguiFontLoadAttempted = true;

        foreach (string fontDir in GetWindowsFontDirectories())
        {
            foreach ((string fileName, string faceName) in PreferredFontFiles)
            {
                string path = Path.Combine(fontDir, fileName);
                if (!File.Exists(path))
                {
                    continue;
                }

                _uguiFont = CreateUnityFont(faceName) ?? CreateUnityFont(path);
                if (_uguiFont != null)
                {
                    TmpFontFallbackPlugin.Logger.LogInfo("Loaded UGUI Chinese font: " + faceName);
                    return _uguiFont;
                }
            }
        }

        foreach (string fontName in PreferredSystemFonts)
        {
            _uguiFont = CreateUnityFont(fontName);
            if (_uguiFont != null)
            {
                TmpFontFallbackPlugin.Logger.LogInfo("Loaded UGUI Chinese font: " + fontName);
                return _uguiFont;
            }
        }

        return null;
    }

    private static object CreateUnityFont(string nameOrPath)
    {
        if (string.IsNullOrEmpty(nameOrPath))
        {
            return null;
        }
        if (Path.IsPathRooted(nameOrPath))
        {
            return null;
        }

        try
        {
            return Font.CreateDynamicFontFromOSFont(nameOrPath, 90);
        }
        catch
        {
            return null;
        }
    }

    private static bool ContainsCjk(string text)
    {
        if (string.IsNullOrEmpty(text))
        {
            return false;
        }

        foreach (char ch in text)
        {
            if ((ch >= '\u3400' && ch <= '\u9fff') || (ch >= '\uf900' && ch <= '\ufaff'))
            {
                return true;
            }
        }

        return false;
    }

    private static int PatchLoadedFontAssets()
    {
        foreach (object fontAsset in FindUnityObjects(_tmpFontAssetType))
        {
            int id = InstanceId(fontAsset);
            if (id == InstanceId(_fallbackAsset) || PatchedFontAssets.Contains(id))
            {
                continue;
            }

            if (AddToListProperty(_tmpFontAssetType, fontAsset, "fallbackFontAssetTable", _fallbackAsset))
            {
                PatchedFontAssets.Add(id);
            }
        }

        return PatchedFontAssets.Count;
    }

    private static int RefreshLoadedTexts()
    {
        if (_tmpTextType == null)
        {
            return 0;
        }

        foreach (object text in FindUnityObjects(_tmpTextType))
        {
            int id = InstanceId(text);
            bool changed = false;
            string current = GetStringProperty(text, "text");
            if (!string.Equals(current, LastTmpTextById.TryGetValue(id, out string last) ? last : null, StringComparison.Ordinal))
            {
                LastTmpTextById[id] = current;
                string normalized = NormalizeTmpTextForFallback(current);
                if (!string.Equals(normalized, current, StringComparison.Ordinal) && TrySetPropertyIfExists(text, "text", normalized))
                {
                    LastTmpTextById[id] = normalized;
                    changed = true;
                }
            }

            if (!DirtiedTexts.Add(id) && !changed)
            {
                continue;
            }

            InvokeNoThrow(text, "SetAllDirty");
            InvokeNoThrow(text, "ForceMeshUpdate", false, false);
        }

        return DirtiedTexts.Count;
    }

    public static int NormalizeLoadedTextsFast()
    {
        if (_tmpTextType == null)
        {
            ResolveTmpTypes();
        }
        if (_tmpTextType == null)
        {
            return 0;
        }

        int changedCount = 0;
        foreach (object text in FindUnityObjects(_tmpTextType))
        {
            int id = InstanceId(text);
            string current = GetStringProperty(text, "text");
            if (string.Equals(current, LastTmpTextById.TryGetValue(id, out string last) ? last : null, StringComparison.Ordinal))
            {
                continue;
            }

            LastTmpTextById[id] = current;
            string normalized = NormalizeTmpTextForFallback(current);
            if (string.Equals(normalized, current, StringComparison.Ordinal) || !TrySetPropertyIfExists(text, "text", normalized))
            {
                continue;
            }

            LastTmpTextById[id] = normalized;
            changedCount++;
            InvokeNoThrow(text, "SetAllDirty");
            InvokeNoThrow(text, "ForceMeshUpdate", false, false);
        }

        return changedCount;
    }

    private static bool TryProtectInteractiveTmpTranslation(object text, string translated, out string preservedOriginal)
    {
        preservedOriginal = null;
        if (text == null || string.IsNullOrEmpty(translated) || !ContainsCjk(translated) || IsInteractiveOverlayText(text))
        {
            return false;
        }

        string current = GetStringProperty(text, "text");
        if (string.IsNullOrEmpty(current) || ContainsCjk(current) || string.Equals(current, translated, StringComparison.Ordinal))
        {
            return false;
        }

        if (!IsLikelyInteractiveChoiceText(text, current))
        {
            return false;
        }

        int sourceId = InstanceId(text);
        if (!TryUpdateInteractiveOverlay(text, translated))
        {
            return false;
        }

        InteractiveOriginalTextBySourceId[sourceId] = current;
        preservedOriginal = current;
        return true;
    }

    private static bool IsInteractiveOverlayText(object text)
    {
        if (text == null)
        {
            return false;
        }

        int id = InstanceId(text);
        if (InteractiveOverlayTextIds.Contains(id))
        {
            return true;
        }

        Component component = text as Component;
        return component != null && component.gameObject != null && component.gameObject.name.StartsWith(InteractiveOverlayName, StringComparison.Ordinal);
    }

    private static bool IsLikelyInteractiveChoiceText(object text, string original)
    {
        Component component = text as Component;
        if (component == null || component.gameObject == null || !LooksLikeChoiceCandidate(original))
        {
            return false;
        }

        string path = GetUnityObjectPath(component).ToLowerInvariant();
        if (IsLogicSensitiveUiPath(path))
        {
            return true;
        }

        if (IsChoiceLikeUiPath(path))
        {
            return true;
        }

        return HasInteractiveComponentInParents(component);
    }

    private static bool IsLogicSensitiveUiPath(string path)
    {
        return path.Contains("notification") ||
               path.Contains("tutorial") ||
               path.Contains("stat") ||
               path.Contains("toast") ||
               path.Contains("popup") ||
               path.Contains("modal") ||
               path.Contains("alert");
    }

    private static bool IsChoiceLikeUiPath(string path)
    {
        return path.Contains("choice") ||
               path.Contains("option") ||
               path.Contains("answer") ||
               path.Contains("response") ||
               path.Contains("selection");
    }

    private static bool LooksLikeChoiceCandidate(string text)
    {
        if (string.IsNullOrWhiteSpace(text) || text.Length > 180)
        {
            return false;
        }

        bool hasLetter = false;
        bool hasLower = false;
        bool hasSeparator = false;
        foreach (char ch in text)
        {
            if (char.IsLetter(ch))
            {
                hasLetter = true;
                if (char.IsLower(ch)) hasLower = true;
            }
            if (char.IsWhiteSpace(ch) || ch == '.' || ch == '?' || ch == '!' || ch == ',' || ch == '\'')
            {
                hasSeparator = true;
            }
        }

        if (!hasLetter || !hasSeparator)
        {
            return false;
        }

        if (!hasLower && text.Length <= 32)
        {
            return false;
        }

        return true;
    }

    private static bool HasInteractiveComponentInParents(Component component)
    {
        Transform t = component.transform;
        for (int depth = 0; t != null && depth < 8; depth++, t = t.parent)
        {
            try
            {
                foreach (Component c in t.gameObject.GetComponents<Component>())
                {
                    if (c == null) continue;
                    string name = c.GetType().Name;
                    string fullName = c.GetType().FullName ?? name;
                    if (name.Contains("Button") || name.Contains("Selectable") || name.Contains("Toggle") ||
                        name.Contains("Dropdown") || name.Contains("Choice") || fullName.Contains(".Button"))
                    {
                        return true;
                    }
                }
            }
            catch
            {
            }
        }

        return false;
    }

    private static string GetUnityObjectPath(Component component)
    {
        System.Text.StringBuilder path = new System.Text.StringBuilder(128);
        Transform t = component.transform;
        for (int depth = 0; t != null && depth < 12; depth++, t = t.parent)
        {
            if (path.Length > 0)
            {
                path.Insert(0, '/');
            }
            path.Insert(0, t.gameObject != null ? t.gameObject.name : t.name);
        }
        return path.ToString();
    }

    private static bool TryUpdateInteractiveOverlay(object sourceText, string translated)
    {
        Component source = sourceText as Component;
        if (source == null || source.gameObject == null)
        {
            return false;
        }

        int sourceId = InstanceId(sourceText);
        object overlayText = GetOrCreateInteractiveOverlay(sourceText, source, sourceId);
        if (overlayText == null)
        {
            return false;
        }

        string path = GetUnityObjectPath(source).ToLowerInvariant();
        bool compactChoiceLayout = IsChoiceLikeUiPath(path) || HasInteractiveComponentInParents(source);
        CopyTmpPresentation(sourceText, overlayText, sourceId);
        ConfigureInteractiveOverlayLayout(sourceText, overlayText, compactChoiceLayout);
        if (!TrySetPropertyIfExists(overlayText, "text", translated))
        {
            DestroyInteractiveOverlay(sourceId);
            return false;
        }

        TrySetPropertyIfExists(overlayText, "raycastTarget", false);
        InvokeNoThrow(overlayText, "SetAllDirty");
        InvokeNoThrow(overlayText, "ForceMeshUpdate", false, false);
        HideSourceTmpText(sourceText, sourceId);
        return true;
    }

    private static object GetOrCreateInteractiveOverlay(object sourceText, Component source, int sourceId)
    {
        if (InteractiveOverlayTextBySourceId.TryGetValue(sourceId, out object existing) && existing != null)
        {
            Component existingComponent = existing as Component;
            if (existingComponent != null && existingComponent.gameObject != null)
            {
                existingComponent.gameObject.SetActive(true);
                return existing;
            }
        }

        try
        {
            GameObject overlayObject = new GameObject(InteractiveOverlayName);
            overlayObject.layer = source.gameObject.layer;
            Transform parent = source.transform.parent;
            overlayObject.transform.SetParent(parent, false);
            overlayObject.transform.SetSiblingIndex(source.transform.GetSiblingIndex() + 1);

            Type concreteTextType = sourceText.GetType();
            Component overlayComponent = AddComponentByRuntimeType(overlayObject, concreteTextType);
            if (overlayComponent == null)
            {
                UnityEngine.Object.Destroy(overlayObject);
                return null;
            }

            CopyRectTransform(source.transform as RectTransform, overlayComponent.transform as RectTransform);
            InteractiveOverlayTextBySourceId[sourceId] = overlayComponent;
            InteractiveOverlayTextIds.Add(InstanceId(overlayComponent));
            return overlayComponent;
        }
        catch
        {
            return null;
        }
    }

    private static Component AddComponentByRuntimeType(GameObject target, Type componentType)
    {
        if (target == null || componentType == null)
        {
            return null;
        }

        object il2CppType = null;
        try
        {
            il2CppType = ResolveIl2CppType(componentType);
        }
        catch
        {
        }

        foreach (MethodInfo method in typeof(GameObject).GetMethods(BindingFlags.Instance | BindingFlags.Public)
            .Where(m => m.Name == "AddComponent" && !m.ContainsGenericParameters && m.GetParameters().Length == 1))
        {
            ParameterInfo parameter = method.GetParameters()[0];
            object arg = null;
            if (parameter.ParameterType == typeof(Type))
            {
                arg = componentType;
            }
            else if (parameter.ParameterType.FullName == "Il2CppSystem.Type")
            {
                arg = il2CppType;
            }

            if (arg == null)
            {
                continue;
            }

            try
            {
                return method.Invoke(target, new[] { arg }) as Component;
            }
            catch
            {
            }
        }

        return null;
    }

    private static void CopyRectTransform(RectTransform source, RectTransform target)
    {
        if (source == null || target == null)
        {
            return;
        }

        target.anchorMin = source.anchorMin;
        target.anchorMax = source.anchorMax;
        target.anchoredPosition = source.anchoredPosition;
        target.sizeDelta = source.sizeDelta;
        target.pivot = source.pivot;
        target.offsetMin = source.offsetMin;
        target.offsetMax = source.offsetMax;
        target.localScale = source.localScale;
        target.localRotation = source.localRotation;
    }

    private static void CopyTmpPresentation(object source, object overlay, int sourceId)
    {
        string[] properties =
        {
            "font", "fontSize", "fontStyle", "fontWeight", "enableAutoSizing",
            "fontSizeMin", "fontSizeMax", "alignment", "horizontalAlignment",
            "verticalAlignment", "enableWordWrapping", "overflowMode", "richText",
            "parseCtrlCharacters", "enableKerning", "characterSpacing", "wordSpacing",
            "lineSpacing", "paragraphSpacing", "margin", "material", "sharedMaterial",
            "spriteAsset", "styleSheet", "extraPadding", "enableVertexGradient",
            "colorGradient", "colorGradientPreset", "outlineWidth", "outlineColor",
            "faceColor", "fontSharedMaterial", "maskable"
        };

        foreach (string property in properties)
        {
            object value = GetPropertyValue(source, property);
            if (value != null)
            {
                TrySetPropertyIfExists(overlay, property, value);
            }
        }

        Color displayColor = Color.white;
        if (!InteractiveOriginalColorBySourceId.TryGetValue(sourceId, out displayColor))
        {
            TryGetColor(source, out displayColor);
        }
        if (displayColor.a <= 0.01f)
        {
            displayColor.a = 1.0f;
        }
        TrySetPropertyIfExists(overlay, "color", displayColor);
    }

    private static void ConfigureInteractiveOverlayLayout(object source, object overlay, bool compactChoiceLayout)
    {
        if (!compactChoiceLayout)
        {
            return;
        }

        TrySetPropertyIfExists(overlay, "enableWordWrapping", false);
        TrySetPropertyIfExists(overlay, "enableAutoSizing", true);
        SetEnumPropertyIfExists(overlay, "overflowMode", "Overflow");

        if (TryGetFloatProperty(source, "fontSize", out float fontSize) && fontSize > 0.1f)
        {
            TrySetPropertyIfExists(overlay, "fontSizeMax", fontSize);
            TrySetPropertyIfExists(overlay, "fontSizeMin", Math.Max(10.0f, fontSize * 0.55f));
            TrySetPropertyIfExists(overlay, "fontSize", Math.Max(10.0f, fontSize * 0.92f));
        }
    }

    private static void HideSourceTmpText(object sourceText, int sourceId)
    {
        if (!TryGetColor(sourceText, out Color color))
        {
            return;
        }

        if (!InteractiveOriginalColorBySourceId.ContainsKey(sourceId) && color.a > 0.01f)
        {
            InteractiveOriginalColorBySourceId[sourceId] = color;
        }

        color.a = 0.0f;
        TrySetPropertyIfExists(sourceText, "color", color);
    }

    private static void RestoreInteractiveTmpTranslation(object text, string incomingText)
    {
        if (text == null || IsInteractiveOverlayText(text))
        {
            return;
        }

        int sourceId = InstanceId(text);
        if (InteractiveOriginalTextBySourceId.TryGetValue(sourceId, out string originalText) &&
            string.Equals(originalText, incomingText, StringComparison.Ordinal) &&
            InteractiveOverlayTextBySourceId.TryGetValue(sourceId, out object existingOverlay))
        {
            Component overlayComponent = existingOverlay as Component;
            if (overlayComponent != null && overlayComponent.gameObject != null)
            {
                overlayComponent.gameObject.SetActive(true);
                HideSourceTmpText(text, sourceId);
                return;
            }
        }

        if (InteractiveOriginalColorBySourceId.TryGetValue(sourceId, out Color originalColor))
        {
            TrySetPropertyIfExists(text, "color", originalColor);
            InteractiveOriginalColorBySourceId.Remove(sourceId);
        }

        InteractiveOriginalTextBySourceId.Remove(sourceId);
        DestroyInteractiveOverlay(sourceId);
    }

    private static void DestroyInteractiveOverlay(int sourceId)
    {
        if (InteractiveOverlayTextBySourceId.TryGetValue(sourceId, out object overlay))
        {
            Component overlayComponent = overlay as Component;
            if (overlayComponent != null && overlayComponent.gameObject != null)
            {
                InteractiveOverlayTextIds.Remove(InstanceId(overlayComponent));
                UnityEngine.Object.Destroy(overlayComponent.gameObject);
            }
            InteractiveOverlayTextBySourceId.Remove(sourceId);
        }
    }

    private static bool TryGetColor(object target, out Color color)
    {
        color = Color.white;
        object value = GetPropertyValue(target, "color");
        if (value is Color c)
        {
            color = c;
            return true;
        }
        return false;
    }

    private static bool TryGetFloatProperty(object target, string propertyName, out float value)
    {
        value = 0.0f;
        object raw = GetPropertyValue(target, propertyName);
        if (raw == null)
        {
            return false;
        }

        try
        {
            value = Convert.ToSingle(raw);
            return true;
        }
        catch
        {
            return false;
        }
    }

    internal static string NormalizeTmpTextForFallback(string text)
    {
        if (string.IsNullOrEmpty(text))
        {
            return text;
        }

        bool containsCjk = ContainsCjk(text);
        if (!containsCjk && !ContainsAlwaysSafePunctuation(text))
        {
            return text;
        }

        string replacement;
        System.Text.StringBuilder builder = null;
        for (int i = 0; i < text.Length; i++)
        {
            char ch = text[i];
            replacement = GetFallbackSafeReplacement(ch, containsCjk);
            if (replacement == null)
            {
                if (builder != null)
                {
                    builder.Append(ch);
                }
                continue;
            }

            builder ??= new System.Text.StringBuilder(text.Length + 4).Append(text, 0, i);
            builder.Append(replacement);
        }

        return builder == null ? text : builder.ToString();
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

    private static object ResolveIl2CppType(Type targetType)
    {
        if (_il2CppTypeCache.TryGetValue(targetType, out object cached))
        {
            return cached;
        }
        object t = Il2CppType.From(targetType, false);
        _il2CppTypeCache[targetType] = t;
        return t;
    }

    private static IEnumerable<object> FindUnityObjects(Type targetType)
    {
        _findObjectsOfTypeAll ??= typeof(Resources).GetMethods(BindingFlags.Static | BindingFlags.Public)
            .FirstOrDefault(m => m.Name == "FindObjectsOfTypeAll" && m.GetParameters().Length == 1);
        MethodInfo method = _findObjectsOfTypeAll;
        if (method == null)
        {
            yield break;
        }

        object il2CppType;
        try
        {
            il2CppType = ResolveIl2CppType(targetType);
        }
        catch
        {
            yield break;
        }
        if (il2CppType == null)
        {
            yield break;
        }

        object array;
        try
        {
            array = method.Invoke(null, new[] { il2CppType });
        }
        catch
        {
            yield break;
        }

        foreach (object item in Enumerate(array))
        {
            if (item != null && targetType.IsInstanceOfType(item))
            {
                yield return item;
            }
        }
    }

    private static bool AddToListProperty(Type ownerType, object owner, string propertyName, object item)
    {
        PropertyInfo property = ownerType.GetProperty(propertyName, BindingFlags.Static | BindingFlags.Instance | BindingFlags.Public | BindingFlags.NonPublic);
        if (property == null)
        {
            return false;
        }

        object list = property.GetValue(owner);
        if (list == null)
        {
            list = Activator.CreateInstance(property.PropertyType);
            if (list == null)
            {
                return false;
            }

            property.SetValue(owner, list);
        }

        if (ListContainsUnityObject(list, item))
        {
            return true;
        }

        MethodInfo add = list.GetType().GetMethods(BindingFlags.Instance | BindingFlags.Public)
            .FirstOrDefault(m => m.Name == "Add" && m.GetParameters().Length == 1);
        if (add == null)
        {
            return false;
        }

        add.Invoke(list, new[] { item });
        return true;
    }

    private static bool ListContainsUnityObject(object list, object item)
    {
        int itemId = InstanceId(item);
        foreach (object existing in Enumerate(list))
        {
            if (InstanceId(existing) == itemId)
            {
                return true;
            }
        }

        return false;
    }

    private static IEnumerable<object> Enumerate(object collection)
    {
        if (collection == null)
        {
            yield break;
        }

        if (collection is IEnumerable enumerable)
        {
            foreach (object item in enumerable)
            {
                yield return item;
            }
            yield break;
        }

        PropertyInfo lengthProperty = collection.GetType().GetProperty("Length") ?? collection.GetType().GetProperty("Count");
        MethodInfo getItem = collection.GetType().GetMethod("get_Item", BindingFlags.Instance | BindingFlags.Public);
        if (lengthProperty == null || getItem == null)
        {
            yield break;
        }

        int length = Convert.ToInt32(lengthProperty.GetValue(collection));
        for (int i = 0; i < length; i++)
        {
            object item = getItem.Invoke(collection, new object[] { i });
            if (item != null)
            {
                yield return item;
            }
        }
    }

    private static int InstanceId(object unityObject)
    {
        Type t = unityObject.GetType();
        if (!_instanceIdMethods.TryGetValue(t, out MethodInfo method))
        {
            method = t.GetMethod("GetInstanceID", BindingFlags.Instance | BindingFlags.Public | BindingFlags.NonPublic);
            _instanceIdMethods[t] = method;
        }
        try
        {
            object value = method?.Invoke(unityObject, null);
            return value == null ? unityObject.GetHashCode() : Convert.ToInt32(value);
        }
        catch
        {
            return unityObject.GetHashCode();
        }
    }

    private static void SetPropertyIfExists(object target, string propertyName, object value)
    {
        TrySetPropertyIfExists(target, propertyName, value);
    }

    private static bool TrySetPropertyIfExists(object target, string propertyName, object value)
    {
        PropertyInfo property = target.GetType().GetProperty(propertyName, BindingFlags.Instance | BindingFlags.Public | BindingFlags.NonPublic);
        try
        {
            if (property == null)
            {
                return false;
            }

            property.SetValue(target, value);
            return true;
        }
        catch
        {
            return false;
        }
    }

    private static object GetPropertyValue(object target, string propertyName)
    {
        if (target == null)
        {
            return null;
        }

        try
        {
            PropertyInfo property = target.GetType().GetProperty(propertyName, BindingFlags.Instance | BindingFlags.Public | BindingFlags.NonPublic);
            return property?.GetValue(target);
        }
        catch
        {
            return null;
        }
    }

    private static string GetStringProperty(object target, string propertyName)
    {
        try
        {
            return GetPropertyValue(target, propertyName)?.ToString();
        }
        catch
        {
            return null;
        }
    }

    private static void SetEnumPropertyIfExists(object target, string propertyName, string enumName)
    {
        PropertyInfo property = target.GetType().GetProperty(propertyName, BindingFlags.Instance | BindingFlags.Public | BindingFlags.NonPublic);
        if (property == null || !property.PropertyType.IsEnum)
        {
            return;
        }

        try
        {
            property.SetValue(target, Enum.Parse(property.PropertyType, enumName));
        }
        catch
        {
        }
    }

    private static void InvokeNoThrow(object target, string methodName, params object[] args)
    {
        try
        {
            MethodInfo method = target.GetType().GetMethods(BindingFlags.Instance | BindingFlags.Public | BindingFlags.NonPublic)
                .FirstOrDefault(m => m.Name == methodName && ArgumentsMatch(m.GetParameters(), args));
            method?.Invoke(target, args);
        }
        catch
        {
        }
    }

    private static bool ArgumentsMatch(ParameterInfo[] parameters, object[] args)
    {
        if (parameters.Length != args.Length)
        {
            return false;
        }

        for (int i = 0; i < parameters.Length; i++)
        {
            object arg = args[i];
            if (arg == null)
            {
                continue;
            }

            Type parameterType = parameters[i].ParameterType;
            Type argumentType = arg.GetType();
            if (parameterType == argumentType || parameterType.IsAssignableFrom(argumentType))
            {
                continue;
            }

            return false;
        }

        return true;
    }

    private static Exception Unwrap(Exception ex)
    {
        return ex is TargetInvocationException && ex.InnerException != null ? ex.InnerException : ex;
    }
}
