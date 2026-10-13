using System.Text;

namespace DstCore
{
    public static class ApiConfigText
    {
        // 启动器与服务器使用同一 UTF-8 字节上限，禁止静默截断或替换代理字符。
        public static bool ModelFits(string model)
        {
            try { return model != null && model.IndexOf('\0') < 0 && new UTF8Encoding(false, true).GetByteCount(model) <= 255; }
            catch (EncoderFallbackException) { return false; }
        }
    }
}
