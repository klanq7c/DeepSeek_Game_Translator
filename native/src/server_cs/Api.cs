using System;
using System.Collections.Concurrent;
using System.Collections.Generic;
using System.IO;
using System.Net;
using System.Net.Http;
using System.Runtime.InteropServices;
using System.Text;
using System.Threading;
using System.Threading.Tasks;
using DstCore;

namespace DstServerCs
{
    /*
     * ApiConfig / ApiClient —— 与 api.h/api.c 对齐的远程翻译客户端。
     *
     * api.ini 解析直接走 Win32 GetPrivateProfileStringA（net472 可 P/Invoke），
     * 与 C 版本逐字段一致，包含超时、并发及批次条数/输入 UTF-8 字节预算；
     * 回环端点使用较长超时和较小批次，所有数值按相同范围钳制。
     * 明文 HTTP 只允许回环端点；空 key 只允许回环端点。
     */

    public sealed class ApiConfig
    {
        public bool Enabled;
        public int TimeoutMs = 15000;
        public int Concurrency = ApiClient.ConcurrencyMax;
        public int BatchMaxItems = 48;
        public int BatchCharBudget = 9600;
        public string Endpoint = "https://api.deepseek.com/v1/chat/completions";
        public string Model = "deepseek-flash";
        public string Key = "";
        public string Glossary = "";

        [DllImport("kernel32.dll", CharSet = CharSet.Ansi, SetLastError = true)]
        private static extern int GetPrivateProfileStringA(
            string section, string key, string def, StringBuilder buffer,
            int size, string file);

        [DllImport("kernel32.dll", CharSet = CharSet.Ansi)]
        private static extern int GetPrivateProfileIntA(
            string section, string key, int def, string file);
        [DllImport("kernel32.dll", CharSet = CharSet.Unicode)]
        private static extern int GetPrivateProfileStringW(string section, string key, string def,
            StringBuilder buffer, int size, string file);

        private static string IniString(string file, string key, string def)
        {
            var sb = new StringBuilder(2048);
            GetPrivateProfileStringA("api", key, def, sb, sb.Capacity, file);
            return sb.ToString();
        }

        public void InitDefaults()
        {
            TimeoutMs = 15000;
            Concurrency = ApiClient.ConcurrencyMax;
            BatchMaxItems = 48;
            BatchCharBudget = 9600;
            Endpoint = "https://api.deepseek.com/v1/chat/completions";
            Model = "deepseek-flash";
            Key = "";
            Glossary = "";
            Enabled = false;
        }

        public void Load(string path)
        {
            InitDefaults();
            if (string.IsNullOrEmpty(path)) return;
            Endpoint = IniString(path, "endpoint", Endpoint);
            var model = new StringBuilder(257);
            GetPrivateProfileStringW("api", "model", Model, model, model.Capacity, path);
            Model = model.ToString();
            if (!ApiConfigText.ModelFits(Model)) {
                Model = "";
                Console.Error.WriteLine("[api] invalid Unicode model or UTF-8 model exceeds 255 bytes"); return;
            }
            Key = IniString(path, "key", "");
            // 与 api.c 一致：本地模型（回环端点）单批生成可能远慢于云 API，
            // 未显式配置 timeout_ms 时默认 180s、上限 300s；远程维持 15s/60s。
            bool loopback = ApiClient.IsLoopbackEndpoint(Endpoint);
            int defaultTimeout = loopback ? 180000 : 15000;
            int maxTimeout = loopback ? 300000 : 60000;
            if (IniString(path, "timeout_ms", "").Length > 0) {
                TimeoutMs = GetPrivateProfileIntA("api", "timeout_ms", defaultTimeout, path);
            } else {
                TimeoutMs = defaultTimeout;
            }
            if (TimeoutMs < 3000) TimeoutMs = 3000;
            if (TimeoutMs > maxTimeout) TimeoutMs = maxTimeout;
            Concurrency = GetPrivateProfileIntA("api", "concurrency", Concurrency, path);
            if (Concurrency < 1) Concurrency = 1;
            if (Concurrency > ApiClient.ConcurrencyMax) Concurrency = ApiClient.ConcurrencyMax;
            // 回环模型默认缩小单批；预算按原始输入的 UTF-8 字节累计，与 C 版一致。
            BatchMaxItems = GetPrivateProfileIntA("api", "batch_max_items", loopback ? 8 : 48, path);
            if (BatchMaxItems < 1) BatchMaxItems = 1;
            if (BatchMaxItems > 48) BatchMaxItems = 48;
            BatchCharBudget = GetPrivateProfileIntA("api", "batch_char_budget", loopback ? 2048 : 9600, path);
            if (BatchCharBudget < 256) BatchCharBudget = 256;
            if (BatchCharBudget > 9600) BatchCharBudget = 9600;
            MigrateDeprecatedDeepseekModel();
            Enabled = !string.IsNullOrEmpty(Endpoint) && !string.IsNullOrEmpty(Model)
                && (Key.Length > 0 || ApiClient.IsLoopbackEndpoint(Endpoint))
                && ApiClient.TransportAllowed(Endpoint);
        }

