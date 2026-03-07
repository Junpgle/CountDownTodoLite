/**
 * settings_window.cpp
 * 现代化卡片式设置界面
 * 布局：左侧固定导航栏（180px） + 右侧内容区，GDI+ 手绘
 */
#include "settings_window.h"
#include "common.h"
#include "utils.h"
#include "api.h"
#include <shlobj.h>
#include <thread>
#include <string>


using namespace Gdiplus;

// ============================================================
// 常量与颜色主题
// ============================================================
static const Color C_BG        = Color(255, 245, 247, 250);
static const Color C_SIDEBAR   = Color(255, 255, 255, 255);
static const Color C_CARD      = Color(255, 255, 255, 255);
static const Color C_ACCENT    = Color(255, 74, 108, 247);
static const Color C_ACCENT_BG = Color(40,  74, 108, 247);
static const Color C_TEXT      = Color(255, 30,  40,  60);
static const Color C_SUB       = Color(255,130, 140, 160);
static const Color C_BORDER    = Color(255,220, 225, 235);
static const Color C_RED       = Color(255,231,  76,  60);
static const Color C_GREEN     = Color(255, 46, 204, 113);
static const Color C_HOVER     = Color(255,235, 238, 248);

// 导航项索引
enum NavPage {
    PAGE_APPEARANCE = 0,
    PAGE_DATASOURCE,
    PAGE_ACCOUNT,
    PAGE_SYNC,
    PAGE_DATA,
    PAGE_ABOUT,
    PAGE_COUNT
};

// 纯文字导航标签（不含 emoji，避免 GDI+ 无法渲染）
static const wchar_t* NAV_LABELS[PAGE_COUNT] = {
    L"  外观",
    L"  数据源",
    L"  账号",
    L"  同步",
    L"  数据",
    L"  关于"
};

// 导航左侧色块颜色（替代 emoji）
static const Color NAV_COLORS[PAGE_COUNT] = {
    Color(255, 255, 149,  0),  // 外观 - 橙
    Color(255,  74, 108, 247), // 数据源 - 蓝
    Color(255,  46, 204, 113), // 账号 - 绿
    Color(255,  52, 199, 220), // 同步 - 青
    Color(255, 255,  69,  58), // 数据 - 红
    Color(255, 142, 142, 147), // 关于 - 灰
};

// 右侧内容区页面标题
static const wchar_t* PAGE_TITLES[PAGE_COUNT] = {
    L"外观",
    L"数据源",
    L"账号",
    L"同步",
    L"数据",
    L"关于"
};

// ============================================================
// 内部状态
// ============================================================
static int  s_Page     = PAGE_APPEARANCE;
static int  s_HoverNav = -1;
static HWND s_hWnd     = NULL;
static HWND s_hParent  = NULL;

static HWND s_hAlphaEdit    = NULL;
static HWND s_hAlphaSlider  = NULL;
static HWND s_hTaiPathEdit  = NULL;
static HWND s_hSyncEdit     = NULL;
static HWND s_hOldPassEdit  = NULL;
static HWND s_hNewPassEdit  = NULL;
static HWND s_hConfPassEdit = NULL;

static std::wstring s_StatusMsg;
static bool         s_StatusOk = true;

struct SettingsHitZone { Rect rect; int id; };
static std::vector<SettingsHitZone> s_HitZones;

// 全局字体句柄（避免每帧泄漏）
static HFONT s_hFont = NULL;

enum HitId {
    HIT_NAV_BASE     = 100,
    HIT_ALPHA_APPLY  = 200,
    HIT_TOP3 = 210, HIT_TOP5 = 211, HIT_TOP10 = 212,
    HIT_FONT_MISANS  = 215, HIT_FONT_SIMHEI = 217,
    HIT_TAI_BROWSE   = 220,
    HIT_TAI_APPLY    = 221,
    HIT_SYNC_NEVER   = 300, HIT_SYNC_5  = 301, HIT_SYNC_10 = 302,
    HIT_SYNC_30      = 303, HIT_SYNC_60 = 304, HIT_SYNC_CUSTOM = 305,
    HIT_SYNC_NOW     = 306,
    HIT_DATA_PULL    = 400,
    HIT_PASSWD_APPLY = 500,
    HIT_LOGOUT       = 501,
    HIT_UPDATE_CHECK = 600,
};

// ============================================================
// 辅助：GDI+ 圆角矩形路径
// ============================================================
static void RoundRect(GraphicsPath& path, RectF r, REAL rad) {
    REAL d = rad * 2;
    if (d > r.Width)  d = r.Width;
    if (d > r.Height) d = r.Height;
    path.AddArc(r.X,              r.Y,              d, d, 180, 90);
    path.AddArc(r.X+r.Width-d,   r.Y,              d, d, 270, 90);
    path.AddArc(r.X+r.Width-d,   r.Y+r.Height-d,  d, d,   0, 90);
    path.AddArc(r.X,              r.Y+r.Height-d,  d, d,  90, 90);
    path.CloseFigure();
}

// ============================================================
// 辅助：绘制卡片（返回内容起始 Y）
// ============================================================
static float DrawCard(Graphics& g, float x, float y, float w, float h, const wchar_t* title) {
    RectF cardR(x, y, w, h);
    GraphicsPath path;
    RoundRect(path, cardR, (REAL)S(10));
    SolidBrush bg(C_CARD);
    g.FillPath(&bg, &path);
    Pen border(C_BORDER, 1.0f);
    g.DrawPath(&border, &path);

    FontFamily& ff = *g_MiSansFamily;
    Font fTitle(&ff, (REAL)S(12), FontStyleBold, UnitPixel);
    SolidBrush tb(C_ACCENT);
    // 标题限定在卡片宽度内
    RectF titleRect(x + S(16), y + S(12), w - S(32), (REAL)S(20));
    StringFormat sfTitle;
    sfTitle.SetTrimming(StringTrimmingEllipsisCharacter);
    sfTitle.SetFormatFlags(StringFormatFlagsNoWrap);
    g.DrawString(title, -1, &fTitle, titleRect, &sfTitle, &tb);

    // 卡片标题下方分割线
    Pen divPen(C_BORDER, 1.0f);
    g.DrawLine(&divPen, x + S(1), y + S(38), x + w - S(1), y + S(38));

    return y + S(48); // 内容区起始 Y（标题行高 48）
}

