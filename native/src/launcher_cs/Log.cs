using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Text;

namespace DstLauncher
{
    /*
     * Log —— ui.c 中 append_log / set_status 的接收端抽象。
     *
     * C 版把日志写进主窗口列表框；没有窗口时走 OutputDebugString。C# 版保留
     * 与窗口同样有界的最近记录，接收端仍逐条收到完整日志。诊断子命令直接从
     * 接收端打印，不受历史记录淘汰影响；一致性测试逐行比较 C/C# 的 stdout。
     *
     * 与 C 版一致：一条日志内的 \r \n \t 都替换为空格，保证一行一条。时间戳前缀
     * 属于 UI 展示层，不在这里加（对比时也不需要）。
     */
    public static class Log
    {
        public const int RetainedLineLimit = 2000;
        private static readonly object Gate = new object();
        private static readonly Queue<string> Lines = new Queue<string>(RetainedLineLimit);
        private static Action<string> _sink;
        private static Action<string> _statusSink;

        /* 让窗口和诊断命令实时接收每一行，不依赖有界历史快照。 */
        public static void SetSink(Action<string> sink)
        {
            lock (Gate) _sink = sink;
        }

        /* 状态栏是独立于日志的一行文本（C 版 set_status 不写日志列表）。 */
        public static void SetStatusSink(Action<string> sink)
        {
            lock (Gate) _statusSink = sink;
        }

        public static void Append(string text)
        {
            string line = Sanitize(text ?? "");
            Action<string> sink;
            lock (Gate) {
                if (Lines.Count == RetainedLineLimit) Lines.Dequeue();
                Lines.Enqueue(line);
                sink = _sink;
            }
            Debug.WriteLine(line);
            if (sink != null) sink(line);
        }

        public static string[] Snapshot()
        {
            lock (Gate) return Lines.ToArray();
        }

        public static void Clear()
        {
            lock (Gate) Lines.Clear();
        }

        /* 状态栏文字：C 版 set_status 只更新状态栏控件，不进日志列表。 */
        public static void Status(string text)
        {
            Action<string> sink;
            lock (Gate) sink = _statusSink;
            if (sink != null) sink(text ?? "");
        }

        private static string Sanitize(string s)
        {
            if (s.IndexOfAny(new[] { '\r', '\n', '\t' }) < 0) return s;
            var sb = new StringBuilder(s.Length);
            foreach (char c in s) sb.Append(c == '\r' || c == '\n' || c == '\t' ? ' ' : c);
            return sb.ToString();
        }
    }
}
