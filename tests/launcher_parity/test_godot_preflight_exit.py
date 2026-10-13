"""源链接真实 autoload 预检收尾代码，固定进程在读取就绪凭据后退出的时序。"""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]


def finish_block(path, method, start, end):
    source = (ROOT / path).read_text(encoding="utf-8-sig")
    method_source = source.split(method, 1)[1]
    return start + method_source.split(start, 1)[1].split(end, 1)[0]


C_PREFIX = r'''
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
static int scenario, term_calls;
static DWORD fake_wait(HANDLE process, DWORD ms) {
    (void)process;
    if (scenario == 2 || scenario == 7 || (scenario == 3 && term_calls)) return WAIT_OBJECT_0;
    if (scenario == 5) { SetLastError(6); return WAIT_FAILED; }
    return ms == 1000 && scenario != 6 ? WAIT_OBJECT_0 : WAIT_TIMEOUT;
}
static BOOL fake_terminate(HANDLE process, UINT code) {
    (void)process; (void)code; term_calls++;
    if (scenario == 2 || scenario == 3 || scenario == 4 || scenario == 7) { SetLastError(5); return FALSE; }
    return TRUE;
}
static BOOL fake_exit(HANDLE process, LPDWORD code) {
    (void)process;
    if (scenario == 7) { SetLastError(31); return FALSE; }
    *code = 1; return TRUE;
}
static void append_log(const WCHAR *format, ...) {
    va_list args; va_start(args, format); vwprintf(format, args); va_end(args); wprintf(L"\n");
}
#define WaitForSingleObject fake_wait
#define TerminateProcess fake_terminate
#define GetExitCodeProcess fake_exit
int main(int argc, char **argv) {
    if (argc != 2) return 2;
    scenario = atoi(argv[1]);
    PROCESS_INFORMATION pi = {0};
    DWORD waited = WAIT_TIMEOUT;
    int ok = scenario != 0;
'''
C_SUFFIX = '\n    wprintf(L"result=%d terminated=%d\\n", ok, term_calls); return 0;\n}\n'
CS_PREFIX = r'''
using System;
class Probe {
    const uint WAIT_OBJECT_0 = 0, WAIT_TIMEOUT = 258, WAIT_FAILED = 0xffffffff;
    static int scenario, termCalls, error;
    struct ProcessInfo { public IntPtr hProcess; }
    static uint WaitForSingleObject(IntPtr process, uint ms) {
        if (scenario == 2 || scenario == 7 || (scenario == 3 && termCalls > 0)) return WAIT_OBJECT_0;
        if (scenario == 5) { error = 6; return WAIT_FAILED; }
        return ms == 1000 && scenario != 6 ? WAIT_OBJECT_0 : WAIT_TIMEOUT;
    }
    static bool TerminateProcess(IntPtr process, uint code) {
        termCalls++;
        if (scenario == 2 || scenario == 3 || scenario == 4 || scenario == 7) { error = 5; return false; }
        return true;
    }
    static bool GetExitCodeProcess(IntPtr process, out uint code) {
        code = 1;
        if (scenario == 7) { error = 31; return false; }
        return true;
    }
    static class Log { public static void Append(string text) { Console.WriteLine(text); } }
    static int Main(string[] args) {
        scenario = int.Parse(args[0]);
        var pi = new ProcessInfo { hProcess = IntPtr.Zero };
        uint waited = WAIT_TIMEOUT;
        bool ok = scenario != 0;
'''
CS_SUFFIX = '\n        Console.WriteLine("result=" + (ok ? 1 : 0) + " terminated=" + termCalls); return 0;\n    }\n}\n'


def execute(args):
    result = subprocess.run([str(arg) for arg in args], capture_output=True, timeout=30,
                            creationflags=subprocess.CREATE_NO_WINDOW)
    output = (result.stdout + result.stderr).decode("utf-8", "replace").replace("\r\n", "\n")
    assert result.returncode == 0, output
    return output


def main():
    c = finish_block('native/src/launcher/ui.c', 'static int godot_runtime_autoload_preflight(',
                     '    DWORD exit_code = 1;', '    CloseHandle(reader); CloseHandle(capture);')
    cs = finish_block('native/src/launcher_cs/GodotLaunch.cs', 'public static bool RuntimeAutoloadPreflight(',
                      '            uint exitCode = 1;', '            reader.Dispose(); capture.Dispose();')
    cs = cs.replace('Marshal.GetLastWin32Error()', 'error')
    root = Path(tempfile.mkdtemp(prefix='dst_preflight_exit_')).resolve()
    try:
        (root / 'probe.c').write_text(C_PREFIX + c + C_SUFFIX, encoding='utf-8')
        (root / 'probe.cs').write_text(CS_PREFIX + cs + CS_SUFFIX, encoding='utf-8')
        gcc = ROOT / 'native/toolchain/w64devkit/bin/gcc.exe'
        if not gcc.is_file():
            gcc = shutil.which('gcc')
            assert gcc, 'gcc not found'
        os.environ['PATH'] = str(Path(gcc).parent) + os.pathsep + os.environ['PATH']
        execute([gcc, '-std=c17', '-O2', '-Wall', '-Wextra', '-Werror', root / 'probe.c', '-o', root / 'c.exe'])
        csc = Path(os.environ['WINDIR']) / 'Microsoft.NET/Framework64/v4.0.30319/csc.exe'
        execute([csc, '/nologo', '/target:exe', '/out:' + str(root / 'cs.exe'), root / 'probe.cs'])
        cases = (
            ('no proof', 0, 'timed out without a load proof.', 'result=0 terminated=1'),
            ('live proof', 1, 'load proof confirmed;', 'result=1 terminated=1'),
            ('exit during proof read', 2, 'exited with code 1.', 'result=0 terminated=0'),
            ('exit racing termination', 3, 'exited with code 1.', 'result=0 terminated=1'),
            ('real termination denial', 4, 'termination failed (Windows error 5).', 'result=0 terminated=1'),
            ('invalid process handle', 5, 'wait failed (Windows error 6).', 'result=0 terminated=0'),
            ('termination wait expires', 6, 'termination failed', 'result=0 terminated=1'),
            ('exit code read fails', 7, 'exit code read failed (Windows error 31).', 'result=0 terminated=0'),
        )
        for name, mode, message, status in cases:
            outputs = [execute([root / binary, mode]) for binary in ('c.exe', 'cs.exe')]
            assert outputs[0] == outputs[1], name + ': C/C# differ:\n' + '\n'.join(outputs)
            assert message in outputs[0] and status in outputs[0], name + ':\n' + outputs[0]
            if mode in (2, 3, 7):
                assert 'termination failed' not in outputs[0] and 'timed out' not in outputs[0], outputs[0]
            print('[PASS] Godot preflight C/C#: ' + name)
    finally:
        assert root.parent == Path(tempfile.gettempdir()).resolve() and root.name.startswith('dst_preflight_exit_')
        for entry in root.iterdir():
            if not entry.is_file() or entry.is_symlink():
                raise RuntimeError('Unexpected fixture entry retained: ' + str(entry))
            entry.unlink()
        root.rmdir()


if __name__ == '__main__':
    main()
