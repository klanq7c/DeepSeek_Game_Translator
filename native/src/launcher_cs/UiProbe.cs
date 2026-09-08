using System;
using System.IO;
using System.Runtime.InteropServices;
using System.Text;

namespace DstLauncher
{
    /*
     * UiProbe —— native/src/launcher/ui_probe.c 的移植：--ui-probe 把主窗口客户区
     * 渲染成确定的一帧。
     *
     * 窗口层无法像 detect/deploy/warmup 那样比对文本输出，所以这里把"画出来的东西"
     * 本身变成可比对的产物：固定 DPI 96、冻结动画时钟、中性化两处运行时标识、给定
     * 服务器状态，创建一个不可见的 WS_POPUP 主窗口，先画父窗口背景再按 z 序把每个
     * 子控件画进同一张 32bpp 位图，最后写成无压缩 BMP 并打印布局报告。
     *
     * tests\launcher_parity 对 BMP 取 SHA-256、对 stdout 逐行比对，两版必须完全一致。
     */
    internal static class UiProbe
    {
        private const string ProbeClass = "DSTNativeLauncherProbeCs";

        private struct ProbeControl
        {
            internal int id;
            internal string name;
            internal bool ownerDraw;
            internal ProbeControl(int i, string n, bool od) { id = i; name = n; ownerDraw = od; }
        }

        /* 顺序即 CreateControls 的创建顺序（也就是 z 序）。 */
        private static readonly ProbeControl[] Controls = {
            new ProbeControl(Theme.IDC_TITLE, "title", false),
            new ProbeControl(Theme.IDC_SUBTITLE, "subtitle", false),
            new ProbeControl(Theme.IDC_STATUS, "status", false),
            new ProbeControl(Theme.IDC_PATH_LABEL, "path_label", false),
            new ProbeControl(Theme.IDC_PATH, "path", false),
            new ProbeControl(Theme.IDC_BROWSE, "browse", true),
            new ProbeControl(Theme.IDC_OPEN, "open", true),
            new ProbeControl(Theme.IDC_START, "start", true),
            new ProbeControl(Theme.IDC_RESTORE, "restore", true),
            new ProbeControl(Theme.IDC_SERVER_TOGGLE, "server", true),
            new ProbeControl(Theme.IDC_API_CONFIG, "api", true),
            new ProbeControl(Theme.IDC_ENGINE, "engine", false),
            new ProbeControl(Theme.IDC_SERVER, "server_val", false),
            new ProbeControl(Theme.IDC_CACHE, "cache", false),
            new ProbeControl(Theme.IDC_CLEAR_CACHE, "clear_cache", true),
            new ProbeControl(Theme.IDC_LOG, "log", false),
        };

        private static Action<string> _out;

        private static void Line(string text) { _out(text); }

        /* 32bpp 无压缩、自上而下的 BMP。像素直接来自 DIB section，无调色板与行填充歧义。 */
        private static bool WriteBmp32(string path, IntPtr pixels, int width, int height)
        {
            int imageBytes = width * height * 4;
            var managed = new byte[imageBytes];
            Marshal.Copy(pixels, managed, 0, imageBytes);
            try {
                using (var fs = new FileStream(path, FileMode.Create, FileAccess.Write, FileShare.None))
                using (var bw = new BinaryWriter(fs)) {
                    bw.Write((ushort)0x4D42);                 /* bfType 'BM' */
                    bw.Write((uint)(54 + imageBytes));        /* bfSize */
                    bw.Write((ushort)0);                      /* bfReserved1 */
                    bw.Write((ushort)0);                      /* bfReserved2 */
                    bw.Write((uint)54);                       /* bfOffBits */
                    bw.Write((uint)40);                       /* biSize */
                    bw.Write(width);                          /* biWidth */
                    bw.Write(-height);                        /* biHeight（负 = 自上而下） */
                    bw.Write((ushort)1);                      /* biPlanes */
                    bw.Write((ushort)32);                     /* biBitCount */
                    bw.Write((uint)Win32.BI_RGB);             /* biCompression */
                    bw.Write((uint)imageBytes);               /* biSizeImage */
                    bw.Write(0);                              /* biXPelsPerMeter */
                    bw.Write(0);                              /* biYPelsPerMeter */
                    bw.Write((uint)0);                        /* biClrUsed */
                    bw.Write((uint)0);                        /* biClrImportant */
                    bw.Write(managed);
                }
                return true;
            } catch (IOException ex) {
                Line("bmp_error=" + ex.Message + "\n");
                return false;
            } catch (UnauthorizedAccessException ex) {
                Line("bmp_error=" + ex.Message + "\n");
                return false;
            }
        }

