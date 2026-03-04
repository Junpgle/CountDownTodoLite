#include "weekly_view_window.h"
#include "common.h"
#include "utils.h"
#include <gdiplus.h>
#include <vector>
#include <map>
#include <string>
#include <ctime>
#include <algorithm>

using namespace Gdiplus;

// --- 占位：未来如果 PC 端也接入课程数据，可以对接此结构体 ---
struct CourseItem {
    std::wstring courseName;
    std::wstring roomName;
    std::wstring teacherName;
    int startTime;
    int endTime;
    int weekday;
};
static std::vector<CourseItem> s_WeekCourses; // 当前周的课程数据缓存

// --- 内部状态 ---
static HWND s_hWeeklyWnd = NULL;
static int s_ScrollY = 0;
static int s_MaxScrollY = 0;
static int s_CurrentWeekOffset = 0; // 0表示本周，-1上周，1下周
static int s_ViewMode = 0; // 0: 混合, 1: 课表, 2: 待办

// 解析日期字符串为 tm 结构体，用于精确提取小时和分钟
static bool ParseDateTimeToTM(const std::wstring& str, tm& out_tm) {
    int y, m, d, hr = 0, min = 0;
    int count = swscanf(str.c_str(), L"%d-%d-%d %d:%d", &y, &m, &d, &hr, &min);
    if (count < 3) return false;
    out_tm = {0};
    out_tm.tm_year = y - 1900;
    out_tm.tm_mon = m - 1;
    out_tm.tm_mday = d;
    out_tm.tm_hour = hr;
    out_tm.tm_min = min;
    out_tm.tm_isdst = -1;
    return true;
}

// 辅助：构建圆角矩形路径
static void AddRoundRect(GraphicsPath& path, const RectF& r, float radius) {
    float d = radius * 2;
    path.AddArc(r.X, r.Y, d, d, 180, 90);
    path.AddArc(r.X + r.Width - d, r.Y, d, d, 270, 90);
    path.AddArc(r.X + r.Width - d, r.Y + r.Height - d, d, d, 0, 90);
    path.AddArc(r.X, r.Y + r.Height - d, d, d, 90, 90);
    path.CloseFigure();
}

// 🚀 核心重构：精准的基于时间的 Y 轴偏移量计算
static float GetYOffsetByTime(int hour, int minute, int viewMode) {
    if (hour < 8) return 0.0f;
    
    float cellHeight = S(65);
    float breakHeight = S(24);
    float shortBreakHeight = (viewMode == 1) ? 0.0f : S(15);
    
    float maxPossibleY = 11 * cellHeight + 2 * breakHeight + 8 * shortBreakHeight;
    if (hour >= 22) return maxPossibleY;

    int totalMins = (hour - 8) * 60 + minute;
    float yOffset = 0.0f;

    // 上午区域 (08:00 - 12:00) : 240 分钟
    if (totalMins <= 240) {
        if (totalMins <= 50) return (totalMins / 50.0f) * cellHeight;
        else if (totalMins <= 60) return cellHeight + ((totalMins - 50) / 10.0f) * shortBreakHeight;
        else if (totalMins <= 110) return cellHeight + shortBreakHeight + ((totalMins - 60) / 50.0f) * cellHeight;
        else if (totalMins <= 130) return 2 * cellHeight + shortBreakHeight + ((totalMins - 110) / 20.0f) * shortBreakHeight;
        else if (totalMins <= 180) return 2 * cellHeight + 2 * shortBreakHeight + ((totalMins - 130) / 50.0f) * cellHeight;
        else if (totalMins <= 190) return 3 * cellHeight + 2 * shortBreakHeight + ((totalMins - 180) / 10.0f) * shortBreakHeight;
        else return 3 * cellHeight + 3 * shortBreakHeight + ((totalMins - 190) / 50.0f) * cellHeight;
    }
    
    yOffset = 4 * cellHeight + 3 * shortBreakHeight;

    // 午休区域：12:00 - 14:00 (120分钟)
    int minsAfterNoon = totalMins - 240;
    if (minsAfterNoon <= 120) return yOffset + (minsAfterNoon / 120.0f) * breakHeight;

    yOffset += breakHeight;

    // 下午区域 (14:00 - 17:50) : 230 分钟
    int minsAfter2PM = totalMins - 360;
    if (minsAfter2PM <= 230) {
        if (minsAfter2PM <= 50) return yOffset + (minsAfter2PM / 50.0f) * cellHeight;
        else if (minsAfter2PM <= 60) return yOffset + cellHeight + ((minsAfter2PM - 50) / 10.0f) * shortBreakHeight;
        else if (minsAfter2PM <= 110) return yOffset + cellHeight + shortBreakHeight + ((minsAfter2PM - 60) / 50.0f) * cellHeight;
        else if (minsAfter2PM <= 120) return yOffset + 2 * cellHeight + shortBreakHeight + ((minsAfter2PM - 110) / 10.0f) * shortBreakHeight;
        else if (minsAfter2PM <= 170) return yOffset + 2 * cellHeight + 2 * shortBreakHeight + ((minsAfter2PM - 120) / 50.0f) * cellHeight;
        else if (minsAfter2PM <= 180) return yOffset + 3 * cellHeight + 2 * shortBreakHeight + ((minsAfter2PM - 170) / 10.0f) * shortBreakHeight;
        else return yOffset + 3 * cellHeight + 3 * shortBreakHeight + ((minsAfter2PM - 180) / 50.0f) * cellHeight;
    }

    yOffset += 4 * cellHeight + 3 * shortBreakHeight;

    // 晚休区域：17:50 - 19:00 (70分钟)
    int minsAfterAfternoon = totalMins - 590;
    if (minsAfterAfternoon <= 70) return yOffset + (minsAfterAfternoon / 70.0f) * breakHeight;

    yOffset += breakHeight;

    // 晚上区域 (19:00 - 21:50) : 170 分钟
    int minsAfter7PM = totalMins - 660;
    if (minsAfter7PM <= 170) {
        if (minsAfter7PM <= 50) return yOffset + (minsAfter7PM / 50.0f) * cellHeight;
        else if (minsAfter7PM <= 60) return yOffset + cellHeight + ((minsAfter7PM - 50) / 10.0f) * shortBreakHeight;
        else if (minsAfter7PM <= 110) return yOffset + cellHeight + shortBreakHeight + ((minsAfter7PM - 60) / 50.0f) * cellHeight;
        else if (minsAfter7PM <= 120) return yOffset + 2 * cellHeight + shortBreakHeight + ((minsAfter7PM - 110) / 10.0f) * shortBreakHeight;
        else return yOffset + 2 * cellHeight + 2 * shortBreakHeight + ((minsAfter7PM - 120) / 50.0f) * cellHeight;
    }

    return maxPossibleY;
}

