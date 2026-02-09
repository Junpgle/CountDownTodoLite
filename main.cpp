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

// 请确保已下载 json.hpp 并放在同级目录
#include "json.hpp"

#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "crypt32.lib")
#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "user32.lib")

using json = nlohmann::json;
using namespace Gdiplus;

// --- 补全 MinGW 可能缺失的 TLS 定义 ---
#ifndef WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2
#define WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2 0x00000800
#endif
#ifndef WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3
#define WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3 0x00002000
#endif

// --- 补全 SSL 忽略标志 ---
#ifndef SECURITY_FLAG_IGNORE_UNKNOWN_CA
#define SECURITY_FLAG_IGNORE_UNKNOWN_CA 0x00000100
#endif
#ifndef SECURITY_FLAG_IGNORE_CERT_DATE_INVALID
#define SECURITY_FLAG_IGNORE_CERT_DATE_INVALID 0x00002000
#endif
#ifndef SECURITY_FLAG_IGNORE_CERT_CN_INVALID
#define SECURITY_FLAG_IGNORE_CERT_CN_INVALID 0x00001000
#endif
#ifndef SECURITY_FLAG_IGNORE_CERT_WRONG_USAGE
#define SECURITY_FLAG_IGNORE_CERT_WRONG_USAGE 0x00000200
#endif

// --- 自定义消息 ---
#define WM_USER_REFRESH (WM_USER + 1)

// --- 配置 ---
const std::wstring API_HOST = L"mathquiz.junpgle.me";
const std::wstring SETTINGS_FILE = L"math_quiz_lite.ini";
const wchar_t *APP_NAME = L"MathQuizLite";

// --- 全局变量 ---
HINSTANCE g_hInst;
int g_UserId = 0;
std::wstring g_Username;
std::wstring g_SavedEmail;
std::wstring g_SavedPass;
HWND g_hWidgetWnd = NULL;

// 数据互斥锁
// 修复：必须定义为 recursive_mutex 以匹配 lock_guard<std::recursive_mutex>
std::recursive_mutex g_DataMutex;

// DPI 缩放因子
float g_Scale = 1.0f;
// 背景透明度 (0-255)，默认 100
BYTE g_BgAlpha = 100;

// 登录界面控件句柄
HWND g_hEmail = NULL;
HWND g_hPass = NULL;
HWND g_hAutoLogin = NULL;
bool g_LoginSuccess = false;

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

std::vector<Todo> g_Todos;
std::vector<Countdown> g_Countdowns;

// --- 交互区域 (Hit Testing) ---
struct HitZone {
    Rect rect;
    int id;
    int type; // 1=AddTodo, 2=AddCountdown, 3=TodoItem, 4=CountdownItem
};

std::vector<HitZone> g_HitZones;

// --- 输入对话框全局状态 ---
namespace InputState {
    std::wstring result1;
    std::wstring result2;
    bool isOk = false;
    int currentType = 0;
}

// --- 工具函数：DPI 缩放 ---
int S(int val) {
    return (int) (val * g_Scale);
}

// --- 工具函数：字符串转换 ---
std::string ToUtf8(const std::wstring &wstr) {
    if (wstr.empty()) return std::string();
    int size_needed = WideCharToMultiByte(CP_UTF8, 0, &wstr[0], (int) wstr.size(), NULL, 0, NULL, NULL);
    std::string strTo(size_needed, 0);
    WideCharToMultiByte(CP_UTF8, 0, &wstr[0], (int) wstr.size(), &strTo[0], size_needed, NULL, NULL);
    return strTo;
}

std::wstring ToWide(const std::string &str) {
    if (str.empty()) return std::wstring();
    int size_needed = MultiByteToWideChar(CP_UTF8, 0, &str[0], (int) str.size(), NULL, 0);
    std::wstring wstrTo(size_needed, 0);
    MultiByteToWideChar(CP_UTF8, 0, &str[0], (int) str.size(), &wstrTo[0], size_needed);
    return wstrTo;
}

// --- 工具函数：时间处理 ---
time_t ParseSqlTime(const std::string &s) {
    if (s.empty()) return 0;
    int y, m, d, H, M, S;
    if (sscanf(s.c_str(), "%d-%d-%d %d:%d:%d", &y, &m, &d, &H, &M, &S) != 6) {
        if (sscanf(s.c_str(), "%d-%d-%dT%d:%d:%d", &y, &m, &d, &H, &M, &S) != 6) return std::time(nullptr);
    }
    std::tm tm = {0};
    tm.tm_year = y - 1900;
    tm.tm_mon = m - 1;
    tm.tm_mday = d;
    tm.tm_hour = H;
    tm.tm_min = M;
    tm.tm_sec = S;
    tm.tm_isdst = 0;
    return _mkgmtime(&tm);
}

// 计算剩余天数
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

// DPAPI 加密/解密
std::wstring EncryptString(const std::wstring &input) {
    if (input.empty()) return L"";
    DATA_BLOB DataIn = {(DWORD) ((input.length() + 1) * sizeof(wchar_t)), (BYTE *) input.c_str()};
    DATA_BLOB DataOut;
    if (CryptProtectData(&DataIn, L"MathQuizPwd", NULL, NULL, NULL, 0, &DataOut)) {
        std::wstringstream ss;
        for (DWORD i = 0; i < DataOut.cbData; ++i)
            ss << std::hex << std::setw(2) << std::setfill(L'0') << (int) DataOut.pbData[i];
        LocalFree(DataOut.pbData);
        return ss.str();
    }
    return L"";
}

std::wstring DecryptString(const std::wstring &hexInput) {
    if (hexInput.empty()) return L"";
    std::vector<BYTE> binary;
    for (size_t i = 0; i < hexInput.length(); i += 2) binary.push_back(
        (BYTE) wcstol(hexInput.substr(i, 2).c_str(), NULL, 16));
    DATA_BLOB DataIn = {(DWORD) binary.size(), binary.data()};
    DATA_BLOB DataOut;
    if (CryptUnprotectData(&DataIn, NULL, NULL, NULL, NULL, 0, &DataOut)) {
        std::wstring result = (wchar_t *) DataOut.pbData;
        LocalFree(DataOut.pbData);
        return result;
    }
    return L"";
}

