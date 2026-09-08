using System;
using System.Runtime.InteropServices;
using System.Text;

namespace DstLauncher
{
    /*
     * ApiConfig —— api_config.c 中与窗口无关的部分：提供商预设表、api.ini 的读写。
     *
     * 读写一律走 Windows Profile API（与 C 版同一函数），不自己解析 INI：
     * WritePrivateProfileStringW 对既有文件的编码、换行、其他小节的保留方式有既定
     * 行为（无 UTF-16 BOM 的文件按系统 ANSI 代码页写入），自己实现必然与 C 版分叉，
     * 而两个服务端读的是同一个文件。
     *
     * 对话框本身（窗口类、控件布局、模态消息循环）属于 UI 层，随 ui.c 一起移植。
     */
    public static class ApiConfig
    {
        [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
        private static extern uint GetPrivateProfileStringW(string section, string key, string def,
                                                            StringBuilder outBuf, uint size, string path);

        [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
        [return: MarshalAs(UnmanagedType.Bool)]
        private static extern bool WritePrivateProfileStringW(string section, string key, string value, string path);

        public struct Preset
        {
            public string Name;
            public string Endpoint;   /* null 表示"自定义"，选中后不改动任何字段 */
            public string Model;
        }

        /* 提供商预设表：显示名 / endpoint / 示例模型。仅用于自动填充，不落盘。
           示例模型名可能随平台更新，以各平台当前模型列表为准。 */
        public static readonly Preset[] Providers = {
            new Preset { Name = "自定义", Endpoint = null, Model = null },
            new Preset { Name = "DeepSeek", Endpoint = "https://api.deepseek.com/v1/chat/completions", Model = "deepseek-v4-flash" },
            new Preset { Name = "硅基流动 SiliconFlow", Endpoint = "https://api.siliconflow.cn/v1/chat/completions", Model = "deepseek-ai/DeepSeek-V3" },
            new Preset { Name = "Moonshot Kimi", Endpoint = "https://api.moonshot.cn/v1/chat/completions", Model = "moonshot-v1-8k" },
            new Preset { Name = "智谱 GLM", Endpoint = "https://open.bigmodel.cn/api/paas/v4/chat/completions", Model = "glm-4-flash" },
            new Preset { Name = "阿里百炼 DashScope", Endpoint = "https://dashscope.aliyuncs.com/compatible-mode/v1/chat/completions", Model = "qwen-plus" },
            new Preset { Name = "OpenAI", Endpoint = "https://api.openai.com/v1/chat/completions", Model = "gpt-4o-mini" },
            new Preset { Name = "Claude (OpenAI 兼容)", Endpoint = "https://api.anthropic.com/v1/chat/completions", Model = "claude-sonnet-4-5" },
            new Preset { Name = "Gemini (OpenAI 兼容)", Endpoint = "https://generativelanguage.googleapis.com/v1beta/openai/chat/completions", Model = "gemini-2.5-flash" },
            new Preset { Name = "Ollama (本地, Key 可留空)", Endpoint = "http://127.0.0.1:11434/v1/chat/completions", Model = "qwen2.5:7b" },
            new Preset { Name = "LM Studio (本地, Key 可留空)", Endpoint = "http://127.0.0.1:1234/v1/chat/completions", Model = "local-model" },
        };

        public const string DefaultEndpoint = "https://api.deepseek.com/v1/chat/completions";
        public const string DefaultModel = "deepseek-v4-flash";

        public struct Values
        {
            public string Endpoint;
            public string Model;
            public string Key;
        }

        private static string ReadProfile(string key, string def, int cap, string path)
        {
            var buf = new StringBuilder(cap);
            GetPrivateProfileStringW("api", key, def, buf, (uint)cap, path);
            return buf.ToString();
        }

        /* WM_CREATE 读取的三个值（缓冲区大小与 C 版一致，超长值会被同样截断）。 */
        public static Values Load()
        {
            string cfg = PathUtil.Join(Launcher.Root, "config\\api.ini");
            return new Values {
                Endpoint = ReadProfile("endpoint", DefaultEndpoint, 1024, cfg),
                Model = ReadProfile("model", DefaultModel, 256, cfg),
                Key = ReadProfile("key", "", 1024, cfg)
            };
        }

        /* 按当前 endpoint 反查预设下标；无匹配返回 0（"自定义"）。 */
        public static int PresetIndexFor(string endpoint)
        {
            for (int i = 1; i < Providers.Length; i++) {
                if (Providers[i].Endpoint != null &&
                    string.Equals(endpoint, Providers[i].Endpoint, StringComparison.Ordinal)) {
                    return i;
                }
            }
            return 0;
        }

        /* 保存按钮：三个键都尝试写入，任一失败则记录日志并返回 false（调用方保持
           对话框打开）。ini 结构与服务端契约不变，只有 endpoint/model/key 三个键。 */
        public static bool Save(string endpoint, string model, string key)
        {
            string cfgdir = PathUtil.Join(Launcher.Root, "config");
            SafeFs.EnsureDir(cfgdir);
            string cfg = PathUtil.Join(Launcher.Root, "config\\api.ini");
            bool okEndpoint = WritePrivateProfileStringW("api", "endpoint", endpoint, cfg);
            bool okModel = WritePrivateProfileStringW("api", "model", model, cfg);
            bool okKey = WritePrivateProfileStringW("api", "key", key, cfg);
            if (!okEndpoint || !okModel || !okKey) {
                int err = Marshal.GetLastWin32Error();
                Log.Append("API 配置保存失败：" + cfg + "（Windows 错误：" + err + "）");
                return false;
            }
            Log.Append("API 配置已保存：" + cfg);
            return true;
        }
    }
}
