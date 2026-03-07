#include "ui.h"
#include "utils.h"
#include "api.h"
#include "common.h" // 🚀 必须包含 .h 而非 .cpp
#include "stats_window.h"
#include "weekly_view_window.h" // 🚀 引入周视图头文件
#include <commctrl.h>
#include <algorithm>
#include <ctime>
#include <shlwapi.h>

#pragma comment(lib, "shlwapi.lib")

using namespace Gdiplus;

// 外部声明
extern void CheckForUpdates(bool isManual = false);
extern void ShowStatsWindow(HWND parent);
extern void ShowCompletedTodosWindow(HWND parent);
extern bool ApiFetchUserStatus(); // 🚀 声明其返回布尔值以检测离线状态

// 定时器 ID
const UINT_PTR SCROLL_TIMER_ID = 1001;

// 前瞻声明
LRESULT CALLBACK LoginWndProc(HWND hWnd, UINT msg, WPARAM wp, LPARAM lp);
LRESULT CALLBACK InputWndProc(HWND hWnd, UINT msg, WPARAM wp, LPARAM lp);
void SaveLoginConfig(const WCHAR* email, const WCHAR* password, bool savePass, bool autoLogin);


// 内部辅助：字符串转 SYSTEMTIME (支持解析时间)
void StringToSystemTime(const std::wstring& dateStr, SYSTEMTIME& st) {
    GetLocalTime(&st);
    int y, m, d, hr = 0, min = 0;
    // 尝试解析带时间的格式，如果失败则解析纯日期
    if (swscanf(dateStr.c_str(), L"%d-%d-%d %d:%d", &y, &m, &d, &hr, &min) >= 3) {
        st.wYear = (WORD)y;
        st.wMonth = (WORD)m;
        st.wDay = (WORD)d;
        st.wHour = (WORD)hr;
        st.wMinute = (WORD)min;
        st.wSecond = 0;
    }
}

// 内部辅助：解析日期字符串为 time_t (支持精确到分钟的计算)
time_t ParseWStringDate(const std::wstring& s, bool endOfDayFallback) {
    int y, m, d, hr = 0, min = 0;
    int count = swscanf(s.c_str(), L"%d-%d-%d %d:%d", &y, &m, &d, &hr, &min);

    if (count < 3) return 0;

    struct tm t = { 0 };
    t.tm_year = y - 1900;
    t.tm_mon = m - 1;
    t.tm_mday = d;

    if (count >= 5) {
        // 如果字符串中包含时间，直接使用
        t.tm_hour = hr;
        t.tm_min = min;
    } else {
        // 只有日期时，根据场景设置默认时间
        if (endOfDayFallback) {
            t.tm_hour = 23; t.tm_min = 59; t.tm_sec = 59;
        } else {
            t.tm_hour = 0; t.tm_min = 0; t.tm_sec = 0;
        }
    }
    t.tm_isdst = -1;
    return mktime(&t);
}

// 内部辅助：判断是否今日相关
bool IsTodayRelevant(const std::wstring& startStr, const std::wstring& endStr) {
    time_t tNow = time(nullptr);
    struct tm now_tm;
    localtime_s(&now_tm, &tNow);
    now_tm.tm_hour = 0; now_tm.tm_min = 0; now_tm.tm_sec = 0;
    time_t todayStart = mktime(&now_tm);
    time_t todayEnd = todayStart + 86399; // 23:59:59

    time_t tStart = ParseWStringDate(startStr, false);
    if (endStr.empty()) {
        return (tStart >= todayStart && tStart <= todayEnd);
    } else {
        time_t tEnd = ParseWStringDate(endStr, true);
        return (tStart <= todayEnd && tEnd >= todayStart);
    }
}