        /* api.c 的迁移逻辑：官方端点的退役别名 deepseek-chat 映射到现役模型，
           只改运行时值，绝不改写 api.ini。 */
        private void MigrateDeprecatedDeepseekModel()
        {
            if (!ApiClient.IsOfficialDeepseekEndpoint(Endpoint)) return;
            if (Model != "deepseek-chat") return;
            Model = "deepseek-flash";
            Console.Error.WriteLine(
                "[api] migrated retired official model deepseek-chat to " +
                "deepseek-flash for this process; api.ini was not rewritten");
        }
    }

    public static class ApiClient
    {
        public const int ConcurrencyMax = 8;
        private static readonly UTF8Encoding StrictResponseUtf8 = new UTF8Encoding(false, true);
        // 截止时间统一由每次请求的配置令牌控制，避免 HttpClient 默认 100 秒提前截断慢速本地模型。
        private static readonly HttpClient Http = new HttpClient(new HttpClientHandler
        {
            AutomaticDecompression = DecompressionMethods.None,
        }) { Timeout = Timeout.InfiniteTimeSpan };

        /* 前台计数：实时翻译在调用远程期间持有，后台预热批次在计数非零时让路。 */
        private static int foreground;
        public static void ForegroundEnter() { Interlocked.Increment(ref foreground); }
        public static void ForegroundLeave() { Interlocked.Decrement(ref foreground); }
        public static async Task WaitForForegroundAsync()
        {
            while (Interlocked.CompareExchange(ref foreground, 0, 0) > 0) {
                await Task.Delay(25).ConfigureAwait(false);
            }
        }

        private static bool EndpointHostBoundary(char c)
        {
            return c == 0 || c == ':' || c == '/' || c == '?' || c == '#';
        }

        public static bool IsLoopbackEndpoint(string endpoint)
        {
            if (string.IsNullOrEmpty(endpoint)) return false;
            int scheme = endpoint.IndexOf("://", StringComparison.Ordinal);
            string host = scheme >= 0 ? endpoint.Substring(scheme + 3) : endpoint;
            if (host.StartsWith("localhost", StringComparison.OrdinalIgnoreCase)
                && (host.Length == 9 || EndpointHostBoundary(host[9]))) return true;
            if (host.StartsWith("127.0.0.1", StringComparison.Ordinal)
                && (host.Length == 9 || EndpointHostBoundary(host[9]))) return true;
            return host.StartsWith("[::1]", StringComparison.Ordinal)
                && (host.Length == 5 || EndpointHostBoundary(host[5]));
        }

        public static bool TransportAllowed(string endpoint)
        {
            if (string.IsNullOrEmpty(endpoint)) return false;
            if (endpoint.Length >= 8 && endpoint.Substring(0, 8).Equals("https://", StringComparison.OrdinalIgnoreCase)) return true;
            if (endpoint.Length < 7 || !endpoint.Substring(0, 7).Equals("http://", StringComparison.OrdinalIgnoreCase)) return false;
            return IsLoopbackEndpoint(endpoint);
        }

