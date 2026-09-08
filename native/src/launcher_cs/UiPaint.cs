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

        /* 细腻的深色科技网格——大间距、低对比，仅作背景纹理 */
        private static void DrawTechGrid(IntPtr dc, int left, int top, int right, int bottom)
        {
            IntPtr pen = Win32.CreatePen(Win32.PS_SOLID, 1, Win32.RGB(14, 20, 30));
            IntPtr old = Win32.SelectObject(dc, pen);
            int step = Sc(56);
            for (int x = left + step; x < right; x += step) {
                Win32.MoveToEx(dc, x, top, IntPtr.Zero);
                Win32.LineTo(dc, x, bottom);
            }
            for (int y = top + step; y < bottom; y += step) {
                Win32.MoveToEx(dc, left, y, IntPtr.Zero);
                Win32.LineTo(dc, right, y);
            }
            Win32.SelectObject(dc, old);
            Win32.DeleteObject(pen);
        }

        /* 页面背景色：顶部标题区与 C_PAGE 一致，向下缓慢加深 */
        private static uint PageColorAt(int y, int height)
        {
            if (height < 1) height = 1;
            int flatTop = Sc(116);
            float t = (float)(y - flatTop) / (float)(height - flatTop);
            if (t < 0.0f) t = 0.0f;
            if (t > 1.0f) t = 1.0f;
            return Mix(Theme.C_PAGE, Win32.RGB(3, 5, 8), t * 0.85f);
        }

        /* ---- 卡片内静态文字的不透明底色画刷（对应 card_text_brush） ---- */
        private static IntPtr _cardBrushLabel = IntPtr.Zero;
        private static IntPtr _cardBrushValue = IntPtr.Zero;

        internal static IntPtr CardTextBrush(bool pickerLabel, out uint outColor)
        {
            uint topCol = Mix(Theme.C_CARD, Theme.C_CARD_ELEV, 0.45f);
            uint col = pickerLabel ? Mix(topCol, Theme.C_CARD, 0.19f)
                                   : Mix(topCol, Theme.C_CARD, 0.57f);
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

        /* 柔光光晕：同心椭圆由外到内逐步接近光源色（GDI 无 alpha，用预混合模拟弥散） */
        private static void DrawSoftGlow(IntPtr dc, int cx, int cy, int radius, uint color, uint base_)
        {
            const int steps = 6;
            IntPtr oldPen = Win32.SelectObject(dc, Win32.GetStockObject(Win32.NULL_PEN));
            for (int i = steps; i >= 1; i--) {
                float f = (float)i / (float)steps;          /* 1 = 最外圈 */
                float k = 0.20f * f * f;                    /* 内圈最强 */
                int rr = (int)((float)radius * (1.05f - f) + (float)radius * 0.05f);
                if (rr < 1) rr = 1;
                IntPtr b = Win32.CreateSolidBrush(Mix(base_, color, k));
                IntPtr ob = Win32.SelectObject(dc, b);
                Win32.Ellipse(dc, cx - rr, cy - rr, cx + rr, cy + rr);
                Win32.SelectObject(dc, ob);
                Win32.DeleteObject(b);
            }
            Win32.SelectObject(dc, oldPen);
        }

        /* 渐变圆角面板：圆角裁剪 + 垂直渐变填充 + 发丝边框 + 顶部高光 */
        internal static void DrawPanelGradient(IntPtr dc, Win32.RECT rc, uint top, uint bot, uint line, int radius)
        {
            IntPtr rgn = Win32.CreateRoundRectRgn(rc.left, rc.top, rc.right + 1, rc.bottom + 1, radius, radius);
            if (rgn != IntPtr.Zero) {
                int saved = Win32.SaveDC(dc);
                Win32.SelectClipRgn(dc, rgn);
                DrawVGradient(dc, rc.left, rc.top, rc.right - rc.left, rc.bottom - rc.top, top, bot);
                Win32.RestoreDC(dc, saved);
                Win32.DeleteObject(rgn);
            } else {
                DrawVGradient(dc, rc.left, rc.top, rc.right - rc.left, rc.bottom - rc.top, top, bot);
            }

            IntPtr p = Win32.CreatePen(Win32.PS_SOLID, 1, line);
            IntPtr ob = Win32.SelectObject(dc, Win32.GetStockObject(Win32.HOLLOW_BRUSH));
            IntPtr op = Win32.SelectObject(dc, p);
            Win32.RoundRect(dc, rc.left, rc.top, rc.right, rc.bottom, radius, radius);
            Win32.SelectObject(dc, ob);
            Win32.SelectObject(dc, op);
            Win32.DeleteObject(p);

            /* 顶部内侧高光线，营造玻璃质感 */
            IntPtr hl = Win32.CreatePen(Win32.PS_SOLID, 1, Mix(line, Theme.C_TEXT, 0.09f));
            op = Win32.SelectObject(dc, hl);
            Win32.MoveToEx(dc, rc.left + radius / 2 + Sc(2), rc.top + Sc(1), IntPtr.Zero);
            Win32.LineTo(dc, rc.right - radius / 2 - Sc(2), rc.top + Sc(1));
            Win32.SelectObject(dc, op);
            Win32.DeleteObject(hl);
        }

        /* 现代卡片：柔影 + 渐变面板 + 顶部强调短线 */
        private static void DrawPanelShell(IntPtr dc, Win32.RECT rc, uint accent, bool elevated)
        {
            /* 两层偏移深色圆角矩形模拟弥散柔影 */
            Win32.RECT sh2 = rc;
            Win32.OffsetRect(ref sh2, 0, Sc(4));
            DrawRound(dc, sh2, Win32.RGB(4, 6, 10), Win32.RGB(4, 6, 10), Sc(16));
            Win32.RECT sh1 = rc;
            Win32.OffsetRect(ref sh1, 0, Sc(2));
            DrawRound(dc, sh1, Win32.RGB(6, 9, 14), Win32.RGB(6, 9, 14), Sc(15));

            uint topCol = elevated ? Theme.C_CARD_ELEV : Mix(Theme.C_CARD, Theme.C_CARD_ELEV, 0.45f);
            DrawPanelGradient(dc, rc, topCol, Theme.C_CARD, Theme.C_LINE, Sc(14));

            /* 顶部强调短线（保留各卡片的多 accent 语义色） */
            IntPtr accentPen = Win32.CreatePen(Win32.PS_SOLID, Sc(2), accent);
            IntPtr old = Win32.SelectObject(dc, accentPen);
            Win32.MoveToEx(dc, rc.left + Sc(16), rc.top + Sc(1), IntPtr.Zero);
            Win32.LineTo(dc, rc.left + Sc(52), rc.top + Sc(1));
            Win32.SelectObject(dc, old);
            Win32.DeleteObject(accentPen);
        }

        /* 英雄区数据线：发丝基线 + 正弦缓动往返的渐隐光束，尾部带紫色残影。 */
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

            /* 紫色拖尾（暗一档、短一截，反方向偏移） */
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
        internal static void PaintBackground(IntPtr hwnd, IntPtr dc)
        {
            Win32.RECT r;
            Win32.GetClientRect(hwnd, out r);

            UiLayout ui = UiLayout.Compute(hwnd);
            int rail = ui.rail;
            int pad = ui.pad;

            /* 页面底色：自上而下的深邃渐变 */
            DrawVGradient(dc, 0, 0, r.right, r.bottom, PageColorAt(0, r.bottom), PageColorAt(r.bottom, r.bottom));

            /* 主区域环境柔光（右上角青、中下部紫，被卡片覆盖形成层次） */
            DrawSoftGlow(dc, r.right - Sc(60), Sc(40), Sc(300), Theme.C_ACCENT, PageColorAt(Sc(40), r.bottom));
            DrawSoftGlow(dc, ui.x + ui.w / 3, r.bottom - Sc(40), Sc(340), Theme.C_VIOLET, PageColorAt(r.bottom - Sc(40), r.bottom));

            DrawTechGrid(dc, rail, 0, r.right, r.bottom);

            /* 侧边导航栏：克制的石墨色渐变 */
            DrawVGradient(dc, 0, 0, rail, r.bottom, Mix(Theme.C_CARD, Theme.C_RAIL, 0.35f), Theme.C_RAIL);

            /* 侧边栏右侧分隔线 */
            IntPtr dvpen = Win32.CreatePen(Win32.PS_SOLID, 1, Theme.C_LINE);
            IntPtr odv = Win32.SelectObject(dc, dvpen);
            Win32.MoveToEx(dc, rail, 0, IntPtr.Zero);
            Win32.LineTo(dc, rail, r.bottom);
            Win32.SelectObject(dc, odv);
            Win32.DeleteObject(dvpen);

            Win32.SetBkMode(dc, Win32.TRANSPARENT);

            /* 品牌铭牌使用实际的应用图标 */
            var brandPlate = new Win32.RECT(Sc(18), Sc(18), Sc(66), Sc(66));
            DrawPanelGradient(dc, brandPlate, Theme.C_CARD_ELEV, Theme.C_CARD,
                              Mix(Theme.C_VIOLET, Theme.C_LINE, 0.45f), Sc(12));
            IntPtr brandIcon = Win32.LoadImageW(MainWindow.Instance, new IntPtr(Theme.IDI_APP_ICON),
                                                Win32.IMAGE_ICON, Sc(38), Sc(38), Win32.LR_SHARED);
            if (brandIcon != IntPtr.Zero) {
                Win32.DrawIconEx(dc, Sc(23), Sc(23), brandIcon, Sc(38), Sc(38), 0, IntPtr.Zero, Win32.DI_NORMAL);
            }
            DrawTextX(dc, "ds\u6E38\u620F", Sc(78), Sc(20), rail - Sc(92), Sc(28), Theme.C_TEXT, Theme.FontHeading,
                      Win32.DT_LEFT | Win32.DT_SINGLELINE | Win32.DT_VCENTER);
            DrawTextX(dc, "\u7FFB\u8BD1\u5668", Sc(78), Sc(46), rail - Sc(92), Sc(20), Theme.C_TEXT_DIM, Theme.FontSmall,
                      Win32.DT_LEFT | Win32.DT_SINGLELINE | Win32.DT_VCENTER);

            /* 分区分隔线 */
            IntPtr sd = Win32.CreatePen(Win32.PS_SOLID, 1, Theme.C_LINE);
            IntPtr osd = Win32.SelectObject(dc, sd);
            Win32.MoveToEx(dc, Sc(24), Sc(90), IntPtr.Zero);
            Win32.LineTo(dc, rail - Sc(24), Sc(90));
            Win32.SelectObject(dc, osd);
            Win32.DeleteObject(sd);

            /* 活动导航项：左侧强调色竖条 */
            int navY = Sc(112);
            int navH = Sc(42);
            var navBg = new Win32.RECT(Sc(16), navY, rail - Sc(16), navY + navH);
            DrawPanelShell(dc, navBg, Theme.C_ACCENT, true);
            var navAcc = new Win32.RECT(Sc(16), navY + Sc(8), Sc(19), navY + navH - Sc(8));
            IntPtr ab2 = Win32.CreateSolidBrush(Theme.C_ACCENT);
            Win32.FillRect(dc, ref navAcc, ab2);
            Win32.DeleteObject(ab2);
            DrawTextX(dc, "运行时汉化", Sc(34), navY, rail - Sc(50), navH, Theme.C_TEXT, Theme.FontBody,
                      Win32.DT_LEFT | Win32.DT_SINGLELINE | Win32.DT_VCENTER);

            /* 功能要点列表 */
            int capY = navY + Sc(64);
            int capStep = Sc(28);
            string[] caps = { "本地缓存优先", "运行时不等待 API", "标签/变量保护" };
            uint[] capColors = { Theme.C_ACCENT, Theme.C_BLUE, Theme.C_VIOLET };
            for (int i = 0; i < 3; i++) {
                int yy = capY + i * capStep;
                IntPtr marker = Win32.CreateSolidBrush(capColors[i]);
                var markerRect = new Win32.RECT(Sc(24), yy + Sc(9), Sc(32), yy + Sc(11));
                Win32.FillRect(dc, ref markerRect, marker);
                Win32.DeleteObject(marker);
                DrawTextX(dc, caps[i], Sc(40), yy, rail - Sc(56), Sc(22), Theme.C_TEXT_DIM, Theme.FontSmall,
                          Win32.DT_LEFT | Win32.DT_SINGLELINE | Win32.DT_VCENTER);
            }

            /* 侧边栏底栏：版本徽章 + 运行时标签 */
            int footY = r.bottom - Sc(58);
            int footH = Sc(26);
            var chip = new Win32.RECT(Sc(20), footY, Sc(104), footY + footH);
            DrawRound(dc, chip, Theme.C_CARD_ELEV, Mix(Theme.C_VIOLET, Theme.C_LINE, 0.45f), Sc(6));
            DrawTextX(dc, "v" + BuildInfo.TranslatorVersion, Sc(20), footY, Sc(84), footH, Theme.C_ACCENT,
                      Theme.FontMonoSmall, Win32.DT_CENTER | Win32.DT_SINGLELINE | Win32.DT_VCENTER);
            DrawTextX(dc, MainWindow.RuntimeTag, Sc(112), footY, rail - Sc(120), footH, Theme.C_MUTED, Theme.FontSmall,
                      Win32.DT_LEFT | Win32.DT_SINGLELINE | Win32.DT_VCENTER);

            /* 主区域几何参数 */
            int x = ui.x;
            int w = ui.w;

            DrawHeroDataLine(dc, x, x + w, Sc(119), PageColorAt(Sc(119), r.bottom));

            bool alive = ServerProcess.Started;

            /* Status pill：胶囊形玻璃药丸 + 呼吸指示灯 */
            int pillW = Sc(176);
            int pillH = Sc(36);
            int pillX = r.right - pad - pillW;
            int pillY = Sc(30);
            var pill = new Win32.RECT(pillX, pillY, pillX + pillW, pillY + pillH);
            uint pillFill = Mix(Theme.C_CARD_ELEV, Theme.C_PAGE, 0.30f);
            DrawRound(dc, pill, pillFill, alive ? Mix(Theme.C_GREEN, Theme.C_LINE, 0.40f) : Theme.C_LINE, Sc(18));
            float pulse = Pulse01(2600);
            uint statusColor = alive ? Theme.C_GREEN : Theme.C_DANGER;
            uint dotCore = Mix(statusColor, Theme.C_TEXT, 0.22f * pulse);
            uint statusHalo = Mix(pillFill, statusColor, 0.24f + 0.38f * pulse);
            DrawDot(dc, pillX + Sc(20), pillY + pillH / 2, Sc(4), dotCore, statusHalo);
            DrawTextX(dc, alive ? "ONLINE" : "OFFLINE", pillX + Sc(36), pillY, pillW - Sc(46), pillH,
                      alive ? Theme.C_GREEN : Theme.C_DANGER, Theme.FontMonoSmall,
                      Win32.DT_LEFT | Win32.DT_SINGLELINE | Win32.DT_VCENTER);

            /* 选择器卡片 */
            DrawPanelShell(dc, ui.picker, Theme.C_BLUE, false);

            /* 指标卡片 */
            int gap = ui.metric_gap;
            int cardW = ui.metric_w;
            int mY = ui.metric_y;
            int mH = ui.metric_h;
            string[] labels = { "ENGINE", "SERVER", "CACHE" };
            uint[] metricColors = { Theme.C_ACCENT, alive ? Theme.C_GREEN : Theme.C_BLUE, Theme.C_AMBER };
            for (int i = 0; i < 3; i++) {
                int cx = x + (cardW + gap) * i;
                var m = new Win32.RECT(cx, mY, cx + cardW, mY + mH);
                DrawPanelShell(dc, m, metricColors[i], false);
                /* Accent left bar（圆角短棒） */
                var bar = new Win32.RECT(cx + Sc(12), mY + Sc(16), cx + Sc(15), mY + Sc(30));
                IntPtr bb = Win32.CreateSolidBrush(metricColors[i]);
                Win32.FillRect(dc, ref bar, bb);
                Win32.DeleteObject(bb);
                DrawTextX(dc, labels[i], cx + Sc(24), mY + Sc(14), cardW - Sc(36), Sc(20), Theme.C_MUTED,
                          Theme.FontMonoSmall, Win32.DT_LEFT | Win32.DT_SINGLELINE | Win32.DT_VCENTER);
            }

            /* 日志卡片（含标题栏） */
            int logCardTop = ui.log_top;
            var logCard = new Win32.RECT(x, logCardTop, x + w, ui.log_bottom);
            DrawPanelShell(dc, logCard, Theme.C_VIOLET, false);

            /* 日志标题栏 */
            int hdrY = logCardTop + Sc(12);
            int hdrH = Sc(24);
            DrawTextX(dc, "ACTIVITY LOG", x + Sc(18), hdrY, w - Sc(140), hdrH, Theme.C_TEXT_DIM, Theme.FontMonoSmall,
                      Win32.DT_LEFT | Win32.DT_SINGLELINE | Win32.DT_VCENTER);

            int liveCX = x + w - Sc(74);
            int liveCY = hdrY + hdrH / 2;
            float livePulse = Pulse01(3000);
            uint liveCore = Mix(Theme.C_VIOLET, Theme.C_TEXT, 0.20f * livePulse);
            uint liveHalo = Mix(Theme.C_CARD, Theme.C_VIOLET, 0.20f + 0.32f * livePulse);
            DrawDot(dc, liveCX, liveCY, Sc(4), liveCore, liveHalo);
            DrawTextX(dc, "LIVE", liveCX + Sc(12), hdrY, Sc(60), hdrH, Theme.C_VIOLET, Theme.FontMonoSmall,
                      Win32.DT_LEFT | Win32.DT_SINGLELINE | Win32.DT_VCENTER);

            IntPtr hp = Win32.CreatePen(Win32.PS_SOLID, 1, Theme.C_LINE);
            IntPtr ohp = Win32.SelectObject(dc, hp);
            Win32.MoveToEx(dc, x + Sc(18), hdrY + Sc(30), IntPtr.Zero);
            Win32.LineTo(dc, x + w - Sc(18), hdrY + Sc(30));
            Win32.SelectObject(dc, ohp);
            Win32.DeleteObject(hp);

            /* 路径输入框（EDIT）外框 */
            int peX = ui.path_x;
            int peY = ui.path_y;
            int peH = ui.path_h;
            int peW = ui.path_w;
            var peBorder = new Win32.RECT(peX - Sc(2), peY - Sc(2), peX + peW + Sc(2), peY + peH + Sc(2));
            DrawRound(dc, peBorder, Theme.C_LOG, Mix(Theme.C_BLUE, Theme.C_LINE, 0.40f), Sc(8));
        }

        /* 脏矩形双缓冲绘制，对应 paint_background_buffered。 */
        internal static void PaintBackgroundBuffered(IntPtr hwnd, IntPtr dc, Win32.RECT dirty)
        {
            if (dirty.right <= dirty.left || dirty.bottom <= dirty.top) return;

            int width = dirty.right - dirty.left;
            int height = dirty.bottom - dirty.top;
            IntPtr bufferDc = Win32.CreateCompatibleDC(dc);
            IntPtr bufferBitmap = bufferDc != IntPtr.Zero ? Win32.CreateCompatibleBitmap(dc, width, height) : IntPtr.Zero;

            /* 当 Windows 耗尽进程级 GDI 资源时，CreateCompatibleDC/CreateCompatibleBitmap
               可能失败。该分配归操作系统所有，无法在上游修复。直接绘制可保持启动器
               可用，但可能重新暴露原始的闪烁问题；诊断信息对调试器可见。 */
            if (bufferDc == IntPtr.Zero || bufferBitmap == IntPtr.Zero) {
                Win32.OutputDebugStringW("ds launcher: GDI back buffer unavailable; using direct paint.\n");
                if (bufferBitmap != IntPtr.Zero) Win32.DeleteObject(bufferBitmap);
                if (bufferDc != IntPtr.Zero) Win32.DeleteDC(bufferDc);
                PaintBackground(hwnd, dc);
                return;
            }

            IntPtr oldBitmap = Win32.SelectObject(bufferDc, bufferBitmap);
            int saved = Win32.SaveDC(bufferDc);
            Win32.SetViewportOrgEx(bufferDc, -dirty.left, -dirty.top, IntPtr.Zero);
            Win32.IntersectClipRect(bufferDc, dirty.left, dirty.top, dirty.right, dirty.bottom);
            PaintBackground(hwnd, bufferDc);
            Win32.RestoreDC(bufferDc, saved);
            Win32.BitBlt(dc, dirty.left, dirty.top, width, height, bufferDc, 0, 0, Win32.SRCCOPY);

            Win32.SelectObject(bufferDc, oldBitmap);
            Win32.DeleteObject(bufferBitmap);
            Win32.DeleteDC(bufferDc);
        }
    }
}
