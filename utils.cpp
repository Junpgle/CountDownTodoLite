#include "utils.h"
#include "common.h"
#include "resource.h"

// 补充标准库和 Windows API 依赖头文件，防止编译报错
#include <windows.h>
#include <shlwapi.h>
#include <dpapi.h>
#include <gdiplus.h>
#include <string>
#include <vector>
#include <ctime>
#include <cmath>
#include <sstream>
#include <iomanip>

// 自动链接必要的 Windows 静态库
#pragma comment(lib, "Crypt32.lib")
#pragma comment(lib, "Shlwapi.lib")

std::string ToUtf8(const std::wstring &w) {
    if (w.empty()) return "";
    int size = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, NULL, 0, NULL, NULL);
    std::string s(size - 1, 0);
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, &s[0], size, NULL, NULL);
    return s;
}

std::wstring ToWide(const std::string &str) {
    if (str.empty()) return std::wstring();
    int size_needed = MultiByteToWideChar(CP_UTF8, 0, &str[0], (int) str.size(), NULL, 0);
    std::wstring wstrTo(size_needed, 0);
    MultiByteToWideChar(CP_UTF8, 0, &str[0], (int) str.size(), &wstrTo[0], size_needed);
    return wstrTo;
}

time_t ParseSqlTime(const std::string &s) {
    if (s.empty()) return 0;
    int y, m, d, H, M, S_time;
    if (sscanf(s.c_str(), "%d-%d-%d %d:%d:%d", &y, &m, &d, &H, &M, &S_time) != 6) {
        if (sscanf(s.c_str(), "%d-%d-%dT%d:%d:%d", &y, &m, &d, &H, &M, &S_time) != 6) {
            return std::time(nullptr);
        }
    }
    std::tm tm = {0};
    tm.tm_year = y - 1900;
    tm.tm_mon = m - 1;
    tm.tm_mday = d;
    tm.tm_hour = H;
    tm.tm_min = M;
    tm.tm_sec = S_time;
    tm.tm_isdst = 0;
    return _mkgmtime(&tm);
}

int CalculateDaysLeft(const std::wstring &dateStr) {
    std::string ds = ToUtf8(dateStr);
    if (ds.length() < 10) return 0;
    int y, m, d;
    if (sscanf(ds.c_str(), "%d-%d-%d", &y, &m, &d) != 3) return 0;
    std::tm target = {0};
    target.tm_year = y - 1900;
    target.tm_mon = m - 1;
    target.tm_mday = d;
    target.tm_isdst = -1;
    std::time_t targetTime = std::mktime(&target);
    std::time_t nowTime = std::time(nullptr);
    if (targetTime == -1) return 0;
    double seconds = std::difftime(targetTime, nowTime);
    return (int) ceil(seconds / (60 * 60 * 24));
}

std::wstring EncryptString(const std::wstring &input) {
    if (input.empty()) return L"";
    DATA_BLOB DataIn = {(DWORD) ((input.length() + 1) * sizeof(wchar_t)), (BYTE *) input.c_str()};
    DATA_BLOB DataOut;
    if (CryptProtectData(&DataIn, L"MathQuizPwd", NULL, NULL, NULL, 0, &DataOut)) {
        std::wstringstream ss;
        for (DWORD i = 0; i < DataOut.cbData; ++i) {
            ss << std::hex << std::setw(2) << std::setfill(L'0') << (int) DataOut.pbData[i];
        }
        LocalFree(DataOut.pbData);
        return ss.str();
    }
    return L"";
}

std::wstring DecryptString(const std::wstring &hexInput) {
    if (hexInput.empty()) return L"";
    std::vector<BYTE> binary;
    for (size_t i = 0; i < hexInput.length(); i += 2) {
        binary.push_back((BYTE) wcstol(hexInput.substr(i, 2).c_str(), NULL, 16));
    }
    DATA_BLOB DataIn = {(DWORD) binary.size(), binary.data()};
    DATA_BLOB DataOut;
    if (CryptUnprotectData(&DataIn, NULL, NULL, NULL, NULL, 0, &DataOut)) {
        std::wstring result = (wchar_t *) DataOut.pbData;
        LocalFree(DataOut.pbData);
        return result;
    }
    return L"";
}

