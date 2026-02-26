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

HWND g_hEmail = NULL;
HWND g_hPass = NULL;
HWND g_hAutoLogin = NULL;
bool g_LoginSuccess = false;

std::wstring g_DeviceName;
std::wstring g_TaiDbPath;

std::vector<Todo> g_Todos;
std::vector<Countdown> g_Countdowns;
std::vector<HitZone> g_HitZones;

// 修正为 vector 类型
std::vector<AppUsageRecord> g_AppUsage;

namespace InputState {
    std::wstring result1;
    std::wstring result2;
    bool isOk = false;
    int currentType = 0;
}