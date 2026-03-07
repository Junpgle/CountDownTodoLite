#include "stats_window.h"
#include "utils.h"
#include "api.h"
#include "tai_reader.h"
#include <map>
#include <vector>
#include <string>
#include <algorithm>
#include <chrono>
#include <thread>
#include <fstream>

using namespace Gdiplus;
using json = nlohmann::json;

// --- 数据结构 ---
struct StatsRecord {
    std::wstring appName;
    std::wstring deviceName;
    std::wstring category;
    int seconds;
};

struct AppGroup {
    std::wstring appName;
    std::wstring category;
    int totalSeconds = 0;
    std::vector<std::pair<std::wstring, int>> deviceDetails;
};

struct CategoryGroup {
    std::wstring category;
    int totalSeconds = 0;
};

struct DayStats {
    std::wstring date;
    int totalSeconds = 0;
    std::vector<StatsRecord> records;
};

// --- 静态状态 ---
static std::vector<DayStats> g_WeeklyStats;
static int g_SelectedDayIdx = 6;
static int g_FilterType = 0;
static bool g_IsDarkMode = false;
static int g_ScrollY = 0;
static int g_MaxScrollY = 0;
static std::wstring g_SelectedCategory = L""; // 用于类别点击下钻

// 辅助：获取分类图标颜色
Color GetCategoryColor(const std::wstring& cat) {
    if (cat == L"影音娱乐") return Color(255, 231, 76, 60);
    if (cat == L"社交通讯") return Color(255, 52, 152, 219);
    if (cat == L"游戏与辅助") return Color(255, 155, 89, 182);
    if (cat == L"实用工具") return Color(255, 230, 126, 34);
    if (cat == L"学习办公") return Color(255, 46, 204, 113);
    if (cat == L"健康运动") return Color(255, 26, 188, 156);
    if (cat == L"系统应用") return Color(255, 149, 165, 166);
    return Color(255, 74, 108, 247);
}

// 辅助：绘制圆角矩形路径
void AddRoundedRectToPath(GraphicsPath& path, RectF rect, REAL radius) {
    if (radius <= 0) {
        path.AddRectangle(rect);
        return;
    }
    REAL diam = radius * 2.0f;
    if (diam > rect.Width) diam = rect.Width;
    if (diam > rect.Height) diam = rect.Height;
    path.AddArc(rect.X, rect.Y, diam, diam, 180, 90);
    path.AddArc(rect.X + rect.Width - diam, rect.Y, diam, diam, 270, 90);
    path.AddArc(rect.X + rect.Width - diam, rect.Y + rect.Height - diam, diam, diam, 0, 90);
    path.AddArc(rect.X, rect.Y + rect.Height - diam, diam, diam, 90, 90);
    path.CloseFigure();
}

// 辅助：设备名汉化
std::wstring GetDeviceTypeName(const std::wstring& deviceName) {
    std::wstring dn = deviceName;
    for (auto& c : dn) c = towlower(c);
    if (dn.find(L"tablet") != std::wstring::npos || dn.find(L"pad") != std::wstring::npos) return L"平板";
    if (dn.find(L"phone") != std::wstring::npos || dn.find(L"iphone") != std::wstring::npos || dn.find(L"android") != std::wstring::npos) return L"手机";
    return L"电脑";
}

int GetDeviceTypeInt(const std::wstring& deviceName) {
    std::wstring type = GetDeviceTypeName(deviceName);
    if (type == L"平板") return 4;
    if (type == L"手机") return 3;
    return 1;
}

std::wstring FormatDurationSimple(int sec) {
    if (sec < 60) return std::to_wstring(sec) + L"秒";
    if (sec < 3600) return std::to_wstring(sec / 60) + L"分";
    return std::to_wstring(sec / 3600) + L"小时 " + std::to_wstring((sec % 3600) / 60) + L"分";
}

std::wstring FormatDurationShort(int sec) {
    if (sec < 3600) return std::to_wstring(sec / 60) + L"分";
    return std::to_wstring(sec / 3600) + L"时" + std::to_wstring((sec % 3600) / 60) + L"分";
}

// --- 缓存系统 ---
std::wstring GetCacheFilePath() {
    WCHAR path[MAX_PATH];
    GetModuleFileNameW(NULL, path, MAX_PATH);
    PathRemoveFileSpecW(path);
    PathAppendW(path, L"stats_cache.json");
    return path;
}

