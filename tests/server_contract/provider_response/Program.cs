using System;
using System.IO;
using System.Text;
using System.Threading;
using System.Threading.Tasks;
using DstServerCs;

internal static class Program
{
    private static int passed;
    private static int failed;

    private static void Check(string name, bool ok)
    {
        Console.WriteLine((ok ? "[PASS] " : "[FAIL] ") + name);
        if (ok) passed++; else failed++;
    }

    private sealed class PatternStream : Stream
    {
        private long remaining;
        internal long ReadBytes;
        internal int LargestRequest;

        internal PatternStream(long length) { remaining = length; }
        public override bool CanRead { get { return true; } }
        public override bool CanSeek { get { return false; } }
        public override bool CanWrite { get { return false; } }
        public override long Length { get { throw new NotSupportedException(); } }
        public override long Position {
            get { throw new NotSupportedException(); }
            set { throw new NotSupportedException(); }
        }
        public override int Read(byte[] buffer, int offset, int count)
        {
            LargestRequest = Math.Max(LargestRequest, count);
            int n = (int)Math.Min(remaining, count);
            for (int i = 0; i < n; i++) buffer[offset + i] = (byte)'A';
            remaining -= n;
            ReadBytes += n;
            return n;
        }
        public override Task<int> ReadAsync(byte[] buffer, int offset, int count,
                                             CancellationToken cancellationToken)
        {
            if (cancellationToken.IsCancellationRequested)
                return Task.FromCanceled<int>(cancellationToken);
            return Task.FromResult(Read(buffer, offset, count));
        }
        public override void Flush() { throw new NotSupportedException(); }
        public override long Seek(long offset, SeekOrigin origin) { throw new NotSupportedException(); }
        public override void SetLength(long value) { throw new NotSupportedException(); }
        public override void Write(byte[] buffer, int offset, int count) { throw new NotSupportedException(); }
    }

    private static async Task Run()
    {
        int limit = ProviderResponseReader.MaxBytes;
        var knownHuge = new PatternStream(limit + 1L);
        byte[] result = await ProviderResponseReader.ReadAsync(
            knownHuge, limit + 1L, CancellationToken.None);
        Check("declared oversize is rejected by actual byte count", result == null && knownHuge.ReadBytes == limit + 1L);

        using (var shortBody = new MemoryStream(Encoding.UTF8.GetBytes("{\"ok\":true}"))) {
            result = await ProviderResponseReader.ReadAsync(
                shortBody, limit + 1L, CancellationToken.None);
        }
        Check("inflated length header cannot reject a short valid body",
            result != null && Encoding.UTF8.GetString(result) == "{\"ok\":true}");

        var unknownHuge = new PatternStream(limit + 1024L);
        result = await ProviderResponseReader.ReadAsync(
            unknownHuge, null, CancellationToken.None);
        Check("unknown length reads only limit plus one", result == null && unknownHuge.ReadBytes == limit + 1L);
        Check("each read request remains bounded", unknownHuge.LargestRequest <= 8192);

        var dishonestLength = new PatternStream(limit + 1L);
        result = await ProviderResponseReader.ReadAsync(
            dishonestLength, 3, CancellationToken.None);
        Check("short declared length cannot bypass stream limit", result == null && dishonestLength.ReadBytes == limit + 1L);

        var exactLimit = new PatternStream(limit);
        result = await ProviderResponseReader.ReadAsync(
            exactLimit, limit, CancellationToken.None);
        Check("exact byte limit is accepted", result != null && result.Length == limit && exactLimit.ReadBytes == limit);

        byte[] unicode = Encoding.UTF8.GetBytes("{\"content\":\"你好 [player] 🌟\"}");
        using (var stream = new MemoryStream(unicode)) {
            result = await ProviderResponseReader.ReadAsync(
                stream, unicode.Length, CancellationToken.None);
        }
        Check("UTF-8 JSON bytes are preserved", result != null
            && Encoding.UTF8.GetString(result) == Encoding.UTF8.GetString(unicode));

        using (var stream = new MemoryStream()) {
            result = await ProviderResponseReader.ReadAsync(stream, null, CancellationToken.None);
        }
        Check("empty response remains empty", result != null && result.Length == 0);

        var canceled = new CancellationTokenSource();
        canceled.Cancel();
        bool sawCancel = false;
        try {
            await ProviderResponseReader.ReadAsync(
                new PatternStream(10), null, canceled.Token);
        } catch (OperationCanceledException) {
            sawCancel = true;
        }
        Check("read cancellation is propagated", sawCancel);
    }

    private static int Main()
    {
        Run().GetAwaiter().GetResult();
        Console.WriteLine("Provider response reader: PASS=" + passed + " FAIL=" + failed);
        return failed == 0 ? 0 : 1;
    }
}