// 注册表操作
void SetAutoStart(bool enable) {
    HKEY hKey;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run", 0, KEY_SET_VALUE,
                      &hKey) == ERROR_SUCCESS) {
        if (enable) {
            WCHAR path[MAX_PATH];
            GetModuleFileNameW(NULL, path, MAX_PATH);
            RegSetValueExW(hKey, APP_NAME, 0, REG_SZ, (BYTE *) path, (lstrlenW(path) + 1) * sizeof(WCHAR));
        } else RegDeleteValueW(hKey, APP_NAME);
        RegCloseKey(hKey);
    }
}

bool IsAutoStart() {
    HKEY hKey;
    bool exists = false;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run", 0, KEY_QUERY_VALUE,
                      &hKey) == ERROR_SUCCESS) {
        if (RegQueryValueExW(hKey, APP_NAME, NULL, NULL, NULL, NULL) == ERROR_SUCCESS) exists = true;
        RegCloseKey(hKey);
    }
    return exists;
}

// HTTP 请求封装 - 修复版 (增强稳定性 + 伪装浏览器 UA)
std::string SendRequest(const std::wstring &path, const std::string &method, const std::string &body) {
    std::string response;
    // 关键修改：使用 Chrome 的 User-Agent，防止被 Cloudflare 等 WAF 拦截
    HINTERNET hSession = WinHttpOpen(
        L"Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/120.0.0.0 Safari/537.36",
        WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (hSession) {
        // 设置超时 (毫秒)
        int timeout = 15000;
        WinHttpSetOption(hSession, WINHTTP_OPTION_CONNECT_TIMEOUT, &timeout, sizeof(timeout));
        WinHttpSetOption(hSession, WINHTTP_OPTION_SEND_TIMEOUT, &timeout, sizeof(timeout));
        WinHttpSetOption(hSession, WINHTTP_OPTION_RECEIVE_TIMEOUT, &timeout, sizeof(timeout));

        DWORD protocols = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2 | WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3;
        WinHttpSetOption(hSession, WINHTTP_OPTION_SECURE_PROTOCOLS, &protocols, sizeof(protocols));

        HINTERNET hConnect = WinHttpConnect(hSession, API_HOST.c_str(), INTERNET_DEFAULT_HTTPS_PORT, 0);
        if (hConnect) {
            HINTERNET hRequest = WinHttpOpenRequest(hConnect, ToWide(method).c_str(), path.c_str(), NULL,
                                                    WINHTTP_NO_REFERER,
                                                    WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
            if (hRequest) {
                DWORD flags =
                        SECURITY_FLAG_IGNORE_UNKNOWN_CA | SECURITY_FLAG_IGNORE_CERT_DATE_INVALID |
                        SECURITY_FLAG_IGNORE_CERT_CN_INVALID | SECURITY_FLAG_IGNORE_CERT_WRONG_USAGE;
                WinHttpSetOption(hRequest, WINHTTP_OPTION_SECURITY_FLAGS, &flags, sizeof(flags));

                std::wstring headers = L"Content-Type: application/json\r\n";
                if (WinHttpSendRequest(hRequest, headers.c_str(), -1L, (LPVOID) body.c_str(), (DWORD) body.size(),
                                       (DWORD) body.size(), 0)) {
                    if (WinHttpReceiveResponse(hRequest, NULL)) {
                        DWORD dwSize = 0, dwDownloaded = 0;
                        do {
                            dwSize = 0;
                            if (!WinHttpQueryDataAvailable(hRequest, &dwSize)) break;
                            if (dwSize == 0) break;
                            std::vector<char> buffer(dwSize + 1);
                            if (WinHttpReadData(hRequest, &buffer[0], dwSize, &dwDownloaded)) response.append(
                                buffer.data(), dwDownloaded);
                        } while (dwSize > 0);
                    }
                } else {
                    // 调试用：如果发送失败，记录错误码 (虽然这里无法直接打印，但可以在 MessageBox 中显示)
                    DWORD err = GetLastError();
                    if (response.empty()) response = "ERROR:" + std::to_string(err);
                }
                WinHttpCloseHandle(hRequest);
            } else {
                if (response.empty()) response = "ERROR:OpenRequest";
            }
            WinHttpCloseHandle(hConnect);
        } else {
            if (response.empty()) response = "ERROR:Connect";
        }
        WinHttpCloseHandle(hSession);
    } else {
        if (response.empty()) response = "ERROR:Session";
    }
    return response;
}

// --- API 操作 ---

void ApiToggleTodo(int id, bool currentStatus) {
    json j;
    j["id"] = id;
    j["is_completed"] = !currentStatus;
    SendRequest(L"/api/todos/toggle", "POST", j.dump());
}

void ApiAddTodo(const std::wstring &content) {
    json j;
    j["user_id"] = g_UserId;
    j["content"] = ToUtf8(content);
    SendRequest(L"/api/todos", "POST", j.dump());
    // 这里不再直接调用 SyncData，而是由主线程的后续流程触发，或者独立触发
}

void ApiDeleteTodo(int id) {
    json j;
    j["id"] = id;
    SendRequest(L"/api/todos", "DELETE", j.dump());
}

void ApiAddCountdown(const std::wstring &title, const std::wstring &dateStr) {
    std::wstring isoTime = dateStr + L"T00:00:00Z";
    json j;
    j["user_id"] = g_UserId;
    j["title"] = ToUtf8(title);
    j["target_time"] = ToUtf8(isoTime);
    SendRequest(L"/api/countdowns", "POST", j.dump());
}

void ApiDeleteCountdown(int id) {
    json j;
    j["id"] = id;
    SendRequest(L"/api/countdowns", "DELETE", j.dump());
}

// --- 核心业务：同步逻辑 ---

void SyncData() {
    if (g_UserId == 0) return;
    std::wstring ts = std::to_wstring(time(NULL));

    // 1. 同步待办事项
    std::string todoJson = SendRequest(L"/api/todos?user_id=" + std::to_wstring(g_UserId) + L"&t=" + ts, "GET", "");
    if (!todoJson.empty()) {
        try {
            auto j = json::parse(todoJson);
            std::lock_guard<std::recursive_mutex> lock(g_DataMutex); // 使用递归锁
            std::vector<Todo> mergedTodos;
            std::map<int, Todo> localMap;
            for (const auto &t: g_Todos) if (t.id > 0) localMap[t.id] = t; // 忽略 ID <= 0 的临时项

            for (const auto &item: j) {
                int id = item["id"].get<int>();
                std::string contentUtf8 = item["content"];
                bool cloudDone = false;
                if (item["is_completed"].is_boolean()) cloudDone = item["is_completed"];
                else if (item["is_completed"].is_number()) cloudDone = item["is_completed"].get<int>() == 1;

                std::string timeStr = item.contains("updated_at") && !item["updated_at"].is_null()
                                          ? item["updated_at"]
                                          : item["created_at"];
                time_t cloudTime = ParseSqlTime(timeStr);

                Todo newItem;
                newItem.id = id;
                newItem.content = ToWide(contentUtf8);

                if (localMap.count(id)) {
                    Todo &localItem = localMap[id];
                    if (localItem.lastUpdated > cloudTime) {
                        newItem.isDone = localItem.isDone;
                        newItem.lastUpdated = localItem.lastUpdated;
                        bool statusToPush = localItem.isDone;
                        CreateThread(NULL, 0, [](LPVOID p) -> DWORD {
                            auto *pData = (std::pair<int, bool> *) p;
                            json j;
                            j["id"] = pData->first;
                            j["is_completed"] = pData->second;
                            SendRequest(L"/api/todos/toggle", "POST", j.dump());
                            delete pData;
                            return 0;
                        }, new std::pair<int, bool>(id, statusToPush), 0, NULL);
                    } else {
                        newItem.isDone = cloudDone;
                        newItem.lastUpdated = cloudTime;
                    }
                } else {
                    newItem.isDone = cloudDone;
                    newItem.lastUpdated = cloudTime;
                }
                mergedTodos.push_back(newItem);
            }
            g_Todos = mergedTodos;
        } catch (...) {
        }
    }

    // 2. 同步倒计时
    std::string countJson = SendRequest(L"/api/countdowns?user_id=" + std::to_wstring(g_UserId) + L"&t=" + ts, "GET",
                                        "");
    if (!countJson.empty()) {
        try {
            auto j = json::parse(countJson);
            std::lock_guard<std::recursive_mutex> lock(g_DataMutex); // 使用递归锁
            g_Countdowns.clear();
            for (const auto &item: j) {
                std::string dateRaw = item.contains("target_time") ? item["target_time"] : item["date"];
                std::string timeStr = item.contains("updated_at") && !item["updated_at"].is_null()
                                          ? item["updated_at"]
                                          : item["created_at"];
                time_t cloudTime = ParseSqlTime(timeStr);
                std::wstring dateW = ToWide(dateRaw);
                int days = CalculateDaysLeft(dateW.substr(0, 10));
                g_Countdowns.push_back({
                    item["id"].get<int>(), ToWide(item["title"]), dateW.substr(0, 10), days, cloudTime
                });
            }
        } catch (...) {
        }
    }

    if (g_hWidgetWnd)
        PostMessage(g_hWidgetWnd, WM_USER_REFRESH, 0, 0);
}

// --- 设置逻辑 ---
void LoadSettings() {
    WCHAR path[MAX_PATH];
    GetModuleFileNameW(NULL, path, MAX_PATH);
    PathRemoveFileSpecW(path);
    PathAppendW(path, SETTINGS_FILE.c_str());
    g_UserId = GetPrivateProfileIntW(L"Auth", L"UserId", 0, path);
    g_BgAlpha = GetPrivateProfileIntW(L"Settings", L"BgAlpha", 100, path);
    WCHAR buf[256];
    GetPrivateProfileStringW(L"Auth", L"Username", L"", buf, 256, path);
    g_Username = buf;
    GetPrivateProfileStringW(L"Auth", L"Email", L"", buf, 256, path);
    g_SavedEmail = buf;
    WCHAR pass[1024];
    GetPrivateProfileStringW(L"Auth", L"Pass", L"", pass, 1024, path);
    g_SavedPass = DecryptString(pass);
}

void SaveSettings(int uid, const std::wstring &name, const std::wstring &email, const std::wstring &pass,
                  bool savePass) {
    WCHAR path[MAX_PATH];
    GetModuleFileNameW(NULL, path, MAX_PATH);
    PathRemoveFileSpecW(path);
    PathAppendW(path, SETTINGS_FILE.c_str());
    WritePrivateProfileStringW(L"Auth", L"UserId", std::to_wstring(uid).c_str(), path);
    WritePrivateProfileStringW(L"Auth", L"Username", name.c_str(), path);
    WritePrivateProfileStringW(L"Auth", L"Email", email.c_str(), path);
    if (savePass) WritePrivateProfileStringW(L"Auth", L"Pass", EncryptString(pass).c_str(), path);
    else WritePrivateProfileStringW(L"Auth", L"Pass", NULL, path);
}

void SaveAlphaSetting() {
    WCHAR path[MAX_PATH];
    GetModuleFileNameW(NULL, path, MAX_PATH);
    PathRemoveFileSpecW(path);
    PathAppendW(path, SETTINGS_FILE.c_str());
    WritePrivateProfileStringW(L"Settings", L"BgAlpha", std::to_wstring(g_BgAlpha).c_str(), path);
}

// --- 渲染逻辑 ---
void RenderWidget() {
    if (!g_hWidgetWnd) return;
    RECT rc;
    GetWindowRect(g_hWidgetWnd, &rc);
    int width = rc.right - rc.left;
    int height = rc.bottom - rc.top;
    if (width <= 0 || height <= 0) return;

    HDC hdcScreen = GetDC(NULL);
    HDC hdcMem = CreateCompatibleDC(hdcScreen);
    BITMAPINFO bmi = {0};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = width;
    bmi.bmiHeader.biHeight = -height;
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;
    void *pBits = nullptr;
    HBITMAP hBitmap = CreateDIBSection(hdcMem, &bmi, DIB_RGB_COLORS, &pBits, NULL, 0);
    HBITMAP hOld = (HBITMAP) SelectObject(hdcMem, hBitmap); {
        Graphics g(hdcMem);
        g.SetSmoothingMode(SmoothingModeAntiAlias);
        g.SetTextRenderingHint(TextRenderingHintClearTypeGridFit);
        g.Clear(Color(0, 0, 0, 0));

        // 加锁读取数据
        std::lock_guard<std::recursive_mutex> lock(g_DataMutex); // 使用递归锁
        g_HitZones.clear();

        int r = S(15);
        GraphicsPath path;
        path.AddArc(0, 0, r * 2, r * 2, 180, 90);
        path.AddArc(width - r * 2, 0, r * 2, r * 2, 270, 90);
        path.AddArc(width - r * 2, height - r * 2, r * 2, r * 2, 0, 90);
        path.AddArc(0, height - r * 2, r * 2, r * 2, 90, 90);
        path.CloseFigure();
        SolidBrush bg(Color(g_BgAlpha, 25, 25, 25));
        g.FillPath(&bg, &path);

        FontFamily ff(L"MiSans");
        Font titleF(&ff, (REAL) S(16), FontStyleBold, UnitPixel);
        Font headF(&ff, (REAL) S(12), FontStyleRegular, UnitPixel);
        SolidBrush wBrush(Color(255, 255, 255, 255));
        SolidBrush gBrush(Color(255, 180, 180, 180));
        SolidBrush grBrush(Color(255, 80, 220, 80));
        SolidBrush rBrush(Color(255, 255, 100, 100));
        Pen linePen(Color(100, 255, 255, 255), 1);

        g.DrawString((L"Hello, " + g_Username).c_str(), -1, &titleF, PointF((REAL) S(15), (REAL) S(15)), &wBrush);
        g.DrawLine(&linePen, S(15), S(45), width - S(15), S(45));

        float y = (float) S(55);
        // "倒计时"
        g.DrawString(L"\u5012\u8ba1\u65f6", -1, &headF, PointF((REAL) S(15), y), &gBrush);
        g.DrawString(L"[+]", -1, &headF, PointF((REAL) (width - S(40)), y), &grBrush);
        g_HitZones.push_back({Rect(width - S(45), (int) y, S(30), S(20)), 0, 2});

        Font contentF(&ff, (REAL) S(14), FontStyleRegular, UnitPixel);
        y += S(20);
        for (const auto &it: g_Countdowns) {
            g_HitZones.push_back({Rect(S(15), (int) y, width - S(30), S(20)), it.id, 4});
            g.DrawString(it.title.c_str(), -1, &contentF, PointF((REAL) S(15), y), &wBrush);
            // " 天"
            std::wstring d = std::to_wstring(it.daysLeft) + L" \u5929";
            g.DrawString(d.c_str(), -1, &contentF, PointF((REAL) (width - S(60)), y),
                         it.daysLeft <= 3 ? &rBrush : &grBrush);
            y += S(20);
        }

        y += S(10);
        // "待办事项"
        g.DrawString(L"\u5f85\u529e\u4e8b\u9879", -1, &headF, PointF((REAL) S(15), y), &gBrush);
        g.DrawString(L"[+]", -1, &headF, PointF((REAL) (width - S(40)), y), &grBrush);
        g_HitZones.push_back({Rect(width - S(45), (int) y, S(30), S(20)), 0, 1});

        y += S(20);
        if (g_Todos.empty()) {
            // "暂无待办"
            g.DrawString(L"\u6682\u65e0\u5f85\u529e", -1, &contentF, PointF((REAL) S(15), y), &gBrush);
        } else {
            for (const auto &it: g_Todos) {
                g_HitZones.push_back({Rect(S(15), (int) y, width - S(30), S(20)), it.id, 3});
                g.DrawRectangle(&linePen, S(15), (int) y + S(4), S(10), S(10));
                if (it.isDone) g.FillRectangle(&wBrush, S(17), (int) y + S(6), S(6), S(6));

                int style = it.isDone ? FontStyleStrikeout : FontStyleRegular;
                SolidBrush dBrush(Color(100, 255, 255, 255));
                Font itemF(&ff, (REAL) S(14), style, UnitPixel);
                g.DrawString(it.content.c_str(), -1, &itemF, PointF((REAL) S(30), y), it.isDone ? &dBrush : &wBrush);
                y += S(20);
            }
        }
    }

    POINT ptSrc = {0, 0};
    POINT ptDest = {rc.left, rc.top};
    SIZE sz = {width, height};
    BLENDFUNCTION bf = {AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
    UpdateLayeredWindow(g_hWidgetWnd, hdcScreen, &ptDest, &sz, hdcMem, &ptSrc, 0, &bf, ULW_ALPHA);
    SelectObject(hdcMem, hOld);
    DeleteObject(hBitmap);
    DeleteDC(hdcMem);
    ReleaseDC(NULL, hdcScreen);
}

void ResizeWidget() {
    if (!g_hWidgetWnd) return;
    std::lock_guard<std::recursive_mutex> lock(g_DataMutex); // 使用递归锁
    int h = S(55) + S(20) + S(10) + S(20) + S(10);
    h += (int) g_Countdowns.size() * S(20);
    h += (g_Todos.empty() ? 1 : (int) g_Todos.size()) * S(20);
    if (h < S(150)) h = S(150);
    RECT rc;
    GetWindowRect(g_hWidgetWnd, &rc);
    SetWindowPos(g_hWidgetWnd, HWND_BOTTOM, rc.left, rc.top, S(300), h, SWP_NOACTIVATE);
    RenderWidget();
}

// --- 输入窗口逻辑 ---
HFONT GetMiSansFont(int s) {
    return CreateFontW(S(s), 0, 0, 0,FW_NORMAL, 0, 0, 0,DEFAULT_CHARSET, 0, 0,CLEARTYPE_QUALITY,DEFAULT_PITCH,
                       L"MiSans");
}

LRESULT CALLBACK InputWndProc(HWND hWnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_COMMAND && LOWORD(wp) == IDOK) {
        WCHAR buf[256];
        GetDlgItemTextW(hWnd, 101, buf, 256);
        InputState::result1 = buf;
        if (InputState::currentType == 0) {
            int idx = SendMessageW(GetDlgItem(hWnd, 105), CB_GETCURSEL, 0, 0);
            if (idx > 0) {
                WCHAR c[20], n[20];
                SendMessageW(GetDlgItem(hWnd, 105), CB_GETLBTEXT, idx, (LPARAM) c);
                GetDlgItemTextW(hWnd, 102, n, 20);
                // "次"
                InputState::result1 += L" [" + std::wstring(c) + L", " + n + L"\u6b21]";
            }
        } else {
            GetDlgItemTextW(hWnd, 102, buf, 256);
            InputState::result2 = buf;
        }
        InputState::isOk = !InputState::result1.empty();
        DestroyWindow(hWnd);
        return 0;
    }
    if (msg == WM_CLOSE) {
        DestroyWindow(hWnd);
        return 0;
    }
    return DefWindowProc(hWnd, msg, wp, lp);
}

bool ShowInputDialog(HWND parent, int type, std::wstring &out1, std::wstring &out2) {
    InputState::isOk = false;
    InputState::currentType = type;
    const wchar_t CN[] = L"InputDlgClass";
    UnregisterClassW(CN, g_hInst);
    WNDCLASSW wc = {0};
    wc.lpfnWndProc = InputWndProc;
    wc.hInstance = g_hInst;
    wc.lpszClassName = CN;
    wc.hbrBackground = (HBRUSH) (COLOR_WINDOW + 1);
    RegisterClassW(&wc);
    HFONT hF = GetMiSansFont(14);

    // "添加待办事项" / "添加倒计时"
    std::wstring title = type == 0 ? L"\u6dfb\u52a0\u5f85\u529e\u4e8b\u9879" : L"\u6dfb\u52a0\u5012\u8ba1\u65f6";
    HWND hDlg = CreateWindowW(CN, title.c_str(), WS_OVERLAPPED|WS_CAPTION|WS_SYSMENU|WS_VISIBLE, CW_USEDEFAULT,
                              CW_USEDEFAULT, S(320), type==0?S(240):S(200), parent, NULL, g_hInst, NULL);

    // "内容:" / "标题:"
    std::wstring label1 = type == 0 ? L"\u5185\u5bb9\u003a" : L"\u6807\u9898\u003a";
    HWND hL1 = CreateWindowW(L"STATIC", label1.c_str(), WS_VISIBLE|WS_CHILD, S(20), S(20), S(50), S(20), hDlg, NULL,
                             g_hInst, NULL);
    SendMessage(hL1, WM_SETFONT, (WPARAM) hF, 1);
    HWND hE1 = CreateWindowW(L"EDIT", L"", WS_VISIBLE|WS_CHILD|WS_BORDER, S(80), S(20), S(200), S(20), hDlg, (HMENU)101,
                             g_hInst, NULL);
    SendMessage(hE1, WM_SETFONT, (WPARAM) hF, 1);

    if (type == 0) {
        // "重复:"
        CreateWindowW(L"STATIC", L"\u91cd\u590d\u003a", WS_VISIBLE|WS_CHILD, S(20), S(55), S(50), S(20), hDlg, NULL,
                      g_hInst, NULL);
        HWND hC = CreateWindowW(L"COMBOBOX", L"", WS_VISIBLE|WS_CHILD|CBS_DROPDOWNLIST|WS_VSCROLL, S(80), S(50), S(200),
                                S(120), hDlg, (HMENU)105, g_hInst, NULL);
        SendMessage(hC, WM_SETFONT, (WPARAM) hF, 1);
        // "不重复", "每天", "每周", "每月", "每年"
        SendMessage(hC, CB_ADDSTRING, 0, (LPARAM) L"\u4e0d\u91cd\u590d");
        SendMessage(hC, CB_ADDSTRING, 0, (LPARAM) L"\u6bcf\u5929");
        SendMessage(hC, CB_ADDSTRING, 0, (LPARAM) L"\u6bcf\u5468");
        SendMessage(hC, CB_ADDSTRING, 0, (LPARAM) L"\u6bcf\u6708");
        SendMessage(hC, CB_ADDSTRING, 0, (LPARAM) L"\u6bcf\u5e74");
        SendMessage(hC, CB_SETCURSEL, 0, 0);
        // "次数:"
        CreateWindowW(L"STATIC", L"\u6b21\u6570\u003a", WS_VISIBLE|WS_CHILD, S(20), S(85), S(50), S(20), hDlg, NULL,
                      g_hInst, NULL);
        HWND hE2 = CreateWindowW(L"EDIT", L"1", WS_VISIBLE|WS_CHILD|WS_BORDER|ES_NUMBER, S(80), S(85), S(200), S(20),
                                 hDlg, (HMENU)102, g_hInst, NULL);
        SendMessage(hE2, WM_SETFONT, (WPARAM) hF, 1);
    } else {
        // "日期:"
        CreateWindowW(L"STATIC", L"\u65e5\u671f\u003a", WS_VISIBLE|WS_CHILD, S(20), S(50), S(50), S(20), hDlg, NULL,
                      g_hInst, NULL);
        SYSTEMTIME st;
        GetLocalTime(&st);
        wchar_t db[20];
        wsprintfW(db, L"%04d-%02d-%02d", st.wYear, st.wMonth, st.wDay);
        HWND hE2 = CreateWindowW(L"EDIT", db, WS_VISIBLE|WS_CHILD|WS_BORDER, S(80), S(50), S(200), S(20), hDlg,
                                 (HMENU)102, g_hInst, NULL);
        SendMessage(hE2, WM_SETFONT, (WPARAM) hF, 1);
    }
    // "确定"
    HWND hBtn = CreateWindowW(L"BUTTON", L"\u786e\u5b9a", WS_VISIBLE|WS_CHILD|BS_DEFPUSHBUTTON, S(110),
                              type==0?S(125):S(100), S(80), S(30), hDlg, (HMENU)IDOK, g_hInst, NULL);
    SendMessage(hBtn, WM_SETFONT, (WPARAM) hF, 1);

    EnableWindow(parent, 0);
    MSG msg;
    while (GetMessage(&msg, NULL, 0, 0)) {
        if (msg.message == WM_KEYDOWN && msg.wParam == VK_RETURN) {
            SendMessage(hDlg, WM_COMMAND, IDOK, 0);
            continue;
        }
        TranslateMessage(&msg);
        DispatchMessage(&msg);
        if (!IsWindow(hDlg)) break;
    }
    EnableWindow(parent, 1);
    SetForegroundWindow(parent);
    DeleteObject(hF);
    out1 = InputState::result1;
    out2 = InputState::result2;
    return InputState::isOk;
}

// --- 主窗口过程 ---
LRESULT CALLBACK WidgetWndProc(HWND hWnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_CREATE: SetTimer(hWnd, 1, 60000, NULL);
            CreateThread(NULL, 0, [](LPVOID) {
                SyncData();
                return (DWORD) 0;
            }, NULL, 0, NULL);
            break;
        case WM_TIMER: CreateThread(NULL, 0, [](LPVOID) {
                SyncData();
                return (DWORD) 0;
            }, NULL, 0, NULL);
            break;
        case WM_USER_REFRESH: RenderWidget();
            ResizeWidget();
            break;
        case WM_LBUTTONDOWN: {
            int x = LOWORD(lp), y = HIWORD(lp);
            bool hit = false;
            std::vector<HitZone> zonesCopy; {
                std::lock_guard<std::recursive_mutex> lock(g_DataMutex);
                zonesCopy = g_HitZones;
            }

            for (const auto &z: zonesCopy) {
                if (z.rect.Contains(x, y)) {
                    hit = true;
                    if (z.type == 1) {
                        // Add Todo
                        std::wstring c, d;
                        if (ShowInputDialog(hWnd, 0, c, d)) {
                            // 1. 本地立即添加 (乐观更新)
                            {
                                std::lock_guard<std::recursive_mutex> lock(g_DataMutex);
                                g_Todos.insert(g_Todos.begin(), {-1, c, false, std::time(nullptr)});
                            }
                            RenderWidget();
                            ResizeWidget();

                            // 2. 后台同步
                            CreateThread(NULL, 0, [](LPVOID p) {
                                std::wstring *s = (std::wstring *) p;
                                ApiAddTodo(*s);
                                SyncData();
                                delete s;
                                return (DWORD) 0;
                            }, new std::wstring(c), 0, NULL);
                        }
                    } else if (z.type == 2) {
                        // Add Count
                        std::wstring t, d;
                        if (ShowInputDialog(hWnd, 1, t, d)) {
                            // 1. 本地立即添加 (乐观更新)
                            {
                                std::lock_guard<std::recursive_mutex> lock(g_DataMutex);
                                int days = CalculateDaysLeft(d);
                                g_Countdowns.push_back({-1, t, d, days, std::time(nullptr)});
                            }
                            RenderWidget();
                            ResizeWidget();

                            // 2. 后台同步
                            struct Ctx {
                                std::wstring t, d;
                            };
                            CreateThread(NULL, 0, [](LPVOID p) {
                                Ctx *c = (Ctx *) p;
                                ApiAddCountdown(c->t, c->d);
                                SyncData();
                                delete c;
                                return (DWORD) 0;
                            }, new Ctx{t, d}, 0, NULL);
                        }
                    } else if (z.type == 3) {
                        // Toggle Todo
                        std::lock_guard<std::recursive_mutex> lock(g_DataMutex);
                        for (auto &t: g_Todos) {
                            if (t.id == z.id) {
                                t.isDone = !t.isDone;
                                t.lastUpdated = std::time(nullptr);
                                PostMessage(hWnd, WM_USER_REFRESH, 0, 0);

                                bool status = t.isDone;
                                int id = t.id;
                                if (id > 0) {
                                    CreateThread(NULL, 0, [](LPVOID p) {
                                        auto *pair = (std::pair<int, bool> *) p;
                                        json j;
                                        j["id"] = pair->first;
                                        j["is_completed"] = pair->second;
                                        SendRequest(L"/api/todos/toggle", "POST", j.dump());
                                        delete pair;
                                        return (DWORD) 0;
                                    }, new std::pair<int, bool>(id, status), 0, NULL);
                                }
                                break;
                            }
                        }
                    }
                    break;
                }
            }
            if (!hit)
                SendMessage(hWnd, WM_NCLBUTTONDOWN, HTCAPTION, 0);
        }
        break;
        case WM_RBUTTONUP: {
            POINT pt;
            GetCursorPos(&pt);
            HMENU hMenu = CreatePopupMenu();
            int selId = 0, selType = 0; {
                std::lock_guard<std::recursive_mutex> lock(g_DataMutex);
                int x = LOWORD(lp), y = HIWORD(lp);
                for (const auto &z: g_HitZones) if (z.rect.Contains(x, y) && (z.type == 3 || z.type == 4)) {
                    selId = z.id;
                    selType = z.type;
                    break;
                }
            }
            // "删除此项"
            if (selId > 0) AppendMenuW(hMenu, 0, 1001, L"\u5220\u9664\u6b64\u9879");

            HMENU hSub = CreatePopupMenu();
            // "20%", "40% (默认)", "60%", "80%", "100% (不透明)"
            AppendMenuW(hSub, MF_STRING | (g_BgAlpha == 50 ? MF_CHECKED : 0), 2001, L"20%");
            AppendMenuW(hSub, MF_STRING | (g_BgAlpha == 100 ? MF_CHECKED : 0), 2002, L"40% (\u9ed8\u8ba4)");
            AppendMenuW(hSub, MF_STRING | (g_BgAlpha == 150 ? MF_CHECKED : 0), 2003, L"60%");
            AppendMenuW(hSub, MF_STRING | (g_BgAlpha == 200 ? MF_CHECKED : 0), 2004, L"80%");
            AppendMenuW(hSub, MF_STRING | (g_BgAlpha == 255 ? MF_CHECKED : 0), 2005, L"100% (\u4e0d\u900f\u660e)");
            // "背景透明度"
            AppendMenuW(hMenu, MF_POPUP, (UINT_PTR) hSub, L"\u80cc\u666f\u900f\u660e\u5ea6");

            // "立即刷新"
            AppendMenuW(hMenu, 0, 1002, L"\u7acb\u5373\u5237\u65b0");
            // "退出程序"
            AppendMenuW(hMenu, 0, 1004, L"\u9000\u51fa\u7a0b\u5e8f");

            int cmd = TrackPopupMenu(hMenu, TPM_RETURNCMD | TPM_TOPALIGN | TPM_LEFTALIGN, pt.x, pt.y, 0, hWnd, NULL);
            if (cmd == 1001) {
                {
                    std::lock_guard<std::recursive_mutex> lock(g_DataMutex);
                    if (selType == 3) {
                        for (auto it = g_Todos.begin(); it != g_Todos.end(); ++it) if (it->id == selId) {
                            g_Todos.erase(it);
                            break;
                        }
                    } else if (selType == 4) {
                        for (auto it = g_Countdowns.begin(); it != g_Countdowns.end(); ++it) if (it->id == selId) {
                            g_Countdowns.erase(it);
                            break;
                        }
                    }
                }
                RenderWidget();
                ResizeWidget();

                if (selType == 3) CreateThread(NULL, 0, [](LPVOID p) {
                    ApiDeleteTodo((int) (uintptr_t) p);
                    return (DWORD) 0;
                }, (LPVOID) (uintptr_t) selId, 0, NULL);
                if (selType == 4) CreateThread(NULL, 0, [](LPVOID p) {
                    ApiDeleteCountdown((int) (uintptr_t) p);
                    return (DWORD) 0;
                }, (LPVOID) (uintptr_t) selId, 0, NULL);
            } else if (cmd >= 2001 && cmd <= 2005) {
                if (cmd == 2001) g_BgAlpha = 50;
                else if (cmd == 2002) g_BgAlpha = 100;
                else if (cmd == 2003) g_BgAlpha = 150;
                else if (cmd == 2004) g_BgAlpha = 200;
                else g_BgAlpha = 255;
                SaveAlphaSetting();
                RenderWidget();
            } else if (cmd == 1002) CreateThread(NULL, 0, [](LPVOID) {
                SyncData();
                return (DWORD) 0;
            }, NULL, 0, NULL);
            else if (cmd == 1004) PostQuitMessage(0);
            DestroyMenu(hMenu);
        }
        break;
        case WM_PAINT: ValidateRect(hWnd, NULL);
            return 0;
        case WM_DESTROY: PostQuitMessage(0);
            break;
    }
    return DefWindowProc(hWnd, msg, wp, lp);
}

// ... Main & Auth functions ...

LRESULT CALLBACK LoginWndProc(HWND hWnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_COMMAND && LOWORD(wp) == IDOK) {
        WCHAR e[100], p[100];
        GetWindowTextW(g_hEmail, e, 100);
        GetWindowTextW(g_hPass, p, 100);
        bool rem = SendMessage(g_hAutoLogin, BM_GETCHECK, 0, 0) == BST_CHECKED;
        // "请输入邮箱和密码", "提示"
        if (lstrlenW(e) == 0 || lstrlenW(p) == 0) {
            MessageBoxW(hWnd, L"\u8bf7\u8f93\u5165\u90ae\u7bb1\u548c\u5bc6\u7801", L"\u63d0\u793a", 0);
            return 0;
        }
        SetCursor(LoadCursor(0, IDC_WAIT));
        std::string body = "{\"email\":\"" + ToUtf8(e) + "\",\"password\":\"" + ToUtf8(p) + "\"}";
        std::string res = SendRequest(L"/api/auth/login", "POST", body);
        SetCursor(LoadCursor(0, IDC_ARROW));

        if (res.empty() || res.find("ERROR:") == 0) {
            // "网络错误"
            std::wstring errMsg = L"\u7f51\u7edc\u9519\u8bef";
            if (res.find("ERROR:") == 0) {
                errMsg += L" (" + ToWide(res) + L")";
            }
            MessageBoxW(hWnd, errMsg.c_str(), L"Error", 16);
            return 0;
        }

        try {
            auto j = json::parse(res);
            if (j["success"].get<bool>()) {
                g_UserId = j["user"]["id"];
                g_Username = ToWide(j["user"]["username"]);
                SaveSettings(g_UserId, g_Username, e, rem ? p : L"", rem);
                g_LoginSuccess = true;
                DestroyWindow(hWnd);
            } else {
                std::string msg = j.contains("message") ? j["message"] : "Unknown error";
                // "登录失败"
                MessageBoxW(hWnd, ToWide(msg).c_str(), L"\u767b\u5f55\u5931\u8d25", 16);
            }
        } catch (...) {
            // "解析响应失败"
            MessageBoxW(hWnd, L"\u89e3\u6790\u54cd\u5e94\u5931\u8d25", L"Error", 16);
        }
    }
    if (msg == WM_CLOSE) DestroyWindow(hWnd);
    if (msg == WM_DESTROY) PostQuitMessage(0);
    return DefWindowProc(hWnd, msg, wp, lp);
}

