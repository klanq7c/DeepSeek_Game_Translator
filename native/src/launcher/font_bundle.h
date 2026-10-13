/* 经许可证核对的内嵌 CJK 字体：只负责在启动器目录释放并验证原始字节。 */
#pragma once
#include <windows.h>
#include <stddef.h>

int font_bundle_source(WCHAR *font, size_t font_cap, WCHAR *license, size_t license_cap);
int font_bundle_game_license(const WCHAR *game_dir);
#define FONT_BUNDLE_GODOT_LICENSE_OWNER "DST_FONT_OWNED_V1:NotoSansCJKsc-Regular.otf:Godot-OFL\n"
