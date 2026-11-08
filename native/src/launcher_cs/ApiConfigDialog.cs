using System;
using System.Runtime.InteropServices;
using System.Text;
using System.Threading.Tasks;

namespace DstLauncher
{
    /*
     * ApiConfigDialog —— api_config.c 中 show_api_config / api_wndproc 的窗口层。
     *
     * 原生输入和下拉控件保留输入法与键盘支持，绘制与布局镜像 C 版星夜主题。
     * 字体和 DPI 由窗口独立持有，跨屏移动不会改变主窗口。
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
        private const int IDC_API_TEST = 212;

        private static readonly Win32.WndProcDelegate ProcHolder = DialogProc;
        private static bool _registered;

        private static DialogTheme _theme;
        private static IntPtr _provider, _endpoint, _model, _key;
        private static bool _done;
        private static IntPtr _refresh, _status;
        private static IntPtr _test, _testStatus;
        private static Task<ApiConnection.Result> _testRequest;
        private static string _testEndpoint, _testModel, _testKey;
        private static bool _testChanged;
        private static Task<ApiModels.Result> _request;
        private static string _requestEndpoint, _requestKey;
        private static bool _changed;
        private static bool _probe;
        private static bool _catalogReady;
        private static ApiModels.Result _catalog = new ApiModels.Result();
        // Key 仅按提供商保存在本次窗口中；关闭即释放引用，取消不写配置。
        private static readonly System.Collections.Generic.Dictionary<int, string> ProviderKeys = new System.Collections.Generic.Dictionary<int, string>();
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
            _catalog = new ApiModels.Result();
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
                CatalogContains(model) ? "当前模型在目录中：" + ModelLabel(model) :
                "当前模型未在目录中；展开下拉列表选择新模型。";
            string summary = "已获取 " + (int)Win32.SendMessageW(_model, 0x0146, IntPtr.Zero, IntPtr.Zero) +
                " 个模型（" + _fetchedAt.ToString("HH:mm:ss", System.Globalization.CultureInfo.InvariantCulture) + "）。";
            if (_catalog.InvalidNames > 0) summary = "已获取 " + _catalog.Items.Length + " 个模型；" + _catalog.InvalidNames + " 个显示名称无效，已使用模型 ID。";
            Win32.SetWindowTextW(_status, summary + "\n" + selection);
        }
        // 控件保留 API ID；显示名称仅用于绘制和说明，选择或保存不改变调用标识。
        private static string ModelLabel(string id)
        {
            for (int i = 0; i < _catalog.Items.Length; i++)
                if (_catalog.Items[i] == id && _catalog.Names[i].Length > 0 && _catalog.Names[i] != id)
                    return _catalog.Names[i] + "  (" + id + ")";
            return id;
        }
        /* 目录只证明本次账号可见性，不证明别名失效；不替用户切换模型。 */
        private static bool ApplyModelResult(string endpoint, string key, ApiModels.Result r)
        {
            if (_changed || Text(_endpoint, 1024) != endpoint || Text(_key, 1024) != key) return false;
            string model = Text(_model, 256); ClearCatalog();
            _catalog = r;
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
            int preset = ApiConfig.PresetIndexFor(Text(_endpoint, 1024));
            if (Text(_key, 1024).Length == 0 && preset > 0 && preset < 9) {
                ClearCatalog();
                Log.Append("模型列表：当前提供商缺少 API Key，未发送请求");
                Win32.SetWindowTextW(_status, "请先填写当前提供商的 API Key，再刷新模型；各厂商 Key 不通用。"); return;
            }
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

        private static void InvalidateConnection()
        {
            _testChanged = true;
            if (_testStatus != IntPtr.Zero) Win32.SetWindowTextW(_testStatus, "配置已变化，请重新检测。\n检测会发送短请求，可能产生少量费用。");
        }
        private static bool ApplyConnectionResult(string endpoint, string model, string key, ApiConnection.Result result)
        {
            if (_testChanged || Text(_endpoint, 1024) != endpoint || Text(_model, 256) != model || Text(_key, 1024) != key) return false;
            Win32.SetWindowTextW(_testStatus, result.Message); return true;
        }
        private static void TestConnection(IntPtr hwnd)
        {
            if (_testRequest != null) return;
            string endpoint = Text(_endpoint, 1024), model = Text(_model, 256), key = Text(_key, 1024);
            int preset = ApiConfig.PresetIndexFor(endpoint);
            bool missing = key.Length == 0 && preset > 0 && preset < 9;
            if (!CanSave(endpoint, model) || missing) {
                string message = missing ? "请先填写当前提供商的 API Key，再检测连通。" : "请先填写 API 地址和模型 ID，再检测连通。";
                Log.Append("检测连通：" + message); Win32.SetWindowTextW(_testStatus, message); return;
            }
            // 先建立轮询，失败时不发送无法跟踪的收费请求。工作线程只持有输入快照。
            if (Win32.SetTimer(hwnd, (UIntPtr)2, 100, IntPtr.Zero) == IntPtr.Zero) {
                Log.Append("检测连通：无法等待请求（Windows 错误 " + Marshal.GetLastWin32Error() + "）");
                Win32.SetWindowTextW(_testStatus, "无法等待检测；请重试"); return;
            }
            _testEndpoint = endpoint; _testModel = model; _testKey = key; _testChanged = false;
            _testRequest = Task.Run(() => {
                var r = ApiConnection.Test(endpoint, model, key);
                System.Diagnostics.Trace.WriteLine("检测连通：" + r.Message); return r;
            });
            _testRequest.ContinueWith(t => System.Diagnostics.Trace.WriteLine(
                "检测连通：后台请求失败（" + t.Exception.GetBaseException().GetType().Name + "）"), TaskContinuationOptions.OnlyOnFaulted);
            Win32.EnableWindow(_test, false); Win32.SetWindowTextW(_test, "检测中…");
            Win32.SetWindowTextW(_testStatus, ApiConnection.WaitMilliseconds(endpoint) > 15000 ?
                "正在请求本地模型…\n首次加载可能较慢，响应等待上限 120 秒。" : "正在请求当前模型…\n可继续编辑或关闭窗口。");
        }

        private sealed class ApiLayout {
            internal int Id, X, Y, W, H; internal string Cls, Text; internal uint Style;
            internal ApiLayout(int id, string cls, string text, uint style, int x, int y, int w, int h) { Id=id; Cls=cls; Text=text; Style=style; X=x; Y=y; W=w; H=h; }
        }
        private static readonly ApiLayout[] Layout = {
            new ApiLayout(208, "STATIC", "提供商", Win32.SS_NOPREFIX, 44, 138, 532, 22),
            new ApiLayout(200, "COMBOBOX", "", 0x10000 | Win32.CBS_DROPDOWNLIST | 0x210 | Win32.WS_VSCROLL, 44, 164, 532, 280),
            new ApiLayout(209, "STATIC", "API 地址", Win32.SS_NOPREFIX, 44, 208, 532, 22),
            new ApiLayout(201, "EDIT", "", 0x10000 | Win32.ES_AUTOHSCROLL, 56, 242, 508, 24),
            new ApiLayout(210, "STATIC", "模型 · 支持手动输入", Win32.SS_NOPREFIX, 44, 280, 532, 22),
            new ApiLayout(202, "COMBOBOX", "", 0x10000 | 2 | 0x40 | 0x210 | Win32.WS_VSCROLL, 44, 306, 386, 240),
            new ApiLayout(206, "BUTTON", "刷新模型", 0x10000 | Win32.BS_OWNERDRAW, 442, 304, 134, 36),
            new ApiLayout(211, "STATIC", "API Key · 本地服务可留空", Win32.SS_NOPREFIX, 44, 350, 532, 22),
            new ApiLayout(203, "EDIT", "", 0x10000 | Win32.ES_AUTOHSCROLL | Win32.ES_PASSWORD, 56, 384, 508, 24),
            new ApiLayout(207, "STATIC", "", Win32.SS_NOPREFIX, 52, 446, 516, 58),
            new ApiLayout(212, "BUTTON", "检测连通", 0x10000 | Win32.BS_OWNERDRAW, 44, 546, 134, 36),
            new ApiLayout(213, "STATIC", "", Win32.SS_NOPREFIX, 196, 540, 380, 58),
            new ApiLayout(205, "BUTTON", "取消", 0x10000 | Win32.BS_OWNERDRAW, 352, 648, 100, 38),
            new ApiLayout(204, "BUTTON", "保存配置", 0x10000 | Win32.BS_OWNERDRAW, 464, 648, 128, 38),
        };
        private static void LayoutControls(IntPtr hwnd) {
            foreach (var a in Layout) {
                IntPtr ctl = Win32.GetDlgItem(hwnd, a.Id); Win32.SendMessageW(ctl, Win32.WM_SETFONT, _theme.Body, IntPtr.Zero);
                Win32.SetWindowPos(ctl, IntPtr.Zero, _theme.Sc(a.X), _theme.Sc(a.Y), _theme.Sc(a.W), _theme.Sc(a.H), Win32.SWP_NOZORDER | Win32.SWP_NOACTIVATE);
            }
            foreach (var combo in new[] { _provider, _model }) {
                Win32.SendMessageW(combo, 0x153, (IntPtr)(-1), (IntPtr)_theme.Sc(24));
                Win32.SendMessageW(combo, 0x153, IntPtr.Zero, (IntPtr)_theme.Sc(26));
            }
            Win32.SendMessageW(_model, 0x160 /* CB_SETDROPPEDWIDTH */, (IntPtr)_theme.Sc(532), IntPtr.Zero);
        }
        private static void Paint(IntPtr hwnd, IntPtr dc) {
            _theme.Paint(hwnd, dc, "配置 API", "连接翻译服务，让故事继续。 ");
            int[] ys = {234, 376}; IntPtr[] edits = {_endpoint, _key};
            for (int i=0; i<2; i++) UiPaint.DrawRound(dc, _theme.Rect(44, ys[i], 532, 38), Theme.C_LOG,
                DialogTheme.GetFocus() == edits[i] ? Theme.C_ACCENT_DEEP : Theme.C_LINE, _theme.Sc(10));
            UiPaint.DrawRound(dc, _theme.Rect(36, 430, 548, 88), Theme.C_LOG, Theme.C_LINE, _theme.Sc(12));
            UiPaint.DrawRound(dc, _theme.Rect(36, 524, 548, 88), Theme.C_LOG, Theme.C_LINE, _theme.Sc(12));
            _theme.Text(dc, "保存后，重启翻译服务生效。", 28, 646, 312, 46, _theme.Small, Theme.C_TEXT_DIM);
        }

        private static IntPtr DialogProc(IntPtr hwnd, uint msg, IntPtr wp, IntPtr lp)
        {
            switch (msg) {
                case Win32.WM_CREATE: {
                    /* 从 INI 读取已有配置，若不存在则使用默认值 */
                    ApiConfig.Values v = ApiConfig.Load();

                    foreach (var a in Layout) _theme.Control(hwnd, a.Cls, a.Text, a.Style, a.Id, a.X, a.Y, a.W, a.H);
                    _provider = Win32.GetDlgItem(hwnd, 200); _endpoint = Win32.GetDlgItem(hwnd, 201);
                    _model = Win32.GetDlgItem(hwnd, 202); _key = Win32.GetDlgItem(hwnd, 203);
                    _refresh = Win32.GetDlgItem(hwnd, 206); _status = Win32.GetDlgItem(hwnd, 207);
                    _test = Win32.GetDlgItem(hwnd, 212); _testStatus = Win32.GetDlgItem(hwnd, 213);
                    LayoutControls(hwnd);
                    foreach (var provider in ApiConfig.Providers) Win32.SendMessageW(_provider, Win32.CB_ADDSTRING, IntPtr.Zero, provider.Name);
                    Win32.SendMessageW(_provider, Win32.CB_SETCURSEL, (IntPtr)ApiConfig.PresetIndexFor(v.Endpoint), IntPtr.Zero);
                    Win32.SendMessageW(_model, 0x0141, (IntPtr)255, IntPtr.Zero);
                    Win32.SetWindowTextW(_endpoint, v.Endpoint); Win32.SetWindowTextW(_model, v.Model); Win32.SetWindowTextW(_key, v.Key);
                    Win32.SetWindowTextW(_status, "填写地址和 Key 后点击刷新；模型也可手动输入");
                    Win32.SetWindowTextW(_testStatus, "发送一次短请求，可能产生少量费用。\n检测当前草稿，不会自动保存。");
                    Win32.SendMessageW(_endpoint, 0x00c5 /* EM_SETLIMITTEXT */, (IntPtr)1023, IntPtr.Zero);
                    Win32.SendMessageW(_key, 0x00c5, (IntPtr)1023, IntPtr.Zero);
                    if (!_probe && (v.Key.Length != 0 || ApiConfig.PresetIndexFor(v.Endpoint) >= 9)) RefreshModels(hwnd);
                    return IntPtr.Zero;
                }
                case Win32.WM_ERASEBKGND: return (IntPtr)1;
                case Win32.WM_PAINT: {
                    Win32.PAINTSTRUCT ps; IntPtr dc = Win32.BeginPaint(hwnd, out ps); Paint(hwnd, dc); Win32.EndPaint(hwnd, ref ps); return IntPtr.Zero;
                }
                case Win32.WM_PRINTCLIENT: Paint(hwnd, wp); return IntPtr.Zero;
                case Win32.WM_CTLCOLORSTATIC: return _theme.Color(wp, lp == _status || lp == _testStatus);
                case Win32.WM_CTLCOLOREDIT:
                case Win32.WM_CTLCOLORLISTBOX: return _theme.Color(wp, true);
                case 0x2c: Marshal.WriteInt32(lp, 16, _theme.Sc(26)); return (IntPtr)1;
                case Win32.WM_DRAWITEM: {
                    var di = (Win32.DRAWITEMSTRUCT)Marshal.PtrToStructure(lp, typeof(Win32.DRAWITEMSTRUCT));
                    string label = di.CtlID == IDC_API_MODEL && di.itemID != uint.MaxValue ? ModelLabel(ModelItem((int)di.itemID)) : null;
                    if (di.CtlType == 3) _theme.Combo(ref di, label); else _theme.Button(ref di, di.CtlID == IDC_API_SAVE); return (IntPtr)1;
                }
                case Win32.WM_DPICHANGED: {
                    var r = (Win32.RECT)Marshal.PtrToStructure(lp, typeof(Win32.RECT));
                    Win32.SetWindowPos(hwnd, IntPtr.Zero, r.left, r.top, r.right-r.left, r.bottom-r.top, Win32.SWP_NOZORDER | Win32.SWP_NOACTIVATE);
                    _theme.Dispose(); _theme = new DialogTheme(_probe ? Win32.HIWORD(wp) : DialogTheme.FitDpi(hwnd, 620, 706, Win32.HIWORD(wp))); LayoutControls(hwnd);
                    _theme.Place(hwnd, hwnd, 620, 706); Win32.InvalidateRect(hwnd, IntPtr.Zero, true); return IntPtr.Zero;
                }
                case Win32.WM_TIMER:
                    if (_testRequest != null && _testRequest.IsCompleted) {
                        Win32.KillTimer(hwnd, (UIntPtr)2);
                        ApiConnection.Result r;
                        if (_testRequest.IsFaulted) {
                            Log.Append("检测连通：后台请求失败（" + _testRequest.Exception.GetBaseException().GetType().Name + "）");
                            r = new ApiConnection.Result { Message = "后台检测失败；请重试" };
                        } else r = _testRequest.Result;
                        Log.Append("检测连通：" + r.Message);
                        ApplyConnectionResult(_testEndpoint, _testModel, _testKey, r);
                        _testRequest = null; _testKey = null; Win32.EnableWindow(_test, true); Win32.SetWindowTextW(_test, "检测连通");
                    }
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
                    if (id == IDC_API_TEST) { TestConnection(hwnd); return IntPtr.Zero; }
                    if (id == IDC_API_REFRESH) { RefreshModels(hwnd); return IntPtr.Zero; }
                    if (_status != IntPtr.Zero && (id == IDC_API_ENDPOINT || id == IDC_API_KEY) && notify == 0x0300 /* EN_CHANGE */) {
                        InvalidateConnection();
                        ClearCatalog();
                        _changed = true;
                        Win32.SetWindowTextW(_status, "地址或 Key 已变化；请刷新模型列表");
                    }
                    if (id == IDC_API_MODEL && (notify == 5 /* CBN_EDITCHANGE */ || notify == Win32.CBN_SELCHANGE)) {
                        InvalidateConnection();
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
                                int previous = ApiConfig.PresetIndexFor(Text(_endpoint, 1024));
                                if (previous > 0) ProviderKeys[previous] = Text(_key, 1024);
                                Win32.SetWindowTextW(_endpoint, ApiConfig.Providers[sel].Endpoint);
                                Win32.SetWindowTextW(_model, "");
                                string key;
                                Win32.SetWindowTextW(_key, ProviderKeys.TryGetValue(sel, out key) ? key : "");
                                Win32.SetWindowTextW(_status, "已切换提供商；请确认对应的 API Key 后刷新模型。各厂商 Key 不通用。");
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
                            ThemedMessage.Show(hwnd, "请填写 API 地址并选择或输入模型。", "配置 API", 0x00000030);
                            return IntPtr.Zero;
                        }
                        /* 写入失败时提示并保持对话框打开，以便用户修正后重试。 */
                        if (!ApiConfig.Save(endpoint, model, key)) {
                            Log.Status("API 配置保存失败");
                            ThemedMessage.Show(hwnd, "API 配置保存失败。模型名最多 255 个 UTF-8 字节；请检查文件权限并查看启动器日志。",
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
                    Win32.KillTimer(hwnd, (UIntPtr)2);
                    _testRequest = null; _testKey = null; _testStatus = IntPtr.Zero;
                    Win32.KillTimer(hwnd, (UIntPtr)1);
                    _request = null; _requestKey = null; _status = IntPtr.Zero;
                    _catalog = new ApiModels.Result();
                    ProviderKeys.Clear();
                    return IntPtr.Zero;
            }
            return Win32.DefWindowProcW(hwnd, msg, wp, lp);
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
                wc.hbrBackground = IntPtr.Zero;
                wc.lpszClassName = Marshal.StringToHGlobalUni(DialogClass);
                bool registered = Win32.RegisterClassExW(ref wc) != 0;
                Marshal.FreeHGlobal(wc.lpszClassName);
                if (!registered) {
                    Log.Append("无法注册 API 配置窗口类（Windows 错误：" + Marshal.GetLastWin32Error() + "）。");
                    return;
                }
                _registered = true;
            }

            _done = false; _catalogReady = false; _changed = false; ProviderKeys.Clear();
            int dpi = _probe ? 96 : Theme.ScaleDpi, parsed;
            if (_probe && int.TryParse(Environment.GetEnvironmentVariable("DST_API_UI_DPI"), out parsed) && parsed >= 96 && parsed <= 240) dpi = parsed;
            if (!_probe) dpi = DialogTheme.FitDpi(owner, 620, 706, dpi);
            _theme = new DialogTheme(dpi);
            bool enabled = owner != IntPtr.Zero && DialogTheme.IsWindowEnabled(owner);
            if (enabled) Win32.EnableWindow(owner, false);
            IntPtr dlg = Win32.CreateWindowExW(Win32.WS_EX_DLGMODALFRAME, DialogClass, "配置 API",
                                               (uint)(Win32.WS_CAPTION | Win32.WS_SYSMENU | Win32.WS_CLIPCHILDREN),
                                               Win32.CW_USEDEFAULT, Win32.CW_USEDEFAULT, 590, 460,
                                               owner, IntPtr.Zero, MainWindow.Instance, IntPtr.Zero);
            if (dlg == IntPtr.Zero) {
                Log.Append("无法创建 API 配置窗口（Windows 错误：" + Marshal.GetLastWin32Error() + "）。");
                if (enabled) { Win32.EnableWindow(owner, true); Win32.SetForegroundWindow(owner); }
                _theme.Dispose(); return;
            }
            _theme.Place(dlg, owner, 620, 706); MainWindow.ApplyWindowChrome(dlg);
            if (_probe) {
                bool bounds = true, fonts = true, tabs = true; Win32.RECT client; Win32.GetClientRect(dlg, out client);
                foreach (var a in Layout) {
                    IntPtr ctl = Win32.GetDlgItem(dlg, a.Id); Win32.RECT r;
                    Win32.GetWindowRect(ctl, out r); Win32.MapWindowPoints(IntPtr.Zero, dlg, ref r, 2);
                    if (r.left < 0 || r.top < 0 || r.right > client.right || r.bottom > client.bottom) bounds = false;
                    if (Win32.SendMessageW(ctl, 0x31, IntPtr.Zero, IntPtr.Zero) != _theme.Body) fonts = false;
                }
                IntPtr previous = IntPtr.Zero;
                foreach (int id in new[] {200, 201, 202, 206, 203, 212, 205, 204, 200}) {
                    previous = DialogTheme.GetNextDlgTabItem(dlg, previous, false); if (DialogTheme.ControlId(previous) != id) tabs = false;
                }
                report("bounds=" + (bounds ? 1 : 0) + "\nfonts=" + (fonts ? 1 : 0) + "\ntabs=" + (tabs ? 1 : 0) + "\npassword=" + ((DialogTheme.GetStyle(_key, -16) & Win32.ES_PASSWORD) != 0 ? 1 : 0) + "\n");
                string bmp = Environment.GetEnvironmentVariable("DST_API_UI_BMP");
                if (!string.IsNullOrEmpty(bmp)) report("render=" + (DialogTheme.RenderProbe(dlg, bmp) ? 1 : 0) + "\n");
                for (int id = IDC_API_PROVIDER; id <= 213; id++) {
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
                var result = new ApiModels.Result { Items = new[] { "catalog-future-release", "catalog-other" }, Names = new[] { "Future Model 2099", "" }, Message = "fixture" };
                string endpoint = ApiConfig.Providers[1].Endpoint;
                Win32.SetWindowTextW(_endpoint, endpoint); Win32.SetWindowTextW(_key, ""); _changed = false;
                ApplyModelResult(endpoint, "", result);
                check("catalog_preserves", Text(_model, 256) == "manual-model");
                check("catalog_new_id", CatalogContains(result.Items[0]));
                check("catalog_display_name", ModelLabel(result.Items[0]) == "Future Model 2099  (catalog-future-release)");
                check("catalog_unlisted", Text(_status, 256).Contains("当前模型未在目录中") && CanSave(endpoint, "manual-model"));
                pick(1); check("provider_same_keeps", Text(_model, 256) == "manual-model" && _catalogReady);
                Win32.SetWindowTextW(_key, "fixture-provider-one-key");
                pick(2); check("provider_change_clears", Text(_model, 256) == "" && !_catalogReady &&
                    Win32.SendMessageW(_model, 0x0146, IntPtr.Zero, IntPtr.Zero) == IntPtr.Zero);
                check("provider_key_isolated", Text(_key, 1024) == "");
                DialogProc(dlg, Win32.WM_COMMAND, (IntPtr)IDC_API_REFRESH, IntPtr.Zero);
                check("provider_missing_key", _request == null && Text(_status, 256).Contains("请先填写当前提供商的 API Key"));
                Win32.SetWindowTextW(_key, "fixture-provider-two-key"); pick(1);
                check("provider_key_restored", Text(_key, 1024) == "fixture-provider-one-key");
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
                check("catalog_selected_name", Text(_status, 256).Contains("Future Model 2099"));
                check("catalog_selected_id", Text(_model, 256) == result.Items[0]);
                result.Names[0] = "Future Model 2100";
                ApplyModelResult(endpoint, "", result);
                check("catalog_refresh_name", ModelLabel(result.Items[0]) == "Future Model 2100  (catalog-future-release)" &&
                    (int)Win32.SendMessageW(_model, 0x146, IntPtr.Zero, IntPtr.Zero) == 2);
                check("catalog_optional_name", ModelLabel(result.Items[1]) == result.Items[1]);
                Win32.SetWindowTextW(_model, "CATALOG-FUTURE-RELEASE");
                DialogProc(dlg, Win32.WM_COMMAND, (IntPtr)(IDC_API_MODEL | (5 << 16)), _model);
                check("catalog_case_sensitive", Text(_status, 256).Contains("当前模型未在目录中"));
                result = new ApiModels.Result { Message = "fixture HTTP 401" };
                ApplyModelResult(endpoint, "", result);
                check("catalog_failure_keeps", Text(_model, 256) == "CATALOG-FUTURE-RELEASE" && !_catalogReady && Text(_status, 256) == result.Message);
                var testResult = new ApiConnection.Result { Message = "fixture connection success" };
                Win32.SetWindowTextW(_key, ""); _testChanged = false;
                check("connection_current", ApplyConnectionResult(endpoint, "CATALOG-FUTURE-RELEASE", "", testResult));
                DialogProc(dlg, Win32.WM_COMMAND, (IntPtr)(IDC_API_MODEL | (5 << 16)), _model);
                check("connection_model_stale", !ApplyConnectionResult(endpoint, "CATALOG-FUTURE-RELEASE", "", testResult));
                _testChanged = false; Win32.SetWindowTextW(_key, "changed"); Win32.SetWindowTextW(_key, "");
                check("connection_key_stale", !ApplyConnectionResult(endpoint, "CATALOG-FUTURE-RELEASE", "", testResult));
                _testChanged = false; Win32.SetWindowTextW(_endpoint, "https://fixture.invalid"); Win32.SetWindowTextW(_endpoint, endpoint);
                check("connection_endpoint_stale", !ApplyConnectionResult(endpoint, "CATALOG-FUTURE-RELEASE", "", testResult));
                TestConnection(dlg);
                check("connection_missing_key", _testRequest == null && Text(_testStatus, 256).Contains("请先填写当前提供商的 API Key"));
                Win32.SetWindowTextW(_model, ""); Win32.SetWindowTextW(_key, "fixture-key"); TestConnection(dlg);
                check("connection_missing_model", _testRequest == null && Text(_testStatus, 256).Contains("请先填写 API 地址和模型 ID"));
                Win32.SetWindowTextW(_model, "CATALOG-FUTURE-RELEASE");
                Win32.RECT before; Win32.GetWindowRect(dlg, out before);
                IntPtr rectangle = Marshal.AllocHGlobal(Marshal.SizeOf(typeof(Win32.RECT)));
                Marshal.StructureToPtr(before, rectangle, false);
                Win32.SendMessageW(dlg, Win32.WM_DPICHANGED, (IntPtr)(144 | (144 << 16)), rectangle);
                Marshal.FreeHGlobal(rectangle);
                check("dpi_preserves", Text(_model, 256) == "CATALOG-FUTURE-RELEASE" && _theme.Dpi == 144);
                Win32.SendMessageW(dlg, Win32.WM_COMMAND, (IntPtr)IDC_API_CANCEL, IntPtr.Zero);
                check("cancel", _done && !Win32.IsWindow(dlg));
                _theme.Dispose();
                if (enabled) Win32.EnableWindow(owner, true);
                ThemedMessage.Probe(dpi, report);
                return;
            }
            Win32.ShowWindow(dlg, Win32.SW_SHOW); DialogTheme.SetFocus(_provider);
            Win32.MSG m;
            while (!_done) {
                int got = Win32.GetMessageW(out m, IntPtr.Zero, 0, 0);
                if (got == 0) {
                    Win32.PostQuitMessage((int)m.wParam);
                    break;
                }
                if (got < 0) { Log.Append("API 窗口消息循环失败（Windows 错误 " + Marshal.GetLastWin32Error() + "）"); break; }
                if (!DialogTheme.Key(dlg, ref m, IDC_API_SAVE, IDC_API_CANCEL)) {
                    Win32.TranslateMessage(ref m);
                    Win32.DispatchMessageW(ref m);
                }
            }
            if (Win32.IsWindow(dlg)) Win32.DestroyWindow(dlg);
            _theme.Dispose();
            if (enabled) { Win32.EnableWindow(owner, true); Win32.SetForegroundWindow(owner); }
        }
    }
}