void SetAutoStart(bool enable) {
    HKEY hKey;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run", 0, KEY_SET_VALUE, &hKey) == ERROR_SUCCESS) {
        if (enable) {
            WCHAR path[MAX_PATH];
            GetModuleFileNameW(NULL, path, MAX_PATH);
            RegSetValueExW(hKey, APP_NAME, 0, REG_SZ, (BYTE *) path, (lstrlenW(path) + 1) * sizeof(WCHAR));
        } else {
            RegDeleteValueW(hKey, APP_NAME);
        }
        RegCloseKey(hKey);
    }
}

bool IsAutoStart() {
    HKEY hKey;
    bool exists = false;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run", 0, KEY_QUERY_VALUE, &hKey) == ERROR_SUCCESS) {
        if (RegQueryValueExW(hKey, APP_NAME, NULL, NULL, NULL, NULL) == ERROR_SUCCESS) exists = true;
        RegCloseKey(hKey);
    }
    return exists;
}

void LoadSettings() {
    WCHAR path[MAX_PATH];
    GetModuleFileNameW(NULL, path, MAX_PATH);
    PathRemoveFileSpecW(path);
    PathAppendW(path, SETTINGS_FILE.c_str());

    g_UserId = GetPrivateProfileIntW(L"Auth", L"UserId", 0, path);
    g_BgAlpha = GetPrivateProfileIntW(L"Settings", L"BgAlpha", 100, path);

    // 🚀 加载自动同步频率
    g_SyncInterval = GetPrivateProfileIntW(L"Auth", L"SyncInterval", 5, path);

    WCHAR buf[256];
    GetPrivateProfileStringW(L"Auth", L"Username", L"", buf, 256, path);
    g_Username = buf;

    GetPrivateProfileStringW(L"Auth", L"Email", L"", buf, 256, path);
    g_SavedEmail = buf;

    WCHAR pass[1024];
    GetPrivateProfileStringW(L"Auth", L"Pass", L"", pass, 1024, path);
    g_SavedPass = DecryptString(pass);

    WCHAR taiPath[MAX_PATH] = { 0 };
    GetPrivateProfileStringW(L"Settings", L"TaiDbPath", L"", taiPath, MAX_PATH, path);
    g_TaiDbPath = taiPath;

    g_TopAppsCount = GetPrivateProfileIntW(L"Settings", L"TopAppsCount", 3, path);

    // 字体名（MiSans / Microsoft YaHei / SimHei）
    WCHAR fontNameBuf[128] = {0};
    GetPrivateProfileStringW(L"Settings", L"FontName", L"MiSans", fontNameBuf, 128, path);
    g_FontName = fontNameBuf;

    // 获取本机计算机名称作为设备名
    WCHAR compName[MAX_COMPUTERNAME_LENGTH + 1];
    DWORD compNameLen = sizeof(compName) / sizeof(WCHAR);
    if (GetComputerNameW(compName, &compNameLen)) {
        g_DeviceName = compName;
    } else {
        g_DeviceName = L"UnknownDevice";
    }

    // 🚀 新增：加载/生成 DeviceId
    g_DeviceId = EnsureDeviceId();

    // 🚀 新增：加载上次同步时间戳
    g_LastSyncTime = LoadLastSyncTime();
}

