using System;
using System.Runtime.InteropServices;

namespace DstLauncher
{
    /*
     * Win32 GDI / USER32 / COMCTL32 声明，供 UI 移植层（Theme / UiPaint / UiButtons /
     * UiLayoutCalc / MainWindow）使用。
     *
     * 这里刻意不用 System.Drawing 或 WinForms：C 版 ui.c 的每一个像素都由 GDI 画出，
     * 只有走同一组 API、同一个参数顺序，tests\launcher_parity 的 --ui-probe 才能对
     * 客户区位图做逐字节比对。任何"等价的托管画法"都会引入抗锯齿/取整差异。
     */
    internal static class Win32
    {
        internal const int MAX_PATH = 260;

        /* ---- 窗口消息 ---- */
        internal const int WM_CREATE = 0x0001;
        internal const int WM_DESTROY = 0x0002;
        internal const int WM_SIZE = 0x0005;
        internal const int WM_PAINT = 0x000F;
        internal const int WM_CLOSE = 0x0010;
        internal const int WM_QUIT = 0x0012;
        internal const int WM_ERASEBKGND = 0x0014;
        internal const int WM_SETFONT = 0x0030;
        internal const int WM_GETMINMAXINFO = 0x0024;
        internal const int WM_DRAWITEM = 0x002B;
        internal const int WM_TIMER = 0x0113;
        internal const int WM_COMMAND = 0x0111;
        internal const int WM_CTLCOLOREDIT = 0x0133;
        internal const int WM_CTLCOLORLISTBOX = 0x0134;
        internal const int WM_CTLCOLORSTATIC = 0x0138;
        internal const int WM_MOUSEMOVE = 0x0200;
        internal const int WM_MOUSELEAVE = 0x02A3;
        internal const int WM_NCDESTROY = 0x0082;
        internal const int WM_PRINTCLIENT = 0x0318;
        internal const int WM_DPICHANGED = 0x02E0;

        internal const int EN_CHANGE = 0x0300;

        /* ---- 列表框 ---- */
        internal const int LB_ADDSTRING = 0x0180;
        internal const int LB_DELETESTRING = 0x0182;
        internal const int LB_GETCOUNT = 0x018B;
        internal const int LB_SETTOPINDEX = 0x0197;
        internal const int LB_SETITEMHEIGHT = 0x01A0;
        internal const int LB_SETHORIZONTALEXTENT = 0x0194;

        /* ---- 窗口样式 ---- */
        internal const int WS_CHILD = 0x40000000;
        internal const int WS_VISIBLE = 0x10000000;
        internal const int WS_VSCROLL = 0x00200000;
        internal const int WS_HSCROLL = 0x00100000;
        internal const int WS_OVERLAPPEDWINDOW = 0x00CF0000;
        internal const int WS_CLIPCHILDREN = 0x02000000;
        internal const int WS_POPUP = unchecked((int)0x80000000);
        internal const int SS_NOPREFIX = 0x0080;
        internal const int SS_ENDELLIPSIS = 0x4000;
        internal const int ES_AUTOHSCROLL = 0x0080;
        internal const int BS_OWNERDRAW = 0x000B;
        internal const int LBS_NOINTEGRALHEIGHT = 0x0100;
        internal const int LBS_DISABLENOSCROLL = 0x1000;
        internal const int ES_PASSWORD = 0x0020;
        internal const int BS_DEFPUSHBUTTON = 0x0001;
        internal const int CBS_DROPDOWNLIST = 0x0003;
        internal const int WS_CAPTION = 0x00C00000;
        internal const int WS_SYSMENU = 0x00080000;
        internal const uint WS_EX_CLIENTEDGE = 0x00000200;
        internal const uint WS_EX_DLGMODALFRAME = 0x00000001;
        internal const int COLOR_WINDOW = 5;

        internal const int CB_ADDSTRING = 0x0143;
        internal const int CB_SETCURSEL = 0x014E;
        internal const int CB_GETCURSEL = 0x0147;
        internal const int CBN_SELCHANGE = 1;

        internal const int SW_SHOW = 5;
        internal const int SW_HIDE = 0;
        internal const int SW_SHOWNORMAL = 1;
        internal const int CW_USEDEFAULT = unchecked((int)0x80000000);

