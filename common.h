#pragma once

#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif

#define NOMINMAX
#include <windows.h>
#include <winhttp.h>
#include <wincrypt.h>
#include <gdiplus.h>
#include <shlwapi.h>
#include <shlobj.h>
#include <string>
#include <vector>
#include <functional>
#include <sstream>
#include <iomanip>
#include <ctime>
#include <commctrl.h>
#include <mutex>
#include <map>
#include <algorithm>

#include "json.hpp"

// 自定义消息
#define WM_USER_REFRESH (WM_USER + 1)
#define WM_USER_TICK    (WM_USER + 2)

// 常量声明 (仅声明，定义在 common.cpp)
extern const std::wstring API_HOST;
extern const std::wstring SETTINGS_FILE;
extern const wchar_t *APP_NAME;

// --- 🚀 数据结构定义 ---

struct Todo {
    int id;
    std::wstring content;
    bool isDone;
    time_t lastUpdated;
    std::wstring createdDate;
    std::wstring dueDate;
};

struct Countdown {
    int id;
    std::wstring title;
    std::wstring dateStr;
    int daysLeft;
    time_t lastUpdated;
};

struct AppUsageRecord {
    std::wstring appName;
    std::wstring deviceName;
    int seconds;
};

struct HitZone {
    Gdiplus::Rect rect;
    int id;
    int type;
};

// --- 🚀 全局变量声明 (仅声明，不分配内存) ---

// 账户与同步状态
extern int g_SyncLimit;
extern int g_SyncCount;
extern int g_SyncInterval;
extern bool g_AutoLogin;
extern std::wstring g_UserTier;

// 用户信息
extern HINSTANCE g_hInst;
extern int g_UserId;
extern std::wstring g_Username;
extern std::wstring g_SavedEmail;
extern std::wstring g_SavedPass;
extern HWND g_hWidgetWnd;

// 界面与配置
extern std::recursive_mutex g_DataMutex;
extern float g_Scale;
extern BYTE g_BgAlpha;
extern int g_TopAppsCount;

// 系统状态
extern bool g_LoginSuccess;
extern std::wstring g_TaiDbPath;
extern std::wstring g_DeviceName;

// 数据集合
extern std::vector<Todo> g_Todos;
extern std::vector<Countdown> g_Countdowns;
extern std::vector<HitZone> g_HitZones;
extern std::vector<AppUsageRecord> g_AppUsage;

namespace InputState {
    extern std::wstring result1;
    extern std::wstring result2;
    extern std::wstring result3;
    extern bool isOk;
}

// 辅助宏：DPI 缩放
#define S(x) (int)((x) * g_Scale)