// ============================================================
// 辅助：绘制 label + value 行，value 自动截断
// ============================================================
static void DrawRow(Graphics& g, float x, float y, float w,
                    const wchar_t* label, const wchar_t* value = nullptr) {
    FontFamily& ff = *g_MiSansFamily;
    Font fLabel(&ff, (REAL)S(13), FontStyleBold, UnitPixel);
    Font fValue(&ff, (REAL)S(12), FontStyleRegular, UnitPixel);
    SolidBrush lb(C_TEXT), vb(C_SUB);

    // label 固定 80px 宽，超长截断
    float labelW = (float)S(80);
    RectF lRect(x, y, labelW, (REAL)S(22));
    StringFormat sfL;
    sfL.SetTrimming(StringTrimmingEllipsisCharacter);
    sfL.SetFormatFlags(StringFormatFlagsNoWrap);
    g.DrawString(label, -1, &fLabel, lRect, &sfL, &lb);

    if (value && value[0]) {
        // value 在 label 右侧，剩余宽度，超长自动省略
        RectF vRect(x + labelW, y, w - labelW, (REAL)S(22));
        StringFormat sf;
        sf.SetAlignment(StringAlignmentNear);
        sf.SetLineAlignment(StringAlignmentNear);
        sf.SetTrimming(StringTrimmingEllipsisCharacter);
        sf.SetFormatFlags(StringFormatFlagsNoWrap);
        g.DrawString(value, -1, &fValue, vRect, &sf, &vb);
    }
}

// ============================================================
// 辅助：绘制按钮并注册 HitZone
// ============================================================
static void DrawButton(Graphics& g, float x, float y, float w, float h,
                       const wchar_t* text, int hitId,
                       Color bg = Color(255,74,108,247),
                       Color fg = Color(255,255,255,255)) {
    RectF r(x, y, w, h);
    GraphicsPath path;
    RoundRect(path, r, (REAL)S(7));
    SolidBrush bgBrush(bg);
    g.FillPath(&bgBrush, &path);
    FontFamily& ff = *g_MiSansFamily;
    Font fBtn(&ff, (REAL)S(13), FontStyleRegular, UnitPixel);
    SolidBrush fgBrush(fg);
    StringFormat sf;
    sf.SetAlignment(StringAlignmentCenter);
    sf.SetLineAlignment(StringAlignmentCenter);
    sf.SetTrimming(StringTrimmingEllipsisCharacter);
    sf.SetFormatFlags(StringFormatFlagsNoWrap);
    g.DrawString(text, -1, &fBtn, r, &sf, &fgBrush);
    s_HitZones.push_back({Rect((int)x,(int)y,(int)w,(int)h), hitId});
}

// 辅助：选项组按钮（多选一）
static void DrawOptionBtn(Graphics& g, float x, float y, float w, float h,
                          const wchar_t* text, bool selected, int hitId) {
    Color bg = selected ? C_ACCENT : C_HOVER;
    Color fg = selected ? Color(255,255,255,255) : C_TEXT;
    DrawButton(g, x, y, w, h, text, hitId, bg, fg);
    // 选中时加左侧 3px 指示条
    if (selected) {
        SolidBrush bar(C_ACCENT);
        g.FillRectangle(&bar, x, y + S(4), (REAL)S(3), h - S(8));
    }
}

// 辅助：分割线
static void DrawDivider(Graphics& g, float x, float y, float w) {
    Pen p(C_BORDER, 1.0f);
    g.DrawLine(&p, x, y, x + w, y);
}

// ============================================================
// 销毁当前页子控件
// ============================================================
static void DestroyPageControls() {
    HWND* handles[] = {
        &s_hAlphaEdit, &s_hAlphaSlider,
        &s_hTaiPathEdit, &s_hSyncEdit,
        &s_hOldPassEdit, &s_hNewPassEdit, &s_hConfPassEdit
    };
    for (auto* h : handles) {
        if (*h && IsWindow(*h)) { DestroyWindow(*h); *h = NULL; }
    }
}

// ============================================================
// 各页面绘制
// ============================================================

// 内部辅助：确保子控件字体已设置
static void EnsureFont() {
    if (!s_hFont) {
        s_hFont = GetMiSansFont(13);
    }
}

