using System;

namespace DstLauncher
{
    /*
     * UiPaint —— native/src/launcher/ui.c 的绘制原语与 paint_background 移植。
     *
     * 每个函数都逐行对应 C 版同名函数：同样的 GDI 调用、同样的顺序、同样的取整。
     * 浮点一律用 float 且不合并运算，x86-64 上 C（SSE）与 C#（SSE）结果逐位相同。
     * 有意保留 C 版的对象创建/选择/删除节奏，因为选入顺序会影响 RoundRect 的边框像素。
     */
    internal static class UiPaint
    {
        private static int Sc(int v) { return Theme.Sc(v); }
        private static uint Mix(uint a, uint b, float t) { return Theme.Mix(a, b, t); }

        /* 绘制圆角矩形（指定填充色和边框色） */
        internal static void DrawRound(IntPtr dc, Win32.RECT rc, uint fill, uint line, int radius)
        {
            IntPtr b = Win32.CreateSolidBrush(fill);
            IntPtr p = Win32.CreatePen(Win32.PS_SOLID, 1, line);
            IntPtr ob = Win32.SelectObject(dc, b);
            IntPtr op = Win32.SelectObject(dc, p);
            Win32.RoundRect(dc, rc.left, rc.top, rc.right, rc.bottom, radius, radius);
            Win32.SelectObject(dc, ob);
            Win32.SelectObject(dc, op);
            Win32.DeleteObject(b);
            Win32.DeleteObject(p);
        }

        /* 绘制带光晕的圆点（用于状态指示灯、列表标记） */
        internal static void DrawDot(IntPtr dc, int cx, int cy, int radius, uint fill, uint halo)
        {
            IntPtr bh = Win32.CreateSolidBrush(halo);
            IntPtr ob = Win32.SelectObject(dc, bh);
            IntPtr op = Win32.SelectObject(dc, Win32.GetStockObject(Win32.NULL_PEN));
            Win32.Ellipse(dc, cx - radius - 3, cy - radius - 3, cx + radius + 3, cy + radius + 3);
            IntPtr bf = Win32.CreateSolidBrush(fill);
            Win32.SelectObject(dc, bf);
            Win32.Ellipse(dc, cx - radius, cy - radius, cx + radius, cy + radius);
            Win32.SelectObject(dc, ob);
            Win32.SelectObject(dc, op);
            Win32.DeleteObject(bh);
            Win32.DeleteObject(bf);
        }

        /* 在指定矩形内绘制文本（设置字体和颜色） */
        internal static void DrawTextX(IntPtr dc, string text, int x, int y, int w, int h,
                                       uint color, IntPtr font, uint flags)
        {
            Win32.SelectObject(dc, font);
            Win32.SetTextColor(dc, color);
            var r = new Win32.RECT(x, y, x + w, y + h);
            Win32.DrawTextW(dc, text, -1, ref r, flags);
        }

        /* 绘制垂直渐变填充（top→bottom） */
        internal static void DrawVGradient(IntPtr dc, int x, int y, int w, int h, uint top, uint bot)
        {
            var v = new Win32.TRIVERTEX[2];
            v[0].x = x; v[0].y = y;
            v[0].Red = (ushort)(Win32.GetRValue(top) << 8);
            v[0].Green = (ushort)(Win32.GetGValue(top) << 8);
            v[0].Blue = (ushort)(Win32.GetBValue(top) << 8);
            v[1].x = x + w; v[1].y = y + h;
            v[1].Red = (ushort)(Win32.GetRValue(bot) << 8);
            v[1].Green = (ushort)(Win32.GetGValue(bot) << 8);
            v[1].Blue = (ushort)(Win32.GetBValue(bot) << 8);
            var gr = new Win32.GRADIENT_RECT { UpperLeft = 0, LowerRight = 1 };
            Win32.GradientFill(dc, v, 2, ref gr, 1, Win32.GRADIENT_FILL_RECT_V);
        }

