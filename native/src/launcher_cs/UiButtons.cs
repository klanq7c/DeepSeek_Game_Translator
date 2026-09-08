using System;
using System.Text;

namespace DstLauncher
{
    /*
     * UiButtons —— native/src/launcher/ui.c 的按钮悬停状态机、draw_button_icon 与
     * draw_button 移植。
     *
     * 工程未启用 comctl32 v6 视觉样式清单，ODS_HOTLIGHT 不可靠，因此和 C 版一样
     * 通过 SetWindowSubclass 跟踪每个自绘按钮的 WM_MOUSEMOVE / WM_MOUSELEAVE。
     */
    internal static class UiButtons
    {
        private sealed class ButtonHover
        {
            internal int id;        /* 控件 ID */
            internal float t;       /* 当前悬停强度 0..1 */
            internal bool target;   /* 目标状态：鼠标悬停为 true */
            internal bool tracking; /* 是否已注册 TrackMouseEvent */
        }

        private static readonly ButtonHover[] Hovers = {
            new ButtonHover { id = Theme.IDC_BROWSE },
            new ButtonHover { id = Theme.IDC_OPEN },
            new ButtonHover { id = Theme.IDC_START },
            new ButtonHover { id = Theme.IDC_RESTORE },
            new ButtonHover { id = Theme.IDC_SERVER_TOGGLE },
            new ButtonHover { id = Theme.IDC_API_CONFIG },
            new ButtonHover { id = Theme.IDC_CLEAR_CACHE },
        };

        /* 委托必须由托管侧持有，否则 GC 回收后 comctl32 回调会跳到已释放的 thunk。 */
        private static readonly Win32.SubclassProcDelegate HoverProc = ButtonHoverProc;

        private static int Sc(int v) { return Theme.Sc(v); }
        private static uint Mix(uint a, uint b, float t) { return Theme.Mix(a, b, t); }

        private static ButtonHover HoverFor(int id)
        {
            for (int i = 0; i < Hovers.Length; i++) {
                if (Hovers[i].id == id) return Hovers[i];
            }
            return null;
        }

        private static float HoverValue(int id)
        {
            ButtonHover h = HoverFor(id);
            return h != null ? h.t : 0.0f;
        }

        private static IntPtr ButtonHoverProc(IntPtr btn, uint msg, IntPtr wp, IntPtr lp,
                                              UIntPtr subId, UIntPtr refData)
        {
            ButtonHover h = HoverFor((int)subId);
            switch (msg) {
                case Win32.WM_MOUSEMOVE:
                    if (h != null && !h.tracking) {
                        var tme = new Win32.TRACKMOUSEEVENT();
                        tme.cbSize = (uint)System.Runtime.InteropServices.Marshal.SizeOf(typeof(Win32.TRACKMOUSEEVENT));
                        tme.dwFlags = Win32.TME_LEAVE;
                        tme.hwndTrack = btn;
                        Win32.TrackMouseEvent(ref tme);
                        h.tracking = true;
                        h.target = true;
                    }
                    break;
                case Win32.WM_MOUSELEAVE:
                    if (h != null) {
                        h.tracking = false;
                        h.target = false;
                    }
                    break;
                case Win32.WM_NCDESTROY:
                    Win32.RemoveWindowSubclass(btn, HoverProc, subId);
                    break;
            }
            return Win32.DefSubclassProc(btn, msg, wp, lp);
        }

        /* 为主窗口所有自绘按钮安装悬停跟踪子类（WM_CREATE 时调用一次） */
        internal static void InstallHoverTracking(IntPtr hwnd)
        {
            for (int i = 0; i < Hovers.Length; i++) {
                IntPtr btn = Win32.GetDlgItem(hwnd, Hovers[i].id);
                if (btn != IntPtr.Zero && Win32.IsWindow(btn)) {
                    Win32.SetWindowSubclass(btn, HoverProc, (UIntPtr)(uint)Hovers[i].id, UIntPtr.Zero);
                }
            }
        }

