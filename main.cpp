#include "common.h"
#include "utils.h"
#include "api.h"
#include "ui.h"
#include "tai_reader.h"
#include <thread>
#include <shellapi.h>
#include <winhttp.h>

#pragma comment(lib, "winhttp.lib")

// --- 自动更新模块 ---
const int CURRENT_VERSION_CODE = 0; // 当前内部版本号，为了测试弹窗这里默认为0，后续发布请调成与JSON对应的值

// 专用于获取更新配置的独立 HTTP 请求（避免影响你原有的 api.h 逻辑）
std::string SendUpdateCheckRequest(const std::wstring& host, const std::wstring& path) {
    std::string res;
    HINTERNET hSession = WinHttpOpen(L"MathQuizLite/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (hSession) {
        // 启用 TLS 1.2 等现代安全协议
        DWORD secureProtocols = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2 | WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3;
        WinHttpSetOption(hSession, WINHTTP_OPTION_SECURE_PROTOCOLS, &secureProtocols, sizeof(secureProtocols));

        HINTERNET hConnect = WinHttpConnect(hSession, host.c_str(), INTERNET_DEFAULT_HTTPS_PORT, 0);
        if (hConnect) {
            HINTERNET hRequest = WinHttpOpenRequest(hConnect, L"GET", path.c_str(), NULL, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
            if (hRequest) {
                if (WinHttpSendRequest(hRequest, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0)) {
                    if (WinHttpReceiveResponse(hRequest, NULL)) {
                        DWORD dwSize = 0, dwDownloaded = 0;
                        do {
                            if (!WinHttpQueryDataAvailable(hRequest, &dwSize) || dwSize == 0) break;
                            std::vector<char> buffer(dwSize);
                            if (WinHttpReadData(hRequest, buffer.data(), dwSize, &dwDownloaded)) {
                                res.append(buffer.data(), dwDownloaded);
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
    return res;
}

void CheckForUpdates() {
    // 放入后台线程，绝对不阻塞主程序的启动速度
    std::thread([]() {
        std::wstring host = L"raw.githubusercontent.com";
        std::wstring path = L"/Junpgle/CountDownTodoLite/refs/heads/master/update_manifest.json?token=GHSAT0AAAAAADOGIPAKUTFPZLF3JUBJKQOQ2NAMKOQ";

        std::string res = SendUpdateCheckRequest(host, path);
        if (!res.empty()) {
            try {
                auto j = json::parse(res);
                int remoteVersionCode = j["version_code"].get<int>();

                if (remoteVersionCode > CURRENT_VERSION_CODE) {
                    bool forceUpdate = false;
                    if (j.contains("force_update") && j["force_update"].is_boolean()) {
                        forceUpdate = j["force_update"].get<bool>();
                    }

                    std::wstring title = ToWide(j["update_info"]["title"].get<std::string>());
                    std::wstring desc = ToWide(j["update_info"]["description"].get<std::string>());
                    std::wstring url = ToWide(j["update_info"]["full_package_url"].get<std::string>());

                    std::wstring msg = desc + L"\n\n发现新版本，是否立即前往下载更新？";

                    UINT mbFlags = MB_ICONINFORMATION;
                    mbFlags |= forceUpdate ? MB_OK : MB_YESNO;

                    // 弹出置顶更新提示框
                    int btn = MessageBoxW(NULL, msg.c_str(), title.c_str(), mbFlags | MB_TOPMOST);
                    if (btn == IDYES || btn == IDOK) {
                        // 调起默认浏览器打开下载链接
                        ShellExecuteW(NULL, L"open", url.c_str(), NULL, NULL, SW_SHOWNORMAL);
                        // 如果强制更新，则退出当前应用
                        if (forceUpdate) exit(0);
                    }
                }
            } catch (...) {
                // JSON 解析错误或网络异常静默处理，不打扰用户
            }
        }
    }).detach();
}


int APIENTRY WinMain(HINSTANCE hI, HINSTANCE, LPSTR, int nC) {
    g_hInst = hI;

    // 初始化 DPI
    SetProcessDPIAware();
    HDC sc = GetDC(0);
    g_Scale = GetDeviceCaps(sc, LOGPIXELSY) / 96.0f;
    ReleaseDC(0, sc);

    // 初始化 GDI+
    Gdiplus::GdiplusStartupInput gsi;
    ULONG_PTR gt;
    Gdiplus::GdiplusStartup(&gt, &gsi, NULL);

    // 加载配置
    LoadSettings();

    // 尝试登录
    if (!AttemptAutoLogin()) {
        if (!ShowLogin()) {
            Gdiplus::GdiplusShutdown(gt);
            return 0;
        }
    }

    // --- 启动 Tai 数据库读取线程 ---
    StartTaiReader();

    // --- 异步检查更新 ---
    CheckForUpdates();

    // 注册和创建主小组件窗口
    WNDCLASSW wc = {0};
    wc.lpfnWndProc = WidgetWndProc;
    wc.hInstance = hI;
    wc.lpszClassName = L"MathWidget";
    wc.hCursor = LoadCursor(0, IDC_ARROW);
    wc.hbrBackground = (HBRUSH) GetStockObject(BLACK_BRUSH);
    RegisterClassW(&wc);
    g_hWidgetWnd = CreateWindowExW(
        WS_EX_LAYERED | WS_EX_TOOLWINDOW, L"MathWidget", L"Math",
        WS_POPUP,
        GetSystemMetrics(0) - S(320), S(100), S(300), S(500),
        0, 0, hI, 0
    );

    ShowWindow(g_hWidgetWnd, nC);
    UpdateWindow(g_hWidgetWnd);

    ResizeWidget();

    // 消息循环
    MSG m;
    while (GetMessage(&m, 0, 0, 0)) {
        TranslateMessage(&m);
        DispatchMessage(&m);
    }

    // 退出前停止 Tai 读取并清理
    StopTaiReader();
    Gdiplus::GdiplusShutdown(gt);

    return (int) m.wParam;
}