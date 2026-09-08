#pragma once

/* ================================================================
 * api_config.h — API 配置对话框声明
 * ----------------------------------------------------------------
 * 提供一个模态对话框，用于设置/修改 DeepSeek API 地址、模型名称
 * 和 API Key。配置持久化到 INI 文件。
 * ================================================================ */

#include "globals.h"

/* 显示 API 配置模态对话框（阻塞调用，直到用户保存或取消） */
void show_api_config(void);

/* 与窗口无关的数据层，供对话框与诊断子命令共用（C# 侧对应 ApiConfig）。
   提供商预设表：显示名 / endpoint / 示例模型；endpoint 为 NULL 表示"自定义"。 */
typedef struct {
    const WCHAR *name;
    const WCHAR *endpoint;
    const WCHAR *model;
} ApiProviderPreset;

int api_config_preset_count(void);
const ApiProviderPreset *api_config_preset(int index);

/* 读取 config\api.ini 的三个键（缺失时用默认值），缓冲区大小固定为
   endpoint/key 1024、model 256 个 WCHAR。 */
void api_config_load(WCHAR *endpoint, WCHAR *model, WCHAR *key);

/* 按 endpoint 反查预设下标；无匹配返回 0（"自定义"）。 */
int api_config_preset_index(const WCHAR *endpoint);

/* 写回三个键；任一失败返回 0 并记录日志（调用方负责提示用户）。 */
int api_config_save(const WCHAR *endpoint, const WCHAR *model, const WCHAR *key);
