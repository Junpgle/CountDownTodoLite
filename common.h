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

// 请确保已下载 json.hpp 并放在同级目录
#include "json.hpp"

// --- 常量定义 ---
#define WM_USER_REFRESH (WM_USER + 1)
#define WM_USER_TICK    (WM_USER + 2) // 用于界面定时刷新

extern const std::wstring API_HOST;
extern const std::wstring SETTINGS_FILE;
extern const wchar_t *APP_NAME;

// --- 数据结构 ---
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

struct HitZone {
    Gdiplus::Rect rect;
    int id;
    int type; // 1=AddTodo, 2=AddCountdown, 3=TodoItem, 4=CountdownItem
};

namespace InputState {
    extern std::wstring result1;
    extern std::wstring result2;
    extern bool isOk;
    extern int currentType;
}

// --- 全局变量声明 ---
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

// 本机设备名称
extern std::wstring g_DeviceName;

extern std::vector<Todo> g_Todos;
extern std::vector<Countdown> g_Countdowns;
extern std::vector<HitZone> g_HitZones;

// 屏幕使用时间记录 (应用名称 -> 使用秒数)
extern std::map<std::wstring, int> g_AppUsage;

// Tai 数据库自定义路径
extern std::wstring g_TaiDbPath;

// 输入状态