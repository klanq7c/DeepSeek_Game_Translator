#include "godot_probe.h"

#include <ctype.h>
#include <stddef.h>
#include <string.h>

static int ascii_contains_case_insensitive(const char *text, const char *needle) {
    if (!text || !needle || !*needle) return 0;
    size_t needle_len = strlen(needle);
    for (const unsigned char *p = (const unsigned char *)text; *p; p++) {
        size_t i = 0;
        while (i < needle_len && p[i] &&
               tolower(p[i]) == tolower((unsigned char)needle[i])) {
            i++;
        }
        if (i == needle_len) return 1;
    }
    return 0;
}

int godot_output_explicitly_rejects_main_pack(const char *output) {
    if (!output || !ascii_contains_case_insensitive(output, "main-pack")) return 0;
    static const char *markers[] = {
        "unknown option",
        "unknown command line",
        "unrecognized option",
        "unrecognized command line",
        "invalid option",
        "invalid command line",
        "unsupported option",
        "not supported",
        "not available"
    };
    for (size_t i = 0; i < sizeof(markers) / sizeof(markers[0]); i++) {
        if (ascii_contains_case_insensitive(output, markers[i])) return 1;
    }
    return 0;
}
