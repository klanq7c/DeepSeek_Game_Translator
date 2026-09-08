using System;
using System.Runtime.InteropServices;
using System.Text;

namespace DstLauncher
{
    /*
     * ApiConfigDialog —— api_config.c 中 show_api_config / api_wndproc 的窗口层。
     *
     * 全部是系统绘制的标准控件（COMBOBOX / EDIT / BUTTON），坐标不过 Sc()——
     * 与 C 版一样用固定像素，因此高 DPI 下两版一样偏小。这是移植，不是重设计。
     *
     * C 版通过 CREATESTRUCTW.lpCreateParams 把栈上的 ApiDialog 传进窗口过程；
     * 这里用静态字段，因为对话框是模态的，同一时刻只可能有一个。
     */
    internal static class ApiConfigDialog
    {
        private const string DialogClass = "DSTApiConfigDialogCs";

        private const int IDC_API_PROVIDER = 200;
        private const int IDC_API_ENDPOINT = 201;
        private const int IDC_API_MODEL = 202;
        private const int IDC_API_KEY = 203;
        private const int IDC_API_SAVE = 204;
        private const int IDC_API_CANCEL = 205;

        private static readonly Win32.WndProcDelegate ProcHolder = DialogProc;
        private static bool _registered;

        private static IntPtr _provider, _endpoint, _model, _key;
        private static bool _done;

        private static IntPtr DialogProc(IntPtr hwnd, uint msg, IntPtr wp, IntPtr lp)
        {
            switch (msg) {
                case Win32.WM_CREATE: {
                    /* 从 INI 读取已有配置，若不存在则使用默认值 */
                    ApiConfig.Values v = ApiConfig.Load();

                    /* 提供商下拉：选中预设即填入地址与示例模型，key 保持不动 */
                    Static(hwnd, "提供商", 24, 20, 120, 22);
                    _provider = Win32.CreateWindowExW(0, "COMBOBOX", null,
                        Win32.WS_CHILD | Win32.WS_VISIBLE | Win32.CBS_DROPDOWNLIST | Win32.WS_VSCROLL,
                        24, 46, 320, 300, hwnd, (IntPtr)IDC_API_PROVIDER, MainWindow.Instance, IntPtr.Zero);
                    for (int i = 0; i < ApiConfig.Providers.Length; i++) {
                        Win32.SendMessageW(_provider, Win32.CB_ADDSTRING, IntPtr.Zero, ApiConfig.Providers[i].Name);
                    }
                    /* 按当前 INI 的 endpoint 反查预设预选；无匹配显示"自定义" */
                    Win32.SendMessageW(_provider, Win32.CB_SETCURSEL,
                                       (IntPtr)ApiConfig.PresetIndexFor(v.Endpoint), IntPtr.Zero);

                    /* 标签 + 编辑框 + 按钮 */
                    Static(hwnd, "API 地址", 24, 88, 120, 22);
                    _endpoint = Edit(hwnd, v.Endpoint, 24, 114, 520, 30, IDC_API_ENDPOINT, false);
                    Static(hwnd, "模型", 24, 158, 120, 22);
                    _model = Edit(hwnd, v.Model, 24, 184, 320, 30, IDC_API_MODEL, false);
                    Static(hwnd, "API Key（本地服务可留空）", 24, 228, 300, 22);
                    _key = Edit(hwnd, v.Key, 24, 254, 520, 30, IDC_API_KEY, true);
                    Win32.CreateWindowExW(0, "BUTTON", "保存",
                        Win32.WS_CHILD | Win32.WS_VISIBLE | Win32.BS_DEFPUSHBUTTON,
                        318, 310, 104, 34, hwnd, (IntPtr)IDC_API_SAVE, MainWindow.Instance, IntPtr.Zero);
                    Win32.CreateWindowExW(0, "BUTTON", "取消", Win32.WS_CHILD | Win32.WS_VISIBLE,
                        440, 310, 104, 34, hwnd, (IntPtr)IDC_API_CANCEL, MainWindow.Instance, IntPtr.Zero);

                    /* 应用全局字体 */
                    Win32.SendMessageW(_provider, Win32.WM_SETFONT, Theme.FontBody, (IntPtr)1);
                    Win32.SendMessageW(_endpoint, Win32.WM_SETFONT, Theme.FontBody, (IntPtr)1);
                    Win32.SendMessageW(_model, Win32.WM_SETFONT, Theme.FontBody, (IntPtr)1);
                    Win32.SendMessageW(_key, Win32.WM_SETFONT, Theme.FontBody, (IntPtr)1);
                    return IntPtr.Zero;
                }
                case Win32.WM_COMMAND: {
                    int id = Win32.LOWORD(wp);
                    int notify = Win32.HIWORD(wp);
                    /* 下拉选中预设：自动填充地址与示例模型（仍可手改）；选"自定义"不动 */
                    if (id == IDC_API_PROVIDER && notify == Win32.CBN_SELCHANGE) {
                        int sel = (int)Win32.SendMessageW(_provider, Win32.CB_GETCURSEL, IntPtr.Zero, IntPtr.Zero);
                        if (sel > 0 && sel < ApiConfig.Providers.Length && ApiConfig.Providers[sel].Endpoint != null) {
                            Win32.SetWindowTextW(_endpoint, ApiConfig.Providers[sel].Endpoint);
                            Win32.SetWindowTextW(_model, ApiConfig.Providers[sel].Model);
                        }
                        return IntPtr.Zero;
                    }
                    if (id == IDC_API_SAVE) {
                        /* 将编辑框内容写入 INI 文件 */
                        string endpoint = Text(_endpoint, 1024);
                        string model = Text(_model, 256);
                        string key = Text(_key, 1024);
                        /* 写入失败时提示并保持对话框打开，以便用户修正后重试。 */
                        if (!ApiConfig.Save(endpoint, model, key)) {
                            Log.Status("API 配置保存失败");
                            Win32.MessageBoxW(hwnd, "API 配置写入失败，请检查配置文件目录的写入权限后重试。",
                                              "配置 API", 0x00000030 /* MB_ICONWARNING */);
                            return IntPtr.Zero;
                        }
                        Log.Status("API 配置已保存");
                        _done = true;
                        Win32.DestroyWindow(hwnd);
                        return IntPtr.Zero;
                    }
                    if (id == IDC_API_CANCEL) {
                        _done = true;
                        Win32.DestroyWindow(hwnd);
                        return IntPtr.Zero;
                    }
                    break;
                }
                case Win32.WM_CLOSE:
                    _done = true;
                    Win32.DestroyWindow(hwnd);
                    return IntPtr.Zero;
            }
            return Win32.DefWindowProcW(hwnd, msg, wp, lp);
        }