// 计算进度条百分比
float CalculateTodoProgress(const std::wstring& startStr, const std::wstring& endStr) {
    time_t tStart, tEnd;
    if (endStr.empty()) {
        tStart = ParseWStringDate(startStr, false);
        tEnd = tStart + 3600; // 默认一小时
    } else {
        tStart = ParseWStringDate(startStr, false);
        tEnd = ParseWStringDate(endStr, true);
    }

    time_t tNow = time(nullptr);
    if (tStart >= tEnd) return 1.0f;
    if (tNow <= tStart) return 0.0f;
    if (tNow >= tEnd) return 1.0f;

    return (float)(tNow - tStart) / (float)(tEnd - tStart);
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

        // --- 修改点：将 MiSans 替换为 Microsoft YaHei ---
        FontFamily ff(L"Microsoft YaHei");
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
        g.DrawString(L"重要日", -1, &headF, PointF((REAL) S(15), y), &gBrush);

        // 🚀 绘制手绘风格的“日历小图标” (周视图入口)
        float calX = (float)(width - S(75));
        float calY = y + S(2);
        Pen calPen(Color(255, 180, 180, 180), 1.5f);
        SolidBrush calFill(Color(255, 180, 180, 180));
        // 主体方框
        g.DrawRectangle(&calPen, calX, calY, (REAL)S(16), (REAL)S(14));
        // 顶栏分割线
        g.DrawLine(&calPen, calX, calY + S(5), calX + S(16), calY + S(5));
        // 顶部的两个小环（活页环）
        g.FillRectangle(&calFill, calX + S(3), calY - S(2), (REAL)S(2), (REAL)S(4));
        g.FillRectangle(&calFill, calX + S(11), calY - S(2), (REAL)S(2), (REAL)S(4));
        // 添加日历命中区 (type = 8 代表周视图)
        g_HitZones.push_back({Rect((int)calX - S(5), (int)y, S(25), S(20)), 0, 8});

        // 原有的 [+] 按钮
        g.DrawString(L"[+]", -1, &headF, PointF((REAL) (width - S(40)), y), &grBrush);
        g_HitZones.push_back({Rect(width - S(45), (int) y, S(30), S(20)), 0, 2});
        y += S(20);

        bool hasActiveCountdown = false;
        for (const auto &it : g_Countdowns) {
            if (it.daysLeft >= 0) {
                hasActiveCountdown = true;
                g_HitZones.push_back({Rect(S(15), (int) y, width - S(30), S(20)), it.id, 4});
                g.DrawString(it.title.c_str(), -1, &contentF, PointF((REAL) S(15), y), &wBrush);
                std::wstring dStr = std::to_wstring(it.daysLeft) + L" 天";
                g.DrawString(dStr.c_str(), -1, &contentF, PointF((REAL) (width - S(65)), y), (it.daysLeft <= 3) ? &rBrush : &grBrush);
                y += S(20);
            }
        }
        if (!hasActiveCountdown) {
            g.DrawString(L"暂无有效倒计时", -1, &contentF, PointF((REAL) S(15), y), &gBrush);
            y += S(20);
        }

        // --- 屏幕时间板块 ---
        y += S(10);
        int totalSec = GetTotalScreenTimeInternal();
        std::wstring totalStr = (totalSec < 3600) ? (std::to_wstring(totalSec/60) + L" m") : (std::to_wstring(totalSec/3600) + L" h " + std::to_wstring((totalSec%3600)/60) + L" m");
        g.DrawString(L"今日屏幕时间", -1, &headF, PointF((REAL) S(15), y), &gBrush);
        g_HitZones.push_back({Rect(S(15), (int) y, S(100), S(20)), 0, 6});
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
                int iconType = 0;
                std::wstring dn = rec.deviceName;
                for(auto &c : dn) c = towlower(c);
                if (dn.find(L"tablet") != std::wstring::npos || dn.find(L"pad") != std::wstring::npos) iconType = 2;
                else if (dn.find(L"phone") != std::wstring::npos || dn.find(L"iphone") != std::wstring::npos || dn.find(L"android") != std::wstring::npos) iconType = 1;
                else iconType = 0;
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

        // 增加 [已完成] 按钮
        g.DrawString(L"[已完成]", -1, &headF, PointF((REAL) (width - S(100)), y), &grBrush);
        g_HitZones.push_back({Rect(width - S(105), (int) y, S(50), S(20)), 0, 7});

        g.DrawString(L"[+]", -1, &headF, PointF((REAL) (width - S(40)), y), &grBrush);
        g_HitZones.push_back({Rect(width - S(45), (int) y, S(30), S(20)), 0, 1});
        y += S(20);

        auto displayTodos = g_Todos;
        // 过滤掉已完成的待办事项
        displayTodos.erase(std::remove_if(displayTodos.begin(), displayTodos.end(), [](const auto& t) { return t.isDone; }), displayTodos.end());

        std::sort(displayTodos.begin(), displayTodos.end(), [](const auto& a, const auto& b) {
            if (a.isDone != b.isDone) return !a.isDone;
            bool aToday = IsTodayRelevant(a.createdDate, a.dueDate);
            bool bToday = IsTodayRelevant(b.createdDate, b.dueDate);
            if (aToday != bToday) return aToday;
            float progA = CalculateTodoProgress(a.createdDate, a.dueDate);
            float progB = CalculateTodoProgress(b.createdDate, b.dueDate);
            if (std::abs(progA - progB) > 0.001f) return progA > progB;
            if (a.dueDate != b.dueDate) {
                if (a.dueDate.empty()) return false;
                if (b.dueDate.empty()) return true;
                return a.dueDate < b.dueDate;
            }
            return false;
        });

        if (displayTodos.empty()) {
            g.DrawString(L"暂无待办", -1, &contentF, PointF((REAL) S(15), y), &gBrush);
        } else {
            for (const auto &it : displayTodos) {
                g_HitZones.push_back({Rect(S(15), (int) y, width - S(65), S(35)), it.id, 3, it.uuid});
                g_HitZones.push_back({Rect(width - S(45), (int) y, S(35), S(35)), it.id, 5, it.uuid});

                g.DrawRectangle(&linePen, S(15), (int) y + S(6), S(12), S(12));
                if (it.isDone) g.FillRectangle(&wBrush, S(17), (int) y + S(8), S(8), S(8));

                // --- 滚动文字实现 ---
                std::wstring dispContent = it.content;
                const size_t MAX_LEN = 5;
                if (dispContent.length() > MAX_LEN && !it.isDone) {
                    std::wstring spacer = L"    ";
                    std::wstring scrollText = dispContent + spacer;
                    size_t textLen = scrollText.length();

                    const DWORD PAUSE_MS = 5000;
                    const DWORD SPEED_MS = 300;
                    DWORD cycleTotalTime = PAUSE_MS + (textLen * SPEED_MS);

                    DWORD currentTime = GetTickCount() % cycleTotalTime;
                    size_t offset = 0;

                    if (currentTime > PAUSE_MS) {
                        offset = (currentTime - PAUSE_MS) / SPEED_MS;
                    }

                    std::wstring doubled = scrollText + scrollText;
                    dispContent = doubled.substr(offset % textLen, MAX_LEN);
                } else if (dispContent.length() > MAX_LEN && it.isDone) {
                    dispContent = dispContent.substr(0, MAX_LEN) + L"...";
                }

                int style = it.isDone ? FontStyleStrikeout : FontStyleRegular;
                Font itemF(&ff, (REAL) S(14), style, UnitPixel);
                g.DrawString(dispContent.c_str(), -1, &itemF, PointF((REAL) S(32), y + S(3)), it.isDone ? &gBrush : &wBrush);
                g.DrawString(L"[-]", -1, &headF, PointF((REAL)(width - S(40)), y + S(5)), &rBrush);

                float progress = CalculateTodoProgress(it.createdDate, it.dueDate);
                if (progress >= 0) {
                    SolidBrush barBg(Color(50, 255, 255, 255));
                    g.FillRectangle(&barBg, S(32), (int)y + S(24), width - S(90), S(4));
                    Color progressColor = (progress > 0.85f && !it.isDone) ? Color(255, 255, 100, 100) : Color(255, 80, 220, 80);
                    SolidBrush barFg(progressColor);
                    g.FillRectangle(&barFg, S(32), (int)y + S(24), (int)((width - S(90)) * progress), S(4));

                    std::wstring dateLabel = (!it.dueDate.empty()) ?
                        (it.createdDate.substr(0, 16) + L" 至 " + it.dueDate.substr(11, 5)) :
                        (L"开始: " + it.createdDate.substr(0, 16));

                    RectF layoutRect(0, 0, (REAL)width, (REAL)S(20));
                    RectF boundRect;
                    g.MeasureString(dateLabel.c_str(), -1, &dateF, layoutRect, &boundRect);
                    float dateX = (float)width - boundRect.Width - S(45);
                    g.DrawString(dateLabel.c_str(), -1, &dateF, PointF(dateX, y + S(5)), &gBrush);
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
    int activeCountdowns = 0;
    for (const auto& c : g_Countdowns) if (c.daysLeft >= 0) activeCountdowns++;
    h += S(30) + (activeCountdowns == 0 ? S(20) : activeCountdowns * S(20));
    int appRows = std::min((int)g_AppUsage.size(), g_TopAppsCount);
    h += S(30) + (appRows == 0 ? S(20) : appRows * S(20));

    // 仅计算未完成事项的高度
    int activeTodos = 0;
    for (const auto& t : g_Todos) if (!t.isDone) activeTodos++;
    h += S(30) + (activeTodos == 0 ? S(20) : activeTodos * S(35));

    h += S(25); if (h < S(180)) h = S(180);
    RECT rc; GetWindowRect(g_hWidgetWnd, &rc);
    SetWindowPos(g_hWidgetWnd, HWND_BOTTOM, rc.left, rc.top, S(300), h, SWP_NOACTIVATE);
    RenderWidget();
}

LRESULT CALLBACK WidgetWndProc(HWND hWnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_CREATE:
            SetTimer(hWnd, SCROLL_TIMER_ID, 200, NULL);
            break;
        case WM_TIMER:
            if (wp == SCROLL_TIMER_ID) {
                if (IsWindowVisible(hWnd)) RenderWidget();
            }
            break;
        case WM_LBUTTONDOWN: {
            int x = LOWORD(lp), y = HIWORD(lp); bool hit = false;
            std::vector<HitZone> zones; { std::lock_guard<std::recursive_mutex> lock(g_DataMutex); zones = g_HitZones; }
            for (const auto& z : zones) {
                if (z.rect.Contains(x, y)) {
                    hit = true;
                    if (z.type == 1) {
                        std::wstring c, d1, d2; if (ShowInputDialog(hWnd, 0, c, d1, d2)) { ApiAddTodo(c, d1, d2, false); SyncData(); }
                    }
                    else if (z.type == 2) {
                        std::wstring t, d, dummy; if (ShowInputDialog(hWnd, 1, t, d, dummy)) { ApiAddCountdown(t, d); SyncData(); }
                    }
                    else if (z.type == 3) {
                        if (x < S(30)) {
                            // 切换完成状态：优先用 uuid 匹配
                            bool done = false;
                            int matchId = z.id;
                            std::wstring matchUuid = z.uuid;
                            {
                                std::lock_guard<std::recursive_mutex> l(g_DataMutex);
                                for (auto &t : g_Todos) {
                                    if ((!matchUuid.empty() && t.uuid == matchUuid) || t.id == matchId) {
                                        done      = t.isDone;
                                        matchId   = t.id;
                                        matchUuid = t.uuid;
                                        break;
                                    }
                                }
                            }
                            if (!matchUuid.empty())
                                ApiToggleTodoByUuid(matchUuid, !done);
                            else
                                ApiToggleTodo(matchId, !done);
                        } else {
                            // 编辑待办：用 uuid 查找原始数据
                            std::wstring c, d1, d2;
                            bool currentDone = false;
                            std::wstring foundUuid = z.uuid;
                            int foundId = z.id;
                            {
                                std::lock_guard<std::recursive_mutex> l(g_DataMutex);
                                for (auto &t : g_Todos) {
                                    if ((!foundUuid.empty() && t.uuid == foundUuid) || t.id == foundId) {
                                        c           = t.content;
                                        d1          = t.createdDate;
                                        d2          = t.dueDate;
                                        currentDone = t.isDone;
                                        foundUuid   = t.uuid; // 确保拿到最新 uuid
                                        foundId     = t.id;
                                        break;
                                    }
                                }
                            }
                            // 🚀 修复：编辑完成后调用 ApiUpdateTodo 原地更新，而非 ApiAddTodo 新增
                            if (ShowInputDialog(hWnd, 0, c, d1, d2)) {
                                ApiUpdateTodo(foundUuid, c, d1, d2, currentDone);
                            }
                        }
                        SyncData();
                    }
                    else if (z.type == 4) {
                        if (MessageBoxW(hWnd, L"确定要删除吗？", L"确认", MB_YESNO) == IDYES) { ApiDeleteCountdown(z.id); SyncData(); }
                    }
                    else if (z.type == 5) {
                        if (MessageBoxW(hWnd, L"确定要删除这条待办吗？", L"确认删除", MB_YESNO | MB_ICONQUESTION) == IDYES) {
                            // 优先用 uuid 找到真实 id
                            int realId = z.id;
                            std::wstring delUuid = z.uuid;
                            {
                                std::lock_guard<std::recursive_mutex> l(g_DataMutex);
                                for (auto &t : g_Todos) {
                                    if ((!delUuid.empty() && t.uuid == delUuid) || t.id == z.id) {
                                        realId = t.id;
                                        break;
                                    }
                                }
                            }
                            ApiDeleteTodo(realId);
                            SyncData();
                        }
                    }
                    else if (z.type == 6) { ShowStatsWindow(hWnd); }
                    else if (z.type == 7) { ShowCompletedTodosWindow(hWnd); }
                    else if (z.type == 8) { ShowWeeklyViewWindow(hWnd); } // 🚀 响应点击，打开周视图
                    break;
                }
            }
            if (!hit) SendMessage(hWnd, WM_NCLBUTTONDOWN, HTCAPTION, 0);
        } break;
        case WM_RBUTTONUP: {
            POINT pt; GetCursorPos(&pt);

            static time_t s_lastFetchTime = 0;
            static bool s_isOffline = true;
            time_t now = time(nullptr);

            // 5 分钟 (300秒) 触发一次限制逻辑
            if (now - s_lastFetchTime >= 300) {
                s_isOffline = !ApiFetchUserStatus();
                s_lastFetchTime = now;
            }

            HMENU hMenu = CreatePopupMenu();

            // 顶部：展示账户信息与等级 (置灰项)
            std::wstring accInfo = L"账号: " + (g_Username.empty() ? L"未登录" : g_Username) + L" (" + g_UserTier + L")";
            AppendMenuW(hMenu, MF_DISABLED | MF_GRAYED, 0, accInfo.c_str());

            std::wstring syncInfo = L"今日同步进度: " + std::to_wstring(g_SyncCount) + L" / " + std::to_wstring(g_SyncLimit);
            if (s_isOffline) {
                syncInfo += L" (离线/非最新)";
            }
            AppendMenuW(hMenu, MF_DISABLED | MF_GRAYED, 0, syncInfo.c_str());
            AppendMenuW(hMenu, MF_SEPARATOR, 0, NULL);

            HMENU hTopSub = CreatePopupMenu();
            AppendMenuW(hTopSub, MF_STRING | (g_TopAppsCount == 3 ? MF_CHECKED : 0), 3003, L"前 3");
            AppendMenuW(hTopSub, MF_STRING | (g_TopAppsCount == 5 ? MF_CHECKED : 0), 3005, L"前 5");
            AppendMenuW(hTopSub, MF_STRING | (g_TopAppsCount == 10 ? MF_CHECKED : 0), 3010, L"前 10");
            AppendMenuW(hMenu, MF_POPUP, (UINT_PTR)hTopSub, L"统计排名展示");

            HMENU hFreqSub = CreatePopupMenu();
            AppendMenuW(hFreqSub, MF_STRING | (g_SyncInterval <= 0 ? MF_CHECKED : 0), 4000, L"从不");
            AppendMenuW(hFreqSub, MF_STRING | (g_SyncInterval == 5 ? MF_CHECKED : 0), 4005, L"每 5 分钟");
            AppendMenuW(hFreqSub, MF_STRING | (g_SyncInterval == 10 ? MF_CHECKED : 0), 4010, L"每 10 分钟");
            AppendMenuW(hFreqSub, MF_STRING | (g_SyncInterval == 30 ? MF_CHECKED : 0), 4030, L"每 30 分钟");
            AppendMenuW(hFreqSub, MF_STRING | (g_SyncInterval == 60 ? MF_CHECKED : 0), 4060, L"每小时");
            AppendMenuW(hFreqSub, MF_SEPARATOR, 0, NULL);

            std::wstring customLabel = L"自定义分钟...";
            bool isPreset = (g_SyncInterval == 0 || g_SyncInterval == 5 || g_SyncInterval == 10 || g_SyncInterval == 30 || g_SyncInterval == 60);
            if (!isPreset && g_SyncInterval > 0) {
                customLabel = L"自定义: " + std::to_wstring(g_SyncInterval) + L" 分钟";
            }
            AppendMenuW(hFreqSub, MF_STRING | (!isPreset ? MF_CHECKED : 0), 4999, customLabel.c_str());
            AppendMenuW(hMenu, MF_POPUP, (UINT_PTR)hFreqSub, L"自动同步频率");

            AppendMenuW(hMenu, MF_SEPARATOR, 0, NULL);
            AppendMenuW(hMenu, 0, 1001, L"立即同步");
            AppendMenuW(hMenu, 0, 1004, L"屏幕时间统计报告");
            AppendMenuW(hMenu, 0, 1002, L"检查更新");
            AppendMenuW(hMenu, 0, 1005, L"退出账号");
            AppendMenuW(hMenu, 0, 1003, L"退出程序");

            int cmd = TrackPopupMenu(hMenu, TPM_RETURNCMD, pt.x, pt.y, 0, hWnd, NULL);

            if (cmd >= 3003 && cmd <= 3010) {
                g_TopAppsCount = (cmd == 3003) ? 3 : (cmd == 3005 ? 5 : 10);
                ResizeWidget();
            }
            else if (cmd >= 4000 && cmd <= 4060) {
                g_SyncInterval = (cmd - 4000);
                SaveLoginConfig(g_SavedEmail.c_str(), g_SavedPass.c_str(), !g_SavedPass.empty(), g_AutoLogin);
            }
            else if (cmd == 4999) {
                std::wstring minStr = std::to_wstring(g_SyncInterval);
                std::wstring d1, d2;
                if (ShowInputDialog(hWnd, 2, minStr, d1, d2)) {
                    int val = _wtoi(minStr.c_str());
                    if (val >= 0) {
                        g_SyncInterval = val;
                        SaveLoginConfig(g_SavedEmail.c_str(), g_SavedPass.c_str(), !g_SavedPass.empty(), g_AutoLogin);
                    }
                }
            }
            else if (cmd == 1001) SyncData();
            else if (cmd == 1004) ShowStatsWindow(hWnd);
            else if (cmd == 1002) CheckForUpdates(true);
            else if (cmd == 1005) {
                g_UserId = 0; g_Username = L""; g_LoginSuccess = false;
                {
                    std::lock_guard<std::recursive_mutex> lock(g_DataMutex);
                    g_Todos.clear(); g_Countdowns.clear(); g_AppUsage.clear();
                }
                ShowWindow(hWnd, SW_HIDE);
                if (ShowLogin(true)) {
                    SyncData();
                    ShowWindow(hWnd, SW_SHOW);
                } else {
                    PostQuitMessage(0);
                }
            }
            else if (cmd == 1003) PostQuitMessage(0);

            DestroyMenu(hTopSub); DestroyMenu(hFreqSub); DestroyMenu(hMenu);
        } break;
        case WM_USER_REFRESH: case WM_USER_TICK: ResizeWidget(); break;
        case WM_DESTROY: KillTimer(hWnd, SCROLL_TIMER_ID); PostQuitMessage(0); break;
        default: return DefWindowProc(hWnd, msg, wp, lp);
    }
    return 0;
}

LRESULT CALLBACK InputWndProc(HWND hWnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_COMMAND && LOWORD(wp) == IDOK) {
        WCHAR content[512]; GetDlgItemTextW(hWnd, 101, content, 512);
        InputState::result1 = content;

        if (GetWindowLongPtr(GetDlgItem(hWnd, 102), GWL_STYLE) & WS_VISIBLE) {
            SYSTEMTIME st1, st2;
            DateTime_GetSystemtime(GetDlgItem(hWnd, 102), &st1);
            DateTime_GetSystemtime(GetDlgItem(hWnd, 103), &st2);
            wchar_t buf[64];
            swprintf_s(buf, L"%04d-%02d-%02d %02d:%02d", st1.wYear, st1.wMonth, st1.wDay, st1.wHour, st1.wMinute);
            InputState::result2 = buf;
            swprintf_s(buf, L"%04d-%02d-%02d %02d:%02d", st2.wYear, st2.wMonth, st2.wDay, st2.wHour, st2.wMinute);
            InputState::result3 = buf;
        }

        InputState::isOk = true; DestroyWindow(hWnd);
    } else if (msg == WM_COMMAND && LOWORD(wp) == IDCANCEL) DestroyWindow(hWnd);
    return DefWindowProc(hWnd, msg, wp, lp);
}

bool ShowInputDialog(HWND parent, int type, std::wstring &o1, std::wstring &o2, std::wstring &o3) {
    InputState::isOk = false;
    WNDCLASSW wc = {0}; wc.lpfnWndProc = InputWndProc; wc.hInstance = GetModuleHandle(NULL);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1); wc.lpszClassName = L"InputDlg"; RegisterClassW(&wc);

    int dlgHeight = (type == 2) ? S(140) : S(260);
    HWND hDlg = CreateWindowExW(0, L"InputDlg",
        type == 0 ? L"待办事项" : (type == 1 ? L"日期设置" : L"同步频率"),
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU,
        100, 100, S(350), dlgHeight, parent, NULL, NULL, NULL);

    HFONT hF = GetMiSansFont(14);

    CreateWindowW(L"STATIC", type == 2 ? L"间隔(分):" : (type == 0 ? L"内容:" : L"标题:"),
        WS_CHILD | WS_VISIBLE, S(20), S(20), S(80), S(20), hDlg, NULL, NULL, NULL);
    CreateWindowW(L"EDIT", o1.c_str(), WS_CHILD | WS_VISIBLE | WS_BORDER | (type == 2 ? ES_NUMBER : 0),
        S(100), S(18), S(200), S(25), hDlg, (HMENU)101, NULL, NULL);

    if (type != 2) {
        CreateWindowW(L"STATIC", type == 0 ? L"开始时间:" : L"目标日期:", WS_CHILD | WS_VISIBLE, S(20), S(60), S(80), S(20), hDlg, NULL, NULL, NULL);
        HWND hPicker1 = CreateWindowExW(0, DATETIMEPICK_CLASS, L"", WS_BORDER | WS_CHILD | WS_VISIBLE,
            S(100), S(58), S(200), S(25), hDlg, (HMENU)102, NULL, NULL);
        DateTime_SetFormat(hPicker1, L"yyyy-MM-dd HH:mm");

        SYSTEMTIME stStart;
        StringToSystemTime(o2.empty() ? GetTodayDate() : o2, stStart);
        DateTime_SetSystemtime(hPicker1, GDT_VALID, &stStart);

        if (type == 0) {
            CreateWindowW(L"STATIC", L"截止时间:", WS_CHILD | WS_VISIBLE, S(20), S(100), S(80), S(20), hDlg, NULL, NULL, NULL);
            HWND hPicker2 = CreateWindowExW(0, DATETIMEPICK_CLASS, L"", WS_BORDER | WS_CHILD | WS_VISIBLE,
                S(100), S(98), S(200), S(25), hDlg, (HMENU)103, NULL, NULL);
            DateTime_SetFormat(hPicker2, L"yyyy-MM-dd HH:mm");

            SYSTEMTIME stEnd;
            if (o3.empty()) {
                GetLocalTime(&stEnd);
                FILETIME ft; SystemTimeToFileTime(&stEnd, &ft);
                ULARGE_INTEGER uli; uli.LowPart = ft.dwLowDateTime; uli.HighPart = ft.dwHighDateTime;
                uli.QuadPart += (ULONGLONG)24 * 60 * 60 * 1000 * 10000;
                ft.dwLowDateTime = uli.LowPart; ft.dwHighDateTime = uli.HighPart;
                FileTimeToSystemTime(&ft, &stEnd);
            } else { StringToSystemTime(o3, stEnd); }
            DateTime_SetSystemtime(hPicker2, GDT_VALID, &stEnd);
        }
    }

    int btnY = (type == 2) ? S(60) : S(160);
    CreateWindowW(L"BUTTON", L"确定", WS_CHILD | WS_VISIBLE, S(120), btnY, S(100), S(35), hDlg, (HMENU)IDOK, NULL, NULL);

    EnumChildWindows(hDlg, [](HWND h, LPARAM p){ SendMessage(h, WM_SETFONT, p, TRUE); return TRUE; }, (LPARAM)hF);
    ShowWindow(hDlg, SW_SHOW); EnableWindow(parent, FALSE);
    MSG m; while (GetMessage(&m, NULL, 0, 0)) { TranslateMessage(&m); DispatchMessage(&m); if (!IsWindow(hDlg)) break; }
    EnableWindow(parent, TRUE); SetForegroundWindow(parent);
    o1 = InputState::result1; o2 = InputState::result2; o3 = InputState::result3;
    return InputState::isOk;
}