// --- 外观页 ---
static void DrawPageAppearance(Graphics& g, float cx, float cy, float cw, float ch) {
    float x = cx + S(20), y = cy + S(10), w = cw - S(40);

    // 卡片1：透明度
    float iy = DrawCard(g, x, y, w, (REAL)S(115), L"窗口透明度");

    // 当前百分比文字
    FontFamily& ff = *g_MiSansFamily;
    Font fSub(&ff, (REAL)S(12), FontStyleRegular, UnitPixel);
    SolidBrush subBr(C_SUB);
    std::wstring pct = std::to_wstring((int)(g_BgAlpha / 255.0f * 100)) + L"%";
    RectF pctRect(x + S(16), iy, (REAL)S(60), (REAL)S(20));
    StringFormat sfPct; sfPct.SetFormatFlags(StringFormatFlagsNoWrap);
    g.DrawString(pct.c_str(), -1, &fSub, pctRect, &sfPct, &subBr);

    // 滑块（宽度留出右侧输入框+按钮）
    int sliderW = (int)(w - S(110));
    if (!s_hAlphaSlider) {
        INITCOMMONCONTROLSEX icex = { sizeof(icex), ICC_BAR_CLASSES };
        InitCommonControlsEx(&icex);
        s_hAlphaSlider = CreateWindowExW(0, TRACKBAR_CLASSW, L"",
            WS_CHILD | WS_VISIBLE | TBS_HORZ | TBS_NOTICKS | TBS_BOTH,
            (int)(x + S(16)), (int)(iy + S(22)),
            sliderW, S(26),
            s_hWnd, (HMENU)3001, GetModuleHandle(NULL), NULL);
        SendMessage(s_hAlphaSlider, TBM_SETRANGE, TRUE, MAKELPARAM(10, 255));
        SendMessage(s_hAlphaSlider, TBM_SETPOS,   TRUE, g_BgAlpha);
        ShowWindow(s_hAlphaSlider, SW_SHOW);
        UpdateWindow(s_hAlphaSlider);
    }
    // 输入框（右侧 60px）
    if (!s_hAlphaEdit) {
        s_hAlphaEdit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT",
            std::to_wstring(g_BgAlpha).c_str(),
            WS_CHILD | WS_VISIBLE | ES_NUMBER | ES_CENTER,
            (int)(x + S(16) + sliderW + S(8)), (int)(iy + S(22)),
            S(52), S(26),
            s_hWnd, (HMENU)3002, GetModuleHandle(NULL), NULL);
        ShowWindow(s_hAlphaEdit, SW_SHOW);
    }
    // 应用按钮
    DrawButton(g,
        x + S(16) + sliderW + S(8) + S(52) + S(6),
        iy + S(22), S(28), S(26),
        L"OK", HIT_ALPHA_APPLY, C_ACCENT, Color(255,255,255,255));

    y += S(115) + S(12);

    // 卡片2：字体选择
    float fontCardH = (REAL)S(96);
    DrawCard(g, x, y, w, fontCardH, L"界面字体");
    {
        float iy2 = y + S(48);
        float fbw = (w - S(32) - S(8)) / 2.0f; // 只有2个选项
        struct { const wchar_t* label; const wchar_t* val; int hid; } fonts[2] = {
            { L"MiSans",  L"MiSans",  HIT_FONT_MISANS  },
            { L"黑体",    L"SimHei",  HIT_FONT_SIMHEI  },
        };
        for (int i = 0; i < 2; i++) {   // ← 改为 i < 2
            DrawOptionBtn(g,
                x + S(16) + i * (fbw + S(8)),
                iy2 + S(10), fbw, (REAL)S(36),
                fonts[i].label,
                g_FontName == fonts[i].val,
                fonts[i].hid);
        }
    }
    y += fontCardH + S(12);

    // 卡片3：统计排名
    {
        float iy3 = DrawCard(g, x, y, w, (REAL)S(96), L"屏幕时间排名展示数量");
        float bw = (w - S(32) - S(16)) / 3.0f;
        DrawOptionBtn(g, x + S(16),                  iy3 + S(10), bw, (REAL)S(36), L"前 3 名",  g_TopAppsCount == 3,  HIT_TOP3);
        DrawOptionBtn(g, x + S(16) + bw + S(8),       iy3 + S(10), bw, (REAL)S(36), L"前 5 名",  g_TopAppsCount == 5,  HIT_TOP5);
        DrawOptionBtn(g, x + S(16) + (bw + S(8)) * 2, iy3 + S(10), bw, (REAL)S(36), L"前 10 名", g_TopAppsCount == 10, HIT_TOP10);
    }
}

// --- 数据源页 ---
static void DrawPageDatasource(Graphics& g, float cx, float cy, float cw, float ch) {
    float x = cx + S(20), y = cy + S(10), w = cw - S(40);

    float iy = DrawCard(g, x, y, w, (REAL)S(140), L"Tai 屏幕时间数据库路径");

    FontFamily& ff = *g_MiSansFamily;
    Font fSub(&ff, (REAL)S(12), FontStyleRegular, UnitPixel);
    SolidBrush subBr(C_SUB);

    // 说明文字（使用 RectF 换行）
    RectF descRect(x + S(16), iy, w - S(32), (REAL)S(30));
    StringFormat sfWrap;
    sfWrap.SetTrimming(StringTrimmingEllipsisCharacter);
    g.DrawString(L"指定 Tai 软件的 data.db 文件路径，用于统计 Windows 屏幕使用时间。",
                 -1, &fSub, descRect, &sfWrap, &subBr);

    // 路径输入框（留出右侧两个按钮）
    float editW = w - S(32) - S(54) - S(46) - S(12);
    if (!s_hTaiPathEdit) {
        s_hTaiPathEdit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT",
            g_TaiDbPath.c_str(),
            WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
            (int)(x + S(16)), (int)(iy + S(38)),
            (int)editW, S(26),
            s_hWnd, (HMENU)3010, GetModuleHandle(NULL), NULL);
        ShowWindow(s_hTaiPathEdit, SW_SHOW);
    }
    DrawButton(g, x + S(16) + editW + S(6),             iy + S(38), S(48), S(26),
               L"浏览", HIT_TAI_BROWSE, C_HOVER, C_TEXT);
    DrawButton(g, x + S(16) + editW + S(6) + S(48) + S(6), iy + S(38), S(44), S(26),
               L"应用", HIT_TAI_APPLY, C_ACCENT, Color(255,255,255,255));

    // 状态
    Color statusColor = g_TaiDbPath.empty() ? C_RED : C_GREEN;
    Font fStatus(&ff, (REAL)S(12), FontStyleRegular, UnitPixel);
    SolidBrush statusBr(statusColor);
    std::wstring statusText = g_TaiDbPath.empty() ? L"未配置，屏幕时间无法采集" : L"路径已配置";
    RectF statusRect(x + S(16), iy + S(74), w - S(32), (REAL)S(20));
    StringFormat sfStat; sfStat.SetTrimming(StringTrimmingEllipsisCharacter); sfStat.SetFormatFlags(StringFormatFlagsNoWrap);
    g.DrawString(statusText.c_str(), -1, &fStatus, statusRect, &sfStat, &statusBr);
}

