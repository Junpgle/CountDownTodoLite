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

// --- 全局变量 ---
HINSTANCE g_hInst;
int g_UserId = 0;
std::wstring g_Username;
HWND g_hWidgetWnd = NULL;

// 登录界面控件句柄 (用于 LoginWndProc 访问)
HWND g_hEmail = NULL;
HWND g_hPass = NULL;
bool g_LoginSuccess = false;

// --- 数据结构 ---
struct Todo {
    std::wstring content;
    bool isDone;
};
struct Countdown {
    std::wstring title;
    std::wstring dateStr;
    int daysLeft;
};

std::vector<Todo> g_Todos;
std::vector<Countdown> g_Countdowns;

// --- 工具函数 ---

// 宽字符转UTF8
std::string ToUtf8(const std::wstring& wstr) {
    if (wstr.empty()) return std::string();
    int size_needed = WideCharToMultiByte(CP_UTF8, 0, &wstr[0], (int)wstr.size(), NULL, 0, NULL, NULL);
    std::string strTo(size_needed, 0);
    WideCharToMultiByte(CP_UTF8, 0, &wstr[0], (int)wstr.size(), &strTo[0], size_needed, NULL, NULL);
    return strTo;
}

// UTF8转宽字符
std::wstring ToWide(const std::string& str) {
    if (str.empty()) return std::wstring();
    int size_needed = MultiByteToWideChar(CP_UTF8, 0, &str[0], (int)str.size(), NULL, 0);
    std::wstring wstrTo(size_needed, 0);
    MultiByteToWideChar(CP_UTF8, 0, &str[0], (int)str.size(), &wstrTo[0], size_needed);
    return wstrTo;
}

// SHA256 哈希 (用于密码)
std::wstring Sha256(const std::wstring& input) {
    HCRYPTPROV hProv = 0;
    HCRYPTHASH hHash = 0;
    std::string utf8 = ToUtf8(input);

    if (!CryptAcquireContext(&hProv, NULL, NULL, PROV_RSA_AES, CRYPT_VERIFYCONTEXT)) return L"";
    if (!CryptCreateHash(hProv, CALG_SHA_256, 0, 0, &hHash)) {
        CryptReleaseContext(hProv, 0);
        return L"";
    }

    if (!CryptHashData(hHash, (BYTE*)utf8.c_str(), utf8.length(), 0)) {
        CryptDestroyHash(hHash);
        CryptReleaseContext(hProv, 0);
        return L"";
    }

    BYTE rgbHash[32];
    DWORD cbHash = 32;
    if (CryptGetHashParam(hHash, HP_HASHVAL, rgbHash, &cbHash, 0)) {
        std::wstringstream ss;
        for (DWORD i = 0; i < cbHash; i++) {
            ss << std::hex << std::setw(2) << std::setfill(L'0') << (int)rgbHash[i];
        }
        CryptDestroyHash(hHash);
        CryptReleaseContext(hProv, 0);
        return ss.str();
    }
    return L"";
}

