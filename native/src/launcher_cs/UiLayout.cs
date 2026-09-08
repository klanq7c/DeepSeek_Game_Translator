using System;

namespace DstLauncher
{
    /*
     * UiLayout —— native/src/launcher/ui.c 的 UiLayout / compute_layout / layout /
     * tick_ui_animation 移植。字段名与 C 版结构体一致，--ui-probe 会逐字段打印出来
     * 与 C 版对比，所以这里不做"更好看"的重命名或合并。
     */
    internal struct UiLayout
    {
        internal int rail;
        internal int pad;
        internal int x;
        internal int w;
        internal int hero_right;
        internal Win32.RECT picker;
        internal int path_label_x;
        internal int path_label_y;
        internal int path_label_w;
        internal int path_label_h;
        internal int path_x;
        internal int path_y;
        internal int path_w;
        internal int path_h;
        internal int side_button_w;
        internal int side_gap;
        internal int browse_x;
        internal int open_x;
        internal int action_y;
        internal int action_h;
        internal int action_gap;
        internal int action_button_w;
        internal int action_x0, action_x1, action_x2, action_x3;
        internal int metric_gap;
        internal int metric_w;
        internal int metric_y;
        internal int metric_h;
        internal int metric_value_y;
        internal int metric_value_h;
        internal int log_top;
        internal int log_bottom;
        internal int log_edit_top;
        internal int log_edit_h;

        internal int ActionX(int i)
        {
            switch (i) {
                case 0: return action_x0;
                case 1: return action_x1;
                case 2: return action_x2;
                default: return action_x3;
            }
        }

        private static int Sc(int v) { return Theme.Sc(v); }

        private static int MaxI(int a, int b) { return a > b ? a : b; }

        /* 根据窗口尺寸和 DPI 计算所有 UI 元素坐标（compute_layout）。 */
        internal static UiLayout Compute(IntPtr hwnd)
        {
            Win32.RECT r;
            Win32.GetClientRect(hwnd, out r);

            var ui = new UiLayout();
            ui.rail = Sc(Theme.RAIL_W);
            ui.pad = Sc(28);
            ui.x = ui.rail + ui.pad;
            ui.w = MaxI(r.right - ui.x - ui.pad, Sc(560));
            ui.hero_right = Sc(220);

            int pickerTop = Sc(136);
            int pickerH = Sc(188);
            ui.picker.left = ui.x;
            ui.picker.top = pickerTop;
            ui.picker.right = ui.x + ui.w;
            ui.picker.bottom = pickerTop + pickerH;

            ui.path_label_x = ui.x + Sc(24);
            ui.path_label_y = pickerTop + Sc(24);
            ui.path_label_w = Sc(240);
            ui.path_label_h = Sc(24);

            ui.path_x = ui.x + Sc(24);
            ui.path_y = pickerTop + Sc(72);
            ui.path_h = Sc(38);
            ui.side_button_w = Sc(112);
            ui.side_gap = Sc(12);
            ui.open_x = ui.x + ui.w - Sc(24) - ui.side_button_w;
            ui.browse_x = ui.open_x - ui.side_gap - ui.side_button_w;
            ui.path_w = MaxI(ui.browse_x - ui.path_x - ui.side_gap, Sc(180));

            ui.action_y = pickerTop + Sc(128);
            ui.action_h = Sc(42);
            ui.action_gap = Sc(12);
            int actionInnerW = ui.w - Sc(48);
            ui.action_button_w = (actionInnerW - ui.action_gap * 3) / 4;
            int actionLeft = ui.x + Sc(24);
            ui.action_x0 = actionLeft;
            ui.action_x1 = actionLeft + 1 * (ui.action_button_w + ui.action_gap);
            ui.action_x2 = actionLeft + 2 * (ui.action_button_w + ui.action_gap);
            ui.action_x3 = actionLeft + 3 * (ui.action_button_w + ui.action_gap);

            ui.metric_gap = Sc(16);
            ui.metric_w = MaxI((ui.w - ui.metric_gap * 2) / 3, Sc(150));
            ui.metric_y = ui.picker.bottom + Sc(20);
            ui.metric_h = Sc(100);
            ui.metric_value_y = ui.metric_y + Sc(40);
            ui.metric_value_h = Sc(34);

            ui.log_top = ui.metric_y + ui.metric_h + Sc(20);
            ui.log_bottom = MaxI(r.bottom - Sc(24), ui.log_top + Sc(96));
            ui.log_edit_top = ui.log_top + Sc(56);
            ui.log_edit_h = MaxI(ui.log_bottom - Sc(18) - ui.log_edit_top, Sc(60));

            return ui;
        }

        private static void PositionControl(IntPtr ctl, int x, int y, int w, int h, uint flags)
        {
            if (ctl != IntPtr.Zero && Win32.IsWindow(ctl)) {
                Win32.SetWindowPos(ctl, IntPtr.Zero, x, y, w, h, flags);
            }
        }

