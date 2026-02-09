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

// 请确保已下载 json.hpp 并放在同级目录
#include "json.hpp"

#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "crypt32.lib")
#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "shlwapi.lib")

using json = nlohmann::json;
using namespace Gdiplus;

// --- 补全 MinGW 可能缺失的 TLS 定义 ---
#ifndef WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2
#define WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2 0x00000800
#endif
#ifndef WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3
#define WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3 0x00002000
#endif

// --- 配置 ---
const std::wstring API_HOST = L"mathquiz.junpgle.me";
const std::wstring SETTINGS_FILE = L"math_quiz_lite.ini";
const wchar_t *APP_NAME = L"MathQuizLite";

// --- 全局变量 ---
HINSTANCE g_hInst;
int g_UserId = 0;
std::wstring g_Username;
std::wstring g_SavedEmail;
std::wstring g_SavedPass; // 内存中保存的密码（如需）
HWND g_hWidgetWnd = NULL;

// 登录界面控件句柄
HWND g_hEmail = NULL;
HWND g_hPass = NULL;
HWND g_hAutoLogin = NULL; // 复选框
bool g_LoginSuccess = false;

// --- 数据结构 ---
struct Todo {
    int id;
    std::wstring content;
    bool isDone;
};

struct Countdown {
    int id;
    std::wstring title;
    std::wstring dateStr;
    int daysLeft;
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

// --- 工具函数 ---

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

// DPAPI 加密 (保护密码)
std::wstring EncryptString(const std::wstring &input) {
    if (input.empty()) return L"";
    DATA_BLOB DataIn;
    DATA_BLOB DataOut;
    DataIn.pbData = (BYTE *) input.c_str();
    DataIn.cbData = (input.length() + 1) * sizeof(wchar_t);

    // 使用当前用户凭据加密，无需额外 entropy
    if (CryptProtectData(&DataIn, L"MathQuizPwd", NULL, NULL, NULL, 0, &DataOut)) {
        // 转为 Hex 字符串存储
        std::wstringstream ss;
        for (DWORD i = 0; i < DataOut.cbData; ++i)
            ss << std::hex << std::setw(2) << std::setfill(L'0') << (int) DataOut.pbData[i];
        LocalFree(DataOut.pbData);
        return ss.str();
    }
    return L"";
}

// DPAPI 解密
std::wstring DecryptString(const std::wstring &hexInput) {
    if (hexInput.empty()) return L"";
    std::vector<BYTE> binary;
    // Hex 转二进制
    for (size_t i = 0; i < hexInput.length(); i += 2) {
        std::wstring byteString = hexInput.substr(i, 2);
        binary.push_back((BYTE) wcstol(byteString.c_str(), NULL, 16));
    }

    DATA_BLOB DataIn;
    DATA_BLOB DataOut;
    DataIn.pbData = binary.data();
    DataIn.cbData = (DWORD) binary.size();

    if (CryptUnprotectData(&DataIn, NULL, NULL, NULL, NULL, 0, &DataOut)) {
        std::wstring result = (wchar_t *) DataOut.pbData;
        LocalFree(DataOut.pbData);
        return result;
    }
    return L"";
}

// 注册表操作：开机自启
void SetAutoStart(bool enable) {
    HKEY hKey;
    const wchar_t *keyPath = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
    if (RegOpenKeyExW(HKEY_CURRENT_USER, keyPath, 0, KEY_SET_VALUE, &hKey) == ERROR_SUCCESS) {
        if (enable) {
            WCHAR path[MAX_PATH];
            GetModuleFileNameW(NULL, path, MAX_PATH);
            RegSetValueExW(hKey, APP_NAME, 0, REG_SZ, (BYTE *) path, (lstrlenW(path) + 1) * sizeof(WCHAR));
        } else {
            RegDeleteValueW(hKey, APP_NAME);
        }
        RegCloseKey(hKey);
    }
}

bool IsAutoStart() {
    HKEY hKey;
    const wchar_t *keyPath = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
    bool exists = false;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, keyPath, 0, KEY_QUERY_VALUE, &hKey) == ERROR_SUCCESS) {
        if (RegQueryValueExW(hKey, APP_NAME, NULL, NULL, NULL, NULL) == ERROR_SUCCESS) {
            exists = true;
        }
        RegCloseKey(hKey);
    }
    return exists;
}

