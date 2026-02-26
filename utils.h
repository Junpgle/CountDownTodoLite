#pragma once
#include "common.h"

// 删除 int S(int val); 声明，改用 common.h 里的宏 S(x)

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