// --- 账号页 ---
static void DrawPageAccount(Graphics& g, float cx, float cy, float cw, float ch) {
    float x = cx + S(20), y = cy + S(10), w = cw - S(40);

    // 卡片1：账号信息（高度 130）
    float iy = DrawCard(g, x, y, w, (REAL)S(128), L"账号信息");
    DrawRow(g, x + S(16), iy,          w - S(32), L"用户名", g_Username.c_str());
    DrawDivider(g, x + S(16), iy + S(26), w - S(32));
    DrawRow(g, x + S(16), iy + S(32),  w - S(32), L"邮箱",   g_SavedEmail.c_str());
    DrawDivider(g, x + S(16), iy + S(58), w - S(32));
    std::wstring tierLabel = (g_UserTier == L"pro") ? L"Pro" : L"Free";
    DrawRow(g, x + S(16), iy + S(64),  w - S(32), L"账户等级", tierLabel.c_str());

    y += S(128) + S(12);

    // 卡片2：修改密码（高度 220 = 48头 + 3*(28+14) + 按钮44 + 余量）
    float passCardH = (REAL)S(220);
    iy = DrawCard(g, x, y, w, passCardH, L"修改密码");

    FontFamily& ff = *g_MiSansFamily;
    Font fLabel(&ff, (REAL)S(12), FontStyleBold, UnitPixel);
    SolidBrush lb(C_TEXT);

    // label 宽 70，edit 从 label 右侧开始，宽度 = 卡片宽 - 左边距 - label宽 - 右边距
    float labelW   = (float)S(70);
    float editX    = x + S(16) + labelW + S(8);
    float editW    = w - S(32) - labelW - S(8);
    float rowStep  = (float)S(44);

    struct { const wchar_t* label; HWND* handle; UINT id; } fields[3] = {
        { L"当前密码", &s_hOldPassEdit,  3020 },
        { L"新密码",   &s_hNewPassEdit,  3021 },
        { L"确认密码", &s_hConfPassEdit, 3022 },
    };
    for (int i = 0; i < 3; i++) {
        float fy = iy + i * rowStep;
        g.DrawString(fields[i].label, -1, &fLabel, PointF(x + S(16), fy + S(5)), &lb);
        if (!*fields[i].handle) {
            *fields[i].handle = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                WS_CHILD | WS_VISIBLE | ES_PASSWORD,
                (int)editX, (int)fy,
                (int)editW, S(28),
                s_hWnd, (HMENU)(UINT_PTR)fields[i].id, GetModuleHandle(NULL), NULL);
            ShowWindow(*fields[i].handle, SW_SHOW);
        }
    }
    // 确认按钮（在第3个输入框下方 S(12) 处）
    float btnY = iy + 3 * rowStep + S(8);
    DrawButton(g, x + S(16), btnY, w - S(32), (REAL)S(34), L"确认修改密码",
               HIT_PASSWD_APPLY, C_ACCENT, Color(255,255,255,255));

    // 状态消息
    if (!s_StatusMsg.empty()) {
        Font fStatus(&ff, (REAL)S(12), FontStyleRegular, UnitPixel);
        SolidBrush sb(s_StatusOk ? C_GREEN : C_RED);
        RectF msgRect(x + S(16), btnY + S(42), w - S(32), (REAL)S(20));
        StringFormat sfNoWrap;
        sfNoWrap.SetTrimming(StringTrimmingEllipsisCharacter);
        sfNoWrap.SetFormatFlags(StringFormatFlagsNoWrap);
        g.DrawString(s_StatusMsg.c_str(), -1, &fStatus, msgRect, &sfNoWrap, &sb);
    }

    y += passCardH + S(12);

    // 退出登录按钮（独立，红色，高度 S(40)）
    DrawButton(g, x, y, w, (REAL)S(40), L"退出登录",
               HIT_LOGOUT, C_RED, Color(255,255,255,255));
}

// --- 同步页 ---
static void DrawPageSync(Graphics& g, float cx, float cy, float cw, float ch) {
    float x = cx + S(20), y = cy + S(10), w = cw - S(40);

    // 卡片1：同步状态（高度 120）
    float iy = DrawCard(g, x, y, w, (REAL)S(120), L"同步状态");
    DrawRow(g, x + S(16), iy,         w - S(32), L"今日同步",
            (std::to_wstring(g_SyncCount) + L" / " + std::to_wstring(g_SyncLimit) + L" 次").c_str());
    DrawDivider(g, x + S(16), iy + S(26), w - S(32));

    // 设备 ID：截断显示（最多 24 字符）
    std::wstring devId = g_DeviceId;
    if (devId.length() > 24) devId = devId.substr(0, 22) + L"...";
    DrawRow(g, x + S(16), iy + S(32),  w - S(32), L"设备 ID", devId.c_str());
    DrawDivider(g, x + S(16), iy + S(58), w - S(32));

    std::wstring lastSync = g_LastSyncTime > 0
        ? UtcMsToDateString(g_LastSyncTime)
        : L"从未同步";
    DrawRow(g, x + S(16), iy + S(64),  w - S(32), L"上次同步", lastSync.c_str());
    y += S(120) + S(12);

    // 卡片2：同步频率（高度 156）
    iy = DrawCard(g, x, y, w, (REAL)S(156), L"自动同步频率");
    float bw = (w - S(32) - S(8) * 2) / 3.0f;

    // 第1行：从不 / 5分钟 / 10分钟
    struct { const wchar_t* lbl; int val; int hid; } opts[5] = {
        {L"从不",    0,  HIT_SYNC_NEVER},
        {L"5 分钟",  5,  HIT_SYNC_5},
        {L"10 分钟", 10, HIT_SYNC_10},
        {L"30 分钟", 30, HIT_SYNC_30},
        {L"60 分钟", 60, HIT_SYNC_60},
    };
    for (int i = 0; i < 3; i++) {
        DrawOptionBtn(g,
            x + S(16) + i * (bw + S(8)),
            iy + S(8), bw, (REAL)S(34),
            opts[i].lbl, g_SyncInterval == opts[i].val, opts[i].hid);
    }
    // 第2行：30分钟 / 60分钟 / 自定义输入框
    for (int i = 3; i < 5; i++) {
        DrawOptionBtn(g,
            x + S(16) + (i - 3) * (bw + S(8)),
            iy + S(8) + S(34) + S(8), bw, (REAL)S(34),
            opts[i].lbl, g_SyncInterval == opts[i].val, opts[i].hid);
    }
    // 自定义输入框（第2行第3列）
    float customX = x + S(16) + 2 * (bw + S(8));
    float customY = iy + S(8) + S(34) + S(8);
    if (!s_hSyncEdit) {
        s_hSyncEdit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT",
            std::to_wstring(g_SyncInterval).c_str(),
            WS_CHILD | WS_VISIBLE | ES_NUMBER | ES_CENTER,
            (int)customX, (int)customY,
            (int)bw, S(34),
            s_hWnd, (HMENU)3030, GetModuleHandle(NULL), NULL);
        ShowWindow(s_hSyncEdit, SW_SHOW);
    }
    s_HitZones.push_back({Rect((int)customX,(int)customY,(int)bw,S(34)), HIT_SYNC_CUSTOM});

    y += S(156) + S(12);

    // 立即同步按钮
    DrawButton(g, x, y, w, (REAL)S(40), L"立即同步", HIT_SYNC_NOW);

    // 同步中状态提示
    if (!s_StatusMsg.empty() && s_Page == PAGE_SYNC) {
        FontFamily& ff = *g_MiSansFamily;
        Font fStatus(&ff, (REAL)S(12), FontStyleRegular, UnitPixel);
        SolidBrush sb(s_StatusOk ? C_GREEN : C_RED);
        RectF stRect(x + S(8), y + S(48), w, (REAL)S(20));
        StringFormat sfSt; sfSt.SetTrimming(StringTrimmingEllipsisCharacter); sfSt.SetFormatFlags(StringFormatFlagsNoWrap);
        g.DrawString(s_StatusMsg.c_str(), -1, &fStatus, stRect, &sfSt, &sb);
    }
}