// 渲染核心
static void RenderWeeklyView(HWND hWnd) {
    RECT rc; GetClientRect(hWnd, &rc);
    int width = rc.right - rc.left;
    int height = rc.bottom - rc.top;
    if (width <= 0 || height <= 0) return;

    PAINTSTRUCT ps;
    HDC hdc = BeginPaint(hWnd, &ps);
    HDC hdcMem = CreateCompatibleDC(hdc);
    HBITMAP hBitmap = CreateCompatibleBitmap(hdc, width, height);
    HBITMAP hOld = (HBITMAP)SelectObject(hdcMem, hBitmap);

    {
        Graphics g(hdcMem);
        g.SetSmoothingMode(SmoothingModeAntiAlias);
        g.SetTextRenderingHint(TextRenderingHintClearTypeGridFit);
        g.Clear(Color(255, 245, 245, 245)); // 亮色背景

        float timeColWidth = S(45);
        float cellWidth = (width - timeColWidth) / 7.0f;
        float headerHeight = S(40);
        float cellH = S(65);
        float breakH = S(24);
        float shortBreakH = (s_ViewMode == 1) ? 0.0f : S(15);
        float totalGridHeight = 11 * cellH + 2 * breakH + 8 * shortBreakH;

        // 更新滚动条参数
        s_MaxScrollY = (int)(totalGridHeight + headerHeight - height + S(20));
        if (s_MaxScrollY < 0) s_MaxScrollY = 0;
        if (s_ScrollY > s_MaxScrollY) s_ScrollY = s_MaxScrollY;

        SCROLLINFO si = { sizeof(si), SIF_RANGE | SIF_PAGE | SIF_POS };
        si.nMin = 0; si.nMax = (int)(totalGridHeight + headerHeight + S(20));
        si.nPage = height; si.nPos = s_ScrollY;
        SetScrollInfo(hWnd, SB_VERT, &si, TRUE);

        FontFamily ff(L"MiSans");
        Font fontBold(&ff, (REAL)S(12), FontStyleBold, UnitPixel);
        Font fontNormal(&ff, (REAL)S(11), FontStyleRegular, UnitPixel);
        Font fontSmall(&ff, (REAL)S(9), FontStyleRegular, UnitPixel);
        SolidBrush textBrush(Color(255, 50, 50, 50));
        SolidBrush lightTextBrush(Color(255, 120, 120, 120));
        Pen linePen(Color(255, 220, 220, 220), 1.0f);

        // 获取本周一的日期
        time_t now = time(nullptr);
        now += s_CurrentWeekOffset * 7 * 86400; 
        tm now_tm; localtime_s(&now_tm, &now);
        int wd = now_tm.tm_wday == 0 ? 7 : now_tm.tm_wday;
        time_t mondayTime = now - (wd - 1) * 86400;

        // --- 1. 绘制固定的表头 ---
        SolidBrush headerBgBrush(Color(255, 255, 255, 255)); // 🚀 修复 rvalue 取址错误
        g.FillRectangle(&headerBgBrush, 0.0f, 0.0f, (float)width, headerHeight);

        const wchar_t* weekNames[] = {L"周一", L"周二", L"周三", L"周四", L"周五", L"周六", L"周日"};
        for (int i = 0; i < 7; i++) {
            time_t dayT = mondayTime + i * 86400;
            tm dt; localtime_s(&dt, &dayT);
            wchar_t dateBuf[32]; swprintf_s(dateBuf, L"%d/%02d", dt.tm_mon + 1, dt.tm_mday);

            float x = timeColWidth + i * cellWidth;
            StringFormat format;
            format.SetAlignment(StringAlignmentCenter);

            g.DrawString(weekNames[i], -1, &fontBold, RectF(x, S(5), cellWidth, S(15)), &format, &textBrush);
            g.DrawString(dateBuf, -1, &fontSmall, RectF(x, S(20), cellWidth, S(15)), &format, &lightTextBrush);
        }
        g.DrawLine(&linePen, 0.0f, headerHeight, (float)width, headerHeight);

        // 设置裁剪和偏移，开始绘制网格内容
        g.SetClip(RectF(0, headerHeight, (float)width, (float)height - headerHeight));
        g.TranslateTransform(0, headerHeight - s_ScrollY);

        // --- 2. 绘制时间网格背景 ---
        float currentY = 0.0f;
        const wchar_t* periodTimes[] = {L"08:00\n08:50", L"09:00\n09:50", L"10:10\n11:00", L"11:10\n12:00",
                                        L"14:00\n14:50", L"15:00\n15:50", L"16:00\n16:50", L"17:00\n17:50",
                                        L"19:00\n19:50", L"20:00\n20:50", L"21:00\n21:50"};

        StringFormat fmt; // 🚀 修复命名错误
        fmt.SetAlignment(StringAlignmentCenter);
        fmt.SetLineAlignment(StringAlignmentCenter);

        SolidBrush breakBgBrush(Color(255, 240, 240, 240)); // 🚀 修复 rvalue 取址错误
        SolidBrush shortBreakBgBrush(Color(255, 248, 248, 248)); // 🚀 修复 rvalue 取址错误

        for (int i = 1; i <= 11; i++) {
            g.DrawLine(&linePen, 0.0f, currentY + cellH, (float)width, currentY + cellH);

            wchar_t numBuf[8]; swprintf_s(numBuf, L"%d", i);
            g.DrawString(numBuf, -1, &fontBold, RectF(0, currentY + S(10), timeColWidth, S(15)), &fmt, &textBrush);
            g.DrawString(periodTimes[i-1], -1, &fontSmall, RectF(0, currentY + S(25), timeColWidth, S(30)), &fmt, &lightTextBrush);

            currentY += cellH;

            // 绘制课间和午晚休
            if (i == 4) {
                g.FillRectangle(&breakBgBrush, 0.0f, currentY, (float)width, breakH);
                g.DrawString(L"午休", -1, &fontSmall, RectF(0, currentY, (float)width, breakH), &fmt, &lightTextBrush);
                currentY += breakH;
            } else if (i == 8) {
                g.FillRectangle(&breakBgBrush, 0.0f, currentY, (float)width, breakH);
                g.DrawString(L"晚休", -1, &fontSmall, RectF(0, currentY, (float)width, breakH), &fmt, &lightTextBrush);
                currentY += breakH;
            } else if (i != 11 && shortBreakH > 0) {
                g.FillRectangle(&shortBreakBgBrush, 0.0f, currentY, (float)width, shortBreakH);
                currentY += shortBreakH;
            }
        }

        // 竖线
        for (int i = 0; i <= 7; i++) {
            g.DrawLine(&linePen, timeColWidth + i * cellWidth, 0.0f, timeColWidth + i * cellWidth, totalGridHeight);
        }

        // --- 3. 绘制待办事项 (仅在非纯课表模式下) ---
        if (s_ViewMode != 1) {
            std::lock_guard<std::recursive_mutex> lock(g_DataMutex);
            std::map<std::wstring, int> collisionMap; // 防重叠栈

            SolidBrush todoTextBrush(Color(255, 255, 255, 255)); // 🚀 修复 rvalue 取址错误

            for (const auto& todo : g_Todos) {
                if (todo.isDone || todo.createdDate.empty()) continue; // 为简化显示，仅渲染未完成项

                tm startTm, endTm;
                if (!ParseDateTimeToTM(todo.createdDate, startTm)) continue;

                time_t startT = mktime(&startTm);
                time_t endT = startT + 3600; // 默认1小时
                if (!todo.dueDate.empty() && ParseDateTimeToTM(todo.dueDate, endTm)) {
                    endT = mktime(&endTm);
                } else {
                    localtime_s(&endTm, &endT);
                }

                // 检查是否在当前周内
                for (int i = 0; i < 7; i++) {
                    time_t dayStart = mondayTime + i * 86400;
                    time_t dayEnd = dayStart + 86399;

                    if (startT <= dayEnd && endT >= dayStart) {
                        // 局部跨天/全天在顶部栏渲染，这里仅处理日内事件
                        float top = GetYOffsetByTime(startTm.tm_hour, startTm.tm_min, s_ViewMode);
                        float bottom = GetYOffsetByTime(endTm.tm_hour, endTm.tm_min, s_ViewMode);
                        float height = bottom - top;
                        if (height < cellH / 2.0f) height = cellH / 2.0f; // 最小高度

                        std::wstring colKey = std::to_wstring(i) + L"_" + std::to_wstring((int)(top / 10));
                        int stackIdx = collisionMap[colKey]++;

                        float finalLeft = timeColWidth + i * cellWidth + 2.0f;
                        float finalWidth = cellWidth - 4.0f;

                        if (stackIdx > 0) {
                            finalLeft += 6.0f * stackIdx;
                            finalWidth -= 6.0f * stackIdx;
                        }

                        RectF rect(finalLeft, top + 2.0f, finalWidth, height - 4.0f);
                        GraphicsPath path; AddRoundRect(path, rect, S(4));

                        SolidBrush bg(Color(220, 255, 152, 0)); // 琥珀色
                        g.FillPath(&bg, &path);

                        StringFormat ellipsisFmt;
                        ellipsisFmt.SetTrimming(StringTrimmingEllipsisCharacter);

                        g.DrawString(todo.content.c_str(), -1, &fontSmall, RectF(rect.X + 2, rect.Y + 2, rect.Width - 4, rect.Height - 4), &ellipsisFmt, &todoTextBrush);
                    }
                }
            }
        }
    }

    BitBlt(hdc, 0, 0, width, height, hdcMem, 0, 0, SRCCOPY);
    SelectObject(hdcMem, hOld);
    DeleteObject(hBitmap);
    DeleteDC(hdcMem);
    EndPaint(hWnd, &ps);
}

