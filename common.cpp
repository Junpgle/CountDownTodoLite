#include "common.h"

// 所有的全局变量在这里分配实际内存并初始化
const std::wstring API_HOST    = L"mathquiz.junpgle.me";
const std::wstring WS_HOST     = L"101.200.13.100";  // 阿里云 WS 服务器公网 IP
const WORD         WS_PORT     = 8081;
const std::wstring SETTINGS_FILE = L"math_quiz_lite.ini";
const wchar_t *APP_NAME = L"MathQuizLite";

HINSTANCE g_hInst = NULL;
int g_UserId = 0;
std::wstring g_Username;
std::wstring g_SavedEmail;
std::wstring g_SavedPass;
HWND g_hWidgetWnd = NULL;

// 🚀 新增全局变量
std::wstring g_AuthToken;       // 登录后由服务器返回的 HMAC Bearer Token
std::wstring g_DeviceId;        // 本机唯一设备 ID，持久化在 INI 中
long long    g_LastSyncTime = 0; // 上次成功同步的服务器时间戳（UTC ms）

std::recursive_mutex g_DataMutex;
float g_Scale = 1.0f;
BYTE g_BgAlpha = 100;
int g_TopAppsCount = 3;

// 同步与账户状态
int g_SyncLimit = 50;
int g_SyncCount = 0;
int g_SyncInterval = 0;
bool g_AutoLogin = false;
std::wstring g_UserTier = L"free"; // 补齐 api.cpp 之前定义的变量

// 系统状态
bool g_LoginSuccess = false;
std::wstring g_TaiDbPath;
std::wstring g_DeviceName;

// GDI+ 字体
Gdiplus::PrivateFontCollection g_FontCollection;
Gdiplus::FontFamily* g_MiSansFamily = nullptr;
std::wstring g_FontName = L"MiSans"; // 默认 MiSans，由设置界面覆盖

// 数据列表
std::vector<Todo> g_Todos;
std::vector<Countdown> g_Countdowns;
std::vector<HitZone> g_HitZones;
std::vector<AppUsageRecord> g_AppUsage;
std::vector<Course> g_Courses;

// 番茄钟全局状态
PomodoroSession g_PomodoroSession;
std::vector<PomodoroTag> g_PomodoroTags;
std::vector<PomodoroRecord> g_PomodoroHistory;
RemoteFocusState g_RemoteFocus;

namespace InputState {
    std::wstring result1;
    std::wstring result2;
    std::wstring result3;
    std::wstring result4; // 备注 remark
    bool isOk = false;
}

// Define global variables for window position and size
int g_WindowPosX = 100;
int g_WindowPosY = 100;
int g_WindowWidth = 800;
int g_WindowHeight = 600;
