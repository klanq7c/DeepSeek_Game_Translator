using System;
using System.Runtime.InteropServices;
using System.Text;
using System.Threading.Tasks;

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
        private const int IDC_API_REFRESH = 206;
        private const int IDC_API_MODEL_STATUS = 207;

        private static readonly Win32.WndProcDelegate ProcHolder = DialogProc;
        private static bool _registered;

        private static IntPtr _provider, _endpoint, _model, _key;
        private static bool _done;
        private static IntPtr _refresh, _status;
        private static Task<ApiModels.Result> _request;
        private static string _requestEndpoint, _requestKey;
        private static bool _changed;
        private static bool _probe;
        private static bool _catalogReady;
        private static DateTime _fetchedAt;

        [DllImport("user32.dll", EntryPoint = "SendMessageW", CharSet = CharSet.Unicode)]
        private static extern IntPtr GetComboText(IntPtr hwnd, uint msg, IntPtr index, StringBuilder text);

        private static string ModelItem(int index)
        {
            var text = new StringBuilder(256);
            GetComboText(_model, 0x0148 /* CB_GETLBTEXT */, (IntPtr)index, text);
            return text.ToString();
        }
        private static void ClearCatalog()
        {
            string model = Text(_model, 256);
            _catalogReady = false;
            Win32.SendMessageW(_model, 0x014b /* CB_RESETCONTENT */, IntPtr.Zero, IntPtr.Zero);
            Win32.SetWindowTextW(_model, model);
        }
        private static bool CatalogContains(string model)
        {
            int count = (int)Win32.SendMessageW(_model, 0x0146 /* CB_GETCOUNT */, IntPtr.Zero, IntPtr.Zero);
            for (int i = 0; i < count; i++) if (ModelItem(i) == model) return true;
            return false;
        }
        private static void UpdateModelStatus()
        {
            if (!_catalogReady) return;
            string model = Text(_model, 256);
            string selection = model.Length == 0 ? "请选择聊天模型，也可手动输入。" :
                CatalogContains(model) ? "当前模型在目录中；可自主选择聊天模型。" :
                "当前模型未在目录中；可重选或手动保存。";
            Win32.SetWindowTextW(_status, "已获取 " + (int)Win32.SendMessageW(_model, 0x0146, IntPtr.Zero, IntPtr.Zero) +
                " 个模型（" + _fetchedAt.ToString("HH:mm:ss", System.Globalization.CultureInfo.InvariantCulture) + "）。\n" + selection);
        }
        /* 目录只证明本次账号可见性，不证明别名失效；不替用户切换模型。 */
        private static bool ApplyModelResult(string endpoint, string key, ApiModels.Result r)
        {
            if (_changed || Text(_endpoint, 1024) != endpoint || Text(_key, 1024) != key) return false;
            string model = Text(_model, 256); ClearCatalog();
            foreach (string item in r.Items) {
                if ((long)Win32.SendMessageW(_model, Win32.CB_ADDSTRING, IntPtr.Zero, item) < 0) {
                    Win32.SetWindowTextW(_model, model); ClearCatalog();
                    Log.Append("模型列表：无法填充模型控件，已丢弃不完整目录");
                    Win32.SetWindowTextW(_status, "模型列表显示失败；可重试或手动输入"); return true;
                }
            }
            Win32.SetWindowTextW(_model, model);
            _catalogReady = r.Items.Length > 0; _fetchedAt = DateTime.Now;
            if (_catalogReady) UpdateModelStatus(); else Win32.SetWindowTextW(_status, r.Message);
            return true;
        }
        private static bool CanSave(string endpoint, string model)
        {
            return !string.IsNullOrWhiteSpace(endpoint) && !string.IsNullOrWhiteSpace(model);
        }

        private static void RefreshModels(IntPtr hwnd)
        {
            if (_request != null) return;
            ClearCatalog();
            _requestEndpoint = Text(_endpoint, 1024); _requestKey = Text(_key, 1024);
            string endpoint = _requestEndpoint, key = _requestKey;
            _changed = false;
            _request = Task.Run(() => {
                var r = ApiModels.Fetch(endpoint, key);
                System.Diagnostics.Trace.WriteLine("模型列表：" + r.Message);
                return r;
            });
            _request.ContinueWith(t => System.Diagnostics.Trace.WriteLine(
                "模型列表：后台请求失败（" + t.Exception.GetBaseException().GetType().Name + "）"),
                TaskContinuationOptions.OnlyOnFaulted);
            Win32.EnableWindow(_refresh, false);
            Win32.SetWindowTextW(_status, "正在获取厂商模型列表…");
            if (Win32.SetTimer(hwnd, (UIntPtr)1, 100, IntPtr.Zero) == IntPtr.Zero) {
                Log.Append("模型列表：无法等待请求（Windows 错误 " + Marshal.GetLastWin32Error() + "）");
                Win32.SetWindowTextW(_status, "无法等待请求；可重试或手动输入");
                _request = null; _requestKey = null;
                Win32.EnableWindow(_refresh, true);
            }
        }

        private static IntPtr DialogProc(IntPtr hwnd, uint msg, IntPtr wp, IntPtr lp)
        {
            switch (msg) {
                case Win32.WM_CREATE: {
                    /* 从 INI 读取已有配置，若不存在则使用默认值 */
                    ApiConfig.Values v = ApiConfig.Load();

                    /* 提供商下拉只填地址，模型由用户从当前目录选择，key 保持不动。 */
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
                    _model = Win32.CreateWindowExW(0, "COMBOBOX", null,
                        Win32.WS_CHILD | Win32.WS_VISIBLE | 0x10000 /* WS_TABSTOP */ | 0x0002 /* CBS_DROPDOWN */ | 0x0040 /* CBS_AUTOHSCROLL */ | Win32.WS_VSCROLL,
                        24, 184, 360, 240, hwnd, (IntPtr)IDC_API_MODEL, MainWindow.Instance, IntPtr.Zero);
                    Win32.SendMessageW(_model, 0x0141 /* CB_LIMITTEXT */, (IntPtr)255, IntPtr.Zero);
                    Win32.SetWindowTextW(_model, v.Model);
                    _refresh = Win32.CreateWindowExW(0, "BUTTON", "刷新模型", Win32.WS_CHILD | Win32.WS_VISIBLE | 0x10000,
                        400, 184, 144, 30, hwnd, (IntPtr)IDC_API_REFRESH, MainWindow.Instance, IntPtr.Zero);
                    Static(hwnd, "API Key（本地服务可留空）", 24, 228, 300, 22);
                    _key = Edit(hwnd, v.Key, 24, 254, 520, 30, IDC_API_KEY, true);
                    _status = Win32.CreateWindowExW(0, "STATIC", "填写地址和 Key 后点击刷新；模型也可手动输入", Win32.WS_CHILD | Win32.WS_VISIBLE,
                        24, 300, 520, 56, hwnd, (IntPtr)IDC_API_MODEL_STATUS, MainWindow.Instance, IntPtr.Zero);
                    Win32.CreateWindowExW(0, "BUTTON", "保存",
                        Win32.WS_CHILD | Win32.WS_VISIBLE | Win32.BS_DEFPUSHBUTTON,
                        318, 368, 104, 34, hwnd, (IntPtr)IDC_API_SAVE, MainWindow.Instance, IntPtr.Zero);
                    Win32.CreateWindowExW(0, "BUTTON", "取消", Win32.WS_CHILD | Win32.WS_VISIBLE,
                        440, 368, 104, 34, hwnd, (IntPtr)IDC_API_CANCEL, MainWindow.Instance, IntPtr.Zero);

                    /* 应用全局字体 */
                    Win32.SendMessageW(_provider, Win32.WM_SETFONT, Theme.FontBody, (IntPtr)1);
                    Win32.SendMessageW(_endpoint, Win32.WM_SETFONT, Theme.FontBody, (IntPtr)1);
                    Win32.SendMessageW(_model, Win32.WM_SETFONT, Theme.FontBody, (IntPtr)1);
                    Win32.SendMessageW(_key, Win32.WM_SETFONT, Theme.FontBody, (IntPtr)1);
                    Win32.SendMessageW(_refresh, Win32.WM_SETFONT, Theme.FontBody, (IntPtr)1);
                    Win32.SendMessageW(_status, Win32.WM_SETFONT, Theme.FontBody, (IntPtr)1);
                    Win32.SendMessageW(_endpoint, 0x00c5 /* EM_SETLIMITTEXT */, (IntPtr)1023, IntPtr.Zero);
                    Win32.SendMessageW(_key, 0x00c5, (IntPtr)1023, IntPtr.Zero);
                    if (!_probe && (v.Key.Length != 0 || ApiConfig.PresetIndexFor(v.Endpoint) >= 9)) RefreshModels(hwnd);
                    return IntPtr.Zero;
                }
                case Win32.WM_TIMER:
                    if (_request != null && _request.IsCompleted) {
                        Win32.KillTimer(hwnd, (UIntPtr)1);
                        if (_request.IsFaulted) {
                            Log.Append("模型列表：后台请求失败（" + _request.Exception.GetBaseException().GetType().Name + "）");
                            ApplyModelResult(_requestEndpoint, _requestKey,
                                new ApiModels.Result { Message = "后台请求失败；可重试或手动输入" });
                        } else {
                            ApiModels.Result r = _request.Result;
                            Log.Append("模型列表：" + r.Message);
                            ApplyModelResult(_requestEndpoint, _requestKey, r);
                        }
                        _request = null; _requestKey = null; Win32.EnableWindow(_refresh, true);
                    }
                    return IntPtr.Zero;
                case Win32.WM_COMMAND: {
                    int id = Win32.LOWORD(wp);
                    int notify = Win32.HIWORD(wp);
                    if (id == IDC_API_REFRESH) { RefreshModels(hwnd); return IntPtr.Zero; }
                    if (_status != IntPtr.Zero && (id == IDC_API_ENDPOINT || id == IDC_API_KEY) && notify == 0x0300 /* EN_CHANGE */) {
                        ClearCatalog();
                        _changed = true;
                        Win32.SetWindowTextW(_status, "地址或 Key 已变化；请刷新模型列表");
                    }
                    if (id == IDC_API_MODEL && (notify == 5 /* CBN_EDITCHANGE */ || notify == Win32.CBN_SELCHANGE)) {
                        if (notify == Win32.CBN_SELCHANGE) {
                            int sel = (int)Win32.SendMessageW(_model, Win32.CB_GETCURSEL, IntPtr.Zero, IntPtr.Zero);
                            if (sel >= 0) Win32.SetWindowTextW(_model, ModelItem(sel));
                        }
                        UpdateModelStatus(); return IntPtr.Zero;
                    }
                    /* 换地址时清空模型；同地址或“自定义”保留当前选择。 */
                    if (id == IDC_API_PROVIDER && notify == Win32.CBN_SELCHANGE) {
                        int sel = (int)Win32.SendMessageW(_provider, Win32.CB_GETCURSEL, IntPtr.Zero, IntPtr.Zero);
                        if (sel > 0 && sel < ApiConfig.Providers.Length && ApiConfig.Providers[sel].Endpoint != null) {
                            if (Text(_endpoint, 1024) != ApiConfig.Providers[sel].Endpoint) {
                                Win32.SetWindowTextW(_endpoint, ApiConfig.Providers[sel].Endpoint);
                                Win32.SetWindowTextW(_model, "");
                            }
                        }
                        return IntPtr.Zero;
                    }
                    if (id == IDC_API_SAVE) {
                        /* 将编辑框内容写入 INI 文件 */
                        string endpoint = Text(_endpoint, 1024);
                        string model = Text(_model, 256);
                        string key = Text(_key, 1024);
                        if (!CanSave(endpoint, model)) {
                            Log.Append("API 配置保存被拒绝：地址或模型为空");
                            Win32.MessageBoxW(hwnd, "请填写 API 地址并选择或输入模型。", "配置 API", 0x00000030);
                            return IntPtr.Zero;
                        }
                        /* 写入失败时提示并保持对话框打开，以便用户修正后重试。 */
                        if (!ApiConfig.Save(endpoint, model, key)) {
                            Log.Status("API 配置保存失败");
                            Win32.MessageBoxW(hwnd, "API 配置保存失败。模型名最多 255 个 UTF-8 字节；请检查文件权限并查看启动器日志。",
                                              "配置 API", 0x00000030 /* MB_ICONWARNING */);
                            return IntPtr.Zero;
                        }
                        Log.Status("API 配置已保存；重启翻译服务后生效");
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
                case Win32.WM_DESTROY:
                    Win32.KillTimer(hwnd, (UIntPtr)1);
                    _request = null; _requestKey = null; _status = IntPtr.Zero;
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
        internal static void Show(IntPtr owner, Action<string> report = null)
        {
            _probe = report != null;
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

            _done = false; _catalogReady = false; _changed = false;
            Win32.EnableWindow(owner, false);
            IntPtr dlg = Win32.CreateWindowExW(Win32.WS_EX_DLGMODALFRAME, DialogClass, "配置 API",
                                               (uint)(Win32.WS_CAPTION | Win32.WS_SYSMENU | (_probe ? 0 : Win32.WS_VISIBLE)),
                                               Win32.CW_USEDEFAULT, Win32.CW_USEDEFAULT, 590, 460,
                                               owner, IntPtr.Zero, MainWindow.Instance, IntPtr.Zero);
            if (dlg == IntPtr.Zero) {
                Log.Append("无法创建 API 配置窗口（Windows 错误：" + Marshal.GetLastWin32Error() + "）。");
                Win32.EnableWindow(owner, true);
                Win32.SetForegroundWindow(owner);
                return;
            }
            if (_probe) {
                for (int id = IDC_API_PROVIDER; id <= IDC_API_MODEL_STATUS; id++) {
                    IntPtr ctl = Win32.GetDlgItem(dlg, id); Win32.RECT rect;
                    Win32.GetWindowRect(ctl, out rect); Win32.MapWindowPoints(IntPtr.Zero, dlg, ref rect, 2);
                    report("control=" + id + "," + rect.left + "," + rect.top + "," + (rect.right - rect.left) + "," + (rect.bottom - rect.top) + "\n");
                }
                Win32.SendMessageW(_model, Win32.CB_ADDSTRING, IntPtr.Zero, "catalog-new-model");
                Win32.SendMessageW(_model, Win32.CB_SETCURSEL, IntPtr.Zero, IntPtr.Zero);
                report("select=" + (Text(_model, 256) == "catalog-new-model" ? 1 : 0) + "\n");
                Win32.SetWindowTextW(_model, "manual-model");
                report("manual=" + (Text(_model, 256) == "manual-model" ? 1 : 0) + "\n");
                /* 使用真实控件和结果接收边界验证选择策略，不联网、不保存配置。 */
                Action<string, bool> check = (name, ok) => report(name + "=" + (ok ? 1 : 0) + "\n");
                Action<int> pick = index => {
                    Win32.SendMessageW(_provider, Win32.CB_SETCURSEL, (IntPtr)index, IntPtr.Zero);
                    DialogProc(dlg, Win32.WM_COMMAND, (IntPtr)(IDC_API_PROVIDER | (Win32.CBN_SELCHANGE << 16)), _provider);
                };
                var result = new ApiModels.Result { Items = new[] { "catalog-future-release", "catalog-other" }, Message = "fixture" };
                string endpoint = ApiConfig.Providers[1].Endpoint;
                Win32.SetWindowTextW(_endpoint, endpoint); Win32.SetWindowTextW(_key, ""); _changed = false;
                ApplyModelResult(endpoint, "", result);
                check("catalog_preserves", Text(_model, 256) == "manual-model");
                check("catalog_new_id", CatalogContains(result.Items[0]));
                check("catalog_unlisted", Text(_status, 256).Contains("当前模型未在目录中") && CanSave(endpoint, "manual-model"));
                pick(1); check("provider_same_keeps", Text(_model, 256) == "manual-model" && _catalogReady);
                pick(2); check("provider_change_clears", Text(_model, 256) == "" && !_catalogReady &&
                    Win32.SendMessageW(_model, 0x0146, IntPtr.Zero, IntPtr.Zero) == IntPtr.Zero);
                Win32.SetWindowTextW(_model, "manual-custom"); pick(0);
                check("provider_custom_keeps", Text(_model, 256) == "manual-custom");
                check("empty_model_rejected", !CanSave("url", "") && !CanSave("url", " \t\u3000") && !CanSave(" ", "model"));
                Win32.SetWindowTextW(_endpoint, endpoint); _changed = false;
                Win32.SetWindowTextW(_key, "changed"); Win32.SetWindowTextW(_key, "");
                check("stale_catalog_ignored", !ApplyModelResult(endpoint, "", result) && !_catalogReady);
                bool allClear = true;
                for (int i = 1; i < ApiConfig.Providers.Length; i++) {
                    Win32.SetWindowTextW(_endpoint, "https://custom.invalid/v1/chat/completions");
                    Win32.SetWindowTextW(_model, "old-provider-model"); pick(i);
                    if (Text(_model, 256) != "") allClear = false;
                }
                check("all_providers_clear", allClear);
                Win32.SetWindowTextW(_endpoint, endpoint); _changed = false;
                ApplyModelResult(endpoint, "", result);
                Win32.SendMessageW(_model, Win32.CB_SETCURSEL, IntPtr.Zero, IntPtr.Zero);
                DialogProc(dlg, Win32.WM_COMMAND, (IntPtr)(IDC_API_MODEL | (Win32.CBN_SELCHANGE << 16)), _model);
                check("catalog_listed", Text(_status, 256).Contains("当前模型在目录中"));
                Win32.SetWindowTextW(_model, "CATALOG-FUTURE-RELEASE");
                DialogProc(dlg, Win32.WM_COMMAND, (IntPtr)(IDC_API_MODEL | (5 << 16)), _model);
                check("catalog_case_sensitive", Text(_status, 256).Contains("当前模型未在目录中"));
                result = new ApiModels.Result { Message = "fixture HTTP 401" };
                ApplyModelResult(endpoint, "", result);
                check("catalog_failure_keeps", Text(_model, 256) == "CATALOG-FUTURE-RELEASE" && !_catalogReady && Text(_status, 256) == result.Message);
                Win32.DestroyWindow(dlg); return;
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