        /* 绘制水平渐变填充（left→right） */
        internal static void DrawHGradient(IntPtr dc, int x, int y, int w, int h, uint left, uint right)
        {
            var v = new Win32.TRIVERTEX[2];
            v[0].x = x; v[0].y = y;
            v[0].Red = (ushort)(Win32.GetRValue(left) << 8);
            v[0].Green = (ushort)(Win32.GetGValue(left) << 8);
            v[0].Blue = (ushort)(Win32.GetBValue(left) << 8);
            v[1].x = x + w; v[1].y = y + h;
            v[1].Red = (ushort)(Win32.GetRValue(right) << 8);
            v[1].Green = (ushort)(Win32.GetGValue(right) << 8);
            v[1].Blue = (ushort)(Win32.GetBValue(right) << 8);
            var gr = new Win32.GRADIENT_RECT { UpperLeft = 0, LowerRight = 1 };
            Win32.GradientFill(dc, v, 2, ref gr, 1, Win32.GRADIENT_FILL_RECT_H);
        }

        /* ---- 卡片内静态文字的不透明底色画刷（对应 card_text_brush） ---- */
        private static IntPtr _cardBrushLabel = IntPtr.Zero;
        private static IntPtr _cardBrushValue = IntPtr.Zero;

        internal static IntPtr CardTextBrush(bool pickerLabel, out uint outColor)
        {
            uint col = Theme.C_CARD;
            outColor = col;
            if (pickerLabel) {
                if (_cardBrushLabel == IntPtr.Zero) _cardBrushLabel = Win32.CreateSolidBrush(col);
                return _cardBrushLabel;
            }
            if (_cardBrushValue == IntPtr.Zero) _cardBrushValue = Win32.CreateSolidBrush(col);
            return _cardBrushValue;
        }

        internal static void FreeCardTextBrushes()
        {
            if (_cardBrushLabel != IntPtr.Zero) { Win32.DeleteObject(_cardBrushLabel); _cardBrushLabel = IntPtr.Zero; }
            if (_cardBrushValue != IntPtr.Zero) { Win32.DeleteObject(_cardBrushValue); _cardBrushValue = IntPtr.Zero; }
        }

        /* 0..1 呼吸脉冲（正弦，周期 period_ms 毫秒） */
        private static float Pulse01(uint periodMs)
        {
            uint now = Theme.AnimTick() % periodMs;
            return 0.5f + 0.5f * (float)Math.Sin((float)now / (float)periodMs * 6.2831853f - 1.5707963f);
        }

        /* 暖白卡片：薄边框与轻柔阴影 */
        private static void DrawPanelShell(IntPtr dc, Win32.RECT rc, uint accent, bool elevated) {
            Win32.RECT shadow = rc;
            Win32.OffsetRect(ref shadow, 0, Sc(2));
            DrawRound(dc, shadow, Win32.RGB(231, 224, 210), Win32.RGB(231, 224, 210), Sc(18));
            DrawRound(dc, rc, Theme.C_CARD, Theme.C_LINE, Sc(18));
        }

        /* 英雄区数据线：发丝基线 + 正弦缓动往返的渐隐光束，尾部带暖灰残影。 */
        private static void DrawHeroDataLine(IntPtr dc, int left, int right, int y, uint base_)
        {
            IntPtr basePen = Win32.CreatePen(Win32.PS_SOLID, 1, Mix(Theme.C_LINE, base_, 0.35f));
            IntPtr old = Win32.SelectObject(dc, basePen);
            Win32.MoveToEx(dc, left, y, IntPtr.Zero);
            Win32.LineTo(dc, right, y);
            Win32.SelectObject(dc, old);
            Win32.DeleteObject(basePen);

            int width = right - left;
            if (width <= 0) return;

            /* 9 秒一个往返，cos 缓动让光束两端减速，运动平滑 */
            uint now = Theme.AnimTick();
            float phase = (float)(now % 9000u) / 9000.0f;
            float eased = 0.5f - 0.5f * (float)Math.Cos(phase * 6.2831853f);
            int beamW = Sc(96);
            int travel = width - beamW;
            if (travel < 1) travel = 1;
            int bx = left + (int)(eased * (float)travel);

            /* 暖灰拖尾（暗一档、短一截，反方向偏移） */
            int tailW = Sc(40);
            int tailX = bx - (int)((eased - 0.5f) * (float)Sc(48));
            DrawHGradient(dc, tailX - tailW / 2, y, tailW / 2, 1, base_, Mix(Theme.C_VIOLET, base_, 0.55f));
            DrawHGradient(dc, tailX, y, tailW / 2, 1, Mix(Theme.C_VIOLET, base_, 0.55f), base_);

            /* 主光束：两端向背景色渐隐 */
            int fade = Sc(28);
            int core = beamW - fade * 2;
            if (core < 1) core = 1;
            DrawHGradient(dc, bx, y - 1, fade, 2, base_, Theme.C_ACCENT);
            IntPtr cb = Win32.CreateSolidBrush(Theme.C_ACCENT);
            var coreRc = new Win32.RECT(bx + fade, y - 1, bx + fade + core, y + 1);
            Win32.FillRect(dc, ref coreRc, cb);
            Win32.DeleteObject(cb);
            DrawHGradient(dc, bx + fade + core, y - 1, fade, 2, Theme.C_ACCENT, base_);
        }

