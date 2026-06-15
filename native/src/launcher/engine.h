#pragma once

#include "globals.h"

typedef enum {
    ENGINE_UNKNOWN,
    ENGINE_RENPY,
    ENGINE_RPGM_MV,
    ENGINE_UNITY,
    ENGINE_UNITY_IL2CPP,
    ENGINE_RPGM_LEGACY
} Engine;

int has_file_pattern(const WCHAR *dir, const WCHAR *pattern);
int find_subdir_suffix(const WCHAR *dir, const WCHAR *suffix);
int find_exe(const WCHAR *dir, WCHAR *out, size_t cap);
int unity_is_il2cpp(const WCHAR *dir);

Engine detect_engine(const WCHAR *dir);
const WCHAR *engine_name(Engine e);
