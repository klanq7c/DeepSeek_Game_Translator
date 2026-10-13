#ifndef DS_GODOT_ENCRYPTED_PACK_H
#define DS_GODOT_ENCRYPTED_PACK_H
#include <windows.h>
/* 仅归一启动器自有副本的已校验 v3 加密目录；游戏源文件只读。 */
int godot_normalize_encrypted_directory(const WCHAR *copy, const WCHAR *game_exe);
#endif
