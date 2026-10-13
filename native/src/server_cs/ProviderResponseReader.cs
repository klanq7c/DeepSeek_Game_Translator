using System;
using System.IO;
using System.Threading;
using System.Threading.Tasks;

namespace DstServerCs
{
    /* 厂商响应按实际字节限量读取；Content-Length 只用于容量预估，错误的
       声明不能改变与 C 服务端相同的接受条件。null 表示超限，空数组表示空响应。 */
    internal static class ProviderResponseReader
    {
        internal const int MaxBytes = 2 * 1024 * 1024;

        internal static async Task<byte[]> ReadAsync(
            Stream stream, long? declaredLength, CancellationToken cancellationToken)
        {
            int capacity = declaredLength.HasValue && declaredLength.Value > 0
                && declaredLength.Value <= MaxBytes ? (int)declaredLength.Value : 0;
            using (var output = new MemoryStream(capacity)) {
                var chunk = new byte[8192];
                for (;;) {
                    int remaining = MaxBytes - (int)output.Length;
                    int request = Math.Min(chunk.Length, remaining + 1);
                    int count = await stream.ReadAsync(
                        chunk, 0, request, cancellationToken).ConfigureAwait(false);
                    if (count == 0) return output.ToArray();
                    if (count > remaining) return null;
                    output.Write(chunk, 0, count);
                }
            }
        }
    }
}