        private static void Static(IntPtr parent, string text, int x, int y, int w, int h)
        {
            Win32.CreateWindowExW(0, "STATIC", text, Win32.WS_CHILD | Win32.WS_VISIBLE,
                                  x, y, w, h, parent, IntPtr.Zero, MainWindow.Instance, IntPtr.Zero);
        }

        private static IntPtr Edit(IntPtr parent, string text, int x, int y, int w, int h, int id, bool password)
        {
            uint style = Win32.WS_CHILD | Win32.WS_VISIBLE | Win32.ES_AUTOHSCROLL;
            if (password) style |= Win32.ES_PASSWORD;
            return Win32.CreateWindowExW(Win32.WS_EX_CLIENTEDGE, "EDIT", text, style,
                                         x, y, w, h, parent, (IntPtr)id, MainWindow.Instance, IntPtr.Zero);
        }

        private static string Text(IntPtr ctl, int cap)
        {
            var sb = new StringBuilder(cap);
            Win32.GetWindowTextW(ctl, sb, cap);
            return sb.ToString();
        }

        /* 显示 API 配置对话框（模态）：注册窗口类（仅首次），创建窗口并运行局部
           消息循环，直到对话框关闭。期间禁用主窗口。 */
        internal static void Show(IntPtr owner)
        {
            if (!_registered) {
                var wc = new Win32.WNDCLASSEXW();
                wc.cbSize = (uint)Marshal.SizeOf(typeof(Win32.WNDCLASSEXW));
                wc.lpfnWndProc = Marshal.GetFunctionPointerForDelegate(ProcHolder);
                wc.hInstance = MainWindow.Instance;
                wc.hIcon = Win32.LoadImageW(MainWindow.Instance, (IntPtr)Theme.IDI_APP_ICON, Win32.IMAGE_ICON,
                                            Win32.GetSystemMetrics(Win32.SM_CXICON),
                                            Win32.GetSystemMetrics(Win32.SM_CYICON), Win32.LR_SHARED);
                wc.hIconSm = Win32.LoadImageW(MainWindow.Instance, (IntPtr)Theme.IDI_APP_ICON, Win32.IMAGE_ICON,
                                              Win32.GetSystemMetrics(Win32.SM_CXSMICON),
                                              Win32.GetSystemMetrics(Win32.SM_CYSMICON), Win32.LR_SHARED);
                wc.hCursor = Win32.LoadCursorW(IntPtr.Zero, (IntPtr)32512 /* IDC_ARROW */);
                wc.hbrBackground = (IntPtr)(Win32.COLOR_WINDOW + 1);
                wc.lpszClassName = Marshal.StringToHGlobalUni(DialogClass);
                if (Win32.RegisterClassExW(ref wc) == 0) {
                    Log.Append("无法注册 API 配置窗口类（Windows 错误：" + Marshal.GetLastWin32Error() + "）。");
                    return;
                }
                _registered = true;
            }

            _done = false;
            Win32.EnableWindow(owner, false);
            IntPtr dlg = Win32.CreateWindowExW(Win32.WS_EX_DLGMODALFRAME, DialogClass, "配置 API",
                                               Win32.WS_CAPTION | Win32.WS_SYSMENU | Win32.WS_VISIBLE,
                                               Win32.CW_USEDEFAULT, Win32.CW_USEDEFAULT, 590, 400,
                                               owner, IntPtr.Zero, MainWindow.Instance, IntPtr.Zero);
            if (dlg == IntPtr.Zero) {
                Log.Append("无法创建 API 配置窗口（Windows 错误：" + Marshal.GetLastWin32Error() + "）。");
                Win32.EnableWindow(owner, true);
                Win32.SetForegroundWindow(owner);
                return;
            }
            Win32.ShowWindow(dlg, Win32.SW_SHOW);
            Win32.MSG m;
            while (!_done) {
                int got = Win32.GetMessageW(out m, IntPtr.Zero, 0, 0);
                if (got == 0) {
                    Win32.PostQuitMessage((int)m.wParam);
                    break;
                }
                if (got < 0) break;
                if (!Win32.IsDialogMessageW(dlg, ref m)) {
                    Win32.TranslateMessage(ref m);
                    Win32.DispatchMessageW(ref m);
                }
            }
            Win32.EnableWindow(owner, true);
            Win32.SetForegroundWindow(owner);
        }
    }
}
