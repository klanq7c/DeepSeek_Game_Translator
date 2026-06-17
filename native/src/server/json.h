/*
 * json.h —— 极简 JSON 解析器声明。
 *
 * 只覆盖服务器实际需要的子集：跳空白、解析字符串、按键取值、取数组。
 * 不做完整对象/数字建模，因为本服务只消费 DeepSeek API 返回的固定结构
 * 和少量请求体字段。解析失败时返回 NULL 或空结果，绝不崩溃。
 */
#pragma once

#include <stddef.h>

/* 字符串指针的动态数组，用于承载 JSON 数组的字符串元素集合。 */
typedef struct {
    char **v;
    size_t n;
    size_t cap;
} List;

void list_push(List *l, char *s);   /* 追加一个字符串（接管所有权） */
void list_free(List *l);            /* 释放数组及每个元素 */

const char *json_skipws(const char *p);                 /* 跳过空白字符，返回首个非空白字符位置 */
char *json_str(const char **pp);                        /* 解析 pp 指向的 JSON 字符串，推进 *pp，返回新分配的解码结果 */
const char *json_key(const char *json, const char *key);/* 定位到 key 对应值的起始位置（不解码），未命中返回 NULL */
char *json_get_str(const char *json, const char *key);  /* 取 key 对应的字符串值并解码，未命中返回 NULL */
List json_array(const char *json, const char *key);     /* 取 key 对应数组里的所有字符串元素 */
