using System;
using System.Collections.Generic;

internal static class Program
{
    private static int passed;
    private static int failed;

    private static void Check(string name, bool condition)
    {
        Console.WriteLine((condition ? "[PASS] " : "[FAIL] ") + name);
        if (condition) passed++; else failed++;
    }

    private static int Main()
    {
        var items = new int[100];
        for (int i = 0; i < items.Length; i++) items[i] = i;
        var cursor = new VisibleScanWindow();
        var seen = new HashSet<int>();
        bool bounded = true;
        for (int pass = 0; pass < 9; pass++) {
            int visited;
            int applied = cursor.Scan(items, 12, 32, item => { seen.Add(item); return false; }, out visited);
            bounded &= visited == 12 && applied == 0;
        }
        Check("miss-heavy passes inspect at most 12 items each", bounded);
        Check("round-robin passes eventually cover all 100 items", seen.Count == 100);

        cursor.Reset();
        var hits = new List<int>();
        int inspected;
        int count = cursor.Scan(items, 100, 2, item => {
            if (item % 3 != 0) return false;
            hits.Add(item);
            return true;
        }, out inspected);
        Check("successful-apply limit stops after two hits", count == 2 && inspected == 4
            && hits.Count == 2 && hits[0] == 0 && hits[1] == 3);
        hits.Clear();
        count = cursor.Scan(items, 100, 2, item => {
            if (item % 3 != 0) return false;
            hits.Add(item);
            return true;
        }, out inspected);
        Check("next pass resumes after the last inspected item", count == 2 && inspected == 6
            && hits.Count == 2 && hits[0] == 6 && hits[1] == 9);

        cursor.Reset();
        cursor.Scan(items, 97, 100, item => false, out inspected);
        int[] shorter = { 0, 1, 2, 3, 4 };
        var changed = new List<int>();
        cursor.Scan(shorter, 3, 100, item => { changed.Add(item); return false; }, out inspected);
        Check("changed object count keeps cursor in range", changed.Count == 3
            && changed[0] == 2 && changed[1] == 3 && changed[2] == 4);

        cursor.Reset();
        int first = -1;
        cursor.Scan(items, int.MaxValue, int.MaxValue, item => {
            if (first < 0) first = item;
            return false;
        }, out inspected);
        Check("full scene pass starts at the first object", first == 0 && inspected == 100);

        Console.WriteLine("Unity visible scan: PASS=" + passed + " FAIL=" + failed);
        return failed == 0 ? 0 : 1;
    }
}