// HTTP 请求封装
std::string SendRequest(const std::wstring &path, const std::string &method, const std::string &body) {
    std::string response;
    HINTERNET hSession = WinHttpOpen(L"MathQuizLite/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME,
                                     WINHTTP_NO_PROXY_BYPASS, 0);
    if (hSession) {
        DWORD protocols = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2 | WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3;
        WinHttpSetOption(hSession, WINHTTP_OPTION_SECURE_PROTOCOLS, &protocols, sizeof(protocols));

        HINTERNET hConnect = WinHttpConnect(hSession, API_HOST.c_str(), INTERNET_DEFAULT_HTTPS_PORT, 0);
        if (hConnect) {
            HINTERNET hRequest = WinHttpOpenRequest(hConnect, ToWide(method).c_str(), path.c_str(), NULL,
                                                    WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                                    WINHTTP_FLAG_SECURE);
            if (hRequest) {
                std::wstring headers = L"Content-Type: application/json\r\n";
                if (WinHttpSendRequest(hRequest, headers.c_str(), -1L, (LPVOID) body.c_str(), body.size(), body.size(),
                                       0)) {
                    if (WinHttpReceiveResponse(hRequest, NULL)) {
                        DWORD dwSize = 0;
                        DWORD dwDownloaded = 0;
                        do {
                            dwSize = 0;
                            if (!WinHttpQueryDataAvailable(hRequest, &dwSize)) break;
                            if (dwSize == 0) break;
                            std::vector<char> buffer(dwSize + 1);
                            if (WinHttpReadData(hRequest, &buffer[0], dwSize, &dwDownloaded)) {
                                response.append(buffer.data(), dwDownloaded);
                            }
                        } while (dwSize > 0);
                    }
                }
                WinHttpCloseHandle(hRequest);
            }
            WinHttpCloseHandle(hConnect);
        }
        WinHttpCloseHandle(hSession);
    }
    return response;
}

// --- 业务逻辑 ---

void LoadSettings() {
    WCHAR path[MAX_PATH];
    GetModuleFileNameW(NULL, path, MAX_PATH);
    PathRemoveFileSpecW(path);
    PathAppendW(path, SETTINGS_FILE.c_str());

    // 读取用户ID和用户名 (缓存)
    g_UserId = GetPrivateProfileIntW(L"Auth", L"UserId", 0, path);
    WCHAR buf[256];
    GetPrivateProfileStringW(L"Auth", L"Username", L"", buf, 256, path);
    g_Username = buf;

    // 读取保存的凭证 (用于自动登录)
    GetPrivateProfileStringW(L"Auth", L"Email", L"", buf, 256, path);
    g_SavedEmail = buf;

    WCHAR passBuf[1024];
    GetPrivateProfileStringW(L"Auth", L"Pass", L"", passBuf, 1024, path);
    // 解密密码
    g_SavedPass = DecryptString(passBuf);
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

    if (savePass) {
        std::wstring encrypted = EncryptString(pass);
        WritePrivateProfileStringW(L"Auth", L"Pass", encrypted.c_str(), path);
    } else {
        WritePrivateProfileStringW(L"Auth", L"Pass", NULL, path); // 删除密码
    }
}

