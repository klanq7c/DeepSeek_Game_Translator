#define WIN32_LEAN_AND_MEAN
#include "../native/src/launcher/warmup.c"
#include "../native/src/launcher/godot_warmup.c"

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
    if (!out || cap == 0) return;
    _snwprintf(out, cap, L"%s%s%s", a,
               (a[0] && a[wcslen(a) - 1] != L'\\') ? L"\\" : L"", b);
    out[cap - 1] = 0;
}

int path_append_suffix(WCHAR *out, size_t cap, const WCHAR *path,
                       const WCHAR *suffix) {
    if (!out || !cap || !path || !suffix) return 0;
    out[0] = 0;
    size_t path_len = wcslen(path);
    size_t suffix_len = wcslen(suffix);
    if (path_len >= cap || suffix_len >= cap - path_len) {
        SetLastError(ERROR_INSUFFICIENT_BUFFER);
        return 0;
    }
    memcpy(out, path, path_len * sizeof(*out));
    memcpy(out + path_len, suffix, (suffix_len + 1) * sizeof(*out));
    return 1;
}

int is_dir(const WCHAR *p) {
    DWORD a = GetFileAttributesW(p);
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

int exists_path(const WCHAR *p) {
    return GetFileAttributesW(p) != INVALID_FILE_ATTRIBUTES;
}

int read_file_bytes(const WCHAR *path, char **out, DWORD *size) {
    if (!path || !out || !size) return 0;
    *out = NULL;
    *size = 0;
    HANDLE h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return 0;
    LARGE_INTEGER li;
    if (!GetFileSizeEx(h, &li) || li.QuadPart < 0 || li.QuadPart > 0x7ffffffe) {
        CloseHandle(h);
        return 0;
    }
    DWORD sz = (DWORD)li.QuadPart;
    char *buf = (char *)malloc(sz + 1);
    if (!buf) {
        CloseHandle(h);
        return 0;
    }
    DWORD read = 0;
    int ok = sz == 0 || (ReadFile(h, buf, sz, &read, NULL) && read == sz);
    CloseHandle(h);
    if (!ok) {
        free(buf);
        return 0;
    }
    buf[sz] = 0;
    *out = buf;
    *size = sz;
    return 1;
}

int write_file_bytes(const WCHAR *path, const char *buf, DWORD size) {
    HANDLE h = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return 0;
    DWORD written = 0;
    int ok = WriteFile(h, buf, size, &written, NULL) && written == size;
    CloseHandle(h);
    return ok;
}

int copy_file_if_absent_safe(const WCHAR *from, const WCHAR *to) {
    return CopyFileW(from, to, TRUE);
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

static int write_probe_bytes(const WCHAR *path, const unsigned char *bytes, DWORD size) {
    HANDLE h = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return 0;
    DWORD written = 0;
    int ok = WriteFile(h, bytes, size, &written, NULL) && written == size;
    CloseHandle(h);
    return ok;
}

static void put_u32le(unsigned char *p, uint32_t v) {
    p[0] = (unsigned char)(v & 0xff);
    p[1] = (unsigned char)((v >> 8) & 0xff);
    p[2] = (unsigned char)((v >> 16) & 0xff);
    p[3] = (unsigned char)((v >> 24) & 0xff);
}

static void put_u64le(unsigned char *p, uint64_t v) {
    put_u32le(p, (uint32_t)(v & 0xffffffffu));
    put_u32le(p + 4, (uint32_t)(v >> 32));
}

static int write_probe_embedded_godot_pck(const WCHAR *path) {
    static const char entry_path[] = "dialog.translation";
    static const unsigned char payload[] =
        "RSRC\0Embedded pack dialogue.\0Start embedded journey.\0";
    const uint32_t path_len = (uint32_t)strlen(entry_path);
    const uint32_t dir_pos = GODOT_PCK_V2_HEADER_SIZE;
    const uint32_t data_off = dir_pos + 4 + path_len + 8 + 8 + 16 + 4;
    const uint32_t payload_size = (uint32_t)(sizeof(payload) - 1);
    const uint32_t pck_size = data_off + payload_size;
    const uint32_t raw_ptr = 0x200;
    const uint32_t raw_size = pck_size + 12;
    const uint32_t exe_size = raw_ptr + raw_size;
    unsigned char *exe = (unsigned char *)calloc(exe_size, 1);
    if (!exe) return 0;

    exe[0] = 'M';
    exe[1] = 'Z';
    put_u32le(exe + 0x3c, 0x80);
    exe[0x80] = 'P';
    exe[0x81] = 'E';
    exe[0x84] = 0x64;
    exe[0x85] = 0x86;
    exe[0x86] = 1;
    exe[0x94] = 0xf0;
    memcpy(exe + 0x188, "pck", 3);
    put_u32le(exe + 0x188 + 16, raw_size);
    put_u32le(exe + 0x188 + 20, raw_ptr);

    unsigned char *pck = exe + raw_ptr;
    put_u32le(pck, GODOT_PCK_MAGIC);
    put_u32le(pck + 4, 2);
    put_u32le(pck + 8, 4);
    put_u32le(pck + 12, 4);
    put_u32le(pck + 16, 1);
    put_u64le(pck + 24, 0);
    put_u32le(pck + 96, 1);
    unsigned char *e = pck + dir_pos;
    put_u32le(e, path_len);
    e += 4;
    memcpy(e, entry_path, path_len);
    e += path_len;
    put_u64le(e, data_off);
    e += 8;
    put_u64le(e, payload_size);
    e += 8 + 16;
    put_u32le(e, 0);
    memcpy(pck + data_off, payload, payload_size);
    put_u64le(exe + raw_ptr + pck_size, pck_size);
    memcpy(exe + raw_ptr + pck_size + 8, "GDPC", 4);

    int ok = write_probe_bytes(path, exe, exe_size);
    free(exe);
    return ok;
}

static int write_probe_embedded_godot_pck_v1(const WCHAR *path) {
    static const char entry_path[] = "scene.tscn";
    static const unsigned char payload[] =
        "[node name=\"Label\" type=\"Label\"]\ntext = \"V1 embedded dialogue.\"\n";
    const uint32_t path_len = (uint32_t)strlen(entry_path);
    const uint32_t dir_pos = GODOT_PCK_V1_HEADER_SIZE;
    const uint32_t data_off = dir_pos + 4 + path_len + 8 + 8 + 16;
    const uint32_t payload_size = (uint32_t)(sizeof(payload) - 1);
    const uint32_t pck_size = data_off + payload_size;
    const uint32_t raw_ptr = 0x200;
    const uint32_t raw_size = pck_size + 12;
    const uint32_t exe_size = raw_ptr + raw_size;
    unsigned char *exe = (unsigned char *)calloc(exe_size, 1);
    if (!exe) return 0;

    exe[0] = 'M';
    exe[1] = 'Z';
    put_u32le(exe + 0x3c, 0x80);
    exe[0x80] = 'P';
    exe[0x81] = 'E';
    exe[0x84] = 0x64;
    exe[0x85] = 0x86;
    exe[0x86] = 1;
    exe[0x94] = 0xf0;
    memcpy(exe + 0x188, "pck", 3);
    put_u32le(exe + 0x188 + 16, raw_size);
    put_u32le(exe + 0x188 + 20, raw_ptr);

    unsigned char *pck = exe + raw_ptr;
    put_u32le(pck, GODOT_PCK_MAGIC);
    put_u32le(pck + 4, 1);
    put_u32le(pck + 8, 3);
    put_u32le(pck + 12, 5);
    put_u32le(pck + 16, 2);
    put_u32le(pck + 84, 1);
    unsigned char *e = pck + dir_pos;
    put_u32le(e, path_len);
    e += 4;
    memcpy(e, entry_path, path_len);
    e += path_len;
    put_u64le(e, raw_ptr + data_off);
    e += 8;
    put_u64le(e, payload_size);
    e += 8 + 16;
    memcpy(pck + data_off, payload, payload_size);
    put_u64le(exe + raw_ptr + pck_size, pck_size);
    memcpy(exe + raw_ptr + pck_size + 8, "GDPC", 4);

    int ok = write_probe_bytes(path, exe, exe_size);
    free(exe);
    return ok;
}

static int write_probe_godot_pck_with_unknown_binary(const WCHAR *path) {
    static const char scene_path[] = "scene.tscn";
    static const unsigned char scene_payload[] =
        "[node name=\"Label\" type=\"Label\"]\ntext = \"Packed scene dialogue.\"\n";
    static const char blob_path[] = "data/blob.bin";
    static const unsigned char blob_payload[] =
        "bin\0Hidden generic blob text.\0";
    const uint32_t scene_path_len = (uint32_t)strlen(scene_path);
    const uint32_t blob_path_len = (uint32_t)strlen(blob_path);
    const uint32_t scene_size = (uint32_t)(sizeof(scene_payload) - 1);
    const uint32_t blob_size = (uint32_t)(sizeof(blob_payload) - 1);
    const uint32_t dir_pos = GODOT_PCK_V2_HEADER_SIZE;
    const uint32_t scene_entry_size = 4 + scene_path_len + 8 + 8 + 16 + 4;
    const uint32_t blob_entry_size = 4 + blob_path_len + 8 + 8 + 16 + 4;
    const uint32_t scene_data_off = dir_pos + scene_entry_size + blob_entry_size;
    const uint32_t blob_data_off = scene_data_off + scene_size;
    const uint32_t pck_size = blob_data_off + blob_size;
    unsigned char *pck = (unsigned char *)calloc(pck_size, 1);
    if (!pck) return 0;

    put_u32le(pck, GODOT_PCK_MAGIC);
    put_u32le(pck + 4, 2);
    put_u32le(pck + 8, 4);
    put_u32le(pck + 12, 4);
    put_u32le(pck + 16, 1);
    put_u64le(pck + 24, 0);
    put_u32le(pck + 96, 2);

    unsigned char *e = pck + dir_pos;
    put_u32le(e, scene_path_len);
    e += 4;
    memcpy(e, scene_path, scene_path_len);
    e += scene_path_len;
    put_u64le(e, scene_data_off);
    e += 8;
    put_u64le(e, scene_size);
    e += 8 + 16;
    put_u32le(e, 0);
    e += 4;

    put_u32le(e, blob_path_len);
    e += 4;
    memcpy(e, blob_path, blob_path_len);
    e += blob_path_len;
    put_u64le(e, blob_data_off);
    e += 8;
    put_u64le(e, blob_size);
    e += 8 + 16;
    put_u32le(e, 0);

    memcpy(pck + scene_data_off, scene_payload, scene_size);
    memcpy(pck + blob_data_off, blob_payload, blob_size);

    int ok = write_probe_bytes(path, pck, pck_size);
    free(pck);
    return ok;
}

static int write_probe_godot_pck_v3(const WCHAR *path) {
    static const char entry_path[] = "ui/menu.tscn";
    static const unsigned char payload[] =
        "[node name=\"Label\" type=\"Label\"]\ntext = \"Format 3 menu text.\"\n";
    const uint32_t path_len = 16u;
    const uint32_t payload_size = (uint32_t)(sizeof(payload) - 1u);
    const uint32_t data_off = 112u;
    const uint32_t directory_off = data_off + payload_size;
    const uint32_t pck_size = directory_off + 4u + 4u + path_len + 8u + 8u + 16u + 4u;
    unsigned char *pck = (unsigned char *)calloc(pck_size, 1);
    if (!pck) return 0;

    put_u32le(pck, GODOT_PCK_MAGIC);
    put_u32le(pck + 4, 3u);
    put_u32le(pck + 8, 4u);
    put_u32le(pck + 12, 6u);
    put_u32le(pck + 16, 2u);
    put_u64le(pck + 24, data_off);
    put_u64le(pck + 32, directory_off);
    memcpy(pck + data_off, payload, payload_size);

    unsigned char *e = pck + directory_off;
    put_u32le(e, 1u);
    e += 4;
    put_u32le(e, path_len);
    e += 4;
    memcpy(e, entry_path, sizeof(entry_path));
    e += path_len;
    put_u64le(e, 0u);
    e += 8;
    put_u64le(e, payload_size);
    e += 8 + 16;
    put_u32le(e, 0u);

    int ok = write_probe_bytes(path, pck, pck_size);
    free(pck);
    return ok;
}

static int textlist_has(TextList *list, const char *text) {
    for (size_t i = 0; i < list->n; i++) {
        if (!strcmp(list->items[i], text)) return 1;
    }
    return 0;
}

static void cleanup_godot_fixture(const WCHAR *root) {
    WCHAR p[MAX_PATH * 4];
    path_join(p, MAX_PATH * 4, root, L"scene.tscn"); DeleteFileW(p);
    path_join(p, MAX_PATH * 4, root, L"dialog.po"); DeleteFileW(p);
    path_join(p, MAX_PATH * 4, root, L"strings.csv"); DeleteFileW(p);
    path_join(p, MAX_PATH * 4, root, L"script.gd"); DeleteFileW(p);
    path_join(p, MAX_PATH * 4, root, L"story.md"); DeleteFileW(p);
    path_join(p, MAX_PATH * 4, root, L"pack.pck"); DeleteFileW(p);
    path_join(p, MAX_PATH * 4, root, L"dst_godot_runtime.gd"); DeleteFileW(p);
    path_join(p, MAX_PATH * 4, root, L"dst_godot_patch.pck"); DeleteFileW(p);
    path_join(p, MAX_PATH * 4, root, L"dst_godot_patch.next.pck"); DeleteFileW(p);
    path_join(p, MAX_PATH * 4, root, L"dst_godot_patch.building"); DeleteFileW(p);
    path_join(p, MAX_PATH * 4, root, L"embedded.exe"); DeleteFileW(p);
    path_join(p, MAX_PATH * 4, root, L"embedded_v1.exe"); DeleteFileW(p);
    path_join(p, MAX_PATH * 4, root, L".godot\\ignored.tscn"); DeleteFileW(p);
    path_join(p, MAX_PATH * 4, root, L".godot"); RemoveDirectoryW(p);
    RemoveDirectoryW(root);
}

static int expect_godot_directory_scan(void) {
    WCHAR temp_dir[MAX_PATH];
    WCHAR root[MAX_PATH];
    if (!GetTempPathW(MAX_PATH, temp_dir)) return 0;
    if (!GetTempFileNameW(temp_dir, L"dsg", 0, root)) return 0;
    DeleteFileW(root);
    if (!CreateDirectoryW(root, NULL)) return 0;

    WCHAR p[MAX_PATH * 4];
    int ok = 1;
    path_join(p, MAX_PATH * 4, root, L"scene.tscn");
    ok = ok && write_probe_file(p,
        "[node name=\"Label\" type=\"Label\"]\n"
        "text = \"Start your journey.\"\n"
        "text = \"Start\"\n");

    path_join(p, MAX_PATH * 4, root, L"dialog.po");
    ok = ok && write_probe_file(p,
        "msgctxt \"menu\"\n"
        "msgid \"Line \"\n"
        "\"Two\"\n"
        "msgstr \"Ligne Deux\"\n");

    path_join(p, MAX_PATH * 4, root, L"strings.csv");
    ok = ok && write_probe_file(p,
        "keys,en\n"
        "MENU_OPTIONS,Options\n"
        "\"Start your journey.\",\"Start your journey.\"\n");

    path_join(p, MAX_PATH * 4, root, L"script.gd");
    ok = ok && write_probe_file(p,
        "var title = tr(\"Continue\")\n"
        "var body = TranslationServer.translate(\"Resume your journey.\")\n"
        "const ID = \"MenuRoot\"\n");

    path_join(p, MAX_PATH * 4, root, L"story.md");
    ok = ok && write_probe_file(p,
        "# introduction\n\n"
        "> Melia\n"
        "If we're lucky, this is the last time we're crossing the border.\n\n"
        "[window_opens]\n"
        "[center]A warm welcome awaits you.[/center]\n"
        "You can fully customize your character at any time.\n");

    static const unsigned char pck[] = "bin\0Caf\xc3\xa9 rendezvous.\0res://icons/start.png\0";
    path_join(p, MAX_PATH * 4, root, L"pack.pck");
    ok = ok && write_probe_bytes(p, pck, (DWORD)(sizeof(pck) - 1));

    path_join(p, MAX_PATH * 4, root, L"dst_godot_runtime.gd");
    ok = ok && write_probe_file(p, "var generated = tr(\"Generated runtime text.\")\n");
    static const unsigned char generated_pck[] = "bin\0Generated patch payload.\0";
    path_join(p, MAX_PATH * 4, root, L"dst_godot_patch.pck");
    ok = ok && write_probe_bytes(p, generated_pck, (DWORD)(sizeof(generated_pck) - 1));
    path_join(p, MAX_PATH * 4, root, L"dst_godot_patch.next.pck");
    ok = ok && write_probe_bytes(p, generated_pck, (DWORD)(sizeof(generated_pck) - 1));

    path_join(p, MAX_PATH * 4, root, L".godot");
    ok = ok && CreateDirectoryW(p, NULL);
    path_join(p, MAX_PATH * 4, root, L".godot\\ignored.tscn");
    ok = ok && write_probe_file(p, "text = \"Should not appear.\"\n");

    TextList list = {0};
    list.max_items = GODOT_WARMUP_MAX_ITEMS;
    if (ok) {
        warmup_scan_godot_resources(root, &list);
        ok = textlist_has(&list, "Start your journey.") &&
             textlist_has(&list, "Start") &&
             textlist_has(&list, "Line Two") &&
             textlist_has(&list, "Options") &&
             textlist_has(&list, "Continue") &&
             textlist_has(&list, "Resume your journey.") &&
             textlist_has(&list, "If we're lucky, this is the last time we're crossing the border.") &&
             textlist_has(&list, "A warm welcome awaits you.") &&
             textlist_has(&list, "You can fully customize your character at any time.") &&
             textlist_has(&list, "Caf\xc3\xa9 rendezvous.") &&
             !textlist_has(&list, "Label") &&
             !textlist_has(&list, "MenuRoot") &&
             textlist_has(&list, "introduction") &&
             !textlist_has(&list, "Melia") &&
             !textlist_has(&list, "window_opens") &&
             !textlist_has(&list, "Generated runtime text.") &&
             !textlist_has(&list, "Generated patch payload.") &&
             !textlist_has(&list, "Should not appear.");
    }
    textlist_free(&list);
    cleanup_godot_fixture(root);
    return ok;
}

static int expect_godot_embedded_pck_scan(void) {
    WCHAR temp_dir[MAX_PATH];
    WCHAR root[MAX_PATH];
    if (!GetTempPathW(MAX_PATH, temp_dir)) return 0;
    if (!GetTempFileNameW(temp_dir, L"dse", 0, root)) return 0;
    DeleteFileW(root);
    if (!CreateDirectoryW(root, NULL)) return 0;

    WCHAR p[MAX_PATH * 4];
    path_join(p, MAX_PATH * 4, root, L"embedded.exe");
    int ok = write_probe_embedded_godot_pck(p);

    TextList list = {0};
    list.max_items = GODOT_WARMUP_MAX_ITEMS;
    if (ok) {
        warmup_scan_godot_resources(root, &list);
        ok = textlist_has(&list, "Embedded pack dialogue.") &&
             textlist_has(&list, "Start embedded journey.");
    }
    textlist_free(&list);
    cleanup_godot_fixture(root);
    return ok;
}

static int expect_godot_embedded_pck_v1_scan(void) {
    WCHAR temp_dir[MAX_PATH];
    WCHAR root[MAX_PATH];
    if (!GetTempPathW(MAX_PATH, temp_dir)) return 0;
    if (!GetTempFileNameW(temp_dir, L"dsv", 0, root)) return 0;
    DeleteFileW(root);
    if (!CreateDirectoryW(root, NULL)) return 0;

    WCHAR p[MAX_PATH * 4];
    path_join(p, MAX_PATH * 4, root, L"embedded_v1.exe");
    int ok = write_probe_embedded_godot_pck_v1(p);

    TextList list = {0};
    list.max_items = GODOT_WARMUP_MAX_ITEMS;
    if (ok) {
        warmup_scan_godot_resources(root, &list);
        ok = textlist_has(&list, "V1 embedded dialogue.");
    }
    textlist_free(&list);
    cleanup_godot_fixture(root);
    return ok;
}

static int expect_godot_valid_pck_does_not_whole_pack_binary_scan(void) {
    WCHAR temp_dir[MAX_PATH];
    WCHAR root[MAX_PATH];
    if (!GetTempPathW(MAX_PATH, temp_dir)) return 0;
    if (!GetTempFileNameW(temp_dir, L"dsp", 0, root)) return 0;
    DeleteFileW(root);
    if (!CreateDirectoryW(root, NULL)) return 0;

    WCHAR p[MAX_PATH * 4];
    path_join(p, MAX_PATH * 4, root, L"pack.pck");
    int ok = write_probe_godot_pck_with_unknown_binary(p);

    TextList list = {0};
    list.max_items = GODOT_WARMUP_MAX_ITEMS;
    if (ok) {
        warmup_scan_godot_resources(root, &list);
        ok = textlist_has(&list, "Packed scene dialogue.") &&
             !textlist_has(&list, "Hidden generic blob text.");
    }
    textlist_free(&list);
    cleanup_godot_fixture(root);
    return ok;
}

static int expect_godot_pck_v3_scan(void) {
    WCHAR temp_dir[MAX_PATH];
    WCHAR root[MAX_PATH];
    if (!GetTempPathW(MAX_PATH, temp_dir)) return 0;
    if (!GetTempFileNameW(temp_dir, L"ds3", 0, root)) return 0;
    DeleteFileW(root);
    if (!CreateDirectoryW(root, NULL)) return 0;

    WCHAR p[MAX_PATH * 4];
    path_join(p, MAX_PATH * 4, root, L"pack.pck");
    int ok = write_probe_godot_pck_v3(p);

    TextList list = {0};
    list.max_items = GODOT_WARMUP_MAX_ITEMS;
    if (ok) {
        ok = scan_godot_pck_file(p, &list) &&
             textlist_has(&list, "Format 3 menu text.");
    }
    textlist_free(&list);
    cleanup_godot_fixture(root);
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

static int expect_rpgm_message_block_scan(void) {
    WCHAR temp_dir[MAX_PATH];
    WCHAR source[MAX_PATH];
    if (!GetTempPathW(MAX_PATH, temp_dir)) return 0;
    if (!GetTempFileNameW(temp_dir, L"dsr", 0, source)) return 0;
    int ok = write_probe_file(source,
        "{\"events\":[{\"pages\":[{\"list\":["
        "{\"code\":101,\"parameters\":[\"\",0,1,2]},"
        "{\"code\":401,\"parameters\":[\"Announced as the new, surefire way of becoming rich and renowned,\"]},"
        "{\"code\":401,\"parameters\":[\"thousands of people take up Chichikawa on their offer:\"]},"
        "{\"code\":401,\"parameters\":[\"Take out a huge loan, and you get a farm, ready to start your\"]},"
        "{\"code\":401,\"parameters\":[\"very own Theriari business. \"]},"
        "{\"code\":102,\"parameters\":[[\"Male appearance\",\"Female appearance\"],-1,0,2,2]},"
        "{\"code\":101,\"parameters\":[\"\",0,0,0]},"
        "{\"code\":401,\"parameters\":[\"\\\\pop[14]\\\\n<Mio>Oh, hello there!\"]},"
        "{\"code\":101,\"parameters\":[\"\",0,0,0]},"
        "{\"code\":401,\"parameters\":[\"\\\\pop[14]\\\\n<Mio>You must be the new owner of\"]},"
        "{\"code\":401,\"parameters\":[\"this farm, right?\"]}"
        "]}]}],"
        "\"terms\":{"
        "\"commands\":[\"Item\",\"Weapon\",\"Catalyst\",\"Key Item\"],"
        "\"basic\":[\"Level\",\"HP\"],"
        "\"params\":[\"Max HP\",\"Attack\"],"
        "\"elements\":[\"Fire\"],"
        "\"equipTypes\":[\"Power Catalyst\"],"
        "\"weaponTypes\":[\"Sword & Shield\"],"
        "\"armorTypes\":[\"Diamond\"],"
        "\"skillTypes\":[\"Skill\"]"
        "},"
        "\"techtrees\":[{\"header\":\"Earth Cultivation\","
        "\"tech_description\":\"Focus on heavy tools, physical vigor, and harvesting speed.\"}]"
        "}");
    TextList list = {0};
    if (ok) {
        parse_rpgm_json_file(source, &list);
        ok = textlist_has(&list,
                 "Announced as the new, surefire way of becoming rich and renowned,\n"
                 "thousands of people take up Chichikawa on their offer:\n"
                 "Take out a huge loan, and you get a farm, ready to start your\n"
                 "very own Theriari business.") &&
             textlist_has(&list, "very own Theriari business.") &&
             textlist_has(&list, "Male appearance") &&
             textlist_has(&list, "Female appearance") &&
             textlist_has(&list, "Item") &&
             textlist_has(&list, "Catalyst") &&
             textlist_has(&list, "Max HP") &&
             textlist_has(&list, "Power Catalyst") &&
             textlist_has(&list, "Sword & Shield") &&
             textlist_has(&list, "Earth Cultivation") &&
             textlist_has(&list, "Focus on heavy tools, physical vigor, and harvesting speed.") &&
             textlist_has(&list, "Oh, hello there!") &&
             textlist_has(&list,
                 "You must be the new owner of\n"
                 "this farm, right?");
    }
    textlist_free(&list);
    DeleteFileW(source);
    return ok;
}

static int expect_unity_bundle_stream_scan(void) {
    WCHAR temp_dir[MAX_PATH];
    WCHAR bundle_path[MAX_PATH];
    WCHAR asset_path[MAX_PATH];
    if (!GetTempPathW(MAX_PATH, temp_dir) ||
        !GetTempFileNameW(temp_dir, L"dub", 0, bundle_path) ||
        !GetTempFileNameW(temp_dir, L"dua", 0, asset_path)) {
        return 0;
    }

    static const unsigned char bundle[] =
        "\x01" "[expression:nopic]Excuse me miss.\rD" "\x02"
        "\x01" "Hi honey.0" "\x02"
        "\x01" "- Go to Anna's store8" "\x02"
        "\x01" "- Get something to eat\\" "\x02"
        "\x01" "Funny, don't push it. Get going, you silly boy.\rD" "\x02"
        "\x01" "% P" "\x02"
        "\x01" "UnityEngine.Rendering.Universal Shader" "\x02";
    int ok = write_probe_bytes(bundle_path, bundle, (DWORD)(sizeof(bundle) - 1));

    TextList bundle_texts = {0};
    bundle_texts.max_items = UNITY_WARMUP_MAX_ITEMS;
    if (ok) {
        scan_unity_bundle_file(bundle_path, &bundle_texts);
        ok = unity_bundle_file_name(L"data.unity3d") &&
             unity_bundle_file_name(L"dialogue.bundle") &&
             unity_bundle_file_name(L"story.assetbundle") &&
             !unity_bundle_file_name(L"notes.txt") &&
             textlist_has(&bundle_texts, "Excuse me miss.") &&
              textlist_has(&bundle_texts, "Hi honey.") &&
              textlist_has(&bundle_texts, "- Go to Anna's store") &&
              textlist_has(&bundle_texts, "- Get something to eat") &&
              textlist_has(&bundle_texts, "Funny, don't push it. Get going, you silly boy.") &&
              !textlist_has(&bundle_texts, "Hi honey.0") &&
              !textlist_has(&bundle_texts, "- Go to Anna's store8") &&
              !textlist_has(&bundle_texts, "- Get something to eat\\") &&
             !textlist_has(&bundle_texts, "% P") &&
             !textlist_has(&bundle_texts, "UnityEngine.Rendering.Universal Shader");
    }
    textlist_free(&bundle_texts);

    static const char asset_text[] = "Small assets dialogue.";
    unsigned char asset[4 + sizeof(asset_text) - 1];
    put_u32le(asset, (uint32_t)(sizeof(asset_text) - 1));
    memcpy(asset + 4, asset_text, sizeof(asset_text) - 1);
    if (ok) ok = write_probe_bytes(asset_path, asset, (DWORD)sizeof asset);
    TextList asset_texts = {0};
    asset_texts.max_items = UNITY_WARMUP_MAX_ITEMS;
    if (ok) {
        scan_unity_asset_file(asset_path, &asset_texts);
        ok = textlist_has(&asset_texts, asset_text);
    }
    textlist_free(&asset_texts);

    DeleteFileW(bundle_path);
    DeleteFileW(asset_path);
    return ok;
}

static int expect_renpy_launcher_hook_excluded(void) {
    WCHAR temp_dir[MAX_PATH];
    WCHAR root[MAX_PATH];
    if (!GetTempPathW(MAX_PATH, temp_dir)) return 0;
    if (!GetTempFileNameW(temp_dir, L"dsr", 0, root)) return 0;
    DeleteFileW(root);
    if (!CreateDirectoryW(root, NULL)) return 0;

    WCHAR path[MAX_PATH * 4];
    path_join(path, MAX_PATH * 4, root, L"script.rpy");
    int ok = write_probe_file(path, "    e \"Real game dialogue.\"\n");
    path_join(path, MAX_PATH * 4, root, L"iron_deepseek.rpy");
    ok = ok && write_probe_file(path,
        "    _ds_http('/batch', {'texts': ['Translator implementation detail.']}, 12.0)\n");

    TextList list = {0};
    list.max_items = RENPY_WARMUP_MAX_ITEMS;
    if (ok) {
        scan_renpy_script_dir(root, &list, 0);
        ok = textlist_has(&list, "Real game dialogue.") &&
             !textlist_has(&list, "Translator implementation detail.");
    }
    textlist_free(&list);

    path_join(path, MAX_PATH * 4, root, L"script.rpy");
    DeleteFileW(path);
    path_join(path, MAX_PATH * 4, root, L"iron_deepseek.rpy");
    DeleteFileW(path);
    RemoveDirectoryW(root);
    return ok;
}

int wmain(int argc, WCHAR **argv) {
    if (argc != 2) return 32;
    if (!expect_extract("    e \"Hello, world.\"", "Hello, world.")) return 1;
    if (!expect_extract("    e 'Single quoted choice.'", "Single quoted choice.")) return 2;
    if (!expect_extract("    e \"Hello, \\\"friend\\\".\\nNext\"", "Hello, \"friend\".\nNext")) return 3;
    if (!expect_extract("    $ locals()[sceneactor](\"Are my arms to your satisfaction, [nickname]?\")",
                        "Are my arms to your satisfaction, [nickname]?")) return 26;

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
    TextList renpyMulti = {0};
    char renpyMultiLine[] = "    e \"First visible sentence.\" \"Second visible sentence.\"";
    collect_renpy_line_strings(renpyMultiLine, &renpyMulti);
    if (renpyMulti.n != 2 ||
        strcmp(renpyMulti.items[0], "First visible sentence.") ||
        strcmp(renpyMulti.items[1], "Second visible sentence.")) {
        textlist_free(&renpyMulti);
        return 27;
    }
    textlist_free(&renpyMulti);
    if (!expect_renpy_launcher_hook_excluded()) return 28;
    if (!should_warm_rpgm_text("\\C[20]Auntie Daisy\\C[0] <br>")) return 9;
    if (!should_warm_rpgm_text("My goodness, \\n[1]. <br>")) return 10;
    if (should_warm_rpgm_text("C:\\Users\\player\\save")) return 11;
    TextList quests = {0};
    char questTitle[] = "<quest 180:Danesia Questline #1 - The First Meeting|1|0>";
    char questBody[] = "Apparently there’s a blacksmith around here.";
    collect_rpgm_text_line(questTitle, &quests);
    collect_rpgm_text_line(questBody, &quests);
    if (quests.n != 3 ||
        strcmp(quests.items[0], "Danesia Questline #1 - The First Meeting") ||
        strcmp(quests.items[1], "The First Meeting") ||
        strcmp(quests.items[2], questBody)) {
        textlist_free(&quests);
        return 12;
    }
    textlist_free(&quests);
    if (!expect_rpgm_message_block_scan()) return 13;
    TextList flatRpgm = {0};
    flatRpgm.max_items = RPGM_WARMUP_MAX_ITEMS;
    warmup_scan_rpgm_resources(argv[1], &flatRpgm);
    if (!textlist_has(&flatRpgm, "Flat layout dialogue from Map001.") ||
        !textlist_has(&flatRpgm,
                      "Localized CSV dialogue must be prefetched before first display.")) {
        textlist_free(&flatRpgm);
        return 33;
    }
    textlist_free(&flatRpgm);
    if (!expect_backup_once()) return 25;
    if (!should_warm_godot_text("Start your journey.")) return 14;
    if (!should_warm_godot_text("Start")) return 18;
    if (should_warm_godot_text("res://scenes/main.tscn")) return 15;
    if (should_warm_godot_text("NodePath:Menu/Button")) return 16;
    TextList godot = {0};
    char godotBuf[] = "[node name=\"Label\" type=\"Label\"]\ntext = \"Start your journey.\"\ntext = \"Start\"\nres://icons/start.png\nA loose menu line.\n";
    scan_godot_quoted_strings(godotBuf, &godot);
    scan_godot_lines(godotBuf, &godot);
    if (godot.n != 3 ||
        strcmp(godot.items[0], "Start your journey.") ||
        strcmp(godot.items[1], "Start") ||
        strcmp(godot.items[2], "A loose menu line.")) {
        textlist_free(&godot);
        return 17;
    }
    textlist_free(&godot);
    TextList godotPo = {0};
    char godotPoBuf[] =
        "msgctxt \"menu\"\n"
        "msgid \"Start\"\n"
        "msgstr \"Commencer\"\n"
        "msgid \"Line \"\n"
        "\"Two\"\n"
        "msgstr \"Ligne Deux\"\n";
    scan_godot_po_strings(godotPoBuf, &godotPo);
    if (godotPo.n != 2 ||
        strcmp(godotPo.items[0], "Start") ||
        strcmp(godotPo.items[1], "Line Two")) {
        textlist_free(&godotPo);
        return 19;
    }
    textlist_free(&godotPo);
    TextList godotCsv = {0};
    char godotCsvBuf[] =
        "keys,en\n"
        "MENU_OPTIONS,Options\n"
        "\"Start your journey.\",\"Start your journey.\"\n";
    scan_godot_csv_strings(godotCsvBuf, &godotCsv);
    if (godotCsv.n != 2 ||
        strcmp(godotCsv.items[0], "Options") ||
        strcmp(godotCsv.items[1], "Start your journey.")) {
        textlist_free(&godotCsv);
        return 20;
    }
    textlist_free(&godotCsv);
    TextList godotCsvHeader = {0};
    char godotCsvHeaderBuf[] =
        "id,fr,en\n"
        "MENU_PLAY,Jouer,Play\n"
        "MENU_EXIT,Quitter,Exit\n";
    scan_godot_csv_strings(godotCsvHeaderBuf, &godotCsvHeader);
    if (!textlist_has(&godotCsvHeader, "Play") ||
        !textlist_has(&godotCsvHeader, "Exit") ||
        textlist_has(&godotCsvHeader, "Jouer") ||
        textlist_has(&godotCsvHeader, "Quitter")) {
        textlist_free(&godotCsvHeader);
        return 30;
    }
    textlist_free(&godotCsvHeader);
    TextList godotScript = {0};
    char godotScriptBuf[] = "var title = tr(\"Continue\")\nvar body = TranslationServer.translate(\"Resume your journey.\")\nconst ID = \"MenuRoot\"\n";
    scan_godot_quoted_strings(godotScriptBuf, &godotScript);
    if (godotScript.n != 2 ||
        strcmp(godotScript.items[0], "Continue") ||
        strcmp(godotScript.items[1], "Resume your journey.")) {
        textlist_free(&godotScript);
        return 21;
    }
    textlist_free(&godotScript);
    TextList godotBin = {0};
    const unsigned char godotBinBuf[] = "bin\0Caf\xc3\xa9 rendezvous.\0res://icons/start.png\0";
    scan_godot_binary_buffer(godotBinBuf, (DWORD)(sizeof(godotBinBuf) - 1), &godotBin);
    if (godotBin.n != 1 || strcmp(godotBin.items[0], "Caf\xc3\xa9 rendezvous.")) {
        textlist_free(&godotBin);
        return 22;
    }
    textlist_free(&godotBin);
    TextList godotRich = {0};
    const unsigned char godotRichBuf[] =
        "[center]Pornography was [color=eb3b61]banned[/color].[/center]\0";
    scan_godot_binary_buffer(godotRichBuf, (DWORD)(sizeof(godotRichBuf) - 1), &godotRich);
    if (!godot_pack_binary_path(".godot/exported/menu.scn") ||
        !textlist_has(&godotRich, "Pornography was") ||
        !textlist_has(&godotRich, "banned") ||
        textlist_has(&godotRich, "[center]Pornography was [color=eb3b61]banned[/color].[/center]")) {
        textlist_free(&godotRich);
        return 31;
    }
    textlist_free(&godotRich);
    if (!expect_godot_directory_scan()) return 23;
    if (!expect_godot_embedded_pck_scan()) return 24;
    if (!expect_godot_embedded_pck_v1_scan()) return 26;
    if (!expect_godot_valid_pck_does_not_whole_pack_binary_scan()) return 29;
    if (!expect_godot_pck_v3_scan()) return 30;
    if (!expect_unity_bundle_stream_scan()) return 28;
    puts("warmup probe passed");
    return 0;
}