bool AttemptAutoLogin() {
    if (g_SavedEmail.empty() || g_SavedPass.empty()) return false;
    std::string body = "{\"email\":\"" + ToUtf8(g_SavedEmail) + "\",\"password\":\"" + ToUtf8(g_SavedPass) + "\"}";
    std::string res = SendRequest(L"/api/auth/login", "POST", body);
    if (res.empty()) return false;
    try {
        auto j = json::parse(res);
        if (j["success"].get<bool>()) {
            g_UserId = j["user"]["id"];
            g_Username = ToWide(j["user"]["username"]);
            SaveSettings(g_UserId, g_Username, g_SavedEmail, g_SavedPass, true);
            return true;
        }
    } catch (...) {
    }
    return false;
}

bool ShowLogin() {
    const wchar_t CN[] = L"LoginClass";
    WNDCLASSW wc = {0};
    wc.lpfnWndProc = LoginWndProc;
    wc.hInstance = g_hInst;
    wc.lpszClassName = CN;
    wc.hbrBackground = (HBRUSH) (COLOR_WINDOW + 1);
    RegisterClassW(&wc);
    HWND h = CreateWindowW(CN, L"Math Quiz Login", WS_OVERLAPPED|WS_CAPTION|WS_SYSMENU|WS_VISIBLE,
                           (GetSystemMetrics(0)-S(300))/2, (GetSystemMetrics(1)-S(220))/2, S(300), S(220), 0, 0,
                           g_hInst, 0);
    HFONT f = GetMiSansFont(14);
    CreateWindowW(L"STATIC", L"Email:", WS_VISIBLE|WS_CHILD, S(20), S(20), S(60), S(20), h, 0, g_hInst, 0);
    g_hEmail = CreateWindowW(L"EDIT", g_SavedEmail.c_str(), WS_VISIBLE|WS_CHILD|WS_BORDER, S(80), S(20), S(180), S(20),
                             h, 0, g_hInst, 0);
    CreateWindowW(L"STATIC", L"Pass:", WS_VISIBLE|WS_CHILD, S(20), S(50), S(60), S(20), h, 0, g_hInst, 0);
    g_hPass = CreateWindowW(L"EDIT", L"", WS_VISIBLE|WS_CHILD|WS_BORDER|ES_PASSWORD, S(80), S(50), S(180), S(20), h, 0,
                            g_hInst, 0);
    // "自动登录 / 记住密码"
    g_hAutoLogin = CreateWindowW(L"BUTTON", L"\u81ea\u52a8\u767b\u5f55 / \u8bb0\u4f4f\u5bc6\u7801",
                                 WS_VISIBLE|WS_CHILD|BS_AUTOCHECKBOX, S(80), S(80), S(180), S(20), h, (HMENU)103,
                                 g_hInst, 0);
    SendMessage(g_hAutoLogin, BM_SETCHECK, BST_CHECKED, 0);
    CreateWindowW(L"BUTTON", L"Login", WS_VISIBLE|WS_CHILD|BS_DEFPUSHBUTTON, S(100), S(120), S(80), S(30), h,
                  (HMENU)IDOK, g_hInst, 0);
    EnumChildWindows(h, [](HWND c, LPARAM p) {
        SendMessage(c, WM_SETFONT, (WPARAM) p, 1);
        return TRUE;
    }, (LPARAM) f);
    MSG m;
    while (GetMessage(&m, 0, 0, 0)) {
        TranslateMessage(&m);
        DispatchMessage(&m);
    }
    DeleteObject(f);
    return g_LoginSuccess;
}