        /* 指数缓动推进悬停强度，过渡期间重绘按钮（tick_ui_animation 的后半段）。 */
        internal static void TickHover(IntPtr hwnd)
        {
            for (int i = 0; i < Hovers.Length; i++) {
                ButtonHover h = Hovers[i];
                float target = h.target ? 1.0f : 0.0f;
                if (h.t == target) continue;
                float next = h.t + (target - h.t) * 0.22f;
                if (Math.Abs(next - target) < 0.012f) next = target;
                if (next == h.t) continue;
                h.t = next;
                IntPtr btn = Win32.GetDlgItem(hwnd, h.id);
                if (btn != IntPtr.Zero && Win32.IsWindow(btn)) Win32.InvalidateRect(btn, IntPtr.Zero, false);
            }
        }

        private static int MaxI(int a, int b) { return a > b ? a : b; }

        /* 按钮矢量图标（draw_button_icon）。返回是否画了图标。 */
        private static bool DrawButtonIcon(IntPtr dc, int id, int cx, int cy, uint color)
        {
            int s = Sc(7);
            IntPtr pen = Win32.CreatePen(Win32.PS_SOLID, MaxI(Sc(2), 1), color);
            IntPtr oldPen = Win32.SelectObject(dc, pen);
            IntPtr oldBrush = Win32.SelectObject(dc, Win32.GetStockObject(Win32.HOLLOW_BRUSH));
            bool drawn = true;

            if (id == Theme.IDC_START) {
                var bolt = new Win32.POINT[6] {
                    new Win32.POINT(cx + Sc(1), cy - s), new Win32.POINT(cx - Sc(5), cy + Sc(1)),
                    new Win32.POINT(cx - Sc(1), cy + Sc(1)), new Win32.POINT(cx - Sc(2), cy + s),
                    new Win32.POINT(cx + Sc(5), cy - Sc(2)), new Win32.POINT(cx + Sc(1), cy - Sc(2))
                };
                IntPtr fill = Win32.CreateSolidBrush(color);
                Win32.SelectObject(dc, fill);
                Win32.SelectObject(dc, Win32.GetStockObject(Win32.NULL_PEN));
                Win32.Polygon(dc, bolt, 6);
                Win32.SelectObject(dc, oldPen);
                Win32.SelectObject(dc, oldBrush);
                Win32.DeleteObject(fill);
                Win32.DeleteObject(pen);
                return true;
            } else if (id == Theme.IDC_BROWSE || id == Theme.IDC_OPEN) {
                Win32.MoveToEx(dc, cx - s, cy - Sc(4), IntPtr.Zero);
                Win32.LineTo(dc, cx - Sc(2), cy - Sc(4));
                Win32.LineTo(dc, cx, cy - s);
                Win32.LineTo(dc, cx + s, cy - s);
                Win32.LineTo(dc, cx + s, cy + Sc(5));
                Win32.LineTo(dc, cx - s, cy + Sc(5));
                Win32.LineTo(dc, cx - s, cy - Sc(4));
                if (id == Theme.IDC_OPEN) {
                    Win32.MoveToEx(dc, cx, cy + Sc(2), IntPtr.Zero);
                    Win32.LineTo(dc, cx + s, cy - Sc(5));
                    Win32.MoveToEx(dc, cx + Sc(2), cy - Sc(5), IntPtr.Zero);
                    Win32.LineTo(dc, cx + s, cy - Sc(5));
                    Win32.LineTo(dc, cx + s, cy + Sc(1));
                }
            } else if (id == Theme.IDC_RESTORE) {
                Win32.Arc(dc, cx - s, cy - s, cx + s, cy + s, cx - s, cy, cx + Sc(3), cy - s);
                var arrow = new Win32.POINT[3] {
                    new Win32.POINT(cx - s, cy), new Win32.POINT(cx - Sc(2), cy - Sc(4)), new Win32.POINT(cx - Sc(2), cy + Sc(3))
                };
                IntPtr fill = Win32.CreateSolidBrush(color);
                Win32.SelectObject(dc, fill);
                Win32.SelectObject(dc, Win32.GetStockObject(Win32.NULL_PEN));
                Win32.Polygon(dc, arrow, 3);
                Win32.SelectObject(dc, pen);
                Win32.SelectObject(dc, Win32.GetStockObject(Win32.HOLLOW_BRUSH));
                Win32.DeleteObject(fill);
            } else if (id == Theme.IDC_SERVER_TOGGLE) {
                Win32.Arc(dc, cx - s, cy - s, cx + s, cy + s, cx - Sc(4), cy - Sc(4), cx + Sc(4), cy - Sc(4));
                Win32.MoveToEx(dc, cx, cy - s, IntPtr.Zero);
                Win32.LineTo(dc, cx, cy + Sc(1));
            } else if (id == Theme.IDC_API_CONFIG) {
                for (int i = -1; i <= 1; i++) {
                    int yy = cy + i * Sc(5);
                    Win32.MoveToEx(dc, cx - s, yy, IntPtr.Zero);
                    Win32.LineTo(dc, cx + s, yy);
                }
                Win32.Ellipse(dc, cx - Sc(4), cy - Sc(7), cx, cy - Sc(3));
                Win32.Ellipse(dc, cx + Sc(1), cy - Sc(2), cx + Sc(5), cy + Sc(2));
                Win32.Ellipse(dc, cx - Sc(3), cy + Sc(3), cx + Sc(1), cy + Sc(7));
            } else if (id == Theme.IDC_CLEAR_CACHE) {
                Win32.Rectangle(dc, cx - Sc(5), cy - Sc(4), cx + Sc(5), cy + s);
                Win32.MoveToEx(dc, cx - s, cy - Sc(6), IntPtr.Zero);
                Win32.LineTo(dc, cx + s, cy - Sc(6));
                Win32.MoveToEx(dc, cx - Sc(2), cy - s, IntPtr.Zero);
                Win32.LineTo(dc, cx + Sc(2), cy - s);
            } else {
                drawn = false;
            }

            Win32.SelectObject(dc, oldPen);
            Win32.SelectObject(dc, oldBrush);
            Win32.DeleteObject(pen);
            return drawn;
        }

