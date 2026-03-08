/**
 * pomodoro_stats_window.cpp
 * 番茄钟统计分析界面
 * 支持按 年 / 月 / 日 / 标签 四个维度切换查看
 */
#include "pomodoro_stats_window.h"
#include "common.h"
#include "api.h"
#include <thread>
#include <string>
#include <vector>
#include <map>
#include <algorithm>
#include <ctime>
#include <numeric>
using namespace Gdiplus;

// ============================================================
// 颜色
// ============================================================
static const Color S_BG     = Color(255, 245, 247, 250);
static const Color S_CARD   = Color(255, 255, 255, 255);
static const Color S_ACCENT = Color(255, 239,  83,  80);
static const Color S_GREEN  = Color(255,  52, 199, 120);
static const Color S_BLUE   = Color(255,  74, 108, 247);
static const Color S_TEXT   = Color(255,  30,  40,  60);
static const Color S_SUB    = Color(255, 130, 140, 160);
static const Color S_BORDER = Color(255, 220, 225, 235);

// ============================================================
// 窗口状态
// ============================================================
static HWND s_hWnd    = NULL;
static HWND s_hParent = NULL;

// 视图模式
enum class StatsView { Year, Month, Day, Tag };
static StatsView s_View = StatsView::Month;

// 当前选中的年 / 月 / 日（用于导航）
static int s_SelYear  = 0;   // 0 = 本年
static int s_SelMonth = 0;   // 0 = 本月
static int s_SelDay   = 0;   // 0 = 今天  (相对偏移，0=今天,-1=昨天…)
[[maybe_unused]] static int s_SelTagIdx = -1; // -1 = 全部标签
[[maybe_unused]] static const UINT_PTR TIMER_STATS = 4001;

// 滚动
static int s_ScrollY = 0;
static int s_WinH    = 0;
static int s_ContentH = 0;

// HitZone
struct SHitZone { Gdiplus::Rect rect; int id; };
static std::vector<SHitZone> s_Hz;

enum SHitId {
    SH_CLOSE        = 1,
    SH_TAB_YEAR     = 10,
    SH_TAB_MONTH    = 11,
    SH_TAB_DAY      = 12,
    SH_TAB_TAG      = 13,
    SH_NAV_PREV     = 20,
    SH_NAV_NEXT     = 21,
    SH_REFRESH      = 30,
    SH_TAG_BASE     = 100, // +i => 第 i 个标签
};


// ============================================================
// 辅助：获取当前系统 年/月/日
// ============================================================
static void GetNow(int& y, int& m, int& d) {
    time_t t = time(nullptr);
    struct tm lt; localtime_s(&lt, &t);
    y = lt.tm_year + 1900;
    m = lt.tm_mon  + 1;
    d = lt.tm_mday;
}

// UTC ms -> local struct tm
static bool MsToLocalTm(long long ms, struct tm& out) {
    if (ms <= 0) return false;
    time_t t = (time_t)(ms / 1000);
    return localtime_s(&out, &t) == 0;
}

// ============================================================
// 数据聚合
// ============================================================
struct DayStats {
    int   sessions   = 0;
    int   totalSecs  = 0; // actualDuration 之和
    int   completed  = 0;
    int   interrupted = 0;
};

// 将 g_PomodoroHistory 按 YYYY-MM-DD 聚合
static std::map<std::wstring, DayStats> AggregatByDay(
    const std::vector<PomodoroRecord>& recs,
    const std::wstring& filterTagUuid = L"")
{
    std::map<std::wstring, DayStats> res;
    for (const auto& r : recs) {
        if (r.isDeleted) continue;
        if (!filterTagUuid.empty() && r.todoUuid.empty()) continue;
        // TODO: tag filtering via todoUuid -> tag relationship (client-side not stored)
        struct tm lt{};
        if (!MsToLocalTm(r.startTime, lt)) continue;
        wchar_t buf[16];
        swprintf_s(buf, L"%04d-%02d-%02d", lt.tm_year+1900, lt.tm_mon+1, lt.tm_mday);
        auto& ds = res[buf];
        ds.sessions++;
        ds.totalSecs += r.actualDuration > 0 ? r.actualDuration : r.plannedDuration;
        if (r.status == L"completed")   ds.completed++;
        else                            ds.interrupted++;
    }
    return res;
}

// 格式化秒数为 "Xh Ym"
static std::wstring FmtDuration(int secs) {
    if (secs <= 0) return L"0 min";
    int h = secs / 3600;
    int m = (secs % 3600) / 60;
    if (h > 0) {
        wchar_t buf[32];
        swprintf_s(buf, L"%dh %dm", h, m);
        return buf;
    }
    wchar_t buf[16];
    swprintf_s(buf, L"%d min", m);
    return buf;
}

// 月份名
static const wchar_t* MonthNames[] = {
    L"",L"一月",L"二月",L"三月",L"四月",L"五月",L"六月",
    L"七月",L"八月",L"九月",L"十月",L"十一月",L"十二月"
};