        /* 把一个子控件画进父窗口的位图：视口原点移到控件左上角，再让它自己打印。 */
        private static void PrintChild(IntPtr hwnd, IntPtr dc, ProbeControl pc, Win32.RECT rc)
        {
            IntPtr ctl = Win32.GetDlgItem(hwnd, pc.id);
            if (ctl == IntPtr.Zero || !Win32.IsWindow(ctl)) return;

            int saved = Win32.SaveDC(dc);
            Win32.IntersectClipRect(dc, rc.left, rc.top, rc.right, rc.bottom);
            if (pc.ownerDraw) {
                /* BS_OWNERDRAW 按钮平时由父窗口的 WM_DRAWITEM 绘制，这里直接合成同样的
                   结构体：rcItem 用客户区坐标，hDC 就是位图 DC，DrawButton 只用这两项。 */
                var di = new Win32.DRAWITEMSTRUCT();
                di.CtlType = Win32.ODT_BUTTON;
                di.CtlID = (uint)pc.id;
                di.itemAction = Win32.ODA_DRAWENTIRE;
                di.itemState = 0;
                di.hwndItem = ctl;
                di.hDC = dc;
                di.rcItem = rc;
                UiButtons.DrawButton(ref di);
            } else {
                Win32.SetViewportOrgEx(dc, rc.left, rc.top, IntPtr.Zero);
                Win32.SendMessageW(ctl, Win32.WM_PRINTCLIENT, dc, (IntPtr)Win32.PRF_CLIENT);
            }
            Win32.RestoreDC(dc, saved);
        }

        private static bool Render(IntPtr hwnd, string bmpPath)
        {
            Win32.RECT client;
            Win32.GetClientRect(hwnd, out client);
            int width = client.right;
            int height = client.bottom;
            if (width <= 0 || height <= 0) return false;

            var bi = new Win32.BITMAPINFOHEADER();
            bi.biSize = (uint)Marshal.SizeOf(typeof(Win32.BITMAPINFOHEADER));
            bi.biWidth = width;
            bi.biHeight = -height;   /* 自上而下 */
            bi.biPlanes = 1;
            bi.biBitCount = 32;
            bi.biCompression = Win32.BI_RGB;

            IntPtr screen = Win32.GetDC(IntPtr.Zero);
            IntPtr dc = Win32.CreateCompatibleDC(screen);
            IntPtr pixels = IntPtr.Zero;
            IntPtr bmp = IntPtr.Zero;
            if (dc != IntPtr.Zero) {
                bmp = Win32.CreateDIBSection(dc, ref bi, Win32.DIB_RGB_COLORS, out pixels, IntPtr.Zero, 0);
            }
            Win32.ReleaseDC(IntPtr.Zero, screen);
            if (dc == IntPtr.Zero || bmp == IntPtr.Zero || pixels == IntPtr.Zero) {
                /* GDI 分配失败归操作系统所有，无法在上游修复；如实报告并按失败退出，
                   不能退化成"渲染成功但内容为空"的假绿。 */
                Line("gdi_error=" + Marshal.GetLastWin32Error() + "\n");
                if (bmp != IntPtr.Zero) Win32.DeleteObject(bmp);
                if (dc != IntPtr.Zero) Win32.DeleteDC(dc);
                return false;
            }
            IntPtr oldBmp = Win32.SelectObject(dc, bmp);

            UiPaint.PaintBackground(hwnd, dc);

            foreach (ProbeControl pc in Controls) {
                IntPtr ctl = Win32.GetDlgItem(hwnd, pc.id);
                if (ctl == IntPtr.Zero || !Win32.IsWindow(ctl)) continue;
                Win32.RECT rc;
                Win32.GetWindowRect(ctl, out rc);
                Win32.MapWindowPoints(IntPtr.Zero, hwnd, ref rc, 2);
                PrintChild(hwnd, dc, pc, rc);
            }

            Win32.GdiFlush();
            bool ok = WriteBmp32(bmpPath, pixels, width, height);

            Win32.SelectObject(dc, oldBmp);
            Win32.DeleteObject(bmp);
            Win32.DeleteDC(dc);
            return ok;
        }