// --- 数据页 ---
static void DrawPageData(Graphics& g, float cx, float cy, float cw, float ch) {
    float x = cx + S(20), y = cy + S(10), w = cw - S(40);

    float iy = DrawCard(g, x, y, w, (REAL)S(155), L"云端数据操作");

    FontFamily& ff = *g_MiSansFamily;
    Font fSub(&ff, (REAL)S(12), FontStyleRegular, UnitPixel);
    SolidBrush subBr(C_SUB);

    RectF descRect(x + S(16), iy, w - S(32), (REAL)S(54));
    StringFormat sfWrap;
    g.DrawString(
        L"全量拉取会将上次同步时间戳重置为 0，强制从服务器重新下载所有\n"
        L"Todos 和倒计时数据，覆盖本地缓存。适用于数据丢失或异常修复。",
        -1, &fSub, descRect, &sfWrap, &subBr);

    DrawButton(g, x + S(16), iy + S(62), w - S(32), (REAL)S(40),
               L"全量拉取云端数据", HIT_DATA_PULL);

    if (!s_StatusMsg.empty() && s_Page == PAGE_DATA) {
        Font fStatus(&ff, (REAL)S(12), FontStyleRegular, UnitPixel);
        SolidBrush sb(s_StatusOk ? C_GREEN : C_RED);
        RectF stRect(x + S(16), iy + S(112), w - S(32), (REAL)S(20));
        StringFormat sfSt; sfSt.SetTrimming(StringTrimmingEllipsisCharacter); sfSt.SetFormatFlags(StringFormatFlagsNoWrap);
        g.DrawString(s_StatusMsg.c_str(), -1, &fStatus, stRect, &sfSt, &sb);
    }
}

// --- 关于页 ---
static void DrawPageAbout(Graphics& g, float cx, float cy, float cw, float ch) {
    float x = cx + S(20), y = cy + S(10), w = cw - S(40);

    float iy = DrawCard(g, x, y, w, (REAL)S(215), L"关于 MathQuizLite");

    FontFamily& ff = *g_MiSansFamily;
    Font fBig(&ff, (REAL)S(24), FontStyleBold, UnitPixel);
    Font fSub(&ff, (REAL)S(12), FontStyleRegular, UnitPixel);
    SolidBrush acBr(C_ACCENT), subBr(C_SUB), textBr(C_TEXT);

    // 大标题
    RectF bigR(x + S(16), iy, w - S(32), (REAL)S(36));
    StringFormat sfBig; sfBig.SetTrimming(StringTrimmingEllipsisCharacter); sfBig.SetFormatFlags(StringFormatFlagsNoWrap);
    g.DrawString(L"MathQuizLite", -1, &fBig, bigR, &sfBig, &acBr);
    // 副标题
    RectF subR(x + S(18), iy + S(34), w - S(36), (REAL)S(20));
    StringFormat sfSub; sfSub.SetTrimming(StringTrimmingEllipsisCharacter); sfSub.SetFormatFlags(StringFormatFlagsNoWrap);
    g.DrawString(L"Windows 桌面生产力悬浮组件  v2.0", -1, &fSub, subR, &sfSub, &subBr);

    DrawDivider(g, x + S(16), iy + S(54), w - S(32));

    struct { const wchar_t* k; const wchar_t* v; } info[] = {
        {L"技术栈",   L"C++ 17  Win32 API  GDI+"},
        {L"网络层",   L"WinHTTP  Bearer Token"},
        {L"同步策略", L"Delta Sync  增量同步"},
        {L"后端",     L"Cloudflare Workers  D1 SQLite"},
        {L"本地存储", L"INI + JSON Cache  DPAPI 加密"},
    };
    Font fInfo(&ff, (REAL)S(12), FontStyleRegular, UnitPixel);
    for (int i = 0; i < 5; i++) {
        float ry = iy + S(62) + i * S(28);
        SolidBrush kb(C_ACCENT);
        g.DrawString(info[i].k, -1, &fInfo, PointF(x + S(16), ry), &kb);
        RectF vr(x + S(80), ry, w - S(96), (REAL)S(22));
        StringFormat sfv;
        sfv.SetTrimming(StringTrimmingEllipsisCharacter);
        sfv.SetFormatFlags(StringFormatFlagsNoWrap);
        g.DrawString(info[i].v, -1, &fInfo, vr, &sfv, &subBr);
    }

    y += S(215) + S(12);
    DrawButton(g, x, y, w, (REAL)S(40), L"检查更新", HIT_UPDATE_CHECK);
}