        internal const uint SWP_NOZORDER = 0x0004;
        internal const uint SWP_NOACTIVATE = 0x0010;
        internal const uint SWP_NOREDRAW = 0x0008;

        internal const uint RDW_INVALIDATE = 0x0001;
        internal const uint RDW_NOERASE = 0x0020;
        internal const uint RDW_ALLCHILDREN = 0x0080;

        internal const int ODS_SELECTED = 0x0001;
        internal const int ODS_FOCUS = 0x0010;

        internal const uint TME_LEAVE = 0x00000002;

        internal const int SM_CXICON = 11;
        internal const int SM_CYICON = 12;
        internal const int SM_CXSMICON = 49;
        internal const int SM_CYSMICON = 50;

        /* ---- GDI ---- */
        internal const int TRANSPARENT = 1;
        internal const int OPAQUE = 2;
        internal const int NULL_PEN = 8;
        internal const int HOLLOW_BRUSH = 5;
        internal const int PS_SOLID = 0;
        internal const int SRCCOPY = 0x00CC0020;
        internal const int LOGPIXELSY = 90;
        internal const uint GRADIENT_FILL_RECT_H = 0x00000000;
        internal const uint GRADIENT_FILL_RECT_V = 0x00000001;
        internal const int DEFAULT_CHARSET = 1;
        internal const int OUT_DEFAULT_PRECIS = 0;
        internal const int CLIP_DEFAULT_PRECIS = 0;
        internal const int CLEARTYPE_QUALITY = 5;
        internal const int DEFAULT_PITCH = 0;
        internal const int FF_DONTCARE = 0;
        internal const int FW_NORMAL = 400;
        internal const int FW_SEMIBOLD = 600;
        internal const int FW_BOLD = 700;
        internal const uint DIB_RGB_COLORS = 0;

        internal const uint DT_LEFT = 0x00000000;
        internal const uint DT_CENTER = 0x00000001;
        internal const uint DT_VCENTER = 0x00000004;
        internal const uint DT_SINGLELINE = 0x00000020;
        internal const uint DT_NOPREFIX = 0x00000800;
        internal const uint DT_END_ELLIPSIS = 0x00008000;

        internal const uint IMAGE_ICON = 1;
        internal const uint LR_SHARED = 0x00008000;
        internal const uint DI_NORMAL = 0x0003;

        [StructLayout(LayoutKind.Sequential)]
        internal struct RECT
        {
            internal int left, top, right, bottom;
            internal RECT(int l, int t, int r, int b) { left = l; top = t; right = r; bottom = b; }
            internal int Width { get { return right - left; } }
            internal int Height { get { return bottom - top; } }
        }

        [StructLayout(LayoutKind.Sequential)]
        internal struct POINT
        {
            internal int x, y;
            internal POINT(int px, int py) { x = px; y = py; }
        }

        [StructLayout(LayoutKind.Sequential)]
        internal struct SIZE { internal int cx, cy; }

        [StructLayout(LayoutKind.Sequential)]
        internal struct MSG
        {
            internal IntPtr hwnd;
            internal uint message;
            internal IntPtr wParam;
            internal IntPtr lParam;
            internal uint time;
            internal POINT pt;
        }

        [StructLayout(LayoutKind.Sequential)]
        internal struct PAINTSTRUCT
        {
            internal IntPtr hdc;
            internal int fErase;
            internal RECT rcPaint;
            internal int fRestore;
            internal int fIncUpdate;
            [MarshalAs(UnmanagedType.ByValArray, SizeConst = 32)]
            internal byte[] rgbReserved;
        }

        [StructLayout(LayoutKind.Sequential)]
        internal struct DRAWITEMSTRUCT
        {
            internal uint CtlType;
            internal uint CtlID;
            internal uint itemID;
            internal uint itemAction;
            internal uint itemState;
            internal IntPtr hwndItem;
            internal IntPtr hDC;
            internal RECT rcItem;
            internal UIntPtr itemData;
        }

        [StructLayout(LayoutKind.Sequential)]
        internal struct TRIVERTEX
        {
            internal int x, y;
            internal ushort Red, Green, Blue, Alpha;
        }

