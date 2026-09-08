namespace DstCore
{
    /*
     * Contract —— 本地翻译服务 HTTP 契约的常量表（与 http.c 对齐）。
     *
     * 这里只放"名字"：端点路径与 JSON 字段名。所有 C# 端（服务器、Unity payload、
     * 后续 C# 启动器）引用同一份常量，避免某一端悄悄拼错字段名而只在运行时暴露。
     * 改动任何一项都是契约变更：必须同步 http.c、脚本 payload（payloads/RenPy、
     * payloads/RPGMaker、payloads/Godot）以及 docs/USER_GUIDE.md。
     */
    public static class Contract
    {
        public const int DefaultPort = 19999;
        public const string DefaultBaseUrl = "http://127.0.0.1:19999";

        /* 端点：GET */
        public const string PathHealth = "/health";
        public const string PathCapabilities = "/capabilities";
        /* 端点：POST（"/" 与 /translate、/batch 同义；/warmup 与 /prefetch 同义） */
        public const string PathRoot = "/";
        public const string PathTranslate = "/translate";
        public const string PathBatch = "/batch";
        public const string PathPrefetch = "/prefetch";
        public const string PathWarmup = "/warmup";
        public const string PathCacheLookup = "/cache/lookup";
        public const string PathCacheImport = "/cache/import";
        public const string PathCacheExport = "/cache/export";
        public const string PathCacheDump = "/cache/dump";
        public const string PathShutdown = "/shutdown";

        /* 请求字段 */
        public const string FieldText = "text";
        public const string FieldTexts = "texts";
        public const string FieldCacheOnly = "cache_only";
        public const string FieldMemoryOnly = "memory_only";
        public const string FieldEntries = "entries";
        public const string FieldKey = "key";
        public const string FieldValue = "value";

        /* 响应字段 */
        public const string FieldStatus = "status";
        public const string FieldError = "error";
        public const string FieldTranslatedText = "translated_text";
        public const string FieldTranslation = "translation";
        public const string FieldTranslations = "translations";
        public const string FieldResults = "results";
        public const string FieldSources = "sources";
        public const string FieldSource = "source";
        public const string FieldHits = "hits";
        public const string FieldQueued = "queued";
        public const string FieldAccepted = "accepted";
        public const string FieldRejected = "rejected";
        public const string FieldImported = "imported";
        public const string FieldCount = "count";
        public const string FieldCacheSize = "cache_size";

        /* /translate 与 /batch 的 sources[] 取值（http.c op_batch/translate_one）：
             cache     命中本地翻译记忆
             api_batch 本次实时 API 批量得到
             pass      无翻译信号或已是 CJK，原文透传（恒等终态，不能再进缓存）
             queued    未命中，已排入后台翻译队列
             miss      未命中且未排队（队列满 / cache_only / API 未启用） */
        public const string SourceCache = "cache";
        public const string SourceApiBatch = "api_batch";
        public const string SourcePass = "pass";
        public const string SourceQueued = "queued";
        public const string SourceMiss = "miss";

        /* 结果已可用（可直接显示并已在缓存里）。 */
        public static bool IsResolvedSource(string source)
        {
            return source == SourceCache || source == SourceApiBatch;
        }

        /* 结果仍在路上：调用方应稍后用 /cache/lookup 复查。 */
        public static bool IsPendingSource(string source)
        {
            return source == SourceQueued || source == SourceMiss;
        }
    }
}
