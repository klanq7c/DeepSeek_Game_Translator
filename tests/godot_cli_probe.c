#include "godot_probe.h"

#include <stdio.h>

static int expect(int actual, int expected, const char *label) {
    if (!!actual == !!expected) return 0;
    fprintf(stderr, "%s: expected %d, got %d\n", label, expected, actual);
    return 1;
}

int main(void) {
    int failed = 0;
    failed |= expect(
        godot_output_explicitly_rejects_main_pack(
            "ERROR: Unknown option \"--main-pack\".\n"),
        1, "explicit unknown option");
    failed |= expect(
        godot_output_explicitly_rejects_main_pack(
            "Unrecognized command line argument: --MAIN-PACK\n"),
        1, "case-insensitive explicit rejection");
    failed |= expect(
        godot_output_explicitly_rejects_main_pack(
            "ERROR: Audio driver initialization failed while launching --main-pack content.\n"),
        0, "unrelated startup failure");
    failed |= expect(
        godot_output_explicitly_rejects_main_pack(
            "SCRIPT ERROR: autoload singleton crashed during headless startup.\n"),
        0, "game startup failure without option");
    failed |= expect(godot_output_explicitly_rejects_main_pack(NULL),
                     0, "missing output is inconclusive");
    if (failed) return 1;
    puts("godot cli probe passed");
    return 0;
}
