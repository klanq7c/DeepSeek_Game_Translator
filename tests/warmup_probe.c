#define WIN32_LEAN_AND_MEAN
#include "../native/src/launcher/warmup.c"

#include <stdarg.h>

void bb_add(ByteBuf *b, const char *s, size_t n) {
    if (!b || !s || n == 0) return;
    if (!b->data) {
        b->cap = n + 64;
        b->data = (char *)malloc(b->cap);
        if (!b->data) {
            b->cap = 0;
            return;
        }
        b->len = 0;
        b->data[0] = 0;
    }
    if (b->len + n + 1 > b->cap) {
        size_t cap = b->cap;
        while (cap < b->len + n + 1) cap *= 2;
        char *grown = (char *)realloc(b->data, cap);
        if (!grown) return;
        b->data = grown;
        b->cap = cap;
    }
    memcpy(b->data + b->len, s, n);
    b->len += n;
    b->data[b->len] = 0;
}

void path_join(WCHAR *out, size_t cap, const WCHAR *a, const WCHAR *b) {
    (void)a;
    (void)b;
    if (out && cap) out[0] = 0;
}

int is_dir(const WCHAR *p) {
    (void)p;
    return 0;
}

int read_file_bytes(const WCHAR *path, char **out, DWORD *size) {
    (void)path;
    if (out) *out = NULL;
    if (size) *size = 0;
    return 0;
}

void append_log(const WCHAR *fmt, ...) {
    (void)fmt;
}

static int extract_line(const char *line, char **out) {
    *out = NULL;
    char *copy = dup_range(line, strlen(line));
    if (!copy) return 0;
    const char *quote = renpy_first_quote(copy);
    if (!quote || renpy_skip_statement(copy, quote)) {
        free(copy);
        return 0;
    }
    const char *cursor = quote;
    *out = renpy_string_at(&cursor);
    free(copy);
    return *out != NULL;
}

static int expect_extract(const char *line, const char *expected) {
    char *actual = NULL;
    int ok = extract_line(line, &actual) && !strcmp(actual, expected);
    free(actual);
    return ok;
}

static int write_probe_file(const WCHAR *path, const char *text) {
    HANDLE h = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return 0;
    DWORD size = (DWORD)strlen(text);
    DWORD written = 0;
    int ok = WriteFile(h, text, size, &written, NULL) && written == size;
    CloseHandle(h);
    return ok;
}

static int file_equals(const WCHAR *path, const char *expected) {
    HANDLE h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return 0;
    char buf[64] = {0};
    DWORD read = 0;
    int ok = ReadFile(h, buf, sizeof buf - 1, &read, NULL);
    CloseHandle(h);
    return ok && read == strlen(expected) && !memcmp(buf, expected, read);
}

static int expect_backup_once(void) {
    WCHAR temp_dir[MAX_PATH];
    WCHAR source[MAX_PATH];
    WCHAR backup[MAX_PATH * 4];
    if (!GetTempPathW(MAX_PATH, temp_dir)) return 0;
    if (!GetTempFileNameW(temp_dir, L"dsw", 0, source)) return 0;
    _snwprintf(backup, MAX_PATH * 4, L"%s.deepseek.bak", source);
    backup[MAX_PATH * 4 - 1] = 0;
    DeleteFileW(backup);

    int ok = write_probe_file(source, "original");
    if (ok) backup_once(source);
    if (ok) ok = write_probe_file(source, "modified");
    if (ok) backup_once(source);
    if (ok) ok = file_equals(backup, "original");

    DeleteFileW(backup);
    DeleteFileW(source);
    return ok;
}

int main(void) {
    if (!expect_extract("    e \"Hello, world.\"", "Hello, world.")) return 1;
    if (!expect_extract("    e 'Single quoted choice.'", "Single quoted choice.")) return 2;
    if (!expect_extract("    e \"Hello, \\\"friend\\\".\\nNext\"", "Hello, \"friend\".\nNext")) return 3;

    char *text = NULL;
    if (extract_line("    define icon = \"portrait.png\"", &text)) {
        free(text);
        return 4;
    }
    if (extract_line("    e \"Unclosed dialogue", &text)) {
        free(text);
        return 5;
    }
    if (extract_line("    \"\"\"Documentation string\"\"\"", &text)) {
        free(text);
        return 6;
    }

    TextList list = {0};
    char resource[] = "portrait.png";
    collect_renpy_string(resource, &list);
    if (list.n != 0) {
        textlist_free(&list);
        return 7;
    }

    char dialogue[] = "A sufficiently clear dialogue line.";
    collect_renpy_string(dialogue, &list);
    if (list.n != 1 || strcmp(list.items[0], dialogue)) {
        textlist_free(&list);
        return 8;
    }
    textlist_free(&list);
    if (!should_warm_rpgm_text("\\C[20]Auntie Daisy\\C[0] <br>")) return 9;
    if (!should_warm_rpgm_text("My goodness, \\n[1]. <br>")) return 10;
    if (should_warm_rpgm_text("C:\\Users\\player\\save")) return 11;
    TextList quests = {0};
    char questTitle[] = "<quest 180:Danesia Questline #1 - The First Meeting|1|0>";
    char questBody[] = "Apparently there’s a blacksmith around here.";
    collect_rpgm_text_line(questTitle, &quests);
    collect_rpgm_text_line(questBody, &quests);
    if (quests.n != 2 ||
        strcmp(quests.items[0], "Danesia Questline #1 - The First Meeting") ||
        strcmp(quests.items[1], questBody)) {
        textlist_free(&quests);
        return 12;
    }
    textlist_free(&quests);
    if (!expect_backup_once()) return 13;
    puts("warmup probe passed");
    return 0;
}