// ============================================================
// 绘制辅助
// ============================================================
static void DrawRoundRect(Graphics& g, float x, float y, float w, float h,
                           float r, const Color& fillColor,
                           const Color* strokeColor = nullptr, float strokeW = 1.0f)
{
    GraphicsPath path;
    path.AddArc(x,       y,       r*2, r*2, 180, 90);
    path.AddArc(x+w-r*2, y,       r*2, r*2, 270, 90);
    path.AddArc(x+w-r*2, y+h-r*2, r*2, r*2,   0, 90);
    path.AddArc(x,       y+h-r*2, r*2, r*2,  90, 90);
    path.CloseFigure();
    SolidBrush br(fillColor);
    g.FillPath(&br, &path);
    if (strokeColor) {
        Pen pen(*strokeColor, strokeW);
        g.DrawPath(&pen, &path);
    }
}

// ============================================================
// 主渲染
// ============================================================
static void StatsRender() {
    if (!s_hWnd) return;
    RECT rc; GetClientRect(s_hWnd, &rc);
    int W = rc.right, H = rc.bottom;
    if (W <= 0 || H <= 0) return;
    s_WinH = H;

    HDC hdcWin = GetDC(s_hWnd);
    const int CANVAS_H = 3200;
    HDC hdcCanvas   = CreateCompatibleDC(hdcWin);
    HBITMAP hBmp    = CreateCompatibleBitmap(hdcWin, W, CANVAS_H);
    HBITMAP hOld    = (HBITMAP)SelectObject(hdcCanvas, hBmp);

    Graphics g(hdcCanvas);
    g.SetSmoothingMode(SmoothingModeAntiAlias);
    g.SetTextRenderingHint(TextRenderingHintAntiAliasGridFit);

    // 背景
    SolidBrush bgBr(S_BG);
    g.FillRectangle(&bgBr, 0, 0, W, CANVAS_H);

    s_Hz.clear();

    FontFamily& ff = *g_MiSansFamily;
    auto MkFont = [&](float sz, int style = FontStyleRegular) -> Font* {
        return new Font(&ff, sz, style, UnitPixel);
    };
    SolidBrush txBr(S_TEXT), subBr(S_SUB);
    StringFormat sfC, sfL, sfR;
    sfC.SetAlignment(StringAlignmentCenter); sfC.SetLineAlignment(StringAlignmentCenter);
    sfL.SetAlignment(StringAlignmentNear);   sfL.SetLineAlignment(StringAlignmentCenter);
    sfL.SetTrimming(StringTrimmingEllipsisCharacter);
    sfL.SetFormatFlags(StringFormatFlagsNoWrap);
    sfR.SetAlignment(StringAlignmentFar);    sfR.SetLineAlignment(StringAlignmentCenter);

    float y = 14.0f;

    // ── 标题栏 ──────────────────────────────────────────────
    {
        Font* fT = MkFont(18, FontStyleBold);
        SolidBrush acBr(S_ACCENT);
        g.DrawString(L"番茄钟统计", -1, fT, RectF(0, y, (REAL)W, 32), &sfC, &acBr);
        delete fT;
        Font* fX = MkFont(14);
        g.DrawString(L"✕", -1, fX, RectF((REAL)(W-40), y, 30, 28), &sfC, &subBr);
        s_Hz.push_back({Gdiplus::Rect(W-40,(int)y,30,28), SH_CLOSE});
        delete fX;
        y += 44.0f;
    }

    // ── Tab 栏 ──────────────────────────────────────────────
    {
        struct TabItem { const wchar_t* name; StatsView view; int hitId; };
        TabItem tabs[] = {
            {L"年",   StatsView::Year,  SH_TAB_YEAR},
            {L"月",   StatsView::Month, SH_TAB_MONTH},
            {L"日",   StatsView::Day,   SH_TAB_DAY},
            {L"标签", StatsView::Tag,   SH_TAB_TAG},
        };
        float tabW = (W - 32.0f) / 4.0f;
        Font* fTab = MkFont(13, FontStyleBold);
        for (int i = 0; i < 4; i++) {
            float tx = 16.0f + tabW * i;
            bool active = (s_View == tabs[i].view);
            Color bg = active ? S_ACCENT : S_CARD;
            DrawRoundRect(g, tx+2, y, tabW-4, 34, 8, bg, &S_BORDER, 1.0f);
            SolidBrush tBr(active ? Color(255,255,255,255) : S_TEXT);
            g.DrawString(tabs[i].name, -1, fTab, RectF(tx+2, y, tabW-4, 34), &sfC, &tBr);
            s_Hz.push_back({Gdiplus::Rect((int)(tx+2),(int)y,(int)(tabW-4),34), tabs[i].hitId});
        }
        delete fTab;
        y += 44.0f;
    }

    // ── 收集数据快照 ─────────────────────────────────────────
    std::vector<PomodoroRecord> recs;
    {
        std::lock_guard<std::recursive_mutex> lk(g_DataMutex);
        recs = g_PomodoroHistory;
    }

    int nowYear, nowMonth, nowDay;
    GetNow(nowYear, nowMonth, nowDay);

    // ── 各视图 ──────────────────────────────────────────────
    if (s_View == StatsView::Year) {
        // ─── 年视图 ─────────────────────────────────────────
        int dispYear = nowYear + s_SelYear; // s_SelYear <= 0

        // 导航行
        {
            Font* fNav = MkFont(14, FontStyleBold);
            SolidBrush acBr(S_ACCENT);
            std::wstring title = std::to_wstring(dispYear) + L" 年";
            g.DrawString(L"<", -1, fNav, RectF(20, y, 30, 30), &sfC, &acBr);
            g.DrawString(title.c_str(), -1, fNav, RectF(56, y, (REAL)(W-112), 30), &sfC, &txBr);
            bool canNext = (dispYear < nowYear);
            SolidBrush nextBr(canNext ? S_ACCENT : S_BORDER);
            g.DrawString(L">", -1, fNav, RectF((REAL)(W-50), y, 30, 30), &sfC, &nextBr);
            s_Hz.push_back({Gdiplus::Rect(20,(int)y,30,30), SH_NAV_PREV});
            if (canNext) s_Hz.push_back({Gdiplus::Rect(W-50,(int)y,30,30), SH_NAV_NEXT});
            delete fNav;
            y += 42.0f;
        }

        // 汇总卡片
        int yearSessions = 0, yearSecs = 0, yearCompleted = 0;
        auto dayMap = AggregatByDay(recs);
        int activeDays = 0;
        for (auto& kv : dayMap) {
            // filter by year
            if (kv.first.size() >= 4 && std::stoi(kv.first.substr(0,4)) == dispYear) {
                yearSessions  += kv.second.sessions;
                yearSecs      += kv.second.totalSecs;
                yearCompleted += kv.second.completed;
                activeDays++;
            }
        }

        // 3 卡片
        float cW = (W - 48.0f) / 3.0f;
        struct CardInfo { std::wstring val; const wchar_t* label; Color color; };
        CardInfo cards[] = {
            {FmtDuration(yearSecs),       L"总专注时长", S_ACCENT},
            {std::to_wstring(yearSessions), L"专注次数",   S_BLUE},
            {std::to_wstring(activeDays),   L"活跃天数",   S_GREEN},
        };
        for (int i = 0; i < 3; i++) {
            float cx = 16.0f + cW * i;
            DrawRoundRect(g, cx, y, cW-4, 72, 12, S_CARD, &S_BORDER, 1.0f);
            Font* fV = MkFont(16, FontStyleBold);
            Font* fL = MkFont(11);
            SolidBrush vBr(cards[i].color);
            g.DrawString(cards[i].val.c_str(), -1, fV, RectF(cx, y+8, cW-4, 30), &sfC, &vBr);
            g.DrawString(cards[i].label, -1, fL, RectF(cx, y+42, cW-4, 22), &sfC, &subBr);
            delete fV; delete fL;
        }
        y += 88.0f;

        // 月柱状图
        {
            Font* fH = MkFont(13, FontStyleBold);
            g.DrawString(L"各月专注时长", -1, fH, RectF(16, y, (REAL)(W-32), 24), &sfL, &txBr);
            delete fH;
            y += 30.0f;

            int monthSecs[12] = {};
            for (auto& kv : dayMap) {
                if (kv.first.size() >= 7 && std::stoi(kv.first.substr(0,4)) == dispYear) {
                    int mo = std::stoi(kv.first.substr(5,2)) - 1;
                    if (mo >= 0 && mo < 12) monthSecs[mo] += kv.second.totalSecs;
                }
            }
            int maxSec = *std::max_element(monthSecs, monthSecs+12);
            if (maxSec <= 0) maxSec = 1;

            float barW   = (W - 48.0f) / 12.0f;
            float barMaxH = 100.0f;
            Font* fMo = MkFont(10);
            for (int mo = 0; mo < 12; mo++) {
                float bx = 24.0f + barW * mo;
                float bh = barMaxH * monthSecs[mo] / maxSec;
                if (bh < 2.0f && monthSecs[mo] > 0) bh = 2.0f;
                // 背景槽
                DrawRoundRect(g, bx, y, barW-4, barMaxH, 4, S_BORDER);
                // 数据条
                if (bh > 0) {
                    DrawRoundRect(g, bx, y+barMaxH-bh, barW-4, bh, 4, S_ACCENT);
                }
                // 月标签
                const wchar_t* moLabel[] = {L"1",L"2",L"3",L"4",L"5",L"6",
                                             L"7",L"8",L"9",L"10",L"11",L"12"};
                g.DrawString(moLabel[mo], -1, fMo,
                    RectF(bx, y+barMaxH+4, barW-4, 16), &sfC, &subBr);
            }
            delete fMo;
            y += barMaxH + 28.0f;
        }

        // 每月明细列表
        {
            Font* fH = MkFont(13, FontStyleBold);
            g.DrawString(L"月度明细", -1, fH, RectF(16, y, (REAL)(W-32), 24), &sfL, &txBr);
            delete fH;
            y += 30.0f;

            Font* fItem = MkFont(13);
            Font* fSub2 = MkFont(11);
            for (int mo = 11; mo >= 0; mo--) {
                int moSecs = 0, moSess = 0, moComp = 0;
                for (auto& kv : dayMap) {
                    if (kv.first.size() >= 7
                        && std::stoi(kv.first.substr(0,4)) == dispYear
                        && std::stoi(kv.first.substr(5,2)) == mo+1) {
                        moSecs += kv.second.totalSecs;
                        moSess += kv.second.sessions;
                        moComp += kv.second.completed;
                    }
                }
                if (moSess == 0) continue;
                DrawRoundRect(g, 16, y, (REAL)(W-32), 56, 10, S_CARD, &S_BORDER, 1.0f);
                // 月名
                SolidBrush acBr(S_ACCENT);
                g.DrawString(MonthNames[mo+1], -1, fItem, RectF(28, y+6, 80, 22), &sfL, &acBr);
                // 时长
                std::wstring durStr = FmtDuration(moSecs);
                g.DrawString(durStr.c_str(), -1, fItem, RectF(28, y+28, 120, 20), &sfL, &subBr);
                // 次数
                std::wstring sessStr = std::to_wstring(moSess) + L" 次  完成 " + std::to_wstring(moComp);
                g.DrawString(sessStr.c_str(), -1, fSub2, RectF((REAL)(W/2), y+8, (REAL)(W/2-32), 40), &sfR, &subBr);
                y += 64.0f;
            }
            delete fItem; delete fSub2;
        }

    } else if (s_View == StatsView::Month) {
        // ─── 月视图 ─────────────────────────────────────────
        int dispMonth = nowMonth + s_SelMonth;
        int dispYear2 = nowYear;
        while (dispMonth < 1)  { dispMonth += 12; dispYear2--; }
        while (dispMonth > 12) { dispMonth -= 12; dispYear2++; }

        // 导航行
        {
            Font* fNav = MkFont(14, FontStyleBold);
            SolidBrush acBr(S_ACCENT);
            std::wstring title = std::to_wstring(dispYear2) + L" 年 " + std::to_wstring(dispMonth) + L" 月";
            g.DrawString(L"<", -1, fNav, RectF(20, y, 30, 30), &sfC, &acBr);
            g.DrawString(title.c_str(), -1, fNav, RectF(56, y, (REAL)(W-112), 30), &sfC, &txBr);
            bool canNext = !(dispYear2 == nowYear && dispMonth == nowMonth);
            SolidBrush nextBr(canNext ? S_ACCENT : S_BORDER);
            g.DrawString(L">", -1, fNav, RectF((REAL)(W-50), y, 30, 30), &sfC, &nextBr);
            s_Hz.push_back({Gdiplus::Rect(20,(int)y,30,30), SH_NAV_PREV});
            if (canNext) s_Hz.push_back({Gdiplus::Rect(W-50,(int)y,30,30), SH_NAV_NEXT});
            delete fNav;
            y += 42.0f;
        }

        // 筛选本月记录
        auto dayMap = AggregatByDay(recs);
        int moSessions = 0, moSecs = 0, moCompleted = 0, activeDays = 0;
        wchar_t prefix[8]; swprintf_s(prefix, L"%04d-%02d", dispYear2, dispMonth);
        for (auto& kv : dayMap) {
            if (kv.first.substr(0,7) == prefix) {
                moSessions  += kv.second.sessions;
                moSecs      += kv.second.totalSecs;
                moCompleted += kv.second.completed;
                activeDays++;
            }
        }

        // 汇总卡片
        float cW = (W - 48.0f) / 3.0f;
        struct CardInfo2 { std::wstring val; const wchar_t* label; Color color; };
        CardInfo2 cards[] = {
            {FmtDuration(moSecs),          L"总专注时长", S_ACCENT},
            {std::to_wstring(moSessions),  L"专注次数",   S_BLUE},
            {std::to_wstring(activeDays),  L"活跃天数",   S_GREEN},
        };
        for (int i = 0; i < 3; i++) {
            float cx = 16.0f + cW * i;
            DrawRoundRect(g, cx, y, cW-4, 72, 12, S_CARD, &S_BORDER, 1.0f);
            Font* fV = MkFont(16, FontStyleBold);
            Font* fL = MkFont(11);
            SolidBrush vBr(cards[i].color);
            g.DrawString(cards[i].val.c_str(), -1, fV, RectF(cx, y+8, cW-4, 30), &sfC, &vBr);
            g.DrawString(cards[i].label, -1, fL, RectF(cx, y+42, cW-4, 22), &sfC, &subBr);
            delete fV; delete fL;
        }
        y += 88.0f;

        // 日历热力图
        {
            Font* fH = MkFont(13, FontStyleBold);
            g.DrawString(L"每日专注热力图", -1, fH, RectF(16, y, (REAL)(W-32), 24), &sfL, &txBr);
            delete fH;
            y += 30.0f;

            // 星期标题
            const wchar_t* weekNames[] = {L"日",L"一",L"二",L"三",L"四",L"五",L"六"};
            float cellW = (W - 32.0f) / 7.0f;
            Font* fWk = MkFont(11);
            for (int w = 0; w < 7; w++)
                g.DrawString(weekNames[w], -1, fWk, RectF(16+cellW*w, y, cellW, 18), &sfC, &subBr);
            delete fWk;
            y += 22.0f;

            // 求最大值（热力归一化）
            int maxDaySecs = 1;
            for (auto& kv : dayMap) {
                if (kv.first.substr(0,7) == prefix && kv.second.totalSecs > maxDaySecs)
                    maxDaySecs = kv.second.totalSecs;
            }

            // 计算该月第1天是星期几
            struct tm tm1{}; tm1.tm_year = dispYear2-1900; tm1.tm_mon = dispMonth-1; tm1.tm_mday = 1;
            mktime(&tm1);
            int firstWeekday = tm1.tm_wday; // 0=Sun

            // 该月天数
            int daysInMonth = 31;
            if (dispMonth == 4||dispMonth==6||dispMonth==9||dispMonth==11) daysInMonth=30;
            else if (dispMonth == 2) {
                bool leap = (dispYear2%4==0 && (dispYear2%100!=0||dispYear2%400==0));
                daysInMonth = leap ? 29 : 28;
            }

            Font* fDay = MkFont(10);
            int col = firstWeekday;
            float calRow = y;
            for (int d = 1; d <= daysInMonth; d++) {
                float cx2 = 16.0f + cellW * col;
                wchar_t dkey[16]; swprintf_s(dkey, L"%04d-%02d-%02d", dispYear2, dispMonth, d);
                auto it = dayMap.find(dkey);
                int daySecs = (it != dayMap.end()) ? it->second.totalSecs : 0;
                // 热力颜色
                float heat = daySecs > 0 ? (float)daySecs / maxDaySecs : 0.0f;
                BYTE alpha = (BYTE)(40 + 215 * heat);
                Color cellColor = daySecs > 0
                    ? Color(alpha, 239, 83, 80)
                    : Color(30, 200, 200, 210);
                DrawRoundRect(g, cx2+2, calRow+2, cellW-4, cellW-4, 6, cellColor);
                // 日数字
                bool isToday = (dispYear2 == nowYear && dispMonth == nowMonth && d == nowDay);
                SolidBrush dayBr(isToday ? S_ACCENT : S_TEXT);
                wchar_t ds[4]; swprintf_s(ds, L"%d", d);
                g.DrawString(ds, -1, fDay, RectF(cx2+2, calRow+2, cellW-4, cellW-4), &sfC, &dayBr);
                col++;
                if (col >= 7) { col = 0; calRow += cellW; }
            }
            delete fDay;
            // 计算日历实际占用行数（用于更新 y）
            int totalCells2 = firstWeekday + daysInMonth;
            int calRows = (totalCells2 + 6) / 7;
            y = y + (REAL)cellW * calRows + 16.0f;
        }

        // 每日明细（降序）
        {
            Font* fH = MkFont(13, FontStyleBold);
            g.DrawString(L"每日明细", -1, fH, RectF(16, y, (REAL)(W-32), 24), &sfL, &txBr);
            delete fH;
            y += 30.0f;

            // 收集属于本月的 day entries，按日期降序
            std::vector<std::pair<std::wstring, DayStats>> entries;
            for (auto& kv : dayMap) {
                if (kv.first.substr(0,7) == prefix)
                    entries.push_back(kv);
            }
            std::sort(entries.begin(), entries.end(),
                [](auto& a, auto& b){ return a.first > b.first; });

            Font* fItem = MkFont(13);
            Font* fSub2 = MkFont(11);
            for (auto& e : entries) {
                DrawRoundRect(g, 16, y, (REAL)(W-32), 56, 10, S_CARD, &S_BORDER, 1.0f);
                // 日期（只显示 MM-DD）
                SolidBrush acBr(S_BLUE);
                std::wstring dateLabel = e.first.substr(5); // MM-DD
                g.DrawString(dateLabel.c_str(), -1, fItem, RectF(28, y+8, 70, 22), &sfL, &acBr);
                // 时长
                std::wstring durStr = FmtDuration(e.second.totalSecs);
                g.DrawString(durStr.c_str(), -1, fItem, RectF(28, y+30, 120, 20), &sfL, &subBr);
                // 次数
                std::wstring sessStr = std::to_wstring(e.second.sessions) + L" 次  完成 "
                                     + std::to_wstring(e.second.completed);
                g.DrawString(sessStr.c_str(), -1, fSub2,
                    RectF((REAL)(W/2), y+8, (REAL)(W/2-32), 40), &sfR, &subBr);
                y += 64.0f;
            }
            if (entries.empty()) {
                g.DrawString(L"本月暂无记录", -1, fItem, RectF(16, y, (REAL)(W-32), 32), &sfC, &subBr);
                y += 40.0f;
            }
            delete fItem; delete fSub2;
        }

    } else if (s_View == StatsView::Day) {
        // ─── 日视图 ─────────────────────────────────────────
        // s_SelDay: 0=今天, -1=昨天 …
        time_t base = time(nullptr) + (time_t)s_SelDay * 86400;
        struct tm bt{}; localtime_s(&bt, &base);
        int dy = bt.tm_year+1900, dm = bt.tm_mon+1, dd = bt.tm_mday;

        // 导航
        {
            Font* fNav = MkFont(14, FontStyleBold);
            SolidBrush acBr(S_ACCENT);
            wchar_t title[32]; swprintf_s(title, L"%d 年 %d 月 %d 日", dy, dm, dd);
            g.DrawString(L"<", -1, fNav, RectF(20, y, 30, 30), &sfC, &acBr);
            g.DrawString(title, -1, fNav, RectF(56, y, (REAL)(W-112), 30), &sfC, &txBr);
            bool canNext = (s_SelDay < 0);
            SolidBrush nextBr(canNext ? S_ACCENT : S_BORDER);
            g.DrawString(L">", -1, fNav, RectF((REAL)(W-50), y, 30, 30), &sfC, &nextBr);
            s_Hz.push_back({Gdiplus::Rect(20,(int)y,30,30), SH_NAV_PREV});
            if (canNext) s_Hz.push_back({Gdiplus::Rect(W-50,(int)y,30,30), SH_NAV_NEXT});
            delete fNav;
            y += 42.0f;
        }

        // 本日记录
        wchar_t dkey[16]; swprintf_s(dkey, L"%04d-%02d-%02d", dy, dm, dd);
        std::vector<PomodoroRecord> dayRecs;
        for (auto& r : recs) {
            if (r.isDeleted) continue;
            struct tm lt{}; if (!MsToLocalTm(r.startTime, lt)) continue;
            wchar_t rk[16]; swprintf_s(rk, L"%04d-%02d-%02d",
                lt.tm_year+1900, lt.tm_mon+1, lt.tm_mday);
            if (std::wstring(rk) == std::wstring(dkey)) dayRecs.push_back(r);
        }
        std::sort(dayRecs.begin(), dayRecs.end(),
            [](auto& a, auto& b){ return a.startTime > b.startTime; });

        // 汇总
        int totalSecs = 0, completed = 0;
        for (auto& r : dayRecs) {
            totalSecs += r.actualDuration > 0 ? r.actualDuration : r.plannedDuration;
            if (r.status == L"completed") completed++;
        }

        // 汇总卡片
        float cW2 = (W - 48.0f) / 3.0f;
        {
            struct C { std::wstring val; const wchar_t* label; Color color; };
            C cards[] = {
                {FmtDuration(totalSecs),           L"专注时长", S_ACCENT},
                {std::to_wstring(dayRecs.size()),   L"专注次数", S_BLUE},
                {std::to_wstring(completed),        L"完成次数", S_GREEN},
            };
            for (int i = 0; i < 3; i++) {
                float cx2 = 16.0f + cW2 * i;
                DrawRoundRect(g, cx2, y, cW2-4, 72, 12, S_CARD, &S_BORDER, 1.0f);
                Font* fV = MkFont(16, FontStyleBold);
                Font* fL = MkFont(11);
                SolidBrush vBr(cards[i].color);
                g.DrawString(cards[i].val.c_str(), -1, fV, RectF(cx2, y+8, cW2-4, 30), &sfC, &vBr);
                g.DrawString(cards[i].label, -1, fL, RectF(cx2, y+42, cW2-4, 22), &sfC, &subBr);
                delete fV; delete fL;
            }
            y += 88.0f;
        }

        // 时间轴
        {
            Font* fH = MkFont(13, FontStyleBold);
            g.DrawString(L"专注时间轴", -1, fH, RectF(16, y, (REAL)(W-32), 24), &sfL, &txBr);
            delete fH;
            y += 30.0f;

            Font* fItem = MkFont(13);
            Font* fSub2 = MkFont(11);
            if (dayRecs.empty()) {
                g.DrawString(L"当日暂无记录", -1, fItem, RectF(16, y, (REAL)(W-32), 32), &sfC, &subBr);
                y += 40.0f;
            }
            for (auto& r : dayRecs) {
                // 时间
                struct tm st{}; MsToLocalTm(r.startTime, st);
                struct tm et{}; MsToLocalTm(r.endTime > 0 ? r.endTime : r.startTime, et);
                wchar_t timeStr[32];
                swprintf_s(timeStr, L"%02d:%02d – %02d:%02d", st.tm_hour, st.tm_min, et.tm_hour, et.tm_min);
                bool isComp = (r.status == L"completed");
                Color cardBg = isComp ? Color(255, 248, 255, 250) : Color(255, 255, 248, 248);
                DrawRoundRect(g, 16, y, (REAL)(W-32), 68, 10, cardBg, &S_BORDER, 1.0f);
                // 左色条
                Color barCol = isComp ? S_GREEN : S_ACCENT;
                DrawRoundRect(g, 16, y, 4, 68, 2, barCol);
                // 时间
                SolidBrush timeBr(S_BLUE);
                g.DrawString(timeStr, -1, fItem, RectF(28, y+8, (REAL)(W-60), 22), &sfL, &timeBr);
                // 时长 + 状态
                std::wstring durLabel = FmtDuration(r.actualDuration > 0 ? r.actualDuration : r.plannedDuration);
                std::wstring statusLabel = isComp ? L"✓ 完成" : L"✗ 中断";
                SolidBrush statusBr(isComp ? S_GREEN : S_ACCENT);
                g.DrawString(durLabel.c_str(), -1, fSub2, RectF(28, y+34, 80, 20), &sfL, &subBr);
                g.DrawString(statusLabel.c_str(), -1, fSub2,
                    RectF((REAL)(W-120), y+34, 90, 20), &sfR, &statusBr);
                // 绑定待办
                if (!r.todoUuid.empty()) {
                    std::wstring todoContent;
                    std::lock_guard<std::recursive_mutex> lk(g_DataMutex);
                    for (auto& t : g_Todos)
                        if (t.uuid == r.todoUuid) { todoContent = t.content; break; }
                    if (!todoContent.empty()) {
                        std::wstring td = L"📌 " + (todoContent.size()>22 ? todoContent.substr(0,20)+L".." : todoContent);
                        g.DrawString(td.c_str(), -1, fSub2,
                            RectF(28, y+48, (REAL)(W-60), 16), &sfL, &subBr);
                    }
                }
                y += 76.0f;
            }
            delete fItem; delete fSub2;
        }

    } else {
        // ─── 标签视图 ─────────────────────────────────────────
        Font* fH = MkFont(13, FontStyleBold);
        g.DrawString(L"按标签统计（最近 30 天）", -1, fH, RectF(16, y, (REAL)(W-32), 24), &sfL, &txBr);
        delete fH;
        y += 34.0f;

        // 30天范围
        long long nowMs  = (long long)time(nullptr) * 1000LL;
        long long from30 = nowMs - (long long)30 * 24 * 3600 * 1000LL;

        // 找出所有绑定了 todo 的记录
        std::map<std::wstring, int> todoSecs; // todoUuid -> totalSecs
        for (auto& r : recs) {
            if (r.isDeleted || r.startTime < from30) continue;
            if (r.todoUuid.empty()) continue;
            todoSecs[r.todoUuid] += r.actualDuration > 0 ? r.actualDuration : r.plannedDuration;
        }

        // 无标签的总时长
        int untaggedSecs = 0;
        for (auto& r : recs) {
            if (r.isDeleted || r.startTime < from30) continue;
            if (r.todoUuid.empty())
                untaggedSecs += r.actualDuration > 0 ? r.actualDuration : r.plannedDuration;
        }

        // 绑定待办名称
        Font* fItem = MkFont(13);
        Font* fSub2 = MkFont(11);

        int maxSecs = 1;
        for (auto& kv : todoSecs) if (kv.second > maxSecs) maxSecs = kv.second;
        if (untaggedSecs > maxSecs) maxSecs = untaggedSecs;

        // 显示标签对应的待办列表
        std::vector<std::pair<std::wstring, int>> sorted(todoSecs.begin(), todoSecs.end());
        std::sort(sorted.begin(), sorted.end(), [](auto& a, auto& b){ return a.second > b.second; });

        {
            std::lock_guard<std::recursive_mutex> lk(g_DataMutex);
            for (auto& kv : sorted) {
                // 找 todo 标题
                std::wstring title = kv.first;
                for (auto& t : g_Todos)
                    if (t.uuid == kv.first) { title = t.content; break; }
                if (title.size() > 24) title = title.substr(0,22)+L"..";

                DrawRoundRect(g, 16, y, (REAL)(W-32), 60, 10, S_CARD, &S_BORDER, 1.0f);
                SolidBrush acBr(S_BLUE);
                g.DrawString(title.c_str(), -1, fItem, RectF(28, y+6, (REAL)(W-140), 22), &sfL, &acBr);
                std::wstring dur = FmtDuration(kv.second);
                SolidBrush acBr2(S_ACCENT);
                g.DrawString(dur.c_str(), -1, fItem, RectF((REAL)(W-130), y+6, 100, 22), &sfR, &acBr2);
                // 进度条
                float barW2 = (W - 56.0f) * kv.second / maxSecs;
                DrawRoundRect(g, 28, y+34, (REAL)(W-56), 8, 4, S_BORDER);
                DrawRoundRect(g, 28, y+34, barW2, 8, 4, S_ACCENT);
                y += 68.0f;
            }
        }

        // 未绑定记录
        if (untaggedSecs > 0) {
            DrawRoundRect(g, 16, y, (REAL)(W-32), 60, 10, S_CARD, &S_BORDER, 1.0f);
            g.DrawString(L"（未绑定待办）", -1, fItem, RectF(28, y+6, (REAL)(W-140), 22), &sfL, &subBr);
            std::wstring dur = FmtDuration(untaggedSecs);
            g.DrawString(dur.c_str(), -1, fItem, RectF((REAL)(W-130), y+6, 100, 22), &sfR, &subBr);
            float barW2 = (W - 56.0f) * untaggedSecs / maxSecs;
            DrawRoundRect(g, 28, y+34, (REAL)(W-56), 8, 4, S_BORDER);
            DrawRoundRect(g, 28, y+34, barW2, 8, 4, S_SUB);
            y += 68.0f;
        }
        if (sorted.empty() && untaggedSecs == 0) {
            g.DrawString(L"近 30 天暂无专注记录", -1, fItem,
                RectF(16, y, (REAL)(W-32), 32), &sfC, &subBr);
            y += 40.0f;
        }
        delete fItem; delete fSub2;
    }

    y += 20.0f; // 底部留白
    s_ContentH = (int)y;

    // ── 将离屏画布贴到窗口（滚动裁剪）────────────────────────
    int maxScroll = std::max(0, s_ContentH - s_WinH);
    if (s_ScrollY > maxScroll) s_ScrollY = maxScroll;

    // 从 hdcCanvas 中裁出当前可见区域，贴到 hdcWin
    BitBlt(hdcWin, 0, 0, W, H, hdcCanvas, 0, s_ScrollY, SRCCOPY);

    // 滚动条
    if (s_ContentH > s_WinH) {
        float ratio = (float)s_WinH / s_ContentH;
        int sbH = std::max(30, (int)(H * ratio));
        int sbMaxY = H - sbH;
        int sbY = maxScroll > 0 ? (int)((float)sbMaxY * s_ScrollY / maxScroll) : 0;
        HBRUSH sbBr = CreateSolidBrush(RGB(180,190,210));
        RECT sbRc = {W-5, sbY, W-1, sbY+sbH};
        FillRect(hdcWin, &sbRc, sbBr);
        DeleteObject(sbBr);
    }

    SelectObject(hdcCanvas, hOld);
    DeleteObject(hBmp);
    DeleteDC(hdcCanvas);
    ReleaseDC(s_hWnd, hdcWin);
}

