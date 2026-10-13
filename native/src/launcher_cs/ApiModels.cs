using System;
using System.Collections.Generic;
using System.IO;
using System.Net;
using System.Text;
using DstCore;

namespace DstLauncher
{
    public static class ApiModels
    {
        public sealed class Result
        {
            public string[] Items = new string[0];
            public string Message;
        }

        private const int Compatible = 0, Anthropic = 1, DashScope = 2;
        private const string InvalidCatalog = "模型列表格式无效、不完整或超出限制";

        private static bool DashScopeHost(string host)
        {
            if (host == "dashscope.aliyuncs.com" || host == "dashscope-intl.aliyuncs.com" || host == "cn-hongkong.dashscope.aliyuncs.com") return true;
            foreach (string suffix in new[] { ".cn-beijing.maas.aliyuncs.com", ".ap-northeast-1.maas.aliyuncs.com", ".eu-central-1.maas.aliyuncs.com", ".us-east-1.maas.aliyuncs.com" }) {
                if (!host.EndsWith(suffix, StringComparison.Ordinal) || host.Length <= suffix.Length) continue;
                foreach (char c in host.Substring(0, host.Length - suffix.Length))
                    if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-')) return false;
                return true;
            }
            return false;
        }
        private static string CatalogUrl(string endpoint, out int kind)
        {
            kind = Compatible;
            string e = endpoint.TrimEnd('/');
            const string suffix = "/chat/completions";
            Uri uri;
            if (!e.EndsWith(suffix, StringComparison.Ordinal) || e.IndexOfAny(new[] { '?', '#', '\r', '\n' }) >= 0 ||
                !Uri.TryCreate(endpoint, UriKind.Absolute, out uri)) return null;
            string url = e.Substring(0, e.Length - suffix.Length) + "/models";
            if (uri.Host == "api.anthropic.com") { kind = Anthropic; return url + "?limit=1000"; }
            if (uri.Host == "api.siliconflow.cn" || uri.Host == "api.siliconflow.com") return url + "?sub_type=chat";
            if (DashScopeHost(uri.Host) && uri.AbsolutePath.TrimEnd('/') == "/compatible-mode/v1/chat/completions") {
                // 百炼目录与兼容聊天接口路径不同；保持原主机，绝不猜业务空间或跨主机发送 Key。
                kind = DashScope;
                return endpoint.Substring(0, endpoint.Length - uri.AbsolutePath.Length) +
                    "/api/v1/models?capabilities=TG&capabilities=Reasoning&page_size=100";
            }
            return url;
        }
        public static string ModelsUrl(string endpoint) { int kind; return CatalogUrl(endpoint, out kind); }

        public static Result Fetch(string endpoint, string key)
        {
            int kind;
            string url = CatalogUrl(endpoint, out kind);
            if (url == null) return Failure(new Result(), "地址不支持；请使用以 /chat/completions 结尾的地址", 0, 0);
            return FetchCatalog(url, key, kind);
        }
        private static object Field(object dom, string name)
        {
            var obj = Json.TopObject(dom); object value;
            return obj != null && obj.TryGetValue(name, out value) ? value : null;
        }
        private static int Number(object dom, string name)
        {
            object value = Field(dom, name);
            if (!(value is double)) return -1;
            double n = (double)value;
            return n >= 0 && n <= 4096 && n == Math.Floor(n) ? (int)n : -1;
        }
        private static bool HasField(object dom, string name)
        {
            var obj = Json.TopObject(dom);
            return obj != null && obj.ContainsKey(name);
        }
        private static string EncodeCursor(string cursor)
        {
            byte[] bytes = new UTF8Encoding(false, true).GetBytes(cursor);
            if (bytes.Length == 0 || bytes.Length > 255) throw new Json.ParseException("invalid cursor");
            var text = new StringBuilder();
            foreach (byte b in bytes) text.Append('%').Append(b.ToString("X2"));
            return text.ToString();
        }

