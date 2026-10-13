/* 链接真实 buf.c/util.c，固定 JSON 输出字节及长 UTF-8 段的扩容上限。 */
#include "buf.h"
#include "util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifndef DST_TEST_ALLOC_COUNTERS
#error Build json_buffer_tests with DST_TEST_ALLOC_COUNTERS
#endif

static int passed;
static int failed;

static void check(int ok, const char *name) {
    if (ok) passed++;
    else {
        failed++;
        fprintf(stderr, "FAIL %s\n", name);
    }
}

static void check_bytes(const char *name, const Buf *b, const char *expected, size_t len) {
    int valid = b->data && b->len == len && b->len < b->cap;
    if (valid) valid = b->data[b->len] == 0 && memcmp(b->data, expected, len) == 0;
    check(valid, name);
    if (!valid) {
        size_t common = b->len < len ? b->len : len;
        size_t at = 0;
        while (b->data && at < common && b->data[at] == expected[at]) at++;
        fprintf(stderr, "  expected_len=%zu actual_len=%zu cap=%zu first_difference=%zu\n",
                len, b->len, b->cap, at);
    }
}

static void expect_json(const char *name, const char *input, const char *expected) {
    Buf b;
    buf_init(&b);
    buf_json(&b, input);
    check_bytes(name, &b, expected, strlen(expected));
    buf_free(&b);
}

static void test_all_non_nul_bytes(void) {
    /* 固定协议预期：当前短转义仅用于 TAB/LF/CR，BS/FF 仍用小写十六进制。 */
    static const char *const controls[] = {
        "\"\"", "\"\\u0001\"", "\"\\u0002\"", "\"\\u0003\"",
        "\"\\u0004\"", "\"\\u0005\"", "\"\\u0006\"", "\"\\u0007\"",
        "\"\\u0008\"", "\"\\t\"", "\"\\n\"", "\"\\u000b\"",
        "\"\\u000c\"", "\"\\r\"", "\"\\u000e\"", "\"\\u000f\"",
        "\"\\u0010\"", "\"\\u0011\"", "\"\\u0012\"", "\"\\u0013\"",
        "\"\\u0014\"", "\"\\u0015\"", "\"\\u0016\"", "\"\\u0017\"",
        "\"\\u0018\"", "\"\\u0019\"", "\"\\u001a\"", "\"\\u001b\"",
        "\"\\u001c\"", "\"\\u001d\"", "\"\\u001e\"", "\"\\u001f\""
    };
    for (unsigned int value = 1; value <= 255; value++) {
        char input[] = {(char)value, 0};
        char unchanged[] = {'"', (char)value, '"', 0};
        char name[40];
        const char *expected = unchanged;
        snprintf(name, sizeof name, "single byte 0x%02x", value);
        if (value < 32) expected = controls[value];
        else if (value == '"') expected = "\"\\\"\"";
        else if (value == '\\') expected = "\"\\\\\"";
        expect_json(name, input, expected);
    }
}

static void test_golden_strings(void) {
    const char nul_terminated[] = {'a', 0, 'b', 'c', 0};
    expect_json("NULL is empty JSON string", NULL, "\"\"");
    expect_json("empty JSON string", "", "\"\"");
    expect_json("first NUL terminates input", nul_terminated, "\"a\"");
    expect_json("slash remains literal", "https://example.test/a/b", "\"https://example.test/a/b\"");
    expect_json("adjacent escapes and ordinary spans", "ab\"\\\n\r\t\001cd\037ef\"tail",
                "\"ab\\\"\\\\\\n\\r\\t\\u0001cd\\u001fef\\\"tail\"");
    expect_json("leading and trailing control characters", "\037A\001", "\"\\u001fA\\u0001\"");
    expect_json("UTF-8 CJK emoji and combining mark", "\xe4\xb8\xad\xe6\x96\x87 \xf0\x9f\x8e\xae e\xcc\x81",
                "\"\xe4\xb8\xad\xe6\x96\x87 \xf0\x9f\x8e\xae e\xcc\x81\"");
    expect_json("UTF-8 spans around escapes", "\xe4\xb8\xad\"\xe6\x96\x87\n\xf0\x9f\x8e\xae",
                "\"\xe4\xb8\xad\\\"\xe6\x96\x87\\n\xf0\x9f\x8e\xae\"");
    expect_json("RenPy variables and text tags", "Say {color=#ff0000}[player_name]{/color}{w=0.2} \"hi\"",
                "\"Say {color=#ff0000}[player_name]{/color}{w=0.2} \\\"hi\\\"\"");
    expect_json("RPGMaker control sequences and markup", "\\C[2]Hello \\V[1]\\I[3]\\!<WordWrap><br>",
                "\"\\\\C[2]Hello \\\\V[1]\\\\I[3]\\\\!<WordWrap><br>\"");
    expect_json("Unity rich text and placeholders", "<color=#ffffff><b>Hello {0}</b></color><sprite name=\"icon\">",
                "\"<color=#ffffff><b>Hello {0}</b></color><sprite name=\\\"icon\\\">\"");
    expect_json("Godot BBCode and format placeholders", "[color=red][url=https://example.test/a]Hello[/url][/color] %s",
                "\"[color=red][url=https://example.test/a]Hello[/url][/color] %s\"");
}

