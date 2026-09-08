#pragma once

#include "globals.h"

/* 在所选游戏旁构建外部 Godot 资源补丁包。先复制原始 .pck 或内嵌包，再用本地
   翻译服务器修补支持的资源。格式 3 的包还可能加入启动器自有的运行时 autoload。
   原始游戏文件不会被修改。 */
int godot_prepare_patch_pack(const WCHAR *dir, WCHAR *out_pack, size_t cap);

/* 松散 Godot 工程（project.godot 加磁盘资源文件）不能用最小 --main-pack 启动，
   否则 Godot 枚举目录时会看到补丁包，而不是原始资源树。此类工程改用 sidecar
   运行时脚本。 */
int godot_is_loose_project(const WCHAR *dir);
int godot_prepare_runtime_sidecar(const WCHAR *dir);
int godot_patch_pack_has_runtime_sidecar(const WCHAR *pack_path);
int godot_patch_pack_has_runtime_autoload(const WCHAR *pack_path);
int godot_prepare_patch_launcher(const WCHAR *dir, WCHAR *out_exe, size_t cap);

/* 若上次运行留下后台构建的补丁包，则将其提升为活动包。此操作刻意与 prepare
   分离：游戏运行时可能一直打开活动包，因此刷新结果可暂存到下次启动。 */
int godot_promote_staged_patch_pack(const WCHAR *dir);
