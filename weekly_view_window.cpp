#include "weekly_view_window.h"
#include "common.h"
#include "utils.h"
#include "api.h" // 确保能调用 ApiFetchCourses() 和 SyncData()
#include <gdiplus.h>
#include <vector>
#include <map>
#include <string>
#include <ctime>
#include <thread>
#include <algorithm>

using namespace Gdiplus;

// 声明外部 API 函数
extern void ApiFetchCourses();
extern void SyncData();
extern void LoadLocalCourses();

// --- 内部状态 ---
static HWND s_hWeeklyWnd = NULL;
static int s_ScrollY = 0;
static int s_MaxScrollY = 0;
static int s_CurrentWeekIndex = 1;
static int s_CurrentWeekOffset = 0;
static int s_ViewMode = 0;
static bool s_LocalCoursesLoaded = false;

// 交互区域记录
struct HitZoneControl {
    RectF rect;
    int id;
    // 1: 同步, 2: 上一周, 3: 下一周, 4: 切换视图, 5: 回到第1周
    // 100-106: 置顶全天待办胶囊
    // 2000+: 课程详情 (id - 2000 为 g_Courses 的索引)
    // 3000+: 待办详情 (id - 3000 为 g_Todos 的索引)
};
static std::vector<HitZoneControl> s_ControlsZones;

// 携带原始索引的待办结构，用于精准映射点击事件
struct TodoItemData {
    Todo todo;
    int originalIndex;
};

// 增强的时间解析器
static bool ParseDateTimeToTM(const std::wstring& str, tm& out_tm) {
    int y, m, d, hr = 0, min = 0, sec = 0;
    int count = 0;

    if (str.find(L"T") != std::wstring::npos) {
        count = swscanf(str.c_str(), L"%d-%d-%dT%d:%d:%d", &y, &m, &d, &hr, &min, &sec);
        if (count < 5) count = swscanf(str.c_str(), L"%d-%d-%dT%d:%d", &y, &m, &d, &hr, &min);
    } else {
        count = swscanf(str.c_str(), L"%d-%d-%d %d:%d:%d", &y, &m, &d, &hr, &min, &sec);
        if (count < 5) count = swscanf(str.c_str(), L"%d-%d-%d %d:%d", &y, &m, &d, &hr, &min);
    }

    if (count < 3) return false;

    out_tm = {0};
    out_tm.tm_year = y - 1900;
    out_tm.tm_mon = m - 1;
    out_tm.tm_mday = d;

    if (count >= 5) {
        out_tm.tm_hour = hr;
        out_tm.tm_min = min;
    } else {
        out_tm.tm_hour = 0;
        out_tm.tm_min = 0;
    }
    out_tm.tm_isdst = -1;
    return true;
}

// 辅助：获取课程的哈希颜色
static Color GetCourseColor(const std::wstring& name) {
    std::vector<Color> colors = {
        Color(240, 100, 149, 237), // Blue
        Color(240, 255, 152, 0),   // Orange
        Color(240, 156, 39, 176),  // Purple
        Color(240, 77, 208, 225),  // Teal
        Color(240, 255, 64, 129),  // Pink
        Color(240, 92, 107, 192),  // Indigo
        Color(240, 102, 187, 106), // Green
        Color(240, 255, 112, 67)   // Deep Orange
    };
    size_t hash = std::hash<std::wstring>{}(name);
    return colors[hash % colors.size()];
}

