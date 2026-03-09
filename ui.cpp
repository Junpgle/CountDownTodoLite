#include "ui.h"
#include "utils.h"
#include "api.h"
#include "common.h"
#include "ws_pomodoro.h"
#include "stats_window.h"
#include "weekly_view_window.h"
#include "settings_window.h"
#include "pomodoro_window.h"
#include <commctrl.h>
#include <algorithm>
#include <ctime>
#include <thread>
#include <shlwapi.h>
#include "pomodoro_overlay.h"

#pragma comment(lib, "shlwapi.lib")

using namespace Gdiplus;

// 外部声明
extern void CheckForUpdates(bool isManual = false);
extern void ShowStatsWindow(HWND parent);
extern void ShowCompletedTodosWindow(HWND parent);
extern bool ApiFetchUserStatus();

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

        FontFamily& ff = *g_MiSansFamily;
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

        // ══════════════════════════════════════════════════
        // 🚀 远端番茄钟实时感知横幅（置顶，仅当有远端专注时显示）
        // ══════════════════════════════════════════════════
        {
            RemoteFocusState remote = g_RemoteFocus; // 已在 lock 内
            if (remote.active && remote.targetEndMs > 0) {
                long long nowMs = (long long)time(nullptr) * 1000LL;
                long long diffMs = remote.targetEndMs - nowMs;
                // 超时 30 秒自动清除
                if (diffMs < -30000) {
                    g_RemoteFocus = RemoteFocusState{};
                } else {
                    int remainSec = diffMs > 0 ? (int)(diffMs / 1000) : 0;
                    Color bannerBg = remote.isRestPhase
                        ? Color(210, 34, 139, 80)   // 休息: 绿
                        : Color(210, 74, 108, 247);  // 专注: 蓝紫

                    // 圆角矩形背景
                    REAL bx = (REAL)S(10), bw = (REAL)(width - S(20));
                    REAL bh = remote.todoContent.empty() ? (REAL)S(62) : (REAL)S(78);
                    REAL br = (REAL)S(10);
                    GraphicsPath bp;
                    bp.AddArc(bx,          y,         br*2, br*2, 180.0f, 90.0f);
                    bp.AddArc(bx+bw-br*2,  y,         br*2, br*2, 270.0f, 90.0f);
                    bp.AddArc(bx+bw-br*2,  y+bh-br*2, br*2, br*2,   0.0f, 90.0f);
                    bp.AddArc(bx,          y+bh-br*2, br*2, br*2,  90.0f, 90.0f);
                    bp.CloseFigure();
                    SolidBrush bannerBr(bannerBg);
                    g.FillPath(&bannerBr, &bp);

                    Font fBLabel(&ff, (REAL)S(11), FontStyleRegular, UnitPixel);
                    Font fBTime (&ff, (REAL)S(20), FontStyleBold,    UnitPixel);
                    Font fBSub  (&ff, (REAL)S(11), FontStyleRegular, UnitPixel);
                    SolidBrush wBr2(Color(255,255,255,255));
                    SolidBrush wSub(Color(200,255,255,255));

                    StringFormat sfC2, sfL2;
                    sfC2.SetAlignment(StringAlignmentCenter);
                    sfC2.SetLineAlignment(StringAlignmentCenter);
                    sfL2.SetAlignment(StringAlignmentNear);
                    sfL2.SetLineAlignment(StringAlignmentCenter);
                    sfL2.SetTrimming(StringTrimmingEllipsisCharacter);
                    sfL2.SetFormatFlags(StringFormatFlagsNoWrap);

                    // 标签行
                    std::wstring label = remote.isRestPhase ? L"📱 休息中" : L"📱 专注中";
                    std::wstring devShort = remote.sourceDevice.size() > 14
                        ? remote.sourceDevice.substr(0,12) + L".."
                        : remote.sourceDevice;
                    std::wstring labelStr = label + L"  " + devShort;
                    g.DrawString(labelStr.c_str(), -1, &fBLabel,
                        RectF(bx + S(10), y + S(4), bw - S(20), (REAL)S(14)), &sfL2, &wSub);

                    // 倒计时大字
                    wchar_t tcBuf[16];
                    swprintf_s(tcBuf, L"%02d:%02d", remainSec/60, remainSec%60);
                    g.DrawString(tcBuf, -1, &fBTime,
                        RectF(bx, y + S(16), bw, (REAL)S(26)), &sfC2, &wBr2);

                    // 进度条
                    REAL totalSecs = (REAL)(remote.plannedSecs > 0 ? remote.plannedSecs : 1500);
                    REAL elapsed   = totalSecs - (REAL)remainSec;
                    REAL prog      = std::max(0.0f, std::min(1.0f, elapsed / totalSecs));
                    REAL pbx = bx + S(10), pby = y + S(44);
                    REAL pbw = bw - S(20), pbh = (REAL)S(4);
                    SolidBrush pbBg(Color(60,255,255,255));
                    SolidBrush pbFg(Color(255,255,255,255));
                    g.FillRectangle(&pbBg, pbx, pby, pbw, pbh);
                    g.FillRectangle(&pbFg, pbx, pby, pbw * prog, pbh);

                    // 待办内容（可选行）
                    if (!remote.todoContent.empty()) {
                        std::wstring tc = L"📌 " + remote.todoContent;
                        g.DrawString(tc.c_str(), -1, &fBSub,
                            RectF(bx + S(10), y + S(52), bw - S(20), (REAL)S(18)), &sfL2, &wSub);
                    }

                    y += bh + S(6);
                }
            }
        }
        // ══════════════════════════════════════════════════

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

        // ================================================================
        // --- 待办事项板块（对齐手机端三区块逻辑）---
        // ================================================================
        y += S(10);

        // 标题行
        g.DrawString(L"待办事项", -1, &headF, PointF((REAL)S(15), y), &gBrush);
        g.DrawString(L"[已完成]", -1, &headF, PointF((REAL)(width - S(100)), y), &grBrush);
        g_HitZones.push_back({Rect(width - S(105), (int)y, S(50), S(20)), 0, 7});
        g.DrawString(L"[+]", -1, &headF, PointF((REAL)(width - S(40)), y), &grBrush);
        g_HitZones.push_back({Rect(width - S(45), (int)y, S(30), S(20)), 0, 1});
        y += S(22);

        // 当前时间 & 今天0点（必须在 parseDate lambda 之前计算）
        time_t tNow  = time(nullptr);
        struct tm nowTm; localtime_s(&nowTm, &tNow);
        nowTm.tm_hour=0; nowTm.tm_min=0; nowTm.tm_sec=0;
        time_t tTodayStart = mktime(&nowTm);
        time_t tTodayEnd   = tTodayStart + 86399;

        // ── 辅助：把 "YYYY-MM-DD HH:MM" 字符串解析为 time_t；空字符串返回今天0点 ──
        auto parseDate = [&tTodayStart](const std::wstring& s, bool endOfDay) -> time_t {
            if (s.empty()) return endOfDay ? (tTodayStart + 86399) : tTodayStart;
            int yr=0,mo=0,dy=0,hr=0,mn=0;
            int cnt = swscanf(s.c_str(), L"%d-%d-%d %d:%d",&yr,&mo,&dy,&hr,&mn);
            if (cnt < 3) return endOfDay ? (tTodayStart + 86399) : tTodayStart;
            struct tm t={};
            t.tm_year=yr-1900; t.tm_mon=mo-1; t.tm_mday=dy;
            if(cnt>=5){t.tm_hour=hr;t.tm_min=mn;}
            else if(endOfDay){t.tm_hour=23;t.tm_min=59;t.tm_sec=59;}
            t.tm_isdst=-1;
            return mktime(&t);
        };


        // ── 分桶：过期 / 今日 / 未来 ──
        struct TodoBucket { const Todo* t; time_t dueTime; float progress; };
        std::vector<TodoBucket> pastTodos, todayTodos, futureTodos;

        for (const auto& t : g_Todos) {
            if (t.isDone || t.isDeleted) continue;

            time_t tDue = t.dueDate.empty() ? 0 : parseDate(t.dueDate, true);

            // 计算进度
            time_t tStart = parseDate(t.createdDate, false);
            time_t tEnd   = (tDue > 0) ? tDue : (tTodayStart + 86399);
            float prog = 0.f;
            if (tStart < tEnd) {
                if      (tNow <= tStart) prog = 0.f;
                else if (tNow >= tEnd)   prog = 1.f;
                else prog = (float)(tNow - tStart) / (float)(tEnd - tStart);
            }

            TodoBucket bk{&t, tDue, prog};

            if (tDue > 0 && tDue < tTodayStart) {
                pastTodos.push_back(bk);
            } else if (tDue > tTodayEnd) {
                futureTodos.push_back(bk);
            } else {
                // tDue 在今天范围内，或无截止日期
                todayTodos.push_back(bk);
            }
        }

        // 排序：今日 —— 进度高优先，同进度则持续短优先
        std::sort(todayTodos.begin(), todayTodos.end(), [](const TodoBucket& a, const TodoBucket& b){
            if (std::abs(a.progress - b.progress) > 0.001f) return a.progress > b.progress;
            return false;
        });
        // 未来 —— 进度高优先，同进度则截止日近优先
        std::sort(futureTodos.begin(), futureTodos.end(), [](const TodoBucket& a, const TodoBucket& b){
            if (std::abs(a.progress - b.progress) > 0.001f) return a.progress > b.progress;
            if (a.dueTime != b.dueTime) return a.dueTime < b.dueTime;
            return false;
        });
        // 过期 —— 进度高优先
        std::sort(pastTodos.begin(), pastTodos.end(), [](const TodoBucket& a, const TodoBucket& b){
            return a.progress > b.progress;
        });

        // ── 通用绘制单条待办 ──
        // colorStyle: 0=今日(白), 1=未来(灰), 2=过期(红)
        auto drawTodoItem = [&](const TodoBucket& bk, int colorStyle) {
            const Todo& it = *bk.t;
            float prog = bk.progress;

            // 命中区
            g_HitZones.push_back({Rect(S(15), (int)y, width - S(65), S(40)), it.id, 3, it.uuid});
            g_HitZones.push_back({Rect(width - S(45), (int)y, S(35), S(40)), it.id, 5, it.uuid});

            // 勾选框
            Pen checkPen(Color(180, 255, 255, 255), 1.2f);
            g.DrawRectangle(&checkPen, (REAL)S(15), y + S(5), (REAL)S(13), (REAL)S(13));

            // 标题颜色
            Color titleCol;
            switch(colorStyle) {
                case 2:  titleCol = Color(255, 255, 120, 100); break; // 逾期 - 红
                case 1:  titleCol = Color(180, 200, 200, 210); break; // 未来 - 暗灰
                default: titleCol = Color(255, 255, 255, 255); break; // 今日 - 白
            }
            SolidBrush titleBr(titleCol);

            // 滚动截断标题（仅 content 滚动，备注单独处理）
            std::wstring dispContent = it.content;
            const size_t MAX_LEN = 8;
            if (dispContent.length() > MAX_LEN) {
                std::wstring spacer = L"   ";
                std::wstring scrollText = dispContent + spacer;
                size_t textLen = scrollText.length();
                const DWORD PAUSE_MS = 4000;
                const DWORD SPEED_MS = 280;
                DWORD cycle = PAUSE_MS + (DWORD)(textLen * SPEED_MS);
                DWORD cur = GetTickCount() % cycle;
                size_t offset = (cur > PAUSE_MS) ? (cur - PAUSE_MS) / SPEED_MS : 0;
                std::wstring doubled = scrollText + scrollText;
                dispContent = doubled.substr(offset % textLen, MAX_LEN);
            }
            Font itemF(&ff, (REAL)S(13), FontStyleRegular, UnitPixel);
            g.DrawString(dispContent.c_str(), -1, &itemF, PointF((REAL)S(35), y + S(3)), &titleBr);

            // 备注：固定显示在标题右侧空白区，右对齐靠近 [-] 按钮，不参与滚动
            if (!it.remark.empty()) {
                // 可用宽度：标题结束(S(35)+S(8*13)) ~ [-] 左边(width-S(42))
                float remarkX  = (REAL)S(35) + S(MAX_LEN * 13) + S(6); // 标题右侧 6px
                float remarkW  = (REAL)(width - S(42)) - remarkX;
                if (remarkW > S(10)) {
                    Font remarkF(&ff, (REAL)S(11), FontStyleRegular, UnitPixel);
                    SolidBrush remarkBr(Color(180, 255, 210, 100)); // 淡黄色
                    RectF remarkRect(remarkX, y + S(4), remarkW, (REAL)S(16));
                    StringFormat sfRem;
                    sfRem.SetAlignment(StringAlignmentNear);
                    sfRem.SetLineAlignment(StringAlignmentNear);
                    sfRem.SetTrimming(StringTrimmingEllipsisCharacter);
                    sfRem.SetFormatFlags(StringFormatFlagsNoWrap);
                    g.DrawString(it.remark.c_str(), -1, &remarkF, remarkRect, &sfRem, &remarkBr);
                }
            }

            // 删除按钮
            g.DrawString(L"[-]", -1, &headF, PointF((REAL)(width - S(40)), y + S(4)), &rBrush);

            // 进度条（高度 S(4)）
            int barX = S(35), barY = (int)y + S(21);
            int barW = width - S(85);
            SolidBrush barBg(Color(40, 255, 255, 255));
            g.FillRectangle(&barBg, barX, barY, barW, S(4));
            Color barColor;
            if (colorStyle == 2) barColor = Color(220, 255, 90, 70);       // 逾期 - 深红
            else if (prog >= 0.85f) barColor = Color(255, 255, 120, 50);   // 紧迫 - 橙
            else barColor = Color(255, 80, 210, 120);                       // 正常 - 绿
            SolidBrush barFg(barColor);
            g.FillRectangle(&barFg, barX, barY, (int)(barW * prog), S(4));

            // 百分比
            std::wstring pctStr = std::to_wstring((int)(prog * 100)) + L"%";
            SolidBrush pctBr(Color(160, 200, 200, 200));
            g.DrawString(pctStr.c_str(), -1, &dateF, PointF((REAL)(width - S(42)), y + S(18)), &pctBr);

            // 日期标签（右对齐，截止在进度条右侧上方）
            std::wstring dateLabel;
            if (!it.dueDate.empty()) {
                std::wstring duePart = it.dueDate.length() >= 16 ? it.dueDate.substr(5, 11) : it.dueDate;
                if (colorStyle == 2) dateLabel = duePart + L" 逾期";
                else if (colorStyle == 1) {
                    // 计算剩余天数
                    time_t diff = bk.dueTime - tNow;
                    int days = (int)(diff / 86400);
                    dateLabel = duePart + L" +" + std::to_wstring(days) + L"天";
                } else {
                    dateLabel = duePart + L" 今天";
                }
            } else {
                std::wstring startPart = it.createdDate.length() >= 16 ? it.createdDate.substr(5, 11) : it.createdDate;
                dateLabel = L"始于 " + startPart;
            }
            SolidBrush dateBr(Color(130, 200, 200, 200));
            RectF dateRect((REAL)S(35), y + S(28), (REAL)(barW), (REAL)S(14));
            StringFormat sfDate;
            sfDate.SetTrimming(StringTrimmingEllipsisCharacter);
            sfDate.SetFormatFlags(StringFormatFlagsNoWrap);
            g.DrawString(dateLabel.c_str(), -1, &dateF, dateRect, &sfDate, &dateBr);

            y += S(42);
        }; // end drawTodoItem

        bool anyTodo = !pastTodos.empty() || !todayTodos.empty() || !futureTodos.empty();

        if (!anyTodo) {
            g.DrawString(L"暂无待办", -1, &contentF, PointF((REAL)S(15), y), &gBrush);
            y += S(20);
        } else {
            // ── 逾期区块 ──
            if (!pastTodos.empty()) {
                SolidBrush sectionBr(Color(200, 255, 100, 80));
                Font sectionF(&ff, (REAL)S(11), FontStyleRegular, UnitPixel);
                g.DrawString((L"以往逾期 (" + std::to_wstring(pastTodos.size()) + L")").c_str(),
                             -1, &sectionF, PointF((REAL)S(15), y), &sectionBr);
                y += S(18);
                for (const auto& bk : pastTodos) drawTodoItem(bk, 2);
            }

            // ── 今日区块 ──
            if (!todayTodos.empty()) {
                SolidBrush sectionBr(Color(200, 160, 200, 255));
                Font sectionF(&ff, (REAL)S(11), FontStyleRegular, UnitPixel);
                g.DrawString((L"今日待办 (" + std::to_wstring(todayTodos.size()) + L")").c_str(),
                             -1, &sectionF, PointF((REAL)S(15), y), &sectionBr);
                y += S(18);
                for (const auto& bk : todayTodos) drawTodoItem(bk, 0);
            }

            // ── 未来区块 ──
            if (!futureTodos.empty()) {
                SolidBrush sectionBr(Color(180, 140, 160, 180));
                Font sectionF(&ff, (REAL)S(11), FontStyleRegular, UnitPixel);
                g.DrawString((L"未来待办 (" + std::to_wstring(futureTodos.size()) + L")").c_str(),
                             -1, &sectionF, PointF((REAL)S(15), y), &sectionBr);
                y += S(18);
                for (const auto& bk : futureTodos) drawTodoItem(bk, 1);
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

    // 🚀 远端番茄钟横幅高度
    if (g_RemoteFocus.active && g_RemoteFocus.targetEndMs > 0) {
        long long nowMs = (long long)time(nullptr) * 1000LL;
        long long diffMs = g_RemoteFocus.targetEndMs - nowMs;
        if (diffMs >= -30000) {
            int bh = g_RemoteFocus.todoContent.empty() ? S(62) : S(78);
            h += bh + S(6);
        }
    }

    int activeCountdowns = 0;
    for (const auto& c : g_Countdowns) if (c.daysLeft >= 0) activeCountdowns++;
    h += S(30) + (activeCountdowns == 0 ? S(20) : activeCountdowns * S(20));
    int appRows = std::min((int)g_AppUsage.size(), g_TopAppsCount);
    h += S(30) + (appRows == 0 ? S(20) : appRows * S(20));

    // 待办三区块：逾期/今日/未来，各 S(42) + 区块标题 S(18)
    {
        time_t tNow = time(nullptr);
        struct tm nowTm; localtime_s(&nowTm, &tNow);
        nowTm.tm_hour=0; nowTm.tm_min=0; nowTm.tm_sec=0;
        time_t tTodayStart = mktime(&nowTm);
        time_t tTodayEnd   = tTodayStart + 86399;

        int pastCnt=0, todayCnt=0, futureCnt=0;
        for (const auto& t : g_Todos) {
            if (t.isDone || t.isDeleted) continue;
            if (t.dueDate.empty()) { todayCnt++; continue; }
            int yr=0,mo=0,dy=0,hr=0,mn=0;
            swscanf(t.dueDate.c_str(), L"%d-%d-%d %d:%d",&yr,&mo,&dy,&hr,&mn);
            struct tm tm={}; tm.tm_year=yr-1900;tm.tm_mon=mo-1;tm.tm_mday=dy;
            tm.tm_hour=hr;tm.tm_min=mn;tm.tm_sec=59;tm.tm_isdst=-1;
            time_t td = mktime(&tm);
            if (td < tTodayStart) pastCnt++;
            else if (td > tTodayEnd) futureCnt++;
            else todayCnt++;
        }
        int todoH = S(30);
        if (pastCnt > 0)   todoH += S(18) + pastCnt   * S(42);
        if (todayCnt > 0)  todoH += S(18) + todayCnt  * S(42);
        if (futureCnt > 0) todoH += S(18) + futureCnt * S(42);
        if (pastCnt + todayCnt + futureCnt == 0) todoH += S(20);
        h += todoH;
    }

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
                        std::wstring c, d1, d2, rem;
                        if (ShowInputDialog(hWnd, 0, c, d1, d2, rem)) {
                            ApiAddTodo(c, d1, d2, false, rem);
                            SyncData();
                        }
                    }
                    else if (z.type == 2) {
                        std::wstring t, d, dummy, dummy2;
                        if (ShowInputDialog(hWnd, 1, t, d, dummy, dummy2)) {
                            ApiAddCountdown(t, d);
                            SyncData();
                        }
                    }
                    else if (z.type == 3) {
                        if (x < S(30)) {
                            // 勾选框：直接在 g_Todos 里 uuid 精确匹配并就地切换
                            std::wstring matchUuid = z.uuid;
                            int matchId = z.id;
                            {
                                std::lock_guard<std::recursive_mutex> l(g_DataMutex);
                                for (auto &t : g_Todos) {
                                    if ((!matchUuid.empty() && t.uuid == matchUuid) ||
                                        (matchUuid.empty() && t.id == matchId)) {
                                        t.isDone      = !t.isDone;
                                        t.isDirty     = true;
                                        t.lastUpdated = time(nullptr);
                                        break;
                                    }
                                }
                            }
                        } else {
                            // 内容区：编辑待办
                            std::wstring c, d1, d2, rem;
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
                                        rem         = t.remark;
                                        currentDone = t.isDone;
                                        foundUuid   = t.uuid;
                                        foundId     = t.id;
                                        break;
                                    }
                                }
                            }
                            if (ShowInputDialog(hWnd, 0, c, d1, d2, rem)) {
                                ApiUpdateTodo(foundUuid, c, d1, d2, currentDone, rem);
                            }
                        }
                        SyncData();
                    }
                    else if (z.type == 4) {
                        if (MessageBoxW(hWnd, L"确定要删除吗？", L"确认", MB_YESNO) == IDYES) { ApiDeleteCountdown(z.id); SyncData(); }
                    }
                    else if (z.type == 5) {
                        if (MessageBoxW(hWnd, L"确定要删除这条待办吗？", L"确认删除", MB_YESNO | MB_ICONQUESTION) == IDYES) {
                            {
                                std::lock_guard<std::recursive_mutex> l(g_DataMutex);
                                for (auto &t : g_Todos) {
                                    if ((!z.uuid.empty() && t.uuid == z.uuid) || t.id == z.id) {
                                        t.isDeleted   = true;
                                        t.isDirty     = true;
                                        t.lastUpdated = time(nullptr);
                                        break;
                                    }
                                }
                            }
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
            HMENU hMenu = CreatePopupMenu();
            AppendMenuW(hMenu, MF_STRING, 2001, L"⚙  设置");
            AppendMenuW(hMenu, MF_SEPARATOR, 0, NULL);
            AppendMenuW(hMenu, MF_STRING, 2006, L"🍅  番茄钟");
            AppendMenuW(hMenu, MF_STRING, 2007, L"🪟  番茄钟悬浮窗");
            AppendMenuW(hMenu, MF_STRING, 2002, L"📊  屏幕时间统计");
            AppendMenuW(hMenu, MF_STRING, 2003, L"⚡  立即同步");
            AppendMenuW(hMenu, MF_STRING, 2004, L"🔍  检查更新");
            AppendMenuW(hMenu, MF_SEPARATOR, 0, NULL);
            AppendMenuW(hMenu, MF_STRING, 2005, L"退出程序");

            int cmd = TrackPopupMenu(hMenu, TPM_RETURNCMD, pt.x, pt.y, 0, hWnd, NULL);
            DestroyMenu(hMenu);

            if      (cmd == 2001) ShowSettingsWindow(hWnd);
            else if (cmd == 2006) ShowPomodoroWindow(hWnd);
            else if (cmd == 2002) ShowStatsWindow(hWnd);
            else if (cmd == 2003) std::thread([]() { SyncData(); }).detach();
            else if (cmd == 2004) CheckForUpdates(true);
            else if (cmd == 2005) PostQuitMessage(0);
            else if (cmd == 2007) {
                extern void ShowPomodoroOverlay();
                extern void HidePomodoroOverlay();
                // 切换显示/隐藏
                HWND hOverlay = FindWindowW(L"PomodoroOverlay", nullptr);
                if (hOverlay && IsWindowVisible(hOverlay))
                    HidePomodoroOverlay();
                else
                    ShowPomodoroOverlay();
            }
        } break;

        // 来自设置窗口的退出登录请求（id=9001）
        case WM_COMMAND: {
            if (LOWORD(wp) == 9001) {
                WsPomodoroDisconnect(); // 🚀 退出登录时断开 WS
                g_UserId = 0; g_Username = L""; g_AuthToken = L""; g_LoginSuccess = false;
                {
                    std::lock_guard<std::recursive_mutex> lock(g_DataMutex);
                    g_Todos.clear(); g_Countdowns.clear(); g_AppUsage.clear();
                }
                ShowWindow(hWnd, SW_HIDE);
                if (ShowLogin(true)) {
                    WsPomodoroConnect(); // 🚀 重新登录后重连 WS
                    std::thread([]() { SyncData(); }).detach();
                    ShowWindow(hWnd, SW_SHOW);
                } else {
                    PostQuitMessage(0);
                }
            }
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

        // 读取备注框（type==0 时存在）
        HWND hRemark = GetDlgItem(hWnd, 104);
        if (hRemark) {
            WCHAR rem[512] = {}; GetWindowTextW(hRemark, rem, 512);
            InputState::result4 = rem;
        } else {
            InputState::result4.clear();
        }

        InputState::isOk = true; DestroyWindow(hWnd);
    } else if (msg == WM_COMMAND && LOWORD(wp) == IDCANCEL) DestroyWindow(hWnd);
    return DefWindowProc(hWnd, msg, wp, lp);
}