        /* ----------------------------------------------------------------
         * PaintBackground — 绘制主窗口背景（对应 paint_background）
         * ---------------------------------------------------------------- */
        /* 星轨以固定种子生成，30 秒缓慢流动；只绘制右侧无子控件的插画区域。 */
        private static void DrawNocturneSky(IntPtr dc, int left, int top, int right, int bottom) {
            var bounds = new Win32.RECT(left, top, right, bottom);
            if (!Win32.RectVisible(dc, ref bounds)) return;
            int saved = Win32.SaveDC(dc);
            Win32.IntersectClipRect(dc, left, top, right, bottom);
            int width = right - left;
            int height = bottom - top;
            double phase = (double)(Theme.AnimTick() % 30000u) / 30000.0 * 6.283185307179586;
            int cx = left + width * 3 / 5;
            int cy = top + height / 2;
            IntPtr old_pen = Win32.SelectObject(dc, Win32.GetStockObject(Win32.NULL_PEN));
            /* 暗蓝色光晕与成束笔触形成星云，不使用外部图像或额外后台线程。 */
            for (int i = 9; i >= 1; i--) {
                int rx = Sc(12 + i * 15);
                int ry = Sc(6 + i * 7);
                IntPtr b = Win32.CreateSolidBrush(Mix(Theme.C_PAGE, Win32.RGB(27, 76, 123), (float)(10 - i) * 0.035f));
                IntPtr ob = Win32.SelectObject(dc, b);
                Win32.Ellipse(dc, cx - rx, cy - ry, cx + rx, cy + ry);
                Win32.SelectObject(dc, ob);
                Win32.DeleteObject(b);
            }
            Win32.SelectObject(dc, old_pen);
            for (int band = 0; band < 14; band++) {
                uint color = Mix(Theme.C_PAGE, band % 4 == 0 ? Theme.C_ACCENT : Win32.RGB(68, 126, 173), band % 4 == 0 ? 0.42f : 0.38f);
                IntPtr p = Win32.CreatePen(Win32.PS_SOLID, 1, color);
                IntPtr op = Win32.SelectObject(dc, p);
                for (int j = 0; j <= 70; j++) {
                    double angle = (double)j * 0.085 + (double)band * 0.026 + Math.Sin(phase) * 0.12;
                    double radius = (double)Sc(15 + band * 7) + (double)j * (double)Sc(1) * 0.60;
                    int px = cx + (int)(Math.Cos(angle) * radius * 1.40);
                    int py = cy + (int)(Math.Sin(angle) * radius * 0.48);
                    if (j == 0) Win32.MoveToEx(dc, px, py, IntPtr.Zero);
                    else Win32.LineTo(dc, px, py);
                }
                Win32.SelectObject(dc, op);
                Win32.DeleteObject(p);
            }
            for (int i = 0; i < 38; i++) {
                int sx = left + (width * ((i * 73 + 19) % 997)) / 997;
                int sy = top + Sc(6) + ((height - Sc(12)) * ((i * 151 + 61) % 991)) / 991;
                float glow = 0.46f + (float)(0.18 * Math.Sin(phase + (double)i));
                uint star = Mix(Theme.C_PAGE, Theme.C_ACCENT, glow);
                IntPtr p = Win32.CreatePen(Win32.PS_SOLID, 1, star);
                IntPtr op = Win32.SelectObject(dc, p);
                int size = Sc(i % 9 == 0 ? 4 : 1);
                Win32.MoveToEx(dc, sx - size, sy, IntPtr.Zero); Win32.LineTo(dc, sx + size + 1, sy);
                Win32.MoveToEx(dc, sx, sy - size, IntPtr.Zero); Win32.LineTo(dc, sx, sy + size + 1);
                Win32.SelectObject(dc, op);
                Win32.DeleteObject(p);
            }
            /* 月亮使用实心暖金色与夜色遮片，不叠加高频闪烁。 */
            IntPtr moonPen = Win32.SelectObject(dc, Win32.GetStockObject(Win32.NULL_PEN));
            int mx = right - Sc(62), my = top + Sc(36);
            IntPtr moon = Win32.CreateSolidBrush(Theme.C_ACCENT);
            IntPtr moonBrush = Win32.SelectObject(dc, moon);
            Win32.Ellipse(dc, mx - Sc(15), my - Sc(15), mx + Sc(15), my + Sc(15));
            IntPtr shade = Win32.CreateSolidBrush(Theme.C_PAGE);
            Win32.SelectObject(dc, shade);
            Win32.Ellipse(dc, mx - Sc(6), my - Sc(19), mx + Sc(20), my + Sc(7));
            Win32.SelectObject(dc, moonBrush); Win32.SelectObject(dc, moonPen);
            Win32.DeleteObject(moon); Win32.DeleteObject(shade);
            Win32.RestoreDC(dc, saved);
        }