// 辅助：格式化时间为 HH:MM
static std::wstring FormatTimeHHMM(int timeInt) {
    int h = timeInt / 100;
    int m = timeInt % 100;
    wchar_t buf[16];
    swprintf_s(buf, L"%02d:%02d", h, m);
    return std::wstring(buf);
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

// 🚀 核心重构：匹配超大号字体的 Y 轴偏移量计算
static float GetYOffsetByTime(int hour, int minute, int viewMode) {
    if (hour < 8) return 0.0f;

    // 再次放大网格基础高度，适配更清晰的展示
    float cellHeight = S(80);
    float breakHeight = S(30);
    float shortBreakHeight = (viewMode == 1) ? 0.0f : S(15);

    float maxPossibleY = 11 * cellHeight + 2 * breakHeight + 8 * shortBreakHeight;
    if (hour >= 22) return maxPossibleY;

    int totalMins = (hour - 8) * 60 + minute;
    float yOffset = 0.0f;

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

    int minsAfterNoon = totalMins - 240;
    if (minsAfterNoon <= 120) return yOffset + (minsAfterNoon / 120.0f) * breakHeight;
    yOffset += breakHeight;

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

    int minsAfterAfternoon = totalMins - 590;
    if (minsAfterAfternoon <= 70) return yOffset + (minsAfterAfternoon / 70.0f) * breakHeight;
    yOffset += breakHeight;

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
        g.Clear(Color(255, 245, 245, 245));

        s_ControlsZones.clear();

        // 🚀 进一步放大 UI 布局
        float timeColWidth = S(55);
        float cellWidth = (width - timeColWidth) / 7.0f;
        float controlHeight = S(50);
        float headerHeight = S(50);

        float cellH = S(80); // 行高加到80
        float breakH = S(30);
        float shortBreakH = (s_ViewMode == 1) ? 0.0f : S(15);
        float totalGridHeight = 11 * cellH + 2 * breakH + 8 * shortBreakH;

        // 🚀 字号继续拔高
        FontFamily ff(L"MiSans");
        Font fontBold(&ff, (REAL)S(15), FontStyleBold, UnitPixel);      // 原 14
        Font fontNormal(&ff, (REAL)S(14), FontStyleRegular, UnitPixel); // 原 13
        Font fontSmall(&ff, (REAL)S(13), FontStyleRegular, UnitPixel);  // 原 12
        Font fontTiny(&ff, (REAL)S(12), FontStyleRegular, UnitPixel);   // 原 11

        SolidBrush textBrush(Color(255, 50, 50, 50));
        SolidBrush lightTextBrush(Color(255, 120, 120, 120));
        Pen linePen(Color(255, 220, 220, 220), 1.0f);

        StringFormat fmtCenter;
        fmtCenter.SetAlignment(StringAlignmentCenter);
        fmtCenter.SetLineAlignment(StringAlignmentCenter);

        // --- 0. 解析日期与区分全天/日内待办 ---
        time_t nowTime = time(nullptr);
        tm now_tm; localtime_s(&now_tm, &nowTime);
        now_tm.tm_hour = 0; now_tm.tm_min = 0; now_tm.tm_sec = 0;
        time_t todayStart = mktime(&now_tm);

        int wd = now_tm.tm_wday == 0 ? 7 : now_tm.tm_wday;
        time_t mondayTime = todayStart - (wd - 1) * 86400 + (s_CurrentWeekOffset * 7 * 86400);

        std::vector<TodoItemData> allDayTodos[7];
        std::vector<TodoItemData> intraDayTodos[7];
        bool hasAnyAllDay = false;

        {
            std::lock_guard<std::recursive_mutex> lock(g_DataMutex);
            for (size_t tIdx = 0; tIdx < g_Todos.size(); ++tIdx) {
                const auto& todo = g_Todos[tIdx];
                if (todo.createdDate.empty()) continue;

                tm startTm = {0}, endTm = {0};
                if (!ParseDateTimeToTM(todo.createdDate, startTm)) continue;
                time_t startT = mktime(&startTm);
                time_t endT = startT + 3600;

                if (!todo.dueDate.empty() && ParseDateTimeToTM(todo.dueDate, endTm)) {
                    endT = mktime(&endTm);
                } else {
                    localtime_s(&endTm, &endT);
                }

                bool isAllDayFlag = (!todo.dueDate.empty() && startTm.tm_hour == 0 && startTm.tm_min == 0 && endTm.tm_hour == 23 && endTm.tm_min == 59);
                bool isCrossDay = (startTm.tm_year != endTm.tm_year || startTm.tm_mon != endTm.tm_mon || startTm.tm_mday != endTm.tm_mday);
                bool treatAsAllDay = isAllDayFlag || isCrossDay;

                for (int i = 0; i < 7; i++) {
                    time_t dayStart = mondayTime + i * 86400;
                    time_t dayEnd = dayStart + 86399;

                    if (startT <= dayEnd && endT >= dayStart) {
                        TodoItemData itemData = {todo, (int)tIdx};
                        if (treatAsAllDay) {
                            allDayTodos[i].push_back(itemData);
                            hasAnyAllDay = true;
                        } else {
                            intraDayTodos[i].push_back(itemData);
                        }
                    }
                }
            }
        }

        float allDayBannerHeight = (hasAnyAllDay && s_ViewMode != 1) ? S(35) : 0.0f; // 置顶高度同步拉高
        float fixedTotalHeight = controlHeight + headerHeight + allDayBannerHeight;

        // 更新滚动条参数
        s_MaxScrollY = (int)(totalGridHeight + fixedTotalHeight - height + S(20));
        if (s_MaxScrollY < 0) s_MaxScrollY = 0;
        if (s_ScrollY > s_MaxScrollY) s_ScrollY = s_MaxScrollY;

        SCROLLINFO si = { sizeof(si), SIF_RANGE | SIF_PAGE | SIF_POS };
        si.nMin = 0; si.nMax = (int)(totalGridHeight + fixedTotalHeight + S(20));
        si.nPage = height; si.nPos = s_ScrollY;
        SetScrollInfo(hWnd, SB_VERT, &si, TRUE);


        // --- 1. 绘制顶部控制栏 ---
        SolidBrush topBarBg(Color(255, 250, 250, 250));
        g.FillRectangle(&topBarBg, 0.0f, 0.0f, (float)width, controlHeight);
        SolidBrush btnBg(Color(255, 230, 230, 230));

        RectF btnSync(S(15), S(10), S(75), S(30));
        GraphicsPath pathSync; AddRoundRect(pathSync, btnSync, S(4));
        g.FillPath(&btnBg, &pathSync);
        g.DrawString(L"🔄 同步", -1, &fontSmall, btnSync, &fmtCenter, &textBrush);
        s_ControlsZones.push_back({btnSync, 1});

        RectF btnPrev(width / 2.0f - S(80), S(10), S(35), S(30));
        GraphicsPath pathPrev; AddRoundRect(pathPrev, btnPrev, S(4));
        g.FillPath(&btnBg, &pathPrev);
        g.DrawString(L"<", -1, &fontBold, btnPrev, &fmtCenter, &textBrush);
        s_ControlsZones.push_back({btnPrev, 2});

        RectF lblWeek(width / 2.0f - S(40), S(10), S(80), S(30));
        std::wstring weekStr = L"第 " + std::to_wstring(s_CurrentWeekIndex) + L" 周";
        g.DrawString(weekStr.c_str(), -1, &fontBold, lblWeek, &fmtCenter, &textBrush);
        s_ControlsZones.push_back({lblWeek, 5});

        RectF btnNext(width / 2.0f + S(45), S(10), S(35), S(30));
        GraphicsPath pathNext; AddRoundRect(pathNext, btnNext, S(4));
        g.FillPath(&btnBg, &pathNext);
        g.DrawString(L">", -1, &fontBold, btnNext, &fmtCenter, &textBrush);
        s_ControlsZones.push_back({btnNext, 3});

        RectF btnMode((float)width - S(110), S(10), S(95), S(30));
        GraphicsPath pathMode; AddRoundRect(pathMode, btnMode, S(4));
        g.FillPath(&btnBg, &pathMode);
        std::wstring modeStr = s_ViewMode == 0 ? L"👀 混合" : (s_ViewMode == 1 ? L"📚 仅课表" : L"✅ 仅待办");
        g.DrawString(modeStr.c_str(), -1, &fontSmall, btnMode, &fmtCenter, &textBrush);
        s_ControlsZones.push_back({btnMode, 4});

        // --- 2. 绘制固定的星期表头 ---
        SolidBrush headerBgBrush(Color(255, 255, 255, 255));
        g.FillRectangle(&headerBgBrush, 0.0f, controlHeight, (float)width, headerHeight);

        const wchar_t* weekNames[] = {L"周一", L"周二", L"周三", L"周四", L"周五", L"周六", L"周日"};
        for (int i = 0; i < 7; i++) {
            time_t dayT = mondayTime + i * 86400;
            tm dt; localtime_s(&dt, &dayT);
            wchar_t dateBuf[32]; swprintf_s(dateBuf, L"%d/%02d", dt.tm_mon + 1, dt.tm_mday);

            float x = timeColWidth + i * cellWidth;
            StringFormat format; format.SetAlignment(StringAlignmentCenter);
            g.DrawString(weekNames[i], -1, &fontBold, RectF(x, controlHeight + S(8), cellWidth, S(15)), &format, &textBrush);
            g.DrawString(dateBuf, -1, &fontSmall, RectF(x, controlHeight + S(28), cellWidth, S(15)), &format, &lightTextBrush);
        }

        // --- 3. 绘制全天/跨天事件置顶横幅 ---
        if (hasAnyAllDay && s_ViewMode != 1) {
            g.FillRectangle(&headerBgBrush, 0.0f, controlHeight + headerHeight, (float)width, allDayBannerHeight);

            for (int i = 0; i < 7; ++i) {
                if (allDayTodos[i].empty()) continue;

                bool allDone = true;
                for (const auto& item : allDayTodos[i]) {
                    if (!item.todo.isDone) { allDone = false; break; }
                }

                float px = timeColWidth + i * cellWidth + S(2);
                float py = controlHeight + headerHeight + S(2);
                float pw = cellWidth - S(4);
                float ph = allDayBannerHeight - S(6);

                RectF pillRect(px, py, pw, ph);
                GraphicsPath path; AddRoundRect(path, pillRect, S(4));

                SolidBrush pillBg(allDone ? Color(128, 76, 175, 80) : Color(220, 255, 152, 0));
                g.FillPath(&pillBg, &path);

                std::wstring txt = allDayTodos[i].size() == 1 ? allDayTodos[i][0].todo.content : (std::to_wstring(allDayTodos[i].size()) + L"项全天");

                StringFormat sf;
                sf.SetAlignment(StringAlignmentCenter);
                sf.SetLineAlignment(StringAlignmentCenter);
                sf.SetTrimming(StringTrimmingEllipsisCharacter);

                SolidBrush txtColor(Color(255, 255, 255, 255));
                Font fPill(&ff, (REAL)S(12), allDone ? FontStyleStrikeout : FontStyleRegular, UnitPixel);
                g.DrawString(txt.c_str(), -1, &fPill, pillRect, &sf, &txtColor);

                // 给这些大胶囊增加交互
                s_ControlsZones.push_back({pillRect, 100 + i});
            }
        }
        g.DrawLine(&linePen, 0.0f, fixedTotalHeight, (float)width, fixedTotalHeight);

        // --- 开始裁剪与滚动偏移 (针对网格内容) ---
        g.SetClip(RectF(0, fixedTotalHeight, (float)width, (float)height - fixedTotalHeight));
        g.TranslateTransform(0, fixedTotalHeight - s_ScrollY);

        // --- 4. 绘制时间网格背景 ---
        float currentY = 0.0f;
        const wchar_t* periodTimes[] = {L"08:00\n08:50", L"09:00\n09:50", L"10:10\n11:00", L"11:10\n12:00",
                                        L"14:00\n14:50", L"15:00\n15:50", L"16:00\n16:50", L"17:00\n17:50",
                                        L"19:00\n19:50", L"20:00\n20:50", L"21:00\n21:50"};

        SolidBrush breakBgBrush(Color(255, 240, 240, 240));
        SolidBrush shortBreakBgBrush(Color(255, 248, 248, 248));

        for (int i = 1; i <= 11; i++) {
            g.DrawLine(&linePen, 0.0f, currentY + cellH, (float)width, currentY + cellH);

            wchar_t numBuf[8]; swprintf_s(numBuf, L"%d", i);
            g.DrawString(numBuf, -1, &fontBold, RectF(0, currentY + S(15), timeColWidth, S(20)), &fmtCenter, &textBrush);
            g.DrawString(periodTimes[i-1], -1, &fontSmall, RectF(0, currentY + S(38), timeColWidth, S(35)), &fmtCenter, &lightTextBrush); // 拉长 Y 轴

            currentY += cellH;

            if (i == 4) {
                g.FillRectangle(&breakBgBrush, 0.0f, currentY, (float)width, breakH);
                g.DrawString(L"午休", -1, &fontSmall, RectF(0, currentY, (float)width, breakH), &fmtCenter, &lightTextBrush);
                currentY += breakH;
            } else if (i == 8) {
                g.FillRectangle(&breakBgBrush, 0.0f, currentY, (float)width, breakH);
                g.DrawString(L"晚休", -1, &fontSmall, RectF(0, currentY, (float)width, breakH), &fmtCenter, &lightTextBrush);
                currentY += breakH;
            } else if (i != 11 && shortBreakH > 0) {
                g.FillRectangle(&shortBreakBgBrush, 0.0f, currentY, (float)width, shortBreakH);
                currentY += shortBreakH;
            }
        }

        for (int i = 0; i <= 7; i++) {
            g.DrawLine(&linePen, timeColWidth + i * cellWidth, 0.0f, timeColWidth + i * cellWidth, totalGridHeight);
        }

        std::lock_guard<std::recursive_mutex> lock(g_DataMutex);
        std::map<std::wstring, int> collisionMap;

        // --- 5. 绘制真实课程表 ---
        if (s_ViewMode != 2) {
            std::map<std::wstring, bool> drawnCourses;
            for (size_t k = 0; k < g_Courses.size(); ++k) {
                const auto& course = g_Courses[k];
                if (course.weekIndex != s_CurrentWeekIndex) continue;

                std::wstring cKey = std::to_wstring(course.weekday) + L"_" + std::to_wstring(course.startTime) + L"_" + course.courseName;
                if (drawnCourses[cKey]) continue;
                drawnCourses[cKey] = true;

                int startHour = course.startTime / 100;
                int startMin = course.startTime % 100;
                int endHour = course.endTime / 100;
                int endMin = course.endTime % 100;

                float top = GetYOffsetByTime(startHour, startMin, s_ViewMode);
                float bottom = GetYOffsetByTime(endHour, endMin, s_ViewMode);
                float height = bottom - top;
                if (height < cellH / 2.0f) height = cellH / 2.0f;

                float finalLeft = timeColWidth + (course.weekday - 1) * cellWidth + 2.0f;
                float finalWidth = cellWidth - 4.0f;

                RectF rect(finalLeft, top + 2.0f, finalWidth, height - 4.0f);
                GraphicsPath path; AddRoundRect(path, rect, S(6));

                SolidBrush bg(GetCourseColor(course.courseName));
                g.FillPath(&bg, &path);

                SolidBrush whiteBrush(Color(255, 255, 255, 255));
                StringFormat textFmt;
                textFmt.SetTrimming(StringTrimmingEllipsisCharacter);

                std::wstring dispText = course.courseName;
                if (!course.roomName.empty() && course.roomName != L"未知教室") {
                    if (height > cellH * 1.5f) {
                        dispText += L"\n@" + course.roomName;
                    } else {
                        dispText += L" @" + course.roomName;
                    }
                }

                RectF textRect(rect.X + 4, rect.Y + 4, rect.Width - 8, rect.Height - 8);
                g.DrawString(dispText.c_str(), -1, &fontTiny, textRect, &textFmt, &whiteBrush);

                s_ControlsZones.push_back({rect, 2000 + (int)k});
            }
        }

        // --- 6. 🚀 绘制网格日内待办事项 ---
        if (s_ViewMode != 1) {
            SolidBrush todoTextBrush(Color(255, 255, 255, 255));

            for (int i = 0; i < 7; i++) {
                for (const auto& item : intraDayTodos[i]) {
                    const auto& todo = item.todo;

                    tm startTm, endTm;
                    ParseDateTimeToTM(todo.createdDate, startTm);
                    time_t startT = mktime(&startTm);
                    time_t endT = startT + 3600;
                    if (!todo.dueDate.empty() && ParseDateTimeToTM(todo.dueDate, endTm)) {
                        endT = mktime(&endTm);
                    } else {
                        localtime_s(&endTm, &endT);
                    }

                    time_t dayStart = mondayTime + i * 86400;
                    time_t dayEnd = dayStart + 86399;

                    int renderStartHour = 8, renderStartMin = 0;
                    int renderEndHour = 22, renderEndMin = 0;

                    if (startT >= dayStart) {
                        renderStartHour = startTm.tm_hour;
                        renderStartMin = startTm.tm_min;
                    }
                    if (endT <= dayEnd) {
                        renderEndHour = endTm.tm_hour;
                        renderEndMin = endTm.tm_min;
                    }

                    float top = GetYOffsetByTime(renderStartHour, renderStartMin, s_ViewMode);
                    float bottom = GetYOffsetByTime(renderEndHour, renderEndMin, s_ViewMode);
                    float height = bottom - top;
                    if (height < cellH / 2.0f) height = cellH / 2.0f;

                    std::wstring colKey = std::to_wstring(i) + L"_" + std::to_wstring((int)(top / 10));
                    int stackIdx = collisionMap[colKey]++;

                    float finalLeft = timeColWidth + i * cellWidth + 2.0f;
                    float finalWidth = cellWidth - 4.0f;

                    if (stackIdx > 0) {
                        finalLeft += 8.0f * stackIdx;
                        finalWidth -= 8.0f * stackIdx;
                    }

                    RectF rect(finalLeft, top + 1.0f, finalWidth, height - 2.0f);
                    GraphicsPath path; AddRoundRect(path, rect, S(4));

                    SolidBrush todoBg(todo.isDone ? Color(128, 76, 175, 80) : Color(220, 255, 152, 0));
                    g.FillPath(&todoBg, &path);

                    if (stackIdx > 0) {
                        Pen shadowPen(Color(50, 0, 0, 0), 1.0f);
                        g.DrawPath(&shadowPen, &path);
                    }

                    StringFormat ellipsisFmt;
                    ellipsisFmt.SetTrimming(StringTrimmingEllipsisCharacter);
                    Font fSmallStrike(&ff, (REAL)S(12), todo.isDone ? FontStyleStrikeout : FontStyleRegular, UnitPixel);

                    g.DrawString(todo.content.c_str(), -1, &fSmallStrike, RectF(rect.X + 4, rect.Y + 4, rect.Width - 8, rect.Height - 8), &ellipsisFmt, &todoTextBrush);

                    // 🚀 为网格里的待办注册点击区域 (以 3000 为基础)
                    s_ControlsZones.push_back({rect, 3000 + item.originalIndex});
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

#define WM_USER_WEEKLY_REFRESH (WM_USER + 101)

static LRESULT CALLBACK WeeklyWndProc(HWND hWnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_PAINT:
            RenderWeeklyView(hWnd);
            break;
        case WM_USER_WEEKLY_REFRESH:
            InvalidateRect(hWnd, NULL, FALSE);
            break;
        case WM_LBUTTONDOWN: {
            int x = LOWORD(lp);
            int y = HIWORD(lp);

            for (const auto& z : s_ControlsZones) {
                float hitY = (float)y;
                // 注意：如果 id >= 2000，表示它是随网格滚动的元素，需要加上 ScrollY
                if (z.id >= 2000) {
                    hitY += s_ScrollY - (S(50) + S(45) + S(35)); // 对应控制栏+表头+全天胶囊的最大预估高度偏移
                }

                // 准确命中判定（如果是滚动区域内，要补偿精准的裁剪高度）
                bool isHit = false;
                if (z.id >= 2000) {
                    // 对于跟随滚动的项，其保存的 rect.Y 是从网格最顶端算起的绝对 Y。
                    // 实际画在屏幕上时经过了 TranslateTransform(0, fixedTotalHeight - ScrollY)
                    // 所以真实的屏幕 y = rect.Y + fixedTotalHeight - ScrollY
                    // 所以鼠标的 hitY 应该是：y - fixedTotalHeight + ScrollY

                    float fixedH = S(50) + S(45);
                    std::lock_guard<std::recursive_mutex> lock(g_DataMutex);
                    // 简单判断一下是否有全天横幅，恢复精准偏移计算
                    bool hasAllDay = false;
                    for (const auto& t : g_Todos) {
                        if (t.dueDate.find(L"T") != std::wstring::npos || t.dueDate.find(L":59") != std::wstring::npos) hasAllDay = true;
                    }
                    if (hasAllDay && s_ViewMode != 1) fixedH += S(35);

                    float preciseHitY = y - fixedH + s_ScrollY;
                    if (preciseHitY >= z.rect.Y && preciseHitY <= z.rect.Y + z.rect.Height &&
                        x >= z.rect.X && x <= z.rect.X + z.rect.Width &&
                        y >= fixedH) { // 必须点在网格区域内才算
                        isHit = true;
                    }
                } else {
                    // 顶部固定控件
                    isHit = z.rect.Contains((REAL)x, (REAL)y);
                }


                if (isHit) {

                    if (z.id >= 3000) {
                        // 🚀 响应：网格中的局部的普通待办点击
                        int todoIdx = z.id - 3000;
                        std::lock_guard<std::recursive_mutex> lock(g_DataMutex);
                        if (todoIdx >= 0 && todoIdx < g_Todos.size()) {
                            const auto& t = g_Todos[todoIdx];
                            std::wstring status = t.isDone ? L"✅ 已完成" : L"⏳ 进行中";
                            std::wstring msgStr = L"【待办详情】\n\n";
                            msgStr += L"📝 内容: " + t.content + L"\n";
                            msgStr += L"📌 状态: " + status + L"\n";
                            msgStr += L"⏰ 开始: " + (t.createdDate.empty() ? L"无" : t.createdDate) + L"\n";
                            msgStr += L"⏳ 截止: " + (t.dueDate.empty() ? L"无限制" : t.dueDate) + L"\n";

                            MessageBoxW(hWnd, msgStr.c_str(), L"待办信息", MB_OK | MB_ICONINFORMATION);
                        }
                        break;
                    }
                    else if (z.id >= 2000) {
                        // 🚀 响应：课程点击
                        int courseIdx = z.id - 2000;
                        std::lock_guard<std::recursive_mutex> lock(g_DataMutex);
                        if (courseIdx >= 0 && courseIdx < g_Courses.size()) {
                            const auto& c = g_Courses[courseIdx];

                            std::wstring msgStr = L"【课程详情】\n\n";
                            msgStr += L"📖 名称: " + c.courseName + L"\n";
                            msgStr += L"👤 教师: " + (c.teacherName.empty() ? L"未知" : c.teacherName) + L"\n";
                            msgStr += L"📍 地点: " + c.roomName + L"\n";
                            msgStr += L"📅 周次: 第 " + std::to_wstring(c.weekIndex) + L" 周，星期" + std::to_wstring(c.weekday) + L"\n";
                            msgStr += L"⏰ 时间: " + FormatTimeHHMM(c.startTime) + L" - " + FormatTimeHHMM(c.endTime) + L"\n";
                            if (!c.lessonType.empty()) {
                                msgStr += L"🏷️ 类型: " + (c.lessonType == L"EXPERIMENT" ? L"实验课" : c.lessonType) + L"\n";
                            }

                            MessageBoxW(hWnd, msgStr.c_str(), L"课程信息", MB_OK | MB_ICONINFORMATION);
                        }
                        break;
                    }
                    else if (z.id >= 100 && z.id < 107) {
                        // 🚀 响应：全天/跨天置顶胶囊的点击
                        int dayOffset = z.id - 100;

                        time_t nowTime = time(nullptr);
                        tm now_tm; localtime_s(&now_tm, &nowTime);
                        now_tm.tm_hour = 0; now_tm.tm_min = 0; now_tm.tm_sec = 0;
                        time_t todayStart = mktime(&now_tm);
                        int wd = now_tm.tm_wday == 0 ? 7 : now_tm.tm_wday;
                        time_t mondayTime = todayStart - (wd - 1) * 86400 + (s_CurrentWeekOffset * 7 * 86400);

                        time_t targetDayStart = mondayTime + dayOffset * 86400;
                        time_t targetDayEnd = targetDayStart + 86399;

                        std::wstring msgStr = L"📅 当日全天/跨天待办：\n";
                        msgStr += L"--------------------------------------\n";
                        int matchCount = 0;

                        std::lock_guard<std::recursive_mutex> lock(g_DataMutex);
                        for (const auto& todo : g_Todos) {
                            if (todo.createdDate.empty()) continue;

                            tm startTm = {0}, endTm = {0};
                            if (!ParseDateTimeToTM(todo.createdDate, startTm)) continue;
                            time_t startT = mktime(&startTm);
                            time_t endT = startT + 3600;
                            if (!todo.dueDate.empty() && ParseDateTimeToTM(todo.dueDate, endTm)) {
                                endT = mktime(&endTm);
                            } else {
                                localtime_s(&endTm, &endT);
                            }

                            bool isAllDayFlag = (!todo.dueDate.empty() && startTm.tm_hour == 0 && startTm.tm_min == 0 && endTm.tm_hour == 23 && endTm.tm_min == 59);
                            bool isCrossDay = (startTm.tm_year != endTm.tm_year || startTm.tm_mon != endTm.tm_mon || startTm.tm_mday != endTm.tm_mday);
                            bool treatAsAllDay = isAllDayFlag || isCrossDay;

                            if (treatAsAllDay && startT <= targetDayEnd && endT >= targetDayStart) {
                                msgStr += (todo.isDone ? L"[✅] " : L"[⏳] ") + todo.content + L"\n";
                                msgStr += L"  起点: " + todo.createdDate + L"\n";
                                msgStr += L"  终点: " + (todo.dueDate.empty() ? L"无期限" : todo.dueDate) + L"\n";
                                msgStr += L"--------------------------------------\n";
                                matchCount++;
                            }
                        }

                        if (matchCount > 0) {
                            MessageBoxW(hWnd, msgStr.c_str(), L"全天待办详情", MB_OK | MB_ICONINFORMATION);
                        }
                    }
                    else if (z.id == 1) {
                        std::thread([hWnd]() {
                            ApiFetchCourses();
                            SyncData();
                            PostMessage(hWnd, WM_USER_WEEKLY_REFRESH, 0, 0);
                        }).detach();
                    }
                    else if (z.id == 2) {
                        if (s_CurrentWeekIndex > 1) {
                            s_CurrentWeekIndex--;
                            s_CurrentWeekOffset--;
                            InvalidateRect(hWnd, NULL, FALSE);
                        }
                    }
                    else if (z.id == 3) {
                        s_CurrentWeekIndex++;
                        s_CurrentWeekOffset++;
                        InvalidateRect(hWnd, NULL, FALSE);
                    }
                    else if (z.id == 4) {
                        s_ViewMode = (s_ViewMode + 1) % 3;
                        InvalidateRect(hWnd, NULL, FALSE);
                    }
                    else if (z.id == 5) {
                        s_CurrentWeekIndex = 1;
                        s_CurrentWeekOffset = 0;
                        InvalidateRect(hWnd, NULL, FALSE);
                    }
                    break;
                }
            }
            break;
        }
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
        case WM_SIZE: {
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
    if (!s_LocalCoursesLoaded) {
        LoadLocalCourses();
        s_LocalCoursesLoaded = true;
    }

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

    // 🚀 更宽敞霸气的初始窗口体验
    int width = S(1200);
    int height = S(900);
    int x = (GetSystemMetrics(SM_CXSCREEN) - width) / 2;
    int y = (GetSystemMetrics(SM_CYSCREEN) - height) / 2;

    s_hWeeklyWnd = CreateWindowExW(
        0, L"WeeklyViewClass", L"课表与待办 (周视图)",
        (WS_OVERLAPPEDWINDOW | WS_VSCROLL),
        x, y, width, height,
        parent, NULL, GetModuleHandle(NULL), NULL
    );

    ShowWindow(s_hWeeklyWnd, SW_SHOW);
    UpdateWindow(s_hWeeklyWnd);
}