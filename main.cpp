#include "common.h"
#include "utils.h"
#include "api.h"
#include "ui.h"
#include "tai_reader.h"

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