void LoadLoginConfig(WCHAR* email, WCHAR* password, bool& savePass, bool& autoLogin) {
    WCHAR path[MAX_PATH];
    GetModuleFileNameW(NULL, path, MAX_PATH);
    PathRemoveFileSpecW(path);
    PathAppendW(path, SETTINGS_FILE.c_str());

    autoLogin = GetPrivateProfileIntW(L"Auth", L"AutoLogin", 0, path) != 0;
    savePass = !g_SavedPass.empty();

    g_SyncInterval = GetPrivateProfileIntW(L"Auth", L"SyncInterval", 5, path);

    g_SyncCount = GetPrivateProfileIntW(L"Auth", L"SyncCount", 0, path);
    g_SyncLimit = GetPrivateProfileIntW(L"Auth", L"SyncLimit", 50, path);
    WCHAR tierBuf[64];
    GetPrivateProfileStringW(L"Auth", L"UserTier", L"free", tierBuf, 64, path);
    g_UserTier = tierBuf;

    if (!g_SavedEmail.empty()) {
        lstrcpynW(email, g_SavedEmail.c_str(), 128);
    } else {
        email[0] = L'\0';
    }

    if (savePass) {
        lstrcpynW(password, g_SavedPass.c_str(), 128);
    } else {
        password[0] = L'\0';
    }
}

