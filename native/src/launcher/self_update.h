#pragma once

/* 把启动器资源中嵌入的本项目自有组件同步到程序根目录。
   真实入口是 wWinMain；第三方运行时不在本模块的所有权范围内。 */
void sync_embedded_payloads(void);