        public static bool IsOfficialDeepseekEndpoint(string endpoint)
        {
            const string host = "api.deepseek.com";
            if (string.IsNullOrEmpty(endpoint)) return false;
            int scheme = endpoint.IndexOf("://", StringComparison.Ordinal);
            string rest = scheme >= 0 ? endpoint.Substring(scheme + 3) : endpoint;
            if (!rest.StartsWith(host, StringComparison.OrdinalIgnoreCase)) return false;
            return rest.Length == host.Length || ":/?".IndexOf(rest[host.Length]) >= 0;
        }

        /* 只认标准本机 Ollama 路由；不按模型名改变其他提供方的请求参数。 */
        public static bool IsLocalOllamaEndpoint(string endpoint)
        {
            const string path = "/v1/chat/completions";
            if (!IsLoopbackEndpoint(endpoint)) return false;
            if (!endpoint.StartsWith("http://", StringComparison.OrdinalIgnoreCase)
                && !endpoint.StartsWith("https://", StringComparison.OrdinalIgnoreCase)) return false;
            int scheme = endpoint.IndexOf("://", StringComparison.Ordinal);
            string host = endpoint.Substring(scheme + 3);
            string port = host.Substring(host[0] == '[' ? 5 : 9);
            if (!port.StartsWith(":11434", StringComparison.Ordinal)) return false;
            string route = port.Substring(6);
            if (!route.StartsWith(path, StringComparison.Ordinal)) return false;
            string tail = route.Substring(path.Length);
            if (tail.StartsWith("/", StringComparison.Ordinal)) tail = tail.Substring(1);
            return tail.Length == 0 || tail[0] == '?' || tail[0] == '#';
        }

        public static void Configure(ApiConfig cfg)
        {
            ModelName = cfg.Model;
            /* V4.1 Flash 的稳定 ID 与旧 V4 系列都保持翻译专用非思考模式。 */
            DisableThinking = IsOfficialDeepseekEndpoint(cfg.Endpoint)
                && (cfg.Model == "deepseek-flash" || cfg.Model.StartsWith("deepseek-v4-", StringComparison.Ordinal));
            DisableOllamaThinking = IsLocalOllamaEndpoint(cfg.Endpoint);
            ChatUrl = cfg.Endpoint;
            AuthHeader = string.IsNullOrEmpty(cfg.Key)
                ? null
                : "Bearer " + cfg.Key;
        }

        /* 单条 user 内容（与 build_request 逐字对齐，回显剥离依赖该前缀行）。 */
        private const string SingleUserLine =
            "Translate this exact game text to Simplified Chinese. Return only the translation.\n";

        private static string BuildBatchUser(IList<string> texts)
        {
            var sb = new StringBuilder(64);
            sb.Append('[');
            for (int i = 0; i < texts.Count; i++) {
                if (i > 0) sb.Append(',');
                sb.Append(Json.Escape(texts[i]));
            }
            sb.Append(']');
            return sb.ToString();
        }

        /* 语境段只列"批外断点"：prev 与批内前一条相同的条目跳过——那条文本
           本身就在待翻译数组里，重复发送只会让输入 token 翻倍。 */
        private static string BuildCtxUser(IList<string> texts, IList<string> originalTexts,
                                           IList<string> prevs)
        {
            var sb = new StringBuilder(128);
            sb.Append("Previous lines (context only, never translate or output them):\n");
            for (int i = 0; i < texts.Count; i++) {
                var p = prevs != null && i < prevs.Count ? prevs[i] : null;
                if (string.IsNullOrEmpty(p)) continue;
                // 不同汉字会映射为相同占位符；去重必须比较原文，再保护保留的前文。
                if (i > 0 && p == originalTexts[i - 1]) continue;
                sb.Append(i).Append(": ").Append(MixedText.Protect(p)).Append('\n');
            }
            sb.Append("\nTexts to translate:\n");
            sb.Append(BuildBatchUser(texts));
            return sb.ToString();
        }

        /* 三种请求模式共享的系统提示词前缀：稳定的开头让提供商的前缀缓存
           在单条/批量/上下文模式之间互相命中（缓存命中输入价约为全价 1/10）。 */
        private const string SystemPrefix =
            "You are localizing game text to Simplified Chinese. Preserve tags, placeholders, variables, numbers, and line breaks exactly. Already Chinese stays unchanged. Do not add explanations.";