// 同步数据 (GET)
void SyncData() {
    if (g_UserId == 0) return;

    std::string todoJson = SendRequest(L"/api/todos?user_id=" + std::to_wstring(g_UserId), "GET", "");
    if (!todoJson.empty()) {
        try {
            auto j = json::parse(todoJson);
            g_Todos.clear();
            for (const auto &item: j) {
                bool done = false;
                if (item["is_completed"].is_boolean()) done = item["is_completed"];
                else if (item["is_completed"].is_number()) done = item["is_completed"].get<int>() == 1;
                if (!done) {
                    g_Todos.push_back({item["id"].get<int>(), ToWide(item["content"]), done});
                }
            }
        } catch (...) {
        }
    }

    std::string countJson = SendRequest(L"/api/countdowns?user_id=" + std::to_wstring(g_UserId), "GET", "");
    if (!countJson.empty()) {
        try {
            auto j = json::parse(countJson);
            g_Countdowns.clear();
            for (const auto &item: j) {
                std::string dateRaw = item.contains("target_time") ? item["target_time"] : item["date"];
                std::wstring dateW = ToWide(dateRaw);
                g_Countdowns.push_back({item["id"].get<int>(), ToWide(item["title"]), dateW.substr(0, 10), 0});
            }
        } catch (...) {
        }
    }

    if (g_hWidgetWnd) InvalidateRect(g_hWidgetWnd, NULL, TRUE);
}

// API 操作封装 (Add/Toggle/Delete Todo/Countdown) 保持不变...
void ApiAddTodo(const std::wstring &content) {
    if (content.empty()) return;
    json j;
    j["user_id"] = g_UserId;
    j["content"] = ToUtf8(content);
    SendRequest(L"/api/todos", "POST", j.dump());
    SyncData();
}

void ApiToggleTodo(int id, bool currentStatus) {
    json j;
    j["id"] = id;
    j["is_completed"] = !currentStatus;
    SendRequest(L"/api/todos/toggle", "POST", j.dump());
    SyncData();
}

void ApiDeleteTodo(int id) {
    json j;
    j["id"] = id;
    SendRequest(L"/api/todos", "DELETE", j.dump());
    SyncData();
}

void ApiAddCountdown(const std::wstring &title, const std::wstring &dateStr) {
    if (title.empty()) return;
    std::wstring isoTime = dateStr + L"T00:00:00Z";
    json j;
    j["user_id"] = g_UserId;
    j["title"] = ToUtf8(title);
    j["target_time"] = ToUtf8(isoTime);
    SendRequest(L"/api/countdowns", "POST", j.dump());
    SyncData();
}

void ApiDeleteCountdown(int id) {
    json j;
    j["id"] = id;
    SendRequest(L"/api/countdowns", "DELETE", j.dump());
    SyncData();
}