void SaveSettings(int uid, const std::wstring &name, const std::wstring &email, const std::wstring &pass, bool savePass) {
    WCHAR path[MAX_PATH];
    GetModuleFileNameW(NULL, path, MAX_PATH);
    PathRemoveFileSpecW(path);
    PathAppendW(path, SETTINGS_FILE.c_str());

    WritePrivateProfileStringW(L"Auth", L"UserId", std::to_wstring(uid).c_str(), path);
    WritePrivateProfileStringW(L"Auth", L"Username", name.c_str(), path);
    WritePrivateProfileStringW(L"Auth", L"Email", email.c_str(), path);

    if (savePass) {
        WritePrivateProfileStringW(L"Auth", L"Pass", EncryptString(pass).c_str(), path);
    } else {
        WritePrivateProfileStringW(L"Auth", L"Pass", NULL, path);
    }

    // 🚀 同时同步保存同步频率
    WritePrivateProfileStringW(L"Auth", L"SyncInterval", std::to_wstring(g_SyncInterval).c_str(), path);
}

void SaveAlphaSetting() {
    WCHAR path[MAX_PATH];
    GetModuleFileNameW(NULL, path, MAX_PATH);
    PathRemoveFileSpecW(path);
    PathAppendW(path, SETTINGS_FILE.c_str());

    WritePrivateProfileStringW(L"Settings", L"BgAlpha", std::to_wstring(g_BgAlpha).c_str(), path);
    WritePrivateProfileStringW(L"Settings", L"TopAppsCount", std::to_wstring(g_TopAppsCount).c_str(), path);
    WritePrivateProfileStringW(L"Settings", L"FontName", g_FontName.c_str(), path);
}

void SaveTaiDbPathSetting() {
    WCHAR path[MAX_PATH];
    GetModuleFileNameW(NULL, path, MAX_PATH);
    PathRemoveFileSpecW(path);
    PathAppendW(path, SETTINGS_FILE.c_str());

    WritePrivateProfileStringW(L"Settings", L"TaiDbPath", g_TaiDbPath.c_str(), path);
}

HFONT GetMiSansFont(int s) {
    static std::wstring fontPath;
    static bool fontRegistered = false;
    static bool fontAvailable  = false;
    if (!fontRegistered) {
        WCHAR exePath[MAX_PATH];
        GetModuleFileNameW(NULL, exePath, MAX_PATH);
        PathRemoveFileSpecW(exePath);
        PathAppendW(exePath, L"MiSans-Regular.ttf");
        fontPath = exePath;
        DWORD cnt = 0;
        fontAvailable  = (AddFontResourceExW(fontPath.c_str(), FR_PRIVATE, nullptr) != 0);
        fontRegistered = true;
    }
    const wchar_t* faceName = fontAvailable ? L"MiSans" : L"SimHei";
    return CreateFontW(S(s), 0, 0, 0, FW_NORMAL, 0, 0, 0,
                       DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY,
                       DEFAULT_PITCH | FF_SWISS, faceName);
}


// 新增获取今日日期的工具函数
std::wstring GetTodayDate() {
    time_t now = time(nullptr);
    struct tm t;
    localtime_s(&t, &now);
    wchar_t buf[20];
    swprintf_s(buf, L"%04d-%02d-%02d", t.tm_year + 1900, t.tm_mon + 1, t.tm_mday);
    return buf;
}

// ============================================================
// 🚀 新增：DeviceId 管理
// ============================================================

// 生成一个简单的伪 UUID（基于机器名 + 随机数，格式 xxxx-xxxx-xxxx-xxxx）
static std::wstring GenerateDeviceId() {
    WCHAR compName[MAX_COMPUTERNAME_LENGTH + 1] = {0};
    DWORD len = MAX_COMPUTERNAME_LENGTH + 1;
    GetComputerNameW(compName, &len);

    // 混合机器名 hash + 随机数
    unsigned long long h = 5381;
    for (WCHAR *p = compName; *p; ++p) h = ((h << 5) + h) ^ (unsigned long long)(*p);

    srand((unsigned int)(h ^ (unsigned int)time(nullptr)));
    wchar_t buf[40];
    swprintf_s(buf, L"%04x%04x-%04x-%04x-%04x-%04x%04x%04x",
        (unsigned)(h & 0xFFFF), (unsigned)rand() & 0xFFFF,
        (unsigned)rand() & 0xFFFF, (unsigned)rand() & 0xFFFF,
        (unsigned)rand() & 0xFFFF,
        (unsigned)rand() & 0xFFFF, (unsigned)rand() & 0xFFFF, (unsigned)rand() & 0xFFFF);
    return buf;
}

