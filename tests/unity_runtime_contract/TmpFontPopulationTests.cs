using System;

internal static class TmpFontPopulationTests
{
    private enum Mode { Static, Dynamic, DynamicOS }
    private class FontAsset
    {
        public Mode atlasPopulationMode { get; set; }
        public object sourceFontFile { get; set; }
        public string m_SourceFontFilePath = "";
        public bool Covered, AddSucceeds;
        public int Calls;
        public bool HasCharacters(string text, out string missing, bool searchFallbacks, bool tryAddCharacter)
        {
            if (searchFallbacks || tryAddCharacter) throw new InvalidOperationException("coverage must be local and read-only");
            missing = Covered ? "" : text;
            return Covered;
        }
        public bool TryAddCharacters(string text, out string missing, bool features)
        {
            Calls++; missing = AddSucceeds ? "" : text; if (AddSucceeds) Covered = true; return AddSucceeds;
        }
        public bool TryAddCharacters(string text, bool features) { Calls++; if (AddSucceeds) Covered = true; return AddSucceeds; }
    }
    private sealed class LegacyFont
    {
        public int Calls;
        public bool TryAddCharacters(string text, bool features) { Calls++; return true; }
    }
    private sealed class LegacyStaticFont
    {
        public Mode atlasPopulationMode { get { return Mode.Static; } }
        public bool HasCharacter(int codepoint) { return codepoint == 0x4e2d || codepoint == 0x1f600; }
        public bool TryAddCharacters(string text, bool features) { throw new InvalidOperationException("static atlas must stay read-only"); }
    }
    private sealed class InvalidResultFont
    {
        public string TryAddCharacters(string text, bool features) { return "no coverage result"; }
    }
    private sealed class BrokenMetadata : FontAsset
    {
        public new Mode atlasPopulationMode { get { throw new InvalidOperationException("native metadata getter failed"); } }
    }