// ============================================================
// 事件
// ============================================================
static void HandleHit(int id) {
    if (id == SH_CLOSE) { DestroyWindow(s_hWnd); return; }

    if (id == SH_TAB_YEAR)  { s_View = StatsView::Year;  s_ScrollY = 0; }
    if (id == SH_TAB_MONTH) { s_View = StatsView::Month; s_ScrollY = 0; }
    if (id == SH_TAB_DAY)   { s_View = StatsView::Day;   s_ScrollY = 0; }
    if (id == SH_TAB_TAG)   { s_View = StatsView::Tag;   s_ScrollY = 0; }

    if (id == SH_NAV_PREV) {
        if      (s_View == StatsView::Year)  s_SelYear--;
        else if (s_View == StatsView::Month) s_SelMonth--;
        else if (s_View == StatsView::Day)   s_SelDay--;
        s_ScrollY = 0;
    }
    if (id == SH_NAV_NEXT) {
        if      (s_View == StatsView::Year  && s_SelYear  < 0) s_SelYear++;
        else if (s_View == StatsView::Month && s_SelMonth < 0) s_SelMonth++;
        else if (s_View == StatsView::Day   && s_SelDay   < 0) s_SelDay++;
        s_ScrollY = 0;
    }
    if (id == SH_REFRESH) {
        std::thread([](){
            long long toMs   = (long long)time(nullptr) * 1000LL;
            long long fromMs = toMs - (long long)90LL * 24 * 3600 * 1000LL;
            ApiFetchPomodoroHistory(fromMs, toMs);
            SavePomodoroLocalCache();
            if (s_hWnd) InvalidateRect(s_hWnd, NULL, FALSE);
        }).detach();
        return;
    }
    StatsRender();
}