// ============================================================
// 核心绘制函数
// ============================================================
static void DrawSettings(Graphics& g, int width, int height) {
    s_HitZones.clear();

    g.SetSmoothingMode(SmoothingModeAntiAlias);
    g.SetTextRenderingHint(TextRenderingHintClearTypeGridFit);
    g.Clear(C_BG);

    // --- 左侧导航栏 ---
    int navW = S(165);
    SolidBrush sidebarBr(C_SIDEBAR);
    g.FillRectangle(&sidebarBr, 0, 0, navW, height);
    Pen borderPen(C_BORDER, 1.0f);
    g.DrawLine(&borderPen, (REAL)navW - 0.5f, 0.0f, (REAL)navW - 0.5f, (REAL)height);

    // App 标题区
    FontFamily& ff = *g_MiSansFamily;
    Font fAppTitle(&ff, (REAL)S(14), FontStyleBold, UnitPixel);
    SolidBrush acBr(C_ACCENT), textBr(C_TEXT);
    g.DrawString(L"设置", -1, &fAppTitle, PointF((REAL)S(20), (REAL)S(20)), &acBr);

    // 导航项
    Font fNav(&ff, (REAL)S(13), FontStyleRegular, UnitPixel);
    for (int i = 0; i < PAGE_COUNT; i++) {
        float ny  = (REAL)(S(62) + i * S(44));
        float nh  = (REAL)S(36);
        bool sel  = (s_Page == i);
        bool hov  = (s_HoverNav == i);

        // 选中/悬停背景
        if (sel || hov) {
            RectF r((REAL)S(8), ny, (REAL)(navW - S(16)), nh);
            GraphicsPath np; RoundRect(np, r, (REAL)S(7));
            SolidBrush sbr(sel ? C_ACCENT_BG : C_HOVER);
            g.FillPath(&sbr, &np);
        }
        // 左侧彩色方块（替代 emoji）
        SolidBrush colorBlock(NAV_COLORS[i]);
        g.FillRectangle(&colorBlock, (REAL)S(18), ny + S(10), (REAL)S(14), (REAL)S(14));

        // 文字：限定在导航栏宽度内，防止溢出
        SolidBrush navTb(sel ? C_ACCENT : C_TEXT);
        RectF navTextRect((REAL)S(40), ny + S(8), (REAL)(navW - S(48)), (REAL)S(22));
        StringFormat sfNav;
        sfNav.SetAlignment(StringAlignmentNear);
        sfNav.SetLineAlignment(StringAlignmentNear);
        sfNav.SetTrimming(StringTrimmingEllipsisCharacter);
        sfNav.SetFormatFlags(StringFormatFlagsNoWrap);
        g.DrawString(NAV_LABELS[i] + 2, -1, &fNav, navTextRect, &sfNav, &navTb);

        s_HitZones.push_back({
            Rect(S(8), (int)ny, navW - S(16), (int)nh),
            HIT_NAV_BASE + i
        });
    }

    // --- 右侧内容区 ---
    float cx  = (float)navW;
    float cw  = (float)(width - navW);
    float ch  = (float)height;

    // 设置裁剪区域，防止内容溢出到导航栏或窗口外
    RectF clipRect(cx, 0, cw, (float)height);
    g.SetClip(clipRect);

    // 内容区顶部标题
    Font fPageTitle(&ff, (REAL)S(16), FontStyleBold, UnitPixel);
    g.DrawString(PAGE_TITLES[s_Page], -1, &fPageTitle,
                 PointF(cx + S(20), (REAL)S(16)), &textBr);
    DrawDivider(g, cx + S(16), (float)S(48), cw - S(32));

    float pageY = (float)S(56);

    switch (s_Page) {
        case PAGE_APPEARANCE: DrawPageAppearance(g, cx, pageY, cw, ch - pageY); break;
        case PAGE_DATASOURCE: DrawPageDatasource(g, cx, pageY, cw, ch - pageY); break;
        case PAGE_ACCOUNT:    DrawPageAccount   (g, cx, pageY, cw, ch - pageY); break;
        case PAGE_SYNC:       DrawPageSync      (g, cx, pageY, cw, ch - pageY); break;
        case PAGE_DATA:       DrawPageData      (g, cx, pageY, cw, ch - pageY); break;
        case PAGE_ABOUT:      DrawPageAbout     (g, cx, pageY, cw, ch - pageY); break;
        default: break;
    }

    g.ResetClip();
}

// ============================================================
// 子控件字体统一
// ============================================================
static void ApplyFontToChildren() {
    EnsureFont();
    EnumChildWindows(s_hWnd, [](HWND h, LPARAM p) -> BOOL {
        SendMessage(h, WM_SETFONT, p, FALSE); // FALSE：不立即重绘，防止递归
        return TRUE;
    }, (LPARAM)s_hFont);
}

// ============================================================
// 切换页面
// ============================================================
static void SwitchPage(int page) {
    DestroyPageControls();
    s_StatusMsg.clear();
    s_Page = page;
    InvalidateRect(s_hWnd, NULL, FALSE);
    UpdateWindow(s_hWnd);       // 触发 WM_PAINT，子控件在 DrawPageXxx 里创建
    ApplyFontToChildren();      // 子控件创建完毕后统一设字体
}

// ============================================================
// 按钮点击处理
// ============================================================
extern void CheckForUpdates(bool isManual);

