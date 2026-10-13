/* 引用真实请求构造器，验证官方模型与自定义端点的参数隔离，不发网络请求。 */
#include "../../native/src/server/api.c"
int main(int argc, char **argv) {
    if (argc != 2) return 2;
    ApiConfig cfg;
    api_config_init(&cfg);
    printf("%s\n", cfg.model);
    if (!api_config_load(&cfg, argv[1])) return 3;
    char *texts[] = { "Hello {name}.", "\\C[1]Open menu." };
    const char *prevs[] = { "Previous line.", "Hello {name}." };
    for (int mode = 0; mode < 3; mode++) {
        Buf body; buf_init(&body);
        if (mode == 0) build_request(&cfg, texts[0], &body);
        else if (mode == 1) build_batch_request(&cfg, texts, 2, &body);
        else build_ctx_request(&cfg, texts, prevs, 2, &body);
        puts(body.data); buf_free(&body);
    }
    return 0;
}