        /* 星夜首页：深蓝主题区、暖白操作区，以及独立的状态与活动记录。 */
        internal static void PaintBackground(IntPtr hwnd, IntPtr dc) {
            Win32.RECT r;
            Win32.GetClientRect(hwnd, out r);
            UiLayout ui = UiLayout.Compute(hwnd);
            int x = ui.x, w = ui.w;
            bool alive = ServerProcess.Started;
            DrawVGradient(dc, 0, 0, r.right, r.bottom, Theme.C_PAGE, Theme.C_PAGE);
            DrawVGradient(dc, 0, Sc(246), r.right, r.bottom - Sc(246), Win32.RGB(245, 240, 229), Win32.RGB(249, 245, 235));
            Win32.SetBkMode(dc, Win32.TRANSPARENT);

            IntPtr icon = Win32.LoadImageW(MainWindow.Instance, new IntPtr(Theme.IDI_APP_ICON), Win32.IMAGE_ICON, Sc(28), Sc(28), Win32.LR_SHARED);
            if (icon != IntPtr.Zero) Win32.DrawIconEx(dc, x, Sc(20), icon, Sc(28), Sc(28), 0, IntPtr.Zero, Win32.DI_NORMAL);
            DrawTextX(dc, "ds 游戏翻译器", x + Sc(40), Sc(17), Sc(190), Sc(24), Win32.RGB(245, 240, 229), Theme.FontHeading, Win32.DT_LEFT | Win32.DT_SINGLELINE | Win32.DT_VCENTER);
            DrawTextX(dc, "每一段故事，都值得抵达", x + Sc(40), Sc(40), Sc(260), Sc(16), Win32.RGB(162, 180, 200), Theme.FontSmall, Win32.DT_LEFT | Win32.DT_SINGLELINE | Win32.DT_VCENTER);
            DrawTextX(dc, "Ren'Py   /   RPG Maker   /   Unity   /   Godot", x + w - Sc(480), Sc(24), Sc(290), Sc(24), Win32.RGB(174, 190, 207), Theme.FontSmall, Win32.DT_RIGHT | Win32.DT_SINGLELINE | Win32.DT_VCENTER);
            Win32.RECT pill = new Win32.RECT(x + w - Sc(166), Sc(21), x + w, Sc(53));
            DrawRound(dc, pill, Theme.C_PAGE, Win32.RGB(70, 81, 96), Sc(32));
            uint status_color = alive ? Win32.RGB(148, 206, 166) : Theme.C_ACCENT;
            DrawDot(dc, pill.left + Sc(18), Sc(37), Sc(3), status_color, Mix(Theme.C_PAGE, status_color, 0.20f + 0.12f * Pulse01(4200)));
            DrawTextX(dc, alive ? "翻译服务已就绪" : "等待开始", pill.left + Sc(34), pill.top, Sc(120), Sc(32), status_color, Theme.FontSmall, Win32.DT_LEFT | Win32.DT_SINGLELINE | Win32.DT_VCENTER);
            IntPtr line = Win32.CreatePen(Win32.PS_SOLID, 1, Win32.RGB(38, 54, 76));
            IntPtr old = Win32.SelectObject(dc, line);
            Win32.MoveToEx(dc, x, Sc(70), IntPtr.Zero); Win32.LineTo(dc, x + w, Sc(70));
            Win32.SelectObject(dc, old); Win32.DeleteObject(line);

            DrawTextX(dc, "游 戏 无 界  /  PLAY BEYOND WORDS", x, Sc(84), Sc(480), Sc(20), Theme.C_ACCENT, Theme.FontSmall, Win32.DT_LEFT | Win32.DT_SINGLELINE | Win32.DT_VCENTER);
            DrawNocturneSky(dc, x + w - ui.hero_right, Sc(75), x + w, Sc(240));
            DrawHeroDataLine(dc, x, x + Sc(208), Sc(232), Theme.C_PAGE);

            Win32.RECT clip;
            Win32.GetClipBox(dc, out clip);
            if (clip.bottom <= Sc(246)) return;
            DrawPanelShell(dc, ui.picker, Theme.C_ACCENT, false);
            DrawTextX(dc, "拖入游戏 EXE 或文件夹，即可开始汉化", x + Sc(200), ui.picker.top + Sc(18), w - Sc(224), Sc(24), Theme.C_MUTED, Theme.FontSmall, Win32.DT_RIGHT | Win32.DT_SINGLELINE | Win32.DT_VCENTER);
            string[] labels = {"游戏引擎", "本地服务", "翻译记忆"};
            string[] numbers = {"01", "02", "03"};
            for (int i = 0; i < 3; i++) {
                int cx = x + (ui.metric_w + ui.metric_gap) * i;
                Win32.RECT card = new Win32.RECT(cx, ui.metric_y, cx + ui.metric_w, ui.metric_y + ui.metric_h);
                DrawPanelShell(dc, card, Theme.C_ACCENT, false);
                DrawTextX(dc, labels[i], cx + Sc(18), ui.metric_y + Sc(12), ui.metric_w - Sc(70), Sc(20), Theme.C_TEXT_DIM, Theme.FontSmall, Win32.DT_LEFT | Win32.DT_SINGLELINE | Win32.DT_VCENTER);
                DrawTextX(dc, numbers[i], cx + ui.metric_w - Sc(48), ui.metric_y + Sc(12), Sc(28), Sc(20), Theme.C_AMBER, Theme.FontMonoSmall, Win32.DT_RIGHT | Win32.DT_SINGLELINE | Win32.DT_VCENTER);
            }
            Win32.RECT log_card = new Win32.RECT(x, ui.log_top, x + w, ui.log_bottom);
            DrawPanelShell(dc, log_card, Theme.C_ACCENT, false);
            DrawTextX(dc, "旅程记录", x + Sc(18), ui.log_top + Sc(10), Sc(110), Sc(24), Theme.C_TEXT, Theme.FontHeading, Win32.DT_LEFT | Win32.DT_SINGLELINE | Win32.DT_VCENTER);
            DrawTextX(dc, "准备、部署与翻译进度，都在这里。", x + Sc(124), ui.log_top + Sc(10), w - Sc(380), Sc(24), Theme.C_MUTED, Theme.FontSmall, Win32.DT_LEFT | Win32.DT_SINGLELINE | Win32.DT_VCENTER);
            DrawTextX(dc, MainWindow.RuntimeTag, x + w - Sc(190), ui.log_top + Sc(10), Sc(170), Sc(24), Theme.C_MUTED, Theme.FontSmall, Win32.DT_RIGHT | Win32.DT_SINGLELINE | Win32.DT_VCENTER);
            DrawTextX(dc, "本地记忆  ·  即时翻译  ·  保留原有格式", x, r.bottom - Sc(22), Sc(430), Sc(16), Theme.C_TEXT_DIM, Theme.FontSmall, Win32.DT_LEFT | Win32.DT_SINGLELINE | Win32.DT_VCENTER);
            DrawTextX(dc, "v" + BuildInfo.TranslatorVersion, x + w - Sc(130), r.bottom - Sc(22), Sc(130), Sc(16), Theme.C_AMBER, Theme.FontMonoSmall, Win32.DT_RIGHT | Win32.DT_SINGLELINE | Win32.DT_VCENTER);
            Win32.RECT border = new Win32.RECT(ui.path_x - Sc(2), ui.path_y - Sc(2), ui.path_x + ui.path_w + Sc(2), ui.path_y + ui.path_h + Sc(2));
            DrawRound(dc, border, Theme.C_LOG, Theme.C_LINE, Sc(10));
        }

