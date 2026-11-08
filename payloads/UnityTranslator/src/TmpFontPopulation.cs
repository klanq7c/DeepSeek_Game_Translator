using System;
using System.Reflection;

namespace DstUnity
{
    internal enum TmpFontPopulationAction { Covered, Populate, Unavailable }

    // 只处理 TMP 显示层；检查自有图集时禁止递归回退和隐式动态加字。
    internal static class TmpFontPopulation
    {
        internal static TmpFontPopulationAction Plan(object asset, string characters, Action<Exception, string> report)
        {
            bool covered;
            if (TryGetCoverage(asset, characters, report, out covered) && covered)
                return TmpFontPopulationAction.Covered;
            try
            {
                object mode;
                if (!TryRead(asset, "atlasPopulationMode", out mode)) TryRead(asset, "m_AtlasPopulationMode", out mode);
                string name = mode == null ? "" : mode.ToString();
                if (name == "Static") return TmpFontPopulationAction.Unavailable;
                if (name == "Dynamic")
                {
                    object source, path;
                    bool knownSource = TryRead(asset, "sourceFontFile", out source) || TryRead(asset, "m_SourceFontFile", out source);
                    if (knownSource && source == null)
                    {
                        if (!TryRead(asset, "m_SourceFontFilePath", out path)) TryRead(asset, "sourceFontFilePath", out path);
                        if (string.IsNullOrEmpty(path as string))
                        {
                            report(new InvalidOperationException("Dynamic TMP font has no sourceFontFile or source path; existing atlas and renderer fallback retained."), "tmp-font-population-source");
                            return TmpFontPopulationAction.Unavailable;
                        }
                    }
                }
                // DynamicOS 可以没有源 Font 引用；旧版/裁剪版缺少元数据时沿用原加字入口。
                return TmpFontPopulationAction.Populate;
            }
            catch (Exception error)
            {
                // 元数据来自游戏版本，不能由共享缓存修复。诊断后保留旧 API 路径，
                // 加字失败仍由调用方返回失败，不把未知覆盖率当作渲染成功。
                report(error, "tmp-font-population-metadata");
                return TmpFontPopulationAction.Populate;
            }
        }

        internal static bool TryGetCoverage(object asset, string characters, Action<Exception, string> report, out bool covered)
        {
            covered = false;
            try
            {
                Type type = asset.GetType();
                MethodInfo coverage = type.GetMethod("HasCharacters", new[] { typeof(string), typeof(string).MakeByRefType(), typeof(bool), typeof(bool) });
                if (coverage != null)
                {
                    object result = coverage.Invoke(asset, new object[] { characters, null, false, false });
                    if (!(result is bool present)) throw new InvalidOperationException("TMP HasCharacters did not return a coverage result.");
                    covered = present;
                    return true;
                }
                // 旧 TMP 的 int 版本只查自有表，并按完整 Unicode 码点读取。
                MethodInfo hasCharacter = type.GetMethod("HasCharacter", new[] { typeof(int) });
                if (hasCharacter == null) return false;
                for (int i = 0; i < characters.Length; i++)
                {
                    int codepoint = characters[i];
                    if (char.IsHighSurrogate(characters[i]) && i + 1 < characters.Length && char.IsLowSurrogate(characters[i + 1]))
                    {
                        codepoint = char.ConvertToUtf32(characters[i], characters[i + 1]);
                        i++;
                    }
                    object result = hasCharacter.Invoke(asset, new object[] { codepoint });
                    if (!(result is bool present)) throw new InvalidOperationException("TMP HasCharacter did not return a coverage result.");
                    if (!present) return true;
                }
                covered = true;
                return true;
            }
            catch (Exception error)
            {
                report(error, "tmp-font-population-coverage");
                return false;
            }
        }

        private static bool TryRead(object asset, string name, out object value)
        {
            for (Type type = asset.GetType(); type != null; type = type.BaseType)
            {
                const BindingFlags flags = BindingFlags.Instance | BindingFlags.Public | BindingFlags.NonPublic | BindingFlags.DeclaredOnly;
                PropertyInfo property = type.GetProperty(name, flags);
                if (property != null && property.CanRead) { value = property.GetValue(asset, null); return true; }
                FieldInfo field = type.GetField(name, flags);
                if (field != null) { value = field.GetValue(asset); return true; }
            }
            value = null;
            return false;
        }
    }
}
