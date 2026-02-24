#include "common.h"

// --- 常量定义 ---
const std::wstring API_HOST = L"mathquiz.junpgle.me";
const std::wstring SETTINGS_FILE = L"math_quiz_lite.ini";
const wchar_t *APP_NAME = L"MathQuizLite";

// --- 全局变量 ---
HINSTANCE g_hInst = NULL;
int g_UserId = 0;
std::wstring g_Username;
std::wstring g_SavedEmail;
std::wstring g_SavedPass;
HWND g_hWidgetWnd = NULL;

std::recursive_mutex g_DataMutex;
float g_Scale = 1.0f;
BYTE g_BgAlpha = 100;

HWND g_hEmail = NULL;
HWND g_hPass = NULL;
HWND g_hAutoLogin = NULL;
bool g_LoginSuccess = false;

std::vector<Todo> g_Todos;
std::vector<Countdown> g_Countdowns;
std::vector<HitZone> g_HitZones;

// 屏幕使用时间记录
std::map<std::wstring, int> g_AppUsage;

// 输入状态
namespace InputState {
    std::wstring result1;
    std::wstring result2;
    bool isOk = false;
    int currentType = 0;
}