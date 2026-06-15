using System;
using System.Collections.Generic;
using System.Net;
using System.Text;
using XUnity.AutoTranslator.Plugin.Core.Endpoints;
using XUnity.AutoTranslator.Plugin.Core.Endpoints.Http;
using XUnity.AutoTranslator.Plugin.Core.Web;

namespace DeepSeekTranslate;

public sealed class DeepSeekTranslateEndpoint : HttpEndpoint
{
    private string _baseUrl = "http://127.0.0.1:19999";
    private int _maxBatch = 16;
    private int _maxConcurrency = 8;
    private bool _displaySafePunctuation = true;

    public override string Id => "DeepSeekTranslate";
    public override string FriendlyName => "DeepSeek Local Batch";
    public override int MaxConcurrency => _maxConcurrency;
    public override int MaxTranslationsPerRequest => _maxBatch;

    public override void Initialize(IInitializationContext context)
    {
        _baseUrl = TrimSlash(context.GetOrCreateSetting("DeepSeek", "Url", _baseUrl));
        _maxBatch = Clamp(context.GetOrCreateSetting("DeepSeek", "MaxBatchSize", _maxBatch), 1, 32);
        _maxConcurrency = Clamp(context.GetOrCreateSetting("DeepSeek", "MaxConcurrency", _maxConcurrency), 1, 8);
        _displaySafePunctuation = context.GetOrCreateSetting("DeepSeek", "DisplaySafePunctuation", _displaySafePunctuation);
        context.SetTranslationDelay(ClampDelay(context.GetOrCreateSetting("DeepSeek", "TranslationDelay", 0.1f)));
        context.DisableSpamChecks();
    }

    public override void OnCreateRequest(IHttpRequestCreationContext context)
    {
        string[] texts = context.UntranslatedTexts;
        if (texts == null || texts.Length == 0)
        {
            texts = new[] { context.UntranslatedText ?? string.Empty };
        }

        bool batch = texts.Length > 1;
        string url = _baseUrl + (batch ? "/batch" : "/translate");
        string payload = BuildPayload(texts, !batch);
        var request = new XUnityWebRequest("POST", url, payload);
        request.Headers[HttpRequestHeader.Accept] = "application/json";
        context.Complete(request);
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

        if (original.Length <= 1)
        {
            string source = ReadStringProperty(data, "source");
            if (IsUnresolvedSource(source))
            {
                context.Fail("DeepSeek local server has no resolved translation yet.");
                return;
            }

            string one = ReadStringProperty(data, "translated_text") ?? ReadStringProperty(data, "translation");
            if (string.IsNullOrEmpty(one))
            {
                context.Fail("DeepSeek local server returned no translation.");
                return;
            }

            context.Complete(PrepareDisplayTranslation(one));
            return;
        }

        string[] results = ReadStringArrayProperty(data, "results");
        if (results == null || results.Length != original.Length)
        {
            results = ReadMapValues(data, original);
        }
        if (results == null || results.Length != original.Length)
        {
            context.Fail("DeepSeek local server returned an invalid batch response.");
            return;
        }

        string[] sources = ReadStringArrayProperty(data, "sources");
        if (HasUnresolvedSource(sources, original.Length))
        {
            context.Fail("DeepSeek local server has unresolved batch translations.");
            return;
        }

        context.Complete(PrepareDisplayTranslations(results));
    }

    private string[] PrepareDisplayTranslations(string[] values)
    {
        if (!_displaySafePunctuation || values == null) return values;
        for (int i = 0; i < values.Length; i++)
        {
            values[i] = NormalizeForTmpDisplay(values[i]);
        }
        return values;
    }

    private string PrepareDisplayTranslation(string value)
    {
        return _displaySafePunctuation ? NormalizeForTmpDisplay(value) : value;
    }

    private static string BuildPayload(string[] texts, bool single)
    {
        var sb = new StringBuilder(texts.Length * 64 + 32);
        if (single)
        {
            sb.Append("{\"text\":");
            AppendJsonString(sb, texts[0]);
            sb.Append('}');
        }
        else
        {
            sb.Append("{\"texts\":[");
            for (int i = 0; i < texts.Length; i++)
            {
                if (i != 0) sb.Append(',');
                AppendJsonString(sb, texts[i]);
            }
            sb.Append("]}");
        }
        return sb.ToString();
    }

    private static string TrimSlash(string value)
    {
        if (string.IsNullOrEmpty(value)) return "http://127.0.0.1:19999";
        return value.Trim().TrimEnd('/');
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

    private static string NormalizeForTmpDisplay(string text)
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

    private static bool IsUnresolvedSource(string source)
    {
        return string.Equals(source, "miss", StringComparison.OrdinalIgnoreCase)
            || string.Equals(source, "queued", StringComparison.OrdinalIgnoreCase);
    }

    private static bool HasUnresolvedSource(string[] sources, int expectedLength)
    {
        if (sources == null || sources.Length != expectedLength) return false;
        for (int i = 0; i < sources.Length; i++)
        {
            if (IsUnresolvedSource(sources[i])) return true;
        }
        return false;
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

    private static string[] ReadMapValues(string json, string[] keys)
    {
        int value = FindPropertyValue(json, "translations", 0);
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

    private static int FindPropertyValue(string json, string name, int start)
    {
        int i = Math.Max(0, start);
        while (i < json.Length)
        {
            SkipWs(json, ref i);
            if (i >= json.Length || json[i] != '"')
            {
                i++;
                continue;
            }

            int keyStart = i;
            string key = ReadJsonString(json, ref i);
            if (key == null)
            {
                i = keyStart + 1;
                continue;
            }
            SkipWs(json, ref i);
            if (i < json.Length && json[i] == ':' && key == name)
            {
                i++;
                SkipWs(json, ref i);
                return i;
            }
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