void SaveLoginConfig(const WCHAR* email, const WCHAR* password, bool savePass, bool autoLogin) {
    SaveSettings(g_UserId, g_Username, email, password, savePass);

    WCHAR path[MAX_PATH];
    GetModuleFileNameW(NULL, path, MAX_PATH);
    PathRemoveFileSpecW(path);
    PathAppendW(path, SETTINGS_FILE.c_str());
    WritePrivateProfileStringW(L"Auth", L"AutoLogin", autoLogin ? L"1" : L"0", path);
    WritePrivateProfileStringW(L"Auth", L"SyncInterval", std::to_wstring(g_SyncInterval).c_str(), path);
}

bool ExecuteLoginWithRetry(HWND hWnd, const WCHAR* email, const WCHAR* password) {
    for (int i = 0; i < 5; ++i) {
        if (ApiLogin(email, password) == "SUCCESS") return true;
        if (i == 4) break;
        DWORD startTick = GetTickCount();
        while (GetTickCount() - startTick < 2000) {
            MSG msg;
            while (PeekMessage(&msg, NULL, 0, 0, PM_REMOVE)) {
                if (msg.message == WM_QUIT) { PostQuitMessage((int)msg.wParam); return false; }
                TranslateMessage(&msg); DispatchMessage(&msg);
            }
            if (hWnd && !IsWindow(hWnd)) return false;
            Sleep(50);
        }
    }
    return false;
}