// HTTP 请求封装
std::string SendRequest(const std::wstring& path, const std::string& method, const std::string& body) {
    std::string response;
    HINTERNET hSession = WinHttpOpen(L"MathQuizLite/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (hSession) {
        // 关键修复：强制开启 TLS 1.2 (Cloudflare 必需)
        DWORD protocols = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2 | WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3;
        WinHttpSetOption(hSession, WINHTTP_OPTION_SECURE_PROTOCOLS, &protocols, sizeof(protocols));

        HINTERNET hConnect = WinHttpConnect(hSession, API_HOST.c_str(), INTERNET_DEFAULT_HTTPS_PORT, 0);
        if (hConnect) {
            HINTERNET hRequest = WinHttpOpenRequest(hConnect, ToWide(method).c_str(), path.c_str(), NULL, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
            if (hRequest) {
                std::wstring headers = L"Content-Type: application/json\r\n";
                if (WinHttpSendRequest(hRequest, headers.c_str(), -1L, (LPVOID)body.c_str(), body.size(), body.size(), 0)) {
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

// --- 逻辑功能 ---

void LoadSettings() {
    WCHAR path[MAX_PATH];
    GetModuleFileNameW(NULL, path, MAX_PATH);
    PathRemoveFileSpecW(path);
    PathAppendW(path, SETTINGS_FILE.c_str());
    g_UserId = GetPrivateProfileIntW(L"Auth", L"UserId", 0, path);
    WCHAR buf[256];
    GetPrivateProfileStringW(L"Auth", L"Username", L"", buf, 256, path);
    g_Username = buf;
}

void SaveSettings(int uid, const std::wstring& name) {
    WCHAR path[MAX_PATH];
    GetModuleFileNameW(NULL, path, MAX_PATH);
    PathRemoveFileSpecW(path);
    PathAppendW(path, SETTINGS_FILE.c_str());
    WritePrivateProfileStringW(L"Auth", L"UserId", std::to_wstring(uid).c_str(), path);
    WritePrivateProfileStringW(L"Auth", L"Username", name.c_str(), path);
}

void SyncData() {
    if (g_UserId == 0) return;

    // 1. 获取待办
    std::string todoJson = SendRequest(L"/api/todos?user_id=" + std::to_wstring(g_UserId), "GET", "");
    if (!todoJson.empty()) {
        try {
            auto j = json::parse(todoJson);
            g_Todos.clear();
            for (const auto& item : j) {
                bool done = false;
                if(item["is_completed"].is_boolean()) done = item["is_completed"];
                else if(item["is_completed"].is_number()) done = item["is_completed"].get<int>() == 1;

                if (!done) { // 只显示未完成
                    g_Todos.push_back({ToWide(item["content"]), done});
                }
            }
        } catch (...) {}
    }

    // 2. 获取倒计时
    std::string countJson = SendRequest(L"/api/countdowns?user_id=" + std::to_wstring(g_UserId), "GET", "");
    if (!countJson.empty()) {
        try {
            auto j = json::parse(countJson);
            g_Countdowns.clear();
            SYSTEMTIME st;
            GetLocalTime(&st);
            for (const auto& item : j) {
                std::string dateRaw = item.contains("target_time") ? item["target_time"] : item["date"];
                std::wstring dateW = ToWide(dateRaw);
                g_Countdowns.push_back({ToWide(item["title"]), dateW.substr(0, 10), 0});
            }
        } catch (...) {}
    }

    // 刷新界面
    if (g_hWidgetWnd) InvalidateRect(g_hWidgetWnd, NULL, TRUE);
}

// --- 窗口绘制 ---
void DrawWidget(HDC hdc, int w, int h) {
    Graphics graphics(hdc);
    graphics.SetSmoothingMode(SmoothingModeAntiAlias);
    graphics.SetTextRenderingHint(TextRenderingHintClearTypeGridFit);

    // 1. 绘制半透明背景
    GraphicsPath path;
    int r = 15;
    path.AddArc(0, 0, r*2, r*2, 180, 90);
    path.AddArc(w - r*2, 0, r*2, r*2, 270, 90);
    path.AddArc(w - r*2, h - r*2, r*2, r*2, 0, 90);
    path.AddArc(0, h - r*2, r*2, r*2, 90, 90);
    path.CloseFigure();

    SolidBrush bgBrush(Color(200, 20, 20, 20));
    graphics.FillPath(&bgBrush, &path);

    // 2. 绘制标题
    FontFamily fontFamily(L"Microsoft YaHei");
    Font titleFont(&fontFamily, 16, FontStyleBold, UnitPixel);
    SolidBrush whiteBrush(Color(255, 255, 255, 255));

    std::wstring title = L"Hello, " + g_Username;
    graphics.DrawString(title.c_str(), -1, &titleFont, PointF(15, 15), &whiteBrush);

    Pen linePen(Color(100, 255, 255, 255), 1);
    graphics.DrawLine(&linePen, 15, 45, w - 15, 45);

    // 3. 绘制列表内容
    Font contentFont(&fontFamily, 14, FontStyleRegular, UnitPixel);
    float y = 55;

    // 倒计时
    if (!g_Countdowns.empty()) {
        Font headerFont(&fontFamily, 12, FontStyleRegular, UnitPixel);
        SolidBrush grayBrush(Color(200, 200, 200, 200));
        graphics.DrawString(L"倒计时", -1, &headerFont, PointF(15, y), &grayBrush);
        y += 20;

        for (const auto& item : g_Countdowns) {
            graphics.DrawString(item.title.c_str(), -1, &contentFont, PointF(15, y), &whiteBrush);
            graphics.DrawString(item.dateStr.c_str(), -1, &contentFont, PointF(w - 100, y), &whiteBrush);
            y += 20;
        }
        y += 10;
    }

    // 待办
    {
        Font headerFont(&fontFamily, 12, FontStyleRegular, UnitPixel);
        SolidBrush grayBrush(Color(200, 200, 200, 200));
        graphics.DrawString(L"待办事项", -1, &headerFont, PointF(15, y), &grayBrush);
        y += 20;

        for (const auto& item : g_Todos) {
            graphics.FillEllipse(&whiteBrush, 15, (int)y + 4, 4, 4);
            graphics.DrawString(item.content.c_str(), -1, &contentFont, PointF(25, y), &whiteBrush);
            y += 20;
        }
    }
}

// --- 桌面微件窗口过程 ---
LRESULT CALLBACK WidgetWndProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam) {
    static int width = 300;
    static int height = 500;

    switch (message) {
    case WM_CREATE:
        SetTimer(hWnd, 1, 30000, NULL);
        CreateThread(NULL, 0, [](LPVOID) -> DWORD { SyncData(); return 0; }, NULL, 0, NULL);
        break;
    case WM_TIMER:
        CreateThread(NULL, 0, [](LPVOID) -> DWORD { SyncData(); return 0; }, NULL, 0, NULL);
        break;
    case WM_LBUTTONDOWN:
        SendMessage(hWnd, WM_NCLBUTTONDOWN, HTCAPTION, 0);
        break;
    case WM_PAINT:
        {
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
    case WM_DESTROY:
        PostQuitMessage(0);
        break;
    default:
        return DefWindowProc(hWnd, message, wParam, lParam);
    }
    return 0;
}

// --- 登录窗口过程 (修复点击无反应问题) ---
LRESULT CALLBACK LoginWndProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
    case WM_COMMAND:
        // IDOK 是登录按钮的ID
        if (LOWORD(wParam) == IDOK) {
             WCHAR email[100], pass[100];
             GetWindowTextW(g_hEmail, email, 100);
             GetWindowTextW(g_hPass, pass, 100);

             if (lstrlenW(email) == 0 || lstrlenW(pass) == 0) {
                 MessageBoxW(hWnd, L"请输入邮箱和密码", L"提示", MB_OK);
                 return 0;
             }

             // 设置等待光标
             HCURSOR hOldCursor = SetCursor(LoadCursor(NULL, IDC_WAIT));

             std::string body = "{\"email\":\"" + ToUtf8(email) + "\",\"password\":\"" + ToUtf8(pass) + "\"}";
             std::string resp = SendRequest(L"/api/auth/login", "POST", body);

             // 恢复光标
             SetCursor(hOldCursor);

             if (resp.empty()) {
                 MessageBoxW(hWnd, L"网络请求失败或连接超时\n请检查网络连接", L"Login Error", MB_OK | MB_ICONERROR);
                 return 0;
             }

             try {
                 auto j = json::parse(resp);
                 if (j["success"].get<bool>()) {
                     int uid = j["user"]["id"];
                     std::string uname = j["user"]["username"];
                     SaveSettings(uid, ToWide(uname));
                     g_UserId = uid;
                     g_Username = ToWide(uname);
                     g_LoginSuccess = true;
                     DestroyWindow(hWnd); // 登录成功，关闭窗口
                 } else {
                     std::string errMsg = j.contains("message") ? j["message"] : "登录失败，请检查账号密码";
                     MessageBoxW(hWnd, ToWide(errMsg).c_str(), L"Login Failed", MB_OK | MB_ICONWARNING);
                 }
             } catch(...) {
                 MessageBoxW(hWnd, L"服务器响应解析失败", L"Error", MB_OK | MB_ICONERROR);
             }
        }
        break;
    case WM_CLOSE:
        DestroyWindow(hWnd);
        break;
    case WM_DESTROY:
        PostQuitMessage(0);
        break;
    default:
        return DefWindowProc(hWnd, message, wParam, lParam);
    }
    return 0;
}

// 显示登录窗口
bool ShowLogin() {
    const wchar_t CLASS_NAME[] = L"LoginClass";
    WNDCLASSW wc = { 0 };
    wc.lpfnWndProc = LoginWndProc; // 关键修复：使用专门的 LoginWndProc
    wc.hInstance = g_hInst;
    wc.lpszClassName = CLASS_NAME;
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    RegisterClassW(&wc);

    HWND hWnd = CreateWindowW(CLASS_NAME, L"Math Quiz Login",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_VISIBLE, // WS_VISIBLE 让窗口直接显示
        CW_USEDEFAULT, CW_USEDEFAULT, 300, 200, NULL, NULL, g_hInst, NULL);

    // 创建控件
    CreateWindowW(L"STATIC", L"Email:", WS_VISIBLE | WS_CHILD, 20, 20, 60, 20, hWnd, NULL, g_hInst, NULL);
    g_hEmail = CreateWindowW(L"EDIT", L"", WS_VISIBLE | WS_CHILD | WS_BORDER, 80, 20, 180, 20, hWnd, NULL, g_hInst, NULL);

    CreateWindowW(L"STATIC", L"Pass:", WS_VISIBLE | WS_CHILD, 20, 50, 60, 20, hWnd, NULL, g_hInst, NULL);
    g_hPass = CreateWindowW(L"EDIT", L"", WS_VISIBLE | WS_CHILD | WS_BORDER | ES_PASSWORD, 80, 50, 180, 20, hWnd, NULL, g_hInst, NULL);

    CreateWindowW(L"BUTTON", L"Login", WS_VISIBLE | WS_CHILD | BS_DEFPUSHBUTTON, 100, 100, 80, 30, hWnd, (HMENU)IDOK, g_hInst, NULL);

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

    if (g_UserId == 0) {
        if (!ShowLogin()) {
            GdiplusShutdown(gdiplusToken);
            return 0;
        }
    }

    const wchar_t CLASS_NAME[] = L"MathQuizWidgetClass";
    WNDCLASSW wc = { 0 };
    wc.lpfnWndProc = WidgetWndProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = CLASS_NAME;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    RegisterClassW(&wc);

    g_hWidgetWnd = CreateWindowExW(
        WS_EX_LAYERED | WS_EX_TOOLWINDOW,
        CLASS_NAME, L"Math Widget",
        WS_POPUP,
        GetSystemMetrics(SM_CXSCREEN) - 320, 100, 300, 500,
        NULL, NULL, hInstance, NULL
    );

    SetLayeredWindowAttributes(g_hWidgetWnd, RGB(0,0,0), 200, LWA_COLORKEY | LWA_ALPHA);

    ShowWindow(g_hWidgetWnd, nCmdShow);
    UpdateWindow(g_hWidgetWnd);

    // 置于底部
    SetWindowPos(g_hWidgetWnd, HWND_BOTTOM, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);

    MSG msg;
    while (GetMessage(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    GdiplusShutdown(gdiplusToken);
    return (int)msg.wParam;
}