static LRESULT CALLBACK WeeklyWndProc(HWND hWnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_PAINT:
            RenderWeeklyView(hWnd);
            break;
        case WM_MOUSEWHEEL: {
            int zDelta = GET_WHEEL_DELTA_WPARAM(wp);
            s_ScrollY -= zDelta;
            if (s_ScrollY < 0) s_ScrollY = 0;
            if (s_ScrollY > s_MaxScrollY) s_ScrollY = s_MaxScrollY;
            InvalidateRect(hWnd, NULL, FALSE);
            break;
        }
        case WM_VSCROLL: {
            switch (LOWORD(wp)) {
                case SB_LINEUP: s_ScrollY -= 30; break;
                case SB_LINEDOWN: s_ScrollY += 30; break;
                case SB_PAGEUP: s_ScrollY -= 150; break;
                case SB_PAGEDOWN: s_ScrollY += 150; break;
                case SB_THUMBTRACK: s_ScrollY = HIWORD(wp); break;
            }
            if (s_ScrollY < 0) s_ScrollY = 0;
            if (s_ScrollY > s_MaxScrollY) s_ScrollY = s_MaxScrollY;
            InvalidateRect(hWnd, NULL, FALSE);
            break;
        }
        case WM_DESTROY:
            s_hWeeklyWnd = NULL;
            break;
        default:
            return DefWindowProc(hWnd, msg, wp, lp);
    }
    return 0;
}

void ShowWeeklyViewWindow(HWND parent) {
    if (s_hWeeklyWnd) {
        SetForegroundWindow(s_hWeeklyWnd);
        return;
    }

    WNDCLASSW wc = {0};
    wc.lpfnWndProc = WeeklyWndProc;
    wc.hInstance = GetModuleHandle(NULL);
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.lpszClassName = L"WeeklyViewClass";
    RegisterClassW(&wc);

    int width = S(800);
    int height = S(600);
    int x = (GetSystemMetrics(SM_CXSCREEN) - width) / 2;
    int y = (GetSystemMetrics(SM_CYSCREEN) - height) / 2;

    s_hWeeklyWnd = CreateWindowExW(
        0, L"WeeklyViewClass", L"课表与待办 (周视图)",
        WS_OVERLAPPEDWINDOW | WS_VSCROLL,
        x, y, width, height,
        parent, NULL, GetModuleHandle(NULL), NULL
    );

    ShowWindow(s_hWeeklyWnd, SW_SHOW);
    UpdateWindow(s_hWeeklyWnd);
}