// --- 窗口绘制 (保持不变) ---
void DrawWidget(HDC hdc, int w, int h) {
    g_HitZones.clear();
    Graphics graphics(hdc);
    graphics.SetSmoothingMode(SmoothingModeAntiAlias);
    graphics.SetTextRenderingHint(TextRenderingHintClearTypeGridFit);

    GraphicsPath path;
    int r = 15;
    path.AddArc(0, 0, r * 2, r * 2, 180, 90);
    path.AddArc(w - r * 2, 0, r * 2, r * 2, 270, 90);
    path.AddArc(w - r * 2, h - r * 2, r * 2, r * 2, 0, 90);
    path.AddArc(0, h - r * 2, r * 2, r * 2, 90, 90);
    path.CloseFigure();

    SolidBrush bgBrush(Color(200, 20, 20, 20));
    graphics.FillPath(&bgBrush, &path);

    FontFamily fontFamily(L"Microsoft YaHei");
    Font titleFont(&fontFamily, 16, FontStyleBold, UnitPixel);
    SolidBrush whiteBrush(Color(255, 255, 255, 255));

    std::wstring title = L"Hello, " + g_Username;
    graphics.DrawString(title.c_str(), -1, &titleFont, PointF(15, 15), &whiteBrush);

    Pen linePen(Color(100, 255, 255, 255), 1);
    graphics.DrawLine(&linePen, 15, 45, w - 15, 45);

    Font contentFont(&fontFamily, 14, FontStyleRegular, UnitPixel);
    Font headerFont(&fontFamily, 12, FontStyleRegular, UnitPixel);
    SolidBrush grayBrush(Color(200, 200, 200, 200));
    SolidBrush greenBrush(Color(255, 0, 200, 0));

    float y = 55;

    // 倒计时
    graphics.DrawString(L"倒计时", -1, &headerFont, PointF(15, y), &grayBrush);
    RectF addCntRect(w - 40, y, 20, 15);
    graphics.DrawString(L"[+]", -1, &headerFont, PointF(w - 40, y), &greenBrush);
    g_HitZones.push_back({
        Rect((int) addCntRect.X, (int) addCntRect.Y, (int) addCntRect.Width, (int) addCntRect.Height), 0, 2
    });

    y += 20;
    for (const auto &item: g_Countdowns) {
        RectF itemRect(15, y, (float) w - 30, 20);
        g_HitZones.push_back({
            Rect((int) itemRect.X, (int) itemRect.Y, (int) itemRect.Width, (int) itemRect.Height), item.id, 4
        });
        graphics.DrawString(item.title.c_str(), -1, &contentFont, PointF(15, y), &whiteBrush);
        graphics.DrawString(item.dateStr.c_str(), -1, &contentFont, PointF(w - 100, y), &whiteBrush);
        y += 20;
    }
    y += 10;

    // 待办
    graphics.DrawString(L"待办事项", -1, &headerFont, PointF(15, y), &grayBrush);
    RectF addTodoRect(w - 40, y, 20, 15);
    graphics.DrawString(L"[+]", -1, &headerFont, PointF(w - 40, y), &greenBrush);
    g_HitZones.push_back({
        Rect((int) addTodoRect.X, (int) addTodoRect.Y, (int) addTodoRect.Width, (int) addTodoRect.Height), 0, 1
    });

    y += 20;
    for (const auto &item: g_Todos) {
        RectF itemRect(15, y, (float) w - 30, 20);
        g_HitZones.push_back({
            Rect((int) itemRect.X, (int) itemRect.Y, (int) itemRect.Width, (int) itemRect.Height), item.id, 3
        });
        graphics.DrawRectangle(&linePen, 15, (int) y + 4, 10, 10);
        if (item.isDone) graphics.FillRectangle(&whiteBrush, 17, (int) y + 6, 6, 6);
        graphics.DrawString(item.content.c_str(), -1, &contentFont, PointF(30, y), &whiteBrush);
        y += 20;
    }
}

// --- 通用输入对话框 ---
std::wstring g_InputResult1;
std::wstring g_InputResult2;
bool g_InputOk = false;

LRESULT CALLBACK InputDlgProc(HWND hDlg, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
        case WM_INITDIALOG: return TRUE;
        case WM_COMMAND:
            if (LOWORD(wParam) == IDOK) {
                WCHAR buf1[256], buf2[256];
                GetDlgItemTextW(hDlg, 101, buf1, 256);
                GetDlgItemTextW(hDlg, 102, buf2, 256);
                g_InputResult1 = buf1;
                g_InputResult2 = buf2;
                g_InputOk = true;
                EndDialog(hDlg, IDOK);
            } else if (LOWORD(wParam) == IDCANCEL) EndDialog(hDlg, IDCANCEL);
            break;
    }
    return FALSE;
}

