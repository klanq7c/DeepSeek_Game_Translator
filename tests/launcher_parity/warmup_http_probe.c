/* HTTP 回归宿主：直接运行生产预热入口，仅把 UI 日志输出到 stdout。 */
#include "warmup.h"
#include "ui.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

static size_t allocation_fail_size;
static int allocation_faults;
/* 仅替换本探针链接的 fsutil.c realloc；生产构建不包含此入口。 */
void *dst_test_realloc(void *ptr, size_t size) {
    if (allocation_fail_size == size && !allocation_faults) {
        allocation_faults++;
        return NULL;
    }
    return realloc(ptr, size);
}

void write_stdout_utf8(const WCHAR *s) {
    char bytes[16384];
    int n = WideCharToMultiByte(CP_UTF8, 0, s, -1, bytes, sizeof bytes, NULL, NULL);
    if (n > 0) fwrite(bytes, 1, (size_t)n - 1, stdout);
}

void append_log(const WCHAR *fmt, ...) {
    WCHAR line[4096];
    va_list args;
    va_start(args, fmt);
    _vsnwprintf(line, 4095, fmt, args);
    va_end(args);
    line[4095] = 0;
    write_stdout_utf8(line);
    write_stdout_utf8(L"\n");
}

int wmain(int argc, WCHAR **argv) {
    if (argc < 3) return 2;
    g_warmup_dump_stdout = argc > 3 && !wcscmp(argv[3], L"dump");
    if (argc > 3 && !wcsncmp(argv[3], L"fault=", 6)) allocation_fail_size = (size_t)_wtoi(argv[3] + 6);
    warmup_translations(argv[1], (Engine)_wtoi(argv[2]));
    if (allocation_fail_size) printf("allocation_faults=%d\n", allocation_faults);
    return 0;
}