        /* 自绘按钮：处理 WM_DRAWITEM（draw_button）。 */
        internal static void DrawButton(ref Win32.DRAWITEMSTRUCT di)
        {
            var sb = new StringBuilder(128);
            Win32.GetWindowTextW(di.hwndItem, sb, 128);
            string text = sb.ToString();

            bool primary = di.CtlID == Theme.IDC_START;
            bool serverBtn = di.CtlID == Theme.IDC_SERVER_TOGGLE;
            bool restoreBtn = di.CtlID == Theme.IDC_RESTORE;
            bool clearCacheBtn = di.CtlID == Theme.IDC_CLEAR_CACHE;
            bool pressed = (di.itemState & Win32.ODS_SELECTED) != 0;
            bool focused = (di.itemState & Win32.ODS_FOCUS) != 0;
            bool serverRunning = serverBtn && ServerProcess.Started;
            float hover = HoverValue((int)di.CtlID);
            if (pressed) hover = 0.0f;

            uint fillTop, fillBot, edge, fg;
            if (primary) {
                /* 主按钮：青绿渐变，悬停提亮，按下压暗 */
                fillTop = Mix(Theme.C_ACCENT, Theme.C_TEXT, 0.08f + 0.14f * hover);
                fillBot = Mix(Theme.C_ACCENT_DARK, Theme.C_ACCENT, 0.55f + 0.25f * hover);
                if (pressed) {
                    fillTop = Theme.C_ACCENT_DARK;
                    fillBot = Mix(Theme.C_ACCENT_DARK, Theme.C_ACCENT_DEEP, 0.55f);
                }
                edge = Mix(Theme.C_ACCENT_DARK, Theme.C_ACCENT, 0.40f);
                fg = Win32.RGB(6, 26, 26);
            } else if (serverRunning) {
                /* 服务器运行中：深绿玻璃态 */
                fillTop = Mix(Win32.RGB(21, 58, 48), Win32.RGB(28, 74, 61), hover);
                fillBot = Mix(Win32.RGB(14, 40, 34), Win32.RGB(18, 52, 43), hover);
                if (pressed) { fillTop = Win32.RGB(13, 36, 30); fillBot = Win32.RGB(11, 30, 26); }
                edge = Mix(Mix(Theme.C_LINE, Theme.C_GREEN, 0.45f), Theme.C_GREEN, hover);
                fg = Theme.C_GREEN;
            } else if (restoreBtn || clearCacheBtn) {
                /* 危险操作：暗红玻璃态 */
                fillTop = Mix(Win32.RGB(48, 25, 35), Win32.RGB(62, 30, 43), hover);
                fillBot = Mix(Win32.RGB(36, 19, 27), Win32.RGB(46, 23, 33), hover);
                if (pressed) { fillTop = Win32.RGB(30, 16, 23); fillBot = Win32.RGB(26, 14, 20); }
                edge = Mix(Mix(Theme.C_LINE, Theme.C_DANGER, 0.45f), Theme.C_DANGER, hover);
                fg = Theme.C_DANGER;
            } else {
                /* 普通按钮：深色玻璃态，悬停抬升并点亮语义色边框 */
                fillTop = Mix(Theme.C_CARD_ELEV, Mix(Theme.C_CARD_ELEV, Theme.C_TEXT, 0.05f), hover);
                fillBot = Mix(Mix(Theme.C_CARD, Theme.C_CARD_ELEV, 0.35f), Theme.C_CARD_ELEV, hover);
                if (pressed) { fillTop = Theme.C_CARD; fillBot = Mix(Theme.C_CARD, Theme.C_PAGE, 0.4f); }
                uint accent = Theme.C_LINE_BRIGHT;
                if (di.CtlID == Theme.IDC_API_CONFIG) accent = Theme.C_VIOLET;
                else if (di.CtlID == Theme.IDC_BROWSE || di.CtlID == Theme.IDC_OPEN) accent = Theme.C_BLUE;
                edge = pressed ? Mix(Theme.C_LINE, Theme.C_TEXT, 0.15f) : Mix(Theme.C_LINE, accent, 0.30f + 0.70f * hover);
                fg = Theme.C_TEXT;
            }

            /* 圆角渐变填充 + 发丝边框 */
            int radius = Sc(10);
            IntPtr rgn = Win32.CreateRoundRectRgn(di.rcItem.left, di.rcItem.top,
                                                  di.rcItem.right + 1, di.rcItem.bottom + 1, radius, radius);
            if (rgn != IntPtr.Zero) {
                int saved = Win32.SaveDC(di.hDC);
                Win32.SelectClipRgn(di.hDC, rgn);
                UiPaint.DrawVGradient(di.hDC, di.rcItem.left, di.rcItem.top,
                                      di.rcItem.right - di.rcItem.left,
                                      di.rcItem.bottom - di.rcItem.top, fillTop, fillBot);
                Win32.RestoreDC(di.hDC, saved);
                Win32.DeleteObject(rgn);
            } else {
                UiPaint.DrawVGradient(di.hDC, di.rcItem.left, di.rcItem.top,
                                      di.rcItem.right - di.rcItem.left,
                                      di.rcItem.bottom - di.rcItem.top, fillTop, fillBot);
            }
            IntPtr p = Win32.CreatePen(Win32.PS_SOLID, 1, edge);
            IntPtr ob = Win32.SelectObject(di.hDC, Win32.GetStockObject(Win32.HOLLOW_BRUSH));
            IntPtr op = Win32.SelectObject(di.hDC, p);
            Win32.RoundRect(di.hDC, di.rcItem.left, di.rcItem.top, di.rcItem.right, di.rcItem.bottom, radius, radius);
            Win32.SelectObject(di.hDC, ob);
            Win32.SelectObject(di.hDC, op);
            Win32.DeleteObject(p);

            /* 顶部内侧高光，强化玻璃质感（主按钮更明显） */
            {
                uint gloss = primary ? Mix(fillTop, Theme.C_TEXT, 0.35f) : Mix(edge, Theme.C_TEXT, 0.18f);
                IntPtr gl = Win32.CreatePen(Win32.PS_SOLID, 1, gloss);
                IntPtr ogl = Win32.SelectObject(di.hDC, gl);
                Win32.MoveToEx(di.hDC, di.rcItem.left + radius / 2 + Sc(2), di.rcItem.top + Sc(1), IntPtr.Zero);
                Win32.LineTo(di.hDC, di.rcItem.right - radius / 2 - Sc(2), di.rcItem.top + Sc(1));
                Win32.SelectObject(di.hDC, ogl);
                Win32.DeleteObject(gl);
            }

            if (focused && !pressed) {
                IntPtr focusPen = Win32.CreatePen(Win32.PS_SOLID, 1, Mix(edge, Theme.C_TEXT, 0.35f));
                IntPtr oldFocus = Win32.SelectObject(di.hDC, focusPen);
                IntPtr oldFocusBrush = Win32.SelectObject(di.hDC, Win32.GetStockObject(Win32.HOLLOW_BRUSH));
                Win32.RoundRect(di.hDC, di.rcItem.left + Sc(3), di.rcItem.top + Sc(3),
                                di.rcItem.right - Sc(3), di.rcItem.bottom - Sc(3), Sc(7), Sc(7));
                Win32.SelectObject(di.hDC, oldFocus);
                Win32.SelectObject(di.hDC, oldFocusBrush);
                Win32.DeleteObject(focusPen);
            }

            Win32.SetBkMode(di.hDC, Win32.TRANSPARENT);
            Win32.SetTextColor(di.hDC, fg);
            IntPtr buttonFont = clearCacheBtn ? Theme.FontSmall : Theme.FontBody;
            Win32.SelectObject(di.hDC, buttonFont);
            Win32.SIZE textSize;
            Win32.GetTextExtentPoint32W(di.hDC, text, text.Length, out textSize);
            int iconW = Sc(14);
            int gap = Sc(7);
            int groupW = iconW + gap + textSize.cx;
            int available = (di.rcItem.right - di.rcItem.left) - Sc(18);
            if (groupW > available) {
                Win32.RECT clipped = di.rcItem;
                Win32.InflateRect(ref clipped, -Sc(8), 0);
                Win32.DrawTextW(di.hDC, text, -1, ref clipped,
                                Win32.DT_CENTER | Win32.DT_VCENTER | Win32.DT_SINGLELINE |
                                Win32.DT_END_ELLIPSIS | Win32.DT_NOPREFIX);
                return;
            }
            int left = di.rcItem.left + ((di.rcItem.right - di.rcItem.left) - groupW) / 2;
            int offset = pressed ? Sc(1) : 0;
            bool iconDrawn = DrawButtonIcon(di.hDC, (int)di.CtlID,
                                            left + iconW / 2 + offset,
                                            (di.rcItem.top + di.rcItem.bottom) / 2 + offset, fg);
            Win32.RECT t = di.rcItem;
            if (iconDrawn) {
                t.left = left + iconW + gap + offset;
                t.right = t.left + textSize.cx + Sc(2);
                Win32.DrawTextW(di.hDC, text, -1, ref t,
                                Win32.DT_LEFT | Win32.DT_VCENTER | Win32.DT_SINGLELINE |
                                Win32.DT_END_ELLIPSIS | Win32.DT_NOPREFIX);
            } else {
                Win32.DrawTextW(di.hDC, text, -1, ref t,
                                Win32.DT_CENTER | Win32.DT_VCENTER | Win32.DT_SINGLELINE |
                                Win32.DT_END_ELLIPSIS | Win32.DT_NOPREFIX);
            }
        }
    }
}
