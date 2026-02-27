#include "ui.h"
#include "utils.h"
#include "api.h"
#include <commctrl.h> // 必须包含此头文件以使用日期选择器

using namespace Gdiplus;

// 外部声明
extern void CheckForUpdates(bool isManual = false);

// 前瞻声明
LRESULT CALLBACK LoginWndProc(HWND hWnd, UINT msg, WPARAM wp, LPARAM lp);
LRESULT CALLBACK InputWndProc(HWND hWnd, UINT msg, WPARAM wp, LPARAM lp);

// 内部辅助：字符串转 SYSTEMTIME (用于初始化选择器)
void StringToSystemTime(const std::wstring& dateStr, SYSTEMTIME& st) {
    GetLocalTime(&st);
    int y, m, d;
    if (swscanf(dateStr.c_str(), L"%d-%d-%d", &y, &m, &d) == 3) {
        st.wYear = (WORD)y;
        st.wMonth = (WORD)m;
        st.wDay = (WORD)d;
    }
}

// 内部辅助函数：计算总屏幕时长
int GetTotalScreenTimeInternal() {
    std::lock_guard<std::recursive_mutex> lock(g_DataMutex);
    int total = 0;
    for (const auto& rec : g_AppUsage) {
        total += rec.seconds;
    }
    return total;
}

// 计算进度条百分比 (0.0 - 1.0)
float CalculateTodoProgress(const std::wstring& start, const std::wstring& end) {
    if (start.empty() || end.empty()) return -1.0f;
    auto parse = [](const std::wstring& s) -> time_t {
        int y, m, d;
        if (swscanf(s.c_str(), L"%d-%d-%d", &y, &m, &d) != 3) return 0;
        struct tm t = {0};
        t.tm_year = y - 1900; t.tm_mon = m - 1; t.tm_mday = d;
        t.tm_isdst = -1;
        return mktime(&t);
    };
    time_t tStart = parse(start);
    time_t tEnd = parse(end);
    time_t tNow = time(nullptr);
    if (tStart >= tEnd) return 1.0f;
    if (tNow <= tStart) return 0.0f;
    if (tNow >= tEnd) return 1.0f;
    return (float)(tNow - tStart) / (float)(tEnd - tStart);
}

// 矢量图标绘制函数
void DrawDeviceIcon(Graphics& g, int type, REAL x, REAL y, REAL size, Brush* brush) {
    g.SetSmoothingMode(SmoothingModeAntiAlias);
    Pen whitePen(Color(200, 255, 255, 255), 1.0f);
    if (type == 0) { // Laptop
        g.FillRectangle(brush, x + size * 0.15f, y + size * 0.1f, size * 0.7f, size * 0.5f);
        GraphicsPath path;
        path.AddRectangle(RectF(x + size * 0.05f, y + size * 0.62f, size * 0.9f, size * 0.08f));
        path.AddRectangle(RectF(x + size * 0.4f, y + size * 0.7f, size * 0.2f, size * 0.05f));
        g.FillPath(brush, &path);
    }
    else if (type == 1) { // Phone
        GraphicsPath path;
        REAL r = size * 0.1f;
        path.AddArc(x + size * 0.25f, y, r, r, 180.0f, 90.0f);
        path.AddArc(x + size * 0.75f - r, y, r, r, 270.0f, 90.0f);
        path.AddArc(x + size * 0.75f - r, y + size * 0.9f - r, r, r, 0.0f, 90.0f);
        path.AddArc(x + size * 0.25f, y + size * 0.9f - r, r, r, 90.0f, 90.0f);
        path.CloseFigure();
        g.DrawPath(&whitePen, &path);
        g.FillRectangle(brush, x + size * 0.3f, y + size * 0.12f, size * 0.4f, size * 0.65f);
        g.FillEllipse(brush, x + size * 0.45f, y + size * 0.81f, size * 0.1f, size * 0.1f);
    }
    else if (type == 2) { // Tablet
        g.DrawRectangle(&whitePen, x + size * 0.15f, y, size * 0.7f, size * 0.9f);
        g.FillRectangle(brush, x + size * 0.22f, y + size * 0.08f, size * 0.56f, size * 0.74f);
    }
}

