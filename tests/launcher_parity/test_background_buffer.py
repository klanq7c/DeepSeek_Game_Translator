"""源链接两版绘图缓冲：计数资源创建，注入 GDI 失败并检查释放与恢复。"""
import argparse
import os
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
C_PREFIX = r'''
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
static int fault, dcs, bitmaps, creates, direct, blits, diagnostics;
static HDC fake_dc(HDC dc) { creates++; if(fault==1) return NULL; dcs++; return (HDC)2; }
static HBITMAP fake_bitmap(HDC dc,int w,int h) { if(fault==2) return NULL; bitmaps++; return (HBITMAP)3; }
static HGDIOBJ fake_select(HDC dc,HGDIOBJ obj) { return fault==3 ? HGDI_ERROR : (HGDIOBJ)4; }
static int fake_save(HDC dc) { return fault==4 ? 0 : 1; }
static BOOL fake_view(HDC dc,int x,int y,LPPOINT p) { return fault!=5; }
static int fake_clip(HDC dc,int l,int t,int r,int b) { return fault==6 ? ERROR : SIMPLEREGION; }
static BOOL fake_restore(HDC dc,int saved) { return fault!=7; }
static BOOL fake_blt(HDC dst,int x,int y,int w,int h,HDC src,int sx,int sy,DWORD rop) { blits++; return fault!=8; }
static BOOL fake_delete_dc(HDC dc) { dcs--; return TRUE; }
static BOOL fake_delete_object(HGDIOBJ obj) { bitmaps--; return TRUE; }
static void fake_log(const WCHAR *s) { diagnostics++; }
static void paint_background(HWND hwnd,HDC dc) { if(dc==(HDC)1) direct++; }
#define CreateCompatibleDC fake_dc
#define CreateCompatibleBitmap fake_bitmap
#define SelectObject fake_select
#define SaveDC fake_save
#define SetViewportOrgEx fake_view
#define IntersectClipRect fake_clip
#define RestoreDC fake_restore
#define BitBlt fake_blt
#define DeleteDC fake_delete_dc
#define DeleteObject fake_delete_object
#define OutputDebugStringW fake_log
'''
C_SUFFIX = r'''
static void check(int ok) { if(!ok) { printf("FAIL fault=%d live=%d,%d direct=%d diagnostics=%d\n",fault,dcs,bitmaps,direct,diagnostics); exit(1); } }
int main(void) {
    RECT full={0,0,1200,800}, part={50,20,900,240}, tall={40,0,140,900}, wide={0,100,1250,180};
    for(int i=0;i<1000;i++) paint_background_buffered((HWND)1,(HDC)1,i==0?&full:&part);
    printf("allocations=%d paints=1000\n",creates);
    if(BASELINE) { check(dcs==0 && bitmaps==0); return 0; }
    check(creates==1 && dcs==1 && bitmaps==1 && direct==0 && blits==1000);
    paint_background_buffered((HWND)1,(HDC)1,&tall);
    paint_background_buffered((HWND)1,(HDC)1,&wide);
    paint_background_buffered((HWND)1,(HDC)1,&tall);
    check(creates==3 && dcs==1 && bitmaps==1);
    paint_background_buffered((HWND)2,(HDC)1,&part);
    check(creates==4 && dcs==1 && bitmaps==1);
    free_background_buffer(); free_background_buffer();
    check(dcs==0 && bitmaps==0);
    for(fault=1;fault<=8;fault++) {
        direct=0; paint_background_buffered((HWND)1,(HDC)1,&full);
        check(direct==1 && dcs==0 && bitmaps==0);
    }
    check(diagnostics==4);
    fault=0; direct=0; paint_background_buffered((HWND)1,(HDC)1,&part);
    check(direct==0 && dcs==1 && bitmaps==1);
    free_background_buffer(); check(dcs==0 && bitmaps==0);
    puts("PASS reuse growth owner disposal eight-failures recovery"); return 0;
}
'''
CS_PREFIX = r'''
using System;
class Probe {
    static int fault, dcs, bitmaps, creates, direct, blits, diagnostics;
    internal static class Win32 {
        internal const int SRCCOPY=0;
        internal struct RECT { public int left,top,right,bottom; public RECT(int l,int t,int r,int b) { left=l;top=t;right=r;bottom=b; } }
        internal static IntPtr CreateCompatibleDC(IntPtr dc) { creates++; if(fault==1) return IntPtr.Zero; dcs++; return (IntPtr)2; }
        internal static IntPtr CreateCompatibleBitmap(IntPtr dc,int w,int h) { if(fault==2) return IntPtr.Zero; bitmaps++; return (IntPtr)3; }
        internal static IntPtr SelectObject(IntPtr dc,IntPtr obj) { return fault==3 ? new IntPtr(-1) : (IntPtr)4; }
        internal static int SaveDC(IntPtr dc) { return fault==4 ? 0 : 1; }
        internal static bool SetViewportOrgEx(IntPtr dc,int x,int y,IntPtr p) { return fault!=5; }
        internal static int IntersectClipRect(IntPtr dc,int l,int t,int r,int b) { return fault==6 ? 0 : 2; }
        internal static bool RestoreDC(IntPtr dc,int saved) { return fault!=7; }
        internal static bool BitBlt(IntPtr dst,int x,int y,int w,int h,IntPtr src,int sx,int sy,int rop) { blits++; return fault!=8; }
        internal static bool DeleteDC(IntPtr dc) { dcs--; return true; }
        internal static bool DeleteObject(IntPtr obj) { bitmaps--; return true; }
        internal static void OutputDebugStringW(string s) { diagnostics++; }
    }
    static void PaintBackground(IntPtr hwnd,IntPtr dc) { if(dc==(IntPtr)1) direct++; }
'''