static void HandleHit(HWND hWnd, int hitId) {
    if (hitId >= HIT_NAV_BASE && hitId < HIT_NAV_BASE + PAGE_COUNT) {
        SwitchPage(hitId - HIT_NAV_BASE);
        return;
    }

    switch (hitId) {

    case HIT_ALPHA_APPLY: {
        if (s_hAlphaSlider) {
            LRESULT pos = SendMessage(s_hAlphaSlider, TBM_GETPOS, 0, 0);
            g_BgAlpha = (BYTE)pos;
        } else if (s_hAlphaEdit) {
            WCHAR buf[16]; GetWindowTextW(s_hAlphaEdit, buf, 16);
            int v = _wtoi(buf);
            if (v >= 10 && v <= 255) g_BgAlpha = (BYTE)v;
        }
        SaveAlphaSetting();
        if (g_hWidgetWnd) PostMessage(g_hWidgetWnd, WM_USER_REFRESH, 0, 0);
        InvalidateRect(hWnd, NULL, FALSE);
        break;
    }
    case HIT_TOP3:  g_TopAppsCount = 3;  SaveAlphaSetting(); if(g_hWidgetWnd) PostMessage(g_hWidgetWnd, WM_USER_REFRESH,0,0); InvalidateRect(hWnd,NULL,FALSE); break;
    case HIT_TOP5:  g_TopAppsCount = 5;  SaveAlphaSetting(); if(g_hWidgetWnd) PostMessage(g_hWidgetWnd, WM_USER_REFRESH,0,0); InvalidateRect(hWnd,NULL,FALSE); break;
    case HIT_TOP10: g_TopAppsCount = 10; SaveAlphaSetting(); if(g_hWidgetWnd) PostMessage(g_hWidgetWnd, WM_USER_REFRESH,0,0); InvalidateRect(hWnd,NULL,FALSE); break;

    case HIT_FONT_MISANS:  g_FontName = L"MiSans";  goto apply_font;
    case HIT_FONT_SIMHEI:  g_FontName = L"SimHei";  goto apply_font;
    apply_font: {
        SaveAlphaSetting();   // FontName 已随 SaveAlphaSetting 一并写入 INI
        RebuildFont();        // 立即切换 g_MiSansFamily
        // 子控件字体也同步更新
        if (s_hFont) { DeleteObject(s_hFont); s_hFont = NULL; }
        EnsureFont();
        ApplyFontToChildren();
        InvalidateRect(hWnd, NULL, FALSE);
        if (g_hWidgetWnd) PostMessage(g_hWidgetWnd, WM_USER_REFRESH, 0, 0);
        break;
    }

    case HIT_TAI_BROWSE: {
        BROWSEINFOW bi = {0};
        bi.hwndOwner = hWnd;
        bi.ulFlags   = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
        bi.lpszTitle = L"选择 Tai 数据库所在文件夹";
        PIDLIST_ABSOLUTE pidl = SHBrowseForFolderW(&bi);
        if (pidl) {
            WCHAR path[MAX_PATH];
            if (SHGetPathFromIDListW(pidl, path)) {
                g_TaiDbPath = std::wstring(path) + L"\\data.db";
                if (s_hTaiPathEdit) SetWindowTextW(s_hTaiPathEdit, g_TaiDbPath.c_str());
            }
            CoTaskMemFree(pidl);
        }
        break;
    }
    case HIT_TAI_APPLY: {
        if (s_hTaiPathEdit) {
            WCHAR buf[MAX_PATH]; GetWindowTextW(s_hTaiPathEdit, buf, MAX_PATH);
            g_TaiDbPath = buf;
        }
        SaveTaiDbPathSetting();
        InvalidateRect(hWnd, NULL, FALSE);
        break;
    }

    case HIT_PASSWD_APPLY: {
        if (!s_hOldPassEdit || !s_hNewPassEdit || !s_hConfPassEdit) break;
        WCHAR oldP[128]={0}, newP[128]={0}, conP[128]={0};
        GetWindowTextW(s_hOldPassEdit,  oldP, 128);
        GetWindowTextW(s_hNewPassEdit,  newP, 128);
        GetWindowTextW(s_hConfPassEdit, conP, 128);
        if (wcslen(newP) == 0) {
            s_StatusMsg = L"新密码不能为空"; s_StatusOk = false;
            InvalidateRect(hWnd,NULL,FALSE); break;
        }
        if (wcscmp(newP, conP) != 0) {
            s_StatusMsg = L"两次输入的新密码不一致"; s_StatusOk = false;
            InvalidateRect(hWnd,NULL,FALSE); break;
        }
        std::wstring op(oldP), np(newP);
        std::thread([hWnd, op, np]() {
            nlohmann::json j;
            j["user_id"]      = g_UserId;
            j["old_password"] = ToUtf8(op);
            j["new_password"] = ToUtf8(np);
            std::string res = SendRequest(L"/api/auth/change_password", "POST", j.dump());
            bool ok = false;
            try { auto r = nlohmann::json::parse(res); ok = r.value("success", false); } catch(...) {}
            s_StatusMsg = ok ? L"密码修改成功" : L"修改失败，请检查当前密码是否正确";
            s_StatusOk  = ok;
            if (ok) { g_SavedPass = np; SaveSettings(g_UserId, g_Username, g_SavedEmail, np, true); }
            InvalidateRect(hWnd, NULL, FALSE);
        }).detach();
        break;
    }
    case HIT_LOGOUT: {
        if (MessageBoxW(hWnd, L"确定要退出登录吗？", L"确认", MB_YESNO | MB_ICONQUESTION) == IDYES) {
            DestroyWindow(hWnd);
            if (g_hWidgetWnd) PostMessage(g_hWidgetWnd, WM_COMMAND, MAKEWPARAM(9001, 0), 0);
        }
        break;
    }

    case HIT_SYNC_NEVER:  g_SyncInterval = 0;  goto save_sync;
    case HIT_SYNC_5:      g_SyncInterval = 5;  goto save_sync;
    case HIT_SYNC_10:     g_SyncInterval = 10; goto save_sync;
    case HIT_SYNC_30:     g_SyncInterval = 30; goto save_sync;
    case HIT_SYNC_60:     g_SyncInterval = 60; goto save_sync;
    case HIT_SYNC_CUSTOM: {
        if (s_hSyncEdit) {
            WCHAR buf[16]; GetWindowTextW(s_hSyncEdit, buf, 16);
            int v = _wtoi(buf); if (v >= 0) g_SyncInterval = v;
        }
        goto save_sync;
    }
    save_sync: {
        WCHAR iniPath[MAX_PATH];
        GetModuleFileNameW(NULL, iniPath, MAX_PATH);
        PathRemoveFileSpecW(iniPath);
        PathAppendW(iniPath, SETTINGS_FILE.c_str());
        WritePrivateProfileStringW(L"Auth", L"SyncInterval",
            std::to_wstring(g_SyncInterval).c_str(), iniPath);
        InvalidateRect(hWnd, NULL, FALSE);
        break;
    }
    case HIT_SYNC_NOW: {
        s_StatusMsg = L"同步中..."; s_StatusOk = true;
        InvalidateRect(hWnd, NULL, FALSE);
        std::thread([hWnd]() {
            SyncData();
            s_StatusMsg = L"同步完成";
            s_StatusOk  = true;
            InvalidateRect(hWnd, NULL, FALSE);
        }).detach();
        break;
    }

    case HIT_DATA_PULL: {
        if (MessageBoxW(hWnd,
            L"全量拉取将重置本地同步时间戳，从服务器重新下载所有数据。\n是否继续？",
            L"确认全量拉取", MB_YESNO | MB_ICONWARNING) == IDYES) {
            s_StatusMsg = L"正在全量拉取..."; s_StatusOk = true;
            InvalidateRect(hWnd, NULL, FALSE);
            std::thread([hWnd]() {
                g_LastSyncTime = 0; SaveLastSyncTime(0);
                SyncData();
                s_StatusMsg = L"全量拉取完成";
                s_StatusOk  = true;
                InvalidateRect(hWnd, NULL, FALSE);
            }).detach();
        }
        break;
    }

    case HIT_UPDATE_CHECK:
        CheckForUpdates(true);
        break;

    default: break;
    }
}