LRESULT CALLBACK LoginWndProc(HWND hWnd, UINT msg, WPARAM wp, LPARAM lp) {
    static bool isLoggingIn = false;

    // 保存密码复选框 (ID=103) 状态变化
    if (msg == WM_COMMAND && LOWORD(wp) == 103 && HIWORD(wp) == BN_CLICKED) {
        bool saveChecked = SendDlgItemMessage(hWnd, 103, BM_GETCHECK, 0, 0) == BST_CHECKED;
        if (!saveChecked) {
            // 取消保存密码 → 同时取消自动登录
            SendDlgItemMessage(hWnd, 104, BM_SETCHECK, BST_UNCHECKED, 0);
            // 立即从 INI 清除密码（但保留邮箱）
            WCHAR iniPath[MAX_PATH];
            GetModuleFileNameW(NULL, iniPath, MAX_PATH);
            PathRemoveFileSpecW(iniPath);
            PathAppendW(iniPath, SETTINGS_FILE.c_str());
            WritePrivateProfileStringW(L"Auth", L"Pass", NULL, iniPath);
            WritePrivateProfileStringW(L"Auth", L"AutoLogin", L"0", iniPath);
            g_SavedPass = L""; // 同步清空内存中的密码
        }
        return 0;
    }

    // 自动登录复选框 (ID=104) 状态变化
    if (msg == WM_COMMAND && LOWORD(wp) == 104 && HIWORD(wp) == BN_CLICKED) {
        bool autoChecked = SendDlgItemMessage(hWnd, 104, BM_GETCHECK, 0, 0) == BST_CHECKED;
        if (autoChecked) {
            // 勾选自动登录 → 强制同时勾上保存密码
            SendDlgItemMessage(hWnd, 103, BM_SETCHECK, BST_CHECKED, 0);
        }
        return 0;
    }

    if (msg == WM_COMMAND && LOWORD(wp) == IDOK) {
        if (isLoggingIn) return 0;
        isLoggingIn = true;
        HWND hBtn = GetDlgItem(hWnd, IDOK);
        EnableWindow(hBtn, FALSE);
        SetWindowTextW(hBtn, L"登录中...");
        WCHAR e[128] = {0}, p[128] = {0};
        GetDlgItemTextW(hWnd, 101, e, 128); GetDlgItemTextW(hWnd, 102, p, 128);
        bool success = ExecuteLoginWithRetry(hWnd, e, p);
        if (!IsWindow(hWnd)) { isLoggingIn = false; return 0; }
        if (success) {
            g_LoginSuccess = true;
            bool savePass = SendDlgItemMessage(hWnd, 103, BM_GETCHECK, 0, 0) == BST_CHECKED;
            bool autoLogin = savePass && (SendDlgItemMessage(hWnd, 104, BM_GETCHECK, 0, 0) == BST_CHECKED);
            SaveLoginConfig(e, p, savePass, autoLogin);
            DestroyWindow(hWnd);
        } else {
            EnableWindow(hBtn, TRUE); SetWindowTextW(hBtn, L"登录");
            MessageBoxW(hWnd, L"登录失败，请检查网络连接或账号密码。", L"错误", MB_ICONERROR);
        }
        isLoggingIn = false; return 0;
    }
    else if (msg == WM_DESTROY) {
        isLoggingIn = false; // 重置，防止下次打开登录窗时按钮永久失效
        // 只有在非登录成功的情况下才发退出消息，防止 DestroyWindow 导致误退出
        if (!g_LoginSuccess) PostQuitMessage(0);
    }
    return DefWindowProc(hWnd, msg, wp, lp);
}

