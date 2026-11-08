namespace DstCore
{
    // 收包缓冲只会追加或扩容。游标保留尚不足四字节的后缀，
    // 跨分片的 CRLFCRLF 因此不会漏掉，也不需要解码完整前缀。
    internal static class HttpRequestFraming
    {
        internal static int FindHeaderEnd(byte[] buffer, int length, ref int cursor)
        {
            int i = cursor;
            while (i + 3 < length)
            {
                if (buffer[i] == 13 && buffer[i + 1] == 10 &&
                    buffer[i + 2] == 13 && buffer[i + 3] == 10)
                {
                    cursor = i;
                    return i + 4;
                }
                i++;
            }
            cursor = i;
            return -1;
        }
    }
}
