#ifndef DST_GODOT_PROBE_H
#define DST_GODOT_PROBE_H

/* Returns non-zero only when captured Godot output explicitly reports that
   the --main-pack command-line option itself is unsupported. */
int godot_output_explicitly_rejects_main_pack(const char *output);

#endif
