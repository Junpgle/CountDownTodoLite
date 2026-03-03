#include "common.h"
#include "utils.h"
#include "api.h"
#include "ui.h"
#include "tai_reader.h"
#include <thread>
#include <shellapi.h>
#include <winhttp.h>
#include <objbase.h>

// 显式引入 JSON 类型别名，解决“无法解析符号”错误
using json = nlohmann::json;

#pragma comment(lib, "winhttp.lib")

// --- 自动更新模块 ---
// 当前内部版本号（每次发布新版本时递增，必须与服务器端 manifest 中的 version_code 对应）
const int CURRENT_VERSION_CODE = 7;

/**
 * 检查版本更新
 * @param isManual 是否为用户手动触发。手动触发时，若无更新或失败会弹出提示。
 */
void CheckForUpdates(bool isManual = false) {
    std::thread([isManual]() {
        // 1. 初始化 COM 环境，确保 ShellExecute 等组件在子线程可用
        HRESULT hr = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);

        bool checkProcessed = false; // 标记是否成功完成逻辑判定

        try {
            // 更新为用户提供的最新公开链接 (不带 token，永久有效)
            std::wstring host = L"raw.githubusercontent.com";
            std::wstring path = L"/Junpgle/CountDownTodoLite/refs/heads/master/update_manifest.json";

            std::string res;
            HINTERNET hSession = WinHttpOpen(L"MathQuizLite/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
            if (hSession) {
                // 强制启用 TLS 1.2/1.3 以确保与 GitHub 服务器的连接安全
                DWORD secureProtocols = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2 | WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3;
                WinHttpSetOption(hSession, WINHTTP_OPTION_SECURE_PROTOCOLS, &secureProtocols, sizeof(secureProtocols));

                HINTERNET hConnect = WinHttpConnect(hSession, host.c_str(), INTERNET_DEFAULT_HTTPS_PORT, 0);
                if (hConnect) {
                    HINTERNET hRequest = WinHttpOpenRequest(hConnect, L"GET", path.c_str(), NULL, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
                    if (hRequest) {
                        if (WinHttpSendRequest(hRequest, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0)) {
                            if (WinHttpReceiveResponse(hRequest, NULL)) {
                                // 检查 HTTP 状态码
                                DWORD dwStatusCode = 0;
                                DWORD dwSize = sizeof(dwStatusCode);
                                WinHttpQueryHeaders(hRequest, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                                                   WINHTTP_HEADER_NAME_BY_INDEX, &dwStatusCode, &dwSize, WINHTTP_NO_HEADER_INDEX);

                                if (dwStatusCode == 200) {
                                    DWORD dwDownloaded = 0;
                                    do {
                                        DWORD dwAvailable = 0;
                                        if (!WinHttpQueryDataAvailable(hRequest, &dwAvailable) || dwAvailable == 0) break;
                                        if (dwAvailable > 256 * 1024) break; // 配置文件不应超过 256KB

                                        std::vector<char> buffer(dwAvailable);
                                        if (WinHttpReadData(hRequest, buffer.data(), dwAvailable, &dwDownloaded)) {
                                            res.append(buffer.data(), dwDownloaded);
                                        }
                                    } while (dwDownloaded > 0);
                                } else {
                                    char buf[64];
                                    sprintf_s(buf, "Update Check: Server returned HTTP %d\n", dwStatusCode);
                                    OutputDebugStringA(buf);
                                    if (isManual) {
                                        MessageBoxW(NULL, L"无法连接到更新服务器，请检查网络设置。", L"检查更新", MB_OK | MB_ICONERROR | MB_TOPMOST);
                                        checkProcessed = true;
                                    }
                                }
                            }
                        }
                        WinHttpCloseHandle(hRequest);
                    }
                    WinHttpCloseHandle(hConnect);
                }
                WinHttpCloseHandle(hSession);
            }

            // 3. 安全解析 JSON 数据
            if (!res.empty()) {
                json j = json::parse(res);

                if (j.contains("version_code") && j["version_code"].is_number()) {
                    int remoteVersionCode = j["version_code"].get<int>();

                    // 调试输出
                    char debugBuf[128];
                    sprintf_s(debugBuf, "Update Check: Local %d, Remote %d\n", CURRENT_VERSION_CODE, remoteVersionCode);
                    OutputDebugStringA(debugBuf);

                    if (remoteVersionCode > CURRENT_VERSION_CODE) {
                        checkProcessed = true;
                        bool forceUpdate = false;
                        if (j.contains("force_update") && j["force_update"].is_boolean()) {
                            forceUpdate = j["force_update"].get<bool>();
                        }

                        std::wstring title = L"发现新版本";
                        std::wstring desc = L"";
                        std::wstring url = L"";

                        if (j.contains("update_info") && j["update_info"].is_object()) {
                            auto& info = j["update_info"];
                            if (info.contains("title") && info["title"].is_string()) {
                                title = ToWide(info["title"].get<std::string>());
                            }
                            if (info.contains("description") && info["description"].is_string()) {
                                desc = ToWide(info["description"].get<std::string>());
                            }
                            if (info.contains("full_package_url") && info["full_package_url"].is_string()) {
                                url = ToWide(info["full_package_url"].get<std::string>());
                            }
                        }

                        std::wstring msg = desc + L"\n\n是否立即前往下载更新？";
                        UINT mbFlags = MB_ICONINFORMATION | MB_TOPMOST;
                        mbFlags |= forceUpdate ? MB_OK : MB_YESNO;

                        int btn = MessageBoxW(NULL, msg.c_str(), title.c_str(), mbFlags);
                        if (btn == IDYES || btn == IDOK) {
                            if (!url.empty()) {
                                ShellExecuteW(NULL, L"open", url.c_str(), NULL, NULL, SW_SHOWNORMAL);
                            }
                            if (forceUpdate) {
                                if (g_hWidgetWnd) PostMessage(g_hWidgetWnd, WM_CLOSE, 0, 0);
                            }
                        }
                    } else {
                        // 版本已是最新
                        if (isManual && !checkProcessed) {
                            MessageBoxW(NULL, L"当前已是最新版本，无需更新。", L"检查更新", MB_OK | MB_ICONINFORMATION | MB_TOPMOST);
                            checkProcessed = true;
                        }
                    }
                }
            } else if (isManual && !checkProcessed) {
                MessageBoxW(NULL, L"获取更新信息失败，请稍后再试。", L"检查更新", MB_OK | MB_ICONWARNING | MB_TOPMOST);
            }
        } catch (const std::exception& e) {
            OutputDebugStringA("Update check exception: ");
            OutputDebugStringA(e.what());
            OutputDebugStringA("\n");
            if (isManual) MessageBoxW(NULL, L"解析更新配置时出错。", L"检查更新", MB_OK | MB_ICONERROR | MB_TOPMOST);
        } catch (...) {
            OutputDebugStringA("Unknown exception in update thread.\n");
            if (isManual) MessageBoxW(NULL, L"检查更新时发生未知错误。", L"检查更新", MB_OK | MB_ICONERROR | MB_TOPMOST);
        }

        if (SUCCEEDED(hr)) {
            CoUninitialize();
        }
    }).detach();
}


int APIENTRY WinMain(HINSTANCE hI, HINSTANCE, LPSTR, int nC) {
    g_hInst = hI;

    // 初始化 DPI 适配
    SetProcessDPIAware();
    HDC sc = GetDC(0);
    g_Scale = GetDeviceCaps(sc, LOGPIXELSY) / 96.0f;
    ReleaseDC(0, sc);

    // 初始化 GDI+ 环境
    Gdiplus::GdiplusStartupInput gsi;
    ULONG_PTR gt;
    Gdiplus::GdiplusStartup(&gt, &gsi, NULL);

    // 加载用户设置
    LoadSettings();

    // 尝试执行自动登录逻辑
    if (!AttemptAutoLogin()) {
        if (!ShowLogin()) {
            Gdiplus::GdiplusShutdown(gt);
            return 0;
        }
    }

    // --- 启动 Tai 数据读取线程 ---
    StartTaiReader();

    // 注册并创建主悬浮组件窗口
    WNDCLASSW wc = {0};
    wc.lpfnWndProc = WidgetWndProc;
    wc.hInstance = hI;
    wc.lpszClassName = L"MathWidget";
    wc.hCursor = LoadCursor(0, IDC_ARROW);
    wc.hbrBackground = (HBRUSH) GetStockObject(BLACK_BRUSH);
    RegisterClassW(&wc);

    g_hWidgetWnd = CreateWindowExW(
        WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_TOPMOST,
        L"MathWidget", L"Math",
        WS_POPUP,
        GetSystemMetrics(SM_CXSCREEN) - S(320), S(100), S(300), S(500),
        0, 0, hI, 0
    );

    if (g_hWidgetWnd) {
        ShowWindow(g_hWidgetWnd, nC);
        UpdateWindow(g_hWidgetWnd);
        ResizeWidget();

        // --- 修复：程序启动时立即进行一次云端数据同步 ---
        std::thread([]() {
            // 稍作延迟确保窗口句柄已经完全准备好接收刷新消息
            Sleep(500);
            SyncData();
        }).detach();

        // --- 执行异步更新检查 (默认自动静默检查) ---
        CheckForUpdates(false);
    }

    // 主消息循环
    MSG m;
    while (GetMessage(&m, 0, 0, 0)) {
        TranslateMessage(&m);
        DispatchMessage(&m);
    }

    // 退出资源清理
    StopTaiReader();
    Gdiplus::GdiplusShutdown(gt);

    return (int) m.wParam;
}