// ============================================================
// 窗口过程
// ============================================================
static LRESULT CALLBACK SettingsWndProc(HWND hWnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE:
        s_hWnd = hWnd;
        EnsureFont();
        PostMessage(hWnd, WM_USER + 1, 0, 0); // 首次绘制后设子控件字体
        break;

    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hWnd, &ps);
        RECT rc; GetClientRect(hWnd, &rc);
        HDC mdc  = CreateCompatibleDC(hdc);
        HBITMAP mbm = CreateCompatibleBitmap(hdc, rc.right, rc.bottom);
        HBITMAP old = (HBITMAP)SelectObject(mdc, mbm);
        { Graphics g(mdc); DrawSettings(g, rc.right, rc.bottom); }
        BitBlt(hdc, 0, 0, rc.right, rc.bottom, mdc, 0, 0, SRCCOPY);
        SelectObject(mdc, old);
        DeleteObject(mbm); DeleteDC(mdc);
        EndPaint(hWnd, &ps);
        // ⚠️ 不在 WM_PAINT 里调用 ApplyFontToChildren，防止 WM_SETFONT→重绘→WM_PAINT 死循环
        break;
    }

    case WM_MOUSEMOVE: {
        int mx = LOWORD(lp), my = HIWORD(lp);
        int newHov = -1;
        int navW = S(165);
        for (int i = 0; i < PAGE_COUNT; i++) {
            float ny = (float)(S(62) + i * S(44));
            if (mx >= S(8) && mx <= navW - S(8) &&
                my >= (int)ny && my <= (int)(ny + S(36))) {
                newHov = i; break;
            }
        }
        if (newHov != s_HoverNav) {
            s_HoverNav = newHov;
            InvalidateRect(hWnd, NULL, FALSE);
        }
        break;
    }

    case WM_LBUTTONDOWN: {
        int mx = LOWORD(lp), my = HIWORD(lp);
        for (const auto& z : s_HitZones) {
            if (mx >= z.rect.X && mx < z.rect.X + z.rect.Width &&
                my >= z.rect.Y && my < z.rect.Y + z.rect.Height) {
                HandleHit(hWnd, z.id);
                break;
            }
        }
        break;
    }

    case WM_HSCROLL: {
        if ((HWND)lp == s_hAlphaSlider) {
            LRESULT pos = SendMessage(s_hAlphaSlider, TBM_GETPOS, 0, 0);
            if (s_hAlphaEdit) SetWindowTextW(s_hAlphaEdit, std::to_wstring(pos).c_str());
        }
        break;
    }

    case WM_COMMAND: break;

    case WM_USER + 1:
        ApplyFontToChildren();
        break;

    case WM_CLOSE:
        DestroyWindow(hWnd);
        break;

    case WM_DESTROY:
        DestroyPageControls();
        if (s_hFont) { DeleteObject(s_hFont); s_hFont = NULL; }
        s_hWnd = NULL;
        break;

    default:
        return DefWindowProc(hWnd, msg, wp, lp);
    }
    return 0;
}

// ============================================================
// 公开入口
// ============================================================
void ShowSettingsWindow(HWND parent) {
    if (s_hWnd && IsWindow(s_hWnd)) {
        SetForegroundWindow(s_hWnd);
        return;
    }

    s_hParent   = parent;
    s_Page      = PAGE_APPEARANCE;
    s_HoverNav  = -1;
    s_StatusMsg.clear();

    static bool s_registered = false;
    if (!s_registered) {
        // 确保 TrackBar 等公共控件可用
        INITCOMMONCONTROLSEX icex = { sizeof(icex), ICC_BAR_CLASSES | ICC_STANDARD_CLASSES };
        InitCommonControlsEx(&icex);

        WNDCLASSW wc = {0};
        wc.lpfnWndProc   = SettingsWndProc;
        wc.hInstance     = GetModuleHandle(NULL);
        wc.hCursor       = LoadCursor(NULL, IDC_ARROW);
        wc.hbrBackground = CreateSolidBrush(RGB(245, 247, 250));
        wc.lpszClassName = L"MathQuizSettingsWnd";
        RegisterClassW(&wc);
        s_registered = true;
    }

    int W = S(680), H = S(530);
    int x = (GetSystemMetrics(SM_CXSCREEN) - W) / 2;
    int y = (GetSystemMetrics(SM_CYSCREEN) - H) / 2;

    HWND h = CreateWindowExW(
        WS_EX_TOPMOST,
        L"MathQuizSettingsWnd", L"设置 - MathQuizLite",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
        x, y, W, H,
        parent, NULL, GetModuleHandle(NULL), NULL);

    ShowWindow(h, SW_SHOW);
    UpdateWindow(h);
}