// 核心绘制逻辑
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
    bmi.bmiHeader.biWidth = width; bmi.bmiHeader.biHeight = -height;
    bmi.bmiHeader.biPlanes = 1; bmi.bmiHeader.biBitCount = 32; bmi.bmiHeader.biCompression = BI_RGB;
    void *pBits = nullptr;
    HBITMAP hBitmap = CreateDIBSection(hdcMem, &bmi, DIB_RGB_COLORS, &pBits, NULL, 0);
    HBITMAP hOld = (HBITMAP) SelectObject(hdcMem, hBitmap);

    {
        Graphics g(hdcMem);
        g.SetSmoothingMode(SmoothingModeAntiAlias);
        g.SetTextRenderingHint(TextRenderingHintAntiAliasGridFit);
        g.Clear(Color(0, 0, 0, 0));

        std::lock_guard<std::recursive_mutex> lock(g_DataMutex);
        g_HitZones.clear();

        REAL r = (REAL)S(15);
        GraphicsPath path;
        path.AddArc(0.0f, 0.0f, r * 2.0f, r * 2.0f, 180.0f, 90.0f);
        path.AddArc((REAL)width - r * 2.0f, 0.0f, r * 2.0f, r * 2.0f, 270.0f, 90.0f);
        path.AddArc((REAL)width - r * 2.0f, (REAL)height - r * 2.0f, r * 2.0f, r * 2.0f, 0.0f, 90.0f);
        path.AddArc(0.0f, (REAL)height - r * 2.0f, r * 2.0f, r * 2.0f, 90.0f, 90.0f);
        path.CloseFigure();

        SolidBrush bgBrush(Color(g_BgAlpha, 25, 25, 25));
        g.FillPath(&bgBrush, &path);

        FontFamily ff(L"MiSans");
        Font titleF(&ff, (REAL) S(16), FontStyleBold, UnitPixel);
        Font headF(&ff, (REAL) S(12), FontStyleRegular, UnitPixel);
        Font contentF(&ff, (REAL) S(14), FontStyleRegular, UnitPixel);
        Font dateF(&ff, (REAL) S(11), FontStyleRegular, UnitPixel);

        SolidBrush wBrush(Color(255, 255, 255, 255));
        SolidBrush gBrush(Color(255, 180, 180, 180));
        SolidBrush grBrush(Color(255, 80, 220, 80));
        SolidBrush rBrush(Color(255, 255, 100, 100));
        Pen linePen(Color(100, 255, 255, 255), 1);

        std::wstring welcomeMsg = L"你好, " + (g_Username.empty() ? L"用户" : g_Username);
        g.DrawString(welcomeMsg.c_str(), -1, &titleF, PointF((REAL) S(15), (REAL) S(15)), &wBrush);
        g.DrawLine(&linePen, (REAL)S(15), (REAL)S(45), (REAL)(width - S(15)), (REAL)S(45));

        float y = (float) S(55);

        // --- 倒计时板块 ---
        g.DrawString(L"倒计时", -1, &headF, PointF((REAL) S(15), y), &gBrush);
        g.DrawString(L"[+]", -1, &headF, PointF((REAL) (width - S(40)), y), &grBrush);
        g_HitZones.push_back({Rect(width - S(45), (int) y, S(30), S(20)), 0, 2});
        y += S(20);
        if (g_Countdowns.empty()) {
            g.DrawString(L"暂无倒计时", -1, &contentF, PointF((REAL) S(15), y), &gBrush);
            y += S(20);
        } else {
            for (const auto &it : g_Countdowns) {
                g_HitZones.push_back({Rect(S(15), (int) y, width - S(30), S(20)), it.id, 4});
                g.DrawString(it.title.c_str(), -1, &contentF, PointF((REAL) S(15), y), &wBrush);
                std::wstring dStr = std::to_wstring(it.daysLeft) + L" 天";
                g.DrawString(dStr.c_str(), -1, &contentF, PointF((REAL) (width - S(65)), y), (it.daysLeft <= 3) ? &rBrush : &grBrush);
                y += S(20);
            }
        }

        // --- 屏幕时间板块 ---
        y += S(10);
        int totalSec = GetTotalScreenTimeInternal();
        std::wstring totalStr = (totalSec < 3600) ? (std::to_wstring(totalSec/60) + L" m") : (std::to_wstring(totalSec/3600) + L" h " + std::to_wstring((totalSec%3600)/60) + L" m");
        g.DrawString(L"今日屏幕时间", -1, &headF, PointF((REAL) S(15), y), &gBrush);
        g.DrawString(totalStr.c_str(), -1, &headF, PointF((REAL) (width - S(90)), y), &wBrush);
        y += S(20);
        std::vector<AppUsageRecord> displayList = g_AppUsage;
        std::sort(displayList.begin(), displayList.end(), [](const auto& a, const auto& b) { return a.seconds > b.seconds; });
        if (displayList.size() > (size_t)g_TopAppsCount) displayList.resize(g_TopAppsCount);
        if (displayList.empty()) {
            g.DrawString(L"暂无数据", -1, &contentF, PointF((REAL) S(15), y), &gBrush);
            y += S(20);
        } else {
            for (const auto& rec : displayList) {
                int iconType = 0; std::wstring dn = rec.deviceName; for(auto &c : dn) c = towlower(c);
                if (dn.find(L"phone") != std::wstring::npos || dn.find(L"iphone") != std::wstring::npos || dn.find(L"android") != std::wstring::npos) iconType = 1;
                else if (dn.find(L"pad") != std::wstring::npos || dn.find(L"tablet") != std::wstring::npos) iconType = 2;
                DrawDeviceIcon(g, iconType, (REAL)S(15), y + S(2), (REAL)S(16), &wBrush);
                std::wstring nameDisp = rec.appName; if (nameDisp.length() > 14) nameDisp = nameDisp.substr(0, 12) + L"...";
                g.DrawString(nameDisp.c_str(), -1, &contentF, PointF((REAL) S(38), y), &wBrush);
                std::wstring tStr = (rec.seconds < 3600) ? (std::to_wstring(rec.seconds/60) + L"m") : (std::to_wstring(rec.seconds/3600) + L"h");
                g.DrawString(tStr.c_str(), -1, &contentF, PointF((REAL) (width - S(60)), y), &grBrush);
                y += S(20);
            }
        }

        // --- 待办事项板块 ---
        y += S(10);
        g.DrawString(L"待办事项", -1, &headF, PointF((REAL) S(15), y), &gBrush);
        g.DrawString(L"[+]", -1, &headF, PointF((REAL) (width - S(40)), y), &grBrush);
        g_HitZones.push_back({Rect(width - S(45), (int) y, S(30), S(20)), 0, 1});
        y += S(20);

        if (g_Todos.empty()) {
            g.DrawString(L"暂无待办", -1, &contentF, PointF((REAL) S(15), y), &gBrush);
        } else {
            for (const auto &it : g_Todos) {
                // 点击区域：分为主体(勾选/编辑)和右侧(删除)
                g_HitZones.push_back({Rect(S(15), (int) y, width - S(65), S(35)), it.id, 3});
                g_HitZones.push_back({Rect(width - S(45), (int) y, S(35), S(35)), it.id, 5}); // type 5 为删除待办

                // 1. 复选框
                g.DrawRectangle(&linePen, S(15), (int) y + S(6), S(12), S(12));
                if (it.isDone) g.FillRectangle(&wBrush, S(17), (int) y + S(8), S(8), S(8));

                // 2. 待办标题与截止日期 (同一行)
                int style = it.isDone ? FontStyleStrikeout : FontStyleRegular;
                Font itemF(&ff, (REAL) S(14), style, UnitPixel);
                g.DrawString(it.content.c_str(), -1, &itemF, PointF((REAL) S(32), y + S(3)), it.isDone ? &gBrush : &wBrush);

                // 绘制删除按钮 [-]
                g.DrawString(L"[-]", -1, &headF, PointF((REAL)(width - S(40)), y + S(5)), &rBrush);

                if (!it.dueDate.empty()) {
                    std::wstring shortDate = it.dueDate.substr(0, 10);
                    std::wstring dateLabel = L"截止: " + shortDate;

                    RectF layoutRect(0, 0, (REAL)width, (REAL)S(20));
                    RectF boundRect;
                    g.MeasureString(dateLabel.c_str(), -1, &dateF, layoutRect, &boundRect);

                    // 靠右绘制，留出删除按钮位置
                    float dateX = (float)width - boundRect.Width - S(50);
                    g.DrawString(dateLabel.c_str(), -1, &dateF, PointF(dateX, y + S(5)), &gBrush);

                    // 3. 进度条 (标题下方)
                    float progress = CalculateTodoProgress(it.createdDate, it.dueDate);
                    if (progress >= 0) {
                        SolidBrush barBg(Color(50, 255, 255, 255));
                        g.FillRectangle(&barBg, S(32), (int)y + S(24), width - S(90), S(4));
                        Color progressColor = (progress > 0.85f && !it.isDone) ? Color(255, 255, 100, 100) : Color(255, 80, 220, 80);
                        SolidBrush barFg(progressColor);
                        g.FillRectangle(&barFg, S(32), (int)y + S(24), (int)((width - S(90)) * progress), S(4));
                    }
                }
                y += S(35);
            }
        }
    }

    POINT ptSrc = {0, 0}; POINT ptDest = {rc.left, rc.top}; SIZE sz = {width, height};
    BLENDFUNCTION bf = {AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
    UpdateLayeredWindow(g_hWidgetWnd, hdcScreen, &ptDest, &sz, hdcMem, &ptSrc, 0, &bf, ULW_ALPHA);
    SelectObject(hdcMem, hOld); DeleteObject(hBitmap); DeleteDC(hdcMem); ReleaseDC(NULL, hdcScreen);
}