// o4 = remark（仅 type==0 待办对话框使用）
bool ShowInputDialog(HWND parent, int type, std::wstring &o1, std::wstring &o2, std::wstring &o3, std::wstring &o4) {
    InputState::isOk = false;
    InputState::result4.clear();
    WNDCLASSW wc = {0}; wc.lpfnWndProc = InputWndProc; wc.hInstance = GetModuleHandle(NULL);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1); wc.lpszClassName = L"InputDlg"; RegisterClassW(&wc);

    // type==0 待办：内容+开始+截止+备注 → 高度更大
    int dlgHeight = (type == 2) ? S(140) : (type == 0 ? S(320) : S(220));
    HWND hDlg = CreateWindowExW(0, L"InputDlg",
        type == 0 ? L"待办事项" : (type == 1 ? L"日期设置" : L"同步频率"),
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU,
        100, 100, S(380), dlgHeight, parent, NULL, NULL, NULL);

    HFONT hF = GetMiSansFont(14);

    // 行1：内容 / 标题 / 间隔
    CreateWindowW(L"STATIC", type == 2 ? L"间隔(分):" : (type == 0 ? L"内容:" : L"标题:"),
        WS_CHILD | WS_VISIBLE, S(20), S(20), S(75), S(20), hDlg, NULL, NULL, NULL);
    CreateWindowW(L"EDIT", o1.c_str(), WS_CHILD | WS_VISIBLE | WS_BORDER | (type == 2 ? ES_NUMBER : 0),
        S(100), S(18), S(240), S(25), hDlg, (HMENU)101, NULL, NULL);

    if (type != 2) {
        // 行2：开始/目标时间
        CreateWindowW(L"STATIC", type == 0 ? L"开始时间:" : L"目标日期:",
            WS_CHILD | WS_VISIBLE, S(20), S(60), S(75), S(20), hDlg, NULL, NULL, NULL);
        HWND hPicker1 = CreateWindowExW(0, DATETIMEPICK_CLASS, L"", WS_BORDER | WS_CHILD | WS_VISIBLE,
            S(100), S(58), S(240), S(25), hDlg, (HMENU)102, NULL, NULL);
        DateTime_SetFormat(hPicker1, L"yyyy-MM-dd HH:mm");
        SYSTEMTIME stStart;
        StringToSystemTime(o2.empty() ? GetTodayDate() : o2, stStart);
        DateTime_SetSystemtime(hPicker1, GDT_VALID, &stStart);

        if (type == 0) {
            // 行3：截止时间
            CreateWindowW(L"STATIC", L"截止时间:",
                WS_CHILD | WS_VISIBLE, S(20), S(100), S(75), S(20), hDlg, NULL, NULL, NULL);
            HWND hPicker2 = CreateWindowExW(0, DATETIMEPICK_CLASS, L"", WS_BORDER | WS_CHILD | WS_VISIBLE,
                S(100), S(98), S(240), S(25), hDlg, (HMENU)103, NULL, NULL);
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

            // 行4：备注（多行 EDIT）
            CreateWindowW(L"STATIC", L"备注:",
                WS_CHILD | WS_VISIBLE, S(20), S(140), S(75), S(20), hDlg, NULL, NULL, NULL);
            HWND hRemark = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", o4.c_str(),
                WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_MULTILINE | ES_AUTOVSCROLL | ES_WANTRETURN,
                S(100), S(138), S(240), S(60), hDlg, (HMENU)104, NULL, NULL);
            (void)hRemark;
        }
    }

    int btnY = (type == 2) ? S(60) : (type == 0 ? S(218) : S(140));
    CreateWindowW(L"BUTTON", L"确定", WS_CHILD | WS_VISIBLE,
        S(130), btnY, S(110), S(35), hDlg, (HMENU)IDOK, NULL, NULL);

    EnumChildWindows(hDlg, [](HWND h, LPARAM p){ SendMessage(h, WM_SETFONT, p, TRUE); return TRUE; }, (LPARAM)hF);
    ShowWindow(hDlg, SW_SHOW); EnableWindow(parent, FALSE);
    MSG m; while (GetMessage(&m, NULL, 0, 0)) { TranslateMessage(&m); DispatchMessage(&m); if (!IsWindow(hDlg)) break; }
    EnableWindow(parent, TRUE); SetForegroundWindow(parent);
    o1 = InputState::result1; o2 = InputState::result2; o3 = InputState::result3; o4 = InputState::result4;
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