void SaveCacheToDisk() {
    try {
        json jRoot = json::array();
        for (const auto& ds : g_WeeklyStats) {
            json jDay;
            jDay["date"] = ToUtf8(ds.date);
            jDay["totalSeconds"] = ds.totalSeconds;
            json jRecs = json::array();
            for (const auto& r : ds.records) {
                json jr;
                jr["app"] = ToUtf8(r.appName);
                jr["dev"] = ToUtf8(r.deviceName);
                jr["cat"] = ToUtf8(r.category);
                jr["sec"] = r.seconds;
                jRecs.push_back(jr);
            }
            jDay["records"] = jRecs;
            jRoot.push_back(jDay);
        }
        std::ofstream ofs(ToUtf8(GetCacheFilePath()).c_str());
        if (ofs.is_open()) ofs << jRoot.dump();
    } catch (...) {}
}

void LoadCacheFromDisk() {
    try {
        std::ifstream ifs(ToUtf8(GetCacheFilePath()).c_str());
        if (!ifs.is_open()) return;
        json jRoot = json::parse(ifs);
        std::vector<DayStats> temp;
        for (auto& jDay : jRoot) {
            DayStats ds;
            ds.date = ToWide(jDay["date"].get<std::string>());
            ds.totalSeconds = jDay["totalSeconds"].get<int>();
            if (jDay.contains("records")) {
                for (auto& jr : jDay["records"]) {
                    ds.records.push_back({
                        ToWide(jr["app"].get<std::string>()),
                        ToWide(jr["dev"].get<std::string>()),
                        ToWide(jr["cat"].get<std::string>()),
                        jr["sec"].get<int>()
                    });
                }
            }
            temp.push_back(ds);
        }
        if (!temp.empty()) g_WeeklyStats = temp;
    } catch (...) {}
}

void FetchWeeklyData(HWND hWndNotify) {
    // 取今天日期字符串，用于判断是否需要用本机 Tai 数据覆盖
    SYSTEMTIME stNow; GetLocalTime(&stNow);
    wchar_t todayBuf[20];
    swprintf_s(todayBuf, L"%04d-%02d-%02d", stNow.wYear, stNow.wMonth, stNow.wDay);
    std::wstring todayStr = todayBuf;

    // 只在获取今天数据时，用本机 Tai 实时快照覆盖本机条目
    auto localSnapshot = GetLocalAppUsageMapCopy();

    std::vector<DayStats> cloudStats;
    time_t now = time(nullptr);
    for (int i = 6; i >= 0; --i) {
        time_t t = now - (i * 24 * 3600);
        struct tm tm_s; localtime_s(&tm_s, &t);
        wchar_t buf[20]; swprintf_s(buf, L"%04d-%02d-%02d", tm_s.tm_year + 1900, tm_s.tm_mon + 1, tm_s.tm_mday);

        std::wstring dateStr = buf;
        std::wstring url = L"/api/screen_time?user_id=" + std::to_wstring(g_UserId) + L"&date=" + dateStr;
        std::string res = SendRequest(url, "GET", "");

        DayStats ds;
        ds.date = dateStr;

        // 建立 app_name → category 映射表（从服务端返回的全部数据中提取，包括本机条目）
        // 服务端已经 JOIN app_name_mappings，category 字段是准确的
        std::map<std::wstring, std::wstring> appCategoryMap;

        try {
            auto j = json::parse(res);
            if (j.is_array()) {
                // 第一遍：先建完整的分类映射表（含本机条目）
                for (auto& item : j) {
                    std::wstring appName = ToWide(item["app_name"].get<std::string>());
                    std::wstring cat     = item.contains("category") ? ToWide(item["category"].get<std::string>()) : L"未分类";
                    if (!appName.empty() && cat != L"未分类") {
                        appCategoryMap[appName] = cat;
                    }
                }

                // 第二遍：填充 ds.records（今天本机条目跳过，改用 Tai 数据）
                for (auto& item : j) {
                    StatsRecord r;
                    r.appName    = ToWide(item["app_name"].get<std::string>());
                    r.deviceName = ToWide(item["device_name"].get<std::string>());
                    r.category   = item.contains("category") ? ToWide(item["category"].get<std::string>()) : L"未分类";
                    r.seconds    = item["duration"].get<int>();

                    // 今天的本机条目跳过，后面用 Tai 实时数据替换
                    if (dateStr == todayStr && r.deviceName == g_DeviceName) continue;

                    ds.records.push_back(r);
                    ds.totalSeconds += r.seconds;
                }
            }
        } catch(...) {}

        // 今天：用本机 Tai 实时数据插入本机条目（覆盖云端旧值）
        // 分类优先从服务端 app_name_mappings 映射表查，查不到才用"未分类"
        if (dateStr == todayStr && !localSnapshot.empty()) {
            for (const auto& p : localSnapshot) {
                StatsRecord r;
                r.appName    = p.first;
                r.deviceName = g_DeviceName;
                auto catIt   = appCategoryMap.find(p.first);
                r.category   = (catIt != appCategoryMap.end()) ? catIt->second : L"未分类";
                r.seconds    = p.second;
                ds.records.push_back(r);
                ds.totalSeconds += r.seconds;
            }
        }

        cloudStats.push_back(ds);
    }

    if (!cloudStats.empty()) {
        std::lock_guard<std::recursive_mutex> lock(g_DataMutex);
        g_WeeklyStats = cloudStats;
        SaveCacheToDisk();
    }
    if (hWndNotify) InvalidateRect(hWndNotify, NULL, TRUE);
}