        /* compute_layout 的结果与每个控件的位置/文本，作为像素之外的第二道断言。 */
        private static void Report(IntPtr hwnd)
        {
            Win32.RECT client;
            Win32.GetClientRect(hwnd, out client);
            Line("client=" + client.right + "|" + client.bottom + "\n");
            Line("dpi=" + Theme.ScaleDpi + "\n");
            Line("runtime_tag=" + MainWindow.RuntimeTag + "\n");
            Line("subtitle=" + MainWindow.SubtitleText + "\n");
            Line("alive=" + (ServerProcess.Started ? 1 : 0) + "\n");

            foreach (ProbeControl pc in Controls) {
                IntPtr ctl = Win32.GetDlgItem(hwnd, pc.id);
                if (ctl == IntPtr.Zero || !Win32.IsWindow(ctl)) {
                    Line("ctl=" + pc.name + "|" + pc.id + "|missing\n");
                    continue;
                }
                Win32.RECT rc;
                Win32.GetWindowRect(ctl, out rc);
                Win32.MapWindowPoints(IntPtr.Zero, hwnd, ref rc, 2);
                var sb = new StringBuilder(512);
                Win32.GetWindowTextW(ctl, sb, 512);
                var text = new StringBuilder(sb.Length);
                foreach (char c in sb.ToString()) {
                    text.Append(c == '\r' || c == '\n' || c == '\t' || c == '|' ? ' ' : c);
                }
                Line("ctl=" + pc.name + "|" + pc.id + "|" +
                     rc.left + "," + rc.top + "," + rc.right + "," + rc.bottom + "|" + text + "\n");
            }
        }

        /* --ui-probe <w> <h> <alive> <out.bmp> */
        internal static int Run(int width, int height, bool alive, string bmpPath, Action<string> writer)
        {
            if (width <= 0 || height <= 0 || string.IsNullOrEmpty(bmpPath)) return 2;
            _out = writer;

            MainWindow.ProbeMode = true;
            Theme.AnimTickOverride = 0;
            ServerProcess.SetStartedForProbe(alive);

            var ic = new Win32.INITCOMMONCONTROLSEX();
            ic.dwSize = (uint)Marshal.SizeOf(typeof(Win32.INITCOMMONCONTROLSEX));
            ic.dwICC = Win32.ICC_STANDARD_CLASSES;
            Win32.InitCommonControlsEx(ref ic);

            MainWindow.Instance = Win32.GetModuleHandleW(null);

            var wc = new Win32.WNDCLASSEXW();
            wc.cbSize = (uint)Marshal.SizeOf(typeof(Win32.WNDCLASSEXW));
            wc.lpfnWndProc = MainWindow.WndProcPointer;
            wc.hInstance = MainWindow.Instance;
            wc.hCursor = Win32.LoadCursorW(IntPtr.Zero, (IntPtr)32512 /* IDC_ARROW */);
            wc.hbrBackground = IntPtr.Zero;
            wc.lpszClassName = Marshal.StringToHGlobalUni(ProbeClass);
            if (Win32.RegisterClassExW(ref wc) == 0) {
                Line("class_error=" + Marshal.GetLastWin32Error() + "\n");
                Line("result=0\n");
                return 1;
            }

            /* WS_POPUP 没有边框和标题栏，窗口尺寸即客户区尺寸，两版无需再对齐非客户区。 */
            IntPtr hwnd = Win32.CreateWindowExW(0, ProbeClass, "ds probe",
                                                unchecked((uint)(Win32.WS_POPUP | Win32.WS_CLIPCHILDREN)),
                                                0, 0, width, height, IntPtr.Zero, IntPtr.Zero,
                                                MainWindow.Instance, IntPtr.Zero);
            if (hwnd == IntPtr.Zero) {
                Line("window_error=" + Marshal.GetLastWin32Error() + "\n");
                Line("result=0\n");
                return 1;
            }
            MainWindow.Hwnd = hwnd;

            /* DPI 固定为 96：WM_CREATE 里的 SetDpiForWindow 会按实际显示器改写它，
               而两版必须用同一个缩放系数。改完重新建字体并重新布局。 */
            if (Theme.ScaleDpi != 96) {
                IntPtr[] old = { Theme.FontTitle, Theme.FontHeading, Theme.FontBody,
                                 Theme.FontSmall, Theme.FontMono, Theme.FontMonoSmall };
                Theme.ScaleDpi = 96;
                Theme.CreateFonts();
                MainWindow.ApplyFonts();
                foreach (IntPtr f in old) Win32.DeleteObject(f);
                UiLayout.Apply(hwnd);
            }

            Report(hwnd);
            bool ok = Render(hwnd, bmpPath);
            Line("result=" + (ok ? 1 : 0) + "\n");

            Win32.DestroyWindow(hwnd);
            MainWindow.Hwnd = IntPtr.Zero;
            return ok ? 0 : 1;
        }
    }
}