        /* 拼接系统提示词：共享前缀 + 模式差异段 + 按命中过滤的术语表。 */
        private static string BuildSystemContent(string baseText, ApiConfig cfg, IList<string> texts)
        {
            if (cfg == null || string.IsNullOrEmpty(cfg.Glossary) ||
                texts == null || texts.Count == 0) {
                return baseText;
            }
            var all = string.Join("\n", texts);
            var hits = new StringBuilder();
            foreach (var rawLine in cfg.Glossary.Split('\n')) {
                var line = rawLine.TrimEnd('\r');
                // 行格式："- 术语 => 译文"
                if (line.Length <= 4 || !line.StartsWith("- ")) continue;
                int sep = line.IndexOf(" => ", StringComparison.Ordinal);
                if (sep < 2) continue;
                var term = line.Substring(2, sep - 2);
                if (term.Length > 0
                    && all.IndexOf(term, StringComparison.OrdinalIgnoreCase) >= 0) {
                    hits.Append('\n').Append(line.TrimEnd('\n'));
                }
            }
            if (hits.Length == 0) return baseText;
            return baseText + "\n\nMandatory glossary (use these exact translations):" + hits.ToString();
        }

        /* 输出上限：中文译文 token 数约为源文本的 2 倍以内，另加每行 JSON
           结构开销与常量余量。防止模型偶发的超长输出按跑飞量计费。 */
        private static int EstimateMaxTokens(IList<string> texts)
        {
            long chars = 0;
            foreach (var t in texts) chars += t != null ? t.Length : 0;
            double est = 96.0 + (double)chars + 4.0 * texts.Count;
            if (est < 512) est = 512;
            if (est > 8192) est = 8192;
            return (int)est;
        }

        private static string ChatUrl;
        private static string AuthHeader;
        private static string ModelName;
        private static bool DisableThinking;
        private static bool DisableOllamaThinking;

        private static string BuildMessages(string system, string user, int maxTokens)
        {
            var sb = new StringBuilder(256);
            sb.Append("{\"model\":").Append(Json.Escape(ModelName));
            sb.Append(",\"messages\":[{\"role\":\"system\",\"content\":")
              .Append(Json.Escape(system));
            sb.Append("},{\"role\":\"user\",\"content\":").Append(Json.Escape(user));
            sb.Append("}],\"temperature\":0,\"max_tokens\":").Append(maxTokens);
            if (DisableThinking) sb.Append(",\"thinking\":{\"type\":\"disabled\"}");
            if (DisableOllamaThinking) sb.Append(",\"reasoning_effort\":\"none\"");
            sb.Append('}');
            return sb.ToString();
        }

        private static readonly ConcurrentDictionary<string, long> DiagCounts =
            new ConcurrentDictionary<string, long>();

        private static void Diag(string stage, string detail)
        {
            // 与 C 服务端一样按失败阶段限频，避免其他错误掩盖首次响应超限。
            long n = DiagCounts.AddOrUpdate(stage, 1, (key, count) => count + 1);
            if (n <= 3 || (n & (n - 1)) == 0) {
                Console.Error.WriteLine("[api] " + stage + " failed #" + n
                    + (detail != null ? " (" + detail + ")" : ""));
            }
        }

        /* 失败分类（与 api.c 的 ApiFailClass 对齐）：
           - 可拆分：内容/形状类失败——模型漏/多数组元素、空内容、响应体超限、
             请求超时、HTTP 400/408/413/422。缩小批次有机会成功。
           - 终止：传输不可达、鉴权失败、限流、5xx。缩小批次只会把同一个错误
             重复 N 次，二分重试必须停止。
           async 方法不能有 out 参数，用引用类型承载。 */
        public sealed class FailureInfo
        {
            public bool Splittable = true;
        }

        private static bool HttpStatusSplittable(int status)
        {
            return status == 400 || status == 408 || status == 413 || status == 422;
        }

