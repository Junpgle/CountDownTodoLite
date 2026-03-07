#pragma once

#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif

#ifndef NOMINMAX
#define NOMINMAX
#endif

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

#include <gdiplus.h>

// 声明全局字体集合和 FontFamily 指针
extern Gdiplus::PrivateFontCollection g_FontCollection;
extern Gdiplus::FontFamily* g_MiSansFamily;

// 全局字体名：可选 L"MiSans" / L"Microsoft YaHei" / L"SimHei"
// 由设置界面写入，InitCustomFont / RebuildFont 读取
extern std::wstring g_FontName;

// 初始化函数的声明
void InitCustomFont();
void CleanupCustomFont();
// 重建字体（切换字体后调用，无需重启）
void RebuildFont();

// --- 🚀 数据结构定义 ---

struct Todo {
    int id            = 0;
    std::wstring uuid;
    std::wstring content;
    bool isDone       = false;
    bool isDeleted    = false;   // 真正的软删除标记（不再用 id<0 混用）
    time_t lastUpdated = 0;
    std::wstring createdDate;
    std::wstring dueDate;
    bool isDirty      = false;   // 本地有修改尚未上传时为 true

    // 循环待办字段（对齐手机端）
    int  recurrence         = 0;  // 0=none,1=daily,2=customDays
    int  customIntervalDays = 0;
    std::wstring recurrenceEndDate;
};

struct Countdown {
    int id;
    std::wstring uuid;       // 🚀 新增：后端 UUID
    std::wstring title;
    std::wstring dateStr;
    int daysLeft;
    time_t lastUpdated;
    bool isDirty = false;    // 🚀 新增：本地有修改尚未上传时为 true
};

struct Course {
    int id;
    std::wstring courseName;
    std::wstring roomName;
    std::wstring teacherName;
    int startTime;
    int endTime;
    int weekday;
    int weekIndex;
    std::wstring lessonType;
    std::wstring date;
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
    std::wstring uuid; // 🚀 新增：用于新架构下精准匹配 Todo/Countdown
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

// 🚀 新增：鉴权 Token（登录后由服务器返回，存入内存，不持久化）
extern std::wstring g_AuthToken;

// 🚀 新增：本机设备唯一标识（首次运行时生成并持久化到 INI）
extern std::wstring g_DeviceId;

// 🚀 新增：上次成功同步的服务器时间戳（UTC ms），用于 Delta Sync
extern long long g_LastSyncTime;

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
extern std::vector<Course> g_Courses;

namespace InputState {
    extern std::wstring result1;
    extern std::wstring result2;
    extern std::wstring result3;
    extern bool isOk;
}

// 辅助宏：DPI 缩放
#define S(x) (int)((x) * g_Scale)