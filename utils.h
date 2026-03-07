#pragma once
#include "common.h"

std::string ToUtf8(const std::wstring &w);
std::wstring ToWide(const std::string &s);
std::wstring EncryptString(const std::wstring &instr);
std::wstring DecryptString(const std::wstring &instr);
int CalculateDaysLeft(const std::wstring &dateStr);
void LoadSettings();
void SaveSettings(int uid, const std::wstring &name, const std::wstring &email, const std::wstring &pass, bool savePass);
void SaveAlphaSetting();
void SaveTaiDbPathSetting();
HFONT GetMiSansFont(int s);
std::wstring GetTodayDate();

// 🚀 新增：DeviceId 管理（首次运行时生成 UUID，之后从 INI 持久化读取）
std::wstring EnsureDeviceId();

// 🚀 新增：LastSyncTime 持久化读写
long long    LoadLastSyncTime();
void         SaveLastSyncTime(long long tsMs);

// 🚀 新增：时间戳 <-> 日期字符串 互转工具
// 将 "YYYY-MM-DD HH:MM" 或 "YYYY-MM-DD" 格式的本地时间字符串转为 UTC 毫秒时间戳
// 若字符串为空返回 0
long long    DateStringToUtcMs(const std::wstring &dateStr);

// 将 UTC 毫秒时间戳转为本地时间的 "YYYY-MM-DD HH:MM" 字符串
// 若 tsMs <= 0 返回空字符串
std::wstring UtcMsToDateString(long long tsMs);

// 将 UTC 毫秒时间戳转为纯日期字符串 "YYYY-MM-DD"（仅 倒计时 targetDate 使用）
std::wstring UtcMsToDateOnly(long long tsMs);

