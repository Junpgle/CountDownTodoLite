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
extern const std::wstring WS_HOST;   // 阿里云 WebSocket 服务器 IP
extern const WORD         WS_PORT;   // WebSocket 端口（8081）
extern const std::wstring SETTINGS_FILE;
extern const wchar_t *APP_NAME;


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

    // 备注字段（对齐后端 remark 列）
    std::wstring remark;
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

// ---  番茄钟数据结构 ---

struct PomodoroTag {
    std::wstring uuid;
    std::wstring name;
    std::wstring color;   // hex 颜色字符串，如 "#4F46E5"
    bool isDeleted = false;
    int  version   = 1;
    long long createdAt = 0; // UTC ms
    long long updatedAt = 0; // UTC ms
    bool isDirty  = false;
};

enum class PomodoroStatus {
    Idle,        // 未开始
    Focusing,    // 专注中
    Resting,     // 休息中
    Paused,      // 已暂停（扩展用）
};

struct PomodoroSession {
    // 持久化字段（INI）
    PomodoroStatus status    = PomodoroStatus::Idle;
    long long      targetEndMs = 0;  // 目标结束时间（UTC ms）；基于绝对时间戳
    int            focusDuration  = 25 * 60; // 秒
    int            restDuration   = 5  * 60; // 秒
    int            loopCount      = 4;        // 总循环次数
    int            currentLoop    = 0;        // 当前完成的循环数
    std::wstring   boundTodoUuid;   // 当前绑定的待办 UUID
    std::wstring   currentRecordUuid; // 当前进行中的专注记录 UUID
    bool           isRestPhase = false; // true = 正在休息
};

struct PomodoroRecord {
    std::wstring uuid;
    std::wstring todoUuid;
    std::wstring todoTitle;   // 🚀 后端 JOIN todos.content 返回，仅用于显示
    long long    startTime       = 0;
    long long    endTime         = 0;    // 0 = 未结束
    int          plannedDuration = 1500;
    int          actualDuration  = 0;
    std::wstring status;   // "completed" / "interrupted" / "switched"
    std::wstring deviceId;
    bool         isDeleted = false;
    int          version   = 1;
    long long    createdAt = 0;
    long long    updatedAt = 0;
    bool         isDirty   = false;
    std::vector<std::wstring> tagUuids; // 🚀 关联标签 UUID 列表
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

// 番茄钟全局状态
extern PomodoroSession g_PomodoroSession;
extern std::vector<PomodoroTag> g_PomodoroTags;
extern std::vector<PomodoroRecord> g_PomodoroHistory; // 本次会话的历史记录（统计用）

// 🚀 跨端感知：其他设备的专注状态（由 WebSocket 推送更新）
struct RemoteFocusState {
    bool         active        = false;
    std::wstring sourceDevice;           // 发起端设备 ID
    std::wstring todoUuid;               // 绑定待办的 UUID（用于同步后回填内容）
    std::wstring todoContent;            // 绑定的待办内容（可为空）
    long long    targetEndMs   = 0;      // 专注结束的 UTC ms 时间戳
    long long    startTimeMs   = 0;      // 专注开始的 UTC ms 时间戳
    int          plannedSecs   = 1500;   // 计划专注秒数
    bool         isRestPhase   = false;  // true=休息中
    long long    receivedAt    = 0;      // 本机收到该状态的时间（用于偏差修正）
    std::vector<std::wstring> tagNames;  // 当前活跃的标签名列表（由 SYNC_TAGS/UPDATE_TAGS 更新）
};
extern RemoteFocusState g_RemoteFocus;

namespace InputState {
    extern std::wstring result1;
    extern std::wstring result2;
    extern std::wstring result3;
    extern std::wstring result4; // 备注 remark
    extern bool isOk;
}

// Declare global variables for window position and size
extern int g_WindowPosX;
extern int g_WindowPosY;
extern int g_WindowWidth;
extern int g_WindowHeight;

// 辅助宏：DPI 缩放
#define S(x) (int)((x) * g_Scale)

// Define default overlay dimensions
#define OV_W 300 // Default width of the overlay
#define OV_H 200 // Default height of the overlay