bool ShowInputDialog(HWND parent, int type, std::wstring &out1, std::wstring &out2) {
    g_InputOk = false;
    const wchar_t CLASS_NAME[] = L"InputDlgClass";
    WNDCLASSW wc = {0};
    wc.lpfnWndProc = DefWindowProc;
    wc.hInstance = g_hInst;
    wc.lpszClassName = CLASS_NAME;
    wc.hbrBackground = (HBRUSH) (COLOR_WINDOW + 1);
    RegisterClassW(&wc);
    HWND hDlg = CreateWindowW(CLASS_NAME, type == 0 ? L"添加待办" : L"添加倒计时",
                              WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_VISIBLE, CW_USEDEFAULT, CW_USEDEFAULT, 300,
                              type == 0 ? 150 : 200, parent, NULL, g_hInst, NULL);
    CreateWindowW(L"STATIC", type == 0 ? L"内容:" : L"标题:", WS_VISIBLE | WS_CHILD, 20, 20, 50, 20, hDlg, NULL, g_hInst,
                  NULL);
    HWND hEdit1 = CreateWindowW(L"EDIT", L"", WS_VISIBLE | WS_CHILD | WS_BORDER, 80, 20, 180, 20, hDlg, (HMENU)101,
                                g_hInst, NULL);
    HWND hEdit2 = NULL;
    if (type == 1) {
        CreateWindowW(L"STATIC", L"日期:", WS_VISIBLE | WS_CHILD, 20, 50, 50, 20, hDlg, NULL, g_hInst, NULL);
        SYSTEMTIME st;
        GetLocalTime(&st);
        wchar_t dateBuf[20];
        wsprintfW(dateBuf, L"%04d-%02d-%02d", st.wYear, st.wMonth, st.wDay);
        hEdit2 = CreateWindowW(L"EDIT", dateBuf, WS_VISIBLE | WS_CHILD | WS_BORDER, 80, 50, 180, 20, hDlg, (HMENU)102,
                               g_hInst, NULL);
    }
    int btnY = type == 0 ? 60 : 100;
    HWND hBtn = CreateWindowW(L"BUTTON", L"确定", WS_VISIBLE | WS_CHILD | BS_DEFPUSHBUTTON, 100, btnY, 80, 30, hDlg,
                              (HMENU)IDOK, g_hInst, NULL);
    EnableWindow(parent, FALSE);
    MSG msg;
    while (GetMessage(&msg, NULL, 0, 0)) {
        if (msg.hwnd == hBtn && msg.message == WM_LBUTTONUP)
            SendMessage(hDlg, WM_COMMAND, IDOK, 0);
        if (msg.message == WM_KEYDOWN && msg.wParam == VK_RETURN)
            SendMessage(hDlg, WM_COMMAND, IDOK, 0);
        if (msg.hwnd == hDlg && msg.message == WM_COMMAND && LOWORD(msg.wParam) == IDOK) {
            WCHAR buf[256];
            GetWindowTextW(hEdit1, buf, 256);
            g_InputResult1 = buf;
            if (hEdit2) {
                GetWindowTextW(hEdit2, buf, 256);
                g_InputResult2 = buf;
            }
            g_InputOk = !g_InputResult1.empty();
            DestroyWindow(hDlg);
        }
        TranslateMessage(&msg);
        DispatchMessage(&msg);
        if (!IsWindow(hDlg)) break;
    }
    EnableWindow(parent, TRUE);
    SetForegroundWindow(parent);
    out1 = g_InputResult1;
    out2 = g_InputResult2;
    return g_InputOk;
}