// --- 绘图逻辑 ---
void DrawDashboard(Graphics& g, int width, int height) {
    REAL commonRadius = (REAL)S(16);
    Color bgColor = Color(255, 245, 247, 250);
    Color cardBgSec = Color(255, 238, 241, 245);
    Color textColor = Color(255, 44, 62, 80);
    Color subTextColor = Color(255, 127, 140, 141);
    Color accentColor = Color(255, 74, 108, 247);
    Color upColor = Color(255, 231, 76, 60);
    Color downColor = Color(255, 46, 204, 113);

    g.Clear(bgColor);
    g.SetSmoothingMode(SmoothingModeAntiAlias);
    g.SetTextRenderingHint(TextRenderingHintAntiAliasGridFit);

    // --- 修改点：将 MiSans 替换为 Microsoft YaHei ---
    FontFamily& ff = *g_MiSansFamily;
    Font titleF(&ff, (REAL)S(24), FontStyleBold, UnitPixel);
    Font bigTimeF(&ff, (REAL)S(32), FontStyleBold, UnitPixel);
    Font cardTitleF(&ff, (REAL)S(18), FontStyleBold, UnitPixel);
    Font normalF(&ff, (REAL)S(16), FontStyleRegular, UnitPixel);
    Font smallF(&ff, (REAL)S(13), FontStyleRegular, UnitPixel);

    SolidBrush textBrush(textColor);
    SolidBrush subBrush(subTextColor);
    SolidBrush accentBrush(accentColor);
    SolidBrush cardSecBrush(cardBgSec);
    SolidBrush whiteBrush(Color::White);
    SolidBrush cardBrush(Color::White);
    SolidBrush upBrush(upColor);
    SolidBrush downBrush(downColor);

    StringFormat centerF;
    centerF.SetAlignment(StringAlignmentCenter);
    centerF.SetLineAlignment(StringAlignmentCenter);

    // --- 固定区域: 顶部筛选器 ---
    std::wstring filters[] = {L"聚合数据", L"电脑端", L"移动端", L"手机", L"平板"};
    float fx = (float)S(25);
    for(int i=0; i<5; ++i) {
        RectF btnRect(fx, (REAL)S(25), (REAL)S(120), (REAL)S(45));
        GraphicsPath bp; AddRoundedRectToPath(bp, btnRect, S(22));
        if (g_FilterType == i) {
            g.FillPath(&accentBrush, &bp);
            g.DrawString(filters[i].c_str(), -1, &normalF, btnRect, &centerF, &whiteBrush);
        } else {
            g.FillPath(&cardBrush, &bp);
            g.DrawString(filters[i].c_str(), -1, &normalF, btnRect, &centerF, &textBrush);
        }
        fx += S(135);
    }

    if (g_WeeklyStats.empty()) {
        g.DrawString(L"正在准备数据汇总分析...", -1, &titleF, PointF((REAL)S(25), (REAL)S(140)), &subBrush);
        return;
    }

    const auto& currentDay = g_WeeklyStats[g_SelectedDayIdx];
    std::map<std::wstring, AppGroup> appMap;
    std::map<std::wstring, int> catAggMap;
    int filteredTotal = 0;

    for (const auto& r : currentDay.records) {
        int typeInt = GetDeviceTypeInt(r.deviceName);
        bool match = (g_FilterType == 0) || (g_FilterType == 1 && typeInt == 1) || (g_FilterType == 2 && (typeInt == 3 || typeInt == 4)) || (g_FilterType == 3 && typeInt == 3) || (g_FilterType == 4 && typeInt == 4);
        if (match) {
            appMap[r.appName].appName = r.appName;
            appMap[r.appName].category = r.category;
            appMap[r.appName].totalSeconds += r.seconds;
            appMap[r.appName].deviceDetails.push_back({r.deviceName, r.seconds});
            catAggMap[r.category] += r.seconds;
            filteredTotal += r.seconds;
        }
    }

    // 较前一日对比
    int yesterdayFilteredTotal = 0;
    if (g_SelectedDayIdx > 0) {
        for (const auto& r : g_WeeklyStats[g_SelectedDayIdx - 1].records) {
            int typeInt = GetDeviceTypeInt(r.deviceName);
            bool match = (g_FilterType == 0) || (g_FilterType == 1 && typeInt == 1) || (g_FilterType == 2 && (typeInt == 3 || typeInt == 4)) || (g_FilterType == 3 && typeInt == 3) || (g_FilterType == 4 && typeInt == 4);
            if (match) yesterdayFilteredTotal += r.seconds;
        }
    }

    // --- 固定区域: 左侧统计卡片 ---
    float lx = (float)S(25), lw = (float)S(480);
    RectF mainCard(lx, (REAL)S(130), lw, (REAL)S(180));
    GraphicsPath mcp; AddRoundedRectToPath(mcp, mainCard, commonRadius);
    g.FillPath(&cardSecBrush, &mcp);

    std::wstring line1 = currentDay.date + L" 使用时长";
    g.DrawString(line1.c_str(), -1, &normalF, PointF(mainCard.X + S(25), mainCard.Y + S(25)), &subBrush);
    g.DrawString(FormatDurationSimple(filteredTotal).c_str(), -1, &bigTimeF, PointF(mainCard.X + S(25), mainCard.Y + S(65)), &textBrush);
    int diffVal = filteredTotal - yesterdayFilteredTotal;
    std::wstring diffLabel = (diffVal >= 0 ? L"较前一日增加 " : L"较前一日减少 ") + FormatDurationSimple(abs(diffVal));
    g.DrawString(diffLabel.c_str(), -1, &normalF, PointF(mainCard.X + S(25), mainCard.Y + S(130)), (diffVal >= 0 ? &upBrush : &downBrush));

    g.DrawString(L"近七日趋势", -1, &cardTitleF, PointF(lx, (REAL)S(350)), &textBrush);
    RectF trendRect(lx, (REAL)S(390), lw, (REAL)S(320));
    GraphicsPath tp; AddRoundedRectToPath(tp, trendRect, commonRadius);
    g.FillPath(&cardSecBrush, &tp);
    float bw = (trendRect.Width - S(40)) / 7.0f;
    int ms = 1; for(auto& d : g_WeeklyStats) if(d.totalSeconds > ms) ms = d.totalSeconds;
    for(int i=0; i<7; ++i) {
        float h = (g_WeeklyStats[i].totalSeconds / (float)ms) * S(200);
        RectF bar(trendRect.X + S(20) + i * bw + S(5), trendRect.Y + S(250) - h, bw - S(10), h);
        if (i == g_SelectedDayIdx) g.FillRectangle(&accentBrush, bar);
        else { SolidBrush f(Color(70, 74, 108, 247)); g.FillRectangle(&f, bar); }

        // 柱子上方标注时间
        std::wstring barTime = FormatDurationShort(g_WeeklyStats[i].totalSeconds);
        g.DrawString(barTime.c_str(), -1, &smallF, RectF(bar.X - S(10), bar.Y - S(20), bar.Width + S(20), S(20)), &centerF, &textBrush);

        std::wstring dl = g_WeeklyStats[i].date.substr(5);
        g.DrawString(dl.c_str(), -1, &smallF, RectF(bar.X - S(5), trendRect.Y + S(260), bw + S(10), S(20)), &centerF, &subBrush);
    }

    // --- 可滑动区域: 右侧详细数据 ---
    float rx = lx + lw + S(60), rw = width - rx - S(25);
    RectF viewport(rx, (REAL)S(110), rw + S(15), (REAL)(height - S(130)));
    Region clipRegion(viewport);
    g.SetClip(&clipRegion);

    GraphicsState state = g.Save();
    g.TranslateTransform(0, (REAL)-g_ScrollY);

    // 1. 类别分布 (Top 排序 + 首字图标)
    std::vector<CategoryGroup> sortedCats;
    for(auto const& [name, time] : catAggMap) sortedCats.push_back({name, time});
    std::sort(sortedCats.begin(), sortedCats.end(), [](auto& a, auto& b){ return a.totalSeconds > b.totalSeconds; });

    g.DrawString(L"应用类别统计", -1, &cardTitleF, PointF(rx, (REAL)S(110)), &textBrush);
    float cy = (float)S(145), cw = (rw - S(20)) / 3.0f, ch = (float)S(140);
    int ci = 0;
    for(auto const& cat : sortedCats) {
        if (ci >= 6) break;
        int row = ci / 3, col = ci % 3;
        RectF cr(rx + col * (cw + S(10)), cy + row * (ch + S(10)), cw, ch);
        GraphicsPath cp; AddRoundedRectToPath(cp, cr, commonRadius);

        // 如果该类别被选中，加深背景
        if (g_SelectedCategory == cat.category) {
            SolidBrush activeBrush(Color(40, accentColor.GetR(), accentColor.GetG(), accentColor.GetB()));
            g.FillPath(&activeBrush, &cp);
            Pen activePen(accentColor, 2.0f);
            g.DrawPath(&activePen, &cp);
        } else {
            g.FillPath(&cardSecBrush, &cp);
        }

        Color cClr = GetCategoryColor(cat.category);
        SolidBrush cb(cClr);
        g.FillEllipse(&cb, (REAL)(cr.X + (cw-S(45))/2), (REAL)(cr.Y + S(20)), (REAL)S(45), (REAL)S(45));
        std::wstring init = cat.category.substr(0, 1);
        g.DrawString(init.c_str(), -1, &cardTitleF, RectF(cr.X, cr.Y + S(20), cr.Width, S(45)), &centerF, &whiteBrush);

        g.DrawString(cat.category.c_str(), -1, &normalF, RectF(cr.X, cr.Y + S(75), cr.Width, S(25)), &centerF, &textBrush);
        g.DrawString(FormatDurationShort(cat.totalSeconds).c_str(), -1, &normalF, RectF(cr.X, cr.Y + S(100), cr.Width, S(25)), &centerF, &subBrush);
        ci++;
    }

    // 2. 应用清单 (如果是下钻模式显示该类全部应用，否则显示 Top 6)
    float aty = cy + (ci > 3 ? 2 : 1) * (ch + S(10)) + S(25);
    std::wstring appTitle = g_SelectedCategory.empty() ? L"最常使用 TOP 6" : (L"类别: " + g_SelectedCategory + L" 应用明细");
    g.DrawString(appTitle.c_str(), -1, &cardTitleF, PointF(rx, aty), &textBrush);

    float agy = aty + S(45), aw = (rw - S(20)) / 3.0f, ah = (float)S(185);

    std::vector<AppGroup> al;
    for(auto& p : appMap) {
        if (g_SelectedCategory.empty() || p.second.category == g_SelectedCategory) {
            al.push_back(p.second);
        }
    }
    std::sort(al.begin(), al.end(), [](auto& a, auto& b){ return a.totalSeconds > b.totalSeconds; });

    int showCount = g_SelectedCategory.empty() ? std::min(6, (int)al.size()) : (int)al.size();
    float finalY = agy;

    for(int i=0; i<showCount; ++i) {
        int r = i / 3, c = i % 3;
        RectF ar(rx + c * (aw + S(10)), agy + r * (ah + S(10)), aw, ah);
        GraphicsPath ap; AddRoundedRectToPath(ap, ar, commonRadius);
        g.FillPath(&cardSecBrush, &ap);

        Color catClr = GetCategoryColor(al[i].category);
        SolidBrush catBrush(catClr);
        g.FillEllipse(&catBrush, (REAL)(ar.X + S(20)), (REAL)(ar.Y + S(20)), (REAL)S(48), (REAL)S(48));
        std::wstring initial = al[i].appName.empty() ? L"?" : al[i].appName.substr(0, 1);
        g.DrawString(initial.c_str(), -1, &cardTitleF, RectF(ar.X + S(20), ar.Y + S(20), S(48), S(48)), &centerF, &whiteBrush);

        std::wstring nameDisp = al[i].appName;
        if (nameDisp.length() > 10) nameDisp = nameDisp.substr(0, 9) + L"..";
        g.DrawString(nameDisp.c_str(), -1, &normalF, PointF(ar.X + S(75), ar.Y + S(32)), &textBrush);
        g.DrawString(FormatDurationShort(al[i].totalSeconds).c_str(), -1, &cardTitleF, PointF(ar.X + S(20), ar.Y + S(85)), &textBrush);

        // 终端明细同行化
        float dy = ar.Y + S(120);
        std::wstring dText = L"";
        for(const auto& d : al[i].deviceDetails) dText += GetDeviceTypeName(d.first) + L":" + FormatDurationShort(d.second) + L" ";
        g.DrawString(dText.c_str(), -1, &smallF, RectF(ar.X + S(20), dy, ar.Width - S(30), S(50)), StringFormat::GenericDefault(), &subBrush);

        finalY = ar.Y + ar.Height;
    }

    // 3. 其他应用详细清单 (仅在非下钻模式且应用多于6个时显示)
    if (g_SelectedCategory.empty() && al.size() > 6) {
        float ly = finalY + S(40);
        g.DrawString(L"其他应用统计清单", -1, &cardTitleF, PointF(rx, ly), &textBrush);
        ly += S(45);

        float itemH = S(75);
        int otherCount = (int)al.size() - 6;
        float cardH = otherCount * itemH + S(40);

        RectF listCardRect(rx, ly, rw, cardH);
        GraphicsPath dcp; AddRoundedRectToPath(dcp, listCardRect, commonRadius);
        g.FillPath(&cardSecBrush, &dcp);

        float iy = ly + S(20);
        for(int i=6; i < (int)al.size(); ++i) {
            Color cClr = GetCategoryColor(al[i].category);
            SolidBrush cb(cClr);
            g.FillEllipse(&cb, (REAL)(rx + S(20)), (REAL)(iy), (REAL)S(40), (REAL)S(40));
            std::wstring init = al[i].appName.empty() ? L"?" : al[i].appName.substr(0, 1);
            g.DrawString(init.c_str(), -1, &normalF, RectF(rx + S(20), iy, S(40), S(40)), &centerF, &whiteBrush);

            g.DrawString(al[i].appName.c_str(), -1, &normalF, PointF(rx + S(75), iy + S(5)), &textBrush);
            std::wstring ts = FormatDurationSimple(al[i].totalSeconds);
            RectF tr; g.MeasureString(ts.c_str(), -1, &normalF, PointF(0,0), &tr);
            g.DrawString(ts.c_str(), -1, &normalF, PointF(rx + rw - tr.Width - S(25), iy + S(5)), &subBrush);

            std::wstring dStr = L"";
            for(auto& d : al[i].deviceDetails) dStr += GetDeviceTypeName(d.first) + L":" + FormatDurationShort(d.second) + L"  ";
            g.DrawString(dStr.c_str(), -1, &smallF, PointF(rx + S(75), iy + S(30)), &subBrush);
            iy += itemH;
        }
        finalY = iy;
    }

    g_MaxScrollY = (int)(finalY + S(120) - height);
    if (g_MaxScrollY < 0) g_MaxScrollY = 0;

    g.Restore(state);
    g.ResetClip();
}

