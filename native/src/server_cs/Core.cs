using System;
using System.Collections.Generic;
using System.IO;
using System.Text;
using DstCore;

namespace DstServerCs
{
    /* 启动读取外部 TSV 时逐块扫描；损坏的超长行只消费、不累积。 */
    internal sealed class BoundedCacheLineReader
    {
        private readonly TextReader source;
        private readonly int maxChars;
        private readonly char[] buffer = new char[16 * 1024];
        private int position;
        private int length;
        private bool skipLf;

        public BoundedCacheLineReader(TextReader source, int maxChars)
        {
            if (source == null) throw new ArgumentNullException(nameof(source));
            if (maxChars < 0) throw new ArgumentOutOfRangeException(nameof(maxChars));
            this.source = source;
            this.maxChars = maxChars;
        }

        public string ReadLine(out bool tooLong)
        {
            tooLong = false;
            var line = new StringBuilder();
            bool hasContent = false;
            while (true) {
                if (position == length) {
                    length = source.Read(buffer, 0, buffer.Length);
                    position = 0;
                    if (length == 0) return hasContent ? (tooLong ? "" : line.ToString()) : null;
                }
                if (skipLf) {
                    skipLf = false;
                    if (buffer[position] == '\n') {
                        position++;
                        continue;
                    }
                }
                int start = position;
                while (position < length && buffer[position] != '\r' && buffer[position] != '\n') position++;
                int count = position - start;
                if (count > 0) {
                    hasContent = true;
                    if (!tooLong) {
                        if (count > maxChars - line.Length) tooLong = true;
                        else line.Append(buffer, start, count);
                    }
                }
                if (position < length) {
                    if (buffer[position++] == '\r') skipLf = true;
                    return tooLong ? "" : line.ToString();
                }
            }
        }
    }

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
        /* 与 map 一起受 opsLock 保护；存在于内存不代表当前值已落盘。 */
        private readonly HashSet<string> unpersisted = new HashSet<string>();
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
                        if (exists && old == cleaned[i] && !unpersisted.Contains(keys[i])) {
                            // 同值且已持久化：不重复扩大 TSV。
                            result.Persisted++;
                            continue;
                        }
                        map[keys[i]] = cleaned[i];
                        unpersisted.Add(keys[i]);
                        changed[i] = true;
                    }
                }
                int wrote = 0;
                bool ioFailed = false;
                for (int i = 0; i < keys.Length; i++) {
                    if (!changed[i]) continue;
                    EnsureWriter();
                    if (persistWriter == null) break; // 打开失败：下一批重试
                    CacheCodec.AppendLine(journal, keys[i], cleaned[i]);
                    try {
                        persistWriter.Write(journal.ToString());
                    } catch (Exception ex) {
                        System.Console.Error.WriteLine(
                            "[cache] persist-write failed (" + ex.GetType().Name
                            + ", path=" + path + ")");
                        ioFailed = true;
                        break;
                    }
                    wrote++;
                    journal.Length = 0;
                }
                // 与 C 的 fflush 边界一致：一批只刷新一次。Write 成功不代表已落盘，
                // 只有全部写入与最终 Flush 均成功，才能确认本批新增行及当前内存值。
                if (wrote > 0 && !ioFailed) {
                    try {
                        persistWriter.Flush();
                    } catch (Exception ex) {
                        System.Console.Error.WriteLine(
                            "[cache] persist-flush failed (" + ex.GetType().Name
                            + ", path=" + path + ")");
                        ioFailed = true;
                    }
                }
                // 磁盘满/权限/句柄失效无法在上游修复；即使关闭时 Flush 恰好成功，
                // 失败批次仍全部保留脏状态并返回未确认计数。stderr 记录操作与路径，
                // 下次同值更新重试追加，绝不把错误藏成成功。A/B/A 也必须整批确认。
                if (ioFailed) CloseWriter();
                if (wrote > 0 && !ioFailed) {
                    result.Persisted += wrote;
                    lock (opsLock) {
                        for (int i = 0; i < keys.Length; i++) {
                            if (changed[i] && map.TryGetValue(keys[i], out var current)
                                && current == cleaned[i]) unpersisted.Remove(keys[i]);
                        }
                    }
                }
            }
            return result;
        }

        private void EnsureWriter()
        {
            if (persistWriter != null) return;
            FileStream fs = null;
            try {
                fs = new FileStream(path, FileMode.OpenOrCreate, FileAccess.ReadWrite, FileShare.Read);
                long end = fs.Seek(0, SeekOrigin.End);
                if (end > 0) {
                    fs.Seek(-1, SeekOrigin.End);
                    int last = fs.ReadByte();
                    if (last < 0) throw new IOException("Cannot read cache tail");
                    fs.Seek(0, SeekOrigin.End);
                    if (last != '\n') {
                        // 磁盘写入中断或外部无换行文件只能在持久化边界识别，
                        // 上游内存值无法修复记录分隔。只追加换行，保留所有既有字节；
                        // stderr 明示异常尾部，分隔写失败仍报告失败并保留脏状态。
                        Console.Error.WriteLine("[cache] persist-tail: unterminated tail; appending separator (path=" + path + ")");
                        fs.WriteByte((byte)'\n');
                    }
                }
                persistWriter = new StreamWriter(fs, new UTF8Encoding(false));
                fs = null; // 句柄所有权交给 writer。
            } catch (Exception ex) {
                persistWriter = null;
                System.Console.Error.WriteLine(
                    "[cache] persist-open failed (" + ex.GetType().Name
                    + ", path=" + path + ")");
            } finally {
                if (fs != null) {
                    try { fs.Dispose(); }
                    catch (IOException ex) {
                        Console.Error.WriteLine("[cache] persist-close failed (" + ex.GetType().Name + ", path=" + path + ")");
                    }
                }
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
            long oversized = 0;
            lock (opsLock) {
                try {
                    using (var stream = new StreamReader(path, new UTF8Encoding(false))) {
                        var reader = new BoundedCacheLineReader(stream, CacheCodec.MaxLineChars);
                        string line;
                        bool tooLong;
                        while ((line = reader.ReadLine(out tooLong)) != null) {
                            if (tooLong) {
                                // 外部缓存文件中的超 8 MiB 损坏行会使无界 ReadLine 占满内存；
                                // 上游已落盘的文件无法从当前内存值修复。跳过该行可能丢失其中的
                                // 原始记录，但绝不伪装为成功译文；stderr 限频记录次数及路径。
                                oversized++;
                                if (oversized <= 3 || (oversized & (oversized - 1)) == 0)
                                    Console.Error.WriteLine("[cache] load-line-too-large skipped (count="
                                        + oversized + ", path=" + path + ")");
                                continue;
                            }
                            string k, v;
                            if (!CacheCodec.TryDecodeLine(line, out k, out v)) continue; // 损坏/超长/回显：跳过
                            map[k] = v;
                            unpersisted.Remove(k);
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
