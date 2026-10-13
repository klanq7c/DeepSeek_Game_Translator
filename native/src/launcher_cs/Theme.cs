using System;

namespace DstLauncher
{
    /*
     * Theme —— native/src/launcher/globals.c 的调色板与 fsutil.c 的 DPI 缩放/字体工厂。
     *
     * 颜色值必须与 globals.c 完全一致：--ui-probe 会把整个客户区渲染成位图逐字节比对，
     * 任何一个分量差 1 都会被判为移植缺陷。Sc() 直接调用 kernel32!MulDiv 而不是自己写
     * 四舍五入，避免 C 与 C# 在边界值上取整方向不同。
     */
    internal static class Theme
    {
        internal static readonly uint C_PAGE = Win32.RGB(6, 21, 46);
        internal static readonly uint C_RAIL = Win32.RGB(3, 12, 30);
        internal static readonly uint C_CARD = Win32.RGB(255, 253, 247);
        internal static readonly uint C_CARD_ELEV = Win32.RGB(249, 245, 235);
        internal static readonly uint C_LINE = Win32.RGB(223, 215, 197);
        internal static readonly uint C_LINE_BRIGHT = Win32.RGB(185, 157, 106);
        internal static readonly uint C_TEXT = Win32.RGB(22, 37, 57);
        internal static readonly uint C_TEXT_DIM = Win32.RGB(84, 98, 111);
        internal static readonly uint C_MUTED = Win32.RGB(120, 125, 126);
        internal static readonly uint C_ACCENT = Win32.RGB(244, 202, 114);
        internal static readonly uint C_ACCENT_DARK = Win32.RGB(234, 180, 92);
        internal static readonly uint C_ACCENT_DEEP = Win32.RGB(142, 103, 47);
        internal static readonly uint C_GREEN = Win32.RGB(48, 129, 97);
        internal static readonly uint C_DANGER = Win32.RGB(174, 78, 76);
        internal static readonly uint C_BLUE = Win32.RGB(32, 83, 135);
        internal static readonly uint C_VIOLET = Win32.RGB(126, 113, 92);
        internal static readonly uint C_AMBER = Win32.RGB(175, 124, 63);
        internal static readonly uint C_LOG = Win32.RGB(249, 246, 238);
        internal static readonly uint C_LOG_TEXT = Win32.RGB(64, 84, 107);

        internal const int RAIL_W = 256;

        /* 控件 ID（globals.h）。 */
        internal const int IDC_PATH = 101;
        internal const int IDC_BROWSE = 102;
        internal const int IDC_START = 103;
        internal const int IDC_OPEN = 104;
        internal const int IDC_STATUS = 105;
        internal const int IDC_LOG = 106;
        internal const int IDC_ENGINE = 107;
        internal const int IDC_TITLE = 108;
        internal const int IDC_SUBTITLE = 109;
        internal const int IDC_SERVER = 110;
        internal const int IDC_CACHE = 111;
        internal const int IDC_PATH_LABEL = 112;
        internal const int IDC_API_CONFIG = 114;
        internal const int IDC_SERVER_TOGGLE = 115;
        internal const int IDC_RESTORE = 116;
        internal const int IDC_CLEAR_CACHE = 117;

        /* csc 的 /win32icon 把主图标组固定放在资源 ID 32512；C 版 windres 用的是 1。
           两边加载的是同一个 assets\app_icon.ico，DrawIconEx 的输出因此一致。 */
        internal const int IDI_APP_ICON = 32512;

        internal static int ScaleDpi = 96;

        /* 把像素值按当前 DPI 缩放（96 基准）。所有 UI 尺寸必须过 Sc()。 */
        internal static int Sc(int v)
        {
            return Win32.MulDiv(v, ScaleDpi, 96);
        }

        /* 按 DPI 缩放创建字体。高度取负值表示 pt -> 像素（CreateFontW 约定）。 */
        internal static IntPtr MakeFont(int pt, int weight, string face)
        {
            int h = -Win32.MulDiv(pt, ScaleDpi, 72);
            return Win32.CreateFontW(h, 0, 0, 0, weight, 0, 0, 0, Win32.DEFAULT_CHARSET,
                                     Win32.OUT_DEFAULT_PRECIS, Win32.CLIP_DEFAULT_PRECIS,
                                     Win32.CLEARTYPE_QUALITY,
                                     Win32.DEFAULT_PITCH | Win32.FF_DONTCARE, face);
        }

        /* 声明进程为 DPI 感知。优先 Per-Monitor V2，降级 V1，再降级 System-DPI。 */
        internal static void EnableDpiAwareness()
        {
            try {
                if (Win32.SetProcessDpiAwarenessContext(new IntPtr(-4))) return;
                if (Win32.SetProcessDpiAwarenessContext(new IntPtr(-3))) return;
            } catch (EntryPointNotFoundException) {
                /* Win8.1 之前没有 SetProcessDpiAwarenessContext，按 C 版一样降级。 */
            }
            try {
                Win32.SetProcessDPIAware();
            } catch (EntryPointNotFoundException) {
                /* WinVista 之前无 DPI 感知 API：与 C 版的 GetProcAddress 失败分支一致，
                   系统会虚拟化 DPI（UI 变模糊但可用）。 */
            }
        }