void ResizeWidget() {
    if (!g_hWidgetWnd) return;
    std::lock_guard<std::recursive_mutex> lock(g_DataMutex);
    int h = S(55);
    h += S(30) + (g_Countdowns.empty() ? S(20) : (int)g_Countdowns.size() * S(20));
    int appRows = std::min((int)g_AppUsage.size(), g_TopAppsCount);
    h += S(30) + (appRows == 0 ? S(20) : appRows * S(20));
    h += S(30) + (g_Todos.empty() ? S(20) : (int)g_Todos.size() * S(35));
    h += S(25); if (h < S(180)) h = S(180);
    RECT rc; GetWindowRect(g_hWidgetWnd, &rc);
    SetWindowPos(g_hWidgetWnd, HWND_BOTTOM, rc.left, rc.top, S(300), h, SWP_NOACTIVATE);
    RenderWidget();
}

LRESULT CALLBACK WidgetWndProc(HWND hWnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_LBUTTONDOWN: {
            int x = LOWORD(lp), y = HIWORD(lp); bool hit = false;
            std::vector<HitZone> zones; { std::lock_guard<std::recursive_mutex> lock(g_DataMutex); zones = g_HitZones; }
            for (const auto& z : zones) {
                if (z.rect.Contains(x, y)) {
                    hit = true;
                    if (z.type == 1) { // 添加
                        std::wstring c, d1, d2; if (ShowInputDialog(hWnd, 0, c, d1, d2)) { ApiAddTodo(c, d1, d2, false); SyncData(); }
                    }
                    else if (z.type == 2) { // 倒计时
                        std::wstring t, d, dummy; if (ShowInputDialog(hWnd, 1, t, d, dummy)) { ApiAddCountdown(t, d); SyncData(); }
                    }
                    else if (z.type == 3) { // 待办点击/编辑
                        if (x < S(30)) {
                            bool done = false; { std::lock_guard<std::recursive_mutex> l(g_DataMutex); for(auto &t:g_Todos) if(t.id==z.id) done=t.isDone; }
                            ApiToggleTodo(z.id, !done);
                        } else {
                            std::wstring c, d1, d2; bool currentDone = false;
                            { std::lock_guard<std::recursive_mutex> l(g_DataMutex); for(auto &t:g_Todos) if(t.id==z.id) { c=t.content; d1=t.createdDate; d2=t.dueDate; currentDone=t.isDone; } }
                            if (ShowInputDialog(hWnd, 0, c, d1, d2)) { ApiAddTodo(c, d1, d2, currentDone); }
                        }
                        SyncData();
                    }
                    else if (z.type == 4) { // 删除倒计时
                        if (MessageBoxW(hWnd, L"确定要删除吗？", L"确认", MB_YESNO) == IDYES) { ApiDeleteCountdown(z.id); SyncData(); }
                    }
                    else if (z.type == 5) { // 删除待办
                        if (MessageBoxW(hWnd, L"确定要删除这条待办吗？", L"确认删除", MB_YESNO | MB_ICONQUESTION) == IDYES) { ApiDeleteTodo(z.id); SyncData(); }
                    }
                    break;
                }
            }
            if (!hit) SendMessage(hWnd, WM_NCLBUTTONDOWN, HTCAPTION, 0);
        } break;
        case WM_RBUTTONUP: {
            POINT pt; GetCursorPos(&pt); HMENU hMenu = CreatePopupMenu(); HMENU hTopSub = CreatePopupMenu();
            AppendMenuW(hTopSub, MF_STRING | (g_TopAppsCount == 3 ? MF_CHECKED : 0), 3003, L"前 3");
            AppendMenuW(hTopSub, MF_STRING | (g_TopAppsCount == 5 ? MF_CHECKED : 0), 3005, L"前 5");
            AppendMenuW(hTopSub, MF_STRING | (g_TopAppsCount == 10 ? MF_CHECKED : 0), 3010, L"前 10");
            AppendMenuW(hMenu, MF_POPUP, (UINT_PTR)hTopSub, L"统计排名");
            AppendMenuW(hMenu, MF_SEPARATOR, 0, NULL);
            AppendMenuW(hMenu, 0, 1001, L"立即同步"); AppendMenuW(hMenu, 0, 1002, L"检查更新"); AppendMenuW(hMenu, 0, 1003, L"退出");
            int cmd = TrackPopupMenu(hMenu, TPM_RETURNCMD, pt.x, pt.y, 0, hWnd, NULL);
            if (cmd >= 3003 && cmd <= 3010) { g_TopAppsCount = (cmd == 3003) ? 3 : (cmd == 3005 ? 5 : 10); ResizeWidget(); }
            else if (cmd == 1001) SyncData(); else if (cmd == 1002) CheckForUpdates(true); else if (cmd == 1003) PostQuitMessage(0);
            DestroyMenu(hTopSub); DestroyMenu(hMenu);
        } break;
        case WM_USER_REFRESH: case WM_USER_TICK: ResizeWidget(); break;
        case WM_DESTROY: PostQuitMessage(0); break;
        default: return DefWindowProc(hWnd, msg, wp, lp);
    }
    return 0;
}

