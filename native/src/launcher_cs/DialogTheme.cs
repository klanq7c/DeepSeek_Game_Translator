using System;
using System.Runtime.InteropServices;
using System.Text;

namespace DstLauncher
{
    /* 对应 dialog_theme.c。二级窗口独立拥有 DPI 与字体，不修改主窗口的缩放状态。 */
    internal sealed class DialogTheme : IDisposable
    {
        internal int Dpi;
        internal IntPtr Title, Body, Small, Card, Field;
        internal int Sc(int value) { return Theme.Sc(value, Dpi); }
        internal DialogTheme(int dpi) {
            Dpi = dpi; Title = Font(22, "SimSun"); Body = Font(10, "Microsoft YaHei UI"); Small = Font(9, "Microsoft YaHei UI");
            Card = Win32.CreateSolidBrush(Theme.C_CARD); Field = Win32.CreateSolidBrush(Theme.C_LOG);
        }
        private IntPtr Font(int pt, string face) {
            return Win32.CreateFontW(-Win32.MulDiv(pt, Dpi, 72), 0, 0, 0, 400, 0, 0, 0, Win32.DEFAULT_CHARSET,
                Win32.OUT_DEFAULT_PRECIS, Win32.CLIP_DEFAULT_PRECIS, Win32.CLEARTYPE_QUALITY, Win32.DEFAULT_PITCH, face);
        }
        public void Dispose() {
            Win32.DeleteObject(Title); Win32.DeleteObject(Body); Win32.DeleteObject(Small); Win32.DeleteObject(Card); Win32.DeleteObject(Field);
        }
        [StructLayout(LayoutKind.Sequential)] private struct MonitorInfo { internal uint size; internal Win32.RECT monitor, work; internal uint flags; }
        [DllImport("user32.dll")] private static extern IntPtr MonitorFromWindow(IntPtr hwnd, uint flags);
        [DllImport("user32.dll", CharSet = CharSet.Unicode)] private static extern bool GetMonitorInfoW(IntPtr monitor, ref MonitorInfo info);
        [DllImport("user32.dll")] internal static extern IntPtr GetFocus();
        [DllImport("user32.dll")] internal static extern IntPtr SetFocus(IntPtr hwnd);
        [DllImport("user32.dll")] internal static extern bool IsWindowEnabled(IntPtr hwnd);
        [DllImport("user32.dll")] private static extern int GetDlgCtrlID(IntPtr hwnd);
        [DllImport("user32.dll")] private static extern IntPtr GetWindow(IntPtr hwnd, uint command);
        [DllImport("user32.dll")] internal static extern IntPtr GetNextDlgTabItem(IntPtr hwnd, IntPtr previous, bool backwards);
        [DllImport("user32.dll", EntryPoint = "GetWindowLongW")] internal static extern int GetStyle(IntPtr hwnd, int index);
        internal static int ControlId(IntPtr hwnd) { return GetDlgCtrlID(hwnd); }
        [DllImport("user32.dll", CharSet = CharSet.Unicode)] private static extern bool SetPropW(IntPtr hwnd, string name, IntPtr data);
        [DllImport("user32.dll", CharSet = CharSet.Unicode)] private static extern IntPtr GetPropW(IntPtr hwnd, string name);
        [DllImport("user32.dll", CharSet = CharSet.Unicode)] private static extern IntPtr RemovePropW(IntPtr hwnd, string name);
        internal static int FitDpi(IntPtr owner, int width, int height, int dpi) {
            var mi = new MonitorInfo { size = (uint)Marshal.SizeOf(typeof(MonitorInfo)) };
            if (!GetMonitorInfoW(MonitorFromWindow(owner, 2), ref mi)) {
                Log.Append("二级窗口：无法计算可用尺寸（Windows 错误 " + Marshal.GetLastWin32Error() + "）"); return dpi;
            }
            int fit = Math.Min(Win32.MulDiv(mi.work.right - mi.work.left - 32, 96, width), Win32.MulDiv(mi.work.bottom - mi.work.top - 64, 96, height));
            return Math.Min(dpi, Math.Max(72, fit));
        }
        internal void Place(IntPtr hwnd, IntPtr owner, int width, int height) {
            Win32.RECT outer, client; Win32.GetWindowRect(hwnd, out outer); Win32.GetClientRect(hwnd, out client);
            int w = Sc(width) + outer.right - outer.left - client.right, h = Sc(height) + outer.bottom - outer.top - client.bottom;
            var mi = new MonitorInfo { size = (uint)Marshal.SizeOf(typeof(MonitorInfo)) };
            if (!GetMonitorInfoW(MonitorFromWindow(owner != IntPtr.Zero ? owner : hwnd, 2), ref mi)) {
                Log.Append("二级窗口：无法读取显示器工作区（Windows 错误 " + Marshal.GetLastWin32Error() + "）"); return;
            }
            Win32.RECT anchor = mi.work; if (owner != IntPtr.Zero) Win32.GetWindowRect(owner, out anchor);
            int x = anchor.left + (anchor.right - anchor.left - w) / 2, y = anchor.top + (anchor.bottom - anchor.top - h) / 2;
            x = Math.Max(mi.work.left, Math.Min(x, mi.work.right - w)); y = Math.Max(mi.work.top, Math.Min(y, mi.work.bottom - h));
            Win32.SetWindowPos(hwnd, IntPtr.Zero, x, y, w, h, Win32.SWP_NOZORDER | Win32.SWP_NOACTIVATE);
        }
        private static readonly Win32.SubclassProcDelegate HoverProc = Hover;
        private static IntPtr Hover(IntPtr hwnd, uint msg, IntPtr wp, IntPtr lp, UIntPtr id, UIntPtr data) {
            if (msg == Win32.WM_MOUSEMOVE && GetPropW(hwnd, "DstDialogHot") == IntPtr.Zero) {
                var track = new Win32.TRACKMOUSEEVENT { cbSize = (uint)Marshal.SizeOf(typeof(Win32.TRACKMOUSEEVENT)), dwFlags = Win32.TME_LEAVE, hwndTrack = hwnd };
                Win32.TrackMouseEvent(ref track); SetPropW(hwnd, "DstDialogHot", (IntPtr)1); Win32.InvalidateRect(hwnd, IntPtr.Zero, false);
            } else if (msg == Win32.WM_MOUSELEAVE) {
                RemovePropW(hwnd, "DstDialogHot"); Win32.InvalidateRect(hwnd, IntPtr.Zero, false);
            } else if (msg == 7 || msg == 8) {
                Win32.InvalidateRect(Win32.GetParent(hwnd), IntPtr.Zero, false);
            } else if (msg == Win32.WM_NCDESTROY) {
                RemovePropW(hwnd, "DstDialogHot"); Win32.RemoveWindowSubclass(hwnd, HoverProc, id);
            }
            return Win32.DefSubclassProc(hwnd, msg, wp, lp);
        }
        internal IntPtr Control(IntPtr hwnd, string cls, string text, uint style, int id, int x, int y, int width, int height) {
            IntPtr ctl = Win32.CreateWindowExW(0, cls, text, Win32.WS_CHILD | Win32.WS_VISIBLE | style,
                Sc(x), Sc(y), Sc(width), Sc(height), hwnd, (IntPtr)id, MainWindow.Instance, IntPtr.Zero);
            Win32.SendMessageW(ctl, Win32.WM_SETFONT, Body, IntPtr.Zero);
            if (!Win32.SetWindowSubclass(ctl, HoverProc, (UIntPtr)1, UIntPtr.Zero))
                Log.Append("二级窗口：无法安装控件焦点与悬停跟踪（控件 " + id + "，Windows 错误 " + Marshal.GetLastWin32Error() + "）");
            return ctl;
        }
        internal Win32.RECT Rect(int x, int y, int w, int h) { return new Win32.RECT(Sc(x), Sc(y), Sc(x + w), Sc(y + h)); }
        internal void Text(IntPtr dc, string text, int x, int y, int w, int h, IntPtr font, uint color) {
            var r = Rect(x, y, w, h); Win32.SelectObject(dc, font); Win32.SetTextColor(dc, color);
            Win32.DrawTextW(dc, text, -1, ref r, Win32.DT_LEFT | Win32.DT_VCENTER | Win32.DT_SINGLELINE | Win32.DT_NOPREFIX);
        }
        internal void Paint(IntPtr hwnd, IntPtr dc, string title, string subtitle) {
            Win32.RECT r; Win32.GetClientRect(hwnd, out r);
            IntPtr bg = Win32.CreateSolidBrush(Theme.C_CARD_ELEV); Win32.FillRect(dc, ref r, bg); Win32.DeleteObject(bg);
            var top = new Win32.RECT(0, 0, r.right, Sc(152)); bg = Win32.CreateSolidBrush(Theme.C_PAGE); Win32.FillRect(dc, ref top, bg); Win32.DeleteObject(bg);
            Win32.SetBkMode(dc, Win32.TRANSPARENT); int width = Win32.MulDiv(r.right, 96, Dpi);
            Text(dc, "游 戏 无 界  /  PLAY BEYOND WORDS", 28, 16, width - 56, 18, Small, Theme.C_ACCENT);
            Text(dc, title, 28, 42, width - 108, 34, Title, Theme.C_CARD);
            Text(dc, subtitle, 28, 84, width - 56, 20, Small, Win32.RGB(174, 190, 207));
            IntPtr ob = Win32.SelectObject(dc, Card), op = Win32.SelectObject(dc, Win32.GetStockObject(Win32.NULL_PEN));
            bg = Win32.CreateSolidBrush(Theme.C_ACCENT); Win32.SelectObject(dc, bg);
            Win32.Ellipse(dc, r.right - Sc(64), Sc(43), r.right - Sc(36), Sc(71));
            IntPtr cut = Win32.CreateSolidBrush(Theme.C_PAGE); Win32.SelectObject(dc, cut);
            Win32.Ellipse(dc, r.right - Sc(56), Sc(37), r.right - Sc(28), Sc(65));
            Win32.SelectObject(dc, ob); Win32.SelectObject(dc, op); Win32.DeleteObject(bg); Win32.DeleteObject(cut);
            var card = new Win32.RECT(Sc(20), Sc(120), r.right - Sc(20), r.bottom - Sc(76));
            UiPaint.DrawRound(dc, card, Theme.C_CARD, Theme.C_LINE, Sc(18));
        }
        internal void Button(ref Win32.DRAWITEMSTRUCT di, bool primary) {
            bool enabled = IsWindowEnabled(di.hwndItem), hot = GetPropW(di.hwndItem, "DstDialogHot") != IntPtr.Zero;
            uint fill = primary ? Theme.C_ACCENT : Theme.C_CARD, line = primary ? Theme.C_ACCENT_DARK : Theme.C_LINE;
            if (enabled && (hot || (di.itemState & Win32.ODS_SELECTED) != 0)) { fill = primary ? Theme.C_ACCENT_DARK : Theme.C_CARD_ELEV; line = Theme.C_ACCENT_DEEP; }
            if (!enabled) fill = Theme.C_CARD_ELEV;
            Win32.FillRect(di.hDC, ref di.rcItem, Card); UiPaint.DrawRound(di.hDC, di.rcItem, fill, line, Sc(16));
            var text = new StringBuilder(128); Win32.GetWindowTextW(di.hwndItem, text, 128); Win32.RECT r = di.rcItem;
            Win32.SetBkMode(di.hDC, Win32.TRANSPARENT); Win32.SetTextColor(di.hDC, enabled ? Theme.C_TEXT : Theme.C_MUTED); Win32.SelectObject(di.hDC, Body);
            Win32.DrawTextW(di.hDC, text.ToString(), -1, ref r, Win32.DT_CENTER | Win32.DT_VCENTER | Win32.DT_SINGLELINE | Win32.DT_NOPREFIX);
            if ((di.itemState & Win32.ODS_FOCUS) != 0) { r.left += Sc(4); r.top += Sc(4); r.right -= Sc(4); r.bottom -= Sc(4); Win32.DrawFocusRect(di.hDC, ref r); }
        }
        internal IntPtr Color(IntPtr dc, bool field) {
            Win32.SetTextColor(dc, field ? Theme.C_TEXT : Theme.C_TEXT_DIM); Win32.SetBkColor(dc, field ? Theme.C_LOG : Theme.C_CARD); return field ? Field : Card;
        }
        [DllImport("user32.dll", EntryPoint = "SendMessageW", CharSet = CharSet.Unicode)]
        private static extern IntPtr ComboText(IntPtr hwnd, uint msg, IntPtr index, StringBuilder text);
        internal void Combo(ref Win32.DRAWITEMSTRUCT di, string label = null) {
            bool selected = (di.itemState & Win32.ODS_SELECTED) != 0 && (di.itemState & 0x1000) == 0;
            IntPtr bg = Win32.CreateSolidBrush(selected ? Theme.C_ACCENT : Theme.C_LOG); Win32.FillRect(di.hDC, ref di.rcItem, bg); Win32.DeleteObject(bg);
            if (di.itemID == uint.MaxValue) return;
            int length = (int)Win32.SendMessageW(di.hwndItem, 0x149, (IntPtr)di.itemID, IntPtr.Zero);
            if (length < 0 || length >= 1024) { Log.Append("二级窗口：下拉项长度无效（" + length + "）"); return; }
            var text = new StringBuilder(1024); ComboText(di.hwndItem, 0x148, (IntPtr)di.itemID, text);
            Win32.RECT r = di.rcItem; r.left += Sc(8); r.right -= Sc(4);
            Win32.SelectObject(di.hDC, Body); Win32.SetBkMode(di.hDC, Win32.TRANSPARENT); Win32.SetTextColor(di.hDC, Theme.C_TEXT);
            Win32.DrawTextW(di.hDC, label ?? text.ToString(), -1, ref r, Win32.DT_LEFT | Win32.DT_SINGLELINE | Win32.DT_VCENTER | Win32.DT_END_ELLIPSIS | Win32.DT_NOPREFIX);
            if ((di.itemState & Win32.ODS_FOCUS) != 0) Win32.DrawFocusRect(di.hDC, ref di.rcItem);
        }
        internal static bool Key(IntPtr hwnd, ref Win32.MSG m, int accept, int cancel) {
            if (m.message == 0x100 && ((int)m.wParam == 27 || (int)m.wParam == 13)) {
                IntPtr focus = GetFocus(), combo = focus; var cls = new StringBuilder(32); Win32.GetClassNameW(combo, cls, 32);
                if (cls.ToString().Equals("Edit", StringComparison.OrdinalIgnoreCase)) combo = Win32.GetParent(focus);
                Win32.GetClassNameW(combo, cls, 32);
                if (cls.ToString().Equals("ComboBox", StringComparison.OrdinalIgnoreCase) && Win32.SendMessageW(combo, 0x157, IntPtr.Zero, IntPtr.Zero) != IntPtr.Zero) return false;
                int id = (int)m.wParam == 27 ? cancel : accept; Win32.GetClassNameW(focus, cls, 32);
                if ((int)m.wParam == 13 && cls.ToString().Equals("Button", StringComparison.OrdinalIgnoreCase)) id = GetDlgCtrlID(focus);
                Win32.SendMessageW(hwnd, Win32.WM_COMMAND, (IntPtr)id, Win32.GetDlgItem(hwnd, id)); return true;
            }
            return Win32.IsDialogMessageW(hwnd, ref m);
        }
        internal static bool RenderProbe(IntPtr hwnd, string path) {
            Win32.RECT r; Win32.GetClientRect(hwnd, out r);
            var bi = new Win32.BITMAPINFOHEADER { biSize = 40, biWidth = r.right, biHeight = -r.bottom, biPlanes = 1, biBitCount = 32 };
            IntPtr screen = Win32.GetDC(IntPtr.Zero), dc = Win32.CreateCompatibleDC(screen), pixels;
            IntPtr bmp = Win32.CreateDIBSection(screen, ref bi, 0, out pixels, IntPtr.Zero, 0); Win32.ReleaseDC(IntPtr.Zero, screen);
            if (dc == IntPtr.Zero || bmp == IntPtr.Zero || pixels == IntPtr.Zero) {
                Log.Append("二级窗口探针：GDI 分配失败（Windows 错误 " + Marshal.GetLastWin32Error() + "）");
                if (bmp != IntPtr.Zero) Win32.DeleteObject(bmp); if (dc != IntPtr.Zero) Win32.DeleteDC(dc); return false;
            }
            IntPtr old = Win32.SelectObject(dc, bmp);
            Win32.SendMessageW(hwnd, Win32.WM_PRINTCLIENT, dc, (IntPtr)4);
            for (IntPtr ctl = GetWindow(hwnd, 5); ctl != IntPtr.Zero; ctl = GetWindow(ctl, 2)) {
                Win32.RECT cr; Win32.GetWindowRect(ctl, out cr); Win32.MapWindowPoints(IntPtr.Zero, hwnd, ref cr, 2);
                int saved = Win32.SaveDC(dc); Win32.IntersectClipRect(dc, cr.left, cr.top, cr.right, cr.bottom);
                Win32.SetViewportOrgEx(dc, cr.left, cr.top, IntPtr.Zero);
                Win32.SendMessageW(ctl, 0x317, dc, (IntPtr)(4 | 2 | 16 | 8));
                Win32.RestoreDC(dc, saved);
            }
            Win32.GdiFlush();
            bool ok = UiProbe.WriteBmp32(path, pixels, r.right, r.bottom, Log.Append);
            Win32.SelectObject(dc, old); Win32.DeleteObject(bmp); Win32.DeleteDC(dc); return ok;
        }
    }

