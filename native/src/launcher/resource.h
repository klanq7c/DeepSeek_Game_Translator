#ifndef DST_LAUNCHER_RESOURCE_H
#define DST_LAUNCHER_RESOURCE_H

/* 此 ID 必须与 build_native.bat 生成的 ICON 项保持一致。 */
#define IDI_APP_ICON 1

/* 引擎运行时脚本（RCDATA）。源文件位于 payloads/ 下，由 build_native.bat 原样嵌入；
   ID 必须与 launcher_payloads.rc 及 scripts/verify_build_artifacts.ps1 保持一致。
   二进制 payload（服务端、Unity 插件等）的 ID 见 self_update.c。 */
#define IDR_SCRIPT_RENPY_HOOK        301   /* payloads/RenPy/iron_deepseek.rpy      -> game/iron_deepseek.rpy */
#define IDR_SCRIPT_RPGM_HOOK         302   /* payloads/RPGMaker/hook_rpgm_mv.js     -> <content>/js/hook_rpgm_mv.js */
#define IDR_SCRIPT_GODOT_RUNTIME_G3  303   /* payloads/Godot/dst_godot_runtime_g3.gd -> Godot 3 运行时侧车/补丁包 */
#define IDR_SCRIPT_GODOT_RUNTIME_G4  304   /* payloads/Godot/dst_godot_runtime_g4.gd -> Godot 4 运行时侧车/补丁包 */

#endif