std::wstring EnsureDeviceId() {
    WCHAR path[MAX_PATH];
    GetModuleFileNameW(NULL, path, MAX_PATH);
    PathRemoveFileSpecW(path);
    PathAppendW(path, SETTINGS_FILE.c_str());

    WCHAR buf[64] = {0};
    GetPrivateProfileStringW(L"Device", L"DeviceId", L"", buf, 64, path);
    if (buf[0] != L'\0') return buf;

    // 首次运行：生成并持久化
    std::wstring newId = GenerateDeviceId();
    WritePrivateProfileStringW(L"Device", L"DeviceId", newId.c_str(), path);
    return newId;
}

// ============================================================
// 🚀 新增：LastSyncTime 持久化
// ============================================================

long long LoadLastSyncTime() {
    WCHAR path[MAX_PATH];
    GetModuleFileNameW(NULL, path, MAX_PATH);
    PathRemoveFileSpecW(path);
    PathAppendW(path, SETTINGS_FILE.c_str());

    WCHAR buf[32] = {0};
    GetPrivateProfileStringW(L"Sync", L"LastSyncTime", L"0", buf, 32, path);
    return wtoll(buf);
}

void SaveLastSyncTime(long long tsMs) {
    WCHAR path[MAX_PATH];
    GetModuleFileNameW(NULL, path, MAX_PATH);
    PathRemoveFileSpecW(path);
    PathAppendW(path, SETTINGS_FILE.c_str());

    WritePrivateProfileStringW(L"Sync", L"LastSyncTime", std::to_wstring(tsMs).c_str(), path);
}

// ============================================================
// 🚀 新增：时间戳 <-> 日期字符串 互转
// ============================================================

long long DateStringToUtcMs(const std::wstring &dateStr) {
    if (dateStr.empty()) return 0;
    int y = 0, mo = 0, d = 0, h = 0, mi = 0;
    int cnt = swscanf(dateStr.c_str(), L"%d-%d-%d %d:%d", &y, &mo, &d, &h, &mi);
    if (cnt < 3) return 0;

    struct tm t = {0};
    t.tm_year = y - 1900;
    t.tm_mon  = mo - 1;
    t.tm_mday = d;
    t.tm_hour = h;
    t.tm_min  = mi;
    t.tm_sec  = 0;
    t.tm_isdst = -1;
    // mktime 返回本地时间的 time_t，转 UTC ms
    time_t local = mktime(&t);
    if (local == -1) return 0;
    return (long long)local * 1000LL;
}

std::wstring UtcMsToDateString(long long tsMs) {
    if (tsMs <= 0) return L"";
    time_t t = (time_t)(tsMs / 1000LL);
    struct tm lt;
    localtime_s(&lt, &t);
    wchar_t buf[32];
    swprintf_s(buf, L"%04d-%02d-%02d %02d:%02d",
        lt.tm_year + 1900, lt.tm_mon + 1, lt.tm_mday,
        lt.tm_hour, lt.tm_min);
    return buf;
}

std::wstring UtcMsToDateOnly(long long tsMs) {
    if (tsMs <= 0) return L"";
    time_t t = (time_t)(tsMs / 1000LL);
    struct tm lt;
    localtime_s(&lt, &t);
    wchar_t buf[16];
    swprintf_s(buf, L"%04d-%02d-%02d", lt.tm_year + 1900, lt.tm_mon + 1, lt.tm_mday);
    return buf;
}