    public static void Run(Action<string, bool> check)
    {
        foreach (bool il2cpp in new[] { false, true })
        {
            string prefix = il2cpp ? "IL2CPP" : "Mono";
            Func<object, string, bool> add = il2cpp ? Il2CppFontPopulationSeam.TryAddCharactersToTmpFontAsset : MonoFontPopulationSeam.TryAddCharactersToTmpFontAsset;
            var errors = il2cpp ? Il2CppFontPopulationSeam.Errors : MonoFontPopulationSeam.Errors;
            var font = new FontAsset { atlasPopulationMode = Mode.Static, Covered = true };
            bool covered = true;
            for (int i = 0; i < 60; i++) covered &= add(font, "已烘焙字形");
            check(prefix + " static atlas coverage stays usable across 60 redraws without native add", covered && font.Calls == 0 && font.atlasPopulationMode == Mode.Static);
            font.Covered = false;
            check(prefix + " static missing glyphs remain failure without native warning spam", !add(font, "缺") && font.Calls == 0);
            font = new FontAsset { atlasPopulationMode = Mode.Dynamic, Covered = true };
            check(prefix + " covered dynamic atlas needs no FontEngine work", add(font, "已") && font.Calls == 0);
            font.Covered = false;
            errors.Clear();
            bool rejected = !add(font, "缺");
            check(prefix + " dynamic font with no source or path skips native call and diagnoses", rejected && font.Calls == 0 && errors.Count > 0);
            font.sourceFontFile = new object(); font.AddSucceeds = true;
            check(prefix + " later source assignment recovers without permanent rejection cache", add(font, "新") && font.Calls == 1);
            font = new FontAsset { atlasPopulationMode = Mode.Dynamic, m_SourceFontFilePath = "runtime-font.otf", AddSucceeds = true };
            check(prefix + " path based dynamic source remains supported", add(font, "新") && font.Calls == 1);
            font = new FontAsset { atlasPopulationMode = Mode.DynamicOS, AddSucceeds = true };
            check(prefix + " DynamicOS may populate without a sourceFontFile reference", add(font, "新") && font.Calls == 1);
            font = new FontAsset { atlasPopulationMode = Mode.Dynamic, sourceFontFile = new object() };
            check(prefix + " native missing glyph result is not retried through equivalent overloads", !add(font, "缺") && font.Calls == 1);
            var legacy = new LegacyFont();
            check(prefix + " older font APIs retain existing add behavior", add(legacy, "新") && legacy.Calls == 1);
            check(prefix + " older static TMP API retains BMP and supplementary glyph coverage", add(new LegacyStaticFont(), "中😀"));
            errors.Clear();
            font = new BrokenMetadata { sourceFontFile = new object(), AddSucceeds = true };
            check(prefix + " metadata getter failure is diagnosed and preserves legacy invocation", add(font, "新") && font.Calls == 1 && errors.Count > 0);
        }
        var unverified = new FontAsset { atlasPopulationMode = Mode.Static };
        Il2CppFontPopulationSeam._fallbackAsset = unverified;
        check("IL2CPP initial warmup sample is not assumed to exist in every loaded atlas",
            !Il2CppFontPopulationSeam.TryWarmFallbackGlyphs("中", out _) && unverified.Calls == 0);
        Il2CppFontPopulationSeam.WarmedFallbackCharacters.Clear();
        unverified.Covered = true;
        check("IL2CPP verifies existing static glyph before remembering it",
            Il2CppFontPopulationSeam.TryWarmFallbackGlyphs("中", out _) && Il2CppFontPopulationSeam.WarmedFallbackCharacters.Contains('中'));
        unverified.Covered = false;
        check("IL2CPP atlas reset invalidates remembered glyph coverage",
            !Il2CppFontPopulationSeam.TryWarmFallbackGlyphs("中", out _) && !Il2CppFontPopulationSeam.WarmedFallbackCharacters.Contains('中'));
        var dynamic = new FontAsset { atlasPopulationMode = Mode.Dynamic, sourceFontFile = new object(), AddSucceeds = true };
        Il2CppFontPopulationSeam._fallbackAsset = dynamic;
        bool refreshed;
        bool firstWarm = Il2CppFontPopulationSeam.TryWarmFallbackGlyphs("字", out refreshed);
        check("IL2CPP dynamic glyph is verified and rendered after native add", firstWarm && refreshed && dynamic.Calls == 1);
        bool stable = true;
        for (int i = 0; i < 60; i++) stable &= Il2CppFontPopulationSeam.TryWarmFallbackGlyphs("字", out refreshed) && !refreshed;
        check("IL2CPP covered redraws do not repeatedly add glyphs or dirty meshes", stable && dynamic.Calls == 1);
        dynamic.Covered = false;
        check("IL2CPP atlas reset repopulates the same glyph and requests a mesh refresh",
            Il2CppFontPopulationSeam.TryWarmFallbackGlyphs("字", out refreshed) && refreshed && dynamic.Calls == 2);
        Il2CppFontPopulationSeam.WarmedFallbackCharacters.Clear();
        var punctuationFont = new FontAsset { atlasPopulationMode = Mode.Static, Covered = true };
        Il2CppFontPopulationSeam._fallbackAsset = punctuationFont;
        check("IL2CPP punctuation-only text verifies static glyph coverage",
            Il2CppFontPopulationSeam.TryWarmFallbackGlyphs("「」【】", out refreshed) && punctuationFont.Calls == 0
            && Il2CppFontPopulationSeam.WarmedFallbackCharacters.Contains('「'));
        punctuationFont.Covered = false;
        check("IL2CPP missing punctuation remains failure after atlas reset",
            !Il2CppFontPopulationSeam.TryWarmFallbackGlyphs("「」【】", out refreshed));
        check("Mono adjacent font eligibility already covers punctuation-only text", MonoFontPopulationSeam.ContainsCjk("「」【】"));
        check("IL2CPP Han-language predicate remains separate from font coverage", !Il2CppFontPopulationSeam.ContainsCjk("「」【】"));
        check("IL2CPP resource identifiers cannot trigger a font swap", !Il2CppFontPopulationSeam.NeedsCjkFont("<link=\"id>「中文」\">Continue</link>"));
        check("IL2CPP plain text retains visible markup-shaped punctuation", Il2CppFontPopulationSeam.NeedsCjkFont("<link=\"「」\">", false));
        check("IL2CPP noparse makes link-shaped Chinese visible", Il2CppFontPopulationSeam.NeedsCjkFont("<noparse><link=\"中文\"></noparse>"));
        check("IL2CPP unclosed noparse retains literal punctuation", Il2CppFontPopulationSeam.NeedsCjkFont("<noparse><link=\"「」\">"));
        check("IL2CPP unknown tags cannot hide visible Chinese", Il2CppFontPopulationSeam.NeedsCjkFont("<status=中文>"));
        check("IL2CPP invalid numeric or color tags retain visible Chinese", Il2CppFontPopulationSeam.NeedsCjkFont("<color=中文>"));
        punctuationFont.Covered = true;
        Il2CppFontPopulationSeam.WarmedFallbackCharacters.Clear();
        check("IL2CPP warms only visible punctuation and keeps link identifiers opaque",
            Il2CppFontPopulationSeam.TryWarmFallbackGlyphs("<link=\"id>中文\">「」</link>", out refreshed)
            && Il2CppFontPopulationSeam.WarmedFallbackCharacters.Contains('「') && !Il2CppFontPopulationSeam.WarmedFallbackCharacters.Contains('中'));
        Il2CppFontPopulationSeam.WarmedFallbackCharacters.Clear();
        check("IL2CPP warms noparse literals while the following real link remains opaque",
            Il2CppFontPopulationSeam.TryWarmFallbackGlyphs("<noparse><link=\"「」\"></noparse><link=\"秘密\">Continue</link>", out refreshed)
            && Il2CppFontPopulationSeam.WarmedFallbackCharacters.Contains('「') && !Il2CppFontPopulationSeam.WarmedFallbackCharacters.Contains('秘'));
        var legacyFallback = new LegacyFont();
        Il2CppFontPopulationSeam._fallbackAsset = legacyFallback;
        Il2CppFontPopulationSeam.WarmedFallbackCharacters.Clear();
        Il2CppFontPopulationSeam.Errors.Clear();
        bool legacyWarm = Il2CppFontPopulationSeam.TryWarmFallbackGlyphs("旧", out refreshed)
            && Il2CppFontPopulationSeam.TryWarmFallbackGlyphs("旧", out refreshed);
        check("IL2CPP absent coverage API retains bounded legacy native work with a diagnostic",
            legacyWarm && legacyFallback.Calls == 1 && Il2CppFontPopulationSeam.Errors.Count > 0);
        Il2CppFontPopulationSeam.Errors.Clear();
        check("IL2CPP unexpected native return type cannot fabricate successful coverage",
            !Il2CppFontPopulationSeam.TryAddCharactersToTmpFontAsset(new InvalidResultFont(), "缺") && Il2CppFontPopulationSeam.Errors.Count > 0);
    }
}