        /* 缓冲只归主窗口的 UI 线程所有；每次重绘脏区，不缓存旧画面。 */
        private static IntPtr _backgroundDc, _backgroundBitmap, _backgroundOwner;
        private static int _backgroundWidth, _backgroundHeight;
        private static uint _backgroundFailures;

        private static void ReportBackgroundFailure()
        {
            /* Windows 的 GDI 资源不足/设备失效无法由上游保证；直接绘制可能恢复
               闪烁，不能算双缓冲成功。首次及 2 的幂次失败向调试器记录，避免刷屏。 */
            uint count = unchecked(++_backgroundFailures);
            if (count == 1 || (count & (count - 1)) == 0)
                Win32.OutputDebugStringW("ds launcher: GDI back buffer operation failed; using direct paint when needed.\n");
        }

        internal static void FreeBackgroundBuffer()
        {
            // 先销毁 DC，解除所选对象和保存状态，再删除自有位图。
            if (_backgroundDc != IntPtr.Zero && !Win32.DeleteDC(_backgroundDc)) ReportBackgroundFailure();
            if (_backgroundBitmap != IntPtr.Zero && !Win32.DeleteObject(_backgroundBitmap)) ReportBackgroundFailure();
            _backgroundDc = _backgroundBitmap = _backgroundOwner = IntPtr.Zero;
            _backgroundWidth = _backgroundHeight = 0;
        }