        /* send_chat_request：发请求 + 校验 2xx + 提取 content + 双重 normalize。 */
        private static async Task<string> SendChatAsync(
            ApiConfig cfg, string body, SemaphoreSlim gate, FailureInfo fail, string source = null)
        {
            // 外部配置可含非法端口；在请求构造边界拒绝并限频记录，不能让后台线程退出。
            // 配置文件可由用户直接编辑，因此发送方仍需校验；失败不拆分、不产生译文。
            Uri target;
            if (!Uri.TryCreate(ChatUrl, UriKind.Absolute, out target)
                || (target.Scheme != Uri.UriSchemeHttp && target.Scheme != Uri.UriSchemeHttps)) {
                Diag("request-uri", "invalid absolute HTTP endpoint");
                if (fail != null) fail.Splittable = false;
                return null;
            }
            await gate.WaitAsync().ConfigureAwait(false);
            try {
                using (var cts = new CancellationTokenSource(cfg.TimeoutMs)) {
                    using (var req = new HttpRequestMessage(HttpMethod.Post, target)) {
                        req.Content = new StringContent(body, Encoding.UTF8, "application/json");
                        if (AuthHeader != null) {
                            req.Headers.TryAddWithoutValidation("Authorization", AuthHeader);
                        }
                        HttpResponseMessage resp;
                        try {
                            resp = await Http.SendAsync(req,
                                HttpCompletionOption.ResponseHeadersRead, cts.Token).ConfigureAwait(false);
                        } catch (OperationCanceledException) {
                            // 超时：大批次在慢速本地模型上可能超时，缩小批次有意义。
                            Diag("send", "timeout");
                            if (fail != null) fail.Splittable = true;
                            return null;
                        } catch (HttpRequestException ex) {
                            // 连接/DNS/TLS 失败：端点不可达，拆分无意义。
                            Diag("send", ex.GetType().Name);
                            if (fail != null) fail.Splittable = false;
                            return null;
                        } catch (InvalidOperationException ex) {
                            // 端点 URL 非法等配置问题：拆分无意义。
                            Diag("send", ex.GetType().Name);
                            if (fail != null) fail.Splittable = false;
                            return null;
                        }
                        using (resp) {
                            int status = (int)resp.StatusCode;
                            if (status < 200 || status >= 300) {
                                Diag("http-status", "status=" + status);
                                if (fail != null) fail.Splittable = HttpStatusSplittable(status);
                                return null;
                            }
                            byte[] rawBytes;
                            try {
                                // .NET Framework 的响应流可能在 ReadAsync 中忽略取消标记。
                                // 截止时关闭实际响应连接，使只发头或半截正文的厂商不能永久占用通道。
                                // 下方按取消状态记录超时；关闭导致的 IO 异常不能伪装为有效译文。
                                using (cts.Token.Register(() => resp.Dispose())) {
                                    using (var stream = await resp.Content.ReadAsStreamAsync().ConfigureAwait(false)) {
                                        rawBytes = await ProviderResponseReader.ReadAsync(
                                            stream, resp.Content.Headers.ContentLength, cts.Token).ConfigureAwait(false);
                                    }
                                }
                            } catch (OperationCanceledException) {
                                Diag("read-body", "timeout");
                                if (fail != null) fail.Splittable = true;
                                return null;
                            } catch (HttpRequestException ex) {
                                Diag("read-body", ex.GetType().Name);
                                if (fail != null) fail.Splittable = false;
                                return null;
                            } catch (IOException ex) {
                                bool timedOut = cts.IsCancellationRequested;
                                Diag("read-body", timedOut ? "timeout" : ex.GetType().Name);
                                if (fail != null) fail.Splittable = timedOut;
                                return null;
                            } catch (ObjectDisposedException ex) {
                                bool timedOut = cts.IsCancellationRequested;
                                Diag("read-body", timedOut ? "timeout" : ex.GetType().Name);
                                if (fail != null) fail.Splittable = timedOut;
                                return null;
                            }
                            if (rawBytes == null || rawBytes.Length == 0) {
                                Diag(rawBytes == null ? "body-too-large" : "empty-body",
                                    rawBytes == null ? "limit=2097152" : null);
                                if (fail != null) fail.Splittable = true;
                                return null;
                            }
                            string raw;
                            try {
                                raw = StrictResponseUtf8.GetString(rawBytes);
                            } catch (DecoderFallbackException) {
                                // 提供商坏字节无法在上游修复；拒绝并记录类别，不用替换字符
                                // 拼出可入库的译文，也不记录响应正文或认证信息。
                                Diag("invalid-response", "invalid UTF-8");
                                if (fail != null) fail.Splittable = true;
                                return null;
                            }
                            string content = ExtractContent(raw);
                            if (content == null) {
                                Diag("missing-content", null);
                                if (fail != null) fail.Splittable = true;
                                return null;
                            }
                            content = TextRules.NormalizeTranslation(content);
                            content = TextRules.NormalizeTranslationResult(content, source);
                            if (content.Length == 0) {
                                Diag("empty-content", null);
                                if (fail != null) fail.Splittable = true;
                                return null;
                            }
                            return content;
                        }
                    }
                }
            } finally {
                gate.Release();
            }
        }

