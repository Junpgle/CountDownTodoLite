#include "common.h"

const std::wstring API_HOST = L"mathquiz.junpgle.me";
const std::wstring SETTINGS_FILE = L"math_quiz_lite.ini";
const wchar_t *APP_NAME = L"MathQuizLite";

HINSTANCE g_hInst = NULL;
int g_UserId = 0;
std::wstring g_Username;
std::wstring g_SavedEmail;
std::wstring g_SavedPass;
HWND g_hWidgetWnd = NULL;

std::recursive_mutex g_DataMutex;
float g_Scale = 1.0f;
BYTE g_BgAlpha = 100;
int g_TopAppsCount = 3;

// 全局状态变量定义
bool g_LoginSuccess = false;
std::wstring g_TaiDbPath;
std::wstring g_DeviceName;

std::vector<Todo> g_Todos;
std::vector<Countdown> g_Countdowns;
std::vector<HitZone> g_HitZones;
std::vector<AppUsageRecord> g_AppUsage;

namespace InputState {
    std::wstring result1;
    std::wstring result2;
    std::wstring result3; // 必须在此定义，以匹配 common.h 中的 extern 声明
    bool isOk = false;
}