// ============================================================
// WndProc
// ============================================================
static LRESULT CALLBACK StatsWndProc(HWND hWnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE:
        s_hWnd = hWnd;
        // 后台刷新近 90 天历史
        std::thread([](){
            LoadPomodoroLocalCache();
            long long toMs   = (long long)time(nullptr) * 1000LL;
            long long fromMs = toMs - (long long)90LL * 24 * 3600 * 1000LL;
            ApiFetchPomodoroHistory(fromMs, toMs);
            SavePomodoroLocalCache();
            if (s_hWnd) InvalidateRect(s_hWnd, NULL, FALSE);
        }).detach();
        break;

    case WM_PAINT: {
        PAINTSTRUCT ps; BeginPaint(hWnd, &ps); EndPaint(hWnd, &ps);
        StatsRender();
        break;
    }
    case WM_ERASEBKGND: return 1;

    case WM_MOUSEWHEEL: {
        int delta = GET_WHEEL_DELTA_WPARAM(wp);
        s_ScrollY -= delta / 3;
        if (s_ScrollY < 0) s_ScrollY = 0;
        int maxS = std::max(0, s_ContentH - s_WinH);
        if (s_ScrollY > maxS) s_ScrollY = maxS;
        StatsRender();
        return 0;
    }

    case WM_LBUTTONDOWN: {
        int mx = LOWORD(lp), my = HIWORD(lp);
        for (auto& hz : s_Hz) {
            if (hz.rect.Contains(mx, my + s_ScrollY)) {
                HandleHit(hz.id);
                return 0;
            }
        }
        SendMessage(hWnd, WM_NCLBUTTONDOWN, HTCAPTION, 0);
        break;
    }

    case WM_SIZE:
        s_WinH = HIWORD(lp);
        StatsRender();
        break;

    case WM_DESTROY:
        s_hWnd = NULL;
        if (s_hParent) EnableWindow(s_hParent, TRUE);
        break;

    default:
        return DefWindowProcW(hWnd, msg, wp, lp);
    }
    return 0;
}

