/*
 * b64.h —— Base64 编解码声明。
 *
 * 缓存持久化文件（TSV）用 Base64 包裹键/值，确保任意文本（含换行、
 * 制表符、引号）都不会破坏行结构或字段分隔。编码/解码均为标准
 * RFC4648 字母表（含 '+'/'/'），'=' 作为填充。
 */
#pragma once

#include <stddef.h>

#include "buf.h"

void b64enc_append(Buf *out, const char *s); /* 直接追加到复用缓冲，避免逐项分配编码结果 */

char *b64dec(const char *s, size_t n);   /* 解码长度为 n 的输入，返回新分配的 NUL 结尾缓冲 */
