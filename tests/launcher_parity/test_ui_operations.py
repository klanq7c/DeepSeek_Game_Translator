"""源链接真实按钮入口与操作执行器；服务/文件边界由事件控制，不接触现有服务。"""
import os
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]


def section(text, start, end):
    return text[text.index(start):text.index(end, text.index(start))]


def run(args, env=None):
    result = subprocess.run([str(x) for x in args], capture_output=True, text=True,
                            encoding='utf-8', errors='replace', env=env, timeout=30)
    if result.returncode:
        raise AssertionError(result.stdout + result.stderr)
    return result.stdout


def main():
    c = (ROOT / 'native/src/launcher/ui.c').read_text(encoding='utf-8-sig')
    server = (ROOT / 'native/src/launcher/server_proc.c').read_text(encoding='utf-8-sig')
    cs = (ROOT / 'native/src/launcher_cs/MainWindow.cs').read_text(encoding='utf-8-sig')
    cs_server = (ROOT / 'native/src/launcher_cs/ServerProcess.cs').read_text(encoding='utf-8-sig')
    c_clear_start = 'typedef struct { int cleared, restarted; }'
    cs_clear_start = '        private sealed class ClearCacheResult'
    c_code = section(c, c_clear_start, '/* 刷新引擎卡片')
    c_code += section(c, 'typedef struct { WCHAR dir[MAX_PATH * 4]; Engine engine; int ok; }', '/* 发行版模板可能')
    c_code += section(c, 'typedef struct {\n    WCHAR dir[MAX_PATH * 4];\n    Engine engine;\n} WarmupLaunchArgs;', 'int run_engine_launch_flow')
    c_code += section(c, 'static void report_server_start_failure', '/* 按引擎部署翻译钩子')
    c_code += c[c.index('static void warmup_launch_thread'):]
    c_code += server[server.index('static void toggle_server_work'):]
    cs_code = section(cs, cs_clear_start, '        /* 本地翻译服务未就绪')
    cs_code += section(cs, '        internal static void RestoreSelectedGame()', '        /* 确认后删除共享翻译缓存')
    cs_code += section(cs, '        private static void ReportServerStartFailure()', '        /* ---------------- 窗口过程')
    cs_toggle = section(cs_server, '        private static void ToggleWork', '        /* ---------------- 二进制选择')
    main_c = (ROOT / 'native/src/launcher/main.c').read_text(encoding='utf-8-sig')
    c_dispatch = section(main_c, '    case WM_TIMER:', '    /* ---- WM_DPICHANGED')
    c_dispatch += section(main_c, '    case WM_CLOSE:', '    case WM_DESTROY:')
    cs_dispatch = section(cs, '                case Win32.WM_TIMER:', '                case Win32.WM_DPICHANGED:')
    cs_dispatch += section(cs, '                case Win32.WM_CLOSE:', '                case Win32.WM_DESTROY:')
    prefix = (ROOT / 'tests/launcher_parity/ui_operations_probe.c.txt').read_text(encoding='utf-8')
    cs_prefix = (ROOT / 'tests/launcher_parity/UiOperationsProbe.cs.txt').read_text(encoding='utf-8')
    with tempfile.TemporaryDirectory(prefix='dst_ui_ops_') as temp:
        work = Path(temp)
        (work / 'probe.c').write_text(prefix.replace('/* REAL_ENTRIES */', c_code).replace('/* REAL_DISPATCH */', c_dispatch), encoding='utf-8')
        (work / 'probe.cs').write_text(cs_prefix.replace('/* REAL_ENTRIES */', cs_code).replace('/* REAL_TOGGLE */', cs_toggle).replace('/* REAL_DISPATCH */', cs_dispatch), encoding='utf-8')
        gcc = ROOT / 'native/toolchain/w64devkit/bin/gcc.exe'
        env = dict(os.environ, PATH=str(gcc.parent) + os.pathsep + os.environ['PATH'])
        c_args = [gcc, '-std=c17', '-Wall', '-Wextra', '-Werror',
                  '-I' + str(ROOT / 'native/src/launcher'), work / 'probe.c',
                  '-DDST_UI_OPERATION_TEST', ROOT / 'native/src/launcher/ui_operation.c']
        run(c_args + ['-o', work / 'c.exe', '-luser32', '-lole32'], env)
        csc = Path(os.environ['WINDIR']) / 'Microsoft.NET/Framework64/v4.0.30319/csc.exe'
        cs_args = [csc, '/nologo', '/out:' + str(work / 'cs.exe'), work / 'probe.cs']
        cs_args += [ROOT / 'native/src/launcher_cs/UiOperation.cs']
        run(cs_args)
        errors = []
        for language in ('c', 'cs'):
            for scenario in ('clear', 'toggle', 'start', 'restore', 'stop-failure', 'restart-failure', 'create-failure', 'start-create-failure', 'restore-create-failure', 'close', 'window-close', 'start-close', 'restore-close', 'modal-close', 'modal-reentry', 'cancel'):
                try:
                    print(language + '/' + scenario + ': ' + run([work / (language + '.exe'), scenario]).strip())
                except AssertionError as error:
                    errors.append(language + '/' + scenario + ': ' + str(error))
        assert not errors, '\n'.join(errors)


if __name__ == '__main__':
    main()