// --- 桌面微件窗口过程 ---
LRESULT CALLBACK WidgetWndProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam) {
    static int width = 300;
    static int height = 500;
    switch (message) {
        case WM_CREATE:
            SetTimer(hWnd, 1, 30000, NULL);
            CreateThread(NULL, 0, [](LPVOID) -> DWORD {
                SyncData();
                return 0;
            }, NULL, 0, NULL);
            break;
        case WM_TIMER:
            CreateThread(NULL, 0, [](LPVOID) -> DWORD {
                SyncData();
                return 0;
            }, NULL, 0, NULL);
            break;
        case WM_LBUTTONDOWN: {
            int x = LOWORD(lParam);
            int y = HIWORD(lParam);
            bool hit = false;
            for (const auto &zone: g_HitZones) {
                if (zone.rect.Contains(x, y)) {
                    hit = true;
                    if (zone.type == 1) {
                        // Add Todo
                        std::wstring content, dummy;
                        if (ShowInputDialog(hWnd, 0, content, dummy)) CreateThread(NULL, 0, [](LPVOID p) -> DWORD {
                            std::wstring *s = (std::wstring *) p;
                            ApiAddTodo(*s);
                            delete s;
                            return 0;
                        }, new std::wstring(content), 0, NULL);
                    } else if (zone.type == 2) {
                        // Add Countdown
                        std::wstring title, date;
                        if (ShowInputDialog(hWnd, 1, title, date)) {
                            struct Ctx {
                                std::wstring t, d;
                            };
                            CreateThread(NULL, 0, [](LPVOID p) -> DWORD {
                                Ctx *c = (Ctx *) p;
                                ApiAddCountdown(c->t, c->d);
                                delete c;
                                return 0;
                            }, new Ctx{title, date}, 0, NULL);
                        }
                    } else if (zone.type == 3) {
                        // Toggle Todo
                        bool currentStatus = false;
                        for (auto &t: g_Todos) if (t.id == zone.id) currentStatus = t.isDone;
                        CreateThread(NULL, 0, [](LPVOID p) -> DWORD {
                            ApiToggleTodo((int) (uintptr_t) p >> 1, (int) (uintptr_t) p & 1);
                            return 0;
                        }, (LPVOID) ((uintptr_t) (zone.id << 1 | (currentStatus ? 1 : 0))), 0, NULL);
                    }
                    break;
                }
            }
            if (!hit)
                SendMessage(hWnd, WM_NCLBUTTONDOWN, HTCAPTION, 0);
        }
        break;
        case WM_RBUTTONUP: {
            int x = LOWORD(lParam);
            int y = HIWORD(lParam);
            POINT pt;
            GetCursorPos(&pt);
            HMENU hMenu = CreatePopupMenu();
            int selectedId = 0;
            int selectedType = 0;
            for (const auto &zone: g_HitZones) {
                if (zone.rect.Contains(x, y)) {
                    if (zone.type == 3 || zone.type == 4) {
                        selectedId = zone.id;
                        selectedType = zone.type;
                        AppendMenuW(hMenu, MF_STRING, 1001, L"删除此项");
                        AppendMenuW(hMenu, MF_SEPARATOR, 0, NULL);
                    }
                    break;
                }
            }
            AppendMenuW(hMenu, MF_STRING, 1002, L"立即刷新");
            if (IsAutoStart()) AppendMenuW(hMenu, MF_STRING | MF_CHECKED, 1003, L"开机自启动");
            else AppendMenuW(hMenu, MF_STRING, 1003, L"开机自启动");
            AppendMenuW(hMenu, MF_SEPARATOR, 0, NULL);
            AppendMenuW(hMenu, MF_STRING, 1004, L"退出程序");
            int cmd = TrackPopupMenu(hMenu, TPM_RETURNCMD | TPM_TOPALIGN | TPM_LEFTALIGN, pt.x, pt.y, 0, hWnd, NULL);
            if (cmd == 1001) {
                if (selectedType == 3) CreateThread(NULL, 0, [](LPVOID p) -> DWORD {
                    ApiDeleteTodo((int) (uintptr_t) p);
                    return 0;
                }, (LPVOID) (uintptr_t) selectedId, 0, NULL);
                if (selectedType == 4) CreateThread(NULL, 0, [](LPVOID p) -> DWORD {
                    ApiDeleteCountdown((int) (uintptr_t) p);
                    return 0;
                }, (LPVOID) (uintptr_t) selectedId, 0, NULL);
            } else if (cmd == 1002) CreateThread(NULL, 0, [](LPVOID) -> DWORD {
                SyncData();
                return 0;
            }, NULL, 0, NULL);
            else if (cmd == 1003) SetAutoStart(!IsAutoStart());
            else if (cmd == 1004) PostQuitMessage(0);
            DestroyMenu(hMenu);
        }
        break;
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC hdc = BeginPaint(hWnd, &ps);
            HDC memDC = CreateCompatibleDC(hdc);
            HBITMAP memBitmap = CreateCompatibleBitmap(hdc, width, height);
            SelectObject(memDC, memBitmap);
            Graphics graphics(memDC);
            graphics.Clear(Color(0, 0, 0, 0));
            DrawWidget(memDC, width, height);
            BitBlt(hdc, 0, 0, width, height, memDC, 0, 0, SRCCOPY);
            DeleteObject(memBitmap);
            DeleteDC(memDC);
            EndPaint(hWnd, &ps);
        }
        break;
        case WM_DESTROY: PostQuitMessage(0);
            break;
        default: return DefWindowProc(hWnd, message, wParam, lParam);
    }
    return 0;
}