        /* 根据计算好的布局移动所有子控件（layout）。 */
        internal static void Apply(IntPtr hwnd)
        {
            UiLayout ui = Compute(hwnd);
            int x = ui.x;
            int w = ui.w;

            const uint positionFlags = Win32.SWP_NOZORDER | Win32.SWP_NOACTIVATE | Win32.SWP_NOREDRAW;

            /* 英雄标题带 */
            int heroRight = ui.hero_right;
            PositionControl(MainWindow.Title, x, Sc(18), w - heroRight, Sc(44), positionFlags);
            PositionControl(MainWindow.Subtitle, x, Sc(66), w - heroRight, Sc(22), positionFlags);
            PositionControl(MainWindow.Status, x, Sc(94), w - heroRight, Sc(22), positionFlags);

            /* 选择器卡片内容 */
            PositionControl(MainWindow.PathLabel, ui.path_label_x, ui.path_label_y, ui.path_label_w, ui.path_label_h, positionFlags);
            PositionControl(MainWindow.Path, ui.path_x, ui.path_y, ui.path_w, ui.path_h, positionFlags);
            PositionControl(Win32.GetDlgItem(hwnd, Theme.IDC_BROWSE), ui.browse_x, ui.path_y, ui.side_button_w, ui.path_h, positionFlags);
            PositionControl(Win32.GetDlgItem(hwnd, Theme.IDC_OPEN), ui.open_x, ui.path_y, ui.side_button_w, ui.path_h, positionFlags);

            /* 操作按钮行 */
            int abY = ui.action_y;
            int abH = ui.action_h;
            PositionControl(Win32.GetDlgItem(hwnd, Theme.IDC_START), ui.action_x0, abY, ui.action_button_w, abH, positionFlags);
            PositionControl(MainWindow.BtnRestore, ui.action_x1, abY, ui.action_button_w, abH, positionFlags);
            PositionControl(MainWindow.BtnServer, ui.action_x2, abY, ui.action_button_w, abH, positionFlags);
            int apiW = x + w - Sc(24) - ui.action_x3;
            PositionControl(MainWindow.BtnApi, ui.action_x3, abY, apiW, abH, positionFlags);

            /* 指标卡片：值定位在每张卡片下半部 */
            int gap = ui.metric_gap;
            int cardW = ui.metric_w;
            int mValY = ui.metric_value_y;
            int mValH = ui.metric_value_h;
            PositionControl(MainWindow.EngineLabel, x + Sc(18), mValY, cardW - Sc(30), mValH, positionFlags);
            PositionControl(MainWindow.ServerLabel, x + cardW + gap + Sc(18), mValY, cardW - Sc(30), mValH, positionFlags);
            int cacheX = x + (cardW + gap) * 2;
            int clearW = Sc(88);
            PositionControl(MainWindow.CacheLabel, cacheX + Sc(18), mValY, cardW - clearW - Sc(42), mValH, positionFlags);
            PositionControl(MainWindow.BtnClearCache, cacheX + cardW - clearW - Sc(14), mValY + Sc(2), clearW, mValH - Sc(4), positionFlags);

            /* 日志卡片内的日志内容区（标题栏由背景绘制） */
            PositionControl(MainWindow.LogList, x + Sc(18), ui.log_edit_top, w - Sc(36), ui.log_edit_h, positionFlags);

            Win32.RedrawWindow(hwnd, IntPtr.Zero, IntPtr.Zero,
                               Win32.RDW_INVALIDATE | Win32.RDW_ALLCHILDREN | Win32.RDW_NOERASE);
        }

        /* 动画心跳（约 60fps），对应 tick_ui_animation：只失效小条带区域。 */
        internal static void TickAnimation(IntPtr hwnd)
        {
            if (hwnd == IntPtr.Zero || !Win32.IsWindow(hwnd) || Win32.IsIconic(hwnd)) return;
            UiLayout ui = Compute(hwnd);
            Win32.RECT client;
            Win32.GetClientRect(hwnd, out client);

            /* 数据线条带：起始于状态控件之下 */
            var dataLine = new Win32.RECT(ui.x, Sc(116), ui.x + ui.w, Sc(122));
            Win32.InvalidateRect(hwnd, ref dataLine, false);

            /* 状态药丸指示灯呼吸区（药丸左端圆点一带） */
            int pillW = Sc(176);
            int pillX = client.right - ui.pad - pillW;
            var pillDot = new Win32.RECT(pillX, Sc(30), pillX + Sc(46), Sc(30) + Sc(36));
            Win32.InvalidateRect(hwnd, ref pillDot, false);

            /* 日志卡 LIVE 呼吸灯区（自绘圆点，不含文字） */
            int liveCx = ui.x + ui.w - Sc(74);
            int liveCy = ui.log_top + Sc(12) + Sc(12);
            var liveDot = new Win32.RECT(liveCx - Sc(13), liveCy - Sc(11), liveCx + Sc(13), liveCy + Sc(11));
            Win32.InvalidateRect(hwnd, ref liveDot, false);

            /* 按钮悬停渐变：指数缓动逼近目标，过渡期间重绘按钮 */
            UiButtons.TickHover(hwnd);
        }
    }
}