LRESULT CALLBACK StatsWndProc(HWND hWnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_CREATE:
            LoadCacheFromDisk();
            InvalidateRect(hWnd, NULL, TRUE);
            std::thread([hWnd]() { FetchWeeklyData(hWnd); }).detach();
            break;
        case WM_MOUSEWHEEL: {
            int delta = GET_WHEEL_DELTA_WPARAM(wp);
            g_ScrollY -= delta / 2;
            if (g_ScrollY < 0) g_ScrollY = 0;
            if (g_ScrollY > g_MaxScrollY) g_ScrollY = g_MaxScrollY;
            InvalidateRect(hWnd, NULL, FALSE);
        } break;
        case WM_LBUTTONDOWN: {
            int x = LOWORD(lp), y = HIWORD(lp);
            // 固定区域：筛选按钮 (Y <= 75)
            if (y >= S(25) && y <= S(75)) {
                int idx = (x - S(25)) / S(135);
                if (idx >= 0 && idx < 5) {
                    g_FilterType = idx; g_ScrollY = 0; g_SelectedCategory = L"";
                    InvalidateRect(hWnd, NULL, FALSE);
                }
            }
            // 固定区域：趋势柱子点击 (左侧 X < 500)
            float lx = (float)S(25), lw = (float)S(480);
            RectF th(lx, (REAL)S(390), lw, (REAL)S(320));
            if (x >= th.X && x <= th.X + th.Width && y >= th.Y && y <= th.Y + th.Height) {
                float bw = (th.Width - S(40)) / 7.0f;
                int c = (int)((x - (th.X + S(20))) / bw);
                if (c >= 0 && c < 7) {
                    g_SelectedDayIdx = c; g_ScrollY = 0; g_SelectedCategory = L"";
                    InvalidateRect(hWnd, NULL, FALSE);
                }
            }
            // 可滑动区域：右侧类别卡片点击 (需考虑 ScrollY)
            float rx = lx + lw + S(60), rw = (S(1400) - rx - S(25));
            int scrollAdjustY = y + g_ScrollY;
            float cy = (float)S(145), cw = (rw - S(20)) / 3.0f, ch = (float)S(140);
            if (x >= rx && x <= rx + rw && scrollAdjustY >= cy && scrollAdjustY <= cy + ch * 2 + S(20)) {
                int col = (int)((x - rx) / (cw + S(10)));
                int row = (int)((scrollAdjustY - cy) / (ch + S(10)));
                int clickIdx = row * 3 + col;

                const auto& currentDay = g_WeeklyStats[g_SelectedDayIdx];
                std::map<std::wstring, int> catAggMap;

                // 核心修复：点击事件必须使用与 DrawDashboard 相同的筛选逻辑
                for (const auto& r : currentDay.records) {
                    int typeInt = GetDeviceTypeInt(r.deviceName);
                    bool match = (g_FilterType == 0) || (g_FilterType == 1 && typeInt == 1) || (g_FilterType == 2 && (typeInt == 3 || typeInt == 4)) || (g_FilterType == 3 && typeInt == 3) || (g_FilterType == 4 && typeInt == 4);
                    if (match) catAggMap[r.category] += r.seconds;
                }

                std::vector<CategoryGroup> sortedCats;
                for(auto const& [name, time] : catAggMap) sortedCats.push_back({name, time});
                std::sort(sortedCats.begin(), sortedCats.end(), [](auto& a, auto& b){ return a.totalSeconds > b.totalSeconds; });

                if (clickIdx >= 0 && clickIdx < (int)sortedCats.size()) {
                    if (g_SelectedCategory == sortedCats[clickIdx].category) g_SelectedCategory = L"";
                    else g_SelectedCategory = sortedCats[clickIdx].category;
                    InvalidateRect(hWnd, NULL, FALSE);
                }
            }
        } break;
        case WM_PAINT: {
            PAINTSTRUCT ps; HDC hdc = BeginPaint(hWnd, &ps);
            RECT rc; GetClientRect(hWnd, &rc);
            HDC mdc = CreateCompatibleDC(hdc);
            HBITMAP mbm = CreateCompatibleBitmap(hdc, rc.right, rc.bottom);
            HBITMAP old = (HBITMAP)SelectObject(mdc, mbm);
            Graphics graphics(mdc);
            DrawDashboard(graphics, rc.right, rc.bottom);
            BitBlt(hdc, 0, 0, rc.right, rc.bottom, mdc, 0, 0, SRCCOPY);
            SelectObject(mdc, old); DeleteObject(mbm); DeleteDC(mdc);
            EndPaint(hWnd, &ps);
        } break;
        case WM_CLOSE: DestroyWindow(hWnd); break;
        default: return DefWindowProc(hWnd, msg, wp, lp);
    }
    return 0;
}

void ShowStatsWindow(HWND parent) {
    WNDCLASSW wc = {0};
    wc.lpfnWndProc = StatsWndProc;
    wc.hInstance = g_hInst;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.lpszClassName = L"StatsWindowFixedScrollV4";
    if (!GetClassInfoW(g_hInst, L"StatsWindowFixedScrollV4", &wc)) RegisterClassW(&wc);

    CreateWindowExW(WS_EX_TOPMOST, L"StatsWindowFixedScrollV4", L"详细统计报告",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_VISIBLE,
        (GetSystemMetrics(SM_CXSCREEN) - S(1400))/2, (GetSystemMetrics(SM_CYSCREEN) - S(900))/2,
        S(1400), S(900), parent, NULL, g_hInst, NULL);
}