        /* 脏矩形双缓冲绘制，对应 paint_background_buffered。 */
        internal static void PaintBackgroundBuffered(IntPtr hwnd, IntPtr dc, Win32.RECT dirty)
        {
            if (dirty.right <= dirty.left || dirty.bottom <= dirty.top) return;

            int width = dirty.right - dirty.left;
            int height = dirty.bottom - dirty.top;
            if (_backgroundOwner != hwnd || width > _backgroundWidth || height > _backgroundHeight) {
                int capacityWidth = _backgroundOwner == hwnd && _backgroundWidth > width ? _backgroundWidth : width;
                int capacityHeight = _backgroundOwner == hwnd && _backgroundHeight > height ? _backgroundHeight : height;
                FreeBackgroundBuffer();
                _backgroundDc = Win32.CreateCompatibleDC(dc);
                _backgroundBitmap = _backgroundDc != IntPtr.Zero ? Win32.CreateCompatibleBitmap(dc, capacityWidth, capacityHeight) : IntPtr.Zero;
                if (_backgroundDc == IntPtr.Zero || _backgroundBitmap == IntPtr.Zero) goto Failed;
                IntPtr previous = Win32.SelectObject(_backgroundDc, _backgroundBitmap);
                if (previous == IntPtr.Zero || previous == new IntPtr(-1)) goto Failed;
                _backgroundOwner = hwnd;
                _backgroundWidth = capacityWidth;
                _backgroundHeight = capacityHeight;
            }

            int saved = Win32.SaveDC(_backgroundDc);
            if (saved == 0 || !Win32.SetViewportOrgEx(_backgroundDc, -dirty.left, -dirty.top, IntPtr.Zero) ||
                Win32.IntersectClipRect(_backgroundDc, dirty.left, dirty.top, dirty.right, dirty.bottom) == 0) goto Failed;
            PaintBackground(hwnd, _backgroundDc);
            if (!Win32.RestoreDC(_backgroundDc, saved) ||
                !Win32.BitBlt(dc, dirty.left, dirty.top, width, height, _backgroundDc, 0, 0, Win32.SRCCOPY)) goto Failed;
            return;

        Failed:
            ReportBackgroundFailure();
            FreeBackgroundBuffer();
            PaintBackground(hwnd, dc);
        }
    }
}