        private static string ExtractContent(string raw)
        {
            try {
                var dom = Json.ParseStrict(raw) as Dictionary<string, object>;
                var choices = Json.TopArray(dom, "choices");
                if (choices == null || choices.Count == 0) return null;
                var first = choices[0] as Dictionary<string, object>;
                var message = Json.TopObject(first);
                var msg = message != null && message.TryGetValue("message", out var m)
                    ? m as Dictionary<string, object> : null;
                string content = msg != null ? (msg.TryGetValue("content", out var c) ? c as string : null) : null;
                if (content == null || content.IndexOf('\0') >= 0) return null;
                // JSON 转义也可能产生孤立代理；不允许编码器把坏译文替换成 U+FFFD。
                StrictResponseUtf8.GetByteCount(content);
                return content;
            } catch (Json.ParseException) {
                return null;
            } catch (EncoderFallbackException) {
                return null;
            }
        }

        /* parse_json_string_array：批量响应必须是等长同序的字符串数组。 */
        private static string[] ParseStringArray(string json, IList<string> sources)
        {
            int expected = sources.Count;
            List<object> arr;
            try {
                var dom = Json.Parse(json);
                arr = dom as List<object>;
            } catch (Json.ParseException) {
                return null;
            }
            if (arr == null || arr.Count != expected) return null;
            var outArr = new string[expected];
            for (int i = 0; i < expected; i++) {
                // 外部模型可能返回等长但混入 null/数字的数组；上游无法约束其输出。
                // 拒绝整批并交给现有 batch-shape 诊断和拆分重试，不把坏元素缓存为译文。
                var item = arr[i] as string;
                if (item == null) return null;
                outArr[i] = TextRules.NormalizeTranslationResult(
                    TextRules.NormalizeTranslation(item), sources[i]);
                if (outArr[i] == null) return null;
            }
            return outArr;
        }

        public static bool IsTranslatable(ApiConfig cfg, string text)
        {
            return cfg != null && cfg.Enabled
                && !string.IsNullOrEmpty(text);
        }

        public static async Task<string> TranslateSingle(
            ApiConfig cfg, string text, SemaphoreSlim gate, FailureInfo fail = null)
        {
            if (!IsTranslatable(cfg, text)) return null;
            var texts = new[] { MixedText.Protect(text) };
            string body = BuildMessages(
                BuildSystemContent(SystemPrefix, cfg, texts),
                SingleUserLine + texts[0],
                EstimateMaxTokens(texts));
            string reply = await SendChatAsync(cfg, body, gate, fail, texts[0]).ConfigureAwait(false);
            if (reply == null) return null;
            if (MixedText.TryRestore(text, reply, out string restored)) return restored;
            Diag("mixed-cjk", "protected Chinese token mismatch");
            if (fail != null) fail.Splittable = true;
            return null;
        }

        public static async Task<string[]> TranslateBatch(
            ApiConfig cfg, IList<string> texts, SemaphoreSlim gate, FailureInfo fail = null)
        {
            if (cfg == null || !cfg.Enabled || texts == null || texts.Count == 0) return null;
            var requests = ProtectMixedTexts(texts);
            string body = BuildMessages(
                BuildSystemContent(
                    SystemPrefix + " Translate each element of the user's JSON array in order. Return only a valid JSON array with the same length and order.",
                    cfg, requests),
                BuildBatchUser(requests),
                EstimateMaxTokens(requests));
            var content = await SendChatAsync(cfg, body, gate, fail).ConfigureAwait(false);
            if (content == null) return null;
            var arr = ParseStringArray(content, requests);
            if (arr == null) {
                Diag("batch-shape", "expected=" + texts.Count);
                if (fail != null) fail.Splittable = true; // 形状错误：缩小批次有机会修复
            }
            return RestoreMixedTexts(texts, arr, fail);
        }