        /* 为指定窗口检测 DPI 并更新 ScaleDpi，对应 dpi_set_for_window。 */
        internal static void SetDpiForWindow(IntPtr hwnd)
        {
            int dpi = 96;
            try {
                if (hwnd != IntPtr.Zero) dpi = (int)Win32.GetDpiForWindow(hwnd);
            } catch (EntryPointNotFoundException) {
                dpi = 0;
            }
            if (dpi <= 0) {
                IntPtr dc = Win32.GetDC(hwnd);
                if (dc != IntPtr.Zero) {
                    dpi = Win32.GetDeviceCaps(dc, Win32.LOGPIXELSY);
                    Win32.ReleaseDC(hwnd, dc);
                }
            }
            if (dpi <= 0) dpi = 96;
            ScaleDpi = dpi;
        }

        /* 线性插值混合两个颜色（t=0 返回 a，t=1 返回 b）。
           全部用 float 运算并按 C 的 (int) 截断，保证与 ui.c 的 mix 逐位一致。 */
        internal static uint Mix(uint a, uint b, float t)
        {
            int ar = Win32.GetRValue(a), ag = Win32.GetGValue(a), ab = Win32.GetBValue(a);
            int br = Win32.GetRValue(b), bg = Win32.GetGValue(b), bb = Win32.GetBValue(b);
            int r = ar + (int)((float)(br - ar) * t);
            int g = ag + (int)((float)(bg - ag) * t);
            int bl = ab + (int)((float)(bb - ab) * t);
            return Win32.RGB(r, g, bl);
        }

        /* 动画时钟。>= 0 时冻结在该毫秒值上：--ui-probe 用它让呼吸灯/光束停在
           确定的相位，否则两次渲染永远对不上。见 ui.c 的 g_anim_tick_override。 */
        internal static long AnimTickOverride = -1;

        internal static bool MotionEnabled = true;
        internal static uint MotionTick;
        private static bool _motionWarning;
        internal static uint MotionLast;

        internal static void RefreshMotion() {
            int enabled;
            if (!Win32.SystemParametersInfoW(0x1042, 0, out enabled, 0)) {
                /* 系统设置读取失败时静态显示；诊断保留 Win32 错误码，不影响翻译流程。 */
                if (!_motionWarning) {
                    Win32.OutputDebugStringW("ds launcher: animation preference unavailable (" + System.Runtime.InteropServices.Marshal.GetLastWin32Error() + "); using static decoration.\n");
                    _motionWarning = true;
                }
                enabled = 0;
            } else { _motionWarning = false; }
            MotionEnabled = enabled != 0;
        }

        internal static uint AnimTick()
        {
            return AnimTickOverride >= 0 ? (uint)AnimTickOverride : MotionTick;
        }

        /* ---- 字体与画刷句柄（对应 globals.c 的 extern 定义） ---- */
        internal static IntPtr FontTitle, FontHeading, FontBody, FontSmall, FontMono, FontMonoSmall;
        internal static IntPtr BrushPage, BrushCard, BrushEdit, BrushLog, BrushTransparent;

        internal static void CreateFonts()
        {
            FontTitle = MakeFont(28, Win32.FW_NORMAL, "SimSun");
            FontHeading = MakeFont(11, Win32.FW_SEMIBOLD, "Microsoft YaHei UI");
            FontBody = MakeFont(10, Win32.FW_NORMAL, "Microsoft YaHei UI");
            FontSmall = MakeFont(9, Win32.FW_NORMAL, "Microsoft YaHei UI");
            FontMono = MakeFont(10, Win32.FW_NORMAL, "Consolas");
            FontMonoSmall = MakeFont(8, Win32.FW_BOLD, "Consolas");
        }

        internal static void CreateBrushes()
        {
            BrushPage = Win32.CreateSolidBrush(C_PAGE);
            BrushCard = Win32.CreateSolidBrush(C_CARD);
            BrushEdit = Win32.CreateSolidBrush(C_LOG);
            BrushLog = Win32.CreateSolidBrush(C_LOG);
            BrushTransparent = Win32.GetStockObject(Win32.HOLLOW_BRUSH);
        }

        internal static void DeleteFontsAndBrushes()
        {
            Win32.DeleteObject(FontTitle);
            Win32.DeleteObject(FontHeading);
            Win32.DeleteObject(FontBody);
            Win32.DeleteObject(FontSmall);
            Win32.DeleteObject(FontMono);
            Win32.DeleteObject(FontMonoSmall);
            Win32.DeleteObject(BrushPage);
            Win32.DeleteObject(BrushCard);
            Win32.DeleteObject(BrushEdit);
            Win32.DeleteObject(BrushLog);
            UiPaint.FreeCardTextBrushes();
        }
    }
}