        [StructLayout(LayoutKind.Sequential)]
        internal struct GRADIENT_RECT
        {
            internal uint UpperLeft, LowerRight;
        }

        [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
        internal struct TEXTMETRICW
        {
            internal int tmHeight, tmAscent, tmDescent, tmInternalLeading, tmExternalLeading;
            internal int tmAveCharWidth, tmMaxCharWidth, tmWeight, tmOverhang;
            internal int tmDigitizedAspectX, tmDigitizedAspectY;
            internal char tmFirstChar, tmLastChar, tmDefaultChar, tmBreakChar;
            internal byte tmItalic, tmUnderlined, tmStruckOut, tmPitchAndFamily, tmCharSet;
        }

        [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
        internal struct WNDCLASSEXW
        {
            internal uint cbSize;
            internal uint style;
            internal IntPtr lpfnWndProc;
            internal int cbClsExtra;
            internal int cbWndExtra;
            internal IntPtr hInstance;
            internal IntPtr hIcon;
            internal IntPtr hCursor;
            internal IntPtr hbrBackground;
            internal IntPtr lpszMenuName;
            internal IntPtr lpszClassName;
            internal IntPtr hIconSm;
        }

        [StructLayout(LayoutKind.Sequential)]
        internal struct MINMAXINFO
        {
            internal POINT ptReserved, ptMaxSize, ptMaxPosition, ptMinTrackSize, ptMaxTrackSize;
        }

        [StructLayout(LayoutKind.Sequential)]
        internal struct TRACKMOUSEEVENT
        {
            internal uint cbSize;
            internal uint dwFlags;
            internal IntPtr hwndTrack;
            internal uint dwHoverTime;
        }

        [StructLayout(LayoutKind.Sequential)]
        internal struct BITMAPINFOHEADER
        {
            internal uint biSize;
            internal int biWidth, biHeight;
            internal ushort biPlanes, biBitCount;
            internal uint biCompression, biSizeImage;
            internal int biXPelsPerMeter, biYPelsPerMeter;
            internal uint biClrUsed, biClrImportant;
        }

        internal delegate IntPtr WndProcDelegate(IntPtr hwnd, uint msg, IntPtr wp, IntPtr lp);
        internal delegate IntPtr SubclassProcDelegate(IntPtr hwnd, uint msg, IntPtr wp, IntPtr lp,
                                                      UIntPtr subId, UIntPtr refData);

        internal static uint RGB(int r, int g, int b)
        {
            return (uint)(r | (g << 8) | (b << 16));
        }

        internal static int GetRValue(uint c) { return (int)(c & 0xFF); }
        internal static int GetGValue(uint c) { return (int)((c >> 8) & 0xFF); }
        internal static int GetBValue(uint c) { return (int)((c >> 16) & 0xFF); }

        internal static int LOWORD(IntPtr v) { return (int)((long)v & 0xFFFF); }
        internal static int HIWORD(IntPtr v) { return (int)(((long)v >> 16) & 0xFFFF); }

        [DllImport("kernel32.dll")]
        internal static extern int MulDiv(int number, int numerator, int denominator);

        [DllImport("kernel32.dll")]
        internal static extern uint GetTickCount();

        [DllImport("kernel32.dll", CharSet = CharSet.Unicode)]
        internal static extern IntPtr GetModuleHandleW(string name);

        [DllImport("kernel32.dll")]
        internal static extern void OutputDebugStringW([MarshalAs(UnmanagedType.LPWStr)] string text);

        /* ---- USER32 ---- */
        [DllImport("user32.dll", CharSet = CharSet.Unicode)]
        internal static extern ushort RegisterClassExW(ref WNDCLASSEXW wc);

        [DllImport("user32.dll", CharSet = CharSet.Unicode)]
        internal static extern IntPtr CreateWindowExW(uint exStyle, string cls, string name, uint style,
                                                      int x, int y, int w, int h, IntPtr parent,
                                                      IntPtr menu, IntPtr inst, IntPtr param);

        [DllImport("user32.dll", CharSet = CharSet.Unicode)]
        internal static extern IntPtr DefWindowProcW(IntPtr hwnd, uint msg, IntPtr wp, IntPtr lp);

        [DllImport("user32.dll", CharSet = CharSet.Unicode)]
        internal static extern IntPtr SendMessageW(IntPtr hwnd, uint msg, IntPtr wp, IntPtr lp);

        [DllImport("user32.dll", CharSet = CharSet.Unicode)]
        internal static extern IntPtr SendMessageW(IntPtr hwnd, uint msg, IntPtr wp, string lp);

        [DllImport("user32.dll", CharSet = CharSet.Unicode)]
        internal static extern int GetWindowTextW(IntPtr hwnd, [Out] System.Text.StringBuilder text, int cap);

        [DllImport("user32.dll", CharSet = CharSet.Unicode)]
        internal static extern bool SetWindowTextW(IntPtr hwnd, string text);

        [DllImport("user32.dll")]
        internal static extern bool GetClientRect(IntPtr hwnd, out RECT rc);

        [DllImport("user32.dll")]
        internal static extern bool GetWindowRect(IntPtr hwnd, out RECT rc);

        [DllImport("user32.dll")]
        internal static extern int MapWindowPoints(IntPtr from, IntPtr to, ref RECT pts, uint count);

        [DllImport("user32.dll")]
        internal static extern bool InvalidateRect(IntPtr hwnd, ref RECT rc, bool erase);

        [DllImport("user32.dll")]
        internal static extern bool InvalidateRect(IntPtr hwnd, IntPtr rc, bool erase);

        [DllImport("user32.dll")]
        internal static extern bool RedrawWindow(IntPtr hwnd, IntPtr rc, IntPtr rgn, uint flags);

        [DllImport("user32.dll")]
        internal static extern bool SetWindowPos(IntPtr hwnd, IntPtr after, int x, int y, int w, int h, uint flags);

        [DllImport("user32.dll")]
        internal static extern IntPtr GetDlgItem(IntPtr hwnd, int id);

        [DllImport("user32.dll")]
        internal static extern bool IsWindow(IntPtr hwnd);

        [DllImport("user32.dll")]
        internal static extern bool IsIconic(IntPtr hwnd);

        [DllImport("user32.dll")]
        internal static extern IntPtr GetParent(IntPtr hwnd);

        [DllImport("user32.dll")]
        internal static extern IntPtr GetDC(IntPtr hwnd);

        [DllImport("user32.dll")]
        internal static extern int ReleaseDC(IntPtr hwnd, IntPtr dc);

        [DllImport("user32.dll")]
        internal static extern IntPtr BeginPaint(IntPtr hwnd, out PAINTSTRUCT ps);

        [DllImport("user32.dll")]
        internal static extern bool EndPaint(IntPtr hwnd, ref PAINTSTRUCT ps);

        [DllImport("user32.dll")]
        internal static extern IntPtr SetTimer(IntPtr hwnd, UIntPtr id, uint ms, IntPtr proc);

        [DllImport("user32.dll")]
        internal static extern bool KillTimer(IntPtr hwnd, UIntPtr id);

        [DllImport("user32.dll")]
        internal static extern bool TrackMouseEvent(ref TRACKMOUSEEVENT tme);

        [DllImport("user32.dll", CharSet = CharSet.Unicode)]
        internal static extern int MessageBoxW(IntPtr hwnd, string text, string caption, uint type);

        [DllImport("user32.dll", CharSet = CharSet.Unicode)]
        internal static extern IntPtr LoadCursorW(IntPtr inst, IntPtr name);

        [DllImport("user32.dll", CharSet = CharSet.Unicode)]
        internal static extern IntPtr LoadImageW(IntPtr inst, IntPtr name, uint type, int cx, int cy, uint load);

        [DllImport("user32.dll")]
        internal static extern bool DrawIconEx(IntPtr dc, int x, int y, IntPtr icon, int cx, int cy,
                                               uint step, IntPtr brush, uint flags);

        [DllImport("user32.dll")]
        internal static extern int GetSystemMetrics(int index);

        [DllImport("user32.dll")]
        internal static extern bool ShowWindow(IntPtr hwnd, int cmd);

        [DllImport("user32.dll")]
        internal static extern bool UpdateWindow(IntPtr hwnd);

        [DllImport("user32.dll")]
        internal static extern bool DestroyWindow(IntPtr hwnd);

        [DllImport("user32.dll")]
        internal static extern void PostQuitMessage(int code);

        [DllImport("user32.dll", CharSet = CharSet.Unicode)]
        internal static extern int GetMessageW(out MSG msg, IntPtr hwnd, uint min, uint max);

        [DllImport("user32.dll")]
        internal static extern bool TranslateMessage(ref MSG msg);

        [DllImport("user32.dll", CharSet = CharSet.Unicode)]
        internal static extern IntPtr DispatchMessageW(ref MSG msg);

        [DllImport("user32.dll", CharSet = CharSet.Unicode)]
        internal static extern int DrawTextW(IntPtr dc, string text, int len, ref RECT rc, uint format);

        [DllImport("user32.dll")]
        internal static extern int FillRect(IntPtr dc, ref RECT rc, IntPtr brush);

        [DllImport("user32.dll")]
        internal static extern bool InflateRect(ref RECT rc, int dx, int dy);

        [DllImport("user32.dll")]
        internal static extern bool OffsetRect(ref RECT rc, int dx, int dy);

        [DllImport("user32.dll")]
        internal static extern bool IsDialogMessageW(IntPtr dlg, ref MSG msg);

        [DllImport("user32.dll")]
        internal static extern bool EnableWindow(IntPtr hwnd, bool enable);

        [DllImport("user32.dll")]
        internal static extern bool SetForegroundWindow(IntPtr hwnd);

        [DllImport("user32.dll")]
        internal static extern uint GetDpiForWindow(IntPtr hwnd);

        [DllImport("user32.dll")]
        internal static extern bool SetProcessDpiAwarenessContext(IntPtr context);

        [DllImport("user32.dll")]
        internal static extern bool SetProcessDPIAware();

        /* ---- GDI32 ---- */
        [DllImport("gdi32.dll")]
        internal static extern IntPtr CreateSolidBrush(uint color);

        [DllImport("gdi32.dll")]
        internal static extern IntPtr CreatePen(int style, int width, uint color);

        [DllImport("gdi32.dll")]
        internal static extern IntPtr SelectObject(IntPtr dc, IntPtr obj);

        [DllImport("gdi32.dll")]
        internal static extern bool DeleteObject(IntPtr obj);

        [DllImport("gdi32.dll")]
        internal static extern IntPtr GetStockObject(int index);

        [DllImport("gdi32.dll")]
        internal static extern bool RoundRect(IntPtr dc, int l, int t, int r, int b, int w, int h);

        [DllImport("gdi32.dll")]
        internal static extern bool Ellipse(IntPtr dc, int l, int t, int r, int b);

        [DllImport("gdi32.dll")]
        internal static extern bool Rectangle(IntPtr dc, int l, int t, int r, int b);

        [DllImport("gdi32.dll")]
        internal static extern bool Polygon(IntPtr dc, POINT[] points, int count);

        [DllImport("gdi32.dll")]
        internal static extern bool Arc(IntPtr dc, int l, int t, int r, int b, int xs, int ys, int xe, int ye);

        [DllImport("gdi32.dll")]
        internal static extern bool MoveToEx(IntPtr dc, int x, int y, IntPtr prev);

        [DllImport("gdi32.dll")]
        internal static extern bool LineTo(IntPtr dc, int x, int y);

        [DllImport("gdi32.dll")]
        internal static extern IntPtr CreateRoundRectRgn(int l, int t, int r, int b, int w, int h);

        [DllImport("gdi32.dll")]
        internal static extern int SelectClipRgn(IntPtr dc, IntPtr rgn);

        [DllImport("gdi32.dll")]
        internal static extern int IntersectClipRect(IntPtr dc, int l, int t, int r, int b);

        [DllImport("gdi32.dll")]
        internal static extern int SaveDC(IntPtr dc);

        [DllImport("gdi32.dll")]
        internal static extern bool RestoreDC(IntPtr dc, int saved);

        [DllImport("gdi32.dll")]
        internal static extern int SetBkMode(IntPtr dc, int mode);

        [DllImport("gdi32.dll")]
        internal static extern uint SetTextColor(IntPtr dc, uint color);

        [DllImport("gdi32.dll")]
        internal static extern uint SetBkColor(IntPtr dc, uint color);

        [DllImport("gdi32.dll", CharSet = CharSet.Unicode)]
        internal static extern bool GetTextExtentPoint32W(IntPtr dc, string text, int len, out SIZE size);

        [DllImport("gdi32.dll", CharSet = CharSet.Unicode)]
        internal static extern bool GetTextMetricsW(IntPtr dc, out TEXTMETRICW tm);

        [DllImport("gdi32.dll", CharSet = CharSet.Unicode)]
        internal static extern IntPtr CreateFontW(int height, int width, int escapement, int orientation,
                                                  int weight, uint italic, uint underline, uint strikeout,
                                                  uint charSet, uint outPrecision, uint clipPrecision,
                                                  uint quality, uint pitchAndFamily, string face);

        [DllImport("gdi32.dll")]
        internal static extern IntPtr CreateCompatibleDC(IntPtr dc);

        [DllImport("gdi32.dll")]
        internal static extern IntPtr CreateCompatibleBitmap(IntPtr dc, int w, int h);

        [DllImport("gdi32.dll")]
        internal static extern IntPtr CreateDIBSection(IntPtr dc, ref BITMAPINFOHEADER bmi, uint usage,
                                                       out IntPtr bits, IntPtr section, uint offset);

        [DllImport("gdi32.dll")]
        internal static extern bool BitBlt(IntPtr dst, int x, int y, int w, int h, IntPtr src, int sx, int sy, int rop);

        [DllImport("gdi32.dll")]
        internal static extern bool SetViewportOrgEx(IntPtr dc, int x, int y, IntPtr prev);

        [DllImport("gdi32.dll")]
        internal static extern bool DeleteDC(IntPtr dc);

        [DllImport("gdi32.dll")]
        internal static extern int GetDeviceCaps(IntPtr dc, int index);

        [DllImport("gdi32.dll")]
        internal static extern bool GdiFlush();

        internal const uint PRF_CLIENT = 0x00000004;
        internal const uint ODT_BUTTON = 4;
        internal const uint ODA_DRAWENTIRE = 0x0001;
        internal const uint BI_RGB = 0;

        [DllImport("msimg32.dll")]
        internal static extern bool GradientFill(IntPtr dc, TRIVERTEX[] vertices, uint vertexCount,
                                                 ref GRADIENT_RECT mesh, uint meshCount, uint mode);

        /* ---- COMCTL32 ---- */
        [DllImport("comctl32.dll", CharSet = CharSet.Unicode)]
        internal static extern bool SetWindowSubclass(IntPtr hwnd, SubclassProcDelegate proc,
                                                      UIntPtr id, UIntPtr refData);

        [DllImport("comctl32.dll", CharSet = CharSet.Unicode)]
        internal static extern bool RemoveWindowSubclass(IntPtr hwnd, SubclassProcDelegate proc, UIntPtr id);

        [DllImport("comctl32.dll", CharSet = CharSet.Unicode)]
        internal static extern IntPtr DefSubclassProc(IntPtr hwnd, uint msg, IntPtr wp, IntPtr lp);

        [StructLayout(LayoutKind.Sequential)]
        internal struct INITCOMMONCONTROLSEX
        {
            internal uint dwSize;
            internal uint dwICC;
        }

        internal const uint ICC_STANDARD_CLASSES = 0x00004000;

        [DllImport("comctl32.dll")]
        internal static extern bool InitCommonControlsEx(ref INITCOMMONCONTROLSEX icc);

        /* ---- Shell / DWM / UxTheme（按需延迟加载，与 C 版 apply_window_chrome 一致） ---- */
        [DllImport("shell32.dll", CharSet = CharSet.Unicode)]
        internal static extern IntPtr ShellExecuteW(IntPtr hwnd, string verb, string file,
                                                    string parameters, string dir, int show);

        [DllImport("ole32.dll")]
        internal static extern int CoInitialize(IntPtr reserved);
    }
}