# 真实 GDI 补充检查：模拟器不能证明 Windows 允许当前的对象解除/删除顺序。
REAL_C_PREFIX = r'''
#include <windows.h>
#include <stdio.h>
static void paint_background(HWND hwnd,HDC dc) {
    (void)hwnd;
    RECT area={0,0,2000,1200};
    HBRUSH brush=CreateSolidBrush(RGB(31,72,109));
    FillRect(dc,&area,brush); DeleteObject(brush);
}
'''
REAL_C_SUFFIX = r'''
int main(void) {
    HDC screen=GetDC(NULL), dc=CreateCompatibleDC(screen);
    HBITMAP bitmap=CreateCompatibleBitmap(screen,1200,800);
    ReleaseDC(NULL,screen);
    if(!dc || !bitmap) { puts("FAIL real GDI setup"); return 1; }
    HGDIOBJ previous=SelectObject(dc,bitmap);
    /* 先完成一次普通绘制，排除 Windows 首次 FillRect 的固定资源初始化。
       随后的基线只衡量本次缓冲新增的资源，断言仍要求释放后精确归零。 */
    paint_background((HWND)1,dc);
    GdiFlush();
    DWORD baseline=GetGuiResources(GetCurrentProcess(),GR_GDIOBJECTS);
    RECT full={0,0,1200,800}, part={50,20,900,240};
    paint_background_buffered((HWND)1,dc,&full);
    DWORD retained=GetGuiResources(GetCurrentProcess(),GR_GDIOBJECTS);
    for(int i=0;i<1000;i++) {
        SetPixel(dc,60,30,RGB(1,2,3));
        paint_background_buffered((HWND)1,dc,&part);
        if(GetPixel(dc,60,30)!=RGB(31,72,109)) { puts("FAIL offset repaint"); return 1; }
    }
    if(retained!=baseline+2 || GetGuiResources(GetCurrentProcess(),GR_GDIOBJECTS)!=retained) {
        printf("FAIL retained resource count: baseline=%lu first=%lu final=%lu\n", baseline, retained,
               GetGuiResources(GetCurrentProcess(),GR_GDIOBJECTS)); return 1;
    }
    free_background_buffer();
    for(int i=0;i<200;i++) {
        paint_background_buffered((HWND)1,dc,&part);
        paint_background_buffered((HWND)1,dc,&full);
        free_background_buffer();
    }
    GdiFlush();
    if(GetGuiResources(GetCurrentProcess(),GR_GDIOBJECTS)!=baseline) {
        puts("FAIL real GDI disposal leak"); return 1;
    }
    SelectObject(dc,previous); DeleteObject(bitmap); DeleteDC(dc);
    puts("PASS real GDI: 1000 offset repaints, 200 grow/dispose cycles, zero retained objects after release");
    return 0;
}
'''