LRESULT CALLBACK InputWndProc(HWND hWnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_COMMAND && LOWORD(wp) == IDOK) {
        WCHAR content[512]; GetDlgItemTextW(hWnd, 101, content, 512);
        InputState::result1 = content;

        SYSTEMTIME st1, st2;
        DateTime_GetSystemtime(GetDlgItem(hWnd, 102), &st1);
        DateTime_GetSystemtime(GetDlgItem(hWnd, 103), &st2);

        wchar_t buf[64];
        swprintf_s(buf, L"%04d-%02d-%02d", st1.wYear, st1.wMonth, st1.wDay);
        InputState::result2 = buf;
        swprintf_s(buf, L"%04d-%02d-%02d", st2.wYear, st2.wMonth, st2.wDay);
        InputState::result3 = buf;

        InputState::isOk = true; DestroyWindow(hWnd);
    } else if (msg == WM_COMMAND && LOWORD(wp) == IDCANCEL) DestroyWindow(hWnd);
    return DefWindowProc(hWnd, msg, wp, lp);
}

bool ShowInputDialog(HWND parent, int type, std::wstring &o1, std::wstring &o2, std::wstring &o3) {
    InputState::isOk = false;
    WNDCLASSW wc = {0}; wc.lpfnWndProc = InputWndProc; wc.hInstance = GetModuleHandle(NULL);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1); wc.lpszClassName = L"InputDlg"; RegisterClassW(&wc);

    HWND hDlg = CreateWindowExW(0, L"InputDlg", type == 0 ? L"待办事项" : L"日期设置", WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU,
        100, 100, S(350), S(260), parent, NULL, NULL, NULL);

    HFONT hF = GetMiSansFont(14);

    CreateWindowW(L"STATIC", type == 0 ? L"内容:" : L"标题:", WS_CHILD | WS_VISIBLE, S(20), S(20), S(80), S(20), hDlg, NULL, NULL, NULL);
    CreateWindowW(L"EDIT", o1.c_str(), WS_CHILD | WS_VISIBLE | WS_BORDER, S(100), S(18), S(200), S(25), hDlg, (HMENU)101, NULL, NULL);

    CreateWindowW(L"STATIC", type == 0 ? L"开始日期:" : L"目标日期:", WS_CHILD | WS_VISIBLE, S(20), S(60), S(80), S(20), hDlg, NULL, NULL, NULL);
    HWND hPicker1 = CreateWindowExW(0, DATETIMEPICK_CLASS, L"", WS_BORDER | WS_CHILD | WS_VISIBLE | DTS_SHORTDATECENTURYFORMAT,
        S(100), S(58), S(200), S(25), hDlg, (HMENU)102, NULL, NULL);

    SYSTEMTIME stStart, stEnd;
    StringToSystemTime(o2.empty() ? GetTodayDate() : o2, stStart);
    DateTime_SetSystemtime(hPicker1, GDT_VALID, &stStart);

    if (type == 0) {
        CreateWindowW(L"STATIC", L"截止日期:", WS_CHILD | WS_VISIBLE, S(20), S(100), S(80), S(20), hDlg, NULL, NULL, NULL);
        HWND hPicker2 = CreateWindowExW(0, DATETIMEPICK_CLASS, L"", WS_BORDER | WS_CHILD | WS_VISIBLE | DTS_SHORTDATECENTURYFORMAT,
            S(100), S(98), S(200), S(25), hDlg, (HMENU)103, NULL, NULL);

        if (o3.empty()) {
            GetLocalTime(&stEnd);
            FILETIME ft; SystemTimeToFileTime(&stEnd, &ft);
            ULARGE_INTEGER uli; uli.LowPart = ft.dwLowDateTime; uli.HighPart = ft.dwHighDateTime;
            uli.QuadPart += (ULONGLONG)24 * 60 * 60 * 1000 * 10000;
            ft.dwLowDateTime = uli.LowPart; ft.dwHighDateTime = uli.HighPart;
            FileTimeToSystemTime(&ft, &stEnd);
        } else {
            StringToSystemTime(o3, stEnd);
        }
        DateTime_SetSystemtime(hPicker2, GDT_VALID, &stEnd);
    }

    CreateWindowW(L"BUTTON", L"确定", WS_CHILD | WS_VISIBLE, S(120), S(160), S(100), S(35), hDlg, (HMENU)IDOK, NULL, NULL);
    EnumChildWindows(hDlg, [](HWND h, LPARAM p){ SendMessage(h, WM_SETFONT, p, TRUE); return TRUE; }, (LPARAM)hF);

    ShowWindow(hDlg, SW_SHOW); EnableWindow(parent, FALSE);
    MSG m; while (GetMessage(&m, NULL, 0, 0)) { TranslateMessage(&m); DispatchMessage(&m); if (!IsWindow(hDlg)) break; }
    EnableWindow(parent, TRUE); SetForegroundWindow(parent);

    o1 = InputState::result1; o2 = InputState::result2; o3 = InputState::result3;
    return InputState::isOk;
}

