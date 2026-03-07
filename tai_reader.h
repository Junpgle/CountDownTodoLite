#pragma once
#include "common.h"

// 启动和停止 Tai 数据库读取线程
void StartTaiReader();
void StopTaiReader();

// 获取格式化后的前 N 个应用程序使用时间数据
std::vector<std::pair<std::wstring, int>> GetTopApps(int topN);

// 获取今日屏幕总使用时长（秒）
int GetTotalScreenTime();

// 把最新本机数据合并到 g_AppUsage（仅未登录时使用）
void UpdateMergedUsage();

// 获取本机 Tai 实时应用用量快照（线程安全副本），供 SyncData() 上传使用
std::map<std::wstring, int> GetLocalAppUsageMapCopy();

