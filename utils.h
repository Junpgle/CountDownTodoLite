#pragma once
#include "common.h"

int S(int val);
std::string ToUtf8(const std::wstring &wstr);
std::wstring ToWide(const std::string &str);

time_t ParseSqlTime(const std::string &s);
int CalculateDaysLeft(const std::wstring &dateStr);

std::wstring EncryptString(const std::wstring &input);
std::wstring DecryptString(const std::wstring &hexInput);

void SetAutoStart(bool enable);
bool IsAutoStart();
void LoadSettings();
void SaveSettings(int uid, const std::wstring &name, const std::wstring &email, const std::wstring &pass, bool savePass);
void SaveAlphaSetting();
void SaveTaiDbPathSetting();

HFONT GetMiSansFont(int s);