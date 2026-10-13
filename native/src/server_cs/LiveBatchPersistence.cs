using System;
using System.Collections.Generic;
using DstCore;

namespace DstServerCs
{
    /* 实时响应先逐项判定可入库性，再作为一批写入；调用方仍按原位置唤醒等待者。 */
    internal static class LiveBatchPersistence
    {
        internal static bool[] Persist(CacheStore cache, string[] texts, string[] values)
        {
            var accepted = new bool[texts.Length];
            if (values == null) return accepted;

            var keys = new List<string>(texts.Length);
            var translations = new List<string>(texts.Length);
            for (int i = 0; i < texts.Length; i++) {
                string value = values[i];
                if (!TextRules.IsResolvedTranslation(texts[i], value)
                    || CacheCodec.NormalizeForStore(texts[i], value) == null) continue;
                keys.Add(texts[i]);
                translations.Add(value);
                accepted[i] = true;
            }

            if (keys.Count == 0) return accepted;
            var result = cache.SetManyPersist(keys.ToArray(), translations.ToArray());
            if (result.Accepted != keys.Count) {
                // 两次准入判定使用同一共享规则，数量不符表示内部状态异常。
                // 记录后交给后台重试，避免把未确认的槽位当作成功翻译。
                Console.Error.WriteLine("[cache] live-batch admission mismatch (expected="
                    + keys.Count + ", accepted=" + result.Accepted + ")");
                Array.Clear(accepted, 0, accepted.Length);
            }
            return accepted;
        }
    }
}