        public static async Task<string[]> TranslateBatchCtx(
            ApiConfig cfg, IList<string> texts, IList<string> prevs, SemaphoreSlim gate,
            FailureInfo fail = null)
        {
            if (cfg == null || !cfg.Enabled || texts == null || texts.Count == 0) return null;
            var requests = ProtectMixedTexts(texts);
            string body = BuildMessages(
                BuildSystemContent(
                    SystemPrefix + " The texts are consecutive in-game lines in story order; some entries list a previous line for context. Use that context to resolve pronouns, tone, and terminology, and keep character naming consistent, but never translate or output the context itself. Translate each element of the JSON array. Return only a valid JSON array with the same length and order.",
                    cfg, requests),
                BuildCtxUser(requests, texts, prevs),
                EstimateMaxTokens(requests));
            var content = await SendChatAsync(cfg, body, gate, fail).ConfigureAwait(false);
            if (content == null) return null;
            var arr = ParseStringArray(content, requests);
            if (arr == null) {
                Diag("batch-shape", "expected=" + texts.Count);
                if (fail != null) fail.Splittable = true;
            }
            return RestoreMixedTexts(texts, arr, fail);
        }

        private static string[] ProtectMixedTexts(IList<string> texts)
        {
            var requests = new string[texts.Count];
            for (int i = 0; i < texts.Count; i++) requests[i] = MixedText.Protect(texts[i]);
            return requests;
        }

        private static string[] RestoreMixedTexts(IList<string> texts, string[] replies, FailureInfo fail)
        {
            if (replies == null) return null;
            for (int i = 0; i < texts.Count; i++) {
                if (!MixedText.TryRestore(texts[i], replies[i], out string restored)) {
                    Diag("mixed-cjk", "protected Chinese token mismatch; slot=" + i);
                    if (fail != null) fail.Splittable = true;
                    return null;
                }
                replies[i] = restored;
            }
            return replies;
        }

        private static long splitAbortCount;