// --- 登录窗口过程 ---
LRESULT CALLBACK LoginWndProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
        case WM_COMMAND:
            if (LOWORD(wParam) == IDOK) {
                WCHAR email[100], pass[100];
                GetWindowTextW(g_hEmail, email, 100);
                GetWindowTextW(g_hPass, pass, 100);
                bool remember = SendMessage(g_hAutoLogin, BM_GETCHECK, 0, 0) == BST_CHECKED;

                if (lstrlenW(email) == 0 || lstrlenW(pass) == 0) {
                    MessageBoxW(hWnd, L"请输入邮箱和密码", L"提示", MB_OK);
                    return 0;
                }

                HCURSOR hOldCursor = SetCursor(LoadCursor(NULL, IDC_WAIT));
                std::string body = "{\"email\":\"" + ToUtf8(email) + "\",\"password\":\"" + ToUtf8(pass) + "\"}";
                std::string resp = SendRequest(L"/api/auth/login", "POST", body);
                SetCursor(hOldCursor);

                if (resp.empty()) {
                    MessageBoxW(hWnd, L"网络请求失败或连接超时", L"Login Error", MB_OK | MB_ICONERROR);
                    return 0;
                }

                try {
                    auto j = json::parse(resp);
                    if (j["success"].get<bool>()) {
                        int uid = j["user"]["id"];
                        std::string uname = j["user"]["username"];
                        // 保存配置：如果勾选Remember则保存密码，否则只保存空密码占位（或删除）
                        SaveSettings(uid, ToWide(uname), email, remember ? pass : L"", remember);

                        g_UserId = uid;
                        g_Username = ToWide(uname);
                        g_LoginSuccess = true;
                        DestroyWindow(hWnd);
                    } else {
                        std::string errMsg = j.contains("message") ? j["message"] : "登录失败";
                        MessageBoxW(hWnd, ToWide(errMsg).c_str(), L"Login Failed", MB_OK | MB_ICONWARNING);
                    }
                } catch (...) { MessageBoxW(hWnd, L"响应解析失败", L"Error", MB_OK | MB_ICONERROR); }
            }
            break;
        case WM_CLOSE: DestroyWindow(hWnd);
            break;
        case WM_DESTROY: PostQuitMessage(0);
            break;
        default: return DefWindowProc(hWnd, message, wParam, lParam);
    }
    return 0;
}

// 自动登录
bool AttemptAutoLogin() {
    if (g_SavedEmail.empty() || g_SavedPass.empty()) return false;

    std::string body = "{\"email\":\"" + ToUtf8(g_SavedEmail) + "\",\"password\":\"" + ToUtf8(g_SavedPass) + "\"}";
    std::string resp = SendRequest(L"/api/auth/login", "POST", body);

    if (resp.empty()) return false;
    try {
        auto j = json::parse(resp);
        if (j["success"].get<bool>()) {
            g_UserId = j["user"]["id"];
            g_Username = ToWide(j["user"]["username"]);
            // 更新一下配置（可能改了用户名？）
            SaveSettings(g_UserId, g_Username, g_SavedEmail, g_SavedPass, true);
            return true;
        }
    } catch (...) {
    }
    return false;
}