LRESULT CALLBACK LoginWndProc(HWND hWnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_COMMAND && LOWORD(wp) == IDOK) {
        WCHAR e[128], p[128]; GetDlgItemTextW(hWnd, 101, e, 128); GetDlgItemTextW(hWnd, 102, p, 128);
        if (ApiLogin(e, p) == "SUCCESS") { g_LoginSuccess = true; DestroyWindow(hWnd); }
        else MessageBoxW(hWnd, L"登录失败", L"错误", MB_ICONERROR);
    } else if (msg == WM_DESTROY) PostQuitMessage(0);
    return DefWindowProc(hWnd, msg, wp, lp);
}

bool ShowLogin() {
    WNDCLASSW wc = {0}; wc.lpfnWndProc = LoginWndProc; wc.hInstance = GetModuleHandle(NULL);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1); wc.lpszClassName = L"LoginWndClass"; RegisterClassW(&wc);
    HWND h = CreateWindowExW(0, L"LoginWndClass", L"MathQuiz 登录", WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU,
        (GetSystemMetrics(SM_CXSCREEN) - S(350)) / 2, (GetSystemMetrics(SM_CYSCREEN) - S(250)) / 2,
        S(350), S(250), NULL, NULL, GetModuleHandle(NULL), NULL);
    HFONT hF = GetMiSansFont(14);
    CreateWindowW(L"STATIC", L"邮箱:", WS_CHILD | WS_VISIBLE, S(40), S(50), S(50), S(20), h, NULL, NULL, NULL);
    CreateWindowW(L"EDIT", L"", WS_CHILD | WS_VISIBLE | WS_BORDER, S(100), S(48), S(200), S(25), h, (HMENU)101, NULL, NULL);
    CreateWindowW(L"STATIC", L"密码:", WS_CHILD | WS_VISIBLE, S(40), S(90), S(50), S(20), h, NULL, NULL, NULL);
    CreateWindowW(L"EDIT", L"", WS_CHILD | WS_VISIBLE | WS_BORDER | ES_PASSWORD, S(100), S(88), S(200), S(25), h, (HMENU)102, NULL, NULL);
    CreateWindowW(L"BUTTON", L"登录", WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON, S(125), S(140), S(100), S(35), h, (HMENU)IDOK, NULL, NULL);
    EnumChildWindows(h, [](HWND ch, LPARAM p){ SendMessage(ch, WM_SETFONT, p, TRUE); return TRUE; }, (LPARAM)hF);
    ShowWindow(h, SW_SHOW); UpdateWindow(h);
    MSG m; while(GetMessage(&m, NULL, 0, 0)) { TranslateMessage(&m); DispatchMessage(&m); if(!IsWindow(h)) break; }
    return g_LoginSuccess;
}