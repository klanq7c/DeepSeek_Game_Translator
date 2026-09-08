#ifndef DST_GODOT_PROBE_H
#define DST_GODOT_PROBE_H

/* 仅当捕获的 Godot 输出明确报告不支持 --main-pack 命令行选项本身时返回非零。 */
int godot_output_explicitly_rejects_main_pack(const char *output);

#endif
