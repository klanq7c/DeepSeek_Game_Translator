using System;

/* Unity Mono 可见文本扫描的轮转窗口：限制每轮检查量，同时使缓存未命中对象
   不会把后面的对象永久挡住。调用方仍负责枚举 Unity 对象及记录渲染异常。 */
internal sealed class VisibleScanWindow
{
    private int next;

    public void Reset()
    {
        next = 0;
    }

    public int Scan<T>(T[] items, int maxInspect, int maxApply,
                       Func<T, bool> tryApply, out int inspected)
    {
        inspected = 0;
        if (items.Length == 0 || maxInspect <= 0 || maxApply <= 0) return 0;

        int index = next % items.Length;
        int limit = Math.Min(items.Length, maxInspect);
        int applied = 0;
        while (inspected < limit && applied < maxApply) {
            if (tryApply(items[index])) applied++;
            inspected++;
            index++;
            if (index == items.Length) index = 0;
        }
        next = index;
        return applied;
    }
}