static void test_append_and_capacity_boundaries(void) {
    static const size_t prefixes[] = {0, 1, 252, 253, 254, 255, 256, 510, 511, 512};
    static const char *const inputs[] = {"", "x", "\"", "\001", "\xe4\xb8\xad"};
    static const char *const encoded[] = {"\"\"", "\"x\"", "\"\\\"\"", "\"\\u0001\"", "\"\xe4\xb8\xad\""};
    char prefix[512];
    char expected[540];
    memset(prefix, 'P', sizeof prefix);
    for (size_t i = 0; i < sizeof prefixes / sizeof prefixes[0]; i++) {
        for (size_t j = 0; j < sizeof inputs / sizeof inputs[0]; j++) {
            Buf b;
            char name[80];
            size_t tail_len = strlen(encoded[j]);
            size_t prefix_len = prefixes[i];
            buf_init(&b);
            buf_addn(&b, prefix, prefix_len);
            buf_json(&b, inputs[j]);
            memcpy(expected, prefix, prefix_len);
            memcpy(expected + prefix_len, encoded[j], tail_len);
            memcpy(expected + prefix_len + tail_len, ":end", 5);
            buf_add(&b, ":end");
            snprintf(name, sizeof name, "append prefix=%zu fixture=%zu", prefix_len, j);
            check_bytes(name, &b, expected, prefix_len + tail_len + 4);
            buf_free(&b);
        }
    }

    Buf b;
    buf_init(&b);
    buf_add(&b, "[");
    buf_json(&b, "first");
    buf_add(&b, ",");
    buf_json(&b, "\"\\\n");
    buf_add(&b, ",");
    buf_json(&b, NULL);
    buf_add(&b, "]");
    const char *sequence = "[\"first\",\"\\\"\\\\\\n\",\"\"]";
    check_bytes("repeated JSON appends preserve framing", &b, sequence, strlen(sequence));
    buf_free(&b);
}

static char *make_long_utf8(size_t *len) {
    *len = 3 * 65536;
    char *input = xmalloc(*len + 1);
    for (size_t offset = 0; offset < *len; offset += 3) memcpy(input + offset, "\xe4\xb8\xad", 3);
    input[*len] = 0;
    return input;
}

static void test_long_utf8_allocation_bound(void) {
    size_t len;
    char *input = make_long_utf8(&len);
    Buf b;
    buf_init(&b);
    dst_test_alloc_reset();
    buf_json(&b, input);
    size_t allocations = dst_test_alloc_count();
    check(allocations == 1, "one growth for a long ordinary UTF-8 span");
    if (allocations != 1) fprintf(stderr, "  expected_allocations=1 actual_allocations=%zu\n", allocations);
    check(b.len == len + 2 && b.len < b.cap && b.data[0] == '"' &&
          memcmp(b.data + 1, input, len) == 0 && b.data[len + 1] == '"' && b.data[len + 2] == 0,
          "long UTF-8 text preserves every byte and JSON framing");
    buf_free(&b);
    free(input);
}

static int bench(void) {
    size_t len;
    const unsigned int iterations = 2000;
    char *input = make_long_utf8(&len);
    Buf b;
    buf_init(&b);
    /* 预先分配容量，让基准只衡量序列化；正确性与扩容次数另由测试约束。 */
    buf_grow(&b, len + 2);
    clock_t start = clock();
    for (unsigned int i = 0; i < iterations; i++) {
        b.len = 0;
        b.data[0] = 0;
        buf_json(&b, input);
    }
    double seconds = (double)(clock() - start) / CLOCKS_PER_SEC;
    check(b.len == len + 2 && b.data[0] == '"' && memcmp(b.data + 1, input, len) == 0 &&
          b.data[len + 1] == '"' && b.data[len + 2] == 0, "benchmark output");
    printf("json_buffer bench: input_bytes=%zu iterations=%u elapsed_ms=%.3f output_bytes=%zu\n",
           len, iterations, seconds * 1000.0, b.len);
    buf_free(&b);
    free(input);
    return failed ? 1 : 0;
}

int main(int argc, char **argv) {
    if (argc == 2 && strcmp(argv[1], "--bench") == 0) return bench();
    if (argc != 1) {
        fputs("usage: json_buffer_tests [--bench]\n", stderr);
        return 2;
    }
    test_all_non_nul_bytes();
    test_golden_strings();
    test_append_and_capacity_boundaries();
    test_long_utf8_allocation_bound();
    printf("json_buffer tests: %d passed, %d failed\n", passed, failed);
    return failed ? 1 : 0;
}