def run(args, env=None):
    result = subprocess.run([str(a) for a in args], capture_output=True, timeout=60,
                            env=env, creationflags=subprocess.CREATE_NO_WINDOW)
    output = (result.stdout + result.stderr).decode('utf-8', 'replace')
    if result.returncode:
        raise AssertionError(output)
    return output


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--baseline', type=Path)
    args = parser.parse_args()
    source_root = args.baseline or ROOT
    c = (source_root / 'native/src/launcher/ui.c').read_text(encoding='utf-8-sig')
    cs = (source_root / 'native/src/launcher_cs/UiPaint.cs').read_text(encoding='utf-8-sig')
    c_start = 'void paint_background_buffered(' if args.baseline else 'static HDC background_dc;'
    cs_start = '        internal static void PaintBackgroundBuffered(' if args.baseline else '        private static IntPtr _backgroundDc'
    c = c[c.index(c_start):c.index('static void position_control(')]
    cs = cs[cs.index(cs_start):cs.rindex('\n    }')]
    suffix = C_SUFFIX.replace('BASELINE', '1' if args.baseline else '0')
    # 同一组时序与断言转成 C#，仅替换测试框架语法，不改被测生产代码。
    cs_suffix = suffix[suffix.index('int main(void)'):].replace('int main(void)', 'static void Main()')
    cs_suffix = cs_suffix.replace('RECT full={0,0,1200,800}, part={50,20,900,240}, tall={40,0,140,900}, wide={0,100,1250,180};',
        'var full=new Win32.RECT(0,0,1200,800); var part=new Win32.RECT(50,20,900,240); var tall=new Win32.RECT(40,0,140,900); var wide=new Win32.RECT(0,100,1250,180);')
    cs_suffix = cs_suffix.replace('paint_background_buffered', 'PaintBackgroundBuffered').replace('free_background_buffer', 'FreeBackgroundBuffer')
    cs_suffix = cs_suffix.replace('(HWND)', '(IntPtr)').replace('(HDC)', '(IntPtr)').replace('&full', 'full').replace('&part', 'part').replace('&tall', 'tall').replace('&wide', 'wide')
    cs_suffix = cs_suffix.replace('printf("allocations=%d paints=1000\\n",creates);', 'Console.WriteLine("allocations="+creates+" paints=1000");')
    cs_suffix = cs_suffix.replace('if(1)', 'if(true)').replace('if(0)', 'if(false)').replace('return 0;', 'return;').replace('puts(', 'Console.WriteLine(')
    cs_suffix = 'static void check(bool ok) { if(!ok) throw new Exception("failure fault="+fault+" live="+dcs+","+bitmaps+" direct="+direct); }\n' + cs_suffix + '\n}'
    if args.baseline:
        c += '\nvoid free_background_buffer(void) {}\n'
        cs += '\nstatic void FreeBackgroundBuffer() {}\n'
    with tempfile.TemporaryDirectory(prefix='dst_background_buffer_') as temp:
        work = Path(temp)
        (work / 'probe.c').write_text(C_PREFIX + c + suffix, encoding='utf-8')
        (work / 'probe.cs').write_text(CS_PREFIX + cs + cs_suffix, encoding='utf-8-sig')
        gcc = ROOT / 'native/toolchain/w64devkit/bin/gcc.exe'
        env = dict(os.environ, PATH=str(gcc.parent) + os.pathsep + os.environ['PATH'])
        run([gcc, '-std=c11', work / 'probe.c', '-o', work / 'c.exe'], env)
        csc = Path(os.environ['WINDIR']) / 'Microsoft.NET/Framework64/v4.0.30319/csc.exe'
        run([csc, '/nologo', '/out:' + str(work / 'cs.exe'), work / 'probe.cs'])
        for language in ('c', 'cs'):
            print(language + ': ' + run([work / (language + '.exe')]).strip())
        if not args.baseline:
            (work / 'real.c').write_text(REAL_C_PREFIX + c + REAL_C_SUFFIX, encoding='utf-8')
            run([gcc, '-std=c11', work / 'real.c', '-lgdi32', '-luser32', '-o', work / 'real.exe'], env)
            print(run([work / 'real.exe']).strip())


if __name__ == '__main__':
    main()
