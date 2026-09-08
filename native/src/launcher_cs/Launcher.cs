using System;
using System.IO;
using System.Reflection;

namespace DstLauncher
{
    /*
     * Launcher —— globals.c 中与 UI 无关的进程级状态。
     *
     * Root：启动器所在目录（C 版 g_root，由 GetModuleFileNameW 去掉文件名得到），
     * config\、payloads\、native\、translation_memory_c.tsv 都从它派生。
     * 诊断子命令允许用 --root 覆盖，以便 tests/launcher_parity 让两版指向同一份 payloads。
     */
    public static class Launcher
    {
        private static string _root;

        public static string Root
        {
            get
            {
                if (_root == null) {
                    string exe = Assembly.GetEntryAssembly().Location;
                    _root = Path.GetDirectoryName(exe) ?? "";
                }
                return _root;
            }
            set { _root = value; }
        }
    }
}
