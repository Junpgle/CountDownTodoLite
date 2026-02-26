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

#define WM_USER_REFRESH (WM_USER + 1)
#define WM_USER_TICK    (WM_USER + 2)

extern const std::wstring API_HOST;
extern const std::wstring SETTINGS_FILE;
extern const wchar_t *APP_NAME;

struct Todo {
    int id;
    std::wstring content;
    bool isDone;
    time_t lastUpdated;
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

namespace InputState {
    extern std::wstring result1;
    extern std::wstring result2;
    extern bool isOk;
    extern int currentType;
}

extern HINSTANCE g_hInst;
extern int g_UserId;
extern std::wstring g_Username;
extern std::wstring g_SavedEmail;
extern std::wstring g_SavedPass;
extern HWND g_hWidgetWnd;

extern std::recursive_mutex g_DataMutex;
extern float g_Scale;
extern BYTE g_BgAlpha;
extern int g_TopAppsCount;

extern HWND g_hEmail;
extern HWND g_hPass;
extern HWND g_hAutoLogin;
extern bool g_LoginSuccess;

extern std::wstring g_DeviceName;
extern std::wstring g_TaiDbPath;

extern std::vector<Todo> g_Todos;
extern std::vector<Countdown> g_Countdowns;
extern std::vector<HitZone> g_HitZones;

// 统一为 vector 结构
extern std::vector<AppUsageRecord> g_AppUsage;

// 宏定义：处理 DPI 缩放
#define S(x) (int)((x) * g_Scale)