// ============================================================
// 公开入口
// ============================================================
void ShowPomodoroStatsWindow(HWND parent) {
    if (s_hWnd && IsWindow(s_hWnd)) {
        SetForegroundWindow(s_hWnd);
        return;
    }

    static bool s_Registered = false;
    if (!s_Registered) {
        WNDCLASSEXW wc{};
        wc.cbSize        = sizeof(wc);
        wc.lpfnWndProc   = StatsWndProc;
        wc.hInstance     = GetModuleHandleW(NULL);
        wc.hbrBackground = (HBRUSH)(COLOR_WINDOW+1);
        wc.hCursor       = LoadCursorW(NULL, IDC_ARROW);
        wc.lpszClassName = L"PomodoroStatsWnd";
        RegisterClassExW(&wc);
        s_Registered = true;
    }

    s_hParent  = parent;
    s_ScrollY  = 0;
    s_SelYear  = 0;
    s_SelMonth = 0;
    s_SelDay   = 0;
    s_View     = StatsView::Month;

    int W = 440, H = 820;
    int sx = GetSystemMetrics(SM_CXSCREEN);
    int sy = GetSystemMetrics(SM_CYSCREEN);
    HWND hWnd = CreateWindowExW(
        WS_EX_APPWINDOW, L"PomodoroStatsWnd", L"番茄钟统计",
        WS_OVERLAPPEDWINDOW,
        (sx-W)/2, (sy-H)/2, W, H,
        parent, NULL, GetModuleHandleW(NULL), NULL);
    if (!hWnd) return;
    ShowWindow(hWnd, SW_SHOW);
    UpdateWindow(hWnd);
}

