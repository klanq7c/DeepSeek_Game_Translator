using System;

/* 深预取只在主线程使用：三类对象各自续扫，并轮换首先扫描的类别，
   防止大量 TextAsset 长期占满预算。窗口只保留整数游标，不持有场景对象。
   预算覆盖已见、空和不活跃条目；Unity 全量发现 API 的成本仍由调用方承担。 */
internal sealed class DeepPrefetchScanWindow
{
    private const int KindCount = 3;
    private readonly int[] next = new int[KindCount];
    private int nextKind;

    public void Reset()
    {
        Array.Clear(next, 0, next.Length);
        nextKind = 0;
    }

    public int Scan<T>(int maxInspect, Func<int, T[]> discover, Func<int, T, bool> inspect)
    {
        int inspected = 0;
        for (int kinds = 0; kinds < KindCount && inspected < maxInspect; kinds++)
        {
            int kind = nextKind;
            nextKind = (kind + 1) % KindCount;
            T[] items = discover(kind);
            if (items.Length == 0)
            {
                next[kind] = 0;
                continue;
            }
            int index = next[kind] % items.Length;
            int limit = Math.Min(items.Length, maxInspect - inspected);
            for (int i = 0; i < limit; i++)
            {
                T item = items[index];
                index = (index + 1) % items.Length;
                next[kind] = index;
                inspected++;
                if (inspect(kind, item)) return inspected;
            }
        }
        return inspected;
    }
}