    internal static class ThemedMessage
    {
        private sealed class State { internal DialogTheme Theme; internal string Text, Title; internal uint Flags; internal int Result, Height; internal bool Done; }
        private static readonly Win32.WndProcDelegate ProcHolder = Proc;
        private static bool _registered;
        private static void Layout(IntPtr hwnd, State d) {
            int[] ids = {300, 7, (d.Flags & 15) == 4 ? 6 : 1}, xs = {44, 280, 404}, ys = {144, d.Height - 56, d.Height - 56}, ws = {472, 112, 112}, hs = {d.Height - 236, 36, 36};
            for (int i = 0; i < 3; i++) {
                IntPtr ctl = Win32.GetDlgItem(hwnd, ids[i]); if (ctl == IntPtr.Zero) continue;
                Win32.SendMessageW(ctl, Win32.WM_SETFONT, d.Theme.Body, IntPtr.Zero);
                Win32.SetWindowPos(ctl, IntPtr.Zero, d.Theme.Sc(xs[i]), d.Theme.Sc(ys[i]), d.Theme.Sc(ws[i]), d.Theme.Sc(hs[i]), Win32.SWP_NOZORDER | Win32.SWP_NOACTIVATE);
            }
        }
        [DllImport("user32.dll", EntryPoint = "SetWindowLongPtrW")] private static extern IntPtr SetData(IntPtr hwnd, int index, IntPtr value);
        [DllImport("user32.dll", EntryPoint = "GetWindowLongPtrW")] private static extern IntPtr GetData(IntPtr hwnd, int index);
        private static IntPtr Proc(IntPtr hwnd, uint msg, IntPtr wp, IntPtr lp) {
            IntPtr ptr = GetData(hwnd, -21);
            if (msg == Win32.WM_CREATE) { ptr = Marshal.ReadIntPtr(lp); SetData(hwnd, -21, ptr); }
            if (ptr == IntPtr.Zero) return Win32.DefWindowProcW(hwnd, msg, wp, lp);
            var d = (State)GCHandle.FromIntPtr(ptr).Target; bool yesno = (d.Flags & 15) == 4;
            switch (msg) {
                case Win32.WM_CREATE:
                    d.Theme.Control(hwnd, "STATIC", d.Text, Win32.SS_NOPREFIX, 300, 44, 144, 472, d.Height - 236);
                    if (yesno) d.Theme.Control(hwnd, "BUTTON", "取消", Win32.BS_OWNERDRAW | 0x10000, 7, 280, d.Height - 56, 112, 36);
                    d.Theme.Control(hwnd, "BUTTON", yesno ? "确认" : "知道了", Win32.BS_OWNERDRAW | 0x10000, yesno ? 6 : 1, 404, d.Height - 56, 112, 36);
                    return IntPtr.Zero;
                case Win32.WM_ERASEBKGND: return (IntPtr)1;
                case Win32.WM_PAINT:
                    Win32.PAINTSTRUCT ps; IntPtr dc = Win32.BeginPaint(hwnd, out ps);
                    d.Theme.Paint(hwnd, dc, d.Title, yesno ? "请确认后继续" : "来自翻译器的提示"); Win32.EndPaint(hwnd, ref ps); return IntPtr.Zero;
                case Win32.WM_PRINTCLIENT:
                    d.Theme.Paint(hwnd, wp, d.Title, yesno ? "请确认后继续" : "来自翻译器的提示"); return IntPtr.Zero;
                case Win32.WM_CTLCOLORSTATIC: return d.Theme.Color(wp, false);
                case Win32.WM_DRAWITEM:
                    var di = (Win32.DRAWITEMSTRUCT)Marshal.PtrToStructure(lp, typeof(Win32.DRAWITEMSTRUCT)); d.Theme.Button(ref di, di.CtlID != 7); return (IntPtr)1;
                case Win32.WM_DPICHANGED: {
                    var r = (Win32.RECT)Marshal.PtrToStructure(lp, typeof(Win32.RECT));
                    Win32.SetWindowPos(hwnd, IntPtr.Zero, r.left, r.top, r.right-r.left, r.bottom-r.top, Win32.SWP_NOZORDER | Win32.SWP_NOACTIVATE);
                    int dpi = DialogTheme.FitDpi(hwnd, 560, d.Height, Win32.HIWORD(wp));
                    d.Theme.Dispose(); d.Theme = new DialogTheme(dpi); Layout(hwnd, d); d.Theme.Place(hwnd, hwnd, 560, d.Height);
                    Win32.InvalidateRect(hwnd, IntPtr.Zero, true); return IntPtr.Zero;
                }
                case Win32.WM_COMMAND:
                    int id = Win32.LOWORD(wp);
                    if (id == 1 || id == 6 || id == 7 || id == 2) {
                        d.Result = id == 2 ? (yesno ? 7 : 1) : id; d.Done = true; Win32.DestroyWindow(hwnd); return IntPtr.Zero;
                    } break;
                case Win32.WM_CLOSE: d.Result = yesno ? 7 : 1; d.Done = true; Win32.DestroyWindow(hwnd); return IntPtr.Zero;
            }
            return Win32.DefWindowProcW(hwnd, msg, wp, lp);
        }
        private static int ShowImpl(IntPtr owner, string text, string title, uint flags, int probe, int dpi, Action<string> report = null) {
            if (!_registered) {
                var wc = new Win32.WNDCLASSEXW { cbSize = (uint)Marshal.SizeOf(typeof(Win32.WNDCLASSEXW)),
                    lpfnWndProc = Marshal.GetFunctionPointerForDelegate(ProcHolder), hInstance = MainWindow.Instance,
                    hCursor = Win32.LoadCursorW(IntPtr.Zero, (IntPtr)32512), lpszClassName = Marshal.StringToHGlobalUni("DstThemedMessageCs") };
                bool ok = Win32.RegisterClassExW(ref wc) != 0; Marshal.FreeHGlobal(wc.lpszClassName);
                if (!ok) { Log.Append("无法注册提示窗口（Windows 错误 " + Marshal.GetLastWin32Error() + "）"); return 0; } _registered = true;
            }
            var d = new State { Text = text, Title = title, Flags = flags, Theme = new DialogTheme(dpi) };
            IntPtr dc = Win32.GetDC(owner), old = Win32.SelectObject(dc, d.Theme.Body);
            var measure = new Win32.RECT(0, 0, d.Theme.Sc(472), 0);
            Win32.DrawTextW(dc, text, -1, ref measure, 0x400 | 0x10 | Win32.DT_NOPREFIX);
            Win32.SelectObject(dc, old); Win32.ReleaseDC(owner, dc); d.Height = Math.Max(286, Win32.MulDiv(measure.bottom, 96, d.Theme.Dpi) + 248);
            if (probe == 0) {
                int fit = DialogTheme.FitDpi(owner, 560, d.Height, dpi);
                if (fit != dpi) { d.Theme.Dispose(); d.Theme = new DialogTheme(fit); }
            }
            var handle = GCHandle.Alloc(d);
            IntPtr dlg = Win32.CreateWindowExW(Win32.WS_EX_DLGMODALFRAME, "DstThemedMessageCs", title,
                Win32.WS_CAPTION | Win32.WS_SYSMENU | Win32.WS_CLIPCHILDREN, Win32.CW_USEDEFAULT, Win32.CW_USEDEFAULT, 560, d.Height, owner, IntPtr.Zero, MainWindow.Instance, GCHandle.ToIntPtr(handle));
            if (dlg == IntPtr.Zero) { Log.Append("无法创建提示窗口（Windows 错误 " + Marshal.GetLastWin32Error() + "）"); handle.Free(); d.Theme.Dispose(); return 0; }
            bool enabled = owner != IntPtr.Zero && DialogTheme.IsWindowEnabled(owner); IntPtr focus = DialogTheme.GetFocus(); if (enabled) Win32.EnableWindow(owner, false);
            d.Theme.Place(dlg, owner, 560, d.Height); MainWindow.ApplyWindowChrome(dlg);
            int accept = (flags & 15) == 4 ? ((flags & 0x100) != 0 ? 7 : 6) : 1;
            if (probe != 0) {
                string bmp = Environment.GetEnvironmentVariable("DST_API_UI_BMP");
                if (probe == 1 && !string.IsNullOrEmpty(bmp)) report("message_render=" + (DialogTheme.RenderProbe(dlg, bmp + ".message.bmp") ? 1 : 0) + "\n");
                var key = new Win32.MSG { hwnd = dlg, message = 0x100, wParam = (IntPtr)(probe == 2 ? 27 : 13) };
                if (probe == 3) Win32.SendMessageW(dlg, Win32.WM_COMMAND, (IntPtr)6, IntPtr.Zero);
                else if (probe == 4) Win32.SendMessageW(dlg, Win32.WM_CLOSE, IntPtr.Zero, IntPtr.Zero);
                else DialogTheme.Key(dlg, ref key, accept, 2);
            } else { Win32.ShowWindow(dlg, Win32.SW_SHOW); DialogTheme.SetFocus(Win32.GetDlgItem(dlg, accept)); }
            Win32.MSG m;
            while (!d.Done) {
                int got = Win32.GetMessageW(out m, IntPtr.Zero, 0, 0);
                if (got <= 0) { if (got == 0) Win32.PostQuitMessage((int)m.wParam); else Log.Append("提示窗口消息循环失败（Windows 错误 " + Marshal.GetLastWin32Error() + "）"); break; }
                if (!DialogTheme.Key(dlg, ref m, accept, 2)) { Win32.TranslateMessage(ref m); Win32.DispatchMessageW(ref m); }
            }
            if (Win32.IsWindow(dlg)) Win32.DestroyWindow(dlg);
            if (enabled) { Win32.EnableWindow(owner, true); Win32.SetForegroundWindow(owner); if (Win32.IsWindow(focus)) DialogTheme.SetFocus(focus); }
            handle.Free(); d.Theme.Dispose(); return d.Result;
        }
        internal static int Show(IntPtr owner, string text, string title, uint flags) { return ShowImpl(owner, text, title, flags, 0, Theme.ScaleDpi); }
        internal static void Probe(int dpi, Action<string> report) {
            string[] names = {"message_enter=", "message_escape=", "message_yes=", "message_close=", "message_info="};
            for (int i = 1; i <= 5; i++) {
                int r = ShowImpl(IntPtr.Zero, "这将清除共享翻译缓存。\n\n游戏文件不会改变。是否继续？", "清除缓存", (uint)(i == 5 ? 0x40 : 0x30 | 4 | 0x100), i, dpi, report);
                report(names[i-1] + (r == (i == 3 ? 6 : i == 5 ? 1 : 7) ? 1 : 0) + "\n");
            }
        }
    }
}