bool ShowLogin() {
    const wchar_t CLASS_NAME[] = L"LoginClass";
    WNDCLASSW wc = {0};
    wc.lpfnWndProc = LoginWndProc;
    wc.hInstance = g_hInst;
    wc.lpszClassName = CLASS_NAME;
    wc.hbrBackground = (HBRUSH) (COLOR_WINDOW + 1);
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    RegisterClassW(&wc);

    // 居中
    int scrW = GetSystemMetrics(SM_CXSCREEN);
    int scrH = GetSystemMetrics(SM_CYSCREEN);
    HWND hWnd = CreateWindowW(CLASS_NAME, L"Math Quiz Login", WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_VISIBLE,
                              (scrW-300)/2, (scrH-220)/2, 300, 220, NULL, NULL, g_hInst, NULL);

    CreateWindowW(L"STATIC", L"Email:", WS_VISIBLE | WS_CHILD, 20, 20, 60, 20, hWnd, NULL, g_hInst, NULL);
    g_hEmail = CreateWindowW(L"EDIT", g_SavedEmail.c_str(), WS_VISIBLE | WS_CHILD | WS_BORDER, 80, 20, 180, 20, hWnd,
                             NULL, g_hInst, NULL);

    CreateWindowW(L"STATIC", L"Pass:", WS_VISIBLE | WS_CHILD, 20, 50, 60, 20, hWnd, NULL, g_hInst, NULL);
    g_hPass = CreateWindowW(L"EDIT", L"", WS_VISIBLE | WS_CHILD | WS_BORDER | ES_PASSWORD, 80, 50, 180, 20, hWnd, NULL,
                            g_hInst, NULL);

    // 复选框
    g_hAutoLogin = CreateWindowW(L"BUTTON", L"自动登录 / 记住密码", WS_VISIBLE | WS_CHILD | BS_AUTOCHECKBOX, 80, 80, 180, 20,
                                 hWnd, (HMENU)103, g_hInst, NULL);
    // 默认勾选
    SendMessage(g_hAutoLogin, BM_SETCHECK, BST_CHECKED, 0);

    CreateWindowW(L"BUTTON", L"Login", WS_VISIBLE | WS_CHILD | BS_DEFPUSHBUTTON, 100, 120, 80, 30, hWnd, (HMENU)IDOK,
                  g_hInst, NULL);

    MSG msg;
    while (GetMessage(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }
    return g_LoginSuccess;
}

int APIENTRY WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow) {
    g_hInst = hInstance;
    GdiplusStartupInput gdiplusStartupInput;
    ULONG_PTR gdiplusToken;
    GdiplusStartup(&gdiplusToken, &gdiplusStartupInput, NULL);

    LoadSettings();

    // 尝试自动登录
    if (!AttemptAutoLogin()) {
        // 失败则显示登录窗
        if (!ShowLogin()) {
            GdiplusShutdown(gdiplusToken);
            return 0;
        }
    }

    const wchar_t CLASS_NAME[] = L"MathQuizWidgetClass";
    WNDCLASSW wc = {0};
    wc.lpfnWndProc = WidgetWndProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = CLASS_NAME;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH) GetStockObject(BLACK_BRUSH);
    RegisterClassW(&wc);

    g_hWidgetWnd = CreateWindowExW(WS_EX_LAYERED | WS_EX_TOOLWINDOW, CLASS_NAME, L"Math Widget", WS_POPUP,
                                   GetSystemMetrics(SM_CXSCREEN) - 320, 100, 300, 500, NULL, NULL, hInstance, NULL);
    SetLayeredWindowAttributes(g_hWidgetWnd, RGB(0, 0, 0), 200, LWA_COLORKEY | LWA_ALPHA);
    ShowWindow(g_hWidgetWnd, nCmdShow);
    UpdateWindow(g_hWidgetWnd);
    SetWindowPos(g_hWidgetWnd, HWND_BOTTOM, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);

    MSG msg;
    while (GetMessage(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    GdiplusShutdown(gdiplusToken);
    return (int) msg.wParam;
}
