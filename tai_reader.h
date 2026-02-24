#pragma once
#include "common.h"

// 启动和停止 Tai 数据库读取线程
void StartTaiReader();
void StopTaiReader();

// 获取格式化后的前 N 个应用程序使用时间数据
std::vector<std::pair<std::wstring, int>> GetTopApps(int topN);