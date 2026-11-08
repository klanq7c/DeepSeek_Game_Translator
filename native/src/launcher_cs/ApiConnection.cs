using System;
using System.IO;
using System.Net;
using System.Text;
using DstCore;

namespace DstLauncher
{
    // 当前配置草稿的一次短聊天请求；与目录刷新分开，不读写配置、翻译记忆或本地服务。
    public static class ApiConnection
    {
        public sealed class Result { public bool Success; public string Message; }
        private const int MaxBytes = 65536;
        private const string InvalidReply = "未获得有效模型回复；请检查接口兼容性或模型输出";
        // 首次模型加载是本机提供商的外部等待，不修改模型或自动重试。
        // 无效地址由 Test 诊断；这里供请求与窗口共用同一响应等待策略。
        public static int WaitMilliseconds(string endpoint)
        {
            Uri uri;
            if (!Uri.TryCreate(endpoint, UriKind.Absolute, out uri) || uri.UserInfo.Length != 0 ||
                (uri.Scheme != "http" && uri.Scheme != "https")) return 15000;
            return ApiModels.IsLiteralLoopback(uri) ? 120000 : 15000;
        }
        private static Result Failure(string stage, int status, int error)
        { return new Result { Message = stage + "（HTTP " + status + " / 错误 " + error + "）" }; }
        private static string HttpError(int status)
        {
            if (status == 401) return "认证失败；请检查 API Key";
            if (status == 403) return "访问被拒绝；请检查账号和模型权限";
            if (status == 404) return "接口或模型不存在；请检查地址和模型 ID";
            if (status == 407) return "代理需要认证；请检查系统代理";
            if (status == 429) return "请求受限；请检查额度或稍后重试";
            if (status >= 300 && status < 400) return "接口发生重定向；请填写最终地址";
            if (status >= 500) return "提供商暂时不可用；请稍后重试";
            return "请求被拒绝；请检查模型、请求兼容性或账号额度";
        }
        public static Result Test(string endpoint, string model, string key)
        {
            Uri uri;
            if (string.IsNullOrEmpty(endpoint) || endpoint.Length > 1023 || endpoint.IndexOfAny(new[] { ' ', '\r', '\n', '\t', '#', '\\' }) >= 0 ||
                !Uri.TryCreate(endpoint, UriKind.Absolute, out uri) || uri.UserInfo.Length != 0)
                return Failure("请填写有效的完整 API 地址", 0, 0);
            bool local = ApiModels.IsLiteralLoopback(uri);
            if (uri.Scheme != "https" && !(uri.Scheme == "http" && local)) return Failure("远程服务需要 HTTPS", 0, 0);
            string stage = "请填写模型 ID（最多 255 个 UTF-8 字节）";
            var encoding = new UTF8Encoding(false, true);
            try {
                if (string.IsNullOrWhiteSpace(model) || encoding.GetByteCount(model) > 255) return Failure(stage, 0, 0);
            } catch (EncoderFallbackException) { return Failure(stage, 0, 0); }
            if (key.Length > 1023) return Failure("API Key 含无效字符", 0, 0);
            foreach (char c in key) if (c <= 32 || c > 126) return Failure("API Key 含无效字符", 0, 0);
            string option = "";
            // 模型使用原始 ID；只在与翻译请求一致的地址边界添加非思考选项。
            if (uri.Host == "api.deepseek.com" && (model == "deepseek-flash" || model.StartsWith("deepseek-v4-", StringComparison.Ordinal)))
                option = ",\"thinking\":{\"type\":\"disabled\"}";
            if (local && uri.Port == 11434 && (uri.AbsolutePath == "/v1/chat/completions" || uri.AbsolutePath == "/v1/chat/completions/"))
                option = ",\"reasoning_effort\":\"none\"";
            byte[] payload = encoding.GetBytes("{\"model\":" + Json.Escape(model) +
                ",\"messages\":[{\"role\":\"user\",\"content\":\"Reply with OK.\"}],\"temperature\":0,\"max_tokens\":128,\"stream\":false" + option + "}");
            int status = 0, error = 0;
            stage = "网络请求失败";
            // 异常边界只包围提供商传输和解码；错误只保留阶段/状态码，避免泄漏响应正文或 Key。
            try {
                var request = (HttpWebRequest)WebRequest.Create(uri);
                request.Method = "POST"; request.ContentType = "application/json"; request.Accept = "application/json";
                request.AllowAutoRedirect = false; request.Timeout = WaitMilliseconds(endpoint); request.ReadWriteTimeout = 15000;
                request.ContentLength = payload.Length;
                if (key.Length != 0) request.Headers["Authorization"] = "Bearer " + key;
                using (var output = request.GetRequestStream()) output.Write(payload, 0, payload.Length);
                using (var response = (HttpWebResponse)request.GetResponse()) {
                    status = (int)response.StatusCode;
                    if (status != 200) return Failure(HttpError(status), status, 0);
                    long expected = response.ContentLength;
                    if (expected >= MaxBytes) return Failure("检测响应过大", status, 0);
                    using (var input = response.GetResponseStream())
                    using (var body = new MemoryStream()) {
                        byte[] chunk = new byte[8192]; var watch = System.Diagnostics.Stopwatch.StartNew();
                        for (;;) {
                            stage = "网络读取失败";
                            int got = input.Read(chunk, 0, chunk.Length);
                            if (got == 0) break;
                            if (body.Length + got >= MaxBytes || watch.ElapsedMilliseconds > 30000)
                                return Failure("检测响应过大或读取超时", status, 0);
                            body.Write(chunk, 0, got);
                        }
                        if (expected >= 0 && body.Length != expected) return Failure("检测响应未完整接收", status, 0);
                        stage = InvalidReply;
                        object dom = Json.ParseStrict(encoding.GetString(body.ToArray()));
                        var root = Json.TopObject(dom); var choices = Json.TopArray(dom, "choices");
                        if (root == null || root.ContainsKey("error") || choices == null || choices.Count == 0) return Failure(stage, status, 0);
                        var choice = Json.TopObject(choices[0]); object message;
                        if (choice == null || !choice.TryGetValue("message", out message)) return Failure(stage, status, 0);
                        string content = Json.TopString(message, "content");
                        if (string.IsNullOrWhiteSpace(content) || content.IndexOf('\0') >= 0) return Failure(stage, status, 0);
                        encoding.GetByteCount(content);
                        return new Result { Success = true, Message = "连通成功：当前模型已返回有效回复" };
                    }
                }
            } catch (WebException ex) {
                using (var response = ex.Response as HttpWebResponse) {
                    if (response != null) { status = (int)response.StatusCode; stage = HttpError(status); }
                    else {
                        error = (int)ex.Status;
                        if (ex.Status == WebExceptionStatus.Timeout) stage = "检测超时；请检查网络或等待模型加载后重试";
                        else if (ex.Status == WebExceptionStatus.TrustFailure || ex.Status == WebExceptionStatus.SecureChannelFailure) stage = "TLS 证书验证失败；请检查证书和系统时间";
                        else if (ex.Status == WebExceptionStatus.NameResolutionFailure) stage = "无法解析服务地址；请检查地址或 DNS";
                    }
                }
            } catch (IOException ex) { error = ex.HResult;
            } catch (Json.ParseException) { stage = InvalidReply;
            } catch (DecoderFallbackException) { stage = InvalidReply;
            } catch (EncoderFallbackException) { stage = InvalidReply; }
            return Failure(stage, status, error);
        }
    }
}
