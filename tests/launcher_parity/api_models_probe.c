/* 源链接生产实现，分页测试只替换请求地址，不向真实厂商发送请求。 */
#include "api_models.c"
#include <stdio.h>
#include <stdlib.h>
static void print(const WCHAR *s) {
    int n = WideCharToMultiByte(CP_UTF8, 0, s, -1, NULL, 0, NULL, NULL);
    char *b = malloc((size_t)n);
    if (!b) exit(9);
    WideCharToMultiByte(CP_UTF8, 0, s, -1, b, n, NULL, NULL);
    puts(b); free(b);
}
int wmain(int argc, WCHAR **argv) {
    if (argc < 3) return 2;
    if (!wcscmp(argv[1], L"url")) {
        WCHAR url[1100]; print(api_models_url(argv[2], url, 1100) ? url : L"INVALID"); return 0;
    }
    ApiModels r;
    if (!wcscmp(argv[1], L"catalog") && argc == 5) fetch_catalog(argv[2], argv[3], _wtoi(argv[4]), &r);
    else api_models_fetch(argv[2], argc > 3 ? argv[3] : L"", &r);
    printf("count=%d\n", r.count); print(r.message);
    for (int i = 0; i < r.count; i++) print(r.items[i]);
    api_models_free(&r); return 0;
}