bool ShowLogin(bool isManualLogout) {
    g_LoginSuccess = false; // 每次显示登录窗口前重置状态，防止残留值导致闪退

    // 防止重复注册 WndClass（第二次退出再登录时会崩溃）
    static bool s_loginClassRegistered = false;
    if (!s_loginClassRegistered) {
        WNDCLASSW wc = {0}; wc.lpfnWndProc = LoginWndProc; wc.hInstance = GetModuleHandle(NULL);
        wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1); wc.lpszClassName = L"LoginWndClass";
        if (RegisterClassW(&wc)) s_loginClassRegistered = true;
    }
    HWND h = CreateWindowExW(0, L"LoginWndClass", L"MathQuiz 登录", WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU,
        (GetSystemMetrics(SM_CXSCREEN) - S(350)) / 2, (GetSystemMetrics(SM_CYSCREEN) - S(250)) / 2,
        S(350), S(250), NULL, NULL, GetModuleHandle(NULL), NULL);
    HFONT hF = GetMiSansFont(14);
    CreateWindowW(L"STATIC", L"邮箱:", WS_CHILD | WS_VISIBLE, S(40), S(35), S(50), S(20), h, NULL, NULL, NULL);
    CreateWindowW(L"EDIT", L"", WS_CHILD | WS_VISIBLE | WS_BORDER, S(100), S(33), S(200), S(25), h, (HMENU)101, NULL, NULL);
    CreateWindowW(L"STATIC", L"密码:", WS_CHILD | WS_VISIBLE, S(40), S(75), S(50), S(20), h, NULL, NULL, NULL);
    CreateWindowW(L"EDIT", L"", WS_CHILD | WS_VISIBLE | WS_BORDER | ES_PASSWORD, S(100), S(73), S(200), S(25), h, (HMENU)102, NULL, NULL);
    CreateWindowW(L"BUTTON", L"保存密码", WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX, S(100), S(110), S(90), S(20), h, (HMENU)103, NULL, NULL);
    CreateWindowW(L"BUTTON", L"自动登录", WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX, S(200), S(110), S(90), S(20), h, (HMENU)104, NULL, NULL);
    CreateWindowW(L"BUTTON", L"登录", WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON, S(125), S(145), S(100), S(35), h, (HMENU)IDOK, NULL, NULL);
    EnumChildWindows(h, [](HWND ch, LPARAM p){ SendMessage(ch, WM_SETFONT, p, TRUE); return TRUE; }, (LPARAM)hF);

    WCHAR savedEmail[128] = {0}, savedPass[128] = {0}; bool savePass = false, autoLogin = false;
    LoadLoginConfig(savedEmail, savedPass, savePass, autoLogin);

    // 无论是否保存了密码，只要有保存的邮箱就填入（方便用户手动输入密码）
    if (savedEmail[0] != L'\0') {
        SetDlgItemTextW(h, 101, savedEmail);
    }
    if (savePass) {
        // 保存了密码：填入密码并勾选"保存密码"
        SetDlgItemTextW(h, 102, savedPass);
        SendDlgItemMessage(h, 103, BM_SETCHECK, BST_CHECKED, 0);
    }
    if (autoLogin) {
        // 勾选自动登录（无论是否手动退出，都恢复勾选状态供用户确认）
        SendDlgItemMessage(h, 104, BM_SETCHECK, BST_CHECKED, 0);
        // 非手动退出时（如程序自启）才执行自动登录
        if (!isManualLogout && savePass) {
            if (ExecuteLoginWithRetry(h, savedEmail, savedPass)) {
                g_LoginSuccess = true;
                DestroyWindow(h);
                return true;
            }
            // 自动登录失败：不闪退，继续显示登录窗口让用户手动登录
            // g_LoginSuccess 保持 false，窗口继续显示
        }
    }

    ShowWindow(h, SW_SHOW); UpdateWindow(h);
    MSG m;
    while(GetMessage(&m, NULL, 0, 0) > 0) {
        TranslateMessage(&m);
        DispatchMessage(&m);
        if(!IsWindow(h)) break;
    }
    if (m.message == WM_QUIT) PostQuitMessage((int)m.wParam);
    return g_LoginSuccess;
}