int APIENTRY WinMain(HINSTANCE hI, HINSTANCE, LPSTR, int nC) {
    g_hInst = hI;
    SetProcessDPIAware();
    HDC sc = GetDC(0);
    g_Scale = GetDeviceCaps(sc, LOGPIXELSY) / 96.0f;
    ReleaseDC(0, sc);
    GdiplusStartupInput gsi;
    ULONG_PTR gt;
    GdiplusStartup(&gt, &gsi, NULL);
    LoadSettings();
    if (!AttemptAutoLogin()) if (!ShowLogin()) {
        GdiplusShutdown(gt);
        return 0;
    }

    WNDCLASSW wc = {0};
    wc.lpfnWndProc = WidgetWndProc;
    wc.hInstance = hI;
    wc.lpszClassName = L"MathWidget";
    wc.hCursor = LoadCursor(0, IDC_ARROW);
    wc.hbrBackground = (HBRUSH) GetStockObject(BLACK_BRUSH);
    RegisterClassW(&wc);
    g_hWidgetWnd = CreateWindowExW(WS_EX_LAYERED | WS_EX_TOOLWINDOW, L"MathWidget", L"Math", WS_POPUP,
                                   GetSystemMetrics(0) - S(320), S(100), S(300), S(500), 0, 0, hI, 0);
    ShowWindow(g_hWidgetWnd, nC);
    UpdateWindow(g_hWidgetWnd);
    ResizeWidget();
    RenderWidget();

    MSG m;
    while (GetMessage(&m, 0, 0, 0)) {
        TranslateMessage(&m);
        DispatchMessage(&m);
    }
    GdiplusShutdown(gt);
    return (int) m.wParam;
}