        /* 整批失败后的二分重试（与 api.c 的 api_translate_split_retry 对齐）：
           对半递归定位最小失败单元（≤4 条时逐条兜底），失败槽位为 null，
           调用方以 null 计 miss，绝不把原文回显当翻译。
           只有内容/形状类失败才拆分：端点不可达、鉴权失败、限流、5xx 属于终止类
           失败，缩小批次只会把同一个错误重复几十次（48 条整批全坏时约 79 次请求，
           比它取代的逐条方案还多）。终止类失败直接返回全 null，条目在后台队列
           收尾时被遗忘，下次 miss 可再次排队。 */
        public static async Task<string[]> TranslateSplitRetry(
            ApiConfig cfg, IList<string> texts, IList<string> prevs, SemaphoreSlim gate,
            Func<bool> yieldCheck = null)
        {
            var arr = new string[texts.Count];
            if (cfg == null || !cfg.Enabled || texts.Count == 0) return arr;

            // 每个子请求前让路：函数入口覆盖首次调用与每层递归，逐条兜底循环
            // 在每个条目前单独检查。让路命中时剩余槽位保持 null 返回，
            // 条目由后台队列收尾遗忘、下次 miss 重新排队，不产生重复翻译。
            if (yieldCheck != null && yieldCheck()) {
                SplitAbortDiag(texts.Count);
                return arr;
            }

            bool hasPrev = false;
            for (int i = 0; i < texts.Count; i++) {
                if (prevs != null && i < prevs.Count && !string.IsNullOrEmpty(prevs[i])) {
                    hasPrev = true;
                    break;
                }
            }

            var fail = new FailureInfo();
            string[] translated = hasPrev
                ? await TranslateBatchCtx(cfg, texts, prevs, gate, fail).ConfigureAwait(false)
                : await TranslateBatch(cfg, texts, gate, fail).ConfigureAwait(false);
            if (translated != null) {
                for (int i = 0; i < texts.Count; i++) arr[i] = translated[i];
                return arr;
            }
            if (!fail.Splittable) {
                SplitAbortDiag(texts.Count);
                return arr;
            }

            const int SplitMinItems = 4;
            if (texts.Count <= SplitMinItems) {
                for (int i = 0; i < texts.Count; i++) {
                    if (yieldCheck != null && yieldCheck()) {
                        // 让路命中：剩余条目按 miss 遗忘（与终止类失败同一收尾语义）。
                        SplitAbortDiag(texts.Count - i);
                        break;
                    }
                    string[] one;
                    var oneFail = new FailureInfo();
                    bool itemHasPrev = prevs != null && i < prevs.Count
                        && !string.IsNullOrEmpty(prevs[i]);
                    if (itemHasPrev) {
                        one = await TranslateBatchCtx(
                            cfg, new[] { texts[i] }, new[] { prevs[i] }, gate, oneFail)
                            .ConfigureAwait(false);
                    } else {
                        one = await TranslateBatch(
                            cfg, new[] { texts[i] }, gate, oneFail).ConfigureAwait(false);
                    }
                    if (one != null && one.Length == 1) {
                        arr[i] = one[0];
                    } else if (!oneFail.Splittable) {
                        // 逐条阶段遇到终止类失败：剩余条目不再逐个撞同一个错误。
                        SplitAbortDiag(texts.Count - i);
                        break;
                    }
                }
                return arr;
            }

            int half = texts.Count / 2;
            var left = new string[half];
            var right = new string[texts.Count - half];
            for (int i = 0; i < half; i++) left[i] = texts[i];
            for (int i = half; i < texts.Count; i++) right[i - half] = texts[i];

            var prevsLeft = prevs != null ? SliceList(prevs, 0, half) : null;
            var prevsRight = prevs != null ? SliceList(prevs, half, texts.Count - half) : null;

            var a = await TranslateSplitRetry(cfg, left, prevsLeft, gate, yieldCheck).ConfigureAwait(false);
            var b = await TranslateSplitRetry(cfg, right, prevsRight, gate, yieldCheck).ConfigureAwait(false);
            for (int i = 0; i < half; i++) arr[i] = a[i];
            for (int i = half; i < texts.Count; i++) arr[i] = b[i - half];
            return arr;
        }

        private static void SplitAbortDiag(int remaining)
        {
            long n = Interlocked.Increment(ref splitAbortCount);
            if (n <= 3 || (n & (n - 1)) == 0) {
                Console.Error.WriteLine("[api] split-retry aborted #" + n
                    + " (terminal failure, remaining=" + remaining + ")");
            }
        }

        private static IList<string> SliceList(IList<string> src, int start, int count)
        {
            var outList = new string[count];
            for (int i = 0; i < count; i++) outList[i] = src[start + i];
            return outList;
        }

        /* api_glossary_load_file：TSV 术语表 → 提示词段（跳过 BOM/注释/畸形行）。 */
        public static void LoadGlossary(ApiConfig cfg, string path)
        {
            if (cfg == null || string.IsNullOrEmpty(path)) return;
            if (!File.Exists(path)) return;
            string[] lines;
            try {
                lines = File.ReadAllLines(path, new UTF8Encoding(false));
            } catch (IOException) {
                return;
            }
            var section = new StringBuilder();
            section.Append("Mandatory glossary (use these exact translations):\n");
            int entries = 0, malformed = 0;
            foreach (var rawLine in lines) {
                var line = rawLine.TrimEnd('\r', '\n');
                if (line.Length == 0 || line[0] == '#') continue;
                int tab = line.IndexOf('\t');
                if (tab <= 0 || tab == line.Length - 1) { malformed++; continue; }
                section.Append("- ").Append(line.Substring(0, tab))
                       .Append(" => ").Append(line.Substring(tab + 1)).Append('\n');
                entries++;
                if (entries >= 256 || section.Length > 8192 / 2) break;
            }
            if (entries > 0) {
                cfg.Glossary = section.ToString();
                Console.Error.WriteLine("[api] glossary loaded: " + entries
                    + " entries from " + path);
            }
            if (malformed > 0) {
                Console.Error.WriteLine("[api] glossary skipped " + malformed
                    + " malformed line(s) in " + path);
            }
        }
    }
}