        /* 分页复用同一主机和认证，不接受服务端给出的跳转 URL；任何页失败都丢弃整份目录。
           网络错误保留手工填写路径，并返回诊断；不记录 URL、Key 或响应正文。 */
        private static Result FetchCatalog(string url, string key, int kind)
        {
            var result = new Result();
            string stage = "地址不支持；请使用以 /chat/completions 结尾的地址";
            int status = 0, error = 0;
            try {
                Uri uri;
                if (!Uri.TryCreate(url, UriKind.Absolute, out uri) || uri.UserInfo.Length != 0) return Failure(result, stage, status, error);
                bool local = uri.Host.Equals("localhost", StringComparison.OrdinalIgnoreCase) || uri.Host == "127.0.0.1" || uri.Host == "[::1]" || uri.Host == "::1";
                stage = "远程服务需要 HTTPS";
                if (uri.Scheme != "https" && !(uri.Scheme == "http" && local)) return Failure(result, stage, status, error);
                stage = "API Key 含无效字符";
                if (key.Length > 1023) return Failure(result, stage, status, error);
                foreach (char c in key) if (c <= 32 || c > 126) return Failure(result, stage, status, error);
                var models = new List<string>();
                var seen = new HashSet<string>(StringComparer.Ordinal);
                int received = 0, total = -1, bytesRead = 0;
                string cursor = "";
                var started = System.Diagnostics.Stopwatch.StartNew();
                for (int page = 1; ; page++) {
                    stage = "模型目录分页超出限制或读取超时";
                    if (page > 64 || started.ElapsedMilliseconds > 60000) return Failure(result, stage, status, error);
                    string pageUrl = url;
                    if (kind == DashScope) pageUrl += (url.Contains("?") ? "&" : "?") + "page_no=" + page;
                    else if (cursor.Length != 0) pageUrl += (url.Contains("?") ? "&" : "?") + "after_id=" + EncodeCursor(cursor);
                    stage = "网络请求失败";
                    var request = (HttpWebRequest)WebRequest.Create(pageUrl);
                    request.Method = "GET";
                    request.Accept = "application/json";
                    request.AllowAutoRedirect = false;
                    request.Timeout = 15000;
                    request.ReadWriteTimeout = 5000;
                    if (key.Length != 0) {
                        request.Headers[kind == Anthropic ? "x-api-key" : "Authorization"] = kind == Anthropic ? key : "Bearer " + key;
                        if (kind == Anthropic) request.Headers["anthropic-version"] = "2023-06-01";
                    }
                    object dom;
                    using (var response = (HttpWebResponse)request.GetResponse()) {
                        status = (int)response.StatusCode;
                        stage = "厂商返回 HTTP 错误";
                        if (status != 200) return Failure(result, stage, status, error);
                        long expected = response.ContentLength;
                        stage = "模型列表过大或读取超时";
                        if (expected >= 2 * 1024 * 1024) return Failure(result, stage, status, error);
                        using (var stream = response.GetResponseStream())
                        using (var body = new MemoryStream()) {
                            byte[] chunk = new byte[16384];
                            var watch = System.Diagnostics.Stopwatch.StartNew();
                            for (;;) {
                                stage = "网络读取失败";
                                int got = stream.Read(chunk, 0, chunk.Length);
                                if (got == 0) break;
                                stage = "模型列表过大或读取超时";
                                if (body.Length + got >= 2 * 1024 * 1024 || watch.ElapsedMilliseconds > 15000) return Failure(result, stage, status, error);
                                bytesRead += got;
                                stage = "模型目录分页超出限制或读取超时";
                                if (bytesRead > 16 * 1024 * 1024 || started.ElapsedMilliseconds > 60000) return Failure(result, stage, status, error);
                                body.Write(chunk, 0, got);
                            }
                            stage = "模型列表响应未完整接收";
                            if (expected >= 0 && body.Length != expected) return Failure(result, stage, status, error);
                            stage = InvalidCatalog;
                            dom = Json.ParseStrict(new UTF8Encoding(false, true).GetString(body.ToArray()));
                        }
                    }
                    bool more = false;
                    string previous = cursor; cursor = "";
                    var data = Json.TopArray(dom, "data");
                    if (kind == DashScope) {
                        object output = Field(dom, "output"), success = Field(dom, "success");
                        if (output == null || (HasField(dom, "success") && (!(success is bool) || !(bool)success))) return Failure(result, stage, status, error);
                        data = Json.TopArray(output, "models");
                        int expected = Number(output, "total");
                        if (expected < 0 || (total >= 0 && total != expected) || Number(output, "page_no") != page) return Failure(result, stage, status, error);
                        total = expected;
                    } else {
                        object hasMore = Field(dom, "has_more");
                        if (HasField(dom, "has_more") && !(hasMore is bool)) return Failure(result, stage, status, error);
                        more = hasMore is bool && (bool)hasMore;
                        if (more) {
                            cursor = Json.TopString(dom, "last_id");
                            if (kind != Anthropic || string.IsNullOrEmpty(cursor) || cursor.Length > 255 || cursor.IndexOf('\0') >= 0) return Failure(result, stage, status, error);
                        }
                    }
                    if (data == null) return Failure(result, stage, status, error);
                    int before = models.Count;
                    foreach (object entry in data) {
                        string id = Json.TopString(entry, kind == DashScope ? "model" : "id");
                        if (string.IsNullOrEmpty(id) || id.Length > 255 || new UTF8Encoding(false, true).GetByteCount(id) > 255) return Failure(result, stage, status, error);
                        foreach (char c in id) if (c <= 32 || c == 127) return Failure(result, stage, status, error);
                        if (seen.Add(id)) models.Add(id);
                        if (models.Count > 4096) return Failure(result, stage, status, error);
                    }
                    received += data.Count;
                    if (kind == DashScope) {
                        if (received > total || (data.Count == 0 && received < total)) return Failure(result, stage, status, error);
                        more = received < total;
                    }
                    if (!more) break;
                    if (data.Count == 0 || models.Count == before || (kind == Anthropic && cursor == previous)) return Failure(result, stage, status, error);
                }
                if (models.Count == 0) return Failure(result, "厂商未返回可选模型；可手动输入", status, error);
                result.Items = models.ToArray();
                result.Message = "已更新 " + models.Count + " 个模型；请选择聊天模型，也可手动输入";
                return result;
            } catch (WebException ex) {
                using (var response = ex.Response as HttpWebResponse) {
                    if (response != null) { status = (int)response.StatusCode; stage = "厂商返回 HTTP 错误"; }
                    else error = (int)ex.Status;
                }
            } catch (IOException ex) { error = ex.HResult;
            } catch (Json.ParseException) { stage = InvalidCatalog;
            } catch (DecoderFallbackException) { stage = InvalidCatalog;
            } catch (EncoderFallbackException) { stage = InvalidCatalog;
            } catch (OutOfMemoryException) { stage = "模型列表内存分配失败"; }
            return Failure(result, stage, status, error);
        }

        private static Result Failure(Result r, string stage, int status, int error)
        {
            r.Message = stage + "（HTTP " + status + " / 错误 " + error + "）；可手动输入";
            return r;
        }
    }
}
