#pragma once

#include "globals.h"

int deploy_renpy(const WCHAR *dir);
int deploy_rpgm(const WCHAR *dir);
int deploy_unity(const WCHAR *dir);
int deploy_unity_il2cpp(const WCHAR *dir);
int find_unity_template(WCHAR *out, size_t cap);
