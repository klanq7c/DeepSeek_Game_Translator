using System;
using System.Collections.Generic;
using System.IO;
using System.Text;
using DstCore;

namespace DstServerCs
{
    /*
     * CacheStore —— 与 cache.c 对齐的本地翻译缓存。
     *
     * 持久化格式与准入规则由共享核心 DstCore.CacheCodec 定义
     * （<base64(key)>\t<base64(value)>\n，标准 Base64；空键/空值/清洗后为空/
     * 清洗后与键同文一律拒绝，绝不伪装成功）。本类只负责内存字典、锁与文件 IO。
     * lock 保护字典；ioLock 仅串行化追加写，磁盘 IO 不持字典锁。
     */

    public class CacheStore
    {
        public sealed class PersistResult
        {
            public int Accepted;
            public int Persisted;
            public int Rejected;
            public string Status
            {
                get
                {
                    if (Persisted == Accepted) return "all";
                    return Persisted > 0 ? "partial" : "failed";
                }
            }
        }

        private readonly object opsLock = new object();
        private readonly object ioLock = new object();
        private readonly Dictionary<string, string> map =
            new Dictionary<string, string>();
        private readonly string path;
        private StreamWriter persistWriter;

        public CacheStore(string cachePath)
        {
            path = cachePath;
        }

        public int Size
        {
            get { lock (opsLock) return map.Count; }
        }

        public bool Contains(string key)
        {
            lock (opsLock) return map.ContainsKey(key);
        }

        public string Get(string key)
        {
            lock (opsLock) return map.TryGetValue(key, out var v) ? v : null;
        }

        /* cache_set_many_persist_result 的语义对应实现。 */
        public PersistResult SetManyPersist(string[] keys, string[] values)
        {
            var result = new PersistResult();
            if (keys == null || values == null || keys.Length == 0) return result;

            var cleaned = new string[keys.Length];
            var changed = new bool[keys.Length];
            for (int i = 0; i < keys.Length; i++) {
                var clean = CacheCodec.NormalizeForStore(keys[i], values[i]);
                if (clean == null) {
                    result.Rejected++;
                    continue;
                }
                cleaned[i] = clean;
                result.Accepted++;
            }

            var journal = new StringBuilder();
            lock (ioLock) {
                lock (opsLock) {
                    for (int i = 0; i < keys.Length; i++) {
                        if (cleaned[i] == null) continue;
                        bool exists = map.TryGetValue(keys[i], out var old);
                        if (exists && old == cleaned[i]) {
                            // 同值且已持久化：不重复扩大 TSV。
                            continue;
                        }
                        map[keys[i]] = cleaned[i];
                        changed[i] = true;
                    }
                }
                for (int i = 0; i < keys.Length; i++) {
                    if (!changed[i]) continue;
                    try {
                        EnsureWriter();
                        if (persistWriter == null) break; // 打开失败：下一批重试
                        CacheCodec.AppendLine(journal, keys[i], cleaned[i]);
                        persistWriter.Write(journal.ToString());
                        persistWriter.Flush();
                        result.Persisted++;
                        journal.Length = 0;
                    } catch (Exception ex) {
                        // 磁盘满/权限/路径非法等外部边界：内存已更新，未落盘部分
                        // 保持脏状态，下次相同更新时重试。诊断写 stderr，不伪装
                        // 成功——调用方以 Persisted/Accepted 差值暴露重启丢失边界。
                        CloseWriter();
                        System.Console.Error.WriteLine(
                            "[cache] persist-write failed (" + ex.GetType().Name
                            + ", path=" + path + ")");
                        break;
                    }
                }
            }
            return result;
        }

        private void EnsureWriter()
        {
            if (persistWriter != null) return;
            try {
                var fs = new FileStream(path, FileMode.Append, FileAccess.Write, FileShare.Read);
                persistWriter = new StreamWriter(fs, new UTF8Encoding(false));
            } catch (Exception ex) {
                persistWriter = null;
                System.Console.Error.WriteLine(
                    "[cache] persist-open failed (" + ex.GetType().Name
                    + ", path=" + path + ")");
            }
        }

        private void CloseWriter()
        {
            try {
                persistWriter?.Dispose();
            } catch (IOException ex) {
                // Dispose 会做最后一次 Flush；磁盘满/句柄失效时这里会再抛一次。
                // 写失败已由调用方记录，这里只补记关闭阶段，不掩盖原始错误。
                System.Console.Error.WriteLine(
                    "[cache] persist-close failed (" + ex.GetType().Name
                    + ", path=" + path + ")");
            } catch (ObjectDisposedException) {
                // 已释放：幂等关闭，无需记录。
            }
            persistWriter = null;
        }

        /* cache_load：启动期全量读入；空键/空值/原文回显跳过。
           读文件失败（权限/被独占/路径非法）是外部边界：记录 stderr 后以空缓存
           启动，与 C 的 cache_load 打不开文件即返回一致；不能让服务器起不来。 */
        public int Load()
        {
            if (!File.Exists(path)) return 0;
            int n = 0;
            lock (opsLock) {
                try {
                    using (var reader = new StreamReader(path, new UTF8Encoding(false))) {
                        string line;
                        while ((line = reader.ReadLine()) != null) {
                            string k, v;
                            if (!CacheCodec.TryDecodeLine(line, out k, out v)) continue; // 损坏/超长/回显：跳过
                            map[k] = v;
                            n++;
                        }
                    }
                } catch (IOException ex) {
                    System.Console.Error.WriteLine(
                        "[cache] load failed after " + n + " entries (" + ex.GetType().Name
                        + ", path=" + path + ")");
                } catch (UnauthorizedAccessException ex) {
                    System.Console.Error.WriteLine(
                        "[cache] load failed after " + n + " entries (" + ex.GetType().Name
                        + ", path=" + path + ")");
                }
            }
            return n;
        }

        /* cache_emit_json_map / cache_emit_json_entries 的数据侧：快照导出。 */
        public KeyValuePair<string, string>[] Snapshot()
        {
            lock (opsLock) {
                var arr = new KeyValuePair<string, string>[map.Count];
                int i = 0;
                foreach (var kv in map) arr[i++] = kv;
                return arr;
            }
        }
    }
}
