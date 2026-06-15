#pragma once

#include <stddef.h>

typedef struct {
    char **v;
    size_t n;
    size_t cap;
} List;

void list_push(List *l, char *s);
void list_free(List *l);

const char *json_skipws(const char *p);
char *json_str(const char **pp);
const char *json_key(const char *json, const char *key);
char *json_get_str(const char *json, const char *key);
List json_array(const char *json, const char *key);
