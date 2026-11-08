#include <stdio.h>
#include "../../native/src/launcher/api_models.c"
int wmain(int argc, WCHAR **argv) {
    if (argc == 3 && !wcscmp(argv[1], L"wait")) { printf("%d\n", api_connection_wait_ms(argv[2])); return 0; }
    if (argc != 4) return 2;
    ApiConnection r; api_connection_test(argv[1], argv[2], argv[3], &r);
    char text[2048]; WideCharToMultiByte(CP_UTF8, 0, r.message, -1, text, sizeof(text), NULL, NULL);
    printf("success=%d\n%s\n", r.success, text); return 0;
}
